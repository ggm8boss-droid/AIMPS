#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CharacterResourceAdmission.h"
#include "aimp/AISync.h"
#include "aimp/AISyncExecutionPolicy.h"
#include "aimp/PlayerPostureRuntime.h"
#include "aimp/PlayerTorchRuntime.h"
#include "aimp/PlayerHidingRuntime.h"
#include "aimp/PlayerHidingPresentationRuntime.h"
#include "aimp/PlayerHidingLocalCapture.h"
#include "aimp/AnimationSyncRuntime.h"
#include "aimp/AnimationSyncPropsRuntime.h"
#include "aimp/HidingDoorPresentation.h"

#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"
#include "aimp/PlayerAnimationClipCapture.h"
#include "aimp/RuntimeFeatureFlags.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <mutex>
#include <string>

namespace aimp::character_direct_step_runtime {
namespace {

constexpr auto kNativeRvas = NativeMovementRvas();
constexpr auto kCharacterRegistry = NativeCharacterRegistry();
constexpr std::uintptr_t kCharacterPositionOffset = 0x0070U;
constexpr std::uintptr_t kNpcLocomotionCycleOffset = 0x036CU;
constexpr std::uintptr_t kNpcLocomotionCycleOwnerOffset = 0x0004U;
constexpr std::uintptr_t kNpcLocomotionCycleStateOffset = 0x001AU;
constexpr std::uintptr_t kMovementActionObjectOffset = 0x42A8U;
constexpr std::uintptr_t kMovementActionStateOffset = 0x0008U;
constexpr std::uintptr_t kMovementActionModeResetOffset = 0x0190U;
constexpr std::uintptr_t kMovementActionMoveTypeOffset = 0x0194U;
constexpr std::uintptr_t kMovementActionCurrentMoveTypeOffset = 0x0198U;
constexpr std::uintptr_t kNativeOrientationOffset = 0x0184U;
constexpr std::uint32_t kMovementActionMoveState = 2U;
constexpr std::uintptr_t kLocomotionSubsystemOffset = 0x03F0U;
constexpr std::uintptr_t kLocomotionEnabledOffsetA = 0x02C8U;
constexpr std::uintptr_t kLocomotionEnabledOffsetB = 0x02CAU;
constexpr std::uint64_t kReplicationLeaseMilliseconds = 600U;
constexpr std::uint32_t kPersistentReplicationFrames = 0xFFFFFFFFU;
constexpr float kControlDirectionDotThreshold = 0.9999619F;
constexpr float kControlMagnitudeEpsilon = 0.0005F;
constexpr float kControlIntensityEpsilon = 0.0005F;
constexpr float kMoveRequestEpsilon = 0.0001F;
constexpr std::uint8_t kNpcLocomotionIdleState = 0x01U;
constexpr std::uint8_t kNpcLocomotionMoveState = 0x38U;
constexpr float kFollowerLateralCorrectionStart = 0.025F;
constexpr float kFollowerLateralCorrectionFull = 0.60F;
constexpr float kFollowerMaximumLateralBlend = 0.55F;
constexpr float kFollowerSettleDistance = 0.080F;
constexpr float kHybridSettleResumeDistance = 0.160F;
constexpr float kFollowerInitialPlacementTolerance = 0.05F;
// After a hiding exit, the owner's network position must hold still this long
// (within this distance) before the representation is re-placed; bounded so
// an owner who walks straight off is still followed.
constexpr std::uint64_t kHidingResumeSettleMs = 250U;
constexpr std::uint64_t kHidingResumeMaximumMs = 1500U;
constexpr float kHidingResumeSettleDistance = 0.02F;
// Smoothing of the entry/exit clip path: per-frame caps (~60 Hz) above a
// walking pace, and a gap beyond which the network target is not this clip.
constexpr float kHidingFollowDeadZone = 0.01F;
constexpr float kHidingFollowMaximumStep = 0.08F;
constexpr float kHidingFollowMaximumGap = 3.0F;
constexpr float kFollowerMaximumMagnitude = 8.0F;
// v13 keeps retail locomotion for normal motion, but allows a lagging proxy to
// recover network error materially faster than v12.8. The hard-resync below is
// still reserved for genuine divergence, not ordinary latency.
constexpr float kFollowerCatchupGain = 0.35F;
constexpr float kFollowerAheadBrakeGain = 0.55F;
constexpr float kFollowerMinimumSpeedMultiplier = 0.45F;
constexpr float kFollowerMaximumSpeedMultiplier = 1.35F;
constexpr float kFollowerHardResyncDistance = 1.50F;
// AISync can carry faster NPC motion than PlayerSync's human follower. Give
// native locomotion time to absorb transient drift before an emergency snap.
constexpr float kAiFollowerMaximumMagnitude = 20.0F;
// Direct Host-pose locomotion: above the Alien sprint, below noise spikes.
constexpr float kAiHybridMaximumMagnitude = 10.0F;
constexpr float kAiFollowerPositionGain = 2.5F;
constexpr float kAiFollowerMaximumCorrectionSpeed = 4.0F;
constexpr float kAiFollowerMaximumSettlingMagnitude = 2.5F;
constexpr float kAiFollowerHardResyncDistance = 3.50F;
constexpr std::uint64_t kAiFollowerHardResyncGraceMilliseconds = 500U;
constexpr std::uint64_t kFollowerHardResyncCooldownMilliseconds = 250U;
// After a native full-body interaction, allow the regular retail follower to
// converge before its emergency snap logic is eligible again.
constexpr std::uint64_t kNativeAnimationFollowerGraceMilliseconds = 1500U;
constexpr std::uint64_t kFollowerHardResyncProbeIntervalMilliseconds = 100U;
constexpr std::uint64_t kMoveTypeValidationIntervalMilliseconds = 250U;
constexpr std::uint64_t kIdleValidationIntervalMilliseconds = 250U;
constexpr std::uint32_t kInvalidMoveType = 0xFFFFFFFFU;
constexpr float kFollowerMinimumSettlingMagnitude = 0.12F;
constexpr float kFollowerMaximumSettlingMagnitude = 0.65F;
constexpr std::uintptr_t kReservedSlot = 1U;

using ActivateLocomotionCycleFn = void(__thiscall*)(void* cycle);
using EnterIdleCycleFn = void(__thiscall*)(void* cycle);
using SubmitLocomotionFn = void(__thiscall*)(
    void* subsystem,
    const float* direction,
    float intensity);
using TeleportCharacterFn = void(__thiscall*)(
    void* registryEntry,
    const float* position);
using ExecutionTransitionFn = void(__thiscall*)(void* cycle);
using NativeTurnLimitsFn = void(__thiscall*)(void*, float, float*, float*);
using NativeFrameDeltaFn = double(__cdecl*)(float, float);

enum class FollowerPolicy : std::uint8_t {
    Default,
    AIHybridNativeOnly,
};

struct MovementSlot final {
    std::atomic<std::uintptr_t> characterAddress{0U};
    std::atomic<std::uintptr_t> stateAddress{0U};
    std::atomic<std::uintptr_t> movementActionAddress{0U};
    std::atomic<std::uint32_t> directionXBits{0U};
    std::atomic<std::uint32_t> directionYBits{0U};
    std::atomic<std::uint32_t> directionZBits{0U};
    std::atomic<std::uint32_t> deltaYawBits{0U};
    std::atomic<std::uint32_t> intensityBits{
        std::bit_cast<std::uint32_t>(1.0F)};
    std::atomic<std::uint32_t> networkXBits{0U};
    std::atomic<std::uint32_t> networkYBits{0U};
    std::atomic<std::uint32_t> networkZBits{0U};
    std::atomic<std::uint32_t> replicatedYawBits{0U};
    std::atomic<std::uint32_t> hybridAngularVelocityBits{0U};
    std::atomic<std::uint32_t> moveType{1U};
    std::atomic<std::uint32_t> appliedMoveType{kInvalidMoveType};
    std::atomic<std::uint64_t> lastMoveTypeValidationTickMs{0U};
    std::atomic<std::uint32_t> remainingFrames{0U};
    std::atomic<std::uint64_t> controlRevision{0U};
    std::atomic<std::uint64_t> appliedControlRevision{0U};
    // Network-target revision changes only when the desired replicated state
    // changes. Heartbeats with an identical target refresh the lease without
    // waking an already-converged idle follower.
    std::atomic<std::uint64_t> targetRevision{0U};
    std::atomic<std::uint64_t> preparedTargetRevision{0U};
    std::atomic<std::uint32_t> preparedDirectionXBits{0U};
    std::atomic<std::uint32_t> preparedDirectionZBits{0U};
    std::atomic<std::uint32_t> preparedIntensityBits{0U};
    std::atomic<bool> preparedDrive{false};
    std::atomic<bool> preparedNetworkFollower{false};
    std::atomic<std::uint64_t> leaseExpiresAtTickMs{0U};
    std::atomic<bool> worldSpace{false};
    std::atomic<bool> hasNetworkTransform{false};
    std::atomic<bool> aiReplicationProfile{false};
    std::atomic<FollowerPolicy> followerPolicy{FollowerPolicy::Default};
    std::atomic<std::uint64_t> aiDivergenceSinceTickMs{0U};
    std::atomic<bool> hasReplicatedYaw{false};
    std::atomic<bool> applyPhysicalStep{false};
    std::atomic<bool> nativeStateActive{false};
    std::atomic<bool> moveActive{false};
    std::atomic<bool> driveRequested{false};
    std::atomic<std::uint8_t> lastObservedNativeState{0U};
    std::atomic<bool> stopPending{false};
    std::atomic<bool> initialPlacementPending{false};
    std::atomic<bool> hidingOwned{false};
    std::atomic<bool> nativeAnimationOwned{false};
    std::atomic<std::uint64_t> nativeAnimationReleasedAt{0U};
    std::atomic<std::uint64_t> hidingResumeAt{0U};
    std::atomic<std::uint64_t> hidingResumeDeadline{0U};
    std::atomic<std::uint32_t> hidingSettleXBits{0U};
    std::atomic<std::uint32_t> hidingSettleYBits{0U};
    std::atomic<std::uint32_t> hidingSettleZBits{0U};
    std::atomic<bool> idleConverged{false};
    // A paired attack that aborted before retail's 4E destructor could clear
    // the victim's grapple bytes leaves the representation frozen.
    std::atomic<bool> pairedHoldReleaseRequested{false};
    std::atomic<std::uint64_t> nextIdleValidationTickMs{0U};
    std::atomic<std::uintptr_t> certifiedCycleAddress{0U};
    std::atomic<std::uint64_t> lastHookTickMs{0U};
    std::atomic<std::uint64_t> lastHardResyncAttemptTickMs{0U};
    std::atomic<std::uint64_t> lastHardResyncProbeTickMs{0U};

