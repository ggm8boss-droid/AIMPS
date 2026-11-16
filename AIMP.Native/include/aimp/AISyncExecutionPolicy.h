#pragma once

#include "aimp/AISync.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace aimp::ai_sync {

enum class PairedPreparationPlan : std::uint8_t { Prepare, Ignore, AbortHostSource };
[[nodiscard]] constexpr PairedPreparationPlan PlanPairedPreparation(Role role, bool hasCarrier,
    bool duplicate, bool remoteParticipant, bool localClientVictim) noexcept {
    if (duplicate) return PairedPreparationPlan::Ignore;
    if (hasCarrier) return PairedPreparationPlan::Prepare;
    // Returning without native failure leaves an unprepared 4E holding its
    // victim indefinitely. Only the Host may abandon this unbound attempt;
    // Clients still require the exact authoritative result before any effect.
    if (role == Role::Host && remoteParticipant) return PairedPreparationPlan::AbortHostSource;
    if (role == Role::Client || role == Role::FrozenClient || remoteParticipant || localClientVictim)
        return PairedPreparationPlan::Ignore;
    return PairedPreparationPlan::Prepare;
}

inline constexpr std::uint32_t kDisableAlien = 1U;
inline constexpr std::uint32_t kAlienPvP = 2U;
constexpr bool AlienSuppressionRequested(const Role role, const std::uint32_t flags) noexcept {
    return role == Role::Host && (flags & kDisableAlien) != 0U;
}

struct ExecutionTransitionEntry final {
    std::uint32_t state, typeRva, entryRva;
    bool ignoredStackArgument{};
};

// Shared by capture and replay. No unvalidated semantic input is admitted.
inline constexpr std::array<ExecutionTransitionEntry, 7U> kExecutionTransitions{{
    {0x37U, 0x00EABE00U, 0x002577F0U},
    {0x03U, 0x00EABA40U, 0x00257980U},
    {0x09U, 0x00EAC1C0U, 0x00258190U},
    {0x10U, 0x00EADC18U, 0x00258080U},
    {0x5CU, 0x00EACDA8U, 0x00258390U},
    {0x51U, 0x00EAC608U, 0x00258420U},
    // 006625E0 -> 00661270: native locomotion stop. The constructor takes
    // its heading from the LOCAL movement object. Machine code ends in RET 4;
    // its forwarded argument is unused by 00661270, so replay supplies zero.
    {0x3BU, 0x00EABEC0U, 0x002625E0U, true},
}};

