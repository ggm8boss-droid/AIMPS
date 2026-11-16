#include "aimp/CharacterActionRuntime.h"

#include "aimp/BuildProfile.h"
#include "aimp/CharacterActionProbeDiagnostics.h" // owned legacy-ref cleanup only
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/NativeResourceLease.h"
#include "aimp/SignatureResolver.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <vector>
#include <utility>

namespace aimp::character_action_runtime {
namespace {

static_assert(sizeof(void*) == 4, "CharacterActionRuntime requires the retail x86 ABI");
using UpdateFn = void(__thiscall*)(void*);
using SetterFn = void(__thiscall*)(void*);
using ItemFn = void*(__thiscall*)(void*);
using RefillFn = std::uint32_t(__thiscall*)(void*, std::int32_t);
constexpr std::size_t kMaximumSessions = 8U;
constexpr double kWaitTimeout = 5.0; // active engine time, never a firing cadence
constexpr unsigned int kReleaseUpdates = 2U;
constexpr ULONGLONG kSteadyValidationIntervalMs = 250ULL;
constexpr std::uintptr_t kMaximumAddress = 0xFFFF0000U;

struct Birth {
    std::uintptr_t character{};
    std::uint32_t spawnKey{};
    std::uint32_t runtimeId{};
    std::uint64_t generation{};
    std::uint64_t epoch{};
};

struct Frame {
    std::uintptr_t controller{};
    std::uintptr_t presentation{};
    std::uintptr_t wrapper{};
    std::uintptr_t item{};
    std::int32_t slot{-1};
    std::uint8_t equipmentActive{};
    std::uint8_t context153C{};
    std::array<std::uint8_t, 8> flags{}; // A0 through A7; read ONLY
    std::int32_t presentationCurrent{-1};
    std::int32_t presentationRequested{-1};
    std::int32_t presentationPhase{-1};
    std::uint8_t presentationActive{};
    std::uint8_t presentationEnabled{};
};

// Admission checks for a NEW press, not a weapon cadence/state machine. These
// values are sampled from the live common item contract used by FUN_007C3B10.
// No field here is written back; no checks are applied between emissions while Held.
enum class StartBlock : std::uint8_t {
    None, CharacterNotReady, ItemBusy, ResourceEmpty, NativeInterval, InvalidState
};

struct StartReadiness {
    std::uint8_t character292{};
    std::int32_t itemState{};
    std::int32_t resources{};
    std::uint8_t flags140{};
    std::uint8_t flags141{};
    float elapsed{};
    float interval{};
};

struct Session {
    Birth birth{};
    std::uintptr_t controller{};
    std::uintptr_t item{};
    std::uintptr_t wrapper{};
    std::uintptr_t presentation{};
    std::int32_t slot{-1};
    Phase phase{Phase::Releasing};
    bool used{};
    bool itemBound{};
    bool legacyCleaned{};
    bool legacyBlocked{};
    bool aimRequested{};
    bool primaryRequested{};
    bool aimStopPending{};
    bool aimStopFailed{};
    bool aimApplied{};
    bool primaryApplied{};
    bool releaseComplete{};
    bool faultReleaseComplete{};
    unsigned int coherentReleaseUpdates{};
    double waitSeconds{};
    std::uint64_t revision{};
    ULONGLONG nextSteadyValidationTick{};
    StartBlock lastStartBlock{StartBlock::None};
    std::uint32_t reportedStartBlocks{}; // each functional refusal once per press
};

struct Ticket {
    std::uintptr_t character{};
    std::uint64_t epoch{};
    std::uint64_t revision{};
    Frame before{};
    double delta{};
    bool stopApplied{};
    bool valid{};
    bool needsAfterUpdate{};
};

struct Runtime {
    // Recursive only for synchronous lifecycle notifications caused by a native
    // setter. The lock is NEVER held across the original EquipmentController update.
    std::recursive_mutex mutex;
    std::array<Birth, 64> births{};
    std::array<Session, kMaximumSessions> sessions{};
    LevelGenerationTracker* generations{};
    GameThreadDispatcher* dispatcher{};
    Diagnostics* diagnostics{};
    const void* aimStart{};
    const void* aimStop{};
    const void* primaryStart{};
    const void* primaryStop{};
    const void* currentItem{};
    const void* deltaAddress{};
    const void* refillResources{};
    std::uintptr_t resourceContextAddress{};
    bool refillInProgress{};
    std::uintptr_t codeBegin{};
    std::uintptr_t codeEnd{};
    std::uint64_t generation{};
    std::uint64_t nextEpoch{1U};
    std::uint32_t releaseThreadId{};
    bool stopping{};
    bool detached{};
    bool enabled{};
};
Runtime gRuntime;
std::atomic<void*> gOriginal{nullptr};
std::atomic<bool> gInstalled{false};
void* gTrampoline{};
thread_local unsigned int gUpdateDepth{};

using GraphCreateFn = void*(__thiscall*)(void*, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint32_t);
using GraphLookupFn = std::uintptr_t(__thiscall*)(void*, const void*);
using BlendLookupFn = std::uintptr_t(__thiscall*)(void*, const void*, void*);
std::atomic<void*> gGraphCreateOriginal{};
void* gGraphCreateTrampoline{};
std::uintptr_t gGraphBase{};

struct GraphAdmission {
    Birth birth{};
    std::uintptr_t owner{}, asset{}, graphLibrary{}, blendLibrary{}, variants{};
};
thread_local const GraphAdmission* gGraphAdmission{};

class GraphAdmissionScope final {
public:
    explicit GraphAdmissionScope(const GraphAdmission& value) noexcept
        : previous_(gGraphAdmission) { gGraphAdmission = &value; }
    explicit GraphAdmissionScope(std::nullptr_t) noexcept
        : previous_(gGraphAdmission) { gGraphAdmission = nullptr; }
    ~GraphAdmissionScope() { gGraphAdmission = previous_; }
private:
    const GraphAdmission* previous_;
};

// This is an admission hint, not an object-lifetime cache. Only AIMP-owned
// controller addresses are published. Every hit is rechecked under mutex and
// against birth/generation/owner before any native call. Retail NPCs therefore
// pay no session lock, snapshot or memory probe in this adapter.
std::array<std::atomic<std::uintptr_t>, kMaximumSessions> gControllerHints{};
std::atomic<std::uint32_t> gControllerMask{0U};

bool NeedsNaturalUpdate(const Session& session) noexcept {
    if (!session.used) return false;
    if (session.legacyBlocked || !session.legacyCleaned) return true;
    if (session.phase != Phase::Idle || !session.releaseComplete) return true;
    if (session.primaryRequested || session.primaryApplied ||
        session.aimStopPending || session.aimStopFailed) return true;
    // A stable Aim HOLD is controller-owned state. Once A1 is applied, lifecycle
    // hooks and explicit commands are sufficient; keeping the controller in the
    // EquipmentController::Update detour only revalidates the same state every frame.
    return session.aimRequested != session.aimApplied;
}

void PublishControllerHintsLocked() noexcept {
    std::uint32_t mask = 0U;
    for (std::size_t i = 0U; i < kMaximumSessions; ++i) {
        const auto& session = gRuntime.sessions[i];
        const auto controller = gRuntime.enabled && NeedsNaturalUpdate(session)
            ? session.controller : 0U;
        gControllerHints[i].store(controller, std::memory_order_release);
        if (controller != 0U) mask |= (1U << i);
    }
    gControllerMask.store(mask, std::memory_order_release);
}

bool MayOwnController(const std::uintptr_t controller) noexcept {
    const auto mask = gControllerMask.load(std::memory_order_acquire);
    if (mask == 0U || controller == 0U) return false;
    // The common one-remote-Character case is one indexed atomic read even
    // when retail updates many unrelated NPC equipment controllers.
    if (std::has_single_bit(mask))
        return gControllerHints[std::countr_zero(mask)].load(
            std::memory_order_acquire) == controller;
    for (std::size_t i = 0U; i < kMaximumSessions; ++i)
        if ((mask & (1U << i)) != 0U &&
            gControllerHints[i].load(std::memory_order_acquire) == controller) return true;
    return false;
}

// Deduplicate VirtualQuery within a *single* validation pass, never across
// frames or callback-capable native calls. The three certified byte-store
// setters cannot invoke callbacks or change mappings, so they do not invalidate
// this pass. Cache permission ranges, NOT object values or their
// validity. Every field is still read live under SEH. This keeps PAGE_GUARD,
// MEM_COMMIT and read/write checks, without requerying the same heap region
// for every field and every repeated owner validation.
struct PermissionRange {
    std::uintptr_t begin{}, end{};
    bool readable{}, writable{};
};

class PermissionPass;
thread_local PermissionPass* gPermissionPass{};

class PermissionPass final {
public:
    PermissionPass() noexcept : previous_(gPermissionPass) { gPermissionPass = this; }
    ~PermissionPass() {
        gPermissionPass = previous_;
        if (previous_ != nullptr) previous_->Reset();
    }
    PermissionPass(const PermissionPass&) = delete;
    PermissionPass& operator=(const PermissionPass&) = delete;

    void Reset() noexcept { count_ = 0U; }

    bool Allows(const void* address, const std::size_t bytes, const bool write) noexcept {
        const auto begin = reinterpret_cast<std::uintptr_t>(address);
        if (begin < 0x10000U || begin > kMaximumAddress ||
            bytes > kMaximumAddress - begin) return false;
        const auto end = begin + bytes;
        auto cursor = begin;
        while (cursor < end) {
            PermissionRange range{};
            bool found = false;
            for (std::size_t i = 0U; i < count_; ++i) {
                if (cursor >= ranges_[i].begin && cursor < ranges_[i].end) {
                    range = ranges_[i];
                    found = true;
                    break;
                }
            }
            if (!found) {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) != sizeof(info) ||
                    info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0U) return false;
                const auto regionBegin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
                if (regionBegin > (std::numeric_limits<std::uintptr_t>::max)() - info.RegionSize)
                    return false;
                const DWORD access = info.Protect & 0xFFU;
                range.begin = regionBegin;
                range.end = regionBegin + info.RegionSize;
                range.writable = access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
                    access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
                range.readable = range.writable || access == PAGE_READONLY || access == PAGE_EXECUTE_READ;
                if (range.end <= cursor) return false;
                // No eviction/allocation needed: an overfull pass simply does
                // fresh queries for the uncommon additional regions.
                if (count_ < ranges_.size()) ranges_[count_++] = range;
            }
            if (!(write ? range.writable : range.readable)) return false;
            cursor = (std::min)(range.end, end);
        }
        return true;
    }
private:
    PermissionPass* previous_{};
    std::array<PermissionRange, 16> ranges_{};
    std::size_t count_{};
};

void ResetPermissions() noexcept {
    if (gPermissionPass != nullptr) gPermissionPass->Reset();
}

