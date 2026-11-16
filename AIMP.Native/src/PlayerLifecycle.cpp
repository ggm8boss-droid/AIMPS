#include "aimp/PlayerLifecycle.h"

#include "aimp/BuildProfile.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/PlayerSpectatePolicy.h"
#include "aimp/SignatureResolver.h"
#include "aimp/WorldPublicActions.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
#include <cstdint>
#include <mutex>
#include <optional>

namespace aimp::player_lifecycle {
namespace {

enum class Decision { Suppress, NoSession, NativeMode, ContextUnavailable };

struct DeathEvent {
    std::uintptr_t character{};
    std::int32_t gameplayState{-1};
    std::int32_t gameplayMode{-1};
    std::uint16_t hp{};
    std::uint32_t healthState{};
    bool healthReadable{};
    Decision decision{Decision::ContextUnavailable};
};

std::atomic<bool> gInstalled{false};
std::atomic<bool> gEnabled{false};
std::atomic<std::uint64_t> gLocalDeathSequence{0};
std::atomic<std::uint64_t> gLocalLevelExitSequence{0};
// Diagnostics only: last observed level-exit decision, drained on the worker.
std::atomic<std::uint64_t> gLevelExitObserved{0}, gLevelExitAllowed{0}, gLevelExitDrained{0};
std::atomic<std::uint32_t> gLevelExitLastPrimitive{0};
std::atomic<std::int32_t> gLevelExitLastMode{-1};
std::atomic<std::uint64_t> gMissionCompleteObserved{0}, gMissionCompleteDrained{0};
// Spectate: the suppressed local death (death boundary) and the published view.
std::atomic<std::uintptr_t> gSpectateVictim{0};
std::atomic<std::uint64_t> gSpectateDiedAt{0};
std::atomic<std::uint64_t> gSpectateTarget{0};
std::atomic<std::uint32_t> gSpectateCandidates{0};
std::atomic<std::uint64_t> gSpectateChanges{0}, gSpectateChangesDrained{0};
std::atomic<std::uint64_t> gSpectateBindFailures{0}, gSpectateBindFailuresDrained{0};
std::atomic<std::uint64_t> gSpectateNoCandidates{0}, gSpectateNoCandidatesDrained{0};
std::atomic<std::uint64_t> gSpectateCameraAttempts{0}, gSpectateCameraAttemptsDrained{0};
// Living participants that can be watched, refreshed every engine pump, dead or alive.
std::atomic<std::uint32_t> gSpectateLivingTeammates{0};
std::atomic<std::uint64_t> gDeathFadeSuppressed{0}, gDeathFadeSuppressedDrained{0};
std::atomic<bool> gLocalPlayerDown{false};
std::atomic<std::uint64_t> gDownEpisodes{0};
// Diagnostics: distinct script effects executed while the local player is down.
struct DownEffect {
    std::uint32_t method{}, entity{}, type{}, primitive{};
    bool operator==(const DownEffect&) const noexcept = default;
};
std::mutex gDownEffectMutex;
std::array<DownEffect, 48> gDownEffects{};
std::size_t gDownEffectCount{};
std::uint64_t gDownEffectEpisode{};
const void* gGlobalRootAddress{};
// Published before HookManager resumes patched threads. Neither destination
// is cleared at logical shutdown: an in-flight naked detour still needs it.
void* gRetailTrampoline{};
const void* gRetailEpilogue{};
std::mutex gEventMutex;
std::array<DeathEvent, 8> gEvents{};
std::size_t gEventCount{};
std::uint64_t gDroppedEvents{};

void RecordEvent(const DeathEvent& event) noexcept {
    std::lock_guard lock(gEventMutex);
    if (gEventCount < gEvents.size()) {
        gEvents[gEventCount++] = event;
    } else {
        ++gDroppedEvents;
    }
}

// This is called by the interior x86 stub, not as a Character virtual.
// At 007B8AAD, EBP still holds the original ECX Character and both definition
// flags (+1C/+1F) already passed. The last cleanup store is at 007B8A95.
int __stdcall DecideLocalDeath(const std::uintptr_t victim) noexcept {
    if (!gEnabled.load(std::memory_order_acquire)) return 0;

    const auto root = crash_guard::SafeRead<std::uintptr_t>(gGlobalRootAddress);
    if (!root || *root == 0U) {
        RecordEvent({victim});
        return 0;
    }
    const auto context = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*root + 0x08U));
    if (!context || *context == 0U) {
        RecordEvent({victim});
        return 0;
    }
    // Same THE_PLAYER resolution as FUN_0078BAF0 and PlayerSyncObserver;
    // no cached equipment, spawn result, appearance or NPC class heuristic.
    const auto count = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(*context + 0x84U));
    const auto players = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*context + 0x80U));
    if (!count || *count == 0U || !players || *players == 0U) {
        RecordEvent({victim});
        return 0;
    }
    const auto localPlayer = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*players));
    if (!localPlayer || *localPlayer == 0U) {
        RecordEvent({victim});
        return 0;
    }
    if (*localPlayer != victim) return 0;

    DeathEvent event{};
    event.character = victim;
    const auto hp = crash_guard::SafeRead<std::uint16_t>(
        reinterpret_cast<const void*>(victim + 0x33CU));
    const auto health = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(victim + 0x32CU));
    event.healthReadable = hp.has_value() && health.has_value();
    if (event.healthReadable) {
        event.hp = *hp;
        event.healthState = *health;
    }

    const auto manager = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*root + 0x48U));
    if (!manager || *manager == 0U) {
        RecordEvent(event);
        return 0;
    }
    const auto state = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(*manager + 0x08U));
    const auto mode = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(*manager + 0x14U));
    if (!state || !mode) {
        RecordEvent(event);
        return 0;
    }
    event.gameplayState = *state;
    event.gameplayMode = *mode;

    // SessionConfig alone is only launcher bootstrap. The existing native
    // authority is bound by a real Host/Client lobby and revoked on disconnect.
    // Native mode also excludes frontend/inactive gameplay exactly as retail.
    // WorldBusActive/AISync/PlayerSync toggles are deliberately irrelevant.
    if (!world_actions::SessionAuthorityBound()) {
        event.decision = Decision::NoSession;
    } else if (*mode == 0 || *mode == 3) {
        event.decision = Decision::NativeMode;
    } else {
        event.decision = Decision::Suppress;
    }
    RecordEvent(event);
    if (event.decision != Decision::Suppress) return 0;
    gSpectateDiedAt.store(GetTickCount64(), std::memory_order_relaxed);
    gSpectateVictim.store(victim, std::memory_order_release);
    gLocalDeathSequence.fetch_add(1U, std::memory_order_acq_rel);
    return 1;
}