// Native execution-cycle constructors (RVA, AI.exe 00d6bb84). Every one
// starts by notifying 0078AAA0, destroys the current cycle in place and
// constructs the new class with the arguments below. Argument kinds were read
// from each constructor body: Value is stored raw; Identity is an object whose
// +54/+58 handle is copied (pointer not retained); Entity is a retained
// Character pointer; Vector is a pointer to four copied floats. Unresolved
// payloads are refused; address readability never determines argument type.
enum class ExecutionArgKind : std::uint32_t { None = 0, Value = 1, Identity = 2, Entity = 3, Vector = 4, Null = 5, Refused = 15 };
enum class TransitionArg : std::uint8_t { None, Value, Identity, Entity, Vector, Ambiguous, FloatBits };
constexpr bool FiniteExecutionFloatBits(const std::uint32_t bits) noexcept {
    return (bits & 0x7F800000U) != 0x7F800000U;
}
enum class TransitionPrologue : std::uint8_t { Simple, Seh, Frame };
struct ExecutionTransitionContract final {
    std::uint32_t rva, state, typeRva, arity;
    TransitionArg args[2];
    TransitionPrologue prologue;
    std::uint32_t sehHandlerRva;
};
inline constexpr std::array<ExecutionTransitionContract, 12U> kExecutionTransitionContracts{{
    {0x258000U, 0x0EU, 0xEAC340U, 1U, {TransitionArg::Value, TransitionArg::None}, TransitionPrologue::Simple, 0U},
    {0x2581D0U, 0x16U, 0xEAC100U, 2U, {TransitionArg::Value, TransitionArg::Value}, TransitionPrologue::Seh, 0xD26279U},
    {0x257D10U, 0x53U, 0xEACE68U, 1U, {TransitionArg::Value, TransitionArg::None}, TransitionPrologue::Simple, 0U},
    // 0065813B/00658180 copy float duration; +48 holds effect bits.
    // Still refused by the intent codec: 00670570 also chooses local timing.
    {0x258100U, 0x56U, 0xEAC908U, 2U, {TransitionArg::FloatBits, TransitionArg::Value}, TransitionPrologue::Simple, 0U},
    // 0066F7F0 -> 00665A30/0066F700 compare both selectors with 1.
    // Both words are values, despite the incomplete old decompiler signature.
    {0x258610U, 0x5AU, 0xEAD558U, 2U, {TransitionArg::Value, TransitionArg::Value}, TransitionPrologue::Simple, 0U},
    // 00657DD7 MOVSS xmm0,[esp+8]; 00657E16 MOVSS [esi+44],xmm0.
    // A float's bits can coincide with mapped memory; never classify it as a pointer.
    {0x257DA0U, 0x57U, 0xEAD918U, 1U, {TransitionArg::FloatBits, TransitionArg::None}, TransitionPrologue::Simple, 0U},
    {0x257760U, 0x27U, 0xEABB00U, 2U, {TransitionArg::Ambiguous, TransitionArg::Value}, TransitionPrologue::Simple, 0U},
    {0x257B40U, 0x40U, 0xEAC788U, 1U, {TransitionArg::Identity, TransitionArg::None}, TransitionPrologue::Seh, 0xD26279U},
    {0x257C30U, 0x52U, 0xEAC6C8U, 1U, {TransitionArg::Identity, TransitionArg::None}, TransitionPrologue::Seh, 0xD26279U},
    {0x2584A0U, 0x58U, 0xEAD3D8U, 1U, {TransitionArg::Identity, TransitionArg::None}, TransitionPrologue::Seh, 0xD262B4U},
    {0x257A40U, 0x3FU, 0xEAC848U, 1U, {TransitionArg::Entity, TransitionArg::None}, TransitionPrologue::Seh, 0xD26279U},
    {0x2586A0U, 0x5BU, 0xEACB08U, 1U, {TransitionArg::Vector, TransitionArg::None}, TransitionPrologue::Frame, 0U},
}};
constexpr const ExecutionTransitionContract* FindExecutionTransitionContract(const std::uint32_t rva) noexcept {
    for (const auto& entry : kExecutionTransitionContracts) if (entry.rva == rva) return &entry;
    return nullptr;
}
constexpr const ExecutionTransitionContract* FindExecutionTransitionContract(
    const std::uint32_t typeRva, const std::uint32_t state) noexcept {
    for (const auto& entry : kExecutionTransitionContracts)
        if (entry.typeRva == typeRva && entry.state == state) return &entry;
    return nullptr;
}

constexpr const ExecutionTransitionEntry* FindExecutionTransition(
    const std::uint32_t typeRva, const std::uint32_t state) noexcept {
    for (const auto& entry : kExecutionTransitions)
        if (entry.typeRva == typeRva && entry.state == state) return &entry;
    return nullptr;
}

enum class MovementOwner : std::uint8_t {
    None, Intent, Navigation, Execution, Paired, Neutral, Consequence
};

struct ParticipantRouteRetry final {
    std::uint64_t emptySince{};
    bool Observe(const bool enabled, const bool queryPending, const bool dirty,
        const std::uint32_t routeCount, const bool nondegenerate, const std::uint64_t now) noexcept {
        if (!enabled || queryPending || dirty || routeCount != 0U || !nondegenerate) {
            emptySince = 0U;
            return false;
        }
        if (emptySince == 0U) emptySince = now;
        if (now - emptySince < 2000ULL) return false;
        emptySince = 0U;
        return true;
    }
};

constexpr bool AICharacterClass(const std::uint32_t type) noexcept {
    // Native template enum: 0 is PLAYER, 2/7 are ANDROID/ANDROID_HEAVY.
    // Fail closed for unadmitted families. The class describes appearance,
    // not network identity; PlayerSync participant ownership is checked
    // independently before NPC admission.
    return type == 1U || type == 2U || type == 3U || type == 4U ||
        type == 6U || type == 7U || type == 9U;
}