// Trivial-only SEH leaf; the PermissionPass C++ lifetime belongs to the caller.
bool CopyWithSeh(void* output, const void* input, const std::size_t bytes) noexcept {
    __try {
        std::memcpy(output, input, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadInputBytesFast(
    const std::uintptr_t controller,
    std::array<std::uint8_t, 2>& inputs) noexcept {
#if defined(_MSC_VER)
    if (controller < 0x10000U || controller > kMaximumAddress - 0xA2U) return false;
    __try {
        inputs[0] = *reinterpret_cast<const volatile std::uint8_t*>(controller + 0xA1U);
        inputs[1] = *reinterpret_cast<const volatile std::uint8_t*>(controller + 0xA2U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return crash_guard::TryCopy(
        inputs.data(), reinterpret_cast<const void*>(controller + 0xA1U), inputs.size());
#endif
}

bool CopyReadable(void* output, const void* input, const std::size_t bytes) noexcept {
    if (gPermissionPass == nullptr) return crash_guard::TryCopy(output, input, bytes);
    return gPermissionPass->Allows(input, bytes, false) && CopyWithSeh(output, input, bytes);
}

bool Readable(const void* address, const std::size_t bytes) noexcept {
    return gPermissionPass != nullptr ? gPermissionPass->Allows(address, bytes, false)
                                     : crash_guard::IsReadable(address, bytes);
}

bool Writable(void* address, const std::size_t bytes) noexcept {
    return gPermissionPass != nullptr ? gPermissionPass->Allows(address, bytes, true)
                                     : crash_guard::IsWritable(address, bytes);
}

template<typename T>
bool Read(const std::uintptr_t base, const std::uintptr_t offset, T& out) noexcept {
    if (base < 0x10000U || base > kMaximumAddress ||
        offset > kMaximumAddress - base) return false;
    return CopyReadable(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
}

bool CodeAddress(const std::uintptr_t address) noexcept {
    return address >= gRuntime.codeBegin && address < gRuntime.codeEnd;
}

// Trivial-only SEH helpers: no C++ objects requiring unwinding in __try functions.
bool SetNativeProtected(const void* address, const std::uintptr_t controller) noexcept {
    if (address == nullptr || controller == 0U) return false;
    __try {
        reinterpret_cast<SetterFn>(const_cast<void*>(address))(
            reinterpret_cast<void*>(controller));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Same certified FUN_007AD7E0 used by Give. Kept in a leaf SEH boundary:
// no C++ objects requiring unwinding and no direct memory writes by the mod.
bool RefillNativeProtected(const std::uintptr_t item, const std::int32_t ammoKind) noexcept {
    if (gRuntime.refillResources == nullptr) return false;
    __try {
        (void)reinterpret_cast<RefillFn>(const_cast<void*>(gRuntime.refillResources))(
            reinterpret_cast<void*>(item), ammoKind);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadNativeItemProtected(const std::uintptr_t controller, std::uintptr_t& item) noexcept {
    __try {
        item = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<ItemFn>(const_cast<void*>(gRuntime.currentItem))(
                reinterpret_cast<void*>(controller)));
        return item >= 0x10000U && item <= kMaximumAddress;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        item = 0U;
        return false;
    }
}

bool SetNative(const void* address, const std::uintptr_t controller) noexcept {
    // These three entry points are independently byte-validated at Install:
    // mov byte ptr [ecx+disp], 0/1; ret. No allocation, mapping change or
    // engine callback. Aim STOP is different: it releases sixteen handles.
    const bool leaf = address != nullptr && (address == gRuntime.aimStart ||
        address == gRuntime.primaryStart || address == gRuntime.primaryStop);
    if (!leaf) ResetPermissions();
    const bool result = SetNativeProtected(address, controller);
    if (!leaf || !result) ResetPermissions();
    return result;
}

bool ReadNativeItem(const std::uintptr_t controller, std::uintptr_t& item) noexcept {
    ResetPermissions();
    const bool result = ReadNativeItemProtected(controller, item);
    ResetPermissions();
    return result;
}

void LogPhase(const Session& session, const char* reason) noexcept {
    try {
        if (gRuntime.diagnostics == nullptr) return;
        gRuntime.diagnostics->Log(
            session.phase == Phase::Faulted ? "warning" : "info",
            "character_action_runtime_state", ToString(session.phase),
            {{"state", ToString(session.phase)}, {"reason", reason},
             {"character", Diagnostics::Pointer(reinterpret_cast<void*>(session.birth.character))},
             {"generation", Diagnostics::Unsigned(session.birth.generation), true},
             {"spawnEpoch", Diagnostics::Unsigned(session.birth.epoch), true},
             {"primaryRequested", Diagnostics::Boolean(session.primaryRequested), true}});
    } catch (...) { /* logging must not unwind through an engine callback */ }
}

void ChangePhase(Session& session, const Phase phase, const char* reason) noexcept {
    if (session.phase == phase) return;
    session.phase = phase;
    session.waitSeconds = 0.0;
    if (phase == Phase::PendingStart) {
        session.lastStartBlock = StartBlock::None;
        session.reportedStartBlocks = 0U;
    }
    LogPhase(session, reason);
}

void Fault(Session& session, const char* reason) noexcept {
    session.primaryRequested = false;
    session.aimRequested = false;
    session.aimStopPending = session.aimStopPending || session.aimApplied;
    session.releaseComplete = false;
    if (session.phase != Phase::Faulted) {
        session.faultReleaseComplete = false;
        session.coherentReleaseUpdates = 0U;
        ChangePhase(session, Phase::Faulted, reason);
    }
}

Session* FindSession(const std::uintptr_t character) noexcept {
    for (auto& session : gRuntime.sessions)
        if (session.used && session.birth.character == character) return &session;
    return nullptr;
}

Birth* FindBirth(const std::uintptr_t character) noexcept {
    for (auto& birth : gRuntime.births)
        if (birth.character == character && character != 0U) return &birth;
    return nullptr;
}

void ForgetLocked(const std::uintptr_t character) noexcept {
    if (auto* session = FindSession(character); session != nullptr) {
        Fault(*session, "character_lifetime_ended");
        *session = {};
        PublishControllerHintsLocked();
    }
    if (auto* birth = FindBirth(character); birth != nullptr) *birth = {};
}

void GenerationLocked(const std::uint64_t generation) noexcept {
    if (generation == gRuntime.generation) return;
    // Do not call old native objects across a level transition.
    for (auto& session : gRuntime.sessions) {
        if (session.used) Fault(session, "generation_changed");
        session = {};
    }
    gRuntime.births = {};
    gRuntime.generation = generation;
    PublishControllerHintsLocked();
}

bool EngineThread() noexcept {
    // Shutdown clears the dispatcher's command queue and binding. A resident
    // release-only hook may still finish on the SAME previously certified
    // engine thread; it must not require the cleared dispatcher to be rebound.
    if (gRuntime.detached) {
        return gRuntime.stopping && gRuntime.releaseThreadId != 0U &&
            gRuntime.releaseThreadId == GetCurrentThreadId();
    }
    if (gRuntime.dispatcher == nullptr) return false;
    const auto threadId = GetCurrentThreadId();
    const bool valid = gRuntime.dispatcher->IsCertifiedEngineThread(threadId);
    if (valid) gRuntime.releaseThreadId = threadId;
    return valid;
}

bool StableGeneration(const Birth& birth) noexcept {
    if (gRuntime.generations == nullptr) return false;
    return gRuntime.generations->IsStableGeneration(birth.generation);
}

bool CharacterIdentity(const Birth& birth, std::uintptr_t& controller,
    std::uintptr_t* validatedDescriptor = nullptr) noexcept {
    const auto* current = FindBirth(birth.character);
    if (current == nullptr || current->epoch != birth.epoch ||
        !StableGeneration(birth)) return false;
    std::uint32_t runtimeId{};
    std::uintptr_t self{}, descriptor{};
    std::int32_t characterClass{};
    const bool valid = Read(birth.character, 0x50U, runtimeId) && runtimeId == birth.runtimeId &&
        Read(birth.character, 0x35A4U, self) && self == birth.character &&
        Read(birth.character, 0x64U, descriptor) &&
        Read(descriptor, 4U, characterClass) && characterClass == 3 &&
        Read(birth.character, 0x1100U, controller) && controller >= 0x10000U;
    if (valid && validatedDescriptor != nullptr) *validatedDescriptor = descriptor;
    return valid;
}

bool GraphOwnerIdentity(const GraphAdmission& admission) noexcept {
    std::lock_guard lock(gRuntime.mutex);
    std::uintptr_t controller{}, owner{}, variants{}, asset{}, graphs{}, blends{};
    return EngineThread() && CharacterIdentity(admission.birth, controller) &&
        admission.owner == admission.birth.character + 0x3F0U &&
        Read(admission.owner, 0x730U, owner) && owner == admission.birth.character &&
        Read(admission.owner, 0x338U, variants) && variants == admission.variants &&
        (admission.asset == 0U ||
            (Read(admission.owner, 0x330U, asset) && asset == admission.asset &&
             Read(asset, 8U, graphs) && graphs == admission.graphLibrary &&
             Read(asset, 0x10U, blends) && blends == admission.blendLibrary));
}

// Only the added preflight lookup is guarded. Its pinned libraries can still
// contain an incomplete table while assets are loading. Refuse that admission;
// never resume a faulted constructor or catch the original graph operation.
std::uintptr_t LookupBlendForAdmission(const std::uintptr_t library,
    const void* request, const std::uintptr_t variants) noexcept {
    __try {
        return reinterpret_cast<BlendLookupFn>(gGraphBase + 0x00636760U)(
            reinterpret_cast<void*>(library), request, reinterpret_cast<void*>(variants));
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 0U;
    }
}

struct GraphWalk {
    const GraphAdmission& admission;
    std::array<std::uintptr_t, 64U> ancestors{};
    std::uint32_t remaining{4096U};

    bool Blend(const std::uintptr_t payload, const std::uintptr_t keyOffset) const noexcept {
        std::array<std::uint32_t, 3U> request{};
        if (admission.blendLibrary == 0U || !Read(payload, keyOffset, request[2]) ||
            !GraphOwnerIdentity(admission)) return false;
        // Exact key/variant resolver used by 006A1B90 -> 00A36760. No
        // replacement key, fallback asset, or cached result is manufactured.
        const auto resource = LookupBlendForAdmission(
            admission.blendLibrary, request.data(), admission.variants);
        return resource != 0U && Readable(reinterpret_cast<const void*>(resource), 0x3AU) &&
            GraphOwnerIdentity(admission);
    }

    bool Node(const std::uintptr_t node, const std::size_t depth) noexcept {
        if (depth >= ancestors.size() || remaining == 0U ||
            !Readable(reinterpret_cast<const void*>(node), 0x28U)) return false;
        --remaining;
        for (std::size_t i = 0U; i < depth; ++i)
            if (ancestors[i] == node) return false;
        ancestors[depth] = node;
        std::uintptr_t payload{}, table{}, factory{}, children{};
        std::uint32_t count{};
        if (!Read(node, 0x14U, payload) || payload == 0U || !Read(payload, 0U, table) ||
            !Read(table, 4U, factory) || !Read(node, 0x1CU, count) ||
            !Read(node, 0x20U, children)) return false;
        // Native serialized node type 4 (A504E0/6A7530) and its two-resource
        // sibling type 5 (A50770/6A7650) both require non-null blend definitions.
        if (factory == gGraphBase + 0x006504E0U && !Blend(payload, 0x10U)) return false;
        if (factory == gGraphBase + 0x00650770U &&
            (!Blend(payload, 0x10U) || !Blend(payload, 0x14U))) return false;
        return Children(children, count, depth + 1U);
    }

    bool Children(const std::uintptr_t children, const std::uint32_t count,
        const std::size_t depth) noexcept {
        if (count > remaining || (count != 0U && (children < 0x10000U ||
            children > kMaximumAddress || count > (kMaximumAddress - children) / 0x28U))) return false;
        for (std::uint32_t i = 0U; i < count; ++i)
            if (!Node(children + i * 0x28U, depth)) return false;
        return true;
    }
};

// Diag 193327/213331 (Crawl Space, 7th level load in one process): both PCs
// crashed inside 6A9360 graph construction when allocator[6] (VA 01BE1BD8+18)
// returned null for a 0x120 clip; retail never checks it. Probe the heap
// before construction and take the native missing-resource path instead.
using AnimHeapAllocFn = void*(__thiscall*)(void*, std::uint32_t, std::uint32_t);
using AnimHeapFreeFn = void(__thiscall*)(void*, void*);
constexpr std::uint32_t kAnimHeapProbeBytes = 0x4000U;
std::atomic<std::uint64_t> gAnimHeapGraphRefusals{};

bool AnimationHeapHasHeadroom() noexcept {
    std::uintptr_t heap{}, vtable{}, alloc{}, release{};
    if (!Read(gGraphBase + 0x017E1BF0U, 0U, heap) || heap == 0U ||
        !Read(heap, 0U, vtable) || !Read(vtable, 0x0CU, alloc) || !Read(vtable, 0x14U, release) ||
        !CodeAddress(alloc) || !CodeAddress(release)) return true; // unknown: retail behaviour
    __try {
        void* const block = reinterpret_cast<AnimHeapAllocFn>(alloc)(
            reinterpret_cast<void*>(heap), kAnimHeapProbeBytes, 0x10U);
        if (block == nullptr) return false;
        reinterpret_cast<AnimHeapFreeFn>(release)(reinterpret_cast<void*>(heap), block);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    return true;
}

std::uintptr_t __fastcall GraphLookupDetour(void* library, void*, const void* request) {
    const auto graph = reinterpret_cast<GraphLookupFn>(gGraphBase + 0x00631400U)(library, request);
    if (graph != 0U && !AnimationHeapHasHeadroom()) {
        gAnimHeapGraphRefusals.fetch_add(1U, std::memory_order_relaxed);
        return 0U; // 6A97A2 -> 6A97DE: nothing allocated yet
    }
    const auto* admission = gGraphAdmission;
    if (admission == nullptr || graph == 0U) return graph;
    std::uintptr_t children{};
    std::uint32_t count{};
    if (reinterpret_cast<std::uintptr_t>(library) != admission->graphLibrary ||
        !GraphOwnerIdentity(*admission) ||
        !Read(graph, 0x1CU, count) || !Read(graph, 0x24U, children)) return 0U;
    GraphWalk walk{*admission};
    // Returning zero feeds 6A97A2 -> 6A97DE, the native missing-resource path,
    // before 6A9360 allocates or partially constructs any graph node.
    return walk.Children(children, count, 0U) ? graph : 0U;
}

void* __fastcall GraphCreateDetour(void* owner, void*, const std::uint32_t a,
    const std::uint32_t b, const std::uint32_t key, const std::uint32_t flags,
    const std::uint32_t layer) {
    const auto original = reinterpret_cast<GraphCreateFn>(gGraphCreateOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return nullptr;
    const auto address = reinterpret_cast<std::uintptr_t>(owner);
    GraphAdmission admission{};
    {
        std::lock_guard lock(gRuntime.mutex);
        // The worker/render graph path remains retail-owned. Only a certified
        // engine-thread operation may borrow a Character's live resource refs.
        // Resident hooks become transparent once resource tracking is disabled.
        const auto* birth = gRuntime.enabled && gRuntime.generations != nullptr &&
            EngineThread() && address >= 0x3F0U
            ? FindBirth(address - 0x3F0U) : nullptr;
        if (birth != nullptr) admission.birth = *birth;
    }
    if (admission.birth.character == 0U) {
        const GraphAdmissionScope scope(nullptr);
        return original(owner, a, b, key, flags, layer);
    }
    admission.owner = address;
    if (!Read(address, 0x338U, admission.variants) || admission.variants == 0U ||
        !Readable(reinterpret_cast<const void*>(admission.variants), 0x44U) ||
        !GraphOwnerIdentity(admission)) return nullptr;
    // Pin the same owner asset and both nested native libraries throughout
    // preflight AND native graph construction, including synchronous callbacks.
    const native_resource::StrongLease asset(address + 0x330U, 0x18U);
    if (!asset) return nullptr;
    const native_resource::StrongLease graphs(asset.Object() + 8U, 0x20U);
    if (!graphs) return nullptr;
    const native_resource::StrongLease blends(asset.Object() + 0x10U, 0x38U);
    admission.asset = asset.Object();
    admission.graphLibrary = graphs.Object();
    admission.blendLibrary = blends.Object();
    const GraphAdmissionScope scope(admission);
    return original(owner, a, b, key, flags, layer);
}

bool ControllerIdentity(const Session& session) noexcept {
    std::uintptr_t controller{}, owner{}, descriptor{};
    std::uint8_t ready{}, characterReady{};
    return CharacterIdentity(session.birth, controller, &descriptor) && controller == session.controller &&
        Read(controller, 0x1528U, owner) && owner == session.birth.character &&
        Read(descriptor, 0x1FU, ready) && ready != 0U &&
        Read(owner, 0x2FBU, characterReady) && characterReady != 0U &&
        Writable(reinterpret_cast<void*>(controller + 0xA1U), 2U) &&
        Writable(reinterpret_cast<void*>(controller + 0x12C0U), 16U * 4U);
}

bool ReadSelection(const Session& session, Frame& frame) noexcept {
    frame.controller = session.controller;
    const auto e = frame.controller;
    std::uintptr_t component{}, vtable{}, accessor{};
    // Both callers have just verified ControllerIdentity. Recheck after the
    // native accessor below, not twice consecutively before calling it.
    if (!Read(session.birth.character, 0x36CU, component) || component < 0x10000U ||
        !Readable(reinterpret_cast<const void*>(component), 0x20U) ||
        !Read(e, 0x150CU, frame.slot) || frame.slot < 0 || frame.slot >= 11 ||
        !Read(e, 0x1510U, frame.equipmentActive) || frame.equipmentActive == 0U ||
        !Read(e, 0x152CU, frame.presentation) || frame.presentation < 0x10000U ||
        !Read(e, 0x153CU, frame.context153C) || !Read(e, 0xA0U, frame.flags) ||
        !Read(e, static_cast<std::uintptr_t>(frame.slot) * 4U, frame.wrapper) ||
        !Read(frame.wrapper, 0U, vtable) || !Read(vtable, 0x14U, accessor) ||
        !CodeAddress(accessor)) return false;
    if (!ReadNativeItem(e, frame.item)) return false;
    // Stop treating a readable address as proof of identity if an accessor
    // synchronously changed the owner/current wrapper.
    std::uintptr_t wrapperAfter{};
    if (!ControllerIdentity(session) ||
        !Read(e, static_cast<std::uintptr_t>(frame.slot) * 4U, wrapperAfter) ||
        wrapperAfter != frame.wrapper) return false;
    return Read(frame.presentation, 8U, frame.presentationActive) &&
        Read(frame.presentation, 0x124U, frame.presentationEnabled) &&
        frame.presentationActive != 0U && frame.presentationEnabled != 0U;
}

bool ReadFrame(const Session& session, Frame& frame) noexcept {
    if (!ReadSelection(session, frame)) return false;
    return Read(frame.presentation, 4U, frame.presentationCurrent) &&
        Read(frame.presentation, 0xCU, frame.presentationPhase) &&
        Read(frame.presentation, 0x118U, frame.presentationRequested);
}

bool SameEquipment(const Session& session, const Frame& frame) noexcept {
    return session.controller == frame.controller && session.slot == frame.slot &&
        session.wrapper == frame.wrapper && session.item == frame.item &&
        session.presentation == frame.presentation;
}

bool SameFrameEquipment(const Frame& a, const Frame& b) noexcept {
    return a.controller == b.controller && a.slot == b.slot && a.wrapper == b.wrapper &&
        a.item == b.item && a.presentation == b.presentation;
}

void BindEquipment(Session& session, const Frame& frame) noexcept {
    session.slot = frame.slot;
    session.item = frame.item;
    session.wrapper = frame.wrapper;
    session.presentation = frame.presentation;
    session.itemBound = true;
}

bool CycleResting(const Frame& frame) noexcept {
    // A1 may remain set while the player keeps aiming. All other flags in
    // this range must have been cleared by retail, not by the adapter.
    return frame.flags[0] == 0U && frame.flags[2] == 0U &&
        frame.flags[3] == 0U && frame.flags[4] == 0U &&
        frame.flags[5] == 0U && frame.flags[6] == 0U && frame.flags[7] == 0U;
}

bool Resting(const Frame& frame) noexcept {
    return CycleResting(frame) &&
        (frame.presentationCurrent == 4 || frame.presentationCurrent == 6) &&
        frame.presentationRequested == frame.presentationCurrent &&
        frame.presentationPhase == 2;
}

bool CanStart(const Session& session, const Frame& frame) noexcept {
    return session.releaseComplete && session.aimRequested && session.aimApplied &&
        !session.aimStopPending && frame.flags[1] != 0U &&
        frame.context153C != 0U && CycleResting(frame) &&
        frame.presentationCurrent == 6 && frame.presentationRequested == 6 &&
        frame.presentationPhase == 2;
}

const char* StartBlockReason(const StartBlock block) noexcept {
    switch (block) {
    case StartBlock::None: return "native_primary_ready";
    case StartBlock::CharacterNotReady: return "native_character_not_ready";
    case StartBlock::ItemBusy: return "native_item_busy";
    case StartBlock::ResourceEmpty: return "primary_resource_empty";
    case StartBlock::NativeInterval: return "native_interval_not_ready";
    case StartBlock::InvalidState: return "native_primary_state_invalid";
    }
    return "native_primary_state_invalid";
}

StartBlock NewPressReadiness(const Session& session, const Frame& frame,
    StartReadiness& state) noexcept {
    // AI.exe.c:752816, 752824, 752826-752840, 753012-753017.
    // A5 is raised BEFORE the final owner+292 event-production guard. On an
    // edge-sensitive item, submitting A2 too early can latch A5 without producing
    // a cycle. Reasserting A2 afterwards cannot recreate that lost rising edge.
    if (!Read(session.birth.character, 0x292U, state.character292) ||
        !Read(frame.item, 0xD8U, state.itemState) ||
        !Read(frame.item, 0xE0U, state.resources) ||
        !Read(frame.item, 0x140U, state.flags140) ||
        !Read(frame.item, 0x141U, state.flags141)) return StartBlock::InvalidState;
    if (state.itemState == 1) return StartBlock::ItemBusy;
    if (state.resources < 0) return StartBlock::InvalidState;
    if (state.resources == 0) return StartBlock::ResourceEmpty;

    // The native charge path establishes A5 before a later discharge. Do not
    // require immediate-discharge permission/timing just to BEGIN charging.
    // This is the common native capability bit, never a weapon ID/name/slot.
    if ((state.flags141 & 2U) != 0U) return StartBlock::None;

    if (state.character292 == 0U) return StartBlock::CharacterNotReady;
    const auto intervalOffset = (state.flags140 & 1U) != 0U ? 0xF0U : 0xF4U;
    if (!Read(frame.controller, 0xACU, state.elapsed) ||
        !Read(frame.item, intervalOffset, state.interval) ||
        !std::isfinite(state.elapsed) || !std::isfinite(state.interval))
        return StartBlock::InvalidState;
    // Observe an already elapsed native interval. Do NOT advance/reset AC,
    // create an AIMP cooldown, or use this check while the input is Held.
    // Retail adds its own dt during this update, so this is conservative.
    return state.elapsed < state.interval ? StartBlock::NativeInterval : StartBlock::None;
}

void RecordStartBlock(Session& session, const StartBlock block,
    const StartReadiness& state) noexcept {
    session.lastStartBlock = block;
    if (block == StartBlock::None) return;
    const auto mask = 1U << static_cast<unsigned int>(block);
    if ((session.reportedStartBlocks & mask) != 0U) return;
    session.reportedStartBlocks |= mask;
    try {
        if (gRuntime.diagnostics == nullptr) return;
        const bool refused = block == StartBlock::ResourceEmpty || block == StartBlock::InvalidState;
        gRuntime.diagnostics->Log(refused ? "warning" : "info",
            "character_action_runtime_start_gate",
            refused ? "Primary press refused; no native START was submitted."
                    : "Primary press pending; native START remains withheld until ready.",
            {{"reason", StartBlockReason(block)}, {"state", ToString(session.phase)},
             {"character", Diagnostics::Pointer(reinterpret_cast<void*>(session.birth.character))},
             {"characterReady", Diagnostics::Unsigned(state.character292), true},
             {"itemState", std::to_string(state.itemState), true},
             {"resources", std::to_string(state.resources), true}});
    } catch (...) { }
}

void BeginRelease(Session& session, const char* reason) noexcept {
    if (session.phase == Phase::Releasing || session.phase == Phase::Faulted) return;
    session.releaseComplete = false;
    session.coherentReleaseUpdates = 0U;
    ChangePhase(session, Phase::Releasing, reason);
}

bool ApplyPrimaryStop(Session& session) noexcept {
    if (!ControllerIdentity(session)) return false;
    const auto epoch = session.birth.epoch;
    const bool applied = SetNative(gRuntime.primaryStop, session.controller);
    if (!session.used || session.birth.epoch != epoch) return false;
    if (!applied) {
        Fault(session, "primary_stop_native_fault");
        return false;
    }
    session.primaryApplied = false;
    return true;
}

bool ApplyAim(Session& session) noexcept {
    if (!ControllerIdentity(session) || session.aimStopFailed) return false;
    const auto epoch = session.birth.epoch;
    if (session.aimStopPending) {
        const bool applied = SetNative(gRuntime.aimStop, session.controller);
        if (!session.used || session.birth.epoch != epoch) return false;
        if (!applied) {
            // Aim STOP has native handle-release side effects. Never retry a
            // partially executed release every frame after a protected fault.
            session.aimStopFailed = true;
            Fault(session, "aim_stop_native_fault");
            return false;
        }
        session.aimStopPending = false;
        session.aimApplied = false;
        return true; // never STOP and re-START Aim in the same update
    }
    if (session.aimRequested && session.phase != Phase::Faulted) {
        // Preserve intent against native drift, not a redundant call when the
        // actual byte already agrees. Aim STOP remains transition-only.
        std::uint8_t aim{};
        if (!Read(session.controller, 0xA1U, aim)) {
            Fault(session, "aim_state_unreadable");
            return false;
        }
        const bool applied = aim != 0U || SetNative(gRuntime.aimStart, session.controller);
        if (!session.used || session.birth.epoch != epoch) return false;
        if (!applied) {
            Fault(session, "aim_start_native_fault");
            return false;
        }
        session.aimApplied = true;
    }
    return true;
}

bool AllReleasedLocked() noexcept {
    for (const auto& session : gRuntime.sessions) {
        if (session.used && (session.aimApplied || session.aimStopPending ||
            session.primaryApplied || session.primaryRequested ||
            (session.phase != Phase::Idle && !session.faultReleaseComplete))) return false;
    }
    return true;
}

Ticket BeforeUpdate(const std::uintptr_t controller) {
    std::lock_guard lock(gRuntime.mutex);
    Ticket ticket{};
    if (!gRuntime.enabled || gRuntime.generations == nullptr) return ticket;
    GenerationLocked(gRuntime.generations->CurrentGeneration());
    Session* session = nullptr;
    for (auto& candidate : gRuntime.sessions)
        if (candidate.used && candidate.controller == controller) { session = &candidate; break; }
    if (session == nullptr) return ticket;
    PermissionPass permissions;
    const auto character = session->birth.character;
    const auto epoch = session->birth.epoch;
    const auto sameSession = [&]() noexcept {
        return session->used && session->birth.epoch == epoch &&
            session->birth.character == character && session->controller == controller;
    };
    if (!EngineThread()) {
        Fault(*session, "unexpected_engine_thread");
        // A new mismatch forbids all STARTs, but must not strand a held input.
        // Safety STOP may still run on the same previously certified thread.
        if (gRuntime.releaseThreadId == 0U ||
            gRuntime.releaseThreadId != GetCurrentThreadId()) return ticket;
    }

    // Primary HOLD can persist for many retail updates. Between periodic deep
    // identity checks, the only state AIMP must observe is A1/A2. Equipment
    // replacement initiated by AIMP always sends STOP first, while lifecycle
    // hooks synchronously invalidate a destroyed Character. If either byte
    // drifts or the cheap read faults, fall through to the fully certified path.
    const bool hotSteadyPrimary = session->legacyCleaned && !session->legacyBlocked &&
        session->itemBound && session->phase == Phase::Held &&
        session->aimRequested && session->aimApplied &&
        session->primaryRequested && session->primaryApplied &&
        !session->aimStopPending && !session->aimStopFailed;
    const auto steadyNow = GetTickCount64();
    if (hotSteadyPrimary && steadyNow < session->nextSteadyValidationTick) {
        std::array<std::uint8_t, 2> inputs{};
        if (ReadInputBytesFast(controller, inputs) &&
            inputs[0] != 0U && inputs[1] != 0U) {
            return ticket;
        }
    }

    if (!ControllerIdentity(*session)) {
        Fault(*session, "character_or_controller_invalidated");
        *session = {}; // no cleanup through a detached/reused controller
        PublishControllerHintsLocked();
        return ticket;
    }

    // One-time migration only. The compatibility helper touches exclusively a
    // handle that the old AIMP trace explicitly retained, never an engine request.
    if (session->legacyBlocked) return ticket;
    if (!session->legacyCleaned) {
        std::string legacyError;
        if (!character_action_probe::RetireOwnedRequestForActionRuntime(
                character, session->birth.generation, legacyError)) {
            if (!sameSession()) return ticket;
            session->legacyBlocked = true;
            Fault(*session, "legacy_request_cleanup_blocked");
            return ticket; // never fight another active ShootingRequest
        }
        ResetPermissions();
        if (!sameSession()) return ticket;
        session->legacyCleaned = true;
        if (!ControllerIdentity(*session)) return ticket;
    }

    // Steady Aim and a Held Primary have no presentation/cadence work: retail
    // owns the latter's firing cycle. Check the live selection (including the
    // native item accessor and its post-callback owner validation) every entry
    // so a changed item never inherits an AIMP input. A1/A2 are still sampled
    // each time, correcting native drift without redundant byte-store calls.
    const bool steadyAim = session->phase == Phase::Idle &&
        session->releaseComplete && !session->primaryRequested &&
        !session->primaryApplied;
    const bool steadyPrimary = session->phase == Phase::Held &&
        session->primaryRequested && session->primaryApplied;
    if ((steadyAim || steadyPrimary) && session->itemBound &&
        session->aimRequested && session->aimApplied &&
        !session->aimStopPending && !session->aimStopFailed) {
        const auto revision = session->revision;
        Frame selection{};
        std::array<std::uint8_t, 2> inputs{};
        if (ReadSelection(*session, selection) && sameSession() &&
            session->revision == revision && session->aimRequested &&
            session->aimApplied && !session->aimStopPending &&
            session->phase == (steadyAim ? Phase::Idle : Phase::Held) &&
            session->primaryRequested == steadyPrimary &&
            SameEquipment(*session, selection) &&
            // Preserve the full snapshot's readability gate even though its
            // presentation values are irrelevant in these steady phases.
            Readable(reinterpret_cast<const void*>(selection.presentation + 4U), 0x121U) &&
            Read(controller, 0xA1U, inputs)) {
            if (!steadyAim || inputs[1] == 0U ||
                SetNative(gRuntime.primaryStop, controller)) {
                if (!sameSession()) return {};
                if (steadyAim) session->primaryApplied = false;
                if (inputs[0] != 0U || SetNative(gRuntime.aimStart, controller)) {
                    if (!sameSession()) return {};
                    session->aimApplied = true;
                    if (steadyAim || inputs[1] != 0U ||
                        SetNative(gRuntime.primaryStart, controller)) {
                        if (!sameSession()) return {};
                        session->nextSteadyValidationTick =
                            GetTickCount64() + kSteadyValidationIntervalMs;
                        PublishControllerHintsLocked();
                        return ticket;
                    }
                }
            }
            if (!sameSession()) return {};
            Fault(*session, "steady_aim_native_fault");
        }
        if (!sameSession()) return {};
        // Unreadable or changed equipment goes through the ordinary fault and
        // release path; never keep the fast path armed on a stale selection.
    }

    Frame frame{};
    // ControllerIdentity was checked above (and again after legacy cleanup
    // when that callback-capable path was used). ReadFrame rechecks it after
    // its own native accessor; no back-to-back duplicate validation here.
    const bool readable = ReadFrame(*session, frame);
    if (!sameSession()) return ticket;
    if (!readable) {
        // A freshly selected/provisioned retail item can be present in the
        // Character equipment table one or two natural updates before its
        // Presentation/current-item chain is fully readable. No AIMP input has
        // been applied yet in this admission state, so this is a transient
        // readiness condition, not a poisoned action cycle. Let retail advance
        // once and retry on the next natural update without converting the
        // session into a permanent Faulted subscriber.
        const bool freshEquipmentSettling = session->phase == Phase::Releasing &&
            !session->itemBound && !session->aimApplied && !session->primaryApplied &&
            !session->aimStopPending && !session->aimStopFailed;
        if (freshEquipmentSettling) {
            PublishControllerHintsLocked();
            return ticket;
        }
        Fault(*session, "equipment_not_ready");
    } else if (!session->itemBound) {
        BindEquipment(*session, frame);
    } else if (!SameEquipment(*session, frame)) {
        Fault(*session, "equipment_changed");
    }

    ticket.character = character;
    ticket.epoch = session->birth.epoch;
    ticket.revision = session->revision;
    double delta{};
    if (CopyReadable(&delta, gRuntime.deltaAddress, sizeof(delta)) &&
        std::isfinite(delta) && delta > 0.0) ticket.delta = (std::min)(delta, 1.0);

    // STOP precedes transition-only Aim STOP. Faults also release the same live
    // controller; they never transfer Primary to the replacement item.
    if (session->phase != Phase::Held || !session->primaryRequested ||
        session->aimStopPending || !session->aimRequested) {
        ticket.stopApplied = ApplyPrimaryStop(*session);
    }
    if (!sameSession()) return {};
    const bool stoppingAimThisUpdate = session->aimStopPending;
    if (!ApplyAim(*session)) {
        if (!sameSession()) return {};
        ticket.stopApplied = ApplyPrimaryStop(*session);
    }
    if (!sameSession()) return {};

    // Capture the input actually supplied to the original update, not the
    // pre-setter snapshot. In particular, an incoming STOP must have reached A2,
    // and a cached aimApplied bit is not evidence that A1 is still set.
    const bool deliveredReadable = readable && Read(controller, 0xA0U, frame.flags);
    if (readable && !deliveredReadable) {
        Fault(*session, "input_state_unreadable");
        ticket.stopApplied = ApplyPrimaryStop(*session);
    }
    if (!sameSession()) return {};
    ticket.before = frame;

    if (deliveredReadable && SameEquipment(*session, frame) && !stoppingAimThisUpdate &&
        session->primaryRequested && session->aimRequested) {
        // A previously completed barrier must not be reused after native cycle
        // state has reappeared. Keep the ONE requested press deferred.
        if (session->phase == Phase::PendingStart && !CycleResting(frame))
            BeginRelease(*session, "native_cycle_changed_before_start");
        if (session->phase == Phase::PendingStart)
            session->lastStartBlock = StartBlock::None; // do not report a stale gate on timeout
        bool newPress = ticket.delta > 0.0 &&
            session->phase == Phase::PendingStart && CanStart(*session, frame);
        if (newPress) {
            StartReadiness readiness{};
            const auto blocked = NewPressReadiness(*session, frame, readiness);
            RecordStartBlock(*session, blocked, readiness);
            newPress = blocked == StartBlock::None;
            if (blocked == StartBlock::ResourceEmpty) {
                // No refill, no implicit reload, and no latent shot when resources
                // later return. The caller must request a NEW press explicitly.
                session->primaryRequested = false;
                ChangePhase(*session, Phase::Idle, "primary_resource_empty");
            } else if (blocked == StartBlock::InvalidState) {
                Fault(*session, "native_primary_state_invalid");
            }
        }
        const bool held = session->phase == Phase::Held;
        if (newPress || held) {
            if (ControllerIdentity(*session) && SetNative(gRuntime.primaryStart, controller)) {
                session->primaryApplied = true;
                session->releaseComplete = false;
                session->coherentReleaseUpdates = 0U;
                ticket.stopApplied = false;
                if (newPress) ChangePhase(*session, Phase::Held, "primary_started");
            } else {
                Fault(*session, "primary_start_native_fault");
                ticket.stopApplied = ApplyPrimaryStop(*session);
            }
        }
    }
    ticket.valid = deliveredReadable && ticket.delta > 0.0;
    // Held and idle aiming have no post-update action or barrier work. Their
    // next entry still reads current identity, slot, wrapper, item and owner
    // before ANY setter. Removing this duplicate read path does not defer a
    // STOP or transfer input to changed equipment. Lifecycle hooks still
    // invalidate sessions synchronously on destruction/generation changes.
    ticket.needsAfterUpdate = session->phase == Phase::Releasing ||
        session->phase == Phase::PendingStart || session->phase == Phase::Faulted;
    if (session->phase == Phase::Held && session->primaryRequested &&
        session->primaryApplied && session->aimRequested && session->aimApplied) {
        session->nextSteadyValidationTick =
            GetTickCount64() + kSteadyValidationIntervalMs;
    }
    // This is what removes a stable Aim HOLD from the detour mask. Transitions,
    // Primary and release/fault barriers stay subscribed to natural updates.
    PublishControllerHintsLocked();
    return ticket;
}

void AfterUpdate(const Ticket& ticket) {
    if (ticket.character == 0U) return;
    std::lock_guard lock(gRuntime.mutex);
    if (!gRuntime.enabled || gRuntime.generations == nullptr) return;
    GenerationLocked(gRuntime.generations->CurrentGeneration());
    auto* session = FindSession(ticket.character);
    if (session == nullptr || session->birth.epoch != ticket.epoch) return;
    PermissionPass permissions;
    if (!EngineThread()) {
        Fault(*session, "unexpected_engine_thread");
        if (gRuntime.releaseThreadId == 0U ||
            gRuntime.releaseThreadId != GetCurrentThreadId()) return;
    }
    if (!ControllerIdentity(*session)) {
        Fault(*session, "post_update_identity_invalid");
        *session = {};
        PublishControllerHintsLocked();
        return;
    }
    Frame after{};
    const bool readable = ReadFrame(*session, after);
    if (!session->used || session->birth.epoch != ticket.epoch ||
        session->birth.character != ticket.character) return;
    if (!readable) Fault(*session, "post_update_equipment_invalid");
    else if (session->itemBound && !SameEquipment(*session, after))
        Fault(*session, "equipment_changed_during_update");
    if (ticket.revision != session->revision) {
        session->coherentReleaseUpdates = 0U;
        return; // a re-entrant command cannot count the older release observation
    }

    if (session->phase == Phase::Releasing || session->phase == Phase::Faulted) {
        // The item helpers run BEFORE FUN_007C3B10 clears A5. A post-update
        // zero alone does not prove the item received a released input. Credit
        // only complete natural updates whose entry AND exit are released.
        const bool coherent = ticket.valid && ticket.stopApplied && readable &&
            !session->aimStopFailed && SameFrameEquipment(ticket.before, after) &&
            CycleResting(ticket.before) && Resting(after) &&
            (session->aimRequested || after.flags[1] == 0U);
        session->coherentReleaseUpdates = coherent
            ? (std::min)(session->coherentReleaseUpdates + 1U, kReleaseUpdates) : 0U;
        if (session->coherentReleaseUpdates >= kReleaseUpdates) {
            if (session->phase == Phase::Faulted) {
                session->faultReleaseComplete = true;
                // A fault is a safety barrier, not a permanent subscription.
                // Once retail has proven a coherent released state and no AIMP
                // input remains applied/pending, there is nothing left for the
                // EquipmentController detour to supervise. Retire the session
                // exactly like a fully released Idle session. A later Aim START
                // will admit a fresh session against the then-current equipment.
                if (!session->aimRequested && !session->primaryRequested &&
                    !session->aimApplied && !session->primaryApplied &&
                    !session->aimStopPending && !session->aimStopFailed) {
                    *session = {};
                    PublishControllerHintsLocked();
                    return;
                }
            } else {
                session->releaseComplete = true;
                ChangePhase(*session, Phase::Idle, "release_barrier_complete");
                if (session->primaryRequested && session->aimRequested)
                    ChangePhase(*session, Phase::PendingStart, "deferred_start_ready");
                // IMPORTANT: no native START here. Only a LATER entry may apply it.
            }
        }
    }
    if (session->phase == Phase::PendingStart || session->phase == Phase::Releasing) {
        session->waitSeconds += ticket.delta;
        if (session->waitSeconds >= kWaitTimeout) {
            const auto* reason = session->phase == Phase::PendingStart &&
                session->lastStartBlock != StartBlock::None
                ? StartBlockReason(session->lastStartBlock) : "native_return_to_ready_timeout";
            Fault(*session, reason);
        }
    }
    if (session->phase == Phase::Idle && session->releaseComplete &&
        !session->aimRequested && !session->primaryRequested &&
        !session->aimApplied && !session->primaryApplied && !session->aimStopPending) {
        // A fully released, unowned input needs no permanent per-frame work.
        // Keep the Birth record: the next explicit Aim request can admit a new
        // session and bind the THEN-current item. This also avoids stale-item
        // Faulted sessions when the user changes equipment after Aim STOP.
        *session = {};
        PublishControllerHintsLocked();
    }
    PublishControllerHintsLocked();
    if (gRuntime.detached && AllReleasedLocked()) {
        gRuntime.enabled = false;
        gRuntime.generations = nullptr;
        gRuntime.dispatcher = nullptr;
        PublishControllerHintsLocked();
    }
}

void AdapterFailed(const std::uintptr_t controller) noexcept {
    try {
        std::lock_guard lock(gRuntime.mutex);
        for (auto& session : gRuntime.sessions)
            if (session.used && session.controller == controller)
                Fault(session, "adapter_failure");
    } catch (...) { }
}

void __fastcall EquipmentUpdateDetour(void* equipment, void*) {
    const auto original = reinterpret_cast<UpdateFn>(gOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return; // trampoline is published before hook activation
    // Nested equipment calls still execute their retail update once. They do
    // not count as extra completed updates for the action release barrier.
    if (gUpdateDepth != 0U) { original(equipment); return; }
    struct DepthGuard { DepthGuard() { ++gUpdateDepth; } ~DepthGuard() { --gUpdateDepth; } } depth;
    if (!MayOwnController(reinterpret_cast<std::uintptr_t>(equipment))) {
        original(equipment);
        return;
    }
    Ticket ticket{};
    try { ticket = BeforeUpdate(reinterpret_cast<std::uintptr_t>(equipment)); }
    catch (...) { AdapterFailed(reinterpret_cast<std::uintptr_t>(equipment)); }
    original(equipment); // exactly once; retail faults remain visible to the crash recorder
    if (ticket.needsAfterUpdate) {
        try { AfterUpdate(ticket); }
        catch (...) { AdapterFailed(reinterpret_cast<std::uintptr_t>(equipment)); }
    }
}

// Hook installation below reads ONLY the certified executable image. It emits
// no RE dump and does not discover alternative entry points. This supplies an
// independent byte signature without guessing a prologue absent from the source ZIP.
class FileHandle final {
public:
    explicit FileHandle(HANDLE handle) noexcept : value(handle) {}
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    HANDLE value;
};

bool LoadCertifiedImage(const std::uintptr_t loadedBase,
    std::vector<std::byte>& mapped, std::string& error) {
    const auto path = CurrentExecutablePath();
    // Keep the file non-writable/non-replaceable while validating and copying it.
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) { error = "Cannot open the certified executable"; return false; }
    const auto validation = ValidateExecutable(path);
    if (!validation.hashMatch) { error = "Character actions require the exact certified executable SHA-256"; return false; }
    const auto& profile = Retail2015Profile();
    std::vector<std::byte> raw(static_cast<std::size_t>(profile.fileSize));
    DWORD readBytes{};
    if (raw.size() > (std::numeric_limits<DWORD>::max)() ||
        !ReadFile(file.value, raw.data(), static_cast<DWORD>(raw.size()), &readBytes, nullptr) ||
        static_cast<std::size_t>(readBytes) != raw.size()) {
        error = "Cannot read the certified executable"; return false;
    }
    mapped.assign(profile.sizeOfImage, std::byte{});
    for (const auto& section : profile.sections) {
        if (section.rawOffset > raw.size() || section.rawSize > raw.size() - section.rawOffset ||
            section.virtualAddress > mapped.size() || section.rawSize > mapped.size() - section.virtualAddress) {
            error = "Invalid certified section bounds"; return false;
        }
        std::memcpy(mapped.data() + section.virtualAddress, raw.data() + section.rawOffset, section.rawSize);
    }
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (raw.size() < sizeof(dos)) return false;
    std::memcpy(&dos, raw.data(), sizeof(dos));
    if (dos.e_lfanew < 0 || static_cast<std::size_t>(dos.e_lfanew) > raw.size() ||
        sizeof(nt) > raw.size() - static_cast<std::size_t>(dos.e_lfanew)) return false;
    std::memcpy(&nt, raw.data() + dos.e_lfanew, sizeof(nt));
    const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const auto delta = static_cast<std::uint32_t>(loadedBase - profile.preferredImageBase);
    if (delta == 0U) return true;
    if (directory.VirtualAddress > mapped.size() || directory.Size > mapped.size() - directory.VirtualAddress)
        return false;
    std::size_t cursor = directory.VirtualAddress;
    const std::size_t end = cursor + directory.Size;
    while (cursor < end) {
        IMAGE_BASE_RELOCATION block{};
        if (sizeof(block) > end - cursor) return false;
        std::memcpy(&block, mapped.data() + cursor, sizeof(block));
        if (block.SizeOfBlock < sizeof(block) || block.SizeOfBlock > end - cursor ||
            ((block.SizeOfBlock - sizeof(block)) & 1U) != 0U) return false;
        const auto count = (block.SizeOfBlock - sizeof(block)) / sizeof(WORD);
        for (std::size_t i = 0U; i < count; ++i) {
            WORD relocation{};
            std::memcpy(&relocation, mapped.data() + cursor + sizeof(block) + i * sizeof(WORD), sizeof(WORD));
            const auto type = relocation >> 12U;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_HIGHLOW) return false;
            const auto rva = static_cast<std::size_t>(block.VirtualAddress) + (relocation & 0xFFFU);
            if (rva > mapped.size() || sizeof(std::uint32_t) > mapped.size() - rva) return false;
            std::uint32_t address{};
            std::memcpy(&address, mapped.data() + rva, sizeof(address));
            address += delta;
            std::memcpy(mapped.data() + rva, &address, sizeof(address));
        }
        cursor += block.SizeOfBlock;
    }
    return true;
}

std::size_t ModRmBytes(const std::span<const std::byte> code, const std::size_t start) noexcept {
    if (start >= code.size()) return 0U;
    const auto modrm = std::to_integer<unsigned int>(code[start]);
    const auto mod = modrm >> 6U;
    const auto rm = modrm & 7U;
    std::size_t size = 1U;
    if (mod != 3U && rm == 4U) {
        if (start + size >= code.size()) return 0U;
        const auto sib = std::to_integer<unsigned int>(code[start + size++]);
        if (mod == 0U && (sib & 7U) == 5U) size += 4U;
    } else if (mod == 0U && rm == 5U) size += 4U;
    if (mod == 1U) ++size;
    if (mod == 2U) size += 4U;
    return size <= code.size() - start ? size : 0U;
}

// Small allow-list, NOT a speculative general-purpose relocator. Every accepted
// instruction is position-independent on x86 after normal PE relocations.
// Relative branches/calls, returns, unknown opcodes and prefixes are refused.
std::size_t SafeInstructionLength(const std::span<const std::byte> code) noexcept {
    if (code.empty()) return 0U;
    std::size_t prefix{};
    if (code[0] == std::byte{0x64} || code[0] == std::byte{0x65}) prefix = 1U;
    if (prefix >= code.size()) return 0U;
    const auto op = std::to_integer<unsigned int>(code[prefix]);
    std::size_t size = prefix + 1U;
    if ((op >= 0x50U && op <= 0x5FU) || op == 0x90U) { }
    else if (op == 0x68U || (op >= 0xB8U && op <= 0xBFU) || op == 0xA1U || op == 0xA3U) size += 4U;
    else if (op == 0x6AU) ++size;
    else if (op == 0x8BU || op == 0x89U || op == 0x8DU || op == 0x85U ||
        op == 0x33U || op == 0x31U || op == 0x8AU || op == 0x88U ||
        op == 0x83U || op == 0x81U) {
        const auto operand = ModRmBytes(code, size);
        if (operand == 0U) return 0U;
        size += operand;
        if (op == 0x83U) ++size;
        if (op == 0x81U) size += 4U;
    } else return 0U;
    return size <= code.size() ? size : 0U;
}

bool ExpectedBytes(const SignatureResolver& resolver, const SymbolId symbol,
    const std::vector<std::byte>& image, const std::size_t bytes, MaskedBytes& expected) {
    const auto* spec = Retail2015Profile().Find(symbol);
    const auto* target = resolver.Resolve(symbol);
    if (spec == nullptr || target == nullptr || spec->rva > image.size() || bytes > image.size() - spec->rva)
        return false;
    expected.bytes.resize(bytes);
    expected.mask.assign(bytes, 'x');
    std::memcpy(expected.bytes.data(), image.data() + spec->rva, bytes);
    std::vector<std::byte> actual(bytes);
    return crash_guard::TryCopy(actual.data(), target, bytes) && expected.Matches(actual);
}

Result Reject(const char* code, const char* message) {
    return {false, code, message};
}
Result Accepted(const Session* session) {
    return {true, "CHARACTER_ACTION_INTENT_ACCEPTED",
        std::string("Intent accepted; state=") + (session == nullptr ? "Idle" : ToString(session->phase)) +
        ". Native application occurs only in the next natural equipment update."};
}

Session* Admit(const std::uintptr_t character, Result& failure) {
    if (!gRuntime.enabled || gRuntime.stopping || gRuntime.generations == nullptr) {
        failure = Reject("CHARACTER_ACTION_UNAVAILABLE", "The certified action runtime is not active.");
        return nullptr;
    }
    GenerationLocked(gRuntime.generations->CurrentGeneration());
    if (!EngineThread()) {
        failure = Reject("CHARACTER_ACTION_THREAD_REJECTED", "Action intentions require the certified engine dispatcher.");
        return nullptr;
    }
    const auto* birth = FindBirth(character);
    std::uintptr_t controller{};
    if (birth == nullptr || !CharacterIdentity(*birth, controller)) {
        failure = Reject("CHARACTER_ACTION_TARGET_INVALID", "Select a live AIMP-owned CIVILIAN from the current generation.");
        return nullptr;
    }
    auto* session = FindSession(character);
    if (session != nullptr && session->controller != controller) {
        Fault(*session, "controller_replaced");
        *session = {};
        PublishControllerHintsLocked();
        failure = Reject("CHARACTER_ACTION_TARGET_CHANGED", "The controller changed. Submit a fresh Aim START for this instance.");
        return nullptr;
    }
    if (session != nullptr) return session;
    for (auto& slot : gRuntime.sessions) {
        if (!slot.used) {
            slot = {};
            slot.used = true;
            slot.birth = *birth;
            slot.controller = controller;
            slot.phase = Phase::Releasing; // admission starts with the SAME two-update release barrier
            PublishControllerHintsLocked();
            LogPhase(slot, "admission_release_barrier");
            return &slot;
        }
    }
    failure = Reject("CHARACTER_ACTION_CAPACITY", "All eight action sessions are already in use.");
    return nullptr;
}

} // namespace

const char* ToString(const Phase phase) noexcept {
    switch (phase) {
    case Phase::Idle: return "Idle";
    case Phase::PendingStart: return "PendingStart";
    case Phase::Held: return "Held";
    case Phase::Releasing: return "Releasing";
    case Phase::Faulted: return "Faulted";
    }
    return "Faulted";
}

bool Install(const SignatureResolver& resolver, HookManager& hooks,
    LevelGenerationTracker& generations, GameThreadDispatcher& dispatcher,
    Diagnostics& diagnostics, const bool exactExecutableHash, std::string& error) {
    if (!exactExecutableHash || !resolver.ExactBuildValidated() || !hooks.MutationAllowed()) {
        error = "CharacterActionRuntime requires the exact certified executable and enabled hooks";
        return false;
    }
    if (Installed()) { error = "CharacterActionRuntime already installed"; return false; }
    const auto* update = resolver.Resolve(SymbolId::CharacterEquipmentControllerUpdate);
    const auto* spec = Retail2015Profile().Find(SymbolId::CharacterEquipmentControllerUpdate);
    if (update == nullptr || spec == nullptr) { error = "Equipment update symbol unavailable"; return false; }
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(update) - spec->rva;
    std::vector<std::byte> reference;
    try {
        if (!LoadCertifiedImage(moduleBase, reference, error)) {
            if (error.empty()) error = "Certified executable mapping or relocation validation failed";
            return false;
        }
    } catch (...) {
        error = "Unable to prepare independent certified-image validation";
        return false;
    }
    MaskedBytes updateSignature;
    if (!ExpectedBytes(resolver, SymbolId::CharacterEquipmentControllerUpdate, reference, 32U, updateSignature)) {
        error = "Equipment update bytes differ from the certified image"; return false;
    }
    // Validate the COMPLETE graph admission function, including CALL 6A9754,
    // TEST/JZ 6A97A2 and the native null-return branch 6A97DE/RET14. Also
    // validate both lookup ABIs against the independently mapped retail image.
    const auto graphBytesMatch = [&](const std::size_t rva, const std::size_t count) {
        if (rva > reference.size() || count > reference.size() - rva) return false;
        std::vector<std::byte> actual(count);
        return crash_guard::TryCopy(actual.data(), reinterpret_cast<const void*>(moduleBase + rva), count) &&
            std::memcmp(actual.data(), reference.data() + rva, count) == 0;
    };
    if (!graphBytesMatch(0x002A9700U, 0xF4U) ||
        !graphBytesMatch(0x00631400U, 16U) || !graphBytesMatch(0x00636760U, 16U)) {
        error = "Native graph admission, failure branch or resource lookup ABI differs from the certified image";
        return false;
    }
    // The single six-byte FS:[0] load has no relative operand and is already
    // supported by the same prefix decoder used for the equipment update.
    if (SafeInstructionLength(std::span<const std::byte>(reference).subspan(0x002A9700U, 6U)) != 6U) {
        error = "Native graph admission prefix is not safely relocatable";
        return false;
    }
    constexpr SymbolId callees[]{SymbolId::CharacterAimStart, SymbolId::CharacterAimStop,
        SymbolId::CharacterPrimaryActionStart, SymbolId::CharacterPrimaryActionStop,
        SymbolId::CharacterCurrentEquipmentItem,
        SymbolId::CharacterItemInitializeNpcResources};
    for (const auto callee : callees) {
        MaskedBytes signature;
        if (!ExpectedBytes(resolver, callee, reference, 16U, signature)) {
            error = "A Character action native primitive differs from the certified image"; return false;
        }
    }
    if (resolver.Resolve(SymbolId::CharacterActionFrameDelta) == nullptr) {
        error = "Engine frame-delta symbol unavailable"; return false;
    }
    std::size_t prefix{};
    const auto code = std::span<const std::byte>(reference).subspan(spec->rva, 32U);
    while (prefix < 5U) {
        const auto length = SafeInstructionLength(code.subspan(prefix));
        if (length == 0U) { error = "Equipment update prefix is not safely relocatable"; return false; }
        prefix += length;
    }
    {
        std::lock_guard lock(gRuntime.mutex);
        gRuntime.generations = &generations;
        gRuntime.dispatcher = &dispatcher;
        gRuntime.diagnostics = &diagnostics;
        gRuntime.generation = generations.CurrentGeneration();
        gRuntime.aimStart = resolver.Resolve(SymbolId::CharacterAimStart);
        gRuntime.aimStop = resolver.Resolve(SymbolId::CharacterAimStop);
        gRuntime.primaryStart = resolver.Resolve(SymbolId::CharacterPrimaryActionStart);
        gRuntime.primaryStop = resolver.Resolve(SymbolId::CharacterPrimaryActionStop);
        gRuntime.currentItem = resolver.Resolve(SymbolId::CharacterCurrentEquipmentItem);
        gRuntime.deltaAddress = resolver.Resolve(SymbolId::CharacterActionFrameDelta);
        gRuntime.refillResources = resolver.Resolve(SymbolId::CharacterItemInitializeNpcResources);
        // DAT_016F0C8C: required by FUN_007AD7E0, already used by Give.
        gRuntime.resourceContextAddress = moduleBase + 0x012F0C8CU;
        gRuntime.codeBegin = moduleBase + Retail2015Profile().textRva;
        gRuntime.codeEnd = gRuntime.codeBegin + Retail2015Profile().textSize;
        gRuntime.stopping = false;
        gRuntime.detached = false;
        gRuntime.enabled = gRuntime.deltaAddress != nullptr;
    }
    HookRequest request{};
    request.name = "character_action_equipment_update";
    request.target = const_cast<void*>(update);
    request.detour = reinterpret_cast<void*>(&EquipmentUpdateDetour);
    request.patchLength = prefix;
    request.expected = std::move(updateSignature);
    request.relocationSafePrefix = true; // independently decoded from the certified file
    request.trampolineOut = &gTrampoline;
    request.publishedTrampolineOut = &gOriginal;
    const auto installStart = hooks.InstalledCount();
    gGraphBase = moduleBase;
    HookRequest graphRequest{};
    graphRequest.name = "character_graph_resource_lifetime";
    graphRequest.target = reinterpret_cast<void*>(moduleBase + 0x002A9700U);
    graphRequest.detour = reinterpret_cast<void*>(&GraphCreateDetour);
    graphRequest.patchLength = 6U;
    graphRequest.expected.bytes.resize(6U);
    graphRequest.expected.mask.assign(6U, 'x');
    std::memcpy(graphRequest.expected.bytes.data(), reference.data() + 0x002A9700U, 6U);
    graphRequest.relocationSafePrefix = true;
    graphRequest.trampolineOut = &gGraphCreateTrampoline;
    graphRequest.publishedTrampolineOut = &gGraphCreateOriginal;
    CallRedirectRequest graphLookup{};
    graphLookup.name = "character_graph_resource_admission";
    graphLookup.callSite = reinterpret_cast<void*>(moduleBase + 0x002A9754U);
    graphLookup.expectedTarget = reinterpret_cast<void*>(moduleBase + 0x00631400U);
    graphLookup.detour = reinterpret_cast<void*>(&GraphLookupDetour);
    if (!hooks.Install(graphRequest, error) || !hooks.InstallCallRedirect(graphLookup, error) ||
        !hooks.Install(request, error)) {
        std::string rollbackError;
        if (!hooks.UninstallFrom(installStart, rollbackError)) error += "; graph admission rollback: " + rollbackError;
        std::lock_guard lock(gRuntime.mutex);
        gRuntime.enabled = false;
        gRuntime.generations = nullptr;
        gRuntime.dispatcher = nullptr;
        gRuntime.diagnostics = nullptr;
        PublishControllerHintsLocked();
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    try {
        diagnostics.Log("info", "character_action_runtime_performance",
            "Stable Aim leaves the EquipmentController detour; Held Primary uses cheap A1/A2 checks with periodic certified validation. Release barriers are unchanged.",
            {{"policy", "event_aim_periodic_primary_v5"}});
        diagnostics.Log("info", "character_action_runtime_rearm_policy",
            "New presses wait for native readiness; two full released updates are required. No native state is reset.",
            {{"policy", "native_ready_edge_release_v2"}});
    } catch (...) { /* installation already succeeded; logging is optional */ }
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }

Result SetAim(const std::uintptr_t character, const bool enabled) {
    std::lock_guard lock(gRuntime.mutex);
    if (gRuntime.refillInProgress)
        return Reject("AMMO_REFILL_BUSY", "An explicit test refill is executing; retry the input command after it returns.");
    Result failure{};
    // An idempotent STOP with no admitted session cannot create an engine action.
    if (!enabled && FindSession(character) == nullptr) return Accepted(nullptr);
    auto* session = Admit(character, failure);
    if (session == nullptr) return failure;
    if (enabled && session->phase == Phase::Faulted) {
        if (!session->faultReleaseComplete)
            return Reject("CHARACTER_ACTION_FAULTED", "The previous cycle has not released safely; Aim START was not queued.");
        const auto birth = session->birth;
        const auto controller = session->controller;
        *session = {};
        session->used = true;
        session->birth = birth;
        session->controller = controller;
        session->phase = Phase::Releasing;
        PublishControllerHintsLocked();
        LogPhase(*session, "explicit_readmission_after_fault");
    }
    if (session->aimRequested == enabled && enabled) return Accepted(session);
    ++session->revision;
    if (!enabled) {
        session->aimStopPending = session->aimStopPending || session->aimRequested || session->aimApplied;
        session->primaryRequested = false;
        session->aimRequested = false;
        BeginRelease(*session, "aim_stop_requested");
    } else session->aimRequested = true;
    PublishControllerHintsLocked();
    return Accepted(session);
}

Result SetPrimaryAction(const std::uintptr_t character, const bool enabled) {
    std::lock_guard lock(gRuntime.mutex);
    if (gRuntime.refillInProgress)
        return Reject("AMMO_REFILL_BUSY", "An explicit test refill is executing; retry the input command after it returns.");
    if (!enabled && FindSession(character) == nullptr) return Accepted(nullptr);
    // Reject before admission: a Primary press cannot wait for a future Aim START.
    auto* existing = FindSession(character);
    if (enabled && (existing == nullptr || !existing->aimRequested))
        return Reject("AIM_REQUIRED", "Primary START requires Aim START first; no latent shot was queued.");
    Result failure{};
    auto* session = Admit(character, failure);
    if (session == nullptr) return failure;
    if (enabled && session->phase == Phase::Faulted)
        return Reject("CHARACTER_ACTION_FAULTED", "Release the previous cycle and explicitly reacquire Aim before firing.");
    if (session->primaryRequested == enabled) return Accepted(session);
    ++session->revision;
    session->primaryRequested = enabled;
    if (!enabled) {
        if (session->phase == Phase::PendingStart && session->releaseComplete)
            ChangePhase(*session, Phase::Idle, "unapplied_start_cancelled");
        else if (session->phase == Phase::Held) BeginRelease(*session, "primary_stop_requested");
    } else if (session->phase == Phase::Idle) {
        ChangePhase(*session, Phase::PendingStart, "primary_start_requested");
    }
    // Releasing stays Releasing. A true request here is exactly one deferred
    // intent; a later STOP cancels it without producing a synthetic click.
    PublishControllerHintsLocked();
    return Accepted(session);
}

Result RefillTestAmmo(const std::uintptr_t character) {
    std::lock_guard lock(gRuntime.mutex);
    if (!gRuntime.enabled || gRuntime.stopping || gRuntime.generations == nullptr ||
        gRuntime.refillResources == nullptr)
        return Reject("AMMO_REFILL_UNAVAILABLE", "The certified action runtime is unavailable.");
    if (!EngineThread() || gUpdateDepth != 0U)
        return Reject("AMMO_REFILL_THREAD_REJECTED", "Refill must run on the engine dispatcher, outside an equipment update.");
    if (gRuntime.refillInProgress)
        return Reject("AMMO_REFILL_BUSY", "A manual refill is already executing; no additional call was made.");

    // Guard callbacks emitted by the native accessor/refill. This flag is not
    // polled per frame and schedules nothing. RAII clears it on every return.
    struct RefillScope final {
        RefillScope() noexcept { gRuntime.refillInProgress = true; }
        ~RefillScope() { gRuntime.refillInProgress = false; }
    } refillScope;
    GenerationLocked(gRuntime.generations->CurrentGeneration());
    PermissionPass permissions;
    const auto* birth = FindBirth(character);
    if (birth == nullptr)
        return Reject("AMMO_REFILL_TARGET_INVALID", "Select a live AIMP-owned CIVILIAN from this generation.");
    Session snapshot{}; // local validation view, NOT an admitted input session
    snapshot.birth = *birth;
    if (!CharacterIdentity(snapshot.birth, snapshot.controller))
        return Reject("AMMO_REFILL_TARGET_INVALID", "The tracked Character identity is no longer valid.");
    const auto* session = FindSession(character);
    const bool hadSession = session != nullptr;
    if (hadSession) {
        if (session->birth.epoch != snapshot.birth.epoch || session->controller != snapshot.controller)
            return Reject("AMMO_REFILL_TARGET_INVALID", "The controller no longer matches its action session.");
        if (session->phase != Phase::Idle || session->primaryRequested || session->primaryApplied ||
            !session->releaseComplete || session->aimStopPending || session->aimStopFailed)
            return Reject("AMMO_REFILL_BUSY", "Use Primary STOP and wait for Idle. Pending, Held, Releasing and Faulted sessions cannot refill.");
        snapshot = *session;
    }
    const auto sameContext = [&]() noexcept {
        if (!gRuntime.enabled || gRuntime.stopping || !EngineThread() || !ControllerIdentity(snapshot))
            return false;
        const auto* current = FindSession(character);
        if (!hadSession) return current == nullptr;
        return current != nullptr && current->birth.epoch == snapshot.birth.epoch &&
            current->controller == snapshot.controller && current->revision == snapshot.revision &&
            current->phase == Phase::Idle && current->releaseComplete &&
            !current->primaryRequested && !current->primaryApplied &&
            !current->aimStopPending && !current->aimStopFailed;
    };
    const auto currentSelectionMatches = [&](const Frame& view) noexcept {
        std::int32_t slot{};
        std::uintptr_t wrapper{}, presentation{};
        std::uint8_t active{};
        return Read(snapshot.controller, 0x150CU, slot) && slot == view.slot &&
            Read(snapshot.controller, 0x1510U, active) && active != 0U &&
            Read(snapshot.controller, static_cast<std::uintptr_t>(slot) * 4U, wrapper) &&
            wrapper == view.wrapper && Read(snapshot.controller, 0x152CU, presentation) &&
            presentation == view.presentation;
    };
    Frame before{};
    if (!sameContext() || !ReadFrame(snapshot, before) || !sameContext() ||
        !currentSelectionMatches(before) || !Read(snapshot.controller, 0xA0U, before.flags))
        return Reject("AMMO_REFILL_TARGET_INVALID", "Character or active equipment validation failed.");
    if (hadSession && (!snapshot.itemBound || !SameEquipment(snapshot, before)))
        return Reject("AMMO_REFILL_TARGET_CHANGED", "The equipment changed. Stop Aim and reacquire the current equipment before refilling.");
    if (!Resting(before))
        return Reject("AMMO_REFILL_BUSY", "The equipment is not at rest. Wait for the native release/presentation transition to finish.");

    std::uintptr_t descriptor{}, owner{};
    std::uint32_t ownerId{};
    std::uint8_t playerDescriptor{}, descriptorReady{}, gameActive{};
    std::int32_t capacity{}, resources{}, ammoKind{}, itemState{};
    if (!Read(character, 0x64U, descriptor) ||
        !Read(descriptor, 0x1CU, playerDescriptor) || playerDescriptor != 0U ||
        !Read(descriptor, 0x1FU, descriptorReady) || descriptorReady == 0U ||
        !Read(gRuntime.resourceContextAddress, 0U, gameActive) || gameActive == 0U ||
        !Read(before.item, 0x28U, owner) || owner != character ||
        !Read(before.item, 0x2CU, ownerId) || ownerId != snapshot.birth.runtimeId ||
        !Read(before.item, 0xECU, capacity) || !Read(before.item, 0xE0U, resources) ||
        !Read(before.item, 0xD0U, ammoKind) || !Read(before.item, 0xD8U, itemState) ||
        capacity < 0 || capacity > 1000000 || resources < 0 || resources > 1000000)
        return Reject("AMMO_REFILL_TARGET_INVALID", "The common item resource contract or its NPC owner is invalid.");
    if (capacity == 0)
        return Reject("AMMO_REFILL_UNSUPPORTED", "The current item has no positive native magazine capacity. Nothing was changed.");
    if (ammoKind < 0 || itemState == 1)
        return Reject("AMMO_REFILL_BUSY", "The current item resource configuration is not ready. Nothing was queued.");
    if (resources >= capacity)
        return {true, "AMMO_REFILL_ALREADY_FULL", "Test ammo already full: " + std::to_string(resources) +
            "/" + std::to_string(capacity) + ". No native refill was called."};
    if (!Writable(reinterpret_cast<void*>(before.item + 0xD0U), 0x20U) ||
        !Writable(reinterpret_cast<void*>(before.item + 0x140U), 1U))
        return Reject("AMMO_REFILL_TARGET_INVALID", "The native item resource storage is not writable.");

    // All native calls below may issue synchronous engine callbacks. Do not
    // retain a session pointer, reuse permission results, or retry a partial call.
    ResetPermissions();
    if (!sameContext() || !currentSelectionMatches(before))
        return Reject("AMMO_REFILL_TARGET_CHANGED", "The Character or selected equipment changed before refill. Nothing was changed.");
    const bool completed = RefillNativeProtected(before.item, ammoKind);
    ResetPermissions();
    if (!completed) {
        if (auto* live = FindSession(character); live != nullptr &&
            live->birth.epoch == snapshot.birth.epoch && live->controller == snapshot.controller)
            Fault(*live, "manual_ammo_refill_native_fault");
        return Reject("AMMO_REFILL_NATIVE_FAULT", "The native refill faulted. It was not retried; the item may have been partially modified.");
    }
    Frame after{};
    if (!sameContext() || !ReadFrame(snapshot, after) || !sameContext() ||
        !currentSelectionMatches(after) || !SameFrameEquipment(before, after))
        return Reject("AMMO_REFILL_TARGET_CHANGED", "The native refill returned but its target changed during callbacks. Result not claimed; no retry.");
    std::int32_t actual{}, accounting{}, capacityAfter{}, kindAfter{};
    if (!Read(after.item, 0x28U, owner) || owner != character ||
        !Read(after.item, 0x2CU, ownerId) || ownerId != snapshot.birth.runtimeId ||
        !Read(after.item, 0xECU, capacityAfter) || capacityAfter != capacity ||
        !Read(after.item, 0xD0U, kindAfter) || kindAfter != ammoKind ||
        !Read(after.item, 0xE0U, actual) || !Read(after.item, 0xE4U, accounting) ||
        actual != capacity || accounting != capacity)
        return Reject("AMMO_REFILL_NOT_CONFIRMED", "The refill returned but the native resources/capacity could not be confirmed. No automatic retry.");

    try {
        if (gRuntime.diagnostics != nullptr)
            gRuntime.diagnostics->Log("info", "character_action_ammo_refill",
                "Explicit manual test refill completed; native capacity is unchanged.",
                {{"character", Diagnostics::Pointer(reinterpret_cast<void*>(character))},
                 {"item", Diagnostics::Pointer(reinterpret_cast<void*>(after.item))},
                 {"generation", Diagnostics::Unsigned(snapshot.birth.generation), true},
                 {"spawnEpoch", Diagnostics::Unsigned(snapshot.birth.epoch), true},
                 {"resourcesBefore", std::to_string(resources), true},
                 {"resourcesAfter", std::to_string(actual), true},
                 {"nativeResourceCapacity", std::to_string(capacity), true},
                 {"policy", "manual_native_capacity"}});
    } catch (...) { /* successful native work must not fail due to logging */ }
    return {true, "AMMO_REFILL_COMPLETED", "Test ammo refilled: " + std::to_string(resources) +
        " -> " + std::to_string(actual) + "; native capacity=" + std::to_string(capacity) +
        ". No automatic refill or Reload action. Request Primary START explicitly to fire again."};
}

void ObserveCreated(const std::uintptr_t character, const std::uint32_t spawnKey,
    const std::uint64_t generation, const bool aimpOwned) noexcept {
    try {
        std::lock_guard lock(gRuntime.mutex);
        if (!gRuntime.enabled || character == 0U) return;
        GenerationLocked(generation);
        ForgetLocked(character); // protects against same-address reuse, including non-AIMP births
        if (!aimpOwned || gRuntime.stopping) return;
        std::uint32_t runtimeId{};
        if (!Read(character, 0x50U, runtimeId)) return;
        for (auto& birth : gRuntime.births) {
            if (birth.character == 0U) {
                birth = {character, spawnKey, runtimeId, generation, gRuntime.nextEpoch++};
                return;
            }
        }
    } catch (...) { }
}

void ForgetCharacter(const std::uintptr_t character) noexcept {
    try { std::lock_guard lock(gRuntime.mutex); ForgetLocked(character); } catch (...) { }
}

void ForgetSpawnKey(const std::uint32_t spawnKey) noexcept {
    try {
        std::lock_guard lock(gRuntime.mutex);
        for (const auto& birth : gRuntime.births)
            if (birth.character != 0U && birth.spawnKey == spawnKey) ForgetLocked(birth.character);
    } catch (...) { }
}

void InvalidateGeneration(const std::uint64_t generation) noexcept {
    try { std::lock_guard lock(gRuntime.mutex); GenerationLocked(generation); } catch (...) { }
}

void RequestStopAll() noexcept {
    try {
        std::lock_guard lock(gRuntime.mutex);
        if (gRuntime.stopping) return;
        gRuntime.stopping = true;
        for (auto& session : gRuntime.sessions) {
            if (!session.used) continue;
            if (session.phase == Phase::Idle && !session.aimRequested &&
                !session.aimApplied && !session.aimStopPending && !session.primaryApplied)
                continue;
            session.aimStopPending = session.aimStopPending || session.aimRequested || session.aimApplied;
            session.aimRequested = false;
            session.primaryRequested = false;
            ++session.revision;
            BeginRelease(session, "runtime_shutdown_requested");
        }
        // Stable Aim sessions are normally absent from the detour mask; shutdown
        // must subscribe them again so their transition-only STOP is consumed.
        PublishControllerHintsLocked();
    } catch (...) { }
}

bool InputsReleased() noexcept {
    try { std::lock_guard lock(gRuntime.mutex); return AllReleasedLocked(); }
    catch (...) { return false; }
}

bool BeginLogicalShutdown() noexcept {
    RequestStopAll();
    try {
        std::lock_guard lock(gRuntime.mutex);
        gRuntime.diagnostics = nullptr;
        gRuntime.detached = true;
        const bool pending = !AllReleasedLocked();
        if (!pending) {
            gRuntime.enabled = false;
            gRuntime.generations = nullptr;
            gRuntime.dispatcher = nullptr;
            PublishControllerHintsLocked();
        }
        return pending;
    } catch (...) { return true; }
}

} // namespace aimp::character_action_runtime