// ---------- spectate ----------
// alien-isolation-coop writes the partner's pose into the KillCamera records
// it finds by scanning memory. Here the remote participant is a real
// Character, so the retail follow camera is retargeted to it instead, with the
// same certified camera calls as AlienPvPLocalController (no process scan).
constexpr std::uintptr_t kCameraGlobalsRva = 0x012F0C88U;
constexpr std::uintptr_t kCameraSetEntityRva = 0x00029220U;
constexpr std::uintptr_t kCameraActivateRva = 0x00032E10U;
constexpr std::uintptr_t kPauseManagerRva = 0x012F194CU;
constexpr std::uintptr_t kCharacterSkeletonOffset = 0x718U;
// Over-the-shoulder on a standing human: position (side, up, back), then aim
// (side, up, ahead). Same FollowBehavior layout as the Alien PvP camera.
constexpr std::array<float, 6> kSpectateFollow{0.4F, 1.55F, -2.2F, 0.0F, 1.35F, 2.0F};

struct SpectateCamera {
    std::uintptr_t manager{}, follow{}, behavior{};
    std::array<float, 6> originalFollow{};
    std::uint8_t originalFirstPerson{};
    bool saved{};
};
struct SpectateRuntime {
    std::uintptr_t victim{};
    std::uint32_t generation{};
    std::uint64_t diedAt{};
    std::uint64_t target{};
    std::uintptr_t targetCharacter{};
    std::uint64_t nextAssertMs{};
    SpectateCamera camera{};
    bool leftDown{}, rightDown{};
};
SpectateRuntime gSpectate{}; // certified engine thread only

template <typename T>
bool ReadAt(const std::uintptr_t at, T& out) noexcept {
    return at != 0U && crash_guard::TryCopy(&out, reinterpret_cast<const void*>(at), sizeof(T));
}
template <typename T>
bool WriteAt(const std::uintptr_t at, const T& value) noexcept {
    return at != 0U && crash_guard::TryCopy(reinterpret_cast<void*>(at), &value, sizeof(T));
}