// The authority incarnation crosses the network. The runtime token only
// validates the admitted Character within this process; never compare it with
// another machine's token, even when they happen to have the same value.
constexpr bool ImpactTargetLifetimeMatches(const std::uint32_t receivedBirth,
    const std::uint32_t authoritativeBirth, const std::uint32_t currentRuntimeId,
    const std::uint32_t admittedRuntimeId) noexcept {
    return receivedBirth != 0U && receivedBirth == authoritativeBirth &&
        admittedRuntimeId != 0U && currentRuntimeId == admittedRuntimeId;
}

// Source-owned generation, not a Client runtime ID. Births increase within a
// native world; including both prevents a replacement actor in the same world
// from reusing its predecessor's owner epoch. No ownership is granted here.
constexpr std::uint64_t NativeOwnerEpoch(const std::uint32_t generation, const std::uint32_t birth) noexcept {
    return generation == 0U || birth == 0U ? 0U : (static_cast<std::uint64_t>(generation) << 32U) | birth;
}

constexpr bool ExecutionCycleOwnsMovement(const std::uint32_t expectedType,
    const std::uint32_t expectedState, const std::uint32_t currentType,
    const std::uint32_t currentState, const std::uint8_t flags) noexcept {
    return expectedType != 0U && expectedType == currentType &&
        expectedState == currentState && (flags & 6U) == 0U;
}

constexpr std::uint32_t PairedPreparationEntry(const std::uint32_t stage) noexcept {
    // Native 4E execution transitions: both run the existing victim producer.
    // No stage/marker is fabricated and no local animation definition is chosen.
    return stage <= 1U ? 0x002638D0U : stage == 4U ? 0x00263900U : 0U;
}

constexpr bool UnclaimedPairLost(const bool busy, const bool sourcePresent,
    const bool victimPresent, const bool claimed) noexcept {
    return !busy && !sourcePresent && !victimPresent && !claimed;
}

constexpr bool HostSuspendsNavigation(const State& state) noexcept {
    // A live goal may remain queued while retail explicitly stops its physical
    // cycle. Finishing that local stop is not permission to resume the goal.
    return (state.stateFlags & ExecutionValid) != 0U && (state.stateFlags & Moving) == 0U &&
        state.navigation.kind != 4U && state.navigation.kind != 7U &&
        state.executionCycleTypeRva == 0x00EABEC0U && state.executionCycleState == 0x3BU;
}

constexpr bool LifecycleBlocksMovementRecovery(const State& state,
    const std::uint32_t completedLifecycleRevision) noexcept {
    // Traversal owns movement for its whole action. SpatialDiscontinuity is
    // instead an event retained in snapshots until the next lifecycle event:
    // it stops recovery only until that exact placement has been applied.
    return (state.lifecycleFlags & LifecycleTraversalAction) != 0U ||
        ((state.lifecycleFlags & LifecycleSpatialDiscontinuity) != 0U &&
            (state.lifecycleRevision == 0U || state.lifecycleRevision != completedLifecycleRevision));
}

constexpr bool HostPausesPointNavigation(const State& state) noexcept {
    // A live destination is not permission to keep moving while the Host's
    // ordinary Move executor is waiting/stopped. Turn/stance and root-motion
    // cycles must retain their own native ownership instead.
    const bool point = state.navigation.kind == 1U ||
        (state.navigation.kind == 9U && (state.navigation.flags & 2U) != 0U);
    return (state.navigation.flags & 1U) != 0U && point &&
        (state.stateFlags & ExecutionValid) != 0U &&
        state.executionCycleTypeRva == 0x00EABF80U && state.executionCycleState == 0x38U &&
        ((state.stateFlags & Moving) == 0U || state.locomotionIntensity <= 0.0F);
}

constexpr bool PairAdmissionPending(const State& state,
    const std::uint32_t completedPair, const std::uint32_t completedTransition) noexcept {
    return (state.stateFlags & InteractionValid) != 0U &&
        state.interactionPairRevision != 0U && state.interactionTransition != 0U &&
        (state.interactionPairRevision != completedPair ||
            state.interactionTransitionRevision != completedTransition) &&
        ((state.interactionFlags & (InteractionCycleActive | InteractionSourceCycleRetained)) != 0U ||
            state.outcome.revision != 0U);
}

