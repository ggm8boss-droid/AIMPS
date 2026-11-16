#include <intrin.h>
#include "aimp/AISync.h"
#include "aimp/AlienPvPLocalController.h"
#include "aimp/AlienPvPSpatialPolicy.h"
#include "aimp/AlienPvPResponsiveStop.h"
#include "aimp/AlienPvPControlRuntime.h"
#include "aimp/AlienPvPControlPolicy.h"
#include "aimp/AlienPvPVents.h"
#include "aimp/AlienPvPInputIsolation.h"
#include "aimp/AlienPvPVentNode.h"
#include "aimp/AlienPvPActionPolicy.h"
#include "aimp/AlienPvPAttackManager.h"
#include "aimp/AlienPvPNativeActionAbi.h"
#include "aimp/PlayerHidingRuntime.h"
#include "aimp/PlayerLifecycle.h"
#include "aimp/PlayerSpectatePolicy.h"
#include <limits>
#include "aimp/AISyncProfiling.h"
#include "aimp/AISyncExecutionPolicy.h"
#include "aimp/AISyncNavigation.h"
#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/AISyncNativeOwnership.h"
#include "aimp/CharacterIncarnation.h"
#include "aimp/AISyncRouteRuntime.h"
#include "aimp/AISyncMarkerIdentity.h"
#include "aimp/AISyncRouteExecution.h"
#include "aimp/AISyncRecoveryMotion.h"
#include "aimp/AISyncHybridMotion.h"
#include "aimp/AISyncHostProducer.h"
#include "aimp/AISyncInteraction.h"
#include "aimp/AISyncInteractionRuntime.h"
#include "aimp/AISyncPairRetirement.h"
#include "aimp/AISyncMeshStorage.h"
#include "aimp/CharacterResourceAdmission.h"
#include "aimp/BuildProfile.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/WorldEntityRegistry.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <utility>
#include <type_traits>
#include <memory>

namespace aimp::ai_sync {
namespace {
static_assert(sizeof(void*) == 4U);
static_assert(std::is_trivially_copyable_v<State>);
static_assert(std::is_trivially_copyable_v<ImpactEvent>);
constexpr std::size_t kParticipants = 32U, kActors = 256U, kRing = 1024U;
constexpr ULONGLONG kSendPeriod = 50ULL, kRefreshPeriod = 250ULL, kTimeout = 1500ULL;
constexpr ULONGLONG kManagerDormancyPeriod = 500ULL;
// Client AI locomotion through a local retail Move (see DriveNativeFollow).
constexpr bool kNativeFollowLocomotion=true;
// GetTickCount64 advances in ~16 ms steps: Host pose velocities from it read
// 15 m/s for an 8 m/s sprint and Client interpolation moved in 16 ms stairs.
// QPC-derived milliseconds on the GetTickCount64 epoch, so values still mix
// with arrival ticks taken from GetTickCount64 (within one tick step).
ULONGLONG PreciseTickMs() noexcept {
    static const auto base=[]{
        LARGE_INTEGER frequency{},counter{};
        QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&counter);
        return std::array<long long,3>{frequency.QuadPart,counter.QuadPart,
            static_cast<long long>(GetTickCount64())};
    }();
    LARGE_INTEGER counter{};
    if(base[0]<=0||!QueryPerformanceCounter(&counter))return GetTickCount64();
    const auto delta=counter.QuadPart-base[1];
    return static_cast<ULONGLONG>(base[2]+(delta/base[0])*1000LL+(delta%base[0])*1000LL/base[0]);
}
constexpr ULONGLONG kDeadTargetGracePeriod = 1500ULL;
constexpr ULONGLONG kPostDeathArbitrationGracePeriod = 2000ULL;
constexpr ULONGLONG kPostDeathRecentSenseHold = 2000ULL;
constexpr ULONGLONG kMotionLease = 600ULL;
constexpr std::uint32_t kStateFlags = PoseValid | Moving | ActionValid | InteractionValid | RoleTaskValid |
    ExecutionValid | ConsequenceEvent | ParticipantConsequenceEvent | ShotEvent | NavigationEvent;
constexpr std::uint32_t kOrderedFlags = ConsequenceEvent | ParticipantConsequenceEvent | ShotEvent | NavigationEvent;
constexpr std::uint32_t kExecutionFlags = ExecutionTransition | ExecutionCertified | ExecutionPaired;
constexpr std::uint32_t kInteractionFlags = InteractionActorIsSource |
    InteractionTargetIsParticipant | InteractionPairValid | InteractionCycleActive | InteractionNativeOutcomeRequired |
    InteractionSourceCycleRetained;
constexpr std::uint32_t kOutcomeFlags = OutcomePrepared | OutcomeVictimAdmitted |
    OutcomeSourceEnded | OutcomeVictimEnded | OutcomeTerminal | OutcomeNativeDeath;
constexpr std::uint32_t kLifecycleFlags = LifecycleSpatialDiscontinuity | LifecycleTraversalAction |
    LifecycleWithdrawn | LifecycleWithdrawState2 | LifecycleHostDormant | LifecycleCharacterBackstage;
constexpr float kLifecycleDiscontinuityDistance = 3.5F;
constexpr float kLifecycleVerticalDiscontinuity = 2.5F;
constexpr std::uintptr_t kPositionOffset = 0x70U, kYawOffset = 0x174U;
constexpr std::uintptr_t kInteractionManagerGlobalRva = 0x012F0C88U;
constexpr std::uintptr_t kRoleTaskCleanupRva = 0x00361A90U;
constexpr auto kCharacterRegistry = character_direct_step_runtime::NativeCharacterRegistry();
constexpr std::uintptr_t kCharacterBackstageRva = 0x0038B140U;
constexpr std::uintptr_t kCharacterFrontstageRva = 0x003C0220U;

constexpr std::uintptr_t kCharacterResolverRegistryRva = 0x0134EF40U;
constexpr std::uintptr_t kCharacterResolverRva = 0x002545C0U;
constexpr std::uintptr_t kResolveImpactDamageRva = 0x003A0A40U;

constexpr float kMotionThreshold = 0.02F, kMaximumVelocity = 20.0F;
using ManagerUpdate = void(__thiscall*)(void*, std::uint32_t, float);
using Coordinator = void(__thiscall*)(void*, std::uint32_t);
using CharacterInteraction = std::uint32_t(__thiscall*)(void*, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint64_t, std::int32_t);
using PairedTransitionA = void(__thiscall*)(void*, std::uintptr_t, std::int32_t, std::uint32_t);
using PairedTransitionB = void(__thiscall*)(void*, std::uintptr_t, std::int32_t,
    std::uint32_t, std::uint32_t);
using CharacterImpact = void(__thiscall*)(void*, int*);
using ResolveImpactDamage = std::int16_t(__fastcall*)(int*);
using SensesCandidates = void(__thiscall*)(void*);
using SenseEvent = void(__thiscall*)(void*, std::uintptr_t);
using SenseCharacterResolver = std::uintptr_t(__thiscall*)(void*, const std::uint32_t*);
// FUN_0065FB80: ECX = action cycle, two stack arguments, RET 8, AL result.
using PairedInitialize = std::uint8_t(__thiscall*)(void*, const std::uint32_t*, std::int32_t);
// Victim transition 0065C450 takes the source Character, parameters and source
// cycle on the stack (RET 0C). Victim initializer 00675F10 takes ECX only.
using PairedVictimTransition = void(__thiscall*)(void*, std::uintptr_t, const void*, std::uintptr_t);
using PairedVictimInitialize = void(__thiscall*)(void*);
// 0064E6C0: ECX = source 4E cycle, target Character and victim flag, RET 8.
using PairedVictimConvergence = void(__thiscall*)(void*, std::uintptr_t, std::uint32_t);
// Native message 17 handler: ECX = original 0x40-byte packet, Character argument,
// RET 4. Keep the packet produced by 00662670, including native convergence.
using PairedVictimEvent = void(__thiscall*)(const void*, void*);
using CharacterMessage = void(__thiscall*)(void*, const void*);
// Certified 007CC900: ECX=Character, NINE stack words, RET 24h.
// The descriptor (word 7) and two trailing flags were lost in the decompiler.
using NativeDamage = void(__thiscall*)(void*, std::uint32_t, std::uintptr_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
// 007B4AD0 is the retail health commit, ECX=Character+320, RET 8.
// It notifies the health state machine and Character callbacks (007AD730).
using NativeHealthCommit = void(__thiscall*)(void*, std::uint32_t, std::uintptr_t);
using Hostility = std::uint32_t(__thiscall*)(void*, std::uintptr_t);
// Both certified functions take ECX only and use plain RET. The record
// validator returns AL (true means discard), not a defined full EAX.
using TargetEligibility = std::uint32_t(__thiscall*)(void*);
using RecordInvalid = std::uint8_t(__thiscall*)(void*);
using BackstageUpdate = std::uint8_t(__thiscall*)(void*);
using BackstageHoldUpdate = void(__thiscall*)(void*);
// 006EFD10: ECX=BT condition, owner and execution context on stack, RET 8.
using TargetIsPlayer = std::uint32_t(__thiscall*)(void*, std::uintptr_t, std::uintptr_t);
using NavigationGoal = void(__thiscall*)(void*, const float*);
using NavigationUpdate = void(__thiscall*)(void*);
// Owned shared path-query reference at +0/+4, ECX only, plain RET.
using NavigationRelease = void(__thiscall*)(void*);
constexpr ULONGLONG kParticipantProbePeriod = 1000ULL;
constexpr std::size_t kParticipantProbeRing = 256U;
struct ParticipantProbe final {
    std::uint64_t sessionCookie{}, peer{}, selectedPeer{}, pendingPeer{}, tick{};
    std::uint32_t nativeGeneration{}, worldGeneration{}, entityA{}, entityB{}, characterClass{};
    std::uint32_t retailEligibility{}, executionStateA{}, executionStateB{}, executionPhase{};
    std::uint32_t activeSenseMask{}, primarySenseIndex{0xFFFFFFFFU}, recentSenseIndex{0xFFFFFFFFU};
    std::uint32_t threatState{0xFFFFFFFFU};
    std::uint8_t executionEnabled{}, recordActive{}, recordSelected{}, recordForced{};
    std::uint8_t ownerEnabled{}, ownerSenseBlocked{}, sensesDisabled{};
    std::uint32_t ownerRetailEligibility{};
    bool ownerExecutionReadable{}, ownerGatesReadable{};
    bool living{}, pairReadable{}, paired{}, executionReadable{}, sensesReadable{}, recordPresent{};
    bool recordReadable{}, selectedReadable{}, selectedPresent{}, threatReadable{}, threatPresent{};
    bool selectedTerminalDead{}, pendingReadable{}, pendingPresent{}, pendingLiving{}, targetSignalsReadable{};
    std::uint8_t haveTargetSignal{}, haveNextTargetSignal{};
    float sightInput{}, sightTimestamp{};
    bool signalRegistryReadable{}, signalRegistryFound{}, signalRegistryMatches{};
    bool visibilityFlagsReadable{};
    std::uint8_t ownerHidden{}, targetHidden{};
    bool playerComponentsReadable{}, playerControlPresent{}, playerNavigationPresent{};
    std::uint8_t playerControlLocal{};
    bool backstageReadable{};
    std::uint32_t backstageState{}, backstageKeyA{}, backstageKeyB{};
    std::uint8_t backstageEnabled{}, backstagePolicy{};
    bool backstageControllerReadable{}, backstageDistanceReadable{};
    std::uint32_t backstageControllerState{};
    std::uint8_t backstageRequestC{}, backstageRequestD{}, backstageRequestE{};
    float backstageVentDistance{};
    bool navigationCached{}, navigationGoalKnown{}, navigationQueryPending{}, navigationGoalDirty{};
    std::uint32_t navigationRouteCount{}, navigationEmptyRouteRetries{};
};
// Engine-thread producer, serialized worker consumers. Samples hold values,
// never pointers to Characters/records that a later worker might dereference.
std::array<ParticipantProbe, kParticipantProbeRing> gParticipantProbes{};
std::atomic<std::uint32_t> gParticipantProbeWrite{}, gParticipantProbeRead{};

enum class TargetSuccessionPhase : std::uint32_t {
    UpdateEntry = 1U,
    PostRecovery = 2U,
    PostRetailUpdate = 3U,
    RecoveryEvaluation = 4U,
    PairRetired = 5U,
    SuccessorPreserved = 6U,
    FallbackReset = 7U,
    FallbackResetRejected = 8U,
    InteractionAdvanced = 9U,
    PostPrepass = 10U,
    PostCoordinator = 11U
};
struct TargetSelectionSnapshot final {
    std::uintptr_t selectedRecord{}, selectedTarget{}, pendingRecord{}, pendingTarget{};
    std::uint64_t selectedPeer{}, pendingPeer{};
    std::uint32_t selectedBirth{}, pendingBirth{};
    std::uint8_t haveTargetSignal{}, haveNextTargetSignal{};
    bool selectionReadable{}, selectedReadable{}, selectedPresent{}, selectedLiving{}, selectedTerminalDead{};
    bool pendingReadable{}, pendingPresent{}, pendingLiving{}, pendingTerminalDead{}, targetSignalsReadable{};
};
struct TargetSuccessionProbe final {
    std::uint64_t sessionCookie{}, tick{}, retiredPeer{}, selectedPeer{}, pendingPeer{};
    std::uint32_t nativeGeneration{}, worldGeneration{}, entityA{}, entityB{}, birth{};
    std::uint32_t interactionRevision{}, pairRevision{}, phase{};
    std::uint32_t selectedBirth{}, pendingBirth{};
    std::uint8_t haveTargetSignal{}, haveNextTargetSignal{};
    bool selectionReadable{}, selectedReadable{}, selectedPresent{}, selectedLiving{}, selectedTerminalDead{};
    bool pendingReadable{}, pendingPresent{}, pendingLiving{}, pendingTerminalDead{}, targetSignalsReadable{};
};
std::array<TargetSuccessionProbe, kParticipantProbeRing> gTargetSuccessionProbes{};
std::atomic<std::uint32_t> gTargetSuccessionProbeWrite{}, gTargetSuccessionProbeRead{};
std::mutex gParticipantProbeConsumerMutex;
constexpr std::size_t kPoseHistory = 8U;
struct PoseSample final {
    ULONGLONG tick{};
    float position[3]{}, yaw{}, velocityX{}, velocityZ{};
};
struct Actor {
    bool initialPlacementReady{};
    transactions::Scope spawnDiagnosticScope{};
    std::uint32_t spawnDiagnosticBirth{};
    ULONGLONG lastSpawnDiagnostic{};
    std::uint32_t appliedLifecycleRevision{}; // Last Host native discontinuity replicated here.
    ULONGLONG diagnosticUpdateTick{};
    std::uintptr_t character{}, tree{}, lastRoleTask{};
    std::uint32_t runtimeId{}, entityA{}, entityB{}, characterClass{}, birth{};
    State desired{}, previous{};
    // Authenticated carrier head copied under the IPC lock, independently of
    // capability execution. Never used for placement or acknowledged as applied.
    State received{};
    State recoveryPose{};
    // The Host owns NPC spatial truth. This timeline never drives an NPC brain.
    hybrid_motion::Timeline directPoseTimeline{};
    std::uint32_t directPoseBirth{};
    // Do not teleport at stream frequency: only recover prolonged multi-metre divergence.
    ULONGLONG directPoseDivergenceSince{}, directPoseLastEmergencyCorrection{};
    // Progress anchor: the body is blocked when retail root motion runs but
    // the Character does not leave this point (514: 4-5 s frozen walks).
    float directPoseAnchor[3]{};
    ULONGLONG directPoseAnchorAt{}, directPoseLastStuckCorrection{}, directPoseAssistAt{};
    movement::ProgressClock recoveryProgress{};
    movement::ObservedTrail observedTrail{};
    ULONGLONG recoveryPoseAt{},recoveryLastTick{},recoveryLastHardSnap{};
    std::uint32_t recoveryHandoffLifecycleRevision{};
    bool recoveryFollowing{},recoveryNativeArmed{};
    // Client native follow: a LOCAL retail Move (kind1) owned by this mod,
    // whose goal tracks the Host pose. Retail navigation/locomotion executes it.
    struct NativeFollow {
        std::uintptr_t object{},control{};
        ULONGLONG createdAt{},stillSince{},retryAt{},divergedSince{},idleYawSince{};
        float divergedAnchorX{},divergedAnchorZ{};
        std::uint32_t failures{};
        bool stopping{};
    } follow{};
    // Latest Host state in causal stream order (never an older queued carrier).
    std::uint32_t streamRevision{}, streamBirth{}, streamCycleState{}, streamPairRevision{}, streamCycleType{};
    // Execution latency trace (diagnostics only).
    ULONGLONG execCaptureTick{}, arrivalExecTick{}, intakeExecTick{}, arrivalExecUtc{}, lastMotionSample{};
    std::uint32_t publishedExecutionRevision{}, arrivalExecRevision{}, intakeExecRevision{}, tracedExecRevision{};
    std::uint32_t execWaitReason{}, execWaits{}, execWaitMask{}, execDiagnosticMask{};
    ULONGLONG lastSend{}, lastReceive{}, lastManagerUpdate{};
    ULONGLONG lastPoseSample{}, lastProxyRefresh{}, lastInteractionAttempt{}, lastInteractionCapture{};
    ULONGLONG deadTargetSince{}, stationarySince{}, lastParticipantProbe{}, lastPresentationProbe{};
    ULONGLONG intentDivergenceSince{}, postDeathArbitrationSince{}, lastLivingParticipantSense{};
    ULONGLONG lastSurvivorTargetAssignment{};
    std::uint64_t lastLivingParticipantSensePeer{};
    std::uint32_t postDeathArbitrationRevision{};
    std::uint64_t postDeathRetiredPeer{};
    std::uintptr_t observedSelectedRecord{}, observedPendingRecord{};
    std::uint64_t observedSelectedPeer{}, observedPendingPeer{};
    std::uint32_t observedSelectedBirth{}, observedPendingBirth{};
    std::uint8_t observedHaveTargetSignal{}, observedHaveNextTargetSignal{};
    bool observedSelectedPresent{}, observedSelectedLiving{}, observedSelectedTerminalDead{};
    bool observedPendingPresent{}, observedPendingLiving{}, observedPendingTerminalDead{};
    bool observedSelectionReadable{}, observedSelectedReadable{}, observedPendingReadable{}, observedTargetSignalsReadable{};
    bool targetSelectionObserved{};
    std::uint32_t participantProbeCursor{};
    std::uint32_t completedRevision{}, completedInteractionRevision{};
    std::uint32_t completedActionRevision{}, completedRoleTaskRevision{}, completedLifecycleRevision{};
    std::uint32_t completedExecutionRevision{};
    std::uint32_t replayedExecutionRevision{};
    std::uint8_t capturedExecutionSerial{};
    std::uintptr_t capturedExecutionCycle{};
    transactions::Scope capturedExecutionScope{};
    bool capturedExecutionScopeKnown{};
    execution::NativeExecutionAnchor replayedCycle{};
    // NativeIntent21/25 complete as instructions before their native passage
    // finishes. Keep causal provenance for that exact local cycle/serial.
    std::uint64_t passageSequence{}, passageCommand{};
    ULONGLONG lastExecutionAttempt{};
    std::uintptr_t capturedNavigationRequest{};
    std::uint32_t capturedNavigationEpoch{};
    std::uint32_t rejectedNavigationRevision{}, rejectedNavigationRequestRevision{};
    ULONGLONG lastNavigationAttempt{};
    MovementOwner movementOwner{MovementOwner::None};
    std::uint32_t interactionActionRevision{}, interactionActionPathHash{};
    std::uint32_t completedPairRevision{}, completedTransitionRevision{}, recoveredInteractionRevision{};
    std::uint32_t resolvedPairRevision{}, localPairFlagsLow{}, localPairFlagsHigh{};
    std::int32_t localPairAttachment{-1};
    std::uintptr_t resolvedPairTarget{};
    std::int32_t resolvedSourceVariant{};
    // Engine-thread-only placement preparation, scoped to the exact live pair.
    // No victim/health/animation state is held or authored by this record.
    ULONGLONG pairPlacementSince{};
    std::uintptr_t pairPlacementTarget{};
    std::uint32_t pairPlacementBirth{}, pairPlacementPair{}, pairPlacementTransition{}, pairPlacementTargetBirth{};
    bool pairPlacementPending{};
    // Client-observer only: remembers a remote fatal pair that has completed
    // locally so delayed Host outro snapshots cannot reopen its 4E presentation.
    // Revisions are value-only authority epochs; no native pointer crosses this
    // boundary or survives a birth/world/session reset.
    std::uint32_t observerReleasedBirth{}, observerReleasedPairRevision{};
    std::uint32_t observerReleasedInteractionRevision{}, observerReleasedActionRevision{};
    std::uint32_t observerReleasedExecutionRevision{}, observerReleasedRoleTaskRevision{};
    std::uint32_t observerReleasedLifecycleRevision{}, observerReleasedLocomotionRevision{};
    std::uint32_t observerReleasedNavigationRevision{}, observerReleasedNavigationRequestRevision{};
    std::uint64_t observerReleasedTargetPeer{};
    bool observerHandoffNeutral{};
    ULONGLONG lastObserverFinishAttempt{};
    // First tick at which this observer kept its local 4E through the Host's
    // stage-6 outro; 0 while no outro is retained.
    ULONGLONG observerOutroSince{};
    std::uint32_t admissionRetryBirth{}, admissionRetryPair{}, admissionRetries{};
    NavigationGoalState convergenceGoal{};
    ULONGLONG convergenceDivergenceSince{};
    bool convergenceActive{};
    // Pursuit: the Host Alien can travel fast in cycles other than the 0x38
    // Move cycle (charge/run-at-target), publishing zero steering intensity.
    // Its measured speed drives a bounded local path toward the Host pose.
    NavigationGoalState pursuitGoal{};
    float pursuitSampleX{}, pursuitSampleZ{}, hostSpeed{};
    ULONGLONG pursuitSampleTick{}, pursuitDivergenceSince{};
    std::uint32_t pursuitSampleRevision{};
    bool pursuitSampleValid{}, pursuitActive{};
    std::uint32_t convergenceAuthorityRequest{};
    std::uint32_t remoteBirth{}, remoteRevision{};
    std::uint32_t healthRevision{}, damageRevision{};
    health::DamageReplayCursor damageCursor{};
    std::uintptr_t reactionCycle{};
    std::uint8_t reactionSerial{};
    std::uint32_t participantDamageRevision{}, participantRuntimeId{};
    std::uint64_t participantDamagePeer{};
    health::DamageReplayCursor participantDamageCursor{};
    std::uint32_t shotRevision{}, navigationEventRevision{};
    std::uint32_t queuedShotRevision{};
    ULONGLONG navigationEventSince{};
    combat::ReplayCursor shotCursor{};
    struct PendingRemoteShot final { combat::ShotEvent shot{}; ULONGLONG deadline{}; };
    std::deque<PendingRemoteShot> pendingRemoteShots{};
    std::deque<combat::Observation> pendingShots{};
    std::deque<health::DamageObservation> pendingDamage{};
    float lastPosition[3]{};
    bool blocked{}, duplicate{}, faulted{}, hasLastPosition{}, interactionOwnsCycle{};
    bool traversalAction{}, hostDormant{}, withdrawKnown{};
    bool backstageKnown{}, backstage{}, placementPending{}, roleTaskCleanupPending{};
    std::uint32_t withdrawState{};
    bool alienSuppressed{};
    bool nativeSourcePrimed{}, transactionDrainStarted{}, transactionDrained{}, transactionNavigationDrainEntered{};
    transactions::Scope nativeSourceScope{};
    std::uint64_t transactionDrainSerial{};
    std::uint32_t suppressedExecutionRevision{};
    std::uint32_t originalAlienWithdraw{};
    // Client presentation only; bounded, engine-thread owned, no native pointers.
    std::array<PoseSample, kPoseHistory> poses{};
    std::size_t poseCount{};
    std::uint32_t poseRevision{};
    bool poseInterpolating{};
};
std::array<Actor, kActors> gActors{}; // engine thread only
bool gPublishingDamage{};
bool gPublishingCombat{}, gPublishingNavigation{}, gPublishingCarriers{};
// Value-only observations distinguish transport liveness, physical drift and
// hidden presentation. One sample/actor/second plus pair admission boundaries.
struct PresentationProbe final {
    std::uint64_t sessionCookie{}, peer{}, tick{}, utcMs{};
    std::uint32_t nativeGeneration{}, worldGeneration{}, role{}, phase{};
    std::uint32_t entityA{}, entityB{}, birth{}, revision{}, pairRevision{}, lifecycleFlags{}, interactionFlags{};
    std::uint32_t observerReleasedPairRevision{};
    NavigationGoalState navigation{};
    std::uint32_t movementOwner{};
    std::uint32_t cycleType{}, stage{0xFFFFFFFFU}, withdrawState{0xFFFFFFFFU};
    float local[3]{}, authority[3]{}, victim[3]{}, authorityVictim[3]{};
    std::uint8_t cycleState{}, cycleFlags{}, hidden{}, invisible{}, enabled{};
    bool invisibleReadable{};
    bool sourceGraphPresent{};
    navigation::HeadObservation navigationHead{};
    std::uint32_t authorityMoveType{}, requestedMoveType{0xFFFFFFFFU}, effectiveMoveType{0xFFFFFFFFU};
    float authorityIntensity{};
    std::uint32_t hybridHold{};
    bool hybridNativeArmed{};
    std::uint64_t hybridSampleAge{},hybridNativeHooks{},hybridNativeWrites{},hybridNativeFailures{};
    std::array<float,28> hybridProbe{};std::array<std::uint64_t,6> hybridBranches{};
    std::uint16_t victimHp{};
    std::uint32_t victimHealthState{};
    bool victimHealthReadable{};
    std::uintptr_t sourceCharacter{}, victimCharacter{}, localCharacter{};
    std::uint32_t sourceRuntimeId{};
    std::uint32_t victimRuntimeId{}, victimEntityA{}, victimEntityB{}, victimCycleType{};
    std::uint32_t localRuntimeId{}, localHp{}, localHealthState{};
    NativeOutcome outcome{};
    std::uint8_t victimCycleState{}, victimAcquired{}, victimEnded{}, victimInteractive{};
    bool victimLocal{}, victimRepresentation{}, localHealthReadable{};
    bool positionReadable{}, victimReadable{}, visibilityReadable{}, cycleReadable{}, ownsPair{}, placementPending{}, pairPlacementPending{};
    bool observerHandoffNeutral{};
};
std::array<PresentationProbe, kParticipantProbeRing> gPresentationProbes{};
std::atomic<std::uint32_t> gPresentationProbeWrite{}, gPresentationProbeRead{};
std::atomic<std::uint64_t> gPresentationSamples{}, gPresentationDropped{};
struct SpawnPlacementProbe {
    transactions::Scope scope{};
    std::uint64_t tick{},sessionCookie{};
    std::uint32_t role{},nativeGeneration{},worldGeneration{},entityA{},entityB{},birth{},revision{},characterClass{};
    std::array<float,3> host{},local{};
    float hostYaw{},localYaw{},positionError{},yawError{};
    bool readable{},confirmed{};
};
std::mutex gSpawnProbeGate;
std::mutex gLifecycleTraceGate;
// One record per Host execution transition: Host capture->publish, then
// Client stream arrival->intake->native application, with the wait reasons.
struct ExecutionTraceSample {
    std::uint32_t role{}, revision{}, executionRevision{}, typeRva{}, state{}, result{}, waitReason{}, waits{}, waitMask{};
    ULONGLONG first{}, second{}, third{}, fourth{};
    std::uint64_t utcMs{}; // Host publish / Client stream arrival, wall clock
    std::uint32_t decision{}, entityA{}, entityB{}, birth{}, constructorRva{}, argumentKinds{}, argument0{}, argument1{};
    std::uint32_t localType{}, localState{}, localSerial{}, localFlags{};
    std::uint64_t commandId{}, globalSeq{}, ownerEpoch{};
};
std::uint64_t UtcMs() noexcept {
    FILETIME time{};GetSystemTimePreciseAsFileTime(&time);
    const auto value=(static_cast<std::uint64_t>(time.dwHighDateTime)<<32U)|time.dwLowDateTime;
    return value/10000ULL-11644473600000ULL;
}
std::mutex gExecutionTraceGate;
// Bounded value traces for every admitted NPC family. Compare command identity
// across PCs; UTC clocks are useful only after accounting for their offset.
struct MotionTraceSample {
    std::uint32_t type{}, role{}; std::uint64_t utcMs{};
    transactions::Scope scope{};
    std::uint64_t sampleTick{},sessionCookie{},ownerEpoch{},ownerSeq{};
    std::uint32_t entityA{}, entityB{},birth{},parameterRevision{},flags{},gait{};
    bool poseReadable{};
    float x{}, y{}, z{}, yaw{};
    std::uint32_t cycleType{}, cycleState{}, headKind{};
    std::uint32_t callbackThread{},onEngineThread{},headLocal{};
    std::uint64_t commandId{}, globalSeq{};
    std::uint32_t opcode{}, nativeKind{}, mode{}, outcome{};
    float aux{}, goalX{}, goalY{}, goalZ{},stopDistance{};
    std::uint32_t intentContract{},intentKinds{},intentCaller{},intentReferenceMask{},intentCountOffset{},intentScriptFlags{};
    std::uint32_t intentArgument0{},intentArgument1{};
    std::uint64_t intentDigest{};
    std::uint64_t waitAgeMs{};
    std::uint32_t waitReason{},executionStatus{},executionResult{};
    std::uint32_t navigationReadMask{},routeNodes{},historyNodes{},cursorOrdinal{UINT32_MAX},blockState{},blockMode{};
    std::uint32_t blockFlags90{},blockFlags91{},requestStatus{},requestedGait{},effectiveGait{};
    std::uint32_t authoredFlags90{},authoredFlags91{};
    std::uint32_t headBlockState{},headBlockMode{},headFlags90{},headFlags91{},headTailState{};
    // Positive reads distinguish a quiet worker from missing observations.
    // These are borrowed values, never permission to change a native mode.
    std::uint32_t navigationStateReadMask{},nativeMovementMode{},navigationWorkerState{},navigationUpdate48{},navigationUpdate49{};
    std::array<float,3> headGoal{},headPendingGoal{};
    float headStopDistance{},headStoppingDistance{};
    std::array<float,3> routeFirst{},routeLast{},routeCursor{};
    float routeDistance{},routeProgress{};
    std::uint64_t localMoveRestarts{};
    std::uint32_t hostCycleType{},hostCycleState{},endDecision{},viewerDistanceCm{};
};
std::mutex gMotionTraceGate;
std::array<MotionTraceSample,1024> gMotionTraces{};
std::size_t gMotionTraceRead{},gMotionTraceCount{};
std::uint64_t gMotionTraceDropped{};
void RecordMotionTrace(const MotionTraceSample& sample) noexcept {
    const std::lock_guard lock(gMotionTraceGate);
    if(gMotionTraceCount==gMotionTraces.size()){++gMotionTraceDropped;return;}
    gMotionTraces[(gMotionTraceRead+gMotionTraceCount++)%gMotionTraces.size()]=sample;
}
std::array<ExecutionTraceSample,256> gExecutionTraces{};
std::size_t gExecutionTraceRead{},gExecutionTraceCount{};
std::uint64_t gExecutionTraceDropped{};
void RecordExecutionTrace(const ExecutionTraceSample& sample) noexcept {
    const std::lock_guard lock(gExecutionTraceGate);
    if(gExecutionTraceCount==gExecutionTraces.size()){++gExecutionTraceDropped;return;}
    gExecutionTraces[(gExecutionTraceRead+gExecutionTraceCount++)%gExecutionTraces.size()]=sample;
}
std::array<execution::LifecycleTraceSample,128> gLifecycleTraces{};
std::size_t gLifecycleTraceRead{},gLifecycleTraceCount{};
std::uint64_t gLifecycleTraceDropped{};
void RecordLifecycleTrace(const execution::LifecycleTraceSample& sample) noexcept {
    const std::lock_guard lock(gLifecycleTraceGate);
    if(gLifecycleTraceCount==gLifecycleTraces.size()){++gLifecycleTraceDropped;return;}
    gLifecycleTraces[(gLifecycleTraceRead+gLifecycleTraceCount++)%gLifecycleTraces.size()]=sample;
}
std::array<SpawnPlacementProbe,128> gSpawnProbes{};
std::size_t gSpawnProbeRead{},gSpawnProbeCount{};
std::uint64_t gSpawnProbeDropped{};
std::atomic<std::uint64_t> gPairedSourcePlacements{}, gPairedSourcePlacementRejected{};
std::atomic<std::uint64_t> gPairedSourcePlacementDeferred{}, gPairedSourcePlacementResumed{};
std::atomic<std::uint64_t> gPairedSourcePlacementCancelled{}, gPairedSourcePlacementExpired{};
std::atomic<std::uint64_t> gPairedSourcePoseSuppressed{};
std::atomic<std::uint64_t> gPairedObserverSnapshotsDeferred{};
// Engine-published identities for retail worker callbacks. Every field is
// atomic: a sequence check over plain fields would still be a C++ data race.
struct PairBinding final {
    std::atomic<std::uint32_t> sequence{}, runtimeId{}, entityA{}, entityB{}, type{}, generation{}, epoch{}, authoritativeBirth{},participantIncarnation{};
    std::atomic<std::uintptr_t> character{}, animated{};
    std::atomic<std::uint64_t> peer{},creationSerial{};
};
struct PairIdentity final {
    std::uintptr_t character{}, animated{};
    std::uint32_t runtimeId{}, entityA{}, entityB{}, type{}, generation{}, epoch{}, authoritativeBirth{},participantIncarnation{};
    std::uint64_t peer{},creationSerial{};
};
// Workers never read Actor/gCurrent. A short value-only exchange carries the
// exact pair and native result. Periodic engine capture only tries this mutex;
// no native calls, logging or I/O run while an exchange holds it.
struct OutcomeBinding final {
    PairIdentity source{}, victim{};
    State state{};
    std::uint8_t serial{};
    bool claimed{}, lateFinishClaimed{}, localVictimEnded{};
    interactions::Key transaction{};
    interactions::Binding transactionActors{};
};
struct PairTransaction final {
    interactions::Key key{};
    interactions::Binding binding{};
    interactions::Receipt receipt{};
    PairIdentity source{},victim{};
    State carrier{};
    State progress{};
    bool used{},drainIssued{},revoked{},startReady{},sourceExitRequested{};
    bool sourceEndActionObserved{},sourceEndActionForwarded{};
    std::uint32_t exitTraceMask{};
    ULONGLONG firstRefusal{}; // first pre-entry refusal of this Host interaction
    ULONGLONG hostLeftAt{}; // first pump seeing the Host source out of this pair
};
std::array<PairTransaction,kActors> gPairTransactions{}; // Qualified engine thread only.
constexpr ULONGLONG kPairRefusalRetryMs=1000U;
constexpr ULONGLONG kLocalVictimEndGraceMs=1500U;
struct PairAdmissionFailure {
    std::uint64_t tick{},command{},serial{};
    std::uint32_t entityA{},entityB{},birth{},pairRevision{},result{},phase{},site{},stage{},reason{};
    bool registered{},transitionEntered{};
    std::uint32_t refusal{},decision{}; // RefusalSite; decision 1 retry, 2 abandoned, 3 scope fenced
    std::uint32_t rawPayloadHigh{},decodedPayloadHigh{};
};
std::array<PairAdmissionFailure,32> gPairAdmissionFailures{};
std::size_t gPairFailureRead{},gPairFailureCount{};
std::mutex gPairFailureMutex;
interactions::Runtime gPairRuntime;
transactions::Scope gPairRuntimeScope{};
std::atomic<bool> gPairRuntimeInitialized{};
std::atomic<std::uint64_t> gWorldResetCount{};
struct PairTransactionEvent final {
    interactions::Key key{};
    interactions::LocalParticipant actor{};
    std::uintptr_t cycle{};
    std::uint8_t serial{};
    interactions::FinishCause cause{};
    NativeOutcome outcome{};
    std::uint32_t evidence{};
    bool retainedEndAction{}; // Marker observed, NOT native completion or disposal.
};
std::array<PairTransactionEvent,256> gPairTransactionEvents{};
std::size_t gPairEventRead{},gPairEventCount{};
std::mutex gPairEventMutex;
std::atomic<bool> gPairEventOverflow{};
bool PairReservedCharacter(std::uintptr_t) noexcept;
bool PairTransactionCandidate(const State&) noexcept;
bool PairTransactionReserved(const transactions::Scope&,const transactions::OwnerKey&,std::uint64_t) noexcept;
bool PairTransactionContextReady(const transactions::Scope&) noexcept;
interactions::Check PairTransactionPermission(void*,const interactions::Command&,const interactions::Binding&) noexcept;
void PumpPairTransactions() noexcept;
std::array<OutcomeBinding, kActors> gOutcomes{};
std::mutex gOutcomeMutex;
std::atomic<std::uint64_t> gOutcomeCaptured{}, gOutcomeApplied{}, gOutcomeRejected{}, gOutcomeDuplicates{};
std::atomic<std::uint64_t> gUnboundHostPreparationAborts{}, gUnboundHostPreparationAbortIncomplete{};
std::atomic<std::uint64_t> gPairRegistrationAccepted{}, gPairRegistrationWorkers{}, gPairTransitionWorkers{};
std::atomic<std::uint32_t> gPairGateLast{}, gPairGateClass{}, gPairGateRevision{}, gPairGateRefusalSite{};
std::atomic<std::uint64_t> gPairGateCount{};
// Client preparation replay rejection reasons (ResolvedClientPreparation).
std::atomic<std::uint64_t> gPrepRejectSubmission{}, gPrepRejectSource{}, gPrepRejectTerminal{},
    gPrepRejectDefinition{}, gPrepRejectGeometry{}, gPrepRejectFault{}, gPrepHealthOnlyApplied{},
    gPairedVictimReleaseRequests{};
std::array<PairBinding, kActors> gPairSources{};
std::array<PairBinding, kParticipants + 1U> gPairParticipants{};
std::array<std::atomic<bool>, kActors> gPairSourceBusy{};
std::atomic<std::uint32_t> gPairBindingEpoch{1U};
// Published on the engine thread; health hooks may read on native workers.
// No raw pointer permission survives a pair-binding generation rollover.
std::atomic<std::uint64_t> gPvPTechnicalPeer{};
std::atomic<std::uint32_t> gPvPTechnicalEpoch{};
thread_local std::uintptr_t gVictimAdmissionSource{}, gVictimAdmissionTarget{}, gVictimAdmissionCycle{};
thread_local std::uint32_t gVictimAdmissionDepth{};
struct NativeVictimSubmission final {
    alignas(16) std::array<std::byte, 0x40U> packet{};
    interactions::Key transaction{};
    interactions::LocalParticipant transactionSource{};
    std::uintptr_t sourceCycle{}, source{}, victim{};
    PairIdentity victimBefore{};
    NativeOutcome outcome{};
    std::uint32_t pairRevision{};
    std::size_t slot{kActors};
    std::uint8_t sourceSerial{};
    bool livingBefore{}, captured{}, playerPreparation{};
    bool authoritativeClient{}, victimDispatch{}, retailDispatched{}, duplicateOutcome{}, nativeFinish{};
    bool preparationApplied{},reactionApplied{},abortHostPreparation{};
};
thread_local NativeVictimSubmission* gNativeVictimSubmission{};
// Exact provenance of the last native HP write. Workers publish values only;
// a paired result must wait for its source's native outcome on each Client.
struct HostHealthOrigin final {
    std::atomic<std::uint64_t> tag{}; // source authority birth : pair revision
    std::atomic<std::uint32_t> victimRuntime{}, sourceSlot{};
};
std::array<HostHealthOrigin, kActors> gHostHealthOrigins{};
// NPC-backed Characters created by AIMP represent remote human players. Their
// addresses are known only after CharacterCreate returns, while their runtime
// spawn keys are known before construction and close the re-entrant admission
// window inside the retail constructor.
std::array<std::atomic<std::uintptr_t>, kActors> gOwned{};
std::array<std::atomic<std::uint64_t>, kActors> gOwnedGeneration{};
std::array<std::atomic<std::uint32_t>, kActors> gOwnedSpawnKeys{};
std::array<std::atomic<std::uint64_t>, kActors> gOwnedSpawnKeyGeneration{};
// Client weapon impacts travel in the reverse direction from ordinary AISync
// state. Fixed-size rings avoid allocation inside Character impact detours.
std::mutex gImpactMailboxMutex;
std::array<ImpactEvent, kRing> gOutgoingImpacts{}, gIncomingImpacts{};
std::size_t gOutgoingImpactRead{}, gOutgoingImpactWrite{};
std::size_t gIncomingImpactRead{}, gIncomingImpactWrite{};
std::uint32_t gImpactSequence{};
struct ImpactSequence final { std::uint64_t peer{}; std::uint32_t sequence{}; };
std::array<ImpactSequence, kParticipants> gLastImpactSequence{};
std::mutex gMailboxMutex;
Configuration gRequested{};
bool gConfigurationPending{};
std::array<State, kActors> gIncoming{};
// First eligible Host pose is immutable until this scope is retired.
std::array<State, kActors> gInitialPlacements{};
std::array<bool, kActors> gInitialPlacementUsed{};
struct PendingConsequence { State state{}; std::uint64_t through{}; };
std::array<std::deque<PendingConsequence>, kActors> gIncomingOutcomes{}; // bounded by kRing globally
std::array<State, kActors> gLastQueuedOutcome{};
std::array<State, kActors> gLastQueuedDamage{};
std::array<State, kActors> gLastQueuedParticipantDamage{};
std::array<State, kActors> gLastQueuedShot{};
std::size_t gIncomingOutcomeCount{};
std::array<bool, kActors> gIncomingUsed{};
transactions::Scope gCausalScope{};
std::array<ULONGLONG, kActors> gIncomingAt{};
std::array<ULONGLONG, kActors> gNextIdentityProbe{};
Configuration gCurrent{}; // engine thread only
std::atomic<Role> gRole{Role::Retail};
// Engine-thread-only PvP scope; pointers are never persisted beyond a qualified
// Character incarnation. This does not grant locomotion/camera control.
struct AlienPvPNativeLease final {
    character_incarnations::Ticket ticket{};
    std::uintptr_t tree{};
    std::uint64_t sessionCookie{}, ownerPeer{};
    std::uint32_t nativeGeneration{}, worldGeneration{}, entityA{}, entityB{}, runtimeId{};
    std::uint32_t boundaryOffset{}, boundaryValue{};
    std::uint32_t retainedKind{},retainedStatus{},retainedRetired{};
    bool fenced{}, drained{}, owned{}, navigationDrainEntered{}, releasing{};
    bool retainedDrainFault{}, backstageCancelEntered{};
    std::uintptr_t cancelledRequest{};
};
AlienPvPNativeLease gPvPLease{};
bool PvPCharacterFenced(std::uintptr_t character) noexcept;
const alien_pvp::ActionHooks& PvPActionHooks() noexcept;
void RetirePvPActions() noexcept;
bool InstallPvPDonorVentHooks(HookManager&,std::string&);
bool PvPLeaseScopeCurrent() noexcept;
bool PvPLeaseLiveIdentity() noexcept;
void ForgetPvPLease(std::uintptr_t destroyedCharacter = 0U) noexcept;

std::atomic<bool> gAlienSuppressed{};
std::atomic<bool> gInstalled{}, gStopping{};
std::atomic<void*> gUpdateOriginal{}, gCoordinatorOriginal{}, gInteractionOriginal{};
std::atomic<void*> gPairedTransitionAOriginal{}, gPairedTransitionBOriginal{};
std::atomic<void*> gCharacterImpactOriginal{}, gCharacterImpactSecondaryOriginal{};
std::atomic<void*> gPrepassOriginal{};
std::atomic<void*> gCandidatesOriginal{}, gHostilityOriginal{}, gThePlayerOriginal{};
std::atomic<void*> gRecordInvalidOriginal{};
std::atomic<void*> gSenseEventOriginal{}, gSoundEventOriginal{};
std::atomic<void*> gPairedInitializeOriginal{};
void* gPairedInitializeTrampoline{};
std::atomic<void*> gPairedVictimTransitionOriginal{}, gPairedVictimInitializeOriginal{};
void* gPairedVictimTransitionTrampoline{}, *gPairedVictimInitializeTrampoline{};
std::atomic<void*> gPairedVictimEventOriginal{}, gPairedVictimSubmitPublished{};
void* gPairedVictimEventTrampoline{}, *gPairedVictimSubmitTrampoline{}, *gPairedVictimSubmitContinue{};
std::atomic<void*> gPairedVictimPrepareOriginal{};
void* gPairedVictimPrepareTrampoline{};
// Only complete MOV instructions are displaced; each untouched path uses its
// own relocation-free trampoline. Adapted paths resume at retail player logic.
void* gPlayerPreparationTrampoline{}, *gPlayerPreparationContinue{};
void* gOutcomePreparationContinue{}, *gOutcomePreparationAbort{};
void* gPlayerCategoryTrampoline{}, *gPlayerCategoryContinue{};
void* gPlayerRuntimeTrampoline{}, *gPlayerRuntimeContinue{};
std::atomic<void*> gPlayerRuntimePublished{};
void* gPairedStopTrampoline{};
std::atomic<void*> gPairedStopOriginal{};
void* gPairedIdleTrampoline{}, *gPairedAlternateIdleTrampoline{};
std::atomic<void*> gPairedIdleOriginal{}, gPairedAlternateIdleOriginal{};
std::atomic<std::uint64_t> gPairedIdleBlocked{}, gPairedStopBlocked{};
void* gSchedulerIdleTrampoline{}, *gSchedulerTransitionTrampoline{};
std::atomic<void*> gSchedulerIdleOriginal{}, gSchedulerTransitionOriginal{};
thread_local std::uintptr_t gSchedulerTransitionOwner{};
struct NativeSchedulerParent {
    host_producer::CommandIdentity identity{};
    std::uintptr_t character{};std::uint32_t runtime{};bool covered{};
};
thread_local NativeSchedulerParent gNativeSchedulerParent{};
struct NativeSchedulerFrame { std::uintptr_t scheduler{};NativeSchedulerParent parent{}; };
thread_local std::array<NativeSchedulerFrame,32> gNativeSchedulerFrames{};
thread_local std::size_t gNativeSchedulerDepth{},gNativeSchedulerOverflow{};
thread_local bool gNativeFallbackCovered{};
thread_local std::uintptr_t gAuthoritativeStanceFallbackCycle{};
struct PlacementChildFrame {
    host_producer::NativePlacementIdentity parent{};
    bool frontstage{},qualified{};
    PlacementChildFrame* previous{};
};
thread_local PlacementChildFrame* gPlacementChild{};
void* gPlacementCycleFirstTrampoline{},*gPlacementCycleSecondTrampoline{},*gPlacementCycleContinue{};
std::atomic<void*> gPlacementCycleFirstOriginal{},gPlacementCycleSecondOriginal{};
void* gPassageBackstageFirstTrampoline{},*gPassageBackstageSecondTrampoline{};
void* gPassageBackstageFirstContinue{},*gPassageBackstageSecondContinue{};
std::atomic<void*> gPassageBackstageFirstOriginal{},gPassageBackstageSecondOriginal{};
std::atomic<std::uint64_t> gPassageTransitionsHeld{},gPassageBackstageInterruptionsHeld{};
std::atomic<std::uint64_t> gPlacementChildrenDerived{},gPlacementFallbacksSuppressed{};
NativeSchedulerParent CaptureNativeSchedulerParent(std::uintptr_t) noexcept;
NativeSchedulerParent CurrentNativeSchedulerParent(std::uintptr_t) noexcept;
thread_local std::uint32_t gSchedulerTransitionRva{};
std::atomic<std::uint32_t> gLastPairedBlockedSchedulerRva{};
std::atomic<std::uint64_t> gNavigationConvergenceApplied{}, gNavigationConvergenceFailed{};
std::atomic<std::uint64_t> gNavigationTimingAccelerated{}, gNavigationTimingBraked{};
std::atomic<std::uint64_t> gPursuitApplied{}, gPursuitFailed{}, gPursuitEnded{};
std::atomic<std::uint64_t> gNavigationPaused{};
std::atomic<std::uint64_t> gPairedRuntimePlayerRemaps{};
std::atomic<void*> gPlayerPreparationPublished{}, gPlayerCategoryPublished{};
void* gVictimUpdateTrampoline{};
std::atomic<void*> gVictimUpdateOriginal{};
void* gHealthCommitTrampoline{};
std::atomic<void*> gHealthCommitOriginal{};
void* gVictimFinishTrampoline{}, *gVictimFinishSkip{}, *gVictimFinishKill{};
std::atomic<void*> gVictimFinishPublished{};
void* gSenseEventTrampoline{}, *gSoundEventTrampoline{};
void* gRecordInvalidTrampoline{};
void* gCandidatesTrampoline{}, *gHostilityTrampoline{}, *gThePlayerTrampoline{};
void* gPrepassTrampoline{};
void* gUpdateTrampoline{}, *gCoordinatorTrampoline{}, *gInteractionTrampoline{};
void* gPairedTransitionATrampoline{}, *gPairedTransitionBTrampoline{};
void* gCharacterImpactTrampoline{}, *gCharacterImpactSecondaryTrampoline{};
void* gBranchTrampoline{}, *gBranchExit{};
std::atomic<void*> gBranchPublished{};
std::atomic<std::uint32_t> gShutdownEngineThread{};
std::uintptr_t gBase{};
std::atomic<std::uint64_t> gMarkerKeysCaptured{},gMarkerKeysResolved{};
std::atomic<std::uint32_t> gMarkerCaptureResult{},gMarkerResolveResult{};
struct MarkerCommandBinding {
    transactions::Scope scope{};
    transactions::OwnerKey owner{};
    std::uint64_t command{};
    marker_identity::Mapping mapping{};
    bool used{};
};
std::array<MarkerCommandBinding,execution::NativeRequestAdapter::kCapacity> gMarkerCommandBindings{};

LevelGenerationTracker* gGenerations{};
GameThreadDispatcher* gDispatcher{};
WorldEntityRegistry* gScriptReferences{};
std::uint64_t gGeneration{};
// Movement writers (steering, path convergence, pursuit, relocation, idle
// holds): only for an actor whose locomotion is NOT owned by the transactional
// executor. Only the exact locally drained actor is transferred.
bool UnownedCleanupAllowed(const Actor& actor) noexcept {
    return !PairReservedCharacter(actor.character)&&(gGeneration > UINT32_MAX || execution_runtime::AllowsLegacy(
        {actor.character, actor.runtimeId, static_cast<std::uint32_t>(gGeneration)}));
}
// Host intentions that are not scheduler requests: action/execution cycle
// transitions, paired attacks/kills, reactions, death, lifecycle. The new
// executor replays the Host's requests (incl. the kind5 action hold); these
// intentions still have to start the matching native cycles on the same actor.

void RevokeTransactionActor(const Actor& actor) noexcept {
    if (gGeneration <= UINT32_MAX) execution_runtime::InvalidateActor(
        {actor.character, actor.runtimeId, static_cast<std::uint32_t>(gGeneration)});
}
// Written during Bootstrap installation, read only by its diagnostics worker.
std::string gTypedExecutionInstallationError;
std::atomic<std::uint32_t> gHandoffStage{},gHandoffResult{},gHandoffOffset{},gHandoffValue{};
std::atomic<std::uint64_t> gHandoffAttempts{},gHandoffNavigationClears{},gHandoffNavigationResets{};
std::uint32_t gNextBirth{1U};
std::uint64_t gNextPairCommand{}; // Host-only engine thread; never reset or wrap.
std::uint32_t gLivingPlayerAlliance{};
bool gLivingPlayerAllianceKnown{};
thread_local Actor* gUpdating{};
// Only AI execution scopes set this. It prevents the multiplayer compatibility
// resolver from changing THE_PLAYER for camera/UI/inventory/mission code.
thread_local std::uintptr_t gAiPlayerQueryOwner{};
std::atomic<void*> gBackstageUpdateOriginal{}, gBackstageHoldOriginal{}, gTargetIsPlayerOriginal{};
void* gBackstageUpdateTrampoline{}, *gBackstageHoldTrampoline{}, *gTargetIsPlayerTrampoline{};
void* gMenacePlayerTrampoline{}, *gMenacePlayerContinue{};
std::atomic<void*> gMenacePlayerPublished{};
void* gSoundSourceClassTrampoline{}, *gSoundSourceClassContinue{};
void* gSoundBackstageClassTrampoline{}, *gSoundBackstageClassContinue{};
void* gSoundThreatClassTrampoline{}, *gSoundThreatClassContinue{};
void* gSoundLostClassTrampoline{}, *gSoundLostClassContinue{};
std::atomic<void*> gSoundSourceClassPublished{}, gSoundBackstageClassPublished{};
std::atomic<void*> gSoundThreatClassPublished{}, gSoundLostClassPublished{};
void* gBackstageEligibilityTrampoline{}, *gBackstageEligibilityContinue{};
std::atomic<void*> gBackstageEligibilityPublished{};
void* gBackstageNavigationTrampoline{}, *gBackstageNavigationContinue{};
std::atomic<void*> gBackstageNavigationPublished{};
std::atomic<std::uint64_t> gBackstageEligibilityOverrides{}, gBackstageNavigationPlans{};
std::atomic<std::uint64_t> gSurvivorTargetsAssigned{}, gSurvivorTargetFaults{}, gLastSurvivorTargetPeer{};
std::atomic<std::uint64_t> gBackstageNavigationUpdates{}, gBackstageNavigationRejected{};
std::atomic<std::uint64_t> gBackstageDeadTargetsRejected{}, gBackstageHoldContexts{};
std::atomic<std::uint64_t> gTargetPlayerOverrides{}, gMenacePlayerOverrides{}, gSoundPlayerOverrides{};
// Same data layout as the optional player route cache made by 006EE5B0.
// Only AISync owns these buffers and their +0/+4 shared query references.
// No player controller is fabricated or installed on a PlayerSync Character.
struct ParticipantNavigation final {
    alignas(16) std::array<std::byte, 0x458U> native{};
    std::uint64_t peer{}, sessionCookie{};
    std::uintptr_t character{};
    std::uint32_t runtimeId{}, nativeGeneration{}, worldGeneration{};
    ULONGLONG lastUpdate{};
    ParticipantRouteRetry emptyRouteRetry{};
    std::uint32_t emptyRouteRetries{};
    float goal[3]{};
    bool goalApplied{};
    bool initialized{}, faulted{};
};
std::array<ParticipantNavigation, kParticipants> gParticipantNavigation{};
float gObjectiveNavigationGoal[3]{};
bool gObjectiveNavigationGoalKnown{};
std::atomic<std::uint64_t> gBackstageGoalsObjective{}, gBackstageGoalsOwner{}, gBackstageGoalsDegenerate{};
bool gNavigationHostActive{};
std::uint64_t gNavigationSession{};
std::uint64_t gNavigationGeneration{};
std::uint32_t gNavigationWorld{};
ULONGLONG gNavigationMaintenance{};
// Only the certified sense-event dispatchers set this scope. CATHODE's
// global entity resolver and unrelated callers keep their retail semantics.
thread_local std::uintptr_t gSenseEventOwner{};
// Only the common native paired-cycle initializer opens this exact victim
// scope. The two redirected resolver CALLs retain their distinct return types.
thread_local std::uintptr_t gPairedResolveOwner{};
thread_local const std::uint32_t* gPairedResolveIdentity{};
// Exact callers supply the owner; a record also supplies its exact candidate.
// Do not adapt unrelated lifecycle/animation/BT uses of FUN_0078B380.
thread_local std::uintptr_t gCandidateEligibilityOwner{};
thread_local std::uintptr_t gRecordEligibilityOwner{}, gRecordEligibilityTarget{};
// Some retail AI paths bypass FUN_0078BAF0 and read the first player slot
// directly. During a Host AI callback only, keep AISync's own identity logic
// anchored to the real local player while the retail slot can be projected to
// a living multiplayer participant. Nested AI callbacks restore stack-wise.
thread_local std::uintptr_t gAiPlayerRealLocal{};
thread_local std::uint32_t gAiPlayerContextDepth{};
std::atomic<std::uint64_t> gObserved{}, gApplied{}, gDropped{}, gRefused{}, gSuspended{}, gFaults{};
std::atomic<std::uint64_t> gInvalidOutgoing{};
std::atomic<std::uint32_t> gLastInvalidOutgoingFlags{}, gLastInvalidOutgoingEntityA{}, gLastInvalidOutgoingEntityB{};
std::atomic<std::uint64_t> gMissing{};
std::atomic<std::uint64_t> gManagerUpdates{}, gAdmitted{}, gIdentityRejected{}, gTreeUnavailable{}, gOwnerMismatch{};
std::atomic<std::uint64_t> gRemoteQueued{};
std::atomic<std::uint64_t> gPoseCaptured{}, gPoseApplied{}, gPoseRejected{};
std::atomic<std::uint64_t> gHybridEntries{},gHybridResumes{},gHybridApplications{},gHybridPaused{},gHybridFailures{};
std::atomic<std::uint64_t> gHybridNativeArms{},gHybridNativeRefusals{},gHybridHardSnaps{};
std::atomic<std::uint64_t> gIntentCaptured{}, gIntentApplied{}, gIntentRejected{}, gIntentRecoveries{};
std::atomic<std::uint64_t> gActionCaptured{}, gActionTransitions{};
std::atomic<std::uint64_t> gActionReferencesCaptured{},gActionReferencesUnbound{};
std::atomic<std::uint64_t> gReservedByOutcome{},gReservedByPair{};
std::atomic<std::uint64_t> gCycleCallsCaptured{},gCycleCallsRefused{},gCycleCallsPublished{},gCycleCallsReplayed{},gCycleCallsReplayFailed{};
std::atomic<std::uint64_t> gDirectLocomotionConstructorsConsumed{};
std::atomic<std::uint64_t> gDirectPoseStuckCorrections{},gDirectPoseEmergencyCorrections{},gDirectPoseAssistFrames{},gDirectPoseActionFollowFrames{};
std::atomic<std::uint32_t> gLastRefusedCycleCall{};
std::atomic<std::uint64_t> gIntakeHeldByPair{},gIntakeHeldByHealth{},gIntakeHeldByParticipant{},gIntakeHeldByCombat{};
std::atomic<std::uint64_t> gExecutionCaptured{}, gExecutionTransitions{}, gExecutionReplayed{}, gExecutionReplayRejected{};
std::atomic<std::uint64_t> gRoleTaskCaptured{}, gRoleTaskTransitions{};
std::atomic<std::uint64_t> gLifecycleCaptured{}, gLifecycleApplied{}, gLifecycleSnaps{}, gExecutionBarriers{};
std::atomic<std::uint64_t> gInitialNativePosesPreserved{};
std::atomic<std::uint64_t> gImpactCaptured{}, gImpactQueued{}, gImpactApplied{}, gImpactRejected{};
std::atomic<std::uint64_t> gImpactShooterRemaps{}, gImpactShooterUnresolved{};
std::atomic<std::uint64_t> gImpactStimulusForwarded{}, gImpactStimulusApplied{}, gImpactDamagelessDropped{};
std::atomic<std::uint64_t> gWithdrawSamples{}, gWithdrawTransitions{}, gWithdrawApplied{};
std::atomic<std::uint64_t> gBackstageCaptured{}, gBackstageTransitions{}, gBackstageApplied{}, gBackstageRejected{};
std::atomic<std::uint64_t> gPlacementVerified{}, gPlacementRejected{}, gLifecycleRetries{};
std::atomic<std::uint64_t> gLifecycleTransitions{};
std::atomic<std::uint64_t> gPoseInterpolated{}, gPairedHostCycleWaits{}, gOutcomeSnapshotsCoalesced{};
std::atomic<std::uint64_t> gInteractionCaptured{}, gInteractionApplied{}, gInteractionRejected{};
std::atomic<std::uint64_t> gInteractionRemapped{};
std::atomic<std::uint64_t> gPairedTransitionCaptured{}, gPairedTransitionApplied{};
std::atomic<std::uint64_t> gPlayerRepresentationsRegistered{};
std::atomic<std::uint64_t> gProvisionalPlayerActorsRetired{};
std::atomic<std::uint64_t> gParticipantCandidates{}, gParticipantTargetRejected{}, gDeadTargetRecoveries{};
std::atomic<std::uint64_t> gPostDeathMeleeSelectionChecks{}, gPostDeathMeleeSelectionRejects{};
std::atomic<std::uint64_t> gPostDeathMeleeSelectionWriteFailures{}, gLastPostDeathRetiredPeer{};
std::atomic<std::uint64_t> gParticipantCandidatePasses{};
std::atomic<std::uint64_t> gSenseEventPasses{};
std::atomic<std::uint64_t> gParticipantSignalLookups{};
std::atomic<std::uint64_t> gParticipantSignalRemaps{};
std::atomic<std::uint64_t> gParticipantSignalRetailMissing{};
std::atomic<std::uint64_t> gParticipantSignalRetailMismatch{};
std::atomic<std::uint64_t> gParticipantSignalAmbiguous{};
std::atomic<std::uint64_t> gLastParticipantSignalPeer{};
std::atomic<std::uint64_t> gParticipantPairLookups{}, gParticipantPairRemaps{};
std::atomic<std::uint64_t> gParticipantPairRetailMissing{}, gParticipantPairAmbiguous{};
std::atomic<std::uint64_t> gLastParticipantPairPeer{};
std::atomic<std::uint64_t> gPairedInitializeSucceeded{}, gPairedInitializeFailed{};
std::atomic<std::uint64_t> gPairedCycleReleased{};
std::atomic<std::uint64_t> gPairedObserverCompletions{}, gPairedObserverReleaseAttempts{};
std::atomic<std::uint64_t> gPairedObserverReleases{}, gPairedObserverReleaseRejected{};
std::atomic<std::uint64_t> gPairedObserverPairReopenSuppressed{}, gPairedObserverNeutralHolds{};
std::atomic<std::uint64_t> gPairedObserverFinishRetries{}, gPairedObserverFinishCompleted{};
std::atomic<std::uint64_t> gOwnedExecutionPumps{};
// Engine-thread published per source slot: until when the Host still retains
// its own 4E for the pair this observer owns, and the outro time bound.
std::array<std::atomic<ULONGLONG>, kActors> gHostSourceRetainedUntil{};
std::array<std::atomic<ULONGLONG>, kActors> gObserverOutroDeadline{};
std::atomic<std::uint64_t> gPairedObserverOutroRetained{}, gPairedObserverOutroExpired{};
std::atomic<std::uint64_t> gPairedOutroSchedulerBlocked{};
std::atomic<std::uint64_t> gNavigationFallbacks{};
std::atomic<std::uint64_t> gPairedObserverHandoffResumed{};
std::atomic<std::uint64_t> gPairedVictimTransitions{}, gPairedVictimTransitionRejected{};
std::atomic<std::uint64_t> gPairedVictimInitializeAborted{};
std::atomic<std::uint64_t> gPairedVictimEventsRejected{}, gPairedVictimDuplicates{}, gPairedVictimBusy{};
std::atomic<std::uint64_t> gPairedVictimNativeDispatches{}, gPairedVictimNativeStarted{};
std::atomic<std::uint64_t> gPairedVictimAvailabilityOverrides{};
std::atomic<std::uint64_t> gPairedPlayerPreparationRemaps{}, gPairedPlayerCategoryRemaps{};
std::atomic<std::uint64_t> gPairedVictimLocalDispatches{}, gPairedVictimLocalStarted{};
std::atomic<std::uint64_t> gPairedVictimConvergenceDetached{}, gPairedVictimSnapshotsDeferred{};
std::atomic<std::uint32_t> gLastRejectedPairedReason{};
std::atomic<std::uint64_t> gPairedVictimHealthContinuations{}, gPairedVictimWorkerDispatches{};
std::atomic<std::uint64_t> gPairedVictimLocalAvailabilityOverrides{}, gPairedDeadTargetRejected{};
std::atomic<std::uint64_t> gPairedRecoveryDeferred{};
std::atomic<std::uint32_t> gLastVictimAdmissionHp{}, gLastVictimAdmissionHealth{}, gLastVictimAdmissionEnabled{};
std::atomic<std::uint32_t> gLastRejectedPairedSourceState{0xFFFFFFFFU};
std::atomic<std::uint64_t> gCandidateEligibilityChecks{}, gCandidateEligibilityOverrides{};
std::atomic<std::uint64_t> gRecordEligibilityChecks{}, gRecordEligibilityOverrides{};
std::atomic<std::uint64_t> gParticipantRecordsValidated{}, gParticipantRecordsRetained{}, gParticipantRecordsRejected{};
std::atomic<std::uint64_t> gParticipantPerceptionSamples{}, gParticipantPerceptionDropped{};
std::atomic<std::uint64_t> gTargetSuccessionSamples{}, gTargetSuccessionDropped{};
std::atomic<std::uint64_t> gPostDeathSuccessionsPreserved{}, gPostDeathArbitrationDeferrals{}, gPostDeathDecisionUncertain{};
std::atomic<std::uint64_t> gAiPlayerQueryRemaps{}, gAiPlayerSlotRemaps{}, gAiPlayerSlotRestoreFailures{};
std::atomic<std::uint64_t> gPostDeathDecisionResets{}, gPostDeathDecisionResetRejected{}, gPostDeathRoleTaskResets{};
std::atomic<std::uint64_t> gInteractionBindingsResolved{}, gInteractionBindingRejected{};
std::atomic<std::uint32_t> gLastHostAttachment{}, gLastLocalAttachment{}, gLastTransitionVariant{};
std::atomic<std::uint32_t> gActiveNativeGeneration{}, gActiveWorldGeneration{};
std::atomic<std::uint64_t> gActiveSessionCookie{};
std::atomic<bool> gLifecycleEventsPending{};
bool gLifecycleDrainActive{}; // Certified engine thread only.
// Worker-safe value-only callback trace. Never read Actor or gCurrent here.
struct PairedGraphProbe final {
    std::uint64_t tick{}, peer{};
    std::uint32_t role{}, generation{}, event{}, side{}, phase{}, pairRevision{}, birth{};
    std::uint32_t sourceA{}, sourceB{}, victimA{}, victimB{}, sourceStage{0xFFFFFFFFU};
    std::uint8_t sourceState{}, victimState{}, sourceFinished{}, victimEnded{}, victimAcquired{};
    std::uintptr_t cycle{}, graph{};
    std::uint32_t callerRva{}, markerIndex{}, thread{};
    std::uint8_t serial{},sourceFlags{},sourceE5{},victimFlags{},victimInteractive{},victimControlActive{};
    bool endPredicateReadable{};
    float sourceAlignment{}, victimAlignment{};
    bool readable{};
};
std::array<PairedGraphProbe, kParticipantProbeRing> gPairedGraphProbes{};
std::mutex gPairedGraphMutex;
std::uint32_t gPairedGraphWrite{}, gPairedGraphRead{};
std::atomic<std::uint64_t> gPairedGraphDropped{};
void* gSourceGraphEventTrampoline{}, *gVictimGraphEventTrampoline{}, *gSourceGraphFinishTrampoline{};
void* gVictimGraphFinishTrampoline{};
std::atomic<void*> gSourceGraphEventOriginal{}, gVictimGraphEventOriginal{}, gSourceGraphFinishOriginal{};
std::atomic<void*> gVictimGraphFinishOriginal{};
std::atomic<std::uint64_t> gPairedLocalVictimFinishes{};
std::atomic<std::uint64_t> gPairedPreparedReplays{}, gPairedAdmissionRetries{}, gPairedAdmissionAbandoned{};
void* gNavigationSubmitTrampoline{};
std::atomic<void*> gNavigationSubmitOriginal{};
std::atomic<std::uint32_t> gLastEntityA{}, gLastEntityB{}, gLastCharacterClass{};
std::atomic<std::uint32_t> gLastActionTypeRva{}, gLastActionPathHash{}, gLastActionStatus{};
std::atomic<std::uint32_t> gLastExecutionTypeRva{}, gLastExecutionState{}, gLastExecutionFlags{};
std::atomic<std::uint32_t> gLastRejectedExecutionTypeRva{}, gLastRejectedExecutionState{};
std::atomic<std::uint32_t> gLastRoleTaskTypeRva{}, gLastRoleTaskKeyA{}, gLastRoleTaskKeyB{};
std::atomic<std::uint32_t> gLastLifecycleFlags{};
std::atomic<std::uint32_t> gLastWithdrawState{};
std::atomic<std::uint32_t> gLastInteractionTargetA{}, gLastInteractionTargetB{};
std::atomic<std::uint32_t> gLastInteractionTargetMode{}, gLastAppliedInteractionTargetA{};
std::atomic<std::uint32_t> gLastAppliedInteractionTargetB{};
std::atomic<std::uint64_t> gLastInteractionTargetPeer{}, gLastAppliedInteractionTargetPeer{};
std::atomic<std::uint64_t> gLastAiPlayerQueryPeer{}, gLastAiPlayerSlotPeer{};
std::atomic<std::uint32_t> gLastInteractionTransition{};

// Native callbacks supply owner lifetimes. Worker adaptations additionally
// require published generation/runtime/GUID identities; no Actor/mailbox read.
// SEH avoids VirtualQuery on the per-frame path; no heap scan.
template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    if (address < 0x10000U || address > 0xFFFF0000U) return false;
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Write(const std::uintptr_t address, const T& value) noexcept {
    if (address < 0x10000U || address > 0xFFFF0000U) return false;
    __try { *reinterpret_cast<T*>(address) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool EngineThread() noexcept {
    if (gStopping.load(std::memory_order_acquire))
        return gShutdownEngineThread.load(std::memory_order_acquire) == GetCurrentThreadId();
    return gDispatcher != nullptr && gDispatcher->IsCertifiedEngineThread(GetCurrentThreadId());
}
bool Client() noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    return role == Role::Client || role == Role::FrozenClient;
}
bool Stable() noexcept {
    return gGenerations != nullptr && gGenerations->IsStableGeneration(gCurrent.nativeGeneration);
}
bool FiniteCoordinate(const float value) noexcept {
    return std::isfinite(value) && std::abs(value) <= 100000.0F;
}
bool CertifiedExecutionCycle(std::uint32_t typeRva, std::uint32_t state) noexcept;
bool SuppressedAlien(std::uintptr_t character) noexcept;
bool ValidCycleCall(const ExecutionCycleCall& call) noexcept {
    if (call.rva == 0U) {
        const ExecutionCycleCall empty{};
        return std::memcmp(&call, &empty, sizeof(call)) == 0;
    }
    const auto* contract = FindExecutionTransitionContract(call.rva);
    if (!contract || (call.kinds >> (contract->arity * 4U)) != 0U) return false;
    for (std::size_t i = 0; i < contract->arity; ++i) {
        const auto kind = (call.kinds >> (i * 4U)) & 0xFU;
        if (kind == 0U || (kind > 5U && kind != 15U)) return false;
        if (contract->args[i] == TransitionArg::FloatBits && kind != 15U &&
            (kind != static_cast<std::uint32_t>(ExecutionArgKind::Value) || !FiniteExecutionFloatBits(call.value[i]))) return false;
    }
    for (const auto value : call.vector) if (!std::isfinite(value)) return false;
    return call.cycleSerial <= 0xFFU;
}
bool Valid(const State& s) noexcept {
    const bool seal=std::any_of(std::begin(s.interactionSession),std::end(s.interactionSession),[](auto b){return b!=0U;});
    if(seal||s.interactionAuthority||s.interactionCommand) {
        std::uint64_t cookie{};for(std::size_t i=0;i<8U;++i)cookie|=static_cast<std::uint64_t>(s.interactionSession[i])<<(i*8U);
        if(!seal||!s.interactionAuthority||!s.interactionCommand||(s.stateFlags&InteractionValid)==0U||
            (cookie?cookie:1U)!=s.sessionCookie)return false;
    }
    const auto& action=s.executionAction;
    const transactions::OwnerKey actionOwner{s.entityA,s.entityB,s.birth,s.characterClass,transactions::Capability::Movement,action.ownerEpoch};
    if(!action_correlation::Valid(action,actionOwner))return false;
    if(!action_correlation::Empty(action)) {
        std::uint64_t cookie{};for(std::size_t i=0;i<8;++i)cookie|=static_cast<std::uint64_t>(action.source.session[i])<<(8*i);
        if((cookie?cookie:1)!=s.sessionCookie||action.source.worldEpoch!=s.worldGeneration||
            (s.stateFlags&ExecutionValid)==0||(s.executionFlags&(ExecutionTransition|ExecutionPaired))==0||
            action.executionRevision!=s.executionRevision||action.cycleState!=s.executionCycleState||
            (seal&&!std::equal(action.source.session.begin(),action.source.session.end(),std::begin(s.interactionSession))))return false;
    }
    if (!navigation::ValidGoal(s.navigation) || !health::Valid(s.health) || !health::Valid(s.damage)) return false;
    const auto& cause = s.healthSource;
    const bool npcCause = cause.entityA != 0U || cause.entityB != 0U;
    if (cause.peer != 0U && (npcCause || cause.characterClass != 0U || cause.birth != 0U)) return false;
    if (npcCause != (cause.birth != 0U) ||
        (npcCause ? !AICharacterClass(cause.characterClass) : cause.characterClass != 0U)) return false;
    if ((cause.peer != 0U || npcCause) && s.health.flags == 0U) return false;
    if (s.damage.flags != 0U && (s.health.flags == 0U ||
        s.damage.result.revision > s.health.revision)) return false;
    if ((s.stateFlags & ConsequenceEvent) != 0U && s.damage.flags == 0U) return false;
    if (!health::Valid(s.participantConsequence.damage) ||
        ((s.participantConsequence.targetPeer != 0U) != (s.participantConsequence.damage.flags != 0U)) ||
        ((s.stateFlags & ParticipantConsequenceEvent) != 0U && s.participantConsequence.targetPeer == 0U)) return false;
    if (!combat::Valid(s.combat) || !combat::Valid(s.shot) ||
        ((s.stateFlags & ShotEvent) != 0U && s.shot.flags == 0U) ||
        ((s.stateFlags & NavigationEvent) != 0U &&
            (s.navigation.flags == 0U || (s.navigation.kind != 4U && s.navigation.kind != 7U)))) return false;
    if (s.healthPairRevision != 0U && (s.health.flags == 0U || cause.peer != 0U || !npcCause)) return false;
    if ((s.stateFlags & InteractionValid) != 0U &&
        ((s.interactionFlags & InteractionTargetIsParticipant) != 0U
            ? s.interactionTargetBirth != 0U || s.interactionTargetClass != 0U
            : s.interactionTargetBirth == 0U || !AICharacterClass(s.interactionTargetClass))) return false;
    const animation::ScriptedState emptyScripted{};
    const bool scripted = s.scripted.flags != 0U;
    if (scripted ? (!animation::ValidScripted(s.scripted) || (s.stateFlags & ExecutionValid) == 0U ||
            s.executionCycleTypeRva != 0x00EADB58U || s.executionCycleState != 0x12U)
        : std::memcmp(&s.scripted, &emptyScripted, sizeof(emptyScripted)) != 0) return false;
    if (s.interactionParticipantIncarnation!=0U&&((s.stateFlags&InteractionValid)==0U||
        (s.interactionFlags&InteractionTargetIsParticipant)==0U||s.interactionTargetPeer==0U))return false;
    if (!ValidCycleCall(s.transition)) return false;
    if((s.motionFlags&~7U)!=0U)return false;
    for(const auto velocity:s.motionVelocity)if(!std::isfinite(velocity)||std::abs(velocity)>128.0F)return false;
    if((s.motionFlags&1U)!=0U) {
        if(!s.motionTick||(s.stateFlags&PoseValid)==0U)return false;
    } else if(s.motionTick||s.motionFlags||s.motionVelocity[0]!=0.0F||s.motionVelocity[1]!=0.0F||s.motionVelocity[2]!=0.0F)return false;
    if (s.version != 16U || s.nativeGeneration == 0U || s.sessionCookie == 0U ||
        s.worldGeneration == 0U || (s.entityA == 0U && s.entityB == 0U) ||
        s.birth == 0U || s.revision == 0U || !AICharacterClass(s.characterClass) ||
        (s.stateFlags & ~kStateFlags) != 0U || s.moveType > 3U ||
        s.actionStatus > 10U || s.actionDepth > 64U ||
        (s.interactionFlags & ~kInteractionFlags) != 0U ||
        s.interactionTransition > 2U || (s.interactionTransitionFlags & ~0xFFFFU) != 0U ||
        s.interactionVariant < -1 || s.interactionVariant > 0xFFFF ||
        s.interactionTransitionVariant < -1 || s.interactionTransitionVariant > 0xFFFF ||
        (s.lifecycleFlags & ~kLifecycleFlags) != 0U ||
        (s.executionFlags & ~kExecutionFlags) != 0U || s.executionCycleState > 0xFFU) return false;
    for (const auto coordinate : s.position)
        if (!FiniteCoordinate(coordinate)) return false;
    for (const auto coordinate : s.interactionTargetPosition)
        if (!FiniteCoordinate(coordinate)) return false;
    if (!std::isfinite(s.yaw) || std::abs(s.yaw) > 3.3F ||
        !std::isfinite(s.velocityX) || !std::isfinite(s.velocityZ) ||
        std::abs(s.velocityX) > kMaximumVelocity || std::abs(s.velocityZ) > kMaximumVelocity ||
        !std::isfinite(s.locomotionIntensity) || s.locomotionIntensity < 0.0F ||
        s.locomotionIntensity > 64.0F)
        return false;
    if ((s.stateFlags & Moving) != 0U &&
        (s.locomotionRevision == 0U || (s.stateFlags & PoseValid) == 0U)) return false;
    if ((s.interactionFlags & InteractionSourceCycleRetained) != 0U &&
        ((s.stateFlags & InteractionValid) == 0U || s.interactionTransition == 0U ||
         (s.interactionFlags & InteractionPairValid) == 0U)) return false;
    if ((s.stateFlags & ActionValid) != 0U &&
        (s.actionTypeRva == 0U || s.actionDepth == 0U || s.actionRevision == 0U)) return false;
    if ((s.stateFlags & InteractionValid) != 0U &&
        (s.interactionRevision == 0U ||
         s.interactionPairRevision == 0U || (s.interactionFlags & InteractionPairValid) == 0U ||
         ((s.interactionFlags & InteractionTargetIsParticipant) != 0U) != (s.interactionTargetPeer != 0U) ||
         (s.interactionTargetA == 0U && s.interactionTargetB == 0U))) return false;
    if ((s.stateFlags & InteractionValid) == 0U && s.interactionTransition != 0U) return false;
    if (s.interactionTransition != 0U && s.interactionTransitionRevision == 0U) return false;
    if ((s.stateFlags & RoleTaskValid) != 0U &&
        (s.roleTaskRevision == 0U || s.roleTaskTypeRva == 0U)) return false;
    if (s.executionFlags != 0U && (s.stateFlags & ExecutionValid) == 0U) return false;
    if ((s.stateFlags & ExecutionValid) != 0U &&
        (s.executionRevision == 0U || s.executionCycleTypeRva == 0U)) return false;
    if ((s.executionFlags & ExecutionCertified) != 0U &&
        ((s.executionFlags & ExecutionTransition) == 0U ||
          (!scripted && !CertifiedExecutionCycle(s.executionCycleTypeRva, s.executionCycleState)))) return false;
    if (s.lifecycleFlags != 0U && s.lifecycleRevision == 0U) return false;
    const auto& o = s.outcome;
    if ((o.flags & ~kOutcomeFlags) != 0U || o.definition > 0xFFFFU ||
        (o.kind != 0U && o.kind != 1U && o.kind != 3U) || o.hpBefore > 0xFFFFU ||
        o.hpPrepared > 0xFFFFU || o.hpReaction > 0xFFFFU || o.healthReaction > 4U ||
        !std::isfinite(o.sourceYaw) || std::abs(o.sourceYaw) > 100000.0F) return false;
    for (const auto coordinate : o.targetOffset) if (!FiniteCoordinate(coordinate)) return false;
    if (o.revision != 0U && ((s.stateFlags & InteractionValid) == 0U ||
        (s.interactionTargetPeer == 0U && (s.interactionTargetBirth == 0U ||
            !AICharacterClass(s.interactionTargetClass))) || s.interactionTransition == 0U ||
        (s.interactionFlags & InteractionNativeOutcomeRequired) == 0U ||
        (o.flags & OutcomePrepared) == 0U || o.hpBefore == 0U || o.hpPrepared > o.hpBefore)) return false;
    if ((o.revision == 0U) != (o.flags == 0U) ||
        ((o.flags & OutcomeNativeDeath) != 0U && (o.flags & OutcomeVictimAdmitted) == 0U) ||
        ((o.flags & OutcomeTerminal) != 0U) != (o.revision != 0U && o.hpReaction == 0U && o.healthReaction == 4U)) return false;
    return true;
}
bool RegisteredPlayerRepresentation(const std::uintptr_t character) noexcept {
    if (character == 0U || gGenerations == nullptr) return false;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gOwned[i].load(std::memory_order_acquire) == character &&
            gOwnedGeneration[i].load(std::memory_order_relaxed) == generation) return true;
    return false;
}
bool RegisteredPlayerSpawnKey(const std::uint32_t spawnKey) noexcept {
    if (spawnKey == 0U || gGenerations == nullptr) return false;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gOwnedSpawnKeys[i].load(std::memory_order_acquire) == spawnKey &&
            gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed) == generation) return true;
    return false;
}
bool PlayerRepresentation(const std::uintptr_t character) noexcept {
    if (RegisteredPlayerRepresentation(character)) return true;
    std::uint32_t runtimeId{};
    return Read(character + 0x50U, runtimeId) && RegisteredPlayerSpawnKey(runtimeId);
}
bool Eligible(const std::uintptr_t c, std::uint32_t& type) noexcept {
    // PlayerSync owns every dynamically created remote-player Character. Test
    // the explicit ownership registry first, then require a certified NPC class
    // as an independent fail-closed boundary in case a spawn reservation is
    // cleared/reordered during retail lifecycle callbacks.
    if (PlayerRepresentation(c)) return false;
    std::uintptr_t characterTemplate{};
    if (!Read(c + 0x64U, characterTemplate) ||
        !Read(characterTemplate + 4U, type)) return false;
    // +1F is an execution gate, not proof that the NPC owns an initialized
    // manager/tree. Admit still validates the actual tree and Character owner.
    // AISync deliberately owns only the retail NPC families certified by the
    // reverse-engineering audit; unknown/player classes stay with their native
    // subsystem instead of being guessed into AI authority.
    return AICharacterClass(type);
}
bool CharacterIdentity(const std::uintptr_t character, std::uint32_t& entityA,
    std::uint32_t& entityB, std::uint32_t& type) noexcept {
    std::uintptr_t characterTemplate{};
    return character != 0U && Read(character + 0x54U, entityA) &&
        Read(character + 0x58U, entityB) && (entityA != 0U || entityB != 0U) &&
        Read(character + 0x64U, characterTemplate) &&
        Read(characterTemplate + 4U, type);
}
template<class Visitor>
bool ForEachCharacter(Visitor&& visitor) noexcept {
    if (gBase == 0U) return false;
    const auto table = gBase + kCharacterRegistry.table;
    for (std::size_t index = 0U; index < kCharacterRegistry.capacity; ++index) {
        const auto entry = table + index * kCharacterRegistry.stride;
        std::uintptr_t character{};
        std::uint8_t active{};
        if (!Read(entry + kCharacterRegistry.character, character) ||
            !Read(entry + kCharacterRegistry.active, active)) return false;
        if (active != 0U && character != 0U && !visitor(character)) return true;
    }
    return true;
}
std::uintptr_t FindCharacterByIdentity(const std::uint32_t entityA,
    const std::uint32_t entityB) noexcept {
    std::uintptr_t found{};
    static_cast<void>(ForEachCharacter([&](const std::uintptr_t character) noexcept {
        std::uint32_t candidateA{}, candidateB{}, type{};
        if (CharacterIdentity(character, candidateA, candidateB, type) &&
            candidateA == entityA && candidateB == entityB) {
            found = character;
            return false;
        }
        return true;
    }));
    return found;
}
bool ResolvePrimaryPlayerSlot(std::uintptr_t& slot, std::uintptr_t& player) noexcept {
    slot = 0U;
    player = 0U;
    std::uintptr_t root{}, context{}, players{};
    std::uint32_t count{};
    if (!Read(gBase + kInteractionManagerGlobalRva, root) ||
        !Read(root + 8U, context) || !Read(context + 0x84U, count) || count == 0U ||
        !Read(context + 0x80U, players) || players == 0U || !Read(players, player)) return false;
    slot = players;
    return player != 0U;
}
std::uintptr_t LocalPlayer() noexcept {
    // AISync identity must never mistake a temporary retail compatibility
    // projection for the actual local participant.
    if (gAiPlayerContextDepth != 0U && gAiPlayerRealLocal != 0U) return gAiPlayerRealLocal;
    std::uintptr_t slot{}, player{};
    return ResolvePrimaryPlayerSlot(slot, player) ? player : 0U;
}
bool LivingCharacter(const std::uintptr_t character) noexcept {
    std::uint16_t hp{};
    std::uint32_t health{};
    std::uint8_t enabled{};
    return character != 0U && Read(character + 0x33CU, hp) && hp != 0U &&
        Read(character + 0x32CU, health) && health < 3U &&
        Read(character + 0x2FAU, enabled) && enabled != 0U;
}
std::uintptr_t LivingAiParticipant(std::uintptr_t owner, std::uint64_t& peer) noexcept;

bool TerminalDeadCharacter(const std::uintptr_t character) noexcept {
    std::uint16_t hp{};
    std::uint32_t health{};
    return character != 0U && Read(character + 0x33CU, hp) && hp == 0U &&
        Read(character + 0x32CU, health) && health == 4U;
}

struct AiPlayerSlotScope final {
    std::uintptr_t slot{}, previous{};
    bool active{}, changed{};

    explicit AiPlayerSlotScope(const std::uintptr_t owner) noexcept {
        if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() ||
            !Stable() || owner == 0U) return;

        std::uintptr_t current{};
        if (!ResolvePrimaryPlayerSlot(slot, current)) return;
        const auto realLocal = gAiPlayerContextDepth != 0U && gAiPlayerRealLocal != 0U
            ? gAiPlayerRealLocal : current;
        // Do not alter THE_PLAYER during the kill itself. Only the certified
        // terminal state used by PlayerLifecycle opens this compatibility scope.
        if (!TerminalDeadCharacter(realLocal)) return;

        std::uint64_t peer{};
        const auto survivor = LivingAiParticipant(owner, peer);
        if (survivor == 0U || peer == 0U) return;

        if (gAiPlayerContextDepth == 0U) gAiPlayerRealLocal = realLocal;
        ++gAiPlayerContextDepth;
        active = true;
        previous = current;
        if (current == survivor) return;
        if (!Write(slot, survivor)) {
            --gAiPlayerContextDepth;
            if (gAiPlayerContextDepth == 0U) gAiPlayerRealLocal = 0U;
            active = false;
            slot = 0U;
            return;
        }
        changed = true;
        gAiPlayerSlotRemaps.fetch_add(1U, std::memory_order_relaxed);
        gLastAiPlayerSlotPeer.store(peer, std::memory_order_relaxed);
    }

    ~AiPlayerSlotScope() noexcept {
        if (!active) return;
        if (changed && !Write(slot, previous))
            gAiPlayerSlotRestoreFailures.fetch_add(1U, std::memory_order_relaxed);
        if (gAiPlayerContextDepth != 0U) --gAiPlayerContextDepth;
        if (gAiPlayerContextDepth == 0U) gAiPlayerRealLocal = 0U;
    }

    AiPlayerSlotScope(const AiPlayerSlotScope&) = delete;
    AiPlayerSlotScope& operator=(const AiPlayerSlotScope&) = delete;
};

bool ParticipantConstructionCurrent(const ParticipantBinding& binding,const bool local=false) noexcept {
    if(!ValidParticipantBinding(binding))return false;
    if(binding.incarnation==0U)return true; // legacy mapping, never strict pair permission
    character_incarnations::Ticket ticket{};
    std::uint32_t runtime{};
    return character_incarnations::NativeRegistry().Find(gCurrent.nativeGeneration,binding.character,ticket)==character_incarnations::Result::Ok&&
        MatchesParticipantTicket(binding,ticket,gCurrent.nativeGeneration,local)&&
        Read(binding.character+0x50U,runtime)&&runtime==binding.runtimeId&&
        character_incarnations::NativeRegistry().Current(ticket);
}
// Participants (local player or remote player proxies) an AI actor is
// currently pairing with. Published from the engine thread for network
// locomotion/equipment adapters, which must not move, teleport or re-equip a
// victim while the kill/grab graph aligns on it. Held briefly after the
// interaction ends so the retail outro also completes undisturbed.
constexpr ULONGLONG kPairedParticipantVictimHoldMs = 1500ULL;
constexpr std::size_t kPairedParticipantVictimSlots = 4U;
std::array<std::atomic<std::uintptr_t>, kPairedParticipantVictimSlots> gPairedParticipantVictims{};
std::array<std::atomic<ULONGLONG>, kPairedParticipantVictimSlots> gPairedParticipantVictimUntil{};

std::uintptr_t ParticipantForPeer(const std::uint64_t peer) noexcept;

void PublishPairedParticipantVictim(const std::uintptr_t victim, const ULONGLONG until) noexcept {
    std::size_t free = kPairedParticipantVictimSlots;
    const auto now = GetTickCount64();
    for (std::size_t i = 0U; i < kPairedParticipantVictimSlots; ++i) {
        if (gPairedParticipantVictims[i].load(std::memory_order_acquire) == victim) {
            gPairedParticipantVictimUntil[i].store(until, std::memory_order_release);
            return;
        }
        if (free == kPairedParticipantVictimSlots &&
            gPairedParticipantVictimUntil[i].load(std::memory_order_acquire) <= now) free = i;
    }
    if (free == kPairedParticipantVictimSlots) return;
    gPairedParticipantVictimUntil[free].store(0U, std::memory_order_release);
    gPairedParticipantVictims[free].store(victim, std::memory_order_release);
    gPairedParticipantVictimUntil[free].store(until, std::memory_order_release);
}

std::uintptr_t ParticipantForPeer(const std::uint64_t peer) noexcept {
    if (peer == 0U) return 0U;
    if (peer == gCurrent.localPeer) {
        const auto local=LocalPlayer();const auto& binding=gCurrent.localBinding;
        return binding.incarnation==0U||(binding.character==local&&ParticipantConstructionCurrent(binding,true))?local:0U;
    }
    for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i) {
        const auto& binding = gCurrent.participants[i];
        // Addresses are a local IPC hint, never a lifetime proof. PlayerSync's
        // generation-bound ownership must still certify the exact Character.
        if (binding.peer == peer && ParticipantConstructionCurrent(binding) && RegisteredPlayerRepresentation(binding.character))
            return binding.character;
    }
    return 0U;
}
std::uint64_t PeerForParticipant(const std::uintptr_t character) noexcept {
    if (character != 0U && character == ParticipantForPeer(gCurrent.localPeer)) return gCurrent.localPeer;
    for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i)
        if (gCurrent.participants[i].character == character &&
            ParticipantConstructionCurrent(gCurrent.participants[i]) &&
            RegisteredPlayerRepresentation(character)) return gCurrent.participants[i].peer;
    return 0U;
}

bool HostParticipantPlayerQuery(const std::uintptr_t owner, const std::uintptr_t target) noexcept {
    std::uint32_t type{};
    return gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        owner != 0U && target != owner && Eligible(owner, type) &&
        RegisteredPlayerRepresentation(target) && PeerForParticipant(target) != 0U && LivingCharacter(target);
}
std::uint32_t __fastcall TargetIsPlayerDetour(void* condition, void*,
    const std::uintptr_t owner, const std::uintptr_t execution) noexcept {
    const auto original = reinterpret_cast<TargetIsPlayer>(gTargetIsPlayerOriginal.load());
    if (original == nullptr) return 0U;
    const auto result = original(condition, owner, execution);
    if (result != 0U || owner == 0U || gAiPlayerQueryOwner != owner) return result;
    // Match the selected record and its native lifetime checks before adapting
    // the final local-FPS test. Never create/promote a candidate here.
    std::uintptr_t selection{}, manager{}, managerOwner{}, record{}, recordOwner{}, target{};
    std::uint32_t recordedBirth{}, currentBirth{};
    std::uint8_t nativeWorldReady{};
    if (!Read(gBase + kInteractionManagerGlobalRva + 4U, nativeWorldReady) || nativeWorldReady == 0U ||
        !Read(owner + 0x34A8U, selection) || selection == 0U ||
        !Read(selection + 4U, manager) || manager == 0U ||
        !Read(manager + 0x128U, managerOwner) || managerOwner != owner ||
        !Read(selection + 0x10U, record) || record == 0U ||
        !Read(record + 8U, recordOwner) || recordOwner != owner ||
        !Read(record + 0x42CU, target) || !HostParticipantPlayerQuery(owner, target) ||
        !Read(record + 0x430U, recordedBirth) || !Read(target + 0x50U, currentBirth) ||
        recordedBirth != currentBirth) return result;
    gTargetPlayerOverrides.fetch_add(1U, std::memory_order_relaxed);
    return 1U;
}
std::uint32_t __cdecl MenaceParticipantPlayer(const std::uintptr_t menace,
    const std::uintptr_t target) noexcept {
    std::uintptr_t owner{}, ownedMenace{};
    if (!Read(menace + 0x1D0U, owner) || gAiPlayerQueryOwner != owner ||
        !HostParticipantPlayerQuery(owner, target) || !Read(owner + 0x34D0U, ownedMenace) ||
        ownedMenace != menace) return 0U;
    gMenacePlayerOverrides.fetch_add(1U, std::memory_order_relaxed);
    return 1U;
}
std::uint32_t __cdecl SoundParticipantPlayer(const std::uintptr_t target) noexcept {
    if (!HostParticipantPlayerQuery(gSenseEventOwner, target)) return 0U;
    gSoundPlayerOverrides.fetch_add(1U, std::memory_order_relaxed);
    return 1U;
}

std::uintptr_t LivingAiParticipant(const std::uintptr_t owner,
    std::uint64_t& peer) noexcept {
    peer = 0U;
    // Prefer the participant already chosen (or promoted next) by retail.
    // The singular-player compatibility context must not drift to a nearer
    // participant while the native selector is pursuing someone else.
    std::uintptr_t selection{}, manager{}, managerOwner{};
    if (owner != 0U && Read(owner + 0x34A8U, selection) && selection != 0U &&
        Read(selection + 4U, manager) && manager != 0U &&
        Read(manager + 0x128U, managerOwner) && managerOwner == owner) {
        for (const auto offset : {0x10U, 0x14U}) {
            std::uintptr_t record{}, recordOwner{}, target{};
            std::uint32_t recordedBirth{}, currentBirth{};
            if (!Read(selection + offset, record) || record == 0U ||
                !Read(record + 8U, recordOwner) || recordOwner != owner ||
                !Read(record + 0x42CU, target) || target == owner ||
                !Read(record + 0x430U, recordedBirth) ||
                !Read(target + 0x50U, currentBirth) || recordedBirth != currentBirth ||
                !LivingCharacter(target)) continue;
            const auto selectedPeer = PeerForParticipant(target);
            if (selectedPeer == 0U) continue;
            peer = selectedPeer;
            return target;
        }
    }
    std::uintptr_t selected{};
    bool selectedHasDistance{};
    float bestDistance{};
    float ownerX{}, ownerY{}, ownerZ{};
    const bool ownerPosition = owner != 0U && Read(owner + 0x70U, ownerX) &&
        Read(owner + 0x74U, ownerY) && Read(owner + 0x78U, ownerZ) &&
        FiniteCoordinate(ownerX) && FiniteCoordinate(ownerY) && FiniteCoordinate(ownerZ);

    for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i) {
        const auto candidatePeer = gCurrent.participants[i].peer;
        const auto candidate = ParticipantForPeer(candidatePeer);
        if (!LivingCharacter(candidate)) continue;

        if (selected == 0U) {
            selected = candidate;
            peer = candidatePeer;
        }
        if (!ownerPosition) continue;

        float x{}, y{}, z{};
        if (!Read(candidate + 0x70U, x) || !Read(candidate + 0x74U, y) ||
            !Read(candidate + 0x78U, z) || !FiniteCoordinate(x) ||
            !FiniteCoordinate(y) || !FiniteCoordinate(z)) continue;
        const auto dx = x - ownerX;
        const auto dy = y - ownerY;
        const auto dz = z - ownerZ;
        const auto distance = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(distance)) continue;
        if (!selectedHasDistance || distance < bestDistance) {
            selectedHasDistance = true;
            bestDistance = distance;
            selected = candidate;
            peer = candidatePeer;
        }
    }
    return selected;
}

std::uintptr_t __cdecl ResolveAiPlayerQuery(const std::uintptr_t retailPlayer) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host ||
        !EngineThread() || !Stable() || gAiPlayerQueryOwner == 0U ||
        retailPlayer == 0U || !TerminalDeadCharacter(retailPlayer)) return retailPlayer;

    std::uint64_t peer{};
    const auto survivor = LivingAiParticipant(gAiPlayerQueryOwner, peer);
    if (survivor == 0U || peer == 0U) return retailPlayer;

    // This is not target forcing. It only adapts single-player-only AI helpers
    // which ask for THE_PLAYER while the real local Host Character is dead.
    // Senses/hostility/BT arbitration still choose the actual target.
    gAiPlayerQueryRemaps.fetch_add(1U, std::memory_order_relaxed);
    gLastAiPlayerQueryPeer.store(peer, std::memory_order_relaxed);
    return survivor;
}
__declspec(naked) std::uintptr_t __cdecl ThePlayerDetour() noexcept {
    // 0078EAF0 keeps its receiver in EDX across 0078BAF0, then writes
    // [EDX+3BC4]. The retail getter touches EAX/ECX/flags only. A normal C++
    // detour may destroy EDX and SIMD state despite its cdecl declaration.
    // Run the original first, then preserve its complete output state while
    // the AI-context mapper changes only the returned player in EAX.
    __asm {
        call dword ptr [gThePlayerTrampoline]
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push eax
        call ResolveAiPlayerQuery
        add esp, 4
        mov [ebx + 28], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        ret
    }
}
void CacheLivingPlayerAlliance() noexcept {
    if (gLivingPlayerAllianceKnown) return;
    const auto player = LocalPlayer();
    std::uint32_t alliance{};
    if (LivingCharacter(player) && Read(player + 0x2E4U, alliance) && alliance < 32U) {
        gLivingPlayerAlliance = alliance;
        gLivingPlayerAllianceKnown = true;
    }
}
bool ReleaseParticipantNavigation(ParticipantNavigation& navigation) noexcept {
    if (navigation.faulted) return false;
    if (navigation.initialized) {
        // The buffer owns a shared reference to the path request, not to an
        // old Character. Release remains valid across a level invalidation;
        // it never reads navigation.character or the old registry.
        __try {
            reinterpret_cast<NavigationRelease>(gBase + 0x002F58D0U)(navigation.native.data());
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            navigation.faulted = true;
            gBackstageNavigationRejected.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
    }
    navigation = {};
    return true;
}
void PumpHostNavigationContext() noexcept {
    const auto generation = gGenerations->CurrentGeneration();
    const bool host = gRole.load(std::memory_order_acquire) == Role::Host;
    if (!host && !gNavigationHostActive) return;
    gNavigationHostActive = host;
    const bool changed = generation != gNavigationGeneration ||
        gCurrent.sessionCookie != gNavigationSession || gCurrent.worldGeneration != gNavigationWorld;
    if (!host || changed) {
        for (auto& navigation : gParticipantNavigation)
            if (navigation.initialized) static_cast<void>(ReleaseParticipantNavigation(navigation));
        gNavigationGeneration = generation;
        gNavigationSession = gCurrent.sessionCookie;
        gNavigationWorld = gCurrent.worldGeneration;
        gNavigationMaintenance = 0U;
        gObjectiveNavigationGoalKnown = false;
    }
    if (!host || !Stable()) return;
    const auto now = GetTickCount64();
    if (!changed && now - gNavigationMaintenance < kRefreshPeriod) return;
    gNavigationMaintenance = now;
    // Retail's ambush sweep walks THE_PLAYER's own route cache toward the
    // current objective. Read the real local player's objective, never the
    // temporary AI slot, and keep the last enabled goal once death suspends
    // that player's manager. It is shared world context, not an AI target.
    std::uintptr_t path{};
    std::uint8_t enabled{};
    float objective[3]{};
    const auto player = LocalPlayer();
    if (LivingCharacter(player) && Read(player + 0x3500U, path) && path != 0U &&
        Read(path + 0x455U, enabled) && enabled != 0U &&
        Read(path + 0xCU, objective[0]) && Read(path + 0x10U, objective[1]) &&
        Read(path + 0x14U, objective[2]) && FiniteCoordinate(objective[0]) &&
        FiniteCoordinate(objective[1]) && FiniteCoordinate(objective[2])) {
        std::memcpy(gObjectiveNavigationGoal, objective, sizeof(objective));
        gObjectiveNavigationGoalKnown = true;
    }
    for (auto& navigation : gParticipantNavigation) {
        if (!navigation.initialized || navigation.faulted) continue;
        std::uint32_t birth{};
        const auto participant = ParticipantForPeer(navigation.peer);
        if (participant != navigation.character || !LivingCharacter(participant) ||
            !Read(participant + 0x50U, birth) || birth != navigation.runtimeId)
            static_cast<void>(ReleaseParticipantNavigation(navigation));
    }
}
std::uintptr_t HostBackstageOwner(const std::uintptr_t sweep) noexcept {
    std::uintptr_t owner{}, adapter{}, ownedSweep{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        !Read(sweep, owner) || !Eligible(owner, type) ||
        !Read(owner + 0x34F0U, adapter) || adapter == 0U ||
        !Read(adapter + 0x1CU, ownedSweep) || ownedSweep != sweep) return 0U;
    return owner;
}
bool HostBackstageParticipant(const std::uintptr_t sweep, const std::uintptr_t target) noexcept {
    const auto owner = HostBackstageOwner(sweep);
    return owner != 0U && gAiPlayerQueryOwner == owner &&
        RegisteredPlayerRepresentation(target) && PeerForParticipant(target) != 0U && LivingCharacter(target);
}
std::uint32_t __cdecl BackstageParticipantEligibility(
    const std::uintptr_t sweep, const std::uintptr_t target) noexcept {
    if (!HostBackstageParticipant(sweep, target)) return 0U;
    std::uintptr_t control{};
    std::uint8_t local{};
    if (!Read(target + 0x31CU, control) ||
        (control != 0U && !Read(control + 0x48U, local))) return 0U;
    if (control != 0U && local != 0U) return 0U; // Already admitted by retail.
    gBackstageEligibilityOverrides.fetch_add(1U, std::memory_order_relaxed);
    return 1U;
}
bool ReadCharacterPosition(const std::uintptr_t character, float (&position)[3]) noexcept {
    return character != 0U && Read(character + 0x70U, position[0]) &&
        Read(character + 0x74U, position[1]) && Read(character + 0x78U, position[2]) &&
        FiniteCoordinate(position[0]) && FiniteCoordinate(position[1]) && FiniteCoordinate(position[2]);
}
float SquaredDistance(const float (&a)[3], const float (&b)[3]) noexcept {
    const auto dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}
bool UpdateParticipantNavigation(ParticipantNavigation& navigation, const std::uintptr_t owner,
    const ULONGLONG now) noexcept {
    if (navigation.faulted) return false;
    if (navigation.sessionCookie != gNavigationSession ||
        navigation.nativeGeneration != gNavigationGeneration || navigation.worldGeneration != gNavigationWorld ||
        (navigation.lastUpdate != 0U && now - navigation.lastUpdate < kSendPeriod)) return true;

    // 0040AF70 walks this cache's route (+50, count +450 must exceed one)
    // from the participant toward a destination and picks emergence vents
    // along it. Retail's destination is THE_PLAYER's objective. A goal equal
    // to the participant's own position yields a one-node route: the sweep
    // then never selects a vent and the Alien stays backstage forever once
    // the Host is dead. Prefer the shared objective; when it is unknown or
    // already reached, route toward the sweeping Alien so the native sweep
    // still has a real path to scan. Neither choice selects an AI target:
    // candidate selection, eligibility and vent choice remain native.
    constexpr float kMinimumRouteLength = 4.0F;
    constexpr float kGoalRefreshDistance = 3.0F;
    const auto target = navigation.character;
    std::uint32_t birth{};
    float start[3]{}, goal[3]{}, alien[3]{};
    if (target == 0U || ParticipantForPeer(navigation.peer) != target || !LivingCharacter(target) ||
        !Read(target + 0x50U, birth) || birth != navigation.runtimeId ||
        !ReadCharacterPosition(target, start)) return false;
    constexpr float kMinimumSquared = kMinimumRouteLength * kMinimumRouteLength;
    if (gObjectiveNavigationGoalKnown &&
        SquaredDistance(gObjectiveNavigationGoal, start) >= kMinimumSquared) {
        std::memcpy(goal, gObjectiveNavigationGoal, sizeof(goal));
        gBackstageGoalsObjective.fetch_add(1U, std::memory_order_relaxed);
    } else if (ReadCharacterPosition(owner, alien) && SquaredDistance(alien, start) >= kMinimumSquared) {
        std::memcpy(goal, alien, sizeof(goal));
        gBackstageGoalsOwner.fetch_add(1U, std::memory_order_relaxed);
    } else {
        std::memcpy(goal, start, sizeof(goal));
        gBackstageGoalsDegenerate.fetch_add(1U, std::memory_order_relaxed);
    }
    // A moving goal on a new polygon forces a native repath. Keep the applied
    // goal until it drifts meaningfully so pending queries can complete.
    if (navigation.goalApplied &&
        SquaredDistance(navigation.goal, goal) < kGoalRefreshDistance * kGoalRefreshDistance)
        std::memcpy(goal, navigation.goal, sizeof(goal));

    __try {
        // 006C5D70: ECX=route cache, goal pointer on stack, RET 4.
        // 00721270: ECX=route cache, plain RET. Only updates/consumes the
        // native path request; does not run the participant's suppressed AI.
        reinterpret_cast<NavigationGoal>(gBase + 0x002C5D70U)(navigation.native.data(), goal);
        // 00721270 caches an empty completed route if the mesh revision,
        // participant position and goal do not change. Hiding participants may
        // remain stationary indefinitely. Re-arm ONLY the route's native dirty
        // flag (the same +454 producer field used by 006C5D70), preserving its
        // endpoints and the native vent/candidate selection. Never replace or
        // cancel an outstanding shared path request.
        const auto address = reinterpret_cast<std::uintptr_t>(navigation.native.data());
        std::uintptr_t query{};
        std::uint32_t routeCount{};
        std::uint8_t dirty{}, enabled{};
        if (!Read(address, query) || !Read(address + 0x450U, routeCount) ||
            !Read(address + 0x454U, dirty) || !Read(address + 0x455U, enabled)) return false;
        if (navigation.emptyRouteRetry.Observe(enabled != 0U, query != 0U, dirty != 0U,
                routeCount, SquaredDistance(goal, start) >= kMinimumSquared, now)) {
            if (!Write(address + 0x454U, std::uint8_t{1U})) return false;
            ++navigation.emptyRouteRetries;
        }
        reinterpret_cast<NavigationUpdate>(gBase + 0x00321270U)(navigation.native.data());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        navigation.faulted = true;
        gBackstageNavigationRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    std::memcpy(navigation.goal, goal, sizeof(goal));
    navigation.goalApplied = true;
    navigation.lastUpdate = now;
    gBackstageNavigationUpdates.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
std::uintptr_t __cdecl BackstageParticipantNavigation(
    const std::uintptr_t sweep, const std::uintptr_t target) noexcept {
    std::uintptr_t retail{};
    if (!Read(target + 0x3500U, retail)) return 0U;
    if (retail != 0U || !HostBackstageParticipant(sweep, target)) return retail;
    const auto peer = PeerForParticipant(target);
    std::uint32_t birth{};
    if (!Read(target + 0x50U, birth)) return 0U;
    ParticipantNavigation* found{};
    ParticipantNavigation* free{};
    for (auto& navigation : gParticipantNavigation) {
        if (navigation.faulted) continue;
        if (!navigation.initialized) { if (free == nullptr) free = &navigation; continue; }
        if (navigation.peer != peer) continue;
        if (navigation.character != target || navigation.runtimeId != birth ||
            navigation.nativeGeneration != gCurrent.nativeGeneration ||
            navigation.worldGeneration != gCurrent.worldGeneration ||
            navigation.sessionCookie != gCurrent.sessionCookie) {
            if (!ReleaseParticipantNavigation(navigation)) return 0U;
        }
        found = &navigation;
        break;
    }
    if (found == nullptr) found = free;
    if (found == nullptr) return 0U;
    if (!found->initialized) {
        found->peer = peer; found->character = target; found->runtimeId = birth;
        found->nativeGeneration = gCurrent.nativeGeneration;
        found->worldGeneration = gCurrent.worldGeneration; found->sessionCookie = gCurrent.sessionCookie;
        const auto address = reinterpret_cast<std::uintptr_t>(found->native.data());
        // Constructor 006EE5B0: null query, Character +8, invalid nav mesh
        // revision +44, empty route/count +450 and disabled goal +454/+455.
        // An empty route still lets 0040BBF0 take its native scripted-vent
        // fallback (0040AD00); a missing cache returned before that fallback.
        if (!Write(address + 8U, target) || !Write(address + 0x44U, 0xFFFFFFFFU)) return 0U;
        found->initialized = true;
    }
    if (!UpdateParticipantNavigation(*found, HostBackstageOwner(sweep), GetTickCount64())) return 0U;
    gBackstageNavigationPlans.fetch_add(1U, std::memory_order_relaxed);
    // Used only as native EDI in 0040BBF0. Never installed at Character+3500,
    // never borrowed from the dead Host, and never transmitted over the wire.
    return reinterpret_cast<std::uintptr_t>(found->native.data());
}
bool NativeVictimFlowRunning(std::uintptr_t source, std::uintptr_t victim) noexcept;
std::uintptr_t __fastcall BackstageSelectedTargetDetour(void* selection, void*) noexcept {
    const auto target = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(gBase + 0x0000A3D0U)(selection);
    const auto owner = gAiPlayerQueryOwner;
    std::uintptr_t ownedSelection{};
    if (owner != 0U && gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        Read(owner + 0x34A8U, ownedSelection) && ownedSelection == reinterpret_cast<std::uintptr_t>(selection) &&
        PeerForParticipant(target) != 0U && TerminalDeadCharacter(target) && !NativeVictimFlowRunning(owner, target)) {
        gBackstageDeadTargetsRejected.fetch_add(1U, std::memory_order_relaxed);
        return 0U; // Let the original native THE_PLAYER fallback run.
    }
    return target;
}
std::uint8_t __fastcall BackstageUpdateDetour(void* sweep, void*) noexcept {
    const auto original = reinterpret_cast<BackstageUpdate>(gBackstageUpdateOriginal.load());
    if (original == nullptr) return 0U;
    const auto owner = HostBackstageOwner(reinterpret_cast<std::uintptr_t>(sweep));
    if (SuppressedAlien(owner)) return 0U;
    const auto previous = gAiPlayerQueryOwner;
    if (owner != 0U) gAiPlayerQueryOwner = owner;
    std::uint8_t result{};
    { AiPlayerSlotScope context(owner); result = original(sweep); }
    gAiPlayerQueryOwner = previous;
    return result;
}
void __fastcall BackstageHoldDetour(void* cycle, void*) noexcept {
    const auto original = reinterpret_cast<BackstageHoldUpdate>(gBackstageHoldOriginal.load());
    if (original == nullptr) return;
    std::uintptr_t owner{}, currentCycle{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        !Read(reinterpret_cast<std::uintptr_t>(cycle) + 4U, owner) || !Eligible(owner, type) ||
        !Read(owner + 0x36CU, currentCycle) || currentCycle != reinterpret_cast<std::uintptr_t>(cycle)) owner = 0U;
    if (SuppressedAlien(owner)) return;
    const auto previous = gAiPlayerQueryOwner;
    if (owner != 0U) { gAiPlayerQueryOwner = owner; gBackstageHoldContexts.fetch_add(1U, std::memory_order_relaxed); }
    // The 4D physical update runs outside BehaviourManager. Its closest-player
    // query must see the same surviving player context as native AI decisions.
    { AiPlayerSlotScope context(owner); original(cycle); }
    gAiPlayerQueryOwner = previous;
}
bool ReadPairedCycleActivity(const std::uintptr_t character, std::uintptr_t& cycle,
    std::uint8_t& state, bool& active) noexcept {
    active = false;
    std::uintptr_t owner{}, target{};
    std::uint8_t flags{};
    std::uint32_t targetBirth{}, currentBirth{}, stage{};
    if (!Read(character + 0x36CU, cycle) || cycle == 0U ||
        !Read(cycle + 4U, owner) || owner != character || !Read(cycle + 0x1AU, state)) return false;
    if (state != 0x42U && state != 0x4EU) return true;
    if (!Read(cycle + 8U, flags) || !Read(cycle + 0x44U, target)) return false;
    // Native completion/interruption flags survive while the persistent cycle
    // still has its paired type and victim GUID. A failed initializer also
    // leaves that type behind with a null cached victim.
    if ((flags & 6U) != 0U || target == 0U) return true;
    if (!Read(cycle + 0x48U, targetBirth) || !Read(target + 0x50U, currentBirth)) return false;
    if (targetBirth != currentBirth) return true;
    // FUN_00663E00 no longer advances stage 6; both failed admission and the
    // natural end can reach it without changing +1A or setting +8 flags.
    if (state == 0x4EU) {
        if (!Read(cycle + 0xE0U, stage)) return false;
        if (stage >= 6U) return true;
    }
    active = true;
    return true;
}
bool PairedCycleMatches(const std::uintptr_t character, const State& interaction) noexcept {
    std::uintptr_t cycle{};
    std::uintptr_t owner{};
    std::uint8_t state{};
    std::uint32_t targetA{}, targetB{};
    return Read(character + 0x36CU, cycle) && Read(cycle + 0x1AU, state) &&
        Read(cycle + 4U, owner) && owner == character &&
        (state == 0x42U || state == 0x4EU) &&
        Read(cycle + (state == 0x4EU ? 0x100U : 0xD8U), targetA) &&
        Read(cycle + (state == 0x4EU ? 0x104U : 0xDCU), targetB) &&
        targetA == interaction.interactionTargetA && targetB == interaction.interactionTargetB;
}
bool NativeVictimFlowRunning(std::uintptr_t source, std::uintptr_t victim) noexcept;
bool IsPvPTechnicalParticipant(std::uintptr_t character) noexcept;
std::uint32_t __fastcall HostilityDetour(void* logical, void*, const std::uintptr_t target) noexcept {
    const auto original = reinterpret_cast<Hostility>(gHostilityOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    if (IsPvPTechnicalParticipant(target)) return 0U;
    std::uintptr_t owner{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        !Read(reinterpret_cast<std::uintptr_t>(logical), owner) || !Eligible(owner, type) ||
        PeerForParticipant(target) == 0U) return original(logical, target);
    if (!LivingCharacter(target) && !NativeVictimFlowRunning(owner, target)) return 0U;
    if (NativeVictimFlowRunning(owner, target)) return 1U;
    if (!RegisteredPlayerRepresentation(target)) return original(logical, target);
    CacheLivingPlayerAlliance();
    std::uintptr_t root{}, matrix{};
    std::uint32_t alliance{}, row{};
    // FUN_0077A800's exact alliance predicate. Treat an AIMP human visual as
    // the living player's alliance, without editing templates/global stances
    // or resurrecting THE_PLAYER (death changes its alliance to 7).
    if (!gLivingPlayerAllianceKnown ||
        !Read(gBase + kInteractionManagerGlobalRva, root) || !Read(root + 0x34U, matrix) ||
        !Read(reinterpret_cast<std::uintptr_t>(logical) + 0x64U, alliance) || alliance >= 32U ||
        !Read(matrix + alliance * 4U, row)) return 0U;
    return (row & (1U << gLivingPlayerAlliance)) != 0U ? 1U : 0U;
}
std::uintptr_t SenseEventOwner(void* context, const std::uintptr_t sensesOffset) noexcept {
    std::uintptr_t senses{}, owner{}, ownedSenses{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        !Read(reinterpret_cast<std::uintptr_t>(context) + sensesOffset, senses) ||
        !Read(senses + 0x8CU, owner) || !Eligible(owner, type) ||
        !Read(owner + 0x34D4U, ownedSenses) || ownedSenses != senses) return 0U;
    return owner;
}
void DispatchSenseEvent(void* context, const std::uintptr_t event,
    const std::uintptr_t sensesOffset, const SenseEvent original) noexcept {
    if (original == nullptr) return;
    const auto previousOwner = gSenseEventOwner;
    gSenseEventOwner = SenseEventOwner(context, sensesOffset);
    if (gSenseEventOwner != 0U) gSenseEventPasses.fetch_add(1U, std::memory_order_relaxed);
    __try { original(context, event); }
    __finally { gSenseEventOwner = previousOwner; }
}
void __fastcall SenseEventDetour(void* context, void*, const std::uintptr_t event) noexcept {
    DispatchSenseEvent(context, event, 0x14U,
        reinterpret_cast<SenseEvent>(gSenseEventOriginal.load(std::memory_order_acquire)));
}
void __fastcall SoundEventDetour(void* context, void*, const std::uintptr_t event) noexcept {
    DispatchSenseEvent(context, event, 0x10U,
        reinterpret_cast<SenseEvent>(gSoundEventOriginal.load(std::memory_order_acquire)));
}
std::uintptr_t __fastcall SenseCharacterResolverDetour(void* resources, void*,
    const std::uint32_t* identity) noexcept {
    const auto retail = reinterpret_cast<SenseCharacterResolver>(gBase + 0x002545C0U)(resources, identity);
    if (gSenseEventOwner == 0U || gRole.load(std::memory_order_acquire) != Role::Host ||
        !EngineThread() || !Stable()) return retail;
    const auto address = reinterpret_cast<std::uintptr_t>(identity);
    std::uint32_t entityA{}, entityB{};
    if (!Read(address, entityA) || !Read(address + 4U, entityB) ||
        (entityA == 0U && entityB == 0U)) return retail;
    std::uintptr_t mapped{};
    std::uint64_t peer{};
    for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i) {
        const auto candidatePeer = gCurrent.participants[i].peer;
        const auto target = ParticipantForPeer(candidatePeer);
        std::uint32_t a{}, b{}, type{};
        if (target == 0U || target == gSenseEventOwner ||
            !RegisteredPlayerRepresentation(target) || !LivingCharacter(target) ||
            !CharacterIdentity(target, a, b, type) || a != entityA || b != entityB) continue;
        if (mapped != 0U && (mapped != target || peer != candidatePeer)) {
            gParticipantSignalAmbiguous.fetch_add(1U, std::memory_order_relaxed);
            return retail;
        }
        mapped = target;
        peer = candidatePeer;
    }
    if (mapped == 0U) return retail;
    // An identity already belonging to another live native Character is not a
    // missing registration. Fail closed on that collision instead of rebinding it.
    if (retail != 0U && retail != mapped) {
        std::uint32_t a{}, b{}, type{};
        if (CharacterIdentity(retail, a, b, type) && a == entityA && b == entityB) {
            gParticipantSignalAmbiguous.fetch_add(1U, std::memory_order_relaxed);
            return retail;
        }
    }
    gParticipantSignalLookups.fetch_add(1U, std::memory_order_relaxed);
    gLastParticipantSignalPeer.store(peer, std::memory_order_relaxed);
    if (retail == mapped) return retail;
    if (retail == 0U) gParticipantSignalRetailMissing.fetch_add(1U, std::memory_order_relaxed);
    else gParticipantSignalRetailMismatch.fetch_add(1U, std::memory_order_relaxed);
    gParticipantSignalRemaps.fetch_add(1U, std::memory_order_relaxed);
    // Restore the exact source of an existing native event, not a sight/threat
    // value or forced target. Native strength, occlusion and arbitration remain.
    return mapped;
}
bool PairGenerationStable() noexcept {
    const auto generation = gActiveNativeGeneration.load(std::memory_order_acquire);
    return !gStopping.load(std::memory_order_acquire) && generation != 0U &&
        gGenerations != nullptr && gGenerations->IsStableGeneration(generation);
}
bool LoadPairBinding(const PairBinding& slot, PairIdentity& value) noexcept {
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        const auto before = slot.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        value.character = slot.character.load(std::memory_order_acquire);
        value.animated = slot.animated.load(std::memory_order_acquire);
        value.runtimeId = slot.runtimeId.load(std::memory_order_acquire);
        value.authoritativeBirth = slot.authoritativeBirth.load(std::memory_order_acquire);
        value.participantIncarnation = slot.participantIncarnation.load(std::memory_order_acquire);
        value.creationSerial = slot.creationSerial.load(std::memory_order_acquire);
        value.entityA = slot.entityA.load(std::memory_order_acquire);
        value.entityB = slot.entityB.load(std::memory_order_acquire);
        value.type = slot.type.load(std::memory_order_acquire);
        value.generation = slot.generation.load(std::memory_order_acquire);
        value.epoch = slot.epoch.load(std::memory_order_acquire);
        value.peer = slot.peer.load(std::memory_order_acquire);
        if (before != slot.sequence.load(std::memory_order_acquire)) continue;
        return value.character != 0U && PairGenerationStable() &&
            value.generation == gActiveNativeGeneration.load(std::memory_order_acquire) &&
            value.epoch == gPairBindingEpoch.load(std::memory_order_acquire);
    }
    return false;
}
bool PairIdentityLive(const PairIdentity& value) noexcept {
    std::uint32_t id{}, a{}, b{}, type{};
    if(!PairGenerationStable()||value.generation!=gActiveNativeGeneration.load(std::memory_order_acquire)||
        value.epoch!=gPairBindingEpoch.load(std::memory_order_acquire))return false;
    character_incarnations::Ticket ticket{};
    if(value.creationSerial!=0U&&(character_incarnations::NativeRegistry().Find(value.generation,value.character,ticket)!=
        character_incarnations::Result::Ok||ticket.serial!=value.creationSerial||ticket.runtimeId!=value.runtimeId))return false;
    return Read(value.character + 0x50U, id) && id == value.runtimeId &&
        CharacterIdentity(value.character, a, b, type) && a == value.entityA &&
        b == value.entityB && type == value.type&&
        (value.creationSerial==0U||character_incarnations::NativeRegistry().Current(ticket))&&PairGenerationStable()&&
        value.generation==gActiveNativeGeneration.load(std::memory_order_acquire)&&
        value.epoch==gPairBindingEpoch.load(std::memory_order_acquire);
}
bool IsPvPTechnicalParticipant(const std::uintptr_t character) noexcept {
    const auto peer=gPvPTechnicalPeer.load(std::memory_order_acquire);
    const auto epoch=gPvPTechnicalEpoch.load(std::memory_order_acquire);
    if(!peer || !character || epoch!=gPairBindingEpoch.load(std::memory_order_acquire))return false;
    for(const auto& slot:gPairParticipants) {
        PairIdentity participant{};
        if(LoadPairBinding(slot,participant) && participant.character==character && participant.peer==peer &&
            participant.epoch==epoch && PairIdentityLive(participant) &&
            peer==gPvPTechnicalPeer.load(std::memory_order_acquire) &&
            epoch==gPvPTechnicalEpoch.load(std::memory_order_acquire))return true;
    }
    return false;
}
bool AllowLocalNativeDamage(const std::uintptr_t character, const std::uintptr_t source) noexcept {
    // Only the exact published technical player of the Alien owner is immune.
    // Does not patch health, protect the Alien NPC or alter survivor damage.
    if(IsPvPTechnicalParticipant(character))return false;
    if (!Client()) return true;
    const auto* submission = gNativeVictimSubmission;
    if (submission != nullptr && submission->authoritativeClient && submission->slot < kActors &&
        submission->pairRevision != 0U && submission->source == source &&
        submission->victimBefore.character == character && PairIdentityLive(submission->victimBefore)) {
        std::uintptr_t current{};
        std::uint8_t serial{};
        if (Read(source + 0x36CU, current) && current == submission->sourceCycle &&
            Read(current + 0x19U, serial) && serial == submission->sourceSerial) return true;
    }
    std::uint32_t type{};
    return !Eligible(character, type) && !Eligible(source, type);
}
bool PublishedPairParticipant(const std::uintptr_t character, PairIdentity& value) noexcept {
    for (const auto& slot : gPairParticipants) {
        PairIdentity candidate{};
        if (!LoadPairBinding(slot, candidate) || candidate.character != character ||
            candidate.peer == 0U || !PairIdentityLive(candidate)) continue;
        if (&slot != &gPairParticipants[0] && !RegisteredPlayerRepresentation(character)) return false;
        value = candidate;
        return true;
    }
    return false;
}
std::size_t PublishedPairSource(const std::uintptr_t character, PairIdentity& value) noexcept {
    for (std::size_t i = 0U; i < kActors; ++i) {
        PairIdentity candidate{};
        std::uint32_t type{};
        if (LoadPairBinding(gPairSources[i], candidate) && candidate.character == character &&
            PairIdentityLive(candidate) && Eligible(character, type) && type == candidate.type) {
            value = candidate;
            return i;
        }
    }
    return kActors;
}

bool PublishedPairVictim(const std::uintptr_t character, PairIdentity& value) noexcept {
    if (PublishedPairParticipant(character, value)) return true;
    return PublishedPairSource(character, value) != kActors && value.peer == 0U &&
        value.authoritativeBirth != 0U;
}
bool PairVictimMatches(const State& state, const PairIdentity& victim) noexcept {
    if ((state.interactionFlags & InteractionTargetIsParticipant) != 0U)
        return MatchesParticipantTarget(state.interactionTargetPeer,state.interactionParticipantIncarnation,
            victim.peer,victim.participantIncarnation)&&
            state.interactionTargetBirth == 0U && state.interactionTargetClass == 0U;
    return state.interactionTargetPeer == 0U && victim.peer == 0U &&
        state.interactionTargetBirth != 0U &&
        victim.authoritativeBirth == state.interactionTargetBirth &&
        victim.type == state.interactionTargetClass &&
        victim.entityA == state.interactionTargetA && victim.entityB == state.interactionTargetB;
}
bool SameInteractionTarget(const State& a,const State& b) noexcept {
    return a.interactionTargetPeer==b.interactionTargetPeer&&
        a.interactionParticipantIncarnation==b.interactionParticipantIncarnation&&
        a.interactionTargetBirth==b.interactionTargetBirth&&a.interactionTargetClass==b.interactionTargetClass&&
        a.interactionTargetA==b.interactionTargetA&&a.interactionTargetB==b.interactionTargetB&&
        a.interactionAuthority==b.interactionAuthority&&a.interactionCommand==b.interactionCommand&&
        std::equal(std::begin(a.interactionSession),std::end(a.interactionSession),std::begin(b.interactionSession));
}
std::uintptr_t PairTargetForState(const State& state) noexcept {
    const bool participant = (state.interactionFlags & InteractionTargetIsParticipant) != 0U;
    const auto find = [&](const auto& slots) noexcept -> std::uintptr_t {
        std::uintptr_t target{};
        for (const auto& slot : slots) {
            PairIdentity candidate{};
            if (!LoadPairBinding(slot, candidate) || !PairIdentityLive(candidate) ||
                !PairVictimMatches(state, candidate)) continue;
            if (target != 0U && target != candidate.character) return 0U;
            target = candidate.character;
        }
        return target;
    };
    return participant ? find(gPairParticipants) : find(gPairSources);
}

void InvalidatePairCharacter(const std::uintptr_t character) noexcept {
    for (auto& slot : gPairSources) {
        auto expected = character;
        slot.character.compare_exchange_strong(expected, 0U, std::memory_order_acq_rel);
    }
    for (auto& slot : gPairParticipants) {
        auto expected = character;
        slot.character.compare_exchange_strong(expected, 0U, std::memory_order_acq_rel);
    }
}
void PublishPairBinding(PairBinding& slot, const std::uintptr_t character,
    const std::uint64_t peer, const std::uint32_t authoritativeBirth = 0U) noexcept {
    // Single engine-thread publisher; getters are never invoked by IPC workers.
    PairIdentity value{};
    value.character = character; value.peer = peer; value.authoritativeBirth = authoritativeBirth;
    value.generation = gCurrent.nativeGeneration;
    value.epoch = gPairBindingEpoch.load(std::memory_order_acquire);
    if (character != 0U && (!Read(character + 0x50U, value.runtimeId) || value.runtimeId == 0U ||
        !CharacterIdentity(character, value.entityA, value.entityB, value.type))) value.character = 0U;
    if(value.character!=0U) {
        character_incarnations::Ticket ticket{};
        if(character_incarnations::NativeRegistry().Find(value.generation,character,ticket)==character_incarnations::Result::Ok&&
            ticket.runtimeId==value.runtimeId)value.creationSerial=ticket.serial;
        const ParticipantBinding* certificate=peer==gCurrent.localPeer?&gCurrent.localBinding:nullptr;
        if(peer!=0U&&!certificate)for(std::size_t i=0;i<gCurrent.participantCount;++i)
            if(gCurrent.participants[i].peer==peer){certificate=&gCurrent.participants[i];break;}
        if(certificate&&certificate->incarnation!=0U) {
            if(certificate->character!=character||!MatchesParticipantTicket(*certificate,ticket,value.generation,peer==gCurrent.localPeer))
                value.character=0U;
            else value.participantIncarnation=certificate->incarnation;
        }
    }
    if (value.character != 0U) {
        PairIdentity previous{};
        if (LoadPairBinding(slot, previous) && previous.character == value.character &&
            previous.runtimeId == value.runtimeId && previous.entityA == value.entityA &&
            previous.entityB == value.entityB && previous.type == value.type && previous.peer == peer &&
            previous.authoritativeBirth == value.authoritativeBirth &&
            previous.creationSerial==value.creationSerial&&previous.participantIncarnation==value.participantIncarnation&&
            previous.animated != 0U)
            return;
        __try {
            std::uintptr_t table{}, getter{};
            if (Read(character, table) && Read(table + 0x1CU, getter) &&
                getter >= gBase + 0x1000U && getter < gBase + 0x00D9F000U)
                value.animated = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(
                    reinterpret_cast<void*>(character));
        } __except (EXCEPTION_EXECUTE_HANDLER) { value.animated = 0U; }
    }
    slot.sequence.fetch_add(1U, std::memory_order_acq_rel);
    slot.character.store(0U, std::memory_order_release);
    slot.runtimeId.store(value.runtimeId, std::memory_order_release);
    slot.authoritativeBirth.store(value.authoritativeBirth, std::memory_order_release);
    slot.creationSerial.store(value.creationSerial,std::memory_order_release);
    slot.participantIncarnation.store(value.participantIncarnation,std::memory_order_release);
    slot.entityA.store(value.entityA, std::memory_order_release);
    slot.entityB.store(value.entityB, std::memory_order_release);
    slot.type.store(value.type, std::memory_order_release);
    slot.generation.store(value.generation, std::memory_order_release);
    slot.epoch.store(value.epoch, std::memory_order_release);
    slot.peer.store(value.peer, std::memory_order_release);
    slot.animated.store(value.animated, std::memory_order_release);
    slot.character.store(value.character, std::memory_order_release);
    slot.sequence.fetch_add(1U, std::memory_order_release);
}
void PublishPairSource(Actor& actor) noexcept {
    const auto index = static_cast<std::size_t>(&actor - gActors.data());
    PublishPairBinding(gPairSources[index], !actor.faulted && !actor.duplicate ? actor.character : 0U, 0U,
        Client() ? actor.remoteBirth : actor.birth);
}
void RefreshPairBindings() noexcept {
    PublishPairBinding(gPairParticipants[0], ParticipantForPeer(gCurrent.localPeer), gCurrent.localPeer);
    for (std::size_t i = 0U; i < kParticipants; ++i) {
        const auto& participant = gCurrent.participants[i];
        const bool admitted = i < gCurrent.participantCount &&
            ParticipantForPeer(participant.peer)==participant.character&&RegisteredPlayerRepresentation(participant.character);
        PublishPairBinding(gPairParticipants[i + 1U], admitted ? participant.character : 0U,
            admitted ? participant.peer : 0U);
    }
    for (auto& actor : gActors) PublishPairSource(actor);
}
void Publish(Actor& actor) noexcept;
bool PublishOutcomeBinding(Actor& actor, const std::uintptr_t victim) noexcept {
    PairIdentity sourceIdentity{}, victimIdentity{};
    const auto slot = PublishedPairSource(actor.character, sourceIdentity);
    if (slot == kActors || !PublishedPairVictim(victim, victimIdentity) ||
        !PairVictimMatches(actor.desired, victimIdentity)) return false;
    std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    auto& binding = gOutcomes[slot];
    const bool retainedPair=binding.state.birth==actor.desired.birth&&actor.desired.interactionPairRevision!=0U&&
        binding.state.interactionPairRevision==actor.desired.interactionPairRevision;
    if(retainedPair&&(binding.state.interactionParticipantIncarnation!=0U||actor.desired.interactionParticipantIncarnation!=0U)&&
        (!SameInteractionTarget(binding.state,actor.desired)||binding.source.character!=sourceIdentity.character||
         binding.source.runtimeId!=sourceIdentity.runtimeId||binding.source.creationSerial!=sourceIdentity.creationSerial))return false;
    const bool same = binding.source.character == sourceIdentity.character &&
        binding.source.runtimeId == sourceIdentity.runtimeId && binding.source.epoch == sourceIdentity.epoch &&
        binding.state.birth == actor.desired.birth &&
        binding.state.interactionPairRevision == actor.desired.interactionPairRevision &&
        binding.state.interactionTargetPeer == actor.desired.interactionTargetPeer &&
        binding.state.interactionTargetA == actor.desired.interactionTargetA &&
        binding.state.interactionTargetB == actor.desired.interactionTargetB &&
        binding.state.interactionTargetBirth == actor.desired.interactionTargetBirth &&
        binding.state.interactionParticipantIncarnation==actor.desired.interactionParticipantIncarnation&&
        binding.state.interactionTargetClass == actor.desired.interactionTargetClass;
    // Both NPC and player pairs freeze the exact local victim incarnation.
    if (!same) binding = {};
    else if(binding.victim.character!=victimIdentity.character||binding.victim.runtimeId!=victimIdentity.runtimeId||
         binding.victim.creationSerial!=victimIdentity.creationSerial||
         binding.victim.participantIncarnation!=victimIdentity.participantIncarnation)return false;
    binding.source = sourceIdentity; binding.victim = victimIdentity;
    // Host workers own the result; refreshing the pair header must not erase it.
    const auto result = binding.state.outcome;
    binding.state = actor.desired;
    if (!Client()) binding.state.outcome = result;
    return true;
}
bool LoadOutcome(const std::uintptr_t source, const std::uintptr_t victim,
    OutcomeBinding& value, const bool wait = false) noexcept {
    PairIdentity identity{};
    const auto slot = PublishedPairSource(source, identity);
    if (slot == kActors) return false;
    std::unique_lock lock(gOutcomeMutex, std::defer_lock);
    if (wait) lock.lock();
    else if (!lock.try_lock()) return false;
    value = gOutcomes[slot];
    PairIdentity currentVictim{};
    if (!PublishedPairVictim(victim, currentVictim) || !PairVictimMatches(value.state, currentVictim)) return false;
    return value.source.character == source && value.source.runtimeId == identity.runtimeId &&
        value.source.epoch == identity.epoch && value.victim.character == victim &&
        value.state.interactionPairRevision != 0U && PairIdentityLive(value.victim);
}
bool ClaimOutcome(NativeVictimSubmission& submission, const std::size_t slot) noexcept {
    std::lock_guard lock(gOutcomeMutex);
    auto& binding = gOutcomes[slot];
    if (binding.source.character != submission.source || binding.victim.character != submission.victimBefore.character ||
        binding.source.epoch != submission.victimBefore.epoch ||
        binding.victim.runtimeId != submission.victimBefore.runtimeId ||
        binding.state.interactionPairRevision == 0U ||
        !PairVictimMatches(binding.state, submission.victimBefore)) return false;
    if (binding.claimed) {
        submission.duplicateOutcome = true;
        gOutcomeDuplicates.fetch_add(1U, std::memory_order_relaxed); return false;
    }
    const bool client = gRole.load(std::memory_order_acquire) == Role::Client;
    if (client && (binding.state.outcome.flags & OutcomePrepared) == 0U) return false;
    binding.claimed = true; binding.serial = submission.sourceSerial;
    submission.slot = slot; submission.pairRevision = binding.state.interactionPairRevision;
    submission.authoritativeClient = client;
    if (client) {
        submission.outcome = binding.state.outcome;
        submission.transaction=binding.transaction;submission.transactionSource=binding.transactionActors.source;
    }
    return true;
}
bool CapturePairTransactionEvent(const std::uintptr_t source,const std::uintptr_t victim,const std::uintptr_t cycle,
    const bool victimSide,const interactions::FinishCause cause,PairTransactionEvent& event) noexcept {
    OutcomeBinding binding{};
    if(gRole.load(std::memory_order_acquire)!=Role::Client||!LoadOutcome(source,victim,binding,true)||
        binding.transaction.command==0U)return false;
    event={};event.key=binding.transaction;event.actor=victimSide?binding.transactionActors.victim:binding.transactionActors.source;
    event.cycle=cycle;event.cause=cause;
    return event.actor.character==(victimSide?victim:source)&&Read(cycle+0x19U,event.serial);
}
void QueuePairTransactionEvent(const PairTransactionEvent& event) noexcept {
    if(event.key.command==0U)return;
    const std::lock_guard lock(gPairEventMutex);
    if(gPairEventCount==gPairTransactionEvents.size()){gPairEventOverflow.store(true,std::memory_order_release);return;}
    gPairTransactionEvents[(gPairEventRead+gPairEventCount++)%gPairTransactionEvents.size()]=event;
}
void PublishPairOutcomeEvidence(const NativeVictimSubmission& submission) noexcept {
    if(!submission.authoritativeClient||submission.transaction.command==0U)return;
    // Keep entry identity even if native execution destroyed its victim or
    // a later publication replaced the live outcome binding.
    PairTransactionEvent event{};event.key=submission.transaction;event.outcome=submission.outcome;
    event.actor=submission.transactionSource;event.cycle=submission.sourceCycle;event.serial=submission.sourceSerial;
    event.evidence=(submission.preparationApplied?interactions::PreparationApplied:0U)|
        (submission.reactionApplied?interactions::ReactionApplied:0U)|(submission.nativeFinish?interactions::NativeDeathApplied:0U);
    if(event.evidence)QueuePairTransactionEvent(event);
}
void RecordOutcome(NativeVictimSubmission& submission) noexcept {
    if (submission.authoritativeClient || submission.slot == kActors ||
        (submission.outcome.flags & OutcomePrepared) == 0U) return;
    float sourcePosition[3]{};
    float sourceYaw{};
    bool pose = submission.source != 0U && Read(submission.source + kYawOffset, sourceYaw);
    for (std::size_t i = 0U; i < 3U; ++i)
        pose = Read(submission.source + kPositionOffset + i * 4U, sourcePosition[i]) &&
            FiniteCoordinate(sourcePosition[i]) && pose;
    std::lock_guard lock(gOutcomeMutex);
    auto& binding = gOutcomes[submission.slot];
    if (binding.state.interactionPairRevision != submission.pairRevision ||
        binding.serial != submission.sourceSerial || binding.source.epoch != submission.victimBefore.epoch ||
        binding.victim.character != submission.victimBefore.character ||
        binding.victim.runtimeId != submission.victimBefore.runtimeId ||
        binding.victim.peer != submission.victimBefore.peer) return;
    if (binding.state.outcome.revision == 0U && pose && std::isfinite(sourceYaw) && std::abs(sourceYaw) <= 3.3F) {
        // Retain the admission pose even if the first engine publication comes
        // after a withdrawal. Later world/vent snapshots must follow the result.
        for (std::size_t i = 0U; i < 3U; ++i) binding.state.position[i] = sourcePosition[i];
        binding.state.yaw = sourceYaw; binding.state.stateFlags |= PoseValid;
    }
    submission.outcome.flags |= binding.state.outcome.flags & OutcomeNativeDeath;
    submission.outcome.revision = binding.state.outcome.revision + 1U;
    binding.state.outcome = submission.outcome;
    gOutcomeCaptured.fetch_add(1U, std::memory_order_relaxed);
}
void CaptureOutcome(Actor& actor) noexcept {
    const auto slot = static_cast<std::size_t>(&actor - gActors.data());
    std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto& binding = gOutcomes[slot];
    if (binding.source.character == actor.character && binding.source.runtimeId == actor.runtimeId &&
        binding.source.epoch == gPairBindingEpoch.load(std::memory_order_acquire) &&
        binding.state.birth == actor.birth &&
        binding.state.interactionPairRevision == actor.desired.interactionPairRevision &&
        binding.state.outcome.revision > actor.desired.outcome.revision)
        actor.desired.outcome = binding.state.outcome;
}
void ObserveVictimOutcome(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uint8_t serial, NativeOutcome& outcome) noexcept {
    std::uint16_t hp{};
    if (!Read(victim + 0x33CU, hp) || !Read(victim + 0x32CU, outcome.healthReaction)) return;
    outcome.hpReaction = hp;
    outcome.flags &= ~OutcomeTerminal;
    if (hp == 0U && outcome.healthReaction == 4U) outcome.flags |= OutcomeTerminal;
    std::uintptr_t cycle{}, table{}, linkedSource{}, linkedCycle{}, current{};
    std::uint8_t state{}, acquired{}, ended{}, flags{}, linkedSerial{}, interactive{};
    if (Read(victim + 0x36CU, cycle) && Read(cycle, table) && table == gBase + 0x00EAE690U &&
        Read(cycle + 0x1AU, state) && state == 0x4FU && Read(cycle + 0xD9U, acquired) && acquired != 0U &&
        Read(cycle + 0x68U, linkedSource) && linkedSource == source &&
        Read(cycle + 0xB0U, linkedCycle) && Read(source + 0x36CU, current) && current == linkedCycle &&
        Read(linkedCycle + 0x19U, linkedSerial) && linkedSerial == serial) {
        outcome.flags |= OutcomeVictimAdmitted;
        if (outcome.kind != 0U && Read(cycle + 0xDAU, interactive) && interactive == 0U)
            outcome.flags |= OutcomeNativeDeath;
        if (Read(cycle + 0xD8U, ended) && Read(cycle + 8U, flags) && (ended != 0U || (flags & 6U) != 0U))
            outcome.flags |= OutcomeVictimEnded;
    }
}
bool PairSourceBusy(const std::uintptr_t character) noexcept {
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gPairSources[i].character.load(std::memory_order_acquire) == character &&
            gPairSourceBusy[i].load(std::memory_order_acquire)) return true;
    return false;
}
std::uintptr_t ResolvePairedParticipant(void* resources, const std::uint32_t* identity,
    const bool animated) noexcept {
    const auto resolver = reinterpret_cast<SenseCharacterResolver>(
        gBase + (animated ? 0x002545C0U : 0x002544E0U));
    const auto retail = resolver(resources, identity);
    const auto role = gRole.load(std::memory_order_acquire);
    if (gPairedResolveOwner == 0U || identity != gPairedResolveIdentity ||
        (role != Role::Host && role != Role::Client) || !PairGenerationStable()) return retail;
    const auto address = reinterpret_cast<std::uintptr_t>(identity);
    std::uint32_t entityA{}, entityB{};
    if (!Read(address, entityA) || !Read(address + 4U, entityB) ||
        (entityA == 0U && entityB == 0U)) return retail;
    PairIdentity target{};
    for (std::size_t i = 1U; i < gPairParticipants.size(); ++i) {
        PairIdentity candidate{};
        if (!LoadPairBinding(gPairParticipants[i], candidate) || candidate.peer == 0U ||
            candidate.character == gPairedResolveOwner || candidate.entityA != entityA ||
            candidate.entityB != entityB || !RegisteredPlayerRepresentation(candidate.character) ||
            !PairIdentityLive(candidate) || !LivingCharacter(candidate.character)) continue;
        if (target.character != 0U &&
            (target.character != candidate.character || target.peer != candidate.peer)) {
            gParticipantPairAmbiguous.fetch_add(1U, std::memory_order_relaxed);
            return retail;
        }
        target = candidate;
    }
    if (target.character == 0U) return retail;
    gParticipantPairLookups.fetch_add(1U, std::memory_order_relaxed);
    gLastParticipantPairPeer.store(target.peer, std::memory_order_relaxed);
    // Only the two common-initializer CALLs; fill a missing native registration.
    if (retail != 0U) return retail;
    gParticipantPairRetailMissing.fetch_add(1U, std::memory_order_relaxed);
    const auto mapped = animated ? target.animated : target.character;
    if (mapped == 0U) return retail;
    gParticipantPairRemaps.fetch_add(1U, std::memory_order_relaxed);
    return mapped;
}
std::uintptr_t __fastcall PairedCharacterResolverDetour(void* resources, void*,
    const std::uint32_t* identity) noexcept {
    return ResolvePairedParticipant(resources, identity, false);
}
std::uintptr_t __fastcall PairedAnimatedResolverDetour(void* resources, void*,
    const std::uint32_t* identity) noexcept {
    return ResolvePairedParticipant(resources, identity, true);
}
bool ParticipantPairBusy(const std::uintptr_t target, bool& busy) noexcept {
    std::uintptr_t cycle{}, owner{};
    std::uint8_t state{};
    if (!Read(target + 0x36CU, cycle) || cycle == 0U ||
        !Read(cycle + 4U, owner) || owner != target || !Read(cycle + 0x1AU, state)) return false;
    busy = state == 0x42U || state == 0x4EU;
    return true;
}
bool EligibilityParticipant(void* logical, const std::uintptr_t owner,
    std::uintptr_t& target) noexcept {
    std::uint32_t type{};
    const auto address = reinterpret_cast<std::uintptr_t>(logical);
    return gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        owner != 0U && Eligible(owner, type) && Read(address, target) && target != owner &&
        address == target + 0x280U && RegisteredPlayerRepresentation(target) &&
        PeerForParticipant(target) != 0U;
}
std::uint32_t AdaptParticipantEligibility(void* logical, const std::uintptr_t owner,
    const std::uintptr_t expectedTarget, std::atomic<std::uint64_t>& checks,
    std::atomic<std::uint64_t>& overrides) noexcept {
    // The original predicate stays intact, including its full-EAX return.
    // Zero is the accepted value at these TWO callers, not a global meaning.
    const auto retail = reinterpret_cast<TargetEligibility>(gBase + 0x0038B380U)(logical);
    std::uintptr_t target{};
    if (!EligibilityParticipant(logical, owner, target) ||
        (expectedTarget != 0U && target != expectedTarget)) return retail;
    checks.fetch_add(1U, std::memory_order_relaxed);
    if (IsPvPTechnicalParticipant(target)) return 1U;
    // A dead proxy can still pass the physical execution predicate. Preserve
    // an already running native death pair, then exclude it from new choices.
    if (NativeVictimFlowRunning(owner, target)) return 0U;
    if (TerminalDeadCharacter(target)) {
        gPairedDeadTargetRejected.fetch_add(1U, std::memory_order_relaxed);
        return 1U;
    }
    bool paired{};
    if (retail == 0U || !LivingCharacter(target) ||
        !ParticipantPairBusy(target, paired) || paired) return retail;
    // Only perception target eligibility is adapted. Never edit Character
    // execution fields, wake its proxy brain, or override sense/target choice.
    overrides.fetch_add(1U, std::memory_order_relaxed);
    return 0U;
}
std::uint32_t __fastcall CandidateEligibilityDetour(void* logical, void*) noexcept {
    return AdaptParticipantEligibility(logical, gCandidateEligibilityOwner, 0U,
        gCandidateEligibilityChecks, gCandidateEligibilityOverrides);
}
std::uint32_t __fastcall RecordEligibilityDetour(void* logical, void*) noexcept {
    return AdaptParticipantEligibility(logical, gRecordEligibilityOwner, gRecordEligibilityTarget,
        gRecordEligibilityChecks, gRecordEligibilityOverrides);
}
// PvP left-click probe on retail ActionMeleeAttack start 00717D90.
// It is the node vtable +34 Start slot that 006D0260 calls with ECX=node and
// FIVE stack words (owner, execution, tree, previousFailure, previousSuccess);
// every exit is RET 14h and the body reads the tree argument. A two-argument
// RET 8 detour left 12 bytes on 006D0260's stack, so it popped EDI=tree,
// ESI=0 and returned to EIP=0 the first time any Host Alien started a melee
// attack. Refusal writes execution+4 = 8.
// Observation only, for the Alien whose melee is armed; may run on workers.
std::atomic<std::uintptr_t> gPvPMeleeWatch{};
// The leased Alien whose left-click melee is armed. Only its retail decision
// slice (manager branch, prepass, coordinator) leaves the PvP brain fence for
// the armed window; capture, publication and pilot input stay fenced. 460
// showed the fenced engine-thread updates kept availability at 0 and the BT
// never started ActionMeleeAttack; only worker-thread updates (458) could.
std::atomic<std::uintptr_t> gPvPMeleeArmedCharacter{};
// 462: a round ended while the kill was Started, the exemption was never
// cleared and the next level's Alien reused the same address: its pilot-driven
// tree then ran unfenced on the engine thread and crashed in 00712D70. The
// exemption is therefore bounded by a deadline and cleared on lease loss.
std::atomic<std::uint64_t> gPvPMeleeArmedUntil{};
void ArmPvPBrain(const std::uintptr_t character, const std::uint64_t until) noexcept {
    gPvPMeleeArmedUntil.store(until, std::memory_order_relaxed);
    gPvPMeleeArmedCharacter.store(character, std::memory_order_release);
}
void DisarmPvPBrain() noexcept {
    gPvPMeleeArmedCharacter.store(0U, std::memory_order_release);
    gPvPMeleeArmedUntil.store(0U, std::memory_order_relaxed);
}
bool PvPBrainArmed(const std::uintptr_t character) noexcept {
    if (character == 0U || character != gPvPMeleeArmedCharacter.load(std::memory_order_acquire)) return false;
    if (GetTickCount64() <= gPvPMeleeArmedUntil.load(std::memory_order_relaxed)) return true;
    DisarmPvPBrain();
    return false;
}
std::atomic<std::uint32_t> gPvPArmedBrainFaults{};
std::atomic<std::uint32_t> gPvPMeleeStarts{},gPvPMeleeRefused{},gPvPMeleeLastType{};
void* gPvPMeleeStartTrampoline{};
std::atomic<void*> gPvPMeleeStartOriginal{};
void __fastcall PvPMeleeStartDetour(void* node, void*, std::uintptr_t character, std::uintptr_t state,
    std::uintptr_t tree, std::uint32_t previousFailure, std::uint32_t previousSuccess) noexcept {
    using MeleeStart = void(__thiscall*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t,
        std::uint32_t, std::uint32_t);
    const auto original = reinterpret_cast<MeleeStart>(gPvPMeleeStartOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    original(node, character, state, tree, previousFailure, previousSuccess);
    if (character == 0U || character != gPvPMeleeWatch.load(std::memory_order_relaxed)) return;
    gPvPMeleeStarts.fetch_add(1U, std::memory_order_relaxed);
    std::uint32_t result{}, type{};
    if (Read(state + 4U, result) && result == 8U) gPvPMeleeRefused.fetch_add(1U, std::memory_order_relaxed);
    if (Read(reinterpret_cast<std::uintptr_t>(node) + 8U, type)) gPvPMeleeLastType.store(type, std::memory_order_relaxed);
}
std::uint8_t __fastcall RecordInvalidDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<RecordInvalid>(gRecordInvalidOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 1U;
    const auto record = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t owner{}, target{};
    std::uint32_t type{};
    const bool participant = gRole.load(std::memory_order_acquire) == Role::Host &&
        EngineThread() && Stable() && Read(record + 8U, owner) && Eligible(owner, type) &&
        Read(record + 0x42CU, target) && RegisteredPlayerRepresentation(target) &&
        PeerForParticipant(target) != 0U && target != owner;
    const auto previousOwner = gRecordEligibilityOwner;
    const auto previousTarget = gRecordEligibilityTarget;
    // Clear a nested non-participant scope instead of inheriting its caller.
    gRecordEligibilityOwner = participant ? owner : 0U;
    gRecordEligibilityTarget = participant ? target : 0U;
    auto invalid = original(context);
    if (participant && TerminalDeadCharacter(target) && !NativeVictimFlowRunning(owner, target)) {
        invalid = 1U;
        gPairedDeadTargetRejected.fetch_add(1U, std::memory_order_relaxed);
    }
    gRecordEligibilityOwner = previousOwner;
    gRecordEligibilityTarget = previousTarget;
    if (participant) {
        gParticipantRecordsValidated.fetch_add(1U, std::memory_order_relaxed);
        (invalid == 0U ? gParticipantRecordsRetained : gParticipantRecordsRejected)
            .fetch_add(1U, std::memory_order_relaxed);
    }
    // Lifetime and hostility checks still run in the original validator.
    return invalid;
}
bool RecordMatches(std::uintptr_t record, std::uintptr_t owner, std::uintptr_t target) noexcept;
bool AdmitParticipantCandidate(const std::uintptr_t senses, const std::uintptr_t target) noexcept {
    if (IsPvPTechnicalParticipant(target)) return false;
    // Mirror the ordinary Character candidate admission. The retail helper
    // owns allocation, insertion, Character lifetime and reference counting.
    // Its forced-perception flag (+493) is removed for a newly admitted record;
    // normal sight/hearing/occlusion still decide whether this human is sensed.
    __try {
        const auto owner = *reinterpret_cast<const std::uintptr_t*>(senses + 0x8CU);
        bool staleRecord{};
        auto node = *reinterpret_cast<const std::uintptr_t*>(senses + 0xCU);
        for (std::size_t i = 0U; node != 0U && i < 1024U; ++i) {
            const auto record = *reinterpret_cast<const std::uintptr_t*>(node);
            if (*reinterpret_cast<const std::uintptr_t*>(record + 0x42CU) == target) {
                if (!RecordMatches(record, owner, target)) {
                    staleRecord = true;
                    node = *reinterpret_cast<const std::uintptr_t*>(node + 8U);
                    continue;
                }
                *reinterpret_cast<std::uint8_t*>(record + 0x490U) = 1U;
                return true;
            }
            node = *reinterpret_cast<const std::uintptr_t*>(node + 8U);
        }
        if (node != 0U) return false;
        // Ensure looks up only the address. Do not reactivate its stale record
        // after Character address reuse; let retail lifetime validation retire
        // it and ordinary candidate admission create the new incarnation.
        if (staleRecord) return false;
        std::uintptr_t record{};
        using Ensure = std::uintptr_t*(__thiscall*)(void*, std::uintptr_t*, std::uintptr_t);
        reinterpret_cast<Ensure>(gBase + 0x0032AE00U)(
            reinterpret_cast<void*>(senses), &record, target);
        if (record == 0U) return false;
        *reinterpret_cast<std::uint8_t*>(record + 0x493U) = 0U;
        auto* refs = reinterpret_cast<std::int32_t*>(record + 0x14U);
        if (--*refs == 0) {
            const auto wrapper = record + 0x10U;
            const auto table = *reinterpret_cast<const std::uintptr_t*>(wrapper);
            reinterpret_cast<void(__thiscall*)(void*)>(
                *reinterpret_cast<const std::uintptr_t*>(table + 4U))(
                    reinterpret_cast<void*>(wrapper));
        }
        gParticipantCandidates.fetch_add(1U, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}
void __fastcall SensesCandidatesDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<SensesCandidates>(gCandidatesOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto senses = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t owner{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        Read(senses + 0x8CU, owner) && Eligible(owner, type)) {
        gParticipantCandidatePasses.fetch_add(1U, std::memory_order_relaxed);
        for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i) {
            const auto target = ParticipantForPeer(gCurrent.participants[i].peer);
            if (target == 0U || target == owner || !LivingCharacter(target)) continue;
            if ((HostilityDetour(reinterpret_cast<void*>(owner + 0x280U), nullptr, target) & 0xFFU) == 0U)
                continue;
            if (!AdmitParticipantCandidate(senses, target))
                gParticipantTargetRejected.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    const auto previousQueryOwner = gAiPlayerQueryOwner;
    const auto previousCandidateOwner = gCandidateEligibilityOwner;
    const bool hostOwner = gRole.load(std::memory_order_acquire) == Role::Host &&
        EngineThread() && Stable() && owner != 0U && Eligible(owner, type);
    gCandidateEligibilityOwner = hostOwner ? owner : 0U;
    if (hostOwner) gAiPlayerQueryOwner = owner;
    {
        AiPlayerSlotScope playerContext(hostOwner ? owner : 0U);
        original(context);
    }
    gCandidateEligibilityOwner = previousCandidateOwner;
    gAiPlayerQueryOwner = previousQueryOwner;
}
bool SuspendExistingTree(Actor& actor) noexcept {
    __try {
        // Retail cleanup, not memset of AI state. Order: selection -> pending
        // -> active; the promotion helpers then swap EMPTY vector roles.
        for (const auto offset : {0x44U, 0x48U, 0x4CU}) {
            const auto header = *reinterpret_cast<const std::uintptr_t*>(actor.tree + offset);
            const auto begin = *reinterpret_cast<const std::uintptr_t*>(header);
            const auto end = *reinterpret_cast<const std::uintptr_t*>(header + 4U);
            if (end < begin || (end - begin) % 4U != 0U || (end - begin) / 4U > 64U) return false;
        }
        using Clear = void(__thiscall*)(void*);
        reinterpret_cast<Clear>(gBase + 0x002FE080U)(reinterpret_cast<void*>(actor.tree));
        reinterpret_cast<Clear>(gBase + 0x002FE5C0U)(reinterpret_cast<void*>(actor.tree));
        reinterpret_cast<Clear>(gBase + 0x002FE690U)(reinterpret_cast<void*>(actor.tree));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

std::uint32_t HashActionWord(std::uint32_t hash, const std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= 16777619U;
    }
    return hash;
}

bool TraversalActionType(const std::uint32_t typeRva) noexcept {
    // Certified Alien traversal/backstage leaf vtables from the current retail build.
    // These are only lifecycle hints: the network never serializes Alien-only state.
    switch (typeRva) {
    case 0x00DBC498U: // MOVE_TO_BACKSTAGE_VIA_VENT_CLOSEST_TO_ALIEN
    case 0x00DBC8E0U: // MOVE_TO_FRONTSTAGE_VIA_FLANKED_VENT_CLOSEST_TO_PLAYER
    case 0x00EBC8B8U: // BACKSTAGE_AREA_SWEEP
    case 0x00ECA338U: // BACKSTAGE_ALIEN_RESPONSE
        return true;
    default:
        return false;
    }
}

std::uint32_t SemanticLifecycleFlags(const Actor& actor) noexcept {
    std::uint32_t flags{};
    if (actor.hostDormant) flags |= LifecycleHostDormant;
    if (actor.backstageKnown && actor.backstage) flags |= LifecycleCharacterBackstage;
    if (actor.withdrawKnown && actor.withdrawState != 0U) {
        flags |= LifecycleWithdrawn;
        if (actor.withdrawState == 2U) flags |= LifecycleWithdrawState2;
    }
    return flags;
}

void RefreshLifecycleFlags(Actor& actor, const std::uint32_t eventFlags) noexcept {
    actor.desired.lifecycleFlags = (eventFlags &
        (LifecycleSpatialDiscontinuity | LifecycleTraversalAction)) |
        SemanticLifecycleFlags(actor);
}

void MarkLifecycle(Actor& actor, const std::uint32_t flags) noexcept {
    // A native relocation must reach the Clients before the commands that
    // follow it: publish now instead of waiting for the periodic snapshot.
    actor.lastSend = 0U;
    ++actor.desired.lifecycleRevision;
    if (actor.desired.lifecycleRevision == 0U) actor.desired.lifecycleRevision = 1U;
    RefreshLifecycleFlags(actor, flags);
    gLifecycleCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastLifecycleFlags.store(actor.desired.lifecycleFlags, std::memory_order_relaxed);
}

void CaptureCharacterBackstage(Actor& actor) noexcept {
    std::uint8_t hidden{};
    if (!Read(actor.character + 0x2DAU, hidden)) return;
    const bool backstage = hidden != 0U;
    gBackstageCaptured.fetch_add(1U, std::memory_order_relaxed);
    const bool changed = actor.backstageKnown && actor.backstage != backstage;
    const bool initialHidden = !actor.backstageKnown && backstage;
    actor.backstageKnown = true; actor.backstage = backstage;
    if (changed || initialHidden) {
        gBackstageTransitions.fetch_add(1U, std::memory_order_relaxed);
        MarkLifecycle(actor, LifecycleSpatialDiscontinuity);
    } else RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
}

// Match retail's backstage restore: ECX Character, one byte stack argument,
// RET 4, followed on re-entry by animated Character vfunc+1C / frontstage reset.
// The native entries update presentation/collision ownership themselves.
bool SetCharacterBackstage(const std::uintptr_t character, const bool backstage) noexcept {
    if (!EngineThread() || !Stable() || character == 0U ||
        (!Client() && !(gPvPLease.owned && gPvPLease.ticket.character==character &&
            gPvPLease.ownerPeer!=gCurrent.localPeer && PvPLeaseScopeCurrent() && PvPLeaseLiveIdentity()))) return false;
    __try {
        using Set = void(__thiscall*)(void*, std::uint8_t);
        reinterpret_cast<Set>(gBase + kCharacterBackstageRva)(
            reinterpret_cast<void*>(character), backstage ? 1U : 0U);
        if (!backstage) {
            std::uintptr_t table{}, getter{};
            if (!Read(character, table) || !Read(table + 0x1CU, getter) ||
                getter < gBase + 0x1000U || getter >= gBase + 0x00D9E13BU) return false;
            const auto animated = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(
                reinterpret_cast<void*>(character));
            if (animated == 0U) return false;
            reinterpret_cast<void(__thiscall*)(void*)>(gBase + kCharacterFrontstageRva)(
                reinterpret_cast<void*>(animated));
        }
        std::uint8_t actual{};
        return Read(character + 0x2DAU, actual) && (actual != 0U) == backstage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

bool ApplyAuthoritativeBackstage(Actor& actor, const State& state) noexcept {
    const bool backstage = (state.lifecycleFlags & LifecycleCharacterBackstage) != 0U;
    // Preserve the withdraw controller's own presentation while Host dormancy
    // has no live Character sample. A live frontstage snapshot repairs it.
    if (!backstage && (state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant)) != 0U)
        return true;
    std::uint8_t actual{};
    if (!Read(actor.character + 0x2DAU, actual)) return false;
    if ((actual != 0U) == backstage) return true;
    if (!SetCharacterBackstage(actor.character, backstage)) {
        gBackstageRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    actor.completedRevision = 0U; actor.lastProxyRefresh = 0U;
    actor.placementPending = !backstage;
    gBackstageApplied.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool ResolveAlienWithdrawContext(const std::uintptr_t character,
    std::uintptr_t& context, std::uint32_t& state) noexcept {
    context = 0U;
    state = 0U;
    std::uintptr_t alienData{};
    if (character == 0U || !Read(character + 0x34F0U, alienData) || alienData == 0U ||
        !Read(alienData + 0x18U, context) || context == 0U || !Read(context, state) || state > 2U)
        return false;
    return true;
}

bool SuppressedAlien(const std::uintptr_t character) noexcept {
    if (!gAlienSuppressed.load(std::memory_order_acquire) ||
        gRole.load(std::memory_order_acquire) != Role::Host || gGenerations == nullptr ||
        !gGenerations->IsStableGeneration(gActiveNativeGeneration.load(std::memory_order_acquire))) return false;
    std::uintptr_t context{};
    std::uint32_t state{}, type{};
    // Template class 1 is the retail Alien; other NPC families keep their AI.
    return Eligible(character, type) && type == 1U &&
        ResolveAlienWithdrawContext(character, context, state);
}

void CaptureWithdrawState(Actor& actor) noexcept {
    std::uintptr_t context{};
    std::uint32_t state{};
    if (!ResolveAlienWithdrawContext(actor.character, context, state)) return;
    gWithdrawSamples.fetch_add(1U, std::memory_order_relaxed);
    gLastWithdrawState.store(state, std::memory_order_relaxed);
    if (!actor.withdrawKnown) {
        actor.withdrawKnown = true;
        actor.withdrawState = state;
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
        return;
    }
    if (actor.withdrawState == state) {
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
        return;
    }
    actor.withdrawState = state;
    gWithdrawTransitions.fetch_add(1U, std::memory_order_relaxed);
    MarkLifecycle(actor, 0U);
}

std::uint32_t DesiredWithdrawState(const State& state) noexcept {
    if ((state.lifecycleFlags & LifecycleWithdrawState2) != 0U) return 2U;
    if ((state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant)) != 0U) return 1U;
    return 0U;
}

bool ApplyAlienWithdrawEntry(const std::uintptr_t context,
    const std::uint32_t desiredState) noexcept {
    __try {
        using Reset = void(__fastcall*)(void*);
        using Set = void(__thiscall*)(void*, int);
        const auto reset = reinterpret_cast<Reset>(gBase + 0x003349D0U); // FUN_007349D0
        const auto set = reinterpret_cast<Set>(gBase + 0x0032FC20U);     // FUN_0072FC20
        if (desiredState == 0U) {
            set(reinterpret_cast<void*>(context), 0);
            reset(reinterpret_cast<void*>(context));
        } else {
            reset(reinterpret_cast<void*>(context));
            set(reinterpret_cast<void*>(context), static_cast<int>(desiredState));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ApplyAuthoritativeWithdraw(Actor& actor, const State& state) noexcept {
    std::uintptr_t context{};
    std::uint32_t current{};
    if (!ResolveAlienWithdrawContext(actor.character, context, current)) return true;
    const auto desired = DesiredWithdrawState(state);
    if (current == desired) return true;
    if (!ApplyAlienWithdrawEntry(context, desired)) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    gWithdrawApplied.fetch_add(1U, std::memory_order_relaxed);
    gLastWithdrawState.store(desired, std::memory_order_relaxed);
    return true;
}

bool ProxyWithdrawn(const State& state) noexcept {
    return (state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant | LifecycleCharacterBackstage)) != 0U;
}

bool ResolveRoleTaskExecutor(const std::uintptr_t character,
    std::uintptr_t& roleManager, std::uintptr_t& executor) noexcept {
    roleManager = 0U;
    executor = 0U;
    // Retail role/task ownership is not embedded directly at Character+424.
    // Character+34DC owns the role manager, whose +424 slot points at the
    // active role-task executor. This is the exact chain used by the retail
    // role/task queries around FUN_006E87B0 and FUN_0076F080.
    return character != 0U && Read(character + 0x34DCU, roleManager) && roleManager != 0U &&
        Read(roleManager + 0x424U, executor) && executor != 0U;
}

bool ClearRoleTaskExecutor(Actor& actor) noexcept {
    std::uintptr_t roleManager{}, executor{};
    if (!ResolveRoleTaskExecutor(actor.character, roleManager, executor)) return true;
    std::uintptr_t currentTask{};
    if (!Read(executor + 8U, currentTask) || currentTask == 0U) return true;
    __try {
        // FUN_00761A90 releases the admitted task and its movement requests.
        // Retail callers pass ECX plus a task-handle address on the stack;
        // the certified body ignores that argument but still ends in RET 4.
        // The decompiler's ECX-only __fastcall prototype drops it and corrupts
        // the wrapper's saved registers on return. Keep the full retail ABI.
        using Cleanup = void(__thiscall*)(void*, const void*);
        reinterpret_cast<Cleanup>(gBase + kRoleTaskCleanupRva)(
            reinterpret_cast<void*>(executor), reinterpret_cast<const void*>(executor + 8U));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

bool QuiesceProxyExecution(Actor& actor) noexcept {
    if (!UnownedCleanupAllowed(actor)) return false;
    if (actor.character == 0U) return false;
    combat::Cancel(actor.character);
    actor.convergenceActive = false; actor.convergenceDivergenceSince = 0U;
    actor.pursuitActive = false; actor.pursuitDivergenceSince = 0U;
    actor.replayedExecutionRevision = 0U;
    actor.completedExecutionRevision = 0U;
    actor.lastExecutionAttempt = 0U;
    actor.rejectedNavigationRevision = actor.rejectedNavigationRequestRevision = 0U;
    actor.lastNavigationAttempt = 0U;
    actor.poseCount = 0U; actor.poseRevision = 0U; actor.poseInterpolating = false;
    static_cast<void>(character_direct_step_runtime::Stop(actor.character));
    const bool tree = actor.tree == 0U || SuspendExistingTree(actor);
    const bool task = ClearRoleTaskExecutor(actor);
    if (task) actor.roleTaskCleanupPending = false;
    actor.completedRevision = 0U;
    actor.lastProxyRefresh = 0U;
    gExecutionBarriers.fetch_add(1U, std::memory_order_relaxed);
    return tree && task;
}

void ApplyAlienSuppression(Actor& actor) noexcept {
    std::uintptr_t context{};
    std::uint32_t state{};
    if (!ResolveAlienWithdrawContext(actor.character, context, state)) return;
    if (!actor.alienSuppressed) {
        actor.originalAlienWithdraw = state;
        static_cast<void>(QuiesceProxyExecution(actor));
        actor.alienSuppressed = true;
    }
    // Use the same retail withdrawal path as native backstage/replica state.
    if (state != 1U && !ApplyAlienWithdrawEntry(context, 1U)) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    CaptureWithdrawState(actor);
}

void RestoreAlienSuppression(Actor& actor) noexcept {
    if (!actor.alienSuppressed) return;
    std::uintptr_t context{};
    std::uint32_t state{};
    if (ResolveAlienWithdrawContext(actor.character, context, state))
        static_cast<void>(ApplyAlienWithdrawEntry(context, actor.originalAlienWithdraw));
    actor.alienSuppressed = false;
}

void RecordSpawnPlacement(const Actor& actor,const State& state,const bool confirmed) noexcept {
    SpawnPlacementProbe sample{};
    if(gRole.load(std::memory_order_acquire)==Role::Host) {
        if(!host_producer::HostScope(sample.scope))return;
    } else if(!execution_runtime::CurrentScope(sample.scope))return;
    sample.tick=GetTickCount64();sample.sessionCookie=gCurrent.sessionCookie;
    sample.role=static_cast<std::uint32_t>(gRole.load());sample.nativeGeneration=gCurrent.nativeGeneration;
    sample.worldGeneration=gCurrent.worldGeneration;sample.entityA=state.entityA;sample.entityB=state.entityB;
    sample.birth=state.birth;sample.revision=state.revision;sample.characterClass=state.characterClass;sample.hostYaw=state.yaw;sample.confirmed=confirmed;
    sample.readable=Read(actor.character+kYawOffset,sample.localYaw)&&std::isfinite(sample.localYaw);
    float error{};
    for(std::size_t i=0;i<3;++i) {
        sample.host[i]=state.position[i];
        sample.readable=Read(actor.character+kPositionOffset+i*sizeof(float),sample.local[i])&&
            std::isfinite(sample.local[i])&&sample.readable;
        const auto delta=sample.local[i]-sample.host[i];error+=delta*delta;
    }
    sample.positionError=std::sqrt(error);
    sample.yawError=std::abs(std::remainder(sample.localYaw-sample.hostYaw,6.283185307F));
    const std::lock_guard lock(gSpawnProbeGate);
    if(gSpawnProbeCount==gSpawnProbes.size()){++gSpawnProbeDropped;return;}
    gSpawnProbes[(gSpawnProbeRead+gSpawnProbeCount++)%gSpawnProbes.size()]=sample;
}

void CaptureRoleTask(Actor& actor) noexcept {
    std::uint32_t typeRva{}, keyA{}, keyB{};
    std::uintptr_t roleManager{}, executor{}, task{}, table{}, descriptor{};
    bool valid = ResolveRoleTaskExecutor(actor.character, roleManager, executor) &&
        Read(executor + 8U, task) && task != 0U && Read(task, table) &&
        table >= gBase && table - gBase <= 0xFFFFFFFFULL;
    if (valid) {
        typeRva = static_cast<std::uint32_t>(table - gBase);
        if (Read(task + 0x18U, descriptor) && descriptor != 0U) {
            static_cast<void>(Read(descriptor + 4U, keyA));
            static_cast<void>(Read(descriptor + 8U, keyB));
        }
    }
    const bool wasValid = (actor.desired.stateFlags & RoleTaskValid) != 0U;
    // Revision follows admission/replacement of the actual task object, not
    // mutable task context fields. Pointer identity is kept Host-local only.
    const bool changed = valid != wasValid || task != actor.lastRoleTask;
    if (changed) {
        ++actor.desired.roleTaskRevision;
        if (actor.desired.roleTaskRevision == 0U) actor.desired.roleTaskRevision = 1U;
        gRoleTaskTransitions.fetch_add(1U, std::memory_order_relaxed);
    }
    actor.lastRoleTask = valid ? task : 0U;
    actor.desired.roleTaskTypeRva = valid ? typeRva : 0U;
    actor.desired.roleTaskKeyA = valid ? keyA : 0U;
    actor.desired.roleTaskKeyB = valid ? keyB : 0U;
    if (valid) {
        actor.desired.stateFlags |= RoleTaskValid;
        gRoleTaskCaptured.fetch_add(1U, std::memory_order_relaxed);
        gLastRoleTaskTypeRva.store(typeRva, std::memory_order_relaxed);
        gLastRoleTaskKeyA.store(keyA, std::memory_order_relaxed);
        gLastRoleTaskKeyB.store(keyB, std::memory_order_relaxed);
    } else {
        actor.desired.stateFlags &= ~RoleTaskValid;
    }
}

void CaptureActiveAction(Actor& actor) noexcept {
    std::uint32_t typeRva{}, pathHash{}, status{}, depth{};
    std::uintptr_t header{}, begin{}, end{};
    if (Read(actor.tree + 0x44U, header) && Read(header, begin) && Read(header + 4U, end) &&
        end >= begin && (end - begin) % 4U == 0U && (end - begin) / 4U <= 64U) {
        depth = static_cast<std::uint32_t>((end - begin) / 4U);
        pathHash = 2166136261U;
        for (auto entry = begin; entry < end && depth != 0U; entry += 4U) {
            std::uintptr_t state{}, node{}, table{};
            std::uint8_t selectedChild{};
            if (!Read(entry, state) || !Read(state + 0x10U, node) || !Read(node, table) ||
                table < gBase || table - gBase > 0xFFFFFFFFULL ||
                !Read(state + 0x1CU, selectedChild)) {
                depth = 0U; break;
            }
            const auto rva = static_cast<std::uint32_t>(table - gBase);
            pathHash = HashActionWord(pathHash, rva);
            pathHash = HashActionWord(pathHash, selectedChild);
            if (entry + 4U == end) {
                typeRva = rva;
                if (!Read(state + 4U, status)) depth = 0U;
            }
        }
    }
    const bool valid = depth != 0U && typeRva != 0U && status <= 10U;
    const bool wasValid = (actor.desired.stateFlags & ActionValid) != 0U;
    const bool changed = valid != wasValid || actor.desired.actionTypeRva != typeRva ||
        actor.desired.actionPathHash != pathHash || actor.desired.actionStatus != status ||
        actor.desired.actionDepth != depth;
    if (changed) {
        ++actor.desired.actionRevision;
        if (actor.desired.actionRevision == 0U) actor.desired.actionRevision = 1U;
        gActionTransitions.fetch_add(1U, std::memory_order_relaxed);
    }
    const bool traversal = valid && TraversalActionType(typeRva);
    if (traversal != actor.traversalAction) {
        actor.traversalAction = traversal;
        MarkLifecycle(actor, traversal ? LifecycleTraversalAction : 0U);
    }
    actor.desired.actionTypeRva = valid ? typeRva : 0U;
    actor.desired.actionPathHash = valid ? pathHash : 0U;
    actor.desired.actionStatus = valid ? status : 0U;
    actor.desired.actionDepth = valid ? depth : 0U;
    if (valid) {
        actor.desired.stateFlags |= ActionValid;
        gActionCaptured.fetch_add(1U, std::memory_order_relaxed);
        gLastActionTypeRva.store(typeRva, std::memory_order_relaxed);
        gLastActionPathHash.store(pathHash, std::memory_order_relaxed);
        gLastActionStatus.store(status, std::memory_order_relaxed);
    } else {
        actor.desired.stateFlags &= ~ActionValid;
    }
}

bool CertifiedExecutionCycle(const std::uint32_t typeRva,
    const std::uint32_t state) noexcept {
    return FindExecutionTransition(typeRva, state) != nullptr || FindExecutionTransitionContract(typeRva, state) != nullptr;
}
bool HostExecutionScopeQualified(const transactions::Scope&,std::uint32_t) noexcept;
void CaptureExecutionCycle(Actor& actor) noexcept {
    std::uintptr_t cycle{}, owner{}, vtable{};
    std::uint8_t state{}, serial{};
    const bool valid = Read(actor.character + 0x36CU, cycle) && cycle != 0U &&
        Read(cycle + 4U, owner) && owner == actor.character && Read(cycle, vtable) &&
        vtable > gBase && vtable - gBase <= 0x02000000U && Read(cycle + 0x1AU, state) &&
        Read(cycle + 0x19U, serial);
    const auto typeRva = valid ? static_cast<std::uint32_t>(vtable - gBase) : 0U;
    const bool newCycle = valid && (!((actor.desired.stateFlags & ExecutionValid) != 0U) ||
        cycle != actor.capturedExecutionCycle || serial != actor.capturedExecutionSerial || actor.desired.executionCycleTypeRva != typeRva ||
        actor.desired.executionCycleState != state);
    actor.desired.scripted = {}; // Observed cycles never create executable inputs.
    actor.desired.transition = {};
    std::uint32_t flags{};
    if (valid) {
        const bool paired = state == 0x42U || state == 0x47U || state == 0x4EU || state == 0x4FU;
        const bool transition = state != 0x01U && state != 0x38U && !paired;
        if (paired) flags |= ExecutionPaired;
        if (transition) flags |= ExecutionTransition;
        if (transition && (CertifiedExecutionCycle(typeRva, state) || actor.desired.scripted.flags != 0U))
            flags |= ExecutionCertified;
    }
    const bool wasValid = (actor.desired.stateFlags & ExecutionValid) != 0U;
    const bool changed = newCycle || valid != wasValid || actor.desired.executionCycleTypeRva != typeRva ||
        actor.desired.executionCycleState != static_cast<std::uint32_t>(state) ||
        actor.desired.executionFlags != flags;
    if (changed) {
        actor.desired.executionAction={};
        ++actor.desired.executionRevision;
        actor.execCaptureTick = GetTickCount64();
        if (actor.desired.executionRevision == 0U) actor.desired.executionRevision = 1U;
        actor.lastSend = 0U;
        gExecutionTransitions.fetch_add(1U, std::memory_order_relaxed);
    }
    actor.desired.executionCycleTypeRva = typeRva;
    actor.desired.executionCycleState = valid ? static_cast<std::uint32_t>(state) : 0U;
    actor.desired.executionFlags = flags;
    actor.capturedExecutionSerial = serial;
    actor.capturedExecutionCycle = valid?cycle:0;
    if(newCycle||!valid) {
        actor.desired.executionAction={};actor.capturedExecutionScope={};
        actor.capturedExecutionScopeKnown=valid&&host_producer::HostScope(actor.capturedExecutionScope)&&
            HostExecutionScopeQualified(actor.capturedExecutionScope,gCurrent.nativeGeneration);
    }
    if (valid) {
        actor.desired.stateFlags |= ExecutionValid;
        gExecutionCaptured.fetch_add(1U, std::memory_order_relaxed);
        gLastExecutionTypeRva.store(typeRva, std::memory_order_relaxed);
        gLastExecutionState.store(static_cast<std::uint32_t>(state), std::memory_order_relaxed);
        gLastExecutionFlags.store(flags, std::memory_order_relaxed);
    } else {
        actor.desired.stateFlags &= ~ExecutionValid;
    }
    if(actor.capturedExecutionScopeKnown) {
        transactions::Scope current{};
        if(!host_producer::HostScope(current)||current!=actor.capturedExecutionScope||
            !HostExecutionScopeQualified(current,gCurrent.nativeGeneration)) {
            actor.capturedExecutionScopeKnown=false;actor.desired.executionAction={};actor.lastSend=0;
        }
    }
    if(Installed()&&valid&&(flags&(ExecutionTransition|ExecutionPaired))&&action_correlation::Empty(actor.desired.executionAction)) {
        transactions::Scope current{};action_correlation::Reference reference{};
        if(host_producer::HostScope(current)&&
            (!actor.capturedExecutionScopeKnown||current==actor.capturedExecutionScope)&&
            HostExecutionScopeQualified(current,gCurrent.nativeGeneration)&&
            host_producer::IdentifyNativeAction(actor.character,cycle,typeRva,state,serial,actor.desired.executionRevision,reference)&&reference.source==current) {
            actor.capturedExecutionScope=current;actor.capturedExecutionScopeKnown=true;
            actor.desired.executionAction=reference;actor.lastSend=0;++gActionReferencesCaptured;
        } else ++gActionReferencesUnbound;
    }
}


bool OrdinaryLocomotionCycle(const std::uint32_t type,const std::uint32_t state) noexcept {
    return HybridLocomotionCycleAllowed(type,static_cast<std::uint8_t>(state))||
        // A Turn is ordinary locomotion for diagnostic classification only.
        // Its actual request and native animation retain heading authority.
        (type==0xEAC040U&&state==0x39U);
}
void CapturePose(Actor& actor, ULONGLONG now) noexcept {
    // Physical observations bound the Client's native navigation goals. The
    // typed Host command still owns permission; no Character transform is set.
    now = PreciseTickMs();
    // Publish gates on the coarse tick (>=50 ms); a precise 47 ms gap must not
    // drop the sample and stretch that send period to 100 ms.
    if (actor.hasLastPosition && now - actor.lastPoseSample < kSendPeriod - 16ULL) return;
    float position[3]{}, yaw{};
    if (!Read(actor.character + kPositionOffset, position[0]) ||
        !Read(actor.character + kPositionOffset + 4U, position[1]) ||
        !Read(actor.character + kPositionOffset + 8U, position[2]) ||
        !Read(actor.character + kYawOffset, yaw) ||
        !FiniteCoordinate(position[0]) || !FiniteCoordinate(position[1]) ||
        !FiniteCoordinate(position[2]) || !std::isfinite(yaw) || std::abs(yaw) > 3.3F) {
        actor.desired.stateFlags &= ~PoseValid;
        actor.desired.motionTick=0;actor.desired.motionFlags=0;
        std::fill(std::begin(actor.desired.motionVelocity),std::end(actor.desired.motionVelocity),0.0F);
        gPoseRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    actor.desired.motionTick=now;actor.desired.motionFlags=1U;
    std::fill(std::begin(actor.desired.motionVelocity),std::end(actor.desired.motionVelocity),0.0F);
    if (actor.hasLastPosition && now > actor.lastPoseSample) {
        const auto elapsed = now - actor.lastPoseSample;
        const auto dx = position[0] - actor.lastPosition[0];
        const auto dy = position[1] - actor.lastPosition[1];
        const auto dz = position[2] - actor.lastPosition[2];
        const auto spatialDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (elapsed <= 500ULL && std::isfinite(spatialDistance) &&
            (spatialDistance >= kLifecycleDiscontinuityDistance ||
             std::abs(dy) >= kLifecycleVerticalDiscontinuity)) {
            actor.desired.motionFlags|=4U;
            MarkLifecycle(actor, LifecycleSpatialDiscontinuity |
                (actor.traversalAction ? LifecycleTraversalAction : 0U));
        }
        if(elapsed<=500ULL&&(actor.desired.motionFlags&4U)==0U) {
            const float scale=1000.0F/static_cast<float>(elapsed);
            for(std::size_t i=0;i<3;++i) {
                const float velocity=(position[i]-actor.lastPosition[i])*scale;
                actor.desired.motionVelocity[i]=std::isfinite(velocity)&&std::abs(velocity)<=128.0F?velocity:0.0F;
            }
        }
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        actor.lastPosition[i] = position[i]; actor.desired.position[i] = position[i];
    }
    actor.hasLastPosition = true; actor.lastPoseSample = now;

    if ((actor.desired.interactionFlags & InteractionCycleActive) != 0U) {
        const auto target = (actor.desired.interactionFlags & InteractionTargetIsParticipant) != 0U
            ? ParticipantForPeer(actor.desired.interactionTargetPeer)
            : FindCharacterByIdentity(actor.desired.interactionTargetA, actor.desired.interactionTargetB);
        float targetPosition[3]{};
        bool readable = target != 0U;
        for (std::size_t i = 0U; i < 3U; ++i) {
            const bool coordinate = target != 0U &&
                Read(target + kPositionOffset + i * sizeof(float), targetPosition[i]) &&
                FiniteCoordinate(targetPosition[i]);
            readable = readable && coordinate;
        }
        if (readable)
            for (std::size_t i = 0U; i < 3U; ++i) actor.desired.interactionTargetPosition[i] = targetPosition[i];
    }
    actor.desired.yaw = yaw;
    actor.desired.stateFlags |= PoseValid;

    std::uintptr_t cycle{};
    std::uintptr_t cycleType{},movement{};std::uint8_t cycleState{};std::uint32_t gait{};
    if(!Read(actor.character+0x36CU,cycle)||!cycle||!Read(cycle,cycleType)||cycleType<gBase||
        !Read(cycle+0x1AU,cycleState)||!OrdinaryLocomotionCycle(static_cast<std::uint32_t>(cycleType-gBase),cycleState)||
        (actor.desired.executionFlags&ExecutionPaired)!=0U)actor.desired.motionFlags|=2U;
    // Direct locomotion: replicate the gait the Host actually PLAYS (+198).
    // The Client hook (0x5B874) overwrites +194 and +198 after retail's own
    // selection, so nothing brakes twice. Sending the input (+194) made the
    // 511 Client run gait 3 while the Host played 2 (diag 174301, t=4-6 s).
    if(Read(actor.character+0x42A8U,movement)&&movement&&Read(movement+0x198U,gait)&&gait<=3U)
        actor.desired.moveType=gait;
    const float speed=std::hypot(actor.desired.motionVelocity[0],actor.desired.motionVelocity[2]);
    actor.desired.velocityX=actor.desired.motionVelocity[0];actor.desired.velocityZ=actor.desired.motionVelocity[2];
    actor.desired.locomotionIntensity=speed>0.02F?1.0F:0.0F;
    if(speed>0.02F)actor.desired.stateFlags|=Moving;else actor.desired.stateFlags&=~Moving;
    ++actor.desired.locomotionRevision;if(!actor.desired.locomotionRevision)++actor.desired.locomotionRevision;
    gPoseCaptured.fetch_add(1U, std::memory_order_relaxed);
}

Actor* Find(const std::uintptr_t c) noexcept {
    for (auto& actor : gActors) if (actor.character == c) return &actor;
    return nullptr;
}






std::uint8_t __fastcall PairedInitializeDetour(void* context, void*,
    const std::uint32_t* identity, const std::int32_t variant) noexcept {
    const auto original = reinterpret_cast<PairedInitialize>(
        gPairedInitializeOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    const auto role = gRole.load(std::memory_order_acquire);
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t owner{}, ownedCycle{};
    std::uint8_t state{};
    PairIdentity binding{};
    if ((role != Role::Host && role != Role::Client) || !PairGenerationStable() ||
        !Read(cycle + 4U, owner) || PublishedPairSource(owner, binding) == kActors ||
        !Read(owner + 0x36CU, ownedCycle) || ownedCycle != cycle ||
        !Read(cycle + 0x1AU, state) || (state != 0x42U && state != 0x4EU))
        return original(context, identity, variant);
    if (identity != reinterpret_cast<const std::uint32_t*>(
            cycle + (state == 0x4EU ? 0x100U : 0xD8U)))
        return original(context, identity, variant);
    const auto previousOwner = gPairedResolveOwner;
    const auto* previousIdentity = gPairedResolveIdentity;
    gPairedResolveOwner = owner;
    gPairedResolveIdentity = identity;
    std::uint8_t result{};
    __try { result = original(context, identity, variant); }
    __finally {
        gPairedResolveOwner = previousOwner;
        gPairedResolveIdentity = previousIdentity;
    }
    if (result != 0U) gPairedInitializeSucceeded.fetch_add(1U, std::memory_order_relaxed);
    else gPairedInitializeFailed.fetch_add(1U, std::memory_order_relaxed);
    return result;
}
bool ManagedPairedVictim(const std::uintptr_t victim, const std::uintptr_t source) noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    if (role != Role::Host && role != Role::Client && role != Role::FrozenClient) return false;
    // A remote participant stays protected even if its queued source has been
    // destroyed/reused or its source metadata is temporarily unavailable.
    if (RegisteredPlayerRepresentation(victim)) return true;
    PairIdentity participant{};
    std::uint32_t type{};
    if (!PublishedPairVictim(victim, participant)) return false;
    if (Eligible(source, type)) return true;
    for (const auto& slot : gPairSources)
        if (slot.character.load(std::memory_order_acquire) == source && source != 0U) return true;
    return false;
}
// Diagnostic reasons: 1=bindings, 2=source layout/lifetime, 3=target identity,
// 4=animation definition, 5=inactive source, 6=victim already dead.
bool RejectPairedVictim(const std::uint32_t state, const std::uint32_t reason) noexcept {
    gLastRejectedPairedSourceState.store(state, std::memory_order_relaxed);
    gLastRejectedPairedReason.store(reason, std::memory_order_relaxed);
    return false;
}
bool ValidPairedVictimLink(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle, std::uint32_t& rejectedState) noexcept {
    rejectedState = 0xFFFFFFFFU;
    std::uintptr_t current{}, owner{}, table{}, target{}, definition{}, getter{};
    std::uint8_t state{};
    std::uint32_t targetA{}, targetB{}, targetBirth{}, definitionKind{}, definitionIndex{};
    PairIdentity sourceIdentity{}, victimIdentity{};
    if (Read(sourceCycle + 0x1AU, state)) rejectedState = state;
    if (!PairGenerationStable() || PublishedPairSource(source, sourceIdentity) == kActors ||
        !PublishedPairVictim(victim, victimIdentity))
        return RejectPairedVictim(rejectedState, 1U);
    // Source addresses persist across Move/Idle/impact and new paired cycles.
    // Neither address equality nor the 4E type alone proves a usable source.
    if (state != 0x4EU || !Read(source + 0x36CU, current) || current != sourceCycle ||
        !Read(sourceCycle + 4U, owner) || owner != source ||
        !Read(sourceCycle, table) || table != gBase + 0x00EAE5D0U)
        return RejectPairedVictim(rejectedState, 2U);
    if (!Read(sourceCycle + 0x44U, target) || target != victim ||
        !Read(sourceCycle + 0x48U, targetBirth) || targetBirth != victimIdentity.runtimeId ||
        !Read(sourceCycle + 0x100U, targetA) || targetA != victimIdentity.entityA ||
        !Read(sourceCycle + 0x104U, targetB) || targetB != victimIdentity.entityB)
        return RejectPairedVictim(rejectedState, 3U);
    // 00675FAD reads animation+8; 0064E6DF reads animation+4. A still-typed
    // source can have lost its definition after an interrupted/finished pair.
    // Validate the retail interface without choosing a clip or animation kind.
    if (!Read(sourceCycle + 0x4CU, definition) || definition == 0U ||
        !Read(definition, table) || !Read(table + 4U, getter) ||
        getter < gBase + 0x1000U || getter >= gBase + 0x00D9F000U ||
        !Read(definition + 4U, definitionIndex) || !Read(definition + 8U, definitionKind))
        return RejectPairedVictim(rejectedState, 4U);
    return true;
}
bool VictimHealthAllowsAdmission(const std::uintptr_t victim) noexcept {
    std::uint16_t hp{};
    std::uint32_t health{};
    std::uint8_t enabled{};
    const bool readable = Read(victim + 0x33CU, hp) && Read(victim + 0x32CU, health);
    static_cast<void>(Read(victim + 0x2FAU, enabled));
    gLastVictimAdmissionHp.store(hp, std::memory_order_relaxed);
    gLastVictimAdmissionHealth.store(health, std::memory_order_relaxed);
    gLastVictimAdmissionEnabled.store(enabled, std::memory_order_relaxed);
    // 2FA is not a victim admission contract: the paired flow takes control of
    // a player while preparing its graph. Keep LivingCharacter for perception.
    return readable && hp != 0U && health < 3U;
}
bool PreparedVictimHealthContinuation(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle) noexcept {
    const auto* submission = gNativeVictimSubmission;
    std::uint8_t serial{};
    return submission != nullptr && submission->livingBefore && submission->source == source &&
        submission->sourceCycle == sourceCycle && submission->victimBefore.character == victim &&
        PairIdentityLive(submission->victimBefore) &&
        Read(sourceCycle + 0x19U, serial) && serial == submission->sourceSerial;
}
bool ValidPairedVictimSource(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle, std::uint32_t& rejectedState) noexcept {
    if (!ValidPairedVictimLink(victim, source, sourceCycle, rejectedState)) return false;
    std::uintptr_t current{};
    std::uint8_t state{};
    bool active{};
    if (!ReadPairedCycleActivity(source, current, state, active) || !active)
        return RejectPairedVictim(rejectedState, 5U);
    if (!VictimHealthAllowsAdmission(victim)) {
        // The certified producer can inflict native damage BEFORE message 17.
        // Continue only its exact same originally living victim/cycle; never
        // admit a stale event or a new attack against a pre-existing corpse.
        if (!PreparedVictimHealthContinuation(victim, source, sourceCycle))
            return RejectPairedVictim(rejectedState, 6U);
        gPairedVictimHealthContinuations.fetch_add(1U, std::memory_order_relaxed);
    }
    return true;
}
bool LocalClientVictim(const std::uintptr_t victim) noexcept {
    PairIdentity local{};
    return gRole.load(std::memory_order_acquire) == Role::Client &&
        LoadPairBinding(gPairParticipants[0], local) && local.character == victim &&
        local.peer != 0U && PairIdentityLive(local);
}
bool NativeVictimFlowRunning(const std::uintptr_t source, const std::uintptr_t victim) noexcept {
    PairIdentity participant{}, sourceIdentity{};
    std::uintptr_t sourceCycle{}, victimCycle{}, owner{}, table{}, linked{}, linkedSource{};
    std::uint32_t cachedBirth{}, rejectedState{};
    std::uint8_t state{}, acquired{}, ended{}, flags{};
    if (!PublishedPairVictim(victim, participant) ||
        PublishedPairSource(source, sourceIdentity) == kActors ||
        !Read(source + 0x36CU, sourceCycle) ||
        !Read(victim + 0x36CU, victimCycle) || victimCycle == 0U ||
        !Read(victimCycle + 4U, owner) || owner != victim ||
        !Read(victimCycle, table) || table != gBase + 0x00EAE690U ||
        !Read(victimCycle + 0x1AU, state) || state != 0x4FU ||
        !Read(victimCycle + 8U, flags) || (flags & 6U) != 0U ||
        !Read(victimCycle + 0xD9U, acquired) || acquired == 0U ||
        !Read(victimCycle + 0xD8U, ended) || ended != 0U ||
        !Read(victimCycle + 0x68U, linkedSource) || linkedSource != source ||
        !Read(victimCycle + 0x6CU, cachedBirth) || cachedBirth != sourceIdentity.runtimeId ||
        !Read(victimCycle + 0xB0U, linked) || linked != sourceCycle) return false;
    // Health changes at the beginning of a fatal 4F graph, not its end.
    return ValidPairedVictimLink(victim, source, sourceCycle, rejectedState);
}


// Host stage 6 is the source's native kill outro: after the victim 4F ends
// (E6/D8), the same 4E graph keeps emitting markers on the Host (an End_Action
// 30-80 ms later) while the Alien finishes the kill. A brainless Client
// observer must not let its idle/stop scheduler replace that live 4E.


bool RetainAuthoritativePassage(std::uintptr_t,std::uintptr_t,std::uint32_t) noexcept;
thread_local bool gNativeIntentDispatchActive{};
bool RetainAuthoritativeSourceGraph(const std::uintptr_t source,
    const std::uintptr_t cycle, const std::uint32_t requestedState) noexcept {
    if(gPairRuntimeInitialized&&EngineThread()&&gRole.load(std::memory_order_acquire)==Role::Client&&PairGenerationStable()) {
        const interactions::Memory memory{gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
        }};
        for(const auto& pair:gPairTransactions)if(pair.used&&!pair.receipt.resourcesReleased&&
            pair.source.character==source&&PairIdentityLive(pair.source)&&
            gPairRuntime.RetainSource(source,cycle,memory))return true;
    }
    return RetainAuthoritativePassage(source,cycle,requestedState);
}
void __fastcall SchedulerIdleDetour(void* scheduler, void*) noexcept {
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(
        gSchedulerIdleOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto previousOwner = gSchedulerTransitionOwner;
    const auto previousRva = gSchedulerTransitionRva;
    const auto previousParent=gNativeSchedulerParent;
    gNativeSchedulerParent=CaptureNativeSchedulerParent(reinterpret_cast<std::uintptr_t>(scheduler));
    gSchedulerTransitionOwner = 0U;
    static_cast<void>(Read(reinterpret_cast<std::uintptr_t>(scheduler), gSchedulerTransitionOwner));
    gSchedulerTransitionRva = 0x00287690U;
    __try { original(scheduler); }
    __finally { gSchedulerTransitionOwner = previousOwner; gSchedulerTransitionRva = previousRva; gNativeSchedulerParent=previousParent; }
}
std::uint32_t __fastcall SchedulerTransitionDetour(void* scheduler, void*, const std::uint32_t previousMode) noexcept {
    const auto original = reinterpret_cast<std::uint32_t(__thiscall*)(void*, std::uint32_t)>(
        gSchedulerTransitionOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    const auto previousOwner = gSchedulerTransitionOwner;
    const auto previousRva = gSchedulerTransitionRva;
    const auto previousParent=gNativeSchedulerParent;
    gNativeSchedulerParent=CaptureNativeSchedulerParent(reinterpret_cast<std::uintptr_t>(scheduler));
    std::uint32_t result{};
    gSchedulerTransitionOwner = 0U;
    static_cast<void>(Read(reinterpret_cast<std::uintptr_t>(scheduler), gSchedulerTransitionOwner));
    gSchedulerTransitionRva = 0x00287190U;
    __try { result = original(scheduler, previousMode); }
    __finally { gSchedulerTransitionOwner = previousOwner; gSchedulerTransitionRva = previousRva; gNativeSchedulerParent=previousParent; }
    host_producer::ObserveNativeActionExecution(reinterpret_cast<std::uintptr_t>(scheduler));
    return result;
}
bool CaptureNativeIntentCall(std::uint32_t,std::uintptr_t,const std::uint32_t*,std::uint32_t) noexcept;
void TraceNativeIntentReturn(std::uint32_t,std::uintptr_t,std::uint32_t) noexcept;
void __fastcall PairedStopDetour(void* context, void*, const std::uint32_t argument) noexcept {
    const auto original = reinterpret_cast<void(__thiscall*)(void*, std::uint32_t)>(
        gPairedStopOriginal.load(std::memory_order_acquire));
    std::uintptr_t source{};
    if (original == nullptr || (Read(reinterpret_cast<std::uintptr_t>(context) + 4U, source) &&
        RetainAuthoritativeSourceGraph(source, reinterpret_cast<std::uintptr_t>(context), 0x3BU))) return;
    const std::uint32_t raw[2]{};
    const auto caller=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase);
    CaptureNativeIntentCall(7U,reinterpret_cast<std::uintptr_t>(context),raw,caller);
    original(context, argument); // one opaque stack word; retail RET 4
    TraceNativeIntentReturn(7U,source,caller);
}

// These class-dispatching entries can destroy a humanoid/Android cycle before
// reaching the shared Idle hook. Guard the same scheduler-owned transition at
// their entry, before any destructor, with each retail stack ABI unchanged.
void __fastcall PairedIdleDetour(void* context, void*, const std::uint32_t argument) noexcept {
    const auto original = reinterpret_cast<void(__thiscall*)(void*, std::uint32_t)>(
        gPairedIdleOriginal.load(std::memory_order_acquire));
    std::uintptr_t source{};
    if (original == nullptr || (Read(reinterpret_cast<std::uintptr_t>(context) + 4U, source) &&
        RetainAuthoritativeSourceGraph(source, reinterpret_cast<std::uintptr_t>(context), 1U))) return;
    const auto caller=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase);
    const bool previous=gNativeFallbackCovered;
    const bool edge=caller==0x287558U||caller==0x287618U||caller==0x287834U||caller==0x2879ABU;
    gNativeFallbackCovered=edge&&gNativeSchedulerParent.covered&&gNativeSchedulerParent.character==source;
    __try {original(context, argument);} __finally {gNativeFallbackCovered=previous;}
}
void __fastcall PairedAlternateIdleDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(
        gPairedAlternateIdleOriginal.load(std::memory_order_acquire));
    std::uintptr_t source{};
    if (original == nullptr || (Read(reinterpret_cast<std::uintptr_t>(context) + 4U, source) &&
        RetainAuthoritativeSourceGraph(source, reinterpret_cast<std::uintptr_t>(context), 2U))) return;
    const auto caller=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase);
    const bool previous=gNativeFallbackCovered;
    const bool edge=caller==0x28756FU||caller==0x28762FU||caller==0x28784BU||caller==0x2879C4U;
    gNativeFallbackCovered=edge&&gNativeSchedulerParent.covered&&gNativeSchedulerParent.character==source;
    __try {original(context);} __finally {gNativeFallbackCovered=previous;}
}

enum class RemoteObserverVictimState : std::uint8_t {
    Unknown = 0U,
    Active = 1U,
    Complete = 2U
};

RemoteObserverVictimState ReadRemoteObserverVictimState(const Actor& actor,
    const std::uintptr_t target, const OutcomeBinding& binding) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Client || target == 0U ||
        LocalClientVictim(target) || binding.victim.character != target ||
        binding.state.birth != actor.desired.birth ||
        binding.state.interactionPairRevision != actor.desired.interactionPairRevision ||
        binding.state.interactionTargetPeer != actor.desired.interactionTargetPeer ||
        !PairIdentityLive(binding.victim)) return RemoteObserverVictimState::Unknown;

    // The native finish notification can replace the 4F before this manager
    // samples it. Retain the exact local proof, never infer it from Host HP.
    if (binding.localVictimEnded) return RemoteObserverVictimState::Complete;
    std::uintptr_t sourceCycle{}, victimCycle{}, owner{}, table{}, linkedSource{}, linkedCycle{}, current{};
    std::uint32_t cachedBirth{};
    std::uint8_t sourceState{}, victimState{}, acquired{}, ended{}, flags{}, interactive{};
    if (!Read(actor.character + 0x36CU, sourceCycle) || sourceCycle == 0U ||
        !Read(sourceCycle + 0x1AU, sourceState) || sourceState != 0x4EU ||
        !Read(target + 0x36CU, victimCycle) || victimCycle == 0U ||
        !Read(victimCycle + 4U, owner) || owner != target ||
        !Read(victimCycle, table) || table != gBase + 0x00EAE690U ||
        !Read(victimCycle + 0x1AU, victimState) || victimState != 0x4FU ||
        !Read(victimCycle + 0x68U, linkedSource) || linkedSource != actor.character ||
        !Read(victimCycle + 0x6CU, cachedBirth) || cachedBirth != binding.source.runtimeId ||
        !Read(victimCycle + 0xB0U, linkedCycle) || linkedCycle != sourceCycle ||
        !Read(actor.character + 0x36CU, current) || current != sourceCycle ||
        !Read(victimCycle + 0xD9U, acquired) ||
        !Read(victimCycle + 0xD8U, ended) ||
        !Read(victimCycle + 8U, flags) ||
        !Read(victimCycle + 0xDAU, interactive))
        return RemoteObserverVictimState::Unknown;

    if (acquired == 0U) return RemoteObserverVictimState::Active;
    if (interactive != 0U) return RemoteObserverVictimState::Active;
    if (ended != 0U || (flags & 6U) != 0U) return RemoteObserverVictimState::Complete;
    return RemoteObserverVictimState::Active;
}






void __fastcall PairedVictimConvergenceDetour(void* context, void* victimContext,
    const std::uintptr_t victim, const std::uint32_t victimFlag) noexcept {
    const auto original = reinterpret_cast<PairedVictimConvergence>(gBase + 0x0024E6C0U);
    const auto sourceCycle = reinterpret_cast<std::uintptr_t>(context);
    const auto victimCycle = reinterpret_cast<std::uintptr_t>(victimContext);
    std::uintptr_t source{}, current{}, owner{}, table{}, linked{};
    std::uint8_t state{};
    if (!Read(victimCycle + 0x68U, source)) return;
    if (!ManagedPairedVictim(victim, source)) { original(context, victim, victimFlag); return; }
    // Only the certified 4F update CALL passes its ESI cycle. Preserve the
    // victim's native graph/death cleanup, but expire an unusable source link
    // exactly as 00655BDE already does for an absent or non-4E source.
    if (!Read(victim + 0x36CU, current) || current != victimCycle ||
        !Read(victimCycle + 4U, owner) || owner != victim ||
        !Read(victimCycle, table) || table != gBase + 0x00EAE690U ||
        !Read(victimCycle + 0x1AU, state) || state != 0x4FU ||
        !Read(victimCycle + 0xB0U, linked) || linked != sourceCycle) return;
    PairIdentity sourceIdentity{};
    std::uint32_t cachedBirth{}, rejectedState{};
    if (PublishedPairSource(source, sourceIdentity) == kActors ||
        !Read(victimCycle + 0x6CU, cachedBirth) || cachedBirth != sourceIdentity.runtimeId ||
        !ValidPairedVictimLink(victim, source, sourceCycle, rejectedState)) {
        if (Write(victimCycle + 0xB0U, std::uintptr_t{}))
            gPairedVictimConvergenceDetached.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    original(context, victim, victimFlag);
}
// CALL 00655BD7: ECX=source, ESI=victim 4F cycle; two arguments already on
// the stack. Fastcall consumes the original target/flag with the same RET 8.
__declspec(naked) void PairedVictimConvergenceShim() {
    __asm {
        mov edx, esi
        jmp PairedVictimConvergenceDetour
    }
}
bool VictimAdmissionMatches(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle) noexcept {
    return gVictimAdmissionDepth != 0U && gVictimAdmissionTarget == victim &&
        gVictimAdmissionSource == source && gVictimAdmissionCycle == sourceCycle;
}
bool BeginVictimAdmission(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle, std::size_t& slot) noexcept {
    PairIdentity binding{};
    slot = PublishedPairSource(source, binding);
    bool expected = false;
    if (slot == kActors) return false;
    if (!gPairSourceBusy[slot].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        gPairedVictimBusy.fetch_add(1U, std::memory_order_relaxed);
        slot = kActors;
        return false;
    }
    std::uint32_t state{};
    std::uint8_t initialized{};
    if (!ValidPairedVictimSource(victim, source, sourceCycle, state)) {
        gLastRejectedPairedSourceState.store(state, std::memory_order_relaxed);
        gPairSourceBusy[slot].store(false, std::memory_order_release);
        slot = kActors;
        return false;
    }
    // Native constructor resets E7; victim initialization sets it. Serialize
    // admission and consume that native handshake once for this source cycle.
    if (!Read(sourceCycle + 0xE7U, initialized) || initialized != 0U) {
        gPairedVictimDuplicates.fetch_add(1U, std::memory_order_relaxed);
        gPairSourceBusy[slot].store(false, std::memory_order_release);
        slot = kActors;
        return false;
    }
    return true;
}
// A PlayerSync representation has an NPC controller. Retail 00662670
// consequently skips the player damage/nonfatal choice, and 006E3960 filters
// its animation definitions as an NPC. The actual Client still takes both
// player branches. Recognize the same remote participant at these two queries,
// only inside its already leased Host preparation. Keep every subsequent
// retail difficulty, health, geometry, definition and fatal-fallback decision.
bool RemotePlayerPreparation(const std::uintptr_t victim,
    const std::uintptr_t sourceCycle) noexcept {
    const auto* submission = gNativeVictimSubmission;
    if (gRole.load(std::memory_order_acquire) != Role::Host ||
        !PairGenerationStable() || submission == nullptr || !submission->livingBefore ||
        submission->captured || submission->sourceCycle != sourceCycle ||
        submission->victimBefore.character != victim || submission->victimBefore.peer == 0U ||
        !PairIdentityLive(submission->victimBefore) || !RegisteredPlayerRepresentation(victim) ||
        !VictimAdmissionMatches(victim, submission->source, sourceCycle)) return false;
    std::uintptr_t current{}, source{}, target{};
    std::uint32_t birth{};
    std::uint8_t serial{}, state{}, flags{};
    return Read(submission->source + 0x36CU, current) && current == sourceCycle &&
        Read(sourceCycle + 4U, source) && source == submission->source &&
        Read(sourceCycle + 0x44U, target) && target == victim &&
        Read(sourceCycle + 0x48U, birth) && birth == submission->victimBefore.runtimeId &&
        Read(sourceCycle + 0x19U, serial) && serial == submission->sourceSerial &&
        Read(sourceCycle + 0x1AU, state) && state == 0x4EU &&
        Read(sourceCycle + 8U, flags) && (flags & 6U) == 0U;
}
bool PairedDamageAlreadyCarried(const std::uintptr_t victim, const std::uintptr_t source) noexcept {
    const auto* submission = gNativeVictimSubmission;
    return submission != nullptr && !submission->authoritativeClient && submission->slot < kActors &&
        submission->pairRevision != 0U && submission->source == source &&
        submission->victimBefore.character == victim && PairIdentityLive(submission->victimBefore);
}
void RecordHostHealthOrigin(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uint16_t before) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host) return;
    std::uint16_t after{};
    PairIdentity identity{};
    const auto slot = PublishedPairSource(victim, identity);
    if (slot == kActors || !Read(victim + 0x33CU, after) || before == after) return;
    auto& origin = gHostHealthOrigins[slot];
    PairIdentity sourceIdentity{};
    const auto sourceSlot = PublishedPairSource(source, sourceIdentity);
    if (PairedDamageAlreadyCarried(victim, source) && sourceSlot != kActors) {
        origin.victimRuntime.store(identity.runtimeId, std::memory_order_relaxed);
        origin.sourceSlot.store(static_cast<std::uint32_t>(sourceSlot), std::memory_order_relaxed);
        const auto tag = (static_cast<std::uint64_t>(sourceIdentity.authoritativeBirth) << 32U) |
            gNativeVictimSubmission->pairRevision;
        origin.tag.store(tag, std::memory_order_release);
    } else origin.tag.store(0U, std::memory_order_release);
}
void CommitOutcomeHealth(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uint32_t hp) noexcept {
    const auto original = reinterpret_cast<NativeHealthCommit>(gHealthCommitOriginal.load(std::memory_order_acquire));
    std::uint16_t before{};
    static_cast<void>(Read(victim + 0x33CU, before));
    if (original != nullptr) {
        original(reinterpret_cast<void*>(victim + 0x320U), hp, source);
        RecordHostHealthOrigin(victim, source, before);
        auto* submission=gNativeVictimSubmission;
        std::uint16_t actual{};std::uint32_t reaction{};
        if(submission&&submission->authoritativeClient&&submission->victimDispatch&&submission->source==source&&
            submission->victimBefore.character==victim&&hp==submission->outcome.hpReaction&&
            PairIdentityLive(submission->victimBefore)&&Read(victim+0x33CU,actual)&&actual==hp&&
            Read(victim+0x32CU,reaction)&&reaction==submission->outcome.healthReaction)submission->reactionApplied=true;
    }
}
void __fastcall OutcomeHealthCommitDetour(void* context, void*, const std::uint32_t hp,
    const std::uintptr_t source) noexcept {
    const auto original = reinterpret_cast<NativeHealthCommit>(gHealthCommitOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    auto resolvedHp = hp;
    if (health::ResolveCommit(reinterpret_cast<std::uintptr_t>(context), resolvedHp)) {
        original(context, resolvedHp, source);
        return;
    }
    const auto* submission = gNativeVictimSubmission;
    if (submission != nullptr && submission->authoritativeClient && submission->source == source &&
        reinterpret_cast<std::uintptr_t>(context) == submission->victimBefore.character + 0x320U &&
        PairIdentityLive(submission->victimBefore)) {
        original(context, submission->victimDispatch ? submission->outcome.hpReaction : submission->outcome.hpPrepared, source);
        return;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(context);
    if (address >= 0x10000U + 0x320U && !AllowLocalNativeDamage(address - 0x320U, source)) return;
    std::uint16_t before{};
    const bool readable = Read(address + 0x1CU, before);
    health::State beforeState{};
    health::Source observedSource{};
    const auto victim = address - 0x320U;
    PairIdentity victimIdentity{}, sourceIdentity{};
    const bool observeCommit = gRole.load(std::memory_order_acquire) == Role::Host &&
        !PairedDamageAlreadyCarried(victim, source) &&
        (PublishedPairSource(victim, victimIdentity) != kActors ||
         (PublishedPairParticipant(victim, victimIdentity) && PublishedPairSource(source, sourceIdentity) != kActors)) &&
        health::Capture(victim, beforeState);
    if (observeCommit && source != 0U && Read(source + 0x50U, observedSource.runtimeId)) observedSource.character = source;
    original(context, hp, source);
    if (readable) RecordHostHealthOrigin(address - 0x320U, source, before);
    if (observeCommit) health::ObserveCommit(victim, victimIdentity.runtimeId, observedSource, beforeState);
}
// A pair that no longer owns a remote player representation must hand it back
// to PlayerSync. Retail clears the victim's grapple bytes only in a completed
// 4E destructor; an aborted or abandoned replay would otherwise freeze it.
// Engine thread: an attacker's live 42/4E source cycle still names this victim.
bool PairedVictimHeld(const std::uintptr_t victim) noexcept {
    if(EngineThread()&&PairReservedCharacter(victim))return true;
    for (const auto& actor : gActors) {
        std::uintptr_t cycle{}, held{};
        std::uint8_t state{}, flags{};
        if (actor.character == 0U || !Read(actor.character + 0x36CU, cycle) || cycle == 0U ||
            !Read(cycle + 0x1AU, state) || (state != 0x4EU && state != 0x42U) ||
            !Read(cycle + 8U, flags) || (flags & 6U) != 0U || !Read(cycle + 0x44U, held)) continue;
        if (held == victim) return true;
    }
    return false;
}
// The attacker left this representation's 4F (its linked source cycle is gone
// or no longer the attacker's live 4E) while the representation is alive:
// a non-lethal hit. Its real player already got up on its own machine.
bool OrphanedParticipantVictim(const std::uintptr_t rep, std::uintptr_t& cycle) noexcept {
    cycle = 0U;
    std::uintptr_t table{}, owner{}, linked{}, source{}, current{};
    std::uint8_t state{}, sourceState{};
    if (!Read(rep + 0x36CU, cycle) || cycle == 0U || !Read(cycle, table) || table != gBase + 0x00EAE690U ||
        !Read(cycle + 4U, owner) || owner != rep || !Read(cycle + 0x1AU, state) || state != 0x4FU ||
        !LivingCharacter(rep) || !Read(cycle + 0xB0U, linked)) return false;
    if (linked == 0U || !Read(cycle + 0x68U, source) || source == 0U) return true;
    return !Read(source + 0x36CU, current) || current != linked ||
        !Read(linked + 0x1AU, sourceState) || sourceState != 0x4EU;
}
constexpr ULONGLONG kOrphanedVictimGraceMs = 1000ULL;
std::array<ULONGLONG, 32U> gOrphanedVictimSince{}; // indexed like gCurrent.participants
std::atomic<std::uint64_t> gOrphanedVictimsReleased{};
void ReleaseOrphanedParticipantVictims() noexcept {
    if (!EngineThread() || !Stable()) return;
    const auto now = GetTickCount64();
    for (std::uint32_t i = 0U; i < gCurrent.participantCount && i < gOrphanedVictimSince.size(); ++i) {
        const auto rep = ParticipantForPeer(gCurrent.participants[i].peer);
        std::uintptr_t cycle{};
        if (rep == 0U || rep == LocalPlayer() || !RegisteredPlayerRepresentation(rep) ||
            PairReservedCharacter(rep) || !OrphanedParticipantVictim(rep, cycle)) { gOrphanedVictimSince[i] = 0U; continue; }
        if (gOrphanedVictimSince[i] == 0U) { gOrphanedVictimSince[i] = now; continue; }
        if (now - gOrphanedVictimSince[i] < kOrphanedVictimGraceMs) continue;
        gOrphanedVictimSince[i] = 0U;
        if (character_direct_step_runtime::FinishOrphanedVictimCycle(rep, cycle))
            gOrphanedVictimsReleased.fetch_add(1U, std::memory_order_relaxed);
    }
}
void ReleasePairedVictim(const std::uintptr_t victim) noexcept {
    if (victim == 0U || !RegisteredPlayerRepresentation(victim)) return;
    character_direct_step_runtime::RequestPairedVictimRelease(victim);
    gPairedVictimReleaseRequests.fetch_add(1U, std::memory_order_relaxed);
}
std::uintptr_t RejectClientPreparation(const std::uintptr_t victim,
    std::atomic<std::uint64_t>& reason) noexcept {
    reason.fetch_add(1U, std::memory_order_relaxed);
    gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed);
    ReleasePairedVictim(victim);
    return 2U;
}
// The Host's prepared result is authoritative even when this Client cannot
// replay its animation (e.g. a non-lethal grab definition this table does not
// resolve). Keep the witnessed health; the source then takes the failed-source
// epilogue and the victim representation is released.
void ApplyPreparationHealthOnly(const std::uintptr_t victim, const NativeVictimSubmission& submission) noexcept {
    const auto& result = submission.outcome;
    const auto damage = result.hpBefore - result.hpPrepared;
    if (damage != 0U)
        reinterpret_cast<NativeDamage>(gBase + 0x003CC900U)(reinterpret_cast<void*>(victim),
            damage, submission.source, 0U, 0U, 1U, 0U, 0x16U, 0U, 0U);
    CommitOutcomeHealth(victim, submission.source, result.hpReaction);
    gPrepHealthOnlyApplied.fetch_add(1U, std::memory_order_relaxed);
}
std::uintptr_t ResolvedClientPreparation(const std::uintptr_t victim,
    const std::uintptr_t sourceCycle) noexcept {
    auto* submission = gNativeVictimSubmission;
    constexpr std::uintptr_t rejected = 2U;
    if (submission == nullptr || !submission->authoritativeClient ||
        submission->sourceCycle != sourceCycle || submission->victimBefore.character != victim ||
        !VictimAdmissionMatches(victim, submission->source, sourceCycle))
        return RejectClientPreparation(victim, gPrepRejectSubmission);
    // A rejected result aborts this producer at its existing retail epilogue;
    // it must never fall back to an independent local difficulty/geometry roll.
    PairIdentity source{};
    std::uintptr_t manager{}, table{}, getter{}, definition{};
    std::uint32_t kind{};
    const auto& result = submission->outcome;
    if ((result.flags & OutcomePrepared) == 0U ||
        PublishedPairSource(submission->source, source) == kActors || source.animated == 0U ||
        !Read(source.animated + 0x34B4U, manager) || manager == 0U ||
        !Read(manager, table) || !Read(table + 0x34U, getter) ||
        getter != gBase + 0x002D60F0U) {
        return RejectClientPreparation(victim, gPrepRejectSource);
    }
    __try {
        if ((result.flags & OutcomeVictimAdmitted) == 0U) {
            // A native interruption may keep preparation damage without ever
            // acquiring 4F. Replay that nonterminal result, then use the retail
            // failed-source epilogue; do not invent an acquired victim graph.
            if ((result.flags & OutcomeTerminal) != 0U)
                return RejectClientPreparation(victim, gPrepRejectTerminal);
            const auto damage = result.hpBefore - result.hpPrepared;
            if (damage != 0U)
                reinterpret_cast<NativeDamage>(gBase + 0x003CC900U)(reinterpret_cast<void*>(victim),
                    damage, submission->source, 0U, 0U, 1U, 0U, 0x16U, 0U, 0U);
            CommitOutcomeHealth(victim, submission->source, result.hpReaction);
            gOutcomeApplied.fetch_add(1U, std::memory_order_relaxed);
            return rejected;
        }
        definition = reinterpret_cast<std::uintptr_t(__thiscall*)(void*, std::uint32_t)>(getter)(
            reinterpret_cast<void*>(manager), result.definition);
        if (definition == 0U || !Read(definition + 8U, kind) || kind != result.kind) {
            if ((result.flags & OutcomeTerminal) == 0U) ApplyPreparationHealthOnly(victim, *submission);
            return RejectClientPreparation(victim, gPrepRejectDefinition);
        }
        float position[3]{};
        for (std::size_t i = 0U; i < 3U; ++i)
            if (!Read(victim + kPositionOffset + i * 4U, position[i]) ||
                !FiniteCoordinate(position[i] + result.targetOffset[i]))
                return RejectClientPreparation(victim, gPrepRejectGeometry);
        if (!Write(sourceCycle + 0xB8U, result.sourceYaw)) return RejectClientPreparation(victim, gPrepRejectGeometry);
        for (std::size_t i = 0U; i < 3U; ++i)
            if (!Write(sourceCycle + 0xD0U + i * 4U, position[i] + result.targetOffset[i]))
                return RejectClientPreparation(victim, gPrepRejectGeometry);
        const auto damage = result.hpBefore - result.hpPrepared;
        if (damage != 0U)
            reinterpret_cast<NativeDamage>(gBase + 0x003CC900U)(reinterpret_cast<void*>(victim),
                damage, submission->source, 0U, 0U, 1U, 0U, 0x16U, 0U, 0U);
        // Native gates can differ on a presentation Character. Commit the
        // witnessed Host value through the SAME health state machine, once.
        CommitOutcomeHealth(victim, submission->source, result.hpPrepared);
        std::uint16_t preparedHp{};
        submission->preparationApplied=PairIdentityLive(submission->victimBefore)&&
            Read(victim+0x33CU,preparedHp)&&preparedHp==result.hpPrepared;
        gOutcomeApplied.fetch_add(1U, std::memory_order_relaxed);
        return definition;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static_cast<void>(rejected);
        return RejectClientPreparation(victim, gPrepRejectFault);
    }
}
std::uintptr_t __cdecl PlayerPreparationBoundary(const std::uintptr_t victim,
    const std::uintptr_t sourceCycle) noexcept {
    if (gNativeVictimSubmission != nullptr && gNativeVictimSubmission->abortHostPreparation &&
        gNativeVictimSubmission->sourceCycle == sourceCycle) return 2U;
    if (gNativeVictimSubmission != nullptr && gNativeVictimSubmission->authoritativeClient)
        return ResolvedClientPreparation(victim, sourceCycle);
    if (!RemotePlayerPreparation(victim, sourceCycle)) return 0;
    gNativeVictimSubmission->playerPreparation = true;
    gPairedPlayerPreparationRemaps.fetch_add(1U, std::memory_order_relaxed);
    return 1;
}
int __cdecl PlayerRuntimeBoundary(const std::uintptr_t cycle, const std::uintptr_t victim) noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    if ((role != Role::Host && role != Role::Client) || !PairGenerationStable() ||
        (!RegisteredPlayerRepresentation(victim) &&
            !(role == Role::Client && LocalClientVictim(victim)))) return 0;
    PairIdentity sourceIdentity{}, victimIdentity{};
    std::uintptr_t source{}, current{}, owner{}, target{}, table{};
    std::uint8_t state{};
    std::uint32_t targetBirth{};
    // Exact published human participation, not a template-class/player-slot
    // guess. The ordinary source update already checked victim lifetime.
    if (!Read(cycle + 4U, source) || PublishedPairSource(source, sourceIdentity) == kActors ||
        !PublishedPairParticipant(victim, victimIdentity) || !PairIdentityLive(sourceIdentity) ||
        !PairIdentityLive(victimIdentity) || !Read(source + 0x36CU, current) || current != cycle ||
        !Read(cycle + 4U, owner) || owner != source || !Read(cycle, table) ||
        table != gBase + 0x00EAE5D0U || !Read(cycle + 0x1AU, state) || state != 0x4EU ||
        !Read(cycle + 0x44U, target) || target != victim ||
        !Read(cycle + 0x48U, targetBirth) || targetBirth != victimIdentity.runtimeId) return 0;
    gPairedRuntimePlayerRemaps.fetch_add(1U, std::memory_order_relaxed);
    return 1;
}
// 00663E51: ESI=source 4E, EAX=victim. Preserve all GPRs, flags and
// x87/MMX/XMM/MXCSR. Human proxies take the same +E0 stage dispatcher as the
// real retail player, bypassing only the NPC-victim/controller-state guard.
__declspec(naked) void PlayerRuntimeShim() {
    __asm {
        pushfd
        pushad
        mov edi, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push dword ptr [edi + 1Ch]
        push dword ptr [edi + 4]
        call PlayerRuntimeBoundary
        add esp, 8
        test eax, eax
        jnz participant
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gPlayerRuntimeTrampoline]
    participant:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gPlayerRuntimeContinue]
    }
}
// End_Action at 00664A10 must keep its actual controller-liveness predicate.
// Unlike the source-update stage dispatcher, it does not classify NPC/player
// graph execution. An absent/inactive victim controller branches to 00664AB2
// (flags |= 6), including for a PlayerSync representation. Remapping that
// participant directly to 00664AA9 incorrectly tested E5 even after its
// controller ended, leaving the source graph and pair reservation alive.
int __cdecl PlayerCategoryBoundary(const std::uintptr_t victim) noexcept {
    const auto* submission = gNativeVictimSubmission;
    if (submission == nullptr || !submission->playerPreparation ||
        !RemotePlayerPreparation(victim, submission->sourceCycle)) return 0;
    gPairedPlayerCategoryRemaps.fetch_add(1U, std::memory_order_relaxed);
    return 1;
}
// 006626F7: preserve GPRs/EFLAGS and aligned FXSAVE state (x87/MMX/XMM/MXCSR).
// EDI holds the saved PUSHAD frame; saved ECX=+18h, ESI=+4, EAX=+1Ch.
// 1 takes the Host player branch;
// a definition pointer resumes at 00662837 with ONLY saved EAX replaced;
// 2 aborts at the retail epilogue after marking this exact cycle failed.
__declspec(naked) void PlayerPreparationShim() {
    __asm {
        pushfd
        pushad
        mov edi, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push dword ptr [edi + 4]
        push dword ptr [edi + 18h]
        call PlayerPreparationBoundary
        add esp, 8
        cmp eax, 1
        je player
        cmp eax, 2
        je abortPreparation
        test eax, eax
        jnz resolved
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gPlayerPreparationTrampoline]
    player:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gPlayerPreparationContinue]
    resolved:
        mov dword ptr [edi + 1Ch], eax
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gOutcomePreparationContinue]
    abortPreparation:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        mov dword ptr [esi + 44h], ebx
        or byte ptr [esi + 8], 6
        jmp dword ptr [gOutcomePreparationAbort]
    }
}
// Interior query: all GPRs, EFLAGS and eight SSE registers survive.
// No controller, health, source parameters, animation or target is written.
__declspec(naked) void PlayerCategoryShim() {
    __asm {
        pushfd
        pushad
        sub esp, 80h
        movdqu xmmword ptr [esp + 0h], xmm0
        movdqu xmmword ptr [esp + 10h], xmm1
        movdqu xmmword ptr [esp + 20h], xmm2
        movdqu xmmword ptr [esp + 30h], xmm3
        movdqu xmmword ptr [esp + 40h], xmm4
        movdqu xmmword ptr [esp + 50h], xmm5
        movdqu xmmword ptr [esp + 60h], xmm6
        movdqu xmmword ptr [esp + 70h], xmm7
        push esi
        call PlayerCategoryBoundary
        add esp, 4
        test eax, eax
        jnz player
        movdqu xmm0, xmmword ptr [esp + 0h]
        movdqu xmm1, xmmword ptr [esp + 10h]
        movdqu xmm2, xmmword ptr [esp + 20h]
        movdqu xmm3, xmmword ptr [esp + 30h]
        movdqu xmm4, xmmword ptr [esp + 40h]
        movdqu xmm5, xmmword ptr [esp + 50h]
        movdqu xmm6, xmmword ptr [esp + 60h]
        movdqu xmm7, xmmword ptr [esp + 70h]
        add esp, 80h
        popad
        popfd
        jmp dword ptr [gPlayerCategoryTrampoline]
    player:
        movdqu xmm0, xmmword ptr [esp + 0h]
        movdqu xmm1, xmmword ptr [esp + 10h]
        movdqu xmm2, xmmword ptr [esp + 20h]
        movdqu xmm3, xmmword ptr [esp + 30h]
        movdqu xmm4, xmmword ptr [esp + 40h]
        movdqu xmm5, xmmword ptr [esp + 50h]
        movdqu xmm6, xmmword ptr [esp + 60h]
        movdqu xmm7, xmmword ptr [esp + 70h]
        add esp, 80h
        popad
        popfd
        jmp dword ptr [gPlayerCategoryContinue]
    }
}
void __fastcall PairedVictimEventDetour(const void* packet, void*, void* character) noexcept {
    const auto original = reinterpret_cast<PairedVictimEvent>(gPairedVictimEventOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto victim = reinterpret_cast<std::uintptr_t>(character);
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    std::uintptr_t source{}, sourceCycle{};
    if (!Read(address, source)) return;
    if (!ManagedPairedVictim(victim, source)) { original(packet, character); return; }
    std::size_t slot = kActors;
    std::uint32_t rejectedState{};
    const bool readable = Read(address + 0x30U, sourceCycle);
    const bool nested = readable && VictimAdmissionMatches(victim, source, sourceCycle);
    std::uint8_t initialized{};
    if (nested && (!Read(sourceCycle + 0xE7U, initialized) || initialized != 0U)) {
        gPairedVictimDuplicates.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (!readable || (nested ? !ValidPairedVictimSource(victim, source, sourceCycle, rejectedState)
                           : !BeginVictimAdmission(victim, source, sourceCycle, slot))) {
        // Reject before the event handler resets the victim's animation layer.
        gPairedVictimEventsRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const auto previousSource = gVictimAdmissionSource, previousTarget = gVictimAdmissionTarget;
    const auto previousCycle = gVictimAdmissionCycle;
    const auto previousDepth = gVictimAdmissionDepth;
    gVictimAdmissionSource = source; gVictimAdmissionTarget = victim;
    gVictimAdmissionCycle = sourceCycle; gVictimAdmissionDepth = previousDepth + 1U;
    __try { original(packet, character); }
    __finally {
        gVictimAdmissionSource = previousSource; gVictimAdmissionTarget = previousTarget;
        gVictimAdmissionCycle = previousCycle; gVictimAdmissionDepth = previousDepth;
        if (slot != kActors) gPairSourceBusy[slot].store(false, std::memory_order_release);
    }
}
void __fastcall PairedVictimTransitionDetour(void* context, void*,
    const std::uintptr_t source, const void* parameters, const std::uintptr_t sourceCycle) noexcept {
    const auto original = reinterpret_cast<PairedVictimTransition>(
        gPairedVictimTransitionOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{}, ownedCycle{};
    if (!Read(cycle + 4U, victim)) return;
    if (!ManagedPairedVictim(victim, source)) { original(context, source, parameters, sourceCycle); return; }
    std::uint32_t state{};
    std::size_t slot = kActors;
    const bool nested = VictimAdmissionMatches(victim, source, sourceCycle);
    if (!Read(victim + 0x36CU, ownedCycle) || ownedCycle != cycle ||
        (nested ? !ValidPairedVictimSource(victim, source, sourceCycle, state)
                : !BeginVictimAdmission(victim, source, sourceCycle, slot))) {
        gPairedVictimTransitionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const auto previousSource = gVictimAdmissionSource, previousTarget = gVictimAdmissionTarget;
    const auto previousCycle = gVictimAdmissionCycle;
    const auto previousDepth = gVictimAdmissionDepth;
    gVictimAdmissionSource = source; gVictimAdmissionTarget = victim;
    gVictimAdmissionCycle = sourceCycle; gVictimAdmissionDepth = previousDepth + 1U;
    gPairedVictimTransitions.fetch_add(1U, std::memory_order_relaxed);
    __try { original(context, source, parameters, sourceCycle); }
    __finally {
        gVictimAdmissionSource = previousSource; gVictimAdmissionTarget = previousTarget;
        gVictimAdmissionCycle = previousCycle; gVictimAdmissionDepth = previousDepth;
        if (slot != kActors) gPairSourceBusy[slot].store(false, std::memory_order_release);
    }
}
void __fastcall PairedVictimInitializeDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<PairedVictimInitialize>(
        gPairedVictimInitializeOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{}, source{}, sourceCycle{}, ownedCycle{}, table{};
    std::uint8_t state{};
    if (Read(cycle + 4U, victim) && Read(cycle + 0x68U, source) &&
        ManagedPairedVictim(victim, source) &&
        Read(victim + 0x36CU, ownedCycle) && ownedCycle == cycle &&
        Read(cycle + 0x1AU, state) && state == 0x4FU &&
        Read(cycle, table) && table == gBase + 0x00EAE690U) {
        std::uint32_t rejectedState{0xFFFFFFFFU};
        if (!Read(cycle + 0xB0U, sourceCycle) ||
            !VictimAdmissionMatches(victim, source, sourceCycle) ||
            !ValidPairedVictimSource(victim, source, sourceCycle, rejectedState)) {
            // Recheck after the victim's previous-cycle destructor: native
            // callbacks can change the source during that transition too.
            // Mark only this newly constructed cycle as failed using retail
            // completion flags. The 4F destructor must not finalize a grapple
            // which was never initialized (D8=ended, D9=no animation acquired).
            std::uint8_t flags{};
            if (Read(cycle + 8U, flags))
                static_cast<void>(Write(cycle + 8U, static_cast<std::uint8_t>(flags | 0x46U)));
            static_cast<void>(Write(cycle + 0xB0U, std::uintptr_t{}));
            static_cast<void>(Write(cycle + 0xB4U, std::uintptr_t{}));
            static_cast<void>(Write(cycle + 0xD8U, std::uint8_t{1U}));
            static_cast<void>(Write(cycle + 0xD9U, std::uint8_t{}));
            gLastRejectedPairedSourceState.store(rejectedState, std::memory_order_relaxed);
            gPairedVictimInitializeAborted.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
    }
    original(context);
    std::uint8_t acquired{};
    if (VictimAdmissionMatches(victim, source, sourceCycle) &&
        Read(cycle + 0xD9U, acquired) && acquired != 0U) {
        gPairedVictimNativeStarted.fetch_add(1U, std::memory_order_relaxed);
        if (LocalClientVictim(victim))
            gPairedVictimLocalStarted.fetch_add(1U, std::memory_order_relaxed);
        if (gNativeVictimSubmission != nullptr && !gNativeVictimSubmission->authoritativeClient &&
            gNativeVictimSubmission->slot != kActors) {
            auto& result = gNativeVictimSubmission->outcome;
            result.flags |= OutcomeVictimAdmitted;
            std::uint8_t interactive{};
            if (result.kind != 0U && Read(cycle + 0xDAU, interactive) && interactive == 0U)
                result.flags |= OutcomeNativeDeath;
        }
        if (gNativeVictimSubmission != nullptr && gNativeVictimSubmission->authoritativeClient &&
            PairIdentityLive(gNativeVictimSubmission->victimBefore) &&
            (gNativeVictimSubmission->outcome.kind != 0U ||
             (gNativeVictimSubmission->outcome.flags & OutcomeTerminal) == 0U))
            CommitOutcomeHealth(victim, source, gNativeVictimSubmission->outcome.hpReaction);
    }
}
void __fastcall PairedVictimSubmitDetour(void* character, void* dispatch, const void* packet) noexcept {
    const auto victim = reinterpret_cast<std::uintptr_t>(character);
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    std::uintptr_t source{}, sourceCycle{};
    std::uint32_t state{};
    // Capture the original 0x40-byte message only inside its certified producer.
    // Dispatch after that producer returns: victim death callbacks must not
    // re-enter a half-prepared source animation. No synthetic packet/clip.
    if (gNativeVictimSubmission != nullptr &&
        !gNativeVictimSubmission->captured && Read(address, source) &&
        Read(address + 0x30U, sourceCycle) && sourceCycle == gNativeVictimSubmission->sourceCycle &&
        gNativeVictimSubmission->slot != kActors &&
        ManagedPairedVictim(victim, source) && ValidPairedVictimSource(victim, source, sourceCycle, state) &&
        crash_guard::TryCopy(gNativeVictimSubmission->packet.data(), packet,
            gNativeVictimSubmission->packet.size())) {
        gNativeVictimSubmission->victim = victim;
        gNativeVictimSubmission->captured = true;
        if (!gNativeVictimSubmission->authoritativeClient) {
            auto& outcome = gNativeVictimSubmission->outcome;
            std::uintptr_t definition{};
            std::uint32_t index{};
            std::uint16_t hp{};
            // The selected definition's own table index is +4 (006D60F0/
            // 006D6050 index the 0x84-byte table with it). Source +108 is the
            // result of manager virtual+2C (006D6050), which always returns 0:
            // replaying it made every Client choose definition 0 (the short
            // NPC grab) instead of the Host's player kill.
            bool complete = Read(sourceCycle + 0x4CU, definition) && definition != 0U &&
                Read(definition + 8U, outcome.kind) && Read(definition + 4U, index) && index < 0x19U &&
                Read(sourceCycle + 0xB8U, outcome.sourceYaw) && Read(victim + 0x33CU, hp);
            outcome.definition = index; outcome.hpPrepared = hp;
            for (std::size_t i = 0U; i < 3U; ++i) {
                float selected{}, targetPosition{};
                complete = Read(sourceCycle + 0xD0U + i * 4U, selected) &&
                    Read(victim + kPositionOffset + i * 4U, targetPosition) && complete;
                outcome.targetOffset[i] = selected - targetPosition;
                complete = FiniteCoordinate(outcome.targetOffset[i]) && complete;
            }
            if (complete && outcome.hpBefore != 0U && outcome.hpPrepared <= outcome.hpBefore &&
                std::isfinite(outcome.sourceYaw) && std::abs(outcome.sourceYaw) <= 100000.0F)
                outcome.flags = OutcomePrepared;
        }
        // Preserve the already working real Host player's inline retail flow.
        // Only remote/Client victims need the existing delayed dispatch guard.
        if (gRole.load(std::memory_order_acquire) == Role::Host &&
            !RegisteredPlayerRepresentation(victim) && dispatch != nullptr) {
            gNativeVictimSubmission->victimDispatch = true;
            gNativeVictimSubmission->retailDispatched = true;
            reinterpret_cast<CharacterMessage>(dispatch)(character, packet);
        }
        return;
    }
    if (dispatch != nullptr) reinterpret_cast<CharacterMessage>(dispatch)(character, packet);
}
void __fastcall PairedVictimPrepareDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<PairedVictimInitialize>(
        gPairedVictimPrepareOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    NativeVictimSubmission submission{};
    submission.sourceCycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{};
    std::size_t slot = kActors;
    // Retail animation workers also invoke this producer. Only atomic identity
    // publications/TLS/native reads are used here, never engine-owned tables.
    // Lease the exact source through preparation AND the native victim handler.
    if (!Read(submission.sourceCycle + 4U, submission.source) ||
        !Read(submission.sourceCycle + 0x44U, victim) ||
        !ManagedPairedVictim(victim, submission.source)) { original(context); return; }
    if (
        !PublishedPairVictim(victim, submission.victimBefore) ||
        !Read(submission.sourceCycle + 0x19U, submission.sourceSerial) ||
        !VictimHealthAllowsAdmission(victim) ||
        !BeginVictimAdmission(victim, submission.source, submission.sourceCycle, slot)) {
        // A duplicate/busy/dead admission must not re-run preparation damage.
        gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const bool hasCarrier = ClaimOutcome(submission, slot);
    const auto preparationPlan=PlanPairedPreparation(gRole.load(std::memory_order_acquire),hasCarrier,
        submission.duplicateOutcome,RegisteredPlayerRepresentation(victim),LocalClientVictim(victim));
    if (preparationPlan != PairedPreparationPlan::Prepare) {
        gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed);
        if (preparationPlan == PairedPreparationPlan::Ignore) {
            gPairSourceBusy[slot].store(false, std::memory_order_release); return;
        }
        // Enter only the existing certified failure epilogue via the
        // preparation shim. No independent definition, damage or victim graph.
        submission.abortHostPreparation=true;
    }
    std::uint16_t hpBefore{};
    if (!submission.authoritativeClient && Read(victim + 0x33CU, hpBefore))
        submission.outcome.hpBefore = hpBefore;
    submission.livingBefore = true;
    auto* previous = gNativeVictimSubmission;
    const auto previousSource = gVictimAdmissionSource, previousTarget = gVictimAdmissionTarget;
    const auto previousCycle = gVictimAdmissionCycle, previousDepth = gVictimAdmissionDepth;
    gNativeVictimSubmission = &submission;
    gVictimAdmissionSource = submission.source; gVictimAdmissionTarget = victim;
    gVictimAdmissionCycle = submission.sourceCycle; gVictimAdmissionDepth = previousDepth + 1U;
    __try {
        original(context);
        if (submission.abortHostPreparation) {
            std::uintptr_t current{}, target{};std::uint8_t serial{},flags{};
            const bool aborted=PairIdentityLive(submission.victimBefore)&&
                Read(submission.source+0x36CU,current)&&current==submission.sourceCycle&&
                Read(current+0x19U,serial)&&serial==submission.sourceSerial&&
                Read(current+8U,flags)&&(flags&6U)==6U&&Read(current+0x44U,target)&&target==0U;
            if(aborted) {
                // The failed-source epilogue cleared +44, so its destructor
                // cannot clear the old victim's grapple bytes. Request the
                // existing safe release; it still checks live pair/death owners.
                ReleasePairedVictim(victim);
                ++gUnboundHostPreparationAborts;
            } else ++gUnboundHostPreparationAbortIncomplete;
        }
        // The original packet/variant/geometry are complete now. Run the same
        // native message handler before the source can be reused, including
        // when native preparation has just changed its victim's health.
        if (submission.captured) {
            submission.victimDispatch = true;
            if (!submission.retailDispatched) {
                gPairedVictimNativeDispatches.fetch_add(1U, std::memory_order_relaxed);
                if (!EngineThread()) gPairedVictimWorkerDispatches.fetch_add(1U, std::memory_order_relaxed);
                if (LocalClientVictim(submission.victim))
                    gPairedVictimLocalDispatches.fetch_add(1U, std::memory_order_relaxed);
                PairedVictimEventDetour(submission.packet.data(), nullptr, reinterpret_cast<void*>(submission.victim));
            }
            if (!submission.authoritativeClient) {
                ObserveVictimOutcome(submission.victim, submission.source, submission.sourceSerial, submission.outcome);
                if (submission.nativeFinish) submission.outcome.flags |= OutcomeNativeDeath;
                RecordOutcome(submission);
            }
        }
    } __finally {
        gNativeVictimSubmission = previous;
        gVictimAdmissionSource = previousSource; gVictimAdmissionTarget = previousTarget;
        gVictimAdmissionCycle = previousCycle; gVictimAdmissionDepth = previousDepth;
        gPairSourceBusy[slot].store(false, std::memory_order_release);
    }
    PublishPairOutcomeEvidence(submission);
}
int __cdecl VictimFinishBoundary(const std::uintptr_t cycle) noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    auto* submission = gNativeVictimSubmission;
    if (role == Role::Host) return 0; // all Host predicates/effects stay retail
    if (role != Role::Client && role != Role::FrozenClient) return 0;
    std::uintptr_t victim{}, source{};
    if (!Read(cycle + 4U, victim) || !Read(cycle + 0x68U, source) ||
        !ManagedPairedVictim(victim, source)) return 0;
    // Recovery revokes execution authority, including direct native death /
    // controller transitions which do not pass through the HP-write guard.
    if (role == Role::FrozenClient) return 1;
    // Skip the WHOLE late-kill block, including control/state transitions.
    // Missing authority cannot authorize a local Character+29B conversion.
    if (submission == nullptr || !submission->authoritativeClient || submission->source != source ||
        submission->victimBefore.character != victim || !PairIdentityLive(submission->victimBefore)) return 1;
    if (submission->outcome.kind != 0U) return 0;
    if ((submission->outcome.flags & (OutcomeNativeDeath | OutcomeTerminal)) !=
        (OutcomeNativeDeath | OutcomeTerminal)) return 1;
    PairIdentity identity{};
    const auto slot = PublishedPairSource(source, identity);
    if (slot == kActors) return 1;
    std::lock_guard lock(gOutcomeMutex);
    auto& binding = gOutcomes[slot];
    if (binding.source.runtimeId != identity.runtimeId || binding.source.epoch != identity.epoch ||
        binding.victim.runtimeId != submission->victimBefore.runtimeId ||
        binding.state.interactionPairRevision != submission->pairRevision || binding.lateFinishClaimed) return 1;
    // HP may already be terminal; the native control/state/finish block is
    // nevertheless required. Claim the transition, not merely an HP change.
    binding.lateFinishClaimed = true;
    return 2;
}
// CALL 00655B88 is reached only AFTER this block's kill and state=2 calls.
// 00649FA0: ECX=4F cycle, no stack arguments, plain RET (0064A078).
void __fastcall VictimFinishExecutedDetour(void* context, void*) noexcept {
    reinterpret_cast<PairedVictimInitialize>(gBase + 0x00249FA0U)(context);
    auto* submission = gNativeVictimSubmission;
    std::uintptr_t victim{};
    if (submission != nullptr && Read(reinterpret_cast<std::uintptr_t>(context) + 4U, victim) &&
        victim == submission->victimBefore.character && PairIdentityLive(submission->victimBefore))
        submission->nativeFinish = true;
}
__declspec(naked) void VictimFinishShim() {
    __asm {
        pushfd
        pushad
        mov edi, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push dword ptr [edi + 4]
        call VictimFinishBoundary
        add esp, 4
        cmp eax, 1
        je skipKill
        cmp eax, 2
        je kill
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gVictimFinishTrampoline]
    skipKill:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gVictimFinishSkip]
    kill:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        mov eax, dword ptr [esi + 4]
        jmp dword ptr [gVictimFinishKill]
    }
}
void __fastcall VictimOutcomeUpdateDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<PairedVictimInitialize>(gVictimUpdateOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{}, source{}, owned{};
    std::uintptr_t linked{}, current{}, table{}, sourceTable{}, sourceOwner{};
    std::uint32_t cachedBirth{};
    std::uint8_t serial{}, sourceState{};
    PairIdentity sourceIdentity{};
    std::size_t slot = kActors;
    OutcomeBinding binding{};
    const bool managed = Read(cycle + 4U, victim) && Read(cycle + 0x68U, source) &&
        Read(victim + 0x36CU, owned) && owned == cycle && LoadOutcome(source, victim, binding) &&
        binding.claimed &&
        (slot = PublishedPairSource(source, sourceIdentity)) != kActors &&
        sourceIdentity.runtimeId == binding.source.runtimeId &&
        sourceIdentity.authoritativeBirth == binding.source.authoritativeBirth &&
        sourceIdentity.epoch == binding.source.epoch &&
        PairIdentityLive(binding.source) && PairIdentityLive(binding.victim) &&
        Read(cycle, table) && table == gBase + 0x00EAE690U &&
        Read(cycle + 0x6CU, cachedBirth) && cachedBirth == binding.source.runtimeId &&
        Read(cycle + 0xB0U, linked) && Read(source + 0x36CU, current) && linked == current &&
        Read(linked, sourceTable) && sourceTable == gBase + 0x00EAE5D0U &&
        Read(linked + 4U, sourceOwner) && sourceOwner == source &&
        Read(linked + 0x1AU, sourceState) && sourceState == 0x4EU &&
        Read(linked + 0x19U, serial) && serial == binding.serial &&
        (binding.state.outcome.flags & OutcomePrepared) != 0U;
    NativeVictimSubmission submission{};
    submission.source = source; submission.victimBefore = binding.victim;
    // The same lease also authorizes native damage/HP writes from this update.
    // Keep its source cycle and serial as well as the portable outcome identity;
    // otherwise a late 4F hit becomes an unrelated generic damage observation.
    if (managed) {
        submission.slot = slot; submission.sourceCycle = linked;
        submission.sourceSerial = serial; submission.victim = victim;
    }
    submission.outcome = binding.state.outcome;
    submission.pairRevision = binding.state.interactionPairRevision;
    submission.authoritativeClient = managed && gRole.load(std::memory_order_acquire) == Role::Client;
    if(submission.authoritativeClient) {submission.transaction=binding.transaction;submission.transactionSource=binding.transactionActors.source;}
    submission.victimDispatch = true;
    auto* previous = gNativeVictimSubmission;
    if (managed) gNativeVictimSubmission = &submission;
    __try {
        original(context);
        if(submission.authoritativeClient&&submission.nativeFinish&&PairIdentityLive(submission.victimBefore))
            CommitOutcomeHealth(victim,source,submission.outcome.hpReaction);
    }
    __finally { gNativeVictimSubmission = previous; }
    PublishPairOutcomeEvidence(submission);
    if (!managed || gRole.load(std::memory_order_acquire) != Role::Host ||
        !PairIdentityLive(binding.source) || !PairIdentityLive(binding.victim)) return;
    PairIdentity currentSource{};
    if (PublishedPairSource(source, currentSource) != slot ||
        currentSource.runtimeId != sourceIdentity.runtimeId ||
        currentSource.authoritativeBirth != sourceIdentity.authoritativeBirth ||
        currentSource.epoch != sourceIdentity.epoch) return;
    auto result = binding.state.outcome;
    ObserveVictimOutcome(victim, source, binding.serial, result);
    if (submission.nativeFinish) result.flags |= OutcomeNativeDeath;
    if (Read(victim + 0x36CU, owned) && owned != cycle) result.flags |= OutcomeVictimEnded;
    if (std::memcmp(&result, &binding.state.outcome, sizeof(result)) == 0) return;
    NativeVictimSubmission observation{};
    observation.slot = slot; observation.source = source; observation.sourceCycle = linked;
    observation.pairRevision = binding.state.interactionPairRevision;
    observation.sourceSerial = binding.serial; observation.victimBefore = binding.victim;
    observation.outcome = result;
    RecordOutcome(observation);
}
// Interior boundary 00662C61: the packet argument is already on the stack,
// ECX is the victim, EDX its virtual +2C dispatcher. The shim preserves the
// retail SEH state write, CALL/RET 4 stack effect and callee-saved registers.
__declspec(naked) void PairedVictimSubmitShim() {
    __asm {
        mov dword ptr [esp + 0F0h], ebx
        call PairedVictimSubmitDetour
        jmp dword ptr [gPairedVictimSubmitContinue]
    }
}
bool RemoteVictimPresentationReady(const std::uintptr_t victim) noexcept {
    PairIdentity binding{};
    std::uintptr_t graph{}, controller{};
    std::uint8_t enabled{}, playerController{};
    if (!RegisteredPlayerRepresentation(victim) || !PublishedPairParticipant(victim, binding) ||
        binding.animated == 0U || !Read(binding.animated + 0x2CF4U, enabled) || enabled == 0U ||
        !Read(binding.animated + 0x34BCU, graph) || graph == 0U ||
        !Read(victim + 0x31CU, controller)) return false;
    return controller == 0U || (Read(controller + 0x48U, playerController) && playerController == 0U);
}
bool RemoteVictimPairPresent(const std::uintptr_t victim, const std::uintptr_t nativeCycle) noexcept {
    std::uintptr_t victimCycle{}, source{}, sourceCycle{}, current{}, owner{}, target{}, table{};
    std::uint32_t birth{}, cachedBirth{}, a{}, b{};
    std::uint8_t state{};
    PairIdentity sourceIdentity{}, victimIdentity{};
    if (!PublishedPairParticipant(victim, victimIdentity) ||
        !Read(nativeCycle + 4U, owner) || !Read(nativeCycle + 0x1AU, state)) return false;
    if (state == 0x4EU && owner != victim) {
        // Source destructor: the victim can already be in its death/idle cycle.
        // ESI still supplies the exact source whose native flags are cleaned.
        source = owner; sourceCycle = nativeCycle;
    } else if (state == 0x4FU && owner == victim) {
        if (!Read(victim + 0x36CU, victimCycle) || victimCycle != nativeCycle ||
            !Read(nativeCycle, table) || table != gBase + 0x00EAE690U ||
            !Read(nativeCycle + 0x68U, source) ||
            !Read(nativeCycle + 0x6CU, cachedBirth) ||
            !Read(nativeCycle + 0xB0U, sourceCycle)) return false;
    } else return false;
    if (PublishedPairSource(source, sourceIdentity) == kActors ||
        (state == 0x4FU && cachedBirth != sourceIdentity.runtimeId) ||
        !Read(source + 0x36CU, current) || current != sourceCycle ||
        !Read(sourceCycle + 4U, owner) || owner != source ||
        !Read(sourceCycle + 0x1AU, state) || state != 0x4EU ||
        !Read(sourceCycle, table) || table != gBase + 0x00EAE5D0U ||
        !Read(sourceCycle + 0x44U, target) || target != victim ||
        !Read(sourceCycle + 0x48U, birth) || birth != victimIdentity.runtimeId ||
        !Read(sourceCycle + 0x100U, a) || a != victimIdentity.entityA ||
        !Read(sourceCycle + 0x104U, b) || b != victimIdentity.entityB) return false;
    return true;
}
bool LocalVictimInitializationReady(const std::uintptr_t victim, const std::uintptr_t nativeCycle) noexcept {
    PairIdentity binding{};
    std::uintptr_t source{}, sourceCycle{}, graph{}, controller{};
    std::uint8_t enabled{}, player{}, state{};
    // Only the original 4F initializer inside a leased Client admission. The
    // real player's destructors and unrelated execution predicates stay retail.
    return LocalClientVictim(victim) && PublishedPairParticipant(victim, binding) &&
        binding.animated != 0U && Read(binding.animated + 0x2CF4U, enabled) && enabled != 0U &&
        Read(binding.animated + 0x34BCU, graph) && graph != 0U &&
        Read(victim + 0x31CU, controller) && controller != 0U &&
        Read(controller + 0x48U, player) && player != 0U &&
        Read(nativeCycle + 0x1AU, state) && state == 0x4FU &&
        Read(nativeCycle + 0x68U, source) && Read(nativeCycle + 0xB0U, sourceCycle) &&
        VictimAdmissionMatches(victim, source, sourceCycle);
}
std::uint32_t __fastcall PairedVictimAvailabilityDetour(void* logical, void*, const std::uintptr_t nativeCycle) noexcept {
    const auto retail = reinterpret_cast<TargetEligibility>(gBase + 0x0038B330U)(logical);
    if (retail == 0U) return retail;
    std::uintptr_t victim{};
    if (!Read(reinterpret_cast<std::uintptr_t>(logical), victim) ||
        reinterpret_cast<std::uintptr_t>(logical) != victim + 0x280U ||
        (!RemoteVictimPresentationReady(victim) && !LocalVictimInitializationReady(victim, nativeCycle)) ||
        !RemoteVictimPairPresent(victim, nativeCycle)) return retail;
    // Only the victim initializer and the two paired destructors call here.
    // A loaded remote presentation actor may have proxy execution phase 1/2/3;
    // permit its native victim/death graph, never its autonomous brain.
    gPairedVictimAvailabilityOverrides.fetch_add(1U, std::memory_order_relaxed);
    if (LocalClientVictim(victim))
        gPairedVictimLocalAvailabilityOverrides.fetch_add(1U, std::memory_order_relaxed);
    return 0U;
}
// All three certified callers keep their current paired cycle in ESI.
// ECX-only predicate: pass ESI to the adaptation, consume it with RET 4,
// then return to the original caller with its stack unchanged.
__declspec(naked) void PairedVictimAvailabilityShim() {
    __asm {
        push esi
        call PairedVictimAvailabilityDetour
        ret
    }
}
void TracePairedGraph(const std::uintptr_t source, const std::uintptr_t victim,
    const std::uint32_t event, const std::uint32_t side, const std::uint32_t phase,
    const animation::Observation* observation = nullptr) noexcept {
    PairIdentity identity{};
    if (PublishedPairSource(source, identity) == kActors || !PairIdentityLive(identity)) return;
    PairedGraphProbe sample{};
    sample.tick = GetTickCount64(); sample.event = event; sample.side = side; sample.phase = phase;
    sample.role = static_cast<std::uint32_t>(gRole.load(std::memory_order_acquire));
    sample.generation = gActiveWorldGeneration.load(std::memory_order_acquire);
    sample.sourceA = identity.entityA; sample.sourceB = identity.entityB;
    sample.thread = GetCurrentThreadId();
    static_cast<void>(Read(source + 0x1004U, sample.sourceAlignment));
    if (victim != 0U) static_cast<void>(Read(victim + 0x1004U, sample.victimAlignment));
    if (observation != nullptr) {
        sample.cycle = observation->cycle; sample.graph = observation->graph;
        sample.callerRva = observation->callerRva; sample.markerIndex = observation->markerIndex;
        static_cast<void>(Read(observation->cycle + 0x19U, sample.serial));
    }
    std::uint32_t victimType{};
    static_cast<void>(CharacterIdentity(victim, sample.victimA, sample.victimB, victimType));
    OutcomeBinding binding{};
    if (LoadOutcome(source, victim, binding)) {
        sample.peer = binding.state.interactionTargetPeer;
        sample.pairRevision = binding.state.interactionPairRevision; sample.birth = binding.state.birth;
    }
    std::uintptr_t sourceCycle{}, victimCycle{}, owner{};
    sample.readable = Read(source + 0x36CU, sourceCycle) && sourceCycle != 0U &&
        Read(sourceCycle + 4U, owner) && owner == source &&
        Read(sourceCycle + 0x1AU, sample.sourceState);
    if (sample.readable && sample.sourceState == 0x4EU) {
        std::uintptr_t control{};
        sample.endPredicateReadable=Read(sourceCycle+8U,sample.sourceFlags)&&
            Read(sourceCycle+0xE5U,sample.sourceE5)&&victim!=0U&&Read(victim+0x31CU,control);
        if(sample.endPredicateReadable&&control)sample.endPredicateReadable=Read(control+0x48U,sample.victimControlActive);
        sample.readable = Read(sourceCycle + 0xE0U, sample.sourceStage) &&
            Read(sourceCycle + 0xE6U, sample.sourceFinished);
    }
    if (victim != 0U && Read(victim + 0x36CU, victimCycle) && victimCycle != 0U &&
        Read(victimCycle + 4U, owner) && owner == victim &&
        Read(victimCycle + 0x1AU, sample.victimState) && sample.victimState == 0x4FU) {
        static_cast<void>(Read(victimCycle+8U,sample.victimFlags));
        static_cast<void>(Read(victimCycle+0xDAU,sample.victimInteractive));
        sample.readable = sample.readable && Read(victimCycle + 0xD8U, sample.victimEnded) &&
            Read(victimCycle + 0xD9U, sample.victimAcquired);
    }
    std::unique_lock lock(gPairedGraphMutex, std::try_to_lock);
    if (!lock.owns_lock() || gPairedGraphWrite - gPairedGraphRead >= kParticipantProbeRing) {
        gPairedGraphDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    gPairedGraphProbes[gPairedGraphWrite++ % kParticipantProbeRing] = sample;
}

void ObservePairedAnimation(const animation::Observation& observation) noexcept {
    // Animation workers read published lifetimes only. Periodic source updates
    // are sampled; every marker boundary is kept in the existing bounded ring.
    if (observation.boundary == animation::Boundary::SourceUpdateEnter) return;
    if (observation.boundary == animation::Boundary::SourceUpdateLeave) {
        static std::array<std::atomic<ULONGLONG>, kActors> last{};
        PairIdentity identity{};
        const auto slot = PublishedPairSource(observation.source, identity);
        if (slot == kActors) return;
        const auto now = GetTickCount64();
        auto before = last[slot].load(std::memory_order_relaxed);
        if (now - before < 250U || !last[slot].compare_exchange_strong(before, now,
            std::memory_order_relaxed)) return;
    }
    TracePairedGraph(observation.source, observation.victim, observation.marker, 4U,
        static_cast<std::uint32_t>(observation.boundary), &observation);
}
std::atomic<std::uint32_t> gEndActionEvent{};
std::atomic<std::uint64_t> gRemoteVictimEndRetained{};
std::uint32_t EndActionEvent() noexcept {
    auto value=gEndActionEvent.load(std::memory_order_acquire);
    if(value)return value;
    // The same native hash 00664A57 computes for its "End_Action" literal.
    __try {
        value=reinterpret_cast<std::uint32_t(__cdecl*)(const char*)>(gBase+0x00002B50U)(
            reinterpret_cast<const char*>(gBase+0x00EAE854U));
    } __except(EXCEPTION_EXECUTE_HANDLER){value=0U;}
    if(value)gEndActionEvent.store(value,std::memory_order_release);
    return value;
}
// 00664A10 End_Action: retail keeps the source cycle while its victim is the
// player CONTROLLED on this machine (victim+31C, controller+48, E5==0). On
// the Host that victim is the Host's own player; on a Client it is that same
// remote human player's representation, whose controller is not local, so
// retail would take its end branch instead. Retain until the Host stream
// leaves the pair, then forward the observed End_Action on the engine thread.
bool RemotePlayerVictimRetains(const std::uintptr_t cycle,const std::uintptr_t victim) noexcept {
    if(gRole.load(std::memory_order_acquire)!=Role::Client||!victim||!RegisteredPlayerRepresentation(victim)||
        LocalClientVictim(victim))return false;
    std::uint8_t state{},e5{},enabled{};std::uintptr_t table{};
    return Read(cycle,table)&&table==gBase+0x00EAE5D0U&&Read(cycle+0x1AU,state)&&state==0x4EU&&
        Read(cycle+0xE5U,e5)&&e5==0U&&Read(gBase+0x012F0C8CU,enabled)&&enabled!=0U;
}
// The same 00664A10 retain branch, taken natively on a Client whose OWN player
// is the victim: retail keeps the source until that player's game over, which
// multiplayer suppresses, so the Alien stayed in 4E forever once the Host's
// source had long left the pair. The Host decides when it ends.
std::atomic<std::uint64_t> gLocalVictimEndRetained{},gLocalVictimEndsFinished{};
bool LocalPlayerVictimRetains(const std::uintptr_t cycle,const std::uintptr_t victim) noexcept {
    if(!LocalClientVictim(victim))return false;
    std::uint8_t state{},e5{},flags{};std::uintptr_t table{};
    return Read(cycle,table)&&table==gBase+0x00EAE5D0U&&Read(cycle+0x1AU,state)&&state==0x4EU&&
        Read(cycle+0xE5U,e5)&&e5==0U&&Read(cycle+8U,flags)&&(flags&6U)==0U;
}
// 00664A10's end branch with E5==0 is exactly flags|=6 (FUN_0064EB90 runs
// only when E5!=0). Forwarding End_Action again would re-take the retain
// branch for a locally controlled victim.
bool FinishRetainedLocalVictimEnd(const std::uintptr_t cycle) noexcept {
    __try {
        auto& flags=*reinterpret_cast<volatile std::uint8_t*>(cycle+8U);
        flags=static_cast<std::uint8_t>(flags|6U);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void __fastcall SourceGraphEventDetour(void* context, void*, const std::uint32_t event) noexcept {
    using Event = void(__thiscall*)(void*, std::uint32_t);
    const auto original = reinterpret_cast<Event>(gSourceGraphEventOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{}, victim{};
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    if (Read(cycle + 4U, source) && Read(cycle + 0x44U, victim))
        TracePairedGraph(source, victim, event, 1U, 0U);
    PairTransactionEvent notification{};
    const bool tracked=CapturePairTransactionEvent(source,victim,cycle,false,interactions::FinishCause::NativeGraphEnd,notification);
    if (event != 0U && event == EndActionEvent() && RemotePlayerVictimRetains(cycle, victim)) {
        // Exactly the native retain branch: End_Action has no other effect.
        // Retain the marker as a value event too: after the Host leaves this
        // pair, the engine thread must finish the deferred native end branch.
        if(tracked){notification.retainedEndAction=true;QueuePairTransactionEvent(notification);}
        gRemoteVictimEndRetained.fetch_add(1U, std::memory_order_relaxed);
        TracePairedGraph(source, victim, event, 1U, 1U);
        return;
    }
    original(context, event);
    std::uint8_t ended{};
    if(tracked&&Read(cycle+8U,ended)) {
        if((ended&6U)==6U)QueuePairTransactionEvent(notification);
        else if(event!=0U&&event==EndActionEvent()&&LocalPlayerVictimRetains(cycle,victim)) {
            // Retail itself took its retain branch: the victim is this
            // Client's own player. Remember the marker exactly as above.
            notification.retainedEndAction=true;QueuePairTransactionEvent(notification);
            gLocalVictimEndRetained.fetch_add(1U,std::memory_order_relaxed);
        }
    }
    TracePairedGraph(source, victim, event, 1U, 1U);
}
void __fastcall VictimGraphEventDetour(void* context, void*, const std::uint32_t event) noexcept {
    using Event = void(__thiscall*)(void*, std::uint32_t);
    const auto original = reinterpret_cast<Event>(gVictimGraphEventOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{}, victim{};
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    if (Read(cycle + 4U, victim) && Read(cycle + 0x68U, source))
        TracePairedGraph(source, victim, event, 2U, 0U);
    original(context, event);
    TracePairedGraph(source, victim, event, 2U, 1U);
}
void __fastcall SourceGraphFinishDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(gSourceGraphFinishOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{}, victim{};
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    if (Read(cycle + 4U, source) && Read(cycle + 0x44U, victim))
        TracePairedGraph(source, victim, 0U, 3U, 0U);
    PairTransactionEvent notification{};
    const bool tracked=CapturePairTransactionEvent(source,victim,cycle,false,interactions::FinishCause::NativeSourceFinish,notification);
    original(context);
    if(tracked)QueuePairTransactionEvent(notification);
    TracePairedGraph(source, victim, 0U, 3U, 1U);
}
void __fastcall VictimGraphFinishDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(
        gVictimGraphFinishOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{}, source{}, current{}, linked{}, table{}, owner{};
    std::uint8_t acquired{}, interactive{}, serial{};
    std::uint32_t sourceBirth{};
    PairTransactionEvent notification{};
    const bool tracked=Read(cycle+4U,victim)&&Read(cycle+0x68U,source)&&
        CapturePairTransactionEvent(source,victim,cycle,true,interactions::FinishCause::NativeVictimFinish,notification);
    OutcomeBinding observed{};
    // Capture while this exact victim cycle is still alive: the native finish
    // may synchronously replace it. No Actor or engine-owned table is used by
    // this animation-worker callback.
    const bool observe = gRole.load(std::memory_order_acquire) == Role::Client &&
        Read(cycle + 4U, victim) && Read(cycle + 0x68U, source) &&
        LoadOutcome(source, victim, observed, true) && observed.claimed &&
        !LocalClientVictim(victim) && Read(victim + 0x36CU, current) && current == cycle &&
        Read(cycle, table) && table == gBase + 0x00EAE690U &&
        Read(cycle + 0xD9U, acquired) && acquired != 0U &&
        Read(cycle + 0xDAU, interactive) && interactive == 0U &&
        Read(cycle + 0x6CU, sourceBirth) && sourceBirth == observed.source.runtimeId &&
        Read(cycle + 0xB0U, linked) && linked != 0U &&
        Read(source + 0x36CU, current) && current == linked &&
        Read(linked + 4U, owner) && owner == source &&
        Read(linked + 0x19U, serial) && serial == observed.serial;
    original(context); // FUN_00655A60 sets victim+D8 and calls source finish.
    if(tracked)QueuePairTransactionEvent(notification);
    if (!observe || !PairIdentityLive(observed.source) || !PairIdentityLive(observed.victim)) return;
    PairIdentity identity{};
    const auto slot = PublishedPairSource(source, identity);
    if (slot == kActors) return;
    std::lock_guard lock(gOutcomeMutex);
    auto& binding = gOutcomes[slot];
    if (binding.source.character == observed.source.character &&
        binding.source.runtimeId == observed.source.runtimeId && binding.source.epoch == observed.source.epoch &&
        binding.victim.character == observed.victim.character && binding.victim.runtimeId == observed.victim.runtimeId &&
        binding.state.birth == observed.state.birth &&
        binding.state.interactionPairRevision == observed.state.interactionPairRevision &&
        binding.state.interactionTargetPeer == observed.state.interactionTargetPeer && binding.serial == serial &&
        !binding.localVictimEnded) {
        binding.localVictimEnded = true;
        gPairedLocalVictimFinishes.fetch_add(1U, std::memory_order_relaxed);
    }
}
void FillMotion(const std::uintptr_t character,MotionTraceSample& sample) noexcept {
    sample.role=static_cast<std::uint32_t>(gRole.load(std::memory_order_acquire));sample.utcMs=UtcMs();
    sample.sampleTick=GetTickCount64();
    sample.callbackThread=GetCurrentThreadId();sample.onEngineThread=EngineThread()?1U:0U;
    if(!character)return;
    sample.poseReadable=Read(character+kPositionOffset,sample.x)&&Read(character+kPositionOffset+4U,sample.y)&&
        Read(character+kPositionOffset+8U,sample.z)&&Read(character+kYawOffset,sample.yaw);
    std::uintptr_t cycle{},type{};std::uint8_t state{};
    if(Read(character+0x36CU,cycle)&&cycle&&Read(cycle,type)&&type>gBase&&Read(cycle+0x1AU,state)){
        sample.cycleType=static_cast<std::uint32_t>(type-gBase);sample.cycleState=state;
    }
    std::uintptr_t head{},first{},request{};std::uint32_t count{},kind{};
    if(Read(character+0x2CD4U,head)&&head&&Read(character+0x2CD8U,count)&&count&&Read(head,first)&&first&&first!=head&&
        Read(first+8U,request)&&request&&Read(request+0x10U,kind))sample.headKind=kind;
    sample.headLocal=static_cast<std::uint32_t>(request);
    if(request&&Read(request+0xCU,sample.requestStatus))sample.navigationReadMask|=8U;
    // Worker callbacks copy only borrowed native values while inside their
    // real call. No mutable producer/executor tables are accessed off-engine.
    if(!sample.onEngineThread)return;
    sample.localMoveRestarts=execution::UpdatesAfterLocalEnd();
    // N+40 is the current planner block, and is often null after installation.
    // Inspect the actual scheduler head independently; never label a missing
    // planner pointer as phase0 of the retained Move. Read only on-engine.
    std::uintptr_t requestType{};
    if(sample.type==1U&&request&&((kind==1U&&Read(request,requestType)&&requestType==gBase+0x00EB1A70U)||
        (kind==3U&&Read(request,requestType)&&requestType==gBase+0x00EB1A50U))) {
        const auto b=request+(kind==1U?0x28U:0x1CU);std::uint8_t f90{},f91{};
        if(Read(b+0x40U,sample.headBlockState)&&Read(b+0x44U,sample.headBlockMode)&&
            Read(b+0x90U,f90)&&Read(b+0x91U,f91)&&Read(b,sample.headGoal)&&
            Read(b+0x2CU,sample.headStoppingDistance)&&Read(b+0x30U,sample.headStopDistance)&&
            (kind!=1U||Read(request+0x34U,sample.headPendingGoal))) {
            sample.headFlags90=f90;sample.headFlags91=f91;sample.navigationReadMask|=64U;
        }
    }
    std::uintptr_t movement{},block{},sentinel{},cursor{},last{};
    if(!Read(character+0x42A8U,movement)||!movement)return;
    const auto n=movement+0xC4U;
    if(Read(movement+0x8U,sample.nativeMovementMode))sample.navigationStateReadMask|=1U;
    std::uintptr_t worker{},workerControl{};
    if(Read(n+0x5CU,worker)&&Read(n+0x60U,workerControl)) {
        sample.navigationWorkerState=(worker?1U:0U)|(workerControl?2U:0U);
        sample.navigationStateReadMask|=2U;
    }
    std::uint8_t update48{},update49{};
    if(Read(n+0x48U,update48)&&Read(n+0x49U,update49)) {
        sample.navigationUpdate48=update48;sample.navigationUpdate49=update49;
        sample.navigationStateReadMask|=4U;
    }
    if(kind==1U&&(sample.navigationReadMask&64U)) {
        std::uintptr_t headTail{},headSentinel{};
        if(Read(n,headSentinel)&&headSentinel&&Read(request+0x64U,headTail)&&headTail)
            sample.headTailState=headTail==headSentinel?2U:1U;
    }
    if(Read(movement+0x194U,sample.requestedGait)&&Read(movement+0x198U,sample.effectiveGait))sample.navigationReadMask|=1U;
    if(Read(n+4U,sample.routeNodes)&&Read(n+0x14U,sample.historyNodes))sample.navigationReadMask|=2U;
    if(Read(n+0x40U,block)&&block) {
        std::uint8_t flags90{},flags91{};
        if(Read(block+0x40U,sample.blockState)&&Read(block+0x44U,sample.blockMode)&&
            Read(block+0x90U,flags90)&&Read(block+0x91U,flags91)&&
            Read(block+0x24U,sample.routeDistance)&&Read(block+0x28U,sample.routeProgress)) {
            sample.blockFlags90=flags90;sample.blockFlags91=flags91;sample.navigationReadMask|=4U;
        }
    }
    if(request&&Read(request+0xCU,sample.requestStatus))sample.navigationReadMask|=8U;
    // Bounded read-only route evidence at the existing 250ms pose cadence.
    // No native pointer is exported and no cursor/progress is modified.
    if(sample.type!=1U||sample.routeNodes>route::kMaxNodes||!Read(n,sentinel)||!sentinel||
        !Read(sentinel,first)||!Read(sentinel+4U,last)||!Read(n+0xCU,cursor))return;
    const auto point=[&](std::uintptr_t link,std::array<float,3>& out) noexcept {
        std::uintptr_t node{};return link&&link!=sentinel&&Read(link+8U,node)&&node&&
            Read(node+0xCU,out[0])&&Read(node+0x10U,out[1])&&Read(node+0x14U,out[2]);
    };
    if(point(first,sample.routeFirst)&&point(last,sample.routeLast))sample.navigationReadMask|=16U;
    if(point(cursor,sample.routeCursor))sample.navigationReadMask|=32U;
    auto at=first;
    for(std::uint32_t i=0;i<sample.routeNodes&&at&&at!=sentinel;++i) {
        if(at==cursor){sample.cursorOrdinal=i;break;}
        if(!Read(at,at))break;
    }
    if(cursor==sentinel)sample.cursorOrdinal=sample.routeNodes;
}
void FillCommand(const transactions::Record& record,MotionTraceSample& sample) noexcept {
    sample.intentContract=record.intent.words[0];sample.intentKinds=record.intent.words[1];
    sample.intentArgument0=record.intent.words[2];sample.intentArgument1=record.intent.words[3];
    sample.intentCountOffset=record.intent.words[34];sample.intentScriptFlags=record.intent.words[33];
    for(std::size_t i=12U;i<21U;++i)if(record.intent.words[i])sample.intentReferenceMask|=1U<<(i-12U);
    if(!native_intent::Empty(record.intent)) {
        sample.intentDigest=14695981039346656037ULL;
        for(auto word:record.intent.words)for(std::size_t byte=0;byte<4U;++byte) {
            sample.intentDigest^=(word>>(byte*8U))&255U;sample.intentDigest*=1099511628211ULL;
        }
    }
    sample.scope=record.scope;sample.entityA=record.owner.entityA;sample.entityB=record.owner.entityB;
    sample.birth=record.owner.birth;sample.ownerEpoch=record.owner.ownerEpoch;sample.ownerSeq=record.ownerSeq;
    sample.parameterRevision=record.parameterRevision;sample.flags=record.flags;sample.gait=record.gait;
    sample.commandId=record.commandId;sample.globalSeq=record.globalSeq;sample.opcode=static_cast<std::uint32_t>(record.opcode);
    sample.nativeKind=record.nativeKind;sample.mode=record.mode;sample.aux=record.aux;
    sample.goalX=record.goalX;sample.goalY=record.goalY;sample.goalZ=record.goalZ;sample.stopDistance=record.stopDistance;
    if(record.nativeKind==1U||record.nativeKind==2U||record.nativeKind==3U){
        sample.goalX=record.movement.after.goal[0];sample.goalY=record.movement.after.goal[1];sample.goalZ=record.movement.after.goal[2];
        sample.gait=record.movement.gait;sample.stopDistance=record.movement.after.stopDistance;
        sample.authoredFlags90=record.movement.after.flags90;sample.authoredFlags91=record.movement.after.flags91;
    }
}
void TraceHostCommand(const transactions::Record& record) noexcept {
    // Producer invokes this under its outbox mutex. Copy values only: do not
    // resolve actors, call CATHODE, or reenter Producer from this callback.
    MotionTraceSample sample{};sample.type=2U;sample.role=1U;sample.utcMs=UtcMs();sample.sampleTick=GetTickCount64();
    FillCommand(record,sample);
    RecordMotionTrace(sample);
}
void TraceClientCommand(const transactions::Record& record,const std::uintptr_t character,const std::uint32_t outcome) noexcept {
    if(auto* actor=Find(character);actor&&execution::SupersedesNativePassage(actor->replayedCycle,
        actor->passageSequence,actor->passageCommand,record,static_cast<transactions::NativeOutcome>(outcome))) {
        actor->passageSequence=actor->passageCommand=0U;
    }
    if(record.nativeKind==2&&record.opcode==transactions::Op::Release&&outcome==static_cast<std::uint32_t>(transactions::NativeOutcome::Admitted))
        for(auto& item:gMarkerCommandBindings)if(item.used&&item.scope==record.scope&&item.owner==record.owner&&item.command==record.commandId)item={};

    MotionTraceSample sample{};sample.type=3U;
    FillMotion(character,sample);
    FillCommand(record,sample);
    sample.outcome=outcome;
    RecordMotionTrace(sample);
}
void TraceClientWait(const transactions::Record& record,const std::uint64_t age,const std::uint32_t reason,
    const transactions::ExecutionDisposition& disposition) noexcept {
    MotionTraceSample sample{};sample.type=4U;sample.role=2U;sample.utcMs=UtcMs();sample.sampleTick=GetTickCount64();
    FillCommand(record,sample);sample.waitAgeMs=age;sample.waitReason=reason;
    sample.executionStatus=static_cast<std::uint32_t>(disposition.status);sample.executionResult=static_cast<std::uint32_t>(disposition.result);
    RecordMotionTrace(sample);
}
void SamplePresentation(Actor& actor, const ULONGLONG now, const std::uint32_t phase = 0U) noexcept {
    if (EngineThread() && Stable() && actor.character != 0U && !actor.duplicate && now - actor.lastMotionSample >= 250U) {
        actor.lastMotionSample = now;
        MotionTraceSample sample{};sample.type=1U;sample.entityA=actor.entityA;sample.entityB=actor.entityB;
        sample.sessionCookie=gCurrent.sessionCookie;sample.scope.worldEpoch=gCurrent.worldGeneration;
        sample.birth=Client()?actor.remoteBirth:actor.birth;
        FillMotion(actor.character,sample);
        if(const auto viewer=LocalPlayer();viewer&&sample.poseReadable) {
            float v[3]{};
            if(Read(viewer+kPositionOffset,v[0])&&Read(viewer+kPositionOffset+4U,v[1])&&Read(viewer+kPositionOffset+8U,v[2])) {
                const float d=std::sqrt((v[0]-sample.x)*(v[0]-sample.x)+(v[1]-sample.y)*(v[1]-sample.y)+(v[2]-sample.z)*(v[2]-sample.z));
                if(std::isfinite(d))sample.viewerDistanceCm=static_cast<std::uint32_t>(std::min(d,100000.0F)*100.0F);
            }
        }
        if(Client()) {
            sample.hostCycleType=(actor.received.stateFlags&ExecutionValid)!=0U?actor.received.executionCycleTypeRva:0U;
            sample.hostCycleState=(actor.received.stateFlags&ExecutionValid)!=0U?actor.received.executionCycleState:0U;
        }
        RecordMotionTrace(sample);
    }
    if (!EngineThread() || !Stable() || actor.character == 0U || actor.duplicate) return;
    if (phase == 0U && now - actor.lastPresentationProbe < kParticipantProbePeriod) return;
    actor.lastPresentationProbe = now;
    PresentationProbe sample{};
    const auto& state = actor.desired;
    sample.sessionCookie = gCurrent.sessionCookie; sample.tick = now; sample.utcMs = UtcMs();
    sample.nativeGeneration = gCurrent.nativeGeneration; sample.worldGeneration = gCurrent.worldGeneration;
    sample.role = static_cast<std::uint32_t>(gRole.load(std::memory_order_acquire)); sample.phase = phase;
    sample.entityA = actor.entityA; sample.entityB = actor.entityB;
    sample.sourceCharacter = actor.character; sample.sourceRuntimeId = actor.runtimeId;
    sample.birth = Client() ? actor.remoteBirth : actor.birth; sample.revision = state.revision;
    sample.pairRevision = state.interactionPairRevision; sample.peer = state.interactionTargetPeer;
    sample.outcome = state.outcome;
    sample.interactionFlags = state.interactionFlags;
    sample.lifecycleFlags = state.lifecycleFlags; sample.ownsPair = actor.interactionOwnsCycle;
    sample.observerReleasedPairRevision = actor.observerReleasedPairRevision;
    sample.navigation = state.navigation;
    sample.navigationHead = navigation::InspectHead(actor.character);
    sample.movementOwner = static_cast<std::uint32_t>(actor.movementOwner);
    sample.authorityMoveType = state.moveType; sample.authorityIntensity = state.locomotionIntensity;
    if(Client()) {
        const auto follower=character_direct_step_runtime::Snapshot(actor.character);
        sample.hybridHold=0U;sample.hybridNativeArmed=follower.armed;
        sample.hybridSampleAge=0U;
        sample.hybridNativeHooks=follower.hookHits;sample.hybridNativeWrites=follower.requestApplications;
        sample.hybridNativeFailures=follower.protectedFailures;
        sample.hybridProbe=follower.hybridProbe;sample.hybridBranches=follower.hybridBranches;
    }
    std::uintptr_t movement{};
    if (Read(actor.character + 0x42A8U, movement) && movement != 0U) {
        static_cast<void>(Read(movement + 0x194U, sample.requestedMoveType));
        static_cast<void>(Read(movement + 0x198U, sample.effectiveMoveType));
    }
    sample.observerHandoffNeutral = actor.observerHandoffNeutral;
    sample.placementPending = actor.placementPending;
    sample.pairPlacementPending = actor.pairPlacementPending;
    sample.positionReadable = true;
    for (std::size_t i = 0U; i < 3U; ++i) {
        const bool readable = Read(actor.character + kPositionOffset + i * sizeof(float), sample.local[i]) &&
            FiniteCoordinate(sample.local[i]);
        sample.positionReadable = sample.positionReadable && readable;
        sample.authority[i] = state.position[i]; sample.authorityVictim[i] = state.interactionTargetPosition[i];
    }
    // +2DA is backstage, +2D9 is independent render invisibility (including
    // NPC_SetInvisible). A frontstage actor can still be invisible.
    sample.invisibleReadable = Read(actor.character + 0x2D9U, sample.invisible);
    sample.visibilityReadable = Read(actor.character + 0x2DAU, sample.hidden) &&
        Read(actor.character + 0x2FAU, sample.enabled);
    std::uintptr_t cycle{}, table{};
    sample.cycleReadable = Read(actor.character + 0x36CU, cycle) && cycle != 0U &&
        Read(cycle + 0x1AU, sample.cycleState) && Read(cycle, table) && table >= gBase;
    if (sample.cycleReadable) {
        sample.cycleType = static_cast<std::uint32_t>(table - gBase);
        static_cast<void>(Read(cycle + 8U, sample.cycleFlags));
        if (sample.cycleState == 0x4EU && table == gBase + 0x00EAE5D0U) {
            static_cast<void>(Read(cycle + 0xE0U, sample.stage));
            std::uintptr_t graph{};
            sample.sourceGraphPresent = Read(cycle + 0x14U, graph) && graph != 0U;
        }
    }
    std::uintptr_t withdraw{};
    static_cast<void>(ResolveAlienWithdrawContext(actor.character, withdraw, sample.withdrawState));
    const auto target = (state.interactionFlags & InteractionTargetIsParticipant) != 0U
        ? ParticipantForPeer(state.interactionTargetPeer)
        : FindCharacterByIdentity(state.interactionTargetA, state.interactionTargetB);
    sample.victimReadable = target != 0U;
    sample.victimCharacter = target; sample.localCharacter = LocalPlayer();
    sample.victimLocal = target != 0U && target == sample.localCharacter;
    sample.victimRepresentation = RegisteredPlayerRepresentation(target);
    static_cast<void>(Read(target + 0x50U, sample.victimRuntimeId));
    static_cast<void>(Read(target + 0x54U, sample.victimEntityA));
    static_cast<void>(Read(target + 0x58U, sample.victimEntityB));
    static_cast<void>(Read(sample.localCharacter + 0x50U, sample.localRuntimeId));
    std::uint16_t localHp{};
    sample.localHealthReadable = sample.localCharacter != 0U &&
        Read(sample.localCharacter + 0x33CU, localHp) &&
        Read(sample.localCharacter + 0x32CU, sample.localHealthState);
    sample.localHp = localHp;
    std::uintptr_t victimCycle{}, victimTable{};
    if (target != 0U && Read(target + 0x36CU, victimCycle) && victimCycle != 0U &&
        Read(victimCycle, victimTable) && victimTable >= gBase &&
        Read(victimCycle + 0x1AU, sample.victimCycleState)) {
        sample.victimCycleType = static_cast<std::uint32_t>(victimTable - gBase);
        if (victimTable == gBase + 0x00EAE690U && sample.victimCycleState == 0x4FU) {
            static_cast<void>(Read(victimCycle + 0xD8U, sample.victimEnded));
            static_cast<void>(Read(victimCycle + 0xD9U, sample.victimAcquired));
            static_cast<void>(Read(victimCycle + 0xDAU, sample.victimInteractive));
        }
    }
    sample.victimHealthReadable = target != 0U && Read(target + 0x33CU, sample.victimHp) &&
        Read(target + 0x32CU, sample.victimHealthState);
    for (std::size_t i = 0U; i < 3U; ++i) {
        const bool readable = target != 0U && Read(target + kPositionOffset + i * sizeof(float), sample.victim[i]) &&
            FiniteCoordinate(sample.victim[i]);
        sample.victimReadable = sample.victimReadable && readable;
    }
    const auto write = gPresentationProbeWrite.load(std::memory_order_relaxed);
    if (write - gPresentationProbeRead.load(std::memory_order_acquire) >= kParticipantProbeRing) {
        gPresentationDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    gPresentationProbes[write % kParticipantProbeRing] = sample;
    gPresentationProbeWrite.store(write + 1U, std::memory_order_release);
    gPresentationSamples.fetch_add(1U, std::memory_order_relaxed);
}
void ReleaseNativeFollow(Actor& actor,bool cancel=true) noexcept;
bool Release(Actor& actor) noexcept;
Actor* Admit(const std::uintptr_t c) noexcept {
    // CharacterCreate may run a manager update before its detour can publish
    // the resulting address. The pre-reserved spawn key rejects that update;
    // the address check also retires an actor admitted by an older build/path.
    if (PlayerRepresentation(c)) {
        if (auto* actor = Find(c); actor != nullptr) {
            // CharacterDirectStep belongs to PlayerSync for this object. Do not
            // call Release(), which would stop the remote-player controller.
            *actor = {};
            gProvisionalPlayerActorsRetired.fetch_add(1U, std::memory_order_relaxed);
        }
        return nullptr;
    }
    if (auto* actor = Find(c); actor != nullptr) {
        std::uint32_t id{};
        std::uintptr_t tree{};
        if (!Read(c + 0x50U, id) || !Read(c + 0x34D8U, tree)) return nullptr;
        if (id == actor->runtimeId) {
            if (tree != actor->tree) {
                static_cast<void>(Release(*actor));
                actor->tree = tree; actor->completedRevision = 0U;
                std::uintptr_t owner{};
                actor->faulted = tree == 0U || !Read(tree + 0xCU, owner) || owner != c;
                if (!actor->faulted && Client()) actor->faulted = !SuspendExistingTree(*actor);
                if (actor->faulted) gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed);
            }
            PublishPairSource(*actor);
            return actor;
        }
        // Address reuse without a destruction notification: do not touch a
        // request belonging to the old Character. Keep this visible in diagnostics.
        *actor = {};
    }
    std::uint32_t type{}, id{}, a{}, b{};
    std::uintptr_t tree{};
    if (!Eligible(c, type)) return nullptr;
    if (!Read(c + 0x50U, id) || id == 0U ||
        !Read(c + 0x54U, a) || !Read(c + 0x58U, b) || (a == 0U && b == 0U)) {
        gIdentityRejected.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    if (!Read(c + 0x34D8U, tree) || tree == 0U) {
        gTreeUnavailable.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    std::uintptr_t owner{};
    if (!Read(tree + 0xCU, owner) || owner != c) {
        gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    for (auto& actor : gActors) {
        if (actor.character != 0U) continue;
        actor = {};
        actor.character = c; actor.tree = tree; actor.runtimeId = id;
        actor.entityA = a; actor.entityB = b; actor.characterClass = type;
        actor.birth = gNextBirth++;
        gAdmitted.fetch_add(1U, std::memory_order_relaxed);
        gLastEntityA.store(a, std::memory_order_relaxed);
        gLastEntityB.store(b, std::memory_order_relaxed);
        gLastCharacterClass.store(type, std::memory_order_relaxed);
        actor.desired.nativeGeneration = gCurrent.nativeGeneration;
        actor.desired.sessionCookie = gCurrent.sessionCookie;
        actor.desired.worldGeneration = gCurrent.worldGeneration;
        actor.desired.entityA = a; actor.desired.entityB = b;
        actor.desired.characterClass = type; actor.desired.birth = actor.birth;
        actor.desired.revision = 1U;
        if (Client()) actor.faulted = !SuspendExistingTree(actor);
        for (auto& other : gActors) {
            if (&other != &actor && other.character != 0U &&
                other.entityA == a && other.entityB == b) {
                actor.duplicate = true; other.duplicate = true;
                InvalidatePairCharacter(other.character);
                gRefused.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        PublishPairSource(actor);
        return &actor;
    }
    gRefused.fetch_add(1U, std::memory_order_relaxed);
    return nullptr;
}
Actor* FindActorByIdentity(const std::uint32_t entityA, const std::uint32_t entityB) noexcept {
    for (auto& actor : gActors)
        if (actor.character != 0U && !actor.duplicate &&
            actor.entityA == entityA && actor.entityB == entityB) return &actor;
    return nullptr;
}
bool HostExecutionScopeQualified(const transactions::Scope&,std::uint32_t) noexcept;
void CaptureInteraction(const std::uint32_t sourceA, const std::uint32_t sourceB,
    const std::uint32_t targetA, const std::uint32_t targetB,
    const std::uint64_t payload, const std::int32_t variant) noexcept {
    if ((sourceA == 0U && sourceB == 0U) || (targetA == 0U && targetB == 0U) ||
        variant < -1 || variant > 0xFFFF) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    // AISync authors only interactions initiated by an admitted authoritative
    // NPC. Player-originated impacts keep their existing Player/World channels.
    Actor* actor = FindActorByIdentity(sourceA, sourceB);
    if (actor == nullptr) {
        const auto source = FindCharacterByIdentity(sourceA, sourceB);
        if (source != 0U) actor = Admit(source);
    }
    if (actor == nullptr || actor->faulted || actor->duplicate) return;
    // Flush the prior pair's immutable result before replacing its identity.
    // Preparation callbacks have released their source lease by this boundary.
    CaptureOutcome(*actor);
    if (actor->desired.outcome.revision != 0U) {
        std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            auto& binding = gOutcomes[static_cast<std::size_t>(actor - gActors.data())];
            if ((binding.state.outcome.flags & OutcomeSourceEnded) == 0U) {
                binding.state.outcome.flags |= OutcomeSourceEnded;
                ++binding.state.outcome.revision;
            }
            actor->desired.outcome = binding.state.outcome;
            actor->desired.interactionFlags &= ~(InteractionCycleActive | InteractionSourceCycleRetained);
        }
        actor->lastSend = 0U;
    }
    Publish(*actor);
    const auto mappedTargetA = targetA;
    const auto mappedTargetB = targetB;
    const auto target = FindCharacterByIdentity(mappedTargetA, mappedTargetB);
    if (target == 0U || target == actor->character) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    // Participant identity is pointer ownership, never Character class. The
    // local player is the engine-resolved THE_PLAYER Character; remote humans
    // are only the generation-certified PlayerSync representations. This also
    // keeps Android classes on the ordinary NPC identity path.
    const bool participant = target == LocalPlayer() || RegisteredPlayerRepresentation(target);
    const auto targetPeer = participant ? PeerForParticipant(target) : 0U;
    if (participant && targetPeer == 0U) {
        gParticipantTargetRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    PairIdentity victimIdentity{};
    if (!participant) {
        auto* victimActor = Admit(target);
        if (victimActor == nullptr || victimActor->faulted || victimActor->duplicate) return;
        PublishPairSource(*victimActor);
    }
    if (!PublishedPairVictim(target, victimIdentity)) return;
    float position[3]{};
    if (!Read(target + kPositionOffset, position[0]) ||
        !Read(target + kPositionOffset + 4U, position[1]) ||
        !Read(target + kPositionOffset + 8U, position[2]) ||
        !FiniteCoordinate(position[0]) || !FiniteCoordinate(position[1]) ||
        !FiniteCoordinate(position[2])) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    transactions::Scope cause{};
    const bool sealed=host_producer::HostScope(cause)&&HostExecutionScopeQualified(cause,gCurrent.nativeGeneration);
    if(sealed&&gNextPairCommand==UINT64_MAX){host_producer::RetireHost();return;}
    auto& state = actor->desired;
    std::fill(std::begin(state.interactionSession),std::end(state.interactionSession),std::uint8_t{});
    state.interactionAuthority=state.interactionCommand=0U;
    if(sealed) {
        std::copy(cause.session.begin(),cause.session.end(),std::begin(state.interactionSession));
        state.interactionAuthority=cause.authorityEpoch;state.interactionCommand=++gNextPairCommand;
    }
    state.outcome = {};
    ++state.interactionRevision;
    if (state.interactionRevision == 0U) state.interactionRevision = 1U;
    state.interactionTargetA = mappedTargetA;
    state.interactionTargetB = mappedTargetB;
    state.interactionFlags = InteractionActorIsSource | InteractionPairValid;
    if (participant) state.interactionFlags |= InteractionTargetIsParticipant;
    state.interactionTargetPeer = targetPeer;
    state.interactionTargetBirth = participant ? 0U : victimIdentity.authoritativeBirth;
    state.interactionParticipantIncarnation=participant?victimIdentity.participantIncarnation:0U;
    state.interactionTargetClass = participant ? 0U : victimIdentity.type;
    state.interactionPairRevision = state.interactionRevision;
    state.interactionTransitionRevision = 0U;
    state.interactionTransition = 0U;
    state.interactionTransitionFlags = 0U;
    state.interactionTransitionVariant = 0;
    state.interactionPayloadLow = static_cast<std::uint32_t>(payload);
    state.interactionPayloadHigh = static_cast<std::uint32_t>(payload >> 32U);
    state.interactionVariant = variant;
    for (std::size_t i = 0U; i < 3U; ++i) state.interactionTargetPosition[i] = position[i];
    state.stateFlags |= InteractionValid;
    // Publish the pair before a transition can synchronously prepare it.
    static_cast<void>(PublishOutcomeBinding(*actor, target));
    actor->lastInteractionCapture = GetTickCount64();
    actor->deadTargetSince = 0U;
    gInteractionCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastInteractionTargetA.store(mappedTargetA, std::memory_order_relaxed);
    gLastInteractionTargetB.store(mappedTargetB, std::memory_order_relaxed);
    gLastInteractionTargetMode.store(participant ? 1U : 0U, std::memory_order_relaxed);
    gLastInteractionTargetPeer.store(targetPeer, std::memory_order_relaxed);
}
Actor* FindActorByActionManager(const std::uintptr_t manager) noexcept {
    if (manager == 0U) return nullptr;
    for (auto& actor : gActors) {
        std::uintptr_t candidate{};
        if (actor.character != 0U && Read(actor.character + 0x36CU, candidate) &&
            candidate == manager) return &actor;
    }
    std::uintptr_t character{};
    static_cast<void>(ForEachCharacter([&](const std::uintptr_t candidate) noexcept {
        std::uintptr_t actionManager{};
        if (Read(candidate + 0x36CU, actionManager) && actionManager == manager) {
            character = candidate;
            return false;
        }
        return true;
    }));
    return character == 0U ? nullptr : Admit(character);
}
void CapturePairedTransition(const std::uintptr_t manager, const std::uintptr_t target,
    const std::int32_t variant, const std::uint32_t transition,
    const std::uint32_t firstFlag, const std::uint32_t secondFlag) noexcept {
    if (variant < -1 || variant > 0xFFFF || firstFlag > 0xFFU || secondFlag > 0xFFU ||
        transition == 0U || transition > 2U) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    auto* actor = FindActorByActionManager(manager);
    std::uint32_t targetA{}, targetB{}, targetType{};
    if (actor == nullptr || actor->faulted || actor->duplicate || target == actor->character ||
        !CharacterIdentity(target, targetA, targetB, targetType)) return;
    const auto now = GetTickCount64();
    auto& state = actor->desired;
    const bool augmentsPair = (state.stateFlags & InteractionValid) != 0U &&
        (state.interactionFlags & InteractionPairValid) != 0U &&
        state.interactionTargetA == targetA && state.interactionTargetB == targetB &&
        actor->lastInteractionCapture != 0U && now - actor->lastInteractionCapture <= 250ULL;
    if (!augmentsPair) {
        // A transition alone is not proof of an admitted pair. Never fabricate
        // an attack with a zero payload or infer its victim by spatial proximity.
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    ++state.interactionRevision;
    if (state.interactionRevision == 0U) state.interactionRevision = 1U;
    state.interactionTransition = transition;
    state.interactionTransitionVariant = variant;
    state.interactionTransitionRevision = state.interactionRevision;
    state.interactionFlags |= InteractionCycleActive | InteractionSourceCycleRetained;
    std::uint8_t nativeCycleState{};
    std::uintptr_t nativeCycleTable{};
    if (Read(manager + 0x1AU, nativeCycleState) && nativeCycleState == 0x4EU &&
        Read(manager, nativeCycleTable) && nativeCycleTable == gBase + 0x00EAE5D0U)
        state.interactionFlags |= InteractionNativeOutcomeRequired;
    state.interactionTransitionFlags = (firstFlag & 0xFFU) | ((secondFlag & 0xFFU) << 8U);
    actor->lastInteractionCapture = now;
    static_cast<void>(PublishOutcomeBinding(*actor, target));
    gPairedTransitionCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastInteractionTransition.store(transition, std::memory_order_relaxed);
    gLastInteractionTargetA.store(targetA, std::memory_order_relaxed);
    gLastInteractionTargetB.store(targetB, std::memory_order_relaxed);
}
bool CharacterReady(const Actor& actor) noexcept {
    std::uint8_t alive{};
    if (!Read(actor.character + 0x2FAU, alive) || alive == 0U) return false;
    __try {
        // Exactly the admission guard in FUN_007342A0; prevents proxy requests
        // during death/disabled-character/script exclusivity states.
        return reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(gBase + 0x0038B380U)(
            reinterpret_cast<void*>(actor.character + 0x280U)) == 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Release(Actor& actor) noexcept {
    if (!UnownedCleanupAllowed(actor)) return false;
    RestoreAlienSuppression(actor);
    if (PairSourceBusy(actor.character)) return true;
    combat::Cancel(actor.character);
    actor.convergenceActive = false; actor.convergenceDivergenceSince = 0U;
    actor.pursuitActive = false; actor.pursuitDivergenceSince = 0U;
    actor.movementOwner = MovementOwner::None;
    actor.replayedExecutionRevision = 0U;
    actor.completedExecutionRevision = 0U;
    actor.lastExecutionAttempt = 0U;
    actor.rejectedNavigationRevision = actor.rejectedNavigationRequestRevision = 0U;
    actor.lastNavigationAttempt = 0U;
    actor.poseCount = 0U; actor.poseRevision = 0U; actor.poseInterpolating = false;
    actor.directPoseTimeline.Reset(); actor.directPoseBirth = 0U;
    actor.directPoseDivergenceSince=actor.directPoseLastEmergencyCorrection=0U;
    ReleaseNativeFollow(actor);
    actor.recoveryFollowing = actor.recoveryNativeArmed = false;
    if (actor.character != 0U && Client()) {
        // Dropping replication must not manufacture Idle over an action that
        // owns native motion. The next scope needs a fresh Host sample.
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(actor.character));
        // A Client role/task can outlive the BT leaf that created it. Always
        // retire that executor through the retail cleanup before authority is
        // released, otherwise an old task can keep fighting future PoseSync.
        if (ClearRoleTaskExecutor(actor)) actor.roleTaskCleanupPending = false;
    }
    actor.lastProxyRefresh = 0U;
    actor.completedRevision = 0U;
    // A timeout releases motion, not the acknowledgement of an irreversible
    // pair/transition. A later heartbeat must not replay the same attack.
    actor.completedActionRevision = 0U;
    actor.completedRoleTaskRevision = 0U;
    actor.completedLifecycleRevision = 0U;
    actor.lastInteractionAttempt = 0U;
    actor.interactionActionRevision = 0U;
    actor.interactionActionPathHash = 0U;
    return true;
}

interactions::Outcome PairOutcome(const NativeOutcome& value) noexcept {
    return {value.revision,value.flags,value.definition,value.kind,value.hpBefore,value.hpPrepared,value.hpReaction,
        value.healthReaction,value.sourceYaw,{value.targetOffset[0],value.targetOffset[1],value.targetOffset[2]}};
}
interactions::LocalParticipant LocalPairActor(const PairIdentity& value) noexcept {
    return {value.character,value.runtimeId,value.entityA,value.entityB,
        value.peer==0U?interactions::Perspective::Npc:(value.peer==gCurrent.localPeer?
            interactions::Perspective::LocalPlayer:interactions::Perspective::RemotePlayer),
        value.creationSerial,value.participantIncarnation};
}
// The source left this pair natively (its retail Idle was requested and its
// current cycle is no longer the admitted paired cycle). The remaining
// release proofs concern the victim (a dead player keeps its 4F graph); they
// must not hold the source's later Host orders (the Alien froze for good).
bool PairSourceLeft(const PairTransaction& pair) noexcept {
    std::uintptr_t current{};
    return pair.sourceExitRequested&&pair.source.character&&Read(pair.source.character+0x36CU,current)&&
        current!=pair.receipt.native.sourceCycle;
}
bool PairReservedCharacter(const std::uintptr_t c) noexcept {
    if(!gPairRuntimeInitialized||!c)return false;
    for(const auto& pair:gPairTransactions)if(pair.used&&!pair.receipt.resourcesReleased&&
        ((pair.source.character==c&&!PairSourceLeft(pair))||pair.victim.character==c))return true;
    return false;
}
bool PairTransactionCandidate(const State& state) noexcept {
    return gPairRuntimeInitialized&&Installed()&&state.interactionCommand!=0U&&
        state.interactionAuthority!=0U&&state.interactionTransition==1U&&
        (state.stateFlags&InteractionValid)!=0U&&(state.interactionFlags&InteractionActorIsSource)!=0U;
}
bool ManualControlReserved(const transactions::Scope&,const transactions::OwnerKey& owner,std::uint64_t) noexcept {
    // Reservation during discovery grants no native authority. Acquisition
    // still requires one unique live Alien and a certified per-actor drain.
    return (gCurrent.flags & kAlienPvP)!=0U && owner.actorClass==1U;
}
bool PairTransactionReserved(const transactions::Scope& scope,const transactions::OwnerKey& owner,std::uint64_t globalSeq) noexcept {
    // Receiving an event does not mean its native consequences completed.
    // Later orders wait for this actor's ordered carrier; earlier action
    // requests still enter so a pair cannot wait on its own prerequisites.
    for(std::size_t i=0;i<kActors;++i)if(gIncomingUsed[i]&&!gIncomingOutcomes[i].empty()&&
        gIncomingOutcomes[i].front().state.entityA==owner.entityA&&
        gIncomingOutcomes[i].front().state.entityB==owner.entityB&&gIncomingOutcomes[i].front().state.birth==owner.birth&&
        globalSeq>gIncomingOutcomes[i].front().through){gReservedByOutcome.fetch_add(1U,std::memory_order_relaxed);return true;}
    for(const auto& pair:gPairTransactions)if(pair.used&&!pair.receipt.resourcesReleased&&pair.key.scope==scope&&
        ((pair.carrier.entityA==owner.entityA&&pair.carrier.entityB==owner.entityB&&pair.carrier.birth==owner.birth&&
          !PairSourceLeft(pair))||
         (pair.victim.peer==0U&&pair.victim.entityA==owner.entityA&&pair.victim.entityB==owner.entityB&&
          pair.victim.authoritativeBirth==owner.birth))) {
        // Orders the Host issued BEFORE its attack action must still run: the
        // pair waits for that action, which queues behind them. Holding them
        // deadlocked the kill and froze the Client Alien. Only later orders wait.
        if(!pair.receipt.pairEntered&&!action_correlation::Empty(pair.carrier.executionAction)&&
            globalSeq!=0U&&globalSeq<=pair.carrier.executionAction.globalSequence)continue;
        gReservedByPair.fetch_add(1U,std::memory_order_relaxed);
        return true;
    }
    return false;
}
bool PairTransactionContextReady(const transactions::Scope& scope) noexcept {
    for(const auto& pair:gPairTransactions)if(pair.used&&pair.key.scope!=scope&&pair.receipt.pairEntered&&
        !pair.receipt.resourcesReleased)return false;
    return true;
}
action_correlation::Readiness PairRequestsReadiness(const PairTransaction& pair) noexcept {
    using Ready=action_correlation::Readiness;
    // The start dependency is not an animation lifetime. Later reservation
    // completion must not revoke the already admitted pair's native outro.
    if(pair.receipt.pairEntered)return Ready::Ready;
    auto sourceRequests=execution_runtime::ActorRequests(pair.source.character);
    if(!action_correlation::Empty(pair.carrier.executionAction)) {
        const auto& reference=pair.carrier.executionAction;
        const transactions::OwnerKey owner{pair.carrier.entityA,pair.carrier.entityB,pair.carrier.birth,pair.carrier.characterClass,
            transactions::Capability::Movement,reference.ownerEpoch};
        const execution::ActorLifetime lifetime{pair.source.character,pair.source.runtimeId,pair.source.generation,pair.source.creationSerial};
        const auto ready=execution_runtime::ActionReadiness(reference,owner,lifetime,&sourceRequests,true);
        if(ready!=Ready::Ready)return ready;
    }
    return sourceRequests==0U&&execution_runtime::ActorRequests(pair.victim.character)==0U?Ready::Ready:Ready::Waiting;
}
interactions::Check PairTransactionPermission(void*,const interactions::Command& command,const interactions::Binding& binding) noexcept {
    transactions::Scope scope{};
    if(!EngineThread()||!Client()||!Stable()||!execution_runtime::CurrentScope(scope)||scope!=command.key.scope)
        return interactions::Check::Waiting;
    for(const auto& pair:gPairTransactions)if(pair.used&&pair.key==command.key&&pair.binding==binding) {
        if(pair.revoked){interactions::RefusalSite()=301U;return interactions::Check::Rejected;}
        if(!PairIdentityLive(pair.source)||!PairIdentityLive(pair.victim)||
            pair.source.creationSerial==0U||pair.victim.creationSerial==0U){interactions::RefusalSite()=302U;return interactions::Check::Rejected;}
        if(pair.source.authoritativeBirth!=command.source.birth||pair.carrier.interactionPairRevision!=command.pairRevision)
            {interactions::RefusalSite()=303U;return interactions::Check::Rejected;}
        if(!PairVictimMatches(pair.carrier,pair.victim)){interactions::RefusalSite()=304U;return interactions::Check::Rejected;}
        if(!interactions::SamePreparation(command.outcome,PairOutcome(pair.carrier.outcome)))
            {interactions::RefusalSite()=305U;return interactions::Check::Rejected;}
        // This dependency authorizes the start, not every later animation
        // callback. The correlated scheduler request may end while its native
        // pair still owns the participants. Keep checking scope, incarnations
        // and the immutable outcome above/below throughout that lifetime.
        if(!pair.startReady&&PairRequestsReadiness(pair)!=action_correlation::Readiness::Ready)
            return interactions::Check::Waiting;
        OutcomeBinding outcome{};
        if(!LoadOutcome(pair.source.character,pair.victim.character,outcome)||outcome.transaction!=command.key||
            outcome.transactionActors!=binding||!SameInteractionTarget(outcome.state,pair.carrier))return interactions::Check::Waiting;
        return interactions::Check::Ready;
    }
    interactions::RefusalSite()=306U;
    return interactions::Check::Rejected;
}
bool PairRetirementQualified(const interactions::Key& key,const interactions::Binding& binding) noexcept {
    transactions::Scope scope{};
    if(!EngineThread()||!Stable()||!Client()||!execution_runtime::CurrentScope(scope)||scope!=key.scope)return false;
    for(const auto& pair:gPairTransactions)if(pair.used&&!pair.revoked&&pair.key==key&&pair.binding==binding)
        return PairIdentityLive(pair.source)&&PairIdentityLive(pair.victim);
    return false;
}
std::atomic<std::uint64_t> gDeadVictimReleases{},gDeadVictimSourcePending{};
std::atomic<std::uint64_t> gEndedSourceExits{};
// The interaction completed natively and its victim died: that victim keeps
// its terminal cycle forever, so waiting for its cleanup reserved the source
// (and every later order of it) indefinitely.
bool DeadVictimReleasable(const interactions::Key& key,const interactions::Receipt& receipt) noexcept {
    // Destruction of the finished source cycle reports Interrupted afterward.
    if(receipt.phase!=interactions::Phase::Completed&&receipt.phase!=interactions::Phase::Interrupted)return false;
    for(const auto& pair:gPairTransactions)if(pair.used&&pair.key==key&&pair.receipt.serial==receipt.serial)
        return TerminalDeadCharacter(pair.victim.character);
    return false;
}
interactions::Check PairTransactionReleaseProof(void*,const interactions::Command& command,const interactions::Binding&,
    const interactions::Receipt& receipt) noexcept {
    const auto released=pair_retirement::CheckReleased(command.key,receipt.serial);
    if(released!=interactions::Check::Waiting||!DeadVictimReleasable(command.key,receipt))return released;
    const auto source=pair_retirement::ReleaseWithDeadVictim(command.key,receipt.serial);
    if(source==interactions::Check::Ready)++gDeadVictimReleases;
    else if(source==interactions::Check::Waiting)++gDeadVictimSourcePending;
    return source;
}
void InvalidatePairTransactions(const std::uintptr_t character) noexcept {
    if(!gPairRuntimeInitialized||!EngineThread())return;
    for(auto& pair:gPairTransactions)if(pair.used&&!pair.receipt.resourcesReleased) {
        if(pair.source.character==character) {
            pair.revoked=true;gPairRuntime.Invalidate(pair.binding.source);pair_retirement::Invalidate(pair.binding.source);
        }
        if(pair.victim.character==character) {
            pair.revoked=true;gPairRuntime.Invalidate(pair.binding.victim);pair_retirement::Invalidate(pair.binding.victim);
        }
    }
}
void PumpPairTransactionEvents() noexcept {
    if(gPairEventOverflow.exchange(false,std::memory_order_acq_rel))
        for(auto& pair:gPairTransactions)if(pair.used&&!pair.receipt.resourcesReleased)
            execution_runtime::Fail(pair.key.scope,execution::Recovery::Capacity);
    for(std::size_t i=0;i<gPairTransactionEvents.size();++i) {
        PairTransactionEvent event{};
        {const std::lock_guard lock(gPairEventMutex);if(!gPairEventCount)break;
            event=gPairTransactionEvents[gPairEventRead];gPairEventRead=(gPairEventRead+1)%gPairTransactionEvents.size();--gPairEventCount;}
        for(auto& pair:gPairTransactions)if(pair.used&&pair.key==event.key&&pair.receipt.serial&&!pair.receipt.resourcesReleased) {
            if(event.retainedEndAction) {
                if(event.actor!=pair.binding.source||event.cycle!=pair.receipt.native.sourceCycle||
                    event.serial!=pair.receipt.native.sourceSerial||pair.revoked||!PairIdentityLive(pair.source))break;
                pair.sourceEndActionObserved=true;
            } else if(event.evidence) {
                if(event.actor!=pair.binding.source||event.cycle!=pair.receipt.native.sourceCycle||
                    event.serial!=pair.receipt.native.sourceSerial)break;
                const auto outcome=PairOutcome(event.outcome);
                // Preparation is immutable; a delayed preparation fact may
                // discharge only that obligation, never a newer reaction.
                if(event.evidence&interactions::PreparationApplied)
                    static_cast<void>(gPairRuntime.OutcomeApplied(pair.key,pair.receipt.serial,outcome,interactions::PreparationApplied));
                const auto other=event.evidence&~interactions::PreparationApplied;
                if(other)static_cast<void>(gPairRuntime.OutcomeApplied(pair.key,pair.receipt.serial,outcome,other));
            } else {
                static_cast<void>(gPairRuntime.Observe(pair.key,pair.receipt.serial,event.actor,event.cycle,event.serial,event.cause,pair.receipt));
            }
            break;
        }
    }
}
std::atomic<std::uint64_t> gPairsAbandoned{},gPairRefusalRetries{};
// A Host interaction this Client could not enter natively owns no native
// resource. Drop it as one instruction: release its reservation so every
// later Host instruction keeps executing. Never fence the whole AISync
// scope for it (that froze every AI until the session ended).
void AbandonPair(Actor& actor,PairTransaction& pair) noexcept {
    pair.revoked=true;pair.startReady=false;pair.receipt.resourcesReleased=true;
    actor.completedPairRevision=pair.carrier.interactionPairRevision;
    actor.completedTransitionRevision=pair.carrier.interactionTransitionRevision;
    actor.completedInteractionRevision=pair.carrier.interactionRevision;
    gPairsAbandoned.fetch_add(1U,std::memory_order_relaxed);
}
void RecordPairRefusal(const PairTransaction& pair,const State& carrier,const std::uint32_t result,
    const std::uint32_t decision) noexcept {
    const auto& receipt=pair.receipt;
    const PairAdmissionFailure failure{GetTickCount64(),pair.key.command,receipt.serial,
        carrier.entityA,carrier.entityB,carrier.birth,carrier.interactionPairRevision,
        result,static_cast<std::uint32_t>(receipt.phase),
        receipt.failureSite,receipt.native.stage,receipt.failureReason,receipt.pairEntered,receipt.transitionEntered,
        interactions::RefusalSite(),decision,carrier.interactionPayloadHigh,
        interactions::DecodePairPayloadHigh(carrier.interactionPayloadHigh)};
    const std::lock_guard lock(gPairFailureMutex);
    if(gPairFailureCount<gPairAdmissionFailures.size())
        gPairAdmissionFailures[(gPairFailureRead+gPairFailureCount++)%gPairAdmissionFailures.size()]=failure;
}
// The Host's attack action (kind5, requested cycle state 4E) natively created
// the paired source cycle. On the Client the ordered interaction transaction
// already entered that same cycle; replaying the action replaced it with Idle
// (the Alien froze until the Host's outro ended).
bool PairConsumedAction(const transactions::Record& r) noexcept {
    if(!gPairRuntimeInitialized||!EngineThread()||r.nativeKind!=execution::kActionKind||r.mode!=0x4EU)return false;
    for(const auto& pair:gPairTransactions)if(pair.used&&pair.receipt.pairEntered&&!pair.receipt.resourcesReleased&&
        pair.key.scope==r.scope&&pair.carrier.entityA==r.owner.entityA&&pair.carrier.entityB==r.owner.entityB&&
        pair.carrier.birth==r.owner.birth)return true;
    return false;
}
bool PublishTransactionOutcome(Actor& actor,PairTransaction& pair) noexcept {
    const auto previous=actor.desired;actor.desired=pair.carrier;
    const bool published=PublishOutcomeBinding(actor,pair.victim.character);
    actor.desired=previous;if(!published)return false;
    const auto slot=static_cast<std::size_t>(&actor-gActors.data());
    const std::lock_guard lock(gOutcomeMutex);
    auto& binding=gOutcomes[slot];
    if(binding.source.creationSerial!=pair.source.creationSerial||binding.victim.creationSerial!=pair.victim.creationSerial||
        binding.state.interactionCommand!=pair.key.command)return false;
    binding.transaction=pair.key;binding.transactionActors=pair.binding;return true;
}
bool PairContextRetired() noexcept {
    for(const auto& pair:gPairTransactions)if(pair.used&&pair.receipt.pairEntered&&!pair.receipt.resourcesReleased)return false;
    return true;
}
std::atomic<std::uint64_t> gEndedSourceExitsDeferred{},gHostEndedRetainedSources{};
std::atomic<std::uint64_t> gDeferredSourceEndsForwarded{};
struct PairExitTrace {
    std::uint64_t tick{},command{},receipt{};
    std::uint32_t stage{},entityA{},entityB{},birth{},hostCycle{},hostPair{},localFlags{},check{};
    bool markerObserved{},forwarded{};
};
std::mutex gPairExitTraceGate;
std::array<PairExitTrace,64> gPairExitTraces{};
std::size_t gPairExitTraceRead{},gPairExitTraceCount{};
std::uint64_t gPairExitTraceDropped{};
void RecordPairExit(PairTransaction& pair,const std::uint32_t stage,const interactions::Check check=interactions::Check::Ready) noexcept {
    if(pair.exitTraceMask&(1U<<stage))return;
    pair.exitTraceMask|=1U<<stage;
    PairExitTrace sample{};sample.tick=GetTickCount64();sample.command=pair.key.command;sample.receipt=pair.receipt.serial;
    sample.stage=stage;sample.entityA=pair.carrier.entityA;sample.entityB=pair.carrier.entityB;sample.birth=pair.carrier.birth;
    sample.markerObserved=pair.sourceEndActionObserved;sample.forwarded=pair.sourceEndActionForwarded;
    sample.check=static_cast<std::uint32_t>(check);
    if(const auto* actor=Find(pair.source.character)){sample.hostCycle=actor->streamCycleState;sample.hostPair=actor->streamPairRevision;}
    std::uint8_t flags{};if(Read(pair.receipt.native.sourceCycle+8U,flags))sample.localFlags=flags;
    const std::lock_guard lock(gPairExitTraceGate);
    if(gPairExitTraceCount==gPairExitTraces.size()){++gPairExitTraceDropped;return;}
    gPairExitTraces[(gPairExitTraceRead+gPairExitTraceCount++)%gPairExitTraces.size()]=sample;
}
bool ForwardDeferredSourceEnd(void* original,const std::uintptr_t cycle,const std::uint32_t event) noexcept {
    __try {reinterpret_cast<void(__thiscall*)(void*,std::uint32_t)>(original)(reinterpret_cast<void*>(cycle),event);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void AdvanceEndedPairSource(PairTransaction& pair) noexcept {
    // Native End_Action has already permitted termination (00664AB2 flags|6).
    // A brainless Client may have no scheduler pulse left to request Idle:
    // waiting for a new Move here deadlocks against this pair's reservation.
    // This requests retail cleanup; only the separate destructor-return ledger
    // below can establish that cleanup and listener disposal actually finished.
    if(pair.revoked||pair.sourceExitRequested||!pair.receipt.pairEntered||pair.receipt.resourcesReleased||
        !EngineThread()||!Stable()||!Client()||!PairIdentityLive(pair.source))return;
    transactions::Scope scope{};
    if(!execution_runtime::CurrentScope(scope)||scope!=pair.key.scope)return;
    const auto character=pair.source.character;
    const auto cycle=pair.receipt.native.sourceCycle;
    // End_Action consults the LOCAL victim controller. A Client's representation
    // of a player has none, so retail permits the end earlier than on the Host,
    // whose real player keeps the source in this cycle. The Host decides: exit
    // only once its stream shows the source left this paired cycle.
    // The Client plays its OWN paired animation to its native End_Action
    // (checked below: flags|6 written by 00664AB2), at the same animation
    // point as the Host. Never cut it short; additionally leave only after
    // the Host's stream shows its source left this paired cycle.
    const auto* source=Find(character);
    const bool hostLeft=source&&source->streamBirth==pair.carrier.birth&&source->streamRevision!=0U&&
        source->streamPairRevision>=pair.carrier.interactionPairRevision&&source->streamCycleState!=0U&&
        source->streamCycleState!=0x4EU&&source->streamCycleState!=0x4FU;
    if(!hostLeft) {
        pair.hostLeftAt=0U;
        RecordPairExit(pair,1U);++gEndedSourceExitsDeferred;return;
    }
    const auto now=GetTickCount64();
    if(!pair.hostLeftAt)pair.hostLeftAt=now;
    // This Client's own player is the victim and retail retained the source.
    // A dead victim may also have cut the source animation before its marker:
    // once the Host has left for a grace period, nothing local can end it.
    const bool localVictim=LocalPlayerVictimRetains(cycle,pair.binding.victim.character);
    const bool nativeDeathEnd=DeadVictimNativeEndReady(hostLeft,localVictim,
        TerminalDeadCharacter(pair.binding.victim.character),
        (pair.receipt.observedOutcomeEvidence&interactions::NativeDeathApplied)!=0U);
    if(localVictim&&!pair.sourceEndActionObserved&&TerminalDeadCharacter(pair.binding.victim.character)&&
        now-pair.hostLeftAt>=kLocalVictimEndGraceMs)pair.sourceEndActionObserved=true;
    const interactions::Memory memory{gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
        return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
    }};
    // The retain branch intentionally did NOT write flags|6. Forward its
    // observed marker through retail only after the Host left, on the exact
    // admitted graph and E5==0 (no lethal side effect in 00664A10).
    if((pair.sourceEndActionObserved||nativeDeathEnd)&&!pair.sourceEndActionForwarded) {
        const auto retained=interactions::CheckSourceRetention(memory,pair.binding.source,pair.receipt.native,cycle);
        std::uint8_t lethal{};std::uintptr_t victim{};
        const bool nonLethal=Read(cycle+0xE5U,lethal)&&lethal==0U&&Read(cycle+0x44U,victim)&&victim==pair.binding.victim.character;
        if(DeferredSourceEndReady(PairIdentityLive(pair.source)&&PairIdentityLive(pair.victim),hostLeft,
            pair.sourceEndActionObserved||nativeDeathEnd,retained==interactions::Check::Ready,nonLethal)) {
            const character_direct_step_runtime::MovementResourceLease resources(character);
            const auto original=gSourceGraphEventOriginal.load(std::memory_order_acquire);
            const auto event=EndActionEvent();
            if(!resources||!original||!event){RecordPairExit(pair,3U,interactions::Check::Waiting);return;}
            if(!PairIdentityLive(pair.source)||!PairIdentityLive(pair.victim)||
                interactions::CheckSourceRetention(memory,pair.binding.source,pair.receipt.native,cycle)!=interactions::Check::Ready||
                !Read(cycle+0xE5U,lethal)||lethal!=0U||!Read(cycle+0x44U,victim)||victim!=pair.binding.victim.character)return;
            pair.sourceEndActionForwarded=true; // Reentrant callbacks cannot dispatch twice.
            if(localVictim?!FinishRetainedLocalVictimEnd(cycle):!ForwardDeferredSourceEnd(original,cycle,event)) {
                RecordPairExit(pair,6U,interactions::Check::Unknown);pair.revoked=true;
                gPairRuntime.Invalidate(pair.binding.source);
                execution_runtime::Fail(pair.key.scope,execution::Recovery::NativeUnavailable);return;
            }
            if(localVictim)++gLocalVictimEndsFinished;
            ++gDeferredSourceEndsForwarded;RecordPairExit(pair,4U);
        }
    }
    // Rejected only means the admitted graph no longer matches; its exact
    // identity (serial, 4E, flags|6) is still rechecked below. Treating it as
    // terminal kept the source in 4E forever and held every later Host order.
    const auto exit=interactions::CheckSourceExit(memory,pair.binding.source,pair.receipt.native,cycle);
    if(exit!=interactions::Check::Ready&&exit!=interactions::Check::Rejected){RecordPairExit(pair,pair.sourceEndActionObserved?3U:2U,exit);return;}
    std::uintptr_t current{},owner{},type{},schedulerOwner{};
    std::uint8_t serial{},state{},flags{};std::uint32_t idleArgument{};
    if(!cycle||!Read(character+0x36CU,current)||current!=cycle||!Read(cycle, type)||
        type!=gBase+0xEAE5D0U||!Read(cycle+4U,owner)||owner!=character||
        !Read(cycle+0x19U,serial)||serial!=pair.receipt.native.sourceSerial||
        !Read(cycle+0x1AU,state)||state!=0x4EU||!Read(cycle+8U,flags)||(flags&6U)!=6U||
        !Read(character+0x2CD0U,schedulerOwner)||schedulerOwner!=character||
        !Read(character+0x2CD0U+0x31CU,idleArgument)) {
        // Previously silent: Host left but this source has no local end yet.
        RecordPairExit(pair,localVictim?7U:2U,exit);return;
    }
    const character_direct_step_runtime::MovementResourceLease resources(character);
    const auto idle=reinterpret_cast<void(__thiscall*)(void*,std::uint32_t)>(gPairedIdleOriginal.load(std::memory_order_acquire));
    if(!resources||!idle||!PairIdentityLive(pair.source)||
        !Read(character+0x36CU,current)||current!=cycle||!Read(cycle+0x19U,serial)||
        serial!=pair.receipt.native.sourceSerial||!Read(cycle+8U,flags)||(flags&6U)!=6U)return;
    // Same ECX/stack contract and argument source as 00687690's normal Idle.
    // Set before callback-capable native dispatch; never re-enter this cycle.
    pair.sourceExitRequested=true;++gEndedSourceExits;
    RecordPairExit(pair,5U);
    idle(reinterpret_cast<void*>(cycle),idleArgument);
}
void PumpPairRetirements() noexcept {
    if(!gPairRuntimeInitialized||!EngineThread())return;
    for(auto& pair:gPairTransactions)if(pair.used&&!pair.receipt.resourcesReleased)AdvanceEndedPairSource(pair);
    // End_Action can destroy the source synchronously before its queued worker
    // observation reaches this pump, while the victim keeps playing its own
    // graph. Do not reinspect that destroyed cycle as a live 4E/4F. Only the
    // exact cleanup-return ledger authorizes this retirement transition; full
    // Release still waits for BOTH cleanups and BOTH action disposals below.
    for(auto& pair:gPairTransactions)if(pair.used&&pair.receipt.pairEntered&&!pair.receipt.resourcesReleased) {
        for(const bool victim:{false,true})if(pair_retirement::CheckCycleRetired(pair.key,pair.receipt.serial,victim)==interactions::Check::Ready)
            static_cast<void>(gPairRuntime.Observe(pair.key,pair.receipt.serial,
                victim?pair.binding.victim:pair.binding.source,
                victim?pair.receipt.native.victimCycle:pair.receipt.native.sourceCycle,
                victim?pair.receipt.native.victimSerial:pair.receipt.native.sourceSerial,
                interactions::FinishCause::NativeDestruction,pair.receipt));
    }
    // Destructor-return proofs contain copied values only. Reap these even
    // after native participant identity or the current world has been revoked.
    PumpPairTransactionEvents();
    for(auto& pair:gPairTransactions)if(pair.used&&pair.receipt.pairEntered&&!pair.receipt.resourcesReleased&&
        (pair_retirement::CheckReleased(pair.key,pair.receipt.serial)==interactions::Check::Ready||
         DeadVictimReleasable(pair.key,pair.receipt))) {
        if(pair.receipt.phase!=interactions::Phase::Completed&&pair.receipt.phase!=interactions::Phase::Interrupted&&
            pair.receipt.phase!=interactions::Phase::Fenced)
            static_cast<void>(gPairRuntime.Observe(pair.key,pair.receipt.serial,pair.binding.source,
                pair.receipt.native.sourceCycle,pair.receipt.native.sourceSerial,interactions::FinishCause::NativeInterruption,pair.receipt));
        static_cast<void>(gPairRuntime.Release(pair.key,&PairTransactionReleaseProof,nullptr,pair.receipt));
        if(pair.receipt.resourcesReleased) {
            auto* actor=Find(pair.source.character);
            if(actor&&actor->runtimeId==pair.source.runtimeId&&actor->remoteBirth==pair.carrier.birth&&PairIdentityLive(pair.source)) {
                actor->completedPairRevision=pair.carrier.interactionPairRevision;
                actor->completedTransitionRevision=pair.carrier.interactionTransitionRevision;
                actor->completedInteractionRevision=pair.carrier.interactionRevision;
            }
        }
    }
}
void PumpPairTransactions() noexcept {
    const profiling::Timer timing(profiling::Section::PairPump);
    if(!gPairRuntimeInitialized||!EngineThread())return;
    PumpPairRetirements();
    if(!Stable()||!Client())return;
    transactions::Scope scope{};if(!execution_runtime::CurrentScope(scope))return;
    if(scope!=gPairRuntimeScope) {
        if(!PairTransactionContextReady(scope))return;
        const auto configured=gPairRuntime.Configure(scope,gCurrent.sessionCookie,gCurrent.worldGeneration);
        if(configured!=interactions::Result::Admitted&&configured!=interactions::Result::Duplicate)return;
        gPairRuntimeScope=scope;for(auto& previous:gPairTransactions)previous={};
    }
    for(auto& actor:gActors) {
        if(!actor.character||actor.duplicate||actor.faulted)continue;
        const auto index=static_cast<std::size_t>(&actor-gActors.data());auto& pair=gPairTransactions[index];
        // Its native cleanup has begun/completed. Keep the reservation until
        // the independent return proofs finish; never resubmit or read the
        // obsolete cycle while waiting for the other participant.
        if(pair.used&&!pair.receipt.resourcesReleased&&pair.receipt.phase==interactions::Phase::Interrupted)continue;
        const auto& carrier=pair.used&&!pair.receipt.resourcesReleased&&pair.progress.interactionCommand==pair.key.command?
            pair.progress:actor.received;
        // Diagnostics only: name the first gate holding a not-yet-completed
        // Host pair (AvS 454 pilot never replayed and logged nothing).
        const bool pending=carrier.interactionPairRevision!=0U&&carrier.interactionPairRevision!=actor.completedPairRevision;
        const auto gate=[&](std::uint32_t code) noexcept {
            if(!pending)return;
            gPairGateLast.store(code,std::memory_order_relaxed);gPairGateClass.store(actor.characterClass,std::memory_order_relaxed);
            gPairGateRevision.store(carrier.interactionPairRevision,std::memory_order_relaxed);
            gPairGateCount.fetch_add(1U,std::memory_order_relaxed);
        };
        if(!actor.initialPlacementReady){gate(1U);continue;}
        if(!PairTransactionCandidate(carrier)) {
            gate(carrier.interactionCommand==0U?2U:carrier.interactionAuthority==0U?3U:carrier.interactionTransition!=1U?4U:5U);
            continue;
        }
        if(carrier.birth!=actor.remoteBirth){gate(6U);continue;}
        if(carrier.sessionCookie!=gCurrent.sessionCookie||carrier.worldGeneration!=gCurrent.worldGeneration){gate(7U);continue;}
        if(carrier.interactionAuthority!=scope.authorityEpoch||
            !std::equal(scope.session.begin(),scope.session.end(),std::begin(carrier.interactionSession))){gate(8U);continue;}
        const interactions::Key key{scope,carrier.interactionCommand};
        if(pair.used&&pair.key!=key&&!pair.receipt.resourcesReleased){gate(9U);continue;}
        if(pair.used&&pair.key==key&&pair.receipt.resourcesReleased)continue;
        if(!pair.used||pair.key!=key) {
            const auto target=PairTargetForState(carrier);PairIdentity source{},victim{};
            if(!target){gate(10U);continue;}
            if(PublishedPairSource(actor.character,source)==kActors){gate(11U);continue;}
            if(!PublishedPairVictim(target,victim)||!PairVictimMatches(carrier,victim)){gate(12U);continue;}
            if(!source.creationSerial||!victim.creationSerial){gate(13U);continue;}
            if(PairReservedCharacter(actor.character)||PairReservedCharacter(target)){gate(14U);continue;}
            pair={};pair.used=true;pair.key=key;pair.source=source;pair.victim=victim;pair.progress=carrier;
            pair.binding.source=LocalPairActor(source);pair.binding.victim=LocalPairActor(victim);
            pair.binding.nativeGeneration=gCurrent.nativeGeneration;
            pair.binding.exclusiveExecution=true;pair.binding.outcomeBound=true;
        }
        if(pair.revoked)continue;
        pair.carrier=carrier;
        if(!PairIdentityLive(pair.source)||!PairIdentityLive(pair.victim)) {
            InvalidatePairTransactions(!PairIdentityLive(pair.source)?pair.source.character:pair.victim.character);continue;
        }
        const auto dependency=PairRequestsReadiness(pair);
        if(dependency!=action_correlation::Readiness::Ready) {
            gate(15U);
            // A missing predecessor may arrive. A refused/retired predecessor
            // cannot: drop this never-entered interaction (it owns nothing
            // native) instead of reserving every following order forever.
            if(dependency!=action_correlation::Readiness::Waiting) {
                interactions::RefusalSite()=400U+static_cast<std::uint32_t>(dependency);
                RecordPairRefusal(pair,carrier,0U,2U);AbandonPair(actor,pair);
            }
            continue;
        }
        if(!pair.drainIssued) {
            actor.convergenceActive=false;actor.pursuitActive=false;
            if(pair.victim.peer!=0U&&pair.victim.peer!=gCurrent.localPeer&&
                !character_direct_step_runtime::Stop(pair.victim.character))continue;
            pair.drainIssued=true;
        }
        if(!pair.receipt.pairEntered) {
            const execution::OwnershipMemory memory{gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
                return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
            }};
            const execution::ActorLifetime lifetime{pair.source.character,pair.source.runtimeId,pair.source.generation,pair.source.creationSerial};
            if(execution::InspectInteractionBoundary(memory,lifetime)!=execution::OwnershipCheck::Ready){gate(16U);continue;}
            // Preserve the Host's semantic flag. NativePairAdapter resolves
            // the source definition through AnimatedCharacter and the bone
            // against THIS victim; there is no second local decision here.
            pair.binding.localPairFlagsHigh=interactions::DecodePairPayloadHigh(carrier.interactionPayloadHigh);
        }
        if(!PublishTransactionOutcome(actor,pair)){gate(17U);continue;}
        // Publish the earned start permission BEFORE callback-capable native
        // registration. Never derive it from an output Receipt being rewritten
        // by Submit/Observe/Inspect. It belongs only to this exact key/binding.
        pair.startReady=true;
        interactions::RefusalSite()=0U;
        const auto admitted=gPairRuntime.Submit(carrier,pair.binding,pair.receipt);
        if(!pair.receipt.pairEntered)pair.startReady=false;
        gate(pair.receipt.pairEntered?100U:200U+static_cast<std::uint32_t>(admitted));
        if(pending)gPairGateRefusalSite.store(interactions::RefusalSite(),std::memory_order_relaxed);
        if(pair.receipt.transitionEntered)
            static_cast<void>(pair_retirement::Track(pair.key,pair.receipt.serial,pair.binding,pair.receipt.native));
        if(admitted==interactions::Result::Conflict||admitted==interactions::Result::Unknown||admitted==interactions::Result::Capacity||
            admitted==interactions::Result::Rejected) {
            const auto result=static_cast<std::uint32_t>(admitted);
            if(pair.receipt.pairEntered||pair.receipt.transitionEntered) {
                // Native entry happened and its state is no longer provable.
                RecordPairRefusal(pair,carrier,result,3U);
                execution_runtime::Fail(scope,execution::Recovery::NativeUnavailable);continue;
            }
            // Nothing native was entered. A Rejected start may be a transient
            // carrier/victim state: retry it for a short bounded time, then
            // drop only this interaction and keep executing the Host stream.
            const auto now=GetTickCount64();
            if(admitted==interactions::Result::Rejected&&(!pair.firstRefusal||now-pair.firstRefusal<kPairRefusalRetryMs)) {
                if(!pair.firstRefusal){pair.firstRefusal=now;RecordPairRefusal(pair,carrier,result,1U);}
                gPairRefusalRetries.fetch_add(1U,std::memory_order_relaxed);continue;
            }
            RecordPairRefusal(pair,carrier,result,2U);AbandonPair(actor,pair);continue;
        }
        if(pair.receipt.pairEntered) {
            const auto inspected=gPairRuntime.Inspect(pair.key,pair.receipt);
            if(inspected!=interactions::Result::Unknown&&inspected!=interactions::Result::Rejected)
                static_cast<void>(pair_retirement::Track(pair.key,pair.receipt.serial,pair.binding,pair.receipt.native));
            static_cast<void>(gPairRuntime.Release(pair.key,&PairTransactionReleaseProof,nullptr,pair.receipt));
            if(pair.receipt.resourcesReleased) {
                actor.completedPairRevision=carrier.interactionPairRevision;actor.completedTransitionRevision=carrier.interactionTransitionRevision;
                actor.completedInteractionRevision=carrier.interactionRevision;
            }
        }
    }
}


// Engine thread, every pump of an owned pair. The scheduler can run before
// the next pump, so publish the Host retention lease ahead of the outro.

// Keep the observer's local 4E through the outro only while the Host still
// retains its own source cycle for this pair, bounded in time.



void CaptureInteractionCycle(Actor& actor) noexcept {
    auto& state = actor.desired;
    if ((state.stateFlags & InteractionValid) == 0U || state.interactionTransition == 0U) return;
    std::uintptr_t cycle{};
    std::uint8_t nativeState{};
    bool running{};
    const bool matching = PairedCycleMatches(actor.character, state) &&
        ReadPairedCycleActivity(actor.character, cycle, nativeState, running);
    const bool active = matching && running;
    std::uintptr_t target{};
    std::uint32_t targetBirth{}, currentBirth{};
    std::uint8_t flags{};
    std::uint32_t stage{};
    // 00663E00 can clear +44 in stage 6 without setting completion flags.
    // Preserve that proven admitted pair's source outro too. A failed typed
    // initializer has no admitted result and must not create such a lease.
    const bool admittedOutro = matching && nativeState == 0x4EU &&
        (state.outcome.flags & OutcomeVictimAdmitted) != 0U && Read(cycle + 0xE0U, stage) && stage == 6U;
    const bool retained = active || (matching && Read(cycle + 8U, flags) && (flags & 6U) == 0U &&
        (admittedOutro || (Read(cycle + 0x44U, target) && target != 0U && Read(cycle + 0x48U, targetBirth) &&
         Read(target + 0x50U, currentBirth) && targetBirth == currentBirth)));
    if (retained) state.interactionFlags |= InteractionSourceCycleRetained;
    else state.interactionFlags &= ~InteractionSourceCycleRetained;
    if (active) state.interactionFlags |= InteractionCycleActive;
    else state.interactionFlags &= ~InteractionCycleActive;
    if (!active && state.outcome.revision != 0U && (state.outcome.flags & OutcomeSourceEnded) == 0U) {
        std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        auto& binding = gOutcomes[static_cast<std::size_t>(&actor - gActors.data())];
        if (binding.state.interactionPairRevision != state.interactionPairRevision) return;
        binding.state.outcome.flags |= OutcomeSourceEnded;
        ++binding.state.outcome.revision;
        state.outcome = binding.state.outcome;
    }
}
bool ReadTargetSelectionSnapshot(const Actor& actor, TargetSelectionSnapshot& snapshot) noexcept {
    snapshot = {};
    std::uintptr_t selection{};
    if (!Read(actor.character + 0x34A8U, selection)) return false;
    snapshot.selectionReadable = true;
    if (selection == 0U) {
        snapshot.selectedReadable = true;
        snapshot.pendingReadable = true;
    } else {
        if (Read(selection + 0x10U, snapshot.selectedRecord)) {
            snapshot.selectedReadable = snapshot.selectedRecord == 0U;
            if (snapshot.selectedRecord != 0U) {
                std::uintptr_t recordOwner{}, target{};
                std::uint32_t recordBirth{}, currentBirth{};
                if (Read(snapshot.selectedRecord + 8U, recordOwner) && recordOwner == actor.character &&
                    Read(snapshot.selectedRecord + 0x42CU, target) && target != 0U &&
                    Read(snapshot.selectedRecord + 0x430U, recordBirth) &&
                    Read(target + 0x50U, currentBirth) && recordBirth == currentBirth) {
                    snapshot.selectedReadable = true;
                    snapshot.selectedPresent = true;
                    snapshot.selectedTarget = target;
                    snapshot.selectedBirth = currentBirth;
                    snapshot.selectedPeer = PeerForParticipant(target);
                    snapshot.selectedLiving = LivingCharacter(target);
                    snapshot.selectedTerminalDead = TerminalDeadCharacter(target);
                }
            }
        }
        if (Read(selection + 0x14U, snapshot.pendingRecord)) {
            snapshot.pendingReadable = snapshot.pendingRecord == 0U;
            if (snapshot.pendingRecord != 0U) {
                std::uintptr_t recordOwner{}, target{};
                std::uint32_t recordBirth{}, currentBirth{};
                if (Read(snapshot.pendingRecord + 8U, recordOwner) && recordOwner == actor.character &&
                    Read(snapshot.pendingRecord + 0x42CU, target) && target != 0U &&
                    Read(snapshot.pendingRecord + 0x430U, recordBirth) &&
                    Read(target + 0x50U, currentBirth) && recordBirth == currentBirth) {
                    snapshot.pendingReadable = true;
                    snapshot.pendingPresent = true;
                    snapshot.pendingTarget = target;
                    snapshot.pendingBirth = currentBirth;
                    snapshot.pendingPeer = PeerForParticipant(target);
                    snapshot.pendingLiving = LivingCharacter(target);
                    snapshot.pendingTerminalDead = TerminalDeadCharacter(target);
                }
            }
        }
    }
    std::uintptr_t haveTarget{}, haveNextTarget{};
    snapshot.targetSignalsReadable = Read(actor.character + 0x3154U, haveTarget) &&
        Read(actor.character + 0x31E8U, haveNextTarget) &&
        (haveTarget == 0U || Read(haveTarget + 0x10U, snapshot.haveTargetSignal)) &&
        (haveNextTarget == 0U || Read(haveNextTarget + 0x10U, snapshot.haveNextTargetSignal));
    return snapshot.selectionReadable;
}
bool TargetSelectionCertain(const TargetSelectionSnapshot& snapshot) noexcept {
    return snapshot.selectionReadable && snapshot.selectedReadable && snapshot.pendingReadable;
}
bool HasLivingSuccessor(const TargetSelectionSnapshot& snapshot) noexcept {
    return (snapshot.selectedPresent && snapshot.selectedLiving) ||
        (snapshot.pendingPresent && snapshot.pendingLiving);
}
void QueueTargetSuccessionProbe(const Actor& actor, const TargetSelectionSnapshot& snapshot,
    const TargetSuccessionPhase phase, const ULONGLONG now) noexcept {
    TargetSuccessionProbe probe{};
    probe.sessionCookie = gCurrent.sessionCookie;
    probe.tick = now;
    probe.retiredPeer = actor.postDeathRetiredPeer;
    probe.selectedPeer = snapshot.selectedPeer;
    probe.pendingPeer = snapshot.pendingPeer;
    probe.nativeGeneration = gCurrent.nativeGeneration;
    probe.worldGeneration = gCurrent.worldGeneration;
    probe.entityA = actor.entityA;
    probe.entityB = actor.entityB;
    probe.birth = actor.birth;
    probe.interactionRevision = actor.desired.interactionRevision;
    probe.pairRevision = actor.desired.interactionPairRevision;
    probe.phase = static_cast<std::uint32_t>(phase);
    probe.selectedBirth = snapshot.selectedBirth;
    probe.pendingBirth = snapshot.pendingBirth;
    probe.haveTargetSignal = snapshot.haveTargetSignal;
    probe.haveNextTargetSignal = snapshot.haveNextTargetSignal;
    probe.selectionReadable = snapshot.selectionReadable;
    probe.selectedReadable = snapshot.selectedReadable;
    probe.selectedPresent = snapshot.selectedPresent;
    probe.selectedLiving = snapshot.selectedLiving;
    probe.selectedTerminalDead = snapshot.selectedTerminalDead;
    probe.pendingReadable = snapshot.pendingReadable;
    probe.pendingPresent = snapshot.pendingPresent;
    probe.pendingLiving = snapshot.pendingLiving;
    probe.pendingTerminalDead = snapshot.pendingTerminalDead;
    probe.targetSignalsReadable = snapshot.targetSignalsReadable;
    const auto write = gTargetSuccessionProbeWrite.load(std::memory_order_relaxed);
    if (write - gTargetSuccessionProbeRead.load(std::memory_order_acquire) >= kParticipantProbeRing) {
        gTargetSuccessionDropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    gTargetSuccessionProbes[write % kParticipantProbeRing] = probe;
    gTargetSuccessionProbeWrite.store(write + 1U, std::memory_order_release);
    gTargetSuccessionSamples.fetch_add(1U, std::memory_order_relaxed);
}
void TraceTargetSelection(Actor& actor, const TargetSuccessionPhase phase,
    const ULONGLONG now, const bool force) noexcept {
    TargetSelectionSnapshot snapshot{};
    static_cast<void>(ReadTargetSelectionSnapshot(actor, snapshot));
    const bool changed = !actor.targetSelectionObserved ||
        actor.observedSelectedRecord != snapshot.selectedRecord ||
        actor.observedPendingRecord != snapshot.pendingRecord ||
        actor.observedSelectedPeer != snapshot.selectedPeer ||
        actor.observedPendingPeer != snapshot.pendingPeer ||
        actor.observedSelectedBirth != snapshot.selectedBirth ||
        actor.observedPendingBirth != snapshot.pendingBirth ||
        actor.observedHaveTargetSignal != snapshot.haveTargetSignal ||
        actor.observedHaveNextTargetSignal != snapshot.haveNextTargetSignal ||
        actor.observedSelectedPresent != snapshot.selectedPresent ||
        actor.observedSelectedLiving != snapshot.selectedLiving ||
        actor.observedSelectedTerminalDead != snapshot.selectedTerminalDead ||
        actor.observedPendingPresent != snapshot.pendingPresent ||
        actor.observedPendingLiving != snapshot.pendingLiving ||
        actor.observedPendingTerminalDead != snapshot.pendingTerminalDead ||
        actor.observedSelectionReadable != snapshot.selectionReadable ||
        actor.observedSelectedReadable != snapshot.selectedReadable ||
        actor.observedPendingReadable != snapshot.pendingReadable ||
        actor.observedTargetSignalsReadable != snapshot.targetSignalsReadable;
    actor.targetSelectionObserved = true;
    actor.observedSelectedRecord = snapshot.selectedRecord;
    actor.observedPendingRecord = snapshot.pendingRecord;
    actor.observedSelectedPeer = snapshot.selectedPeer;
    actor.observedPendingPeer = snapshot.pendingPeer;
    actor.observedSelectedBirth = snapshot.selectedBirth;
    actor.observedPendingBirth = snapshot.pendingBirth;
    actor.observedHaveTargetSignal = snapshot.haveTargetSignal;
    actor.observedHaveNextTargetSignal = snapshot.haveNextTargetSignal;
    actor.observedSelectedPresent = snapshot.selectedPresent;
    actor.observedSelectedLiving = snapshot.selectedLiving;
    actor.observedSelectedTerminalDead = snapshot.selectedTerminalDead;
    actor.observedPendingPresent = snapshot.pendingPresent;
    actor.observedPendingLiving = snapshot.pendingLiving;
    actor.observedPendingTerminalDead = snapshot.pendingTerminalDead;
    actor.observedSelectionReadable = snapshot.selectionReadable;
    actor.observedSelectedReadable = snapshot.selectedReadable;
    actor.observedPendingReadable = snapshot.pendingReadable;
    actor.observedTargetSignalsReadable = snapshot.targetSignalsReadable;
    if (force || changed) QueueTargetSuccessionProbe(actor, snapshot, phase, now);
}
bool PostKillTargetTraceActive(const Actor& actor) noexcept {
    if (actor.postDeathArbitrationRevision != 0U) return true;
    const auto& state = actor.desired;
    if (state.interactionTargetPeer == 0U || state.actionTypeRva != 0x00ECB770U) return false;
    const auto target = ParticipantForPeer(state.interactionTargetPeer);
    return target != 0U && TerminalDeadCharacter(target);
}
void ClearPostDeathArbitration(Actor& actor) noexcept {
    actor.postDeathArbitrationRevision = 0U;
    actor.postDeathArbitrationSince = 0U;
    actor.postDeathRetiredPeer = 0U;
}
void RecoverDeadParticipantTarget(Actor& actor, const ULONGLONG now) noexcept {
    if (PairSourceBusy(actor.character)) return;
    const auto& state = actor.desired;

    // Once the old pair has been physically retired, give retail arbitration a
    // bounded opportunity to promote/reacquire a valid successor before using
    // the legacy global cleanup as a last-resort recovery. The dead-melee guard
    // is armed by recoveredInteractionRevision independently of this reset.
    if (actor.postDeathArbitrationRevision != 0U) {
        if (state.interactionRevision != actor.postDeathArbitrationRevision) {
            TraceTargetSelection(actor, TargetSuccessionPhase::InteractionAdvanced, now, true);
            ClearPostDeathArbitration(actor);
            actor.deadTargetSince = 0U;
            return;
        }
        TargetSelectionSnapshot selection{};
        static_cast<void>(ReadTargetSelectionSnapshot(actor, selection));
        TraceTargetSelection(actor, TargetSuccessionPhase::RecoveryEvaluation, now, false);
        if (!TargetSelectionCertain(selection)) {
            gPostDeathDecisionUncertain.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        if (HasLivingSuccessor(selection)) {
            gPostDeathSuccessionsPreserved.fetch_add(1U, std::memory_order_relaxed);
            TraceTargetSelection(actor, TargetSuccessionPhase::SuccessorPreserved, now, true);
            ClearPostDeathArbitration(actor);
            actor.deadTargetSince = 0U;
            return;
        }
        // A live participant can become visible just after the old paired kill
        // retires. In .186 the one-second fallback reset landed in exactly that
        // window and erased retail arbitration before pending/selected could be
        // rebuilt. Keep the BT intact while a real participant sense is recent;
        // this never invents sight, threat or a target.
        const auto sensedParticipant = ParticipantForPeer(actor.lastLivingParticipantSensePeer);
        if (actor.lastLivingParticipantSense != 0U && sensedParticipant != 0U &&
            LivingCharacter(sensedParticipant) &&
            now - actor.lastLivingParticipantSense < kPostDeathRecentSenseHold) {
            gPostDeathArbitrationDeferrals.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        if (now - actor.postDeathArbitrationSince < kPostDeathArbitrationGracePeriod) {
            gPostDeathArbitrationDeferrals.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        bool survivor = LivingCharacter(LocalPlayer());
        for (std::uint32_t i = 0U; !survivor && i < gCurrent.participantCount; ++i)
            survivor = LivingCharacter(ParticipantForPeer(gCurrent.participants[i].peer));
        if (!survivor) return;

        const bool hadRoleTask = actor.lastRoleTask != 0U;
        const bool treeReset = SuspendExistingTree(actor);
        const bool roleReset = ClearRoleTaskExecutor(actor);
        if (roleReset) {
            if (hadRoleTask) gPostDeathRoleTaskResets.fetch_add(1U, std::memory_order_relaxed);
            actor.lastRoleTask = 0U;
        }
        if (treeReset && roleReset) {
            gPostDeathDecisionResets.fetch_add(1U, std::memory_order_relaxed);
            TraceTargetSelection(actor, TargetSuccessionPhase::FallbackReset, now, true);
        } else {
            gPostDeathDecisionResetRejected.fetch_add(1U, std::memory_order_relaxed);
            TraceTargetSelection(actor, TargetSuccessionPhase::FallbackResetRejected, now, true);
        }
        ClearPostDeathArbitration(actor);
        actor.deadTargetSince = 0U;
        return;
    }

    // EC B770 is the retail ActionMeleeAttack vtable (factory 006F6640).
    // Retire only this exact completed paired attack; never cancel traversal or
    // a live victim. A previously retired revision is handled by the bounded
    // arbitration phase above, not by another destructive cleanup.
    if (state.interactionTargetPeer == 0U || state.actionTypeRva != 0x00ECB770U ||
        !PairedCycleMatches(actor.character, state) ||
        actor.recoveredInteractionRevision == state.interactionRevision) {
        actor.deadTargetSince = 0U;
        return;
    }
    const auto target = ParticipantForPeer(state.interactionTargetPeer);
    std::uint16_t hp{};
    std::uint32_t health{};
    if (target == 0U || !Read(target + 0x33CU, hp) || hp != 0U ||
        !Read(target + 0x32CU, health) || health != 4U) {
        actor.deadTargetSince = 0U;
        return;
    }
    if (NativeVictimFlowRunning(actor.character, target)) {
        actor.deadTargetSince = 0U;
        gPairedRecoveryDeferred.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (actor.deadTargetSince == 0U) actor.deadTargetSince = now;
    if (now - actor.deadTargetSince < kDeadTargetGracePeriod) return;

    TargetSelectionSnapshot before{};
    static_cast<void>(ReadTargetSelectionSnapshot(actor, before));
    TraceTargetSelection(actor, TargetSuccessionPhase::RecoveryEvaluation, now, true);
    // Unknown selector state is not equivalent to "no successor". Do not run a
    // destructive recovery until selected/pending can both be revalidated.
    if (!TargetSelectionCertain(before)) {
        gPostDeathDecisionUncertain.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    bool survivor = LivingCharacter(LocalPlayer());
    for (std::uint32_t i = 0U; !survivor && i < gCurrent.participantCount; ++i)
        survivor = LivingCharacter(ParticipantForPeer(gCurrent.participants[i].peer));
    if (!survivor || !character_direct_step_runtime::FinishReplicatedInteraction(actor.character)) return;

    // The old pair is now retired. Arm the dead-melee guard immediately, even
    // though no global BT/task reset has happened. This keeps a new melee leaf
    // from being admitted against the terminal incarnation while retail gets a
    // chance to preserve/promote another target naturally.
    actor.recoveredInteractionRevision = state.interactionRevision;
    actor.postDeathArbitrationRevision = state.interactionRevision;
    actor.postDeathArbitrationSince = now;
    actor.postDeathRetiredPeer = state.interactionTargetPeer;
    actor.deadTargetSince = 0U;
    gDeadTargetRecoveries.fetch_add(1U, std::memory_order_relaxed);
    TraceTargetSelection(actor, TargetSuccessionPhase::PairRetired, now, true);

    TargetSelectionSnapshot after{};
    static_cast<void>(ReadTargetSelectionSnapshot(actor, after));
    if (!TargetSelectionCertain(after)) {
        gPostDeathDecisionUncertain.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (HasLivingSuccessor(after)) {
        gPostDeathSuccessionsPreserved.fetch_add(1U, std::memory_order_relaxed);
        TraceTargetSelection(actor, TargetSuccessionPhase::SuccessorPreserved, now, true);
        ClearPostDeathArbitration(actor);
        return;
    }
    // Deliberately return to the retail manager for at least a bounded window.
    // The fallback reset is evaluated on subsequent manager frames only.
    gPostDeathArbitrationDeferrals.fetch_add(1U, std::memory_order_relaxed);
}
// Retail "set target" sequence of the CATHODE script method 005FBB40:
// 0072AFB0 (ECX=selector [Alien+34A8], target on stack, RET 4) makes the
// target's record the selector's forced, perceived target (+C, record+493);
// 0072B880 (ECX=perception [Alien+34D8], char 0, RET 4) refreshes it.
bool AssignNativeTarget(const std::uintptr_t selector, const std::uintptr_t perception,
    const std::uintptr_t target) noexcept {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, std::uintptr_t)>(gBase + 0x0032AFB0U)(
            reinterpret_cast<void*>(selector), target);
        reinterpret_cast<void(__thiscall*)(void*, std::uint32_t)>(gBase + 0x0032B880U)(
            reinterpret_cast<void*>(perception), 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Once the Host is terminally dead, retail drops his target record and an
// unsensed survivor is never promoted: the selector stays empty and the
// Alien's tree remains in its last-resort BACKSTAGE_ONLY sweep, whose hold
// never ends without a target. In solo the player's record is never dropped
// while he lives. Give the idle Alien the living participant through the
// same native sequence level scripts use; pursuit, vent choice and emergence
// stay native. Never during a kill or paired flow, never over a living
// selected/pending target, never during the post-death arbitration window.
void AssignSurvivorTargetWhenIdle(Actor& actor, const ULONGLONG now) noexcept {
    constexpr ULONGLONG kSurvivorTargetPeriod = 3000U;
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        PairSourceBusy(actor.character) || actor.postDeathArbitrationRevision != 0U ||
        now - actor.lastSurvivorTargetAssignment < kSurvivorTargetPeriod) return;
    const auto realLocal = LocalPlayer();
    if (!TerminalDeadCharacter(realLocal) || NativeVictimFlowRunning(actor.character, realLocal)) return;
    TargetSelectionSnapshot selection{};
    if (!ReadTargetSelectionSnapshot(actor, selection) || !TargetSelectionCertain(selection) ||
        HasLivingSuccessor(selection)) return;
    std::uintptr_t selector{}, perception{}, forced{};
    if (!Read(actor.character + 0x34A8U, selector) || selector == 0U ||
        !Read(actor.character + 0x34D8U, perception) || perception == 0U ||
        !Read(selector + 0xCU, forced)) return;
    std::uint64_t peer{};
    const auto survivor = LivingAiParticipant(actor.character, peer);
    if (survivor == 0U || peer == 0U || !LivingCharacter(survivor) ||
        (HostilityDetour(reinterpret_cast<void*>(actor.character + 0x280U), nullptr, survivor) & 0xFFU) == 0U)
        return;
    std::uintptr_t forcedTarget{};
    if (forced != 0U && Read(forced + 0x42CU, forcedTarget) && forcedTarget == survivor) return;
    actor.lastSurvivorTargetAssignment = now;
    if (!AssignNativeTarget(selector, perception, survivor)) {
        gSurvivorTargetFaults.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    gSurvivorTargetsAssigned.fetch_add(1U, std::memory_order_relaxed);
    gLastSurvivorTargetPeer.store(peer, std::memory_order_relaxed);
}
// Removed by the v253-v330 rewrite; restored in v344 (the Host Alien stayed
// hidden in a vent for good after killing the Host, sweep counters at 0).

// .174 observed 17,523 new pair attempts against the terminally dead Host,
// after a successful BT reset and an Idle source. ActionMeleeAttack's start
// checks record lifetime, not health. Reject its NEW candidate during retail
// root selection, before promotion/execution; failure 8 is propagated by
// FUN_00726190 to the parent, which owns fallback and target switching.
// This does not edit target records/signals or call SetPending/Commit.
bool RetiredMeleeSelection(void* node, const std::uintptr_t owner,
    const std::uintptr_t execution, const std::uintptr_t tree, std::uint64_t& peer) noexcept {
    peer = 0U;
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() ||
        !Stable() || owner == 0U || gAiPlayerQueryOwner != owner) return false;
    std::uintptr_t table{}, frameNode{}, frameTree{}, treeOwner{}, waiter{}, waiterControl{};
    std::uint32_t mode{}, status{};
    std::uint8_t deferred{}, closed{};
    // Factory 006F0A10 initializes mode 3 for selection, an unstarted frame,
    // and empty waiter handles. Mode 1 belongs to execution and is untouched.
    if (!Read(reinterpret_cast<std::uintptr_t>(node), table) || table != gBase + 0x00ECB770U ||
        !Read(execution + 0x10U, frameNode) || frameNode != reinterpret_cast<std::uintptr_t>(node) ||
        !Read(execution + 0x14U, frameTree) || frameTree != tree ||
        !Read(tree + 0xCU, treeOwner) || treeOwner != owner ||
        !Read(execution + 8U, mode) || mode != 3U ||
        !Read(execution + 4U, status) || status != 0U ||
        !Read(execution + 0x1FU, deferred) || deferred == 0U ||
        !Read(execution + 0x1EU, closed) || closed != 0U ||
        !Read(execution + 0x2CU, waiter) || waiter != 0U ||
        !Read(execution + 0x30U, waiterControl) || waiterControl != 0U) return false;
    auto* actor = Find(owner);
    std::uint32_t runtimeId{};
    if (actor == nullptr || actor->tree != tree || actor->blocked || actor->faulted || actor->duplicate ||
        !Read(owner + 0x50U, runtimeId) || runtimeId != actor->runtimeId || PairSourceBusy(owner)) return false;
    gPostDeathMeleeSelectionChecks.fetch_add(1U, std::memory_order_relaxed);
    const auto& state = actor->desired;
    if ((state.stateFlags & InteractionValid) == 0U || state.interactionTargetPeer == 0U ||
        state.interactionRevision == 0U || actor->recoveredInteractionRevision != state.interactionRevision)
        return false;
    const auto target = ParticipantForPeer(state.interactionTargetPeer);
    if (target == 0U || !TerminalDeadCharacter(target) || NativeVictimFlowRunning(owner, target)) return false;
    std::uintptr_t cycle{};
    std::uint8_t cycleState{};
    bool active{};
    if (!ReadPairedCycleActivity(owner, cycle, cycleState, active) || cycleState != 1U || active) return false;
    std::uintptr_t selection{}, manager{}, selectedOwner{}, record{}, selectedTarget{};
    if (!Read(owner + 0x34A8U, selection) || selection == 0U ||
        !Read(selection + 4U, manager) || manager == 0U ||
        !Read(manager + 0x128U, selectedOwner) || selectedOwner != owner ||
        !Read(selection + 0x10U, record) || record == 0U ||
        !Read(record + 0x42CU, selectedTarget) || selectedTarget != target ||
        !RecordMatches(record, owner, target)) return false;
    peer = state.interactionTargetPeer;
    return true;
}
void __fastcall MeleeSelectionDetour(void* node, void*, const std::uintptr_t owner,
    const std::uintptr_t execution, const std::uintptr_t tree,
    const std::uint32_t previousFailure, const std::uint32_t previousSuccess) noexcept {
    // The redirected CALL is ONLY in root selection (007261EB). The distinct
    // execution CALL in 00712E20, active animation updates, and victim workers
    // remain retail. ECX=node, FIVE stack arguments, RET 14h (20 bytes).
    using Dispatch = void(__thiscall*)(void*, std::uintptr_t, std::uintptr_t,
        std::uintptr_t, std::uint32_t, std::uint32_t);
    const auto original = reinterpret_cast<Dispatch>(gBase + 0x002D0260U);
    std::uint64_t peer{};
    const bool reject = RetiredMeleeSelection(node, owner, execution, tree, peer);
    original(node, owner, execution, tree, previousFailure, previousSuccess);
    if (!reject) return;
    std::uint32_t status{};
    // For this certified deferred leaf, the original selection callback is
    // empty (00AEF7B0/RET 14h) and leaves status 1. No cycle/waiter has begun.
    // Retail unwinding then releases this native frame and tries alternatives.
    if (!Read(execution + 4U, status) || status != 1U || !Write(execution + 4U, std::uint32_t{8U})) {
        gPostDeathMeleeSelectionWriteFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    gPostDeathMeleeSelectionRejects.fetch_add(1U, std::memory_order_relaxed);
    gLastPostDeathRetiredPeer.store(peer, std::memory_order_relaxed);
}






void RecordActorExecutionTrace(const Actor& actor, ExecutionTraceSample sample) noexcept {
    const auto& state = actor.desired;
    sample.entityA=state.entityA;sample.entityB=state.entityB;sample.birth=state.birth;
    sample.constructorRva=state.transition.rva;sample.argumentKinds=state.transition.kinds;
    sample.argument0=state.transition.value[0];sample.argument1=state.transition.value[1];
    sample.commandId=state.executionAction.command;sample.globalSeq=state.executionAction.globalSequence;
    sample.ownerEpoch=state.executionAction.ownerEpoch;
    std::uintptr_t cycle{},owner{},table{};std::uint8_t localState{},serial{},flags{};
    if(Read(actor.character+0x36CU,cycle)&&cycle&&Read(cycle+4U,owner)&&owner==actor.character&&
        Read(cycle,table)&&table>=gBase&&Read(cycle+0x1AU,localState)&&Read(cycle+0x19U,serial)&&Read(cycle+8U,flags)) {
        sample.localType=static_cast<std::uint32_t>(table-gBase);sample.localState=localState;
        sample.localSerial=serial;sample.localFlags=flags;
    }
    RecordExecutionTrace(sample);
}
// Cycle snapshots are diagnostic only; executable native commands own behavior.

// Measured Host speed from successive authoritative poses (>=200ms apart).




// Engine thread, Intent owner only. Never during pairs, scripted/Host-replayed
// execution, death or backstage; the catastrophic relocation stays the last resort.


bool ExecutionRetirementQualified(const transactions::Scope& scope, const std::uint32_t generation) noexcept {
    std::uint64_t cookie{};
    for (std::size_t i = 0; i < 8; ++i) cookie |= static_cast<std::uint64_t>(scope.session[i]) << (i * 8U);
    if (cookie == 0) cookie = 1;
    const auto role = gRole.load(std::memory_order_acquire);
    return Installed() && !gStopping.load(std::memory_order_acquire) && EngineThread() &&
        (role == Role::Client || role == Role::FrozenClient) &&
        (gCurrent.role == Role::Client || gCurrent.role == Role::FrozenClient) && Stable() && gGenerations != nullptr &&
        generation == gGeneration && generation == gCurrent.nativeGeneration &&
        generation == gGenerations->CurrentGeneration() && scope.worldEpoch == gCurrent.worldGeneration &&
        cookie == gCurrent.sessionCookie;
}
bool ExecutionScopeQualified(const transactions::Scope& scope, const std::uint32_t generation) noexcept {
    return ExecutionRetirementQualified(scope,generation) &&
        gRole.load(std::memory_order_acquire) == Role::Client && gCurrent.role == Role::Client;
}
bool HostExecutionScopeQualified(const transactions::Scope& scope, const std::uint32_t generation) noexcept {
    std::uint64_t cookie{};
    for (std::size_t i = 0; i < 8; ++i) cookie |= static_cast<std::uint64_t>(scope.session[i]) << (i * 8U);
    if (!cookie) cookie = 1;
    return Installed() && !gStopping.load(std::memory_order_acquire) && EngineThread() && Stable() &&
        gRole.load(std::memory_order_acquire) == Role::Host && gCurrent.role == Role::Host &&
        gGenerations && generation == gGeneration && generation == gCurrent.nativeGeneration &&
        generation == gGenerations->CurrentGeneration() && scope.worldEpoch == gCurrent.worldGeneration &&
        cookie == gCurrent.sessionCookie;
}
void DrainHostLifecycleEvents() noexcept;
bool HostNativeCaptureQualified() noexcept {
    if(!EngineThread())return false; // Workers must not read gCurrent.
    transactions::Scope scope{};
    return Installed() && host_producer::HostScope(scope) &&
        HostExecutionScopeQualified(scope,gCurrent.nativeGeneration);
}
bool HostNativeCaptureGuard() noexcept {
    if(!HostNativeCaptureQualified())return false;
    // Worker events are value-only. Consume them before another engine-thread
    // producer emits a later command for the same authority.
    if(!gLifecycleDrainActive&&gLifecycleEventsPending.load(std::memory_order_acquire))DrainHostLifecycleEvents();
    return HostNativeCaptureQualified();
}
bool HostNativeCaptureContext(std::uintptr_t character, transactions::OwnerKey& owner) noexcept {
    // Native retirement remains qualified separately. Manual actors must not
    // enter the Coop command producer while waiting for their possession.
    if (!HostNativeCaptureGuard() || (gPvPLease.owned && PvPCharacterFenced(character))) return false;
    const auto* actor = Find(character); std::uint32_t runtime{};
    // Reserve manual Aliens before possession, otherwise the Client's Coop
    // executor owns the actor before ALIEN_PVP_BIND can acquire it.
    if (actor && (gCurrent.flags & kAlienPvP) != 0U && actor->characterClass == 1U) return false;
    if (!actor || actor->duplicate || actor->faulted || !actor->birth || !actor->characterClass ||
        (actor->entityA == 0 && actor->entityB == 0) || !Read(character + 0x50U,runtime) || runtime != actor->runtimeId) return false;
    owner = {actor->entityA,actor->entityB,actor->birth,actor->characterClass,transactions::Capability::Movement,
        NativeOwnerEpoch(gCurrent.nativeGeneration,actor->birth)};
    return true;
}
bool HostNativeRetirementQualified(const transactions::Scope& scope, const transactions::OwnerKey& owner,
    std::uintptr_t character, std::uint32_t runtime) noexcept {
    if (!HostExecutionScopeQualified(scope,gCurrent.nativeGeneration)) return false;
    const auto* actor = Find(character); std::uint32_t current{};
    return actor && !actor->duplicate && !actor->faulted && actor->runtimeId == runtime &&
        actor->entityA == owner.entityA && actor->entityB == owner.entityB && actor->birth == owner.birth &&
        actor->characterClass == owner.actorClass && owner.ownerEpoch == NativeOwnerEpoch(gCurrent.nativeGeneration,actor->birth) &&
        Read(character+0x50U,current) && current == runtime;
}
bool HostBindingQualified(const transactions::Scope& scope, std::uint32_t generation) noexcept {
    return HostExecutionScopeQualified(scope,generation) &&
        (host_producer::EngineProducer().Current(scope,generation) ||
         host_producer::NativeIdle() || host_producer::NativeReadyForNewWorld(scope));
}
bool HostRouteQualified(const transactions::Scope& scope) noexcept {
    return HostExecutionScopeQualified(scope,gCurrent.nativeGeneration);
}
bool ClientRouteQualified(const transactions::Scope& scope) noexcept {
    return ExecutionScopeQualified(scope,gCurrent.nativeGeneration);
}
execution::OwnershipCheck ResolveTransactionActor(void*, const transactions::Scope& scope,
    const transactions::OwnerKey& owner, execution::ActorLifetime& lifetime) noexcept {
    lifetime = {};
    if (!ExecutionRetirementQualified(scope,gCurrent.nativeGeneration)) return execution::OwnershipCheck::Waiting;
    auto* actor = FindActorByIdentity(owner.entityA,owner.entityB);
    if (!actor) return execution::OwnershipCheck::Waiting;
    if (actor->duplicate || actor->faulted || actor->characterClass != owner.actorClass || !owner.birth ||
        owner.capability != transactions::Capability::Movement || !owner.ownerEpoch) return execution::OwnershipCheck::Reject;
    std::uint32_t runtime{};
    if (!Read(actor->character + 0x50U,runtime) || runtime != actor->runtimeId) return execution::OwnershipCheck::Reject;
    // The authenticated Host incarnation is mapped once to this exact local
    // Character. A later birth must retire the old owner before remapping.
    if (actor->remoteBirth && actor->remoteBirth != owner.birth) return execution::OwnershipCheck::Reject;
    character_incarnations::Ticket ticket{};
    auto& registry=character_incarnations::NativeRegistry();
    auto found=registry.Find(gCurrent.nativeGeneration,actor->character,ticket);
    if(found==character_incarnations::Result::Missing||found==character_incarnations::Result::WrongGeneration)
        found=registry.BindExisting(gCurrent.nativeGeneration,actor->character,runtime,ticket);
    if((found!=character_incarnations::Result::Ok&&found!=character_incarnations::Result::Duplicate)||
        ticket.runtimeId!=runtime||!registry.Current(ticket))return execution::OwnershipCheck::Unknown;
    lifetime = {actor->character,runtime,gCurrent.nativeGeneration,ticket.serial}; return execution::OwnershipCheck::Ready;
}
NativeSchedulerParent CaptureNativeSchedulerParent(const std::uintptr_t scheduler) noexcept {
    NativeSchedulerParent result{};
    if(!HostNativeCaptureQualified())return result;
    std::uintptr_t head{},first{},request{},owner{};std::uint32_t count{},kind{},status{};
    if(!Read(scheduler,result.character)||scheduler!=result.character+0x2CD0U||
        !Read(scheduler+4U,head)||!head||!Read(scheduler+8U,count)||
        !Read(head,first)||!first)return {};
    // A completed head has already left the queue when the scheduler calls
    // its ordinary fallback. Retain the represented parent, not a guessed pose
    // or a different request selected by that callback.
    if(!count||first==head)return CurrentNativeSchedulerParent(scheduler);
    if(!Read(first+8U,request)||!request||
        !Read(request+0x14U,owner)||owner!=scheduler||!Read(request+0x10U,kind)||
        !Read(request+0xCU,status)||(status!=1U&&status!=2U)||
        (kind!=1U&&kind!=2U&&kind!=3U&&kind!=4U&&kind!=7U&&kind!=9U&&!scheduler_control::Supported(kind))||
        !Read(result.character+0x50U,result.runtime)||
        !host_producer::EngineProducer().Identify(request,result.identity)||result.identity.nativeKind!=kind||
        !HostNativeRetirementQualified(result.identity.scope,result.identity.owner,result.character,result.runtime))return {};
    result.covered=true;return result;
}
NativeSchedulerParent CurrentNativeSchedulerParent(std::uintptr_t scheduler) noexcept {
    if(!EngineThread()||gNativeSchedulerOverflow||!gNativeSchedulerDepth)return {};
    const auto& frame=gNativeSchedulerFrames[gNativeSchedulerDepth-1U];const auto& parent=frame.parent;
    return frame.scheduler==scheduler&&parent.covered&&
        HostNativeRetirementQualified(parent.identity.scope,parent.identity.owner,parent.character,parent.runtime)?parent:NativeSchedulerParent{};
}
void ObserveNativeScheduler(std::uintptr_t scheduler,bool entering) noexcept {
    if(entering) {
        if(gNativeSchedulerOverflow||gNativeSchedulerDepth==gNativeSchedulerFrames.size()) {
            ++gNativeSchedulerOverflow;
            if(HostNativeCaptureQualified())host_producer::EngineProducer().LoseCoverage(execution::Recovery::Capacity);
            return;
        }
        const auto parent=CaptureNativeSchedulerParent(scheduler);
        gNativeSchedulerFrames[gNativeSchedulerDepth++]={scheduler,parent};
    } else if(gNativeSchedulerOverflow)--gNativeSchedulerOverflow;
    else if(gNativeSchedulerDepth)gNativeSchedulerFrames[--gNativeSchedulerDepth]={};
}
// 006799D0 exits native scheduler mode6 for mode7 through this exact CALL.
// Admission order does not guarantee that a delayed Client ticks mode6 before
// its replacement. Capture the Host constructor as authoritative work; suppress
// only the same local derived constructor if the Client actually takes this exit.
void __fastcall ControlStanceFallbackDetour(void* cycle,void*) noexcept {
    const auto address=reinterpret_cast<std::uintptr_t>(cycle);
    const bool savedCovered=gNativeFallbackCovered;
    const auto savedCycle=gAuthoritativeStanceFallbackCycle;
    gNativeFallbackCovered=false;
    gAuthoritativeStanceFallbackCycle=0U;
    transactions::Scope scope{};std::uintptr_t character{},current{};std::uint32_t runtime{};
    if(EngineThread()&&execution_runtime::CurrentScope(scope)&&ExecutionScopeQualified(scope,gCurrent.nativeGeneration)&&
        Read(address+4U,character)&&Read(character+0x36CU,current)&&current==address) {
        const auto* actor=Find(character);
        if(actor&&!actor->duplicate&&!actor->faulted&&actor->initialPlacementReady&&actor->remoteBirth&&
            Read(character+0x50U,runtime)&&runtime==actor->runtimeId)
            gAuthoritativeStanceFallbackCycle=address;
    }
    __try {reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x25BDD0U)(cycle);}
    __finally {gNativeFallbackCovered=savedCovered;gAuthoritativeStanceFallbackCycle=savedCycle;}
}
// A wire contract selects a locally certified native ABI. No address, observed
// cycle state, or Character pointer received over the network selects a call.
struct NativeIntentEntry { std::uint32_t entry{},type{},state{},arity{}; };
// Exact 00d6 wrappers. The scheduler kind5 only reserves the cycle created
// here; replaying that reservation cannot construct these actions.
constexpr std::array<NativeIntentEntry,4U> kPassageIntents{{
    {0x24A8A0U,0xDBB2A0U,0x4BU,1U}, // borrowed XYZ, retail RET4
    {0x24A9C0U,0xDBB1B8U,0x4DU,0U}, // fixed zero-duration initializer
    {0x24AA50U,0xDBB1B8U,0x4DU,1U}, // authored float duration, retail RET4
    // 0064A930 RET8 ->004017E0 copies XYZ and a speed-table selector.
    {0x24A930U,0xDBB360U,0x4CU,2U},
}};
NativeIntentEntry NativeIntentContract(const std::uint32_t id) noexcept {
    if(id>=1U&&id<=7U) {
        const auto& c=kExecutionTransitions[id-1U];
        return {c.entryRva,c.typeRva,c.state,c.ignoredStackArgument?1U:0U};
    }
    if(id>=8U&&id<=19U) {
        const auto& c=kExecutionTransitionContracts[id-8U];return {c.rva,c.typeRva,c.state,c.arity};
    }
    if(id>=21U&&id<=23U)return kPassageIntents[id-21U];
    if(id==native_intent::kPassageTraverseContract)return kPassageIntents[3U];
    return id==20U?NativeIntentEntry{0x2579F0U,0xEADB58U,0x12U,2U}:NativeIntentEntry{};
}
bool NativeIntentActorCurrent(const transactions::Record& record,const execution::LocalActorBinding& binding) noexcept {
    if(!ExecutionScopeQualified(record.scope,gCurrent.nativeGeneration))return false;
    const auto* actor=Find(binding.character);std::uint32_t runtime{};
    character_incarnations::Ticket ticket{};auto& registry=character_incarnations::NativeRegistry();
    return actor&&!actor->duplicate&&!actor->faulted&&actor->initialPlacementReady&&
        actor->entityA==record.owner.entityA&&actor->entityB==record.owner.entityB&&
        actor->characterClass==record.owner.actorClass&&(!actor->remoteBirth||actor->remoteBirth==record.owner.birth)&&
        actor->runtimeId==binding.runtimeId&&Read(binding.character+0x50U,runtime)&&runtime==binding.runtimeId&&
        registry.Find(gCurrent.nativeGeneration,binding.character,ticket)==character_incarnations::Result::Ok&&
        ticket.runtimeId==runtime&&ticket.serial==binding.creationSerial&&registry.Current(ticket);
}
bool LocomotionExitIntent(const transactions::Record& record) noexcept {
    return record.nativeKind==native_intent::kKind&&
        (record.intent.words[0]==1U||record.intent.words[0]==2U||record.intent.words[0]==7U);
}
execution::Readiness CheckNativeIntent(const transactions::Record& record,const execution::LocalActorBinding& binding) noexcept {
    if(!execution::ValidateExecutable(record)||native_intent::Empty(record.intent)||
        !NativeIntentActorCurrent(record,binding))return execution::Readiness::Rejected;
    const auto* actor=Find(binding.character);
    if(!actor||health::Terminal(actor->desired.health))return execution::Readiness::Rejected;
    // An exact ordered Host exit may replace its old reaction. Waiting for
    // that local reaction to finish would block the operation that exits it.
    if(actor->reactionCycle&&!LocomotionExitIntent(record)) {
        std::uintptr_t current{};std::uint8_t serial{},flags{};
        if(!Read(binding.character+0x36CU,current)||!Read(current+0x19U,serial)||!Read(current+8U,flags))
            return execution::Readiness::Waiting;
        if(current==actor->reactionCycle&&serial==actor->reactionSerial&&(flags&6U)==0U)return execution::Readiness::Waiting;
    }
    std::uintptr_t cycle{},owner{};
    if(PairTransactionReserved(record.scope,record.owner,record.globalSeq)||PairSourceBusy(binding.character)||
        !character_direct_step_runtime::HasLiveMovementResources(binding.character)||
        !Read(binding.character+0x36CU,cycle)||!cycle||!Read(cycle+4U,owner)||owner!=binding.character)
        return execution::Readiness::Waiting;
    if(record.intent.words[0]==20U) {
        native_intent::Payload isolated{};
        if(!native_intent::IsolateObserverScripted(record.intent,isolated))return execution::Readiness::Rejected;
        // Foreign scene refs are intentionally not resolved on an observer.
        // In particular, THE_PLAYER must remain this PC's own camera/player.
    }
    return execution::Readiness::Ready;
}
Actor* LiveAuthoritativePassage(const std::uintptr_t character,const std::uintptr_t expectedCycle=0U) noexcept {
    if(!EngineThread()||!Client()||!PairGenerationStable())return nullptr;
    auto* actor=Find(character);
    if(!actor||!actor->passageSequence||!actor->passageCommand||actor->duplicate||actor->faulted||
        !actor->initialPlacementReady)return nullptr;
    if(expectedCycle&&expectedCycle!=actor->replayedCycle.cycle)return nullptr;
    transactions::Scope scope{};execution::ActorLifetime lifetime{};
    const execution::OwnershipMemory memory{gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
        return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
    }};
    if(!execution_runtime::CurrentScope(scope)||!ExecutionScopeQualified(scope,gCurrent.nativeGeneration)||
        ResolveTransactionActor(nullptr,scope,actor->replayedCycle.owner,lifetime)!=execution::OwnershipCheck::Ready||
        health::Terminal(actor->desired.health)||
        !execution::IsLiveNativePassage(memory,actor->replayedCycle,scope,actor->replayedCycle.owner,lifetime)) {
        actor->passageSequence=actor->passageCommand=0U;
        return nullptr;
    }
    return actor;
}
bool RetainAuthoritativePassage(const std::uintptr_t character,const std::uintptr_t cycle,
    const std::uint32_t requestedState) noexcept {
    if(!execution::OrdinaryPassageTransition(requestedState))return false;
    const auto* actor=LiveAuthoritativePassage(character,cycle);
    if(!actor)return false;
    // Only a real, newer Host invocation may interrupt this live local75/76.
    // Local movement continuation never installs this synchronous context.
    const auto invocation=execution::CurrentNativeInvocation();
    const auto* record=invocation.record;
    const auto& anchor=actor->replayedCycle;
    const execution::ActorLifetime invoked{invocation.actor.character,invocation.actor.runtimeId,
        gCurrent.nativeGeneration,invocation.actor.creationSerial};
    if(record&&invoked==anchor.actor&&record->scope==anchor.scope&&record->owner==anchor.owner&&
        record->globalSeq>actor->passageSequence&&record->commandId!=actor->passageCommand&&
        record->opcode==transactions::Op::Submit) {
        if(gNativeIntentDispatchActive&&record->nativeKind==native_intent::kKind)return false;
        // The wrapper relocations call controller callbacks before their
        // common position primitive. Permit those exact Host callbacks too.
        if(execution::SupersedesNativePassage(anchor,actor->passageSequence,actor->passageCommand,
            *record,transactions::NativeOutcome::Admitted))return false;
    }
    ++gPassageTransitionsHeld;
    return true;
}
bool DispatchNativeIntent(const std::uintptr_t cycle,const NativeIntentEntry entry,const std::uint32_t* arguments) noexcept {
    const bool previous=gNativeIntentDispatchActive;
    gNativeIntentDispatchActive=true;
    __try {
        __try {
            if(entry.arity==0U)reinterpret_cast<void(__thiscall*)(void*)>(gBase+entry.entry)(reinterpret_cast<void*>(cycle));
            else if(entry.arity==1U)reinterpret_cast<void(__thiscall*)(void*,std::uint32_t)>(gBase+entry.entry)(reinterpret_cast<void*>(cycle),arguments[0]);
            else if(entry.arity==2U)reinterpret_cast<void(__thiscall*)(void*,std::uint32_t,std::uint32_t)>(gBase+entry.entry)(reinterpret_cast<void*>(cycle),arguments[0],arguments[1]);
            else return false;
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    } __finally {gNativeIntentDispatchActive=previous;}
}
transactions::NativeOutcome InvokeNativeIntent(const transactions::Record& record,const execution::LocalActorBinding& binding) noexcept {
    using transactions::NativeOutcome;
    if(CheckNativeIntent(record,binding)!=execution::Readiness::Ready)return NativeOutcome::Unknown;
    // Host locomotion start/stop constructors (55/03/59) are pure presentation
    // of its own Move. With the direct pose motor already holding an ordinary
    // locomotion cycle, replaying them destroyed that cycle: 511 rebuilt the
    // native graph on every Host start/stop (move_activate from 55/59). They
    // still run when they are the Host's real exit from a reaction/script.
    if(LocomotionExitIntent(record)) {
        if(auto* actor=Find(binding.character);actor&&(actor->recoveryNativeArmed||kNativeFollowLocomotion)) {
            std::uintptr_t cycle{},table{};std::uint8_t state{};
            if(Read(binding.character+0x36CU,cycle)&&cycle&&Read(cycle,table)&&table>=gBase&&
                Read(cycle+0x1AU,state)&&
                HybridMayConsumeConstructor(record.intent.words[0],static_cast<std::uint32_t>(table-gBase),state)) {
                actor->reactionCycle=0U;
                gDirectLocomotionConstructorsConsumed.fetch_add(1U,std::memory_order_relaxed);
                return NativeOutcome::Admitted;
            }
        }
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(binding.character));
    }
    // Constructors destroy the previous cycle before initializing the new one.
    // A readiness probe releases its temporary Ref before that destruction;
    // keep the retail movement resource alive across callbacks and readback.
    const character_direct_step_runtime::MovementResourceLease resources(binding.character);
    if(!resources||!NativeIntentActorCurrent(record,binding))return NativeOutcome::Unknown;
    // Receipts keep the exact Host instruction. Only the local native call is
    // isolated from global CATHODE placement/camera/clock references.
    native_intent::Payload isolated{};
    const native_intent::Payload* replay=&record.intent;
    if(record.intent.words[0]==20U) {
        if(!native_intent::IsolateObserverScripted(record.intent,isolated))return NativeOutcome::Rejected;
        replay=&isolated;
    }
    const auto& w=replay->words;const auto entry=NativeIntentContract(w[0]);
    std::uint32_t args[2]{w[2],w[3]};alignas(16) std::uint8_t identities[2][0x60]{};
    // 006586A0 -> 00401410 copies with MOVAPS: the pointed-to vector,
    // not just the enclosing stack frame, must be aligned to sixteen bytes.
    alignas(16) std::uint32_t vector[4]{w[8],w[9],w[10],w[11]};
    alignas(16) std::uint32_t scripted[22]{};
    for(std::size_t i=0;i<2U;++i) {
        const auto kind=(w[1]>>(4U*i))&15U;
        if(kind==static_cast<std::uint32_t>(ExecutionArgKind::Identity)) {
            std::memcpy(identities[i]+0x54U,&w[4U+i],4U);std::memcpy(identities[i]+0x58U,&w[6U+i],4U);
            args[i]=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(identities[i]));
        } else if(kind==static_cast<std::uint32_t>(ExecutionArgKind::Vector))
            args[i]=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(vector));
        else if(kind==static_cast<std::uint32_t>(ExecutionArgKind::Null))args[i]=0U;
    }
    if(w[0]==20U) {
        std::copy_n(w.begin()+12,22U,scripted);
        args[0]=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(scripted));args[1]=w[34];
    }
    std::uintptr_t cycle{},current{},type{};std::uint8_t state{},flags{};
    if(!entry.entry||!Read(binding.character+0x36CU,cycle)||!cycle||!DispatchNativeIntent(cycle,entry,args)||
        !NativeIntentActorCurrent(record,binding)||!Read(binding.character+0x36CU,current)||!current||
        !Read(current,type)||!Read(current+4U,cycle)||cycle!=binding.character||
        !Read(current+0x1AU,state)||!Read(current+8U,flags)) {
        ++gCycleCallsReplayFailed;return NativeOutcome::Unknown;
    }
    // Initializers can synchronously finish or replace a short-lived cycle.
    // The operation is the exact call, never an obligation to force its final
    // sampled class/state. Only the still-current class's explicit failure is
    // attributable to this constructor.
    if(type==gBase+entry.type&&state==entry.state&&(flags&4U)!=0U&&
        !native_intent::CompletesAtConstruction(record.intent)){++gCycleCallsReplayFailed;return NativeOutcome::Rejected;}
    // Exact local provenance, not a replicated cycle snapshot. An instruction
    // may legitimately run before scheduler ownership exists. Its resulting
    // live cycle must not later be mistaken for a competing local decision.
    std::uint8_t serial{};
    if(!Read(current+0x19U,serial))return NativeOutcome::Unknown;
    if(auto* actor=Find(binding.character)) {
        actor->replayedCycle={record.scope,record.owner,
            {binding.character,binding.runtimeId,gCurrent.nativeGeneration,binding.creationSerial},current,serial};
        actor->passageSequence=actor->passageCommand=0U;
        if((w[0]==21U||w[0]==native_intent::kPassageTraverseContract)&&
            type==gBase+entry.type&&state==entry.state&&(flags&0x06U)==0U) {
            actor->passageSequence=record.globalSeq;actor->passageCommand=record.commandId;
        }
        if(LocomotionExitIntent(record))actor->reactionCycle=0U;
    } else return NativeOutcome::Unknown;
    ++gCycleCallsReplayed;return NativeOutcome::Admitted;
}
bool ExecutionOwnedCharacter(const std::uintptr_t character) noexcept {
    return PairReservedCharacter(character)||PairSourceBusy(character)||LiveAuthoritativePassage(character)!=nullptr;
}
bool QueryNativeHostProgress(void*,const execution::RetirementIdentity& identity,movement::HostProgress& output) noexcept {
    transactions::Scope scope{};execution::ActorLifetime lifetime{};
    if(!EngineThread()||!Client()||!execution_runtime::CurrentScope(scope)||scope!=identity.scope||
        ResolveTransactionActor(nullptr,scope,identity.owner,lifetime)!=execution::OwnershipCheck::Ready||
        lifetime.character!=identity.actor.character||lifetime.runtimeId!=identity.actor.runtimeId||
        lifetime.creationSerial!=identity.actor.creationSerial)return false;
    auto* actor=FindActorByIdentity(identity.owner.entityA,identity.owner.entityB);
    if(!actor||actor->duplicate||actor->faulted||!actor->initialPlacementReady)return false;
    const auto& state=actor->recoveryPose;const auto now=GetTickCount64();
    if(!actor->recoveryProgress.Fresh(now)||(state.motionFlags&1U)==0U||
        state.birth!=identity.owner.birth||state.characterClass!=identity.owner.actorClass||
        state.sessionCookie!=gCurrent.sessionCookie||state.worldGeneration!=scope.worldEpoch||
        (state.stateFlags&PoseValid)==0U||state.moveType>3U)return false;
    for(std::size_t i=0;i<3;++i){output.position[i]=state.position[i];output.velocity[i]=state.motionVelocity[i];}
    output.gait=state.moveType;
    output.protectedMotion=(state.executionFlags&ExecutionPaired)!=0U||(state.motionFlags&6U)!=0U||
        ProxyWithdrawn(state)||health::Terminal(state.health);
    std::array<float,3> local{};
    if(!output.protectedMotion&&Read(actor->character+kPositionOffset,local)&&movement::FinitePoint(local))
        output.observed=actor->observedTrail.Goal(local);
    return movement::FinitePoint(output.position)&&movement::FinitePoint(output.velocity);
}
bool LifecyclePlacementReady(const std::uintptr_t character) noexcept {
    const auto* actor=Find(character);return actor!=nullptr&&actor->initialPlacementReady;
}
bool TransactionActorMatches(const execution::OwnershipToken& token, Actor*& actor) noexcept {
    execution::ActorLifetime current{}; actor = nullptr;
    if (ResolveTransactionActor(nullptr,token.scope,token.owner,current) != execution::OwnershipCheck::Ready || current != token.actor) return false;
    actor = Find(current.character); return actor != nullptr;
}
bool OwnsReplayedCycle(const Actor& actor,const transactions::Scope& scope,const transactions::OwnerKey& owner,
    const execution::ActorLifetime& lifetime) noexcept {
    const execution::OwnershipMemory memory{gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
        return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
    }};
    return execution::MatchesNativeExecution(memory,actor.replayedCycle,scope,owner,lifetime);
}
execution::OwnershipCheck PrepareTransactionActor(void*, const transactions::Scope& scope,
    const transactions::OwnerKey& owner, const execution::ActorLifetime& lifetime) noexcept {
    gHandoffStage=1;
    execution::ActorLifetime current{};
    if (ResolveTransactionActor(nullptr,scope,owner,current) != execution::OwnershipCheck::Ready || current != lifetime)
        return execution::OwnershipCheck::Unknown;
    const auto* actor = Find(current.character);
    gHandoffStage=2;
    if(actor&&!actor->initialPlacementReady)return execution::OwnershipCheck::Waiting;
    gHandoffStage=3;
    if (!actor || PairSourceBusy(actor->character))
        return execution::OwnershipCheck::Waiting;
    // A pending pair is not a second executor. Its correlated action must
    // acquire this ownership before pair admission can start. The actual
    // native cycle boundary below still rejects a running 4E/4F/traversal.
    const execution::OwnershipMemory memory{gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
        return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
    }};
    execution::OwnershipBoundaryFailure failure{};
    gHandoffStage=4;
    const auto ready=OwnsReplayedCycle(*actor,scope,owner,lifetime)?
        execution::InspectAuthoritativeExecutionBoundary(memory,lifetime,false,&failure):
        (!actor->replayedCycle.cycle||actor->replayedCycle.scope!=scope||
         actor->replayedCycle.owner!=owner||actor->replayedCycle.actor!=lifetime)?
        execution::InspectInitialBackstageBoundary(memory,lifetime,false,&failure):
        execution::InspectMovementBoundary(memory,lifetime,false,&failure);
    gHandoffResult=static_cast<std::uint32_t>(ready);gHandoffOffset=failure.offset;gHandoffValue=failure.value;
    return ready;
}
execution::OwnershipCheck NativeMovementQuiescent(const execution::OwnershipToken& token,
    execution::OwnershipBoundaryFailure* output=nullptr) noexcept {
    if(output)*output={};
    Actor* actor{}; if (!TransactionActorMatches(token,actor)) return execution::OwnershipCheck::Unknown;
    gHandoffStage=5;
    const character_direct_step_runtime::MovementResourceLease resources(actor->character);
    if (!resources || PairSourceBusy(actor->character)) return execution::OwnershipCheck::Waiting;
    const execution::OwnershipMemory memory{gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
        return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
    }};
    execution::OwnershipBoundaryFailure failure{};
    gHandoffStage=6;
    const auto ready=OwnsReplayedCycle(*actor,token.scope,token.owner,token.actor)?
        execution::InspectAuthoritativeExecutionBoundary(memory,token.actor,true,&failure):
        (!actor->replayedCycle.cycle||actor->replayedCycle.scope!=token.scope||
         actor->replayedCycle.owner!=token.owner||actor->replayedCycle.actor!=token.actor)?
        execution::InspectInitialBackstageBoundary(memory,token.actor,true,&failure):
        execution::InspectMovementBoundary(memory,token.actor,true,&failure);
    if(output)*output=failure;
    gHandoffResult=static_cast<std::uint32_t>(ready);gHandoffOffset=failure.offset;gHandoffValue=failure.value;
    return ready;
}
bool ResetDrainedNavigation(const std::uintptr_t navigation) noexcept {
    __try {
        // 00462640 initializes N+40 but leaves N+44 untouched. 0044D200
        // resets both before inspecting B. Call only with empty B lists,
        // no current B, crossing or pending result, under the pinned M.
        reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x4D200U)(reinterpret_cast<void*>(navigation));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ClearDrainedNavigation(const std::uintptr_t navigation,const bool history) noexcept {
    __try {
        // 00456BD0 clears the route through 00741570, resets the native cursor
        // and link. N+10 is the second list of the same owned node Ref type.
        // Call retail destructors; never zero a list or copy a foreign Ref.
        if(history)reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x341570U)(
            reinterpret_cast<void*>(navigation+0x10U));
        else reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x56BD0U)(reinterpret_cast<void*>(navigation));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
execution::OwnershipCheck DrainTransactionNavigation(const execution::OwnershipToken& token) noexcept {
    using Check=execution::OwnershipCheck;
    execution::OwnershipBoundaryFailure failure{};
    const auto quiet=NativeMovementQuiescent(token,&failure);
    if(quiet!=Check::Waiting)return quiet;
    // A stopped scheduler leaves cached lists, and a never-used N can still
    // have an uninitialized +44. Normalize through retail before requiring
    // strict quiescence; retirement itself must keep the zero-value proof.
    const auto refusal=failure.offset;
    if(refusal!=0x10004U&&refusal!=0x10014U&&refusal!=0x10044U)return Check::Waiting;
    Actor* actor{};if(!TransactionActorMatches(token,actor))return Check::Unknown;
    const character_direct_step_runtime::MovementResourceLease resources(actor->character);
    if(!resources||PairSourceBusy(actor->character))return Check::Waiting;
    std::uintptr_t movement{},control{};
    if(!Read(actor->character+0x42A8U,movement)||!movement||!Read(actor->character+0x42ACU,control)||!control)return Check::Unknown;
    const auto navigation=movement+0xC4U;
    std::uintptr_t navigationOwner{};
    if(!Read(navigation+0x64U,navigationOwner)||navigationOwner!=movement)return Check::Unknown;
    // No request, block, pending result or active crossing may survive this
    // boundary. A native worker's consumer Ref is never cleared to fake quiet.
    for(const auto offset:{0x2CD8U,0x2CE4U}) {
        std::uint32_t count{};if(!Read(actor->character+offset,count))return Check::Unknown;
        if(count){gHandoffOffset=offset;gHandoffValue=count;return Check::Waiting;}
    }
    for(const auto offset:{0x24U,0x2CU,0x38U,0x40U,0x5CU,0x60U}) {
        std::uint32_t value{};if(!Read(navigation+offset,value))return Check::Unknown;
        if(value){gHandoffOffset=0x10000U+offset;gHandoffValue=value;return Check::Waiting;}
    }
    // N+44 is reset by retail without dereferencing its previous value.
    // The lists retail walks must independently be proven empty and valid.
    for(const auto offset:{0x28U,0x34U}) {
        std::uintptr_t sentinel{},next{},previous{};
        if(!Read(navigation+offset,sentinel)||!sentinel||!Read(sentinel,next)||
            !Read(sentinel+4U,previous)||next!=sentinel||previous!=sentinel)return Check::Unknown;
    }
    if(actor->transactionNavigationDrainEntered)return Check::Unknown;
    actor->transactionNavigationDrainEntered=true;
    gHandoffStage=7;
    if(!ResetDrainedNavigation(navigation)||!TransactionActorMatches(token,actor))return Check::Unknown;
    ++gHandoffNavigationResets;
    std::uintptr_t after{},afterControl{};
    if(!Read(actor->character+0x42A8U,after)||after!=movement||
        !Read(actor->character+0x42ACU,afterControl)||afterControl!=control)return Check::Unknown;
    gHandoffStage=8;
    if(!ClearDrainedNavigation(navigation,false)||!TransactionActorMatches(token,actor))return Check::Unknown;
    if(!Read(actor->character+0x42A8U,after)||after!=movement||
        !Read(actor->character+0x42ACU,afterControl)||afterControl!=control)return Check::Unknown;
    if(!ClearDrainedNavigation(navigation,true)||!TransactionActorMatches(token,actor)||
        !Read(actor->character+0x42A8U,after)||after!=movement||
        !Read(actor->character+0x42ACU,afterControl)||afterControl!=control)return Check::Unknown;
    ++gHandoffNavigationClears;
    return NativeMovementQuiescent(token);
}
execution::OwnershipCheck DrainTransactionActor(void*, const execution::OwnershipToken& token) noexcept {
    ++gHandoffAttempts;
    Actor* actor{}; if (!TransactionActorMatches(token,actor)) return execution::OwnershipCheck::Unknown;
    if (actor->transactionDrainStarted && actor->transactionDrainSerial != token.serial) return execution::OwnershipCheck::Unknown;
    // A Host action needs an empty scheduler: retire the local follow Move.
    ReleaseNativeFollow(*actor);
    if (!actor->transactionDrainStarted) {
        const auto prepared = PrepareTransactionActor(nullptr,token.scope,token.owner,token.actor);
        if (prepared != execution::OwnershipCheck::Ready) return prepared;
        const character_direct_step_runtime::MovementResourceLease resources(actor->character);
        if (!resources || PairSourceBusy(actor->character)) return execution::OwnershipCheck::Waiting;
        actor->transactionDrainStarted = true; actor->transactionDrainSerial = token.serial;
        actor->remoteBirth = token.owner.birth;
        // The coordinator fenced the capability before these native drains.
        if (!TransactionActorMatches(token,actor)) return execution::OwnershipCheck::Unknown;
        combat::Cancel(actor->character);
        if (!TransactionActorMatches(token,actor) || (actor->tree && !SuspendExistingTree(*actor)) ||
            !TransactionActorMatches(token,actor) || !ClearRoleTaskExecutor(*actor) ||
            !TransactionActorMatches(token,actor) || !character_direct_step_runtime::Stop(actor->character) ||
            !TransactionActorMatches(token,actor) || !character_direct_step_runtime::ReleaseAIIntentOwnership(actor->character) ||
            !TransactionActorMatches(token,actor)) return execution::OwnershipCheck::Unknown;
        actor->convergenceActive = actor->pursuitActive = false;
        actor->poseCount = 0; actor->poseInterpolating = false;
        actor->transactionDrained = true;
    }
    return actor->transactionDrained ? DrainTransactionNavigation(token) : execution::OwnershipCheck::Unknown;
}
execution::OwnershipCheck TransactionNativeReleased(void*, const execution::OwnershipToken& token) noexcept {
    return NativeMovementQuiescent(token);
}
execution::OwnershipCheck RestoreTransactionActor(void*, const execution::OwnershipToken& token) noexcept {
    Actor* actor{}; if (!TransactionActorMatches(token,actor)) return execution::OwnershipCheck::Unknown;
    const auto quiet = NativeMovementQuiescent(token); if (quiet != execution::OwnershipCheck::Ready) return quiet;
    actor->transactionDrainStarted = actor->transactionDrained = actor->transactionNavigationDrainEntered = false; actor->transactionDrainSerial = 0;
    // Release restores capability bookkeeping only. The autonomous Client BT
    // is never restored here.
    return execution::OwnershipCheck::Ready;
}
void NoteExecutionHold(Actor& actor,std::uint32_t reason) noexcept {
    if(actor.desired.executionRevision&&actor.completedExecutionRevision!=actor.desired.executionRevision){
        ++actor.execWaits;actor.execWaitReason=reason;actor.execWaitMask|=1U<<reason;
    }
}
bool ApplyCommandAction(Actor& actor, const ULONGLONG now) noexcept {
    // 6: a paired interaction owns the Character; 7: a local damage reaction plays.
    if (PairReservedCharacter(actor.character) || PairSourceBusy(actor.character)) { NoteExecutionHold(actor,6U); return true; }
    if (health::Terminal(actor.desired.health)) return true;
    if(actor.reactionCycle!=0U) {
        std::uintptr_t cycle{};std::uint8_t serial{},flags{};
        if(Read(actor.character+0x36CU,cycle)&&cycle==actor.reactionCycle&&
            Read(cycle+0x19U,serial)&&serial==actor.reactionSerial&&Read(cycle+8U,flags)&&(flags&6U)==0U){NoteExecutionHold(actor,7U);return true;}
        actor.reactionCycle=0U;
    }
    actor.replayedExecutionRevision = 0U;
    // Actual scheduler/navigation ownership, not a pose follower, selects movement.
    actor.movementOwner = MovementOwner::Navigation;
    actor.completedRevision = actor.desired.revision;
    actor.lastProxyRefresh = now;
    return true;
}
ConsequenceSource CaptureConsequenceSource(const health::Source source) noexcept {
    ConsequenceSource portable{};
    std::uint32_t runtimeId{};
    if (source.character == 0U || !Read(source.character + 0x50U, runtimeId) ||
        runtimeId != source.runtimeId) return portable;
    portable.peer = PeerForParticipant(source.character);
    if (portable.peer != 0U) return portable;
    const auto* actor = Find(source.character);
    if (actor == nullptr || actor->duplicate || actor->runtimeId != source.runtimeId) return {};
    portable.entityA = actor->entityA; portable.entityB = actor->entityB;
    portable.characterClass = actor->characterClass; portable.birth = actor->birth;
    return portable;
}
bool ResolveConsequenceSource(const ConsequenceSource& portable, health::Source& source) noexcept {
    source = {};
    if (portable.peer != 0U) source.character = ParticipantForPeer(portable.peer);
    else if (portable.birth != 0U) {
        const auto* actor = FindActorByIdentity(portable.entityA, portable.entityB);
        if (actor == nullptr || actor->remoteBirth != portable.birth || actor->duplicate ||
            actor->characterClass != portable.characterClass) return false;
        source.character = actor->character;
    } else return true; // Native Host damage can have no Character cause.
    return source.character != 0U && Read(source.character + 0x50U, source.runtimeId) &&
        source.runtimeId != 0U;
}
void CaptureAuthoritativeHealth(Actor& actor) noexcept {
    health::State observed{};
    if (!health::Capture(actor.character, observed) || health::SameValue(observed, actor.desired.health)) return;
    observed.revision = actor.desired.health.revision + 1U;
    if (observed.revision == 0U) observed.revision = 1U;
    actor.desired.health = observed;
    actor.desired.healthPairRevision = 0U;
    const auto& origin = gHostHealthOrigins[static_cast<std::size_t>(&actor - gActors.data())];
    const auto tag = origin.tag.load(std::memory_order_acquire);
    const auto sourceSlot = origin.sourceSlot.load(std::memory_order_relaxed);
    PairIdentity pairedSource{};
    if (tag != 0U && origin.victimRuntime.load(std::memory_order_relaxed) == actor.runtimeId && sourceSlot < kActors &&
        LoadPairBinding(gPairSources[sourceSlot], pairedSource) && PairIdentityLive(pairedSource) &&
        pairedSource.authoritativeBirth == static_cast<std::uint32_t>(tag >> 32U) &&
        origin.tag.load(std::memory_order_acquire) == tag) {
        actor.desired.healthSource = CaptureConsequenceSource({pairedSource.character, pairedSource.runtimeId});
        actor.desired.healthPairRevision = static_cast<std::uint32_t>(tag);
        return;
    }
    // Preserve the last observed damage cause while alive. A scripted native
    // death may instead provide its exact scheduler-owned killer reference.
    health::Source source{};
    if (health::Terminal(observed) && health::CaptureDeathSource(actor.character, source))
        actor.desired.healthSource = CaptureConsequenceSource(source);
}
bool ApplyAuthoritativeHealth(Actor& actor, const State& state) noexcept {
    if (state.healthPairRevision != 0U && state.health.revision > actor.healthRevision) {
        health::State current{};
        if (health::Capture(actor.character, current) && health::SameValue(current, state.health)) {
            actor.healthRevision = state.health.revision;
            return true;
        }
        health::Source pairedSource{};
        OutcomeBinding binding{};
        if (!ResolveConsequenceSource(state.healthSource, pairedSource) ||
            !LoadOutcome(pairedSource.character, actor.character, binding) || !binding.claimed ||
            binding.state.interactionPairRevision != state.healthPairRevision ||
            PairSourceBusy(pairedSource.character)) return false;
    }
    if ((state.stateFlags & ExecutionValid) != 0U && state.executionCycleState == 0x4FU &&
        state.executionCycleTypeRva == 0x00EAE690U && state.health.revision > actor.healthRevision) {
        std::uintptr_t cycle{};
        std::uint8_t type{}, acquired{};
        // The source's ordered outcome must admit its victim BEFORE an NPC's
        // independent health snapshot can make that victim terminal locally.
        if (!Read(actor.character + 0x36CU, cycle) || !Read(cycle + 0x1AU, type) || type != 0x4FU ||
            !Read(cycle + 0xD9U, acquired) || acquired == 0U) return false;
    }
    health::Source source{};
    if (!ResolveConsequenceSource(state.healthSource, source)) return false;
    const auto matched = [](const health::ApplyResult result) noexcept {
        return result == health::ApplyResult::Applied || result == health::ApplyResult::AlreadyMatched;
    };
    const auto& event = state.damage;
    const bool ordered = (state.stateFlags & ConsequenceEvent) != 0U;
    if (ordered && event.revision > actor.damageRevision) {
        if (event.result.revision <= actor.healthRevision) {
            // An absolute bootstrap snapshot already contains this historical
            // result. Never reopen a wound/death reaction after joining late.
            actor.damageRevision = event.revision;
        } else {
            const bool commitOnly = (event.flags & health::CommitOnly) != 0U;
            if (!commitOnly &&
                (actor.damageCursor.revision != event.revision || !actor.damageCursor.invoked)) {
                const health::State before{event.result.revision, health::Present,
                    event.hpBefore, event.healthStateBefore};
                if (!matched(health::ApplySnapshot(actor.character, actor.runtimeId, before, source))) return false;
            }
            // A native HP setter (e.g. regeneration) is not a new impact or
            // movement reaction. Preserve its live path and execution lease.
            // Real hits and terminal commits still yield to native reactions.
            const bool wasInvoked = actor.damageCursor.revision == event.revision && actor.damageCursor.invoked;
            if (!wasInvoked && (!commitOnly || health::Terminal(event.result))) {
                static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(actor.character));
            }
            std::uintptr_t previousCycle{};
            std::uint8_t previousSerial{};
            const bool previousKnown = Read(actor.character + 0x36CU, previousCycle) && previousCycle != 0U &&
                Read(previousCycle + 0x19U, previousSerial);
            const auto result = health::ApplyDamageEvent(actor.character, actor.runtimeId, event, source, actor.damageCursor);
            if (!commitOnly && !wasInvoked && actor.damageCursor.invoked) {
                std::uintptr_t cycle{};
                std::uint8_t nativeState{}, serial{};
                if (Read(actor.character + 0x36CU, cycle) && Read(cycle + 0x1AU, nativeState) &&
                    nativeState != 1U && nativeState != 0x38U && Read(cycle + 0x19U, serial) &&
                    previousKnown && (cycle != previousCycle || serial != previousSerial)) {
                    actor.reactionCycle = cycle; actor.reactionSerial = serial;
                }
            }
            if (!matched(result)) return false;
            actor.damageRevision = event.revision;
            actor.healthRevision = event.result.revision;
        }
    }
    if (state.health.revision > actor.healthRevision) {
        if (health::Terminal(state.health)) {
            static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(actor.character));
        }
        if (!matched(health::ApplySnapshot(actor.character, actor.runtimeId, state.health, source))) return false;
        actor.healthRevision = state.health.revision;
    }
    return true;
}
bool ApplyAuthoritativeParticipantDamage(Actor& actor, const State& state) noexcept {
    const auto& consequence = state.participantConsequence;
    if ((state.stateFlags & ParticipantConsequenceEvent) == 0U ||
        consequence.damage.revision <= actor.participantDamageRevision) return true;
    const auto victim = ParticipantForPeer(consequence.targetPeer);
    std::uint32_t runtimeId{}, sourceRuntimeId{};
    if (victim == 0U || !Read(victim + 0x50U, runtimeId) || runtimeId == 0U ||
        !Read(actor.character + 0x50U, sourceRuntimeId) || sourceRuntimeId != actor.runtimeId) return false;
    if (actor.participantDamagePeer != consequence.targetPeer || actor.participantRuntimeId != runtimeId) {
        actor.participantDamagePeer = consequence.targetPeer;
        actor.participantRuntimeId = runtimeId;
        actor.participantDamageCursor = {};
    }
    const auto matched = [](const health::ApplyResult result) noexcept {
        return result == health::ApplyResult::Applied || result == health::ApplyResult::AlreadyMatched;
    };
    const health::Source source{actor.character, actor.runtimeId};
    const auto& event = consequence.damage;
    if ((event.flags & health::CommitOnly) == 0U &&
        (actor.participantDamageCursor.revision != event.revision || !actor.participantDamageCursor.invoked)) {
        const health::State before{event.result.revision, health::Present, event.hpBefore, event.healthStateBefore};
        if (!matched(health::ApplySnapshot(victim, runtimeId, before, source))) return false;
    }
    if (!matched(health::ApplyDamageEvent(victim, runtimeId, event, source, actor.participantDamageCursor))) return false;
    actor.participantDamageRevision = event.revision;
    return true;
}
void PumpHostDamageObservations() noexcept {
    if (gPublishingDamage || gRole.load(std::memory_order_acquire) != Role::Host || !Stable()) return;
    gPublishingDamage = true;
    health::DamageObservation observation{};
    for (std::size_t budget = 0U; budget < 256U && health::TryPopDamageObservation(observation); ++budget) {
        std::uint32_t current{}, type{};
        if (!Read(observation.character + 0x50U, current) || current != observation.runtimeId) continue;
        const bool npcVictim = Eligible(observation.character, type);
        if (!npcVictim && (PeerForParticipant(observation.character) == 0U ||
            !Read(observation.source.character + 0x50U, current) || current != observation.source.runtimeId ||
            !Eligible(observation.source.character, type))) continue;
        auto* actor = Admit(npcVictim ? observation.character : observation.source.character);
        if (actor == nullptr || actor->duplicate || actor->faulted) continue;
        std::size_t pending{};
        for (const auto& tracked : gActors) pending += tracked.pendingDamage.size();
        if (pending >= kRing) { gDropped.fetch_add(1U, std::memory_order_relaxed); break; }
        actor->pendingDamage.push_back(observation);
    }
    for (auto& actor : gActors) {
        while (!actor.pendingDamage.empty()) {
            const auto& pending = actor.pendingDamage.front();
            const bool npcVictim = pending.character == actor.character;
            if ((npcVictim ? pending.runtimeId : pending.source.runtimeId) != actor.runtimeId) {
                actor.pendingDamage.pop_front(); continue;
            }
            if (!npcVictim) {
                auto& consequence = actor.desired.participantConsequence;
                if (consequence.damage.revision == actor.previous.participantConsequence.damage.revision) {
                    const auto peer = PeerForParticipant(pending.character);
                    if (peer == 0U) { actor.pendingDamage.pop_front(); continue; }
                    const auto revision = consequence.damage.revision + 1U;
                    consequence.targetPeer = peer;
                    consequence.damage = pending.event;
                    consequence.damage.revision = revision == 0U ? 1U : revision;
                    consequence.damage.result.revision = consequence.damage.revision;
                }
            } else if (actor.desired.damage.revision == actor.previous.damage.revision) {
                const auto revision = actor.desired.damage.revision + 1U;
                const auto healthRevision = actor.desired.health.revision + 1U;
                actor.desired.damage = pending.event;
                actor.desired.damage.revision = revision == 0U ? 1U : revision;
                actor.desired.damage.result.revision = healthRevision == 0U ? 1U : healthRevision;
                actor.desired.health = actor.desired.damage.result;
                actor.desired.healthPairRevision = 0U;
                actor.desired.healthSource = CaptureConsequenceSource(pending.source);
            }
            actor.lastSend = 0U;
            Publish(actor);
            if (npcVictim ? actor.previous.damage.revision != actor.desired.damage.revision :
                actor.previous.participantConsequence.damage.revision != actor.desired.participantConsequence.damage.revision) break;
            actor.pendingDamage.pop_front();
        }
    }
    gPublishingDamage = false;
}

void CaptureAuthoritativeCombat(Actor& actor) noexcept {
    combat::State observed{};
    if (!combat::Capture(actor.character, observed) || combat::SameValue(observed, actor.desired.combat)) return;
    observed.revision = actor.desired.combat.revision + 1U;
    if (observed.revision == 0U) observed.revision = 1U;
    actor.desired.combat = observed;
}
bool ApplyAuthoritativeCombat(Actor& actor, const State& state, const ULONGLONG now) noexcept {
    if (health::Terminal(state.health) || ProxyWithdrawn(state) || (state.executionFlags & ExecutionPaired) != 0U) {
        combat::Cancel(actor.character);
        actor.pendingRemoteShots.clear();
        return true;
    }
    const auto expires = now + kMotionLease;
    if ((state.stateFlags & ShotEvent) != 0U && state.shot.revision > actor.queuedShotRevision) {
        if (actor.pendingRemoteShots.size() < 32U)
            actor.pendingRemoteShots.push_back({state.shot, now + kTimeout});
        else gFaults.fetch_add(1U, std::memory_order_relaxed);
        actor.queuedShotRevision = state.shot.revision;
    }
    while (!actor.pendingRemoteShots.empty()) {
        const auto& pending = actor.pendingRemoteShots.front();
        const auto result = now >= pending.deadline ? combat::ApplyResult::Failed :
            combat::ApplyShot(actor.character, actor.runtimeId, pending.shot, actor.shotCursor, expires);
        if (result == combat::ApplyResult::Deferred) break;
        // A failed native admission cannot be invoked again safely. Retire
        // that presentation event while keeping later Host consequences live.
        if (result == combat::ApplyResult::Failed) gFaults.fetch_add(1U, std::memory_order_relaxed);
        actor.shotRevision = pending.shot.revision;
        actor.pendingRemoteShots.pop_front();
    }
    const auto result = combat::ApplyState(actor.character, actor.runtimeId, state.combat, expires);
    // Missing local presentation resources cannot hold later health/death or
    // movement decisions. The bounded shot queue retries its native admission.
    if (result == combat::ApplyResult::Failed) gFaults.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
void PumpHostShotObservations() noexcept {
    if (gPublishingCombat || gRole.load(std::memory_order_acquire) != Role::Host || !Stable()) return;
    gPublishingCombat = true;
    combat::Observation observation{};
    for (std::size_t budget = 0U; budget < 256U && combat::TryPopObservation(observation); ++budget) {
        std::uint32_t runtimeId{};
        auto* actor = Find(observation.character);
        if (actor == nullptr || actor->duplicate || actor->faulted ||
            !Read(actor->character + 0x50U, runtimeId) || runtimeId != observation.runtimeId ||
            runtimeId != actor->runtimeId) continue;
        std::size_t pending{};
        for (const auto& tracked : gActors) pending += tracked.pendingShots.size();
        if (pending >= kRing) { gDropped.fetch_add(1U, std::memory_order_relaxed); break; }
        actor->pendingShots.push_back(observation);
    }
    for (auto& actor : gActors) {
        while (!actor.pendingShots.empty()) {
            if (actor.desired.shot.revision == actor.previous.shot.revision) {
                const auto revision = actor.desired.shot.revision + 1U;
                const auto stateRevision = actor.desired.combat.revision + 1U;
                actor.desired.shot = actor.pendingShots.front().shot;
                actor.desired.shot.revision = revision == 0U ? 1U : revision;
                actor.desired.shot.state.revision = stateRevision == 0U ? 1U : stateRevision;
                actor.desired.combat = actor.desired.shot.state;
            }
            actor.lastSend = 0U;
            Publish(actor);
            if (actor.previous.shot.revision != actor.desired.shot.revision) break;
            actor.pendingShots.pop_front();
        }
    }
    gPublishingCombat = false;
}

void PumpHostCarriers() noexcept {
    if (gPublishingCarriers) return;
    gPublishingCarriers = true;
    // Shot admission precedes the damage resulting from its native raycast.
    PumpHostShotObservations();
    PumpHostDamageObservations();
    gPublishingCarriers = false;
}

bool SameState(const State& a, const State& b) noexcept {
    if(a.motionTick!=b.motionTick||a.motionFlags!=b.motionFlags||
        std::memcmp(a.motionVelocity,b.motionVelocity,sizeof(a.motionVelocity))!=0)return false;
    if (std::memcmp(&a.combat, &b.combat, sizeof(a.combat)) != 0 ||
        std::memcmp(&a.shot, &b.shot, sizeof(a.shot)) != 0) return false;
    if (a.healthPairRevision != b.healthPairRevision) return false;
    if (std::memcmp(&a.health, &b.health, sizeof(a.health)) != 0 ||
        std::memcmp(&a.healthSource, &b.healthSource, sizeof(a.healthSource)) != 0 ||
        std::memcmp(&a.damage, &b.damage, sizeof(a.damage)) != 0) return false;
    if (std::memcmp(&a.scripted, &b.scripted, sizeof(a.scripted)) != 0) return false;
    if (std::memcmp(&a.transition, &b.transition, sizeof(a.transition)) != 0) return false;
    if (std::memcmp(&a.participantConsequence, &b.participantConsequence, sizeof(a.participantConsequence)) != 0) return false;
    if (std::memcmp(&a.outcome, &b.outcome, sizeof(a.outcome)) != 0) return false;
    if (std::memcmp(&a.navigation, &b.navigation, sizeof(NavigationGoalState)) != 0) return false;
    if (a.stateFlags != b.stateFlags || a.moveType != b.moveType ||
        a.locomotionRevision != b.locomotionRevision ||
        std::abs(a.locomotionIntensity - b.locomotionIntensity) > 0.002F ||
        a.actionTypeRva != b.actionTypeRva || a.actionPathHash != b.actionPathHash ||
        a.actionRevision != b.actionRevision || a.actionStatus != b.actionStatus ||
        a.actionDepth != b.actionDepth ||
        a.executionRevision != b.executionRevision ||
        a.executionCycleTypeRva != b.executionCycleTypeRva ||
        a.executionCycleState != b.executionCycleState ||
        a.executionFlags != b.executionFlags ||
        a.executionAction != b.executionAction ||
        a.roleTaskRevision != b.roleTaskRevision || a.roleTaskTypeRva != b.roleTaskTypeRva ||
        a.lifecycleRevision != b.lifecycleRevision || a.lifecycleFlags != b.lifecycleFlags ||
        a.interactionRevision != b.interactionRevision ||
        a.interactionTargetA != b.interactionTargetA ||
        a.interactionTargetB != b.interactionTargetB ||
        a.interactionFlags != b.interactionFlags ||
        a.interactionTransition != b.interactionTransition ||
        a.interactionTransitionFlags != b.interactionTransitionFlags ||
        a.interactionPayloadLow != b.interactionPayloadLow ||
        a.interactionPayloadHigh != b.interactionPayloadHigh ||
        a.interactionVariant != b.interactionVariant) return false;
    if (a.interactionTargetPeer != b.interactionTargetPeer ||
        a.interactionTargetBirth != b.interactionTargetBirth ||
        a.interactionParticipantIncarnation != b.interactionParticipantIncarnation ||
        a.interactionAuthority!=b.interactionAuthority||a.interactionCommand!=b.interactionCommand||
        !std::equal(std::begin(a.interactionSession),std::end(a.interactionSession),std::begin(b.interactionSession))||
        a.interactionTargetClass != b.interactionTargetClass ||
        a.interactionTransitionVariant != b.interactionTransitionVariant ||
        a.interactionPairRevision != b.interactionPairRevision ||
        a.interactionTransitionRevision != b.interactionTransitionRevision) return false;
    for (std::size_t i = 0U; i < 3U; ++i)
        if (std::abs(a.interactionTargetPosition[i] - b.interactionTargetPosition[i]) > 0.01F)
            return false;
    // Constant steering still moves the Host. Publish fresh pose evidence at
    // the bounded 50ms send cadence so timing feedback need not chase a 250ms
    // old heartbeat. Pose changes never directly relocate ordinary movement.
    for (std::size_t i = 0U; i < 3U; ++i)
        if (std::abs(a.position[i] - b.position[i]) > 0.01F) return false;
    if (std::abs(a.yaw - b.yaw) > 0.01F) return false;
    return std::abs(a.velocityX - b.velocityX) <= 0.02F &&
        std::abs(a.velocityZ - b.velocityZ) <= 0.02F;
}

bool ValidateOutgoing(const State& state) noexcept {
    if (Valid(state)) return true;
    gInvalidOutgoing.fetch_add(1U, std::memory_order_relaxed);
    gLastInvalidOutgoingFlags.store(state.stateFlags, std::memory_order_relaxed);
    gLastInvalidOutgoingEntityA.store(state.entityA, std::memory_order_relaxed);
    gLastInvalidOutgoingEntityB.store(state.entityB, std::memory_order_relaxed);
    return false;
}
void Publish(Actor& actor) noexcept {
    if (PairSourceBusy(actor.character)) return;
    PumpHostCarriers();
    if (!gPublishingDamage && actor.pendingDamage.empty()) CaptureAuthoritativeHealth(actor);
    if (!gPublishingCombat && actor.pendingShots.empty()) CaptureAuthoritativeCombat(actor);
    CaptureOutcome(actor);
    // Execution cycles can be shorter than the 20 Hz network cadence. Observe
    // them on every certified BehaviourManager update; a cycle change clears
    // lastSend so the exact transition gets an immediate authority snapshot.
    CaptureExecutionCycle(actor);
    const auto now = GetTickCount64();
    bool newOutcome = actor.desired.outcome.revision != 0U &&
        (actor.desired.interactionPairRevision != actor.previous.interactionPairRevision ||
         actor.desired.outcome.revision != actor.previous.outcome.revision);
    const bool newDamage = actor.desired.damage.revision != 0U &&
        actor.desired.damage.revision != actor.previous.damage.revision;
    const bool newParticipantDamage = actor.desired.participantConsequence.damage.revision != 0U &&
        actor.desired.participantConsequence.damage.revision != actor.previous.participantConsequence.damage.revision;
    const bool newShot = actor.desired.shot.revision != 0U && actor.desired.shot.revision != actor.previous.shot.revision;
    if (!newOutcome && !newDamage && !newParticipantDamage && !newShot && now - actor.lastSend < kSendPeriod) return;
    CaptureInteractionCycle(actor);
    newOutcome = actor.desired.outcome.revision != 0U &&
        (actor.desired.interactionPairRevision != actor.previous.interactionPairRevision ||
         actor.desired.outcome.revision != actor.previous.outcome.revision);
    if ((actor.desired.stateFlags & InteractionValid) != 0U &&
        (actor.desired.interactionFlags & InteractionCycleActive) == 0U &&
        actor.desired.outcome.revision == 0U &&
        actor.lastInteractionCapture != 0U && now - actor.lastInteractionCapture > kTimeout) {
        actor.desired.stateFlags &= ~InteractionValid;
        actor.desired.interactionFlags = 0U;
        actor.desired.interactionParticipantIncarnation=0U;
        std::fill(std::begin(actor.desired.interactionSession),std::end(actor.desired.interactionSession),std::uint8_t{});
        actor.desired.interactionAuthority=actor.desired.interactionCommand=0U;
        actor.desired.interactionTransition = 0U;
        actor.desired.interactionTransitionFlags = 0U;
    }
    CaptureWithdrawState(actor);
    CaptureCharacterBackstage(actor);
    CapturePose(actor, now);
    CaptureActiveAction(actor);
    CaptureRoleTask(actor);
    SamplePresentation(actor, now, newOutcome ? 3U : 0U);
    const bool changed = !SameState(actor.desired, actor.previous);
    if (!changed && now - actor.lastSend < kRefreshPeriod) return;
    auto state = actor.desired;
    if (changed) ++state.revision;
    if (newDamage) state.stateFlags |= ConsequenceEvent;
    if (newParticipantDamage) state.stateFlags |= ParticipantConsequenceEvent;
    if (newShot) state.stateFlags |= ShotEvent;
    if (newOutcome && (actor.previous.interactionPairRevision != state.interactionPairRevision ||
        actor.previous.outcome.revision == 0U)) {
        OutcomeBinding binding{};
        if (!LoadOutcome(actor.character, PairTargetForState(state), binding)) return;
        // This event presents the prepared pair first, before a possibly newer
        // hidden/vent pose. Ordinary convergence follows on the next snapshot.
        for (std::size_t i = 0U; i < 3U; ++i) state.position[i] = binding.state.position[i];
        state.yaw = binding.state.yaw;
        state.lifecycleFlags = binding.state.lifecycleFlags &
            ~(LifecycleWithdrawn | LifecycleWithdrawState2 | LifecycleHostDormant | LifecycleCharacterBackstage);
        state.stateFlags |= PoseValid;
        if((state.motionFlags&1U)!=0U)state.motionFlags|=2U; // Prepared native pair pose.
    }
    // Native reads can fail during lifecycle changes. Validate the complete
    // carrier before publishing/acknowledging it, just as the Launcher does.
    if (!ValidateOutgoing(state)) return;
    // Events and ordinary evidence use the same native producer outbox as
    // Submit/Update and native lifecycle calls. No second gameplay sender.
    if(host_producer::EngineProducer().AppendState(state)!=host_producer::SourceResult::Ok)return;
    if(state.executionRevision!=actor.publishedExecutionRevision||
        std::memcmp(&state.transition,&actor.previous.transition,sizeof(state.transition))!=0){
        const bool sameRevision=state.executionRevision==actor.publishedExecutionRevision;
        actor.publishedExecutionRevision=state.executionRevision;
        RecordActorExecutionTrace(actor,{1U,state.revision,state.executionRevision,state.executionCycleTypeRva,state.executionCycleState,
            sameRevision?1U:0U,0U,0U,0U,actor.execCaptureTick,GetTickCount64(),0U,0U,UtcMs()});
    }
    transactions::Scope spawnScope{};
    if((state.stateFlags&PoseValid)!=0U&&!ProxyWithdrawn(state)&&host_producer::HostScope(spawnScope)&&
        (actor.spawnDiagnosticScope!=spawnScope||actor.spawnDiagnosticBirth!=state.birth)) {
        actor.spawnDiagnosticScope=spawnScope;actor.spawnDiagnosticBirth=state.birth;
        RecordSpawnPlacement(actor,state,true);
    }
    actor.desired.revision = state.revision;
    actor.previous = state; actor.lastSend = now;
    actor.previous.stateFlags &= ~kOrderedFlags;
    gObserved.fetch_add(1U, std::memory_order_relaxed);
}

// BehaviourManager is not guaranteed to tick while an Alien is withdrawn,
// backstage or traversing a vent. AISync authority must therefore not depend on
// that manager callback for transport liveness. The CharacterManager pump is
// certified every frame and keeps the last admitted Host actor authoritative
// while its AI manager is dormant.
void PublishDormantHostActor(Actor& actor, const ULONGLONG now,
    const bool characterPresent) noexcept {
    if (PairSourceBusy(actor.character) || now - actor.lastSend < kRefreshPeriod) return;
    if (characterPresent) CaptureAuthoritativeHealth(actor);
    CaptureOutcome(actor);
    // A Character can leave its paired layout while its brain goes dormant
    // (backstage/vent). Retention must stay fresh on the independent pump too.
    if (characterPresent) CaptureInteractionCycle(actor);
    if (actor.desired.outcome.revision != 0U &&
        (actor.desired.interactionPairRevision != actor.previous.interactionPairRevision ||
         actor.desired.outcome.revision != actor.previous.outcome.revision)) {
        actor.lastSend = 0U;
        Publish(actor);
        return;
    }

    if (characterPresent) {
        CaptureWithdrawState(actor);
        CaptureCharacterBackstage(actor);
        CaptureExecutionCycle(actor);
    }
    if (!actor.hostDormant) {
        actor.hostDormant = true;
        MarkLifecycle(actor, LifecycleSpatialDiscontinuity |
            (actor.traversalAction ? LifecycleTraversalAction : 0U));
    } else {
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
    }

    // If the same Character remains registered (usual backstage path), keep
    // its actual authoritative transform fresh even though BehaviourManager is
    // sleeping. If retail temporarily removes it from the active registry,
    // retain the last safe pose solely as an authority heartbeat.
    if (characterPresent) {
        CapturePose(actor, now);
    }
    actor.desired.stateFlags &= ~Moving;
    if (actor.desired.navigation.flags != 0U) {
        const auto revision = actor.desired.navigation.revision;
        const auto requestRevision = actor.desired.navigation.requestRevision;
        actor.desired.navigation = {};
        actor.desired.navigation.requestRevision = requestRevision;
        actor.desired.navigation.revision = revision + 1U;
        if (actor.desired.navigation.revision == 0U) actor.desired.navigation.revision = 1U;
        actor.capturedNavigationRequest = 0U;
    }
    actor.desired.velocityX = 0.0F;
    actor.desired.velocityZ = 0.0F;
    SamplePresentation(actor, now);

    const auto changed = !SameState(actor.desired, actor.previous);
    auto state = actor.desired;
    if (changed) ++state.revision;
    if (!ValidateOutgoing(state)) return;
    if(host_producer::EngineProducer().AppendState(state)!=host_producer::SourceResult::Ok)return;
    if(state.executionRevision!=actor.publishedExecutionRevision){
        actor.publishedExecutionRevision=state.executionRevision;
        RecordExecutionTrace({1U,state.revision,state.executionRevision,state.executionCycleTypeRva,state.executionCycleState,
            0U,0U,0U,0U,actor.execCaptureTick,GetTickCount64(),0U,0U,UtcMs()});
    }
    transactions::Scope spawnScope{};
    if((state.stateFlags&PoseValid)!=0U&&!ProxyWithdrawn(state)&&host_producer::HostScope(spawnScope)&&
        (actor.spawnDiagnosticScope!=spawnScope||actor.spawnDiagnosticBirth!=state.birth)) {
        actor.spawnDiagnosticScope=spawnScope;actor.spawnDiagnosticBirth=state.birth;
        RecordSpawnPlacement(actor,state,true);
    }
    actor.desired.revision = state.revision;
    actor.previous = state;
    actor.lastSend = now;
    gObserved.fetch_add(1U, std::memory_order_relaxed);
}

void PumpHostAuthority() noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host || !Stable()) return;
    const auto now = GetTickCount64();
    CacheLivingPlayerAlliance();

    for (auto& actor : gActors) {
        if (actor.character == 0U || actor.duplicate || actor.faulted) continue;
        if(gPvPLease.owned && PvPCharacterFenced(actor.character)) {
            Publish(actor);continue;
        }
        if (actor.lastManagerUpdate != 0U &&
            now - actor.lastManagerUpdate <= kManagerDormancyPeriod) continue;
        // Dormant publication already uses this cadence. Avoid a complete
        // Character-registry scan on frames where no heartbeat can be emitted.
        if (now - actor.lastSend < kRefreshPeriod) continue;

        // Resolve through stable retail identity rather than trusting a stale
        // Character pointer. This also detects the rare case where a backstage
        // transition replaces the Character object while retaining the same
        // logical NPC identity.
        const auto current = FindCharacterByIdentity(actor.entityA, actor.entityB);
        if (current == actor.character) {
            PublishDormantHostActor(actor, now, true);
            continue;
        }
        if (current == 0U) {
            PublishDormantHostActor(actor, now, false);
            continue;
        }

        // Same logical NPC, new Character object. Re-admit it with a new birth
        // so Clients perform one lifecycle barrier/snap instead of remaining
        // bound to the stale pre-vent instance.
        const auto previousActor = actor;
        actor = {};
        auto* rebound = Admit(current);
        if (rebound == nullptr) {
            actor = previousActor;
            PublishDormantHostActor(actor, now, false);
            continue;
        }
        rebound->lastManagerUpdate = now;
        MarkLifecycle(*rebound, LifecycleSpatialDiscontinuity);
        Publish(*rebound);
    }
}

// Interior hook at 0073433E. The original preamble 007342A0..0073433D still
// runs, including +4234/+4238 updates and Character housekeeping. EDI is on
// the retail stack at this point. Both paths restore all registers/flags.
// Client skips ONLY 0073433E..007344D3 (senses/targeting/BT/roles/AI adapters),
// then takes the retail POP ESI / RET 8 epilogue at 007344D4.
int __cdecl BypassBranch(const std::uintptr_t manager) noexcept {
    if (gUpdating != nullptr && gUpdating->blocked) return 1;
    std::uintptr_t owner{};
    std::uint32_t type{};
    // ESI is the retail manager on every native thread. This decision reads
    // only generation-bound ownership atomics and native owner/class fields.
    return Read(manager + 0x128U, owner) &&
        (PairSourceBusy(owner) || PlayerRepresentation(owner) || SuppressedAlien(owner) ||
            (Client() && Eligible(owner, type))) ? 1 : 0;
}
__declspec(naked) void BranchDetour() {
    __asm {
        pushfd
        pushad
        push esi
        call BypassBranch
        add esp, 4
        test eax, eax
        jnz bypass
        popad
        popfd
        jmp dword ptr [gBranchTrampoline]
    bypass:
        popad
        popfd
        pop edi
        jmp dword ptr [gBranchExit]
    }
}
bool RecordMatches(const std::uintptr_t record, const std::uintptr_t owner,
    const std::uintptr_t target) noexcept {
    std::uintptr_t recordOwner{}, recordTarget{};
    std::uint32_t birth{}, currentBirth{};
    // FUN_00783B00 returns Character+50; retain its lifetime check when reading.
    return Read(record + 8U, recordOwner) && recordOwner == owner &&
        Read(record + 0x42CU, recordTarget) && recordTarget == target &&
        Read(record + 0x430U, birth) && Read(target + 0x50U, currentBirth) && birth == currentBirth;
}
bool ReadCharacterExecution(const std::uintptr_t target, ParticipantProbe& sample) noexcept {
    __try {
        std::uintptr_t table{}, getter{};
        if (!Read(target, table) || !Read(table + 0x1CU, getter) ||
            getter < gBase + 0x1000U || getter >= gBase + 0x00D9E13BU) return false;
        // FUN_0078B380 reads the animated Character returned by vfunc+1C,
        // not necessarily the logical Character at the participant address.
        const auto animated = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(
            reinterpret_cast<void*>(target));
        return animated != 0U && Read(animated + 0x2CF4U, sample.executionEnabled) &&
            Read(animated + 0x2CD0U + 0x20U, sample.executionStateA) &&
            Read(animated + 0x2CD0U + 0xE4U, sample.executionStateB) &&
            Read(animated + 0x2F90U, sample.executionPhase);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ProbeParticipantSignalRegistry(const std::uintptr_t target, ParticipantProbe& sample) noexcept {
    std::uintptr_t resources{};
    std::uint32_t entityA{}, entityB{}, type{};
    if (!Read(gBase + 0x0134EF40U, resources) || resources == 0U ||
        !CharacterIdentity(target, entityA, entityB, type)) return false;
    const std::array<std::uint32_t, 2U> identity{entityA, entityB};
    __try {
        const auto retail = reinterpret_cast<SenseCharacterResolver>(gBase + 0x002545C0U)(
            reinterpret_cast<void*>(resources), identity.data());
        sample.signalRegistryFound = retail != 0U;
        sample.signalRegistryMatches = retail == target;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void SampleParticipantPerception(Actor& actor, const std::uintptr_t manager, const ULONGLONG now) noexcept {
    if (gCurrent.participantCount == 0U || now - actor.lastParticipantProbe < kParticipantProbePeriod) return;
    actor.lastParticipantProbe = now;
    // Rotate peers, one sample per actor per second. All loops and the queue
    // are bounded; diagnostics never allocate, log, or wait on this thread.
    const auto index = actor.participantProbeCursor++ % gCurrent.participantCount;
    const auto peer = gCurrent.participants[index].peer;
    const auto target = ParticipantForPeer(peer);
    if (target == 0U) return;
    ParticipantProbe sample{};
    sample.sessionCookie = gCurrent.sessionCookie; sample.peer = peer; sample.tick = now;
    sample.nativeGeneration = gCurrent.nativeGeneration; sample.worldGeneration = gCurrent.worldGeneration;
    sample.entityA = actor.entityA; sample.entityB = actor.entityB; sample.characterClass = actor.characterClass;
    sample.living = LivingCharacter(target);
    std::uintptr_t control{}, navigation{}, backstageAdapter{}, sweep{}, sweepOwner{};
    sample.playerComponentsReadable = Read(target + 0x31CU, control) &&
        Read(target + 0x3500U, navigation) &&
        (control == 0U || Read(control + 0x48U, sample.playerControlLocal));
    sample.playerControlPresent = control != 0U;
    sample.playerNavigationPresent = navigation != 0U;
    sample.backstageReadable = Read(actor.character + 0x34F0U, backstageAdapter) && backstageAdapter != 0U &&
        Read(backstageAdapter + 0x1CU, sweep) && sweep != 0U && Read(sweep, sweepOwner) &&
        sweepOwner == actor.character && Read(sweep + 0x28U, sample.backstageState) &&
        Read(sweep + 0x2CU, sample.backstageKeyA) && Read(sweep + 0x30U, sample.backstageKeyB) &&
        Read(sweep + 0x51U, sample.backstageEnabled) && Read(sweep + 0x53U, sample.backstagePolicy);
    if (sample.backstageReadable) {
        sample.backstageControllerReadable = Read(backstageAdapter + 0x10U, sample.backstageControllerState) &&
            Read(backstageAdapter + 0xCU, sample.backstageRequestC) &&
            Read(backstageAdapter + 0xDU, sample.backstageRequestD) &&
            Read(backstageAdapter + 0xEU, sample.backstageRequestE);
        float position[3]{}, ventX{}, ventZ{};
        if (sample.backstageKeyA != 0U && sample.backstageKeyB != 0U &&
            ReadCharacterPosition(target, position) && Read(sweep + 0x34U, ventX) &&
            Read(sweep + 0x3CU, ventZ) && FiniteCoordinate(ventX) && FiniteCoordinate(ventZ)) {
            const auto dx = position[0] - ventX, dz = position[2] - ventZ;
            sample.backstageVentDistance = std::sqrt(dx * dx + dz * dz);
            sample.backstageDistanceReadable = std::isfinite(sample.backstageVentDistance);
        }
    }
    for (const auto& cached : gParticipantNavigation) {
        if (!cached.initialized || cached.faulted || cached.peer != peer || cached.character != target ||
            cached.nativeGeneration != gCurrent.nativeGeneration || cached.worldGeneration != gCurrent.worldGeneration ||
            cached.sessionCookie != gCurrent.sessionCookie) continue;
        sample.navigationCached = true;
        const auto address = reinterpret_cast<std::uintptr_t>(cached.native.data());
        std::uint8_t goalEnabled{}, dirty{};
        std::uintptr_t query{};
        sample.navigationGoalKnown = Read(address + 0x455U, goalEnabled) && goalEnabled != 0U;
        sample.navigationGoalDirty = Read(address + 0x454U, dirty) && dirty != 0U;
        sample.navigationQueryPending = Read(address, query) && query != 0U;
        sample.navigationEmptyRouteRetries = cached.emptyRouteRetries;
        static_cast<void>(Read(address + 0x450U, sample.navigationRouteCount));
        break;
    }
    sample.signalRegistryReadable = ProbeParticipantSignalRegistry(target, sample);
    sample.visibilityFlagsReadable = Read(actor.character + 0x2DAU, sample.ownerHidden) &&
        Read(target + 0x2DAU, sample.targetHidden);
    sample.pairReadable = ParticipantPairBusy(target, sample.paired);
    sample.executionReadable = ReadCharacterExecution(target, sample);
    if (sample.executionReadable) {
        sample.retailEligibility = sample.executionEnabled == 0U ||
            ((sample.executionStateA == 9U || sample.executionStateB == 9U) && sample.executionPhase == 3U)
            ? 1U : 0U;
    }
    // The manager counter is entry-only; its retail execution guard can
    // still stop the decision slice. Report that separately from target gates.
    ParticipantProbe ownerExecution{};
    sample.ownerExecutionReadable = ReadCharacterExecution(actor.character, ownerExecution);
    if (sample.ownerExecutionReadable) {
        sample.ownerRetailEligibility = ownerExecution.executionEnabled == 0U ||
            ((ownerExecution.executionStateA == 9U || ownerExecution.executionStateB == 9U) &&
                ownerExecution.executionPhase == 3U) ? 1U : 0U;
    }
    std::uintptr_t characterTemplate{};
    sample.ownerGatesReadable = Read(actor.character + 0x2FAU, sample.ownerEnabled) &&
        Read(actor.character + 0x64U, characterTemplate) && characterTemplate != 0U &&
        Read(characterTemplate + 0x1CU, sample.ownerSenseBlocked);
    std::uintptr_t senses{}, sensesOwner{}, node{};
    if (Read(manager + 0xF4U, senses) && senses != 0U &&
        Read(senses + 0x8CU, sensesOwner) && sensesOwner == actor.character && Read(senses + 0xCU, node)) {
        sample.sensesReadable = Read(senses + 0xC5U, sample.sensesDisabled);
        for (std::size_t i = 0U; node != 0U && i < 1024U; ++i) {
            std::uintptr_t record{}, next{};
            if (!Read(node, record) || !Read(node + 8U, next)) { sample.sensesReadable = false; break; }
            if (RecordMatches(record, actor.character, target)) {
                sample.recordPresent = true;
                sample.recordReadable = Read(record + 0x490U, sample.recordActive) &&
                    Read(record + 0x491U, sample.recordSelected) && Read(record + 0x493U, sample.recordForced) &&
                    Read(record + 0x474U, sample.sightInput) && std::isfinite(sample.sightInput) &&
                    Read(record + 0x478U, sample.sightTimestamp) && std::isfinite(sample.sightTimestamp) &&
                    Read(record + 0x434U, sample.primarySenseIndex) && Read(record + 0x438U, sample.recentSenseIndex);
                for (std::size_t sense = 0U; sense < 8U; ++sense) {
                    std::uint8_t active{};
                    if (!Read(record + 0x88U + sense * 0x74U, active)) { sample.recordReadable = false; break; }
                    if (active != 0U) sample.activeSenseMask |= 1U << sense;
                }
                node = 0U;
                break;
            }
            node = next;
        }
        if (node != 0U) sample.sensesReadable = false;
    }
    // Read the ordinary selected-target record (FUN_0040A3D0), not a BT task
    // key or THE_PLAYER projection. Observation has no effect on arbitration.
    std::uintptr_t selection{}, selectedRecord{}, selectedTarget{};
    if (Read(actor.character + 0x34A8U, selection) && selection != 0U && Read(selection + 0x10U, selectedRecord)) {
        sample.selectedReadable = selectedRecord == 0U;
        if (selectedRecord != 0U && Read(selectedRecord + 0x42CU, selectedTarget) &&
            selectedTarget != 0U && RecordMatches(selectedRecord, actor.character, selectedTarget)) {
            sample.selectedReadable = true;
            sample.selectedPresent = true;
            sample.selectedPeer = PeerForParticipant(selectedTarget);
            sample.selectedTerminalDead = TerminalDeadCharacter(selectedTarget);
        }
    }
    // Pending remains the retail choice. Record it separately so the next
    // trace distinguishes fallback from an absent eligible replacement.
    std::uintptr_t pendingRecord{}, pendingTarget{}, haveTarget{}, haveNextTarget{};
    if (selection != 0U && Read(selection + 0x14U, pendingRecord)) {
        sample.pendingReadable = pendingRecord == 0U;
        if (pendingRecord != 0U && Read(pendingRecord + 0x42CU, pendingTarget) &&
            pendingTarget != 0U && RecordMatches(pendingRecord, actor.character, pendingTarget)) {
            sample.pendingReadable = true;
            sample.pendingPresent = true;
            sample.pendingPeer = PeerForParticipant(pendingTarget);
            sample.pendingLiving = LivingCharacter(pendingTarget);
        }
    }
    sample.targetSignalsReadable = Read(actor.character + 0x3154U, haveTarget) &&
        Read(actor.character + 0x31E8U, haveNextTarget) &&
        (haveTarget == 0U || Read(haveTarget + 0x10U, sample.haveTargetSignal)) &&
        (haveNextTarget == 0U || Read(haveNextTarget + 0x10U, sample.haveNextTargetSignal));
    // Generic threat-manager entries: +18/count+1C, stride 6C, Character+4,
    // native state+58 (FUN_0071AEC0 / FUN_0071AFE0). No state is rewritten.
    std::uintptr_t threats{}, entries{};
    std::uint32_t count{};
    if (Read(actor.character + 0x34E0U, threats) && threats != 0U &&
        Read(threats + 0x18U, entries) && Read(threats + 0x1CU, count) &&
        count <= 256U && (count == 0U || entries != 0U)) {
        sample.threatReadable = true;
        for (std::uint32_t i = 0U; i < count; ++i) {
            std::uintptr_t entryTarget{};
            const auto entry = entries + i * 0x6CU;
            if (!Read(entry + 4U, entryTarget)) { sample.threatReadable = false; break; }
            if (entryTarget != target) continue;
            sample.threatPresent = true;
            sample.threatReadable = Read(entry + 0x58U, sample.threatState);
            break;
        }
    }
    if (sample.living && sample.recordPresent && sample.recordReadable && sample.recordActive != 0U &&
        (sample.sightInput > 0.0F || sample.activeSenseMask != 0U)) {
        actor.lastLivingParticipantSense = now;
        actor.lastLivingParticipantSensePeer = peer;
    }
    const auto write = gParticipantProbeWrite.load(std::memory_order_relaxed);
    if (write - gParticipantProbeRead.load(std::memory_order_acquire) >= kParticipantProbeRing) {
        gParticipantPerceptionDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    gParticipantProbes[write % kParticipantProbeRing] = sample;
    gParticipantProbeWrite.store(write + 1U, std::memory_order_release);
    gParticipantPerceptionSamples.fetch_add(1U, std::memory_order_relaxed);
}
// 461 crashed the Host once inside the engine-thread tree execution of the
// armed Alien (null call in 00712D70). Contain a fault, disarm and let the
// action expire instead of terminating the game.
bool ArmedManagerUpdate(ManagerUpdate original, void* manager, std::uint32_t previousTime, float delta) noexcept {
    __try {original(manager, previousTime, delta); return true;}
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DisarmPvPBrain();
        gPvPArmedBrainFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}
void __fastcall UpdateDetour(void* manager, void*, std::uint32_t previousTime, float delta) noexcept {
    const auto original = reinterpret_cast<ManagerUpdate>(gUpdateOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (!EngineThread()) { original(manager, previousTime, delta); return; }
    const profiling::Timer timing(profiling::Section::BehaviourHook);
    const auto callOriginal=[&]() noexcept {
        const profiling::Timer retailTiming(profiling::Section::BehaviourOriginal);
        original(manager,previousTime,delta);
    };

    std::uintptr_t c{};
    if (!Read(reinterpret_cast<std::uintptr_t>(manager) + 0x128U, c) || c == 0U) {
        if (OwnsAI()) gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed);
        callOriginal(); return;
    }

    // PlayerSync Characters are retail NPC-backed presentation proxies. They
    // must never run an autonomous retail brain on ANY peer: PlayerSync owns
    // their locomotion/equipment/actions, while the local BT would otherwise
    // choose targets/scripts independently. Keep Character housekeeping but
    // bypass the same decision slice used by AISync clients. This remains
    // active even when AISync traffic is isolated/off.
    if (PlayerRepresentation(c)) {
        Actor playerProxy{};
        playerProxy.character = c;
        playerProxy.blocked = true;
        auto* previous = gUpdating;
        gUpdating = &playerProxy;
        gSuspended.fetch_add(1U, std::memory_order_relaxed);
        callOriginal();
        gUpdating = previous;
        return;
    }

    // A single selected retail Alien must never run its autonomous decision
    // branch while the PvP native lease is fencing or owned. Preserve the
    // manager's retail housekeeping exactly as we do for PlayerSync proxies.
    if (PvPCharacterFenced(c) && PvPBrainArmed(c)) {
        static_cast<void>(ArmedManagerUpdate(original, manager, previousTime, delta));
        return;
    }
    if (PvPCharacterFenced(c)) {
        Actor pvpProxy{};
        pvpProxy.character = c;
        pvpProxy.blocked = true;
        auto* previous = gUpdating;
        gUpdating = &pvpProxy;
        gSuspended.fetch_add(1U, std::memory_order_relaxed);
        callOriginal();
        gUpdating = previous;
        return;
    }

    if (!OwnsAI()) { callOriginal(); return; }
    if (!Client() && !Stable()) { callOriginal(); return; }
    gManagerUpdates.fetch_add(1U, std::memory_order_relaxed);

    Actor* actor = Admit(c);
    // A capacity/key refusal must NOT silently re-enable an eligible Client brain.
    Actor refused{};
    std::uint32_t type{};
    if (actor == nullptr && Client() && Eligible(c, type)) {
        refused.character = c; refused.blocked = true; actor = &refused;
    }
    auto* previous = gUpdating;
    const auto previousQueryOwner = gAiPlayerQueryOwner;
    bool traceTargetSuccession{};
    gUpdating = actor;
    if (actor != nullptr) {
        const auto updateTick=GetTickCount64();
        if(actor->diagnosticUpdateTick&&updateTick>=actor->diagnosticUpdateTick)
            profiling::Value(profiling::Section::ActorInterval,(updateTick-actor->diagnosticUpdateTick)*1000U);
        actor->diagnosticUpdateTick=updateTick;
        if(std::isfinite(delta)&&delta>=0.0F&&delta<10.0F)
            profiling::Value(profiling::Section::BehaviourDelta,static_cast<std::uint64_t>(delta*1000000.0F));
        actor->blocked = Client() || SuppressedAlien(c);
        if (!Client() && actor->blocked) ApplyAlienSuppression(*actor);
        if (gRole.load(std::memory_order_acquire) == Role::Host) {
            transactions::Scope sourceScope{};
            if (HostNativeCaptureGuard() && host_producer::HostScope(sourceScope) &&
                (!actor->nativeSourcePrimed || actor->nativeSourceScope != sourceScope)) {
                actor->nativeSourcePrimed = host_producer::BootstrapNativeActor(actor->character);
                if (actor->nativeSourcePrimed) actor->nativeSourceScope = sourceScope;
            }
            gAiPlayerQueryOwner = actor->character;
            // Preserve the retail player's pre-death alliance before a kill can
            // move THE_PLAYER to the dead alliance used by the solo game.
            CacheLivingPlayerAlliance();
            traceTargetSuccession = PostKillTargetTraceActive(*actor);
            if (traceTargetSuccession)
                TraceTargetSelection(*actor, TargetSuccessionPhase::UpdateEntry, GetTickCount64(), false);
            else
                actor->targetSelectionObserved = false;
        }
        if (actor->blocked) gSuspended.fetch_add(1U, std::memory_order_relaxed);
        else {
            RecoverDeadParticipantTarget(*actor, GetTickCount64());
            AssignSurvivorTargetWhenIdle(*actor, GetTickCount64());
            if (gRole.load(std::memory_order_acquire) == Role::Host &&
                (traceTargetSuccession || actor->targetSelectionObserved || PostKillTargetTraceActive(*actor)))
                TraceTargetSelection(*actor, TargetSuccessionPhase::PostRecovery, GetTickCount64(), false);
        }
    }
    {
        AiPlayerSlotScope playerContext(actor != nullptr && !actor->blocked ? actor->character : 0U);
        callOriginal();
    }
    if (actor != nullptr && actor != &refused && actor->character == c &&
        gRole.load(std::memory_order_acquire) == Role::Host &&
        (traceTargetSuccession || actor->targetSelectionObserved || PostKillTargetTraceActive(*actor)))
        TraceTargetSelection(*actor, TargetSuccessionPhase::PostRetailUpdate, GetTickCount64(), false);
    gAiPlayerQueryOwner = previousQueryOwner;
    gUpdating = previous;
    if (actor == nullptr || actor == &refused || actor->character != c) return;
    if (actor->duplicate) { static_cast<void>(Release(*actor)); return; }
    if (gRole.load(std::memory_order_acquire) == Role::Host) {
        const auto now = GetTickCount64();
        if (actor->hostDormant) {
            actor->hostDormant = false;
            CaptureWithdrawState(*actor);
            MarkLifecycle(*actor, LifecycleSpatialDiscontinuity |
                (actor->traversalAction ? LifecycleTraversalAction : 0U));
            // Do not let the dormant heartbeat defer the first live snapshot.
            actor->lastSend = 0U;
        }
        actor->lastManagerUpdate = now;
        if (!PvPCharacterFenced(c)) {
            SampleParticipantPerception(*actor, reinterpret_cast<std::uintptr_t>(manager), now);
            Publish(*actor);
        }
    } else if (Client() && Stable() && !actor->faulted && !actor->duplicate) {
        const auto now=GetTickCount64();
        SamplePresentation(*actor,now);
        if (!PvPCharacterFenced(c))
            static_cast<void>(ApplyCommandAction(*actor,now)); // records its own holds
    }
}
void __fastcall PrepassDetour(void* context, void*) noexcept {
    std::uintptr_t manager{}, owner{};
    std::uint32_t type{};
    // FUN_00733850 assigns (Character+34AC)->+4 to the embedded AI manager.
    // FUN_00712020 invokes task arbitration BEFORE the normal manager frame.
    // Player representations are presentation-only on Host and Clients, so
    // their task arbitration is always suppressed.
    if (Read(reinterpret_cast<std::uintptr_t>(context) + 4U, manager) &&
        Read(manager + 0x128U, owner)) {
        if (PairSourceBusy(owner)) return;
        if (PlayerRepresentation(owner)) return;
        if (EngineThread() && PvPCharacterFenced(owner) && !PvPBrainArmed(owner)) return;
        if (Client() && Eligible(owner, type)) return;
    }
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(gPrepassOriginal.load());
    if (original == nullptr) return;
    const auto previousQueryOwner = gAiPlayerQueryOwner;
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        owner != 0U && Eligible(owner, type)) gAiPlayerQueryOwner = owner;
    {
        AiPlayerSlotScope playerContext(gAiPlayerQueryOwner == owner ? owner : 0U);
        original(context);
    }
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable()) {
        if (auto* actor = Find(owner); actor != nullptr &&
            (actor->targetSelectionObserved || PostKillTargetTraceActive(*actor)))
            TraceTargetSelection(*actor, TargetSuccessionPhase::PostPrepass, GetTickCount64(), false);
    }
    gAiPlayerQueryOwner = previousQueryOwner;
}
void __fastcall CoordinatorDetour(void* tree, void*, std::uint32_t execute) noexcept {
    const auto original = reinterpret_cast<Coordinator>(gCoordinatorOriginal.load(std::memory_order_acquire));
    // CATHODE forced-target/relance also call this outside the ordinary AI frame.
    std::uintptr_t owner{};
    std::uint32_t type{};
    if (Read(reinterpret_cast<std::uintptr_t>(tree) + 0xCU, owner)) {
        if (PairSourceBusy(owner)) return;
        if (PlayerRepresentation(owner)) return;
        if (EngineThread() && PvPCharacterFenced(owner) && !PvPBrainArmed(owner)) return;
        if (Client() && Eligible(owner, type)) return;
    }
    if (original == nullptr) return;
    const auto previousQueryOwner = gAiPlayerQueryOwner;
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        owner != 0U && Eligible(owner, type)) gAiPlayerQueryOwner = owner;
    {
        AiPlayerSlotScope playerContext(gAiPlayerQueryOwner == owner ? owner : 0U);
        original(tree, execute);
    }
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable()) {
        if (auto* actor = Find(owner); actor != nullptr &&
            (actor->targetSelectionObserved || PostKillTargetTraceActive(*actor)))
            TraceTargetSelection(*actor, TargetSuccessionPhase::PostCoordinator, GetTickCount64(), false);
    }
    gAiPlayerQueryOwner = previousQueryOwner;
}

bool ClientAIImpactVictim(void* receiver, std::uintptr_t& victim) noexcept {
    victim = 0U;
    if (!Client() || receiver == nullptr) return false;
    std::uint32_t type{};
    // Both certified Character impact receivers keep their owning Character at
    // +0x24. On a Client, AISync-owned NPC consequences are Host-authoritative:
    // local/remote weapon presentation may run, but it must not independently
    // damage or retarget the replicated AI.
    return Read(reinterpret_cast<std::uintptr_t>(receiver) + 0x24U, victim) &&
        victim != 0U && Eligible(victim, type);
}

// Remote player Replicas are not inserted in the retail Character registry
// (0x134EF40). On the Host, a Replica's shot therefore resolves its shooter to
// null, and the zero-damage stimulus branch of 007CDCF0 dereferences it
// (crash at 007CDE34). Map the missing registration to the published
// participant with the same identity. Published bindings only: impact
// dispatch may run on a worker.
std::uintptr_t HostReplicaShooter(const std::uint32_t* identity) noexcept {
    if (identity == nullptr || gRole.load(std::memory_order_acquire) != Role::Host ||
        !PairGenerationStable()) return 0U;
    const auto address = reinterpret_cast<std::uintptr_t>(identity);
    std::uint32_t entityA{}, entityB{};
    if (!Read(address, entityA) || !Read(address + 4U, entityB) ||
        (entityA == 0U && entityB == 0U)) return 0U;
    std::uintptr_t mapped{};
    for (std::size_t i = 1U; i < gPairParticipants.size(); ++i) {
        PairIdentity candidate{};
        if (!LoadPairBinding(gPairParticipants[i], candidate) || candidate.peer == 0U ||
            candidate.entityA != entityA || candidate.entityB != entityB ||
            !RegisteredPlayerRepresentation(candidate.character) || !PairIdentityLive(candidate)) continue;
        if (mapped != 0U && mapped != candidate.character) {
            gParticipantSignalAmbiguous.fetch_add(1U, std::memory_order_relaxed);
            return 0U;
        }
        mapped = candidate.character;
    }
    return mapped;
}
std::uintptr_t ResolveImpactShooter(void* resources, const std::uint32_t* identity) noexcept {
    std::uintptr_t retail{};
    __try {
        retail = reinterpret_cast<SenseCharacterResolver>(gBase + kCharacterResolverRva)(resources, identity);
    } __except (EXCEPTION_EXECUTE_HANDLER) { retail = 0U; }
    if (retail != 0U) return retail;
    const auto mapped = HostReplicaShooter(identity);
    if (mapped != 0U) gImpactShooterRemaps.fetch_add(1U, std::memory_order_relaxed);
    return mapped;
}
std::uintptr_t __fastcall ImpactShooterResolverDetour(void* resources, void*,
    const std::uint32_t* identity) noexcept {
    return ResolveImpactShooter(resources, identity);
}
std::uintptr_t ResolveImpactSource(int* hit) noexcept {
    if (hit == nullptr || gBase == 0U) return 0U;
    std::uintptr_t resources{};
    if (!Read(gBase + kCharacterResolverRegistryRva, resources) || resources == 0U) return 0U;
    return ResolveImpactShooter(reinterpret_cast<void*>(resources), reinterpret_cast<const std::uint32_t*>(hit));
}

// Keep SEH in leaf functions that do not require C++ object unwinding. MSVC
// rejects __try in callers that also own std::lock_guard or other unwindable
// objects (C2712). These helpers isolate the retail calls without changing
// the authority semantics of the impact pipeline.
bool ResolveImpactDamageSafe(int* hit, std::int32_t& damage) noexcept {
    damage = 0;
    if (hit == nullptr || gBase == 0U) return false;
    __try {
        damage = static_cast<std::int32_t>(
            reinterpret_cast<ResolveImpactDamage>(gBase + kResolveImpactDamageRva)(hit));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        damage = 0;
        return false;
    }
}

bool ApplyNativeImpactDamageSafe(const std::uintptr_t target, const std::uint32_t damage,
    const std::uintptr_t source, const std::uint32_t descriptor) noexcept {
    if (target == 0U || source == 0U || damage == 0U || gBase == 0U) return false;
    __try {
        reinterpret_cast<NativeDamage>(gBase + 0x003CC900U)(
            reinterpret_cast<void*>(target), damage, source, 0U,
            0U, 1U, 0U, descriptor, 0U, 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Exact replay of the zero-damage branch of the retail receiver 007CDCF0
// (007CDE26-007CDEF4) for a Client-owned hit: zone table entry from
// [[016F182C]+20C][descriptor]+4C+zone*30; if entry+2E bit0, notify the
// victim brain (007C4EC0(this=victimBrain, shooterBrain, descriptor)); then,
// when the victim brain is the Alien kind (brain+64 -> +4 == 1) and entry+18
// exceeds [0148FD04], add pellets*entry+18 to the repel meter
// [brain+4238]+6C unless +79 is set (retail then adds 0).
bool ApplyNativeImpactStimulusSafe(const std::uintptr_t target, const std::uintptr_t source,
    const std::uint32_t descriptor, const std::uint32_t zone, const std::uint32_t pellets) noexcept {
    using Getter = std::uintptr_t(__thiscall*)(void*);
    using Notify = void(__thiscall*)(void*, std::uintptr_t, std::uint32_t);
    if (target == 0U || source == 0U || gBase == 0U || pellets == 0U || pellets > 16U) return false;
    __try {
        const auto brainOf = [](const std::uintptr_t object) noexcept {
            const auto vtable = *reinterpret_cast<std::uintptr_t*>(object);
            return reinterpret_cast<Getter>(*reinterpret_cast<std::uintptr_t*>(vtable + 0x1CU))(
                reinterpret_cast<void*>(object));
        };
        const auto zones = *reinterpret_cast<std::uintptr_t*>(
            *reinterpret_cast<std::uintptr_t*>(gBase + 0x012F182CU) + 0x20CU);
        const auto definition = zones ? *reinterpret_cast<std::uintptr_t*>(zones + (descriptor & 0xFFFFU) * 4U) : 0U;
        if (definition == 0U) return false;
        const auto entry = definition + 0x4CU + zone * 0x30U;
        const auto victimBrain = brainOf(target);
        if (victimBrain == 0U) return false;
        if ((*reinterpret_cast<std::uint8_t*>(entry + 0x2EU) & 1U) != 0U) {
            const auto shooterBrain = brainOf(source);
            reinterpret_cast<Notify>(gBase + 0x003C4EC0U)(
                reinterpret_cast<void*>(victimBrain), shooterBrain, descriptor & 0xFFFFU);
        }
        const auto kind = *reinterpret_cast<std::uintptr_t*>(victimBrain + 0x64U);
        const float weight = *reinterpret_cast<float*>(entry + 0x18U);
        if (kind != 0U && *reinterpret_cast<std::int32_t*>(kind + 4U) == 1 &&
            weight > *reinterpret_cast<float*>(gBase + 0x0108FD04U)) {
            const auto repel = *reinterpret_cast<std::uintptr_t*>(victimBrain + 0x4238U);
            if (repel != 0U && *reinterpret_cast<std::uint8_t*>(repel + 0x79U) == 0U)
                *reinterpret_cast<float*>(repel + 0x6CU) += static_cast<float>(pellets) * weight;
        }
        gImpactStimulusApplied.fetch_add(1U, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PublishClientImpact(const std::uintptr_t victim, int* hit,
    std::uint32_t channel) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Client || !EngineThread() || !Stable() || victim == 0U ||
        hit == nullptr || gCurrent.localPeer == 0U || (channel != 1U && channel != 2U)) return false;
    if (ResolveImpactSource(hit) != LocalPlayer()) return false;

    std::int32_t damage{};
    if (channel == 1U) {
        if (!ResolveImpactDamageSafe(hit, damage)) return false;
    } else {
        std::uint16_t secondary{};
        if (!Read(reinterpret_cast<std::uintptr_t>(hit) + 0x38U, secondary)) return false;
        damage = static_cast<std::int32_t>(secondary);
    }
    // Zero-HP hits (flame, revolver on the Alien) only run the retail stimulus
    // branch of 007CDCF0: target notification + flame-repel meter. 517 dropped
    // them here, so a Client could never repel the Host Alien. Forward them as
    // channel 3 with the zone (hit+1A) and pellet count (hit+24).
    std::uint32_t packedStimulus{};
    if (damage == 0 && channel == 1U) {
        std::int8_t zone{}; std::int32_t pellets{};
        if (!Read(reinterpret_cast<std::uintptr_t>(hit) + 0x1AU, zone) || zone < 0 ||
            !Read(reinterpret_cast<std::uintptr_t>(hit) + 0x24U, pellets) || pellets <= 0 || pellets > 16) {
            gImpactDamagelessDropped.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        packedStimulus = static_cast<std::uint32_t>(static_cast<std::uint8_t>(zone)) |
            (static_cast<std::uint32_t>(pellets) << 8U);
        channel = 3U;
    } else if (damage <= 0 || damage > 0xFFFF) {
        gImpactDamagelessDropped.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    std::uint32_t entityA{}, entityB{}, type{}, runtimeId{}, descriptor{};
    if (!CharacterIdentity(victim, entityA, entityB, type) || !AICharacterClass(type) ||
        !Read(victim + 0x50U, runtimeId) ||
        !Read(reinterpret_cast<std::uintptr_t>(hit) + 0x18U, descriptor)) return false;
    const auto* actor = Find(victim); // EngineThread is required above.
    if (actor == nullptr || actor->duplicate || actor->faulted ||
        actor->entityA != entityA || actor->entityB != entityB || actor->characterClass != type ||
        actor->desired.sessionCookie != gCurrent.sessionCookie ||
        actor->desired.worldGeneration != gCurrent.worldGeneration ||
        !ImpactTargetLifetimeMatches(actor->remoteBirth, actor->desired.birth,
            runtimeId, actor->runtimeId)) return false;

    ImpactEvent event{};
    event.nativeGeneration = gCurrent.nativeGeneration;
    event.sessionCookie = gCurrent.sessionCookie;
    event.worldGeneration = gCurrent.worldGeneration;
    event.sourcePeer = gCurrent.localPeer;
    event.targetA = entityA; event.targetB = entityB; event.targetClass = type;
    event.targetBirth = actor->remoteBirth;
    event.damage = static_cast<std::uint32_t>(damage);
    event.descriptor = channel == 3U ? (descriptor & 0xFFFFU) : descriptor;
    event.channel = channel;
    event.reserved = packedStimulus;
    if (channel == 3U) gImpactStimulusForwarded.fetch_add(1U, std::memory_order_relaxed);

    std::lock_guard lock(gImpactMailboxMutex);
    const auto pending = gOutgoingImpactWrite - gOutgoingImpactRead;
    if (pending >= kRing) {
        gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    event.sequence = ++gImpactSequence;
    if (event.sequence == 0U) event.sequence = ++gImpactSequence;
    gOutgoingImpacts[gOutgoingImpactWrite % kRing] = event;
    ++gOutgoingImpactWrite;
    gImpactCaptured.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool ClientNpcImpactConsequence(int* hit) noexcept {
    if (!Client() || hit == nullptr) return false;
    const auto source = ResolveImpactSource(hit);
    std::uint32_t type{};
    // Both receivers invoke victim.virtual+70 before common health damage.
    // Refuse the whole locally authored NPC impact, including its pre-reaction,
    // on Clients. Ordered Host damage replay calls 007CC900 directly and never
    // enters these receivers, so its native reaction remains available.
    return source != 0U && Eligible(source, type);
}
bool HostReplicaImpactAlreadyOwned(void* receiver, int* hit,
    const std::uint32_t channel) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host ||
        receiver == nullptr || hit == nullptr) return false;
    std::uintptr_t victim{};
    std::uint32_t type{};
    if (!Read(reinterpret_cast<std::uintptr_t>(receiver) + 0x24U, victim) ||
        victim == 0U || !Eligible(victim, type)) return false;
    const auto source = ResolveImpactSource(hit);
    PairIdentity participant{};
    // PlayerSync runs the same retail weapon update on the Host's Replica as
    // on its owning Client. Only the owner's ordered ClientImpact request may
    // damage this NPC; accepting both paths applies the same shot twice. Use
    // published generation/lifetime bindings because impact dispatch may run
    // on a worker. No Actor list or mutable engine-thread configuration is read.
    if (source == 0U || !RegisteredPlayerRepresentation(source) ||
        !PublishedPairParticipant(source, participant) || participant.peer == 0U) return false;
    std::int32_t damage{};
    if (channel == 1U) {
        if (!ResolveImpactDamageSafe(hit, damage)) return false;
    } else if (channel == 2U) {
        std::uint16_t secondary{};
        if (!Read(reinterpret_cast<std::uintptr_t>(hit) + 0x38U, secondary)) return false;
        damage = static_cast<std::int32_t>(secondary);
    } else {
        return false;
    }
    // 518: the owning Client now forwards primary zero-damage hits as the
    // exact stimulus (channel 3). The Replica's own retail copy would apply
    // that stimulus a second time; secondary zero-damage stays Host-local.
    return damage > 0 || channel == 1U;
}
void __fastcall CharacterImpactDetour(void* receiver, void*, int* hit) noexcept {
    const auto original = reinterpret_cast<CharacterImpact>(
        gCharacterImpactOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (ClientNpcImpactConsequence(hit)) return;
    std::uintptr_t victim{};
    if (ClientAIImpactVictim(receiver, victim)) {
        static_cast<void>(PublishClientImpact(victim, hit, 1U));
        return;
    }
    if (HostReplicaImpactAlreadyOwned(receiver, hit, 1U)) return;
    // Last resort only: the zero-damage branch unconditionally dereferences
    // the shooter. Keep every resolvable or damaging impact retail.
    std::int32_t damage{};
    if (gRole.load(std::memory_order_acquire) == Role::Host && ResolveImpactSource(hit) == 0U &&
        ResolveImpactDamageSafe(hit, damage) && damage == 0) {
        gImpactShooterUnresolved.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    original(receiver, hit);
}
void __fastcall CharacterImpactSecondaryDetour(void* receiver, void*, int* hit) noexcept {
    const auto original = reinterpret_cast<CharacterImpact>(
        gCharacterImpactSecondaryOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (ClientNpcImpactConsequence(hit)) return;
    std::uintptr_t victim{};
    if (ClientAIImpactVictim(receiver, victim)) {
        static_cast<void>(PublishClientImpact(victim, hit, 2U));
        return;
    }
    if (!HostReplicaImpactAlreadyOwned(receiver, hit, 2U)) original(receiver, hit);
}
bool RefuseDeadPairedTarget(const std::uintptr_t source, const std::uintptr_t target) noexcept {
    PairIdentity sourceIdentity{}, targetIdentity{};
    if (gRole.load(std::memory_order_acquire) != Role::Host ||
        !PublishedPairParticipant(target, targetIdentity) ||
        PublishedPairSource(source, sourceIdentity) == kActors || !TerminalDeadCharacter(target)) return false;
    gPairedDeadTargetRejected.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
std::uint32_t __fastcall InteractionDetour(void* manager, void*,
    const std::uint32_t sourceA, const std::uint32_t sourceB,
    const std::uint32_t targetA, const std::uint32_t targetB,
    const std::uint64_t payload, const std::int32_t variant) noexcept {
    const auto original = reinterpret_cast<CharacterInteraction>(
        gInteractionOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable()) {
        PairIdentity sourceIdentity{}, targetIdentity{};
        std::uintptr_t source{}, target{};
        // Use published exact GUIDs; no entity scan, proximity or species rule.
        for (const auto& binding : gPairSources)
            if (LoadPairBinding(binding, sourceIdentity) && sourceIdentity.entityA == sourceA &&
                sourceIdentity.entityB == sourceB) { source = sourceIdentity.character; break; }
        for (const auto& binding : gPairParticipants)
            if (LoadPairBinding(binding, targetIdentity) && targetIdentity.entityA == targetA &&
                targetIdentity.entityB == targetB) { target = targetIdentity.character; break; }
        if (RefuseDeadPairedTarget(source, target)) return 0U;
    }
    const auto result = original(manager, sourceA, sourceB, targetA, targetB, payload, variant);
    if ((result & 0xFFU) != 0U && gRole.load(std::memory_order_acquire) == Role::Host) {
        ++gPairRegistrationAccepted;
        if(!EngineThread())++gPairRegistrationWorkers;
    }
    if ((result & 0xFFU) != 0U && gRole.load(std::memory_order_acquire) == Role::Host &&
        EngineThread() && Stable()) {
        CaptureInteraction(sourceA, sourceB, targetA, targetB, payload, variant);
    }
    return result;
}
void __fastcall PairedTransitionADetour(void* manager, void*,
    const std::uintptr_t target, const std::int32_t variant,
    const std::uint32_t firstFlag) noexcept {
    const auto original = reinterpret_cast<PairedTransitionA>(
        gPairedTransitionAOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{};
    if (Read(reinterpret_cast<std::uintptr_t>(manager) + 4U, source) &&
        RefuseDeadPairedTarget(source, target)) return;
    if (EngineThread() && RegisteredPlayerRepresentation(target))
        static_cast<void>(character_direct_step_runtime::Stop(target));
    original(manager, target, variant, firstFlag);
    if(gRole.load(std::memory_order_acquire)==Role::Host&&!EngineThread())++gPairTransitionWorkers;
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable())
        CapturePairedTransition(reinterpret_cast<std::uintptr_t>(manager), target,
            variant, 1U, firstFlag, 0U);
}
void __fastcall PairedTransitionBDetour(void* manager, void*,
    const std::uintptr_t target, const std::int32_t variant,
    const std::uint32_t firstFlag, const std::uint32_t secondFlag) noexcept {
    const auto original = reinterpret_cast<PairedTransitionB>(
        gPairedTransitionBOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{};
    if (Read(reinterpret_cast<std::uintptr_t>(manager) + 4U, source) &&
        RefuseDeadPairedTarget(source, target)) return;
    if (EngineThread() && RegisteredPlayerRepresentation(target))
        static_cast<void>(character_direct_step_runtime::Stop(target));
    original(manager, target, variant, firstFlag, secondFlag);
    if(gRole.load(std::memory_order_acquire)==Role::Host&&!EngineThread())++gPairTransitionWorkers;
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable())
        CapturePairedTransition(reinterpret_cast<std::uintptr_t>(manager), target,
            variant, 2U, firstFlag, secondFlag);
}
std::uint8_t __fastcall NavigationSubmitDetour(void* request, void*) noexcept {
    const auto original = reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(
        gNavigationSubmitOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    host_producer::BeforeNativeAdmission(reinterpret_cast<std::uintptr_t>(request));
    const auto accepted = original(request);
    host_producer::ObserveNativeAdmission(reinterpret_cast<std::uintptr_t>(request),accepted != 0U);
    return accepted;
}
bool Hook(HookManager& hooks, const char* name, std::uintptr_t rva, void* detour,
    std::initializer_list<unsigned int> prefix, void** trampoline,
    std::atomic<void*>* published, std::string& error,
    const std::uint32_t relocatedAbsoluteRva = 0U) {
    MaskedBytes bytes;
    for (const auto byte : prefix) bytes.bytes.push_back(static_cast<std::uint8_t>(byte));
    if (relocatedAbsoluteRva != 0U) {
        // Retail HIGHLOW relocations adjust the
        // absolute SEH-handler operand in PUSH -1; PUSH handler. Match the
        // actual loaded address, without wildcarding the certified prefix.
        if (bytes.bytes.size() != 7U || bytes.bytes[0] != 0x6AU ||
            bytes.bytes[1] != 0xFFU || bytes.bytes[2] != 0x68U) {
            error = std::string(name) + ": unexpected relocated prefix shape";
            return false;
        }
        const auto absolute = static_cast<std::uint32_t>(gBase) + relocatedAbsoluteRva;
        std::memcpy(bytes.bytes.data() + 3U, &absolute, sizeof(absolute));
    }
    bytes.mask.assign(bytes.bytes.size(), 'x');
    HookRequest request;
    request.name = name; request.target = reinterpret_cast<void*>(gBase + rva);
    request.detour = detour; request.patchLength = bytes.bytes.size();
    request.expected = bytes; request.relocationSafePrefix = true;
    request.trampolineOut = trampoline; request.publishedTrampolineOut = published;
    return hooks.Install(request, error);
}

bool MatchRetailBytes(const std::uintptr_t rva, std::initializer_list<std::uint8_t> expected) noexcept {
    std::array<std::uint8_t, 64U> actual{};
    if (expected.size() > actual.size() || !crash_guard::TryCopy(actual.data(),
            reinterpret_cast<const void*>(gBase + rva), expected.size())) return false;
    return std::memcmp(actual.data(), expected.begin(), expected.size()) == 0;
}
__declspec(naked) void BackstageEligibilityShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push edi
        push esi
        call BackstageParticipantEligibility
        add esp, 8
        mov [ebx + 28], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        test eax, eax
        jnz accepted
        jmp dword ptr [gBackstageEligibilityTrampoline]
    accepted:
        jmp dword ptr [gBackstageEligibilityContinue]
    }
}
__declspec(naked) void BackstageNavigationShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push ebp
        push ecx
        call BackstageParticipantNavigation
        add esp, 8
        mov [ebx], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        // Replaces only MOV EDI,[EBP+3500]; all other registers and flags
        // retain their retail values. EDI was already saved by 0040BBF0.
        jmp dword ptr [gBackstageNavigationContinue]
    }
}
// Preserve all incoming GPRs, flags and floating-point state around registry
// queries. Sound shims replace only the source-kind result/comparison; native
// event strength, flags, hostility, occlusion and subsequent dispatch are retained.
__declspec(naked) void MenacePlayerShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push edi
        push esi
        call MenaceParticipantPlayer
        add esp, 8
        test eax, eax
        jnz participant
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [gMenacePlayerTrampoline]
    participant:
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [gMenacePlayerContinue]
    }
}
__declspec(naked) void SoundSourceClassShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push edi
        call SoundParticipantPlayer
        add esp, 4
        test eax, eax
        jnz participant
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [gSoundSourceClassTrampoline]
    participant:
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        xor eax, eax
        jmp dword ptr [gSoundSourceClassContinue]
    }
}
__declspec(naked) void SoundBackstageClassShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push edi
        call SoundParticipantPlayer
        add esp, 4
        test eax, eax
        jnz participant
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [gSoundBackstageClassTrampoline]
    participant:
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        mov ecx, [edi + 64h]
        cmp eax, eax
        jmp dword ptr [gSoundBackstageClassContinue]
    }
}
__declspec(naked) void SoundThreatClassShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push edi
        call SoundParticipantPlayer
        add esp, 4
        test eax, eax
        jnz participant
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [gSoundThreatClassTrampoline]
    participant:
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        mov edx, [edi + 64h]
        cmp eax, eax
        jmp dword ptr [gSoundThreatClassContinue]
    }
}
__declspec(naked) void SoundLostClassShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push edi
        call SoundParticipantPlayer
        add esp, 4
        test eax, eax
        jnz participant
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [gSoundLostClassTrampoline]
    participant:
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        mov eax, [edi + 64h]
        cmp eax, eax
        jmp dword ptr [gSoundLostClassContinue]
    }
}
bool HookParticipantPlayerSemantics(HookManager& hooks, std::string& error) {
    // The target predicate and menace producer both end with the solo
    // Character+31C / control+48 test. The sound dispatcher tests source kind
    // repeatedly, including the explicit exclusion of NPC sounds backstage.
    // These are read-only query adaptations, not changes to NPC templates or
    // the local player's FPS controller. Keep all native lifetime/target tests.
    if (!MatchRetailBytes(0x002EFD10U, {0x8B,0x44,0x24,0x04,0x56,0x57,0x8B,0xB8,0xA8,0x34,0x00,0x00}) ||
        !MatchRetailBytes(0x002EFD67U, {0x8B,0x80,0x1C,0x03,0x00,0x00,0x85,0xC0,0x74,0x10,0x80,0x78,0x48,0x00,0x74,0x0A}) ||
        !MatchRetailBytes(0x002EFD77U, {0x5F,0xB8,0x01,0x00,0x00,0x00,0x5E,0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x002EFD81U, {0x5F,0x33,0xC0,0x5E,0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x002FBF5FU, {0x8B,0x87,0x1C,0x03,0x00,0x00,0x85,0xC0,0x0F,0x84,0x66,0x01,0x00,0x00,0x80,0x78,0x48,0x00,0x0F,0x84,0x5C,0x01,0x00,0x00}) ||
        !MatchRetailBytes(0x002FBF77U, {0x0F,0x57,0xC0,0x8D,0x44,0x24,0x0C,0x50,0x57,0x8B,0xCE}) ||
        !MatchRetailBytes(0x00330C97U, {0x8B,0x47,0x64,0x8B,0x40,0x04}) ||
        !MatchRetailBytes(0x00330CFDU, {0x8B,0x4F,0x64,0x83,0x79,0x04,0x00,0x74,0x04}) ||
        !MatchRetailBytes(0x00330D3CU, {0x8B,0x57,0x64,0x83,0x7A,0x04,0x00,0x75,0x0A}) ||
        !MatchRetailBytes(0x00330F35U, {0x8B,0x47,0x64,0x83,0x78,0x04,0x00,0x75,0x04})) {
        error = "aisync participant player semantics: certified predicate/menace/sound ABI mismatch";
        return false;
    }
    gMenacePlayerContinue = reinterpret_cast<void*>(gBase + 0x002FBF77U);
    gSoundSourceClassContinue = reinterpret_cast<void*>(gBase + 0x00330C9DU);
    gSoundBackstageClassContinue = reinterpret_cast<void*>(gBase + 0x00330D04U);
    gSoundThreatClassContinue = reinterpret_cast<void*>(gBase + 0x00330D43U);
    gSoundLostClassContinue = reinterpret_cast<void*>(gBase + 0x00330F3CU);
    return Hook(hooks, "aisync_target_is_participant_player", 0x002EFD10U,
            reinterpret_cast<void*>(&TargetIsPlayerDetour), {0x8B,0x44,0x24,0x04,0x56},
            &gTargetIsPlayerTrampoline, &gTargetIsPlayerOriginal, error) &&
        Hook(hooks, "aisync_menace_participant_player", 0x002FBF5FU,
            reinterpret_cast<void*>(&MenacePlayerShim), {0x8B,0x87,0x1C,0x03,0x00,0x00},
            &gMenacePlayerTrampoline, &gMenacePlayerPublished, error) &&
        Hook(hooks, "aisync_sound_participant_source", 0x00330C97U,
            reinterpret_cast<void*>(&SoundSourceClassShim), {0x8B,0x47,0x64,0x8B,0x40,0x04},
            &gSoundSourceClassTrampoline, &gSoundSourceClassPublished, error) &&
        Hook(hooks, "aisync_sound_participant_backstage", 0x00330CFDU,
            reinterpret_cast<void*>(&SoundBackstageClassShim), {0x8B,0x4F,0x64,0x83,0x79,0x04,0x00},
            &gSoundBackstageClassTrampoline, &gSoundBackstageClassPublished, error) &&
        Hook(hooks, "aisync_sound_participant_threat", 0x00330D3CU,
            reinterpret_cast<void*>(&SoundThreatClassShim), {0x8B,0x57,0x64,0x83,0x7A,0x04,0x00},
            &gSoundThreatClassTrampoline, &gSoundThreatClassPublished, error) &&
        Hook(hooks, "aisync_sound_participant_lost", 0x00330F35U,
            reinterpret_cast<void*>(&SoundLostClassShim), {0x8B,0x47,0x64,0x83,0x78,0x04,0x00},
            &gSoundLostClassTrampoline, &gSoundLostClassPublished, error);
}
bool HookBackstageParticipantContext(HookManager& hooks, std::string& error) {
    // 0040C1C0: ECX=sweep context, no stack arguments, AL result, plain RET.
    // 0040C29F: ESI=sweep, EDI=chosen Character, skip only the solo FPS gate.
    // 0040BBFA: EBP=chosen Character, ECX=sweep; EDI is callee-saved already.
    // 004015E0: ECX=4D cycle; aligned native frame, ESI saved, plain RET.
    if (!MatchRetailBytes(0x0000C29FU,
            {0x8B,0x87,0x1C,0x03,0x00,0x00,0x85,0xC0,0x74,0x37,0x80,0x78,0x48,0x00,0x74,0x31}) ||
        !MatchRetailBytes(0x0000C2AFU, {0x83,0x7E,0x2C,0x00}) ||
        !MatchRetailBytes(0x0000C2C7U, {0x53,0x57,0x8B,0xCE,0xE8,0x20,0xF9,0xFF,0xFF}) ||
        !MatchRetailBytes(0x0000C2DAU, {0x8A,0xC3,0x5B,0x5F,0x5E,0xC3}) ||
        !MatchRetailBytes(0x0000BBF0U, {0x83,0xEC,0x10,0x55,0x8B,0x6C,0x24,0x18,0x56,0x57}) ||
        !MatchRetailBytes(0x0000BBFAU, {0x8B,0xBD,0x00,0x35,0x00,0x00,0x8B,0xF1,0x85,0xFF}) ||
        !MatchRetailBytes(0x0000BC0BU, {0x83,0xC4,0x10,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00321270U, {0x83,0xEC,0x7C,0x53,0x56,0x8B,0xF1}) ||
        !MatchRetailBytes(0x00321375U, {0x5E,0x5B,0x83,0xC4,0x7C,0xC3}) ||
        !MatchRetailBytes(0x003214E5U, {0x5F,0x8B,0xCE,0x5E,0x5B,0x83,0xC4,0x7C,0xE9,0x8E,0x6D,0xFB,0xFF}) ||
        !MatchRetailBytes(0x002D8280U, {0x81,0xEC,0x0C,0x0C,0x00,0x00,0x53,0x56,0x8B,0xF1,0x33,0xDB}) ||
        !MatchRetailBytes(0x002D851DU, {0x5E,0x5B,0x81,0xC4,0x0C,0x0C,0x00,0x00,0xC3}) ||
        !MatchRetailBytes(0x002EE5CBU, {0x6A,0x04,0x68,0x58,0x04,0x00,0x00,0xFF,0xD2}) ||
        !MatchRetailBytes(0x002EE61FU, {0x89,0x58,0x48,0x89,0x98,0x50,0x04,0x00,0x00,0x66,0x89,0x98,0x54,0x04,0x00,0x00}) ||
        !MatchRetailBytes(0x002EE639U, {0xC7,0x40,0x44,0xFF,0xFF,0xFF,0xFF,0x5B,0xC3}) ||
        !MatchRetailBytes(0x002C5DBAU, {0x66,0xC7,0x81,0x54,0x04,0x00,0x00,0x01,0x01,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x002C5DC6U, {0xC6,0x81,0x55,0x04,0x00,0x00,0x01,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x002F58D0U, {0x56,0x8B,0x71,0x04,0xC7,0x41,0x04,0x00,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x002F5919U, {0x5E,0xC3}) ||
        !MatchRetailBytes(0x00001628U, {0x8B,0x48,0x08,0x52,0xE8,0x9F,0x82,0x37,0x00}) ||
        !MatchRetailBytes(0x00001656U, {0x5E,0x8B,0xE5,0x5D,0xC3})) {
        error = "aisync backstage context: certified registers/stack/route-cache ABI mismatch";
        return false;
    }
    gBackstageEligibilityContinue = reinterpret_cast<void*>(gBase + 0x0000C2AFU);
    gBackstageNavigationContinue = reinterpret_cast<void*>(gBase + 0x0000BC00U);
    if (!Hook(hooks, "aisync_backstage_participant_update", 0x0000C1C0U,
            reinterpret_cast<void*>(&BackstageUpdateDetour), {0x56,0x8B,0xF1,0x8B,0x4E,0x10},
            &gBackstageUpdateTrampoline, &gBackstageUpdateOriginal, error) ||
        !Hook(hooks, "aisync_backstage_participant_eligibility", 0x0000C29FU,
            reinterpret_cast<void*>(&BackstageEligibilityShim), {0x8B,0x87,0x1C,0x03,0x00,0x00},
            &gBackstageEligibilityTrampoline, &gBackstageEligibilityPublished, error) ||
        !Hook(hooks, "aisync_backstage_participant_navigation", 0x0000BBFAU,
            reinterpret_cast<void*>(&BackstageNavigationShim), {0x8B,0xBD,0x00,0x35,0x00,0x00},
            &gBackstageNavigationTrampoline, &gBackstageNavigationPublished, error) ||
        !Hook(hooks, "aisync_backstage_hold_player_context", 0x000015E0U,
            reinterpret_cast<void*>(&BackstageHoldDetour), {0x55,0x8B,0xEC,0x83,0xE4,0xF0},
            &gBackstageHoldTrampoline, &gBackstageHoldOriginal, error)) return false;
    CallRedirectRequest selected;
    selected.name = "aisync_backstage_living_selected_player";
    selected.callSite = reinterpret_cast<void*>(gBase + 0x0000C285U);
    selected.expectedTarget = reinterpret_cast<void*>(gBase + 0x0000A3D0U);
    selected.detour = reinterpret_cast<void*>(&BackstageSelectedTargetDetour);
    return hooks.InstallCallRedirect(selected, error);
}
bool ValidateRoleTaskCleanupAbi(std::string& error) {
    // Validate the ECX receiver, saved-register epilogue/RET 4, and the
    // explicit stack argument at all three direct retail CALL sites. These
    // bytes contain no PE HIGHLOW operands and remain valid after ASLR.
    if (!MatchRetailBytes(kRoleTaskCleanupRva + 0x18U,
            {0x53,0x56,0x8B,0xF1,0x8B,0x46,0x0C,0x8B,0x4E,0x08}) ||
        !MatchRetailBytes(kRoleTaskCleanupRva + 0x119U,
            {0x8B,0x4C,0x24,0x14,0x5F,0x5E,0x5B,0x64,0x89,0x0D,0x00,0x00,0x00,0x00,
             0x83,0xC4,0x14,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00366D54U,
            {0x8B,0x88,0x24,0x04,0x00,0x00,0x57,0xE8,0x30,0xAD,0xFF,0xFF}) ||
        !MatchRetailBytes(0x00366F74U,
            {0x57,0x8B,0xCB,0xE8,0x14,0xAB,0xFF,0xFF}) ||
        !MatchRetailBytes(0x0036735BU,
            {0x56,0xE8,0x2F,0xA7,0xFF,0xFF})) {
        error = "aisync role-task cleanup: certified ECX/stack argument/RET 4 bytes mismatch";
        return false;
    }
    return true;
}
bool HookPairedPlayerClassification(HookManager& hooks, std::string& error) {
    // 006626F7: ECX=victim, ESI=source 4E cycle, EBX=0. Skip only the
    // NPC-controller test; 0066270E retains 29B/difficulty/health/fallback.
    // 006E3A4D: ESI=victim, EBX=filter. Resume at MOV EAX,1 (player category)
    // then the original three-argument filter call, with ECX/EDX unchanged.
    if (!MatchRetailBytes(0x002626F4U,
            {0x8B,0x4E,0x44,0x8B,0x81,0x1C,0x03,0x00,0x00,0x3B,0xC3,
             0x0F,0x84,0xFE,0x00,0x00,0x00,0x38,0x58,0x48,0x0F,0x84,0xF5,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x0026270EU,
            {0x38,0x99,0x9B,0x02,0x00,0x00,0x0F,0x85,0xE9,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x00262730U,
            {0x8B,0x4E,0x04,0x8B,0x01,0x8B,0x50,0x1C,0xFF,0xD2,
             0x8B,0x80,0x34,0x42,0x00,0x00,0x83,0x78,0x74,0x02,0x0F,0x8C,0xB9,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x002627FAU, {0xE8,0xD1,0x8B,0x09,0x00,0x3B,0xC3,0x75,0x34}) ||
        !MatchRetailBytes(0x002FB437U, {0xC2,0x10,0x00}) ||
        !MatchRetailBytes(0x002FB446U, {0xC2,0x10,0x00}) ||
        !MatchRetailBytes(0x002E3A4DU,
            {0x8B,0x86,0x1C,0x03,0x00,0x00,0x85,0xC0,0x74,0x0D,0x80,0x78,0x48,0x00,0x74,0x07,
             0xB8,0x01,0x00,0x00,0x00,0xEB,0x11}) ||
        !MatchRetailBytes(0x002E3A75U,
            {0x50,0x51,0x52,0x8B,0xCB,0xE8,0x41,0x23,0xFF,0xFF}) ||
        !MatchRetailBytes(0x002E3B06U,
            {0x5F,0x5E,0xB0,0x01,0x5B,0x8B,0xE5,0x5D,0xC2,0x14,0x00}) ||
        !MatchRetailBytes(0x002D5E25U, {0x5F,0x5B,0x5E,0x5D,0xC2,0x0C,0x00}) ||
        !MatchRetailBytes(0x00263E4AU,
            {0x8B,0x46,0x44,0x85,0xC0,0x74,0x58,0x8B,0x80,0x1C,0x03,0x00,0x00,
             0x85,0xC0,0x74,0x06,0x80,0x78,0x48,0x00,0x75,0x48}) ||
        !MatchRetailBytes(0x00263E71U, {0x83,0x79,0x74,0x03}) ||
        !MatchRetailBytes(0x00263EA9U, {0x8B,0x86,0xE0,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x002638D0U, {0x56,0x8B,0xF1,0xE8,0x98,0xED,0xFF,0xFF}) ||
        !MatchRetailBytes(0x002638EAU, {0xC7,0x86,0xE0,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x5E,0xC3}) ||
        !MatchRetailBytes(0x00263900U, {0x56,0x8B,0xF1,0x8B,0x86,0x18,0x01,0x00,0x00,0x24,0x07,0x3C,0x01}) ||
        !MatchRetailBytes(0x0026390FU, {0xE8,0x5C,0xED,0xFF,0xFF,0xC7,0x86,0xE0,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x5E,0xC3}) ||
        !MatchRetailBytes(0x00264A96U, {0x8B,0x46,0x44,0x8B,0x80,0x1C,0x03,0x00,0x00,0x85,0xC0,0x74,0x0F,
            0x80,0x78,0x48,0x00,0x74,0x09,0x80,0xBE,0xE5,0x00,0x00,0x00,0x00,0x74,0x14})) {
        error = "aisync paired player classification: certified victim/register/branch/stack bytes mismatch";
        return false;
    }
    gPlayerPreparationContinue = reinterpret_cast<void*>(gBase + 0x0026270EU);
    gPlayerCategoryContinue = reinterpret_cast<void*>(gBase + 0x002E3A5DU);
    gPlayerRuntimeContinue = reinterpret_cast<void*>(gBase + 0x00263EA9U);
    gOutcomePreparationContinue = reinterpret_cast<void*>(gBase + 0x00262837U);
    gOutcomePreparationAbort = reinterpret_cast<void*>(gBase + 0x002626CCU);
    return Hook(hooks, "aisync_paired_player_preparation", 0x002626F7U,
        reinterpret_cast<void*>(&PlayerPreparationShim), {0x8B,0x81,0x1C,0x03,0x00,0x00},
        &gPlayerPreparationTrampoline, &gPlayerPreparationPublished, error) &&
        Hook(hooks, "aisync_paired_player_category", 0x002E3A4DU,
        reinterpret_cast<void*>(&PlayerCategoryShim), {0x8B,0x86,0x1C,0x03,0x00,0x00},
        &gPlayerCategoryTrampoline, &gPlayerCategoryPublished, error) &&
        Hook(hooks, "aisync_paired_player_runtime", 0x00263E51U,
        reinterpret_cast<void*>(&PlayerRuntimeShim), {0x8B,0x80,0x1C,0x03,0x00,0x00},
        &gPlayerRuntimeTrampoline, &gPlayerRuntimePublished, error);
}

bool HookNativeOutcomes(HookManager& hooks, std::string& error) {
    // Every continuation starts at an instruction boundary. No relocated
    // operand or relative branch is copied into these three trampolines.
    std::uintptr_t definitionGetter{};
    if (!Read(gBase + 0x00ECC330U + 0x34U, definitionGetter) || definitionGetter != gBase + 0x002D60F0U ||
        !MatchRetailBytes(0x002D60F0U,
            {0x8B,0x44,0x24,0x04,0x83,0xF8,0x19,0x73,0x17,0x83,0xF8,0xFF,0x74,0x12,
             0x8B,0x89,0xC0,0x01,0x00,0x00,0x69,0xC0,0x84,0x00,0x00,0x00,
             0x03,0x41,0x04,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00262837U,
            {0x89,0x46,0x4C,0x39,0x58,0x08,0x0F,0x94,0xC0,0x88,0x86,0xE5,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x002626CCU,
            {0x8B,0x8C,0x24,0xE4,0x00,0x00,0x00,0x64,0x89,0x0D,0x00,0x00,0x00,0x00,
             0x5F,0x5E,0x5B,0x8B,0xE5,0x5D,0xC3}) ||
        !MatchRetailBytes(0x00255B30U, {0x81,0xEC,0x84,0x00,0x00,0x00,0x53,0x55,0x56,0x8B,0xF1}) ||
        !MatchRetailBytes(0x00255B4AU,
            {0x80,0xBE,0xDA,0x00,0x00,0x00,0x00,0x74,0x3A,0x8B,0x46,0x04,
             0x80,0xB8,0x9B,0x02,0x00,0x00,0x00,0x74,0x2E}) ||
        !MatchRetailBytes(0x00255B5FU,
            {0xC6,0x80,0xE8,0x02,0x00,0x00,0x00,0x8B,0x46,0x04,
             0xC6,0x80,0xEA,0x02,0x00,0x00,0x00,0x8B,0x4E,0x68,0x51,0x8B,0x4E,0x04,
             0xE8,0x94,0xB0,0x17,0x00,0x8B,0x4E,0x04,0x6A,0x02,
             0xE8,0x3A,0x48,0x14,0x00,0x8B,0xCE,0xE8,0x13,0x44,0xFF,0xFF}) ||
        !MatchRetailBytes(0x003B4AD0U,
            {0x56,0x8B,0xF1,0x0F,0xB7,0x46,0x1C,0x66,0x85,0xC0,0x74,0x62,
             0x66,0x8B,0x4C,0x24,0x08}) ||
        !MatchRetailBytes(0x003B4B3EU, {0x5E,0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x003CC952U, {0xC2,0x24,0x00}) ||
        !MatchRetailBytes(0x003D0C63U, {0x5F,0x5E,0x59,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x0026BF29U,
            {0x8B,0x4E,0x04,0x8B,0x11,0x8B,0x42,0x1C,0xFF,0xD0,
             0x8B,0x88,0xB4,0x34,0x00,0x00,0x0F,0xB7,0x86,0x0A,0x01,0x00,0x00,
             0x8B,0x11,0x8B,0x52,0x34,0x50,0xFF,0xD2,0x89,0x46,0x4C}) ||
        !MatchRetailBytes(0x00255DFAU, {0x5F,0x5E,0x5D,0x5B,0x81,0xC4,0x84,0x00,0x00,0x00,0xC3})) {
        error = "aisync native outcome: certified ECX/ESI/EAX/RET/continuation bytes mismatch";
        return false;
    }
    gVictimFinishSkip = reinterpret_cast<void*>(gBase + 0x00255B8DU);
    gVictimFinishKill = reinterpret_cast<void*>(gBase + 0x00255B5FU);
    if (!MatchRetailBytes(0x00249FA0U, {0x51,0x56,0x8B,0xF1,0x8B,0x46,0x04}) ||
        !MatchRetailBytes(0x0024A076U, {0x5E,0x59,0xC3})) {
        error = "aisync native outcome: victim finish ECX-only/plain RET mismatch"; return false;
    }
    if (!(Hook(hooks, "aisync_native_outcome_health_commit", 0x003B4AD0U,
            reinterpret_cast<void*>(&OutcomeHealthCommitDetour), {0x56,0x8B,0xF1,0x0F,0xB7,0x46,0x1C},
            &gHealthCommitTrampoline, &gHealthCommitOriginal, error) &&
        Hook(hooks, "aisync_native_outcome_victim_update", 0x00255B30U,
            reinterpret_cast<void*>(&VictimOutcomeUpdateDetour), {0x81,0xEC,0x84,0x00,0x00,0x00,0x53},
            &gVictimUpdateTrampoline, &gVictimUpdateOriginal, error) &&
        Hook(hooks, "aisync_native_outcome_victim_finish", 0x00255B4AU,
            reinterpret_cast<void*>(&VictimFinishShim), {0x80,0xBE,0xDA,0x00,0x00,0x00,0x00},
            &gVictimFinishTrampoline, &gVictimFinishPublished, error))) return false;
    CallRedirectRequest finished;
    finished.name = "aisync_native_outcome_victim_finish_executed";
    finished.callSite = reinterpret_cast<void*>(gBase + 0x00255B88U);
    finished.expectedTarget = reinterpret_cast<void*>(gBase + 0x00249FA0U);
    finished.detour = reinterpret_cast<void*>(&VictimFinishExecutedDetour);
    return hooks.InstallCallRedirect(finished, error);
}

bool HookPostDeathMeleeSelection(HookManager& hooks, std::string& error) {
    std::uintptr_t callback{};
    // These are ActionMeleeAttack's callbacks, not an Alien class test.
    // Compare relocated pointers by RVA; all byte ranges below contain no
    // HIGHLOW operands. Installation is refused on an unrecognized contract.
    if (!Read(gBase + 0x00ECB770U + 0x30U, callback) || callback != gBase + 0x006EF7B0U ||
        !Read(gBase + 0x00ECB770U + 0x34U, callback) || callback != gBase + 0x00317D90U ||
        !Read(gBase + 0x00ECB770U + 0x48U, callback) || callback != gBase + 0x002F0A10U ||
        !Read(gBase + 0x00ECB770U + 0x3D8U, callback) || callback != gBase + 0x00AA70F0U ||
        !MatchRetailBytes(0x006EF7B0U, {0xC2,0x14,0x00}) ||
        !MatchRetailBytes(0x00AA70F0U, {0xB0,0x01,0xC3}) ||
        !MatchRetailBytes(0x003261DDU,
            {0x52,0x0F,0xB6,0x11,0x8B,0x4E,0x0C,0x52,0x56,0x50,0x51,0x8B,0x48,0x10,
             0xE8,0x70,0xA0,0xFA,0xFF}) ||
        !MatchRetailBytes(0x002D0260U,
            {0x56,0x8B,0x74,0x24,0x0C,0x80,0x7E,0x1F,0x00,0x57,0x8B,0xF9,0x74,0x4C,
             0x8B,0x07,0x8B,0x90,0xD8,0x03,0x00,0x00,0xFF,0xD2,0x84,0xC0,0x74,0x3E,
             0xB8,0x01,0x00,0x00,0x00,0x39,0x46,0x08,0x74,0x0A,0x89,0x46,0x04,
             0x8B,0x07,0x8B,0x40,0x30,0xEB,0x36}) ||
        !MatchRetailBytes(0x002D02B5U, {0x5F,0x5E,0xC2,0x14,0x00}) ||
        !MatchRetailBytes(0x002D02DFU, {0x5F,0x5E,0xC2,0x14,0x00}) ||
        !MatchRetailBytes(0x002F0A33U,
            {0x8B,0x74,0x24,0x08,0x89,0x48,0x10,0x8B,0x4C,0x24,0x0C,0x89,0x70,0x08,
             0x89,0x50,0x04,0x89,0x50,0x0C,0x89,0x48,0x14,0x89,0x50,0x18,0x89,0x50,0x20,
             0x89,0x50,0x24,0x88,0x50,0x28,0xC7,0x40,0x1C,0x00,0x00,0x00,0x01}) ||
        !MatchRetailBytes(0x002F0A63U,
            {0x89,0x50,0x2C,0x89,0x50,0x30,0x5E,0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x0032630BU,
            {0x8B,0x4F,0x10,0x8B,0x11,0x8B,0x46,0x0C,0x8B,0x52,0x44,0x56,
             0x6A,0x08,0x57,0x50,0xFF,0xD2})) {
        error = "aisync post-death melee selection: certified mode/frame/ECX/five arguments/RET 14h mismatch";
        return false;
    }
    CallRedirectRequest selection;
    selection.name = "aisync_post_death_melee_selection";
    selection.callSite = reinterpret_cast<void*>(gBase + 0x003261EBU);
    selection.expectedTarget = reinterpret_cast<void*>(gBase + 0x002D0260U);
    selection.detour = reinterpret_cast<void*>(&MeleeSelectionDetour);
    return hooks.InstallCallRedirect(selection, error);
}
void* gBackstageEventTrampoline{},*gFrontstageEventTrampoline{};
std::atomic<void*> gBackstageEventOriginal{},gFrontstageEventOriginal{};
void* gForcedRelocateTrampoline{};
std::atomic<void*> gForcedRelocateOriginal{};
thread_local std::uint32_t gLifecycleEventDepth{};
struct LifecycleEventFrame {
    LifecycleEventFrame() noexcept {++gLifecycleEventDepth;}
    ~LifecycleEventFrame(){--gLifecycleEventDepth;}
};
bool AnimatedCharacterForEvent(std::uintptr_t character,std::uintptr_t& animated) noexcept {
    __try {
        std::uintptr_t table{},getter{};
        if(!Read(character,table)||!Read(table+0x1CU,getter)||getter<gBase+0x1000U||getter>=gBase+0xD9E13BU)return false;
        animated=reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(reinterpret_cast<void*>(character));
        return animated!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void TraceHostLifecycle(std::uint32_t stage,std::uintptr_t character,const host_producer::Command& command,
    std::uint32_t result,std::uint32_t caller) noexcept {
    if(gRole.load(std::memory_order_acquire)!=Role::Host||(EngineThread()&&!Find(character)))return;
    execution::LifecycleTraceSample sample{};
    sample.stage=stage;sample.kind=command.kind;sample.mode=command.mode;sample.result=result;sample.caller=caller;
    sample.callbackThread=GetCurrentThreadId();
    sample.hasGoal=command.point;sample.x=command.x;sample.y=command.y;sample.z=command.z;sample.yaw=command.heading;
    float position[3]{};
    if(ReadCharacterPosition(character,position)){sample.afterX=position[0];sample.afterY=position[1];sample.afterZ=position[2];}
    RecordLifecycleTrace(sample);
}
std::atomic<std::uint64_t> gUnpublishedPlacements{};
// Engine-thread journal for native events observed before the authenticated
// producer opens. A pose refresh alone cannot repair an already sent baseline.
struct PendingLifecycleEvent {
    host_producer::Command command{};
    transactions::Scope source{};
    std::uintptr_t character{};
    std::uint64_t cookie{}, world{}, epoch{};
    std::uint32_t generation{}, runtime{}, birth{}, caller{};
    bool sourceKnown{};
};
std::array<PendingLifecycleEvent,128> gPendingLifecycle{};
std::mutex gPendingLifecycleGate;
std::size_t gPendingLifecycleRead{},gPendingLifecycleCount{};
bool gPendingLifecycleOverflow{};
std::uint64_t gPendingLifecycleEpoch{};
transactions::Scope gLifecycleObservedScope{};
bool gLifecycleObservedScopeKnown{};
bool LifecycleContextAvailable() noexcept {
    return Installed()&&!gStopping.load(std::memory_order_acquire)&&EngineThread()&&
        gRole.load(std::memory_order_acquire)==Role::Host&&gCurrent.role==Role::Host&&
        gGenerations&&gGeneration==gCurrent.nativeGeneration&&
        gGeneration==gGenerations->CurrentGeneration()&&gCurrent.sessionCookie&&gCurrent.worldGeneration;
}
bool PublishLifecycleBaseline(Actor& actor,const transactions::Scope& scope) noexcept {
    if(actor.spawnDiagnosticScope==scope&&actor.spawnDiagnosticBirth==actor.birth)return true;
    auto state=actor.desired;float position[3]{},yaw{};
    if(!ReadCharacterPosition(actor.character,position)||!Read(actor.character+kYawOffset,yaw))return false;
    for(std::size_t i=0;i<3;++i)state.position[i]=position[i];
    state.yaw=yaw;state.stateFlags|=PoseValid;
    if(!ValidateOutgoing(state))return false;
    // The ordered Client must see a baseline BEFORE the first native event;
    // putting it behind a waiting relocation would deadlock actor preparation.
    if(host_producer::EngineProducer().AppendState(state)!=host_producer::SourceResult::Ok)return false;
    actor.spawnDiagnosticScope=scope;actor.spawnDiagnosticBirth=actor.birth;
    RecordSpawnPlacement(actor,state,true);
    return true;
}
void DrainHostLifecycleEvents() noexcept {
    if(!LifecycleContextAvailable()||gLifecycleDrainActive)return;
    struct DrainGuard {
        DrainGuard(){gLifecycleDrainActive=true;}
        ~DrainGuard(){gLifecycleDrainActive=false;}
    } draining;
    const std::lock_guard lock(gPendingLifecycleGate);
    const auto epoch=gPairBindingEpoch.load(std::memory_order_acquire);
    // An overflow belongs to its configuration, never to the next round.
    if(gPendingLifecycleEpoch!=epoch) {
        gPendingLifecycleRead=gPendingLifecycleCount=0;gPendingLifecycleOverflow=false;gPendingLifecycleEpoch=epoch;
        gLifecycleObservedScope={};gLifecycleObservedScopeKnown=false;
        gLifecycleEventsPending=false;
    }
    if(!HostNativeCaptureQualified())return;
    if(gPendingLifecycleOverflow) {
        host_producer::EngineProducer().LoseCoverage(execution::Recovery::Capacity);return;
    }
    transactions::Scope scope{};if(!host_producer::HostScope(scope))return;
    gLifecycleObservedScope=scope;gLifecycleObservedScopeKnown=true;
    while(gPendingLifecycleCount) {
        const auto event=gPendingLifecycle[gPendingLifecycleRead];
        const auto pop=[]() noexcept {
            gPendingLifecycleRead=(gPendingLifecycleRead+1)%gPendingLifecycle.size();--gPendingLifecycleCount;
            gLifecycleEventsPending.store(gPendingLifecycleCount!=0,std::memory_order_release);
        };
        if(event.cookie!=gCurrent.sessionCookie||event.world!=gCurrent.worldGeneration||
            event.generation!=gCurrent.nativeGeneration||event.epoch!=epoch||
            (event.sourceKnown&&event.source!=scope)){pop();continue;} // Retired configuration.
        auto* actor=Find(event.character);std::uint32_t runtime{};
        if(!actor||actor->birth!=event.birth||actor->runtimeId!=event.runtime||
            !Read(event.character+0x50U,runtime)||runtime!=event.runtime){pop();continue;} // Retired construction.
        // The owned PvP actor has its own command stream. Its locomotion
        // lifecycle must not turn a deliberate Coop exclusion into a global
        // LoseCoverage/recovery. Pre-ownership draining events still pass.
        if(gPvPLease.owned && PvPCharacterFenced(event.character)){pop();continue;}
        if(!PublishLifecycleBaseline(*actor,scope)) {
            // Retain the fact, but never let a later command cross an
            // unrepresentable baseline and start a silently divergent stream.
            host_producer::EngineProducer().LoseCoverage(execution::Recovery::NativeUnavailable);return;
        }
        transactions::OwnerKey owner{};
        if(!HostNativeCaptureContext(event.character,owner)) {
            TraceHostLifecycle(5U,event.character,event.command,static_cast<std::uint32_t>(execution::Recovery::NativeUnavailable),event.caller);
            host_producer::EngineProducer().LoseCoverage(execution::Recovery::NativeUnavailable);return;
        }
        const auto result=host_producer::EngineProducer().NativeEvent(owner,event.command);
        TraceHostLifecycle(result==host_producer::SourceResult::Ok?7U:5U,event.character,event.command,
            static_cast<std::uint32_t>(result),event.caller);
        if(result!=host_producer::SourceResult::Ok) {
            host_producer::EngineProducer().LoseCoverage(execution::Recovery::NativeUnavailable);return;
        }
        if(event.command.kind==native_intent::kKind)++gCycleCallsPublished;
        pop();
    }
}
bool QueueHostLifecycleEvent(std::uintptr_t character,const host_producer::Command& command,std::uint32_t caller) noexcept {
    if(!Installed()||gStopping.load(std::memory_order_acquire)||gRole.load(std::memory_order_acquire)!=Role::Host)return false;
    PendingLifecycleEvent captured{};captured.command=command;captured.character=character;captured.caller=caller;
    captured.sourceKnown=host_producer::HostScope(captured.source);
    if(EngineThread()) {
        if(!LifecycleContextAvailable())return false;
        const auto* actor=Find(character);std::uint32_t runtime{};
        if(!actor||actor->duplicate||actor->faulted||!actor->birth||
            !Read(character+0x50U,runtime)||runtime!=actor->runtimeId)return false;
        captured.runtime=runtime;captured.birth=actor->birth;captured.cookie=gCurrent.sessionCookie;
        captured.world=gCurrent.worldGeneration;captured.generation=gCurrent.nativeGeneration;
        captured.epoch=gPairBindingEpoch.load(std::memory_order_acquire);
    } else {
        // Native lifecycle callbacks may execute on a CATHODE worker. Never
        // read Actor/gCurrent here and never invoke a network/native executor.
        // The existing atomically published NPC binding supplies its birth.
        PairIdentity identity{};bool found{};
        for(const auto& slot:gPairSources)if(LoadPairBinding(slot,identity)&&identity.character==character&&
            identity.authoritativeBirth&&identity.runtimeId&&identity.type&&
            (identity.entityA||identity.entityB)) {found=true;break;}
        std::uint32_t runtime{};
        if(!found||!Read(character+0x50U,runtime)||runtime!=identity.runtimeId)return false;
        captured.runtime=runtime;captured.birth=identity.authoritativeBirth;
        captured.cookie=gActiveSessionCookie.load(std::memory_order_acquire);
        captured.world=gActiveWorldGeneration.load(std::memory_order_acquire);
        captured.generation=identity.generation;captured.epoch=identity.epoch;
        if(!captured.cookie||!captured.world)return false;
    }
    if(captured.sourceKnown) {
        std::uint64_t cookie{};for(std::size_t i=0;i<8;++i)cookie|=static_cast<std::uint64_t>(captured.source.session[i])<<(8*i);
        if((cookie?cookie:1)!=captured.cookie||captured.source.worldEpoch!=captured.world)return false;
    }
    const auto epoch=captured.epoch;
    const std::lock_guard lock(gPendingLifecycleGate);
    if(gRole.load(std::memory_order_acquire)!=Role::Host||epoch!=gPairBindingEpoch.load(std::memory_order_acquire)||
        captured.generation!=gActiveNativeGeneration.load(std::memory_order_acquire))return false;
    if(gPendingLifecycleEpoch!=epoch) {
        gPendingLifecycleRead=gPendingLifecycleCount=0;gPendingLifecycleOverflow=false;gPendingLifecycleEpoch=epoch;
        gLifecycleObservedScope={};gLifecycleObservedScopeKnown=false;
    }
    if(gPendingLifecycleCount==gPendingLifecycle.size()) {
        gPendingLifecycleOverflow=true;gLifecycleEventsPending=true;return false;
    }
    auto& event=gPendingLifecycle[(gPendingLifecycleRead+gPendingLifecycleCount++)%gPendingLifecycle.size()];
    event=captured;
    if(event.sourceKnown){gLifecycleObservedScope=event.source;gLifecycleObservedScopeKnown=true;}
    else if(gLifecycleObservedScopeKnown){event.source=gLifecycleObservedScope;event.sourceKnown=true;}
    gLifecycleEventsPending.store(true,std::memory_order_release);
    return true;
}
void RefreshPublishedPose(std::uintptr_t character) noexcept {
    auto* actor=Find(character);float position[3]{},yaw{};
    if(!actor||actor->duplicate||actor->faulted||!ReadCharacterPosition(character,position)||
        !Read(character+kYawOffset,yaw)||!std::isfinite(yaw)||std::abs(yaw)>3.3F)return;
    for(std::size_t i=0;i<3U;++i){actor->lastPosition[i]=position[i];actor->desired.position[i]=position[i];}
    actor->desired.yaw=yaw;actor->desired.stateFlags|=PoseValid;
    actor->hasLastPosition=true;actor->lastPoseSample=GetTickCount64();actor->lastSend=0U;
    gUnpublishedPlacements.fetch_add(1U,std::memory_order_relaxed);
}
void CaptureLifecycleEvent(std::uintptr_t character,const host_producer::Command& command,bool nested=false,
    std::uint32_t caller=0) noexcept {
    if(gPlacementChild&&gPlacementChild->qualified&&gPlacementChild->parent.character==character) {
        const auto& child=*gPlacementChild;host_producer::NativePlacementIdentity current{};
        const bool represented=child.frontstage?command.kind==execution::kFrontstageKind:
            (command.kind==execution::kRelocateKind||command.kind==execution::kForcedRelocateKind||
             command.kind==execution::kDirectRelocateKind||command.kind==execution::kDirectForcedRelocateKind);
        if(represented&&host_producer::IdentifyNativePlacement(child.parent.scheduler,current)&&current==child.parent&&
            HostNativeRetirementQualified(current.command.scope,current.command.owner,character,current.runtime)) {
            ++gPlacementChildrenDerived;return;
        }
    }
    if(gLifecycleEventDepth&&!nested){TraceHostLifecycle(3U,character,command,0U,caller);return;}
    // All callback threads use one FIFO. Only its certified consumer accesses
    // Actor, publishes baselines and submits commands to the Host producer.
    const bool queued=QueueHostLifecycleEvent(character,command,caller);
    TraceHostLifecycle(queued?6U:1U,character,command,0U,caller);
    if(queued&&EngineThread())DrainHostLifecycleEvents();
}
void __fastcall ForcedRelocateDetour(void* character,void*,const float* position,float yaw,
    std::uint32_t mode,std::uint32_t force) noexcept {
    const auto original=reinterpret_cast<void(__thiscall*)(void*,const float*,float,std::uint32_t,std::uint32_t)>(
        gForcedRelocateOriginal.load(std::memory_order_acquire));
    if(!original)return;
    // Capture the common native position writer, including non-forced and
    // nested calls. Replaying the wrapper alone missed these real Host causes.
    const bool hostRole=gRole.load(std::memory_order_acquire)==Role::Host;
    const bool unpublished=hostRole&&!HostNativeCaptureGuard();
    if(hostRole) {
        std::array<float,3> point{};
        if(crash_guard::TryCopy(point.data(),position,sizeof(point))) {
            const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            host_producer::Command command{};
            // Preserve the wrapper's controller callbacks when the Host used
            // it; raw spawn/physics callers replay the common primitive itself.
            command.kind=(force&0xFFU)?
                (caller==gBase+0x3C6420U?execution::kForcedRelocateKind:execution::kDirectForcedRelocateKind):
                (caller==gBase+0x3C63A0U?execution::kRelocateKind:execution::kDirectRelocateKind);
            command.admission=1;command.point=true;command.heading=yaw;command.mode=mode;
            command.x=point[0];command.y=point[1];command.z=point[2];
            CaptureLifecycleEvent(reinterpret_cast<std::uintptr_t>(character),command,true,static_cast<std::uint32_t>(caller-gBase));
        }
    }
    {const LifecycleEventFrame frame;original(character,position,yaw,mode,force);}
    // Refresh the initial pose as well, but preserve the native event in the
    // startup journal even when the Client already accepted an older baseline.
    if(unpublished&&EngineThread())RefreshPublishedPose(reinterpret_cast<std::uintptr_t>(character));
}
// The Character pose at a backstage/frontstage transition belongs to that
// native event: retail moved the hidden body along its own backstage network,
// which a Client cannot choose for it. The Client applies this pose while the
// body is hidden, so the emergence happens exactly where the Host's did.
void AttachTransitionPose(std::uintptr_t character,host_producer::Command& command) noexcept {
    float position[3]{},yaw{};
    if(!ReadCharacterPosition(character,position)||!Read(character+kYawOffset,yaw)||!std::isfinite(yaw)||std::abs(yaw)>3.3F)return;
    command.point=true;command.x=position[0];command.y=position[1];command.z=position[2];command.heading=yaw;
}
void __fastcall BackstageEventDetour(void* character,void*,std::uint8_t backstage) noexcept {
    host_producer::Command command{};command.kind=execution::kBackstageKind;command.admission=1;command.mode=backstage?1U:0U;
    AttachTransitionPose(reinterpret_cast<std::uintptr_t>(character),command);
    CaptureLifecycleEvent(reinterpret_cast<std::uintptr_t>(character),command);
    const auto original=reinterpret_cast<void(__thiscall*)(void*,std::uint8_t)>(gBackstageEventOriginal.load());
    const LifecycleEventFrame frame;original(character,backstage);
}
void __fastcall FrontstageEventDetour(void* animated,void*) noexcept {
    if(!gLifecycleEventDepth&&!EngineThread()&&gRole.load(std::memory_order_acquire)==Role::Host) {
        PairIdentity identity{};
        for(const auto& slot:gPairSources)if(LoadPairBinding(slot,identity)&&identity.animated==reinterpret_cast<std::uintptr_t>(animated)) {
            host_producer::Command command{};command.kind=execution::kFrontstageKind;command.admission=1;
            AttachTransitionPose(identity.character,command);CaptureLifecycleEvent(identity.character,command);break;
        }
    }
    if(!gLifecycleEventDepth&&LifecycleContextAvailable())for(const auto& actor:gActors) {
        if(!actor.character||actor.duplicate||actor.faulted)continue;
        std::uintptr_t local{};
        if(AnimatedCharacterForEvent(actor.character,local)&&local==reinterpret_cast<std::uintptr_t>(animated)) {
            host_producer::Command command{};command.kind=execution::kFrontstageKind;command.admission=1;
            AttachTransitionPose(actor.character,command);
            CaptureLifecycleEvent(actor.character,command);break;
        }
    }
    const auto original=reinterpret_cast<void(__thiscall*)(void*)>(gFrontstageEventOriginal.load());
    const LifecycleEventFrame frame;original(animated);
}

host_producer::NativePlacementIdentity PlacementParent(std::uintptr_t character) noexcept {
    host_producer::NativePlacementIdentity result{};
    if(!execution::PlacementDedupReady()||!HostNativeCaptureQualified())return result;
    const auto scheduler=character+0x2CD0U;
    std::uintptr_t head{},first{},request{};std::uint32_t kind{};
    if(!Read(scheduler+4,head)||!head||!Read(head,first)||first==head||
        !Read(first+8,request)||!request||!Read(request+0x10,kind)||kind!=execution::kPlacementKind)return result;
    // A producer may edit the payload after Submit. Publish that revision
    // before allowing its native child to be represented by the request.
    host_producer::ObserveNativeMutation(request);
    if(!host_producer::IdentifyNativePlacement(scheduler,result)||result.character!=character||
        !HostNativeRetirementQualified(result.command.scope,result.command.owner,character,result.runtime))return {};
    return result;
}
template<bool Forced> void __fastcall PlacementChildDetour(void* character,void*,const float* position,
    float yaw,std::uint32_t mode) noexcept {
    PlacementChildFrame frame{};frame.previous=gPlacementChild;
    frame.parent=PlacementParent(reinterpret_cast<std::uintptr_t>(character));frame.qualified=frame.parent.request!=0;
    gPlacementChild=&frame;
    __try {
        constexpr auto target=Forced?0x3C63B0U:0x3C6330U;
        reinterpret_cast<void(__thiscall*)(void*,const float*,float,std::uint32_t)>(gBase+target)(character,position,yaw,mode);
    }__finally {gPlacementChild=frame.previous;}
}
void __fastcall PlacementFrontstageChildDetour(void* animated,void*) noexcept {
    PlacementChildFrame frame{};frame.previous=gPlacementChild;frame.frontstage=true;
    if(HostNativeCaptureQualified())for(const auto& actor:gActors)if(actor.character&&!actor.duplicate&&!actor.faulted) {
        std::uintptr_t current{};
        if(AnimatedCharacterForEvent(actor.character,current)&&current==reinterpret_cast<std::uintptr_t>(animated)) {
            frame.parent=PlacementParent(actor.character);frame.qualified=frame.parent.request!=0;break;
        }
    }
    gPlacementChild=&frame;
    __try {reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x3C0070U)(animated);}
    __finally {gPlacementChild=frame.previous;}
}
bool __cdecl SuppressPlacementCycleFallback(std::uintptr_t scheduler) noexcept {
    if(!EngineThread()||gRole.load(std::memory_order_acquire)!=Role::Client||
        !execution::PlacementDedupReady()||!execution_runtime::IsOwnedPlacement(scheduler))return false;
    ++gPlacementFallbacksSuppressed;return true;
}
// Exact mode8 branches006810B1/006810E9: ESI=scheduler. Preserve every
// register/flag and floating-point value; skip BEFORE cycle virtual+34 can
// destroy the current action. The separate Host constructor remains ordered.
__declspec(naked) void PlacementCycleFirstShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push esi
        call SuppressPlacementCycleFallback
        add esp, 4
        test al, al
        fxrstor [esp]
        mov esp, ebx
        jnz suppressed
        popad
        popfd
        jmp dword ptr [gPlacementCycleFirstTrampoline]
    suppressed:
        popad
        popfd
        jmp dword ptr [gPlacementCycleContinue]
    }
}
__declspec(naked) void PlacementCycleSecondShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push esi
        call SuppressPlacementCycleFallback
        add esp, 4
        test al, al
        fxrstor [esp]
        mov esp, ebx
        jnz suppressed
        popad
        popfd
        jmp dword ptr [gPlacementCycleSecondTrampoline]
    suppressed:
        popad
        popfd
        jmp dword ptr [gPlacementCycleContinue]
    }
}

bool __cdecl SuppressPassageBackstageInterruption(const std::uintptr_t scheduler) noexcept {
    std::uintptr_t character{},cycle{},movement{};std::uint32_t mode{};std::uint8_t backstage{};
    if(!EngineThread()||!Client()||!PairGenerationStable()||
        !Read(scheduler,character)||scheduler!=character+0x2CD0U||
        !Read(character+0x2DAU,backstage)||!backstage||
        !Read(character+0x42A8U,movement)||!movement||!Read(movement+8U,mode)||mode!=5U||
        !Read(character+0x36CU,cycle)||!cycle||!RetainAuthoritativePassage(character,cycle,0x4DU))return false;
    ++gPassageBackstageInterruptionsHeld;
    return true;
}
// 00687190 establishes M+8=5 before either branch. That native mode lets
// 007C67B0 consume the passage target instead of projecting it onto the floor.
// Preserve the prefix, retained Refs, kind3 completion and native return value;
// skip only cycle.virtual+34 (004016E0) and the replacing77 constructor.
__declspec(naked) void PassageBackstageFirstShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push esi
        call SuppressPassageBackstageInterruption
        add esp, 4
        test al, al
        fxrstor [esp]
        mov esp, ebx
        jnz suppressed
        popad
        popfd
        jmp dword ptr [gPassageBackstageFirstTrampoline]
    suppressed:
        popad
        popfd
        jmp dword ptr [gPassageBackstageFirstContinue]
    }
}
__declspec(naked) void PassageBackstageSecondShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push esi
        call SuppressPassageBackstageInterruption
        add esp, 4
        test al, al
        fxrstor [esp]
        mov esp, ebx
        jnz suppressed
        popad
        popfd
        jmp dword ptr [gPassageBackstageSecondTrampoline]
    suppressed:
        popad
        popfd
        jmp dword ptr [gPassageBackstageSecondContinue]
    }
}

// ---- Native execution-cycle constructor calls (v15) ----------------------
// Native command producers feed the SAME bounded FIFO as Character events.
// A constructor's inputs are copied at entry, before it destroys the previous
// cycle. No last-cycle slot, sampled state, or retry timer can create a call.
std::array<std::atomic<void*>,kExecutionTransitionContracts.size()> gCycleCallOriginal{};
std::array<void*,kExecutionTransitionContracts.size()> gCycleCallTrampoline{};
std::array<std::atomic<void*>,4U> gPassageIntentOriginal{};
std::array<void*,4U> gPassageIntentTrampoline{};
std::array<std::atomic<void*>,6U> gSimpleIntentOriginal{};
std::array<void*,6U> gSimpleIntentTrampoline{};
std::atomic<void*> gScriptIntentOriginal{};
void* gScriptIntentTrampoline{};
std::atomic<std::uint64_t> gNativeIntentDerived{},gNativeIntentQueueRefused{};
std::atomic<std::uint32_t> gNativeIntentLastCaller{},gNativeIntentLastContract{};
bool NativeIntentDerived(const std::uint32_t id,const std::uintptr_t character,const std::uint32_t caller) noexcept {
    if(!EngineThread())return false;
    if(id==4U&&caller==0x25BE09U&&gNativeFallbackCovered&&gNativeSchedulerParent.covered&&
        gNativeSchedulerParent.character==character&&HostNativeRetirementQualified(gNativeSchedulerParent.identity.scope,
            gNativeSchedulerParent.identity.owner,character,gNativeSchedulerParent.runtime))return true;
    if(id!=7U||(caller!=0x63D17U&&caller!=0x63D24U&&caller!=0x63D3BU))return false;
    std::uintptr_t resource{},block{};std::uint32_t runtime{},part{};
    host_producer::CommandIdentity identity{};
    // The callback's actual N/B identifies an admitted Move/Stop, independent
    // of whichever scheduler request a reentrant status callback selected next.
    return Read(character+0x42A8U,resource)&&resource&&Read(resource+0x104U,block)&&block&&
        Read(character+0x50U,runtime)&&host_producer::IdentifyNativeNavigation(resource+0xC4U,block,identity,part)&&
        (identity.nativeKind==1U||identity.nativeKind==2U||identity.nativeKind==3U)&&
        HostNativeRetirementQualified(identity.scope,identity.owner,character,runtime);
}
std::atomic<std::uint64_t> gNativeIntentDropped{};std::atomic<std::uint32_t> gLastIntentRefusal{};
// Diagnostic: the scripted call (contract 20) that failed validation, raw
// parameters and the first failing clause of native_intent::Validate.
std::mutex gScriptedRefusalGate;
std::array<std::uint32_t,22> gScriptedRefusalWords{};std::uint32_t gScriptedRefusalCount{},gScriptedRefusalClause{},gScriptedRefusalCountOffset{};
std::uint32_t ScriptedClause(const native_intent::Payload& p) noexcept {
    const auto& w=p.words;
    const auto real=[&w](std::size_t i,float low,float high) noexcept {
        const auto f=std::bit_cast<float>(w[i]);return std::isfinite(f)&&f>=low&&f<=high;};
    const auto integer=[&w](std::size_t i,std::int32_t high) noexcept {
        const auto n=std::bit_cast<std::int32_t>(w[i]);return n>=-1&&n<=high;};
    if(w[35]!=0U)return 1U;
    for(std::size_t i=12;i<20;i+=2)if(w[i]==0U&&w[i+1]!=0U)return 2U;
    if(w[20]!=0U)return 20U;if(!w[21])return 21U;if(!w[22])return 22U;
    if(!integer(23,1000000))return 23U;if(!integer(24,1000000))return 24U;if(!integer(25,65535))return 25U;
    if(!real(26,0.F,64.F))return 26U;if(!real(27,0.F,60.F))return 27U;if(!real(28,0.F,60.F))return 28U;
    if(!real(30,-1.F,1000000.F))return 30U;if(!real(31,0.F,60.F))return 31U;if(!real(32,-100000.F,100000.F))return 32U;
    if((w[33]&~native_intent::kScriptedFlags)!=0U)return 33U;if(w[34]!=0U)return 34U;
    return 0U;
}
bool CaptureNativeIntentCall(const std::uint32_t id,const std::uintptr_t cycle,
    const std::uint32_t* raw,const std::uint32_t caller) noexcept {
    if(gRole.load(std::memory_order_acquire)!=Role::Host||!Installed())return false;
    std::uintptr_t character{};if(!Read(cycle+4U,character)||!character)return false;
    transactions::OwnerKey sourceOwner{};
    if(EngineThread()) {
        const auto* actor=Find(character);if(!actor)return false;
        sourceOwner={actor->entityA,actor->entityB,actor->birth,actor->characterClass,transactions::Capability::Movement,
            NativeOwnerEpoch(gCurrent.nativeGeneration,actor->birth)};
    } else {
        bool found{};PairIdentity identity{};
        for(const auto& slot:gPairSources)if(LoadPairBinding(slot,identity)&&identity.character==character&&identity.authoritativeBirth){found=true;break;}
        if(!found)return false;
        sourceOwner={identity.entityA,identity.entityB,identity.authoritativeBirth,identity.type,transactions::Capability::Movement,
            NativeOwnerEpoch(identity.generation,identity.authoritativeBirth)};
    }
    if(NativeIntentDerived(id,character,caller)){++gNativeIntentDerived;return false;}
    host_producer::Command command{};command.kind=native_intent::kKind;command.admission=1U;
    auto& w=command.intent.words;w[0]=id;
    bool valid=true;std::uint32_t refusal{};
    const auto fail=[&](std::uint32_t code) noexcept {if(valid&&!refusal)refusal=code;valid=false;};
    if(id>=8U&&id<=19U) {
        const auto& contract=kExecutionTransitionContracts[id-8U];
        for(std::size_t i=0;i<contract.arity&&i<2U;++i) {
            const auto kind=contract.args[i];const auto value=raw[i];ExecutionArgKind encoded=ExecutionArgKind::None;
            if(kind==TransitionArg::Value||kind==TransitionArg::FloatBits) {
                encoded=ExecutionArgKind::Value;w[2U+i]=value;
            } else if(kind==TransitionArg::Identity) {
                if(!value)encoded=ExecutionArgKind::Null;
                else {encoded=ExecutionArgKind::Identity;
                    if(!Read(value+0x54U,w[4U+i])||!Read(value+0x58U,w[6U+i]))fail(5U);}
            } else if(kind==TransitionArg::Vector) {
                encoded=ExecutionArgKind::Vector;
                if(!crash_guard::TryCopy(w.data()+8U,reinterpret_cast<const void*>(value),16U))fail(6U);
            } else fail(7U); // No heuristic pointer/value classification.
            w[1]|=static_cast<std::uint32_t>(encoded)<<(4U*i);
        }
    } else if(id==21U||id==native_intent::kPassageTraverseContract) {
        w[1]=static_cast<std::uint32_t>(ExecutionArgKind::Vector);
        // 00401770/004017E0 ->004013F0 read XYZ only. The fourth word is padding,
        // not an input: do not read beyond the Host's twelve-byte carrier.
        if(!crash_guard::TryCopy(w.data()+8U,reinterpret_cast<const void*>(raw[0]),12U))fail(6U);
        if(id==native_intent::kPassageTraverseContract) {
            w[1]|=static_cast<std::uint32_t>(ExecutionArgKind::Value)<<4U;
            w[3]=raw[1];
        }
    } else if(id==23U) {
        w[1]=static_cast<std::uint32_t>(ExecutionArgKind::Value);w[2]=raw[0];
    } else if(id==20U) {
        std::array<std::uint32_t,22U> parameters{};
        if(!crash_guard::TryCopy(parameters.data(),reinterpret_cast<const void*>(raw[0]),sizeof(parameters)))fail(1U);
        if(valid&&!native_intent::CaptureScripted(parameters,raw[1],command.intent)) {
            fail(2U);
            native_intent::Payload probe{};probe.words[0]=20U;
            for(std::size_t i=0;i<22U;++i)probe.words[12U+i]=parameters[i];
            probe.words[33]&=native_intent::kScriptedFlags;probe.words[34]=raw[1];
            const std::lock_guard lock(gScriptedRefusalGate);
            gScriptedRefusalWords=parameters;++gScriptedRefusalCount;
            gScriptedRefusalClause=ScriptedClause(probe);gScriptedRefusalCountOffset=raw[1];
        }
        if(valid)for(std::size_t i=12U;i<20U;i+=2U) {
            if(!w[i]&&!w[i+1U])continue;
            if(!EngineThread()||!gScriptReferences){fail(3U);break;}
            bool fault{};
            auto reference=gScriptReferences->ResolveRuntimeReference({w[i],w[i+1U]},fault);
            if(fault){fail(4U);break;}
            if(reference)w[1]|=1U<<((i-12U)/2U);
        }
    }
    if(valid&&(!native_intent::Validate(command.intent)||native_intent::Empty(command.intent)))fail(8U);
    if(!valid){++gCycleCallsRefused;gLastRefusedCycleCall.store(NativeIntentContract(id).entry,std::memory_order_relaxed);}
    MotionTraceSample captured{};captured.type=5U;captured.role=1U;captured.utcMs=UtcMs();captured.sampleTick=GetTickCount64();
    transactions::Record evidence{};evidence.nativeKind=native_intent::kKind;evidence.intent=command.intent;evidence.owner=sourceOwner;
    static_cast<void>(host_producer::HostScope(evidence.scope));
    FillCommand(evidence,captured);captured.intentCaller=caller;captured.outcome=valid?0U:1U;captured.waitReason=refusal;
    RecordMotionTrace(captured);
    // A native call this build cannot represent is ONE Host instruction the
    // Client will not replay. Publishing it as a refusal closed the whole
    // command stream (every AI froze for the rest of the session). Drop it.
    if(!valid){++gNativeIntentDropped;gLastIntentRefusal.store(refusal|(id<<8U),std::memory_order_relaxed);return false;}
    const bool queued=QueueHostLifecycleEvent(character,command,caller);
    if(queued) {++gCycleCallsCaptured;TraceHostLifecycle(6U,character,command,id,caller);}
    else {++gNativeIntentQueueRefused;TraceHostLifecycle(1U,character,command,id,caller);}
    gNativeIntentLastCaller.store(caller,std::memory_order_relaxed);gNativeIntentLastContract.store(id,std::memory_order_relaxed);
    if(queued&&EngineThread())DrainHostLifecycleEvents();
    return queued&&valid;
}
void TraceNativeIntentReturn(std::uint32_t id,std::uintptr_t character,std::uint32_t caller) noexcept {
    if(!character||gRole.load(std::memory_order_acquire)!=Role::Host||!Installed())return;
    PairIdentity identity{};if(PublishedPairSource(character,identity)==kActors)return;
    MotionTraceSample sample{};sample.type=6;FillMotion(character,sample);
    sample.intentContract=id;sample.intentCaller=caller;sample.entityA=identity.entityA;
    sample.entityB=identity.entityB;sample.birth=identity.authoritativeBirth;
    RecordMotionTrace(sample);
}
template<std::size_t I> void __fastcall SimpleIntentDetour(void* cycle,void*) noexcept {
    const auto original=reinterpret_cast<void(__thiscall*)(void*)>(gSimpleIntentOriginal[I].load(std::memory_order_acquire));
    if(!original)return;
    if constexpr(I==3U) {
        if(gAuthoritativeStanceFallbackCycle==reinterpret_cast<std::uintptr_t>(cycle)&&
            reinterpret_cast<std::uintptr_t>(_ReturnAddress())==gBase+0x25BE09U)return;
    }
    const std::uint32_t raw[2]{};
    static_cast<void>(CaptureNativeIntentCall(static_cast<std::uint32_t>(I+1U),reinterpret_cast<std::uintptr_t>(cycle),raw,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase)));
    original(cycle);
}
void __fastcall ScriptIntentDetour(void* cycle,void*,const std::uint32_t* parameters,const std::uint32_t countOffset) noexcept {
    const auto original=reinterpret_cast<void(__thiscall*)(void*,const std::uint32_t*,std::uint32_t)>(gScriptIntentOriginal.load(std::memory_order_acquire));
    if(!original)return;
    const std::uint32_t raw[2]{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(parameters)),countOffset};
    static_cast<void>(CaptureNativeIntentCall(20U,reinterpret_cast<std::uintptr_t>(cycle),raw,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase)));
    original(cycle,parameters,countOffset);
}
template<std::size_t I> void __fastcall CycleCallDetour1(void* cycle,void*,std::uint32_t a0) noexcept {
    const auto original=reinterpret_cast<void(__thiscall*)(void*,std::uint32_t)>(gCycleCallOriginal[I].load(std::memory_order_acquire));
    if(!original)return;
    const std::uint32_t raw[2]{a0,0U};
    static_cast<void>(CaptureNativeIntentCall(static_cast<std::uint32_t>(I+8U),reinterpret_cast<std::uintptr_t>(cycle),raw,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase)));
    original(cycle,a0);
}
template<std::size_t I> void __fastcall CycleCallDetour2(void* cycle,void*,std::uint32_t a0,std::uint32_t a1) noexcept {
    const auto original=reinterpret_cast<void(__thiscall*)(void*,std::uint32_t,std::uint32_t)>(gCycleCallOriginal[I].load(std::memory_order_acquire));
    if(!original)return;
    const std::uint32_t raw[2]{a0,a1};
    static_cast<void>(CaptureNativeIntentCall(static_cast<std::uint32_t>(I+8U),reinterpret_cast<std::uintptr_t>(cycle),raw,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase)));
    original(cycle,a0,a1);
}
template<std::size_t I> void* CycleCallDetour() noexcept {
    if constexpr(kExecutionTransitionContracts[I].arity==1U) return reinterpret_cast<void*>(&CycleCallDetour1<I>);
    else return reinterpret_cast<void*>(&CycleCallDetour2<I>);
}
template<std::size_t I> void __fastcall PassageIntentDetour(void* cycle,void*,const std::uint32_t argument) noexcept {
    const auto original=reinterpret_cast<void(__thiscall*)(void*,std::uint32_t)>(gPassageIntentOriginal[I].load(std::memory_order_acquire));
    if(!original)return;
    const std::uint32_t raw[2]{argument,0U};
    static_cast<void>(CaptureNativeIntentCall(static_cast<std::uint32_t>(21U+I),reinterpret_cast<std::uintptr_t>(cycle),raw,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase)));
    original(cycle,argument);
}
void __fastcall PassageIdleIntentDetour(void* cycle,void*) noexcept {
    const auto original=reinterpret_cast<void(__thiscall*)(void*)>(gPassageIntentOriginal[1].load(std::memory_order_acquire));
    if(!original)return;
    const std::uint32_t raw[2]{};
    static_cast<void>(CaptureNativeIntentCall(22U,reinterpret_cast<std::uintptr_t>(cycle),raw,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase)));
    original(cycle);
}
void __fastcall PassageTraverseIntentDetour(void* cycle,void*,const float* destination,
    const std::uint32_t selector) noexcept {
    const auto original=reinterpret_cast<void(__thiscall*)(void*,const float*,std::uint32_t)>(
        gPassageIntentOriginal[3U].load(std::memory_order_acquire));
    if(!original)return;
    const std::uint32_t raw[2]{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(destination)),selector};
    static_cast<void>(CaptureNativeIntentCall(native_intent::kPassageTraverseContract,
        reinterpret_cast<std::uintptr_t>(cycle),raw,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())-gBase)));
    original(cycle,destination,selector);
}
template<std::size_t... I> bool HookCycleCalls(HookManager& hooks,std::string& error,std::index_sequence<I...>) {
    static const std::array<void*,sizeof...(I)> detours{CycleCallDetour<I>()...};
    for(std::size_t i=0;i<kExecutionTransitionContracts.size();++i) {
        const auto& contract=kExecutionTransitionContracts[i];
        const auto name="aisync native cycle constructor "+std::to_string(contract.state);
        bool ok{};
        switch(contract.prologue) {
        case TransitionPrologue::Simple:
            ok=Hook(hooks,name.c_str(),contract.rva,detours[i],{0x56,0x8B,0xF1,0x8A,0x46,0x1A},&gCycleCallTrampoline[i],&gCycleCallOriginal[i],error);break;
        case TransitionPrologue::Seh:
            ok=Hook(hooks,name.c_str(),contract.rva,detours[i],{0x6A,0xFF,0x68,0,0,0,0},&gCycleCallTrampoline[i],&gCycleCallOriginal[i],error,contract.sehHandlerRva);break;
        case TransitionPrologue::Frame:
            ok=Hook(hooks,name.c_str(),contract.rva,detours[i],{0x55,0x8B,0xEC,0x83,0xE4,0xF0},&gCycleCallTrampoline[i],&gCycleCallOriginal[i],error);break;
        }
        if(!ok)return false;
    }
    return true;
}
bool HookNativePassageAuthority(HookManager& hooks,std::string& error) {
    // No absolute operands in these instruction ranges. The two eight-byte
    // prefixes relocate verbatim; continuations are rebased with the image.
    if(!MatchRetailBytes(0x28722DU,{0xC7,0x40,0x08,0x05,0x00,0x00,0x00})||
        !MatchRetailBytes(0x28739EU,{0x8B,0x01,0x8B,0x50,0x34,0xFF,0xD2})||
        !MatchRetailBytes(0x2873ADU,{0xE8,0x0E,0x36,0xFC,0xFF})||
        !MatchRetailBytes(0x2873B2U,{0x8B,0xCE,0xE8,0x47,0xF9,0xFF,0xFF})||
        !MatchRetailBytes(0x2873D2U,{0x8B,0x01,0x8B,0x50,0x34,0xFF,0xD2})||
        !MatchRetailBytes(0x2873E1U,{0xE8,0xDA,0x35,0xFC,0xFF})||
        !MatchRetailBytes(0x2873E6U,{0x8B,0x74,0x24,0x2C,0x85,0xF6})) {
        error="aisync passage ownership: certified backstage fallback/continuation mismatch";
        return false;
    }
    gPassageBackstageFirstContinue=reinterpret_cast<void*>(gBase+0x2873B2U);
    gPassageBackstageSecondContinue=reinterpret_cast<void*>(gBase+0x2873E6U);
    return Hook(hooks,"AISync passage first backstage authority",0x287396U,reinterpret_cast<void*>(&PassageBackstageFirstShim),
        {0x8B,0x06,0x8B,0x88,0x6C,0x03,0x00,0x00},&gPassageBackstageFirstTrampoline,&gPassageBackstageFirstOriginal,error)&&
        Hook(hooks,"AISync passage second backstage authority",0x2873CAU,reinterpret_cast<void*>(&PassageBackstageSecondShim),
        {0x8B,0x06,0x8B,0x88,0x6C,0x03,0x00,0x00},&gPassageBackstageSecondTrampoline,&gPassageBackstageSecondOriginal,error);
}
bool HookExecutionCycleCalls(HookManager& hooks,std::string& error) {
    static const std::array<void*,6U> detours{reinterpret_cast<void*>(&SimpleIntentDetour<0>),
        reinterpret_cast<void*>(&SimpleIntentDetour<1>),reinterpret_cast<void*>(&SimpleIntentDetour<2>),
        reinterpret_cast<void*>(&SimpleIntentDetour<3>),reinterpret_cast<void*>(&SimpleIntentDetour<4>),reinterpret_cast<void*>(&SimpleIntentDetour<5>)};
    for(std::size_t i=0;i<detours.size();++i)
        if(!Hook(hooks,"aisync native zero argument intention",kExecutionTransitions[i].entryRva,detours[i],
            {0x56,0x8B,0xF1,0x8A,0x46,0x1A},&gSimpleIntentTrampoline[i],&gSimpleIntentOriginal[i],error))return false;
    const std::array<void*,4U> passageDetours{reinterpret_cast<void*>(&PassageIntentDetour<0U>),
        reinterpret_cast<void*>(&PassageIdleIntentDetour),reinterpret_cast<void*>(&PassageIntentDetour<2U>),
        reinterpret_cast<void*>(&PassageTraverseIntentDetour)};
    for(std::size_t i=0;i<kPassageIntents.size();++i)
        if(!Hook(hooks,"aisync native passage intention",kPassageIntents[i].entry,passageDetours[i],
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},&gPassageIntentTrampoline[i],&gPassageIntentOriginal[i],error,0xD23FC1U))return false;
    CallRedirectRequest fallback{"AISync native control-to-stance fallback",reinterpret_cast<void*>(gBase+0x2799DFU),
        reinterpret_cast<void*>(gBase+0x25BDD0U),reinterpret_cast<void*>(&ControlStanceFallbackDetour)};
    return HookNativePassageAuthority(hooks,error)&&hooks.InstallCallRedirect(fallback,error)&&Hook(hooks,"aisync native scripted intention",0x2579F0U,reinterpret_cast<void*>(&ScriptIntentDetour),
        {0x56,0x8B,0xF1,0x8A,0x46,0x1A},&gScriptIntentTrampoline,&gScriptIntentOriginal,error)&&
        HookCycleCalls(hooks,error,std::make_index_sequence<kExecutionTransitionContracts.size()>{});
}
bool HookLifecycleEvents(HookManager& hooks,std::string& error) {
    execution::SetPlacementDedupReady(false);
    gPlacementCycleContinue=reinterpret_cast<void*>(gBase+0x281105U);
    const auto child=[&](const char* name,std::uintptr_t site,std::uintptr_t target,void* detour) {
        return hooks.InstallCallRedirect({name,reinterpret_cast<void*>(gBase+site),reinterpret_cast<void*>(gBase+target),detour},error);
    };
    return child("AISync kind8 forced entry",0x28106BU,0x3C63B0U,reinterpret_cast<void*>(&PlacementChildDetour<true>))&&
        child("AISync kind8 normal entry",0x281087U,0x3C6330U,reinterpret_cast<void*>(&PlacementChildDetour<false>))&&
        child("AISync kind8 forced update",0x287E78U,0x3C63B0U,reinterpret_cast<void*>(&PlacementChildDetour<true>))&&
        child("AISync kind8 normal update",0x287EA6U,0x3C6330U,reinterpret_cast<void*>(&PlacementChildDetour<false>))&&
        child("AISync kind8 frontstage entry",0x28107BU,0x3C0070U,reinterpret_cast<void*>(&PlacementFrontstageChildDetour))&&
        child("AISync kind8 frontstage update",0x287E88U,0x3C0070U,reinterpret_cast<void*>(&PlacementFrontstageChildDetour))&&
        Hook(hooks,"AISync kind8 first cycle authority",0x2810B1U,reinterpret_cast<void*>(&PlacementCycleFirstShim),
            {0x8B,0x06,0x8B,0x88,0x6C,0x03,0x00,0x00},&gPlacementCycleFirstTrampoline,&gPlacementCycleFirstOriginal,error)&&
        Hook(hooks,"AISync kind8 second cycle authority",0x2810E9U,reinterpret_cast<void*>(&PlacementCycleSecondShim),
            {0x8B,0x06,0x8B,0x88,0x6C,0x03,0x00,0x00},&gPlacementCycleSecondTrampoline,&gPlacementCycleSecondOriginal,error)&&
        Hook(hooks,"aisync native forced placement event",0x3C2710U,reinterpret_cast<void*>(&ForcedRelocateDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0},&gForcedRelocateTrampoline,&gForcedRelocateOriginal,error)&&
        Hook(hooks,"aisync native backstage event",0x38B140U,reinterpret_cast<void*>(&BackstageEventDetour),
            {0x53,0x8B,0x5C,0x24,0x08},&gBackstageEventTrampoline,&gBackstageEventOriginal,error)&&
        Hook(hooks,"aisync native frontstage event",0x3C0220U,reinterpret_cast<void*>(&FrontstageEventDetour),
            {0x64,0xA1,0x00,0x00,0x00,0x00},&gFrontstageEventTrampoline,&gFrontstageEventOriginal,error);
}
bool ValidateCharacterLifecycleAbi(std::string& error) {
    if (!MatchRetailBytes(0x003C63B0U,{0x53,0x8B,0x5C,0x24,0x08,0x55,0x8B,0x6C,0x24,0x14,0x57,0x8B,0xF9}) ||
        !MatchRetailBytes(0x003C640FU,{0x6A,0x01}) || !MatchRetailBytes(0x003C6423U,{0xC2,0x0C,0x00}) ||
        !MatchRetailBytes(0x003C2904U,{0xC2,0x10,0x00}) || !MatchRetailBytes(0x003C2946U,{0xC2,0x10,0x00}) ||
        !MatchRetailBytes(0x0038B140U,
            {0x53,0x8B,0x5C,0x24,0x08,0x56,0x8B,0xF1,0x8D,0x8E,0xF0,0x03,0x00,0x00,
             0x88,0x9E,0xDA,0x02,0x00,0x00,0x88,0x9E,0xD9,0x02,0x00,0x00}) ||
        !MatchRetailBytes(0x0038B15EU, {0x53,0xE8,0x9C,0x69,0xF0,0xFF}) ||
        !MatchRetailBytes(0x0038B177U, {0x53,0xE8,0xF3,0x76,0xFF,0xFF}) ||
        !MatchRetailBytes(0x0038B17DU, {0x5E,0x5B,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x003C2A65U,
            {0x53,0x8B,0xCE,0xE8,0xD3,0x86,0xFC,0xFF,0x8B,0x06,0x8B,0x50,0x1C,
             0x8B,0xCE,0xFF,0xD2,0x8B,0xC8,0xE8,0xA3,0xD7,0xFF,0xFF}) ||
        !MatchRetailBytes(0x003C0238U, {0x53,0x56,0x8B,0xF1,0x33,0xDB,0x38,0x9E,0xF8,0x02,0x00,0x00}) ||
        !MatchRetailBytes(0x003C03A7U, {0x83,0xC4,0x28,0xC3}) ||
        !MatchRetailBytes(0x003C6330U,
            {0x53,0x8B,0x5C,0x24,0x08,0x55,0x8B,0x6C,0x24,0x14,0x57,0x8B,0xF9}) ||
        !MatchRetailBytes(0x003C6389U,
            {0xF3,0x0F,0x10,0x44,0x24,0x14,0x6A,0x00,0x55,0x51,0xF3,0x0F,0x11,0x04,0x24,
             0x53,0x8B,0xCF,0xE8,0x70,0xC3,0xFF,0xFF,0x5F,0x5D,0x5B,0xC2,0x0C,0x00})) {
        error = "aisync Character lifecycle: certified backstage/frontstage/placement ABI mismatch";
        return false;
    }
    return true;
}
bool HookParticipantSenseSignals(HookManager& hooks, std::string& error) {
    // FUN_00731060 and FUN_00730C30 are ECX + one argument, RET 4.
    // Their instruction-aligned prefixes contain no relative/ASLR operands.
    if (!Hook(hooks, "aisync_sense_event_scope", 0x00331060U,
            reinterpret_cast<void*>(&SenseEventDetour), {0x64,0xA1,0x00,0x00,0x00,0x00},
            &gSenseEventTrampoline, &gSenseEventOriginal, error) ||
        !Hook(hooks, "aisync_sound_event_scope", 0x00330C30U,
            reinterpret_cast<void*>(&SoundEventDetour), {0x83,0xEC,0x08,0x53,0x8B,0x5C,0x24,0x10},
            &gSoundEventTrampoline, &gSoundEventOriginal, error)) return false;
    // Every resolver CALL inside these two certified dispatchers, including
    // positive/lost sight and sound. Never redirect the global resolver itself.
    constexpr std::array<std::uintptr_t, 16U> calls{
        0x003310A5U, 0x0033112BU, 0x003311C8U, 0x003313CEU,
        0x0033141CU, 0x003314C2U, 0x00331551U, 0x003315D5U,
        0x00331657U, 0x00331705U, 0x0033177CU, 0x00331828U,
        0x00331918U, 0x00330C5AU, 0x00330E57U, 0x00330ECCU};
    for (const auto rva : calls) {
        CallRedirectRequest request;
        request.name = "aisync_participant_sense_identity_" + std::to_string(rva);
        request.callSite = reinterpret_cast<void*>(gBase + rva);
        request.expectedTarget = reinterpret_cast<void*>(gBase + 0x002545C0U);
        request.detour = reinterpret_cast<void*>(&SenseCharacterResolverDetour);
        if (!hooks.InstallCallRedirect(request, error)) return false;
    }
    return true;
}

bool HookImpactShooterIdentity(HookManager& hooks, std::string& error) {
    // Every shooter resolver CALL in both certified Character impact
    // receivers (007CDCF0, 007CE020); never the global resolver itself.
    constexpr std::array<std::uintptr_t, 3U> calls{0x003CDD0CU, 0x003CDFF0U, 0x003CE03CU};
    for (const auto rva : calls) {
        CallRedirectRequest request;
        request.name = "aisync_impact_shooter_identity_" + std::to_string(rva);
        request.callSite = reinterpret_cast<void*>(gBase + rva);
        request.expectedTarget = reinterpret_cast<void*>(gBase + kCharacterResolverRva);
        request.detour = reinterpret_cast<void*>(&ImpactShooterResolverDetour);
        if (!hooks.InstallCallRedirect(request, error)) return false;
    }
    return true;
}

bool HookParticipantPairedIdentity(HookManager& hooks, std::string& error) {
    // The shared initializer is used by both certified paired cycle families.
    // Complete prologue instructions have no relative or HIGHLOW operands.
    if (!MatchRetailBytes(0x0025FBBCU, {0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x0025FC6FU, {0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x0025FC7CU, {0xC2,0x08,0x00})) {
        error = "aisync paired initialization: certified ECX/two arguments/RET 8 bytes mismatch";
        return false;
    }
    if (!Hook(hooks, "aisync_paired_initialize_scope", 0x0025FB80U,
            reinterpret_cast<void*>(&PairedInitializeDetour), {0x55,0x8B,0x6C,0x24,0x08,0x56},
            &gPairedInitializeTrampoline, &gPairedInitializeOriginal, error)) return false;
    CallRedirectRequest character;
    character.name = "aisync_paired_character_identity";
    character.callSite = reinterpret_cast<void*>(gBase + 0x0025FB98U);
    character.expectedTarget = reinterpret_cast<void*>(gBase + 0x002544E0U);
    character.detour = reinterpret_cast<void*>(&PairedCharacterResolverDetour);
    if (!hooks.InstallCallRedirect(character, error)) return false;
    CallRedirectRequest animated;
    animated.name = "aisync_paired_animated_identity";
    animated.callSite = reinterpret_cast<void*>(gBase + 0x0025FC1EU);
    animated.expectedTarget = reinterpret_cast<void*>(gBase + 0x002545C0U);
    animated.detour = reinterpret_cast<void*>(&PairedAnimatedResolverDetour);
    return hooks.InstallCallRedirect(animated, error);
}

bool ValidatePairedSchedulerAbi(std::string& error) {
    // ECX is the current cycle in both class-dispatching Idle entries. They
    // differ in stack arity, including their early/fallback return paths.
    if (!MatchRetailBytes(0x0025FC98U, {0x56,0x8B,0xF1,0x8B,0x4E,0x04}) ||
        !MatchRetailBytes(0x0025FD39U, {0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x0025FD52U, {0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x0025FD78U, {0x56,0x8B,0xF1,0x8B,0x4E,0x04}) ||
        !MatchRetailBytes(0x0025FE10U, {0xC3}) ||
        !MatchRetailBytes(0x0025FE27U, {0xC3})) {
        error = "AISync paired scheduler Idle dispatch ABI mismatch";
        return false;
    }
    return true;
}
bool HookPairedGraphTrace(HookManager& hooks, std::string& error) {
    if (!MatchRetailBytes(0x00264AC8U, {0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00265A25U, {0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00255A5BU, {0xC3}) ||
        !MatchRetailBytes(0x00255A69U, {0xC6,0x86,0xD8,0x00,0x00,0x00,0x01}) ||
        !MatchRetailBytes(0x00255B24U, {0xC3})) {
        error = "AISync paired graph callback ABI mismatch";
        return false;
    }
    // 00664A10: PUSH ESI; PUSH EDI; PUSH string (PE HIGHLOW). Complete
    // prefix with its loaded ASLR operand. Both event handlers have RET 4.
    HookRequest source;
    source.name = "aisync_paired_source_graph_events";
    source.target = reinterpret_cast<void*>(gBase + 0x00264A10U);
    source.detour = reinterpret_cast<void*>(&SourceGraphEventDetour);
    source.patchLength = 7U;
    source.expected.bytes = {0x56,0x57,0x68,0,0,0,0};
    const auto operand = static_cast<std::uint32_t>(gBase + 0x00EAE984U);
    std::memcpy(source.expected.bytes.data() + 3U, &operand, sizeof(operand));
    source.expected.mask = "xxxxxxx";
    source.relocationSafePrefix = true;
    source.trampolineOut = &gSourceGraphEventTrampoline;
    source.publishedTrampolineOut = &gSourceGraphEventOriginal;
    return hooks.Install(source, error) &&
        Hook(hooks, "aisync_paired_victim_graph_events", 0x002658E0U,
            reinterpret_cast<void*>(&VictimGraphEventDetour), {0x83,0xEC,0x1C,0x56,0x57},
            &gVictimGraphEventTrampoline, &gVictimGraphEventOriginal, error) &&
        Hook(hooks, "aisync_paired_source_native_finish", 0x002559A0U,
            reinterpret_cast<void*>(&SourceGraphFinishDetour), {0x83,0xEC,0x10,0x56,0x8B,0xF1},
            &gSourceGraphFinishTrampoline, &gSourceGraphFinishOriginal, error) &&
        Hook(hooks, "aisync_paired_victim_graph_finish", 0x00255A60U,
            reinterpret_cast<void*>(&VictimGraphFinishDetour),
            {0x56,0x8B,0xF1,0x8B,0x8E,0xB0,0x00,0x00,0x00},
            &gVictimGraphFinishTrampoline, &gVictimGraphFinishOriginal, error);
}
bool HookPairedVictimGuards(HookManager& hooks, std::string& error) {
    // Both the ABI and unsafe source-animation load are independently checked
    // against the certified executable before installing these two guards.
    if (!MatchRetailBytes(0x0025C49DU,
            {0x8B,0x44,0x24,0x24,0x8B,0x4C,0x24,0x20,0x8B,0x54,0x24,0x1C,0x50,0x51,0x52}) ||
        !MatchRetailBytes(0x0025C4DBU, {0xC2,0x0C,0x00}) ||
        !MatchRetailBytes(0x00275FA3U,
            {0x8B,0x86,0xB4,0x00,0x00,0x00,0x3B,0xC3,0x74,0x1D,0x8B,0x40,0x08}) ||
        !MatchRetailBytes(0x002763DFU, {0x5F,0x5E,0x5B,0x81,0xC4,0x88,0x00,0x00,0x00,0xC3})) {
        error = "aisync paired victim: certified RET 0C/ECX-only initializer bytes mismatch";
        return false;
    }
    if (!Hook(hooks, "aisync_paired_victim_transition", 0x0025C450U,
            reinterpret_cast<void*>(&PairedVictimTransitionDetour),
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedVictimTransitionTrampoline, &gPairedVictimTransitionOriginal, error,
            0x00D23FC1U)) return false;
    // MOVSD xmm0,[absolute time]; SUB ESP,88. The complete HIGHLOW operand
    // stays valid in the trampoline and is matched at its loaded ASLR address.
    MaskedBytes bytes;
    bytes.bytes = {0xF2,0x0F,0x10,0x05,0x00,0x00,0x00,0x00,
                   0x81,0xEC,0x88,0x00,0x00,0x00};
    const auto absolute = static_cast<std::uint32_t>(gBase + 0x012410A8U);
    std::memcpy(bytes.bytes.data() + 4U, &absolute, sizeof(absolute));
    bytes.mask.assign(bytes.bytes.size(), 'x');
    HookRequest request;
    request.name = "aisync_paired_victim_initialize";
    request.target = reinterpret_cast<void*>(gBase + 0x00275F10U);
    request.detour = reinterpret_cast<void*>(&PairedVictimInitializeDetour);
    request.patchLength = bytes.bytes.size(); request.expected = bytes;
    request.relocationSafePrefix = true;
    request.trampolineOut = &gPairedVictimInitializeTrampoline;
    request.publishedTrampolineOut = &gPairedVictimInitializeOriginal;
    if (!hooks.Install(request, error)) return false;
    if (!MatchRetailBytes(0x00378F2BU,
            {0x8B,0x46,0x30,0x8B,0x16,0x50,0x8D,0x4E,0x10,0x51,0x8B,0x8F,0x6C,0x03,0x00,0x00,0x52}) ||
        !MatchRetailBytes(0x00378F41U, {0x5F,0x5E,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00262C51U,
            {0x8B,0x4E,0x44,0x8B,0x11,0x8B,0x52,0x2C,0x8D,0x84,0x24,0x90,0x00,0x00,0x00,0x50}) ||
        !MatchRetailBytes(0x00262C6AU, {0x8B,0x4E,0x4C,0x8B,0x11,0x8B,0x42,0x04})) {
        error = "aisync native victim message: certified packet/dispatcher/RET 4 bytes mismatch";
        return false;
    }
    if (!Hook(hooks, "aisync_paired_victim_event", 0x00378EF0U,
            reinterpret_cast<void*>(&PairedVictimEventDetour), {0x56,0x57,0x8B,0x7C,0x24,0x0C},
            &gPairedVictimEventTrampoline, &gPairedVictimEventOriginal, error)) return false;
    if (!Hook(hooks, "aisync_paired_victim_prepare", 0x00262670U,
            reinterpret_cast<void*>(&PairedVictimPrepareDetour), {0x55,0x8B,0xEC,0x83,0xE4,0xF0},
            &gPairedVictimPrepareTrampoline, &gPairedVictimPrepareOriginal, error)) return false;
    gPairedVictimSubmitContinue = reinterpret_cast<void*>(gBase + 0x00262C6AU);
    if (!Hook(hooks, "aisync_paired_victim_native_message", 0x00262C61U,
            reinterpret_cast<void*>(&PairedVictimSubmitShim), {0x89,0x9C,0x24,0xF0,0x00,0x00,0x00,0xFF,0xD2},
            &gPairedVictimSubmitTrampoline, &gPairedVictimSubmitPublished, error)) return false;
    if (!MatchRetailBytes(0x00275F5EU, {0x8B,0x4E,0x04}) ||
        !MatchRetailBytes(0x0025B6B7U, {0x8B,0x4E,0x04,0x81,0xC1,0x80,0x02,0x00,0x00}) ||
        !MatchRetailBytes(0x0025B207U, {0x8D,0x88,0x80,0x02,0x00,0x00})) {
        error = "aisync paired availability: ESI owner/ECX logical-character mismatch";
        return false;
    }
    if (!MatchRetailBytes(0x0025B04FU, {0xFE,0x46,0x19}) ||
        !MatchRetailBytes(0x002627B1U, {0xE8,0x4A,0xA1,0x16,0x00}) ||
        !MatchRetailBytes(0x002627B6U, {0x8B,0x46,0x44,0x0F,0xB7,0x80,0x3C,0x03,0x00,0x00}) ||
        !MatchRetailBytes(0x00255B39U, {0x8B,0xF1}) ||
        !MatchRetailBytes(0x00255BCCU, {0x80,0x79,0x1A,0x4E,0x75,0x0C,0x8B,0x56,0x04,0x53,0x52}) ||
        !MatchRetailBytes(0x00255BDCU, {0xEB,0x06,0x89,0xAE,0xB0,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x0024E6D8U, {0x8B,0x43,0x4C}) ||
        !MatchRetailBytes(0x0024E6DFU, {0x83,0x78,0x04,0x0B}) ||
        !MatchRetailBytes(0x0024E8ABU, {0xC2,0x08,0x00})) {
        error = "aisync victim convergence: certified ESI/source animation/RET 8 bytes mismatch";
        return false;
    }
    CallRedirectRequest convergence;
    convergence.name = "aisync_paired_victim_convergence";
    convergence.callSite = reinterpret_cast<void*>(gBase + 0x00255BD7U);
    convergence.expectedTarget = reinterpret_cast<void*>(gBase + 0x0024E6C0U);
    convergence.detour = reinterpret_cast<void*>(&PairedVictimConvergenceShim);
    if (!hooks.InstallCallRedirect(convergence, error)) return false;
    constexpr std::array<std::uintptr_t, 3U> availabilityCalls{0x00275F75U, 0x0025B6C0U, 0x0025B20DU};
    for (const auto rva : availabilityCalls) {
        CallRedirectRequest call;
        call.name = "aisync_paired_victim_availability_" + std::to_string(rva);
        call.callSite = reinterpret_cast<void*>(gBase + rva);
        call.expectedTarget = reinterpret_cast<void*>(gBase + 0x0038B330U);
        call.detour = reinterpret_cast<void*>(&PairedVictimAvailabilityShim);
        if (!hooks.InstallCallRedirect(call, error)) return false;
    }
    return true;
}

bool HookParticipantEligibility(HookManager& hooks, std::string& error) {
    // Fixed, independently certified CALL sites. Never scan every byte of
    // .text or change all callers of a shared physical-state predicate.
    if (!MatchRetailBytes(0x0038B380U, {
            0x8BU,0x09U,0x8BU,0x01U,0x8BU,0x50U,0x1CU,0x56U,0xFFU,0xD2U,0x8BU,0xF0U,
            0x80U,0xBEU,0xF4U,0x2CU,0x00U,0x00U,0x00U,0x74U,0x1CU,0x8DU,0x8EU,0xD0U,
            0x2CU,0x00U,0x00U,0xE8U,0xC0U,0xE4U,0xEEU,0xFFU,0x84U,0xC0U,0x74U,0x09U,
            0x83U,0xBEU,0x90U,0x2FU,0x00U,0x00U,0x03U,0x74U,0x04U,0x33U,0xC0U,0x5EU,
            0xC3U,0xB8U,0x01U,0x00U,0x00U,0x00U,0x5EU,0xC3U}) ||
        !MatchRetailBytes(0x0032ACA3U,
            {0x8D,0x8D,0x80,0x02,0x00,0x00,0xE8,0xD2,0x06,0x06,0x00,0x84,0xC0,0x0F,0x85,0x1C,0x01,0x00,0x00}) ||
        !MatchRetailBytes(0x002CDB49U,
            {0x8D,0x89,0x80,0x02,0x00,0x00,0xE8,0x2C,0xD8,0x0B,0x00,0x84,0xC0,0x75,0x1C})) {
        error = "aisync participant eligibility: certified target-validation bytes mismatch";
        return false;
    }
    CallRedirectRequest candidate;
    candidate.name = "aisync_candidate_target_eligibility";
    candidate.callSite = reinterpret_cast<void*>(gBase + 0x0032ACA9U);
    candidate.expectedTarget = reinterpret_cast<void*>(gBase + 0x0038B380U);
    candidate.detour = reinterpret_cast<void*>(&CandidateEligibilityDetour);
    if (!hooks.InstallCallRedirect(candidate, error)) return false;
    CallRedirectRequest record;
    record.name = "aisync_record_target_eligibility";
    record.callSite = reinterpret_cast<void*>(gBase + 0x002CDB4FU);
    record.expectedTarget = reinterpret_cast<void*>(gBase + 0x0038B380U);
    record.detour = reinterpret_cast<void*>(&RecordEligibilityDetour);
    return hooks.InstallCallRedirect(record, error);
}

bool HookThePlayerResolver(HookManager& hooks, std::string& error) {
    // FUN_0078BAF0 starts with A1 <GlobalStateRoot>. The complete displaced
    // instruction carries a PE HIGHLOW absolute already relocated in memory,
    // so copying those five bytes to the trampoline is safe.
    MaskedBytes bytes;
    bytes.bytes = {0xA1U, 0U, 0U, 0U, 0U};
    const auto absolute = static_cast<std::uint32_t>(gBase + kInteractionManagerGlobalRva);
    std::memcpy(bytes.bytes.data() + 1U, &absolute, sizeof(absolute));
    bytes.mask.assign(bytes.bytes.size(), 'x');

    HookRequest request;
    request.name = "aisync_ai_player_context";
    request.target = reinterpret_cast<void*>(gBase + 0x0038BAF0U);
    request.detour = reinterpret_cast<void*>(&ThePlayerDetour);
    request.patchLength = 5U;
    request.expected = bytes;
    request.relocationSafePrefix = true;
    request.trampolineOut = &gThePlayerTrampoline;
    request.publishedTrampolineOut = &gThePlayerOriginal;
    return hooks.Install(request, error);
}

} // namespace

bool AnimationHumanPair(const std::uintptr_t cycle, const std::uintptr_t victim) noexcept {
    return PlayerRuntimeBoundary(cycle, victim) != 0;
}
bool AnimationManagedSource(const std::uintptr_t character) noexcept {
    if (gRole.load(std::memory_order_acquire) == Role::Retail) return false;
    PairIdentity identity{};
    return PublishedPairSource(character, identity) != kActors;
}
bool AnimationCanReplayScripted(const std::uintptr_t character) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Client || !Stable() || !EngineThread()) return false;
    const auto* actor = Find(character);
    std::uint32_t runtimeId{};
    return actor != nullptr && !actor->faulted && !actor->duplicate &&
        Read(character + 0x50U, runtimeId) && runtimeId == actor->runtimeId;
}


bool Install(const std::uintptr_t base, HookManager& hooks, LevelGenerationTracker& generations,
    GameThreadDispatcher& dispatcher, std::string& error, WorldEntityRegistry* scriptReferences) {
    if (base < 0x10000U || !hooks.MutationAllowed() ||
        !character_direct_step_runtime::Installed()) {
        error = "AISync native transactions require the certified retail x86 executable and Character runtime.";
        return false;
    }
    gBase = base; gGenerations = &generations; gDispatcher = &dispatcher;gScriptReferences=scriptReferences;
    if (!ValidateRoleTaskCleanupAbi(error) || !ValidateCharacterLifecycleAbi(error) ||
        !ValidatePairedSchedulerAbi(error) || !navigation::Initialize(base, error) ||
        !health::Initialize(base, error) || !combat::Initialize(base, error)) return false;
    health::SetNativeDamageGuard(&AllowLocalNativeDamage);
    health::SetPairedDamagePredicate(&PairedDamageAlreadyCarried);
    combat::SetNpcPredicate(&AnimationManagedSource);
    gBranchExit = reinterpret_cast<void*>(base + 0x003344D4U);
    const auto first = hooks.InstalledCount();
    // Complete instructions; no relative CALL/Jcc in a prefix. The PUSH
    // handler operands below are certified PE HIGHLOW relocations; their loaded
    // absolute values remain valid when copied into the trampoline.
    const bool installed =
        alien_pvp_local::InstallCameraHooks(base,hooks,error) &&
        InstallPvPDonorVentHooks(hooks,error) &&
        HookNativeOutcomes(hooks, error) &&
        HookLifecycleEvents(hooks,error) &&
        health::InstallDamageObserver(hooks, error) &&
        combat::Install(hooks, error) &&
        HookParticipantSenseSignals(hooks, error) &&
        HookImpactShooterIdentity(hooks, error) &&
        HookParticipantPairedIdentity(hooks, error) &&
        HookPairedVictimGuards(hooks, error) &&
        HookPairedGraphTrace(hooks, error) &&
        Hook(hooks, "aisync_paired_scheduler_idle_scope", 0x00287690U,
            reinterpret_cast<void*>(&SchedulerIdleDetour), {0x6A,0xFF,0x68,0x33,0x72,0x12,0x01},
            &gSchedulerIdleTrampoline, &gSchedulerIdleOriginal, error, 0x00D27233U) &&
        Hook(hooks, "aisync_paired_scheduler_transition_scope", 0x00287190U,
            reinterpret_cast<void*>(&SchedulerTransitionDetour), {0x55,0x8B,0xEC,0x83,0xE4,0xF8},
            &gSchedulerTransitionTrampoline, &gSchedulerTransitionOriginal, error) &&
        Hook(hooks, "aisync_paired_stop_ownership", 0x002625E0U,
            reinterpret_cast<void*>(&PairedStopDetour), {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedStopTrampoline, &gPairedStopOriginal, error, 0x00D23FC1U) &&
        Hook(hooks, "aisync_paired_idle_ownership", 0x0025FC80U,
            reinterpret_cast<void*>(&PairedIdleDetour), {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedIdleTrampoline, &gPairedIdleOriginal, error, 0x00D23FC1U) &&
        Hook(hooks, "aisync_paired_alternate_idle_ownership", 0x0025FD60U,
            reinterpret_cast<void*>(&PairedAlternateIdleDetour), {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedAlternateIdleTrampoline, &gPairedAlternateIdleOriginal, error, 0x00D23FC1U) &&
        Hook(hooks, "aisync_native_navigation_submission", 0x00283CE0U,
            reinterpret_cast<void*>(&NavigationSubmitDetour), {0x64,0xA1,0,0,0,0,0x6A,0xFF},
            &gNavigationSubmitTrampoline, &gNavigationSubmitOriginal, error) &&
        HookParticipantEligibility(hooks, error) &&
        HookParticipantPlayerSemantics(hooks, error) &&
        HookPostDeathMeleeSelection(hooks, error) &&
        Hook(hooks, "aisync_pvp_melee_start_probe", 0x00317D90U,
            reinterpret_cast<void*>(&PvPMeleeStartDetour), {0x83,0xEC,0x2C,0x53,0x55,0x56},
            &gPvPMeleeStartTrampoline, &gPvPMeleeStartOriginal, error) &&
        HookPairedPlayerClassification(hooks, error) &&
        HookBackstageParticipantContext(hooks, error) &&
        Hook(hooks, "aisync_participant_record_validation", 0x002CDB00U,
            reinterpret_cast<void*>(&RecordInvalidDetour),
            {0x56,0x8B,0xF1,0x83,0xBE,0x2C,0x04,0x00,0x00,0x00},
            &gRecordInvalidTrampoline, &gRecordInvalidOriginal, error) &&
        Hook(hooks, "aisync_brain_branch", 0x0033433EU, reinterpret_cast<void*>(&BranchDetour),
            {0x8B,0x8E,0xF4,0x00,0x00,0x00,0x5F}, &gBranchTrampoline, &gBranchPublished, error) &&
        Hook(hooks, "aisync_manager_update", 0x003342A0U, reinterpret_cast<void*>(&UpdateDetour),
            {0x56,0x8B,0xF1,0x8B,0x8E,0x28,0x01,0x00,0x00}, &gUpdateTrampoline, &gUpdateOriginal, error) &&
        Hook(hooks, "aisync_coordinator", 0x0032B880U, reinterpret_cast<void*>(&CoordinatorDetour),
            {0x83,0xEC,0x08,0x53,0x55}, &gCoordinatorTrampoline, &gCoordinatorOriginal, error) &&
        Hook(hooks, "aisync_task_arbitration", 0x00312020U, reinterpret_cast<void*>(&PrepassDetour),
            {0x6A,0xFF,0x68,0xE8,0xC2,0x12,0x01}, &gPrepassTrampoline, &gPrepassOriginal, error,
            0x00D2C2E8U) &&
        Hook(hooks, "aisync_character_interaction", 0x00254820U,
            reinterpret_cast<void*>(&InteractionDetour),
            {0x83,0xEC,0x24,0x53,0x8B,0x5C,0x24,0x2C,0x56,0x8B,0xF1},
            &gInteractionTrampoline, &gInteractionOriginal, error) &&
        Hook(hooks, "aisync_paired_transition_a", 0x0025C4E0U,
            reinterpret_cast<void*>(&PairedTransitionADetour),
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedTransitionATrampoline, &gPairedTransitionAOriginal, error,
            0x00D23FC1U) &&
        Hook(hooks, "aisync_paired_transition_b", 0x0025BF60U,
            reinterpret_cast<void*>(&PairedTransitionBDetour),
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedTransitionBTrampoline, &gPairedTransitionBOriginal, error,
            0x00D23FC1U) &&
        Hook(hooks, "aisync_character_impact", 0x003CDCF0U,
            reinterpret_cast<void*>(&CharacterImpactDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x64},
            &gCharacterImpactTrampoline, &gCharacterImpactOriginal, error) &&
        Hook(hooks, "aisync_character_impact_secondary", 0x003CE020U,
            reinterpret_cast<void*>(&CharacterImpactSecondaryDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x24},
            &gCharacterImpactSecondaryTrampoline, &gCharacterImpactSecondaryOriginal, error) &&
        Hook(hooks, "aisync_participant_candidates", 0x0032AC00U,
            reinterpret_cast<void*>(&SensesCandidatesDetour),
            {0x6A,0xFF,0x68,0x13,0xAD,0x12,0x01},
            &gCandidatesTrampoline, &gCandidatesOriginal, error, 0x00D2AD13U) &&
        Hook(hooks, "aisync_participant_hostility", 0x0037A800U,
            reinterpret_cast<void*>(&HostilityDetour),
            {0x8B,0x54,0x24,0x04,0x85,0xD2},
            &gHostilityTrampoline, &gHostilityOriginal, error) &&
        HookThePlayerResolver(hooks, error) &&
        animation::Install(base, hooks, {&AnimationHumanPair, &AnimationManagedSource, &ObservePairedAnimation,
            &AnimationCanReplayScripted}, error) &&
        // Animation validates the original scripted constructor at 0x2579F0.
        // Install the intention detours only after that ABI validation; an
        // earlier detour replaces the bytes and rejects our own installation.
        HookExecutionCycleCalls(hooks,error);
    if (!installed) {
        std::string rollback;
        static_cast<void>(hooks.UninstallFrom(first, rollback));
        if (!rollback.empty()) error += "; rollback: " + rollback;
        return false;
    }
    character_direct_step_runtime::SetNativeAITransitionGuard(&RetainAuthoritativeSourceGraph);
    character_direct_step_runtime::SetPairedVictimHeldQuery(&PairedVictimHeld);
    // The complete native transaction pipeline is mandatory.
    const execution::OwnershipAdapter ownership{nullptr,&ResolveTransactionActor,&DrainTransactionActor,
        &TransactionNativeReleased,&RestoreTransactionActor,&PrepareTransactionActor};
    execution::SetLifecycleTrace(&RecordLifecycleTrace);
    execution::SetCommandTrace(&TraceClientCommand);
    execution::SetConsumedAction(&PairConsumedAction);
    execution::SetWaitTrace(&TraceClientWait);
    execution::SetNativeIntentAdapter(&CheckNativeIntent,&InvokeNativeIntent);
    host_producer::SetNativeMarkerCapture([](std::uint32_t local,native_intent::Payload& output) noexcept {
        if(!HostNativeCaptureGuard()||local>INT32_MAX)return false;
        marker_identity::Key key{};
        const auto result=marker_identity::Capture(gBase,0,gCurrent.nativeGeneration,static_cast<std::int32_t>(local),key);
        gMarkerCaptureResult.store(static_cast<std::uint32_t>(result),std::memory_order_relaxed);
        if(result!=marker_identity::Result::Ok)return false;
        gMarkerKeysCaptured.fetch_add(1U,std::memory_order_relaxed);
        output={};output.words[0]=24;output.words[1]=1;
        for(std::size_t i=0;i<8;++i)for(std::size_t b=0;b<4;++b)
            output.words[2+i]|=static_cast<std::uint32_t>(key.contentSha256[i*4+b])<<(b*8);
        output.words[10]=key.coverOrdinal;output.words[11]=key.markerOrdinal;
        return true;
    });
    execution::SetMarkerResolver([](const transactions::Record& record,std::uint32_t& local) noexcept {
        local=UINT32_MAX;
        if(!EngineThread()||record.nativeKind!=2||record.intent.words[0]!=24||record.intent.words[1]!=1||
            !execution_runtime::Matches(record.scope))return false;
        marker_identity::Key key{};
        for(std::size_t i=0;i<8;++i)for(std::size_t b=0;b<4;++b)
            key.contentSha256[i*4+b]=static_cast<std::uint8_t>(record.intent.words[2+i]>>(b*8));
        key.coverOrdinal=record.intent.words[10];key.markerOrdinal=record.intent.words[11];
        MarkerCommandBinding* bound{};MarkerCommandBinding* empty{};
        for(auto& item:gMarkerCommandBindings) {
            if(item.used&&item.scope!=record.scope)item={};
            if(item.used&&item.scope==record.scope&&item.owner==record.owner&&item.command==record.commandId)bound=&item;
            if(!item.used&&!empty)empty=&item;
        }
        marker_identity::Result result{};
        if(bound)result=bound->mapping.key==key?marker_identity::Revalidate(gBase,bound->mapping):marker_identity::Result::ContentMismatch;
        else {
            marker_identity::Mapping mapping{};
            result=marker_identity::Resolve(gBase,0,gCurrent.nativeGeneration,key,mapping);
            if(result==marker_identity::Result::Ok) {
                if(!empty)result=marker_identity::Result::CapacityExceeded;
                else {*empty={record.scope,record.owner,record.commandId,mapping,true};bound=empty;}
            }
        }
        gMarkerResolveResult.store(static_cast<std::uint32_t>(result),std::memory_order_relaxed);
        if(result!=marker_identity::Result::Ok||!bound||bound->mapping.marker<0)return false;
        gMarkerKeysResolved.fetch_add(1U,std::memory_order_relaxed);
        local=static_cast<std::uint32_t>(bound->mapping.marker);return true;
    });
    host_producer::SetNativeSchedulerObserver(&ObserveNativeScheduler);
    host_producer::SetCommandTrace(&TraceHostCommand);
    execution::SetExecutionOwnedQuery(&ExecutionOwnedCharacter);
    execution::SetHostProgressQuery(&QueryNativeHostProgress,nullptr);
    const bool ready=execution_runtime::Initialize(base,hooks.ExactBuildValidated(),&ExecutionScopeQualified,
            ownership,&ExecutionRetirementQualified,true,&PairTransactionReserved,&PairTransactionContextReady,
            &LifecyclePlacementReady,&ManualControlReserved) &&
        host_producer::InstallNative(base,hooks,&HostNativeCaptureContext,&HostNativeCaptureGuard,
            &HostNativeRetirementQualified,error) &&
        marker_identity::Install(base,hooks,&EngineThread,error) &&
        mesh_storage::Install(base,hooks,&EngineThread,error) &&
        host_producer::Initialize(&HostBindingQualified) &&
        pair_retirement::Install(base,hooks,&PairRetirementQualified,error) &&
        gPairRuntime.Initialize(base,hooks.ExactBuildValidated(),{
            reinterpret_cast<std::uintptr_t>(gInteractionOriginal.load(std::memory_order_acquire)),
            reinterpret_cast<std::uintptr_t>(gPairedTransitionAOriginal.load(std::memory_order_acquire)),
            reinterpret_cast<std::uintptr_t>(gPairedTransitionBOriginal.load(std::memory_order_acquire))},
            &PairTransactionPermission,nullptr) &&
        route_runtime::Install(base,hooks,&EngineThread,&HostRouteQualified,&ClientRouteQualified,error);
    if(!ready) {
        if(error.empty())error="AISync native transaction pipeline initialization refused";
        gTypedExecutionInstallationError=error;
        gInstalled.store(false,std::memory_order_release);
        std::string rollback;static_cast<void>(hooks.UninstallFrom(first,rollback));
        if(!rollback.empty())error+="; rollback: "+rollback;
        return false;
    }
    gPairRuntimeInitialized=true;
    execution::SetPlacementDedupReady(true);
    gInstalled.store(true,std::memory_order_release);

    return true;
}
bool Configure(const std::span<const std::byte> payload) noexcept {
    Configuration value{};
    if (payload.size() != sizeof(value) || !Installed()) return false;
    std::memcpy(&value, payload.data(), sizeof(value));
    if (value.version != 3U || value.participantCount > kParticipants || (value.flags & ~(kDisableAlien | kAlienPvP)) != 0U ||
        static_cast<std::uint32_t>(value.role) > 3U ||
        (value.role != Role::Retail && value.sessionCookie == 0U) ||
        (value.role != Role::Retail && value.localPeer == 0U) ||
        (value.role != Role::Retail && value.role != Role::FrozenClient &&
            (value.nativeGeneration == 0U || value.worldGeneration == 0U)) ||
        !ValidParticipantBinding(value.localBinding) ||
        (value.localBinding.peer!=0U&&(value.localBinding.peer!=value.localPeer||value.localBinding.incarnation==0U))) return false;
    for (std::uint32_t i = 0U; i < value.participantCount; ++i) {
        const auto& binding = value.participants[i];
        if (binding.peer == 0U || binding.peer == value.localPeer ||
            !ValidParticipantBinding(binding) ||
            (value.localBinding.peer!=0U&&binding.character==value.localBinding.character)) return false;
        for (std::uint32_t j = 0U; j < i; ++j)
            if (value.participants[j].peer == binding.peer ||
                value.participants[j].character == binding.character) return false;
    }
    for(std::size_t i=value.participantCount;i<kParticipants;++i)
        if(value.participants[i]!=ParticipantBinding{})return false;
    std::lock_guard lock(gMailboxMutex);
    if (std::memcmp(&gRequested, &value, sizeof(value)) == 0) return true;
    const bool contextChanged = value.role != gRequested.role ||
        value.nativeGeneration != gRequested.nativeGeneration ||
        value.worldGeneration != gRequested.worldGeneration ||
        value.sessionCookie != gRequested.sessionCookie || value.localPeer != gRequested.localPeer;
    gRequested = value; gConfigurationPending = true;
    if (contextChanged) {
        execution_runtime::Retire();
        host_producer::RetireHost();
        gIncomingUsed.fill(false); gInitialPlacementUsed.fill(false);
        for (auto& queue : gIncomingOutcomes) queue.clear();
        gLastQueuedOutcome.fill({}); gLastQueuedDamage.fill({}); gLastQueuedParticipantDamage.fill({}); gLastQueuedShot.fill({}); gIncomingOutcomeCount = 0U;
        std::lock_guard impactLock(gImpactMailboxMutex);
        gOutgoingImpactRead = gOutgoingImpactWrite = 0U;
        gIncomingImpactRead = gIncomingImpactWrite = 0U;
        gImpactSequence = 0U;
        gLastImpactSequence.fill({});
    }
    return true;
}

bool ValidImpactEvent(const ImpactEvent& value) noexcept {
    return value.version == 2U && value.nativeGeneration != 0U && value.sessionCookie != 0U &&
        value.worldGeneration != 0U && value.sequence != 0U && value.sourcePeer != 0U &&
        (value.targetA != 0U || value.targetB != 0U) && AICharacterClass(value.targetClass) &&
        value.targetBirth != 0U &&
        (((value.channel == 1U || value.channel == 2U) && value.damage != 0U &&
            value.damage <= 0xFFFFU && value.reserved == 0U) ||
         (value.channel == 3U && value.damage == 0U && value.descriptor <= 0xFFFFU &&
            (value.reserved & 0xFFU) < 0x80U && (value.reserved >> 8U) >= 1U && (value.reserved >> 8U) <= 16U));
}

bool QueueRemote(const std::span<const std::byte> payload, const std::uint64_t through) noexcept {
    State value{};
    if (payload.size() != sizeof(value) || !Installed()) return false;
    std::memcpy(&value, payload.data(), sizeof(value));
    if (!Valid(value)) return false;
    std::lock_guard lock(gMailboxMutex);
    const auto& configuration = gRequested;
    if (configuration.role != Role::Client || value.sessionCookie != configuration.sessionCookie ||
        value.worldGeneration != configuration.worldGeneration) {
        gDropped.fetch_add(1U, std::memory_order_relaxed); return true;
    }
    gRemoteQueued.fetch_add(1U, std::memory_order_relaxed);
    std::size_t free = kActors;
    std::size_t selected = kActors;
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (!gIncomingUsed[i]) { if (free == kActors) free = i; continue; }
        const auto& old = gIncoming[i];
        if (old.entityA != value.entityA || old.entityB != value.entityB) continue;
        if (value.birth < old.birth || (value.birth == old.birth && value.revision < old.revision)) return true;
        selected = i; break;
    }
    if (selected == kActors) selected = free;
    if (selected == kActors) { gDropped.fetch_add(1U, std::memory_order_relaxed); return false; }
    auto& last = gLastQueuedOutcome[selected];
    const bool differentActor = last.entityA != value.entityA || last.entityB != value.entityB;
    const bool newOutcome = value.outcome.revision != 0U && (differentActor || value.birth > last.birth ||
        (value.birth == last.birth && (value.interactionPairRevision > last.interactionPairRevision ||
         (value.interactionPairRevision == last.interactionPairRevision && value.outcome.revision > last.outcome.revision))));
    auto& lastDamage = gLastQueuedDamage[selected];
    const bool newDamage = (value.stateFlags & ConsequenceEvent) != 0U && value.damage.revision != 0U &&
        (lastDamage.entityA != value.entityA || lastDamage.entityB != value.entityB || value.birth > lastDamage.birth ||
         (value.birth == lastDamage.birth && value.damage.revision > lastDamage.damage.revision));
    auto& lastParticipant = gLastQueuedParticipantDamage[selected];
    const bool newParticipantDamage = (value.stateFlags & ParticipantConsequenceEvent) != 0U &&
        (lastParticipant.entityA != value.entityA || lastParticipant.entityB != value.entityB || value.birth > lastParticipant.birth ||
         (value.birth == lastParticipant.birth && value.participantConsequence.damage.revision >
            lastParticipant.participantConsequence.damage.revision));
    auto& lastShot = gLastQueuedShot[selected];
    const bool newShot = (value.stateFlags & ShotEvent) != 0U &&
        (lastShot.entityA != value.entityA || lastShot.entityB != value.entityB || value.birth > lastShot.birth ||
         (value.birth == lastShot.birth && value.shot.revision > lastShot.shot.revision));
    if (newOutcome || newDamage || newParticipantDamage || newShot) {
        if (gIncomingOutcomeCount >= kRing) {
            gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed); return false;
        }
        gIncomingOutcomes[selected].push_back({value,through}); ++gIncomingOutcomeCount;
        if (newOutcome) last = value;
        if (newDamage) lastDamage = value;
        if (newParticipantDamage) lastParticipant = value;
        if (newShot) lastShot = value;
    }
    auto& baseline=gInitialPlacements[selected];
    if((!gInitialPlacementUsed[selected]||baseline.entityA!=value.entityA||baseline.entityB!=value.entityB||baseline.birth!=value.birth)&&
        (value.stateFlags&PoseValid)!=0U&&!ProxyWithdrawn(value)) {
        baseline=value;gInitialPlacementUsed[selected]=true;
    }
    gIncoming[selected] = value; gIncomingUsed[selected] = true; gIncomingAt[selected] = GetTickCount64();
    if (selected == free) gNextIdentityProbe[selected] = 0ULL;
    return true;
}

void BeginExecutionCarrierScope(const transactions::Scope& scope) noexcept {
    if(!EngineThread()||gCausalScope==scope)return;
    // Called only after the executor/ownership and active-pair retirement
    // barriers accepted the new scope. Old value packets never gain the new
    // authority's permission. Applied health/damage cursors stay monotonic.
    const std::lock_guard lock(gMailboxMutex);
    gIncomingUsed.fill(false); gInitialPlacementUsed.fill(false);
    for(auto& pending:gIncomingOutcomes)pending.clear();gIncomingOutcomeCount=0;
    for(auto& actor:gActors)if(actor.character){
        if(actor.recoveryNativeArmed)
            static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(actor.character));
        actor.received={};actor.streamRevision=actor.streamBirth=actor.streamCycleState=actor.streamPairRevision=actor.streamCycleType=0U;
        actor.desired={};actor.pendingRemoteShots.clear();actor.initialPlacementReady=false;actor.lastSpawnDiagnostic=0;
        actor.recoveryPose={};actor.recoveryPoseAt=actor.recoveryLastTick=actor.recoveryLastHardSnap=0;
        actor.directPoseTimeline.Reset();actor.directPoseBirth=0U;
        actor.directPoseDivergenceSince=actor.directPoseLastEmergencyCorrection=0U;
        actor.observedTrail.Clear();
        actor.recoveryProgress={};
        actor.recoveryHandoffLifecycleRevision=0;
        ReleaseNativeFollow(actor);
        actor.recoveryFollowing=actor.recoveryNativeArmed=false;
    }
    route_execution::BeginScope(scope);
    gCausalScope=scope;
}
bool PrepareInitialPlacement(Actor& actor,const State& baseline,const ULONGLONG now) noexcept {
    if(actor.initialPlacementReady)return true;
    const auto waiting=[&]() noexcept {
        if(now-actor.lastSpawnDiagnostic>=1000U){RecordSpawnPlacement(actor,baseline,false);actor.lastSpawnDiagnostic=now;}
        return false;
    };
    if(!UnownedCleanupAllowed(actor)||!CharacterReady(actor)||
        !ApplyAuthoritativeWithdraw(actor,baseline)||!ApplyAuthoritativeBackstage(actor,baseline))return waiting();
    // Keep the native spawn pose. Authority renewal must never reposition a live body.
    std::uint32_t runtime{};transactions::Scope scope{};
    if(!Read(actor.character+0x50U,runtime)||runtime!=actor.runtimeId||
        !execution_runtime::CurrentScope(scope)||scope!=gCausalScope)return false;
    actor.initialPlacementReady=true;actor.appliedLifecycleRevision=baseline.lifecycleRevision;
    ++gInitialNativePosesPreserved;return true;
}
bool ApplyExecutionCarrier(const State& state,std::uint64_t through,std::uint64_t arrivalTick) noexcept {
    if(!EngineThread()||!Stable()||!Client())return false;
    // Zero is a valid prefix (baseline before the first command), so presence
    // comes from the queue, never a sentinel value in this sequence number.
    // Each irreversible event retains its own accepted command prefix. A
    // pending ordinary sample is not its causal predecessor; reusing that
    // sample's prefix can block the very action required by the new event.
    if(!QueueRemote(std::as_bytes(std::span(&state,1)),through))return false;
    auto* actor=FindActorByIdentity(state.entityA,state.entityB);
    if(!actor||actor->duplicate||actor->faulted||actor->characterClass!=state.characterClass||state.birth<actor->remoteBirth||
        (state.birth==actor->remoteBirth&&state.revision<actor->remoteRevision))return true;
    actor->received=state;
    if((state.stateFlags&PoseValid)!=0U&&(state.birth>actor->recoveryPose.birth||
        (state.birth==actor->recoveryPose.birth&&state.revision>actor->recoveryPose.revision))) {
        // Use IPC intake time, not time spent waiting behind native work. A
        // delayed causal carrier cannot renew an already expired motion lease.
        if(state.birth!=actor->recoveryPose.birth){actor->recoveryProgress={};actor->observedTrail.Clear();}
        if((state.motionFlags&1U)!=0U&&actor->recoveryProgress.Observe(state.motionTick,arrivalTick)) {
            const bool ordinary=(state.motionFlags&6U)==0U&&(state.executionFlags&ExecutionPaired)==0U&&
                !ProxyWithdrawn(state)&&!health::Terminal(state.health);
            actor->observedTrail.Observe({state.position[0],state.position[1],state.position[2]},state.motionTick,ordinary);
        } else if((state.motionFlags&6U)!=0U||(state.executionFlags&ExecutionPaired)!=0U||
            ProxyWithdrawn(state)||health::Terminal(state.health))actor->observedTrail.Clear();
        actor->recoveryPose=state;actor->recoveryPoseAt=arrivalTick;
        // Motion has a Host monotonic clock and an authenticated local arrival
        // timestamp. Keep only the current incarnation; old segments must
        // never interpolate across a death, respawn or world transition.
        if(actor->directPoseBirth!=state.birth) {
            actor->directPoseTimeline.Reset();
            actor->directPoseBirth=state.birth;
        }
        if((state.motionFlags&hybrid_motion::Valid)!=0U) {
            hybrid_motion::Sample sample{};
            sample.tick=state.motionTick;
            for(std::size_t axis=0;axis<3U;++axis) {
                sample.position[axis]=state.position[axis];
                sample.velocity[axis]=state.motionVelocity[axis];
            }
            sample.yaw=state.yaw;sample.flags=state.motionFlags;
            static_cast<void>(actor->directPoseTimeline.Push(sample,arrivalTick));
        } else {
            actor->directPoseTimeline.Reset();
        }
    }
    if(state.executionRevision!=0U&&state.executionRevision!=actor->arrivalExecRevision){
        actor->arrivalExecRevision=state.executionRevision;actor->arrivalExecTick=GetTickCount64();actor->arrivalExecUtc=UtcMs();
    }
    if(state.birth>actor->streamBirth||(state.birth==actor->streamBirth&&state.revision>=actor->streamRevision)) {
        actor->streamBirth=state.birth;actor->streamRevision=state.revision;
        actor->streamCycleState=(state.stateFlags&ExecutionValid)!=0U?state.executionCycleState:0U;
        actor->streamCycleType=(state.stateFlags&ExecutionValid)!=0U?state.executionCycleTypeRva:0U;
        actor->streamPairRevision=state.interactionPairRevision;
    }
    // Establish the baseline in stream order, before a following relocation
    // or request can run. The ordinary pump retries if the actor isn't ready.
    if(!actor->initialPlacementReady)for(std::size_t i=0;i<kActors;++i) {
        const auto& baseline=gInitialPlacements[i];
        if(gInitialPlacementUsed[i]&&baseline.entityA==state.entityA&&baseline.entityB==state.entityB&&baseline.birth==state.birth) {
            static_cast<void>(PrepareInitialPlacement(*actor,baseline,GetTickCount64()));break;
        }
    }
    // Make a pair's reservation visible BEFORE processing the next operation
    // in this causal queue, rather than waiting for a second snapshot pump.
    auto& pair=gPairTransactions[static_cast<std::size_t>(actor-gActors.data())];
    if(pair.used&&!pair.receipt.resourcesReleased&&state.birth==pair.source.authoritativeBirth&&
        state.interactionCommand==pair.key.command&&SameInteractionTarget(state,pair.progress)&&
        state.outcome.revision>=pair.progress.outcome.revision)pair.progress=state;
    if(gPairRuntimeInitialized){RefreshPairBindings();PumpPairTransactions();}
    return true;
}
bool QueueRemoteImpact(const std::span<const std::byte> payload) noexcept {
    ImpactEvent value{};
    if (payload.size() != sizeof(value) || !Installed()) return false;
    std::memcpy(&value, payload.data(), sizeof(value));
    if (!ValidImpactEvent(value)) return false;
    std::lock_guard mailboxLock(gMailboxMutex);
    std::lock_guard lock(gImpactMailboxMutex);
    if (gRequested.role != Role::Host || value.sessionCookie != gRequested.sessionCookie ||
        value.worldGeneration != gRequested.worldGeneration || value.sourcePeer == gRequested.localPeer) {
        gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    if (gIncomingImpactWrite - gIncomingImpactRead >= kRing) {
        gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    gIncomingImpacts[gIncomingImpactWrite % kRing] = value;
    ++gIncomingImpactWrite;
    gImpactQueued.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool TryPopObservedImpact(ImpactEvent& impact) noexcept {
    std::lock_guard lock(gImpactMailboxMutex);
    if (gOutgoingImpactRead == gOutgoingImpactWrite) return false;
    impact = gOutgoingImpacts[gOutgoingImpactRead % kRing];
    ++gOutgoingImpactRead;
    return true;
}

bool AcceptImpactSequence(const std::uint64_t peer, const std::uint32_t sequence) noexcept {
    ImpactSequence* free{};
    for (auto& entry : gLastImpactSequence) {
        if (entry.peer == peer) {
            if (sequence <= entry.sequence) return false;
            entry.sequence = sequence;
            return true;
        }
        if (entry.peer == 0U && free == nullptr) free = &entry;
    }
    if (free == nullptr) return false;
    free->peer = peer; free->sequence = sequence;
    return true;
}

void PumpHostImpacts() noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host || !Stable()) return;
    std::array<ImpactEvent, 32U> batch{};
    std::size_t count{};
    {
        std::lock_guard lock(gImpactMailboxMutex);
        while (count < batch.size() && gIncomingImpactRead != gIncomingImpactWrite) {
            batch[count++] = gIncomingImpacts[gIncomingImpactRead % kRing];
            ++gIncomingImpactRead;
        }
    }
    for (std::size_t i = 0U; i < count; ++i) {
        const auto& impact = batch[i];
        if (!ValidImpactEvent(impact) || impact.sessionCookie != gCurrent.sessionCookie ||
            impact.worldGeneration != gCurrent.worldGeneration || impact.sourcePeer == gCurrent.localPeer ||
            !AcceptImpactSequence(impact.sourcePeer, impact.sequence)) {
            gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        const auto source = ParticipantForPeer(impact.sourcePeer);
        const auto target = FindCharacterByIdentity(impact.targetA, impact.targetB);
        const auto* actor = Find(target);
        std::uint32_t entityA{}, entityB{}, type{}, runtimeId{};
        if (source == 0U || !RegisteredPlayerRepresentation(source) || target == 0U ||
            actor == nullptr || actor->duplicate || actor->faulted ||
            actor->desired.sessionCookie != gCurrent.sessionCookie ||
            actor->desired.worldGeneration != gCurrent.worldGeneration ||
            !CharacterIdentity(target, entityA, entityB, type) || entityA != impact.targetA ||
            entityB != impact.targetB || type != impact.targetClass ||
            actor->entityA != entityA || actor->entityB != entityB || actor->characterClass != type ||
            !Read(target + 0x50U, runtimeId) ||
            !ImpactTargetLifetimeMatches(impact.targetBirth, actor->birth, runtimeId, actor->runtimeId) ||
            TerminalDeadCharacter(target)) {
            gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        std::uint32_t eligibleType{};
        if (!Eligible(target, eligibleType) || eligibleType != type) {
            gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        const bool applied = impact.channel == 3U
            ? ApplyNativeImpactStimulusSafe(target, source, impact.descriptor,
                impact.reserved & 0xFFU, impact.reserved >> 8U)
            : ApplyNativeImpactDamageSafe(target, impact.damage, source, impact.descriptor);
        if (!applied) {
            gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (auto* updated = Find(target); updated != nullptr) updated->lastSend = 0U;
        gImpactApplied.fetch_add(1U, std::memory_order_relaxed);
    }
}

// ---- Client native follow locomotion (524+) ------------------------------
// Retail producer 0070A940 sequence (Alien MoveToMostRecentSensedPosition):
// gait M+194 -> factory 00683BF0 -> initial goal 006777B0 -> +B8|=1 ->
// replace-submit 00683CE0 -> clear +B8 bit5. useStop=0 and stopping/stop
// distances -1 keep the Move alive while 006C7550 (phase3, no pending edit)
// moves its goal every frame; retail replans or edits the terminal in place.
// A Host stop becomes a final goal: retail brakes and plays its own stop.
std::atomic<std::uint64_t> gFollowCreated{},gFollowCreateFailures{},gFollowCompleted{},gFollowCancelled{},
    gFollowEmergencies{},gFollowIdleTurns{},gFollowDivergenceReplans{},gSpectateFallbackCandidates{};
struct FollowRef {std::uintptr_t object,control;};
bool FollowNativeCreate(std::uintptr_t character,const float* goal,FollowRef& out) noexcept {
    out={};
    __try {
        reinterpret_cast<FollowRef*(__thiscall*)(void*,FollowRef*)>(gBase+0x283BF0U)(
            reinterpret_cast<void*>(character+0x2CD0U),&out);
        if(!out.object||!out.control)return false;
        if(*reinterpret_cast<const std::uintptr_t*>(out.object)!=gBase+0xEB1A70U)return false;
        reinterpret_cast<void(__thiscall*)(void*,const float*)>(gBase+0x2777B0U)(reinterpret_cast<void*>(out.object),goal);
        *reinterpret_cast<std::uint8_t*>(out.object+0x18U)=0U; // our own Ref observes it; no retained list entry
        *reinterpret_cast<std::uint8_t*>(out.object+0xB8U)|=1U; // 004659D0 local route bounds
        std::uint32_t type{};
        const auto info=*reinterpret_cast<const std::uintptr_t*>(character+0x64U);
        if(info)type=*reinterpret_cast<const std::uint32_t*>(info+4U);
        if(type==3U||type==4U||type==6U||type==9U) {
            *reinterpret_cast<float*>(out.object+0x9CU)=8.0F;*reinterpret_cast<float*>(out.object+0xA0U)=1.15F;
        }
        const auto accepted=reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(gBase+0x283CE0U)(reinterpret_cast<void*>(out.object));
        *reinterpret_cast<std::uint8_t*>(out.object+0xB8U)&=static_cast<std::uint8_t>(~0x20U);
        return accepted!=0U;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool FollowNativeGoal(std::uintptr_t request,const float* goal) noexcept {
    __try {reinterpret_cast<void(__thiscall*)(void*,const float*)>(gBase+0x2C7550U)(reinterpret_cast<void*>(request),goal);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void FollowNativeRelease(FollowRef ref,bool cancel) noexcept {
    __try {
        if(cancel) {
            const auto status=*reinterpret_cast<const std::uint32_t*>(ref.object+0xCU);
            if(status<=2U)reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x283DB0U)(reinterpret_cast<void*>(ref.object));
        }
        reinterpret_cast<void(__thiscall*)(FollowRef*)>(gBase+0xD770U)(&ref);
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}
bool FollowGait(std::uintptr_t character,std::uint32_t gait) noexcept {
    __try {
        const auto movement=*reinterpret_cast<const std::uintptr_t*>(character+0x42A8U);
        if(!movement||gait>3U)return false;
        auto& input=*reinterpret_cast<std::uint32_t*>(movement+0x194U);
        if(input!=gait){input=gait;*reinterpret_cast<std::uint32_t*>(movement+0x190U)=0U;}
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// The declaration lives in the file's original unnamed namespace.
// Reopen it so this definition refers to that same function: otherwise
// the enclosing ai_sync namespace adds a second overload and C2668 fires.
namespace {
void ReleaseNativeFollow(Actor& actor,bool cancel) noexcept {
    if(!actor.follow.object)return;
    FollowNativeRelease({actor.follow.object,actor.follow.control},cancel);
    if(cancel)gFollowCancelled.fetch_add(1U,std::memory_order_relaxed);
    const auto failures=actor.follow.failures;const auto retry=actor.follow.retryAt;
    actor.follow={};actor.follow.failures=failures;actor.follow.retryAt=retry;
}
} // namespace
void FollowIdleFacing(Actor& actor,const hybrid_motion::Target& target,const float* local,float yaw,ULONGLONG now) noexcept {
    // Standing still on both peers: keep the Host facing with a bounded turn.
    auto& f=actor.follow;
    float delta=target.yaw-yaw;
    while(delta>3.14159265F)delta-=6.2831853F;
    while(delta<-3.14159265F)delta+=6.2831853F;
    if(std::abs(delta)<0.35F){f.idleYawSince=0U;return;}
    if(!f.idleYawSince){f.idleYawSince=now;return;}
    if(now-f.idleYawSince<400U)return;
    const float step=std::clamp(delta,-0.05F,0.05F); // ~3 rad/s at 60 fps
    if(character_direct_step_runtime::ApplyAIRecoveryTransform(actor.character,local,yaw+step))
        gFollowIdleTurns.fetch_add(1U,std::memory_order_relaxed);
}
void DriveNativeFollow(Actor& actor,const hybrid_motion::Target& target,std::uint32_t hostGait,ULONGLONG now) noexcept {
    auto& f=actor.follow;
    float local[3]{},yaw{};
    if(!Read(actor.character+kPositionOffset,local[0])||!Read(actor.character+kPositionOffset+4U,local[1])||
        !Read(actor.character+kPositionOffset+8U,local[2])||!Read(actor.character+kYawOffset,yaw)){ReleaseNativeFollow(actor);return;}
    const float dx=target.position[0]-local[0],dz=target.position[2]-local[2];
    const float planar=std::sqrt(dx*dx+dz*dz);
    const float speed=std::hypot(target.velocity[0],target.velocity[2]);
    // Distance alone is not a stuck route: the Host may be several metres
    // ahead while the local Alien is still walking. Only retire a native
    // follow that has failed to translate 0.15 m over 750 ms despite that
    // large error. With no live route, allow creation/retry normally.
    if(planar>3.5F && f.object) {
        if(!f.divergedSince) {
            f.divergedSince=now;
            f.divergedAnchorX=local[0];f.divergedAnchorZ=local[2];
        } else if(now-f.divergedSince>=750U) {
            const float moved=std::hypot(local[0]-f.divergedAnchorX,local[2]-f.divergedAnchorZ);
            if(moved<0.15F) {
                ReleaseNativeFollow(actor);
                f.retryAt=now+250U;
                gFollowDivergenceReplans.fetch_add(1U,std::memory_order_relaxed);
                return;
            }
            f.divergedSince=now;
            f.divergedAnchorX=local[0];f.divergedAnchorZ=local[2];
        }
    } else f.divergedSince=0U;
    // Observe the live request: 3 done, 4 failed, 5 removed, 6 released.
    if(f.object) {
        std::uint32_t status{};
        if(!Read(f.object+0xCU,status)||status>2U) {
            if(status==3U)gFollowCompleted.fetch_add(1U,std::memory_order_relaxed);
            else if(status==4U){++f.failures;f.retryAt=now+std::min<ULONGLONG>(2000U,150U*f.failures);}
            ReleaseNativeFollow(actor,false);
        }
    }
    const bool hostMoving=speed>0.25F;
    if(hostMoving)f.stillSince=0U;else if(!f.stillSince)f.stillSince=now;
    const bool hostStopped=!hostMoving&&now-f.stillSince>=150U;
    // Gait: the Host's effective gait; one step faster when clearly behind.
    std::uint32_t gait=hostGait<=3U?hostGait:1U;
    if(planar>2.0F&&gait<3U)++gait;
    float goal[3]{target.position[0],target.position[1],target.position[2]};
    if(!hostStopped&&speed>0.05F) {
        // Through-goal ahead of the Host so retail never reaches its braking
        // distance while the Host keeps moving.
        const float lead=std::clamp(speed*0.6F,1.2F,4.0F);
        goal[0]+=target.velocity[0]/speed*lead;goal[2]+=target.velocity[2]/speed*lead;
    }
    const bool needsMove=hostMoving||planar>0.6F;
    if(!f.object) {
        if(!needsMove||now<f.retryAt){FollowIdleFacing(actor,target,local,yaw,now);return;}
        const character_direct_step_runtime::MovementResourceLease resources(actor.character);
        if(!resources)return;
        static_cast<void>(FollowGait(actor.character,gait));
        FollowRef ref{};
        // A failed far goal (off-mesh extrapolation) retries on the Host pose.
        const float* initial=(f.failures&1U)?target.position.data():goal;
        if(!FollowNativeCreate(actor.character,initial,ref)) {
            if(ref.object)FollowNativeRelease(ref,true);
            ++f.failures;f.retryAt=now+std::min<ULONGLONG>(2000U,150U*f.failures);
            gFollowCreateFailures.fetch_add(1U,std::memory_order_relaxed);return;
        }
        f.object=ref.object;f.control=ref.control;f.createdAt=now;f.stopping=false;f.idleYawSince=0U;
        gFollowCreated.fetch_add(1U,std::memory_order_relaxed);return;
    }
    f.failures=0U;
    static_cast<void>(FollowGait(actor.character,gait));
    f.stopping=hostStopped;
    // Every frame: 006C7550 accepts only in phase3 without a pending edit.
    static_cast<void>(FollowNativeGoal(f.object,goal));
}
void PublishLocalIncarnation() noexcept {
    auto& registry=character_incarnations::NativeRegistry();
    character_incarnations::Ticket ticket{};
    if(Stable()) {
        const auto generation=gGenerations->CurrentGeneration();
        const auto player=LocalPlayer();std::uint32_t runtime{},after{};
        if(player&&Read(player+0x50U,runtime)&&runtime) {
            const auto found=registry.Find(generation,player,ticket);
            if(found==character_incarnations::Result::Missing||found==character_incarnations::Result::WrongGeneration)
                static_cast<void>(registry.BindExisting(generation,player,runtime,ticket));
            if(ticket.runtimeId!=runtime||LocalPlayer()!=player||!Read(player+0x50U,after)||after!=runtime)ticket={};
        }
    }
    static_cast<void>(registry.PublishLocal(ticket));
}
// PlayerLifecycle spectate: living remote participants of this exact world,
// ascending peer order. The Alien PvP owner's parked human is never watchable.
static void PumpPlayerSpectate(const std::uint32_t generation, const bool cameraReserved,
    const std::uint64_t alienOwner) noexcept {
    std::array<player_lifecycle::SpectateCandidate, player_lifecycle::spectate::kMaxCandidates> candidates{};
    std::size_t count{};
    if (Stable() && gCurrent.nativeGeneration == generation) {
        for (std::uint32_t i = 0U; i < gCurrent.participantCount && count < candidates.size(); ++i) {
            const auto peer = gCurrent.participants[i].peer;
            if (peer == 0U || peer == gCurrent.localPeer || peer == alienOwner) continue;
            const auto character = ParticipantForPeer(peer);
            if (LivingCharacter(character)) {
                candidates[count++] = {peer, character};
            } else if (character != 0U && RegisteredPlayerRepresentation(character)) {
                // A replicated player may temporarily have its activation
                // flag cleared while its HP and native lifetime remain valid.
                // Do not discard the only living spectate target on that flag
                // alone; ParticipantForPeer already certified the incarnation.
                std::uint16_t hp{};std::uint32_t health{};
                if(Read(character+0x33CU,hp)&&hp!=0U&&Read(character+0x32CU,health)&&health<3U) {
                    candidates[count++] = {peer, character};
                    gSpectateFallbackCandidates.fetch_add(1U,std::memory_order_relaxed);
                }
            }
        }
        std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count),
            [](const auto& a, const auto& b) { return a.peer < b.peer; });
    }
    player_lifecycle::PumpSpectateOnEngineThread({gBase, LocalPlayer(), generation,
        candidates.data(), count, cameraReserved});
}
void PumpOnEngineThread() noexcept {
    const profiling::Timer timing(profiling::Section::AiPump);
    if (!Installed() || !EngineThread()) return;
    PublishLocalIncarnation();
    ReleaseOrphanedParticipantVictims();
    {
        const auto until = GetTickCount64() + kPairedParticipantVictimHoldMs;
        for (const auto& actor : gActors) {
            if (actor.character == 0U || actor.duplicate) continue;
            const auto flags = actor.desired.interactionFlags;
            if ((flags & InteractionTargetIsParticipant) == 0U ||
                (flags & (InteractionCycleActive | InteractionPairValid)) == 0U) continue;
            // PairValid persists after the pair (until the next interaction):
            // holding on it alone froze the survivor until the session ended.
            std::uintptr_t attackerCycle{};std::uint8_t attackerState{};
            if ((flags & InteractionCycleActive) == 0U &&
                (!Read(actor.character + 0x36CU, attackerCycle) || attackerCycle == 0U ||
                 !Read(attackerCycle + 0x1AU, attackerState) ||
                 (attackerState != 0x42U && attackerState != 0x47U && attackerState != 0x4EU))) continue;
            const auto victim = ParticipantForPeer(actor.desired.interactionTargetPeer);
            if (victim != 0U) PublishPairedParticipantVictim(victim, until);
        }
    }
    // Revoke old-world value permissions BEFORE retirement and configuration
    // barriers. Waiting for owner retirement before forgetting its invalid
    // world deadlocked every later round at contextBlocker 5.
    const auto currentWorld = gGenerations->CurrentGeneration();
    if (gGeneration != currentWorld) {
        if (currentWorld == 0U || currentWorld > UINT32_MAX ||
            !execution_runtime::RevokePreviousWorld(static_cast<std::uint32_t>(currentWorld)) ||
            (gPairRuntimeInitialized && !gPairRuntime.ResetForWorld(static_cast<std::uint32_t>(currentWorld))) ||
            !pair_retirement::ResetForWorld(static_cast<std::uint32_t>(currentWorld))) return;
        // The prior world's route M/mesh remain strongly pinned by the route
        // executor. Close only paths that have lost every B/R/worker owner,
        // before the new-world configuration waits on their retirement.
        route_execution::RevokePreviousWorld();
        for (auto& actor : gActors) if (actor.character != 0U)
            static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(actor.character));
        // Reset in place: aggregate assignment of 256 carriers materializes
        // an enormous temporary on the game's small x86 engine-thread stack.
        for (auto& previous : gPairTransactions) previous = {};
        gPairRuntimeScope = {};
        { const std::lock_guard outcomeLock(gOutcomeMutex); for (auto& outcome : gOutcomes) outcome = {}; }
        { const std::lock_guard eventLock(gPairEventMutex);
            gPairEventRead = gPairEventCount = 0U; gPairEventOverflow.store(false, std::memory_order_release); }
        if (Installed()) host_producer::RetireNative(false);
        combat::Reset(false);
    }
    // Retire the old scope while its exact live context is still available,
    // before applying a pending OFF/session change under the legacy mailbox.
    if (execution_runtime::Available()) execution_runtime::PumpRetirements();
    PumpPairRetirements();
    // Retained no-worker M/R/mesh pins must progress while a pending new-world
    // configuration still waits for the old context. Otherwise that very
    // configuration gate prevents its prerequisite route retirement forever.
    route_execution::Pump();
    // NativeIdle counts deliberately pinned old-world observations. They are
    // not executable after RetireNative(false), and must not keep triggering
    // RetireHost(): that increments the Host source revision and overwrites a
    // freshly queued second-round Offer with Close before PumpHost sees it.
    // Retire only when an unfenced native observation still needs cleanup.
    if (Installed() && !host_producer::EngineProducer().Bound() &&
        host_producer::NativeNeedsLiveWorldRetirement() &&
        gGeneration == gGenerations->CurrentGeneration())
        host_producer::RetireNative(true,false);
    combat::Pump(GetTickCount64());
    // Own native path-query references independently of Characters. Maintain
    // them before taking the IPC mailbox mutex; cleanup can invoke native code.
    PumpHostNavigationContext();
    // Native Alien local pilot is deliberately outside the IPC mailbox lock.
    // Only the proven lease owner may issue physical requests on this process.
    const auto pvpPhysicalGeneration=gGenerations->CurrentGeneration();
    const auto currentRole=gRole.load(std::memory_order_acquire);
    if (gPvPLease.releasing || (gPvPLease.ownerPeer && ((gCurrent.flags & kAlienPvP)==0U ||
        gPvPLease.sessionCookie!=gCurrent.sessionCookie || gPvPLease.nativeGeneration!=pvpPhysicalGeneration))) {
        AlienPvPLeaseStatus ignored{};
        static_cast<void>(ReleaseAlienPvPLease(ignored));
    }
    const bool activeLease=(currentRole==Role::Host || currentRole==Role::Client) &&
        gPvPLease.owned && !gPvPLease.releasing && (gCurrent.flags & kAlienPvP)!=0U && PvPLeaseScopeCurrent() &&
        PvPLeaseLiveIdentity() && gCurrent.localPeer!=0U;
    if(activeLease) {
        gPvPTechnicalEpoch.store(gPairBindingEpoch.load(std::memory_order_acquire),std::memory_order_release);
        gPvPTechnicalPeer.store(gPvPLease.ownerPeer,std::memory_order_release);
    } else gPvPTechnicalPeer.store(0U,std::memory_order_release);
    const bool localLease=activeLease && gPvPLease.ownerPeer==gCurrent.localPeer;
    const bool localRequested=(gCurrent.flags & kAlienPvP)!=0U && !gPvPLease.releasing &&
        gPvPLease.ownerPeer!=0U && gPvPLease.ownerPeer==gCurrent.localPeer &&
        gPvPLease.sessionCookie==gCurrent.sessionCookie && gPvPLease.nativeGeneration==pvpPhysicalGeneration;
    if(localRequested) {
        gPvPTechnicalEpoch.store(gPairBindingEpoch.load(std::memory_order_acquire),std::memory_order_release);
        gPvPTechnicalPeer.store(gPvPLease.ownerPeer,std::memory_order_release);
    }
    alien_pvp_local::Pump({gBase,
        localLease?gPvPLease.ticket.character:0U,
        localRequested?LocalPlayer():0U,
        static_cast<std::uint32_t>(pvpPhysicalGeneration),
        gPvPLease.ownerPeer,gCurrent.localPeer,localLease,localRequested});
    PumpPlayerSpectate(static_cast<std::uint32_t>(pvpPhysicalGeneration),localRequested,
        (gCurrent.flags & kAlienPvP)!=0U?gPvPLease.ownerPeer:0U);
    const auto pilot=alien_pvp_local::Snapshot();
    alien_pvp::Context pvp{};
    if(activeLease) {
        pvp.scope={gPvPLease.nativeGeneration,gPvPLease.worldGeneration,
            gPvPLease.sessionCookie,gPvPLease.ownerPeer,gPvPLease.ticket.serial,
            gPvPLease.entityA,gPvPLease.entityB};
        pvp.base=gBase;pvp.character=gPvPLease.ticket.character;
        pvp.host=currentRole==Role::Host;pvp.localOwner=localLease;pvp.owned=true;
        pvp.pilotReady=!localLease || (pilot.inputIsolated && pilot.cameraBound && pilot.technicalHidden);
    }
    pvp.actions=&PvPActionHooks();
    RetirePvPActions();
    alien_pvp::Pump(pvp);
    std::unique_lock lock(gMailboxMutex, std::try_to_lock);
    if (!lock.owns_lock()) return; // never wait for IPC on the engine thread
    if (gConfigurationPending) {
        const bool contextChanged = gRequested.role != gCurrent.role ||
            gRequested.sessionCookie != gCurrent.sessionCookie || gRequested.worldGeneration != gCurrent.worldGeneration ||
            gRequested.nativeGeneration != gCurrent.nativeGeneration || gRequested.localPeer != gCurrent.localPeer;
        const bool sameLifetime = gRequested.sessionCookie == gCurrent.sessionCookie &&
            gRequested.worldGeneration == gCurrent.worldGeneration && gRequested.nativeGeneration == gCurrent.nativeGeneration &&
            gRequested.localPeer == gCurrent.localPeer;
        // Retire PvP while the old actor registry/configuration is still
        // available. Never erase gActors and strand a fenced live Character.
        if(gPvPLease.fenced && (contextChanged || (gRequested.flags & kAlienPvP)==0U)) {
            lock.unlock();
            AlienPvPLeaseStatus status{};
            static_cast<void>(ReleaseAlienPvPLease(status));
            return;
        }
        // A disconnect may freeze immediately. OFF/new-session/Host handoff
        // in a live world must not wake a brain while old native Refs remain.
        // A generation already destroyed by the engine is handled below without
        // dereferencing its old Characters; that is not a cleanup success.
        if (contextChanged && !(sameLifetime && gRequested.role == Role::FrozenClient) &&
            gGeneration == gGenerations->CurrentGeneration() &&
            (!execution_runtime::ContextRetirementComplete()||!PairContextRetired())) {
            if (gCurrent.role == Role::Client || gCurrent.role == Role::FrozenClient)
                gRole.store(Role::FrozenClient,std::memory_order_release);
            return;
        }
        if (gRequested.role == Role::Retail || gRequested.sessionCookie != gCurrent.sessionCookie ||
            gRequested.worldGeneration != gCurrent.worldGeneration) {
            health::ResetObservations();
            combat::Reset(gGeneration == gGenerations->CurrentGeneration());
            for (auto& origin : gHostHealthOrigins) origin.tag.store(0U, std::memory_order_release);
            for (auto& actor : gActors) {
                if (actor.character != 0U && gGeneration == gGenerations->CurrentGeneration())
                    static_cast<void>(Release(actor));
                actor = {};
            }
            // Keep births monotonic across live OFF/ON toggles. A Client may
            // retain an older Host birth while only the Host is being isolated.
        }
        if (gRequested.sessionCookie != gCurrent.sessionCookie ||
            gRequested.worldGeneration != gCurrent.worldGeneration ||
            gRequested.nativeGeneration != gCurrent.nativeGeneration ||
            (gRequested.role == Role::Retail) != (gCurrent.role == Role::Retail))
            gPairBindingEpoch.fetch_add(1U, std::memory_order_acq_rel);
        gCurrent = gRequested; gConfigurationPending = false;
        gActiveNativeGeneration.store(gCurrent.nativeGeneration, std::memory_order_relaxed);
        gActiveWorldGeneration.store(gCurrent.worldGeneration, std::memory_order_relaxed);
        gActiveSessionCookie.store(gCurrent.sessionCookie,std::memory_order_release);
        gRole.store(gCurrent.role, std::memory_order_release);
        gAlienSuppressed.store(AlienSuppressionRequested(gCurrent.role, gCurrent.flags), std::memory_order_release);
        combat::SetMode(static_cast<combat::Mode>(gCurrent.role));
        health::SetObservingHost(gCurrent.role == Role::Host);
    }
    const auto generation = gGenerations->CurrentGeneration();
    if (generation != gGeneration) {
        health::ResetObservations();
        for (auto& origin : gHostHealthOrigins) origin.tag.store(0U, std::memory_order_release);
        // Normal destruction hooks release states BEFORE Character/BT destruction.
        // Never dereference an old-generation Character on an exceptional missed hook.
        for (auto& actor : gActors) {
            actor = {};
        }
        // Pair reservations were revoked above. Never fabricate a native
        // resourcesReleased receipt as a substitute for a world transition.
        gWorldResetCount.fetch_add(1U, std::memory_order_relaxed);
        gPairBindingEpoch.fetch_add(1U, std::memory_order_acq_rel);
        gGeneration = generation;
        gLivingPlayerAllianceKnown = false;
        gIncomingUsed.fill(false); gInitialPlacementUsed.fill(false);
        for (auto& queue : gIncomingOutcomes) queue.clear();
        gLastQueuedOutcome.fill({}); gLastQueuedDamage.fill({}); gLastQueuedParticipantDamage.fill({}); gLastQueuedShot.fill({}); gIncomingOutcomeCount = 0U;
        {
            std::lock_guard impactLock(gImpactMailboxMutex);
            gOutgoingImpactRead = gOutgoingImpactWrite = 0U;
            gIncomingImpactRead = gIncomingImpactWrite = 0U;
            gImpactSequence = 0U;
            gLastImpactSequence.fill({});
        }
    }
    // Intake is independent of locomotion ownership. Copy the oldest ordered
    // carrier (or latest ordinary state) before releasing the IPC mutex. The
    // movement preflight can then defer to a pending pair without reading a
    // worker-mutated mailbox, applying a pose or consuming an ordered event.
    if (Stable() && Client()) for (std::size_t i=0;i<kActors;++i) {
        if (!gIncomingUsed[i]) continue;
        const auto& received = gIncomingOutcomes[i].empty() ? gIncoming[i] : gIncomingOutcomes[i].front().state;
        if (received.sessionCookie != gCurrent.sessionCookie || received.worldGeneration != gCurrent.worldGeneration) continue;
        auto* actor = FindActorByIdentity(received.entityA,received.entityB);
        if (actor && !actor->duplicate && !actor->faulted && actor->characterClass == received.characterClass &&
            received.birth >= actor->remoteBirth) {
            actor->received = received;
            auto& pair=gPairTransactions[static_cast<std::size_t>(actor-gActors.data())];
            if(pair.used&&!pair.receipt.resourcesReleased) {
                const auto progress=[&](const State& value) noexcept {
                    if(value.birth==pair.source.authoritativeBirth&&value.interactionCommand==pair.key.command&&
                        SameInteractionTarget(value,pair.progress)&&value.outcome.revision>=pair.progress.outcome.revision)
                        pair.progress=value;
                };
                for(const auto& value:gIncomingOutcomes[i])progress(value.state);
                progress(gIncoming[i]);
            }
        }
    }
    // Reserve the participants before the request executor can enter another
    // command. Copying pair progress above does not consume ordered gameplay.
    if(gPairRuntimeInitialized) {
        if(Stable())RefreshPairBindings();
        lock.unlock();PumpPairTransactions();if(!lock.try_lock())return;
    }
    // The endpoint may call CATHODE; no legacy IPC mailbox lock may span it.
    if (execution_runtime::Available()) {
        lock.unlock();
        execution_runtime::Pump(false); // Retirement budget was consumed before context changes.
        if (!lock.try_lock()) return;
    }
    if (Installed()) {
        // Host hooks may reenter CATHODE and must not hold the IPC mailbox lock.
        lock.unlock(); host_producer::PumpHost(); DrainHostLifecycleEvents(); host_producer::PumpNative();route_runtime::Pump();
        if (!lock.try_lock()) return;
    }
    if (Stable()) RefreshPairBindings();
    PumpHostImpacts();
    PumpHostCarriers();
    // Host publication must survive AI-manager dormancy (vent/backstage).
    // CharacterManager invokes this pump independently of each NPC brain.
    PumpHostAuthority();

    if (!Stable() || !Client()) return;
    const auto now = GetTickCount64();
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (!gIncomingUsed[i]) continue;
        auto& outcomes = gIncomingOutcomes[i];
        if (outcomes.empty() && now - gIncomingAt[i] > kTimeout) {
            gIncomingUsed[i] = false; gMissing.fetch_add(1U, std::memory_order_relaxed); continue;
        }
        auto value = outcomes.empty() ? gIncoming[i] : outcomes.front().state;
        if (value.sessionCookie != gCurrent.sessionCookie || value.worldGeneration != gCurrent.worldGeneration) {
            gIncomingUsed[i] = false; continue;
        }
        if (now < gNextIdentityProbe[i]) continue;
        Actor* found = nullptr;
        for (auto& actor : gActors)
            if (actor.character != 0U && !actor.duplicate && actor.entityA == value.entityA &&
                actor.entityB == value.entityB && actor.characterClass == value.characterClass) found = &actor;
        // Spawn can legitimately follow an incoming snapshot. Retain one latest
        // snapshot per identity; no spawn fallback or matching by list index.
        if (found == nullptr) {
            gMissing.fetch_add(1U, std::memory_order_relaxed);
            gNextIdentityProbe[i] = now + kSendPeriod;
            continue;
        }
        auto& actor = *found;
        if(value.birth<actor.remoteBirth||(value.birth==actor.remoteBirth&&value.revision<actor.remoteRevision)) {
            if(!outcomes.empty()){outcomes.pop_front();--gIncomingOutcomeCount;}
            else gIncomingUsed[i]=false;
            continue;
        }
        if(PairReservedCharacter(actor.character)||PairSourceBusy(actor.character)){if(!outcomes.empty())++gIntakeHeldByPair;continue;}
        if(actor.remoteBirth!=0U&&actor.remoteBirth!=value.birth) {RevokeTransactionActor(actor);continue;}
        // Pose initialization is permitted once, before native ownership.
        if(!actor.initialPlacementReady) {
            const auto& baseline=gInitialPlacements[i];
            if(!gInitialPlacementUsed[i]||baseline.entityA!=value.entityA||baseline.entityB!=value.entityB||baseline.birth!=value.birth||
                !PrepareInitialPlacement(actor,baseline,now))continue;
        }
        // Runtime relocation/backstage/frontstage now come from observed
        // native calls in the causal execution stream, never pose differences.
        // Subsequent poses are diagnostics; no request/task/cycle is inferred.
        actor.remoteBirth=value.birth;actor.remoteRevision=value.revision;
        actor.desired=value;actor.lastReceive=now;
        if(value.executionRevision!=actor.intakeExecRevision){
            actor.intakeExecRevision=value.executionRevision;actor.intakeExecTick=now;actor.execWaits=0U;actor.execWaitReason=0U;actor.execWaitMask=0U;actor.execDiagnosticMask=0U;
        }
        if(!ApplyAuthoritativeHealth(actor,value)){if(!outcomes.empty())++gIntakeHeldByHealth;continue;}
        if(!ApplyAuthoritativeParticipantDamage(actor,value)){if(!outcomes.empty())++gIntakeHeldByParticipant;continue;}
        if(!ApplyAuthoritativeCombat(actor,value,now)){if(!outcomes.empty())++gIntakeHeldByCombat;continue;}
        if(!outcomes.empty()){outcomes.pop_front();--gIncomingOutcomeCount;}
        gIncomingUsed[i]=!outcomes.empty()||gIncoming[i].birth>value.birth||
            (gIncoming[i].birth==value.birth&&gIncoming[i].revision>value.revision);
        if(value.outcome.revision!=0U)static_cast<void>(PublishOutcomeBinding(actor,PairTargetForState(value)));
        gApplied.fetch_add(1U,std::memory_order_relaxed);
    }
    // Health/death convergence cannot depend on a sleeping NPC manager or on
    // another network packet. Pending native phases retry without replaying FX.
    for (auto& actor : gActors)
        if (actor.character != 0U && actor.remoteBirth != 0U && !actor.duplicate && !actor.faulted &&
            (actor.desired.health.revision > actor.healthRevision ||
             ((actor.desired.stateFlags & ConsequenceEvent) != 0U && actor.desired.damage.revision > actor.damageRevision)))
            static_cast<void>(ApplyAuthoritativeHealth(actor, actor.desired));
    for (auto& actor : gActors)
        if (actor.character != 0U && actor.remoteBirth != 0U && !actor.duplicate && !actor.faulted &&
            actor.desired.sessionCookie==gCurrent.sessionCookie&&actor.desired.worldGeneration==gCurrent.worldGeneration)
        {
            static_cast<void>(ApplyAuthoritativeParticipantDamage(actor, actor.desired));
            static_cast<void>(ApplyAuthoritativeCombat(actor, actor.desired, now));
        }
    // Host pose drives ordinary locomotion. Native actions (paired kills,
    // passages, scripted reactions and terminal states) still own their cycles.
    // The native follower supplies gait/footsteps; the POST pass commits the
    // actual authoritative physics + visual transform after retail root motion.
    lock.unlock();
    for(auto& actor:gActors) {
        if(!actor.character || actor.duplicate || actor.faulted)continue;
        const auto relinquish=[&]() noexcept {
            ReleaseNativeFollow(actor);
            if(actor.recoveryNativeArmed)
                static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(actor.character));
            actor.recoveryNativeArmed=actor.recoveryFollowing=false;
            actor.directPoseDivergenceSince=0U;
        };
        std::uint32_t liveRuntime{},liveEntityA{},liveEntityB{};
        if(!Read(actor.character+0x50U,liveRuntime)||liveRuntime!=actor.runtimeId||
            !Read(actor.character+0x54U,liveEntityA)||liveEntityA!=actor.entityA||
            !Read(actor.character+0x58U,liveEntityB)||liveEntityB!=actor.entityB) {
            relinquish();continue;
        }
        if(!actor.remoteBirth || !actor.initialPlacementReady ||
            actor.directPoseBirth!=actor.remoteBirth ||
            gGeneration!=gGenerations->CurrentGeneration() ||
            PvPCharacterFenced(actor.character) ||
            ExecutionOwnedCharacter(actor.character) ||
            PairReservedCharacter(actor.character) || PairSourceBusy(actor.character) ||
            (actor.desired.executionFlags&ExecutionPaired)!=0U ||
            (actor.received.executionFlags&ExecutionPaired)!=0U ||
            ProxyWithdrawn(actor.desired)||ProxyWithdrawn(actor.recoveryPose) ||
            health::Terminal(actor.desired.health)||health::Terminal(actor.recoveryPose.health)) {
            relinquish();continue;
        }
        hybrid_motion::Target target{};
        if(!actor.directPoseTimeline.SampleAt(PreciseTickMs(),target) || target.stale ||
            target.nativeAction || target.discontinuity) {
            relinquish();continue;
        }
        const auto& state=actor.recoveryPose;
        if(state.birth!=actor.remoteBirth || state.sessionCookie!=gCurrent.sessionCookie ||
            state.worldGeneration!=gCurrent.worldGeneration ||
            (state.stateFlags&PoseValid)==0U || state.moveType>3U) {
            relinquish();continue;
        }
        if(kNativeFollowLocomotion) {
            std::uintptr_t cycle{},table{};std::uint8_t cycleState{};
            // Only an ordinary (idle/locomotion) cycle may be driven by a Move;
            // reactions, scripts and interactions keep their own executor.
            if(!Read(actor.character+0x36CU,cycle)||!cycle||!Read(cycle,table)||table<gBase||
                !Read(cycle+0x1AU,cycleState)||
                !HybridLocomotionCycleAllowed(static_cast<std::uint32_t>(table-gBase),cycleState)) {
                if(!actor.follow.object){relinquish();continue;}
            }
            if(actor.recoveryNativeArmed) {
                static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(actor.character));
                actor.recoveryNativeArmed=actor.recoveryFollowing=false;
            }
            DriveNativeFollow(actor,target,state.moveType,GetTickCount64());
            continue;
        }
        const float speed=std::hypot(target.velocity[0],target.velocity[2]);
        std::string error;
        if(!character_direct_step_runtime::ArmAIHybridReplication(
            actor.character,target.velocity[0],target.velocity[2],
            speed>0.02F?1.0F:0.0F,
            target.position[0],target.position[1],target.position[2],
            target.yaw,state.moveType,error,actor.recoveryPoseAt+hybrid_motion::kStaleAfterMs)) {
            relinquish();continue;
        }
        actor.recoveryNativeArmed=actor.recoveryFollowing=true;
    }
}
// The retail NPC motor owns ordinary frame-by-frame locomotion and animation.
// Never teleport the physics body for centimetre-size interpolation errors:
// forcing Character/registry pose after every retail step fights root motion,
// resetting stride and producing visible Move/Idle hesitation. Authoritative
// placement is a last-resort, sustained multi-metre recovery only.
// Vent entry/exit clips (0x12) and passages (0x4B-0x4D) translate the Host
// Alien by native root motion and its own passage driver, while the Client
// replay of the same cycles stays in place (diag 213331: Host crossed 15-30 m
// of vent; Client stayed at the mouth, played the exit clip there, then the
// 3.5 m emergency snapped it 15.8/31.7 m right onto the player before a kill).
// Carry the Client body along the Host pose for these non-paired actions.
// Paired kills keep native alignment and are never touched here.
bool FollowHostActionPose(Actor& actor) noexcept {
    if(!actor.character||actor.duplicate||actor.faulted||!actor.initialPlacementReady||
        !actor.remoteBirth||actor.directPoseBirth!=actor.remoteBirth||
        PvPCharacterFenced(actor.character)||PairReservedCharacter(actor.character)||
        PairSourceBusy(actor.character)||
        (actor.desired.executionFlags&ExecutionPaired)!=0U||
        (actor.received.executionFlags&ExecutionPaired)!=0U||
        health::Terminal(actor.desired.health)||health::Terminal(actor.recoveryPose.health))return false;
    std::uintptr_t cycle{};std::uint8_t cycleState{};
    if(!Read(actor.character+0x36CU,cycle)||!cycle||!Read(cycle+0x1AU,cycleState))return false;
    const bool scripted=cycleState==0x12U;
    const bool passage=cycleState==0x4BU||cycleState==0x4CU||cycleState==0x4DU;
    // Any other Host-ordered native action (e.g. state 0x57) plays its own
    // root motion on each PC; the Host body moved 1.5 m forward while the
    // Client's went 1.3 m back (diag 074748). Locomotion (1/0x37/0x38/0x3B)
    // stays with the direct motor; paired states are excluded above.
    const bool locomotion=cycleState==1U||cycleState==0x37U||cycleState==0x38U||cycleState==0x3BU;
    const bool action=!scripted&&!passage&&!locomotion&&cycleState!=0x4EU&&cycleState!=0x4FU;
    if(!scripted&&!passage&&!action)return false;
    std::uint32_t liveRuntime{},liveEntityA{},liveEntityB{};
    if(!Read(actor.character+0x50U,liveRuntime)||liveRuntime!=actor.runtimeId||
        !Read(actor.character+0x54U,liveEntityA)||liveEntityA!=actor.entityA||
        !Read(actor.character+0x58U,liveEntityB)||liveEntityB!=actor.entityB)return true;
    const auto& state=actor.recoveryPose;
    if(state.birth!=actor.remoteBirth||state.sessionCookie!=gCurrent.sessionCookie||
        state.worldGeneration!=gCurrent.worldGeneration||(state.stateFlags&PoseValid)==0U)return true;
    hybrid_motion::Target target{};
    if(!actor.directPoseTimeline.SampleAt(PreciseTickMs(),target)||target.stale)return true;
    float local[3]{};
    if(!Read(actor.character+kPositionOffset,local[0])||!Read(actor.character+kPositionOffset+4U,local[1])||
        !Read(actor.character+kPositionOffset+8U,local[2]))return true;
    const float dx=target.position[0]-local[0],dy=target.position[1]-local[1],dz=target.position[2]-local[2];
    const float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
    // Generic actions keep their native root motion inside 0.25 m.
    if(!std::isfinite(distance)||distance<(action?0.25F:0.03F))return true;
    // Never jump the physics body over several metres in a single frame,
    // even inside a vent. A delayed passage must approach at bounded steps;
    // the old uncapped jump could materialize the Alien on a living survivor.
    const float cap=passage?0.35F:(action?0.10F:0.15F);
    const float scale=std::min(1.0F,cap/distance);
    const float pose[3]{local[0]+dx*scale,local[1]+dy*scale,local[2]+dz*scale};
    if(character_direct_step_runtime::ApplyAIRecoveryTransform(actor.character,pose,target.yaw))
        gDirectPoseActionFollowFrames.fetch_add(1U,std::memory_order_relaxed);
    return true;
}
void ReconcileDirectPoseOnEngineThread() noexcept {
    if(!Installed()||!EngineThread()||!Stable()||!Client() ||
        gGeneration!=gGenerations->CurrentGeneration())return;
    constexpr float kEmergencyDistance=3.5F;
    constexpr ULONGLONG kEmergencyDwellMs=750ULL;
    constexpr ULONGLONG kEmergencyCooldownMs=3000ULL;
    const auto now=PreciseTickMs();
    for(auto& actor:gActors) {
        if(FollowHostActionPose(actor))continue;
        if(!actor.character||!actor.recoveryNativeArmed||!actor.recoveryFollowing||
            !character_direct_step_runtime::Snapshot(actor.character).armed||
            actor.duplicate||actor.faulted||!actor.initialPlacementReady||
            actor.directPoseBirth!=actor.remoteBirth||PvPCharacterFenced(actor.character)||
            ExecutionOwnedCharacter(actor.character)||PairSourceBusy(actor.character)||
            PairReservedCharacter(actor.character)||
            (actor.desired.executionFlags&ExecutionPaired)!=0U ||
            (actor.received.executionFlags&ExecutionPaired)!=0U ||
            ProxyWithdrawn(actor.desired)||ProxyWithdrawn(actor.recoveryPose)||
            health::Terminal(actor.desired.health)||health::Terminal(actor.recoveryPose.health)) {
            actor.directPoseDivergenceSince=0U;actor.directPoseAnchorAt=actor.directPoseAssistAt=0U;continue;
        }
        std::uint32_t liveRuntime{},liveEntityA{},liveEntityB{};
        if(!Read(actor.character+0x50U,liveRuntime)||liveRuntime!=actor.runtimeId||
            !Read(actor.character+0x54U,liveEntityA)||liveEntityA!=actor.entityA||
            !Read(actor.character+0x58U,liveEntityB)||liveEntityB!=actor.entityB) {
            actor.directPoseDivergenceSince=0U;actor.directPoseAnchorAt=actor.directPoseAssistAt=0U;continue;
        }
        hybrid_motion::Target target{};
        if(!actor.directPoseTimeline.SampleAt(PreciseTickMs(),target)||target.stale||
            target.nativeAction||target.discontinuity) {
            actor.directPoseDivergenceSince=0U;actor.directPoseAnchorAt=actor.directPoseAssistAt=0U;continue;
        }
        const auto& state=actor.recoveryPose;
        if(state.birth!=actor.remoteBirth||state.sessionCookie!=gCurrent.sessionCookie||
            state.worldGeneration!=gCurrent.worldGeneration||
            (state.stateFlags&PoseValid)==0U) {
            actor.directPoseDivergenceSince=0U;actor.directPoseAnchorAt=actor.directPoseAssistAt=0U;continue;
        }
        std::uintptr_t cycle{},table{};
        std::uint8_t cycleState{};
        if(!Read(actor.character+0x36CU,cycle)||!cycle||!Read(cycle,table)||
            table<gBase||!Read(cycle+0x1AU,cycleState)||
            !HybridLocomotionCycleAllowed(static_cast<std::uint32_t>(table-gBase),cycleState)) {
            actor.directPoseDivergenceSince=0U;actor.directPoseAnchorAt=actor.directPoseAssistAt=0U;continue;
        }
        float local[3]{};
        if(!Read(actor.character+kPositionOffset,local[0])||
            !Read(actor.character+kPositionOffset+4U,local[1])||
            !Read(actor.character+kPositionOffset+8U,local[2])) {
            actor.directPoseDivergenceSince=0U;actor.directPoseAnchorAt=actor.directPoseAssistAt=0U;continue;
        }
        const float dx=target.position[0]-local[0];
        const float dy=target.position[1]-local[1];
        const float dz=target.position[2]-local[2];
        const float distance2=dx*dx+dy*dy+dz*dz;
        // Kinematic tracking. Retail root motion often does not translate the
        // Client Alien at all outside steady runs (diag 205545: measured speed
        // 0 with demand 1-5 m/s). A dead-zone assist (516-520) made a sawtooth:
        // stall to 0.20 m, burst at Host speed + 1.5 m/s back to 0.15 m, stall
        // again, which read as stutter (diag 085355). The interpolated Host pose
        // is already smooth (100 ms delay), so within 0.75 m the body is placed
        // on it every frame; the native cycle keeps animating from the demand.
        // Beyond that, approach at most Host speed + 3 m/s (0.30 m/frame).
        constexpr float kTrackDistance=0.75F, kTrackEpsilon=0.005F, kCatchUpMaximumStep=0.30F;
        const float frameSeconds=actor.directPoseAssistAt&&now>actor.directPoseAssistAt
            ? std::min(static_cast<float>(now-actor.directPoseAssistAt)/1000.0F,0.05F) : 0.0F;
        actor.directPoseAssistAt=now;
        const float planar=std::sqrt(dx*dx+dz*dz);
        const float spatial=std::sqrt(distance2);
        if(std::isfinite(planar)&&std::isfinite(spatial)&&spatial>kTrackEpsilon&&
            planar<kEmergencyDistance&&frameSeconds>0.0F) {
            float scale=1.0F;
            if(planar>kTrackDistance) {
                const float hostSpeed=std::hypot(target.velocity[0],target.velocity[2]);
                scale=std::min({1.0F,(hostSpeed+3.0F)*frameSeconds/planar,kCatchUpMaximumStep/planar});
            }
            const float pose[3]{local[0]+dx*scale,local[1]+dy*std::min(1.0F,scale*2.0F),local[2]+dz*scale};
            if(character_direct_step_runtime::ApplyAIRecoveryTransform(actor.character,pose,target.yaw))
                gDirectPoseAssistFrames.fetch_add(1U,std::memory_order_relaxed);
        }
        if(!std::isfinite(distance2)||distance2<kEmergencyDistance*kEmergencyDistance) {
            actor.directPoseDivergenceSince=0U;continue;
        }
        if(!actor.directPoseDivergenceSince) {
            actor.directPoseDivergenceSince=now;continue;
        }
        if(now-actor.directPoseDivergenceSince<kEmergencyDwellMs ||
            (actor.directPoseLastEmergencyCorrection &&
             now-actor.directPoseLastEmergencyCorrection<kEmergencyCooldownMs))continue;
        const float pose[3]{target.position[0],target.position[1],target.position[2]};
        if(character_direct_step_runtime::ApplyAIRecoveryTransform(actor.character,pose,target.yaw)) {
            actor.directPoseLastEmergencyCorrection=now;
            actor.directPoseDivergenceSince=0U;actor.directPoseAnchorAt=0U;
            gDirectPoseEmergencyCorrections.fetch_add(1U,std::memory_order_relaxed);
        }
    }
}
void ObservePlayerRepresentationSpawn(const std::uint32_t spawnKey) noexcept {
    if (spawnKey == 0U || gGenerations == nullptr) return;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwnedSpawnKeys[i].load(std::memory_order_acquire) == spawnKey &&
            gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed) == generation) return;
    }
    for (std::size_t i = 0U; i < kActors; ++i) {
        const auto value = gOwnedSpawnKeys[i].load(std::memory_order_relaxed);
        const auto valueGeneration = gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed);
        if (value != 0U && valueGeneration == generation) continue;
        // Clear the publication before recycling a stale-generation slot.
        gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
        gOwnedSpawnKeyGeneration[i].store(generation, std::memory_order_relaxed);
        gOwnedSpawnKeys[i].store(spawnKey, std::memory_order_release);
        gPlayerRepresentationsRegistered.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
}
void ObservePlayerRepresentation(const std::uintptr_t c) noexcept {
    if (c == 0U || gGenerations == nullptr) return;
    std::uint32_t runtimeId{};
    if (Read(c + 0x50U, runtimeId)) ObservePlayerRepresentationSpawn(runtimeId);
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i) {
        auto& owned = gOwned[i];
        auto value = owned.load(std::memory_order_relaxed);
        if (value == c || ((value == 0U || gOwnedGeneration[i].load() != generation) &&
                owned.compare_exchange_strong(value, c))) {
            gOwnedGeneration[i].store(generation, std::memory_order_release);
            break;
        }
    }
    if (!EngineThread()) return;
    if (auto* actor = Find(c); actor != nullptr) {
        // Never stop CharacterDirectStep here: this slot is owned by PlayerSync.
        RevokeTransactionActor(*actor);
        *actor = {};
        gProvisionalPlayerActorsRetired.fetch_add(1U, std::memory_order_relaxed);
    }
}
void BeforeCharacterDestruction(const std::uintptr_t c) noexcept {
    InvalidatePairTransactions(c);
    if(!EngineThread()||!Stable()||gGeneration>UINT32_MAX)return;
    const auto* actor=Find(c);std::uint32_t runtime{};
    if(!actor||actor->duplicate||!Read(c+0x50U,runtime)||runtime!=actor->runtimeId)return;
    static_cast<void>(host_producer::BeforeNativeActorDestruction(c,runtime));
    character_incarnations::Ticket ticket{};
    static_cast<void>(character_incarnations::NativeRegistry().Find(gGeneration,c,ticket));
    execution_runtime::BeforeActorDestruction({c,runtime,static_cast<std::uint32_t>(gGeneration),ticket.serial});
}
void ForgetCharacter(const std::uintptr_t c) noexcept {
    if (EngineThread()) ForgetPvPLease(c);
    InvalidatePairTransactions(c);
    InvalidatePairCharacter(c);
    const bool playerRepresentation = RegisteredPlayerRepresentation(c);
    std::uint32_t runtimeId{};
    const bool hasRuntimeId = playerRepresentation && Read(c + 0x50U, runtimeId);
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwned[i].load(std::memory_order_relaxed) != c) continue;
        gOwned[i].store(0U, std::memory_order_release);
        gOwnedGeneration[i].store(0U, std::memory_order_relaxed);
    }
    if (hasRuntimeId) {
        for (std::size_t i = 0U; i < kActors; ++i) {
            if (gOwnedSpawnKeys[i].load(std::memory_order_relaxed) != runtimeId) continue;
            gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
            gOwnedSpawnKeyGeneration[i].store(0U, std::memory_order_relaxed);
        }
    }
    if (!EngineThread()) return;
    for (auto& navigation : gParticipantNavigation)
        if (navigation.initialized && navigation.character == c)
            static_cast<void>(ReleaseParticipantNavigation(navigation));
    if (auto* actor = Find(c); actor != nullptr) {
        RevokeTransactionActor(*actor);
        host_producer::InvalidateNativeActor(c,actor->runtimeId);
        if (!playerRepresentation) static_cast<void>(Release(*actor));
        // A callback can destroy an actor while a producer is unwinding. End
        // its diagnostic capture window before the local address is recycled.
        *actor = {};
    }
}
void ForgetSpawnKey(const std::uint32_t id) noexcept {
    if (id == 0U) return;
    if (EngineThread() && gPvPLease.runtimeId == id) ForgetPvPLease();
    // The manager's key-based destroy wrapper runs BEFORE FullDestroy. Drain
    // while its actor/ownership mapping is still live, rather than revoking it
    // here and making the later FullDestroy preflight unable to find it.
    if(EngineThread()&&Stable())for(const auto& actor:gActors)
        if(actor.character&&actor.runtimeId==id)BeforeCharacterDestruction(actor.character);
    for (auto& slot : gPairSources)
        if (slot.runtimeId.load(std::memory_order_acquire) == id) slot.character.store(0U, std::memory_order_release);
    for (auto& slot : gPairParticipants)
        if (slot.runtimeId.load(std::memory_order_acquire) == id) slot.character.store(0U, std::memory_order_release);
    const auto generation = gGenerations == nullptr ? 0U : gGenerations->CurrentGeneration();
    const bool playerRepresentation = RegisteredPlayerSpawnKey(id);
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwnedSpawnKeys[i].load(std::memory_order_relaxed) != id ||
            (generation != 0U && gOwnedSpawnKeyGeneration[i].load() != generation)) continue;
        gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
        gOwnedSpawnKeyGeneration[i].store(0U, std::memory_order_relaxed);
    }
    for (std::size_t i = 0U; i < kActors; ++i) {
        const auto character = gOwned[i].load(std::memory_order_relaxed);
        std::uint32_t runtimeId{};
        if (character == 0U || !Read(character + 0x50U, runtimeId) || runtimeId != id) continue;
        gOwned[i].store(0U, std::memory_order_release);
        gOwnedGeneration[i].store(0U, std::memory_order_relaxed);
    }
    if (!EngineThread()) return;
    for (auto& navigation : gParticipantNavigation)
        if (navigation.initialized && navigation.runtimeId == id)
            static_cast<void>(ReleaseParticipantNavigation(navigation));
    for (auto& actor : gActors) {
        if (actor.character == 0U || actor.runtimeId != id) continue;
        RevokeTransactionActor(actor);
        host_producer::InvalidateNativeActor(actor.character,actor.runtimeId);
        if (!playerRepresentation) static_cast<void>(Release(actor));
        actor = {};
    }
}
void TransportLost() noexcept {
    execution_runtime::Retire();
    host_producer::RetireHost();
    health::SetObservingHost(false);
    combat::SetMode(combat::Mode::FrozenClient);
    combat::ResetObservations();
    std::lock_guard lock(gMailboxMutex);
    if (gRequested.role == Role::Client || Client()) gRequested.role = Role::FrozenClient;
    else gRequested.role = Role::Retail;
    gConfigurationPending = true; gIncomingUsed.fill(false);
    for (auto& queue : gIncomingOutcomes) queue.clear();
    gLastQueuedOutcome.fill({}); gLastQueuedDamage.fill({}); gLastQueuedParticipantDamage.fill({}); gLastQueuedShot.fill({}); gIncomingOutcomeCount = 0U;
    {
        std::lock_guard impactLock(gImpactMailboxMutex);
        gOutgoingImpactRead = gOutgoingImpactWrite = 0U;
        gIncomingImpactRead = gIncomingImpactWrite = 0U;
        gImpactSequence = 0U;
        gLastImpactSequence.fill({});
    }
}
void BeginLogicalShutdown() noexcept {
    static_cast<void>(mesh_storage::Retire());
    health::SetObservingHost(false);
    if (gDispatcher != nullptr) gShutdownEngineThread.store(gDispatcher->EngineThreadId());
    gStopping.store(true, std::memory_order_release);
    TransportLost();
    gRole.store(Client() ? Role::FrozenClient : Role::Retail, std::memory_order_release);
}
bool OwnsAI() noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    return Installed() && role != Role::Retail;
}
bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }

bool PairedParticipantVictim(const std::uintptr_t character) noexcept {
    if (character == 0U) return false;
    const auto now = GetTickCount64();
    for (std::size_t i = 0U; i < kPairedParticipantVictimSlots; ++i)
        if (gPairedParticipantVictims[i].load(std::memory_order_acquire) == character &&
            now < gPairedParticipantVictimUntil[i].load(std::memory_order_acquire)) return true;
    return false;
}
bool AlienPvPCaptureVictim(const std::uintptr_t character) noexcept {
    if(!EngineThread() || !gPvPLease.owned || gPvPLease.releasing ||
        !PvPLeaseScopeCurrent() || !PvPLeaseLiveIdentity() || !PairedParticipantVictim(character))return false;
    const auto* actor=Find(gPvPLease.ticket.character);
    std::uintptr_t cycle{};std::uint8_t state{};
    return actor && !actor->faulted && !actor->duplicate &&
        ParticipantForPeer(actor->desired.interactionTargetPeer)==character &&
        Read(actor->character+0x36CU,cycle) && cycle && Read(cycle+0x1AU,state) && state==78U;
}
std::uintptr_t ResolveLivingParticipant(const std::uint64_t peer,
    const std::uint32_t nativeGeneration) noexcept {
    if (!EngineThread() || !Stable() || gCurrent.nativeGeneration != nativeGeneration) return 0U;
    const auto character = ParticipantForPeer(peer);
    return LivingCharacter(character) ? character : 0U;
}

// No native Character is dereferenced by this helper; the local incarnation
// registry is our lifetime authority. Every call occurs on the certified
// engine thread, including ManagerUpdate/BT detours.
namespace {
// Fencing is tied to the physical native lifetime, NOT to the lobby
// configuration. Unbinding a disconnected Host must not wake an Alien whose
// old movement requests are still in flight in that same retail world.
bool PvPLeaseLifetimeCurrent() noexcept {
    return gPvPLease.fenced && EngineThread() && gGenerations != nullptr &&
        gPvPLease.nativeGeneration != 0U &&
        gGenerations->CurrentGeneration() == gPvPLease.nativeGeneration &&
        character_incarnations::NativeRegistry().Current(gPvPLease.ticket);
}
bool PvPLeaseScopeCurrent() noexcept {
    return PvPLeaseLifetimeCurrent() && Installed() && Stable() &&
        gPvPLease.sessionCookie != 0U && gPvPLease.sessionCookie == gCurrent.sessionCookie &&
        gPvPLease.nativeGeneration == gCurrent.nativeGeneration &&
        gPvPLease.worldGeneration == gCurrent.worldGeneration;
}
bool PvPCharacterFenced(const std::uintptr_t character) noexcept {
    return character != 0U && gPvPLease.ticket.character == character &&
        PvPLeaseLifetimeCurrent();
}
void ForgetPvPLease(const std::uintptr_t destroyedCharacter) noexcept {
    if (destroyedCharacter != 0U && gPvPLease.ticket.character != destroyedCharacter) return;
    DisarmPvPBrain();
    gPvPTechnicalPeer.store(0U,std::memory_order_release);
    // Partial input/camera installation must also be undone, not only Drives().
    alien_pvp_local::Reset(gGenerations!=nullptr &&
        gGenerations->CurrentGeneration()==gPvPLease.nativeGeneration,false);
    alien_pvp::Reset(false);
    gPvPLease={};
}
bool PvPLeaseLiveIdentity() noexcept {
    if (!PvPLeaseLifetimeCurrent()) return false;
    std::uint32_t a{}, b{}, type{}, runtime{};
    std::uintptr_t tree{}, owner{};
    return CharacterIdentity(gPvPLease.ticket.character,a,b,type) && type == 1U &&
        a == gPvPLease.entityA && b == gPvPLease.entityB &&
        Read(gPvPLease.ticket.character+0x50U,runtime) && runtime == gPvPLease.runtimeId &&
        Read(gPvPLease.ticket.character+0x34D8U,tree) && tree == gPvPLease.tree &&
        Read(tree+0xCU,owner) && owner == gPvPLease.ticket.character;
}
AlienPvPLeaseStatus PvPStatus(const AlienPvPLeasePhase phase) noexcept {
    AlienPvPLeaseStatus status{};
    status.phase=phase;
    status.ownerPeer=gPvPLease.ownerPeer;
    status.entityA=gPvPLease.entityA;
    status.entityB=gPvPLease.entityB;
    status.runtimeId=gPvPLease.runtimeId;
    status.nativeGeneration=gPvPLease.nativeGeneration;
    status.worldGeneration=gPvPLease.worldGeneration;
    status.boundaryOffset=gPvPLease.boundaryOffset;
    status.boundaryValue=gPvPLease.boundaryValue;
    status.retainedKind=gPvPLease.retainedKind;status.retainedStatus=gPvPLease.retainedStatus;
    status.retainedRetired=gPvPLease.retainedRetired;
    status.isLocalOwner=gPvPLease.ownerPeer!=0U &&
        gPvPLease.ownerPeer==gCurrent.localPeer;
    status.localPilotReady=status.isLocalOwner &&
        alien_pvp_local::Drives(gPvPLease.ticket.character,gPvPLease.nativeGeneration);
    if (status.isLocalOwner) {
        const auto pilot=alien_pvp_local::Snapshot();
        status.pilotInputIsolated=pilot.inputIsolated;
        status.pilotCameraBound=pilot.cameraBound;
        status.pilotTechnicalHidden=pilot.technicalHidden;
        status.pilotMoving=pilot.moving;
    }
    return status;
}
// The native movement scheduler can be idle while its N navigation cache
// still owns list Refs. Copy the already-certified transactional drain gates
// instead of pretending empty scheduler counts imply a safe possession.
bool PvPTryClearDrainedNavigation(Actor& actor,
    const execution::OwnershipBoundaryFailure failure) noexcept {
    if (failure.offset != 0x10004U && failure.offset != 0x10014U &&
        failure.offset != 0x10044U) return false;
    if (gPvPLease.navigationDrainEntered || !PvPLeaseScopeCurrent() ||
        !PvPLeaseLiveIdentity() || PairSourceBusy(actor.character) ||
        !UnownedCleanupAllowed(actor)) return false;
    std::uintptr_t movement{}, control{};
    if (!Read(actor.character + 0x42A8U,movement) || movement == 0U ||
        !Read(actor.character + 0x42ACU,control) || control == 0U) return false;
    const auto navigation=movement+0xC4U;
    std::uintptr_t owner{};
    if (!Read(navigation+0x64U,owner) || owner!=movement) return false;
    for(const auto offset:{0x2CD8U,0x2CE4U}) {
        std::uint32_t active{};
        if (!Read(actor.character+offset,active) || active) return false;
    }
    for(const auto offset:{0x24U,0x2CU,0x38U,0x40U,0x5CU,0x60U}) {
        std::uint32_t active{};
        if (!Read(navigation+offset,active) || active) return false;
    }
    // Validate both retail-owned list sentinels before calling their
    // destructors. Never overwrite N+44 or fabricate a cleared native Ref.
    for(const auto offset:{0x28U,0x34U}) {
        std::uintptr_t sentinel{}, next{}, previous{};
        if (!Read(navigation+offset,sentinel) || !sentinel ||
            !Read(sentinel,next) || !Read(sentinel+4U,previous) ||
            next!=sentinel || previous!=sentinel) return false;
    }
    const auto current=[&]() noexcept {
        std::uintptr_t m{}, c{};
        return PvPLeaseScopeCurrent() && PvPLeaseLiveIdentity() &&
            !PairSourceBusy(actor.character) && UnownedCleanupAllowed(actor) &&
            Read(actor.character+0x42A8U,m) && m==movement &&
            Read(actor.character+0x42ACU,c) && c==control;
    };
    if (!current()) return false;
    gPvPLease.navigationDrainEntered=true;
    if (!ResetDrainedNavigation(navigation) || !current()) return false;
    if (!ClearDrainedNavigation(navigation,false) || !current()) return false;
    if (!ClearDrainedNavigation(navigation,true) || !current()) return false;
    // A completed drain may be repeated if a late native result repopulates
    // history. A partial/faulted drain keeps this fence and cannot be retried.
    gPvPLease.navigationDrainEntered=false;
    return true;
}

bool PvPRetireTerminalRequest(std::uintptr_t scheduler,std::uintptr_t request,std::uintptr_t control) noexcept {
    struct Ref {std::uintptr_t object,control;} ref{request,control};
    __try {
        // Pin independently of the scheduler list: native cancellation and
        // collection may release its last list Ref. Use the retail Ref dtor.
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(control+4U));
        reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x00283DB0U)(reinterpret_cast<void*>(request));
        if(*reinterpret_cast<const std::uint8_t*>(request+0x18U)!=0U)return false;
        // 006838D0 returns early for status5 (replaced). 00683470 performs
        // the missing retained-list removal once Cancel cleared retention.
        reinterpret_cast<void(__thiscall*)(void*,void*)>(gBase+0x00283470U)(reinterpret_cast<void*>(scheduler),&ref);
        const bool retired=*reinterpret_cast<const std::uint32_t*>(request+0xCU)==6U;
        reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x0000D770U)(&ref);
        return retired;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool PvPTryRetireRetainedMovement(Actor& actor) noexcept {
    if(!gPvPLease.drained || gPvPLease.owned || gPvPLease.retainedDrainFault ||
        !PvPLeaseScopeCurrent() || !PvPLeaseLiveIdentity() ||
        PairSourceBusy(actor.character) || !UnownedCleanupAllowed(actor))return false;
    const auto scheduler=actor.character+0x2CD0U;
    // Exact unhooked collector body; no vtable-only inference about requests.
    std::array<std::uint8_t,8> code{};
    if(!Read(gBase+0x00283470U,code) || code!=std::array<std::uint8_t,8>{0x53,0x8B,0x5C,0x24,0x08,0x56,0x8B,0x33})return false;
    for(unsigned budget=0;budget<64U;++budget) {
        std::uintptr_t owner{},sentinel{},node{},previous{},request{},control{},type{},requestOwner{},requestControl{};
        std::uint32_t active{},count{},kind{},state{},strong{};
        if(!PvPLeaseLiveIdentity() || !Read(scheduler,owner) || owner!=actor.character ||
            !Read(scheduler+8U,active) || active || !Read(scheduler+0x14U,count) || count>64U)return false;
        if(!count)return true;
        if(!Read(scheduler+0x10U,sentinel)||!sentinel||!Read(sentinel,node)||!node||node==sentinel||
            !Read(node+4U,previous)||previous!=sentinel||!Read(node+8U,request)||!request||
            !Read(node+12U,control)||!control||!Read(control+4U,strong)||!strong||strong>=0x100000U||
            !Read(request+4U,requestControl)||requestControl!=control||
            !Read(request,type)||!Read(request+0x14U,requestOwner)||requestOwner!=scheduler||
            !Read(request+0x10U,kind)||!Read(request+0xCU,state))return false;
        gPvPLease.retainedKind=kind;gPvPLease.retainedStatus=state;
        std::uintptr_t expected{};
        switch(kind) {
        case 1U:expected=0xEB1A70U;break;case 2U:expected=0xEB1A60U;break;
        case 3U:expected=0xEB1A50U;break;case 4U:expected=0xEB19CCU;break;
        case 7U:expected=0xEB19BCU;break;case 9U:expected=0xEB19ACU;break;
        // Only the backstage reservation this lease itself cancelled.
        case 5U:if(!gPvPLease.backstageCancelEntered)return false;expected=0xEB198CU;break;
        default:return false; // No action, placement, pair or scheduler-mode request.
        }
        if(type!=gBase+expected || state<3U || state>5U)return false;
        if(!PvPRetireTerminalRequest(scheduler,request,control)) {
            gPvPLease.retainedDrainFault=true;return false;
        }
        ++gPvPLease.retainedRetired;
    }
    return false;
}
// The fence stops new BT decisions, not a request already running. Two seen
// on the Host at spawn (465/466), both leaving possession Draining:
// - the spawn Move (kind1, issued on the Alien's first frame) whose route
//   enters a vent: about 45s of cycle75 traversal before it ends;
// - then the Alien parked backstage: cycle77 timed hold (00401520, no
//   traversal path/listener) under a running kind5 reservation that only the
//   fenced BT would ever clear.
// Cancel exactly that single active request through the retail request
// cancel, once per request; the retained-list collector then retires it like
// any other terminal request. Cycle75/76 traversal is never cancelled.
bool PvPTryCancelActiveRequest(Actor& actor) noexcept {
    if(!gPvPLease.drained || gPvPLease.owned || gPvPLease.retainedDrainFault ||
        !PvPLeaseScopeCurrent() || !PvPLeaseLiveIdentity() ||
        PairSourceBusy(actor.character) || !UnownedCleanupAllowed(actor)){gPvPLease.retainedKind=0x101U;return false;}
    const auto scheduler=actor.character+0x2CD0U;
    std::uintptr_t owner{},cycle{},cycleType{},sentinel{},node{},request{},type{},requestOwner{};
    std::uint32_t count{},kind{},state{};std::uint8_t cycleState{},a{},b{};
    // Active list (+2CD4 sentinel, +2CD8 count) is an MSVC std::list of raw
    // requests: node +0 next, +8 request (same walk as BootstrapNativeActor).
    // Diagnostic: a refusal leaves 0x100+step in retainedKind.
    if(!Read(scheduler,owner)||owner!=actor.character||
        !Read(actor.character+0x36CU,cycle)||!cycle||!Read(cycle,cycleType)||!Read(cycle+0x1AU,cycleState)||
        !Read(actor.character+0x2DAU,a)||!Read(actor.character+0x2DBU,b)){gPvPLease.retainedKind=0x102U;return false;}
    if(!Read(scheduler+4U,sentinel)||!sentinel||!Read(scheduler+8U,count)||count!=1U||
        !Read(sentinel,node)||!node||node==sentinel||!Read(node+8U,request)||!request){gPvPLease.retainedKind=0x103U;return false;}
    if(!Read(request,type)||!Read(request+0x10U,kind)||
        !Read(request+0x14U,requestOwner)||requestOwner!=scheduler||
        !Read(request+0xCU,state)||state<1U||state>2U){gPvPLease.retainedKind=0x104U;return false;}
    if(request==gPvPLease.cancelledRequest)return false;
    const bool frontstageMove=!a&&!b&&(cycleState==55U||cycleState==56U||cycleState==59U)&&
        ((kind==1U&&type==gBase+0xEB1A70U)||(kind==2U&&type==gBase+0xEB1A60U)||(kind==3U&&type==gBase+0xEB1A50U));
    const bool backstageHold=(a||b)&&cycleType==gBase+0xDBB1B8U&&cycleState==0x4DU&&
        kind==5U&&type==gBase+0xEB198CU&&!gPvPLease.backstageCancelEntered;
    if(!frontstageMove&&!backstageHold){gPvPLease.retainedKind=0x110U+kind;gPvPLease.retainedStatus=cycleState;return false;}
    gPvPLease.retainedKind=kind;gPvPLease.retainedStatus=state;
    // One attempt per request: a fault or a request that survives its own
    // cancel stays visible as Draining instead of being cancelled in a loop.
    gPvPLease.cancelledRequest=request;
    if(kind==5U)gPvPLease.backstageCancelEntered=true;
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x00283DB0U)(reinterpret_cast<void*>(request));
    } __except(EXCEPTION_EXECUTE_HANDLER) {gPvPLease.retainedDrainFault=true;return false;}
    return true;
}

// Host-validated owner observations update only the exact possessed replica.
// Native setters restore visibility/collision when leaving backstage; no raw
// stage bits are fabricated and active pairs/portals retain their ownership.
bool PvPShowReplica(std::uintptr_t character) noexcept {
    __try {reinterpret_cast<void(__thiscall*)(void*,bool)>(gBase+0x0038B100U)(reinterpret_cast<void*>(character),false);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ReplicatePvPPose(const alien_pvp::Context& context,const alien_pvp::Frame& pose) noexcept {
    if(context.localOwner || !context.owned || !PvPLeaseScopeCurrent() || !PvPLeaseLiveIdentity() ||
        context.character!=gPvPLease.ticket.character || PairSourceBusy(context.character) ||
        PairReservedCharacter(context.character) || (pose.flags&alien_pvp::OwnerMeasured)==0U)return false;
    std::uintptr_t cycle{};std::uint8_t state{},a{},b{};
    if(!Read(context.character+0x36CU,cycle)||!cycle||!Read(cycle+0x1AU,state)||
        !Read(context.character+0x2DAU,a)||!Read(context.character+0x2DBU,b)||
        (state!=1U&&state!=55U&&state!=56U&&state!=57U&&state!=59U&&state!=75U&&state!=77U))return false;
    const bool backstage=(pose.flags&alien_pvp::Backstage)!=0U;
    const character_direct_step_runtime::MovementResourceLease resources(context.character);
    if(!resources)return false;
    if(!backstage) {
        std::uintptr_t withdrawal{};std::uint32_t withdrawState{};
        if(ResolveAlienWithdrawContext(context.character,withdrawal,withdrawState) && withdrawState &&
            !ApplyAlienWithdrawEntry(withdrawal,0U))return false;
    }
    if((a||b)!=backstage && !SetCharacterBackstage(context.character,backstage))return false;
    if(!backstage) {
        std::uint8_t invisible{};
        if(!Read(context.character+0x2D9U,invisible))return false;
        if(invisible && !PvPShowReplica(context.character))return false;
    }
    const float position[]{pose.x,pose.y,pose.z};
    return character_direct_step_runtime::ApplyAIRecoveryTransform(context.character,position,pose.yaw);
}
#include "AISyncPvPActions.inl"

bool PvPTryDrain(AlienPvPLeaseStatus& status) noexcept {
    using Check=execution::OwnershipCheck;
    if (!PvPLeaseScopeCurrent() || !PvPLeaseLiveIdentity()) {
        gPvPLease.owned=false;
        status=PvPStatus(AlienPvPLeasePhase::Stale); return false;
    }
    auto* actor=Find(gPvPLease.ticket.character);
    if (!actor || actor->duplicate || actor->faulted || actor->runtimeId != gPvPLease.runtimeId) {
        gPvPLease.owned=false;
        status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement); return false;
    }
    if(gPvPLease.retainedDrainFault) {
        status=PvPStatus(AlienPvPLeasePhase::Draining);
        status.waitReason="retained-request-retirement-fault";return false;
    }
    if (gPvPLease.owned && !gPvPLease.releasing &&
        (alien_pvp_local::Drives(actor->character,gPvPLease.nativeGeneration) ||
         alien_pvp::Owns(actor->character,gPvPLease.nativeGeneration))) {
        // A live local pilot now owns an AIMP locomotion slot. Its legitimate
        // movement must never be mistaken for a leftover retail AI request
        // and drained by the periodic bind recheck. The incarnation and native
        // resource pins are still verified on every renewal.
        const character_direct_step_runtime::MovementResourceLease resources(actor->character);
        if (!resources) {
            status=PvPStatus(AlienPvPLeasePhase::Draining);return false;
        }
        status=PvPStatus(AlienPvPLeasePhase::Owned);
        return true;
    }
    if(PairSourceBusy(actor->character) || !UnownedCleanupAllowed(*actor)) {
        gPvPLease.owned=false;status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);
        status.waitReason=PairSourceBusy(actor->character)?"paired-action":"coop-ownership";return false;
    }
    const execution::OwnershipMemory memory{gBase,nullptr,
        [](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
        }};
    const execution::ActorLifetime lifetime{actor->character,actor->runtimeId,
        gPvPLease.nativeGeneration,gPvPLease.ticket.serial};
    execution::OwnershipBoundaryFailure failure{};
    // Fencing stops new AI decisions, but an already-running spawn passage
    // still owns its movement request. Cancelling it here strands cycle75/76
    // permanently, so neither replica can ever advertise readiness.
    if(execution::InspectAlienPossessionBoundary(memory,lifetime,false,&failure)!=Check::Ready) {
        gPvPLease.boundaryOffset=failure.offset;gPvPLease.boundaryValue=failure.value;
        status=PvPStatus(AlienPvPLeasePhase::Draining);
        status.waitReason="initial-native-cycle";return false;
    }
    if (!gPvPLease.drained) {
        // All per-actor BT decisions are already fenced, and the exact native
        // owner has no transaction/paired lease. Use retail destructors, not
        // a memcpy/zero of movement requests or the shared Alien controller.
        if (!QuiesceProxyExecution(*actor)) {
            status=PvPStatus(AlienPvPLeasePhase::Draining); return false;
        }
        if (!character_direct_step_runtime::ReleaseAIIntentOwnership(actor->character)) {
            status=PvPStatus(AlienPvPLeasePhase::Draining); return false;
        }
        gPvPLease.drained=true;
    }
    const character_direct_step_runtime::MovementResourceLease resources(actor->character);
    if (!resources) { status=PvPStatus(AlienPvPLeasePhase::Draining); return false; }
    failure={};
    auto check=execution::InspectAlienPossessionBoundary(memory,lifetime,true,&failure);
    // A finished retained request can also hold locomotion state56 open.
    // The non-quiet cycle proof above allows retiring its request, but the
    // final quiet proof still waits for the cycle to finish natively.
    if(check==Check::Waiting && (failure.offset==0x2CE4U ||
        (failure.offset==0x36CU && failure.value==56U)) && PvPTryRetireRetainedMovement(*actor)) {
        failure={};check=execution::InspectAlienPossessionBoundary(memory,lifetime,true,&failure);
    }
    if(check==Check::Waiting && (failure.offset==0x2CD8U || (failure.offset==0x36CU && failure.value==56U)) &&
        PvPTryCancelActiveRequest(*actor)) {
        failure={};check=execution::InspectAlienPossessionBoundary(memory,lifetime,true,&failure);
        if(check==Check::Waiting && failure.offset==0x2CE4U && PvPTryRetireRetainedMovement(*actor)) {
            failure={};check=execution::InspectAlienPossessionBoundary(memory,lifetime,true,&failure);
        }
    }
    if (check == Check::Waiting && PvPTryClearDrainedNavigation(*actor,failure)) {
        failure={};
        check=execution::InspectAlienPossessionBoundary(memory,lifetime,true,&failure);
    }
    gPvPLease.boundaryOffset=failure.offset;gPvPLease.boundaryValue=failure.value;
    if (check != Check::Ready) {
        gPvPLease.owned=false;
        status=PvPStatus(AlienPvPLeasePhase::Draining);
        status.waitReason=gPvPLease.retainedDrainFault?"retained-request-retirement-fault":
            failure.offset==0x2CE4U?"initial-retained-request":
            failure.offset>=0x10000U?"initial-navigation-retirement":"initial-scheduler-retirement";
        return false;
    }
    gPvPLease.owned=true;
    status=PvPStatus(AlienPvPLeasePhase::Owned);
    return true;
}
} // namespace

bool AcquireAlienPvPLease(const std::uint64_t ownerPeer, AlienPvPLeaseStatus& status) noexcept {
    status={};
    if (!ownerPeer || !Installed() || !EngineThread() || !Stable() || (gCurrent.flags & kAlienPvP)==0U ||
        gCurrent.role == Role::Retail || gCurrent.role == Role::FrozenClient ||
        !gCurrent.sessionCookie || !gCurrent.nativeGeneration || !gCurrent.worldGeneration ||
        !gGenerations || gGenerations->CurrentGeneration() != gCurrent.nativeGeneration) return false;
    if (gPvPLease.releasing) {
        status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);return false;
    }
    // Never replace a live fenced incarnation just because a different Alien
    // was observed in a vent or the lobby selected a different owner. The
    // old lease must be released or its original Character actually destroyed.
    if (gPvPLease.fenced && !PvPLeaseLifetimeCurrent()) ForgetPvPLease();
    if (gPvPLease.fenced && (!PvPLeaseScopeCurrent() || gPvPLease.ownerPeer != ownerPeer)) {
        status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);
        return false;
    }
    // Persist the selected role even before the map creates its Alien. The
    // local Amanda must be isolated during this wait, not offered as a survivor.
    if (!gPvPLease.fenced) {
        gPvPLease.ownerPeer=ownerPeer;gPvPLease.sessionCookie=gCurrent.sessionCookie;
        gPvPLease.nativeGeneration=gCurrent.nativeGeneration;gPvPLease.worldGeneration=gCurrent.worldGeneration;
    }
    AlienPvPProbe candidate{};
    if (!ProbeAlienPvPCandidate(candidate)) return false;
    if (!candidate.unique) {
        status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);
        status.waitReason=candidate.candidates==0U?"map-alien-not-created":"multiple-map-aliens";
        if (!gPvPLease.fenced)
            status.phase=candidate.candidates==0U?AlienPvPLeasePhase::WaitingForAlien:
                AlienPvPLeasePhase::Ambiguous;
        return false;
    }
    if (gPvPLease.fenced) {
        if (candidate.runtimeId != gPvPLease.runtimeId ||
            candidate.entityA != gPvPLease.entityA || candidate.entityB != gPvPLease.entityB ||
            !PvPLeaseLiveIdentity()) {
            status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);
            return false;
        }
        // A previous success is not permanent proof. Re-inspect scheduler
        // and navigation ownership even during a quiet long-lived round.
        return PvPTryDrain(status);
    }
    const auto character=FindCharacterByIdentity(candidate.entityA,candidate.entityB);
    if (!character) { status.phase=AlienPvPLeasePhase::WaitingForAlien; return false; }
    auto* actor=Admit(character);
    if (!actor || actor->duplicate || actor->faulted ||
        actor->runtimeId!=candidate.runtimeId || actor->characterClass!=1U ||
        !UnownedCleanupAllowed(*actor) || PairSourceBusy(character)) {
        status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);
        status.waitReason=!actor?"actor-missing":actor->duplicate?"actor-duplicate":actor->faulted?"actor-faulted":
            actor->runtimeId!=candidate.runtimeId?"actor-runtime-changed":actor->characterClass!=1U?"actor-class":
            PairSourceBusy(character)?"paired-action":"coop-ownership";
        return false;
    }
    character_incarnations::Ticket ticket{};
    auto& registry=character_incarnations::NativeRegistry();
    auto found=registry.Find(gCurrent.nativeGeneration,character,ticket);
    if (found==character_incarnations::Result::Missing || found==character_incarnations::Result::WrongGeneration)
        found=registry.BindExisting(gCurrent.nativeGeneration,character,candidate.runtimeId,ticket);
    if ((found!=character_incarnations::Result::Ok && found!=character_incarnations::Result::Duplicate) ||
        ticket.runtimeId!=candidate.runtimeId || !ticket.serial || !registry.Current(ticket)) {
        status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);
        status.waitReason="incarnation-not-current";return false;
    }
    gPvPLease.ticket=ticket;
    gPvPLease.tree=actor->tree;
    gPvPLease.ownerPeer=ownerPeer;
    gPvPLease.sessionCookie=gCurrent.sessionCookie;
    gPvPLease.nativeGeneration=gCurrent.nativeGeneration;
    gPvPLease.worldGeneration=gCurrent.worldGeneration;
    gPvPLease.entityA=candidate.entityA;
    gPvPLease.entityB=candidate.entityB;
    gPvPLease.runtimeId=candidate.runtimeId;
    gPvPLease.fenced=true;
    return PvPTryDrain(status);
}
bool ReleaseAlienPvPLease(AlienPvPLeaseStatus& status) noexcept {
    status={};
    if (!EngineThread()) return false;
    if (!gPvPLease.fenced) { ForgetPvPLease();status.phase=AlienPvPLeasePhase::Released; return true; }
    // A pending release may span frames. Never rearm the pilot while native
    // workers or paired cycles are still retiring.
    if(!gPvPLease.releasing) {
        gPvPTechnicalPeer.store(0U,std::memory_order_release);
        gPvPLease.releasing=true;
        const bool living=PvPLeaseLiveIdentity();
        alien_pvp_local::Reset(gGenerations!=nullptr &&
            gGenerations->CurrentGeneration()==gPvPLease.nativeGeneration,living);
        alien_pvp::Reset(living);
    }
    RetirePvPActions();
    if(pvp_actions::ResourcesPending()){status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);return false;}
    if (!PvPLeaseLifetimeCurrent()) {
        // Destruction or new physical world is the only safe proof that the
        // old fenced Character cannot receive any more retail decisions.
        ForgetPvPLease();
        status.phase=AlienPvPLeasePhase::Released;
        return true;
    }
    if (!PvPLeaseLiveIdentity()) {
        status=PvPStatus(AlienPvPLeasePhase::Stale);return false;
    }
    auto* actor=Find(gPvPLease.ticket.character);
    if (!actor || actor->duplicate || actor->faulted ||
        actor->runtimeId!=gPvPLease.runtimeId || PairSourceBusy(actor->character) ||
        !UnownedCleanupAllowed(*actor)) {
        status=PvPStatus(AlienPvPLeasePhase::AwaitingRetirement);return false;
    }
    // The owner must stop locally before the exact native movement boundary
    // can certify safe release. Repeated RELEASE probes may wait for retirement.
    if (alien_pvp_local::Drives(actor->character,gPvPLease.nativeGeneration))
        alien_pvp_local::Reset(true,true);
    // Native navigation may still retain worker Refs.
    if (!gPvPLease.owned && !PvPTryDrain(status)) return false;
    if(!character_direct_step_runtime::ReleaseAIIntentOwnership(actor->character)) {
        status=PvPStatus(AlienPvPLeasePhase::Draining);return false;
    }
    const character_direct_step_runtime::MovementResourceLease resources(actor->character);
    if (!resources) {status=PvPStatus(AlienPvPLeasePhase::Draining);return false;}
    const execution::OwnershipMemory memory{gBase,nullptr,
        [](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
        }};
    const execution::ActorLifetime lifetime{actor->character,actor->runtimeId,
        gPvPLease.nativeGeneration,gPvPLease.ticket.serial};
    execution::OwnershipBoundaryFailure failure{};
    if (execution::InspectAlienPossessionBoundary(memory,lifetime,true,&failure) !=
        execution::OwnershipCheck::Ready) {
        gPvPLease.boundaryOffset=failure.offset;
        gPvPLease.boundaryValue=failure.value;
        status=PvPStatus(AlienPvPLeasePhase::Draining);return false;
    }
    if (!character_direct_step_runtime::ReleaseAIIntentOwnership(actor->character)) {
        status=PvPStatus(AlienPvPLeasePhase::Draining);return false;
    }
    ForgetPvPLease();
    status.phase=AlienPvPLeasePhase::Released;
    return true;
}
void ConfirmAlienPvPDestruction(std::uintptr_t character,std::uint32_t runtime,
    std::uint64_t serial,std::uint32_t nativeGeneration) noexcept {
    pvp_actions::DonorDestroyed(character,runtime,serial,nativeGeneration);
}
bool SnapshotAlienVents(std::uint32_t page,std::string& json,int goTo,std::uint64_t incarnation) {
    if(page>=64U || !EngineThread() || !PvPLeaseScopeCurrent() || !PvPLeaseLiveIdentity() ||
        !gPvPLease.owned || gPvPLease.releasing || gPvPLease.ownerPeer!=gCurrent.localPeer ||
        !alien_pvp_local::Drives(gPvPLease.ticket.character,gPvPLease.nativeGeneration))return false;
    const auto reader=+[](std::uintptr_t at,void* out,std::size_t size) noexcept {
        return at && crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
    };
    alien_pvp::vents::Table table{};alien_pvp::vents::Point body{};
    if(!alien_pvp::vents::ReadTable(gPvPLease.ticket.character,reader,table) ||
        !Read(gPvPLease.ticket.character+0x70U,body))return false;
    if(goTo>=0 && (incarnation!=gPvPLease.ticket.serial || !alien_pvp_local::GoToVent(static_cast<std::uint32_t>(goTo))))return false;
    const auto ui=alien_pvp_local::VentSnapshot();
    const auto authority=alien_pvp::VentAuthority();
    json="AVSVENTS:{\"scope\":\""+std::to_string(gPvPLease.sessionCookie)+":"+
        std::to_string(gPvPLease.ownerPeer)+":"+std::to_string(gPvPLease.nativeGeneration)+":"+
        std::to_string(gPvPLease.worldGeneration)+":"+std::to_string(gPvPLease.ticket.serial)+"\",\"page\":"+
        std::to_string(page)+",\"count\":"+std::to_string(table.count)+",\"nearby\":"+std::to_string(ui.nearby)+
        ",\"selected\":"+std::to_string(ui.selected)+",\"open\":"+(ui.open?"true":"false")+
        ",\"backstage\":"+(ui.backstage?"true":"false")+",\"freeCamera\":"+(ui.freeCamera?"true":"false")+",\"x\":"+std::to_string(body[0])+
        ",\"y\":"+std::to_string(body[1])+",\"z\":"+std::to_string(body[2])+
        ",\"action\":"+std::to_string(static_cast<unsigned>(authority.action))+
        ",\"phase\":"+std::to_string(static_cast<unsigned>(authority.phase))+
        ",\"pending\":"+(alien_pvp::VentPending()?"true":"false")+
        ",\"controlReady\":"+(alien_pvp::ControlActive()?"true":"false")+",\"vents\":[";
    bool comma{};
    for(auto i=page*64U;i<std::min(table.count,(page+1U)*64U);++i) {
        alien_pvp::vents::Point point{};
        if(!alien_pvp::vents::ReadPoint(table,i,reader,point))continue;
        if(comma)json+=",";
        comma=true;
        json+="["+std::to_string(i)+","+std::to_string(point[0])+","+std::to_string(point[1])+","+std::to_string(point[2])+"]";
    }
    json+="]}";return true;
}

bool ProbeAlienPvPCandidate(AlienPvPProbe& result) noexcept {
    result = {};
    if (!Installed() || !EngineThread() || !Stable() ||
        gCurrent.nativeGeneration == 0U || gCurrent.worldGeneration == 0U ||
        gRole.load(std::memory_order_acquire) == Role::Retail)
        return false;

    result.nativeGeneration = gCurrent.nativeGeneration;
    result.worldGeneration = gCurrent.worldGeneration;
    // Enumerate the native Character registry, including currently active
    // characters that have not yet been admitted into AISync's actor cache.
    const bool scanned = ForEachCharacter([&](const std::uintptr_t character) noexcept {
        std::uint32_t entityA{}, entityB{}, type{}, runtimeId{};
        // A template-class match alone is insufficient: the retail Alien
        // must own a live behaviour manager/tree. The registry may enumerate
        // half-constructed or retiring Characters during vent transitions.
        std::uintptr_t selection{}, manager{}, tree{}, treeOwner{}, managerOwner{};
        if (CharacterIdentity(character, entityA, entityB, type) &&
            type == 1U && Eligible(character, type) &&
            Read(character + 0x50U, runtimeId) && runtimeId != 0U &&
            Read(character + 0x34D8U, tree) && tree != 0U &&
            Read(tree + 0xCU, treeOwner) && treeOwner == character &&
            Read(character + 0x34A8U, selection) && selection != 0U &&
            Read(selection + 4U, manager) && manager != 0U &&
            Read(manager + 0x128U, managerOwner) && managerOwner == character) {
            ++result.candidates;
            if (result.candidates == 1U) {
                result.entityA = entityA;
                result.entityB = entityB;
                result.runtimeId = runtimeId;
            }
        }
        return true;
    });
    if (!scanned) { result = {}; return false; }
    result.unique = result.candidates == 1U;
    if (!result.unique) {
        // No ambiguous or stale identity may be retained as a controller.
        result.entityA = result.entityB = result.runtimeId = 0U;
    }
    return true;
}

Counters DiagnosticsSnapshot() noexcept {
    Counters value{gObserved.load(), gApplied.load(), gDropped.load(), gRefused.load(),
        gSuspended.load(), gFaults.load(), gMissing.load()};
    value.managerUpdates = gManagerUpdates.load(); value.admitted = gAdmitted.load();
    value.identityRejected = gIdentityRejected.load(); value.treeUnavailable = gTreeUnavailable.load();
    value.ownerMismatch = gOwnerMismatch.load(); value.remoteQueued = gRemoteQueued.load();
    value.poseCaptured = gPoseCaptured.load(); value.poseApplied = gPoseApplied.load();
    value.poseRejected = gPoseRejected.load();
    value.intentCaptured = gIntentCaptured.load(); value.intentApplied = gIntentApplied.load();
    value.intentRejected = gIntentRejected.load(); value.intentRecoveries = gIntentRecoveries.load();
    value.actionCaptured = gActionCaptured.load();
    value.actionTransitions = gActionTransitions.load();
    value.executionCaptured = gExecutionCaptured.load();
    value.executionTransitions = gExecutionTransitions.load();
    value.executionReplayed = gExecutionReplayed.load();
    value.executionReplayRejected = gExecutionReplayRejected.load();
    value.roleTaskCaptured = gRoleTaskCaptured.load();
    value.roleTaskTransitions = gRoleTaskTransitions.load();
    value.lifecycleCaptured = gLifecycleCaptured.load();
    value.lifecycleApplied = gLifecycleApplied.load();
    value.lifecycleSnaps = gLifecycleSnaps.load();
    value.backstageCaptured = gBackstageCaptured.load();
    value.backstageTransitions = gBackstageTransitions.load();
    value.backstageApplied = gBackstageApplied.load();
    value.backstageRejected = gBackstageRejected.load();
    value.placementVerified = gPlacementVerified.load();
    value.placementRejected = gPlacementRejected.load();
    value.lifecycleRetries = gLifecycleRetries.load();
    value.executionBarriers = gExecutionBarriers.load();
    value.impactCaptured = gImpactCaptured.load();
    value.impactShooterRemaps = gImpactShooterRemaps.load();
    value.impactShooterUnresolved = gImpactShooterUnresolved.load();
    value.impactQueued = gImpactQueued.load();
    value.impactApplied = gImpactApplied.load();
    value.impactRejected = gImpactRejected.load();
    value.withdrawSamples = gWithdrawSamples.load();
    value.withdrawTransitions = gWithdrawTransitions.load();
    value.withdrawApplied = gWithdrawApplied.load();
    value.interactionCaptured = gInteractionCaptured.load();
    value.interactionApplied = gInteractionApplied.load();
    value.interactionRejected = gInteractionRejected.load();
    value.interactionRemapped = gInteractionRemapped.load();
    value.pairedTransitionCaptured = gPairedTransitionCaptured.load();
    value.pairedTransitionApplied = gPairedTransitionApplied.load();
    value.playerRepresentationsRegistered = gPlayerRepresentationsRegistered.load();
    value.provisionalPlayerActorsRetired = gProvisionalPlayerActorsRetired.load();
    value.participantCandidates = gParticipantCandidates.load();
    value.participantTargetRejected = gParticipantTargetRejected.load();
    value.participantCandidatePasses = gParticipantCandidatePasses.load();
    value.senseEventPasses = gSenseEventPasses.load();
    value.participantSignalLookups = gParticipantSignalLookups.load();
    value.participantSignalRemaps = gParticipantSignalRemaps.load();
    value.participantSignalRetailMissing = gParticipantSignalRetailMissing.load();
    value.participantSignalRetailMismatch = gParticipantSignalRetailMismatch.load();
    value.participantSignalAmbiguous = gParticipantSignalAmbiguous.load();
    value.lastParticipantSignalPeer = gLastParticipantSignalPeer.load();
    value.participantPairLookups = gParticipantPairLookups.load();
    value.participantPairRemaps = gParticipantPairRemaps.load();
    value.participantPairRetailMissing = gParticipantPairRetailMissing.load();
    value.participantPairAmbiguous = gParticipantPairAmbiguous.load();
    value.lastParticipantPairPeer = gLastParticipantPairPeer.load();
    value.pairedInitializeSucceeded = gPairedInitializeSucceeded.load();
    value.pairedInitializeFailed = gPairedInitializeFailed.load();
    value.pairedCycleReleased = gPairedCycleReleased.load();
    value.pairedObserverCompletions = gPairedObserverCompletions.load();
    value.pairedObserverReleaseAttempts = gPairedObserverReleaseAttempts.load();
    value.pairedObserverReleases = gPairedObserverReleases.load();
    value.pairedObserverReleaseRejected = gPairedObserverReleaseRejected.load();
    value.pairedObserverPairReopenSuppressed = gPairedObserverPairReopenSuppressed.load();
    value.pairedObserverNeutralHolds = gPairedObserverNeutralHolds.load();
    value.pairedObserverHandoffResumed = gPairedObserverHandoffResumed.load();
    value.pairedVictimTransitions = gPairedVictimTransitions.load();
    value.pairedVictimTransitionRejected = gPairedVictimTransitionRejected.load();
    value.pairedVictimInitializeAborted = gPairedVictimInitializeAborted.load();
    value.pairedVictimEventsRejected = gPairedVictimEventsRejected.load();
    value.pairedVictimDuplicates = gPairedVictimDuplicates.load();
    value.pairedVictimBusy = gPairedVictimBusy.load();
    value.pairedVictimNativeDispatches = gPairedVictimNativeDispatches.load();
    value.pairedVictimNativeStarted = gPairedVictimNativeStarted.load();
    value.pairedVictimAvailabilityOverrides = gPairedVictimAvailabilityOverrides.load();
    value.pairedPlayerPreparationRemaps = gPairedPlayerPreparationRemaps.load();
    value.pairedPlayerCategoryRemaps = gPairedPlayerCategoryRemaps.load();
    value.pairedVictimLocalDispatches = gPairedVictimLocalDispatches.load();
    value.pairedVictimLocalStarted = gPairedVictimLocalStarted.load();
    value.pairedVictimConvergenceDetached = gPairedVictimConvergenceDetached.load();
    value.pairedVictimSnapshotsDeferred = gPairedVictimSnapshotsDeferred.load();
    value.pairedVictimHealthContinuations = gPairedVictimHealthContinuations.load();
    value.pairedVictimWorkerDispatches = gPairedVictimWorkerDispatches.load();
    value.pairedVictimLocalAvailabilityOverrides = gPairedVictimLocalAvailabilityOverrides.load();
    value.pairedDeadTargetRejected = gPairedDeadTargetRejected.load();
    value.pairedRecoveryDeferred = gPairedRecoveryDeferred.load();
    value.pairedSourcePlacements = gPairedSourcePlacements.load();
    value.pairedSourcePlacementRejected = gPairedSourcePlacementRejected.load();
    value.pairedSourcePlacementDeferred = gPairedSourcePlacementDeferred.load();
    value.pairedSourcePlacementResumed = gPairedSourcePlacementResumed.load();
    value.pairedSourcePlacementCancelled = gPairedSourcePlacementCancelled.load();
    value.pairedSourcePlacementExpired = gPairedSourcePlacementExpired.load();
    value.pairedSourcePoseSuppressed = gPairedSourcePoseSuppressed.load();
    value.pairedObserverSnapshotsDeferred = gPairedObserverSnapshotsDeferred.load();
    value.presentationSamples = gPresentationSamples.load();
    value.presentationDropped = gPresentationDropped.load();
    value.lastVictimAdmissionHp = gLastVictimAdmissionHp.load();
    value.lastVictimAdmissionHealth = gLastVictimAdmissionHealth.load();
    value.lastVictimAdmissionEnabled = gLastVictimAdmissionEnabled.load();
    value.lastRejectedPairedReason = gLastRejectedPairedReason.load();
    value.lastRejectedPairedSourceState = gLastRejectedPairedSourceState.load();
    value.candidateEligibilityChecks = gCandidateEligibilityChecks.load();
    value.candidateEligibilityOverrides = gCandidateEligibilityOverrides.load();
    value.recordEligibilityChecks = gRecordEligibilityChecks.load();
    value.recordEligibilityOverrides = gRecordEligibilityOverrides.load();
    value.participantRecordsValidated = gParticipantRecordsValidated.load();
    value.participantRecordsRetained = gParticipantRecordsRetained.load();
    value.participantRecordsRejected = gParticipantRecordsRejected.load();
    value.participantPerceptionSamples = gParticipantPerceptionSamples.load();
    value.participantPerceptionDropped = gParticipantPerceptionDropped.load();
    value.targetSuccessionSamples = gTargetSuccessionSamples.load();
    value.targetSuccessionDropped = gTargetSuccessionDropped.load();
    value.postDeathSuccessionsPreserved = gPostDeathSuccessionsPreserved.load();
    value.postDeathArbitrationDeferrals = gPostDeathArbitrationDeferrals.load();
    value.postDeathDecisionUncertain = gPostDeathDecisionUncertain.load();
    value.deadTargetRecoveries = gDeadTargetRecoveries.load();
    value.aiPlayerQueryRemaps = gAiPlayerQueryRemaps.load();
    value.aiPlayerSlotRemaps = gAiPlayerSlotRemaps.load();
    value.aiPlayerSlotRestoreFailures = gAiPlayerSlotRestoreFailures.load();
    value.postDeathMeleeSelectionChecks = gPostDeathMeleeSelectionChecks.load();
    value.postDeathMeleeSelectionRejects = gPostDeathMeleeSelectionRejects.load();
    value.postDeathMeleeSelectionWriteFailures = gPostDeathMeleeSelectionWriteFailures.load();
    value.lastPostDeathRetiredPeer = gLastPostDeathRetiredPeer.load();
    value.postDeathDecisionResets = gPostDeathDecisionResets.load();
    value.postDeathDecisionResetRejected = gPostDeathDecisionResetRejected.load();
    value.postDeathRoleTaskResets = gPostDeathRoleTaskResets.load();
    value.interactionBindingsResolved = gInteractionBindingsResolved.load();
    value.interactionBindingRejected = gInteractionBindingRejected.load();
    value.lastHostAttachment = gLastHostAttachment.load();
    value.lastLocalAttachment = gLastLocalAttachment.load();
    value.lastTransitionVariant = gLastTransitionVariant.load();
    value.role = static_cast<std::uint32_t>(gRole.load());
    value.nativeGeneration = gActiveNativeGeneration.load(); value.worldGeneration = gActiveWorldGeneration.load();
    value.lastEntityA = gLastEntityA.load(); value.lastEntityB = gLastEntityB.load();
    value.lastCharacterClass = gLastCharacterClass.load();
    value.lastActionTypeRva = gLastActionTypeRva.load();
    value.lastActionPathHash = gLastActionPathHash.load();
    value.lastActionStatus = gLastActionStatus.load();
    value.lastExecutionTypeRva = gLastExecutionTypeRva.load();
    value.lastExecutionState = gLastExecutionState.load();
    value.lastExecutionFlags = gLastExecutionFlags.load();
    value.lastRejectedExecutionTypeRva = gLastRejectedExecutionTypeRva.load();
    value.lastRejectedExecutionState = gLastRejectedExecutionState.load();
    value.lastRoleTaskTypeRva = gLastRoleTaskTypeRva.load();
    value.lastRoleTaskKeyA = gLastRoleTaskKeyA.load();
    value.lastRoleTaskKeyB = gLastRoleTaskKeyB.load();
    value.lastLifecycleFlags = gLastLifecycleFlags.load();
    value.lastWithdrawState = gLastWithdrawState.load();
    value.lastInteractionTargetA = gLastInteractionTargetA.load();
    value.lastInteractionTargetB = gLastInteractionTargetB.load();
    value.lastInteractionTargetMode = gLastInteractionTargetMode.load();
    value.lastInteractionTargetPeer = gLastInteractionTargetPeer.load();
    value.lastAppliedInteractionTargetPeer = gLastAppliedInteractionTargetPeer.load();
    value.lastAiPlayerQueryPeer = gLastAiPlayerQueryPeer.load();
    value.lastAiPlayerSlotPeer = gLastAiPlayerSlotPeer.load();
    value.lastAppliedInteractionTargetA = gLastAppliedInteractionTargetA.load();
    value.lastAppliedInteractionTargetB = gLastAppliedInteractionTargetB.load();
    value.lastInteractionTransition = gLastInteractionTransition.load();
    value.stable = gGenerations != nullptr && gGenerations->IsStableGeneration(value.nativeGeneration);
    return value;
}
void LogDiagnostics(Diagnostics& diagnostics) {
    pvp_actions::LogVent(diagnostics);
    pvp_actions::LogAttack(diagnostics);
    {
        static std::uint64_t lastGateCount{};
        const auto count=gPairGateCount.load(std::memory_order_relaxed);
        if(count!=lastGateCount) {
            lastGateCount=count;
            diagnostics.Log("info","aisync_pair_gate","Client pair replay: first gate holding a pending Host pair. 1 placement 2 noCommand 3 noAuthority 4 noTransition 5 notSource 6 birth 7 cookie/world 8 scope 9 otherPair 10 target 11 source 12 victim 13 serial 14 reserved 15 dependency 16 boundary 17 outcome 100 entered 2xx submit result.",
                {{"gate",Diagnostics::Unsigned(gPairGateLast.load()),true},
                 {"characterClass",Diagnostics::Unsigned(gPairGateClass.load()),true},
                 {"pairRevision",Diagnostics::Unsigned(gPairGateRevision.load()),true},
                 {"refusalSite",Diagnostics::Unsigned(gPairGateRefusalSite.load()),true},
                 {"count",Diagnostics::Unsigned(count),true}});
        }
    }
    static bool movementFailureLogged{};
    if(!movementFailureLogged) {
        const auto f=host_producer::FirstMovementCaptureFailure();
        if(f.stage) {
            const auto words=[](const movement::Parameters& p) {
                std::string out="[";
                const auto append=[&out](std::uint32_t word){if(out.size()>1U)out+=',';out+=std::to_string(word);};
                append(p.retained);append(p.style);
                for(const auto v:p.goal)append(std::bit_cast<std::uint32_t>(v));
                append(std::bit_cast<std::uint32_t>(p.stoppingDistance));append(std::bit_cast<std::uint32_t>(p.stopDistance));
                for(const auto v:p.query)append(std::bit_cast<std::uint32_t>(v));
                append(p.flags90);append(p.flags91);out+=']';return out;
            };
            diagnostics.Log("error","aisync_host_movement_failure",
                "First failed capture only. stage1=gait,2=request snapshot,3=pending flag,4=phase,5=invalid Admit,6=invalid Update. Parameter arrays are uint32 raw words: retained,style,goal XYZ,stoppingDistance,stopDistance,query[9],flags90,flags91. Missing native reads remain zero; bundle may be partial for stages1..4. No normalization or recovery authorization.",
                {{"stage",Diagnostics::Unsigned(f.stage),true},{"request",Diagnostics::Unsigned(f.request),true},
                 {"sourceKind",Diagnostics::Unsigned(f.kind),true},{"observedKind",Diagnostics::Unsigned(f.observedKind),true},
                 {"type",Diagnostics::Unsigned(f.type),true},{"status",Diagnostics::Unsigned(f.status),true},
                 {"phase",Diagnostics::Unsigned(f.phase),true},{"pendingFlags",Diagnostics::Unsigned(f.flags),true},
                 {"version",Diagnostics::Unsigned(f.bundle.version),true},{"parts",Diagnostics::Unsigned(f.bundle.parts),true},
                 {"navigation",Diagnostics::Unsigned(static_cast<std::uint32_t>(f.bundle.navigation)),true},
                 {"gait",Diagnostics::Unsigned(f.gait),true},{"primaryFlags",Diagnostics::Unsigned(f.bundle.primaryFlags),true},
                 {"companionFlags",Diagnostics::Unsigned(f.bundle.companionFlags),true},
                 {"companionRetainedAfter",Diagnostics::Unsigned(f.bundle.companionRetainedAfter),true},
                 {"beforeWords",words(f.bundle.before),true},{"afterWords",words(f.bundle.after),true},
                 {"companionWords",words(f.bundle.companion),true}});
            movementFailureLogged=true;
        }
    }
    for(std::size_t i=0;i<gMotionTraces.size();++i) {
        MotionTraceSample sample{};std::uint64_t dropped{};
        {const std::lock_guard lock(gMotionTraceGate);if(!gMotionTraceCount)break;
            sample=gMotionTraces[gMotionTraceRead];gMotionTraceRead=(gMotionTraceRead+1)%gMotionTraces.size();--gMotionTraceCount;dropped=gMotionTraceDropped;}
        const auto number=[](float value){return std::isfinite(value)?std::to_string(value):std::string("null");};
        constexpr char hex[]="0123456789abcdef";std::string session(32,'0');
        for(std::size_t k=0;k<sample.scope.session.size();++k){session[k*2]=hex[sample.scope.session[k]>>4];session[k*2+1]=hex[sample.scope.session[k]&15];}
        diagnostics.Log("info","aisync_motion_trace",
            "type1=pose, 2=Host emitted operation, 3=Client adapter result (outcome0 Admitted 1 Rejected 2 Unknown; not proof of native entry), 4=Client dependency hold >=100ms (once/reason and at resolution), 5=Host native call capture (outcome0 supported, 1 unsupported), 6=Host stop constructor return (observation only). headLocal is an ephemeral local address, never a portable identity or release proof. waitReason1=actor ordering 2=reservation 3=ownership/baseline 4=resources 5=route 6=goal edit 7=Move restart 8=Host start 9=native intent graph/resources 10=placement pose 11=placement native completion. executionStatus2=Waiting; another status does not alone prove native entry. opcode1 Begin 2 Update 3 Submit 4 Cancel 5 Stop 6 Complete 7 Release. Match full identity; UTC offset is not network latency.",
            {{"type",Diagnostics::Unsigned(sample.type),true},{"role",Diagnostics::Unsigned(sample.role),true},
             {"utcMs",Diagnostics::Unsigned(sample.utcMs),true},{"entityA",Diagnostics::Unsigned(sample.entityA),true},
             {"sampleTick",Diagnostics::Unsigned(sample.sampleTick),true},{"sessionBytes",session,false},
             {"sessionCookie",Diagnostics::Unsigned(sample.sessionCookie),true},
             {"authorityEpoch",Diagnostics::Unsigned(sample.scope.authorityEpoch),true},{"worldEpoch",Diagnostics::Unsigned(sample.scope.worldEpoch),true},
             {"consumerEpoch",Diagnostics::Unsigned(sample.scope.consumerEpoch),true},{"entityB",Diagnostics::Unsigned(sample.entityB),true},
             {"birth",Diagnostics::Unsigned(sample.birth),true},{"ownerEpoch",Diagnostics::Unsigned(sample.ownerEpoch),true},
             {"ownerSeq",Diagnostics::Unsigned(sample.ownerSeq),true},{"parameterRevision",Diagnostics::Unsigned(sample.parameterRevision),true},
             {"flags",Diagnostics::Unsigned(sample.flags),true},{"gait",Diagnostics::Unsigned(sample.gait),true},
             {"poseReadable",Diagnostics::Boolean(sample.poseReadable),true},
             {"x",number(sample.x),true},{"y",number(sample.y),true},{"z",number(sample.z),true},{"yaw",number(sample.yaw),true},
             {"cycleType",Diagnostics::Unsigned(sample.cycleType),true},{"cycleState",Diagnostics::Unsigned(sample.cycleState),true},
             {"headKind",Diagnostics::Unsigned(sample.headKind),true},
             {"headLocal",Diagnostics::Unsigned(sample.headLocal),true},
             {"callbackThread",Diagnostics::Unsigned(sample.callbackThread),true},
             {"onEngineThread",Diagnostics::Unsigned(sample.onEngineThread),true},
             {"navigationReadMask",Diagnostics::Unsigned(sample.navigationReadMask),true},
             {"headBlockState",Diagnostics::Unsigned(sample.headBlockState),true},
             {"headBlockMode",Diagnostics::Unsigned(sample.headBlockMode),true},
             {"headFlags90",Diagnostics::Unsigned(sample.headFlags90),true},
             {"headFlags91",Diagnostics::Unsigned(sample.headFlags91),true},
             {"headTailState",Diagnostics::Unsigned(sample.headTailState),true},
             {"navigationStateReadMask",Diagnostics::Unsigned(sample.navigationStateReadMask),true},
             {"nativeMovementMode",Diagnostics::Unsigned(sample.nativeMovementMode),true},
             {"navigationWorkerState",Diagnostics::Unsigned(sample.navigationWorkerState),true},
             {"navigationUpdate48",Diagnostics::Unsigned(sample.navigationUpdate48),true},
             {"navigationUpdate49",Diagnostics::Unsigned(sample.navigationUpdate49),true},
             {"headGoalX",number(sample.headGoal[0]),true},{"headGoalY",number(sample.headGoal[1]),true},{"headGoalZ",number(sample.headGoal[2]),true},
             {"headPendingGoalX",number(sample.headPendingGoal[0]),true},{"headPendingGoalY",number(sample.headPendingGoal[1]),true},{"headPendingGoalZ",number(sample.headPendingGoal[2]),true},
             {"headStopDistance",number(sample.headStopDistance),true},{"headStoppingDistance",number(sample.headStoppingDistance),true},
             {"routeNodes",Diagnostics::Unsigned(sample.routeNodes),true},{"historyNodes",Diagnostics::Unsigned(sample.historyNodes),true},
             {"cursorOrdinal",Diagnostics::Unsigned(sample.cursorOrdinal),true},{"blockState",Diagnostics::Unsigned(sample.blockState),true},
             {"blockMode",Diagnostics::Unsigned(sample.blockMode),true},{"blockFlags90",Diagnostics::Unsigned(sample.blockFlags90),true},
             {"blockFlags91",Diagnostics::Unsigned(sample.blockFlags91),true},{"requestStatus",Diagnostics::Unsigned(sample.requestStatus),true},
             {"requestedGait",Diagnostics::Unsigned(sample.requestedGait),true},{"effectiveGait",Diagnostics::Unsigned(sample.effectiveGait),true},
             {"authoredFlags90",Diagnostics::Unsigned(sample.authoredFlags90),true},{"authoredFlags91",Diagnostics::Unsigned(sample.authoredFlags91),true},
             {"routeDistance",number(sample.routeDistance),true},{"routeProgress",number(sample.routeProgress),true},
             {"routeFirstX",number(sample.routeFirst[0]),true},{"routeFirstY",number(sample.routeFirst[1]),true},{"routeFirstZ",number(sample.routeFirst[2]),true},
             {"routeLastX",number(sample.routeLast[0]),true},{"routeLastY",number(sample.routeLast[1]),true},{"routeLastZ",number(sample.routeLast[2]),true},
             {"routeCursorX",number(sample.routeCursor[0]),true},{"routeCursorY",number(sample.routeCursor[1]),true},{"routeCursorZ",number(sample.routeCursor[2]),true},
             {"localMoveRestarts",Diagnostics::Unsigned(sample.localMoveRestarts),true},
             {"hostCycleType",Diagnostics::Unsigned(sample.hostCycleType),true},{"hostCycleState",Diagnostics::Unsigned(sample.hostCycleState),true},
             {"viewerDistanceCm",Diagnostics::Unsigned(sample.viewerDistanceCm),true},
             {"commandId",Diagnostics::Unsigned(sample.commandId),true},{"globalSeq",Diagnostics::Unsigned(sample.globalSeq),true},
             {"opcode",Diagnostics::Unsigned(sample.opcode),true},{"kind",Diagnostics::Unsigned(sample.nativeKind),true},
             {"mode",Diagnostics::Unsigned(sample.mode),true},{"outcome",Diagnostics::Unsigned(sample.outcome),true},
             {"intentContract",Diagnostics::Unsigned(sample.intentContract),true},{"intentKinds",Diagnostics::Unsigned(sample.intentKinds),true},
             {"intentDigest",Diagnostics::Unsigned(sample.intentDigest),true},{"intentCaller",Diagnostics::Unsigned(sample.intentCaller),true},
             {"intentArgument0",Diagnostics::Unsigned(sample.intentArgument0),true},{"intentArgument1",Diagnostics::Unsigned(sample.intentArgument1),true},
             {"intentReferenceMask",Diagnostics::Unsigned(sample.intentReferenceMask),true},{"intentCountOffset",Diagnostics::Unsigned(sample.intentCountOffset),true},
             {"intentScriptFlags",Diagnostics::Unsigned(sample.intentScriptFlags),true},
             {"waitAgeMs",Diagnostics::Unsigned(sample.waitAgeMs),true},{"waitReason",Diagnostics::Unsigned(sample.waitReason),true},
             {"executionStatus",Diagnostics::Unsigned(sample.executionStatus),true},{"executionResult",Diagnostics::Unsigned(sample.executionResult),true},
             {"aux",number(sample.aux),true},{"goalX",number(sample.goalX),true},{"goalY",number(sample.goalY),true},{"goalZ",number(sample.goalZ),true},
             {"stopDistance",number(sample.stopDistance),true},{"dropped",Diagnostics::Unsigned(dropped),true}});
    }
    for(std::size_t i=0;i<gExecutionTraces.size();++i) {
        ExecutionTraceSample sample{};
        {const std::lock_guard lock(gExecutionTraceGate);if(!gExecutionTraceCount)break;
            sample=gExecutionTraces[gExecutionTraceRead];gExecutionTraceRead=(gExecutionTraceRead+1)%gExecutionTraces.size();--gExecutionTraceCount;}
        const auto span=[](ULONGLONG from,ULONGLONG to){return from&&to>=from?Diagnostics::Unsigned(to-from):std::string("null");};
        diagnostics.Log("info","aisync_execution_trace",
            "Observed execution cycle only. Snapshots never start a native behavior. Native commands are correlated separately in aisync_motion_trace using commandId, globalSeq, intentContract and intentDigest. UTC across PCs requires clock offset calibration.",
            {{"role",Diagnostics::Unsigned(sample.role),true},{"revision",Diagnostics::Unsigned(sample.revision),true},
             {"executionRevision",Diagnostics::Unsigned(sample.executionRevision),true},
             {"cycleType",Diagnostics::Unsigned(sample.typeRva),true},{"cycleState",Diagnostics::Unsigned(sample.state),true},
             {"tick",Diagnostics::Unsigned(sample.role==1U?sample.second:sample.third),true},
             {"utcMs",Diagnostics::Unsigned(sample.utcMs),true},
             {"captureToPublishMs",sample.role==1U?span(sample.first,sample.second):std::string("null"),true},
             {"arrivalToIntakeMs",sample.role==2U?span(sample.first,sample.second):std::string("null"),true},
             {"intakeToApplyMs",sample.role==2U?span(sample.second,sample.third):std::string("null"),true},
             {"result",Diagnostics::Unsigned(sample.result),true},{"lastWait",Diagnostics::Unsigned(sample.waitReason),true},
             {"decision",Diagnostics::Unsigned(sample.decision),true},
             {"entityA",Diagnostics::Unsigned(sample.entityA),true},{"entityB",Diagnostics::Unsigned(sample.entityB),true},
             {"birth",Diagnostics::Unsigned(sample.birth),true},{"ownerEpoch",Diagnostics::Unsigned(sample.ownerEpoch),true},
             {"commandId",Diagnostics::Unsigned(sample.commandId),true},{"globalSeq",Diagnostics::Unsigned(sample.globalSeq),true},
             {"constructorRva",Diagnostics::Unsigned(sample.constructorRva),true},{"argumentKinds",Diagnostics::Unsigned(sample.argumentKinds),true},
             {"argument0",Diagnostics::Unsigned(sample.argument0),true},{"argument1",Diagnostics::Unsigned(sample.argument1),true},
             {"localType",Diagnostics::Unsigned(sample.localType),true},{"localState",Diagnostics::Unsigned(sample.localState),true},
             {"localSerial",Diagnostics::Unsigned(sample.localSerial),true},{"localFlags",Diagnostics::Unsigned(sample.localFlags),true},
             {"waits",Diagnostics::Unsigned(sample.waits),true},{"waitMask",Diagnostics::Unsigned(sample.waitMask),true},
             {"dropped",Diagnostics::Unsigned(gExecutionTraceDropped),true}});
    }
    for(std::size_t i=0;i<gLifecycleTraces.size();++i) {
        execution::LifecycleTraceSample sample{};
        {const std::lock_guard lock(gLifecycleTraceGate);if(!gLifecycleTraceCount)break;
            sample=gLifecycleTraces[gLifecycleTraceRead];gLifecycleTraceRead=(gLifecycleTraceRead+1)%gLifecycleTraces.size();--gLifecycleTraceCount;}
        const auto number=[](float value){return std::isfinite(value)?std::to_string(value):std::string("null");};
        diagnostics.Log("info","aisync_lifecycle_trace",
            "Native Character lifecycle event. Host stage 1=not captured 2=actor untracked 3=nested skipped 4=emitted 5=emit failed 6=startup queued 7=startup emitted; Client stage 10=validation rejected 12=replayed (result 0=Admitted 1=Rejected 2=Unknown). kind 256=relocate 257=backstage 258=frontstage 259=forced 260=direct 261=direct forced.",
            {{"stage",Diagnostics::Unsigned(sample.stage),true},{"kind",Diagnostics::Unsigned(sample.kind),true},
             {"mode",Diagnostics::Unsigned(sample.mode),true},{"result",Diagnostics::Unsigned(sample.result),true},
             {"caller",Diagnostics::Unsigned(sample.caller),true},{"seq",Diagnostics::Unsigned(sample.seq),true},
             {"callbackThread",Diagnostics::Unsigned(sample.callbackThread),true},
             {"hasGoal",Diagnostics::Boolean(sample.hasGoal),true},
             {"x",number(sample.x),true},{"y",number(sample.y),true},{"z",number(sample.z),true},{"yaw",number(sample.yaw),true},
             {"beforeX",number(sample.beforeX),true},{"beforeZ",number(sample.beforeZ),true},
             {"afterX",number(sample.afterX),true},{"afterY",number(sample.afterY),true},{"afterZ",number(sample.afterZ),true},
             {"dropped",Diagnostics::Unsigned(gLifecycleTraceDropped),true}});
    }
    for(std::size_t i=0;i<gSpawnProbes.size();++i) {
        SpawnPlacementProbe sample{};
        {const std::lock_guard lock(gSpawnProbeGate);if(!gSpawnProbeCount)break;
            sample=gSpawnProbes[gSpawnProbeRead];gSpawnProbeRead=(gSpawnProbeRead+1)%gSpawnProbes.size();--gSpawnProbeCount;}
        const auto number=[](float value){return std::isfinite(value)?std::to_string(value):std::string("null");};
        diagnostics.Log(sample.confirmed?"info":"warning","aisync_spawn_placement",
            "Initial authoritative pose; match session/authority/world/entity/birth/hostRevision. Client commands wait for readback confirmation.",
            {{"sampleTick",Diagnostics::Unsigned(sample.tick),true},
             {"phase",sample.role==1U?"host_baseline":(sample.confirmed?"client_confirmed":"client_waiting")},
             {"role",Diagnostics::Unsigned(sample.role),true},
             {"sessionCookie",Diagnostics::Unsigned(sample.sessionCookie),true},
             {"authorityEpoch",Diagnostics::Unsigned(sample.scope.authorityEpoch),true},
             {"nativeGeneration",Diagnostics::Unsigned(sample.nativeGeneration),true},
             {"worldGeneration",Diagnostics::Unsigned(sample.worldGeneration),true},
             {"entityA",Diagnostics::Unsigned(sample.entityA),true},
             {"entityB",Diagnostics::Unsigned(sample.entityB),true},
             {"birth",Diagnostics::Unsigned(sample.birth),true},
             {"hostRevision",Diagnostics::Unsigned(sample.revision),true},
             {"characterClass",Diagnostics::Unsigned(sample.characterClass),true},
             {"hostX",number(sample.host[0]),true},{"hostY",number(sample.host[1]),true},{"hostZ",number(sample.host[2]),true},
             {"localX",sample.readable?number(sample.local[0]):"null",true},
             {"localY",sample.readable?number(sample.local[1]):"null",true},
             {"localZ",sample.readable?number(sample.local[2]):"null",true},
             {"hostYaw",number(sample.hostYaw),true},{"localYaw",sample.readable?number(sample.localYaw):"null",true},
             {"positionError",sample.readable?number(sample.positionError):"null",true},
             {"yawError",sample.readable?number(sample.yawError):"null",true},
             {"confirmed",Diagnostics::Boolean(sample.confirmed),true}});
    }
    for(std::size_t i=0;i<gPairAdmissionFailures.size();++i) {
        PairAdmissionFailure sample{};
        {const std::lock_guard lock(gPairFailureMutex);if(!gPairFailureCount)break;
            sample=gPairAdmissionFailures[gPairFailureRead];gPairFailureRead=(gPairFailureRead+1)%gPairAdmissionFailures.size();--gPairFailureCount;}
        diagnostics.Log("warning","aisync_pair_admission_refused","Native pair start refused. refusalSite: 101 carrier invalid, 102 binding/victim identity, 103 Host cycle not active, 104 source already ended, 201 command invalid, 204 native registration refused, 211 binding invalid, 212 player victim incarnation, 213 participant not live, 301 revoked, 302 identity not live, 303 birth/pair revision, 304 victim mismatch, 305 preparation mismatch, 306 no pair, 40x action dependency (2 superseded, 3 rejected). decision: 1 retrying, 2 dropped this interaction (stream continues), 3 scope fenced (native entry happened).",
            {{"sampleTick",Diagnostics::Unsigned(sample.tick),true},{"command",Diagnostics::Unsigned(sample.command),true},
             {"receiptSerial",Diagnostics::Unsigned(sample.serial),true},{"entityA",Diagnostics::Unsigned(sample.entityA),true},
             {"entityB",Diagnostics::Unsigned(sample.entityB),true},{"birth",Diagnostics::Unsigned(sample.birth),true},
             {"pairRevision",Diagnostics::Unsigned(sample.pairRevision),true},{"result",Diagnostics::Unsigned(sample.result),true},
             {"phase",Diagnostics::Unsigned(sample.phase),true},{"adapterSite",Diagnostics::Unsigned(sample.site),true},
             {"stage",Diagnostics::Unsigned(sample.stage),true},{"reasonMask",Diagnostics::Unsigned(sample.reason),true},{"registered",Diagnostics::Boolean(sample.registered),true},
             {"transitionEntered",Diagnostics::Boolean(sample.transitionEntered),true},
             {"refusalSite",Diagnostics::Unsigned(sample.refusal),true},{"decision",Diagnostics::Unsigned(sample.decision),true},
             {"rawPayloadHigh",Diagnostics::Unsigned(sample.rawPayloadHigh),true},
             {"decodedPayloadHigh",Diagnostics::Unsigned(sample.decodedPayloadHigh),true}});
    }
    route_runtime::ClientFailure failure{};
    for(std::size_t i=0;i<64&&route_runtime::PopClientFailure(failure);++i) {
        diagnostics.Log("warning","aisync_route_client_refusal","Exact native route refusal; copied values only, no admission rules relaxed.",
            {{"sampleTick",Diagnostics::Unsigned(failure.tick),true},
             {"site",Diagnostics::Unsigned(failure.site),true},
             {"result",Diagnostics::Unsigned(failure.result),true},
             {"currentCheck",Diagnostics::Unsigned(failure.currentCheck),true},
             {"ownership",Diagnostics::Unsigned(failure.ownership),true},
             {"command",Diagnostics::Unsigned(failure.command),true},
             {"parameterRevision",Diagnostics::Unsigned(failure.revision),true},
             {"routeRevision",Diagnostics::Unsigned(failure.routeRevision),true},
             {"part",Diagnostics::Unsigned(failure.part),true},
             {"currentParameterRevision",Diagnostics::Unsigned(failure.currentRevision),true},
             {"receivedCommand",Diagnostics::Unsigned(failure.receivedCommand),true},
             {"receivedParameterRevision",Diagnostics::Unsigned(failure.receivedRevision),true},
             {"receivedRouteRevision",Diagnostics::Unsigned(failure.receivedRoute),true},
             {"receivedPart",Diagnostics::Unsigned(failure.receivedPart),true},
             {"expectedWorldToken",Diagnostics::Unsigned(failure.worldToken),true},
             {"nativeWorldToken",Diagnostics::Unsigned(failure.nativeToken),true},
             {"resourceDifference",Diagnostics::Unsigned(failure.resourceDifference),true},
             {"readMask",Diagnostics::Unsigned(failure.readMask),true},
             {"contextCheck",Diagnostics::Unsigned(failure.contextCheck),true},
             {"importStage",Diagnostics::Unsigned(failure.importStage),true},
             {"importNode",Diagnostics::Unsigned(failure.importNode),true},
             {"bindingDifference",Diagnostics::Unsigned(failure.bindingDifference),true},
             {"navigationNodes",Diagnostics::Unsigned(failure.native[0]),true},
             {"navigationHistory",Diagnostics::Unsigned(failure.native[1]),true},
             {"activeBlocks",Diagnostics::Unsigned(failure.native[2]),true},
             {"retiredBlocks",Diagnostics::Unsigned(failure.native[3]),true},
             {"previousBlockPresent",Diagnostics::Unsigned(failure.native[4]),true},
             {"currentBlockMatches",Diagnostics::Unsigned(failure.native[5]),true},
             {"currentResultMatches",Diagnostics::Unsigned(failure.native[6]),true},
             {"blockState",Diagnostics::Unsigned(failure.native[7]),true},
             {"blockMode",Diagnostics::Unsigned(failure.native[8]),true},
             {"blockStopBits",Diagnostics::Unsigned(failure.native[9]),true},
             {"resultReady",Diagnostics::Unsigned(failure.native[10]),true},
             {"resultStatus",Diagnostics::Unsigned(failure.native[11]),true},
             {"resultNodes",Diagnostics::Unsigned(failure.native[12]),true},
             {"droppedFailureRecords",Diagnostics::Unsigned(route_runtime::ClientFailuresDropped()),true}});
    }
    for(std::size_t i=0;i<gPairExitTraces.size();++i) {
        PairExitTrace sample{};std::uint64_t dropped{};
        {const std::lock_guard lock(gPairExitTraceGate);if(!gPairExitTraceCount)break;
            sample=gPairExitTraces[gPairExitTraceRead];gPairExitTraceRead=(gPairExitTraceRead+1U)%gPairExitTraces.size();--gPairExitTraceCount;dropped=gPairExitTraceDropped;}
        diagnostics.Log("info","aisync_pair_exit","Source cleanup: stage1 awaiting Host exit, 2 awaiting local End_Action, 3 guard/resources pending, 4 deferred native End_Action forwarded, 5 retail Idle requested, 6 native call fault, 7 Host left but local-player victim retains the source. No stage implies resource disposal; destructor receipts remain required.",
            {{"tick",Diagnostics::Unsigned(sample.tick),true},{"command",Diagnostics::Unsigned(sample.command),true},
             {"receiptSerial",Diagnostics::Unsigned(sample.receipt),true},{"stage",Diagnostics::Unsigned(sample.stage),true},
             {"entityA",Diagnostics::Unsigned(sample.entityA),true},{"entityB",Diagnostics::Unsigned(sample.entityB),true},
             {"birth",Diagnostics::Unsigned(sample.birth),true},{"hostCycle",Diagnostics::Unsigned(sample.hostCycle),true},
             {"hostPair",Diagnostics::Unsigned(sample.hostPair),true},{"localFlags",Diagnostics::Unsigned(sample.localFlags),true},
             {"markerObserved",Diagnostics::Boolean(sample.markerObserved),true},{"forwarded",Diagnostics::Boolean(sample.forwarded),true},
             {"check",Diagnostics::Unsigned(sample.check),true},{"dropped",Diagnostics::Unsigned(dropped),true}});
    }
    const auto retirement = execution_runtime::RetirementSnapshot();
    diagnostics.Log("info", "aisync_execution_retirement", "Local native-reference retirement; no Host command completion is inferred. contextBlocker: 1 operation in progress, 2 ownership transition, 3 session retirement, 4 route slots not quiet, 5 owner not released. routeFirstQuietMask bits: 1 route empty, 2 B list empty, 4 R list empty, 8 cursor at head, 16 history empty, 32 pending words clear.",
        {{"available", Diagnostics::Boolean(execution_runtime::Available()), true},
         {"retired", Diagnostics::Boolean(retirement.retired), true},
         {"references", Diagnostics::Unsigned(retirement.references), true},
         {"unknownReferences", Diagnostics::Unsigned(retirement.unknownReferences), true},
         {"fencedOwners", Diagnostics::Unsigned(retirement.fencedOwners), true},
         {"referencesReleased", Diagnostics::Boolean(retirement.referencesReleased), true},
         {"contextReleased", Diagnostics::Boolean(retirement.contextReleased), true},
         {"contextBlocker", Diagnostics::Unsigned(execution_runtime::ContextRetirementBlocker()), true},
         {"routeSlots", Diagnostics::Unsigned(route_execution::LiveSlots().total), true},
         {"routeSlotsLocal", Diagnostics::Unsigned(route_execution::LiveSlots().local), true},
         {"routeSlotsQuarantined", Diagnostics::Unsigned(route_execution::LiveSlots().quarantined), true},
         {"routeSlotsDestroyed", Diagnostics::Unsigned(route_execution::LiveSlots().destroyed), true},
         {"routeSlotsRevoked", Diagnostics::Unsigned(route_execution::LiveSlots().revoked), true},
         {"routeFirstQuietMask", Diagnostics::Unsigned(route_execution::LiveSlots().quietMask), true}});
    for(std::size_t index=0;index<profiling::kNames.size();++index) {
        const auto sample=profiling::Read(static_cast<profiling::Section>(index));
        diagnostics.Log("info","aisync_performance","Cumulative wall-time samples in microseconds; nested costs are not additive; intervals and delta are not costs or render FPS.",
            {{"section",profiling::kNames[index]}, {"sampleTick",Diagnostics::Unsigned(GetTickCount64()),true},
             {"role",Diagnostics::Unsigned(gRole.load()==Role::Host?1U:Client()?2U:0U),true},
             {"count",Diagnostics::Unsigned(sample.count),true}, {"totalUs",Diagnostics::Unsigned(sample.totalUs),true},
             {"maxUs",Diagnostics::Unsigned(sample.maxUs),true}, {"over10ms",Diagnostics::Unsigned(sample.over10ms),true},
             {"over100ms",Diagnostics::Unsigned(sample.over100ms),true}});
    }
    const auto storage = mesh_storage::Diagnostics();
    const auto routes = route_runtime::Diagnostics();
    const auto nativeRoutes = route_execution::Diagnostics();
    const auto mailboxFailure = execution_runtime::LastMailboxFailure();
    const auto sourceHistory = host_producer::EngineProducer().History();
    const auto clientHistory = execution_runtime::HistorySnapshot();
    diagnostics.Log("info", "aisync_execution_history", "Approximate retained-history counters; checkpoints require final native consumer acknowledgments.",
        {{"hostThrough", Diagnostics::Unsigned(sourceHistory.through), true},
         {"hostCommandSlots", Diagnostics::Unsigned(sourceHistory.commands), true},
         {"hostPackets", Diagnostics::Unsigned(sourceHistory.packets), true},
         {"clientThrough", Diagnostics::Unsigned(clientHistory.through), true},
         {"clientCheckpoints", Diagnostics::Unsigned(clientHistory.checkpoints), true},
         {"clientRecords", Diagnostics::Unsigned(clientHistory.records), true}});
    const auto waits = execution_runtime::WaitSnapshot();
    diagnostics.Log("info", "aisync_client_waits", "Client orders held behind a native dependency. reason 1=earlier order of the same actor, 2=dependency gate (see reservedByOutcome/reservedByPair), 3=actor ownership, 4=locomotion resources, 5=Host route provider, 6=goal edit pending natively, 7=ended Move restart, 10=placement pose, 11=placement native completion. opcode 1=Begin 2=Update 3=Submit 4=Cancel 5=Stop 6=Complete 7=Release.",
        {{"waiting", Diagnostics::Unsigned(waits.waiting), true},
         {"oldestSeq", Diagnostics::Unsigned(waits.oldestSeq), true},
         {"oldestKind", Diagnostics::Unsigned(waits.oldestKind), true},
         {"oldestOpcode", Diagnostics::Unsigned(waits.oldestOpcode), true},
         {"oldestAdmission", Diagnostics::Unsigned(waits.oldestAdmission), true},
         {"oldestReason", Diagnostics::Unsigned(waits.oldestReason), true},
         {"oldestAgeMs", Diagnostics::Unsigned(waits.oldestAgeMs), true},
         {"maxSeq", Diagnostics::Unsigned(waits.maxSeq), true},
         {"maxKind", Diagnostics::Unsigned(waits.maxKind), true},
         {"maxOpcode", Diagnostics::Unsigned(waits.maxOpcode), true},
         {"maxReason", Diagnostics::Unsigned(waits.maxReason), true},
         {"maxAgeMs", Diagnostics::Unsigned(waits.maxAgeMs), true},
         {"reservedByOutcome", Diagnostics::Unsigned(gReservedByOutcome.load()), true},
         {"reservedByPair", Diagnostics::Unsigned(gReservedByPair.load()), true},
         {"transitionPlacements", Diagnostics::Unsigned(waits.transitionPlacements), true},
         {"lifecycleEvents", Diagnostics::Unsigned(waits.lifecycleEvents), true},
         {"hostStartHolds", Diagnostics::Unsigned(execution::HostStartHolds()), true},
         {"steersSuspended", Diagnostics::Unsigned(execution::SteersSuspended()), true},
         {"hostProgressCatchups", Diagnostics::Unsigned(execution::HostProgressCatchups()), true},
         {"nativeIntentDerived",Diagnostics::Unsigned(gNativeIntentDerived.load()),true},
         {"nativeIntentQueueRefused",Diagnostics::Unsigned(gNativeIntentQueueRefused.load()),true},
         {"nativeIntentLastContract",Diagnostics::Unsigned(gNativeIntentLastContract.load()),true},
         {"nativeIntentLastCaller",Diagnostics::Unsigned(gNativeIntentLastCaller.load()),true},
         {"hostCycleCallsCaptured", Diagnostics::Unsigned(gCycleCallsCaptured.load()), true},
         {"hostCycleCallsRefused", Diagnostics::Unsigned(gCycleCallsRefused.load()), true},
         {"hostLastRefusedCycleCall", Diagnostics::Unsigned(gLastRefusedCycleCall.load()), true},
         {"hostCycleCallsPublished", Diagnostics::Unsigned(gCycleCallsPublished.load()), true},
         {"clientCycleCallsReplayed", Diagnostics::Unsigned(gCycleCallsReplayed.load()), true},
         {"directLocomotionConstructorsConsumed", Diagnostics::Unsigned(gDirectLocomotionConstructorsConsumed.load()), true},
         {"directPoseAssistFrames", Diagnostics::Unsigned(gDirectPoseAssistFrames.load()), true},
         {"directPoseActionFollowFrames", Diagnostics::Unsigned(gDirectPoseActionFollowFrames.load()), true},
         {"nativeFollowCreated", Diagnostics::Unsigned(gFollowCreated.load()), true},
         {"nativeFollowCreateFailures", Diagnostics::Unsigned(gFollowCreateFailures.load()), true},
         {"nativeFollowCompleted", Diagnostics::Unsigned(gFollowCompleted.load()), true},
         {"nativeFollowCancelled", Diagnostics::Unsigned(gFollowCancelled.load()), true},
         {"nativeFollowEmergencies", Diagnostics::Unsigned(gFollowEmergencies.load()), true},
         {"nativeFollowDivergenceReplans", Diagnostics::Unsigned(gFollowDivergenceReplans.load()), true},
         {"spectateFallbackCandidates", Diagnostics::Unsigned(gSpectateFallbackCandidates.load()), true},
         {"nativeFollowIdleTurns", Diagnostics::Unsigned(gFollowIdleTurns.load()), true},
         {"directPoseEmergencyCorrections", Diagnostics::Unsigned(gDirectPoseEmergencyCorrections.load()), true},
         {"clientCycleCallsReplayFailed", Diagnostics::Unsigned(gCycleCallsReplayFailed.load()), true},
         {"endedSourceExitsDeferred", Diagnostics::Unsigned(gEndedSourceExitsDeferred.load()), true},
         {"localVictimEndRetained", Diagnostics::Unsigned(gLocalVictimEndRetained.load()), true},
         {"orphanedParticipantVictimsReleased", Diagnostics::Unsigned(gOrphanedVictimsReleased.load()), true},
         {"localVictimEndsFinished", Diagnostics::Unsigned(gLocalVictimEndsFinished.load()), true},
         {"pairsAbandoned", Diagnostics::Unsigned(gPairsAbandoned.load()), true},
         {"pairConsumedActions", Diagnostics::Unsigned(execution::ConsumedActions()), true},
         {"hostNativeCallsDropped", Diagnostics::Unsigned(gNativeIntentDropped.load()), true},
         {"hostLastDroppedCall", Diagnostics::Unsigned(gLastIntentRefusal.load()), true},
         {"hostScriptedRefusals", Diagnostics::Unsigned(gScriptedRefusalCount), true},
         {"hostScriptedRefusalClause", Diagnostics::Unsigned(gScriptedRefusalClause), true},
         {"hostScriptedRefusalCountOffset", Diagnostics::Unsigned(gScriptedRefusalCountOffset), true},
         {"hostScriptedRefusalWords", [] {std::string text;const std::lock_guard lock(gScriptedRefusalGate);
             for(const auto word:gScriptedRefusalWords){if(!text.empty())text+=' ';text+=std::to_string(word);}
             return text;}(), false},
         {"pairRefusalRetries", Diagnostics::Unsigned(gPairRefusalRetries.load()), true},
         {"remoteVictimEndRetained", Diagnostics::Unsigned(gRemoteVictimEndRetained.load()), true},
         {"deferredSourceEndsForwarded", Diagnostics::Unsigned(gDeferredSourceEndsForwarded.load()), true},
         {"hostRoutesUnavailable", Diagnostics::Unsigned(route_runtime::HostRoutesUnavailable()), true},
         {"localPlanningFallbacks", Diagnostics::Unsigned(route_runtime::LocalPlanningFallbacks()), true},
         {"localRouteStartPlans", Diagnostics::Unsigned(route_runtime::LocalStartPlans()), true},
         {"localRouteReplans", Diagnostics::Unsigned(route_runtime::LocalReplans()), true},
         {"hostEndedRetainedSources", Diagnostics::Unsigned(gHostEndedRetainedSources.load()), true},
         {"deadVictimReleases", Diagnostics::Unsigned(gDeadVictimReleases.load()), true},
         {"deadVictimSourcePending", Diagnostics::Unsigned(gDeadVictimSourcePending.load()), true},
         {"endedSourceExitRequests", Diagnostics::Unsigned(gEndedSourceExits.load()), true},
         {"updateRefusals", Diagnostics::Unsigned(execution::UpdateRefusals().count), true},
         {"updateRefusalCommand", Diagnostics::Unsigned(execution::UpdateRefusals().command), true},
         {"updateRefusalBits", Diagnostics::Unsigned(execution::UpdateRefusals().bits), true},
         {"updateRefusalStatus", Diagnostics::Unsigned(execution::UpdateRefusals().status), true},
         {"placementHoldFrames", Diagnostics::Unsigned(execution::UpdateRefusals().placementHolds), true},
         {"placementHoldTimeouts", Diagnostics::Unsigned(execution::UpdateRefusals().placementTimeouts), true},
         {"hostUnpublishedPlacements", Diagnostics::Unsigned(gUnpublishedPlacements.load()), true},
         {"intakeHeldByPair", Diagnostics::Unsigned(gIntakeHeldByPair.load()), true},
         {"intakeHeldByHealth", Diagnostics::Unsigned(gIntakeHeldByHealth.load()), true},
         {"intakeHeldByParticipantDamage", Diagnostics::Unsigned(gIntakeHeldByParticipant.load()), true},
         {"intakeHeldByCombat", Diagnostics::Unsigned(gIntakeHeldByCombat.load()), true}});
    const auto guidance=execution::GuidanceDiagnostics();
    diagnostics.Log("info","aisync_native_guidance","Host corridor waypoints, local CATHODE navigation; no corrective transforms or hybrid motor.",
        {{"goalEdits",Diagnostics::Unsigned(guidance.edits),true},
         {"goalDeposits",Diagnostics::Unsigned(guidance.goalDeposits),true},
         {"goalConsumptions",Diagnostics::Unsigned(guidance.goalConsumptions),true},
         {"goalDiscards",Diagnostics::Unsigned(guidance.goalDiscards),true},
         {"stopChanges",Diagnostics::Unsigned(guidance.stopChanges),true},
         {"startWaits",Diagnostics::Unsigned(guidance.startWaits),true},
         {"retainedWaits",Diagnostics::Unsigned(guidance.retainedWaits),true},
         {"queueWaits",Diagnostics::Unsigned(guidance.queueWaits),true},
         {"exhaustedTailContinuations",Diagnostics::Unsigned(guidance.exhaustedTailContinuations),true},
         {"deferred",Diagnostics::Unsigned(guidance.deferred),true},
         {"suspended",Diagnostics::Unsigned(guidance.suspended),true},
         {"faults",Diagnostics::Unsigned(guidance.faults),true},
         {"samplesWithoutRoute",Diagnostics::Unsigned(guidance.withoutRoute),true},
         {"continuations",Diagnostics::Unsigned(guidance.continuations),true},
         {"continuationLimitSamples",Diagnostics::Unsigned(guidance.continuationLimits),true},
         {"pacedGoalSamples",Diagnostics::Unsigned(guidance.pacedSamples),true},
         {"pacingHoldSamples",Diagnostics::Unsigned(guidance.holdSamples),true},
         {"pacingUnavailableSamples",Diagnostics::Unsigned(guidance.unavailableSamples),true},
         {"hostRequestedGaitSamples",Diagnostics::Unsigned(guidance.gaitSamples),true},
         {"physicalTrailGoalSamples",Diagnostics::Unsigned(guidance.physicalTrailSamples),true},
         {"lastLeadMeters",std::to_string(guidance.lastLead),true},
         {"correctiveTeleportsEnabled",Diagnostics::Boolean(false),true}});
    const auto passageRepairs=execution::PassageRepairs();
    diagnostics.Log("info","aisync_native_passage","Exact live Host passage75/76 protects its native cycle. Pre-passage repair reasons: 1 critical, 2 ambiguous, 4 no Move, 8 authority, 16 lifetime, 32 unreadable, 64 active, 128 worker, 256 foreign block, 512 retained block, 1024 route update, 2048 queue, 4096 retained ring unknown, 8192 retained status5, 16384 partial failure. Counts do not certify physical completion.",
        {{"localTransitionsHeld",Diagnostics::Unsigned(gPassageTransitionsHeld.load()),true},
         {"backstageInterruptionsHeld",Diagnostics::Unsigned(gPassageBackstageInterruptionsHeld.load()),true},
         {"moveRepairs",Diagnostics::Unsigned(passageRepairs.repaired),true},
         {"moveRepairDeferred",Diagnostics::Unsigned(passageRepairs.deferred),true},
         {"moveRepairFailed",Diagnostics::Unsigned(passageRepairs.failed),true},
         {"moveRepairReasonBits",Diagnostics::Unsigned(passageRepairs.reasonBits),true},
         {"moveRepairStatus",Diagnostics::Unsigned(passageRepairs.status),true},
         {"moveRepairCommand",Diagnostics::Unsigned(passageRepairs.lastMoveCommand),true},
         {"moveRepairPassageSeq",Diagnostics::Unsigned(passageRepairs.lastPassageSeq),true}});
    const auto replacement=route_execution::ReplacementDiagnostics();
    diagnostics.Log("info","aisync_route_replacement","Native replacement: wait1=start area, 2=native boundary busy, 3=compound parts. Counts do not certify visual completion.",
        {{"requested",Diagnostics::Unsigned(replacement.requested),true},
         {"committed",Diagnostics::Unsigned(replacement.committed),true},
         {"waitSamples",Diagnostics::Unsigned(replacement.waitSamples),true},
         {"lastCommand",Diagnostics::Unsigned(replacement.command),true},
         {"lastWaitReason",Diagnostics::Unsigned(replacement.reason),true},
         {"currentArea",Diagnostics::Unsigned(replacement.currentArea),true},
         {"expectedArea",Diagnostics::Unsigned(replacement.expectedArea),true},
         {"parameterRevision",Diagnostics::Unsigned(replacement.parameterRevision),true},
         {"routeRevision",Diagnostics::Unsigned(replacement.routeRevision),true},
         {"placementContractReady",Diagnostics::Boolean(execution::PlacementDedupReady()),true},
         {"placementChildrenDerived",Diagnostics::Unsigned(gPlacementChildrenDerived.load()),true},
         {"placementFallbacksSuppressed",Diagnostics::Unsigned(gPlacementFallbacksSuppressed.load()),true}});
    const auto nativeHandoff=route_execution::NativeHandoffDiagnostics();
    diagnostics.Log("info","aisync_native_motion","Host commands and routes drive native Client locomotion and rotation.",
        {{"worldResets",Diagnostics::Unsigned(gWorldResetCount.load()),true},
         {"nativeHandoffCandidates",Diagnostics::Unsigned(nativeHandoff.candidates),true},
         {"nativeHandoffConfirmed",Diagnostics::Unsigned(nativeHandoff.confirmed),true},
         {"nativeHandoffStalled",Diagnostics::Unsigned(nativeHandoff.stalled),true},
         {"hybridFollowerEnabled",Diagnostics::Boolean(false),true},
         {"clientRoutePlanningEnabled",Diagnostics::Boolean(true),true},
         {"correctionTeleportsEnabled",Diagnostics::Boolean(false),true},
         {"initialNativePosesPreserved",Diagnostics::Unsigned(gInitialNativePosesPreserved.load()),true},
         {"hybridRecoveryEntries",Diagnostics::Unsigned(gHybridEntries.load()),true},
         {"hybridRecoveryResumes",Diagnostics::Unsigned(gHybridResumes.load()),true},
         {"hybridRecoveryApplications",Diagnostics::Unsigned(gHybridApplications.load()),true},
         {"hybridNativeArms",Diagnostics::Unsigned(gHybridNativeArms.load()),true},
         {"hybridNativeRefusals",Diagnostics::Unsigned(gHybridNativeRefusals.load()),true},
         {"hybridHardSnaps",Diagnostics::Unsigned(gHybridHardSnaps.load()),true},
         {"hybridRecoveryPaused",Diagnostics::Unsigned(gHybridPaused.load()),true},
         {"hybridRecoveryFailures",Diagnostics::Unsigned(gHybridFailures.load()),true}});
    diagnostics.Log("info", "aisync_typed_execution", "Typed Host commands and authoritative routes execute with native CATHODE locomotion.",
        {{"enabled", Diagnostics::Boolean(Installed()), true},
         {"hostSourceBound", Diagnostics::Boolean(host_producer::EngineProducer().Bound()), true},
         {"hostNativeRetainedLeases", Diagnostics::Unsigned(host_producer::RetainedNativeLeases()), true},
         {"hostNativeFencedLeases", Diagnostics::Unsigned(host_producer::FencedOldNativeLeases()), true},
         {"hostNativeNeedsRetirement", Diagnostics::Boolean(host_producer::NativeNeedsLiveWorldRetirement()), true},
         {"hostOffersQueued", Diagnostics::Unsigned(host_producer::BindingPumpStatus().queued), true},
         {"hostOffersCancelled", Diagnostics::Unsigned(host_producer::BindingPumpStatus().cancelled), true},
         {"hostOffersUnqualified", Diagnostics::Unsigned(host_producer::BindingPumpStatus().qualificationRejected), true},
         {"hostOffersConfigureRejected", Diagnostics::Unsigned(host_producer::BindingPumpStatus().configurationRejected), true},
         {"hostOffersReadyProduced", Diagnostics::Unsigned(host_producer::BindingPumpStatus().readyProduced), true},
         {"hostCoverageLosses", Diagnostics::Unsigned(host_producer::Coverage().losses), true},
         {"hostFirstLossSite", Diagnostics::Unsigned(host_producer::Coverage().firstSite), true},
         {"hostLastLossSite", Diagnostics::Unsigned(host_producer::Coverage().lastSite), true},
         {"hostBootstrapCancels", Diagnostics::Unsigned(host_producer::BootstrapCancels()), true},
         {"clientUpdatesAfterLocalEnd", Diagnostics::Unsigned(execution::UpdatesAfterLocalEnd()), true},
         {"clientFencedRestartsSkipped", Diagnostics::Unsigned(execution::FencedRestartsSkipped()), true},
         {"clientHostPathSteers", Diagnostics::Unsigned(execution::HostPathSteers()), true},
         {"clientLifecycleTransitions", Diagnostics::Unsigned(gLifecycleTransitions.load()), true},
         {"hostSourceFailures", Diagnostics::Unsigned(host_producer::EngineProducer().LastFailure().count), true},
         {"hostSourceFailureSite", Diagnostics::Unsigned(host_producer::EngineProducer().LastFailure().site), true},
         {"hostSourceFailureReason", Diagnostics::Unsigned(host_producer::EngineProducer().LastFailure().reason), true},
         {"hostSourceFailureKind", Diagnostics::Unsigned(host_producer::EngineProducer().LastFailure().kind), true},
         {"clientMailboxFailures", Diagnostics::Unsigned(mailboxFailure.count), true},
         {"clientMailboxFailureSite", Diagnostics::Unsigned(mailboxFailure.site), true},
         {"clientMailboxFailureReason", Diagnostics::Unsigned(mailboxFailure.reason), true},
         {"clientMailboxFailureStage", Diagnostics::Unsigned(mailboxFailure.stage), true},
         {"clientMailboxFailureSequence", Diagnostics::Unsigned(mailboxFailure.sequence), true},
         {"clientMailboxFailureKind", Diagnostics::Unsigned(mailboxFailure.kind), true},
         {"clientMailboxFailureOpcode", Diagnostics::Unsigned(mailboxFailure.opcode), true},
         {"clientHandoffAttempts", Diagnostics::Unsigned(gHandoffAttempts.load()), true},
         {"clientHandoffStage", Diagnostics::Unsigned(gHandoffStage.load()), true},
         {"clientHandoffResult", Diagnostics::Unsigned(gHandoffResult.load()), true},
         {"clientHandoffOffset", Diagnostics::Unsigned(gHandoffOffset.load()), true},
         {"clientHandoffValue", Diagnostics::Unsigned(gHandoffValue.load()), true},
         {"clientHandoffNavigationClears", Diagnostics::Unsigned(gHandoffNavigationClears.load()), true},
         {"clientHandoffNavigationResets", Diagnostics::Unsigned(gHandoffNavigationResets.load()), true},
         {"routeDiagnosticQueued", Diagnostics::Unsigned(routes.queued), true},
         {"routeDiagnosticQueueRejected", Diagnostics::Unsigned(routes.queueRejected), true},
         {"routeDiagnosticEngineQueries", Diagnostics::Unsigned(routes.engineQueries), true},
         {"routeDiagnosticWorkerQueries", Diagnostics::Unsigned(routes.workerQueries), true},
         {"routeDiagnosticStrictQueries", Diagnostics::Unsigned(routes.strictQueries), true},
         {"routeDiagnosticUnownedQueries", Diagnostics::Unsigned(routes.unownedQueries), true},
         {"routeDiagnosticRequeues", Diagnostics::Unsigned(routes.requeues), true},
         {"routeDiagnosticQueuedPackets", Diagnostics::Unsigned(routes.queuedPackets), true},
         {"routeDiagnosticLastQueueRejection", Diagnostics::Unsigned(routes.lastQueueRejection), true},
         {"routeDiagnosticLastQueryOwnership", Diagnostics::Unsigned(routes.lastQueryOwnership), true},
         {"routeDiagnosticLastQueryPolicy", Diagnostics::Unsigned(routes.lastQueryPolicy), true},
         {"routeDiagnosticLastQueryKind", Diagnostics::Unsigned(routes.lastQueryKind), true},
         {"routeHostFailureSite", Diagnostics::Unsigned(routes.hostFailureSite), true},
         {"routeHostFailureResult", Diagnostics::Unsigned(routes.hostFailureResult), true},
         {"routeHostNavigationNodes", Diagnostics::Unsigned(routes.hostNavigationNodes), true},
         {"routeHostResultNodes", Diagnostics::Unsigned(routes.hostResultNodes), true},
         {"routeHostBlockState", Diagnostics::Unsigned(routes.hostBlockState), true},
         {"routeHostBlockMode", Diagnostics::Unsigned(routes.hostBlockMode), true},
         {"routeHostStopBits", Diagnostics::Unsigned(routes.hostStopBits), true},
         {"routeHostNodeType", Diagnostics::Unsigned(routes.hostNodeType), true},
         {"routeHostMetadataBytes", Diagnostics::Unsigned(routes.hostMetadataBytes), true},
         {"routeHostCaptureDeferred", Diagnostics::Unsigned(routes.hostCaptureDeferred), true},
         {"routeHostEndpointEditsSkipped", Diagnostics::Unsigned(routes.hostEndpointEditsSkipped), true},
         {"routeHostCaptureCheck", Diagnostics::Unsigned(routes.hostCaptureCheck), true},
         {"routeHostCaptureNode", Diagnostics::Unsigned(routes.hostCaptureNode), true},
         {"routeHostMarkerIndex", Diagnostics::Unsigned(routes.hostMarkerIndex), true},
         {"routeHostMarkerKind", Diagnostics::Unsigned(routes.hostMarkerKind), true},
         {"routeHostMarkerTypeRva", Diagnostics::Unsigned(routes.hostMarkerTypeRva), true},
         {"untrackedMoveKind1", Diagnostics::Unsigned(host_producer::UncoveredKind(1)), true},
         {"untrackedMoveKind3", Diagnostics::Unsigned(host_producer::UncoveredKind(3)), true},
         {"uncoveredKind0", Diagnostics::Unsigned(host_producer::UncoveredKind(0)), true},
         {"uncoveredKind2", Diagnostics::Unsigned(host_producer::UncoveredKind(2)), true},
         {"markerKeysCaptured", Diagnostics::Unsigned(gMarkerKeysCaptured.load()), true},
         {"markerKeysResolved", Diagnostics::Unsigned(gMarkerKeysResolved.load()), true},
         {"markerCaptureResult", Diagnostics::Unsigned(gMarkerCaptureResult.load()), true},
         {"markerResolveResult", Diagnostics::Unsigned(gMarkerResolveResult.load()), true},
         {"uncoveredKind6", Diagnostics::Unsigned(host_producer::UncoveredKind(6)), true},
         {"uncoveredKind8", Diagnostics::Unsigned(host_producer::UncoveredKind(8)), true},
         {"uncoveredKind9", Diagnostics::Unsigned(host_producer::UncoveredKind(9)), true},
         {"uncoveredKind10", Diagnostics::Unsigned(host_producer::UncoveredKind(10)), true},
         {"uncoveredKind11", Diagnostics::Unsigned(host_producer::UncoveredKind(11)), true},
         {"pairedTransactionsAvailable", Diagnostics::Boolean(gPairRuntimeInitialized.load(std::memory_order_acquire)), true},
         {"movementRoutesAvailable", Diagnostics::Boolean(route_runtime::Available()), true},
         {"nativeRouteQueriesSuppressed", Diagnostics::Unsigned(nativeRoutes.suppressed), true},
         {"nativeRouteResultsPublished", Diagnostics::Unsigned(nativeRoutes.published), true},
         {"nativeRoutesInstalled", Diagnostics::Unsigned(nativeRoutes.installed), true},
         {"nativeRouteReplacements", Diagnostics::Unsigned(nativeRoutes.replacements), true},
         {"nativeRouteRejected", Diagnostics::Unsigned(nativeRoutes.rejected), true},
         {"nativeRouteRetirements", Diagnostics::Unsigned(nativeRoutes.retired), true},
         {"nativeRouteFailureCommand", Diagnostics::Unsigned(nativeRoutes.failureCommand), true},
         {"nativeRouteFailureSite", Diagnostics::Unsigned(nativeRoutes.failureSite), true},
         {"nativeRouteFailureResult", Diagnostics::Unsigned(nativeRoutes.failureResult), true},
         {"nativeRouteImportStage", Diagnostics::Unsigned(nativeRoutes.importStage), true},
         {"nativeRouteContextCheck", Diagnostics::Unsigned(nativeRoutes.contextCheck), true},
         {"nativeRouteAppendsPublished", Diagnostics::Unsigned(nativeRoutes.appendPublished), true},
         {"nativeRouteContinuationsPublished", Diagnostics::Unsigned(nativeRoutes.continuationPublished), true},
         {"nativeRouteCompoundResets", Diagnostics::Unsigned(nativeRoutes.compoundRequests), true},
         {"nativeRouteLastAdmissionMode", Diagnostics::Unsigned(nativeRoutes.lastAdmissionMode), true},
         {"nativeRouteLastIncomingNodes", Diagnostics::Unsigned(nativeRoutes.lastIncomingCount), true},
         {"nativeRouteCorridorJoins", Diagnostics::Unsigned(nativeRoutes.corridorJoins), true},
         {"nativeRouteTrailBridges", Diagnostics::Unsigned(nativeRoutes.trailBridges), true},
         {"nativeRouteTrailMisses", Diagnostics::Unsigned(nativeRoutes.trailMisses), true},
         {"nativeRouteTrailMissDetail", Diagnostics::Unsigned(nativeRoutes.trailMissDetail), true},
         {"nativeRouteLastCompoundParts", Diagnostics::Unsigned(nativeRoutes.lastCompoundParts), true},
         {"authoritativeRouteHooksAvailable", Diagnostics::Boolean(route_runtime::Available()), true},
         {"routesCaptured", Diagnostics::Unsigned(routes.captured), true},
         {"routesImported", Diagnostics::Unsigned(routes.imported), true},
         {"routesBlocked", Diagnostics::Unsigned(routes.blocked), true},
         {"routesPending", Diagnostics::Unsigned(routes.pending), true},
         {"routesRetained", Diagnostics::Unsigned(routes.retained), true},
         {"routeNativeInvalidations", Diagnostics::Unsigned(routes.invalidations), true},
         {"routeReplansSuppressed", Diagnostics::Unsigned(routes.replanSuppressed), true},
         {"routeGoalChangesDeferred", Diagnostics::Unsigned(routes.goalDeferred), true},
         {"routeResultsAwaitingHost", Diagnostics::Unsigned(routes.awaitingHost), true},
         {"routeReplacementsPublished", Diagnostics::Unsigned(routes.replacements), true},
         {"routeReplacementsAdmitted", Diagnostics::Unsigned(routes.replacementAdmissions), true},
         {"routeGraphsSuperseded", Diagnostics::Unsigned(routes.superseded), true},
         {"routeCommandFaults", Diagnostics::Unsigned(routes.commandFaults), true},
         {"routePacketsDiscarded", Diagnostics::Unsigned(routes.discarded), true},
         {"routeGlobalFailures", Diagnostics::Unsigned(routes.globalFailures), true},
         {"ownedNavigationMeshes", Diagnostics::Unsigned(storage.owned), true},
         {"navigationStorageRejected", Diagnostics::Unsigned(storage.rejected), true},
         {"navigationStorageCapacity", Diagnostics::Unsigned(storage.capacity), true},
         {"navigationStorageAllocationFailures", Diagnostics::Unsigned(storage.allocationFailures), true},
         {"fullReplacementReady", "false", true},
         {"installationError", gTypedExecutionInstallationError, false}});
    const auto nav = navigation::Snapshot();
    const auto hp = health::Snapshot();
    const auto shots = combat::Snapshot();
    const auto graph = animation::Snapshot();
    diagnostics.Log("info", "aisync_native_consequences", "Host health and damage executed through native retail entry points; v14 wire.",
        {{"healthCaptured", Diagnostics::Unsigned(hp.captured), true},
         {"healthApplied", Diagnostics::Unsigned(hp.applied), true},
         {"healthDeferred", Diagnostics::Unsigned(hp.deferred), true},
         {"healthFailed", Diagnostics::Unsigned(hp.failed), true},
         {"damageCaptured", Diagnostics::Unsigned(hp.damageCaptured), true},
         {"damageReplayed", Diagnostics::Unsigned(hp.damageReplayed), true},
         {"damageDropped", Diagnostics::Unsigned(hp.damageDropped), true},
         {"nativeDamageBlocked", Diagnostics::Unsigned(hp.nativeDamageBlocked), true},
         {"commitsResolved", Diagnostics::Unsigned(hp.commitsResolved), true},
         {"shotsCaptured", Diagnostics::Unsigned(shots.captured), true},
         {"shotsApplied", Diagnostics::Unsigned(shots.shotsApplied), true},
         {"shotsDeferred", Diagnostics::Unsigned(shots.deferred), true},
         {"shotsFailed", Diagnostics::Unsigned(shots.failed), true},
         {"shotDecisionsBlocked", Diagnostics::Unsigned(shots.decisionsBlocked), true},
         {"missingMessageOwners", Diagnostics::Unsigned(shots.missingMessageOwners), true}});
    diagnostics.Log("info", "aisync_native_animation", "Native paired updates and markers are observed, never ticked twice.",
        {{"sourceUpdates", Diagnostics::Unsigned(graph.sourceUpdates), true},
         {"coupledPlayerUpdates", Diagnostics::Unsigned(graph.coupledPlayerUpdates), true},
         {"humanAlignmentUpdates", Diagnostics::Unsigned(graph.humanAlignmentUpdates), true},
         {"humanGraphInitializations", Diagnostics::Unsigned(graph.humanGraphInitializations), true},
         {"humanVictimGraphInitializations", Diagnostics::Unsigned(graph.humanVictimGraphInitializations), true},
         {"markerCallbacks", Diagnostics::Unsigned(graph.markerCallbacks), true},
         {"markerDispatches", Diagnostics::Unsigned(graph.markerDispatches), true},
         {"markerCallbacksSkipped", Diagnostics::Unsigned(graph.markerCallbacksSkipped), true},
         {"scriptedCaptured", Diagnostics::Unsigned(graph.scriptedCaptured), true},
         {"scriptedUnsupported", Diagnostics::Unsigned(graph.scriptedUnsupported), true},
         {"scriptedApplied", Diagnostics::Unsigned(graph.scriptedApplied), true},
         {"scriptedFailed", Diagnostics::Unsigned(graph.scriptedFailed), true}});
    diagnostics.Log("info", "aisync_navigation", "Host resolved goals executed by local retail navigation; v14 wire.",
        {{"captured", Diagnostics::Unsigned(nav.captured), true},
         {"submitted", Diagnostics::Unsigned(nav.submitted), true},
         {"updated", Diagnostics::Unsigned(nav.updated), true},
         {"completed", Diagnostics::Unsigned(nav.completed), true},
         {"failed", Diagnostics::Unsigned(nav.failed), true},
         {"cancelled", Diagnostics::Unsigned(nav.cancelled), true},
         {"expired", Diagnostics::Unsigned(nav.expired), true},
         {"gaitApplied", Diagnostics::Unsigned(nav.gaitApplied), true},
         {"directedCaptured", Diagnostics::Unsigned(nav.directedCaptured), true},
         {"endpointsCaptured", Diagnostics::Unsigned(nav.endpointsCaptured), true},
         {"pathGoalsCaptured", Diagnostics::Unsigned(nav.pathGoalsCaptured), true},
         {"turnCaptured", Diagnostics::Unsigned(nav.turnCaptured), true},
         {"stanceCaptured", Diagnostics::Unsigned(nav.stanceCaptured), true},
         {"submissionsCaptured", Diagnostics::Unsigned(nav.submissionsCaptured), true},
         {"submissionsDropped", Diagnostics::Unsigned(nav.submissionsDropped), true},
         {"intentFallbacks", Diagnostics::Unsigned(gNavigationFallbacks.load()), true},
         {"pairedGraphDropped", Diagnostics::Unsigned(gPairedGraphDropped.load()), true}});
    for (;;) {
        PairedGraphProbe sample{};
        {
            std::unique_lock lock(gPairedGraphMutex, std::try_to_lock);
            if (!lock.owns_lock() || gPairedGraphRead == gPairedGraphWrite) break;
            sample = gPairedGraphProbes[gPairedGraphRead++ % kParticipantProbeRing];
        }
        diagnostics.Log("info", "aisync_paired_graph", "Retail callback trace: side 1=source event, 2=victim event, 3=source finish, 4=shared animation boundary. Side 4 phase follows animation::Boundary; other phases 0=entry, 1=return. Observation only.",
            {{"sampleTick", Diagnostics::Unsigned(sample.tick), true},
             {"role", Diagnostics::Unsigned(sample.role), true},
             {"worldGeneration", Diagnostics::Unsigned(sample.generation), true},
             {"nativeEventId", Diagnostics::Unsigned(sample.event), true},
             {"side", Diagnostics::Unsigned(sample.side), true},
             {"phase", Diagnostics::Unsigned(sample.phase), true},
             {"callbackThread", Diagnostics::Unsigned(sample.thread), true},
             {"cycle", Diagnostics::Pointer(reinterpret_cast<const void*>(sample.cycle))},
             {"graph", Diagnostics::Pointer(reinterpret_cast<const void*>(sample.graph))},
             {"cycleSerial", Diagnostics::Unsigned(sample.serial), true},
             {"callerRva", Diagnostics::Unsigned(sample.callerRva), true},
             {"markerIndex", Diagnostics::Unsigned(sample.markerIndex), true},
             {"sourceAlignment", std::isfinite(sample.sourceAlignment) ? std::to_string(sample.sourceAlignment) : "null", true},
             {"victimAlignment", std::isfinite(sample.victimAlignment) ? std::to_string(sample.victimAlignment) : "null", true},
             {"sourceA", Diagnostics::Unsigned(sample.sourceA), true},
             {"sourceB", Diagnostics::Unsigned(sample.sourceB), true},
             {"victimA", Diagnostics::Unsigned(sample.victimA), true},
             {"victimB", Diagnostics::Unsigned(sample.victimB), true},
             {"peer", Diagnostics::Unsigned(sample.peer), true},
             {"birth", Diagnostics::Unsigned(sample.birth), true},
             {"pairRevision", Diagnostics::Unsigned(sample.pairRevision), true},
             {"sourceState", Diagnostics::Unsigned(sample.sourceState), true},
             {"victimState", Diagnostics::Unsigned(sample.victimState), true},
             {"sourceStage", Diagnostics::Unsigned(sample.sourceStage), true},
             {"sourceFlags", Diagnostics::Unsigned(sample.sourceFlags), true},
             {"sourceE5", Diagnostics::Unsigned(sample.sourceE5), true},
             {"victimFlags", Diagnostics::Unsigned(sample.victimFlags), true},
             {"victimInteractive", Diagnostics::Unsigned(sample.victimInteractive), true},
             {"victimControlActive", Diagnostics::Unsigned(sample.victimControlActive), true},
             {"endPredicateReadable", Diagnostics::Boolean(sample.endPredicateReadable), true},
             {"sourceFinished", Diagnostics::Unsigned(sample.sourceFinished), true},
             {"victimEnded", Diagnostics::Unsigned(sample.victimEnded), true},
             {"victimAcquired", Diagnostics::Unsigned(sample.victimAcquired), true},
             {"readable", Diagnostics::Boolean(sample.readable), true}});
    }
    const auto ai = DiagnosticsSnapshot();
    diagnostics.Log("info", "aisync_v2_snapshot", "AISync V2 high-level Character authority counters; animation/audio and downstream CATHODE consequences remain retail-local.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"ownsAI", Diagnostics::Boolean(OwnsAI()), true},
         {"role", Diagnostics::Unsigned(ai.role), true},
         {"nativeGeneration", Diagnostics::Unsigned(ai.nativeGeneration), true},
         {"worldGeneration", Diagnostics::Unsigned(ai.worldGeneration), true},
         {"stable", Diagnostics::Boolean(ai.stable), true},
         {"managerUpdates", Diagnostics::Unsigned(ai.managerUpdates), true},
         {"admitted", Diagnostics::Unsigned(ai.admitted), true},
         {"identityRejected", Diagnostics::Unsigned(ai.identityRejected), true},
         {"treeUnavailable", Diagnostics::Unsigned(ai.treeUnavailable), true},
         {"ownerMismatch", Diagnostics::Unsigned(ai.ownerMismatch), true},
         {"poseCaptured", Diagnostics::Unsigned(ai.poseCaptured), true},
         {"poseApplied", Diagnostics::Unsigned(ai.poseApplied), true},
         {"intentCaptured", Diagnostics::Unsigned(ai.intentCaptured), true},
         {"intentApplied", Diagnostics::Unsigned(ai.intentApplied), true},
         {"intentRejected", Diagnostics::Unsigned(ai.intentRejected), true},
         {"intentRecoveries", Diagnostics::Unsigned(ai.intentRecoveries), true},
         {"poseInterpolated", Diagnostics::Unsigned(gPoseInterpolated.load(std::memory_order_relaxed)), true},
         {"pairedHostCycleWaits", Diagnostics::Unsigned(gPairedHostCycleWaits.load(std::memory_order_relaxed)), true},
         {"outcomeSnapshotsCoalesced", Diagnostics::Unsigned(gOutcomeSnapshotsCoalesced.load(std::memory_order_relaxed)), true},
         {"backstageEligibilityOverrides", Diagnostics::Unsigned(gBackstageEligibilityOverrides.load(std::memory_order_relaxed)), true},
         {"survivorTargetsAssigned", Diagnostics::Unsigned(gSurvivorTargetsAssigned.load(std::memory_order_relaxed)), true},
         {"survivorTargetFaults", Diagnostics::Unsigned(gSurvivorTargetFaults.load(std::memory_order_relaxed)), true},
         {"lastSurvivorTargetPeer", Diagnostics::Unsigned(gLastSurvivorTargetPeer.load(std::memory_order_relaxed)), true},
         {"backstageNavigationPlans", Diagnostics::Unsigned(gBackstageNavigationPlans.load(std::memory_order_relaxed)), true},
         {"backstageNavigationUpdates", Diagnostics::Unsigned(gBackstageNavigationUpdates.load(std::memory_order_relaxed)), true},
         {"backstageNavigationRejected", Diagnostics::Unsigned(gBackstageNavigationRejected.load(std::memory_order_relaxed)), true},
         {"backstageDeadTargetsRejected", Diagnostics::Unsigned(gBackstageDeadTargetsRejected.load(std::memory_order_relaxed)), true},
         {"backstageHoldContexts", Diagnostics::Unsigned(gBackstageHoldContexts.load(std::memory_order_relaxed)), true},
         {"targetPlayerOverrides", Diagnostics::Unsigned(gTargetPlayerOverrides.load(std::memory_order_relaxed)), true},
         {"menacePlayerOverrides", Diagnostics::Unsigned(gMenacePlayerOverrides.load(std::memory_order_relaxed)), true},
         {"soundPlayerOverrides", Diagnostics::Unsigned(gSoundPlayerOverrides.load(std::memory_order_relaxed)), true},
         {"backstageGoalsObjective", Diagnostics::Unsigned(gBackstageGoalsObjective.load(std::memory_order_relaxed)), true},
         {"backstageGoalsOwner", Diagnostics::Unsigned(gBackstageGoalsOwner.load(std::memory_order_relaxed)), true},
         {"backstageGoalsDegenerate", Diagnostics::Unsigned(gBackstageGoalsDegenerate.load(std::memory_order_relaxed)), true},
         {"poseRejected", Diagnostics::Unsigned(ai.poseRejected), true},
         {"actionCaptured", Diagnostics::Unsigned(ai.actionCaptured), true},
         {"actionTransitions", Diagnostics::Unsigned(ai.actionTransitions), true},
         {"executionCaptured", Diagnostics::Unsigned(ai.executionCaptured), true},
         {"executionTransitions", Diagnostics::Unsigned(ai.executionTransitions), true},
         {"executionReplayed", Diagnostics::Unsigned(ai.executionReplayed), true},
         {"executionReplayRejected", Diagnostics::Unsigned(ai.executionReplayRejected), true},
         {"lastExecutionTypeRva", Diagnostics::Unsigned(ai.lastExecutionTypeRva), true},
         {"lastExecutionState", Diagnostics::Unsigned(ai.lastExecutionState), true},
         {"lastExecutionFlags", Diagnostics::Unsigned(ai.lastExecutionFlags), true},
         {"lastRejectedExecutionTypeRva", Diagnostics::Unsigned(ai.lastRejectedExecutionTypeRva), true},
         {"lastRejectedExecutionState", Diagnostics::Unsigned(ai.lastRejectedExecutionState), true},
         {"roleTaskCaptured", Diagnostics::Unsigned(ai.roleTaskCaptured), true},
         {"roleTaskTransitions", Diagnostics::Unsigned(ai.roleTaskTransitions), true},
         {"lifecycleCaptured", Diagnostics::Unsigned(ai.lifecycleCaptured), true},
         {"lifecycleApplied", Diagnostics::Unsigned(ai.lifecycleApplied), true},
         {"lifecycleSnaps", Diagnostics::Unsigned(ai.lifecycleSnaps), true},
         {"backstageCaptured", Diagnostics::Unsigned(ai.backstageCaptured), true},
         {"backstageTransitions", Diagnostics::Unsigned(ai.backstageTransitions), true},
         {"backstageApplied", Diagnostics::Unsigned(ai.backstageApplied), true},
         {"backstageRejected", Diagnostics::Unsigned(ai.backstageRejected), true},
         {"placementVerified", Diagnostics::Unsigned(ai.placementVerified), true},
         {"placementRejected", Diagnostics::Unsigned(ai.placementRejected), true},
         {"lifecycleRetries", Diagnostics::Unsigned(ai.lifecycleRetries), true},
         {"executionBarriers", Diagnostics::Unsigned(ai.executionBarriers), true},
         {"impactCaptured", Diagnostics::Unsigned(ai.impactCaptured), true},
         {"impactQueued", Diagnostics::Unsigned(ai.impactQueued), true},
         {"impactApplied", Diagnostics::Unsigned(ai.impactApplied), true},
         {"impactRejected", Diagnostics::Unsigned(ai.impactRejected), true},
         {"impactShooterRemaps", Diagnostics::Unsigned(ai.impactShooterRemaps), true},
         {"impactShooterUnresolved", Diagnostics::Unsigned(ai.impactShooterUnresolved), true},
         {"impactStimulusForwarded", Diagnostics::Unsigned(gImpactStimulusForwarded.load()), true},
         {"impactStimulusApplied", Diagnostics::Unsigned(gImpactStimulusApplied.load()), true},
         {"impactDamagelessDropped", Diagnostics::Unsigned(gImpactDamagelessDropped.load()), true},
         {"withdrawSamples", Diagnostics::Unsigned(ai.withdrawSamples), true},
         {"withdrawTransitions", Diagnostics::Unsigned(ai.withdrawTransitions), true},
         {"withdrawApplied", Diagnostics::Unsigned(ai.withdrawApplied), true},
         {"interactionCaptured", Diagnostics::Unsigned(ai.interactionCaptured), true},
         {"interactionApplied", Diagnostics::Unsigned(ai.interactionApplied), true},
         {"interactionRejected", Diagnostics::Unsigned(ai.interactionRejected), true},
         {"interactionRemapped", Diagnostics::Unsigned(ai.interactionRemapped), true},
         {"pairedTransitionCaptured", Diagnostics::Unsigned(ai.pairedTransitionCaptured), true},
         {"pairedTransitionApplied", Diagnostics::Unsigned(ai.pairedTransitionApplied), true},
         {"playerRepresentationsRegistered", Diagnostics::Unsigned(ai.playerRepresentationsRegistered), true},
         {"provisionalPlayerActorsRetired", Diagnostics::Unsigned(ai.provisionalPlayerActorsRetired), true},
         {"participantCandidates", Diagnostics::Unsigned(ai.participantCandidates), true},
         {"participantTargetRejected", Diagnostics::Unsigned(ai.participantTargetRejected), true},
         {"participantCandidatePasses", Diagnostics::Unsigned(ai.participantCandidatePasses), true},
         {"senseEventPasses", Diagnostics::Unsigned(ai.senseEventPasses), true},
         {"participantSignalLookups", Diagnostics::Unsigned(ai.participantSignalLookups), true},
         {"participantSignalRemaps", Diagnostics::Unsigned(ai.participantSignalRemaps), true},
         {"participantSignalRetailMissing", Diagnostics::Unsigned(ai.participantSignalRetailMissing), true},
         {"participantSignalRetailMismatch", Diagnostics::Unsigned(ai.participantSignalRetailMismatch), true},
         {"participantSignalAmbiguous", Diagnostics::Unsigned(ai.participantSignalAmbiguous), true},
         {"lastParticipantSignalPeer", Diagnostics::Unsigned(ai.lastParticipantSignalPeer), true},
         {"participantPairLookups", Diagnostics::Unsigned(ai.participantPairLookups), true},
         {"participantPairRemaps", Diagnostics::Unsigned(ai.participantPairRemaps), true},
         {"participantPairRetailMissing", Diagnostics::Unsigned(ai.participantPairRetailMissing), true},
         {"participantPairAmbiguous", Diagnostics::Unsigned(ai.participantPairAmbiguous), true},
         {"lastParticipantPairPeer", Diagnostics::Unsigned(ai.lastParticipantPairPeer), true},
         {"pairedInitializeSucceeded", Diagnostics::Unsigned(ai.pairedInitializeSucceeded), true},
         {"pairedInitializeFailed", Diagnostics::Unsigned(ai.pairedInitializeFailed), true},
         {"pairedCycleReleased", Diagnostics::Unsigned(ai.pairedCycleReleased), true},
         {"pairedObserverCompletions", Diagnostics::Unsigned(ai.pairedObserverCompletions), true},
         {"pairedObserverOutroRetained", Diagnostics::Unsigned(gPairedObserverOutroRetained.load()), true},
         {"ownedExecutionPumps", Diagnostics::Unsigned(gOwnedExecutionPumps.load()), true},
         {"actionReferencesCaptured", Diagnostics::Unsigned(gActionReferencesCaptured.load()), true},
         {"actionReferenceMisses", Diagnostics::Unsigned(gActionReferencesUnbound.load()), true},
         {"pairedObserverOutroExpired", Diagnostics::Unsigned(gPairedObserverOutroExpired.load()), true},
         {"pairedOutroSchedulerBlocked", Diagnostics::Unsigned(gPairedOutroSchedulerBlocked.load()), true},
         {"pairedObserverFinishRetries", Diagnostics::Unsigned(gPairedObserverFinishRetries.load()), true},
         {"pairedObserverFinishCompleted", Diagnostics::Unsigned(gPairedObserverFinishCompleted.load()), true},
         {"pairedLocalVictimFinishes", Diagnostics::Unsigned(gPairedLocalVictimFinishes.load()), true},
         {"pairedPreparedReplays", Diagnostics::Unsigned(gPairedPreparedReplays.load()), true},
         {"pairedIdleBlocked", Diagnostics::Unsigned(gPairedIdleBlocked.load()), true},
         {"pairedStopBlocked", Diagnostics::Unsigned(gPairedStopBlocked.load()), true},
         {"lastPairedBlockedSchedulerRva", Diagnostics::Unsigned(gLastPairedBlockedSchedulerRva.load()), true},
         {"navigationConvergenceApplied", Diagnostics::Unsigned(gNavigationConvergenceApplied.load()), true},
         {"navigationConvergenceFailed", Diagnostics::Unsigned(gNavigationConvergenceFailed.load()), true},
         {"navigationTimingAccelerated", Diagnostics::Unsigned(gNavigationTimingAccelerated.load()), true},
         {"navigationTimingBraked", Diagnostics::Unsigned(gNavigationTimingBraked.load()), true},
         {"pursuitApplied", Diagnostics::Unsigned(gPursuitApplied.load()), true},
         {"pursuitFailed", Diagnostics::Unsigned(gPursuitFailed.load()), true},
         {"pursuitEnded", Diagnostics::Unsigned(gPursuitEnded.load()), true},
         {"navigationHostPauses", Diagnostics::Unsigned(gNavigationPaused.load()), true},
         {"pairedAdmissionRetries", Diagnostics::Unsigned(gPairedAdmissionRetries.load()), true},
         {"pairedAdmissionAbandoned", Diagnostics::Unsigned(gPairedAdmissionAbandoned.load()), true},
         {"pairedRuntimePlayerRemaps", Diagnostics::Unsigned(gPairedRuntimePlayerRemaps.load()), true},
         {"pairedObserverReleaseAttempts", Diagnostics::Unsigned(ai.pairedObserverReleaseAttempts), true},
         {"pairedObserverReleases", Diagnostics::Unsigned(ai.pairedObserverReleases), true},
         {"pairedObserverReleaseRejected", Diagnostics::Unsigned(ai.pairedObserverReleaseRejected), true},
         {"pairedObserverPairReopenSuppressed", Diagnostics::Unsigned(ai.pairedObserverPairReopenSuppressed), true},
         {"pairedObserverNeutralHolds", Diagnostics::Unsigned(ai.pairedObserverNeutralHolds), true},
         {"pairedObserverHandoffResumed", Diagnostics::Unsigned(ai.pairedObserverHandoffResumed), true},
         {"pairedVictimTransitions", Diagnostics::Unsigned(ai.pairedVictimTransitions), true},
         {"pairedVictimTransitionRejected", Diagnostics::Unsigned(ai.pairedVictimTransitionRejected), true},
         {"pairedVictimInitializeAborted", Diagnostics::Unsigned(ai.pairedVictimInitializeAborted), true},
         {"pairedVictimEventsRejected", Diagnostics::Unsigned(ai.pairedVictimEventsRejected), true},
         {"pairedVictimDuplicates", Diagnostics::Unsigned(ai.pairedVictimDuplicates), true},
         {"pairedVictimBusy", Diagnostics::Unsigned(ai.pairedVictimBusy), true},
         {"pairedVictimNativeDispatches", Diagnostics::Unsigned(ai.pairedVictimNativeDispatches), true},
         {"pairedVictimNativeStarted", Diagnostics::Unsigned(ai.pairedVictimNativeStarted), true},
         {"nativeOutcomeCaptured", Diagnostics::Unsigned(gOutcomeCaptured.load()), true},
         {"nativeOutcomeApplied", Diagnostics::Unsigned(gOutcomeApplied.load()), true},
         {"nativeOutcomeRejected", Diagnostics::Unsigned(gOutcomeRejected.load()), true},
         {"unboundHostPreparationAborts", Diagnostics::Unsigned(gUnboundHostPreparationAborts.load()), true},
         {"unboundHostPreparationAbortIncomplete", Diagnostics::Unsigned(gUnboundHostPreparationAbortIncomplete.load()), true},
         {"pairRegistrationsAccepted", Diagnostics::Unsigned(gPairRegistrationAccepted.load()), true},
         {"pairRegistrationsOnWorkers", Diagnostics::Unsigned(gPairRegistrationWorkers.load()), true},
         {"pairTransitionsOnWorkers", Diagnostics::Unsigned(gPairTransitionWorkers.load()), true},
         {"prepRejectSubmission", Diagnostics::Unsigned(gPrepRejectSubmission.load()), true},
         {"prepRejectSource", Diagnostics::Unsigned(gPrepRejectSource.load()), true},
         {"prepRejectTerminal", Diagnostics::Unsigned(gPrepRejectTerminal.load()), true},
         {"prepRejectDefinition", Diagnostics::Unsigned(gPrepRejectDefinition.load()), true},
         {"prepRejectGeometry", Diagnostics::Unsigned(gPrepRejectGeometry.load()), true},
         {"prepRejectFault", Diagnostics::Unsigned(gPrepRejectFault.load()), true},
         {"prepHealthOnlyApplied", Diagnostics::Unsigned(gPrepHealthOnlyApplied.load()), true},
         {"pairedVictimReleaseRequests", Diagnostics::Unsigned(gPairedVictimReleaseRequests.load()), true},
         {"stalePairedHoldReleases", Diagnostics::Unsigned(character_direct_step_runtime::StalePairedHoldReleases()), true},
         {"invalidOutgoing", Diagnostics::Unsigned(gInvalidOutgoing.load()), true},
         {"lastInvalidOutgoingFlags", Diagnostics::Unsigned(gLastInvalidOutgoingFlags.load()), true},
         {"lastInvalidOutgoingEntityA", Diagnostics::Unsigned(gLastInvalidOutgoingEntityA.load()), true},
         {"lastInvalidOutgoingEntityB", Diagnostics::Unsigned(gLastInvalidOutgoingEntityB.load()), true},
         {"nativeOutcomeDuplicates", Diagnostics::Unsigned(gOutcomeDuplicates.load()), true},
         {"pairedVictimAvailabilityOverrides", Diagnostics::Unsigned(ai.pairedVictimAvailabilityOverrides), true},
         {"pairedPlayerPreparationRemaps", Diagnostics::Unsigned(ai.pairedPlayerPreparationRemaps), true},
         {"pairedPlayerCategoryRemaps", Diagnostics::Unsigned(ai.pairedPlayerCategoryRemaps), true},
         {"pairedVictimLocalDispatches", Diagnostics::Unsigned(ai.pairedVictimLocalDispatches), true},
         {"pairedVictimLocalStarted", Diagnostics::Unsigned(ai.pairedVictimLocalStarted), true},
         {"pairedVictimConvergenceDetached", Diagnostics::Unsigned(ai.pairedVictimConvergenceDetached), true},
         {"pairedVictimSnapshotsDeferred", Diagnostics::Unsigned(ai.pairedVictimSnapshotsDeferred), true},
         {"pairedVictimHealthContinuations", Diagnostics::Unsigned(ai.pairedVictimHealthContinuations), true},
         {"pairedVictimWorkerDispatches", Diagnostics::Unsigned(ai.pairedVictimWorkerDispatches), true},
         {"pairedVictimLocalAvailabilityOverrides", Diagnostics::Unsigned(ai.pairedVictimLocalAvailabilityOverrides), true},
         {"pairedDeadTargetRejected", Diagnostics::Unsigned(ai.pairedDeadTargetRejected), true},
         {"pairedRecoveryDeferred", Diagnostics::Unsigned(ai.pairedRecoveryDeferred), true},
         {"pairedSourcePlacements", Diagnostics::Unsigned(ai.pairedSourcePlacements), true},
         {"pairedSourcePlacementRejected", Diagnostics::Unsigned(ai.pairedSourcePlacementRejected), true},
         {"pairedSourcePlacementDeferred", Diagnostics::Unsigned(ai.pairedSourcePlacementDeferred), true},
         {"pairedSourcePlacementResumed", Diagnostics::Unsigned(ai.pairedSourcePlacementResumed), true},
         {"pairedSourcePlacementCancelled", Diagnostics::Unsigned(ai.pairedSourcePlacementCancelled), true},
         {"pairedSourcePlacementExpired", Diagnostics::Unsigned(ai.pairedSourcePlacementExpired), true},
         {"pairedSourcePoseSuppressed", Diagnostics::Unsigned(ai.pairedSourcePoseSuppressed), true},
         {"pairedObserverSnapshotsDeferred", Diagnostics::Unsigned(ai.pairedObserverSnapshotsDeferred), true},
         {"presentationSamples", Diagnostics::Unsigned(ai.presentationSamples), true},
         {"presentationDropped", Diagnostics::Unsigned(ai.presentationDropped), true},
         {"lastVictimAdmissionHp", Diagnostics::Unsigned(ai.lastVictimAdmissionHp), true},
         {"lastVictimAdmissionHealth", Diagnostics::Unsigned(ai.lastVictimAdmissionHealth), true},
         {"lastVictimAdmissionEnabled", Diagnostics::Unsigned(ai.lastVictimAdmissionEnabled), true},
         {"lastRejectedPairedReason", Diagnostics::Unsigned(ai.lastRejectedPairedReason), true},
         {"lastRejectedPairedSourceState", Diagnostics::Unsigned(ai.lastRejectedPairedSourceState), true},
         {"candidateEligibilityChecks", Diagnostics::Unsigned(ai.candidateEligibilityChecks), true},
         {"candidateEligibilityOverrides", Diagnostics::Unsigned(ai.candidateEligibilityOverrides), true},
         {"recordEligibilityChecks", Diagnostics::Unsigned(ai.recordEligibilityChecks), true},
         {"recordEligibilityOverrides", Diagnostics::Unsigned(ai.recordEligibilityOverrides), true},
         {"participantRecordsValidated", Diagnostics::Unsigned(ai.participantRecordsValidated), true},
         {"participantRecordsRetained", Diagnostics::Unsigned(ai.participantRecordsRetained), true},
         {"participantRecordsRejected", Diagnostics::Unsigned(ai.participantRecordsRejected), true},
         {"participantPerceptionSamples", Diagnostics::Unsigned(ai.participantPerceptionSamples), true},
         {"participantPerceptionDropped", Diagnostics::Unsigned(ai.participantPerceptionDropped), true},
         {"targetSuccessionSamples", Diagnostics::Unsigned(ai.targetSuccessionSamples), true},
         {"targetSuccessionDropped", Diagnostics::Unsigned(ai.targetSuccessionDropped), true},
         {"postDeathSuccessionsPreserved", Diagnostics::Unsigned(ai.postDeathSuccessionsPreserved), true},
         {"postDeathArbitrationDeferrals", Diagnostics::Unsigned(ai.postDeathArbitrationDeferrals), true},
         {"postDeathDecisionUncertain", Diagnostics::Unsigned(ai.postDeathDecisionUncertain), true},
         {"deadTargetRecoveries", Diagnostics::Unsigned(ai.deadTargetRecoveries), true},
         {"aiPlayerQueryRemaps", Diagnostics::Unsigned(ai.aiPlayerQueryRemaps), true},
         {"aiPlayerSlotRemaps", Diagnostics::Unsigned(ai.aiPlayerSlotRemaps), true},
         {"aiPlayerSlotRestoreFailures", Diagnostics::Unsigned(ai.aiPlayerSlotRestoreFailures), true},
         {"postDeathMeleeSelectionChecks", Diagnostics::Unsigned(ai.postDeathMeleeSelectionChecks), true},
         {"postDeathMeleeSelectionRejects", Diagnostics::Unsigned(ai.postDeathMeleeSelectionRejects), true},
         {"postDeathMeleeSelectionWriteFailures", Diagnostics::Unsigned(ai.postDeathMeleeSelectionWriteFailures), true},
         {"lastPostDeathRetiredPeer", Diagnostics::Unsigned(ai.lastPostDeathRetiredPeer), true},
         {"postDeathDecisionResets", Diagnostics::Unsigned(ai.postDeathDecisionResets), true},
         {"postDeathDecisionResetRejected", Diagnostics::Unsigned(ai.postDeathDecisionResetRejected), true},
         {"postDeathRoleTaskResets", Diagnostics::Unsigned(ai.postDeathRoleTaskResets), true},
         {"interactionBindingsResolved", Diagnostics::Unsigned(ai.interactionBindingsResolved), true},
         {"interactionBindingRejected", Diagnostics::Unsigned(ai.interactionBindingRejected), true},
         {"lastHostAttachment", Diagnostics::Unsigned(ai.lastHostAttachment), true},
         {"lastLocalAttachment", Diagnostics::Unsigned(ai.lastLocalAttachment), true},
         {"lastTransitionVariant", Diagnostics::Unsigned(ai.lastTransitionVariant), true},
         {"lastEntityA", Diagnostics::Unsigned(ai.lastEntityA), true},
         {"lastEntityB", Diagnostics::Unsigned(ai.lastEntityB), true},
         {"lastCharacterClass", Diagnostics::Unsigned(ai.lastCharacterClass), true},
         {"lastActionTypeRva", Diagnostics::Unsigned(ai.lastActionTypeRva), true},
         {"lastActionPathHash", Diagnostics::Unsigned(ai.lastActionPathHash), true},
         {"lastActionStatus", Diagnostics::Unsigned(ai.lastActionStatus), true},
         {"lastRoleTaskTypeRva", Diagnostics::Unsigned(ai.lastRoleTaskTypeRva), true},
         {"lastRoleTaskKeyA", Diagnostics::Unsigned(ai.lastRoleTaskKeyA), true},
         {"lastRoleTaskKeyB", Diagnostics::Unsigned(ai.lastRoleTaskKeyB), true},
         {"lastLifecycleFlags", Diagnostics::Unsigned(ai.lastLifecycleFlags), true},
         {"lastWithdrawState", Diagnostics::Unsigned(ai.lastWithdrawState), true},
         {"lastInteractionTargetA", Diagnostics::Unsigned(ai.lastInteractionTargetA), true},
         {"lastInteractionTargetB", Diagnostics::Unsigned(ai.lastInteractionTargetB), true},
         {"lastInteractionTargetMode", Diagnostics::Unsigned(ai.lastInteractionTargetMode), true},
         {"lastInteractionTargetPeer", Diagnostics::Unsigned(ai.lastInteractionTargetPeer), true},
         {"lastAppliedInteractionTargetPeer", Diagnostics::Unsigned(ai.lastAppliedInteractionTargetPeer), true},
         {"lastAiPlayerQueryPeer", Diagnostics::Unsigned(ai.lastAiPlayerQueryPeer), true},
         {"lastAiPlayerSlotPeer", Diagnostics::Unsigned(ai.lastAiPlayerSlotPeer), true},
         {"lastAppliedInteractionTargetA", Diagnostics::Unsigned(ai.lastAppliedInteractionTargetA), true},
         {"lastAppliedInteractionTargetB", Diagnostics::Unsigned(ai.lastAppliedInteractionTargetB), true},
         {"lastInteractionTransition", Diagnostics::Unsigned(ai.lastInteractionTransition), true},
         {"remoteQueued", Diagnostics::Unsigned(ai.remoteQueued), true},
         {"observed", Diagnostics::Unsigned(ai.observed), true},
         {"applied", Diagnostics::Unsigned(ai.applied), true},
         {"dropped", Diagnostics::Unsigned(ai.dropped), true},
         {"refused", Diagnostics::Unsigned(ai.refused), true},
         {"suspended", Diagnostics::Unsigned(ai.suspended), true},
         {"faults", Diagnostics::Unsigned(ai.faults), true},
         {"missingIdentity", Diagnostics::Unsigned(ai.missingIdentity), true}});
    // Bootstrap and IPC can both request diagnostics. Serialize consumers
    // only; the engine-thread producer never takes this mutex.
    std::lock_guard sampleLock(gParticipantProbeConsumerMutex);
    for (std::size_t i = 0U; i < kParticipantProbeRing; ++i) {
        const auto read = gPresentationProbeRead.load(std::memory_order_relaxed);
        if (read == gPresentationProbeWrite.load(std::memory_order_acquire)) break;
        const auto sample = gPresentationProbes[read % kParticipantProbeRing];
        gPresentationProbeRead.store(read + 1U, std::memory_order_release);
        diagnostics.Log("info", "aisync_presentation", "Exact source/victim/local identities, health and paired result; phase 0=periodic, 1=before admission, 2=after transition, 3=resolved outcome, 4=remote observer completion, 5=remote observer release, 6=observer handoff resume, 7=authoritative native preparation, 8=lost admission recovery. Terminal health alone never completes a paired graph.",
            {{"sessionCookie", Diagnostics::Unsigned(sample.sessionCookie), true},
             {"nativeGeneration", Diagnostics::Unsigned(sample.nativeGeneration), true},
             {"worldGeneration", Diagnostics::Unsigned(sample.worldGeneration), true},
             {"sampleTick", Diagnostics::Unsigned(sample.tick), true},
             {"utcMs", Diagnostics::Unsigned(sample.utcMs), true},
             {"role", Diagnostics::Unsigned(sample.role), true},
             {"phase", Diagnostics::Unsigned(sample.phase), true},
             {"entityA", Diagnostics::Unsigned(sample.entityA), true},
             {"entityB", Diagnostics::Unsigned(sample.entityB), true},
             {"sourceCharacter", Diagnostics::Pointer(reinterpret_cast<const void*>(sample.sourceCharacter))},
             {"sourceRuntimeId", Diagnostics::Unsigned(sample.sourceRuntimeId), true},
             {"birth", Diagnostics::Unsigned(sample.birth), true},
             {"revision", Diagnostics::Unsigned(sample.revision), true},
             {"pairRevision", Diagnostics::Unsigned(sample.pairRevision), true},
             {"movementOwner", Diagnostics::Unsigned(sample.movementOwner), true},
             {"authorityMoveType", Diagnostics::Unsigned(sample.authorityMoveType), true},
             {"authorityIntensity", std::to_string(sample.authorityIntensity), true},
             {"hybridHold",Diagnostics::Unsigned(sample.hybridHold),true},
             {"hybridNativeArmed",Diagnostics::Boolean(sample.hybridNativeArmed),true},
             {"hybridSampleAgeMs",Diagnostics::Unsigned(sample.hybridSampleAge),true},
             {"hybridNativeHooks",Diagnostics::Unsigned(sample.hybridNativeHooks),true},
             {"hybridNativeWrites",Diagnostics::Unsigned(sample.hybridNativeWrites),true},
             {"hybridNativeFailures",Diagnostics::Unsigned(sample.hybridNativeFailures),true},
             {"hybridProbeVersion", Diagnostics::Unsigned(2U), true},
             {"hybridProbe",[&]{std::string text;for(const auto value:sample.hybridProbe){if(!text.empty())text+=' ';text+=std::to_string(value);}return text;}()},
             {"hybridBranches",[&]{std::string text;for(const auto value:sample.hybridBranches){if(!text.empty())text+=' ';text+=std::to_string(value);}return text;}()},
             {"requestedMoveType", Diagnostics::Unsigned(sample.requestedMoveType), true},
             {"effectiveMoveType", Diagnostics::Unsigned(sample.effectiveMoveType), true},
             {"navigationRevision", Diagnostics::Unsigned(sample.navigation.revision), true},
             {"navigationRequestRevision", Diagnostics::Unsigned(sample.navigation.requestRevision), true},
             {"navigationFlags", Diagnostics::Unsigned(sample.navigation.flags), true},
             {"navigationGoalX", std::to_string(sample.navigation.position[0]), true},
             {"navigationGoalY", std::to_string(sample.navigation.position[1]), true},
             {"navigationGoalZ", std::to_string(sample.navigation.position[2]), true},
             {"peer", Diagnostics::Unsigned(sample.peer), true},
             {"victimCharacter", Diagnostics::Pointer(reinterpret_cast<const void*>(sample.victimCharacter))},
             {"victimRuntimeId", Diagnostics::Unsigned(sample.victimRuntimeId), true},
             {"victimEntityA", Diagnostics::Unsigned(sample.victimEntityA), true},
             {"victimEntityB", Diagnostics::Unsigned(sample.victimEntityB), true},
             {"victimLocal", Diagnostics::Boolean(sample.victimLocal), true},
             {"victimRepresentation", Diagnostics::Boolean(sample.victimRepresentation), true},
             {"localCharacter", Diagnostics::Pointer(reinterpret_cast<const void*>(sample.localCharacter))},
             {"localRuntimeId", Diagnostics::Unsigned(sample.localRuntimeId), true},
             {"localHealthReadable", Diagnostics::Boolean(sample.localHealthReadable), true},
             {"localHp", sample.localHealthReadable ? Diagnostics::Unsigned(sample.localHp) : "null", true},
             {"localHealthState", sample.localHealthReadable ? Diagnostics::Unsigned(sample.localHealthState) : "null", true},
             {"victimCycleType", Diagnostics::Unsigned(sample.victimCycleType), true},
             {"victimCycleState", Diagnostics::Unsigned(sample.victimCycleState), true},
             {"victimAcquired", Diagnostics::Unsigned(sample.victimAcquired), true},
             {"victimEnded", Diagnostics::Unsigned(sample.victimEnded), true},
             {"victimInteractive", Diagnostics::Unsigned(sample.victimInteractive), true},
             {"outcomeRevision", Diagnostics::Unsigned(sample.outcome.revision), true},
             {"outcomeFlags", Diagnostics::Unsigned(sample.outcome.flags), true},
             {"outcomeDefinition", Diagnostics::Unsigned(sample.outcome.definition), true},
             {"outcomeKind", Diagnostics::Unsigned(sample.outcome.kind), true},
             {"outcomeHpBefore", Diagnostics::Unsigned(sample.outcome.hpBefore), true},
             {"outcomeHpPrepared", Diagnostics::Unsigned(sample.outcome.hpPrepared), true},
             {"outcomeHpReaction", Diagnostics::Unsigned(sample.outcome.hpReaction), true},
             {"outcomeHealthReaction", Diagnostics::Unsigned(sample.outcome.healthReaction), true},
             {"lifecycleFlags", Diagnostics::Unsigned(sample.lifecycleFlags), true},
             {"interactionFlags", Diagnostics::Unsigned(sample.interactionFlags), true},
             {"cycleType", Diagnostics::Unsigned(sample.cycleType), true},
             {"cycleState", Diagnostics::Unsigned(sample.cycleState), true},
             {"cycleFlags", Diagnostics::Unsigned(sample.cycleFlags), true},
             {"sourceGraphPresent", Diagnostics::Boolean(sample.sourceGraphPresent), true},
             {"navigationHeadReadable", Diagnostics::Boolean(sample.navigationHead.readable), true},
             {"navigationHeadCount", Diagnostics::Unsigned(sample.navigationHead.count), true},
             {"navigationHeadTypeRva", Diagnostics::Unsigned(sample.navigationHead.typeRva), true},
             {"navigationHeadKind", Diagnostics::Unsigned(sample.navigationHead.kind), true},
             {"navigationHeadStatus", Diagnostics::Unsigned(sample.navigationHead.status), true},
             {"stage", Diagnostics::Unsigned(sample.stage), true},
             {"withdrawState", Diagnostics::Unsigned(sample.withdrawState), true},
             {"hidden", Diagnostics::Unsigned(sample.hidden), true},
             {"invisible", sample.invisibleReadable ? Diagnostics::Unsigned(sample.invisible) : "null", true},
             {"invisibleReadable", Diagnostics::Boolean(sample.invisibleReadable), true},
             {"enabled", Diagnostics::Unsigned(sample.enabled), true},
             {"positionReadable", Diagnostics::Boolean(sample.positionReadable), true},
             {"victimReadable", Diagnostics::Boolean(sample.victimReadable), true},
             {"victimHealthReadable", Diagnostics::Boolean(sample.victimHealthReadable), true},
             {"victimHp", sample.victimHealthReadable ? Diagnostics::Unsigned(sample.victimHp) : "null", true},
             {"victimHealthState", sample.victimHealthReadable ? Diagnostics::Unsigned(sample.victimHealthState) : "null", true},
             {"visibilityReadable", Diagnostics::Boolean(sample.visibilityReadable), true},
             {"cycleReadable", Diagnostics::Boolean(sample.cycleReadable), true},
             {"ownsPair", Diagnostics::Boolean(sample.ownsPair), true},
             {"observerReleasedPairRevision", Diagnostics::Unsigned(sample.observerReleasedPairRevision), true},
             {"observerHandoffNeutral", Diagnostics::Boolean(sample.observerHandoffNeutral), true},
             {"placementPending", Diagnostics::Boolean(sample.placementPending), true},
             {"pairPlacementPending", Diagnostics::Boolean(sample.pairPlacementPending), true},
             {"localX", sample.positionReadable ? std::to_string(sample.local[0]) : "null", true},
             {"localY", sample.positionReadable ? std::to_string(sample.local[1]) : "null", true},
             {"localZ", sample.positionReadable ? std::to_string(sample.local[2]) : "null", true},
             {"authorityX", std::to_string(sample.authority[0]), true},
             {"authorityY", std::to_string(sample.authority[1]), true},
             {"authorityZ", std::to_string(sample.authority[2]), true},
             {"victimX", sample.victimReadable ? std::to_string(sample.victim[0]) : "null", true},
             {"victimY", sample.victimReadable ? std::to_string(sample.victim[1]) : "null", true},
             {"victimZ", sample.victimReadable ? std::to_string(sample.victim[2]) : "null", true},
             {"authorityVictimX", std::to_string(sample.authorityVictim[0]), true},
             {"authorityVictimY", std::to_string(sample.authorityVictim[1]), true},
             {"authorityVictimZ", std::to_string(sample.authorityVictim[2]), true}});
    }
    for (std::size_t i = 0U; i < kParticipantProbeRing; ++i) {
        const auto read = gParticipantProbeRead.load(std::memory_order_relaxed);
        if (read == gParticipantProbeWrite.load(std::memory_order_acquire)) break;
        const auto sample = gParticipantProbes[read % kParticipantProbeRing];
        gParticipantProbeRead.store(read + 1U, std::memory_order_release);
        diagnostics.Log("info", "aisync_participant_perception", "Sampled retail participant candidate, senses, threat and selected target; observation only.",
            {{"sessionCookie", Diagnostics::Unsigned(sample.sessionCookie), true},
             {"nativeGeneration", Diagnostics::Unsigned(sample.nativeGeneration), true},
             {"worldGeneration", Diagnostics::Unsigned(sample.worldGeneration), true},
             {"sampleTick", Diagnostics::Unsigned(sample.tick), true},
             {"entityA", Diagnostics::Unsigned(sample.entityA), true},
             {"entityB", Diagnostics::Unsigned(sample.entityB), true},
             {"characterClass", Diagnostics::Unsigned(sample.characterClass), true},
             {"peer", Diagnostics::Unsigned(sample.peer), true},
             {"living", Diagnostics::Boolean(sample.living), true},
             {"playerComponentsReadable", Diagnostics::Boolean(sample.playerComponentsReadable), true},
             {"playerControlPresent", Diagnostics::Boolean(sample.playerControlPresent), true},
             {"playerControlLocal", Diagnostics::Unsigned(sample.playerControlLocal), true},
             {"playerNavigationPresent", Diagnostics::Boolean(sample.playerNavigationPresent), true},
             {"backstageReadable", Diagnostics::Boolean(sample.backstageReadable), true},
             {"backstageState", Diagnostics::Unsigned(sample.backstageState), true},
             {"backstageKeyA", Diagnostics::Unsigned(sample.backstageKeyA), true},
             {"backstageKeyB", Diagnostics::Unsigned(sample.backstageKeyB), true},
             {"backstageEnabled", Diagnostics::Unsigned(sample.backstageEnabled), true},
             {"backstagePolicy", Diagnostics::Unsigned(sample.backstagePolicy), true},
             {"backstageControllerReadable", Diagnostics::Boolean(sample.backstageControllerReadable), true},
             {"backstageControllerState", Diagnostics::Unsigned(sample.backstageControllerState), true},
             {"backstageRequestC", Diagnostics::Unsigned(sample.backstageRequestC), true},
             {"backstageRequestD", Diagnostics::Unsigned(sample.backstageRequestD), true},
             {"backstageRequestE", Diagnostics::Unsigned(sample.backstageRequestE), true},
             {"backstageDistanceReadable", Diagnostics::Boolean(sample.backstageDistanceReadable), true},
             {"backstageVentDistance", std::to_string(sample.backstageDistanceReadable ? sample.backstageVentDistance : 0.0F), true},
             {"navigationCached", Diagnostics::Boolean(sample.navigationCached), true},
             {"navigationGoalKnown", Diagnostics::Boolean(sample.navigationGoalKnown), true},
             {"navigationRouteCount", Diagnostics::Unsigned(sample.navigationRouteCount), true},
             {"navigationQueryPending", Diagnostics::Boolean(sample.navigationQueryPending), true},
             {"navigationGoalDirty", Diagnostics::Boolean(sample.navigationGoalDirty), true},
             {"navigationEmptyRouteRetries", Diagnostics::Unsigned(sample.navigationEmptyRouteRetries), true},
             {"signalRegistryReadable", Diagnostics::Boolean(sample.signalRegistryReadable), true},
             {"signalRegistryFound", Diagnostics::Boolean(sample.signalRegistryFound), true},
             {"signalRegistryMatches", Diagnostics::Boolean(sample.signalRegistryMatches), true},
             {"visibilityFlagsReadable", Diagnostics::Boolean(sample.visibilityFlagsReadable), true},
             {"ownerHidden", Diagnostics::Unsigned(sample.ownerHidden), true},
             {"targetHidden", Diagnostics::Unsigned(sample.targetHidden), true},
             {"pairReadable", Diagnostics::Boolean(sample.pairReadable), true},
             {"paired", Diagnostics::Boolean(sample.paired), true},
             {"executionReadable", Diagnostics::Boolean(sample.executionReadable), true},
             {"retailEligibility", Diagnostics::Unsigned(sample.retailEligibility), true},
             {"executionEnabled", Diagnostics::Unsigned(sample.executionEnabled), true},
             {"executionStateA", Diagnostics::Unsigned(sample.executionStateA), true},
             {"executionStateB", Diagnostics::Unsigned(sample.executionStateB), true},
             {"executionPhase", Diagnostics::Unsigned(sample.executionPhase), true},
             {"ownerExecutionReadable", Diagnostics::Boolean(sample.ownerExecutionReadable), true},
             {"ownerRetailEligibility", Diagnostics::Unsigned(sample.ownerRetailEligibility), true},
             {"ownerGatesReadable", Diagnostics::Boolean(sample.ownerGatesReadable), true},
             {"ownerEnabled", Diagnostics::Unsigned(sample.ownerEnabled), true},
             {"ownerSenseBlocked", Diagnostics::Unsigned(sample.ownerSenseBlocked), true},
             {"sensesReadable", Diagnostics::Boolean(sample.sensesReadable), true},
             {"sensesDisabled", Diagnostics::Unsigned(sample.sensesDisabled), true},
             {"recordPresent", Diagnostics::Boolean(sample.recordPresent), true},
             {"recordReadable", Diagnostics::Boolean(sample.recordReadable), true},
             {"recordActive", Diagnostics::Unsigned(sample.recordActive), true},
             {"recordSelected", Diagnostics::Unsigned(sample.recordSelected), true},
             {"recordForced", Diagnostics::Unsigned(sample.recordForced), true},
             {"sightInput", sample.recordReadable ? std::to_string(sample.sightInput) : "null", true},
             {"sightTimestamp", sample.recordReadable ? std::to_string(sample.sightTimestamp) : "null", true},
             {"activeSenseMask", Diagnostics::Unsigned(sample.activeSenseMask), true},
             {"primarySenseIndex", Diagnostics::Unsigned(sample.primarySenseIndex), true},
             {"recentSenseIndex", Diagnostics::Unsigned(sample.recentSenseIndex), true},
             {"threatReadable", Diagnostics::Boolean(sample.threatReadable), true},
             {"threatPresent", Diagnostics::Boolean(sample.threatPresent), true},
             {"threatState", Diagnostics::Unsigned(sample.threatState), true},
             {"selectedReadable", Diagnostics::Boolean(sample.selectedReadable), true},
             {"selectedPresent", Diagnostics::Boolean(sample.selectedPresent), true},
             {"selectedPeer", Diagnostics::Unsigned(sample.selectedPeer), true},
             {"selectedTerminalDead", Diagnostics::Boolean(sample.selectedTerminalDead), true},
             {"pendingReadable", Diagnostics::Boolean(sample.pendingReadable), true},
             {"pendingPresent", Diagnostics::Boolean(sample.pendingPresent), true},
             {"pendingPeer", Diagnostics::Unsigned(sample.pendingPeer), true},
             {"pendingLiving", Diagnostics::Boolean(sample.pendingLiving), true},
             {"targetSignalsReadable", Diagnostics::Boolean(sample.targetSignalsReadable), true},
             {"haveTargetSignal", Diagnostics::Unsigned(sample.haveTargetSignal), true},
             {"haveNextTargetSignal", Diagnostics::Unsigned(sample.haveNextTargetSignal), true}});
    }
    for (std::size_t i = 0U; i < kParticipantProbeRing; ++i) {
        const auto read = gTargetSuccessionProbeRead.load(std::memory_order_relaxed);
        if (read == gTargetSuccessionProbeWrite.load(std::memory_order_acquire)) break;
        const auto sample = gTargetSuccessionProbes[read % kParticipantProbeRing];
        gTargetSuccessionProbeRead.store(read + 1U, std::memory_order_release);
        diagnostics.Log("info", "aisync_target_succession", "Observed post-kill target-selector mutation/recovery boundary; diagnostics only, no target is injected.",
            {{"sessionCookie", Diagnostics::Unsigned(sample.sessionCookie), true},
             {"nativeGeneration", Diagnostics::Unsigned(sample.nativeGeneration), true},
             {"worldGeneration", Diagnostics::Unsigned(sample.worldGeneration), true},
             {"sampleTick", Diagnostics::Unsigned(sample.tick), true},
             {"entityA", Diagnostics::Unsigned(sample.entityA), true},
             {"entityB", Diagnostics::Unsigned(sample.entityB), true},
             {"birth", Diagnostics::Unsigned(sample.birth), true},
             {"interactionRevision", Diagnostics::Unsigned(sample.interactionRevision), true},
             {"pairRevision", Diagnostics::Unsigned(sample.pairRevision), true},
             {"phase", Diagnostics::Unsigned(sample.phase), true},
             {"retiredPeer", Diagnostics::Unsigned(sample.retiredPeer), true},
             {"selectionReadable", Diagnostics::Boolean(sample.selectionReadable), true},
             {"selectedReadable", Diagnostics::Boolean(sample.selectedReadable), true},
             {"selectedPresent", Diagnostics::Boolean(sample.selectedPresent), true},
             {"selectedPeer", Diagnostics::Unsigned(sample.selectedPeer), true},
             {"selectedBirth", Diagnostics::Unsigned(sample.selectedBirth), true},
             {"selectedLiving", Diagnostics::Boolean(sample.selectedLiving), true},
             {"selectedTerminalDead", Diagnostics::Boolean(sample.selectedTerminalDead), true},
             {"pendingReadable", Diagnostics::Boolean(sample.pendingReadable), true},
             {"pendingPresent", Diagnostics::Boolean(sample.pendingPresent), true},
             {"pendingPeer", Diagnostics::Unsigned(sample.pendingPeer), true},
             {"pendingBirth", Diagnostics::Unsigned(sample.pendingBirth), true},
             {"pendingLiving", Diagnostics::Boolean(sample.pendingLiving), true},
             {"pendingTerminalDead", Diagnostics::Boolean(sample.pendingTerminalDead), true},
             {"targetSignalsReadable", Diagnostics::Boolean(sample.targetSignalsReadable), true},
             {"haveTargetSignal", Diagnostics::Unsigned(sample.haveTargetSignal), true},
             {"haveNextTargetSignal", Diagnostics::Unsigned(sample.haveNextTargetSignal), true}});
    }
}
} // namespace aimp::ai_sync