    std::atomic<std::uint64_t> activationAttempts{0U};
    std::atomic<std::uint64_t> activationApplications{0U};
    std::atomic<std::uint64_t> activationReuses{0U};
    std::atomic<std::uint64_t> activationFailures{0U};
    std::atomic<std::uint64_t> nativeStateLosses{0U};
    std::atomic<std::uint64_t> nativeStateReacquireAttempts{0U};
    std::atomic<std::uint64_t> nativeStateReacquireSuccesses{0U};
    std::atomic<std::uint64_t> enterMoveTransitions{0U};
    std::atomic<std::uint64_t> updateMoveTransitions{0U};
    std::atomic<std::uint64_t> exitMoveTransitions{0U};
    std::atomic<std::uint64_t> idleHoldApplications{0U};
    std::atomic<std::uint64_t> enginePumpApplications{0U};
    std::atomic<std::uint64_t> aiActivationSuppressions{0U};
    std::atomic<std::uint64_t> aiIdleSuppressions{0U};
    std::atomic<std::uint64_t> nativeIdleApplications{0U};
    std::atomic<std::uint64_t> followerApplications{0U};
    std::atomic<std::uint64_t> effectiveMoveTypeApplications{0U};
    std::atomic<std::uint64_t> hookHits{0U};
    std::atomic<std::uint64_t> finalSubmissions{0U};
    std::atomic<std::uint64_t> requestApplications{0U};
    std::atomic<std::uint64_t> transformApplications{0U};
    std::atomic<std::uint64_t> teleportApplications{0U};
    std::atomic<std::uint64_t> hardResyncAttempts{0U};
    std::atomic<std::uint64_t> hardResyncApplications{0U};
    std::atomic<std::uint64_t> physicalApplications{0U};
    std::atomic<std::uint64_t> stopApplications{0U};
    std::atomic<std::uint64_t> protectedFailures{0U};
    std::atomic<std::uintptr_t> lastCharacter{0U};
    // Diagnostics only: last hybrid locomotion hook values and branch counts.
    std::array<std::atomic<std::uint32_t>, 28> hybridProbe{};
    std::array<std::atomic<std::uint64_t>, 6> hybridBranches{};
    std::atomic<std::uint32_t> preparedBranch{0U};
    std::atomic<std::uint32_t> preparedPlanarErrorBits{0U};
    std::atomic<std::uint32_t> probeLastXBits{0U}, probeLastZBits{0U};
    std::atomic<std::uint64_t> probeLastTickMs{0U};
    std::atomic<std::uint64_t> probeIncarnation{0U};
};
// hybridProbe indices
enum HybridProbe : std::size_t {
    ProbeBranch, ProbePlanarError, ProbeDemandMagnitude, ProbeDemandIntensity,
    ProbeRetailMagnitude, ProbeRetailHeading, ProbeRetailIntensity, ProbeRetailAngular,
    ProbeControlMagnitude, ProbeControlHeading, ProbeControlIntensity,
    ProbeYaw, ProbeTargetYaw, ProbeTurnMaximum, ProbeTurnAcceleration, ProbeFrameDelta, ProbeAngularWritten,
    ProbeMeasuredSpeed, ProbeFacingStatus, ProbeInputSpeed,
    // Probe v2: previous-frame native root motion (+730..+760), and the
    // submitted local vector/intensity (+1030..+1040). +1010 is an optional
    // additive displacement, NOT the animation output (007C67B0).
    ProbeDeltaX, ProbeDeltaY, ProbeDeltaZ, ProbeDeltaYaw, ProbeLocalX, ProbeLocalZ, ProbeSubmitIntensity, ProbeSubmitAngular
};
void StoreProbe(MovementSlot& slot, const HybridProbe index, const float value) noexcept {
    slot.hybridProbe[index].store(std::bit_cast<std::uint32_t>(value), std::memory_order_relaxed);
}

bool UsesAIHybridReplication(const MovementSlot& slot) noexcept {
    return slot.followerPolicy.load(std::memory_order_acquire) ==
        FollowerPolicy::AIHybridNativeOnly;
}

bool CanForceReplicatedYaw(const MovementSlot&) noexcept {
    // 511 left yaw to retail while walking: diag 194257 shows the Client Alien
    // stuck at yaw 0 while the Host turned to -0.27/1.18/-0.94 (retail only
    // turns while translating fast), so it slid sideways. The interpolated Host
    // facing is the only authority for direct locomotion.
    return true;
}

bool ReplicationLeaseExpired(const MovementSlot& slot) noexcept {
    const auto lease = slot.leaseExpiresAtTickMs.load(std::memory_order_acquire);
    return lease != 0U && GetTickCount64() >= lease;
}

std::atomic<bool> gInstalled{false};
std::atomic<NativeAIIntentObserver> gNativeAIIntentObserver{nullptr};
std::atomic<NativeAITransitionGuard> gNativeAITransitionGuard{nullptr};
std::array<MovementSlot, MaximumConcurrentTargets()> gSlots{};
std::atomic<std::uint64_t> gProbeIncarnation{0U};
std::atomic<std::uintptr_t> gLastArmedCharacter{0U};
std::atomic<std::uint64_t> gStaleGrappleReleases{0U};
std::atomic<PairedVictimHeldQuery> gPairedVictimHeldQuery{nullptr};
std::uintptr_t gModuleBase{};
NativeTurnLimitsFn gNativeTurnLimits{};
NativeFrameDeltaFn gNativeFrameDelta{};
std::uintptr_t gGlobalStateRootAddress{};
ActivateLocomotionCycleFn gActivateLocomotionCycle{};
EnterIdleCycleFn gEnterIdleCycle{};
SubmitLocomotionFn gSubmitLocomotion{};
TeleportCharacterFn gTeleportCharacter{};
void* gActivateLocomotionTrampoline{};
void* gEnterIdleCycleTrampoline{};
void* gMovementActionPostGaitTrampoline{};
void* gNpcLocomotionPreTransformTrampoline{};
void* gNpcLocomotionUpdateTrampoline{};
std::atomic<std::uint64_t> gNpcLocomotionUpdatesWithoutMovement{};

// Hot-path memory access. Every address used here is either a live object
// supplied by the retail engine on its own thread, or a pointer previously
// certified by this runtime. SEH preserves the old fail-closed behaviour while
// avoiding VirtualQuery on every scalar read/write. Invalid/stale pointers stay
// on the exceptional path and are invalidated by the caller.
template <typename T>
bool GuardedReadValue(const std::uintptr_t address, T& value) noexcept {
    if (address == 0U) return false;
#if defined(_MSC_VER)
    __try {
        value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    const auto read = crash_guard::SafeRead<T>(reinterpret_cast<const void*>(address));
    if (!read.has_value()) return false;
    value = *read;
    return true;
#endif
}

bool GameplayLoaded() noexcept {
    std::uintptr_t root{}, gameplay{};
    std::uint32_t state{}, mode{};
    // The retail frontend request (793780) synchronously sets G+14=0 and
    // G+8=7, before queued network commands can be drained again. Read the
    // profile-resolved global cell; never cache the per-level manager.
    return GuardedReadValue(gGlobalStateRootAddress, root) && root != 0U &&
        GuardedReadValue(root + 0x48U, gameplay) && gameplay != 0U &&
        GuardedReadValue(gameplay + 0x14U, mode) && mode != 0U && mode != 3U &&
        GuardedReadValue(gameplay + 0x08U, state) && state != 7U;
}

template <typename T>
bool GuardedWriteValue(const std::uintptr_t address, const T& value) noexcept {
    if (address == 0U) return false;
#if defined(_MSC_VER)
    __try {
        *reinterpret_cast<T*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return crash_guard::TryWrite(
        reinterpret_cast<void*>(address), &value, sizeof(value));
#endif
}

bool GuardedReadFloat3(
    const std::uintptr_t address,
    float& x,
    float& y,
    float& z) noexcept {
    if (address == 0U) return false;
#if defined(_MSC_VER)
    __try {
        const auto* values = reinterpret_cast<const float*>(address);
        x = values[0];
        y = values[1];
        z = values[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::array<float, 3> values{};
    if (!crash_guard::TryCopy(
            values.data(), reinterpret_cast<const void*>(address), sizeof(values)))
        return false;
    x = values[0];
    y = values[1];
    z = values[2];
    return true;
#endif
}

std::mutex gLocomotionFlightMutex;
HANDLE gLocomotionFlightFile{INVALID_HANDLE_VALUE};
std::atomic<std::uint64_t> gLocomotionFlightHeartbeatTickMs{0U};

[[nodiscard]] bool EnsureLocomotionFlightOpenLocked() noexcept {
    if (gLocomotionFlightFile != INVALID_HANDLE_VALUE) return true;
    wchar_t root[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", root, _countof(root));
    if (length == 0U || length >= _countof(root)) {
        if (GetTempPathW(_countof(root), root) == 0U) return false;
    }
    std::size_t used = wcslen(root);
    if (used != 0U && root[used - 1U] != L'\\') {
        if (used + 1U >= _countof(root)) return false;
        root[used++] = L'\\'; root[used] = L'\0';
    }
    if (wcscat_s(root, _countof(root), L"AIMP") != 0) return false;
    CreateDirectoryW(root, nullptr);
    if (wcscat_s(root, _countof(root), L"\\logs") != 0) return false;
    CreateDirectoryW(root, nullptr);
    wchar_t suffix[128]{};
    if (_snwprintf_s(suffix, _countof(suffix), _TRUNCATE,
            L"\\AIMP.RemoteLocomotionFlight-%lu.jsonl", GetCurrentProcessId()) < 0 ||
        wcscat_s(root, _countof(root), suffix) != 0) return false;
    gLocomotionFlightFile = CreateFileW(
        root, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    return gLocomotionFlightFile != INVALID_HANDLE_VALUE;
}

void AppendLocomotionFlight(
    const char* eventName,
    const MovementSlot& slot,
    const std::uintptr_t characterAddress,
    const std::uintptr_t cycleAddress,
    const std::uint8_t cycleState,
    const std::uint32_t detail = 0U) noexcept {
    if (!runtime_features::Enabled(
            runtime_features::Feature::LocomotionFlightRecorder)) {
        return;
    }
    std::lock_guard lock(gLocomotionFlightMutex);
    if (!EnsureLocomotionFlightOpenLocked()) return;
    char line[2048]{};
    const auto length = _snprintf_s(
        line, sizeof(line), _TRUNCATE,
        "{\"version\":1,\"tickMs\":%llu,\"tid\":%lu,"
        "\"event\":\"%s\",\"character\":%llu,\"cycle\":%llu,"
        "\"cycleState\":%u,\"detail\":%u,\"armed\":%s,\"moveActive\":%s,"
        "\"nativeStateActive\":%s,\"driveRequested\":%s,\"moveType\":%u,"
        "\"activationAttempts\":%llu,\"activationApplications\":%llu,"
        "\"activationFailures\":%llu,\"nativeStateLosses\":%llu,"
        "\"reacquireAttempts\":%llu,\"reacquireSuccesses\":%llu,"
        "\"protectedFailures\":%llu,\"hardResyncAttempts\":%llu,"
        "\"hardResyncApplications\":%llu,\"aiReplicationProfile\":%s}\r\n",
        static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(), eventName,
        static_cast<unsigned long long>(characterAddress),
        static_cast<unsigned long long>(cycleAddress),
        static_cast<unsigned>(cycleState), detail,
        slot.characterAddress.load(std::memory_order_relaxed) != 0U ? "true" : "false",
        slot.moveActive.load(std::memory_order_relaxed) ? "true" : "false",
        slot.nativeStateActive.load(std::memory_order_relaxed) ? "true" : "false",
        slot.driveRequested.load(std::memory_order_relaxed) ? "true" : "false",
        slot.moveType.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(slot.activationAttempts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slot.activationApplications.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slot.activationFailures.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slot.nativeStateLosses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slot.nativeStateReacquireAttempts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slot.nativeStateReacquireSuccesses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slot.protectedFailures.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slot.hardResyncAttempts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slot.hardResyncApplications.load(std::memory_order_relaxed)),
        slot.aiReplicationProfile.load(std::memory_order_relaxed) ? "true" : "false");
    if (length <= 0) return;
    DWORD written = 0U;
    static_cast<void>(WriteFile(
        gLocomotionFlightFile, line, static_cast<DWORD>(length), &written, nullptr));
}

// Diagnostics only (v347): Client AI locomotion that stops translating for a
// whole game. One baseline while the hybrid follower really moves, then the
// same memory once it is stuck under a real demand: changed dwords are logged
// to the flight file, plus one absolute dump of the stuck state.
constexpr std::size_t kStuckCharacterBytes = 0x1100U, kStuckActionBytes = 0x300U;
struct StuckProbe {
    std::uintptr_t character{};
    std::uint64_t incarnation{};
    std::array<std::uint8_t, kStuckCharacterBytes> character0{}, characterNow{};
    std::array<std::uint8_t, kStuckActionBytes> action0{}, actionNow{};
    bool baseline{}, absoluteDumped{}, demandLogged{}, movingLogged{};
    std::uint64_t stuckSince{}, baselineTick{};
    std::uint32_t diffs{};
};
// Separate bounded histories: a second AI or a new round must not consume
// another actor's only dump or compare memory from a recycled Character.
std::array<StuckProbe, MaximumConcurrentTargets()> gStuckProbes{};
bool CopyRegion(const std::uintptr_t address, std::uint8_t* out, const std::size_t size) noexcept {
    if (address == 0U) return false;
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void AppendFlightText(const std::string& text) noexcept {
    if (!runtime_features::Enabled(runtime_features::Feature::LocomotionFlightRecorder)) return;
    std::lock_guard lock(gLocomotionFlightMutex);
    if (!EnsureLocomotionFlightOpenLocked()) return;
    DWORD written = 0U;
    static_cast<void>(WriteFile(gLocomotionFlightFile, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
}
template<std::size_t Size>
void AppendDiff(std::string& out, const char* name, const std::array<std::uint8_t, Size>& before,
    const std::array<std::uint8_t, Size>& after) {
    out += ",\""; out += name; out += "\":\"";
    char item[48]{};
    std::size_t count{};
    for (std::size_t offset = 0; offset + 4U <= Size && count < 400U; offset += 4U) {
        std::uint32_t a{}, b{};
        std::memcpy(&a, before.data() + offset, 4U); std::memcpy(&b, after.data() + offset, 4U);
        if (a == b) continue;
        _snprintf_s(item, sizeof(item), _TRUNCATE, "%X:%08X>%08X ", static_cast<unsigned>(offset), a, b);
        out += item; ++count;
    }
    out += "\"";
}
template<std::size_t Size>
void AppendHex(std::string& out, const char* name, const std::array<std::uint8_t, Size>& bytes) {
    out += ",\""; out += name; out += "\":\"";
    char item[4]{};
    for (const auto value : bytes) { _snprintf_s(item, sizeof(item), _TRUNCATE, "%02X", value); out += item; }
    out += "\"";
}
void AppendMotionGates(std::string& line,
    const std::array<std::uint8_t, kStuckCharacterBytes>& bytes) {
    // 007C67B0 applies root displacement only with Character+260 set.
    // 007C0220 sets it on frontstage admission unless +2F8 blocks that call.
    // These are observations, never permissions for AIMP to change the flags.
    std::uint32_t runtime{}, rootX{}, rootY{}, rootZ{}, rootYaw{};
    std::memcpy(&runtime, bytes.data()+0x50U, 4U);
    std::memcpy(&rootX, bytes.data()+0x730U, 4U);
    std::memcpy(&rootY, bytes.data()+0x734U, 4U);
    std::memcpy(&rootZ, bytes.data()+0x738U, 4U);
    std::memcpy(&rootYaw, bytes.data()+0x760U, 4U);
    char fields[512]{};
    _snprintf_s(fields, sizeof(fields), _TRUNCATE,
        ",\"probeVersion\":2,\"runtimeId\":%u,\"translationEnabled\":%u,\"rotationEnabled\":%u,"
        "\"backstage\":%u,\"frontstageBlocked\":%u,\"motionEnabledA\":%u,\"motionEnabledB\":%u,"
        "\"rootMotionBits\":\"%08X %08X %08X %08X\"",
        runtime, static_cast<unsigned>(bytes[0x260U]), static_cast<unsigned>(bytes[0x261U]),
        static_cast<unsigned>(bytes[0x266U]), static_cast<unsigned>(bytes[0x2F8U]),
        static_cast<unsigned>(bytes[0x2C8U]), static_cast<unsigned>(bytes[0x2CAU]),
        rootX, rootY, rootZ, rootYaw);
    line += fields;
}
void ProbeStuckLocomotion(MovementSlot& slot, const std::uintptr_t character,
    const float speed, const float demand, const std::uint64_t now) noexcept {
    if (!runtime_features::Enabled(runtime_features::Feature::LocomotionFlightRecorder)) return;
    auto& probe = gStuckProbes[static_cast<std::size_t>(&slot - gSlots.data())];
    const auto incarnation = slot.probeIncarnation.load(std::memory_order_acquire);
    if (probe.incarnation != incarnation || probe.character != character) {
        probe = {};
        probe.incarnation = incarnation;
        probe.character = character;
    }
    std::uintptr_t action{};
    static_cast<void>(GuardedReadValue(character + kMovementActionObjectOffset, action));
    // One real-demand state and one actually-moving state per local binding.
    // A completely frozen round still has a reference to compare with a good
    // round; a moving baseline alone would never be logged in that case.
    const bool firstDemand = demand > 0.3F && !probe.demandLogged;
    const bool firstMoving = demand > 0.3F && speed > 0.5F && !probe.movingLogged;
    if ((firstDemand || firstMoving) &&
        CopyRegion(character, probe.characterNow.data(), kStuckCharacterBytes)) {
        try {
            std::string line = "{\"version\":1,\"event\":\"hybrid_motion_gate\",\"tickMs\":";
            line += std::to_string(now);
            line += ",\"incarnation\":" + std::to_string(incarnation);
            line += ",\"character\":" + std::to_string(character);
            line += ",\"moving\":";
            line += speed > 0.5F ? "true" : "false";
            AppendMotionGates(line, probe.characterNow);
            line += "}\r\n";
            AppendFlightText(line);
            probe.demandLogged = true;
            if (firstMoving) probe.movingLogged = true;
        } catch (...) {}
    }
    if (speed > 0.5F && demand > 0.3F) {
        if (probe.baseline && probe.character == character && now - probe.baselineTick < 1000U) return;
        if (CopyRegion(character, probe.character0.data(), kStuckCharacterBytes)) {
            probe.action0 = {};
            static_cast<void>(CopyRegion(action, probe.action0.data(), kStuckActionBytes));
            probe.character = character; probe.baseline = true; probe.baselineTick = now; probe.stuckSince = 0U;
        }
        return;
    }
    if (demand <= 1.0F || speed >= 0.02F) { probe.stuckSince = 0U; return; }
    if (probe.stuckSince == 0U) { probe.stuckSince = now; return; }
    if (now - probe.stuckSince < 1500U) return;
    probe.stuckSince = now; // at most one record per 1.5 s of continued stall
    const bool diff = probe.baseline && probe.character == character && probe.diffs < 3U;
    if (!diff && probe.absoluteDumped) return;
    if (!CopyRegion(character, probe.characterNow.data(), kStuckCharacterBytes)) return;
    probe.actionNow = {};
    static_cast<void>(CopyRegion(action, probe.actionNow.data(), kStuckActionBytes));
    try {
        std::string line;
        char head[256]{};
        _snprintf_s(head, sizeof(head), _TRUNCATE,
            "{\"version\":1,\"tickMs\":%llu,\"event\":\"%s\",\"character\":%llu,\"action\":%llu,\"baselineAgeMs\":%llu,\"demand\":%.3f",
            static_cast<unsigned long long>(now), diff ? "hybrid_stuck_diff" : "hybrid_stuck_dump",
            static_cast<unsigned long long>(character), static_cast<unsigned long long>(action),
            static_cast<unsigned long long>(diff ? now - probe.baselineTick : 0U), demand);
        line = head;
        line += ",\"incarnation\":" + std::to_string(incarnation);
        AppendMotionGates(line, probe.characterNow);
        if (diff) {
            AppendDiff(line, "characterDiff", probe.character0, probe.characterNow);
            AppendDiff(line, "actionDiff", probe.action0, probe.actionNow);
            ++probe.diffs; probe.baseline = false; // next diff needs a fresh moving baseline
        } else {
            AppendHex(line, "characterHex", probe.characterNow);
            AppendHex(line, "actionHex", probe.actionNow);
            probe.absoluteDumped = true;
        }
        line += "}\r\n";
        AppendFlightText(line);
    } catch (...) {}
}

template<std::size_t Size>
bool ValidateBytes(
    const std::uintptr_t address,
    const std::array<std::uint8_t, Size>& expected) noexcept {
#if defined(_MSC_VER)
    __try {
        return std::memcmp(
            reinterpret_cast<const void*>(address),
            expected.data(),
            expected.size()) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return std::memcmp(
        reinterpret_cast<const void*>(address),
        expected.data(),
        expected.size()) == 0;
#endif
}

bool ValidateSubmitFunction(const std::uintptr_t address) noexcept {
    constexpr std::array<std::uint8_t, 16> expected{
        0x55U, 0x8BU, 0xECU, 0x83U,
        0xE4U, 0xF0U, 0x8BU, 0x45U,
        0x08U, 0x83U, 0xECU, 0x1CU,
        0x56U, 0x8BU, 0xF1U, 0x50U,
    };
    return ValidateBytes(address, expected);
}

bool ValidateActivationFunction(const std::uintptr_t address) noexcept {
    constexpr std::array<std::uint8_t, 13> expected{
        0x56U, 0x8BU, 0xF1U, 0x8AU,
        0x46U, 0x1AU, 0x8BU, 0x4EU,
        0x04U, 0x88U, 0x46U, 0x1BU,
        0xE8U,
    };
    return ValidateBytes(address, expected);
}

bool ValidateIdleTransitionFunction(const std::uintptr_t address) noexcept {
    // The absolute EH handler pointer is relocated, so only validate stable
    // instruction bytes at the function entry and the ECX->ESI prologue.
    constexpr std::array<std::uint8_t, 2> entry{0x6AU, 0xFFU};
    constexpr std::array<std::uint8_t, 3> thisPrologue{0x56U, 0x8BU, 0xF1U};
    return ValidateBytes(address, entry) &&
        ValidateBytes(address + 0x18U, thisPrologue);
}

bool ValidateLocomotionVtable(const std::uintptr_t moduleBase) noexcept {
    const auto vtable = moduleBase + kNativeRvas.npcLocomotionVtable;
    const auto update = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(vtable + 0x20U));
    return update.has_value() &&
        *update == moduleBase + kNativeRvas.npcLocomotionCycleUpdate;
}

bool ValidateTeleportFunction(const std::uintptr_t address) noexcept {
    constexpr std::array<std::uint8_t, 16> expected{
        0x55U, 0x8BU, 0xECU, 0x83U,
        0xE4U, 0xF0U, 0x83U, 0xECU,
        0x44U, 0x53U, 0x56U, 0x8BU,
        0xF1U, 0x8BU, 0x0EU, 0x57U,
    };
    return ValidateBytes(address, expected);
}

void ResetPayload(MovementSlot& slot) noexcept {
    slot.probeIncarnation.store(
        gProbeIncarnation.fetch_add(1U, std::memory_order_relaxed)+1U,
        std::memory_order_release);
    slot.stateAddress.store(0U, std::memory_order_relaxed);
    slot.movementActionAddress.store(0U, std::memory_order_relaxed);
    slot.directionXBits.store(0U, std::memory_order_relaxed);
    slot.directionYBits.store(0U, std::memory_order_relaxed);
    slot.directionZBits.store(0U, std::memory_order_relaxed);
    slot.deltaYawBits.store(0U, std::memory_order_relaxed);
    slot.intensityBits.store(
        std::bit_cast<std::uint32_t>(1.0F),
        std::memory_order_relaxed);
    slot.networkXBits.store(0U, std::memory_order_relaxed);
    slot.networkYBits.store(0U, std::memory_order_relaxed);
    slot.networkZBits.store(0U, std::memory_order_relaxed);
    slot.replicatedYawBits.store(0U, std::memory_order_relaxed);
    slot.hybridAngularVelocityBits.store(0U, std::memory_order_relaxed);
    slot.moveType.store(1U, std::memory_order_relaxed);
    slot.appliedMoveType.store(kInvalidMoveType, std::memory_order_relaxed);
    slot.lastMoveTypeValidationTickMs.store(0U, std::memory_order_relaxed);
    slot.remainingFrames.store(0U, std::memory_order_release);
    slot.controlRevision.store(0U, std::memory_order_release);
    slot.appliedControlRevision.store(0U, std::memory_order_release);
    slot.targetRevision.store(0U, std::memory_order_release);
    slot.preparedTargetRevision.store(0U, std::memory_order_release);
    slot.preparedDirectionXBits.store(0U, std::memory_order_relaxed);
    slot.preparedDirectionZBits.store(0U, std::memory_order_relaxed);
    slot.preparedIntensityBits.store(0U, std::memory_order_relaxed);
    slot.preparedDrive.store(false, std::memory_order_release);
    slot.preparedNetworkFollower.store(false, std::memory_order_release);
    slot.leaseExpiresAtTickMs.store(0U, std::memory_order_release);
    slot.worldSpace.store(false, std::memory_order_release);
    slot.hasNetworkTransform.store(false, std::memory_order_release);
    slot.aiReplicationProfile.store(false, std::memory_order_release);
    slot.followerPolicy.store(FollowerPolicy::Default, std::memory_order_release);
    slot.aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
    slot.hasReplicatedYaw.store(false, std::memory_order_release);
    slot.applyPhysicalStep.store(false, std::memory_order_release);
    slot.nativeStateActive.store(false, std::memory_order_release);
    slot.moveActive.store(false, std::memory_order_release);
    slot.driveRequested.store(false, std::memory_order_release);
    slot.lastObservedNativeState.store(0U, std::memory_order_release);
    slot.stopPending.store(false, std::memory_order_release);
    slot.initialPlacementPending.store(false, std::memory_order_release);
    slot.hidingOwned.store(false, std::memory_order_release);
    slot.nativeAnimationOwned.store(false, std::memory_order_release);
    slot.nativeAnimationReleasedAt.store(0U, std::memory_order_release);
    slot.hidingResumeAt.store(0U, std::memory_order_release);
    slot.hidingResumeDeadline.store(0U, std::memory_order_release);
    slot.idleConverged.store(false, std::memory_order_release);
    slot.nextIdleValidationTickMs.store(0U, std::memory_order_release);
    slot.certifiedCycleAddress.store(0U, std::memory_order_release);
    slot.lastHookTickMs.store(0U, std::memory_order_release);
    slot.lastHardResyncAttemptTickMs.store(0U, std::memory_order_release);
    slot.lastHardResyncProbeTickMs.store(0U, std::memory_order_release);
}

void ResetCounters(MovementSlot& slot) noexcept {
    slot.activationAttempts.store(0U, std::memory_order_relaxed);
    slot.activationApplications.store(0U, std::memory_order_relaxed);
    slot.activationReuses.store(0U, std::memory_order_relaxed);
    slot.activationFailures.store(0U, std::memory_order_relaxed);
    slot.nativeStateLosses.store(0U, std::memory_order_relaxed);
    slot.nativeStateReacquireAttempts.store(0U, std::memory_order_relaxed);
    slot.nativeStateReacquireSuccesses.store(0U, std::memory_order_relaxed);
    slot.enterMoveTransitions.store(0U, std::memory_order_relaxed);
    slot.updateMoveTransitions.store(0U, std::memory_order_relaxed);
    slot.exitMoveTransitions.store(0U, std::memory_order_relaxed);
    slot.idleHoldApplications.store(0U, std::memory_order_relaxed);
    slot.enginePumpApplications.store(0U, std::memory_order_relaxed);
    slot.aiActivationSuppressions.store(0U, std::memory_order_relaxed);
    slot.aiIdleSuppressions.store(0U, std::memory_order_relaxed);
    slot.nativeIdleApplications.store(0U, std::memory_order_relaxed);
    slot.followerApplications.store(0U, std::memory_order_relaxed);
    slot.effectiveMoveTypeApplications.store(0U, std::memory_order_relaxed);
    slot.hookHits.store(0U, std::memory_order_relaxed);
    for (auto& value : slot.hybridProbe) value.store(0U, std::memory_order_relaxed);
    for (auto& value : slot.hybridBranches) value.store(0U, std::memory_order_relaxed);
    slot.probeLastTickMs.store(0U, std::memory_order_relaxed);
    slot.finalSubmissions.store(0U, std::memory_order_relaxed);
    slot.requestApplications.store(0U, std::memory_order_relaxed);
    slot.transformApplications.store(0U, std::memory_order_relaxed);
    slot.teleportApplications.store(0U, std::memory_order_relaxed);
    slot.hardResyncAttempts.store(0U, std::memory_order_relaxed);
    slot.hardResyncApplications.store(0U, std::memory_order_relaxed);
    slot.physicalApplications.store(0U, std::memory_order_relaxed);
    slot.stopApplications.store(0U, std::memory_order_relaxed);
    slot.protectedFailures.store(0U, std::memory_order_relaxed);
    slot.lastCharacter.store(0U, std::memory_order_relaxed);
}

MovementSlot* FindSlot(const std::uintptr_t characterAddress) noexcept {
    if (characterAddress == 0U) return nullptr;
    for (auto& slot : gSlots) {
        if (slot.characterAddress.load(std::memory_order_acquire) ==
            characterAddress)
            return &slot;
    }
    return nullptr;
}

MovementSlot* FindSlotByMovementAction(
    const std::uintptr_t movementActionAddress) noexcept {
    if (movementActionAddress == 0U) return nullptr;
    for (auto& slot : gSlots) {
        if (slot.movementActionAddress.load(std::memory_order_acquire) ==
            movementActionAddress)
            return &slot;
    }
    return nullptr;
}

MovementSlot* FindOrAllocateSlot(
    const std::uintptr_t characterAddress) noexcept {
    if (auto* existing = FindSlot(characterAddress)) return existing;
    for (auto& slot : gSlots) {
        auto empty = std::uintptr_t{0U};
        if (!slot.characterAddress.compare_exchange_strong(
                empty,
                kReservedSlot,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
            continue;
        ResetPayload(slot);
        ResetCounters(slot);
        slot.characterAddress.store(characterAddress, std::memory_order_release);
        return &slot;
    }
    return nullptr;
}

void ReleaseSlot(MovementSlot& slot) noexcept {
    // Posture and torch belong to the representation, not its current Move/Idle
    // mailbox. Traversal and paired actions can release motion while it lives.
    slot.remainingFrames.store(0U, std::memory_order_release);
    slot.leaseExpiresAtTickMs.store(0U, std::memory_order_release);
    slot.nativeStateActive.store(false, std::memory_order_release);
    slot.moveActive.store(false, std::memory_order_release);
    slot.driveRequested.store(false, std::memory_order_release);
    slot.lastObservedNativeState.store(0U, std::memory_order_release);
    slot.stateAddress.store(0U, std::memory_order_release);
    slot.certifiedCycleAddress.store(0U, std::memory_order_release);
    slot.movementActionAddress.store(0U, std::memory_order_release);
    slot.appliedMoveType.store(kInvalidMoveType, std::memory_order_release);
    slot.lastMoveTypeValidationTickMs.store(0U, std::memory_order_release);
    slot.lastHookTickMs.store(0U, std::memory_order_release);
    slot.lastHardResyncAttemptTickMs.store(0U, std::memory_order_release);
    slot.lastHardResyncProbeTickMs.store(0U, std::memory_order_release);
    slot.hasNetworkTransform.store(false, std::memory_order_release);
    slot.aiReplicationProfile.store(false, std::memory_order_release);
    slot.followerPolicy.store(FollowerPolicy::Default, std::memory_order_release);
    slot.aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
    slot.hasReplicatedYaw.store(false, std::memory_order_release);
    slot.applyPhysicalStep.store(false, std::memory_order_release);
    slot.nativeAnimationOwned.store(false, std::memory_order_release);
    slot.nativeAnimationReleasedAt.store(0U, std::memory_order_release);
    slot.stopPending.store(false, std::memory_order_release);
    slot.initialPlacementPending.store(false, std::memory_order_release);
    slot.idleConverged.store(false, std::memory_order_release);
    slot.nextIdleValidationTickMs.store(0U, std::memory_order_release);
    slot.preparedTargetRevision.store(0U, std::memory_order_release);
    slot.preparedDrive.store(false, std::memory_order_release);
    slot.preparedNetworkFollower.store(false, std::memory_order_release);
    slot.characterAddress.store(0U, std::memory_order_release);
}

bool ReadTransform(
    const std::uintptr_t characterAddress,
    float& x,
    float& y,
    float& z) noexcept {
    if (!GuardedReadFloat3(
            characterAddress + kCharacterPositionOffset, x, y, z))
        return false;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

std::uintptr_t FindCharacterRegistryEntry(
    const std::uintptr_t characterAddress) noexcept {
    if (gModuleBase == 0U || characterAddress == 0U) return 0U;
    const auto table = gModuleBase + kCharacterRegistry.table;
    for (std::size_t index = 0U;
         index < kCharacterRegistry.capacity;
         ++index) {
        const auto entry = table + index * kCharacterRegistry.stride;
        const auto character = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(entry + kCharacterRegistry.character));
        if (!character.has_value() || *character != characterAddress) continue;
        const auto physicalBody = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(entry + kCharacterRegistry.physicalBody));
        const auto secondaryBody = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(entry + kCharacterRegistry.secondaryBody));
        const auto active = crash_guard::SafeRead<std::uint8_t>(
            reinterpret_cast<const void*>(entry + kCharacterRegistry.active));
        if (!physicalBody.has_value() || *physicalBody == 0U ||
            !secondaryBody.has_value() || *secondaryBody == 0U ||
            !active.has_value() || *active == 0U)
            return 0U;
        return entry;
    }
    return 0U;
}

bool TeleportCharacter(
    const std::uintptr_t characterAddress,
    const float x,
    const float y,
    const float z) noexcept {
    if (gTeleportCharacter == nullptr || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(z))
        return false;
    const auto entry = FindCharacterRegistryEntry(characterAddress);
    if (entry == 0U) return false;
    alignas(16) const std::array<float, 4> position{x, y, z, 0.0F};
#if defined(_MSC_VER)
    __try {
        gTeleportCharacter(
            reinterpret_cast<void*>(entry),
            position.data());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    gTeleportCharacter(reinterpret_cast<void*>(entry), position.data());
    return true;
#endif
}

bool ApplyPhysicalDelta(
    const std::uintptr_t characterAddress,
    const float x,
    const float y,
    const float z,
    const float yaw) noexcept {
    const auto offsets = PhysicalOffsets();
    return GuardedWriteValue(characterAddress + offsets.x, x) &&
        GuardedWriteValue(characterAddress + offsets.y, y) &&
        GuardedWriteValue(characterAddress + offsets.z, z) &&
        GuardedWriteValue(characterAddress + offsets.yaw, yaw);
}

bool ReadNativeCycle(
    const std::uintptr_t characterAddress,
    std::uintptr_t& cycleAddress,
    std::uint8_t& cycleState) noexcept {
    std::uintptr_t cycle = 0U;
    if (!GuardedReadValue(
            characterAddress + kNpcLocomotionCycleOffset, cycle) ||
        cycle == 0U)
        return false;
    std::uintptr_t owner = 0U;
    std::uint8_t state = 0U;
    if (!GuardedReadValue(cycle + kNpcLocomotionCycleOwnerOffset, owner) ||
        owner != characterAddress ||
        !GuardedReadValue(cycle + kNpcLocomotionCycleStateOffset, state))
        return false;
    cycleAddress = cycle;
    cycleState = state;
    return true;
}

bool IsNativeLocomotionCycle(const std::uintptr_t cycleAddress) noexcept {
    if (cycleAddress == 0U || gModuleBase == 0U) return false;
    std::uintptr_t vtable = 0U;
    if (!GuardedReadValue(cycleAddress, vtable) ||
        vtable != gModuleBase + kNativeRvas.npcLocomotionVtable)
        return false;
    std::uintptr_t update = 0U;
    return GuardedReadValue(vtable + 0x20U, update) &&
        update == gModuleBase + kNativeRvas.npcLocomotionCycleUpdate;
}

bool RetailOwnsPhysicalCycle(const std::uintptr_t characterAddress) noexcept {
    std::uintptr_t cycle{};
    std::uint8_t state{};
    std::uint8_t grappled{};
    std::uint32_t health{};
    // The victim need not own the attacker's 42/4E cycle. Retail paired
    // entry marks the victim +297; PlayerSync must respect that same ownership.
    if (GuardedReadValue(characterAddress + 0x297U, grappled) && grappled != 0U) return true;
    if (ReadNativeCycle(characterAddress, cycle, state) &&
        (state == 0x16U || state == 0x42U || state == 0x4EU || state == 0x4FU || state == 0x47U)) return true;
    return GuardedReadValue(characterAddress + 0x32CU, health) && health >= 3U;
}

bool AIHybridCanOwnCurrentCycle(const std::uintptr_t characterAddress) noexcept {
    std::uintptr_t cycle{}, type{};
    std::uint8_t state{};
    return gModuleBase != 0U && !RetailOwnsPhysicalCycle(characterAddress) &&
        ReadNativeCycle(characterAddress, cycle, state) &&
        GuardedReadValue(cycle, type) && type >= gModuleBase &&
        ai_sync::HybridLocomotionCycleAllowed(
            static_cast<std::uint32_t>(type - gModuleBase), state);
}

bool PairedCycleState(const std::uint8_t state) noexcept {
    return state == 0x16U || state == 0x42U || state == 0x4EU || state == 0x4FU || state == 0x47U;
}

// Engine thread. Retail clears victim +297/+29A in the 4E destructor
// (0065B190) through the source cycle's +44 victim pointer. An aborted Client
// replay zeroes +44 first, so the bytes leak and RetailOwnsPhysicalCycle keeps
// the representation frozen. Clear them only when no paired cycle owns the
// Character and it is not dying, after AISync explicitly retires this pair.
// Elapsed time is not proof that a native interaction has ended.
bool ReleaseStalePairedHold(MovementSlot& slot, const std::uintptr_t characterAddress) noexcept {
    std::uint8_t grappled{};
    if (!GuardedReadValue(characterAddress + 0x297U, grappled) || grappled == 0U) {
        slot.pairedHoldReleaseRequested.store(false, std::memory_order_relaxed);
        return false;
    }
    std::uintptr_t cycle{};
    std::uint8_t state{};
    std::uint32_t health{};
    const auto held = gPairedVictimHeldQuery.load(std::memory_order_acquire);
    if (!ReadNativeCycle(characterAddress, cycle, state) || PairedCycleState(state) ||
        !GuardedReadValue(characterAddress + 0x32CU, health) || health >= 3U ||
        (held != nullptr && held(characterAddress))) {
        return false;
    }
    if (!slot.pairedHoldReleaseRequested.load(std::memory_order_acquire)) return false;
    constexpr std::uint8_t kClear = 0U;
    if (!GuardedWriteValue(characterAddress + 0x297U, kClear) ||
        !GuardedWriteValue(characterAddress + 0x29AU, kClear)) return false;
    slot.pairedHoldReleaseRequested.store(false, std::memory_order_release);
    // Hybrid resumes through native locomotion from the current transform;
    // legacy player replication retains its placement recovery policy.
    slot.idleConverged.store(false, std::memory_order_release);
    slot.initialPlacementPending.store(!UsesAIHybridReplication(slot), std::memory_order_release);
    if (UsesAIHybridReplication(slot))
        slot.preparedTargetRevision.store(0U, std::memory_order_release);
    slot.targetRevision.fetch_add(1U, std::memory_order_release);
    gStaleGrappleReleases.fetch_add(1U, std::memory_order_relaxed);
    AppendLocomotionFlight("paired_hold_released", slot, characterAddress, cycle, state, grappled);
    return true;
}

bool EnsureCertifiedCycle(
    MovementSlot& slot,
    const std::uintptr_t cycleAddress) noexcept {
    if (cycleAddress == 0U) return false;
    // Retail reconstructs this persistent address in place for other actions.
    // An address previously certified as Move does not certify its current type.
    if (!IsNativeLocomotionCycle(cycleAddress)) return false;
    slot.certifiedCycleAddress.store(cycleAddress, std::memory_order_release);
    return true;
}

bool IsNativeMoveState(
    const std::uintptr_t cycleAddress,
    const std::uint8_t cycleState) noexcept {
    // Runtime evidence from the retail executable is explicit here: the AI
    // caller checks cycle+0x1A against 0x38 before invoking +257900, and the
    // +257900 transition itself writes 0x38. The vtable proves the object type;
    // it is not sufficient evidence that the Move state is actually active.
    return cycleState == kNpcLocomotionMoveState &&
        IsNativeLocomotionCycle(cycleAddress);
}

bool IsNativeIdleState(const std::uint8_t cycleState) noexcept {
    return cycleState == kNpcLocomotionIdleState;
}

bool ResolveMovementAction(
    MovementSlot& slot,
    const std::uintptr_t characterAddress,
    std::uintptr_t& movementAction,
    const bool refresh = false) noexcept {
    movementAction = slot.movementActionAddress.load(std::memory_order_acquire);
    if (movementAction != 0U && !refresh) return true;

    std::uintptr_t resolved = 0U;
    if (!GuardedReadValue(
            characterAddress + kMovementActionObjectOffset, resolved) ||
        resolved == 0U)
        return false;
    if (movementAction != 0U && movementAction != resolved) {
        slot.appliedMoveType.store(kInvalidMoveType, std::memory_order_release);
    }
    movementAction = resolved;
    slot.movementActionAddress.store(resolved, std::memory_order_release);
    return true;
}

bool ApplyRetailMoveType(
    MovementSlot& slot,
    const std::uintptr_t characterAddress,
    const std::uint32_t moveType,
    const bool forceValidation = false) noexcept {
    if (characterAddress == 0U || moveType > 3U) return false;

    const auto now = GetTickCount64();
    const auto applied = slot.appliedMoveType.load(std::memory_order_acquire);
    const auto lastValidation =
        slot.lastMoveTypeValidationTickMs.load(std::memory_order_acquire);
    if (!forceValidation && applied == moveType &&
        now - lastValidation < kMoveTypeValidationIntervalMilliseconds)
        return true;

    std::uintptr_t movementAction = 0U;
    if (!ResolveMovementAction(
            slot, characterAddress, movementAction, true))
        return false;

    // The MovementAction pointer is stable for the Character generation and is
    // cached after first resolution. Guarded direct access preserves the old
    // fail-closed behavior without VirtualQuery on each validation.
    std::uint32_t current = 0U;
    if (!GuardedReadValue(
            movementAction + kMovementActionMoveTypeOffset, current)) {
        slot.movementActionAddress.store(0U, std::memory_order_release);
        if (!ResolveMovementAction(slot, characterAddress, movementAction) ||
            !GuardedReadValue(
                movementAction + kMovementActionMoveTypeOffset, current))
            return false;
    }

    if (forceValidation || current != moveType || applied != moveType) {
        const std::uint32_t reset = 0U;
        if (!GuardedWriteValue(
                movementAction + kMovementActionMoveTypeOffset, moveType) ||
            !GuardedWriteValue(
                movementAction + kMovementActionModeResetOffset, reset))
            return false;
    }

    slot.appliedMoveType.store(moveType, std::memory_order_release);
    slot.lastMoveTypeValidationTickMs.store(now, std::memory_order_release);
    return true;
}

bool PrepareNativeMovementAction(
    MovementSlot& slot,
    const std::uintptr_t characterAddress) noexcept {
    if (characterAddress == 0U) return false;

    std::uintptr_t movementAction = 0U;
    if (!ResolveMovementAction(slot, characterAddress, movementAction)) return false;

    // Retail FUN_00450710 prepares movementAction+08 = 2 before +257900.
    const auto moveState = kMovementActionMoveState;
    return GuardedWriteValue(
        movementAction + kMovementActionStateOffset, moveState);
}

bool CallNativeMoveTransitionWithResources(
    MovementSlot& slot,
    const std::uintptr_t characterAddress,
    const bool reacquisition) noexcept {
    slot.activationAttempts.fetch_add(1U, std::memory_order_relaxed);
    AppendLocomotionFlight(reacquisition ? "move_reacquire_attempt" : "move_activate_attempt",
        slot, characterAddress, slot.stateAddress.load(std::memory_order_relaxed),
        slot.lastObservedNativeState.load(std::memory_order_relaxed));
    if (reacquisition) {
        slot.nativeStateReacquireAttempts.fetch_add(
            1U, std::memory_order_relaxed);
    }

    std::uintptr_t cycleAddress = 0U;
    std::uint8_t cycleState = 0U;
    if (gActivateLocomotionCycle == nullptr ||
        !ReadNativeCycle(characterAddress, cycleAddress, cycleState)) {
        slot.activationFailures.fetch_add(1U, std::memory_order_relaxed);
        AppendLocomotionFlight("move_cycle_unavailable", slot, characterAddress,
            cycleAddress, cycleState);
        return false;
    }
    slot.stateAddress.store(cycleAddress, std::memory_order_release);
    slot.lastObservedNativeState.store(cycleState, std::memory_order_release);

    // 0x38 is the real native Move state. Do not treat the locomotion-class
    // vtable alone as an active Move: the same persistent cycle object can be
    // observed while its state byte is Idle/transitioning. This was the v10
    // regression that let network control write movement values without first
    // starting the native animation state.
    if (IsNativeMoveState(cycleAddress, cycleState)) {
        slot.nativeStateActive.store(true, std::memory_order_release);
        slot.activationReuses.fetch_add(1U, std::memory_order_relaxed);
        if (reacquisition) {
            slot.nativeStateReacquireSuccesses.fetch_add(
                1U, std::memory_order_relaxed);
        }
        AppendLocomotionFlight(reacquisition ? "move_reacquire_reuse" : "move_activate_reuse",
            slot, characterAddress, cycleAddress, cycleState);
        return true;
    }

    const auto moveType = slot.moveType.load(std::memory_order_acquire);
    if (!ApplyRetailMoveType(slot, characterAddress, moveType, true) ||
        !PrepareNativeMovementAction(slot, characterAddress)) {
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.activationFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

#if defined(_MSC_VER)
    __try {
        // This is the HookManager trampoline, not the detoured entry. Retail
        // FUN_00450710 prepares movementAction+08 before making this call; the
        // remote controller reproduced that prerequisite immediately above.
        gActivateLocomotionCycle(reinterpret_cast<void*>(cycleAddress));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.activationFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
#else
    gActivateLocomotionCycle(reinterpret_cast<void*>(cycleAddress));
#endif

    std::uintptr_t activatedCycle = 0U;
    std::uint8_t activatedState = 0U;
    const auto activated = ReadNativeCycle(
            characterAddress, activatedCycle, activatedState) &&
        activatedCycle == cycleAddress &&
        IsNativeMoveState(activatedCycle, activatedState);
    slot.lastObservedNativeState.store(
        activatedState, std::memory_order_release);
    slot.nativeStateActive.store(activated, std::memory_order_release);
    if (!activated) {
        slot.activationFailures.fetch_add(1U, std::memory_order_relaxed);
        AppendLocomotionFlight("move_activate_failed", slot, characterAddress,
            activatedCycle, activatedState);
        return false;
    }
    slot.activationApplications.fetch_add(1U, std::memory_order_relaxed);
    if (reacquisition) {
        slot.nativeStateReacquireSuccesses.fetch_add(
            1U, std::memory_order_relaxed);
    }
    AppendLocomotionFlight(reacquisition ? "move_reacquire_applied" : "move_activate_applied",
        slot, characterAddress, activatedCycle, activatedState);
    return true;
}

bool CallNativeMoveTransition(
    MovementSlot& slot,
    const std::uintptr_t characterAddress,
    const bool reacquisition) noexcept {
    const MovementResourceLease resources(characterAddress);
    if (!resources) {
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.activationFailures.fetch_add(1U, std::memory_order_relaxed);
        AppendLocomotionFlight("move_resources_unavailable", slot, characterAddress,
            slot.stateAddress.load(std::memory_order_relaxed),
            slot.lastObservedNativeState.load(std::memory_order_relaxed));
        return false;
    }
    return CallNativeMoveTransitionWithResources(slot, characterAddress, reacquisition);
}

bool CallNativeIdleTransitionWithResources(
    MovementSlot& slot,
    const std::uintptr_t characterAddress) noexcept {
    std::uintptr_t cycleAddress = 0U;
    std::uint8_t cycleState = 0U;
    if (!ReadNativeCycle(characterAddress, cycleAddress, cycleState)) {
        slot.protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    slot.stateAddress.store(cycleAddress, std::memory_order_release);
    slot.lastObservedNativeState.store(cycleState, std::memory_order_release);
    if (IsNativeIdleState(cycleState)) {
        slot.nativeStateActive.store(false, std::memory_order_release);
        return true;
    }
    AppendLocomotionFlight("idle_transition_attempt", slot, characterAddress, cycleAddress, cycleState);
    if (gEnterIdleCycle == nullptr) return false;
#if defined(_MSC_VER)
    __try {
        gEnterIdleCycle(reinterpret_cast<void*>(cycleAddress));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot.protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
#else
    gEnterIdleCycle(reinterpret_cast<void*>(cycleAddress));
#endif
    std::uintptr_t nextCycle = 0U;
    std::uint8_t nextState = 0U;
    if (!ReadNativeCycle(characterAddress, nextCycle, nextState)) {
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.nativeIdleApplications.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    slot.stateAddress.store(nextCycle, std::memory_order_release);
    slot.lastObservedNativeState.store(nextState, std::memory_order_release);
    const auto stillMoving = IsNativeMoveState(nextCycle, nextState);
    slot.nativeStateActive.store(stillMoving, std::memory_order_release);
    if (!IsNativeIdleState(nextState)) {
        slot.protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    slot.nativeIdleApplications.fetch_add(1U, std::memory_order_relaxed);
    AppendLocomotionFlight("idle_transition_applied", slot, characterAddress, nextCycle, nextState);
    return true;
}

bool CallNativeIdleTransition(
    MovementSlot& slot,
    const std::uintptr_t characterAddress) noexcept {
    const MovementResourceLease resources(characterAddress);
    if (!resources) {
        slot.protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    return CallNativeIdleTransitionWithResources(slot, characterAddress);
}

bool WriteReplicatedYaw(
    const std::uintptr_t characterAddress,
    const float yaw) noexcept {
    return characterAddress != 0U && std::isfinite(yaw) &&
        GuardedWriteValue(characterAddress + ReplicatedYawOffset(), yaw);
}

bool ReadReplicatedYaw(
    const std::uintptr_t characterAddress,
    float& yaw) noexcept {
    if (!GuardedReadValue(characterAddress + ReplicatedYawOffset(), yaw) ||
        !std::isfinite(yaw))
        return false;
    return true;
}

float LoadFloat(const std::atomic<std::uint32_t>& bits) noexcept {
    return std::bit_cast<float>(bits.load(std::memory_order_relaxed));
}

// v338-v342 animated facing (angular graph input) never turned nor moved the
// Client AI (measured speed 0 in v340-v342). v337 behaviour restored: the
// Host facing is applied to the Character yaw each PRE/POST (never position).
constexpr bool kHybridAnimatedFacing = false;
// Below this facing error no turn is requested (3 degrees).
constexpr float kHybridFacingDeadband = 0.0523598776F;
// Turn rate per radian of facing error (1/s): proportional, not "this frame".
constexpr float kHybridFacingGain = 3.0F;
bool HybridFacingActive(const MovementSlot& slot, const std::uintptr_t character) noexcept {
    if (!UsesAIHybridReplication(slot) || ReplicationLeaseExpired(slot) ||
        !slot.hasReplicatedYaw.load(std::memory_order_acquire)) return false;
    float yaw{};
    if (!ReadReplicatedYaw(character, yaw)) return false;
    const auto error=std::remainder(LoadFloat(slot.replicatedYawBits)-yaw,6.28318530717958647692F);
    return std::isfinite(error) && (std::abs(error)>kHybridFacingDeadband ||
        std::abs(LoadFloat(slot.hybridAngularVelocityBits))>0.01F);
}

bool BuildHybridAngularVelocity(const float error, const float previous,
    const float dt, const float maximum, const float acceleration, float& velocity) noexcept {
    if (!std::isfinite(error)||!std::isfinite(previous)||!std::isfinite(dt)||dt<=0.0F||
        !std::isfinite(maximum)||maximum<=0.0F||!std::isfinite(acceleration)||acceleration<=0.0F)
        return false;
    // Same angular units and speed/acceleration tables as 0044D4F0. The Host
    // supplies facing; the local animation graph supplies the resulting turn.
    // v338 asked to close the whole error within one frame, so every request
    // saturated at the native maximum: the graph stayed in its turn and never
    // translated (measured speed 0 with a 9 m/s demand, facing already right).
    const auto desired=std::abs(error)<=kHybridFacingDeadband?0.0F:
        std::clamp(error*kHybridFacingGain,-maximum,maximum);
    velocity=std::clamp(desired,previous-acceleration*dt,previous+acceleration*dt);
    velocity=std::clamp(velocity,-maximum,maximum);
    return std::isfinite(velocity);
}

bool ApplyHybridNativeFacing(MovementSlot& slot, const std::uintptr_t character,
    const std::uintptr_t cycle, const std::uintptr_t movement, const float speed) noexcept {
    if (!UsesAIHybridReplication(slot)) return true;
    StoreProbe(slot,ProbeTargetYaw,LoadFloat(slot.replicatedYawBits));
    if (!kHybridAnimatedFacing) {
        float yaw{};if (ReadReplicatedYaw(character,yaw)) StoreProbe(slot,ProbeYaw,yaw);
        StoreProbe(slot,ProbeFacingStatus,6.0F);return true; // retail angular untouched
    }
    if (gNativeTurnLimits==nullptr||gNativeFrameDelta==nullptr||movement==0U) {StoreProbe(slot,ProbeFacingStatus,1.0F);return false;}
    float current{}, maximum{}, acceleration{}, velocity{};
    std::uintptr_t liveMovement{};
    if (!ReadReplicatedYaw(character,current)||
        !GuardedReadValue(character+kMovementActionObjectOffset,liveMovement)||liveMovement!=movement)
        {StoreProbe(slot,ProbeFacingStatus,2.0F);return false;}
    StoreProbe(slot,ProbeYaw,current);
    float retailAngular{};
    if (GuardedReadValue(cycle+0x48U,retailAngular)) StoreProbe(slot,ProbeRetailAngular,retailAngular);
    // While travelling, retail steering already turns the Character toward the
    // requested travel heading (cycle+48 nonzero). Overwriting it with the
    // Host facing kept the graph from aligning, so it never translated
    // (v338-v341: measured speed 0). Host facing only drives standing turns.
    if (speed>kMoveRequestEpsilon) {
        slot.hybridAngularVelocityBits.store(0U,std::memory_order_relaxed);
        StoreProbe(slot,ProbeFacingStatus,5.0F);StoreProbe(slot,ProbeAngularWritten,retailAngular);
        return true;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        gNativeTurnLimits(reinterpret_cast<void*>(movement),speed,&maximum,&acceleration);
        const auto dt=static_cast<float>(gNativeFrameDelta(0.0027777778F,0.041666668F));
        StoreProbe(slot,ProbeTurnMaximum,maximum);StoreProbe(slot,ProbeTurnAcceleration,acceleration);StoreProbe(slot,ProbeFrameDelta,dt);
        const auto error=std::remainder(LoadFloat(slot.replicatedYawBits)-current,6.28318530717958647692F);
        if (!BuildHybridAngularVelocity(error,LoadFloat(slot.hybridAngularVelocityBits),
                dt,maximum,acceleration,velocity)) {StoreProbe(slot,ProbeFacingStatus,3.0F);return false;}
        if (ReplicationLeaseExpired(slot)) velocity=0.0F;
        // 006618C0 normally copies M+14 to cycle+48. 0066D4B0 copies that to
        // +70 (graph parameter "angularVelocity") before our hook, then passes
        // +48/+58 through 00689200 to native root-motion correction. Update
        // both scalar inputs; never write Character yaw or graph output.
        *reinterpret_cast<float*>(cycle+0x48U)=velocity;
        *reinterpret_cast<float*>(cycle+0x70U)=velocity;
    } __except(EXCEPTION_EXECUTE_HANDLER) { StoreProbe(slot,ProbeFacingStatus,4.0F); return false; }
    StoreProbe(slot,ProbeFacingStatus,0.0F);StoreProbe(slot,ProbeAngularWritten,velocity);
#else
    return false;
#endif
    slot.hybridAngularVelocityBits.store(std::bit_cast<std::uint32_t>(velocity),std::memory_order_relaxed);
    return true;
}

bool WriteNativeCycleControl(
    const std::uintptr_t cycleAddress,
    const NativeCycleControlPlan& plan) noexcept {
    if (cycleAddress == 0U || !std::isfinite(plan.magnitude) ||
        !std::isfinite(plan.worldHeading) ||
        !std::isfinite(plan.intensity))
        return false;
    const auto offsets = NativeCycleOffsets();
#if defined(_MSC_VER)
    __try {
        *reinterpret_cast<float*>(cycleAddress + offsets.magnitude) = plan.magnitude;
        *reinterpret_cast<float*>(cycleAddress + offsets.worldHeading) = plan.worldHeading;
        *reinterpret_cast<float*>(cycleAddress + offsets.intensity) = plan.intensity;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return GuardedWriteValue(cycleAddress + offsets.magnitude, plan.magnitude) &&
        GuardedWriteValue(cycleAddress + offsets.worldHeading, plan.worldHeading) &&
        GuardedWriteValue(cycleAddress + offsets.intensity, plan.intensity);
#endif
}

bool ControlPayloadChanged(
    const MovementSlot& slot,
    const float directionX,
    const float directionY,
    const float directionZ,
    const float deltaYaw,
    const float intensity,
    const bool worldSpace,
    const bool applyPhysicalStep) noexcept {
    if (slot.controlRevision.load(std::memory_order_acquire) == 0U ||
        slot.worldSpace.load(std::memory_order_acquire) != worldSpace ||
        slot.applyPhysicalStep.load(std::memory_order_acquire) !=
            applyPhysicalStep)
        return true;

    const auto previousX = LoadFloat(slot.directionXBits);
    const auto previousY = LoadFloat(slot.directionYBits);
    const auto previousZ = LoadFloat(slot.directionZBits);
    const auto previousYaw = LoadFloat(slot.deltaYawBits);
    const auto previousIntensity = LoadFloat(slot.intensityBits);
    if (applyPhysicalStep) {
        return directionX != previousX || directionY != previousY ||
            directionZ != previousZ || deltaYaw != previousYaw ||
            intensity != previousIntensity;
    }

    const auto previousMagnitude = std::hypot(previousX, previousZ);
    const auto nextMagnitude = std::hypot(directionX, directionZ);
    if (!std::isfinite(previousMagnitude) || !std::isfinite(nextMagnitude) ||
        std::abs(previousMagnitude - nextMagnitude) >=
            kControlMagnitudeEpsilon ||
        std::abs(previousIntensity - intensity) >=
            kControlIntensityEpsilon)
        return true;
    if (previousMagnitude <= 0.0001F || nextMagnitude <= 0.0001F)
        return previousMagnitude != nextMagnitude;
    const auto dot = std::clamp(
        (previousX * directionX + previousZ * directionZ) /
            (previousMagnitude * nextMagnitude),
        -1.0F,
        1.0F);
    return dot < kControlDirectionDotThreshold;
}

bool PublishControlPayload(
    MovementSlot& slot,
    const float directionX,
    const float directionY,
    const float directionZ,
    const float deltaYaw,
    const float intensity,
    const bool worldSpace,
    const bool applyPhysicalStep) noexcept {
    if (!ControlPayloadChanged(
            slot,
            directionX,
            directionY,
            directionZ,
            deltaYaw,
            intensity,
            worldSpace,
            applyPhysicalStep))
        return false;
    // Publishing a new control revision never reconstructs the cycle. The
    // post-AI hook reapplies the latest values after native preprocessing,
    // because the spawned CIVILIAN controller overwrites them every tick.
    slot.directionXBits.store(
        std::bit_cast<std::uint32_t>(directionX),
        std::memory_order_relaxed);
    slot.directionYBits.store(
        std::bit_cast<std::uint32_t>(directionY),
        std::memory_order_relaxed);
    slot.directionZBits.store(
        std::bit_cast<std::uint32_t>(directionZ),
        std::memory_order_relaxed);
    slot.deltaYawBits.store(
        std::bit_cast<std::uint32_t>(deltaYaw),
        std::memory_order_relaxed);
    slot.intensityBits.store(
        std::bit_cast<std::uint32_t>(intensity),
        std::memory_order_relaxed);
    slot.worldSpace.store(worldSpace, std::memory_order_relaxed);
    slot.applyPhysicalStep.store(applyPhysicalStep, std::memory_order_relaxed);
    slot.controlRevision.fetch_add(1U, std::memory_order_release);
    return true;
}

struct FollowerDemand final {
    bool drive{};
    bool networkFollower{};
    bool inputMoving{};
    float directionX{};
    float directionZ{};
    float intensity{};
    std::uint32_t branch{}; // diagnostics, see hybridBranches
    float planarError{};
};

bool LoadNetworkTarget(
    const MovementSlot& slot,
    float& x,
    float& y,
    float& z) noexcept {
    if (!slot.hasNetworkTransform.load(std::memory_order_acquire)) return false;
    if (player_posture::ApproachTarget(slot.characterAddress.load(std::memory_order_acquire), x, y, z))
        return true;
    x = LoadFloat(slot.networkXBits);
    y = LoadFloat(slot.networkYBits);
    z = LoadFloat(slot.networkZBits);
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool ApplyHardNetworkResync(
    MovementSlot& slot,
    const std::uintptr_t characterAddress) noexcept {
    // This policy is selected before the first target is published, including
    // a rebind after a native action. Divergence never authorizes a hybrid snap.
    if (UsesAIHybridReplication(slot)) return false;
    if (slot.initialPlacementPending.load(std::memory_order_acquire)) return false;
    float approachX{}, approachY{}, approachZ{};
    if (player_posture::ApproachTarget(characterAddress, approachX, approachY, approachZ)) return false;

    const auto now = GetTickCount64();
    const auto lease = slot.leaseExpiresAtTickMs.load(std::memory_order_acquire);
    if (lease == 0U || now >= lease) {
        slot.aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
        return false;
    }

    // 5.14 performance pass: the normal follower below already reads the
    // character transform every engine frame. Hard resync is only an emergency
    // guard for multi-metre divergence, so probing it every frame duplicated a
    // native transform read for every remote character. Keep the emergency
    // path responsive while bounding that duplicate work to 10 Hz.
    const auto lastProbe = slot.lastHardResyncProbeTickMs.load(
        std::memory_order_acquire);
    if (lastProbe != 0U &&
        now - lastProbe < kFollowerHardResyncProbeIntervalMilliseconds)
        return false;
    slot.lastHardResyncProbeTickMs.store(now, std::memory_order_release);

    float targetX = 0.0F;
    float targetY = 0.0F;
    float targetZ = 0.0F;
    if (!LoadNetworkTarget(slot, targetX, targetY, targetZ)) return false;

    float currentX = 0.0F;
    float currentY = 0.0F;
    float currentZ = 0.0F;
    if (!ReadTransform(characterAddress, currentX, currentY, currentZ)) return false;

    const auto dx = targetX - currentX;
    const auto dy = targetY - currentY;
    const auto dz = targetZ - currentZ;
    const auto distanceSquared = dx * dx + dy * dy + dz * dz;
    const auto aiFollower = slot.aiReplicationProfile.load(std::memory_order_acquire);
    const auto hardDistance = aiFollower
        ? kAiFollowerHardResyncDistance : kFollowerHardResyncDistance;
    if (!std::isfinite(distanceSquared) || distanceSquared < hardDistance * hardDistance) {
        slot.aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
        return false;
    }
    if (aiFollower) {
        auto since = slot.aiDivergenceSinceTickMs.load(std::memory_order_acquire);
        if (since == 0U) {
            slot.aiDivergenceSinceTickMs.store(now, std::memory_order_release);
            return false;
        }
        if (now - since < kAiFollowerHardResyncGraceMilliseconds) return false;
    }

    const auto last = slot.lastHardResyncAttemptTickMs.load(std::memory_order_acquire);
    if (last != 0U && now - last < kFollowerHardResyncCooldownMilliseconds)
        return false;
    slot.lastHardResyncAttemptTickMs.store(now, std::memory_order_release);
    slot.hardResyncAttempts.fetch_add(1U, std::memory_order_relaxed);
    AppendLocomotionFlight("hard_resync_attempt", slot, characterAddress,
        slot.stateAddress.load(std::memory_order_relaxed),
        slot.lastObservedNativeState.load(std::memory_order_relaxed));

    if (!TeleportCharacter(characterAddress, targetX, targetY, targetZ)) {
        AppendLocomotionFlight("hard_resync_failed", slot, characterAddress,
            slot.stateAddress.load(std::memory_order_relaxed),
            slot.lastObservedNativeState.load(std::memory_order_relaxed));
        return false;
    }
    slot.transformApplications.fetch_add(1U, std::memory_order_relaxed);
    slot.teleportApplications.fetch_add(1U, std::memory_order_relaxed);
    slot.hardResyncApplications.fetch_add(1U, std::memory_order_relaxed);
    slot.aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
    AppendLocomotionFlight("hard_resync_applied", slot, characterAddress,
        slot.stateAddress.load(std::memory_order_relaxed),
        slot.lastObservedNativeState.load(std::memory_order_relaxed));
    return true;
}

bool BuildFollowerDemand(
    const MovementSlot& slot,
    const std::uintptr_t characterAddress,
    FollowerDemand& demand) noexcept {
    demand = {};
    auto inputX = LoadFloat(slot.directionXBits);
    auto inputZ = LoadFloat(slot.directionZBits);
    auto intensity = LoadFloat(slot.intensityBits);
    if (!std::isfinite(inputX) || !std::isfinite(inputZ) ||
        !std::isfinite(intensity))
        return false;

    const auto stale = ReplicationLeaseExpired(slot);
    float approachX{}, approachY{}, approachZ{};
    const auto approaching = player_posture::ApproachTarget(characterAddress, approachX, approachY, approachZ);
    if (stale || approaching) {
        inputX = 0.0F;
        inputZ = 0.0F;
    }

    const auto inputMagnitude = std::hypot(inputX, inputZ);
    demand.inputMoving = !stale &&
        std::isfinite(inputMagnitude) &&
        inputMagnitude > kMoveRequestEpsilon &&
        intensity > kMoveRequestEpsilon;

    float targetX = 0.0F;
    float targetY = 0.0F;
    float targetZ = 0.0F;
    // Never chase a stale network transform after its replication lease has
    // expired. v12.8 could keep settling toward the last old sample even after
    // network updates stopped, which amplified visible desynchronization.
    const auto hasTarget = !stale && LoadNetworkTarget(slot, targetX, targetY, targetZ);
    float errorX = 0.0F;
    float errorZ = 0.0F;
    auto planarError = 0.0F;
    if (hasTarget) {
        float currentX = 0.0F;
        float currentY = 0.0F;
        float currentZ = 0.0F;
        if (ReadTransform(characterAddress, currentX, currentY, currentZ)) {
            errorX = targetX - currentX;
            errorZ = targetZ - currentZ;
            planarError = std::hypot(errorX, errorZ);
            if (!std::isfinite(planarError)) return false;
        }
    }
    demand.networkFollower = hasTarget;
    demand.planarError = planarError;

    if (demand.inputMoving) {
        // Remote players too (513 diag 182335): the forward-only human policy
        // capped catch-up at x1.35, the Host replica trailed its player by
        // >1.5 m (7 teleports in 30 s) and the Alien killed it 3 m away from
        // the real player, so the Client's kill slid the Alien 6 m.
        if (hasTarget) {
            // NPC intent is sampled native displacement, rather than human
            // input. Follow the buffered path with bounded positional feedback
            // in both axes; the human forward-only/lateral-blend policy leaves
            // an NPC behind at corners until its emergency resync fires.
            // Direct AI locomotion uses it too: with the human policy the 511
            // Client Alien lagged 1.4 m behind a slow Host walk (speed x1.35
            // max, retail never translated below ~0.8) and never recovered.
            const auto correctionSpeed = std::min(planarError * kAiFollowerPositionGain,
                kAiFollowerMaximumCorrectionSpeed);
            const auto correctionScale = planarError > kMoveRequestEpsilon ? correctionSpeed / planarError : 0.0F;
            auto velocityX = inputX + errorX * correctionScale;
            auto velocityZ = inputZ + errorZ * correctionScale;
            const auto aiFollower = slot.aiReplicationProfile.load(std::memory_order_acquire);
            if (!aiFollower) {
                // A human proxy never walks backwards while its player moves:
                // keep at least a quarter of the input speed along its travel.
                const auto along = (velocityX * inputX + velocityZ * inputZ) / inputMagnitude;
                const auto minimumAlong = inputMagnitude * 0.25F;
                if (along < minimumAlong) {
                    velocityX += inputX / inputMagnitude * (minimumAlong - along);
                    velocityZ += inputZ / inputMagnitude * (minimumAlong - along);
                }
            }
            const auto magnitude = std::hypot(velocityX, velocityZ);
            // A 15 m/s Host sample (511, t=7 s) drove magnitude 20 and the
            // Client Alien stalled in Move for 0.5 s. Keep demands physical.
            const auto maximum = UsesAIHybridReplication(slot) ? kAiHybridMaximumMagnitude
                : aiFollower ? kAiFollowerMaximumMagnitude : kFollowerMaximumMagnitude;
            if (magnitude > maximum) {
                velocityX *= maximum / magnitude;
                velocityZ *= maximum / magnitude;
            }
            demand.drive = magnitude > kMoveRequestEpsilon;
            demand.branch = 5U;
            demand.directionX = velocityX; demand.directionZ = velocityZ;
            demand.intensity = demand.drive ? intensity : 0.0F;
            return true;
        }
        auto headingX = inputX / inputMagnitude;
        auto headingZ = inputZ / inputMagnitude;
        auto alongTrackError = 0.0F;

        if (hasTarget && planarError > kMoveRequestEpsilon) {
            // Keep the remote player's actual travel direction authoritative.
            // Correct only the lateral component of network error while the
            // player is moving; otherwise chasing the latest snapshot directly
            // can make the NPC cut diagonally or briefly travel backwards.
            alongTrackError = errorX * headingX + errorZ * headingZ;
            const auto lateralX = errorX - headingX * alongTrackError;
            const auto lateralZ = errorZ - headingZ * alongTrackError;
            const auto lateralError = std::hypot(lateralX, lateralZ);
            if (lateralError > kFollowerLateralCorrectionStart) {
                const auto correction = std::clamp(
                    (lateralError - kFollowerLateralCorrectionStart) /
                        (kFollowerLateralCorrectionFull -
                         kFollowerLateralCorrectionStart),
                    0.0F,
                    kFollowerMaximumLateralBlend);
                const auto lateralDirectionX = lateralX / lateralError;
                const auto lateralDirectionZ = lateralZ / lateralError;
                headingX = headingX * (1.0F - correction) +
                    lateralDirectionX * correction;
                headingZ = headingZ * (1.0F - correction) +
                    lateralDirectionZ * correction;
                const auto correctedLength = std::hypot(headingX, headingZ);
                if (correctedLength > kMoveRequestEpsilon) {
                    headingX /= correctedLength;
                    headingZ /= correctedLength;
                }
            }
        }

        // Along-track error changes speed, not travel direction. This lets a
        // slightly lagging replica catch up without steering away from the path
        // taken by the remote player. Never reverse while the source is moving.
        const auto catchupMultiplier = hasTarget
            ? (alongTrackError >= 0.0F
                ? std::clamp(
                    1.0F + alongTrackError * kFollowerCatchupGain,
                    1.0F,
                    kFollowerMaximumSpeedMultiplier)
                : std::clamp(
                    1.0F + alongTrackError * kFollowerAheadBrakeGain,
                    kFollowerMinimumSpeedMultiplier,
                    1.0F))
            : 1.0F;
        const auto magnitude = std::clamp(
            inputMagnitude * catchupMultiplier,
            UsesAIHybridReplication(slot) ? 0.0F : 0.35F,
            slot.aiReplicationProfile.load(std::memory_order_acquire)
                ? kAiFollowerMaximumMagnitude : kFollowerMaximumMagnitude);
        demand.drive = true;
        demand.branch = 1U;
        demand.directionX = headingX * magnitude;
        demand.directionZ = headingZ * magnitude;
        demand.intensity = intensity;
        return true;
    }

    // Once the remote player has stopped, finish the remaining network error
    // through the native locomotion cycle instead of applying a final snap.
    // A settled hybrid actor needs a larger error to restart than to stop.
    // PRE and POST otherwise toggle Idle/Move in one frame at the same .08m
    // boundary, rebuilding the native graph for millimetres of pose noise.
    const auto settleDistance=UsesAIHybridReplication(slot)&&!slot.moveActive.load(std::memory_order_acquire)
        ? kHybridSettleResumeDistance : kFollowerSettleDistance;
    if (hasTarget && planarError > settleDistance) {
        const auto magnitude = std::clamp(
            planarError * 2.5F,
            kFollowerMinimumSettlingMagnitude,
            slot.aiReplicationProfile.load(std::memory_order_acquire)
                ? kAiFollowerMaximumSettlingMagnitude : kFollowerMaximumSettlingMagnitude);
        demand.drive = true;
        demand.branch = 2U;
        demand.directionX = errorX / planarError * magnitude;
        demand.directionZ = errorZ / planarError * magnitude;
        demand.intensity = std::max(intensity, 1.0F);
        return true;
    }

    // A stationary Host can turn. Keep the native locomotion graph, with zero
    // linear velocity, until its angular input has settled; do not spin its pose.
    demand.drive = kHybridAnimatedFacing && !stale && HybridFacingActive(slot,characterAddress);
    demand.branch = demand.drive ? 3U : 4U;
    demand.intensity = 0.0F;
    return true;
}

void StorePreparedFollowerDemand(
    MovementSlot& slot,
    const FollowerDemand& demand,
    const std::uint64_t targetRevision) noexcept {
    slot.preparedDirectionXBits.store(
        std::bit_cast<std::uint32_t>(demand.directionX),
        std::memory_order_relaxed);
    slot.preparedDirectionZBits.store(
        std::bit_cast<std::uint32_t>(demand.directionZ),
        std::memory_order_relaxed);
    slot.preparedIntensityBits.store(
        std::bit_cast<std::uint32_t>(demand.intensity),
        std::memory_order_relaxed);
    slot.preparedNetworkFollower.store(
        demand.networkFollower, std::memory_order_relaxed);
    slot.preparedDrive.store(demand.drive, std::memory_order_relaxed);
    slot.preparedBranch.store(demand.branch, std::memory_order_relaxed);
    slot.preparedPlanarErrorBits.store(std::bit_cast<std::uint32_t>(demand.planarError), std::memory_order_relaxed);
    slot.preparedTargetRevision.store(targetRevision, std::memory_order_release);
}

bool LoadPreparedFollowerDemand(
    const MovementSlot& slot,
    const std::uint64_t targetRevision,
    FollowerDemand& demand) noexcept {
    // A lease can expire between PRE and the retail hook without changing the
    // target revision. Never let that cached demand keep chasing an old pose.
    if (UsesAIHybridReplication(slot) && ReplicationLeaseExpired(slot))
        return false;
    if (targetRevision == 0U ||
        slot.preparedTargetRevision.load(std::memory_order_acquire) !=
            targetRevision)
        return false;
    demand = {};
    demand.drive = slot.preparedDrive.load(std::memory_order_relaxed);
    demand.networkFollower =
        slot.preparedNetworkFollower.load(std::memory_order_relaxed);
    demand.directionX = LoadFloat(slot.preparedDirectionXBits);
    demand.directionZ = LoadFloat(slot.preparedDirectionZBits);
    demand.intensity = LoadFloat(slot.preparedIntensityBits);
    demand.branch = slot.preparedBranch.load(std::memory_order_relaxed);
    demand.planarError = LoadFloat(slot.preparedPlanarErrorBits);
    return std::isfinite(demand.directionX) &&
        std::isfinite(demand.directionZ) &&
        std::isfinite(demand.intensity);
}

bool PrepareFollowerDemand(
    MovementSlot& slot,
    const std::uintptr_t characterAddress,
    FollowerDemand& demand) noexcept {
    const auto revision = slot.targetRevision.load(std::memory_order_acquire);
    if (!BuildFollowerDemand(slot, characterAddress, demand)) return false;
    StorePreparedFollowerDemand(slot, demand, revision);
    return true;
}

bool GetFollowerDemandForHook(
    MovementSlot& slot,
    const std::uintptr_t characterAddress,
    FollowerDemand& demand) noexcept {
    const auto revision = slot.targetRevision.load(std::memory_order_acquire);
    if (LoadPreparedFollowerDemand(slot, revision, demand)) return true;

    // A network command can arrive between the PRE pump and the locomotion
    // hook. Recompute only in that uncommon race; steady-state frames consume
    // the demand already prepared by CharacterManager PRE.
    if (!BuildFollowerDemand(slot, characterAddress, demand)) return false;
    StorePreparedFollowerDemand(slot, demand, revision);
    return true;
}

bool SubmitNativeRequest(
    const std::uintptr_t characterAddress,
    const float x,
    const float y,
    const float z,
    const float intensity) noexcept {
    if (gSubmitLocomotion == nullptr ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(intensity))
        return false;
    alignas(16) const std::array<float, 4> direction{x, y, z, 0.0F};
    const std::uint8_t enabled = 1U;
    static_assert(sizeof(enabled) == NativeLocomotionFlagWidth());
    if (!GuardedWriteValue(
            characterAddress + kLocomotionEnabledOffsetA, enabled) ||
        !GuardedWriteValue(
            characterAddress + kLocomotionEnabledOffsetB, enabled))
        return false;
#if defined(_MSC_VER)
    __try {
        gSubmitLocomotion(
            reinterpret_cast<void*>(
                characterAddress + kLocomotionSubsystemOffset),
            direction.data(),
            intensity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    gSubmitLocomotion(
        reinterpret_cast<void*>(
            characterAddress + kLocomotionSubsystemOffset),
        direction.data(),
        intensity);
    return true;
#endif
}

bool SubmitStopRequest(
    MovementSlot& slot,
    const std::uintptr_t characterAddress) noexcept {
    std::uintptr_t liveCycle{};
    std::uint8_t liveState{};
    if (RetailOwnsPhysicalCycle(characterAddress) ||
        (ReadNativeCycle(characterAddress, liveCycle, liveState) &&
            !IsNativeLocomotionCycle(liveCycle))) {
        // The persistent cycle object changes vtable/layout in place. A cached
        // Move address is now a paired action: writing its old control offsets
        // would corrupt attack data. Release only our mailbox/ownership.
        ReleaseSlot(slot);
        return true;
    }
    // This is a hard release used for teardown or a failed request. Normal
    // network idle uses HoldWorldReplicationIdle and keeps overriding the AI.
    const auto cycleAddress =
        slot.stateAddress.load(std::memory_order_acquire);
    const NativeCycleControlPlan stoppedPlan{};
    const auto controlStopped = !IsNativeLocomotionCycle(cycleAddress) ||
        WriteNativeCycleControl(cycleAddress, stoppedPlan);
    const auto cycleStopped = !IsNativeLocomotionCycle(cycleAddress) ||
        CallNativeIdleTransition(slot, characterAddress);
    const auto nativeStopped = SubmitNativeRequest(
        characterAddress,
        0.0F,
        0.0F,
        0.0F,
        0.0F);
    auto physicalStopped = true;
    if (!UsesAIHybridReplication(slot) &&
        slot.applyPhysicalStep.load(std::memory_order_acquire)) {
        physicalStopped = ApplyPhysicalDelta(
            characterAddress,
            0.0F,
            0.0F,
            0.0F,
            0.0F);
    }
    if (controlStopped && cycleStopped && nativeStopped && physicalStopped) {
        slot.stopApplications.fetch_add(1U, std::memory_order_relaxed);
    } else {
        slot.protectedFailures.fetch_add(1U, std::memory_order_relaxed);
    }
    ReleaseSlot(slot);
    return controlStopped && cycleStopped && nativeStopped && physicalStopped;
}

// Retail ActNPCLocomotionCycle update 0066D4B0 copies the owner's movement
// action smart pointer (+42A8) and reads its +184 with no null check. 458: a
// remote victim replica kept Move (56) after that action was released while
// the Host pair dragged it, and the Alien pilot's PC crashed there (C0000005
// at 0066D4F6). Skipping that frame is strictly safer than retail's fault.
void __fastcall NpcLocomotionUpdateDetour(void* cycle, void*) noexcept {
    const auto original = reinterpret_cast<ActivateLocomotionCycleFn>(gNpcLocomotionUpdateTrampoline);
    if (original == nullptr) return;
    const auto cycleAddress = reinterpret_cast<std::uintptr_t>(cycle);
    std::uintptr_t owner = 0U, movement = 0U;
    if (cycleAddress != 0U &&
        GuardedReadValue(cycleAddress + kNpcLocomotionCycleOwnerOffset, owner) && owner != 0U &&
        GuardedReadValue(owner + kMovementActionObjectOffset, movement) && movement == 0U) {
        // Cycle updates may run on workers: count only, no flight recorder.
        gNpcLocomotionUpdatesWithoutMovement.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    original(cycle);
}

void __fastcall ActivateLocomotionCycleDetour(
    void* cycle,
    void*) noexcept {
    const auto original = reinterpret_cast<ActivateLocomotionCycleFn>(
        gActivateLocomotionTrampoline);
    if (original == nullptr || cycle == nullptr) return;
    const auto cycleAddress = reinterpret_cast<std::uintptr_t>(cycle);
    std::uintptr_t owner = 0U;
    if (!GuardedReadValue(
            cycleAddress + kNpcLocomotionCycleOwnerOffset, owner) ||
        owner == 0U) {
        original(cycle);
        return;
    }
    // 00450710 can request56 while a direct Host passage75 is still live.
    // Consult its exact native owner before feature/slot fallbacks destroy it.
    const auto guard = gNativeAITransitionGuard.load(std::memory_order_acquire);
    if (guard != nullptr && guard(owner, cycleAddress, kNpcLocomotionMoveState)) return;
    if (!runtime_features::Enabled(runtime_features::Feature::RemoteMovementRuntime)) {
        original(cycle);
        return;
    }
    auto* slot = FindSlot(owner);
    if (slot == nullptr) {
        original(cycle);
        return;
    }
    if (!GameplayLoaded()) {
        ReleaseSlot(*slot);
        original(cycle);
        return;
    }

    // If the network is currently asking this Character to move, an AI-side
    // activation can safely be co-opted as a native 0x38 state transition. The
    // post-AI hook still replaces the AI heading/speed before locomotion is
    // submitted, so no AI destination survives. While network-idle, activation
    // remains blocked to prevent the delayed return-to-spawn behavior.
    std::uint8_t state = 0U;
    const auto hasState = GuardedReadValue(
        cycleAddress + kNpcLocomotionCycleStateOffset, state);
    if (RetailOwnsPhysicalCycle(owner)) return;
    if (UsesAIHybridReplication(*slot) && !AIHybridCanOwnCurrentCycle(owner)) {
        ReleaseSlot(*slot);
        original(cycle);
        return;
    }
    if (slot->driveRequested.load(std::memory_order_acquire) &&
        (!hasState || state != kNpcLocomotionMoveState)) {
        original(cycle);
        return;
    }
    if (runtime_features::Enabled(
            runtime_features::Feature::NativeDiagnostics)) {
        slot->aiActivationSuppressions.fetch_add(1U, std::memory_order_relaxed);
    }
    return;
}

void __fastcall EnterIdleCycleDetour(
    void* cycle,
    void*) noexcept {
    const auto original = reinterpret_cast<EnterIdleCycleFn>(
        gEnterIdleCycleTrampoline);
    if (original == nullptr || cycle == nullptr) return;
    const auto cycleAddress = reinterpret_cast<std::uintptr_t>(cycle);
    std::uintptr_t owner = 0U;
    if (!GuardedReadValue(
            cycleAddress + kNpcLocomotionCycleOwnerOffset, owner) ||
        owner == 0U) {
        original(cycle);
        return;
    }
    if (!GameplayLoaded()) {
        ForgetCharacter(owner);
        original(cycle);
        return;
    }
    // Pair admission releases the locomotion slot. Its native graph still has
    // a separate owner: an idle scheduler must not destroy it on its next tick.
    const auto guard = gNativeAITransitionGuard.load(std::memory_order_acquire);
    if (guard != nullptr && guard(owner, cycleAddress, kNpcLocomotionIdleState)) return;
    if (!runtime_features::Enabled(runtime_features::Feature::RemoteMovementRuntime)) {
        original(cycle);
        return;
    }
    auto* slot = FindSlot(owner);
    if (slot == nullptr) {
        original(cycle);
        return;
    }

    if (RetailOwnsPhysicalCycle(owner)) {
        original(cycle); // permit retail attack completion/death cleanup
        return;
    }
    if (UsesAIHybridReplication(*slot) && !AIHybridCanOwnCurrentCycle(owner)) {
        ReleaseSlot(*slot);
        original(cycle);
        return;
    }
    // During a network Move, CIVILIAN AI cannot tear down 0x38 underneath the
    // remote controller. Once the network wants Idle, allowing the engine's
    // own Idle transition is harmless and keeps the animation graph natural;
    // the engine-thread pump independently reconciles the state as a fallback.
    if (!slot->driveRequested.load(std::memory_order_acquire)) {
        original(cycle);
        return;
    }
    if (runtime_features::Enabled(
            runtime_features::Feature::NativeDiagnostics)) {
        slot->aiIdleSuppressions.fetch_add(1U, std::memory_order_relaxed);
    }
    return;
}

void __cdecl ApplyReplicatedEffectiveMoveType(
    const std::uintptr_t movementActionAddress) noexcept {
    if (!runtime_features::Enabled(
            runtime_features::Feature::RemoteMovementRuntime)) {
        return;
    }
    auto* slot = FindSlotByMovementAction(movementActionAddress);
    if (slot == nullptr ||
        !slot->driveRequested.load(std::memory_order_acquire))
        return;
    if (!GameplayLoaded()) {
        ReleaseSlot(*slot);
        return;
    }
    if (RetailOwnsPhysicalCycle(
            slot->characterAddress.load(std::memory_order_acquire))) return;
    if (UsesAIHybridReplication(*slot) && !AIHybridCanOwnCurrentCycle(
            slot->characterAddress.load(std::memory_order_acquire))) return;

    const auto moveType = slot->moveType.load(std::memory_order_acquire);
    if (moveType > 3U) return;

    // Retail state 2 can recompute +0x198 from MovementAction's own internal
    // velocity immediately before this hook. The remote Character is driven by
    // ActNPCLocomotionCycle, so that private velocity is not authoritative for
    // our proxy. Keep both requested (+0x194) and effective (+0x198) gait
    // coherent for downstream animation consumers, but NEVER write the private
    // +0x1AC/+0x1B4 velocity fields. v10.7 did that and corrupted leg motion
    // even at rest because those fields are engine-owned and use a different
    // frame/lifecycle than our replicated world vector.
    const auto applied =
        GuardedWriteValue(
            movementActionAddress + kMovementActionMoveTypeOffset, moveType) &&
        GuardedWriteValue(
            movementActionAddress + kMovementActionCurrentMoveTypeOffset, moveType);
    if (applied) {
        if (runtime_features::Enabled(
                runtime_features::Feature::NativeDiagnostics)) {
            slot->effectiveMoveTypeApplications.fetch_add(
                1U, std::memory_order_relaxed);
        }
    } else {
        slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        slot->movementActionAddress.store(0U, std::memory_order_release);
    }
}

void __cdecl ApplyAtNpcLocomotionPreTransform(
    const std::uintptr_t cycleAddress,
    const std::uintptr_t characterAddress,
    const std::uintptr_t orientationObject) noexcept {
    if (!runtime_features::Enabled(
            runtime_features::Feature::RemoteMovementRuntime)) {
        return;
    }

    // Observe the Host's already-resolved retail steering before any AIMP
    // override. This hook sits after AI/pathfinding/local-steering populated
    // +4C/+50/+5C and before FUN_0043CD30 converts that request to a
    // Character-local vector. The callback is passive and performs no writes.
    if (cycleAddress != 0U && characterAddress != 0U) {
        const auto observer = gNativeAIIntentObserver.load(std::memory_order_acquire);
        if (observer != nullptr) {
            std::uintptr_t owner{};
            std::uint8_t observed{};
            float magnitude{}, worldHeading{}, nativeIntensity{};
            if (GuardedReadValue(cycleAddress + kNpcLocomotionCycleOwnerOffset, owner) &&
                owner == characterAddress &&
                GuardedReadValue(cycleAddress + kNpcLocomotionCycleStateOffset, observed) &&
                GuardedReadValue(cycleAddress + NativeCycleOffsets().magnitude, magnitude) &&
                GuardedReadValue(cycleAddress + NativeCycleOffsets().worldHeading, worldHeading) &&
                GuardedReadValue(cycleAddress + NativeCycleOffsets().intensity, nativeIntensity) &&
                std::isfinite(magnitude) && std::isfinite(worldHeading) &&
                std::isfinite(nativeIntensity) && magnitude >= 0.0F && magnitude <= 64.0F) {
                NativeAIIntent intent{};
                intent.moving = observed == kNpcLocomotionMoveState &&
                    magnitude > kMoveRequestEpsilon && nativeIntensity > kMoveRequestEpsilon;
                if (intent.moving) {
                    intent.directionX = std::sin(worldHeading) * magnitude;
                    intent.directionZ = std::cos(worldHeading) * magnitude;
                    intent.intensity = nativeIntensity;
                }
                std::uintptr_t movementAction{};
                std::uint32_t moveType{};
                if (GuardedReadValue(characterAddress + kMovementActionObjectOffset, movementAction) &&
                    movementAction != 0U &&
                    GuardedReadValue(movementAction + kMovementActionCurrentMoveTypeOffset, moveType) &&
                    moveType <= 3U) {
                    intent.moveType = moveType;
                }
                observer(characterAddress, intent);
            }
        }
    }

    auto* slot = FindSlot(characterAddress);
    if (slot == nullptr || cycleAddress == 0U) return;
    if (!GameplayLoaded()) {
        ReleaseSlot(*slot);
        return;
    }
    if (RetailOwnsPhysicalCycle(characterAddress)) return;

    // ESI is the live retail locomotion cycle and characterAddress is read from
    // ESI+4 by the detour itself. The cycle's address can be reconstructed in
    // place, so its current type must be certified before any control write.
    if (slot->stateAddress.load(std::memory_order_acquire) != cycleAddress) {
        std::uintptr_t owner = 0U;
        if (!GuardedReadValue(
                cycleAddress + kNpcLocomotionCycleOwnerOffset, owner) ||
            owner != characterAddress ||
            !EnsureCertifiedCycle(*slot, cycleAddress)) {
            slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        slot->stateAddress.store(cycleAddress, std::memory_order_release);
    } else if (!EnsureCertifiedCycle(*slot, cycleAddress)) {
        slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    std::uint8_t observedState = 0U;
    if (!GuardedReadValue(
            cycleAddress + kNpcLocomotionCycleStateOffset, observedState)) {
        slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        slot->certifiedCycleAddress.store(0U, std::memory_order_release);
        return;
    }
    slot->lastObservedNativeState.store(
        observedState, std::memory_order_release);
    const auto moveStateActive = observedState == kNpcLocomotionMoveState;
    slot->nativeStateActive.store(
        moveStateActive, std::memory_order_release);

    if (runtime_features::Enabled(
            runtime_features::Feature::NativeDiagnostics)) {
        slot->hookHits.fetch_add(1U, std::memory_order_relaxed);
        slot->lastCharacter.store(characterAddress, std::memory_order_relaxed);
        slot->lastHookTickMs.store(GetTickCount64(), std::memory_order_release);
    }

    // Never feed movement controls into an Idle/transition state. PRE owns the
    // 0x01 -> 0x38 transition; controls start on the next valid 0x38 tick.
    if (!moveStateActive) return;

    FollowerDemand demand{};
    if (!GetFollowerDemandForHook(*slot, characterAddress, demand)) {
        slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (UsesAIHybridReplication(*slot)) {
        // Diagnostics only: what this frame asks and what retail prepared.
        slot->hybridBranches[std::min<std::size_t>(demand.branch,5U)].fetch_add(1U,std::memory_order_relaxed);
        StoreProbe(*slot,ProbeBranch,static_cast<float>(demand.branch));
        StoreProbe(*slot,ProbePlanarError,demand.planarError);
        StoreProbe(*slot,ProbeDemandMagnitude,std::hypot(demand.directionX,demand.directionZ));
        StoreProbe(*slot,ProbeDemandIntensity,demand.intensity);
        StoreProbe(*slot,ProbeInputSpeed,std::hypot(LoadFloat(slot->directionXBits),LoadFloat(slot->directionZBits)));
        constexpr std::pair<HybridProbe,std::uintptr_t> kMotion[]{{ProbeDeltaX,0x730U},{ProbeDeltaY,0x734U},{ProbeDeltaZ,0x738U},
            {ProbeDeltaYaw,0x760U},{ProbeLocalX,0x1030U},{ProbeLocalZ,0x1038U},{ProbeSubmitIntensity,0x1040U},{ProbeSubmitAngular,0x1044U}};
        for (const auto& [index,offset] : kMotion) {
            float value{};
            if (GuardedReadValue(characterAddress+offset,value)) StoreProbe(*slot,index,value);
        }
        float retail{};
        if (GuardedReadValue(cycleAddress+NativeCycleOffsets().magnitude,retail)) StoreProbe(*slot,ProbeRetailMagnitude,retail);
        if (GuardedReadValue(cycleAddress+NativeCycleOffsets().worldHeading,retail)) StoreProbe(*slot,ProbeRetailHeading,retail);
        if (GuardedReadValue(cycleAddress+NativeCycleOffsets().intensity,retail)) StoreProbe(*slot,ProbeRetailIntensity,retail);
        float x{},y{},z{};
        const auto now=GetTickCount64();
        if (ReadTransform(characterAddress,x,y,z)) {
            const auto last=slot->probeLastTickMs.exchange(now,std::memory_order_relaxed);
            const auto lastX=LoadFloat(slot->probeLastXBits),lastZ=LoadFloat(slot->probeLastZBits);
            slot->probeLastXBits.store(std::bit_cast<std::uint32_t>(x),std::memory_order_relaxed);
            slot->probeLastZBits.store(std::bit_cast<std::uint32_t>(z),std::memory_order_relaxed);
            if (last!=0U&&now>last&&now-last<500U) {
                const auto speed=std::hypot(x-lastX,z-lastZ)*1000.0F/static_cast<float>(now-last);
                StoreProbe(*slot,ProbeMeasuredSpeed,speed);
                ProbeStuckLocomotion(*slot,characterAddress,speed,std::hypot(demand.directionX,demand.directionZ),now);
            }
        }
    }

    NativeCycleControlPlan control{};
    if (demand.drive) {
        auto built = false;

        if (demand.networkFollower) {
            if (!slot->hasReplicatedYaw.load(std::memory_order_acquire) ||
                orientationObject == 0U) {
                slot->protectedFailures.fetch_add(
                    1U, std::memory_order_relaxed);
                return;
            }
            float nativeOrientation = 0.0F;
            auto replicatedYaw = LoadFloat(slot->replicatedYawBits);
            // Hybrid facing is now animation-owned. Translate world travel
            // using the actual current yaw, not the requested future facing.
            if constexpr (kHybridAnimatedFacing) {
                if (UsesAIHybridReplication(*slot) &&
                    !ReadReplicatedYaw(characterAddress, replicatedYaw)) {
                    slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
                    return;
                }
            }
            if (!GuardedReadValue(
                    orientationObject + kNativeOrientationOffset,
                    nativeOrientation) ||
                !TryBuildOrientationAwareNativeCycleControlPlan(
                    demand.directionX,
                    demand.directionZ,
                    demand.intensity,
                    replicatedYaw,
                    nativeOrientation,
                    control)) {
                slot->protectedFailures.fetch_add(
                    1U, std::memory_order_relaxed);
                return;
            }
            built = true;
        } else {
            auto characterYaw = 0.0F;
            const auto worldSpace =
                slot->worldSpace.load(std::memory_order_acquire);
            if (!worldSpace &&
                !ReadReplicatedYaw(characterAddress, characterYaw)) {
                slot->protectedFailures.fetch_add(
                    1U, std::memory_order_relaxed);
                return;
            }
            built = TryBuildNativeCycleControlPlan(
                demand.directionX,
                demand.directionZ,
                demand.intensity,
                worldSpace,
                characterYaw,
                control);
        }

        if (!built) {
            slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        if (demand.networkFollower && runtime_features::Enabled(
                runtime_features::Feature::NativeDiagnostics)) {
            slot->followerApplications.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    // Retail overwrites +4C/+50/+5C during its own preprocessing, so the
    // values still must be restored at this exact hook. The write is now one
    // SEH-guarded direct block instead of three VirtualQuery-backed writes.
    if (!ApplyHybridNativeFacing(*slot,characterAddress,cycleAddress,orientationObject,control.magnitude)) {
        // A missing/zero turn resource must not discard the Host's linear
        // movement or leave an old angular request active.
        slot->protectedFailures.fetch_add(1U,std::memory_order_relaxed);
        slot->hybridAngularVelocityBits.store(0U,std::memory_order_relaxed);
        static_cast<void>(GuardedWriteValue(cycleAddress+0x48U,0.0F));
        static_cast<void>(GuardedWriteValue(cycleAddress+0x70U,0.0F));
    }
    if (!WriteNativeCycleControl(cycleAddress, control)) {
        slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        slot->certifiedCycleAddress.store(0U, std::memory_order_release);
        return;
    }
    if (UsesAIHybridReplication(*slot)) {
        StoreProbe(*slot,ProbeControlMagnitude,control.magnitude);
        StoreProbe(*slot,ProbeControlHeading,control.worldHeading);
        StoreProbe(*slot,ProbeControlIntensity,control.intensity);
    }
    slot->appliedControlRevision.store(
        slot->controlRevision.load(std::memory_order_acquire),
        std::memory_order_release);
    if (runtime_features::Enabled(
            runtime_features::Feature::NativeDiagnostics)) {
        slot->requestApplications.fetch_add(1U, std::memory_order_relaxed);
    }

    if (!UsesAIHybridReplication(*slot) &&
        slot->applyPhysicalStep.load(std::memory_order_acquire)) {
        const auto directionX = LoadFloat(slot->directionXBits);
        const auto directionY = LoadFloat(slot->directionYBits);
        const auto directionZ = LoadFloat(slot->directionZBits);
        const auto deltaYaw = LoadFloat(slot->deltaYawBits);
        if (ApplyPhysicalDelta(
                characterAddress,
                directionX,
                directionY,
                directionZ,
                deltaYaw)) {
            slot->physicalApplications.fetch_add(1U, std::memory_order_relaxed);
        } else {
            slot->protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    // World replication is persistent ownership. Snapshot loss makes the
    // follower settle/idle; it must never release the slot back to the AI.
    if (slot->hasNetworkTransform.load(std::memory_order_acquire)) return;

    auto remaining = slot->remainingFrames.load(std::memory_order_acquire);
    if (remaining == 0U || remaining == kPersistentReplicationFrames) return;
    while (remaining > 0U &&
        !slot->remainingFrames.compare_exchange_weak(
            remaining,
            remaining - 1U,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
    }
    if (remaining == 1U) {
        slot->stopPending.store(true, std::memory_order_release);
    }
}

bool ArmCore(
    const std::uintptr_t characterAddress,
    const float directionX,
    const float directionY,
    const float directionZ,
    const float deltaYaw,
    const float intensity,
    const bool worldSpace,
    const bool hasNetworkTransform,
    const float networkX,
    const float networkY,
    const float networkZ,
    const bool hasReplicatedYaw,
    const float replicatedYaw,
    const bool hasMoveType,
    const std::uint32_t moveType,
    const bool applyPhysicalStep,
    const std::uint32_t frameCount,
    std::string& error,
    const FollowerPolicy followerPolicy = FollowerPolicy::Default,
    const std::uint64_t authorityDeadlineMs = 0U) noexcept {
    if (!runtime_features::Enabled(
            runtime_features::Feature::RemoteMovementRuntime)) {
        error = "remote movement runtime disabled by performance isolation";
        return false;
    }
    if (!Installed()) {
        error = "Character remote-player controller is not installed";
        return false;
    }
    if (!GameplayLoaded()) {
        ForgetCharacter(characterAddress);
        error = "Remote movement rejected while gameplay is inactive or unloading";
        return false;
    }
    if (characterAddress == 0U ||
        !std::isfinite(directionX) || !std::isfinite(directionY) ||
        !std::isfinite(directionZ) || !std::isfinite(deltaYaw) ||
        !std::isfinite(intensity)) {
        error = "Character remote-player arguments are invalid";
        return false;
    }
    if (hasNetworkTransform &&
        (!worldSpace || !std::isfinite(networkX) ||
         !std::isfinite(networkY) || !std::isfinite(networkZ))) {
        error = "Authoritative network transform is invalid";
        return false;
    }
    if (hasReplicatedYaw &&
        (!worldSpace || !std::isfinite(replicatedYaw) ||
         replicatedYaw < -3.3F || replicatedYaw > 3.3F)) {
        error = "Replicated yaw is invalid";
        return false;
    }
    if (hasMoveType && (!worldSpace || moveType > 3U)) {
        error = "Retail move_type is outside SLOW_WALK..RUN";
        return false;
    }
    if (frameCount == 0U && !hasNetworkTransform) return Stop(characterAddress);

    const auto hybrid = followerPolicy == FollowerPolicy::AIHybridNativeOnly;
    if (hybrid && !AIHybridCanOwnCurrentCycle(characterAddress)) {
        error = "AI hybrid locomotion requires a qualified ordinary native cycle";
        return false;
    }

    const auto wasKnown = FindSlot(characterAddress) != nullptr;
    auto* slot = FindOrAllocateSlot(characterAddress);
    if (slot == nullptr) {
        error = "No free remote-player locomotion slot is available";
        return false;
    }

    // An AI caller must never arm with the player placement policy and change
    // profiles afterwards. Select the policy before any control/pose publication
    // or preparation can observe this slot. Hybrid callers run on the engine
    // thread, so no native pump can interleave the remaining payload stores.
    const auto previousPolicy = slot->followerPolicy.exchange(
        followerPolicy, std::memory_order_acq_rel);
    const auto policyChanged = previousPolicy != followerPolicy;
    if (policyChanged) {
        // Retain prepared native demand across frames in the same AI policy.
        // Clearing it every frame creates redundant native graph work.
        slot->aiReplicationProfile.store(hybrid, std::memory_order_release);
        slot->initialPlacementPending.store(false, std::memory_order_release);
        slot->aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
        slot->preparedTargetRevision.store(0U, std::memory_order_release);
    }
    const auto renewedHybridLease = hybrid && ReplicationLeaseExpired(*slot);

    auto targetChanged = PublishControlPayload(
        *slot,
        directionX,
        directionY,
        directionZ,
        deltaYaw,
        intensity,
        worldSpace,
        applyPhysicalStep);
    targetChanged = targetChanged || policyChanged || renewedHybridLease;

    const auto networkXBits = std::bit_cast<std::uint32_t>(networkX);
    const auto networkYBits = std::bit_cast<std::uint32_t>(networkY);
    const auto networkZBits = std::bit_cast<std::uint32_t>(networkZ);
    const auto yawBits = std::bit_cast<std::uint32_t>(replicatedYaw);
    const auto previousHasNetwork =
        slot->hasNetworkTransform.load(std::memory_order_acquire);
    const auto previousHasYaw =
        slot->hasReplicatedYaw.load(std::memory_order_acquire);
    if (previousHasNetwork != hasNetworkTransform ||
        (hasNetworkTransform &&
         (slot->networkXBits.load(std::memory_order_relaxed) != networkXBits ||
          slot->networkYBits.load(std::memory_order_relaxed) != networkYBits ||
          slot->networkZBits.load(std::memory_order_relaxed) != networkZBits)) ||
        previousHasYaw != hasReplicatedYaw ||
        (hasReplicatedYaw &&
         slot->replicatedYawBits.load(std::memory_order_relaxed) != yawBits)) {
        targetChanged = true;
    }

    slot->networkXBits.store(networkXBits, std::memory_order_relaxed);
    slot->networkYBits.store(networkYBits, std::memory_order_relaxed);
    slot->networkZBits.store(networkZBits, std::memory_order_relaxed);
    slot->replicatedYawBits.store(yawBits, std::memory_order_relaxed);
    slot->hasNetworkTransform.store(
        hasNetworkTransform, std::memory_order_release);
    slot->hasReplicatedYaw.store(hasReplicatedYaw, std::memory_order_release);

    if (hasMoveType) {
        const auto previousMoveType =
            slot->moveType.exchange(moveType, std::memory_order_acq_rel);
        if (previousMoveType != moveType) {
            targetChanged = true;
            slot->appliedMoveType.store(
                kInvalidMoveType, std::memory_order_release);
            slot->lastMoveTypeValidationTickMs.store(
                0U, std::memory_order_release);
        }
    }

    slot->leaseExpiresAtTickMs.store(
        hasNetworkTransform
            ? (followerPolicy==FollowerPolicy::AIHybridNativeOnly&&authorityDeadlineMs
                ? authorityDeadlineMs : GetTickCount64() + kReplicationLeaseMilliseconds)
            : 0U,
        std::memory_order_release);
    slot->stopPending.store(false, std::memory_order_release);
    const auto requestedMagnitude = std::hypot(directionX, directionZ);
    const auto requestedDrive = std::isfinite(requestedMagnitude) &&
        requestedMagnitude > kMoveRequestEpsilon &&
        intensity > kMoveRequestEpsilon;
    if (slot->driveRequested.exchange(
            requestedDrive, std::memory_order_acq_rel) != requestedDrive) {
        targetChanged = true;
    }
    if (slot->remainingFrames.exchange(
            frameCount, std::memory_order_acq_rel) != frameCount) {
        targetChanged = true;
    }
    if (!wasKnown && hasNetworkTransform && !hybrid) {
        slot->initialPlacementPending.store(true, std::memory_order_release);
        targetChanged = true;
    }

    if (targetChanged || !wasKnown) {
        slot->targetRevision.fetch_add(1U, std::memory_order_release);
        slot->idleConverged.store(false, std::memory_order_release);
        slot->nextIdleValidationTickMs.store(0U, std::memory_order_release);
    }

    // Native state changes are intentionally NOT performed from the command
    // dispatch path. CharacterManager PRE owns normal preparation/transitions;
    // POST only reconciles a target that changed during the retail update.
    // Resolve native pointers once on first bind. Afterwards the engine-thread
    // hooks/pump own their generation-local cached identities.
    if (!wasKnown) {
        std::uintptr_t cycleAddress = 0U;
        std::uint8_t cycleState = 0U;
        if (ReadNativeCycle(characterAddress, cycleAddress, cycleState)) {
            slot->stateAddress.store(cycleAddress, std::memory_order_release);
            slot->lastObservedNativeState.store(
                cycleState, std::memory_order_release);
            slot->nativeStateActive.store(
                IsNativeMoveState(cycleAddress, cycleState),
                std::memory_order_release);
        }
    }
    gLastArmedCharacter.store(characterAddress, std::memory_order_release);
    return true;
}

#if defined(_MSC_VER) && defined(_M_IX86)
__declspec(naked) void MovementActionPostGaitDetour() {
    __asm {
        // mod.main()+5B874 / AI.exe.text+5A874 is the first instruction
        // after retail state-2 gait selection has finished updating +0x198.
        // ESI is the MovementAction. Preserve the machine state, restore the
        // replicated effective gait for owned remote Characters, then let the
        // trampoline execute the displaced movss xmm0,[esi+144].
        sub esp, 080h
        movdqu [esp + 000h], xmm0
        movdqu [esp + 010h], xmm1
        movdqu [esp + 020h], xmm2
        movdqu [esp + 030h], xmm3
        movdqu [esp + 040h], xmm4
        movdqu [esp + 050h], xmm5
        movdqu [esp + 060h], xmm6
        movdqu [esp + 070h], xmm7
        pushfd
        pushad

        push esi
        call ApplyReplicatedEffectiveMoveType
        add esp, 4

        popad
        popfd
        movdqu xmm0, [esp + 000h]
        movdqu xmm1, [esp + 010h]
        movdqu xmm2, [esp + 020h]
        movdqu xmm3, [esp + 030h]
        movdqu xmm4, [esp + 040h]
        movdqu xmm5, [esp + 050h]
        movdqu xmm6, [esp + 060h]
        movdqu xmm7, [esp + 070h]
        add esp, 080h
        jmp dword ptr [gMovementActionPostGaitTrampoline]
    }
}

__declspec(naked) void NpcLocomotionPreTransformDetour() {
    __asm {
        // At mod.main()+26D4FE the retail code has just executed
        //   mov eax,[helperResult]
        //   movss xmm0,[eax+184]
        // so ESI is the locomotion cycle and EAX is the exact native
        // orientation object used by FUN_0043CD30. Preserve the full machine
        // state, inject network controls in that same orientation frame, then
        // execute the displaced LEA/PUSH sequence through the trampoline.
        sub esp, 080h
        movdqu [esp + 000h], xmm0
        movdqu [esp + 010h], xmm1
        movdqu [esp + 020h], xmm2
        movdqu [esp + 030h], xmm3
        movdqu [esp + 040h], xmm4
        movdqu [esp + 050h], xmm5
        movdqu [esp + 060h], xmm6
        movdqu [esp + 070h], xmm7
        pushfd
        pushad

        push eax
        mov eax, dword ptr [esi + 4]
        push eax
        push esi
        call ApplyAtNpcLocomotionPreTransform
        add esp, 12

        popad
        popfd
        movdqu xmm0, [esp + 000h]
        movdqu xmm1, [esp + 010h]
        movdqu xmm2, [esp + 020h]
        movdqu xmm3, [esp + 030h]
        movdqu xmm4, [esp + 040h]
        movdqu xmm5, [esp + 050h]
        movdqu xmm6, [esp + 060h]
        movdqu xmm7, [esp + 070h]
        add esp, 080h
        jmp dword ptr [gNpcLocomotionPreTransformTrampoline]
    }
}

#endif

std::atomic<std::uint64_t> gAnimationFollowFrames{0U};
std::atomic<std::uint64_t> gAnimationFollowRejected{0U};

// AnimationSync: the owner's Character really converges onto the interaction
// (button panel, generator, vent...) while its clip plays. The representation
// plays the same clip in place, so carry it along the owner's network path and
// heading, smoothed and capped per frame like the locker clips.
void FollowAnimationClipPath(MovementSlot& slot, const std::uintptr_t characterAddress) noexcept {
    float targetX{}, targetY{}, targetZ{}, currentX{}, currentY{}, currentZ{};
    if (!LoadNetworkTarget(slot, targetX, targetY, targetZ) ||
        !ReadTransform(characterAddress, currentX, currentY, currentZ)) return;
    targetY += animation_sync::HeightOffset(characterAddress, targetY);
    const auto dx = targetX - currentX, dy = targetY - currentY, dz = targetZ - currentZ;
    const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distance) || distance > kHidingFollowMaximumGap) {
        gAnimationFollowRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (distance > kHidingFollowDeadZone) {
        const auto scale = distance > kHidingFollowMaximumStep ? kHidingFollowMaximumStep / distance : 1.0F;
        const float position[3]{currentX + dx * scale, currentY + dy * scale, currentZ + dz * scale};
        if (!TeleportCharacter(characterAddress, position[0], position[1], position[2]) ||
            !GuardedWriteValue(characterAddress + kCharacterPositionOffset, position[0]) ||
            !GuardedWriteValue(characterAddress + kCharacterPositionOffset + 4, position[1]) ||
            !GuardedWriteValue(characterAddress + kCharacterPositionOffset + 8, position[2])) {
            gAnimationFollowRejected.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
    }
    // Match the owner's heading for ordinary interactions, but preserve the
    // outward half-turn authored by a hiding exit. Bounded turn per frame.
    if (slot.hasReplicatedYaw.load(std::memory_order_acquire)) {
        float yaw{};
        const auto target = animation_sync::PlaybackYaw(characterAddress, LoadFloat(slot.replicatedYawBits));
        if (ReadReplicatedYaw(characterAddress, yaw)) {
            constexpr float kMaximumTurn = 0.12F;
            const auto error = std::remainder(target - yaw, 6.28318530717958647692F);
            if (std::isfinite(error) && std::abs(error) > 0.005F)
                static_cast<void>(WriteReplicatedYaw(characterAddress,
                    yaw + (std::max)(-kMaximumTurn, (std::min)(kMaximumTurn, error))));
        }
    }
    gAnimationFollowFrames.fetch_add(1U, std::memory_order_relaxed);
}

// Locker entry/hidden/exit. The first-person clip's movement (walking in,
// half-turn, crouching place) is the owner's Character transform itself, not
// its pose, and retail snaps a representation moved into a prop back in front
// of it. So the Character entity stays where retail keeps it; only the
// rendered body follows the owner's real path, sampled at the time our own
// clip has reached (AnimationSync records it). Engine thread only.
std::atomic<std::uint64_t> gHidingEntryApproachFrames{0U};
std::atomic<std::uint32_t> gHidingEntryMaxPullMm{0U};
struct HidingVisual final {
    std::uintptr_t character{};
    std::array<float, 3> last{};
    float lastYaw{};
};
std::array<HidingVisual, 40> gHidingVisuals{};
std::atomic<std::uintptr_t> gHidingVisualLast{};
// Physical vent traversal is not replicated to the NPC. Correct its rendered
// root only while the native body clip plays; avoid per-frame teleports.
std::array<HidingVisual, 40> gVentVisuals{};
std::atomic<std::uint64_t> gVentVisualFrames{}, gVentVisualRejected{};
std::atomic<std::uint64_t> gVentVisualRelocations{}, gVentVisualRelocationFailures{};
std::atomic<std::uint32_t> gVentVisualMaxOffsetMm{};
HidingVisual* FindHidingVisual(const std::uintptr_t characterAddress, const bool create) noexcept {
    for (auto& visual : gHidingVisuals) if (visual.character == characterAddress) return &visual;
    if (!create) return nullptr;
    for (auto& visual : gHidingVisuals) if (visual.character == 0U) { visual = {}; visual.character = characterAddress; return &visual; }
    return nullptr;
}
void ShowHidingPath(const std::uintptr_t characterAddress) noexcept {
    std::array<float, 3> target{};
    float targetYaw{}, x{}, y{}, z{}, yaw{};
    if (!animation_sync::HidingTarget(characterAddress, target, targetYaw) ||
        !ReadTransform(characterAddress, x, y, z) || !ReadReplicatedYaw(characterAddress, yaw)) return;
    const auto wx = target[0] - x, wy = target[1] - y, wz = target[2] - z;
    const auto gap = std::sqrt(wx * wx + wy * wy + wz * wz);
    if (!std::isfinite(gap) || gap > kHidingFollowMaximumGap) return;
    auto* visual = FindHidingVisual(characterAddress, true);
    if (visual == nullptr) return;
    visual->last = target; visual->lastYaw = targetYaw;
    const auto mm = static_cast<std::uint32_t>(gap * 1000.0F);
    if (mm > gHidingEntryMaxPullMm.load()) gHidingEntryMaxPullMm.store(mm);
    // World offset into the Character's model space: undo its heading.
    const auto c = std::cos(yaw), s = std::sin(yaw);
    const std::array<float, 3> offset{wx * c - wz * s, wy, wz * c + wx * s};
    const auto turn = static_cast<float>(std::remainder(targetYaw - yaw, 6.28318530717958647692F));
    gHidingVisualLast.store(characterAddress);
    if (animation_sync::SetVisualOffset(characterAddress, true, offset, turn))
        gHidingEntryApproachFrames.fetch_add(1U, std::memory_order_relaxed);
}
HidingVisual* FindVentVisual(const std::uintptr_t character, const bool create) noexcept {
    for (auto& v : gVentVisuals) if (v.character == character) return &v;
    if (!create) return nullptr;
    for (auto& v : gVentVisuals)
        if (v.character == 0U) { v = {}; v.character = character; return &v; }
    return nullptr;
}
void ShowVentVisualPath(MovementSlot& slot, const std::uintptr_t character) noexcept {
    float tx{}, ty{}, tz{}, x{}, y{}, z{}, yaw{};
    if (!LoadNetworkTarget(slot, tx, ty, tz) ||
        !ReadTransform(character, x, y, z) ||
        !ReadReplicatedYaw(character, yaw)) {
        gVentVisualRejected.fetch_add(1U, std::memory_order_relaxed); return;
    }
    const float wx = tx-x, wy = ty-y, wz = tz-z;
    const float gap = std::sqrt(wx*wx + wy*wy + wz*wz);
    if (!std::isfinite(gap) || gap > kHidingFollowMaximumGap) {
        gVentVisualRejected.fetch_add(1U, std::memory_order_relaxed); return;
    }
    auto* visual = FindVentVisual(character, true);
    if (visual == nullptr) {
        gVentVisualRejected.fetch_add(1U, std::memory_order_relaxed); return;
    }
    // Reuse the already verified locker world-to-model convention.
    const auto c = std::cos(yaw), s = std::sin(yaw);
    const std::array<float,3> offset{wx*c-wz*s, wy, wz*c+wx*s};
    const float ownerYaw = slot.hasReplicatedYaw.load(std::memory_order_acquire)
        ? LoadFloat(slot.replicatedYawBits) : yaw;
    const float turn = static_cast<float>(std::remainder(ownerYaw-yaw, 6.28318530717958647692F));
    if (!animation_sync::SetVisualOffset(character, true, offset, turn, true)) {
        gVentVisualRejected.fetch_add(1U, std::memory_order_relaxed); return;
    }
    visual->last = {tx,ty,tz}; visual->lastYaw = ownerYaw;
    gVentVisualFrames.fetch_add(1U, std::memory_order_relaxed);
    const auto mm = static_cast<std::uint32_t>(gap * 1000.0F);
    auto previous = gVentVisualMaxOffsetMm.load(std::memory_order_relaxed);
    while (mm > previous && !gVentVisualMaxOffsetMm.compare_exchange_weak(
        previous, mm, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
void FinishVentVisualPath(const std::uintptr_t character) noexcept {
    auto* visual = FindVentVisual(character, false);
    if (visual == nullptr) return;
    // One endpoint placement after native cycle ownership ends. Its physics
    // outcome remains experimental and is tracked separately.
    if (ApplyAIRecoveryTransform(character, visual->last.data(), visual->lastYaw))
        gVentVisualRelocations.fetch_add(1U, std::memory_order_relaxed);
    else
        gVentVisualRelocationFailures.fetch_add(1U, std::memory_order_relaxed);
    static_cast<void>(animation_sync::SetVisualOffset(character, false, {}, 0.0F));
    *visual = {};
}
// The locker sequence is over: put the entity where the body is shown (the
// owner's real position once out of the prop), then drop the offset.
void FinishHidingPath(const std::uintptr_t characterAddress) noexcept {
    auto* visual = FindHidingVisual(characterAddress, false);
    if (visual == nullptr) return;
    static_cast<void>(ApplyAIRecoveryTransform(characterAddress, visual->last.data(), visual->lastYaw));
    static_cast<void>(animation_sync::SetVisualOffset(characterAddress, false, {}, 0.0F));
    animation_sync::ClearHidingTrack(characterAddress);
    *visual = {};
}

void PumpSlotOnEngineThread(
    MovementSlot& slot,
    const EnginePumpPhase phase) noexcept {
    const auto characterAddress =
        slot.characterAddress.load(std::memory_order_acquire);
    if (characterAddress == 0U || characterAddress == kReservedSlot) return;
    // The rendered body follows the owner's real locker path for the whole
    // sequence (entry clip, hidden, exit clip), after the retail tick.
    if (phase == EnginePumpPhase::PostCharacterManager) {
        if (animation_sync::HidingClipOwned(characterAddress) || player_hiding::Hidden(characterAddress))
            ShowHidingPath(characterAddress);
        else FinishHidingPath(characterAddress);
    }

    if (UsesAIHybridReplication(slot) && !AIHybridCanOwnCurrentCycle(characterAddress)) {
        // Never replace a scripted/reaction/traversal cycle with Move or Idle.
        // Dropping this mailbox does not touch the native action or transform.
        ReleaseSlot(slot);
        return;
    }

    // A replicated clip owns this Character. Native full-body clips retain
    // their own engine root motion and MUST NOT also chase the FP network
    // path. The FP fallback still follows the network; hiding remains visual.
    const auto clipPolicy = animation_sync::MovementPolicyFor(characterAddress);
    if (clipPolicy != animation_sync::MovementPolicy::NativeVentVisual)
        FinishVentVisualPath(characterAddress);
    else if (phase == EnginePumpPhase::PostCharacterManager)
        ShowVentVisualPath(slot, characterAddress);
    if (clipPolicy == animation_sync::MovementPolicy::NativeRoot ||
        clipPolicy == animation_sync::MovementPolicy::NativeVentVisual) {
        slot.nativeAnimationOwned.store(true, std::memory_order_release);
    } else if (slot.nativeAnimationOwned.exchange(false, std::memory_order_acq_rel)) {
        slot.nativeAnimationReleasedAt.store(GetTickCount64(), std::memory_order_release);
        slot.initialPlacementPending.store(false, std::memory_order_release);
        slot.idleConverged.store(false, std::memory_order_release);
        slot.preparedTargetRevision.store(0U, std::memory_order_release);
    }
    if (clipPolicy != animation_sync::MovementPolicy::None) {
        // The admission path already placed a native clip at its interaction
        // anchor. A pending initial-placement teleport would snap it after
        // the clip ends; natural locomotion must reacquire from its final pose.
        slot.initialPlacementPending.store(
            clipPolicy != animation_sync::MovementPolicy::NativeRoot &&
                clipPolicy != animation_sync::MovementPolicy::NativeVentVisual,
            std::memory_order_release);
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.moveActive.store(false, std::memory_order_release);
        slot.driveRequested.store(false, std::memory_order_release);
        slot.certifiedCycleAddress.store(0U, std::memory_order_release);
        slot.preparedTargetRevision.store(0U, std::memory_order_release);
        // A locker clip shows the owner's recorded path on the body instead.
        if (phase == EnginePumpPhase::PostCharacterManager) {
            if (animation_sync::FollowNetworkPath(clipPolicy))
                FollowAnimationClipPath(slot, characterAddress);
        }
        return;
    }

    if (player_hiding_presentation::Active(characterAddress)) {
        slot.hidingOwned.store(true, std::memory_order_release);
        slot.initialPlacementPending.store(true, std::memory_order_release);
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.moveActive.store(false, std::memory_order_release);
        slot.driveRequested.store(false, std::memory_order_release);
        slot.certifiedCycleAddress.store(0U, std::memory_order_release);
        slot.preparedTargetRevision.store(0U, std::memory_order_release);
        // The entity stays in front of the prop; the body shows the owner's
        // hidden transform (ShowHidingPath).
        return;
    }

    if (phase == EnginePumpPhase::PreCharacterManager &&
        !player_hiding::Hidden(characterAddress) &&
        player_posture::PumpOnEngineThread(characterAddress)) {
        if (slot.stopPending.load(std::memory_order_acquire)) {
            ReleaseSlot(slot); // drop ownership without interrupting retail traversal cleanup
            return;
        }
        slot.initialPlacementPending.store(false, std::memory_order_release);
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.moveActive.store(false, std::memory_order_release);
        slot.driveRequested.store(false, std::memory_order_release);
        slot.certifiedCycleAddress.store(0U, std::memory_order_release);
        slot.preparedTargetRevision.store(0U, std::memory_order_release);
        return;
    }

    // A hidden owner is inside a locker: park the invisible representation and
    // re-place it at the network target once the owner is visible again.
    if (player_hiding::Hidden(characterAddress)) {
        slot.hidingOwned.store(true, std::memory_order_release);
        if (slot.stopPending.load(std::memory_order_acquire)) {
            ReleaseSlot(slot);
            return;
        }
        slot.driveRequested.store(false, std::memory_order_release);
        // Stopping publication alone leaves retail's previous Move active.
        // Cancel locomotion on the engine thread, preserving death/paired cycles.
        std::uintptr_t cycle{};
        std::uint8_t cycleState{};
        if (!RetailOwnsPhysicalCycle(characterAddress) &&
            !ai_sync::PairedParticipantVictim(characterAddress) &&
            ReadNativeCycle(characterAddress, cycle, cycleState) &&
            IsNativeLocomotionCycle(cycle)) {
            const NativeCycleControlPlan stopped{};
            static_cast<void>(WriteNativeCycleControl(cycle, stopped));
            static_cast<void>(SubmitNativeRequest(characterAddress, 0.0F, 0.0F, 0.0F, 0.0F));
            if (!IsNativeIdleState(cycleState)) static_cast<void>(CallNativeIdleTransition(slot, characterAddress));
        }
        slot.initialPlacementPending.store(true, std::memory_order_release);
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.moveActive.store(false, std::memory_order_release);
        slot.driveRequested.store(false, std::memory_order_release);
        slot.certifiedCycleAddress.store(0U, std::memory_order_release);
        slot.preparedTargetRevision.store(0U, std::memory_order_release);
        return;
    }

    // Leaving a locker: the representation stays where its hiding clip left
    // it (inside the prop), while the owner already stands in front of it. An
    // idle follower never closes that gap, so re-place it exactly once at the
    // owner's position. Teleporting to an in-flight exit sample misplaced the
    // low locker by 30 cm: wait until the network target has stopped moving.
    if (slot.hidingOwned.exchange(false, std::memory_order_acq_rel)) {
        const auto now = GetTickCount64();
        slot.initialPlacementPending.store(true, std::memory_order_release);
        slot.idleConverged.store(false, std::memory_order_release);
        slot.preparedTargetRevision.store(0U, std::memory_order_release);
        slot.hidingResumeAt.store(now + kHidingResumeSettleMs, std::memory_order_release);
        slot.hidingResumeDeadline.store(now + kHidingResumeMaximumMs, std::memory_order_release);
        slot.hidingSettleXBits.store(0x7FC00000U, std::memory_order_release); // NaN: no sample yet
    }
    if (const auto resumeAt = slot.hidingResumeAt.load(std::memory_order_acquire); resumeAt != 0U) {
        const auto now = GetTickCount64();
        float targetX{}, targetY{}, targetZ{};
        if (LoadNetworkTarget(slot, targetX, targetY, targetZ)) {
            const auto dx = targetX - LoadFloat(slot.hidingSettleXBits);
            const auto dy = targetY - LoadFloat(slot.hidingSettleYBits);
            const auto dz = targetZ - LoadFloat(slot.hidingSettleZBits);
            const auto moved = std::sqrt(dx * dx + dy * dy + dz * dz);
            // NaN (first sample) or a still-moving owner restarts the window.
            if (!(moved <= kHidingResumeSettleDistance)) {
                slot.hidingSettleXBits.store(std::bit_cast<std::uint32_t>(targetX), std::memory_order_release);
                slot.hidingSettleYBits.store(std::bit_cast<std::uint32_t>(targetY), std::memory_order_release);
                slot.hidingSettleZBits.store(std::bit_cast<std::uint32_t>(targetZ), std::memory_order_release);
                slot.hidingResumeAt.store(now + kHidingResumeSettleMs, std::memory_order_release);
                if (now < slot.hidingResumeDeadline.load(std::memory_order_acquire)) return;
            } else if (now < resumeAt) {
                return;
            }
        } else if (now < slot.hidingResumeDeadline.load(std::memory_order_acquire)) {
            return;
        }
        // Settled (or bounded): the initial-placement path below teleports
        // once to the owner's position, then the ordinary follower resumes.
        slot.hidingResumeAt.store(0U, std::memory_order_release);
        slot.hidingResumeDeadline.store(0U, std::memory_order_release);
    }

    float approachX{}, approachY{}, approachZ{};
    if (player_posture::ApproachTarget(characterAddress, approachX, approachY, approachZ)) {
        slot.idleConverged.store(false, std::memory_order_release);
        slot.initialPlacementPending.store(false, std::memory_order_release);
    }

    // Network locomotion must not reacquire Move/Idle, teleport or rewrite yaw
    // over an admitted paired attack or a real death. Incoming commands may
    // update their mailbox, but the retail cycle keeps physical ownership.
    if (phase == EnginePumpPhase::PreCharacterManager)
        static_cast<void>(ReleaseStalePairedHold(slot, characterAddress));
    if (RetailOwnsPhysicalCycle(characterAddress)) {
        // A paired/death cycle retires the hybrid mailbox entirely. Only a
        // newly qualified Host update may rearm it after native completion.
        if (UsesAIHybridReplication(slot) ||
            slot.stopPending.load(std::memory_order_acquire)) ReleaseSlot(slot);
        return;
    }
    // An AI kill/grab already targets this participant even though its own
    // cycle may still be locomotion. Do not move or teleport it under the
    // aligning attacker; the mailbox keeps the latest target for afterwards.
    if (!UsesAIHybridReplication(slot) && ai_sync::PairedParticipantVictim(characterAddress)) return;

    if (runtime_features::Enabled(
            runtime_features::Feature::NativeDiagnostics)) {
        slot.enginePumpApplications.fetch_add(1U, std::memory_order_relaxed);
        slot.lastCharacter.store(characterAddress, std::memory_order_relaxed);
    }

    if (slot.stopPending.load(std::memory_order_acquire)) {
        SubmitStopRequest(slot, characterAddress);
        return;
    }

    const auto targetRevision =
        slot.targetRevision.load(std::memory_order_acquire);
    const auto preparedRevision =
        slot.preparedTargetRevision.load(std::memory_order_acquire);
    const auto hybridLeaseExpired = UsesAIHybridReplication(slot) &&
        ReplicationLeaseExpired(slot);

    if (phase == EnginePumpPhase::PostCharacterManager) {
        // PRE already prepared the frame that retail just consumed. POST only
        // needs to reconcile a command that arrived while CharacterManager was
        // executing, or a native Move state that the in-manager hook observed
        // as lost. This removes the old unconditional second full follower pass.
        const auto moveActive = slot.moveActive.load(std::memory_order_acquire);
        const auto nativeReconcileNeeded =
            moveActive && !slot.nativeStateActive.load(std::memory_order_acquire);
        if (targetRevision == preparedRevision && !nativeReconcileNeeded &&
            !(hybridLeaseExpired && slot.driveRequested.load(std::memory_order_acquire)) &&
            !slot.initialPlacementPending.load(std::memory_order_acquire)) {
            // Preserve replicated yaw ownership after the retail tick, even
            // for a converged idle proxy: retail may touch Character orientation.
            // This is now one direct SEH-guarded scalar write, not a
            // VirtualQuery-backed TryWrite plus a second full follower pass.
            if ((!kHybridAnimatedFacing || !UsesAIHybridReplication(slot)) && CanForceReplicatedYaw(slot) &&
                !hybridLeaseExpired && slot.hasReplicatedYaw.load(std::memory_order_acquire)) {
                const auto yaw = LoadFloat(slot.replicatedYawBits);
                if (!WriteReplicatedYaw(characterAddress, yaw)) {
                    slot.protectedFailures.fetch_add(
                        1U, std::memory_order_relaxed);
                }
            }
            return;
        }
    } else if (slot.idleConverged.load(std::memory_order_acquire) &&
        targetRevision == preparedRevision &&
        !slot.driveRequested.load(std::memory_order_acquire) &&
        !slot.initialPlacementPending.load(std::memory_order_acquire)) {
        // A stable network-idle remote is intentionally almost free. Do one
        // complete validation at 4 Hz so unexpected engine-side displacement or
        // pointer/state changes are still discovered without per-frame queries.
        const auto now = GetTickCount64();
        const auto nextValidation =
            slot.nextIdleValidationTickMs.load(std::memory_order_acquire);
        if (nextValidation != 0U && now < nextValidation) return;
        slot.idleConverged.store(false, std::memory_order_release);
    }

    if ((!kHybridAnimatedFacing || !UsesAIHybridReplication(slot)) && CanForceReplicatedYaw(slot) &&
                !hybridLeaseExpired && slot.hasReplicatedYaw.load(std::memory_order_acquire)) {
        const auto yaw = LoadFloat(slot.replicatedYawBits);
        if (!WriteReplicatedYaw(characterAddress, yaw)) {
            slot.protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    if (!UsesAIHybridReplication(slot) &&
        slot.initialPlacementPending.load(std::memory_order_acquire)) {
        float targetX = 0.0F;
        float targetY = 0.0F;
        float targetZ = 0.0F;
        if (LoadNetworkTarget(slot, targetX, targetY, targetZ)) {
            float currentX = 0.0F;
            float currentY = 0.0F;
            float currentZ = 0.0F;
            if (ReadTransform(characterAddress, currentX, currentY, currentZ)) {
                const auto dx = targetX - currentX;
                const auto dy = targetY - currentY;
                const auto dz = targetZ - currentZ;
                const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (std::isfinite(distance) &&
                    distance <= kFollowerInitialPlacementTolerance) {
                    slot.initialPlacementPending.store(
                        false, std::memory_order_release);
                } else if (std::isfinite(distance) &&
                    TeleportCharacter(
                        characterAddress, targetX, targetY, targetZ)) {
                    slot.initialPlacementPending.store(
                        false, std::memory_order_release);
                    slot.transformApplications.fetch_add(
                        1U, std::memory_order_relaxed);
                    slot.teleportApplications.fetch_add(
                        1U, std::memory_order_relaxed);
                }
            }
        }
    }

    // Emergency divergence correction is still retained, but this call now
    // happens once in the normal PRE pass (or only when POST actually has new
    // work), rather than twice unconditionally per CharacterManager frame.
    const auto nativeReleasedAt = slot.nativeAnimationReleasedAt.load(std::memory_order_acquire);
    if (nativeReleasedAt == 0U || GetTickCount64() - nativeReleasedAt >=
        kNativeAnimationFollowerGraceMilliseconds)
        static_cast<void>(ApplyHardNetworkResync(slot, characterAddress));

    std::uintptr_t cycleAddress = 0U;
    std::uint8_t cycleState = 0U;
    if (!ReadNativeCycle(characterAddress, cycleAddress, cycleState)) {
        AppendLocomotionFlight(
            "pump_cycle_unavailable", slot, characterAddress,
            cycleAddress, cycleState);
        slot.nativeStateActive.store(false, std::memory_order_release);
        slot.stateAddress.store(0U, std::memory_order_release);
        slot.certifiedCycleAddress.store(0U, std::memory_order_release);
        return;
    }
    const auto previousCycle = slot.stateAddress.exchange(
        cycleAddress, std::memory_order_acq_rel);
    if (previousCycle != 0U && previousCycle != cycleAddress) {
        slot.certifiedCycleAddress.store(0U, std::memory_order_release);
        slot.movementActionAddress.store(0U, std::memory_order_release);
        slot.appliedMoveType.store(kInvalidMoveType, std::memory_order_release);
        }
    slot.lastObservedNativeState.store(cycleState, std::memory_order_release);
    auto nativeMoving = cycleState == kNpcLocomotionMoveState &&
        EnsureCertifiedCycle(slot, cycleAddress);
    const auto cachedNative = slot.nativeStateActive.exchange(
        nativeMoving, std::memory_order_acq_rel);
    const auto wasMoving = slot.moveActive.load(std::memory_order_acquire);
    if (wasMoving && cachedNative &&
        (!nativeMoving ||
         (previousCycle != 0U && previousCycle != cycleAddress))) {
        slot.nativeStateLosses.fetch_add(1U, std::memory_order_relaxed);
        AppendLocomotionFlight(
            "native_state_loss", slot, characterAddress,
            cycleAddress, cycleState);
    }

    if (runtime_features::Enabled(
            runtime_features::Feature::LocomotionFlightRecorder)) {
        const auto flightNow = GetTickCount64();
        auto previousHeartbeat =
            gLocomotionFlightHeartbeatTickMs.load(std::memory_order_relaxed);
        if ((previousHeartbeat == 0U ||
             flightNow - previousHeartbeat >= 1000U) &&
            gLocomotionFlightHeartbeatTickMs.compare_exchange_strong(
                previousHeartbeat, flightNow,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            AppendLocomotionFlight(
                "heartbeat", slot, characterAddress,
                cycleAddress, cycleState);
        }
    }

    FollowerDemand demand{};
    if (!PrepareFollowerDemand(slot, characterAddress, demand)) {
        slot.protectedFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    slot.driveRequested.store(demand.drive, std::memory_order_release);

    if (demand.drive) {
        slot.idleConverged.store(false, std::memory_order_release);
        slot.nextIdleValidationTickMs.store(0U, std::memory_order_release);
        if (!ApplyRetailMoveType(
                slot,
                characterAddress,
                slot.moveType.load(std::memory_order_acquire))) {
            slot.protectedFailures.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        if (runtime_features::Enabled(
                runtime_features::Feature::NativeDiagnostics)) {
            if (!wasMoving) {
                slot.enterMoveTransitions.fetch_add(
                    1U, std::memory_order_relaxed);
            } else {
                slot.updateMoveTransitions.fetch_add(
                    1U, std::memory_order_relaxed);
            }
        }
        if (!nativeMoving) {
            if (!CallNativeMoveTransition(slot, characterAddress, wasMoving)) {
                slot.nativeStateActive.store(false, std::memory_order_release);
                slot.moveActive.store(false, std::memory_order_release);
                return;
            }
            std::uintptr_t verifiedCycle = 0U;
            std::uint8_t verifiedState = 0U;
            nativeMoving = ReadNativeCycle(
                    characterAddress, verifiedCycle, verifiedState) &&
                verifiedState == kNpcLocomotionMoveState &&
                EnsureCertifiedCycle(slot, verifiedCycle);
            slot.stateAddress.store(verifiedCycle, std::memory_order_release);
            slot.lastObservedNativeState.store(
                verifiedState, std::memory_order_release);
        }
        slot.nativeStateActive.store(nativeMoving, std::memory_order_release);
        slot.moveActive.store(nativeMoving, std::memory_order_release);
        return;
    }

    if (runtime_features::Enabled(
            runtime_features::Feature::NativeDiagnostics)) {
        if (wasMoving) {
            slot.exitMoveTransitions.fetch_add(1U, std::memory_order_relaxed);
        } else {
            slot.idleHoldApplications.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    if (!IsNativeIdleState(cycleState) &&
        !CallNativeIdleTransition(slot, characterAddress)) {
        return;
    }
    slot.moveActive.store(false, std::memory_order_release);
    slot.nativeStateActive.store(false, std::memory_order_release);
    slot.idleConverged.store(true, std::memory_order_release);
    slot.nextIdleValidationTickMs.store(
        GetTickCount64() + kIdleValidationIntervalMilliseconds,
        std::memory_order_release);
}

RuntimeSnapshot SnapshotSlot(const MovementSlot& slot) noexcept {
    RuntimeSnapshot result{};
    result.posture = player_posture::Snapshot(slot.characterAddress.load(std::memory_order_acquire));
    result.torch = player_torch::Snapshot(slot.characterAddress.load(std::memory_order_acquire));
    result.hiding = player_hiding::Snapshot(slot.characterAddress.load(std::memory_order_acquire));
    result.characterAddress =
        slot.characterAddress.load(std::memory_order_acquire);
    result.remainingFrames =
        slot.remainingFrames.load(std::memory_order_acquire);
    result.activationAttempts =
        slot.activationAttempts.load(std::memory_order_relaxed);
    result.activationApplications =
        slot.activationApplications.load(std::memory_order_relaxed);
    result.activationReuses =
        slot.activationReuses.load(std::memory_order_relaxed);
    result.activationFailures =
        slot.activationFailures.load(std::memory_order_relaxed);
    result.nativeStateLosses =
        slot.nativeStateLosses.load(std::memory_order_relaxed);
    result.nativeStateReacquireAttempts =
        slot.nativeStateReacquireAttempts.load(std::memory_order_relaxed);
    result.nativeStateReacquireSuccesses =
        slot.nativeStateReacquireSuccesses.load(std::memory_order_relaxed);
    result.enterMoveTransitions =
        slot.enterMoveTransitions.load(std::memory_order_relaxed);
    result.updateMoveTransitions =
        slot.updateMoveTransitions.load(std::memory_order_relaxed);
    result.exitMoveTransitions =
        slot.exitMoveTransitions.load(std::memory_order_relaxed);
    result.idleHoldApplications =
        slot.idleHoldApplications.load(std::memory_order_relaxed);
    result.enginePumpApplications =
        slot.enginePumpApplications.load(std::memory_order_relaxed);
    result.aiActivationSuppressions =
        slot.aiActivationSuppressions.load(std::memory_order_relaxed);
    result.aiIdleSuppressions =
        slot.aiIdleSuppressions.load(std::memory_order_relaxed);
    result.nativeIdleApplications =
        slot.nativeIdleApplications.load(std::memory_order_relaxed);
    result.followerApplications =
        slot.followerApplications.load(std::memory_order_relaxed);
    result.effectiveMoveTypeApplications =
        slot.effectiveMoveTypeApplications.load(std::memory_order_relaxed);
    result.hookHits = slot.hookHits.load(std::memory_order_relaxed);
    for (std::size_t i = 0U; i < result.hybridProbe.size(); ++i)
        result.hybridProbe[i] = LoadFloat(slot.hybridProbe[i]);
    for (std::size_t i = 0U; i < result.hybridBranches.size(); ++i)
        result.hybridBranches[i] = slot.hybridBranches[i].load(std::memory_order_relaxed);
    result.finalSubmissions =
        slot.finalSubmissions.load(std::memory_order_relaxed);
    result.requestApplications =
        slot.requestApplications.load(std::memory_order_relaxed);
    result.transformApplications =
        slot.transformApplications.load(std::memory_order_relaxed);
    result.teleportApplications =
        slot.teleportApplications.load(std::memory_order_relaxed);
    result.hardResyncAttempts =
        slot.hardResyncAttempts.load(std::memory_order_relaxed);
    result.hardResyncApplications =
        slot.hardResyncApplications.load(std::memory_order_relaxed);
    result.physicalApplications =
        slot.physicalApplications.load(std::memory_order_relaxed);
    result.stopApplications =
        slot.stopApplications.load(std::memory_order_relaxed);
    result.protectedFailures =
        slot.protectedFailures.load(std::memory_order_relaxed);
    result.lastCharacter =
        slot.lastCharacter.load(std::memory_order_relaxed);
    result.lastStateAddress =
        slot.stateAddress.load(std::memory_order_acquire);
    result.nativeCycleState =
        slot.lastObservedNativeState.load(std::memory_order_acquire);
    result.nativeStateActive =
        slot.nativeStateActive.load(std::memory_order_acquire);
    result.moveActive =
        slot.moveActive.load(std::memory_order_acquire);
    result.armed = result.characterAddress != 0U &&
        result.characterAddress != kReservedSlot &&
        result.remainingFrames != 0U;
    return result;
}

} // namespace

bool TryBuildNativeCycleControlPlan(
    const float directionX,
    const float directionZ,
    const float intensity,
    const bool worldSpace,
    const float characterYaw,
    NativeCycleControlPlan& plan) noexcept {
    if (!std::isfinite(directionX) || !std::isfinite(directionZ) ||
        !std::isfinite(intensity) ||
        (!worldSpace && !std::isfinite(characterYaw)))
        return false;
    const auto magnitude = std::hypot(directionX, directionZ);
    if (!std::isfinite(magnitude)) return false;
    const auto sourceHeading = std::atan2(directionX, directionZ);
    constexpr auto twoPi = 6.28318530717958647692F;
    const auto worldHeading = worldSpace
        ? sourceHeading
        : std::remainder(characterYaw + sourceHeading, twoPi);
    if (!std::isfinite(worldHeading)) return false;
    plan.magnitude = magnitude;
    plan.worldHeading = worldHeading;
    plan.intensity = intensity;
    return true;
}

bool TryBuildOrientationAwareNativeCycleControlPlan(
    const float worldDirectionX,
    const float worldDirectionZ,
    const float intensity,
    const float replicatedYaw,
    const float nativeOrientation,
    NativeCycleControlPlan& plan) noexcept {
    if (!std::isfinite(worldDirectionX) ||
        !std::isfinite(worldDirectionZ) ||
        !std::isfinite(intensity) ||
        !std::isfinite(replicatedYaw) ||
        !std::isfinite(nativeOrientation))
        return false;

    const auto magnitude = std::hypot(worldDirectionX, worldDirectionZ);
    if (!std::isfinite(magnitude)) return false;

    // AI.exe FUN_0066DC20 demonstrates the retail yaw transform:
    //   worldX = localX*cos(yaw) + localZ*sin(yaw)
    //   worldZ = localZ*cos(yaw) - localX*sin(yaw)
    // Invert that exact transform so the remote receiver asks its animation /
    // locomotion graph for the same Character-local motion as the source.
    const auto sinYaw = std::sin(replicatedYaw);
    const auto cosYaw = std::cos(replicatedYaw);
    const auto localX =
        worldDirectionX * cosYaw - worldDirectionZ * sinYaw;
    const auto localZ =
        worldDirectionX * sinYaw + worldDirectionZ * cosYaw;
    if (!std::isfinite(localX) || !std::isfinite(localZ)) return false;

    const auto localHeading = std::atan2(localX, localZ);
    constexpr auto twoPi = 6.28318530717958647692F;
    const auto nativeHeading =
        std::remainder(nativeOrientation + localHeading, twoPi);
    if (!std::isfinite(nativeHeading)) return false;

    plan.magnitude = magnitude;
    plan.worldHeading = nativeHeading;
    plan.intensity = intensity;
    return true;
}

bool Install(
    const std::uintptr_t moduleBase,
    const void* globalStateRootAddress,
    HookManager& hooks,
    std::string& error) {
#if !defined(_MSC_VER) || !defined(_M_IX86)
    error = "Character remote-player hooks require MSVC x86";
    return false;
#else
    if (gInstalled.load(std::memory_order_acquire)) return true;
    if (moduleBase == 0U || globalStateRootAddress == nullptr) {
        error = "AI.exe module base or certified gameplay root is unavailable";
        return false;
    }

    const auto activateAddress =
        moduleBase + kNativeRvas.activateLocomotionCycle;
    const auto idleAddress = moduleBase + kNativeRvas.enterIdleCycle;
    const auto submitAddress = moduleBase + kNativeRvas.submitLocomotion;
    const auto teleportAddress = moduleBase + kNativeRvas.teleportCharacter;
    if (!ValidateActivationFunction(activateAddress)) {
        error =
            "mod.main()+257900 ActNPCLocomotionCycle transition bytes do not match";
        return false;
    }
    if (!ValidateIdleTransitionFunction(idleAddress)) {
        error = "mod.main()+25BDD0 native Idle transition bytes do not match";
        return false;
    }
    if (!ValidateLocomotionVtable(moduleBase)) {
        error =
            "ActNPCLocomotionCycle vtable/update identity does not match mod.main()+26D4B0";
        return false;
    }
    if (!ValidateSubmitFunction(submitAddress)) {
        error = "mod.main()+28D6D0 native locomotion bytes do not match";
        return false;
    }
    if (!ValidateTeleportFunction(teleportAddress)) {
        error =
            "mod.main()+3ECCA0 physical Character repositioning bytes do not match";
        return false;
    }

    constexpr std::array<std::uint8_t,11> turnPrefix{
        0xF3U,0x0FU,0x10U,0x44U,0x24U,0x04U,0x33U,0xC0U,0x8DU,0x51U,0x4CU};
    constexpr std::array<std::uint8_t,4> framePrefix{0xF2U,0x0FU,0x10U,0x05U};
    std::uintptr_t frameClock{};
    if (!ValidateBytes(moduleBase+0x000404A0U,turnPrefix)||
        !ValidateBytes(moduleBase+0x0037D440U,framePrefix)||
        !GuardedReadValue(moduleBase+0x0037D444U,frameClock)||frameClock!=moduleBase+0x012410C0U) {
        error="Native locomotion turn limits/frame clock identity does not match";
        return false;
    }
    gModuleBase = moduleBase;
    gNativeTurnLimits=reinterpret_cast<NativeTurnLimitsFn>(moduleBase+0x000404A0U);
    gNativeFrameDelta=reinterpret_cast<NativeFrameDeltaFn>(moduleBase+0x0037D440U);
    gGlobalStateRootAddress = reinterpret_cast<std::uintptr_t>(globalStateRootAddress);
    gSubmitLocomotion = reinterpret_cast<SubmitLocomotionFn>(submitAddress);
    gTeleportCharacter =
        reinterpret_cast<TeleportCharacterFn>(teleportAddress);

    const auto hookCheckpoint = hooks.InstalledCount();

    HookRequest activationRequest{};
    activationRequest.name = "remote_player_block_ai_move_activation";
    activationRequest.target = reinterpret_cast<void*>(activateAddress);
    activationRequest.detour =
        reinterpret_cast<void*>(&ActivateLocomotionCycleDetour);
    activationRequest.patchLength = 6U;
    activationRequest.expected = MaskedBytes{
        {0x56U, 0x8BU, 0xF1U, 0x8AU, 0x46U, 0x1AU},
        "xxxxxx"};
    activationRequest.relocationSafePrefix = true;
    activationRequest.trampolineOut = &gActivateLocomotionTrampoline;
    if (!hooks.Install(activationRequest, error)) {
        gModuleBase = 0U;
        gEnterIdleCycle = nullptr;
        gSubmitLocomotion = nullptr;
        gTeleportCharacter = nullptr;
        gActivateLocomotionTrampoline = nullptr;
        gEnterIdleCycleTrampoline = nullptr;
        error = "Remote Character AI move-activation hook: " + error;
        return false;
    }
    gActivateLocomotionCycle = reinterpret_cast<ActivateLocomotionCycleFn>(
        gActivateLocomotionTrampoline);

    HookRequest idleRequest{};
    idleRequest.name = "remote_player_block_ai_idle_transition";
    idleRequest.target = reinterpret_cast<void*>(idleAddress);
    idleRequest.detour = reinterpret_cast<void*>(&EnterIdleCycleDetour);
    idleRequest.patchLength = 7U;
    idleRequest.expected = MaskedBytes{
        {0x6AU, 0xFFU, 0x68U, 0x00U, 0x00U, 0x00U, 0x00U},
        "xxx????"};
    idleRequest.relocationSafePrefix = true;
    idleRequest.trampolineOut = &gEnterIdleCycleTrampoline;
    if (!hooks.Install(idleRequest, error)) {
        std::string rollbackError;
        hooks.UninstallFrom(hookCheckpoint, rollbackError);
        gActivateLocomotionTrampoline = nullptr;
        gEnterIdleCycleTrampoline = nullptr;
        gModuleBase = 0U;
        gActivateLocomotionCycle = nullptr;
        gEnterIdleCycle = nullptr;
        gSubmitLocomotion = nullptr;
        gTeleportCharacter = nullptr;
        error = "Remote Character AI idle-transition hook: " + error +
            (rollbackError.empty()
                ? std::string{}
                : "; rollback: " + rollbackError);
        return false;
    }
    gEnterIdleCycle = reinterpret_cast<EnterIdleCycleFn>(
        gEnterIdleCycleTrampoline);

    HookRequest gaitRequest{};
    gaitRequest.name = "remote_player_movement_action_effective_gait";
    gaitRequest.target = reinterpret_cast<void*>(
        moduleBase + kNativeRvas.movementActionPostGaitSelection);
    gaitRequest.detour = reinterpret_cast<void*>(&MovementActionPostGaitDetour);
    // Exact retail bytes at mod.main()+5B874 / AI.exe.text+5A874:
    //   movss xmm0,[esi+144]
    // This is after +0x198 has been selected for state 2 and before the
    // downstream movement/animation calculations consume it.
    gaitRequest.patchLength = 8U;
    gaitRequest.expected = MaskedBytes{
        {0xF3U, 0x0FU, 0x10U, 0x86U, 0x44U, 0x01U, 0x00U, 0x00U},
        "xxxxxxxx"};
    gaitRequest.relocationSafePrefix = true;
    gaitRequest.trampolineOut = &gMovementActionPostGaitTrampoline;
    if (!hooks.Install(gaitRequest, error)) {
        std::string rollbackError;
        hooks.UninstallFrom(hookCheckpoint, rollbackError);
        gActivateLocomotionTrampoline = nullptr;
        gEnterIdleCycleTrampoline = nullptr;
        gMovementActionPostGaitTrampoline = nullptr;
        gModuleBase = 0U;
        gActivateLocomotionCycle = nullptr;
        gEnterIdleCycle = nullptr;
        gSubmitLocomotion = nullptr;
        gTeleportCharacter = nullptr;
        error = "Remote MovementAction effective-gait hook: " + error +
            (rollbackError.empty()
                ? std::string{}
                : "; rollback: " + rollbackError);
        return false;
    }

    HookRequest tickRequest{};
    tickRequest.name = "remote_player_npc_locomotion_pre_transform_control";
    tickRequest.target = reinterpret_cast<void*>(
        moduleBase + kNativeRvas.npcLocomotionPreTransformControl);
    tickRequest.detour =
        reinterpret_cast<void*>(&NpcLocomotionPreTransformDetour);
    // Exact retail bytes at mod.main()+26D4FE / AI.exe.text+26C4FE:
    //   lea edi,[esi+64]; push edi; lea ebx,[esi+68]; push ebx
    // The prefix has no relative operands and is safe to relocate verbatim.
    tickRequest.patchLength = 8U;
    tickRequest.expected = MaskedBytes{
        {0x8DU, 0x7EU, 0x64U, 0x57U, 0x8DU, 0x5EU, 0x68U, 0x53U},
        "xxxxxxxx"};
    tickRequest.relocationSafePrefix = true;
    tickRequest.trampolineOut = &gNpcLocomotionPreTransformTrampoline;
    if (!hooks.Install(tickRequest, error)) {
        std::string rollbackError;
        hooks.UninstallFrom(hookCheckpoint, rollbackError);
        gActivateLocomotionTrampoline = nullptr;
        gEnterIdleCycleTrampoline = nullptr;
        gMovementActionPostGaitTrampoline = nullptr;
        gNpcLocomotionPreTransformTrampoline = nullptr;
        gModuleBase = 0U;
        gActivateLocomotionCycle = nullptr;
        gEnterIdleCycle = nullptr;
        gSubmitLocomotion = nullptr;
        gTeleportCharacter = nullptr;
        error = "ActNPCLocomotionCycle pre-transform control hook: " + error +
            (rollbackError.empty()
                ? std::string{}
                : "; rollback: " + rollbackError);
        return false;
    }

    HookRequest updateRequest{};
    updateRequest.name = "npc_locomotion_update_movement_guard";
    updateRequest.target = reinterpret_cast<void*>(moduleBase + kNativeRvas.npcLocomotionCycleUpdate);
    updateRequest.detour = reinterpret_cast<void*>(&NpcLocomotionUpdateDetour);
    // push ebp; mov ebp,esp; and esp,-10h: no relative operands.
    updateRequest.patchLength = 6U;
    updateRequest.expected = MaskedBytes{{0x55U, 0x8BU, 0xECU, 0x83U, 0xE4U, 0xF0U}, "xxxxxx"};
    updateRequest.relocationSafePrefix = true;
    updateRequest.trampolineOut = &gNpcLocomotionUpdateTrampoline;
    if (!hooks.Install(updateRequest, error)) {
        std::string rollbackError;
        hooks.UninstallFrom(hookCheckpoint, rollbackError);
        gNpcLocomotionUpdateTrampoline = nullptr;
        error = "ActNPCLocomotionCycle update movement guard: " + error;
        return false;
    }

    // The former post-final-submit hook only incremented a diagnostic counter.
    // Removing it eliminates a full XMM/GPR save/restore on every retail NPC
    // locomotion submission without changing the native submission itself.

    if (!player_animation_clip_capture::Install(moduleBase, hooks, error)) {
        std::string rollbackError;
        hooks.UninstallFrom(hookCheckpoint, rollbackError);
        error = "Remote animation clip probes: " + error;
        return false;
    }
    if (!player_posture::InstallAimHook(moduleBase, hooks, error)) {
        std::string rollbackError;
        hooks.UninstallFrom(hookCheckpoint, rollbackError);
        error = "Remote crouched aiming: " + error;
        return false;
    }
    player_posture::Initialize(moduleBase, &RefreshPostureAnimations);
    player_torch::Initialize(moduleBase);
    player_hiding::Initialize(moduleBase);
    player_hiding_presentation::Initialize(moduleBase, reinterpret_cast<std::uintptr_t>(gEnterIdleCycle));
    // Optional: prop attachments only decorate a replayed clip. A failed
    // observation hook must never take remote movement down with it.
    std::string propsError;
    static_cast<void>(animation_sync::props::Install(hooks, moduleBase, propsError));
    animation_sync::Initialize(moduleBase, reinterpret_cast<std::uintptr_t>(gEnterIdleCycle));
    // Optional as well: without the layer-blend redirect clips play unmasked.
    std::string maskError;
    static_cast<void>(animation_sync::InstallMaskHook(hooks, moduleBase, maskError));
    gInstalled.store(true, std::memory_order_release);
    return true;
#endif
}

bool Installed() noexcept {
    return gInstalled.load(std::memory_order_acquire);
}
VentVisualStats VentVisualSnapshot() noexcept {
    return {gVentVisualFrames.load(std::memory_order_relaxed),
        gVentVisualRejected.load(std::memory_order_relaxed),
        gVentVisualRelocations.load(std::memory_order_relaxed),
        gVentVisualRelocationFailures.load(std::memory_order_relaxed),
        gVentVisualMaxOffsetMm.load(std::memory_order_relaxed)};
}

void SetNativeAIIntentObserver(const NativeAIIntentObserver observer) noexcept {
    gNativeAIIntentObserver.store(observer, std::memory_order_release);
}
void SetNativeAITransitionGuard(const NativeAITransitionGuard guard) noexcept {
    gNativeAITransitionGuard.store(guard, std::memory_order_release);
}

void SetPairedVictimHeldQuery(const PairedVictimHeldQuery query) noexcept {
    gPairedVictimHeldQuery.store(query, std::memory_order_release);
}

void PumpOnEngineThread(const EnginePumpPhase phase) noexcept {
    if (!Installed() || !runtime_features::Enabled(
            runtime_features::Feature::RemoteMovementRuntime)) {
        return;
    }
    if (!GameplayLoaded()) {
        ForgetAll();
        // Empty value slots cannot access actors; release any pinned streamed
        // clip requests on this engine thread while their DB is still owned.
        player_hiding_presentation::PumpAllOnEngineThread();
        animation_sync::PumpAllOnEngineThread();
        return;
    }
    if (phase == EnginePumpPhase::PreCharacterManager) {
        player_hiding_local::PumpOnEngineThread();
        hiding_door_presentation::PumpAllOnEngineThread();
        player_hiding_presentation::PumpAllOnEngineThread();
        animation_sync::PumpLocalOnEngineThread();
        animation_sync::PumpAllOnEngineThread();
    }
    for (auto& slot : gSlots) {
        PumpSlotOnEngineThread(slot, phase);
    }
    // Move/Idle activation and the retail tick may overwrite stance. Reconcile
    // after those consumers, even for a proxy without a locomotion mailbox.
    player_posture::ReconcileAllOnEngineThread();
    // Run after all pose corrections, including for actors whose locomotion
    // mailbox was released by a traversal/paired cycle this frame.
    if (phase == EnginePumpPhase::PostCharacterManager) {
        player_torch::PumpAllOnEngineThread();
        player_hiding::PumpAllOnEngineThread();
        animation_sync::ApplyMasksOnEngineThread();
    }
}

bool ArmDirection(
    const std::uintptr_t characterAddress,
    const float directionX,
    const float directionZ,
    const float intensity,
    const std::uint32_t frameCount,
    std::string& error) noexcept {
    return ArmCore(
        characterAddress,
        directionX,
        0.0F,
        directionZ,
        0.0F,
        intensity,
        false,
        false,
        0.0F,
        0.0F,
        0.0F,
        false,
        0.0F,
        false,
        1U,
        false,
        frameCount,
        error);
}

bool ArmWorldDirection(
    const std::uintptr_t characterAddress,
    const float directionX,
    const float directionZ,
    const float intensity,
    const std::uint32_t frameCount,
    std::string& error) noexcept {
    return ArmCore(
        characterAddress,
        directionX,
        0.0F,
        directionZ,
        0.0F,
        intensity,
        true,
        false,
        0.0F,
        0.0F,
        0.0F,
        false,
        0.0F,
        false,
        1U,
        false,
        frameCount,
        error);
}

bool ArmAIIntentReplication(
    const std::uintptr_t characterAddress,
    const float directionX,
    const float directionZ,
    const float intensity,
    const std::uint32_t moveType,
    std::string& error) noexcept {
    if (!ArmCore(
        characterAddress, directionX, 0.0F, directionZ, 0.0F, intensity,
        true, false, 0.0F, 0.0F, 0.0F, false, 0.0F, true, moveType, false,
        kPersistentReplicationFrames, error))
        return false;
    if (auto* slot = FindSlot(characterAddress)) {
        // Intent ownership deliberately carries no network transform. The
        // lease only expires the steering request; it never enables hard
        // positional resync because LoadNetworkTarget remains unavailable.
        slot->aiReplicationProfile.store(false, std::memory_order_release);
        slot->initialPlacementPending.store(false, std::memory_order_release);
        slot->aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
        slot->leaseExpiresAtTickMs.store(
            GetTickCount64() + kReplicationLeaseMilliseconds,
            std::memory_order_release);
    }
    return true;
}

bool HoldAIIntentIdle(
    const std::uintptr_t characterAddress,
    const std::uint32_t moveType,
    std::string& error) noexcept {
    if (!ArmCore(
        characterAddress, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
        true, false, 0.0F, 0.0F, 0.0F, false, 0.0F, true, moveType, false,
        kPersistentReplicationFrames, error))
        return false;
    if (auto* slot = FindSlot(characterAddress)) {
        slot->aiReplicationProfile.store(false, std::memory_order_release);
        slot->initialPlacementPending.store(false, std::memory_order_release);
        slot->aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
        slot->leaseExpiresAtTickMs.store(
            GetTickCount64() + kReplicationLeaseMilliseconds,
            std::memory_order_release);
    }
    return true;
}

bool ReleaseAIIntentOwnership(const std::uintptr_t characterAddress) noexcept {
    auto* slot = FindSlot(characterAddress);
    if (slot == nullptr) return true;
    ReleaseSlot(*slot);
    return true;
}

bool ArmAIHybridReplication(
    const std::uintptr_t characterAddress,
    const float velocityX,
    const float velocityZ,
    const float intensity,
    const float targetX,
    const float targetY,
    const float targetZ,
    const float yaw,
    const std::uint32_t moveType,
    std::string& error,
    const std::uint64_t authorityDeadlineMs) noexcept {
    return ArmCore(
        characterAddress, velocityX, 0.0F, velocityZ, 0.0F, intensity,
        true, true, targetX, targetY, targetZ, true, yaw, true, moveType,
        false, kPersistentReplicationFrames, error,
        FollowerPolicy::AIHybridNativeOnly,authorityDeadlineMs);
}

bool ArmWorldReplication(
    const std::uintptr_t characterAddress,
    const float directionX,
    const float directionZ,
    const float intensity,
    const float networkX,
    const float networkY,
    const float networkZ,
    const float replicationYaw,
    const std::uint32_t moveType,
    const std::uint32_t frameCount,
    std::string& error) noexcept {
    return ArmCore(
        characterAddress,
        directionX,
        0.0F,
        directionZ,
        0.0F,
        intensity,
        true,
        true,
        networkX,
        networkY,
        networkZ,
        true,
        replicationYaw,
        true,
        moveType,
        false,
        frameCount,
        error);
}

bool HoldWorldReplicationIdle(
    const std::uintptr_t characterAddress,
    const float networkX,
    const float networkY,
    const float networkZ,
    const float replicationYaw,
    const std::uint32_t moveType,
    std::string& error) noexcept {
    // Network Idle keeps ownership of the Character without pretending that a
    // native Move is active. If a small residual position error remains, the
    // follower can finish it using the last replicated retail gait; no GoTo or
    // pathfinding destination is ever created.
    if (!ArmCore(
        characterAddress,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        true,
        true,
        networkX,
        networkY,
        networkZ,
        true,
        replicationYaw,
        true,
        moveType,
        false,
        kPersistentReplicationFrames,
        error))
        return false;
    if (auto* slot = FindSlot(characterAddress)) {
        // Idle ownership is released only by an explicit teardown command.
        // A missing network heartbeat must freeze the proxy, not return it to
        // the CIVILIAN spawn objective observed in Cheat Engine.
        slot->leaseExpiresAtTickMs.store(0U, std::memory_order_release);
    }
    return true;
}

bool UseAIReplicationProfile(const std::uintptr_t characterAddress) noexcept {
    auto* slot = FindSlot(characterAddress);
    if (slot == nullptr || !slot->hasNetworkTransform.load(std::memory_order_acquire)) return false;
    if (!slot->aiReplicationProfile.exchange(true, std::memory_order_acq_rel)) {
        // AISync owns birth/frontstage/real discontinuity placement itself.
        // Re-arming after a native attack must converge from its current pose,
        // not teleport on first bind to a potentially older network position.
        slot->initialPlacementPending.store(false, std::memory_order_release);
        slot->aiDivergenceSinceTickMs.store(0U, std::memory_order_release);
        slot->targetRevision.fetch_add(1U, std::memory_order_release);
    }
    return true;
}

bool ApplyReplicatedYaw(
    const std::uintptr_t characterAddress,
    const float yaw,
    std::string& error) noexcept {
    if (!GameplayLoaded()) {
        ForgetCharacter(characterAddress);
        error = "Replicated yaw rejected while gameplay is inactive or unloading";
        return false;
    }
    if (characterAddress == 0U || !std::isfinite(yaw) ||
        yaw < -3.3F || yaw > 3.3F) {
        error = "Replicated yaw or CharacterBase is invalid";
        return false;
    }
    if (WriteReplicatedYaw(characterAddress, yaw)) return true;
    error = "The replicated Character yaw field could not be written safely";
    return false;
}
HidingFollowStats HidingFollowSnapshot() noexcept {
    HidingFollowStats stats{};
    stats.frames = gHidingEntryApproachFrames.load(std::memory_order_relaxed);
    const auto visual = gHidingVisualLast.load();
    stats.refAlignFrames = visual != 0U ? animation_sync::VisualOffsetFrames(visual) : 0U;
    stats.maxPullMm = gHidingEntryMaxPullMm.load(std::memory_order_relaxed);
    return stats;
}

bool ApplyAIRecoveryTransform(std::uintptr_t character,const float* position,float yaw) noexcept {
    if(!GameplayLoaded()||!character||!position||!std::isfinite(yaw)||std::abs(yaw)>3.3F)return false;
    for(std::size_t i=0;i<3;++i)if(!std::isfinite(position[i])||std::abs(position[i])>100000.0F)return false;
    if(!TeleportCharacter(character,position[0],position[1],position[2]))return false;
    return GuardedWriteValue(character+kCharacterPositionOffset,position[0])&&
        GuardedWriteValue(character+kCharacterPositionOffset+4,position[1])&&
        GuardedWriteValue(character+kCharacterPositionOffset+8,position[2])&&WriteReplicatedYaw(character,yaw);
}

void RequestPairedVictimRelease(const std::uintptr_t characterAddress) noexcept {
    if (auto* slot = FindSlot(characterAddress))
        slot->pairedHoldReleaseRequested.store(true, std::memory_order_release);
}

std::uint64_t StalePairedHoldReleases() noexcept {
    return gStaleGrappleReleases.load(std::memory_order_relaxed);
}

bool FinishReplicatedInteraction(const std::uintptr_t characterAddress) noexcept {
    if (!GameplayLoaded()) {
        ForgetCharacter(characterAddress);
        return true;
    }
    std::uintptr_t cycle{};
    std::uint8_t state{};
    if (!ReadNativeCycle(characterAddress, cycle, state)) return false;
    if (state != 0x42U && state != 0x4EU) return true;
    if (gEnterIdleCycle == nullptr) return false;
#if defined(_MSC_VER)
    __try {
#endif
        gEnterIdleCycle(reinterpret_cast<void*>(cycle));
        std::uintptr_t current{};
        std::uint8_t currentState{};
        return ReadNativeCycle(characterAddress, current, currentState) &&
            currentState != 0x42U && currentState != 0x4EU;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

bool RefreshPostureAnimations(const std::uintptr_t characterAddress) noexcept {
    if (!GameplayLoaded()) return false;
    std::uintptr_t cycle{};
    std::uint8_t state{}, grappled{};
    std::uint32_t health{};
    if (!ReadNativeCycle(characterAddress, cycle, state) ||
        (!IsNativeIdleState(state) && state != kNpcLocomotionMoveState) ||
        !GuardedReadValue(characterAddress + 0x297U, grappled) || grappled != 0U ||
        !GuardedReadValue(characterAddress + 0x32CU, health) || health >= 3U) return false;
    const MovementResourceLease resources(characterAddress);
    if (!resources) return false;
    if (state == kNpcLocomotionMoveState) {
        auto* slot = FindSlot(characterAddress);
        if (slot == nullptr || !PrepareNativeMovementAction(*slot, characterAddress)) return false;
    }
    const auto refreshed = player_posture::RefreshAnimationCycle(characterAddress,
        reinterpret_cast<std::uintptr_t>(gEnterIdleCycle),
        reinterpret_cast<std::uintptr_t>(gActivateLocomotionCycle));
    if (auto* slot = FindSlot(characterAddress)) {
        AppendLocomotionFlight(refreshed
            ? (state == kNpcLocomotionMoveState ? "posture_move_refresh" : "posture_idle_refresh")
            : "posture_refresh_failed", *slot, characterAddress, cycle, state);
    }
    return refreshed;
}

// A non-lethal grab leaves a PlayerSync representation in its 4F victim cycle:
// retail waits for the victim's PLAYER controller to get up, which a
// representation (NPC controller) never provides, so it lay in T-pose and
// RetailOwnsPhysicalCycle kept PlayerSync out for good. The caller proves that
// the attacker has left the pair and the representation is alive.
bool FinishOrphanedVictimCycle(const std::uintptr_t characterAddress, const std::uintptr_t expectedCycle) noexcept {
    if (!GameplayLoaded() || gEnterIdleCycle == nullptr || expectedCycle == 0U) return false;
    std::uintptr_t cycle{};
    std::uint8_t state{};
    if (!ReadNativeCycle(characterAddress, cycle, state) || cycle != expectedCycle || state != 0x4FU) return false;
#if defined(_MSC_VER)
    __try {
#endif
        gEnterIdleCycle(reinterpret_cast<void*>(cycle));
        std::uintptr_t current{};
        std::uint8_t currentState{};
        const bool ended = ReadNativeCycle(characterAddress, current, currentState) && currentState != 0x4FU;
        if (ended) if (auto* slot = FindSlot(characterAddress)) {
            // The 4E destructor that normally clears +297/+29A already ran.
            slot->pairedHoldReleaseRequested.store(true, std::memory_order_release);
            slot->targetRevision.fetch_add(1U, std::memory_order_release);
        }
        return ended;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

bool Arm(
    const std::uintptr_t characterAddress,
    const float deltaX,
    const float deltaY,
    const float deltaZ,
    const float deltaYaw,
    const std::uint32_t frameCount,
    std::string& error) noexcept {
    return ArmCore(
        characterAddress,
        deltaX,
        deltaY,
        deltaZ,
        deltaYaw,
        1.0F,
        false,
        false,
        0.0F,
        0.0F,
        0.0F,
        false,
        0.0F,
        false,
        1U,
        true,
        frameCount,
        error);
}

void Stop() noexcept {
    for (auto& slot : gSlots) {
        const auto character =
            slot.characterAddress.load(std::memory_order_acquire);
        if (character == 0U || character == kReservedSlot) continue;
        std::string ignored;
        static_cast<void>(player_hiding::Publish(character, false, {}, ignored));
        static_cast<void>(player_hiding_presentation::Publish(character, {}, ignored));
        static_cast<void>(animation_sync::Publish(character, {}, ignored));
        hiding_door_presentation::Forget(character);
        slot.remainingFrames.store(0U, std::memory_order_release);
        slot.stopPending.store(true, std::memory_order_release);
    }
}

void ForgetCharacter(const std::uintptr_t characterAddress) noexcept {
    player_posture::Forget(characterAddress);
    player_torch::Forget(characterAddress);
    player_hiding::Forget(characterAddress);
    player_hiding_presentation::Forget(characterAddress);
    animation_sync::Forget(characterAddress);
    hiding_door_presentation::Forget(characterAddress);
    if (auto* slot = FindSlot(characterAddress); slot != nullptr) ReleaseSlot(*slot);
}

void ForgetAll() noexcept {
    player_posture::ForgetAll();
    player_torch::ForgetAll();
    player_hiding::ForgetAll();
    player_hiding_presentation::ForgetAll();
    player_hiding_local::ForgetAll();
    animation_sync::ForgetAll();
    hiding_door_presentation::ForgetAll();
    for (auto& slot : gSlots) {
        const auto character =
            slot.characterAddress.load(std::memory_order_acquire);
        if (character == 0U || character == kReservedSlot) continue;
        ReleaseSlot(slot);
    }
}

bool Stop(const std::uintptr_t characterAddress) noexcept {
    if (!GameplayLoaded()) {
        ForgetCharacter(characterAddress);
        return true;
    }
    // Explicit teardown also releases a locker if locomotion already retired.
    std::string ignored;
    static_cast<void>(player_hiding::Publish(characterAddress, false, {}, ignored));
    static_cast<void>(player_hiding_presentation::Publish(characterAddress, {}, ignored));
    static_cast<void>(animation_sync::Publish(characterAddress, {}, ignored));
    hiding_door_presentation::Forget(characterAddress);
    auto* slot = FindSlot(characterAddress);
    if (slot == nullptr) return true;
    slot->remainingFrames.store(0U, std::memory_order_release);
    slot->stopPending.store(false, std::memory_order_release);
    return SubmitStopRequest(*slot, characterAddress);
}

std::uint32_t RemainingFrames() noexcept {
    return RemainingFrames(
        gLastArmedCharacter.load(std::memory_order_acquire));
}

std::uint32_t RemainingFrames(
    const std::uintptr_t characterAddress) noexcept {
    const auto* slot = FindSlot(characterAddress);
    return slot == nullptr
        ? 0U
        : slot->remainingFrames.load(std::memory_order_acquire);
}

RuntimeSnapshot Snapshot() noexcept {
    const auto last = gLastArmedCharacter.load(std::memory_order_acquire);
    if (const auto* slot = FindSlot(last)) return SnapshotSlot(*slot);
    for (const auto& slot : gSlots) {
        const auto character =
            slot.characterAddress.load(std::memory_order_acquire);
        if (character != 0U && character != kReservedSlot)
            return SnapshotSlot(slot);
    }
    return RuntimeSnapshot{};
}

RuntimeSnapshot Snapshot(const std::uintptr_t characterAddress) noexcept {
    const auto* slot = FindSlot(characterAddress);
    if (slot != nullptr) return SnapshotSlot(*slot);
    RuntimeSnapshot result{};
    result.characterAddress = characterAddress;
    result.posture = player_posture::Snapshot(characterAddress);
    result.torch = player_torch::Snapshot(characterAddress);
    result.hiding = player_hiding::Snapshot(characterAddress);
    return result;
}


} // namespace aimp::character_direct_step_runtime