// Only known locomotion cycles can be handed off by the movement drain. A
// pair, damage reaction, traversal or unknown cycle retains its own executor;
// neither an end flag nor zero HP turns it into a movement-only operation.
constexpr bool MovementDrainCycleAllowed(const std::uint32_t typeRva,const std::uint8_t state) noexcept {
    // 00656430 installs the neutral base cycle (RVA EAD858), state 1.
    // 0065BDD0 selects it for NPCs when no execution is active. It is distinct
    // from locomotion Idle; requiring locomotion before admitting the first
    // request deadlocks a Client which is correctly holding this neutral cycle.
    // This only permits the drain. Scheduler, navigation, health, ownership
    // and incarnation checks must still prove quiescence before execution.
    return (typeRva == 0xEAD858U && state == 0x01U) ||
        (typeRva == 0xEABF80U && (state == 0x01U || state == 0x38U)) ||
        (typeRva == 0xEABEC0U && state == 0x3BU);
}

// Alien possession pre-drain. The map Alien enters its spawn Move within one
// bind poll of creation, so the drain usually first sees locomotion-start55.
// Refusing it made the one-shot frontstage Move cancel unreachable and let the
// route carry the Alien into a vent passage75 (~45s without possession). Only
// this frontstage start is added: passages75/76 and traversal entries remain
// native-owned, and the quiet proof still requires empty scheduler/navigation.
constexpr bool AlienPossessionDrainCycleAllowed(const std::uint32_t typeRva,const std::uint8_t state) noexcept {
    return MovementDrainCycleAllowed(typeRva,state)||(typeRva==0xEABE00U&&state==0x37U);
}

// The pose motor uses the certified native 00657900 Move transition, without
// acquiring a path/scheduler drain. Host constructors006577F0/00657980 can
// legitimately leave it in locomotion-start37 or neutral3 after an action.
// These extra entries do NOT relax the separate movement-drain proof above.
constexpr bool HybridLocomotionCycleAllowed(const std::uint32_t typeRva,const std::uint8_t state) noexcept {
    return MovementDrainCycleAllowed(typeRva,state)||
        (typeRva==0xEABE00U&&state==0x37U)||(typeRva==0xEABA40U&&state==3U);
}
constexpr bool HybridMayConsumeConstructor(const std::uint32_t contract,const std::uint32_t typeRva,
    const std::uint8_t state) noexcept {
    // A movement constructor is also the Host's actual exit from reactions,
    // scripts and interactions. Only an already ordinary cycle can omit it.
    return (contract==1U||contract==2U||contract==7U)&&HybridLocomotionCycleAllowed(typeRva,state);
}

constexpr bool NavigationAuthorityAdvanced(const std::uint32_t flags,
    const std::uint32_t revision, const std::uint32_t requestRevision,
    const std::uint32_t releasedRevision, const std::uint32_t releasedRequestRevision) noexcept {
    return (flags & 1U) != 0U && (revision > releasedRevision || requestRevision > releasedRequestRevision);
}

constexpr bool HostLeftPairedExecution(const bool executionValid, const bool executionPaired,
    const bool interactionActive, const bool sourceRetained) noexcept {
    return executionValid && !executionPaired && !interactionActive && !sourceRetained;
}

// The observer's local End_Action may be retained while the Host still plays
// its outro. Both facts are required before forwarding that deferred marker.
constexpr bool DeferredSourceEndReady(const bool currentLifetime, const bool hostLeft,
    const bool localEndAction, const bool exactGraphRetained, const bool nonLethalBranch) noexcept {
    return currentLifetime && hostLeft && localEndAction && exactGraphRetained && nonLethalBranch;
}
constexpr bool DeadVictimNativeEndReady(const bool hostLeft,const bool localVictim,
    const bool terminalVictim,const bool nativeDeathApplied) noexcept {
    // HP zero alone is insufficient. A returned native victim-finish callback
    // and the Host's source exit can replace a missing local animation marker.
    return hostLeft&&localVictim&&terminalVictim&&nativeDeathApplied;
}

// Metadata gives no local ownership until replay succeeds for this revision.
constexpr MovementOwner ChooseMovementOwner(const bool paired, const bool neutral,
    const bool executionApplied, const bool navigationAvailable) noexcept {
    if (paired) return MovementOwner::Paired;
    if (neutral) return MovementOwner::Neutral;
    if (executionApplied) return MovementOwner::Execution;
    if (navigationAvailable) return MovementOwner::Navigation;
    return MovementOwner::Intent;
}

} // namespace aimp::ai_sync