constexpr std::uint32_t BehaviorNameHash(const char* name, const std::size_t size) noexcept {
    std::uint32_t h{};
    for (std::size_t i = 0U, j = 0U; i < 256U; ++i, j = (j + 1U) % size)
        h += static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[j])) << (i & 31U);
    return h;
}

std::uintptr_t FindFollowBehavior(const std::uintptr_t camera) noexcept {
    std::uintptr_t array{};
    std::uint32_t count{};
    if (!ReadAt(camera + 0xBCU, array) || !array || !ReadAt(camera + 0xC0U, count) || count > 256U) return 0U;
    for (std::uint32_t i = 0U; i < count; ++i) {
        std::uintptr_t behavior{};
        std::uint32_t type{};
        if (ReadAt(array + i * sizeof(std::uintptr_t), behavior) && behavior &&
            ReadAt(behavior + 8U, type) && type == BehaviorNameHash("FollowBehavior", 14U)) return behavior;
    }
    return 0U;
}

bool CurrentFollowCamera(const std::uintptr_t base, std::uintptr_t& manager, std::uintptr_t& follow) noexcept {
    std::uintptr_t root{};
    return base != 0U && ReadAt(base + kCameraGlobalsRva, root) && root &&
        ReadAt(root + 0x58U, manager) && manager && ReadAt(manager + 0x1DCU, follow) && follow;
}

