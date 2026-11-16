#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include "aimp/AISyncHealth.h"
#include "aimp/AISyncAnimation.h"
#include "aimp/AISyncCombat.h"
#include "aimp/AISyncParticipantIdentity.h"
#include "aimp/AISyncActionCorrelation.h"

namespace aimp {
class HookManager;
class LevelGenerationTracker;
class GameThreadDispatcher;
class Diagnostics;
class WorldEntityRegistry;

namespace ai_sync {
// Host decisions are NOT WorldMutation/PlayerSync messages. AISync owns every
// retail NPC Character at the Character/behaviour-runtime boundary. Like
// WorldSync, the wire carries stable identities plus high-level resolved
// carriers only. Animation clips/frames, skeleton state, audio, particles and
// downstream CATHODE world consequences are deliberately absent: retail systems
// regenerate them locally after the carrier is applied. Paired attack results
// carry the Host's resolved native definition and health, without clip frames.
// Process pointers and species-specific state never cross the network.
enum class Role : std::uint32_t { Retail, Host, Client, FrozenClient };

enum StateFlag : std::uint32_t {
    PoseValid = 1U << 0U,
    Moving = 1U << 1U,
    ActionValid = 1U << 2U,
    InteractionValid = 1U << 3U,
    RoleTaskValid = 1U << 4U,
    // Character execution-cycle identity selected by the Host. This is not a
    // serialized native object: the wire carries only the retail cycle type
    // RVA/state plus an authority revision. Clients replay only certified
    // transition entry points and otherwise fail closed.
    ExecutionValid = 1U << 5U,
    // This snapshot carries a live ordered damage event. Ordinary heartbeats
    // retain its identity as history but must never replay its reaction.
    ConsequenceEvent = 1U << 6U,
    ParticipantConsequenceEvent = 1U << 7U,
    ShotEvent = 1U << 8U,
    NavigationEvent = 1U << 9U,
};

enum ExecutionFlag : std::uint32_t {
    ExecutionTransition = 1U << 0U,
    ExecutionCertified = 1U << 1U,
    ExecutionPaired = 1U << 2U,
};

enum LifecycleFlag : std::uint32_t {
    LifecycleSpatialDiscontinuity = 1U << 0U,
    LifecycleTraversalAction = 1U << 1U,
    // Persistent Host lifecycle state. Withdrawn maps the Alien retail
    // withdraw controller (Character+34F0 -> +18); State2 preserves the
    // controller's second non-zero mode. HostDormant covers backstage periods
    // where the BehaviourManager itself no longer ticks.
    LifecycleWithdrawn = 1U << 2U,
    LifecycleWithdrawState2 = 1U << 3U,
    LifecycleHostDormant = 1U << 4U,
    // Generic retail Character backstage (+2DA), independent of Alien withdraw.
    // Persistent value survives skipped/coalesced vent enter/exit snapshots.
    LifecycleCharacterBackstage = 1U << 5U,
};

enum InteractionFlag : std::uint32_t {
    InteractionActorIsSource = 1U << 0U,
    InteractionTargetIsParticipant = 1U << 1U,
    InteractionPairValid = 1U << 2U,
    InteractionCycleActive = 1U << 3U,
    InteractionNativeOutcomeRequired = 1U << 4U,
    // The Host still owns the exact source 42/4E cycle, including its outro
    // after the active attack/victim graph ends. Distinct from Active/health.
    InteractionSourceCycleRetained = 1U << 5U,
};

enum OutcomeFlag : std::uint32_t {
    OutcomePrepared = 1U << 0U,
    OutcomeVictimAdmitted = 1U << 1U,
    OutcomeSourceEnded = 1U << 2U,
    OutcomeVictimEnded = 1U << 3U,
    // Native health observation, NOT completion of the local death flow.
    OutcomeTerminal = 1U << 4U,
    // Fatal 4F initialization or the certified late kill/control/finish block
    // actually ran on Host. Distinct from HP=0 and from death-flow completion.
    OutcomeNativeDeath = 1U << 5U,
};

#pragma pack(push, 1)
struct NativeOutcome final {
    std::uint32_t revision{}, flags{}, definition{}, kind{};
    std::uint32_t hpBefore{}, hpPrepared{}, hpReaction{}, healthReaction{};
    float sourceYaw{}, targetOffset[3]{};
};
struct NavigationGoalState final {
    std::uint32_t revision{}, flags{}; // bit 0: live request; bit 1: proven directed endpoint
    float position[3]{}, stoppingDistance{};
    // Request parameter: point mode, turn style bits, or stance enum. Physical
    // gait remains State.moveType (Movement+194), independently of this union.
    std::uint32_t moveType{}, requestRevision{};
    std::uint32_t kind{}; // 1: point; 9: heading/distance
    float heading{}, turnTolerance{}, distance{}, origin[3]{};
};
struct ConsequenceSource final {
    std::uint64_t peer{};
    std::uint32_t entityA{}, entityB{}, characterClass{}, birth{};
};
struct ParticipantConsequence final {
    std::uint64_t targetPeer{};
    health::DamageEvent damage{};
};
// v15: the native execution-cycle constructor CATHODE called on the Host to
// enter the current cycle, with portable arguments. rva==0: none captured.
// kinds: 4 bits per argument (ExecutionArgKind). Entity identities are the
// Character/entity handle at object+54/+58, never a process pointer.
struct ExecutionCycleCall final {
    std::uint32_t rva{}, kinds{};
    std::uint32_t value[2]{};
    std::uint32_t entityA[2]{}, entityB[2]{};
    float vector[4]{};
    std::uint32_t cycleSerial{};
};
struct State final {
    std::uint32_t version{16U};
    std::uint32_t nativeGeneration{}; // informational: generations differ per process
    std::uint64_t sessionCookie{};
    std::uint32_t worldGeneration{};
    std::uint32_t entityA{}, entityB{}, characterClass{}, birth{}, revision{};
    // Recovery pose is retained only for spawn/lifecycle/catastrophic drift.
    // Continuous movement is NOT position-followed. The typed navigation goal
    // below is preferred for certified native requests. velocityX/Z carry the
    // Host's resolved world-space locomotion request observed after retail
    // pathfinding/local steering. CATHODE on the Client owns the physical move,
    // collision, turning and animation generated from that intention.
    std::uint32_t stateFlags{};
    float position[3]{};
    float yaw{};
    float velocityX{}, velocityZ{};
    std::uint32_t moveType{}; // Host Movement+194 INPUT; +198 is a local output.
    float locomotionIntensity{};
    std::uint32_t locomotionRevision{};
    // Stable action identity/lifecycle metadata for the certified executable:
    // leaf definition vtable RVA plus a hash of the complete active BT path.
    // It is never interpreted as a serialized leaf or animation state.
    std::uint32_t actionTypeRva{}, actionPathHash{}, actionRevision{};
    std::uint32_t actionStatus{}, actionDepth{};
    // Ordered high-level Character-to-Character carrier selected by the Host.
    // The target is remapped locally before invoking the same retail boundary;
    // retail/CATHODE then produce animation, audio and gameplay consequences.
    // Payload/variant values are resolved outputs, never species-specific state.
    std::uint32_t interactionRevision{}, interactionTargetA{}, interactionTargetB{};
    std::uint32_t interactionFlags{}, interactionTransition{}, interactionTransitionFlags{};
    std::uint32_t interactionPayloadLow{}, interactionPayloadHigh{};
    std::int32_t interactionVariant{};
    float interactionTargetPosition[3]{};
    // Role execution is a second decision layer below ATOMIC_PERFORM_ROLE.
    // The Host exports the admitted task identity/revision as an authority
    // epoch. Clients never synthesize native C++ task objects from a vtable;
    // stale local task/BT execution is quiesced before the Host locomotion
    // intention for that epoch is replayed.
    std::uint32_t roleTaskRevision{}, roleTaskTypeRva{}, roleTaskKeyA{}, roleTaskKeyB{};
    // Lifecycle transitions cover vent/backstage/frontstage discontinuities.
    // They are not animation events: a revision means the proxy execution
    // context must be quiesced/rebound before normal convergence resumes.
    std::uint32_t lifecycleRevision{}, lifecycleFlags{};
    // Participant identity is portable; THE_PLAYER and dynamic CATHODE keys
    // are process-local. No Character address is carried by an NPC snapshot.
    std::uint64_t interactionTargetPeer{};
    std::int32_t interactionTransitionVariant{};
    std::uint32_t interactionPairRevision{}, interactionTransitionRevision{};
    // Result of the admitted 4E/4F native preparation, keyed by this source
    // birth + pair revision + target peer/NPC birth. Definition is a retail manager index,
    // never an animation pointer. Clients consume it instead of choosing again.
    NativeOutcome outcome{};
    // Exact Host Character execution-cycle identity. Appended after the v9
    // payload so every established field keeps its prior wire offset.
    std::uint32_t executionRevision{};
    std::uint32_t executionCycleTypeRva{};
    std::uint32_t executionCycleState{};
    std::uint32_t executionFlags{};
    // Resolved Host goal, above pathfinding/steering. The Client builds a
    // local retail request; no corridor, ObjectRef, BT or transform is copied.
    NavigationGoalState navigation{};
    // Absolute health repairs late joins; ordered native damage events regenerate
    // reactions. Their cause is a peer or NPC authority birth, never a pointer.
    health::State health{};
    ConsequenceSource healthSource{};
    health::DamageEvent damage{};
    animation::ScriptedState scripted{};
    ParticipantConsequence participantConsequence{};
    std::uint32_t interactionTargetBirth{}, interactionTargetClass{};
    combat::State combat{};
    combat::ShotEvent shot{};
    // Exact native HP-write provenance. A victim snapshot waits for this pair's
    // consumed outcome; it cannot kill the victim before native admission.
    std::uint32_t healthPairRevision{};
    // v13: exact portable player life selected at Host registration. Zero
    // preserves the uncertified legacy carrier and grants no strict admission.
    std::uint32_t interactionParticipantIncarnation{};
    // Causal seal captured at registration while the Host command source is
    // actually bound. All zero means legacy; never stamp an old pair later.
    std::uint8_t interactionSession[16]{};
    std::uint64_t interactionAuthority{},interactionCommand{};
    // v14: frozen reference to a proven observed kind5 reservation. Source
    // scope consumer=1; the snapshot actor supplies the rest of OwnerKey.
    // All zero is explicitly uncorrelated, never a replay permission.
    action_correlation::Reference executionAction{};
    ExecutionCycleCall transition{};
    // v16: Host physical motion, independent of navigation goals and the
    // animation's control vector. Monotonic Host time, never local/UTC time.
    std::uint64_t motionTick{};
    float motionVelocity[3]{};
    std::uint32_t motionFlags{}; // 1 valid, 2 native action owns motion, 4 discontinuity
};
struct ImpactEvent final {
    // v2 targetBirth is the Host's portable AISync incarnation, never +50.
    std::uint32_t version{2U};
    std::uint32_t nativeGeneration{}; // informational; differs per process
    std::uint64_t sessionCookie{};
    std::uint32_t worldGeneration{};
    std::uint32_t sequence{};
    std::uint64_t sourcePeer{};
    std::uint32_t targetA{}, targetB{}, targetClass{}, targetBirth{};
    std::uint32_t damage{}, descriptor{}, channel{}, reserved{};
};
struct Configuration final {
    std::uint32_t version{3U};
    Role role{Role::Retail};
    std::uint32_t nativeGeneration{}, worldGeneration{};
    std::uint64_t sessionCookie{};
    // Launcher -> local DLL only. Never passed to the lobby transport.
    std::uint64_t localPeer{};
    std::uint32_t participantCount{}, flags{}; // bit 0: suppress Alien; bit 1: explicit PvP possession permission
    ParticipantBinding localBinding{}; // all zero if not certified yet
    ParticipantBinding participants[32]{};
};
#pragma pack(pop)
static_assert(offsetof(Configuration, flags) == 36U);
static_assert(sizeof(NativeOutcome) == 48U);
static_assert(offsetof(State, outcome) == 192U);
static_assert(offsetof(State, executionRevision) == 240U);
static_assert(sizeof(NavigationGoalState) == 60U);
static_assert(offsetof(State, navigation) == 256U);
static_assert(sizeof(ConsequenceSource) == 24U);
static_assert(offsetof(State, health) == 316U);
static_assert(offsetof(State, healthSource) == 332U);
static_assert(offsetof(State, damage) == 356U);
static_assert(offsetof(State, scripted) == 420U);
static_assert(sizeof(ParticipantConsequence) == 72U);
static_assert(offsetof(State, participantConsequence) == 472U);
static_assert(offsetof(State, interactionTargetBirth) == 544U);
static_assert(offsetof(State, combat) == 552U && offsetof(State, shot) == 588U);
static_assert(offsetof(State, healthPairRevision) == 636U);
static_assert(offsetof(State,interactionParticipantIncarnation)==640U&&offsetof(State,interactionSession)==644U&&
    offsetof(State,interactionAuthority)==660U&&offsetof(State,interactionCommand)==668U&&offsetof(State,executionAction)==676U&&offsetof(State,transition)==764U&&
    offsetof(State,motionTick)==816U&&offsetof(State,motionVelocity)==824U&&offsetof(State,motionFlags)==836U&&sizeof(State) == 840U);
static_assert(sizeof(ImpactEvent) == 64U);
static_assert(offsetof(Configuration,localBinding)==40U && offsetof(Configuration,participants)==72U);
static_assert(sizeof(Configuration) == 1096U);

bool Install(std::uintptr_t moduleBase, HookManager& hooks,
    LevelGenerationTracker& generations, GameThreadDispatcher& dispatcher,
    std::string& error, WorldEntityRegistry* scriptReferences = nullptr);
bool Configure(std::span<const std::byte> payload) noexcept;
bool QueueRemote(std::span<const std::byte> payload, std::uint64_t through = 0) noexcept;
void BeginExecutionCarrierScope(const transactions::Scope&) noexcept;
bool ApplyExecutionCarrier(const State&,std::uint64_t through,std::uint64_t arrivalTick) noexcept;
bool QueueRemoteImpact(std::span<const std::byte> payload) noexcept;
bool TryPopObservedImpact(ImpactEvent& impact) noexcept;
void PumpOnEngineThread() noexcept;
// Engine thread, after retail CharacterManager: reconcile only ordinary NPC
// motion. Paired kills, passages, reactions, and player puppets are excluded.
void ReconcileDirectPoseOnEngineThread() noexcept;
// SpawnCharacter creates the NPC-backed visual representation of a remote
// player. Reserve its runtime identity before native construction because the
// Character manager can update it re-entrantly before CharacterCreate returns.
void ObservePlayerRepresentationSpawn(std::uint32_t spawnKey) noexcept;
void ObservePlayerRepresentation(std::uintptr_t character) noexcept;
void BeforeCharacterDestruction(std::uintptr_t character) noexcept;
void ForgetCharacter(std::uintptr_t character) noexcept;
void ForgetSpawnKey(std::uint32_t spawnKey) noexcept;
void TransportLost() noexcept;
void BeginLogicalShutdown() noexcept;
// True while AISync, rather than retail AI, owns the NPC authority boundary.
bool OwnsAI() noexcept;
bool Installed() noexcept;
// True while an AI actor pairs with this participant Character (kill/grab),
// and briefly after. Any thread; network adapters must not move or re-equip it.
bool PairedParticipantVictim(std::uintptr_t character) noexcept;
// Certified engine-thread query, only the possessed Alien's active native pair.
bool AlienPvPCaptureVictim(std::uintptr_t character) noexcept;
struct Counters final {
    std::uint64_t observed{}, applied{}, dropped{}, refused{}, suspended{}, faults{};
    std::uint64_t missingIdentity{};
    std::uint64_t managerUpdates{}, admitted{}, identityRejected{}, treeUnavailable{}, ownerMismatch{};
    std::uint64_t remoteQueued{};
    std::uint64_t poseCaptured{}, poseApplied{}, poseRejected{};
    std::uint64_t intentCaptured{}, intentApplied{}, intentRejected{}, intentRecoveries{};
    std::uint64_t actionCaptured{}, actionTransitions{};
    std::uint64_t executionCaptured{}, executionTransitions{}, executionReplayed{}, executionReplayRejected{};
    std::uint64_t roleTaskCaptured{}, roleTaskTransitions{};
    std::uint64_t lifecycleCaptured{}, lifecycleApplied{}, lifecycleSnaps{}, executionBarriers{};
    std::uint64_t impactCaptured{}, impactQueued{}, impactApplied{}, impactRejected{};
    std::uint64_t impactShooterRemaps{}, impactShooterUnresolved{};
    std::uint64_t backstageCaptured{}, backstageTransitions{}, backstageApplied{}, backstageRejected{};
    std::uint64_t placementVerified{}, placementRejected{}, lifecycleRetries{};
    std::uint64_t withdrawSamples{}, withdrawTransitions{}, withdrawApplied{};
    std::uint64_t interactionCaptured{}, interactionApplied{}, interactionRejected{}, interactionRemapped{};
    std::uint64_t pairedTransitionCaptured{}, pairedTransitionApplied{};
    std::uint64_t playerRepresentationsRegistered{}, provisionalPlayerActorsRetired{};
    std::uint64_t participantCandidates{}, participantTargetRejected{}, deadTargetRecoveries{};
    std::uint64_t participantCandidatePasses{};
    std::uint64_t senseEventPasses{};
    std::uint64_t participantSignalLookups{};
    std::uint64_t participantSignalRemaps{};
    std::uint64_t participantSignalRetailMissing{};
    std::uint64_t participantSignalRetailMismatch{};
    std::uint64_t participantSignalAmbiguous{};
    std::uint64_t lastParticipantSignalPeer{};
    std::uint64_t participantPairLookups{}, participantPairRemaps{};
    std::uint64_t participantPairRetailMissing{}, participantPairAmbiguous{};
    std::uint64_t lastParticipantPairPeer{};
    std::uint64_t pairedInitializeSucceeded{}, pairedInitializeFailed{}, pairedCycleReleased{};
    std::uint64_t pairedObserverCompletions{}, pairedObserverReleaseAttempts{}, pairedObserverReleases{};
    std::uint64_t pairedObserverReleaseRejected{}, pairedObserverPairReopenSuppressed{};
    std::uint64_t pairedObserverNeutralHolds{}, pairedObserverHandoffResumed{};
    std::uint64_t pairedVictimTransitions{}, pairedVictimTransitionRejected{}, pairedVictimInitializeAborted{};
    std::uint64_t pairedVictimEventsRejected{}, pairedVictimDuplicates{}, pairedVictimBusy{}, pairedVictimNativeDispatches{}, pairedVictimNativeStarted{}, pairedVictimAvailabilityOverrides{};
    std::uint64_t pairedPlayerPreparationRemaps{}, pairedPlayerCategoryRemaps{};
    std::uint64_t pairedVictimLocalDispatches{}, pairedVictimLocalStarted{};
    std::uint64_t pairedVictimConvergenceDetached{}, pairedVictimSnapshotsDeferred{};
    std::uint64_t pairedVictimHealthContinuations{}, pairedVictimWorkerDispatches{};
    std::uint64_t pairedVictimLocalAvailabilityOverrides{}, pairedDeadTargetRejected{}, pairedRecoveryDeferred{};
    std::uint64_t pairedSourcePlacements{}, pairedSourcePlacementRejected{}, pairedObserverSnapshotsDeferred{};
    std::uint64_t pairedSourcePlacementDeferred{}, pairedSourcePlacementResumed{}, pairedSourcePlacementCancelled{};
    std::uint64_t pairedSourcePlacementExpired{}, pairedSourcePoseSuppressed{};
    std::uint64_t presentationSamples{}, presentationDropped{};
    std::uint32_t lastVictimAdmissionHp{}, lastVictimAdmissionHealth{}, lastVictimAdmissionEnabled{};
    std::uint32_t lastRejectedPairedSourceState{}, lastRejectedPairedReason{};
    std::uint64_t candidateEligibilityChecks{};
    std::uint64_t candidateEligibilityOverrides{};
    std::uint64_t recordEligibilityChecks{};
    std::uint64_t recordEligibilityOverrides{};
    std::uint64_t participantRecordsValidated{};
    std::uint64_t participantRecordsRetained{};
    std::uint64_t participantRecordsRejected{};
    std::uint64_t participantPerceptionSamples{};
    std::uint64_t participantPerceptionDropped{};
    std::uint64_t targetSuccessionSamples{}, targetSuccessionDropped{};
    std::uint64_t postDeathSuccessionsPreserved{}, postDeathArbitrationDeferrals{}, postDeathDecisionUncertain{};
    std::uint64_t aiPlayerQueryRemaps{}, aiPlayerSlotRemaps{}, aiPlayerSlotRestoreFailures{};
    std::uint64_t postDeathMeleeSelectionChecks{}, postDeathMeleeSelectionRejects{};
    std::uint64_t postDeathMeleeSelectionWriteFailures{}, lastPostDeathRetiredPeer{};
    std::uint64_t postDeathDecisionResets{}, postDeathDecisionResetRejected{}, postDeathRoleTaskResets{};
    std::uint64_t interactionBindingsResolved{}, interactionBindingRejected{};
    std::uint32_t lastHostAttachment{}, lastLocalAttachment{}, lastTransitionVariant{};
    std::uint32_t role{}, nativeGeneration{}, worldGeneration{};
    std::uint32_t lastEntityA{}, lastEntityB{}, lastCharacterClass{};
    std::uint32_t lastActionTypeRva{}, lastActionPathHash{}, lastActionStatus{};
    std::uint32_t lastExecutionTypeRva{}, lastExecutionState{}, lastExecutionFlags{};
    std::uint32_t lastRejectedExecutionTypeRva{}, lastRejectedExecutionState{};
    std::uint32_t lastRoleTaskTypeRva{}, lastRoleTaskKeyA{}, lastRoleTaskKeyB{};
    std::uint32_t lastLifecycleFlags{}, lastWithdrawState{};
    std::uint32_t lastInteractionTargetA{}, lastInteractionTargetB{}, lastInteractionTargetMode{};
    std::uint64_t lastInteractionTargetPeer{}, lastAppliedInteractionTargetPeer{};
    std::uint64_t lastAiPlayerQueryPeer{}, lastAiPlayerSlotPeer{};
    std::uint32_t lastAppliedInteractionTargetA{}, lastAppliedInteractionTargetB{};
    std::uint32_t lastInteractionTransition{};
    bool stable{};
};
Counters DiagnosticsSnapshot() noexcept;

// Read-only identity probe. It never grants possession, creates a Character,
// modifies CATHODE state or exports process-local Character pointers.
struct AlienPvPProbe final {
    std::uint32_t nativeGeneration{}, worldGeneration{}, candidates{};
    std::uint32_t entityA{}, entityB{}, runtimeId{};
    bool unique{};
};
bool ProbeAlienPvPCandidate(AlienPvPProbe& result) noexcept;

// Native PvP possession is local to one engine instance. A lease grants
// exclusive authority over exactly one retail Alien Character, never a class.
// No process address or pointer is returned to the Launcher or network.
enum class AlienPvPLeasePhase : std::uint32_t {
    Unavailable, WaitingForAlien, Ambiguous, AwaitingRetirement,
    Draining, Owned, Released, Stale
};
struct AlienPvPLeaseStatus final {
    AlienPvPLeasePhase phase{AlienPvPLeasePhase::Unavailable};
    std::uint64_t ownerPeer{};
    std::uint32_t entityA{}, entityB{}, runtimeId{}, nativeGeneration{}, worldGeneration{};
    std::uint32_t boundaryOffset{}, boundaryValue{};
    std::uint32_t retainedKind{},retainedStatus{},retainedRetired{};
    bool isLocalOwner{}, localPilotReady{};
    bool pilotInputIsolated{}, pilotCameraBound{}, pilotTechnicalHidden{}, pilotMoving{};
    const char* waitReason{"none"}; // static local diagnostic text, never a wire pointer
};
bool AcquireAlienPvPLease(std::uint64_t ownerPeer, AlienPvPLeaseStatus& status) noexcept;
bool ReleaseAlienPvPLease(AlienPvPLeaseStatus& status) noexcept;
// Copied destructor-return proof, drained on the engine thread.
void ConfirmAlienPvPDestruction(std::uintptr_t character,std::uint32_t runtime,
    std::uint64_t serial,std::uint32_t nativeGeneration) noexcept;
bool SnapshotAlienVents(std::uint32_t page,std::string& json,int goTo=-1,std::uint64_t incarnation=0);
// Engine-thread participant identity for source-attributed native subsystems.
// Uses the same native generation/lifetime proof as perception; never chooses a
// target. The launcher's world generation is AISync-private and not compared.
std::uintptr_t ResolveLivingParticipant(std::uint64_t peer, std::uint32_t nativeGeneration) noexcept;
// Call only from IPC/bootstrap workers, never from an engine detour.
void LogDiagnostics(Diagnostics& diagnostics);
} // namespace ai_sync
} // namespace aimp