bool ActivateFollowOn(const std::uintptr_t base, const SpectateCamera& camera,
    const std::uintptr_t character, const bool activate) noexcept {
    std::uintptr_t skeleton{};
    if (!base || !camera.manager || !camera.follow || !character ||
        !ReadAt(character + kCharacterSkeletonOffset, skeleton) || !skeleton) return false;
#if defined(_MSC_VER)
    __try {
        reinterpret_cast<void(__thiscall*)(void*, void*)>(base + kCameraSetEntityRva)(
            reinterpret_cast<void*>(camera.follow), reinterpret_cast<void*>(character));
        if (activate)
            static_cast<void>(reinterpret_cast<unsigned(__thiscall*)(void*, void*)>(base + kCameraActivateRva)(
                reinterpret_cast<void*>(camera.manager), reinterpret_cast<void*>(camera.follow)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}

bool BindSpectateCamera(const std::uintptr_t base, SpectateCamera& camera) noexcept {
    std::uintptr_t manager{}, follow{};
    if (!CurrentFollowCamera(base, manager, follow)) return false;
    if (camera.saved && (camera.manager != manager || camera.follow != follow)) camera = {};
    camera.manager = manager;
    camera.follow = follow;
    const auto behavior = FindFollowBehavior(follow);
    if (!behavior) return false;
    if (!camera.saved) {
        if (!ReadAt(behavior + 0x9CU, camera.originalFollow) ||
            !ReadAt(behavior + 0xF6U, camera.originalFirstPerson)) return false;
        camera.behavior = behavior;
        camera.saved = true;
    }
    return camera.behavior == behavior && WriteAt(behavior + 0x9CU, kSpectateFollow) &&
        WriteAt(behavior + 0xF6U, std::uint8_t{0});
}

// Same world only: put the follow camera back as retail left it. The dead
// local player keeps whatever script camera the game activates next.
void ReleaseSpectateCamera(const std::uintptr_t base, const bool sameWorld) noexcept {
    auto& camera = gSpectate.camera;
    std::uintptr_t manager{}, follow{};
    if (sameWorld && camera.saved && CurrentFollowCamera(base, manager, follow) &&
        manager == camera.manager && follow == camera.follow && FindFollowBehavior(follow) == camera.behavior) {
        static_cast<void>(WriteAt(camera.behavior + 0x9CU, camera.originalFollow));
        static_cast<void>(WriteAt(camera.behavior + 0xF6U, camera.originalFirstPerson));
        if (gSpectate.victim) static_cast<void>(ActivateFollowOn(base, camera, gSpectate.victim, false));
    }
    camera = {};
    gSpectate.target = 0U;
    gSpectate.targetCharacter = 0U;
}

bool SpectateInputAllowed(const std::uintptr_t base) noexcept {
    DWORD process{};
    const auto foreground = GetForegroundWindow();
    if (!foreground || !GetWindowThreadProcessId(foreground, &process) || process != GetCurrentProcessId())
        return false;
    std::uintptr_t pauseManager{};
    std::uint32_t pauseReasons{};
    return !(ReadAt(base + kPauseManagerRva, pauseManager) && pauseManager &&
        ReadAt(pauseManager + 4U, pauseReasons) && (pauseReasons & 0x00012000U) != 0U);
}

int SpectateStep(const std::uintptr_t base) noexcept {
    const bool allowed = SpectateInputAllowed(base);
    const bool left = allowed && (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
    const bool right = allowed && (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    const int step = (left && !gSpectate.leftDown ? -1 : 0) + (right && !gSpectate.rightDown ? 1 : 0);
    gSpectate.leftDown = left;
    gSpectate.rightDown = right;
    return step;
}

void RecordDownEffect(const std::uint32_t method, const std::uint32_t entity,
    const std::uint32_t type, const std::uint32_t primitive) noexcept {
    const DownEffect effect{method, entity, type, primitive};
    std::lock_guard lock(gDownEffectMutex);
    const auto episode = gDownEpisodes.load(std::memory_order_relaxed);
    if (episode != gDownEffectEpisode) { gDownEffectEpisode = episode; gDownEffectCount = 0U; }
    for (std::size_t i = 0U; i < gDownEffectCount; ++i) if (gDownEffects[i] == effect) return;
    if (gDownEffectCount < gDownEffects.size()) gDownEffects[gDownEffectCount++] = effect;
}

void PublishSpectate(const std::uint64_t target, const std::uint32_t candidates) noexcept {
    gSpectateCandidates.store(candidates, std::memory_order_relaxed);
    if (gSpectateTarget.exchange(target, std::memory_order_acq_rel) != target)
        gSpectateChanges.fetch_add(1U, std::memory_order_relaxed);
}

std::optional<std::int32_t> ReadGameplayMode() noexcept {
    const auto root = crash_guard::SafeRead<std::uintptr_t>(gGlobalRootAddress);
    if (!root || *root == 0U) return std::nullopt;
    const auto manager = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*root + 0x48U));
    if (!manager || *manager == 0U) return std::nullopt;
    return crash_guard::SafeRead<std::int32_t>(reinterpret_cast<const void*>(*manager + 0x14U));
}

#if defined(_MSC_VER) && defined(_M_IX86)
__declspec(naked) void LocalDeathBoundaryDetour() {
    __asm {
        pushfd
        pushad
        // Preserve x87/MMX/XMM/MXCSR as well as all registers and EFLAGS.
        // ESI is callee-saved by DecideLocalDeath and holds the saved stack.
        mov esi, esp
        sub esp, 0210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push ebp
        call DecideLocalDeath
        test eax, eax
        jz retail

        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        jmp dword ptr [gRetailEpilogue]

    retail:
        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        // Replay the complete displaced A1 imm32, then continue at 007B8AB2.
        jmp dword ptr [gRetailTrampoline]
    }
}
#endif

} // namespace

bool Install(const BuildProfile& profile, const SignatureResolver& resolver,
    HookManager& hooks, const bool exactExecutableHash, std::string& error) {
    if (Installed()) return true;
    if (!exactExecutableHash || !resolver.ExactBuildValidated()) {
        error = "PlayerLifecycle requires the SHA-256-certified executable and fixed-RVA profile.";
        return false;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    static_assert(sizeof(void*) == 4U, "PlayerLifecycle requires the x86 ABI.");
    const auto* boundary = profile.Find(SymbolId::CharacterDeathSoloGameOver);
    const auto* epilogue = resolver.Resolve(SymbolId::CharacterDeathSoloGameOverEpilogue);
    const auto* target = resolver.Resolve(SymbolId::CharacterDeathSoloGameOver);
    const auto* root = resolver.Resolve(SymbolId::GlobalStateRoot);
    if (boundary == nullptr || target == nullptr || epilogue == nullptr || root == nullptr ||
        boundary->approvedPatchLength != 5U || !boundary->trampolineRelocationSafe) {
        error = "PlayerLifecycle death boundary, epilogue or global-root signature is unavailable.";
        return false;
    }
    // A1's absolute operand is masked for PE rebasing, but its loaded value
    // must still point to the exact GlobalStateRoot resolved by the profile.
    const auto operand = crash_guard::SafeRead<std::uintptr_t>(
        static_cast<const std::byte*>(target) + 1U);
    if (!operand || *operand != reinterpret_cast<std::uintptr_t>(root)) {
        error = "PlayerLifecycle displaced MOV does not reference GlobalStateRoot.";
        return false;
    }
    gGlobalRootAddress = root;
    gRetailEpilogue = epilogue;
    gEnabled.store(true, std::memory_order_release);
    HookRequest request{};
    request.name = "PlayerLifecycle.LocalDeathSoloGameOver";
    request.target = const_cast<void*>(target);
    request.detour = reinterpret_cast<void*>(&LocalDeathBoundaryDetour);
    request.patchLength = boundary->approvedPatchLength;
    request.expected = boundary->expected;
    request.relocationSafePrefix = boundary->trampolineRelocationSafe;
    request.trampolineOut = &gRetailTrampoline;
    if (!hooks.Install(request, error)) {
        gEnabled.store(false, std::memory_order_release);
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    return true;
#else
    (void)profile;
    (void)hooks;
    error = "PlayerLifecycle requires MSVC x86 interior-hook support.";
    return false;
#endif
}

bool Installed() noexcept {
    return gInstalled.load(std::memory_order_acquire);
}

std::uint64_t LocalDeathSequence() noexcept {
    return gLocalDeathSequence.load(std::memory_order_acquire);
}

// Same session/mode contract as the death boundary: only an AIMP gameplay
// session suppresses the retail transition; solo play and frontend keep it.
bool DecideLevelExit(const std::uint32_t primitive) noexcept {
    if (!gEnabled.load(std::memory_order_acquire) || !LevelExitPrimitive(primitive)) return false;
    const auto mode = ReadGameplayMode();
    gLevelExitObserved.fetch_add(1U, std::memory_order_relaxed);
    gLevelExitLastPrimitive.store(primitive, std::memory_order_relaxed);
    gLevelExitLastMode.store(mode.value_or(-1), std::memory_order_relaxed);
    if (!world_actions::SessionAuthorityBound() || !mode || *mode == 0 || *mode == 3) {
        gLevelExitAllowed.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    gLocalLevelExitSequence.fetch_add(1U, std::memory_order_acq_rel);
    return true;
}

std::uint64_t LocalLevelExitSequence() noexcept {
    return gLocalLevelExitSequence.load(std::memory_order_acquire);
}

void PumpSpectateOnEngineThread(const SpectateContext& context) noexcept {
    const auto teammates = context.cameraReserved ? 0U
        : static_cast<std::uint32_t>(std::min(context.candidateCount, spectate::kMaxCandidates));
    gSpectateLivingTeammates.store(teammates, std::memory_order_relaxed);
    {
        // Down = dying in a kill (health state >= 3 before the death boundary)
        // or already dead; the same fields DecideLocalDeath records.
        std::uint16_t hp{};
        std::uint32_t health{};
        const bool dying = context.localPlayer != 0U && ReadAt(context.localPlayer + 0x33CU, hp) &&
            ReadAt(context.localPlayer + 0x32CU, health) && (hp == 0U || health >= 3U);
        const bool down = teammates != 0U &&
            (dying || gSpectateVictim.load(std::memory_order_acquire) != 0U);
        if (gLocalPlayerDown.exchange(down, std::memory_order_relaxed) != down && down)
            gDownEpisodes.fetch_add(1U, std::memory_order_relaxed);
    }
    auto& s = gSpectate;
    const auto victim = gSpectateVictim.load(std::memory_order_acquire);
    if (victim != 0U && victim != s.victim) {
        // A new suppressed death. Any previous view belonged to another world.
        s = {};
        s.victim = victim;
        s.generation = context.nativeGeneration;
        s.diedAt = gSpectateDiedAt.load(std::memory_order_relaxed);
    }
    if (s.victim == 0U) return;
    const bool sameWorld = context.nativeGeneration == s.generation;
    if (!gEnabled.load(std::memory_order_acquire) || !sameWorld ||
        (context.localPlayer != 0U && context.localPlayer != s.victim)) {
        // Level change, reload or shutdown: the dead body is gone, stop for good.
        ReleaseSpectateCamera(context.moduleBase, sameWorld && gEnabled.load(std::memory_order_acquire));
        auto expected = s.victim;
        gSpectateVictim.compare_exchange_strong(expected, 0U, std::memory_order_acq_rel);
        s = {};
        PublishSpectate(0U, 0U);
        return;
    }

    std::array<std::uint64_t, spectate::kMaxCandidates> peers{};
    const auto count = std::min(context.candidateCount, spectate::kMaxCandidates);
    for (std::size_t i = 0U; i < count; ++i) peers[i] = context.candidates[i].peer;
    const auto candidates = static_cast<std::uint32_t>(count);
    if (context.cameraReserved) {
        if (s.target != 0U) ReleaseSpectateCamera(context.moduleBase, true);
        PublishSpectate(0U, candidates);
        return;
    }
    const auto now = GetTickCount64();
    const int step = SpectateStep(context.moduleBase);
    if (now - s.diedAt < spectate::kStartDelayMs) {
        PublishSpectate(0U, candidates);
        return;
    }
    const auto next = step != 0 ? spectate::Cycle(s.target, peers.data(), count, step)
                                : spectate::KeepOrNext(s.target, peers.data(), count);
    if (next == 0U) {
        // A dead player without a certified camera target previously failed
        // silently, leaving the game on its retail black death screen.
        gSpectateNoCandidates.fetch_add(1U, std::memory_order_relaxed);
        PublishSpectate(s.target, 0U);
        return;
    }
    std::uintptr_t character{};
    for (std::size_t i = 0U; i < count; ++i)
        if (context.candidates[i].peer == next) character = context.candidates[i].character;
    const bool changed = next != s.target || character != s.targetCharacter;
    if (changed || now >= s.nextAssertMs) {
        std::uintptr_t active{};
        const bool owned = !changed && s.camera.saved &&
            ReadAt(s.camera.manager + 0x1D8U, active) && active == s.camera.follow;
        if (!owned) {
            gSpectateCameraAttempts.fetch_add(1U, std::memory_order_relaxed);
            if (BindSpectateCamera(context.moduleBase, s.camera) &&
                ActivateFollowOn(context.moduleBase, s.camera, character, true)) {
                s.target = next;
                s.targetCharacter = character;
            } else {
                gSpectateBindFailures.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        s.nextAssertMs = now + spectate::kReassertMs;
    }
    PublishSpectate(s.target, candidates);
}

SpectateState SpectateSnapshot() noexcept {
    return {gSpectateTarget.load(std::memory_order_acquire), gSpectateCandidates.load(std::memory_order_relaxed)};
}

bool LocalPlayerDown() noexcept {
    return gLocalPlayerDown.load(std::memory_order_relaxed);
}

bool SuppressDeathPresentation(const std::uint32_t method, const std::uint32_t entity,
    const std::uint32_t type, const std::uint32_t primitive) noexcept {
    // ShortGuids of the CATHODE ScreenFadeOutTo{Black,White}[Timed] function
    // types and their AV variants (CathodeLib algorithm, checked against
    // SwitchLevel/GoToFrontend/trigger/TriggerDeath). An Alien kill fades
    // through these, not through Health_Damage_Death.
    constexpr std::array<std::uint32_t, 8> kScreenFadeOut{
        0x9BD3BC8EU, 0x3558D11EU, 0xE6BB387AU, 0x5513C887U,
        0x9EB7BC86U, 0xF3C51273U, 0x03D53481U, 0x421953F3U};
    const bool down = gLocalPlayerDown.load(std::memory_order_relaxed);
    if (down) RecordDownEffect(method, entity, type, primitive);
    const auto isFade = [&](const std::uint32_t value) noexcept {
        return value != 0U && std::find(kScreenFadeOut.begin(), kScreenFadeOut.end(), value) != kScreenFadeOut.end();
    };
    if (down && (isFade(primitive) || isFade(type)) && gEnabled.load(std::memory_order_acquire) &&
        world_actions::SessionAuthorityBound()) {
        const auto mode = ReadGameplayMode();
        if (mode && *mode != 0 && *mode != 3) {
            gDeathFadeSuppressed.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
    }
    // IDs from alien-isolation-coop (coopnet.c swallow_cb), same retail build:
    // TriggerDeath on the Health_Damage_Death composite (fade to black,
    // vignette, colour), and "Death DOF".."Death DOF_3"/"Time to Death" in
    // Global\systemic_post_processing. The DOF starts ~0.3 s before the death,
    // so the rule depends on a watchable teammate, not on our death state.
    constexpr std::uint32_t kTriggerDeath = 0xAE498037U, kHealthDamageDeath = 0x3F4DEFE1U;
    constexpr std::uint32_t kDeathDofMethod = 0x830C2104U;
    constexpr std::array<std::uint32_t, 5> kDeathDof{0xBCF22095U, 0xAF7FD498U, 0x97E51B56U, 0xBCD40132U, 0x006BBDB7U};
    const bool effect = (method == kTriggerDeath && type == kHealthDamageDeath) ||
        (method == kDeathDofMethod && std::find(kDeathDof.begin(), kDeathDof.end(), entity) != kDeathDof.end());
    if (!effect || !gEnabled.load(std::memory_order_acquire) || !world_actions::SessionAuthorityBound() ||
        gSpectateLivingTeammates.load(std::memory_order_relaxed) == 0U) return false;
    const auto mode = ReadGameplayMode();
    if (!mode || *mode == 0 || *mode == 3) return false;
    gDeathFadeSuppressed.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

void ObserveMissionComplete() noexcept {
    if (!gEnabled.load(std::memory_order_acquire) || !world_actions::SessionAuthorityBound()) return;
    const auto mode = ReadGameplayMode();
    if (!mode || *mode == 0 || *mode == 3) return;
    gMissionCompleteObserved.fetch_add(1U, std::memory_order_relaxed);
    gLocalLevelExitSequence.fetch_add(1U, std::memory_order_acq_rel);
}

void Shutdown() noexcept {
    gEnabled.store(false, std::memory_order_release);
}

void DrainDiagnostics(Diagnostics& diagnostics) {
    std::array<DeathEvent, 8> events{};
    std::size_t count{};
    std::uint64_t dropped{};
    {
        std::lock_guard lock(gEventMutex);
        count = gEventCount;
        events = gEvents;
        dropped = gDroppedEvents;
        gEventCount = 0U;
        gDroppedEvents = 0U;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto& event = events[index];
        if (event.decision == Decision::ContextUnavailable) {
            diagnostics.Log("error", "playerlifecycle_context_unavailable",
                "[PlayerLifecycle] death boundary context unavailable; retail path preserved.",
                {{"character", Diagnostics::Pointer(reinterpret_cast<const void*>(event.character))}});
            continue;
        }
        diagnostics.Log("info", "playerlifecycle_local_death_boundary",
            "[PlayerLifecycle] local death boundary reached",
            {{"character", Diagnostics::Pointer(reinterpret_cast<const void*>(event.character))},
             {"healthReadable", Diagnostics::Boolean(event.healthReadable), true},
             {"hp", Diagnostics::Unsigned(event.hp), true},
             {"healthState", Diagnostics::Unsigned(event.healthState), true},
             {"gameplayStateBefore", std::to_string(event.gameplayState), true},
             {"gameplayMode", std::to_string(event.gameplayMode), true}});
        diagnostics.Log("info",
            event.decision == Decision::Suppress
                ? "playerlifecycle_solo_gameover_suppressed" : "playerlifecycle_retail_gameover_allowed",
            event.decision == Decision::Suppress
                ? "[PlayerLifecycle] solo GameOver suppressed"
                : (event.decision == Decision::NoSession
                    ? "[PlayerLifecycle] retail GameOver allowed: no active AIMP session"
                    : "[PlayerLifecycle] retail path allowed: native gameplay mode inactive"));
    }
    const auto observed = gLevelExitObserved.load(std::memory_order_relaxed);
    if (observed != gLevelExitDrained.exchange(observed, std::memory_order_relaxed)) {
        diagnostics.Log("info", "playerlifecycle_level_exit",
            "[PlayerLifecycle] retail level exit (GoToFrontend/SwitchLevel) observed",
            {{"observed", Diagnostics::Unsigned(observed), true},
             {"suppressed", Diagnostics::Unsigned(LocalLevelExitSequence()), true},
             {"retailAllowed", Diagnostics::Unsigned(gLevelExitAllowed.load(std::memory_order_relaxed)), true},
             {"lastPrimitive", Diagnostics::Unsigned(gLevelExitLastPrimitive.load(std::memory_order_relaxed)), true},
             {"lastGameplayMode", std::to_string(gLevelExitLastMode.load(std::memory_order_relaxed)), true}});
    }
    const auto completed = gMissionCompleteObserved.load(std::memory_order_relaxed);
    if (completed != gMissionCompleteDrained.exchange(completed, std::memory_order_relaxed)) {
        diagnostics.Log("info", "playerlifecycle_mission_complete",
            "[PlayerLifecycle] local terminal HUD trigger executed; completion reported before Flash dispatch.",
            {{"observed", Diagnostics::Unsigned(completed), true},
             {"levelExitSequence", Diagnostics::Unsigned(LocalLevelExitSequence()), true}});
    }
    const auto spectateChanges = gSpectateChanges.load(std::memory_order_relaxed);
    if (spectateChanges != gSpectateChangesDrained.exchange(spectateChanges, std::memory_order_relaxed)) {
        const auto state = SpectateSnapshot();
        diagnostics.Log("info", "playerlifecycle_spectate",
            state.targetPeer != 0U ? "[PlayerLifecycle] spectating a living participant"
                                   : "[PlayerLifecycle] spectate stopped",
            {{"targetPeer", Diagnostics::Unsigned(state.targetPeer), true},
             {"candidates", Diagnostics::Unsigned(state.candidates), true},
             {"changes", Diagnostics::Unsigned(spectateChanges), true}});
    }
    {
        std::array<DownEffect, 48> effects{};
        std::size_t effectCount{};
        std::uint64_t episode{};
        {
            std::lock_guard lock(gDownEffectMutex);
            effects = gDownEffects;
            effectCount = gDownEffectCount;
            episode = gDownEffectEpisode;
        }
        static std::uint64_t drainedEpisode{};
        static std::size_t drainedCount{};
        if (episode != drainedEpisode) { drainedEpisode = episode; drainedCount = 0U; }
        const auto hex = [](const std::uint32_t value) {
            char text[11]{};
            std::snprintf(text, sizeof(text), "0x%08X", value);
            return std::string(text);
        };
        for (; drainedCount < effectCount; ++drainedCount) {
            const auto& effect = effects[drainedCount];
            diagnostics.Log("info", "playerlifecycle_down_effect",
                "[PlayerLifecycle] script effect executed while the local player is down",
                {{"episode", Diagnostics::Unsigned(episode), true},
                 {"method", hex(effect.method)},
                 {"entity", hex(effect.entity)},
                 {"type", hex(effect.type)},
                 {"primitive", hex(effect.primitive)}});
        }
    }
    const auto fades = gDeathFadeSuppressed.load(std::memory_order_relaxed);
    if (fades != gDeathFadeSuppressedDrained.exchange(fades, std::memory_order_relaxed)) {
        diagnostics.Log("info", "playerlifecycle_death_presentation_suppressed",
            "[PlayerLifecycle] death fade/DOF suppressed so the spectate view stays visible",
            {{"suppressed", Diagnostics::Unsigned(fades), true}});
    }
    const auto noCandidates = gSpectateNoCandidates.load(std::memory_order_relaxed);
    if (noCandidates != gSpectateNoCandidatesDrained.exchange(noCandidates, std::memory_order_relaxed)) {
        diagnostics.Log("warning", "playerlifecycle_spectate_no_candidate",
            "[PlayerLifecycle] dead player has no qualified living spectator target; camera remains unbound.",
            {{"frames", Diagnostics::Unsigned(noCandidates), true},
             {"cameraAttempts", Diagnostics::Unsigned(gSpectateCameraAttempts.load(std::memory_order_relaxed)), true}});
    }
    const auto attempts = gSpectateCameraAttempts.load(std::memory_order_relaxed);
    if (attempts != gSpectateCameraAttemptsDrained.exchange(attempts, std::memory_order_relaxed)) {
        const auto state = SpectateSnapshot();
        diagnostics.Log("info", "playerlifecycle_spectate_camera_attempts",
            "[PlayerLifecycle] spectate camera binding attempts and current state.",
            {{"attempts", Diagnostics::Unsigned(attempts), true},
             {"targetPeer", Diagnostics::Unsigned(state.targetPeer), true},
             {"candidates", Diagnostics::Unsigned(state.candidates), true}});
    }
    const auto bindFailures = gSpectateBindFailures.load(std::memory_order_relaxed);
    if (bindFailures != gSpectateBindFailuresDrained.exchange(bindFailures, std::memory_order_relaxed)) {
        diagnostics.Log("warning", "playerlifecycle_spectate_camera_unavailable",
            "[PlayerLifecycle] spectate follow camera could not be bound; retrying.",
            {{"failures", Diagnostics::Unsigned(bindFailures), true}});
    }
    if (dropped != 0U) {
        diagnostics.Log("warning", "playerlifecycle_diagnostics_overflow",
            "[PlayerLifecycle] death diagnostics queue overflow; hook decisions were unaffected.",
            {{"dropped", Diagnostics::Unsigned(dropped), true}});
    }
}

} // namespace aimp::player_lifecycle
