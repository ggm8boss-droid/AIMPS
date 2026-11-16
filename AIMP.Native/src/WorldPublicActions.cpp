#include "aimp/AISync.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/PlayerLocalControls.h"
#include "aimp/DeferredDiagnosticBuffer.h"
#include "aimp/Diagnostics.h"
#include "aimp/RuntimeFeatureFlags.h"

#include "aimp/BuildProfile.h"
#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/PlayerLifecycle.h"
#include "aimp/HookManager.h"
#include "aimp/ReplicationCore.h"
#include "aimp/SignatureResolver.h"
#include "aimp/WorldActionLineage.h"
#include "aimp/WorldAnimatedAdapters.h"
#include "aimp/MissionSync.h"

#include <Windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace aimp {
struct WorldPublicActionLeaseAccess final {
    [[nodiscard]] static void* SharedRef(const WorldEntityLease& lease) noexcept {
        return lease.sharedRef_;
    }
};
namespace world_actions {
namespace {

#if defined(_WIN32)
static_assert(sizeof(void*) == 4U, "WS3 requires the certified MSVC x86 retail ABI.");
#endif

// STATIC: supplied AI.exe (image base 00400000, .text RVA 00001000).
// 00965920 == .text+564920 == image RVA 00565920, ret 18h.
// 00926680 -> OWNER holder's entity.vtable+70 -> 008BD920 -> interface.vtable+394.
// Its TARGET holder is a separate argument, forwarded as `instance` below.
// Public arguments are borrowed holders; callbackHolder itself MUST be valid
// even when *callbackHolder is null.
using PublicFn = void (__thiscall*)(void*, void**, const std::uint32_t*,
                                   void**, double, std::uint32_t);
using DefinitionFn = void* (__thiscall*)(void*);
using WorkBuildFn = void* (__thiscall*)(void*, void**, const std::uint32_t*,
    void*, std::uint32_t, double, void**, void**); // 009265C0 ret 20h
using WorkExecuteFn = std::uint8_t (__thiscall*)(void*, void**, void**, void**);
using WorkDestroyFn = void (__thiscall*)(void*); // 00922800 ret

// STATIC CONFIRMED in the supplied retail binary and C decompilation:
// 00965720 constructs CompositeInstance: Entity vptr = 013E1D2C at +00,
// CompositeInterface vptr = 013E196C at +1C. The secondary vtable's MSVC
// CompleteObjectLocator has offset=1C and the same type as the primary.
// Secondary slot +394 is 00965920; primary slot +70 is 0096FB90, which returns
// ECX+1C (or null). All addresses below are IMAGE RVAs, adjusted for ASLR.
// This is a checked secondary-base conversion, not IdentifyRuntimeEntity(self)
// and not a search through descendants or manifest reverse mappings.
constexpr std::uintptr_t kCompositeEntityVtableRva = 0x00FE1D2CU;
constexpr std::uintptr_t kCompositeInterfaceVtableRva = 0x00FE196CU;
constexpr std::uintptr_t kCompositeInterfaceGetterRva = 0x0056FB90U;
constexpr std::uintptr_t kCompositeInterfaceOffset = 0x1CU;

// STATIC CONFIRMED in the supplied retail binary:
// Entity::slot+11C == 008C29A0. It tests Entity flags bit 0x8000 and, only
// when that bit is set, resolves a runtime association through the CATHODE
// manager before 008BE0D0 continues through the resolved entity's +70/+394.
// WM08 intentionally does not invent a static COMMANDS edge for that runtime
// association. An EmittedOutput ROOT is therefore complete only when this
// implicit leg is provably absent at the capture boundary.
constexpr std::uintptr_t kEntityForwardingResolverRva = 0x004C29A0U;
constexpr std::uint32_t kEntityForwardingFlag = 0x00008000U;

std::atomic<void*> gOutputTrampoline{}, gDrainTrampoline{};
void* gOutputRaw{}; void* gDrainRaw{};
std::atomic<LocalEmittedOutputObserver> gLocalOutputObserver{};
std::atomic<void*> gPublicTrampoline{}, gBuildTrampoline{}, gExecuteTrampoline{}, gDestroyTrampoline{};
void* gPublicRaw{}; void* gBuildRaw{}; void* gExecuteRaw{}; void* gDestroyRaw{};
std::atomic<WorldEntityRegistry*> gRegistry{};
std::atomic<bool> gInstalled{}, gEnabled{};
std::atomic<std::uint64_t> gLocalPeer{};
std::atomic<bool> gAuthorityBound{};
std::atomic<bool> gAuthorityHost{};
std::atomic<bool> gWorldBusActive{};
std::atomic<bool> gEmptyCallbackTest{};
// Serialize bootstrap fallback with IPC updates. An IPC value, including OFF,
// must never be replaced by inherited environment during a later Arm/retry.
std::mutex gEmptyCallbackConfigMutex;
bool gEmptyCallbackRuntimeConfigured{};
std::atomic<std::uint32_t> gEpoch{1U};
std::uintptr_t gModule{};
void* gPublicTarget{};
std::mutex gLineageMutex;
LineageTable<> gLineage;
CausalProvenanceTable<32768U> gCausalProvenance;
CausalGroupTable<32768U> gCausalGroups;
std::atomic<std::size_t> gLiveTags{};
std::atomic<std::size_t> gLiveCausalTags{};
std::atomic<std::size_t> gLiveCausalGroups{};
std::atomic<std::size_t> gLiveParentContexts{};
std::atomic<std::uint64_t> gCausalChainSerial{1U};
// v0.11.4.57: the pre-QueueInput callback bridge is retired from gameplay.
// Runtime tracing proved the Door hits were generic behavior controls
// (stop -> activate_behavior/deactivate_behavior), not the Door lock cause.
// Keep the code/diagnostics available for postmortem comparison, but never
// open a bridge scope in normal WS3 capture.
constexpr bool kCallbackBridgeEnabled = false;

struct IdentityCandidateCounters final {
    std::atomic<std::uint64_t> runtimeKeyValid{}, runtimeKeyReject{}, candidateValid{},
        candidateReject{}, publicInputValid{}, publicInputReject{};

    void Observe(bool keyValid, bool idValid, bool inputAllowed) noexcept {
        (keyValid ? runtimeKeyValid : runtimeKeyReject).fetch_add(1U, std::memory_order_relaxed);
        (idValid ? candidateValid : candidateReject).fetch_add(1U, std::memory_order_relaxed);
        if (idValid)
            (inputAllowed ? publicInputValid : publicInputReject).fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] IdentityCandidateDiagnostics Snapshot() const noexcept {
        return {runtimeKeyValid.load(std::memory_order_relaxed),
            runtimeKeyReject.load(std::memory_order_relaxed),
            candidateValid.load(std::memory_order_relaxed), candidateReject.load(std::memory_order_relaxed),
            publicInputValid.load(std::memory_order_relaxed), publicInputReject.load(std::memory_order_relaxed)};
    }
};

struct Counters final {
    std::atomic<std::uint64_t> contextUnsupported{}, contextReconstructed{}, contextPending{},
        contextChainsCaptured{}, contextFramesCaptured{}, contextChainDepthPeak{},
        contextChainOverflow{}, contextFramesReconstructed{}, actorUnresolved{}, causeTableOverflow{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(ActionClass::Count)> classified{};
    std::atomic<std::uint64_t> targetOwnerDistinct{}, externalRoots{}, delayedRoots{};
    std::atomic<std::uint64_t> seen{}, publicCandidates{}, rootsSent{}, compositeInputRootsSent{}, emittedOutputRootsSent{}, scheduledInputRootsSent{}, schedulerEdgeRootsSent{},
        typedMessageRootsSent{}, typedMessageRootsApplied{}, typedPlayerPayloadLocalOnly{},
        canonicalCompositeRootsSent{}, canonicalCompositeRootsApplied{},
        doorLockStateRootsSent{}, doorLockStateRootsApplied{},
        schedulerEdgeSidecarObserved{}, schedulerEdgeSidecarSubmitted{},
        schedulerEdgeRemoteNaturalRemembered{}, schedulerEdgeRemoteDedupConsumed{},
        scheduledEventRootsSuppressed{}, descendantsSuppressed{},
        remoteSuppressed{}, identityRejected{}, contextRejected{}, queueRejected{},
        playerCausalRejected{}, playerCausalNodesScanned{}, playerCausalNodesMapped{},
        targetOwnerRejected{}, targetOwnerMatched{}, probeTargetOwnerRejected{},
        causalUnknownRejected{}, causalShapeRejected{}, causalUnmappedNodes{},
        causalCycleRejected{}, causalDepthRejected{},
        lineageOverflow{}, applied{}, applyRejected{}, nativeFaults{},
        ws2Bypassed{}, callbackRoots{}, callbackGameplayRejected{},
        callbackBoundaryCandidates{}, callbackBoundaryRoots{},
        callbackBoundaryPlayerRoots{}, callbackBoundaryAiRoots{},
        callbackBoundaryUiRoots{}, callbackBoundaryAimpRoots{}, callbackBoundaryApplied{},
        callbackWorldContinuationRejected{}, callbackBoundaryUnknownRejected{},
        causalProvenanceSeeded{}, causalProvenanceInherited{}, causalProvenanceBoundary{},
        causalProvenanceOverflow{}, causalProvenancePlayer{}, causalProvenanceAi{},
        causalProvenanceUi{}, causalProvenanceAimp{},
        causalChainsCreated{}, causalChainsReleased{}, causalGroupsLivePeak{},
        causalRootReserved{}, causalRootCommitted{}, causalRootReservationCanceled{},
        causalRootAlreadyClaimed{}, causalRootMissing{}, causalRootNonWorldLocal{},
        causalRootGroupOverflow{}, causalRootSuppressedSiblings{}, causalRootProbeStandalone{},
        causalBoundaryProfileMissing{}, causalBoundaryNoWorldEffect{},
        causalBoundaryPlayerDependent{}, causalBoundaryWorldCandidate{},
        causalBoundaryTruncated{}, causalBoundaryUnknownDependency{},
        parentContextBound{}, parentContextBoundLocalRoot{}, parentContextBoundRemoteRoot{},
        parentContextInheritedOrigin{}, parentContextInheritedCausal{}, parentContextInheritedCause{},
        parentContextReleased{}, parentContextStale{}, parentContextOverflow{}, parentContextsLivePeak{},
        lineageGuardSuppressed{}, probeCallbackExceptions{}, probeCausalUnknownExceptions{},
        rootRateRejected{}, authorityRetailPreserved{}, authorityLocalSuppressed{},
        authorityAwaitSubmitted{}, authorityAutonomousSuppressed{},
        causalBoundaryBranching{}, emittedOutputForwardingRejected{},
        carrierObserved{}, carrierNetworkEligible{}, carrierForwardingComplete{},
        carrierSelectorResolved{}, carrierSelectorRejected{}, carrierContextCaptured{},
        carrierManifestAdmitted{}, carrierPreBusPreserved{}, carrierSubmitAttempt{},
        carrierSubmitSuccess{}, scheduledCoveredByCarrier{},
        scheduledCoveredByCarrierSuppressed{}, scheduledStateTransitionNoCarrier{},
        scheduledStateTransitionQuarantinedNoCarrier{},
        compositeCarrierObserved{}, compositeCarrierNetworkEligible{},
        compositeCarrierTargetAdmitted{}, compositeCarrierManifestAdmitted{},
        compositeCarrierPreBusPreserved{}, compositeCarrierSubmitAttempt{},
        compositeCarrierSubmitSuccess{},
        scheduledInputCertificationRejected{}, callbackBridgeScopes{}, callbackBridgeSubmitted{},
        callbackBridgeOwnerRejected{}, callbackBridgeSameInputRejected{},
        callbackBridgeAlreadyClaimedRejected{}, callbackBridgeRateRejected{},
        callbackBridgeQueueRejected{}, admissionForbiddenInputBypassed{},
        admissionGuidPrefilterRejected{}, admissionGuidPrefilterBypassed{},
        admissionPayloadBypassed{}, admissionCallbackShapeBypassed{}, admissionTailBypassed{},
        admissionPlayerTargetBypassed{}, admissionAiTargetBypassed{},
        admissionUiTargetBypassed{}, admissionAimpTargetBypassed{},
        admissionWorldContinuationBypassed{}, admissionUnknownCausalityBypassed{},
        admissionManifestOwnerInputBypassed{}, admissionManifestTargetBypassed{},
        admissionDescendantBypassed{},
        relevanceCandidates{}, relevanceReplicateIntrinsic{}, relevanceReplicateCarrier{},
        relevanceDefer{}, relevanceNotRelevant{}, relevanceUnknown{},
        relevanceReasonRouting{}, relevanceReasonSchedulerMaintenance{},
        relevanceReasonInternalState{}, relevanceReasonNoSink{},
        relevanceReasonBetterDescendant{}, relevanceReasonCycleAmbiguous{},
        relevanceReasonHistorySensitive{}, relevanceReasonStateTransitionCarrier{},
        relevanceReasonCompositeStateTransitionCarrier{},
        relevanceReasonCompositeRelevantFrontierRelaxed{}, relevanceInboundRejected{},
        installFailures{},
        rootApplyTraceWrites{}, rootApplyJournalWrites{}, rootApplyTraceStarted{}, rootApplyTraceCompleted{},
        rootApplyTraceLastStartedObservation{}, rootApplyTraceLastStartedSequence{},
        rootApplyTraceLastCompletedObservation{}, rootApplyTraceLastCompletedSequence{},
        rootApplyTraceCurrentStage{};
    std::atomic<std::uint64_t> argRejected{}, payloadRejected{}, callbackRejected{}, delayRejected{}, tailRejected{};
    std::atomic<std::uint64_t> lineageBuildInherit{}, lineageExecuteRestore{}, lineageCallbackRestore{};
    std::atomic<std::uint64_t> probeDirectSeen{}, probeDirectArgRejected{}, probeDirectPayload{},
        probeDirectCallback{}, probeDirectDelay{}, probeDirectTail{}, probeDirectEligible{};
    std::atomic<std::uint64_t> identityRuntimeKeyRejected{}, identityRegistryLookupRejected{},
        identityPublicInputRejected{}, identityDefinitionGetterRejected{}, identityDefinitionNullRejected{},
        identityDefinitionDispatchRejected{}, identityDefinitionFaultRejected{}, identityDefinitionSelfRejected{};
    std::atomic<std::uint64_t> probeIdentityRuntimeKeyRejected{}, probeIdentityRegistryLookupRejected{},
        probeIdentityPublicInputRejected{}, probeIdentityDefinitionGetterRejected{}, probeIdentityDefinitionNullRejected{},
        probeIdentityDefinitionDispatchRejected{}, probeIdentityDefinitionFaultRejected{}, probeIdentityDefinitionSelfRejected{};
    IdentityCandidateCounters identityCurrent{}, identitySelf{}, probeIdentityCurrent{}, probeIdentitySelf{};
    std::atomic<std::uint64_t> identityDirectOwnerValid{}, identityInterfaceMapValid{}, identityInterfaceMapRejected{},
        probeIdentityDirectOwnerValid{}, probeIdentityInterfaceMapValid{}, probeIdentityInterfaceMapRejected{};
    std::atomic<std::uint64_t> identityBoundaryLayoutRejected{}, probeIdentityBoundaryLayoutRejected{},
        identityBoundaryOwnerValid{}, probeIdentityBoundaryOwnerValid{}, probeDirectRootsSent{};
    IdentityCandidateCounters identityOwner{}, probeIdentityOwner{};
} gStats;

struct RelevanceProducerSlot final {
    std::uint64_t count{};
    WorldEntityId owner{};
    std::uint32_t shortGuid{};
    std::uint32_t primitiveType{};
    std::uint32_t replicationRelevance{};
    std::uint8_t rootKind{};
    bool occupied{};
};

constexpr std::size_t kRelevanceProducerCapacity = 64U;
std::mutex gRelevanceProducerMutex;
std::array<RelevanceProducerSlot, kRelevanceProducerCapacity> gRelevanceProducers{};

void RecordRelevanceCandidate(
    const WorldEntityRegistry& registry, const WorldEntityId& owner,
    const std::uint32_t shortGuid, const world_interactions::PublicRootKind rootKind,
    const WorldCompositePublicInputCausalProfile& profile) noexcept {
    gStats.relevanceCandidates.fetch_add(1U, std::memory_order_relaxed);
    switch (profile.RelevanceDisposition()) {
    case WorldReplicationRelevanceDisposition::ReplicateIntrinsic:
        gStats.relevanceReplicateIntrinsic.fetch_add(1U, std::memory_order_relaxed); break;
    case WorldReplicationRelevanceDisposition::ReplicateCarrier:
        gStats.relevanceReplicateCarrier.fetch_add(1U, std::memory_order_relaxed); break;
    case WorldReplicationRelevanceDisposition::DeferToDescendant:
        gStats.relevanceDefer.fetch_add(1U, std::memory_order_relaxed); break;
    case WorldReplicationRelevanceDisposition::NotRelevant:
        gStats.relevanceNotRelevant.fetch_add(1U, std::memory_order_relaxed); break;
    case WorldReplicationRelevanceDisposition::Unknown:
    default:
        gStats.relevanceUnknown.fetch_add(1U, std::memory_order_relaxed); break;
    }
    const auto evidence = profile.RelevanceEvidence();
    if ((evidence & WorldReplicationEvidenceRoutingOnly) != 0U)
        gStats.relevanceReasonRouting.fetch_add(1U, std::memory_order_relaxed);
    if ((evidence & WorldReplicationEvidenceSchedulerMaintenance) != 0U)
        gStats.relevanceReasonSchedulerMaintenance.fetch_add(1U, std::memory_order_relaxed);
    if ((evidence & WorldReplicationEvidenceInternalStateOnly) != 0U)
        gStats.relevanceReasonInternalState.fetch_add(1U, std::memory_order_relaxed);
    if ((evidence & WorldReplicationEvidenceNoReplicableSink) != 0U)
        gStats.relevanceReasonNoSink.fetch_add(1U, std::memory_order_relaxed);
    if ((evidence & WorldReplicationEvidenceCompleteDescendantFrontier) != 0U)
        gStats.relevanceReasonBetterDescendant.fetch_add(1U, std::memory_order_relaxed);
    if ((evidence & WorldReplicationEvidenceCycleAmbiguous) != 0U)
        gStats.relevanceReasonCycleAmbiguous.fetch_add(1U, std::memory_order_relaxed);
    if ((evidence & WorldReplicationEvidenceReplayHistorySensitive) != 0U)
        gStats.relevanceReasonHistorySensitive.fetch_add(1U, std::memory_order_relaxed);
    if ((evidence & WorldReplicationEvidenceStateTransitionCarrier) != 0U)
        gStats.relevanceReasonStateTransitionCarrier.fetch_add(1U, std::memory_order_relaxed);
    if ((evidence & WorldReplicationEvidenceCompositeStateTransitionCarrier) != 0U) {
        gStats.relevanceReasonStateTransitionCarrier.fetch_add(1U, std::memory_order_relaxed);
        gStats.relevanceReasonCompositeStateTransitionCarrier.fetch_add(
            1U, std::memory_order_relaxed);
    }
    if ((evidence & WorldReplicationEvidenceCompositeRelevantFrontierRelaxed) != 0U)
        gStats.relevanceReasonCompositeRelevantFrontierRelaxed.fetch_add(
            1U, std::memory_order_relaxed);

    const auto primitiveType = registry.FindWorldEntityPrimitiveType(owner).value_or(0U);
    const auto root = static_cast<std::uint8_t>(rootKind);
    std::lock_guard lock(gRelevanceProducerMutex);
    RelevanceProducerSlot* empty{};
    RelevanceProducerSlot* minimum{};
    for (auto& slot : gRelevanceProducers) {
        if (!slot.occupied) { if (empty == nullptr) empty = &slot; continue; }
        if (slot.rootKind == root && slot.primitiveType == primitiveType &&
            slot.shortGuid == shortGuid &&
            slot.replicationRelevance == profile.replicationRelevance) {
            ++slot.count; slot.owner = owner; return;
        }
        if (minimum == nullptr || slot.count < minimum->count) minimum = &slot;
    }
    auto* slot = empty != nullptr ? empty : minimum;
    if (slot == nullptr) return;
    const auto inheritedError = slot->occupied ? slot->count : 0U;
    *slot = RelevanceProducerSlot{inheritedError + 1U, owner, shortGuid,
        primitiveType, profile.replicationRelevance, root, true};
}

void Classify(ActionClass value) noexcept {
    gStats.classified[static_cast<std::size_t>(value)].fetch_add(1U, std::memory_order_relaxed);
}

[[nodiscard]] bool SecurityEnabled(const AdmissionSecurityGate) noexcept {
    // WS3 0.11.4.66: admission-policy safeties are intentionally disabled.
    // Structural/runtime invariants (valid native reads, wire validation,
    // authority ordering, RemoteApply anti-echo and queue integrity) remain.
    return false;
}

[[nodiscard]] bool LocalTargetSecurityEnabled(const WorldDomain domain) noexcept {
    switch (domain) {
    case WorldDomain::Player:
        return SecurityEnabled(AdmissionSecurityGate::PlayerTargetLocalOnly);
    case WorldDomain::AI:
        return ai_sync::OwnsAI() || SecurityEnabled(AdmissionSecurityGate::AiTargetLocalOnly);
    case WorldDomain::UI:
        return SecurityEnabled(AdmissionSecurityGate::UiTargetLocalOnly);
    case WorldDomain::AIMP:
        return SecurityEnabled(AdmissionSecurityGate::AimpTargetLocalOnly);
    default:
        return true;
    }
}

std::mutex gProbeIdentityMutex;
ProbeIdentityObservation gProbeIdentityLast{};

struct ExecutionFrame final {
    ExecutionFrame* previous{};
    std::uintptr_t item{};
    Origin origin{};
    CausalStamp causalStamp{};
    std::uint32_t epoch{};
    ContextTrace cause{};
};
thread_local ExecutionFrame* gFrame{};
thread_local CausalStamp gEmissionStamp{};
thread_local std::uint32_t gEmissionEpoch{};
thread_local Origin gPublicOrigin{};
thread_local std::uint32_t gPublicEpoch{};
thread_local ContextTrace gPublicCause{};
thread_local std::uint64_t gRemotePeer{}, gRemoteSequence{};
// Patch 5.3.1: identifies the recursive QueueInput issued by the SchedulerEdge
// ROOT itself. It must never be captured as another sidecar and must not be
// mistaken for a naturally regenerated edge of a higher RemoteRoot.
thread_local bool gSchedulerEdgeRetailScope{};

// WS3 0.11.4.70: persistent remote-ROOT flight recorder. The file is a
// constantly overwritten snapshot, not an append-only gameplay log: it keeps
// only the ROOT currently crossing the dangerous native boundaries and the
// last ROOT that returned normally. If AI.exe dies inside a native call, the
// last successful WriteFile remains on disk and identifies the exact boundary.
enum class RootApplyTraceStage : std::uint32_t {
    None = 0U,
    Resolve = 1U,
    RebuildContext = 2U,
    ContextReady = 3U,
    InjectExecute = 4U,
    InvokeReturned = 5U,
    ReleaseContext = 6U,
    Completed = 7U,
    Rejected = 8U,
    DescendantExecute = 9U,
    DescendantCompleted = 10U,
    Ingress = 11U,
};

struct RootApplyTraceRecord final {
    std::uint64_t serial{};
    std::uint64_t sourcePeer{};
    std::uint64_t authoritySequence{};
    std::uint64_t observationId{};
    std::uint64_t contextInvocation{};
    WorldEntityId owner{};
    WorldEntityId target{};
    WorldEntityId actor{};
    CathodeRuntimeKey contextRuntimeKey{};
    std::uint32_t publicInputShortGuid{};
    std::uint32_t schedulerParameterShortGuid{};
    std::uint32_t primitiveType{};
    std::uint32_t replicationRelevance{};
    std::uint32_t replicationContractKind{};
    WorldEntityId semanticTarget{};
    std::uint32_t semanticOperation{};
    std::uint32_t relevanceEvidence{};
    std::uint32_t contextFlags{};
    std::uint32_t contextInputShortGuid{};
    std::uint16_t contextPayloadSize{};
    std::uint8_t contextDepth{};
    std::uint8_t rootFlags{}, rootKind{};
    std::uint64_t causalSerial{}, segmentSerial{}, authorityTransaction{};
    std::uint16_t rootOrdinal{}, rootCount{};
    std::uint8_t actorKind{};
    std::uint8_t contextKind{};
    std::uint8_t relevanceDisposition{};
    std::uint8_t relevanceSemanticClass{};
    bool intrinsicEffect{};
    bool descendantFrontier{};
    bool historySensitive{};
    std::uint64_t contextPayloadHash{};
    std::uint8_t scope{}; // 0 = ROOT apply, 1 = retained descendant WorkItem
    RootApplyTraceStage stage{RootApplyTraceStage::None};
    std::uint32_t detail{};
    std::uint64_t tickMs{};
};

std::mutex gRootApplyTraceMutex;
std::mutex gRootApplyFileMutex;
DeferredDiagnosticBuffer<RootApplyTraceRecord, 8192U> gDeferredRootJournal;
HANDLE gRootApplyTraceFile{INVALID_HANDLE_VALUE};
HANDLE gRootApplyJournalFile{INVALID_HANDLE_VALUE};
std::wstring gRootApplyTracePath;
std::wstring gRootApplyJournalPath;
RootApplyTraceRecord gRootApplyTraceCurrent{};
RootApplyTraceRecord gRootApplyTraceCompleted{};
bool gRootApplyTraceHasCurrent{};
bool gRootApplyTraceHasCompleted{};
std::atomic<std::uint64_t> gRootApplyTraceSerial{1U};

[[nodiscard]] const char* RootApplyTraceStageName(const RootApplyTraceStage stage) noexcept {
    switch (stage) {
    case RootApplyTraceStage::Resolve: return "Resolve";
    case RootApplyTraceStage::RebuildContext: return "RebuildContext";
    case RootApplyTraceStage::ContextReady: return "ContextReady";
    case RootApplyTraceStage::InjectExecute: return "InjectExecute";
    case RootApplyTraceStage::InvokeReturned: return "InvokeReturned";
    case RootApplyTraceStage::ReleaseContext: return "ReleaseContext";
    case RootApplyTraceStage::Completed: return "Completed";
    case RootApplyTraceStage::Rejected: return "Rejected";
    case RootApplyTraceStage::DescendantExecute: return "DescendantExecute";
    case RootApplyTraceStage::DescendantCompleted: return "DescendantCompleted";
    case RootApplyTraceStage::Ingress: return "Ingress";
    case RootApplyTraceStage::None:
    default: return "None";
    }
}

[[nodiscard]] std::uint64_t RootApplyPayloadHash(
    const world_interactions::CompositePublicInputEvent& event) noexcept {
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    auto hash = kOffset;
    const auto count = (std::min<std::size_t>)(event.contextPayloadSize, event.contextPayload.size());
    for (std::size_t i = 0U; i < count; ++i) {
        hash ^= static_cast<std::uint8_t>(event.contextPayload[i]);
        hash *= kPrime;
    }
    return hash;
}

[[nodiscard]] std::string RootApplyEntityText(const WorldEntityId& id) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0')
           << std::setw(16) << id.high << std::setw(16) << id.low;
    return stream.str();
}

[[nodiscard]] std::string RootApplyRuntimeKeyText(const CathodeRuntimeKey& key) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0')
           << std::setw(8) << key.entityA << ':' << std::setw(8) << key.entityB;
    return stream.str();
}

[[nodiscard]] bool EnsureRootApplyTraceOpenLocked() noexcept {
    if (gRootApplyTraceFile != INVALID_HANDLE_VALUE) return true;

    wchar_t rootBuffer[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", rootBuffer, _countof(rootBuffer));
    std::wstring root;
    if (length > 0U && length < _countof(rootBuffer)) {
        root.assign(rootBuffer, length);
    } else {
        wchar_t tempBuffer[32768]{};
        const auto tempLength = GetTempPathW(_countof(tempBuffer), tempBuffer);
        if (tempLength == 0U || tempLength >= _countof(tempBuffer)) return false;
        root.assign(tempBuffer, tempLength);
    }
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
    root += L"AIMP";
    CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\logs";
    CreateDirectoryW(root.c_str(), nullptr);
    gRootApplyTracePath = root + L"\\AIMP.WS3.RootTrace-" +
        std::to_wstring(GetCurrentProcessId()) + L".json";
    gRootApplyTraceFile = CreateFileW(
        gRootApplyTracePath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    return gRootApplyTraceFile != INVALID_HANDLE_VALUE;
}

void AppendRootApplyRecordJson(std::ostringstream& stream,
    const RootApplyTraceRecord& record) {
    stream << "{\"serial\":" << record.serial
           << ",\"rootId\":" << record.authoritySequence
           << ",\"observationId\":" << record.observationId
           << ",\"sourcePeer\":" << record.sourcePeer
           << ",\"owner\":\"" << RootApplyEntityText(record.owner) << '\"'
           << ",\"target\":\"" << RootApplyEntityText(record.target) << '\"'
           << ",\"actor\":\"" << RootApplyEntityText(record.actor) << '\"'
           << ",\"actorKind\":" << static_cast<unsigned>(record.actorKind)
           << ",\"publicInputShortGuid\":" << record.publicInputShortGuid
           << ",\"schedulerParameterShortGuid\":" << record.schedulerParameterShortGuid
           << ",\"primitiveType\":" << record.primitiveType
           << ",\"replicationRelevance\":" << record.replicationRelevance
           << ",\"replicationContractKind\":" << record.replicationContractKind
           << ",\"semanticTarget\":\"" << RootApplyEntityText(record.semanticTarget) << '\"'
           << ",\"semanticOperation\":" << record.semanticOperation
           << ",\"relevanceDisposition\":" << static_cast<unsigned>(record.relevanceDisposition)
           << ",\"relevanceSemanticClass\":" << static_cast<unsigned>(record.relevanceSemanticClass)
           << ",\"relevanceEvidence\":" << record.relevanceEvidence
           << ",\"intrinsicEffect\":" << (record.intrinsicEffect ? "true" : "false")
           << ",\"descendantFrontier\":" << (record.descendantFrontier ? "true" : "false")
           << ",\"historySensitive\":" << (record.historySensitive ? "true" : "false")
           << ",\"callbackShortGuid\":" << record.contextInputShortGuid
           << ",\"rootKind\":" << static_cast<unsigned>(record.rootKind)
           << ",\"causalSerial\":" << record.causalSerial
           << ",\"segmentSerial\":" << record.segmentSerial
           << ",\"authorityTransaction\":" << record.authorityTransaction
           << ",\"rootOrdinal\":" << record.rootOrdinal
           << ",\"rootCount\":" << record.rootCount
           << ",\"rootFlags\":" << static_cast<unsigned>(record.rootFlags)
           << ",\"contextKind\":" << static_cast<unsigned>(record.contextKind)
           << ",\"contextFlags\":" << record.contextFlags
           << ",\"contextPayloadSize\":" << record.contextPayloadSize
           << ",\"contextDepth\":" << static_cast<unsigned>(record.contextDepth)
           << ",\"contextPayloadHash\":" << record.contextPayloadHash
           << ",\"scope\":" << static_cast<unsigned>(record.scope)
           << ",\"contextRuntimeKey\":\"" << RootApplyRuntimeKeyText(record.contextRuntimeKey) << '\"'
           << ",\"contextInvocation\":" << record.contextInvocation
           << ",\"stage\":\"" << RootApplyTraceStageName(record.stage) << '\"'
           << ",\"stageCode\":" << static_cast<std::uint32_t>(record.stage)
           << ",\"detail\":" << record.detail
           << ",\"tickMs\":" << record.tickMs << '}';
}


[[nodiscard]] bool EnsureRootApplyJournalOpenLocked() noexcept {
    if (gRootApplyJournalFile != INVALID_HANDLE_VALUE) return true;

    wchar_t rootBuffer[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", rootBuffer, _countof(rootBuffer));
    std::wstring root;
    if (length > 0U && length < _countof(rootBuffer)) {
        root.assign(rootBuffer, length);
    } else {
        wchar_t tempBuffer[32768]{};
        const auto tempLength = GetTempPathW(_countof(tempBuffer), tempBuffer);
        if (tempLength == 0U || tempLength >= _countof(tempBuffer)) return false;
        root.assign(tempBuffer, tempLength);
    }
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
    root += L"AIMP";
    CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\logs";
    CreateDirectoryW(root.c_str(), nullptr);
    gRootApplyJournalPath = root + L"\\AIMP.WS3.RootJournal-" +
        std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    gRootApplyJournalFile = CreateFileW(
        gRootApplyJournalPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    return gRootApplyJournalFile != INVALID_HANDLE_VALUE;
}

void AppendRootApplyJournalLocked(const RootApplyTraceRecord& record) noexcept {
    if (!EnsureRootApplyJournalOpenLocked()) return;
    try {
        std::ostringstream stream;
        stream << "{\"version\":1,\"pid\":" << GetCurrentProcessId()
               << ",\"tid\":" << GetCurrentThreadId()
               << ",\"writeTickMs\":" << GetTickCount64()
               << ",\"record\":";
        AppendRootApplyRecordJson(stream, record);
        stream << "}\r\n";
        const auto text = stream.str();
        DWORD written = 0U;
        if (!WriteFile(gRootApplyJournalFile, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
            written != static_cast<DWORD>(text.size())) return;
        gStats.rootApplyJournalWrites.fetch_add(1U, std::memory_order_relaxed);
    } catch (...) {
        // Flight-recorder diagnostics must never perturb gameplay.
    }
}

void WriteRootApplyTraceLocked(const RootApplyTraceRecord& current, bool hasCurrent,
    const RootApplyTraceRecord& completed, bool hasCompleted) noexcept {
    if (!EnsureRootApplyTraceOpenLocked()) return;
    try {
        std::ostringstream stream;
        stream << "{\"version\":2,\"pid\":" << GetCurrentProcessId()
               << ",\"tid\":" << GetCurrentThreadId()
               << ",\"writeTickMs\":" << GetTickCount64()
               << ",\"current\":";
        if (hasCurrent) AppendRootApplyRecordJson(stream, current);
        else stream << "null";
        stream << ",\"lastCompleted\":";
        if (hasCompleted) AppendRootApplyRecordJson(stream, completed);
        else stream << "null";
        stream << "}\r\n";
        const auto text = stream.str();

        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(gRootApplyTraceFile, zero, nullptr, FILE_BEGIN)) return;
        DWORD written = 0U;
        if (!WriteFile(gRootApplyTraceFile, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
            written != static_cast<DWORD>(text.size())) return;
        static_cast<void>(SetEndOfFile(gRootApplyTraceFile));
        gStats.rootApplyTraceWrites.fetch_add(1U, std::memory_order_relaxed);
    } catch (...) {
        // Diagnostics must never perturb the native replay path.
    }
}

void UpdateRootApplyTrace(RootApplyTraceRecord& record,
    const RootApplyTraceStage stage, const std::uint32_t detail = 0U) noexcept {
    record.stage = stage;
    record.detail = detail;
    record.tickMs = static_cast<std::uint64_t>(GetTickCount64());
    std::lock_guard lock(gRootApplyTraceMutex);
    gRootApplyTraceCurrent = record;
    gRootApplyTraceHasCurrent = true;
    if (stage == RootApplyTraceStage::Completed) {
        gRootApplyTraceCompleted = record;
        gRootApplyTraceHasCompleted = true;
        gStats.rootApplyTraceCompleted.fetch_add(1U, std::memory_order_relaxed);
        gStats.rootApplyTraceLastCompletedObservation.store(record.observationId, std::memory_order_relaxed);
        gStats.rootApplyTraceLastCompletedSequence.store(record.authoritySequence, std::memory_order_relaxed);
    }
    gStats.rootApplyTraceCurrentStage.store(static_cast<std::uint32_t>(stage), std::memory_order_relaxed);
    // Keep terminal ROOT evidence in memory for the diagnostic worker. Even
    // terminal-only synchronous I/O overwhelmed the high-volume replay path.
    const auto journalBoundary =
        stage == RootApplyTraceStage::Completed ||
        stage == RootApplyTraceStage::Rejected;
    if (record.scope == 0U && journalBoundary)
        static_cast<void>(gDeferredRootJournal.Push(record));

    // The worker snapshots current/completed under this short memory lock.
    // File I/O never holds the lock used by the gameplay thread.
}

void TraceRemoteDescendant(const ContextTrace& cause,
    const RootApplyTraceStage stage, const std::uint32_t detail = 0U) noexcept {
    if (cause.cause == 0U || cause.sourcePeer == 0U || cause.authoritySequence == 0U) return;
    RootApplyTraceRecord record{};
    record.serial = gRootApplyTraceSerial.fetch_add(1U, std::memory_order_relaxed);
    record.scope = 1U;
    record.sourcePeer = cause.sourcePeer;
    record.authoritySequence = cause.authoritySequence;
    record.observationId = cause.cause;
    record.contextInvocation = cause.invocation;
    record.owner = cause.owner;
    record.target = cause.target;
    record.actor = cause.actor;
    record.contextRuntimeKey = cause.contextRuntimeKey;
    record.publicInputShortGuid = cause.input;
    record.actorKind = cause.actorKind;
    record.contextKind = cause.contextKind;
    UpdateRootApplyTrace(record, stage, detail);
}


void TraceRootIngress(
    const world_interactions::CompositePublicInputEvent& event,
    const WorldEntityId& owner) noexcept {
    RootApplyTraceRecord record{};
    record.serial = gRootApplyTraceSerial.fetch_add(1U, std::memory_order_relaxed);
    record.sourcePeer = gRemotePeer;
    record.authoritySequence = gRemoteSequence;
    record.observationId = event.observationId;
    record.owner = owner;
    record.target = event.targetEntity;
    record.actor = event.actorEntity;
    record.publicInputShortGuid = event.publicInputShortGuid;
    record.schedulerParameterShortGuid = event.schedulerParameterShortGuid;
    if (event.rootKind == world_interactions::PublicRootKind::SchedulerEdge ||
        event.rootKind == world_interactions::PublicRootKind::TypedMessage ||
        event.rootKind == world_interactions::PublicRootKind::CanonicalCompositeInput) {
        record.replicationContractKind = static_cast<std::uint32_t>(WorldReplicationContractKind::Event);
        record.semanticTarget = event.targetEntity;
        record.semanticOperation = event.publicInputShortGuid;
    } else if (event.rootKind == world_interactions::PublicRootKind::DoorLockState) {
        record.replicationContractKind = static_cast<std::uint32_t>(WorldReplicationContractKind::State);
        record.semanticTarget = event.targetEntity;
        record.semanticOperation = event.publicInputShortGuid;
    }
    record.rootFlags = event.flags;
    record.rootKind = static_cast<std::uint8_t>(event.rootKind);
    record.causalSerial = event.causalSerial;
    record.segmentSerial = event.segmentSerial;
    record.authorityTransaction = event.authorityTransaction;
    record.rootOrdinal = event.rootOrdinal;
    record.rootCount = event.rootCount;
    record.actorKind = static_cast<std::uint8_t>(event.actorKind);
    record.contextKind = static_cast<std::uint8_t>(event.contextKind);
    record.contextPayloadSize = event.contextPayloadSize;
    record.contextDepth = event.contextFrameCount;
    record.contextPayloadHash = RootApplyPayloadHash(event);
    record.stage = RootApplyTraceStage::Ingress;
    record.tickMs = static_cast<std::uint64_t>(GetTickCount64());
    if (event.contextFrameCount != 0U) {
        const auto& immediate = event.contextFrames[0];
        record.contextInvocation = immediate.invocation;
        record.contextRuntimeKey = immediate.runtimeKey;
        record.contextFlags = immediate.constructorFlags;
        record.contextInputShortGuid = immediate.inputShortGuid;
    }
    std::lock_guard lock(gRootApplyTraceMutex);
    gRootApplyTraceCurrent = record;
    gRootApplyTraceHasCurrent = true;
    gStats.rootApplyTraceCurrentStage.store(
        static_cast<std::uint32_t>(RootApplyTraceStage::Ingress), std::memory_order_relaxed);
    // The in-memory ingress is immediately visible to diagnostics; the file
    // snapshot follows on the worker cadence and can lag a sudden process crash.
    // Persist the latest ingress from the diagnostics worker.
}

class RootApplyTraceScope final {
public:
    RootApplyTraceScope(const world_interactions::CompositePublicInputEvent& event,
        const WorldEntityId& owner, const WorldEntityRegistry& registry) noexcept {
        record_.serial = gRootApplyTraceSerial.fetch_add(1U, std::memory_order_relaxed);
        record_.sourcePeer = gRemotePeer;
        record_.authoritySequence = gRemoteSequence;
        record_.observationId = event.observationId;
        record_.owner = owner;
        record_.target = event.targetEntity;
        record_.actor = event.actorEntity;
        record_.publicInputShortGuid = event.publicInputShortGuid;
        record_.schedulerParameterShortGuid = event.schedulerParameterShortGuid;
        const bool genericEventRoot =
            event.rootKind == world_interactions::PublicRootKind::SchedulerEdge ||
            event.rootKind == world_interactions::PublicRootKind::TypedMessage ||
            event.rootKind == world_interactions::PublicRootKind::CanonicalCompositeInput;
        const bool doorLockState =
            event.rootKind == world_interactions::PublicRootKind::DoorLockState;
        record_.primitiveType = registry.FindWorldEntityPrimitiveType(
            (genericEventRoot || doorLockState) ? event.targetEntity : owner).value_or(0U);
        if (genericEventRoot) {
            record_.replicationContractKind = static_cast<std::uint32_t>(WorldReplicationContractKind::Event);
            record_.semanticTarget = event.targetEntity;
            record_.semanticOperation = event.publicInputShortGuid;
        } else if (doorLockState) {
            record_.replicationContractKind = static_cast<std::uint32_t>(WorldReplicationContractKind::State);
            record_.semanticTarget = event.targetEntity;
            record_.semanticOperation = event.publicInputShortGuid;
        }
        std::optional<WorldCompositePublicInputCausalProfile> relevanceProfile;
        switch (event.rootKind) {
        case world_interactions::PublicRootKind::CompositeInput:
            relevanceProfile = registry.FindCompositePublicInputCausalProfile(owner, event.publicInputShortGuid);
            break;
        case world_interactions::PublicRootKind::EmittedOutput:
            relevanceProfile = registry.FindEmittedOutputCausalProfile(owner, event.publicInputShortGuid);
            break;
        case world_interactions::PublicRootKind::ScheduledInput:
            relevanceProfile = registry.FindScheduledInputCausalProfile(owner, event.publicInputShortGuid);
            break;
        default:
            break;
        }
        if (relevanceProfile) {
            record_.replicationRelevance = relevanceProfile->replicationRelevance;
            record_.replicationContractKind = static_cast<std::uint32_t>(relevanceProfile->replicationContractKind);
            record_.semanticTarget = relevanceProfile->semanticTargetEntity;
            record_.semanticOperation = relevanceProfile->semanticOperation;
            record_.relevanceDisposition = static_cast<std::uint8_t>(relevanceProfile->RelevanceDisposition());
            record_.relevanceSemanticClass = static_cast<std::uint8_t>(relevanceProfile->SemanticClass());
            record_.relevanceEvidence = relevanceProfile->RelevanceEvidence();
            record_.intrinsicEffect = relevanceProfile->HasRelevanceEvidence(
                WorldReplicationEvidenceIntrinsicSharedEffect);
            record_.descendantFrontier = relevanceProfile->HasRelevanceEvidence(
                WorldReplicationEvidenceCompleteDescendantFrontier);
            record_.historySensitive = relevanceProfile->HasRelevanceEvidence(
                WorldReplicationEvidenceReplayHistorySensitive);
        }
        record_.contextPayloadSize = event.contextPayloadSize;
        record_.contextDepth = event.contextFrameCount;
        if (event.contextFrameCount != 0U) {
            const auto& immediate = event.contextFrames[0];
            record_.contextInvocation = immediate.invocation;
            record_.contextRuntimeKey = immediate.runtimeKey;
            record_.contextFlags = immediate.constructorFlags;
            record_.contextInputShortGuid = immediate.inputShortGuid;
        }
        record_.rootFlags = event.flags;
        record_.rootKind = static_cast<std::uint8_t>(event.rootKind);
        record_.causalSerial = event.causalSerial;
        record_.segmentSerial = event.segmentSerial;
        record_.authorityTransaction = event.authorityTransaction;
        record_.rootOrdinal = event.rootOrdinal;
        record_.rootCount = event.rootCount;
        record_.actorKind = static_cast<std::uint8_t>(event.actorKind);
        record_.contextKind = static_cast<std::uint8_t>(event.contextKind);
        record_.contextPayloadHash = RootApplyPayloadHash(event);
        gStats.rootApplyTraceStarted.fetch_add(1U, std::memory_order_relaxed);
        gStats.rootApplyTraceLastStartedObservation.store(record_.observationId, std::memory_order_relaxed);
        gStats.rootApplyTraceLastStartedSequence.store(record_.authoritySequence, std::memory_order_relaxed);
        UpdateRootApplyTrace(record_, RootApplyTraceStage::Resolve);
    }

    ~RootApplyTraceScope() noexcept {
        if (!finished_) UpdateRootApplyTrace(record_, RootApplyTraceStage::Rejected);
    }

    void Stage(const RootApplyTraceStage stage, const std::uint32_t detail = 0U) noexcept {
        UpdateRootApplyTrace(record_, stage, detail);
    }

    void Complete() noexcept {
        if (finished_) return;
        finished_ = true;
        UpdateRootApplyTrace(record_, RootApplyTraceStage::Completed);
    }

private:
    RootApplyTraceRecord record_{};
    bool finished_{};
};
// Allocation-free metadata on the hooked scheduler path. Addresses are borrowed
// keys, cleared on constructor reuse/destruction and generation transitions.
class TaskCauseTable final {
public:
    const ContextTrace* Find(std::uintptr_t token) const noexcept {
        if (token == 0U) return nullptr;
        const auto start = Hash(token);
        for (std::size_t i = 0U; i < 32U; ++i) {
            const auto& slot = slots_[(start + i) & 32767U];
            if (slot.token == token) return &slot.value;
        }
        return nullptr;
    }
    bool Put(std::uintptr_t token, const ContextTrace& value) noexcept {
        const auto start = Hash(token);
        Slot* empty = nullptr;
        for (std::size_t i = 0U; i < 32U; ++i) {
            auto& slot = slots_[(start + i) & 32767U];
            if (slot.token == token) { slot.value = value; return true; }
            if (slot.token == 0U && empty == nullptr) empty = &slot;
        }
        if (empty == nullptr) return false; // Never evict a live invocation.
        empty->token = token;
        empty->value = value;
        return true;
    }
    void Erase(std::uintptr_t token) noexcept {
        const auto start = Hash(token);
        for (std::size_t i = 0U; i < 32U; ++i) {
            auto& slot = slots_[(start + i) & 32767U];
            if (slot.token == token) slot = {};
        }
    }
    void Clear() noexcept { for (auto& slot : slots_) slot = {}; }
private:
    struct Slot { std::uintptr_t token{}; ContextTrace value{}; };
    static std::size_t Hash(std::uintptr_t token) noexcept {
        token ^= token >> 16U;
        token *= static_cast<std::uintptr_t>(0x9E3779B1U);
        return static_cast<std::size_t>(token) & 32767U;
    }
    std::array<Slot, 32768U> slots_{};
} gTaskCauses;

// 0.11.4.74: a ROOT may be selected by one child while later/delayed siblings
// are still built from the same retained callback parent. Tagging that shared
// parent as LocalRoot/RemoteRoot is too coarse because the retail callback can
// be observed independently. Instead keep a process-local binding qualified by
// the parent's invocation id. Pointer reuse therefore cannot inherit stale
// lineage after WorkDestroy/constructor reuse.
struct ParentContextBinding final {
    std::uint64_t invocation{};
    Origin origin{};
    CausalStamp causalStamp{};
    std::uint64_t cause{};
    std::uint64_t sourcePeer{};
    std::uint64_t authoritySequence{};
};

class ParentContextTable final {
public:
    [[nodiscard]] const ParentContextBinding* Find(
        const std::uintptr_t token, const std::uint64_t invocation) const noexcept {
        if (token == 0U || invocation == 0U) return nullptr;
        const auto start = Hash(token);
        for (std::size_t i = 0U; i < 32U; ++i) {
            const auto& slot = slots_[(start + i) & 8191U];
            if (slot.token == token)
                return slot.value.invocation == invocation ? &slot.value : nullptr;
        }
        return nullptr;
    }

    [[nodiscard]] ParentContextBinding* FindAny(const std::uintptr_t token) noexcept {
        if (token == 0U) return nullptr;
        const auto start = Hash(token);
        for (std::size_t i = 0U; i < 32U; ++i) {
            auto& slot = slots_[(start + i) & 8191U];
            if (slot.token == token) return &slot.value;
        }
        return nullptr;
    }

    [[nodiscard]] bool Put(const std::uintptr_t token, ParentContextBinding value) noexcept {
        if (token == 0U || value.invocation == 0U) return false;
        const auto start = Hash(token);
        Slot* empty = nullptr;
        for (std::size_t i = 0U; i < 32U; ++i) {
            auto& slot = slots_[(start + i) & 8191U];
            if (slot.token == token) { slot.value = value; return true; }
            if (slot.token == 0U && empty == nullptr) empty = &slot;
        }
        if (empty == nullptr) return false;
        empty->token = token;
        empty->value = value;
        ++size_;
        return true;
    }

    [[nodiscard]] bool Erase(const std::uintptr_t token, ParentContextBinding& old) noexcept {
        if (token == 0U) return false;
        const auto start = Hash(token);
        for (std::size_t i = 0U; i < 32U; ++i) {
            auto& slot = slots_[(start + i) & 8191U];
            if (slot.token == token) {
                old = slot.value;
                slot = {};
                --size_;
                return true;
            }
        }
        return false;
    }

    void Clear() noexcept {
        for (auto& slot : slots_) slot = {};
        size_ = 0U;
    }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }

private:
    struct Slot final { std::uintptr_t token{}; ParentContextBinding value{}; };
    [[nodiscard]] static std::size_t Hash(std::uintptr_t token) noexcept {
        token ^= token >> 16U;
        token *= static_cast<std::uintptr_t>(0x9E3779B1U);
        return static_cast<std::size_t>(token) & 8191U;
    }
    std::array<Slot, 8192U> slots_{};
    std::size_t size_{};
} gParentContexts;

std::atomic<std::uint64_t> gInvocationSerial{1U};
std::mutex gTraceMutex;
std::array<ContextTrace, 64U> gContextTrace{};
std::uint64_t gTraceSerial{};

// Patch 5.3.1: receiver-local anti-double-effect ledger. A higher RemoteRoot
// can naturally regenerate the same QueueInput that the Host also published as
// an authoritative SchedulerEdge sidecar. Remember that local edge with the
// parent authority sequence, then consume exactly one matching later sidecar.
// This is not semantic time-window dedupe for independent ROOTs: a match is
// eligible only when it was produced by a preceding RemoteRoot and the
// authority sequence distance is bounded.
struct SchedulerEdgeNaturalMatch final {
    std::uint64_t hash{};
    std::uint64_t expiryTickMs{};
    std::uint64_t parentAuthoritySequence{};
};
constexpr std::size_t kSchedulerEdgeNaturalMatchCapacity = 256U;
constexpr std::uint64_t kSchedulerEdgeNaturalMatchLifetimeMs = 2000U;
constexpr std::uint64_t kSchedulerEdgeNaturalMatchMaximumSequenceDelta = 32U;
std::mutex gSchedulerEdgeNaturalMatchMutex;
std::array<SchedulerEdgeNaturalMatch, kSchedulerEdgeNaturalMatchCapacity> gSchedulerEdgeNaturalMatches{};

[[nodiscard]] std::uint64_t SchedulerEdgeFingerprint(
    const WorldEntityId& source, const WorldEntityId& target,
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const double relativeDelaySeconds) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](const std::uint64_t value) noexcept {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            hash ^= (value >> shift) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    mix(source.high); mix(source.low);
    mix(target.high); mix(target.low);
    mix(schedulerParameterShortGuid);
    mix(targetInputShortGuid);
    // Retail QueueInput composes a float descriptor delay with a double call
    // delay. Millisecond quantization tolerates harmless FP representation
    // differences while remaining specific enough for the short sequence gate.
    const auto delayMs = static_cast<std::uint64_t>(
        std::llround((std::max)(0.0, relativeDelaySeconds) * 1000.0));
    mix(delayMs);
    return hash == 0U ? 1U : hash;
}

// Stages: 1 capture, 2 unsupported context, 3 receive, 4 parent constructed,
// 5 work built, 6 work execute, 7 work destroyed, 8 dependency pending, 9 fault,
// 10 legacy/authority suppression, 11 late local fallback, 12 AwaitAuthority submitted,
// 13 certified autonomous Host-owned suppression.
void TraceContext(ContextTrace value, const std::uint8_t stage) noexcept {
    value.stage = stage;
    std::lock_guard lock(gTraceMutex);
    value.serial = ++gTraceSerial;
    gContextTrace[static_cast<std::size_t>((value.serial - 1U) % gContextTrace.size())] = value;
}

[[nodiscard]] ContextTrace TaskCause(void* item, const std::uint32_t epoch) noexcept {
    if (item == nullptr) return {};
    std::lock_guard lock(gLineageMutex);
    const auto* found = gTaskCauses.Find(reinterpret_cast<std::uintptr_t>(item));
    return epoch == gEpoch.load(std::memory_order_relaxed) && found != nullptr
        ? *found : ContextTrace{};
}

void StoreTaskCause(void* item, ContextTrace value, const std::uint32_t epoch) noexcept {
    if (item == nullptr) return;
    std::lock_guard lock(gLineageMutex);
    if (epoch != gEpoch.load(std::memory_order_relaxed)) return;
    const auto key = reinterpret_cast<std::uintptr_t>(item);
    value.item = key;
    value.epoch = epoch;
    if (!gTaskCauses.Put(key, value)) {
        gStats.causeTableOverflow.fetch_add(1U, std::memory_order_relaxed);
        gEnabled.store(false, std::memory_order_release);
        return;
    }

}

[[nodiscard]] std::uint64_t EnsureParentInvocation(
    void* parent, const std::uint32_t epoch) noexcept {
    if (parent == nullptr || epoch != gEpoch.load(std::memory_order_relaxed)) return 0U;
    auto trace = TaskCause(parent, epoch);
    if (trace.invocation != 0U) return trace.invocation;
    trace.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
    if (trace.invocation == 0U)
        trace.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
    if (trace.invocation == 0U) return 0U;
    trace.epoch = epoch;
    StoreTaskCause(parent, trace, epoch);
    return trace.invocation;
}

[[nodiscard]] ParentContextBinding ParentContext(
    void* parent, const std::uint32_t epoch) noexcept {
    if (parent == nullptr || gLiveParentContexts.load(std::memory_order_acquire) == 0U)
        return {};
    const auto trace = TaskCause(parent, epoch);
    if (trace.invocation == 0U) return {};
    std::lock_guard lock(gLineageMutex);
    if (epoch != gEpoch.load(std::memory_order_relaxed)) return {};
    const auto token = reinterpret_cast<std::uintptr_t>(parent);
    const auto* binding = gParentContexts.Find(token, trace.invocation);
    if (binding != nullptr) return *binding;
    if (gParentContexts.FindAny(token) != nullptr)
        gStats.parentContextStale.fetch_add(1U, std::memory_order_relaxed);
    return {};
}

void RefreshParentContextLiveCounters() noexcept {
    const auto live = gParentContexts.Size();
    gLiveParentContexts.store(live, std::memory_order_release);
    const auto peak = gStats.parentContextsLivePeak.load(std::memory_order_relaxed);
    if (live > peak)
        gStats.parentContextsLivePeak.store(live, std::memory_order_relaxed);
    gLiveCausalGroups.store(gCausalGroups.Size(), std::memory_order_release);
}

void BindParentContext(void* parent, const Origin origin, const CausalStamp stamp,
                       const ContextTrace& cause, const std::uint32_t epoch) noexcept {
    if (parent == nullptr || epoch != gEpoch.load(std::memory_order_relaxed)) return;
    const auto invocation = EnsureParentInvocation(parent, epoch);
    if (invocation == 0U) return;

    std::lock_guard lock(gLineageMutex);
    if (epoch != gEpoch.load(std::memory_order_relaxed)) return;
    const auto token = reinterpret_cast<std::uintptr_t>(parent);
    auto* existing = gParentContexts.FindAny(token);
    if (existing != nullptr && existing->invocation != invocation) {
        if (existing->causalStamp.Valid() &&
            gCausalGroups.ReleaseRef(existing->causalStamp.chainId))
            gStats.causalChainsReleased.fetch_add(1U, std::memory_order_relaxed);
        ParentContextBinding ignored{};
        static_cast<void>(gParentContexts.Erase(token, ignored));
        gStats.parentContextStale.fetch_add(1U, std::memory_order_relaxed);
        existing = nullptr;
    }

    ParentContextBinding value{};
    if (existing != nullptr) value = *existing;
    else value.invocation = invocation;

    // RemoteRoot is strongest because it is the anti-echo boundary. LocalRoot
    // is stronger than None. Never downgrade an existing root origin.
    if (origin == Origin::RemoteRoot ||
        (origin == Origin::LocalRoot && value.origin == Origin::None))
        value.origin = origin;
    if (stamp.Valid() && value.causalStamp.Valid() && !value.causalStamp.SameChain(stamp)) {
        // One live parent invocation must never bridge two independent local
        // causal groups. Keep the first binding and expose the ambiguity.
        gStats.parentContextStale.fetch_add(1U, std::memory_order_relaxed);
        RefreshParentContextLiveCounters();
        return;
    }
    if (stamp.Valid() && !value.causalStamp.Valid()) {
        if (!gCausalGroups.AddRef(stamp.chainId)) {
            gStats.parentContextOverflow.fetch_add(1U, std::memory_order_relaxed);
            RefreshParentContextLiveCounters();
            return;
        }
        value.causalStamp = stamp;
    }
    if (cause.cause != 0U) value.cause = cause.cause;
    if (cause.sourcePeer != 0U) value.sourcePeer = cause.sourcePeer;
    if (cause.authoritySequence != 0U) value.authoritySequence = cause.authoritySequence;

    if (!gParentContexts.Put(token, value)) {
        if (existing == nullptr && value.causalStamp.Valid() &&
            gCausalGroups.ReleaseRef(value.causalStamp.chainId))
            gStats.causalChainsReleased.fetch_add(1U, std::memory_order_relaxed);
        gStats.parentContextOverflow.fetch_add(1U, std::memory_order_relaxed);
        RefreshParentContextLiveCounters();
        return;
    }
    gStats.parentContextBound.fetch_add(1U, std::memory_order_relaxed);
    if (origin == Origin::LocalRoot)
        gStats.parentContextBoundLocalRoot.fetch_add(1U, std::memory_order_relaxed);
    else if (origin == Origin::RemoteRoot)
        gStats.parentContextBoundRemoteRoot.fetch_add(1U, std::memory_order_relaxed);
    RefreshParentContextLiveCounters();
}

void ReleaseParentContextLocked(const std::uintptr_t token) noexcept {
    ParentContextBinding binding{};
    if (!gParentContexts.Erase(token, binding)) return;
    if (binding.causalStamp.Valid() &&
        gCausalGroups.ReleaseRef(binding.causalStamp.chainId))
        gStats.causalChainsReleased.fetch_add(1U, std::memory_order_relaxed);
    gStats.parentContextReleased.fetch_add(1U, std::memory_order_relaxed);
    RefreshParentContextLiveCounters();
}

void MergeParentContext(const ParentContextBinding& binding, Origin& origin,
                        CausalStamp& stamp, ContextTrace& cause) noexcept {
    if (origin == Origin::None && binding.origin != Origin::None) {
        origin = binding.origin;
        gStats.parentContextInheritedOrigin.fetch_add(1U, std::memory_order_relaxed);
    }
    if (!stamp.Valid() && binding.causalStamp.Valid()) {
        stamp = binding.causalStamp;
        gStats.parentContextInheritedCausal.fetch_add(1U, std::memory_order_relaxed);
    }
    bool inheritedCause = false;
    if (cause.cause == 0U && binding.cause != 0U) {
        cause.cause = binding.cause;
        inheritedCause = true;
    }
    if (cause.sourcePeer == 0U && binding.sourcePeer != 0U) {
        cause.sourcePeer = binding.sourcePeer;
        inheritedCause = true;
    }
    if (cause.authoritySequence == 0U && binding.authoritySequence != 0U) {
        cause.authoritySequence = binding.authoritySequence;
        inheritedCause = true;
    }
    if (cause.causalChainId == 0U && binding.causalStamp.Valid()) {
        cause.causalChainId = binding.causalStamp.chainId;
        cause.causalSource = static_cast<std::uint8_t>(binding.causalStamp.source);
    }
    if (inheritedCause)
        gStats.parentContextInheritedCause.fetch_add(1U, std::memory_order_relaxed);
}


struct CallbackBridgeFrame final {
    CallbackBridgeFrame* previous{};
    WorldEntityId owner{};
    std::uint32_t outerInput{};
    std::uint32_t epoch{};
    bool claimed{};
};
thread_local CallbackBridgeFrame* gCallbackBridgeFrame{};

class CallbackBridgeScope final {
public:
    CallbackBridgeScope(const WorldEntityId& owner, const std::uint32_t outerInput,
        const std::uint32_t epoch) noexcept {
        frame_.previous = gCallbackBridgeFrame;
        frame_.owner = owner;
        frame_.outerInput = outerInput;
        frame_.epoch = epoch;
        gCallbackBridgeFrame = &frame_;
        gStats.callbackBridgeScopes.fetch_add(1U, std::memory_order_relaxed);
    }
    ~CallbackBridgeScope() {
        // A nested callback may be the first place where the stable pre-Queue
        // cause becomes visible. Once any nested scope claims it, propagate the
        // claim to its active ancestors so the same synchronous interaction can
        // never publish a second independent bridge ROOT on unwind.
        if (frame_.claimed && frame_.previous != nullptr &&
            frame_.previous->epoch == frame_.epoch) {
            frame_.previous->claimed = true;
        }
        gCallbackBridgeFrame = frame_.previous;
    }
    CallbackBridgeScope(const CallbackBridgeScope&) = delete;
    CallbackBridgeScope& operator=(const CallbackBridgeScope&) = delete;
private:
    CallbackBridgeFrame frame_{};
};

[[nodiscard]] bool TryConsumeRootRateBudget() noexcept {
    // WS3 0.11.4.66: the admission-rate fuse is intentionally disabled.
    // Queue bounds and authority ordering still provide structural backpressure.
    return true;
}

[[nodiscard]] Origin CurrentOrigin() noexcept {
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    if (gPublicEpoch == epoch && gPublicOrigin != Origin::None) return gPublicOrigin;
    // Historical pre-Queue bridge support is compile-time dormant in v0.11.4.59.
    // Keep its postmortem code without paying a TLS check on the public hot path.
    if constexpr (kCallbackBridgeEnabled) {
        if (gCallbackBridgeFrame != nullptr && gCallbackBridgeFrame->epoch == epoch &&
            gCallbackBridgeFrame->claimed) return Origin::LocalRoot;
    }
    return gFrame != nullptr && gFrame->epoch == epoch ? gFrame->origin : Origin::None;
}

[[nodiscard]] std::uint64_t CurrentAuthoritySequence() noexcept {
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    if (gPublicEpoch == epoch && gPublicCause.authoritySequence != 0U)
        return gPublicCause.authoritySequence;
    if (gFrame != nullptr && gFrame->epoch == epoch && gFrame->cause.authoritySequence != 0U)
        return gFrame->cause.authoritySequence;
    return replication_core::RemoteApplyActive() ? gRemoteSequence : 0U;
}

[[nodiscard]] std::uint8_t CurrentRootKind() noexcept {
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    if (gPublicEpoch == epoch && gPublicCause.rootKind != 0U) return gPublicCause.rootKind;
    if (gFrame != nullptr && gFrame->epoch == epoch) return gFrame->cause.rootKind;
    return 0U;
}

// Process-local lineage marker; never a WorldSync wire root kind. It survives
// delayed WorkItems through the existing retained callback/cause tables.
constexpr std::uint8_t kMissionRootKind = 0xFEU;

// Borrowed retail references only. The constructor retains the work-item in
// +0C until 00922800 releases it; this is not the target/shared-ref at +08.
// Keep protected native reads in POD helpers (MSVC C2712).
[[nodiscard]] bool ScheduledConsumerDebugLatchClear(void* manager) noexcept {
    if (manager == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        // Retail FUN_0092CE20 only raises manager+0x10A around a special
        // scheduler-drain assertion path. FUN_00926680 merely clears it and
        // breaks when a debugger is attached. It is not portable gameplay
        // context, so ScheduledInput ROOTs are certified only when it is clear.
        return *reinterpret_cast<const std::uint8_t*>(
            static_cast<const std::byte*>(manager) + 0x10AU) == 0U;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

[[nodiscard]] void* ReadWorkHolder(void** holder) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        return holder == nullptr ? nullptr : *holder;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
#endif
}

[[nodiscard]] void* ReadRetainedParent(void* item) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        return item == nullptr ? nullptr :
            *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(item) + 0x0CU);
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
#endif
}

[[nodiscard]] void* ExecutionFromShared(void* shared) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        // Existing CATHODE shared-ref code in this project uses +0x0C for the
        // borrowed execution object. Never retain or serialize the raw pointer.
        return shared == nullptr ? nullptr :
            *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(shared) + 0x0CU);
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
#endif
}

// Entity ID (+0C) and composite type (+10) of the borrowed execution object,
// the same fields alien-isolation-coop reads from call_method's target.
bool ReadExecutionIdentity(void* execution, std::uint32_t& entity, std::uint32_t& type) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        if (execution == nullptr) return false;
        const auto at = reinterpret_cast<std::uintptr_t>(execution);
        entity = *reinterpret_cast<const std::uint32_t*>(at + 0x0CU);
        type = *reinterpret_cast<const std::uint32_t*>(at + 0x10U);
        return true;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

// Read one retained WorkItem frame using the retail layout established from the
// supplied AI.exe. Every frame owns source at +08, optional parent at +0C,
// MemoryAllocation<char> at +10 and flags at +20. Unlike ROOT v8, v9 does not
// require the retained parent itself to be null and does not assume input="callback".
bool ReadContextFrame(void* item, ContextTrace& trace, void*& execution,
    void*& retainedParent, std::uint32_t& inputShortGuid, std::uint32_t& constructorFlags,
    std::uint16_t& payloadBytes, const std::byte*& payloadData,
    std::uint32_t* descriptorDelayBits = nullptr) noexcept {
    trace.parent = reinterpret_cast<std::uintptr_t>(item);
    trace.parentInput = trace.parentFlags = 0U;
    trace.parentPayloadBytes = 0U;
    trace.parentPayload = trace.parentReadable = false;
    execution = nullptr;
    retainedParent = nullptr;
    inputShortGuid = constructorFlags = 0U;
    payloadBytes = 0U;
    payloadData = nullptr;
    if (item == nullptr) { trace.parentReadable = true; return true; }
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        const auto* words = static_cast<const std::uint32_t*>(item);
        inputShortGuid = words[0];
        constructorFlags = words[8];
        if (descriptorDelayBits != nullptr) *descriptorDelayBits = words[1];
        retainedParent = reinterpret_cast<void*>(words[3]);
        trace.parentInput = inputShortGuid;
        trace.parentFlags = constructorFlags;
        trace.parentReadable = true;
        execution = ExecutionFromShared(reinterpret_cast<void*>(words[2]));
        if (inputShortGuid == 0U || execution == nullptr) return false;

        std::uint32_t size = 0U;
        const auto allocation = reinterpret_cast<const std::byte*>(words[4]);
        const std::byte* data = nullptr;
        if (allocation != nullptr) {
            size = *reinterpret_cast<const std::uint32_t*>(allocation + 8U);
            data = *reinterpret_cast<const std::byte* const*>(allocation + 0x0CU);
            if (size > world_interactions::kCompositePublicRootMaximumContextPayloadBytes ||
                (size != 0U && data == nullptr)) return false;
        }
        payloadBytes = static_cast<std::uint16_t>(size);
        payloadData = data;
        trace.parentPayloadBytes = payloadBytes;
        trace.parentPayload = payloadBytes != 0U;
        return true;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}


// Compatibility helper for local lineage inspection. v9 no longer constrains
// the retained WorkItem to callback/no-parent; callers that only need the
// execution object still use this compact shape reader.
bool ReadContextShape(void* item, ContextTrace& trace, void*& execution) noexcept {
    void* retainedParent{};
    std::uint32_t inputShortGuid{};
    std::uint32_t constructorFlags{};
    std::uint16_t payloadBytes{};
    const std::byte* payloadData{};
    return ReadContextFrame(item, trace, execution, retainedParent, inputShortGuid,
        constructorFlags, payloadBytes, payloadData);
}

void IdentifyActor(WorldEntityRegistry& registry, void** source, ContextTrace& trace) noexcept {
    using world_interactions::PublicActorKind;
    void* execution = ExecutionFromShared(ReadWorkHolder(source));
    if (execution == nullptr) return;
    const auto domain = registry.IdentifyRuntimeEntityDomain(execution);
    if (domain == WorldDomain::Player) {
        trace.actorKind = static_cast<std::uint8_t>(PublicActorKind::SourcePlayer);
        trace.actor = {};
    } else if (domain == WorldDomain::AI || domain == WorldDomain::World) {
        const auto id = registry.IdentifyRuntimeEntity(execution);
        if (id && id->Valid()) {
            trace.actorKind = static_cast<std::uint8_t>(PublicActorKind::WorldEntity);
            trace.actor = *id;
        }
    }
}

bool CaptureContext(WorldEntityRegistry& registry, void* parent,
    world_interactions::CompositePublicInputEvent& event, ContextTrace& trace) noexcept {
    using namespace world_interactions;
    event.contextFrameCount = 0U;
    event.contextPayloadSize = 0U;
    if (parent == nullptr) {
        event.contextKind = PublicContextKind::Autonomous;
        return true;
    }

    std::array<std::uintptr_t, kCompositePublicRootMaximumContextDepth> seen{};
    void* cursor = parent;
    while (cursor != nullptr) {
        if (event.contextFrameCount >= kCompositePublicRootMaximumContextDepth) {
            gStats.contextChainOverflow.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        const auto token = reinterpret_cast<std::uintptr_t>(cursor);
        for (std::size_t i = 0; i < event.contextFrameCount; ++i)
            if (seen[i] == token) return false;
        seen[event.contextFrameCount] = token;

        void* execution{};
        void* nextParent{};
        std::uint32_t inputShortGuid{};
        std::uint32_t constructorFlags{};
        std::uint16_t payloadBytes{};
        const std::byte* payloadData{};
        std::uint32_t descriptorDelayBits{};
        if (!ReadContextFrame(cursor, trace, execution, nextParent, inputShortGuid,
                constructorFlags, payloadBytes, payloadData, &descriptorDelayBits)) return false;

        // Retained allocation bytes are not a portable schema. Preserve the
        // canonical empty callback and reject opaque payloads rather than copy
        // potential process-local references into another process.
        if (payloadBytes != 0U) return false;
        float descriptorDelay{};
        std::memcpy(&descriptorDelay, &descriptorDelayBits, sizeof(descriptorDelay));
        if (!std::isfinite(descriptorDelay) || descriptorDelay < 0.0F) return false;
        CathodeRuntimeKey key{};
        if (!registry.TryExtractRuntimeKey(execution, key) || key.entityA == 0U) {
            gStats.actorUnresolved.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        // A sender's local player RuntimeKey cannot be rebound safely until a
        // peer -> native remote-player shared-ref exists. Never substitute the
        // receiver's local player singleton.
        if (registry.IdentifyRuntimeEntityDomain(execution) == WorldDomain::Player) {
            gStats.actorUnresolved.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        const auto nextPayloadSize = static_cast<std::size_t>(event.contextPayloadSize) + payloadBytes;
        const auto nextFrameCount = static_cast<std::size_t>(event.contextFrameCount) + 1U;
        const auto wireBytes = kCompositePublicRootHeaderBytes +
            nextFrameCount * kCompositePublicRootContextFrameBytes + nextPayloadSize;
        if (nextPayloadSize > event.contextPayload.size() || wireBytes > kWorldPayloadMaximumBytes) {
            gStats.contextChainOverflow.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        auto& frame = event.contextFrames[event.contextFrameCount];
        frame.runtimeKey = key;
        frame.constructorFlags = constructorFlags;
        frame.inputShortGuid = inputShortGuid;
        frame.descriptorDelayBits = descriptorDelayBits;
        frame.payloadOffset = event.contextPayloadSize;
        frame.payloadSize = payloadBytes;
        if (payloadBytes != 0U)
            std::memcpy(event.contextPayload.data() + event.contextPayloadSize, payloadData, payloadBytes);
        event.contextPayloadSize = static_cast<std::uint16_t>(nextPayloadSize);

        auto parentTrace = TaskCause(cursor, gEpoch.load(std::memory_order_relaxed));
        if (parentTrace.invocation == 0U) {
            parentTrace.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
            StoreTaskCause(cursor, parentTrace, gEpoch.load(std::memory_order_relaxed));
        }
        frame.invocation = parentTrace.invocation;
        if (event.contextFrameCount == 0U) {
            trace.parentInvocation = parentTrace.invocation;
            trace.contextRuntimeKey = key;
        }
        ++event.contextFrameCount;
        cursor = nextParent;
    }

    event.contextKind = PublicContextKind::NativeContextChain;
    if (event.contextFrameCount != 0U) {
        gStats.contextChainsCaptured.fetch_add(1U, std::memory_order_relaxed);
        gStats.contextFramesCaptured.fetch_add(event.contextFrameCount, std::memory_order_relaxed);
        const auto peak = gStats.contextChainDepthPeak.load(std::memory_order_relaxed);
        if (event.contextFrameCount > peak)
            gStats.contextChainDepthPeak.store(event.contextFrameCount, std::memory_order_relaxed);
        return true;
    }
    return false;
}

[[nodiscard]] bool IsLocalCausalSource(const CausalSource source) noexcept {
    return source == CausalSource::Player || source == CausalSource::AI ||
        source == CausalSource::UI || source == CausalSource::AIMP || source == CausalSource::World;
}

[[nodiscard]] CausalSource CausalSourceFromDomain(const WorldDomain domain) noexcept {
    switch (domain) {
    case WorldDomain::Player: return CausalSource::Player;
    case WorldDomain::AI: return CausalSource::AI;
    case WorldDomain::UI: return CausalSource::UI;
    case WorldDomain::AIMP: return CausalSource::AIMP;
    case WorldDomain::World: return CausalSource::World;
    default: return CausalSource::None;
    }
}

[[nodiscard]] CausalSource IdentifySourceCausality(
    WorldEntityRegistry& registry, void** sourceHolder) noexcept {
    void* shared = ReadWorkHolder(sourceHolder);
    void* execution = ExecutionFromShared(shared);
    if (shared == nullptr || execution == nullptr) return CausalSource::None;
    const auto domain = registry.IdentifyRuntimeEntityDomain(execution);
    return domain.has_value() ? CausalSourceFromDomain(*domain) : CausalSource::None;
}

[[nodiscard]] CausalStamp FindCausalStamp(void* item, std::uint32_t epoch) noexcept;
[[nodiscard]] CausalStamp CurrentCausalStamp(std::uint32_t epoch) noexcept;

// STATIC CONFIRMED in AI.exe 00D6...364: FUN_00926680 passes
// `&workItem->parent` (workItem+0x0C) as the callback holder to 008BD920 and
// ultimately FUN_00965920. Therefore the HOLDER address itself can identify
// the currently executing work-item even when the TLS WorkExecute scope is no
// longer visible. Never serialize this pointer; it is local causal metadata.
[[nodiscard]] void* WorkItemFromCallbackHolder(void** callbackHolder) noexcept {
    if (callbackHolder == nullptr) return nullptr;
    const auto address = reinterpret_cast<std::uintptr_t>(callbackHolder);
    if (address < 0x0CU) return nullptr;
    return reinterpret_cast<void*>(address - 0x0CU);
}

[[nodiscard]] Origin FindExactOrigin(void* item, std::uint32_t epoch) noexcept {
    if (item == nullptr || gLiveTags.load(std::memory_order_acquire) == 0U)
        return Origin::None;
    std::lock_guard lock(gLineageMutex);
    if (epoch != gEpoch.load(std::memory_order_relaxed)) return Origin::None;
    return gLineage.Find(reinterpret_cast<std::uintptr_t>(item));
}

[[nodiscard]] Origin FindOrigin(void* item, std::uint32_t epoch) noexcept {
    if (item == nullptr || gLiveTags.load(std::memory_order_acquire) == 0U) return Origin::None;
    std::lock_guard lock(gLineageMutex);
    if (epoch != gEpoch.load(std::memory_order_relaxed)) return Origin::None;
    // Resolve at USE time as well as construction time: a retained parent can
    // acquire its accepted ROOT origin after this child was constructed.
    // Only an existing tag is evidence. Never tag an upstream parent, cache a
    // negative result, or infer ownership from a shared untagged callback.
    // Brent's cycle guard needs no allocation, extra native reads or time limit.
    auto* checkpoint = item;
    std::size_t distance{}, period{1U};
    std::size_t depth{};
    while (item != nullptr) {
        if (++depth > 64U) {
            // A single pathological retained chain must not disable WorldSync
            // for the entire level. Fail closed for THIS lineage only. Actual
            // lineage-table capacity loss still trips the global circuit breaker.
            gStats.causalDepthRejected.fetch_add(1U, std::memory_order_relaxed);
            gStats.lineageGuardSuppressed.fetch_add(1U, std::memory_order_relaxed);
            return Origin::Suppressed;
        }
        const auto origin = gLineage.Find(reinterpret_cast<std::uintptr_t>(item));
        if (origin != Origin::None) return origin;
        item = ReadRetainedParent(item);
        if (item == checkpoint) {
            gStats.causalCycleRejected.fetch_add(1U, std::memory_order_relaxed);
            gStats.lineageGuardSuppressed.fetch_add(1U, std::memory_order_relaxed);
            return Origin::Suppressed;
        }
        if (++distance == period) {
            checkpoint = item;
            distance = 0U;
            period *= 2U;
        }
    }
    return Origin::None;
}

[[nodiscard]] CausalStamp FindCausalStamp(void* item, const std::uint32_t epoch) noexcept {
    if (item == nullptr || gLiveCausalTags.load(std::memory_order_acquire) == 0U)
        return {};
    std::lock_guard lock(gLineageMutex);
    if (epoch != gEpoch.load(std::memory_order_relaxed)) return {};
    auto* checkpoint = item;
    std::size_t distance{}, period{1U};
    std::size_t depth{};
    while (item != nullptr) {
        if (++depth > 64U) {
            gStats.causalDepthRejected.fetch_add(1U, std::memory_order_relaxed);
            return {};
        }
        const auto stamp = gCausalProvenance.Find(reinterpret_cast<std::uintptr_t>(item));
        if (stamp.Valid()) return stamp;
        item = ReadRetainedParent(item);
        if (item == checkpoint) {
            gStats.causalCycleRejected.fetch_add(1U, std::memory_order_relaxed);
            return {};
        }
        if (++distance == period) {
            checkpoint = item;
            distance = 0U;
            period *= 2U;
        }
    }
    return {};
}

[[nodiscard]] CausalStamp CurrentCausalStamp(const std::uint32_t epoch) noexcept {
    if (gEmissionEpoch == epoch && gEmissionStamp.Valid()) return gEmissionStamp;
    if (gFrame == nullptr || gFrame->epoch != epoch) return {};
    return gFrame->causalStamp;
}

void RecordCausalSourceDomain(const CausalSource source) noexcept {
    switch (source) {
    case CausalSource::Player:
        gStats.causalProvenancePlayer.fetch_add(1U, std::memory_order_relaxed);
        break;
    case CausalSource::AI:
        gStats.causalProvenanceAi.fetch_add(1U, std::memory_order_relaxed);
        break;
    case CausalSource::UI:
        gStats.causalProvenanceUi.fetch_add(1U, std::memory_order_relaxed);
        break;
    case CausalSource::AIMP:
        gStats.causalProvenanceAimp.fetch_add(1U, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

[[nodiscard]] CausalStamp NewCausalStamp(const CausalSource source,
                                         const std::uint32_t epoch) noexcept {
    if (!IsLocalCausalSource(source) || epoch != gEpoch.load(std::memory_order_relaxed))
        return {};
    auto chainId = gCausalChainSerial.fetch_add(1U, std::memory_order_relaxed);
    if (chainId == 0U)
        chainId = gCausalChainSerial.fetch_add(1U, std::memory_order_relaxed);
    if (chainId == 0U) return {};
    std::lock_guard lock(gLineageMutex);
    if (epoch != gEpoch.load(std::memory_order_relaxed) ||
        !gCausalGroups.Create(chainId, source)) {
        gStats.causalRootGroupOverflow.fetch_add(1U, std::memory_order_relaxed);
        return {};
    }
    gStats.causalChainsCreated.fetch_add(1U, std::memory_order_relaxed);
    const auto live = gCausalGroups.Size();
    gLiveCausalGroups.store(live, std::memory_order_release);
    if (live > gStats.causalGroupsLivePeak.load(std::memory_order_relaxed))
        gStats.causalGroupsLivePeak.store(live, std::memory_order_relaxed);
    return {source, chainId};
}

void TagCausalStamp(void* item, const CausalStamp stamp, const std::uint32_t epoch,
                    const bool directSeed) noexcept {
    if (item == nullptr || !stamp.Valid()) return;
    std::lock_guard lock(gLineageMutex);
    if (epoch != gEpoch.load(std::memory_order_relaxed)) return;
    const auto token = reinterpret_cast<std::uintptr_t>(item);
    const auto previous = gCausalProvenance.Find(token);
    if (previous.Valid() && previous.SameChain(stamp)) return;
    if (previous.Valid()) {
        if (gCausalGroups.ReleaseRef(previous.chainId))
            gStats.causalChainsReleased.fetch_add(1U, std::memory_order_relaxed);
    }
    if (!gCausalProvenance.Put(token, stamp) || !gCausalGroups.AddRef(stamp.chainId)) {
        gCausalProvenance.Erase(token);
        if (gCausalGroups.DiscardIfUnreferenced(stamp.chainId))
            gStats.causalChainsReleased.fetch_add(1U, std::memory_order_relaxed);
        gStats.causalProvenanceOverflow.fetch_add(1U, std::memory_order_relaxed);
        gLiveCausalGroups.store(gCausalGroups.Size(), std::memory_order_release);
        return;
    }
    gLiveCausalTags.store(gCausalProvenance.Size(), std::memory_order_release);
    gLiveCausalGroups.store(gCausalGroups.Size(), std::memory_order_release);
    (directSeed ? gStats.causalProvenanceSeeded : gStats.causalProvenanceInherited)
        .fetch_add(1U, std::memory_order_relaxed);
    if (directSeed) RecordCausalSourceDomain(stamp.source);
}

void DiscardUnreferencedCausalStamp(const CausalStamp stamp,
                                    const std::uint32_t epoch) noexcept {
    if (!stamp.Valid()) return;
    std::lock_guard lock(gLineageMutex);
    if (epoch == gEpoch.load(std::memory_order_relaxed) &&
        gCausalGroups.DiscardIfUnreferenced(stamp.chainId)) {
        gStats.causalChainsReleased.fetch_add(1U, std::memory_order_relaxed);
        gLiveCausalGroups.store(gCausalGroups.Size(), std::memory_order_release);
    }
}

void Tag(void* item, Origin origin, std::uint32_t epoch) noexcept {
    if (item == nullptr || origin == Origin::None) return;
    std::lock_guard lock(gLineageMutex);
    // A constructor/dispatch that straddled a generation reset must not publish
    // its previous generation's origin into the freshly cleared table.
    if (epoch != gEpoch.load(std::memory_order_relaxed)) return;
    if (!gLineage.Put(reinterpret_cast<std::uintptr_t>(item), origin)) {
        gStats.lineageOverflow.fetch_add(1U, std::memory_order_relaxed);
        // Stop taking ownership of NEW roots; existing tags still suppress
        // recapture. No live tag is evicted to make space for another chain.
        gEnabled.store(false, std::memory_order_release);
    }
    gLiveTags.store(gLineage.Size(), std::memory_order_release);
}

void CarryOriginInFrame(Origin origin, std::uint32_t epoch, std::uintptr_t item) noexcept {
    if (origin == Origin::None || gFrame == nullptr || gFrame->item != item || gFrame->epoch != epoch ||
        epoch != gEpoch.load(std::memory_order_relaxed)) return;
    gFrame->origin = origin;
    Tag(reinterpret_cast<void*>(gFrame->item), origin, epoch);
}

void Forget(void* item) noexcept {
    if (item == nullptr) return;
    std::lock_guard lock(gLineageMutex);
    const auto token = reinterpret_cast<std::uintptr_t>(item);
    const auto stamp = gCausalProvenance.Find(token);
    ReleaseParentContextLocked(token);
    gTaskCauses.Erase(token);
    gLineage.Erase(token);
    gCausalProvenance.Erase(token);
    if (stamp.Valid() && gCausalGroups.ReleaseRef(stamp.chainId))
        gStats.causalChainsReleased.fetch_add(1U, std::memory_order_relaxed);
    gLiveTags.store(gLineage.Size(), std::memory_order_release);
    gLiveCausalTags.store(gCausalProvenance.Size(), std::memory_order_release);
    gLiveCausalGroups.store(gCausalGroups.Size(), std::memory_order_release);
}

// SEH lives only in POD helpers (MSVC C2712). Neither helper retains a pointer.
bool ReadPublicInput(const std::uint32_t* input, std::uint32_t& guid) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        if (input == nullptr) return false;
        guid = *input;
        return guid != 0U;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

bool RetailControlIdsMatch() noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        return *reinterpret_cast<const std::uint32_t*>(gModule + 0x0134DAF0U) == kActivateBehavior &&
            *reinterpret_cast<const std::uint32_t*>(gModule + 0x0134DAFCU) == kDeactivateBehavior;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

bool ReadPublicArguments(void** instance, const std::uint32_t* input,
                         void** callback, void*& execution, std::uint32_t& guid,
                         void*& callbackValue, bool& payloadless) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        // Read the public id first so a readable ProbeDirect id can still be
        // attributed when a later argument/context read fails. SEH stays here.
        if (input == nullptr) return false;
        guid = *input;
        if (instance == nullptr || *instance == nullptr || callback == nullptr)
            return false;
        execution = *reinterpret_cast<void**>(static_cast<std::byte*>(*instance) + 0x0CU);
        callbackValue = *callback;
        // 009265C0 / 009213D0: WorkItem+10 is the managed data reference for
        // THIS public invocation. The retained parent in +0C may carry its own
        // data, but that parent remains sender-local when a callback boundary is
        // detached; its payload is not the payload of the World action being
        // canonicalized. Never read/copy the parent data as action data.
        const bool workExecuteShape =
            reinterpret_cast<std::uintptr_t>(callback) ==
            reinterpret_cast<std::uintptr_t>(input) + 0x0CU;
        if (workExecuteShape) {
            payloadless = *reinterpret_cast<void* const*>(
                reinterpret_cast<const std::byte*>(input) + 0x10U) == nullptr;
        } else {
            // Outside the certified WorkExecute ABI, only an actually empty
            // callback holder is evidence for the None payload schema.
            payloadless = callbackValue == nullptr;
        }
        return execution != nullptr && guid != 0U;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

// Laboratory id used ONLY to count observations. This helper does not return
// an eligibility decision and never affects capture, lineage or replay.
constexpr std::uint32_t kDiagnosticProbeDirectShortGuid = 0x08F5AC62U;
void ObserveProbeDirectContext(std::uint32_t guid, bool argumentsRead,
    void* callbackValue, bool payloadless, double delay, std::uint32_t tail) noexcept {
    if (guid != kDiagnosticProbeDirectShortGuid) return;
    gStats.probeDirectSeen.fetch_add(1U, std::memory_order_relaxed);
    if (!argumentsRead) {
        gStats.probeDirectArgRejected.fetch_add(1U, std::memory_order_relaxed);
        return; // Other context fields are unknown, not assumed empty.
    }
    // Independent observations: e.g. payload and callback may both be present.
    if (!payloadless) gStats.probeDirectPayload.fetch_add(1U, std::memory_order_relaxed);
    if (callbackValue != nullptr) gStats.probeDirectCallback.fetch_add(1U, std::memory_order_relaxed);
    if (!std::isfinite(delay) || delay != 0.0) gStats.probeDirectDelay.fetch_add(1U, std::memory_order_relaxed);
    if (tail != 0U) gStats.probeDirectTail.fetch_add(1U, std::memory_order_relaxed);
}

// STATIC getter ABI and dispatch slot are taken from 00926680/008BD920.
// A manifest identity alone never authorizes calling an arbitrary vtable slot.
enum class IdentityFailure : std::uint8_t {
    None, BoundaryLayout, RuntimeKey, RegistryLookup, PublicInput, DefinitionGetter,
    DefinitionNull, DefinitionDispatch, DefinitionFault, DefinitionSelf
};

// The optional output only describes the existing failure branches. Callers
// outside the sender retain the same checks and do not record identity counters.
bool PublicDefinition(void* execution, void*& definition,
                      IdentityFailure* failure = nullptr) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        const auto vtable = *static_cast<std::byte**>(execution);
        const auto getter = *reinterpret_cast<DefinitionFn*>(vtable + 0x70U);
        const auto getterAddress = reinterpret_cast<std::uintptr_t>(getter);
        if (getterAddress < gModule + 0x1000U || getterAddress >= gModule + 0xD9E13BU) {
            if (failure != nullptr) *failure = IdentityFailure::DefinitionGetter;
            return false;
        }
        definition = getter(execution);
        if (definition == nullptr) {
            if (failure != nullptr) *failure = IdentityFailure::DefinitionNull;
            return false;
        }
        const auto definitionVtable = *static_cast<std::byte**>(definition);
        const bool matches = *reinterpret_cast<void**>(definitionVtable + 0x394U) == gPublicTarget;
        if (!matches && failure != nullptr) *failure = IdentityFailure::DefinitionDispatch;
        return matches;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (failure != nullptr) *failure = IdentityFailure::DefinitionFault;
        return false;
    }
#endif
}

// Borrow the containing Entity base only while the public call owns its object.
// A mismatch fails closed before native key extraction or any packet submission.
// No heuristic offset, alternative owner, or raw self key is ever accepted.
bool ReadBoundaryOwner(void* self, void*& owner, IdentityFailure& failure) noexcept {
    owner = nullptr;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        const auto address = reinterpret_cast<std::uintptr_t>(self);
        if (address < kCompositeInterfaceOffset || gModule == 0U || gPublicTarget == nullptr) {
            failure = IdentityFailure::BoundaryLayout;
            return false;
        }
        const auto interfaceVtable = *static_cast<std::uintptr_t*>(self);
        if (interfaceVtable != gModule + kCompositeInterfaceVtableRva ||
            *reinterpret_cast<void**>(interfaceVtable + 0x394U) != gPublicTarget) {
            failure = IdentityFailure::BoundaryLayout;
            return false;
        }
        auto* candidate = reinterpret_cast<void*>(address - kCompositeInterfaceOffset);
        const auto entityVtable = *static_cast<std::uintptr_t*>(candidate);
        if (entityVtable != gModule + kCompositeEntityVtableRva) {
            failure = IdentityFailure::BoundaryLayout;
            return false;
        }
        if (*reinterpret_cast<std::uintptr_t*>(entityVtable + 0x70U) !=
            gModule + kCompositeInterfaceGetterRva) {
            failure = IdentityFailure::DefinitionGetter;
            return false;
        }
        owner = candidate;
        return true;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failure = IdentityFailure::DefinitionFault;
        return false;
    }
#endif
}

void ObserveIdentityCandidates(WorldEntityRegistry& registry, void* self,
    std::uint32_t guid, const CathodeRuntimeKey& currentKey,
    bool currentIdValid, bool currentInputAllowed) noexcept {
    // IdentifyRuntimeEntity writes observedKey only after successful native
    // extraction (nonzero entityA), even when FindWorldEntityId then fails.
    // Preserve the original Current and raw Self comparisons as observations.
    // Neither authorizes the sender. No getter is called on raw self.
    CathodeRuntimeKey selfKey{};
    const auto selfOwner = registry.IdentifyRuntimeEntity(self, &selfKey);
    const bool selfInputAllowed = selfOwner && registry.IsCompositePublicInputAllowed(*selfOwner, guid);
    gStats.identityCurrent.Observe(currentKey.entityA != 0U, currentIdValid, currentInputAllowed);
    gStats.identitySelf.Observe(selfKey.entityA != 0U, selfOwner.has_value(), selfInputAllowed);
    if (guid == kDiagnosticProbeDirectShortGuid) {
        gStats.probeIdentityCurrent.Observe(currentKey.entityA != 0U, currentIdValid, currentInputAllowed);
        gStats.probeIdentitySelf.Observe(selfKey.entityA != 0U, selfOwner.has_value(), selfInputAllowed);
    }
}

void RecordIdentityFailure(IdentityFailure failure, std::uint32_t guid) noexcept {
    if (failure == IdentityFailure::None) return;
    gStats.identityRejected.fetch_add(1U, std::memory_order_relaxed);
    const bool probe = guid == kDiagnosticProbeDirectShortGuid;
    // One reason per global rejection of the authoritative boundary owner.
#define WS3_IDENTITY_REASON(kind) \
    case IdentityFailure::kind: \
        gStats.identity##kind##Rejected.fetch_add(1U, std::memory_order_relaxed); \
        if (probe) gStats.probeIdentity##kind##Rejected.fetch_add(1U, std::memory_order_relaxed); \
        break
    switch (failure) {
        WS3_IDENTITY_REASON(BoundaryLayout);
        WS3_IDENTITY_REASON(RuntimeKey);
        WS3_IDENTITY_REASON(RegistryLookup);
        WS3_IDENTITY_REASON(PublicInput);
        WS3_IDENTITY_REASON(DefinitionGetter);
        WS3_IDENTITY_REASON(DefinitionNull);
        WS3_IDENTITY_REASON(DefinitionDispatch);
        WS3_IDENTITY_REASON(DefinitionFault);
        WS3_IDENTITY_REASON(DefinitionSelf);
        case IdentityFailure::None: break;
    }
#undef WS3_IDENTITY_REASON
}

void ObserveCurrentOwnerHypotheses(WorldEntityRegistry& registry, const WorldEntityId& currentId,
    std::uint32_t guid, bool& currentInputAllowed) noexcept {
    // Keep the old diagnostic series comparable. These lookups are passive:
    // success, failure or ambiguity cannot choose or veto the actual owner.
    currentInputAllowed = registry.IsCompositePublicInputAllowed(currentId, guid);
    if (currentInputAllowed) {
        gStats.identityDirectOwnerValid.fetch_add(1U, std::memory_order_relaxed);
        if (guid == kDiagnosticProbeDirectShortGuid)
            gStats.probeIdentityDirectOwnerValid.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    const auto descriptor = registry.FindCompositePublicInputForInterfaceTarget(currentId, guid);
    if (!descriptor || !descriptor->ownerCompositeInstance.Valid() ||
        !(descriptor->interfaceVariableEntity == currentId) ||
        descriptor->publicInputShortGuid != guid || !descriptor->noTypedData ||
        !registry.IsCompositePublicInputAllowed(descriptor->ownerCompositeInstance, guid)) {
        gStats.identityInterfaceMapRejected.fetch_add(1U, std::memory_order_relaxed);
        if (guid == kDiagnosticProbeDirectShortGuid)
            gStats.probeIdentityInterfaceMapRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    gStats.identityInterfaceMapValid.fetch_add(1U, std::memory_order_relaxed);
    if (guid == kDiagnosticProbeDirectShortGuid)
        gStats.probeIdentityInterfaceMapValid.fetch_add(1U, std::memory_order_relaxed);
}

bool ResolvePublicRootOwner(WorldEntityRegistry& registry, void* self, std::uint32_t guid,
    void*& ownerExecution, CathodeRuntimeKey& ownerKey, WorldEntityId& ownerId,
    bool& inputAllowed, IdentityFailure& failure) noexcept {
    ownerId = {};
    ownerKey = {};
    ownerExecution = nullptr;
    void* candidate{};
    if (!ReadBoundaryOwner(self, candidate, failure)) return false;
    void* definition{};
    if (!PublicDefinition(candidate, definition, &failure)) return false;
    if (definition != self) {
        failure = IdentityFailure::DefinitionSelf;
        return false;
    }
    // The certified Entity -> CompositeInterface round trip establishes the
    // object type independently of RuntimeKey success or manifest membership.
    ownerExecution = candidate;
    gStats.identityBoundaryOwnerValid.fetch_add(1U, std::memory_order_relaxed);
    const bool probe = guid == kDiagnosticProbeDirectShortGuid;
    if (probe) gStats.probeIdentityBoundaryOwnerValid.fetch_add(1U, std::memory_order_relaxed);
    // Gate the owner of THIS public action, never the player/source that
    // initiated its chain. Non-World owners must remain local so retail can
    // continue to the first eligible World public action without a lineage tag.
    const auto id = registry.IdentifyRuntimeEntity(candidate, &ownerKey);
    inputAllowed = false;
    if (id && id->Valid()) {
        ownerId = *id;
        // Runtime context already proved THIS invocation payloadless before
        // entering identity resolution. The exact forward port may have any
        // static DataType; noTypedData is the legacy v1 policy, not this proof.
        inputAllowed = registry.IsCompositePublicRootKnown(ownerId, guid);
    }
    gStats.identityOwner.Observe(ownerKey.entityA != 0U, ownerId.Valid(), inputAllowed);
    if (probe) gStats.probeIdentityOwner.Observe(ownerKey.entityA != 0U, ownerId.Valid(), inputAllowed);
    if (!ownerId.Valid()) {
        failure = ownerKey.entityA == 0U ? IdentityFailure::RuntimeKey : IdentityFailure::RegistryLookup;
        return false;
    }
    if (!inputAllowed &&
        (probe || SecurityEnabled(AdmissionSecurityGate::ManifestOwnerInput))) {
        failure = IdentityFailure::PublicInput;
        return false;
    }
    if (!inputAllowed)
        gStats.admissionManifestOwnerInputBypassed.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

void ObserveProbeIdentity(WorldEntityRegistry& registry, std::uint32_t guid, void* execution,
    void* ownerExecution, const CathodeRuntimeKey& currentKey, const WorldEntityId& currentId,
    const CathodeRuntimeKey& ownerKey, const WorldEntityId& ownerId) noexcept {
    if (guid != kDiagnosticProbeDirectShortGuid) return;
    ProbeIdentityObservation observation{};
    observation.observed = true;
    observation.boundaryOwnerValid = ownerExecution != nullptr;
    observation.ownerMatchesCurrent = ownerExecution != nullptr && ownerExecution == execution;
    observation.currentKey = currentKey;
    observation.ownerKey = ownerKey;
    observation.currentId = currentId;
    observation.ownerId = ownerId;
    observation.currentKind = registry.FindWorldEntityKind(currentId).value_or(WorldEntityKind::Unknown);
    observation.ownerKind = registry.FindWorldEntityKind(ownerId).value_or(WorldEntityKind::Unknown);
    const auto manifest = registry.InspectCompositePublicInputManifest(ownerId, guid);
    observation.manifestReady = manifest.manifestReady;
    observation.manifestGuidKnown = manifest.guidKnown;
    observation.manifestExactForwardKeyPresent = manifest.exactForwardKeyPresent;
    observation.manifestExactForwardAllowed = manifest.exactForwardAllowed;
    observation.manifestExactOwnerSeenForGuid = manifest.exactOwnerSeenForGuid;
    observation.manifestExactBridgeCount = manifest.exactBridgeCount;
    observation.manifestExactPayloadlessBridgeCount = manifest.exactPayloadlessBridgeCount;
    observation.manifestExactTypedBridgeCount = manifest.exactTypedBridgeCount;
    observation.manifestGuidBridgeCount = manifest.guidBridgeCount;
    observation.manifestGuidOwnerCount = manifest.guidOwnerCount;
    observation.manifestFirstGuidOwner = manifest.firstGuidOwner;
    observation.manifestFirstGuidDataType = manifest.firstGuidDataType;
    observation.manifestFirstGuidNoTypedData = manifest.firstGuidNoTypedData;
    std::lock_guard lock(gProbeIdentityMutex);
    gProbeIdentityLast = observation;
}

void InvokePublicScoped(PublicFn original, Origin origin, std::uint32_t epoch, void* self, void** instance,
    const std::uint32_t* input, void** callback, double delay, std::uint32_t tail,
    const ContextTrace* cause = nullptr) {
    const auto previousCause = gPublicCause;
    if (cause != nullptr) gPublicCause = *cause;
    else if (gFrame != nullptr && gFrame->epoch == epoch) gPublicCause = gFrame->cause;
    const auto previous = gPublicOrigin;
    const auto previousEpoch = gPublicEpoch;
    gPublicOrigin = origin;
    gPublicEpoch = epoch;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        original(self, instance, input, callback, delay, tail);
    } __finally {
        gPublicOrigin = previous;
        gPublicEpoch = previousEpoch;
        gPublicCause = previousCause;
    }
#else
    original(self, instance, input, callback, delay, tail);
    gPublicOrigin = previous;
    gPublicEpoch = previousEpoch;
    gPublicCause = previousCause;
#endif
}

// 008BE0D0 is the common emitted-port propagator. Its ECX can be the
// Entity+70 or Entity+6C interface. Reconstruct only a getter actually matched
// against the borrowed execution object; no guessed interface offset.
bool OutputDefinition(void* execution, const std::uint8_t selector, void*& definition) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        if (execution == nullptr || (selector != 1U && selector != 2U)) return false;
        const auto vtable = *static_cast<std::byte**>(execution);
        const auto getter = *reinterpret_cast<DefinitionFn*>(vtable + (selector == 1U ? 0x70U : 0x6CU));
        const auto address = reinterpret_cast<std::uintptr_t>(getter);
        if (address < gModule + 0x1000U || address >= gModule + 0xD9E13BU) return false;
        definition = getter(execution);
        if (definition == nullptr) return false;
        const auto dispatch = *reinterpret_cast<void**>(*static_cast<std::byte**>(definition) + 0x394U);
        // The observed base propagation may be called from an override.
        // The sender proves getter(execution) == observed self; the receiver
        // obtains that same base interface through the portable selector.
        // Replaying the base emission must not call the override's decision.
        const auto dispatchAddress = reinterpret_cast<std::uintptr_t>(dispatch);
        return dispatchAddress >= gModule + 0x1000U && dispatchAddress < gModule + 0xD9E13BU;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

// 008BE0D0 performs this implicit forwarding BEFORE looking up the explicit
// output-port edges at 008BD270. If it can run, the current WM08 exact-output
// profile does not describe the entire native invocation and must not claim it
// as covered. Fail closed on unknown +11C implementations: running retail via
// InvokeEmissionScoped keeps the causal stamp but does not install LocalRoot,
// so already-existing CompositeInput/EmittedOutput boundaries remain able to
// observe the actually executed downstream path instead of being swallowed by
// an incomplete outer ROOT.
[[nodiscard]] bool EmittedOutputProfileCompleteAtRuntime(void* execution) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        if (execution == nullptr || gModule == 0U) return false;
        const auto vtable = *static_cast<std::byte**>(execution);
        if (vtable == nullptr) return false;
        const auto resolver = *reinterpret_cast<void**>(vtable + 0x11CU);
        if (resolver != reinterpret_cast<void*>(gModule + kEntityForwardingResolverRva))
            return false;
        const auto flags = *reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::byte*>(execution) + 0x04U);
        return (flags & kEntityForwardingFlag) == 0U;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

void InvokeEmissionScoped(PublicFn original, const CausalStamp stamp, std::uint32_t epoch,
    void* self, void** instance, const std::uint32_t* input, void** callback,
    double delay, std::uint32_t tail) {
    const auto previous = gEmissionStamp;
    const auto previousEpoch = gEmissionEpoch;
    gEmissionStamp = stamp;
    gEmissionEpoch = epoch;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try { original(self, instance, input, callback, delay, tail); }
    __finally { gEmissionStamp = previous; gEmissionEpoch = previousEpoch; }
#else
    original(self, instance, input, callback, delay, tail);
    gEmissionStamp = previous; gEmissionEpoch = previousEpoch;
#endif
}

void PublicCapture(void* self, void** instance,
    const std::uint32_t* input, void** callback, double delay, std::uint32_t tail) {
    const auto original = reinterpret_cast<PublicFn>(gPublicTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    const bool scheduledShape = input != nullptr &&
        reinterpret_cast<std::uintptr_t>(callback) == reinterpret_cast<std::uintptr_t>(input) + 0x0CU;
    auto origin = scheduledShape ? FindOrigin(const_cast<std::uint32_t*>(input), epoch) : CurrentOrigin();
    if (origin == Origin::None && scheduledShape && gFrame != nullptr &&
        gFrame->epoch == epoch && gFrame->item == reinterpret_cast<std::uintptr_t>(input))
        origin = gFrame->origin;
    void* restoredParent{};
    ParentContextBinding restoredParentContext{};
    if (!scheduledShape && replication_core::RemoteApplyActive()) origin = Origin::RemoteRoot;
    if (origin == Origin::None) {
        // Retail WorkExecute passes &currentWorkItem->+0x0C, not the retained
        // parent value itself. The previous fallback dereferenced that holder
        // first and therefore skipped the tagged current item. Recover the
        // current item from the holder address and consult ONLY an exact tag;
        // if this call came from a different ABI shape, fall back to the
        // retained-parent ancestry without trusting the derived pointer.
        if (reinterpret_cast<std::uintptr_t>(callback) ==
            reinterpret_cast<std::uintptr_t>(input) + 0x0CU)
            origin = FindExactOrigin(WorkItemFromCallbackHolder(callback), epoch);
        restoredParent = ReadWorkHolder(callback);
        if (origin == Origin::None)
            origin = FindOrigin(restoredParent, epoch);
        if (origin == Origin::None) {
            restoredParentContext = ParentContext(restoredParent, epoch);
            if (restoredParentContext.origin != Origin::None) {
                origin = restoredParentContext.origin;
                gStats.parentContextInheritedOrigin.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        if (origin != Origin::None) {
            gStats.lineageCallbackRestore.fetch_add(1U, std::memory_order_relaxed);
            CarryOriginInFrame(origin, epoch, reinterpret_cast<std::uintptr_t>(input));
        }
    }
    const auto missionCause = scheduledShape ? TaskCause(const_cast<std::uint32_t*>(input), epoch)
        : TaskCause(restoredParent != nullptr ? restoredParent : ReadWorkHolder(callback), epoch);
    if (!mission_sync::ReplayActive() && CurrentRootKind() != kMissionRootKind
        && missionCause.rootKind != kMissionRootKind) {
        void* execution{}; void* parent{}; std::uint32_t port{};
        bool payloadless{}, method{};
        // The public dispatcher supplies a caller execution as well as its
        // interface self. Derive the receiving owner from self, then verify it;
        // caller identity is never authority to replay a different composite.
        void* boundaryOwner{}; IdentityFailure failure{};
        if (ReadPublicArguments(instance, input, callback, execution, port, parent, payloadless)
            && ReadBoundaryOwner(self, boundaryOwner, failure)
            && mission_sync::Candidate(boundaryOwner, port, method, 3U)) {
            if ((!payloadless && !method) || delay != 0.0 || tail != 0U) {
                mission_sync::NoteUnsafeShape(!payloadless, delay != 0.0, tail != 0U);
            } else if (origin == Origin::RemoteRoot) {
                return; // A WorldSync replay cannot run ahead of its mission commit.
            } else if ((origin == Origin::None || origin == Origin::LocalRoot)
                && !replication_core::RemoteApplyActive()
                && mission_sync::Capture(boundaryOwner, port, 3U,
                    CurrentCausalStamp(epoch).source == CausalSource::Player)) {
                mission_sync::NoteCapturedShape(!payloadless, false, false);
                return;
            }
        }
    }
    if (origin != Origin::None) {
        // This invocation is covered by a concrete ROOT scope or by work
        // created inside that scope. Shared causal identity alone never grants
        // coverage to a sibling invocation.
        void* const lineageItem = scheduledShape
            ? const_cast<std::uint32_t*>(input) : restoredParent;
        ContextTrace restoredCause = scheduledShape
            ? TaskCause(lineageItem, epoch)
            : (gPublicEpoch == epoch && gPublicOrigin != Origin::None
                ? gPublicCause : TaskCause(lineageItem, epoch));
        if (restoredParentContext.invocation != 0U) {
            if (restoredCause.cause == 0U) restoredCause.cause = restoredParentContext.cause;
            if (restoredCause.sourcePeer == 0U) restoredCause.sourcePeer = restoredParentContext.sourcePeer;
            if (restoredCause.authoritySequence == 0U)
                restoredCause.authoritySequence = restoredParentContext.authoritySequence;
            if (restoredCause.causalChainId == 0U && restoredParentContext.causalStamp.Valid()) {
                restoredCause.causalChainId = restoredParentContext.causalStamp.chainId;
                restoredCause.causalSource = static_cast<std::uint8_t>(restoredParentContext.causalStamp.source);
            }
        }
        (origin == Origin::RemoteRoot ? gStats.remoteSuppressed : gStats.descendantsSuppressed)
            .fetch_add(1U, std::memory_order_relaxed);
        InvokePublicScoped(original, origin, epoch, self, instance, input, callback, delay, tail,
            restoredCause.invocation != 0U || restoredCause.cause != 0U ? &restoredCause : nullptr);
        return;
    }

    const auto invokeRetailFallback = [&]() noexcept {
        original(self, instance, input, callback, delay, tail);
    };
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!Enabled() || registry == nullptr || !registry->RuntimeCaptureReady()) {
        invokeRetailFallback();
        return;
    }
    // An inactive World bus already takes the retail-only path below. Avoid
    // manifest/profile/context construction for a ROOT that cannot be sent.
    // Origin restoration above, work lineage hooks and the native call remain
    // intact. Keep the final gate as well: the bus can close during admission.
    if (!SessionAuthorityBound() || !WorldBusActive()) {
        invokeRetailFallback();
        return;
    }
    gStats.seen.fetch_add(1U, std::memory_order_relaxed);
    void* execution{}; void* callbackValue{}; std::uint32_t guid{}; bool payloadless{};
    if (!ReadPublicInput(input, guid)) {
        Classify(ActionClass::Unsafe);
        gStats.argRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.contextRejected.fetch_add(1U, std::memory_order_relaxed);
        invokeRetailFallback();
        return;
    }
    const bool probeDirect = guid == kDiagnosticProbeDirectShortGuid;
    if (ForbiddenPublicInput(guid)) {
        if (SecurityEnabled(AdmissionSecurityGate::ForbiddenBehaviorInputs)) {
            Classify(ActionClass::LocalOnly);
            invokeRetailFallback();
            return;
        }
        gStats.admissionForbiddenInputBypassed.fetch_add(1U, std::memory_order_relaxed);
    }
    // The manifest ShortGuid prefilter is a performance/admission gate, not a
    // structural prerequisite. It can be opened live to test observed retail
    // public actions that the static manifest did not enumerate.
    if (!registry->CouldBeCompositePublicInputShortGuid(guid)) {
        if (SecurityEnabled(AdmissionSecurityGate::ManifestGuidPrefilter)) {
            gStats.admissionGuidPrefilterRejected.fetch_add(1U, std::memory_order_relaxed);
            invokeRetailFallback();
            return;
        }
        gStats.admissionGuidPrefilterBypassed.fetch_add(1U, std::memory_order_relaxed);
    }
    gStats.publicCandidates.fetch_add(1U, std::memory_order_relaxed);
    const bool argumentsRead = ReadPublicArguments(instance, input, callback,
        execution, guid, callbackValue, payloadless);
    ObserveProbeDirectContext(guid, argumentsRead, callbackValue, payloadless, delay, tail);
    if (!argumentsRead) {
        Classify(ActionClass::Unsafe);
        // Null/invalid arguments, zero id, null execution or protected read fault.
        // If the id itself is unreadable, no probe attribution can be made.
        gStats.argRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.contextRejected.fetch_add(1U, std::memory_order_relaxed);
        invokeRetailFallback();
        return;
    }
    // ROOT v9 reads the scheduled invocation through its verified holder ABI.
    // Parent dependence is checked separately from the action's typed data.
    const bool workExecuteShape =
        reinterpret_cast<std::uintptr_t>(callback) ==
        reinterpret_cast<std::uintptr_t>(input) + 0x0CU;
    void* const currentWorkItem = workExecuteShape
        ? const_cast<std::uint32_t*>(input) : nullptr;
    const auto publicParentContext = ParentContext(callbackValue, epoch);
    auto causalStamp = workExecuteShape
        ? FindCausalStamp(currentWorkItem, epoch) : CurrentCausalStamp(epoch);
    if (!causalStamp.Valid() && publicParentContext.causalStamp.Valid()) {
        causalStamp = publicParentContext.causalStamp;
        gStats.parentContextInheritedCausal.fetch_add(1U, std::memory_order_relaxed);
    }

    // These are properties of the action itself and can be rejected before any
    // retained-parent classification. They remain a first-failure partition of
    // contextRejected for Arg/Payload/Callback/Delay/Tail diagnostics.
    std::atomic<std::uint64_t>* contextReason = nullptr;
    if (!payloadless) contextReason = &gStats.payloadRejected; // no certified action payload schema
    if (contextReason == nullptr &&
        (!std::isfinite(delay) || delay < 0.0 || delay > 600.0))
        contextReason = &gStats.delayRejected;
    if (contextReason == nullptr && tail != 0U) contextReason = &gStats.tailRejected;
    if (contextReason == nullptr && callbackValue != nullptr &&
        !workExecuteShape) {
        if (SecurityEnabled(AdmissionSecurityGate::CertifiedCallbackShape))
            contextReason = &gStats.callbackRejected;
        else
            gStats.admissionCallbackShapeBypassed.fetch_add(1U, std::memory_order_relaxed);
    }
    if (contextReason != nullptr) {
        Classify(contextReason == &gStats.delayRejected ? ActionClass::Unsafe : ActionClass::NeedsAdapter);
        if (contextReason == &gStats.callbackRejected && !probeDirect)
            gStats.callbackGameplayRejected.fetch_add(1U, std::memory_order_relaxed);
        contextReason->fetch_add(1U, std::memory_order_relaxed);
        gStats.contextRejected.fetch_add(1U, std::memory_order_relaxed);
        invokeRetailFallback();
        return;
    }

    CathodeRuntimeKey currentKey{};
    const auto currentId = registry->IdentifyRuntimeEntity(execution, &currentKey);
    const auto currentDomain = currentId
        ? registry->FindWorldEntityDomain(*currentId) : std::nullopt;
    if (ai_sync::OwnsAI() && currentDomain == WorldDomain::AI) {
        Classify(ActionClass::LocalOnly);
        invokeRetailFallback();
        return;
    }
    bool currentInputAllowed = false;
    if (probeDirect) {
        if (currentId) ObserveCurrentOwnerHypotheses(*registry, *currentId, guid, currentInputAllowed);
        ObserveIdentityCandidates(*registry, self, guid, currentKey,
            currentId.has_value(), currentInputAllowed);
    }

    // 0.11.4.73 restores a SEMANTIC World boundary, not the old admission
    // safeties. Player/AI/UI/AIMP actions remain local mechanics; their causal
    // stamp continues through WorkBuild/WorkExecute until the first stable
    // public action whose actual target belongs to the shared World domain.
    // Unknown/unmapped targets are not a valid causal ROOT because there is no
    // receiver-stable shared-world endpoint to replay. ProbeDirect remains the
    // controlled transport witness and may exercise its already proven path.
    if (!probeDirect && (!currentDomain.has_value() || *currentDomain != WorldDomain::World)) {
        if (currentDomain.has_value() && *currentDomain != WorldDomain::Unknown) {
            Classify(ActionClass::LocalOnly);
            gStats.causalRootNonWorldLocal.fetch_add(1U, std::memory_order_relaxed);
            if (*currentDomain == WorldDomain::Player)
                gStats.playerCausalRejected.fetch_add(1U, std::memory_order_relaxed);
        } else {
            Classify(ActionClass::Unknown);
            gStats.causalRootMissing.fetch_add(1U, std::memory_order_relaxed);
        }
        invokeRetailFallback();
        return;
    }

    // Normal gameplay must have a real local causal transaction before a World
    // boundary can become a network ROOT. This replaces the unrestricted .66
    // behavior that treated every unrelated World callback as a fresh cause.
    if (!probeDirect && !causalStamp.Valid()) {
        gStats.causalRootMissing.fetch_add(1U, std::memory_order_relaxed);
        if (callbackValue != nullptr)
            gStats.callbackWorldContinuationRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.callbackBoundaryUnknownRejected.fetch_add(1U, std::memory_order_relaxed);
        Classify(callbackValue != nullptr ? ActionClass::NeedsAdapter : ActionClass::Unknown);
        invokeRetailFallback();
        return;
    }

    // Parent dependence is represented by the ROOT v9 retained context chain. Local personal effects were
    // separated above; the surviving candidate is the first replayable World
    // action of one causal transaction (or the ProbeDirect witness).
    const bool externalInvocation = callbackValue == nullptr && gFrame == nullptr &&
        !workExecuteShape;
    if (!externalInvocation && !workExecuteShape) {
        gStats.contextUnsupported.fetch_add(1U, std::memory_order_relaxed);
        invokeRetailFallback();
        return;
    }
    ContextTrace cause = workExecuteShape ? TaskCause(const_cast<std::uint32_t*>(input), epoch)
                                         : ContextTrace{};
    Origin parentContextOrigin{};
    auto parentContextStamp = causalStamp;
    MergeParentContext(publicParentContext, parentContextOrigin, parentContextStamp, cause);
    if (!causalStamp.Valid() && parentContextStamp.Valid()) causalStamp = parentContextStamp;
    if (cause.invocation == 0U) {
        cause.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
        if (workExecuteShape) gStats.actorUnresolved.fetch_add(1U, std::memory_order_relaxed);
    }
    cause.epoch = epoch;
    cause.sourcePeer = gLocalPeer.load(std::memory_order_acquire);
    cause.input = guid;
    cause.causalChainId = causalStamp.chainId;
    cause.causalSource = static_cast<std::uint8_t>(causalStamp.source);
    cause.target = currentId.value_or(WorldEntityId{});
    world_interactions::CompositePublicInputEvent event{};
    event.retailRoot = true;
    event.publicInputShortGuid = guid;
    event.targetEntity = cause.target;
    event.relativeDelaySeconds = delay;
    event.actorKind = static_cast<world_interactions::PublicActorKind>(cause.actorKind);
    event.actorEntity = cause.actor;
    if (causalStamp.source == CausalSource::Player) {
        event.actorKind = world_interactions::PublicActorKind::SourcePlayer;
        event.actorEntity = {};
    }
    if (!CaptureContext(*registry, callbackValue, event, cause)) {
        gStats.contextUnsupported.fetch_add(1U, std::memory_order_relaxed);
        gStats.contextRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.callbackRejected.fetch_add(1U, std::memory_order_relaxed);
        Classify(ActionClass::NeedsAdapter);
        void* diagnosticOwner{};
        IdentityFailure diagnosticFailure{};
        if (ReadBoundaryOwner(self, diagnosticOwner, diagnosticFailure))
            cause.owner = registry->IdentifyRuntimeEntity(diagnosticOwner).value_or(WorldEntityId{});
        TraceContext(cause, 2U);
        invokeRetailFallback();
        return;
    }
    const bool detachedCallback = callbackValue != nullptr; // wire bit retained; context is reconstructed
    cause.contextKind = static_cast<std::uint8_t>(event.contextKind);
    cause.contextRuntimeKey = event.contextFrameCount != 0U
        ? event.contextFrames[0].runtimeKey : CathodeRuntimeKey{};
    if (!probeDirect && causalStamp.Valid()) {
        gStats.causalProvenanceBoundary.fetch_add(1U, std::memory_order_relaxed);
        if (detachedCallback)
            gStats.callbackBoundaryCandidates.fetch_add(1U, std::memory_order_relaxed);
    }

    // Diagnostic only: context eligibility is not proof of identity or sending.
    if (probeDirect)
        gStats.probeDirectEligible.fetch_add(1U, std::memory_order_relaxed);

    // Resolve exactly one owner from the public boundary's certified subobject.
    // `execution` remains the independently identified target.
    WorldEntityId ownerCompositeInstance{};
    CathodeRuntimeKey ownerKey{};
    void* ownerExecution{};
    IdentityFailure identityFailure = IdentityFailure::None;
    bool ownerInputAllowed = false;
    const bool ownerResolved = ResolvePublicRootOwner(*registry, self, guid, ownerExecution,
        ownerKey, ownerCompositeInstance, ownerInputAllowed, identityFailure);
    ObserveProbeIdentity(*registry, guid, execution, ownerExecution, currentKey,
        currentId.value_or(WorldEntityId{}), ownerKey, ownerCompositeInstance);
    if (!ownerResolved) {
        const auto domain = registry->FindWorldEntityDomain(ownerCompositeInstance);
        Classify(domain && *domain != WorldDomain::World && *domain != WorldDomain::Unknown
            ? ActionClass::LocalOnly : ActionClass::Unknown);
        RecordIdentityFailure(identityFailure, guid);
        invokeRetailFallback();
        return;
    }

    bool compositeStateTransitionCarrier = false;

    // Generic Actor -> World boundary selection. The manifest compiler follows
    // this exact public Composite input through COMMANDS and records what its
    // downstream graph can reach. Ordinary ROOTs retain the conservative
    // actor-independent World-domain rule. Patch 5.1 additionally admits a
    // compiler-certified high-level Composite StateTransition carrier when all
    // known relevant descendants converge on one exact semantic target/op.
    // Non-truncated UnknownDependency side paths may remain in the aggregate
    // profile and are explicitly marked by relaxed-frontier evidence. No
    // gameplay object, archetype, path or input ShortGuid is special-cased
    // here. Earlier uncertified actor/UI-only steps run locally and leave the
    // causal transaction open for a later certified boundary.
    {
        const auto boundaryProfile = registry->FindCompositePublicInputCausalProfile(
            ownerCompositeInstance, guid);
        if (!boundaryProfile.has_value()) {
            gStats.causalBoundaryProfileMissing.fetch_add(1U, std::memory_order_relaxed);
            Classify(ActionClass::Unknown);
            invokeRetailFallback();
            return;
        }
        RecordRelevanceCandidate(*registry, ownerCompositeInstance, guid,
            world_interactions::PublicRootKind::CompositeInput, *boundaryProfile);
        compositeStateTransitionCarrier =
            boundaryProfile->CertifiedCompositeStateTransitionCarrierBoundary();
        if (compositeStateTransitionCarrier)
            gStats.compositeCarrierObserved.fetch_add(1U, std::memory_order_relaxed);
        if (boundaryProfile->Has(WorldCausalDependencyUnknown))
            gStats.causalBoundaryUnknownDependency.fetch_add(1U, std::memory_order_relaxed);
        if (boundaryProfile->Has(WorldCausalDependencyTruncated))
            gStats.causalBoundaryTruncated.fetch_add(1U, std::memory_order_relaxed);
        if (!boundaryProfile->Has(WorldCausalDependencyWorldEffect))
            gStats.causalBoundaryNoWorldEffect.fetch_add(1U, std::memory_order_relaxed);
        if (boundaryProfile->Has(WorldCausalDependencyBranching))
            gStats.causalBoundaryBranching.fetch_add(1U, std::memory_order_relaxed);
        if (boundaryProfile->Has(WorldCausalDependencyPlayer))
            gStats.causalBoundaryPlayerDependent.fetch_add(1U, std::memory_order_relaxed);

        // WM08: domain reachability is not network admission. Unknown,
        // NotRelevant and DeferToDescendant always execute retail here so
        // causal lineage can continue to a lower certified Replicate* boundary.
        if (!boundaryProfile->NetworkReplicableBoundary()) {
            if (boundaryProfile->Has(WorldCausalDependencyWorldEffect))
                gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
            switch (boundaryProfile->RelevanceDisposition()) {
            case WorldReplicationRelevanceDisposition::Unknown:
                Classify(ActionClass::NeedsAdapter); break;
            case WorldReplicationRelevanceDisposition::NotRelevant:
                Classify(ActionClass::LocalOnly); break;
            case WorldReplicationRelevanceDisposition::DeferToDescendant:
            default:
                break;
            }
            invokeRetailFallback();
            return;
        }
        if (compositeStateTransitionCarrier)
            gStats.compositeCarrierNetworkEligible.fetch_add(1U, std::memory_order_relaxed);
        gStats.causalBoundaryWorldCandidate.fetch_add(1U, std::memory_order_relaxed);
    }

    // Context shape has the same meaning for gameplay and diagnostic inputs.
    // DetachedCallback keeps its historical name but now requires a reconstruction recipe.
    std::uint8_t actionFlags{};
    if (externalInvocation)
        actionFlags |= world_interactions::kPublicRootExternalInvocation;
    if (detachedCallback)
        actionFlags |= world_interactions::kPublicRootDetachedCallback;

    const bool targetMatchesOwner = currentId.has_value() && currentId->Valid() &&
        *currentId == ownerCompositeInstance;
    bool targetAllowed = false;
    if (currentId && currentId->Valid()) {
        const bool stableWorldTarget = currentDomain.has_value() &&
            *currentDomain == WorldDomain::World;

        // Patch 5.1: a compiler-certified high-level Composite carrier already
        // has stronger semantic ownership evidence than the historical static
        // owner/input/target whitelist. Keep stable runtime identity + World
        // domain as the hard requirement, but do not make the legacy target
        // edge a second independent veto. This deliberately leaves the valve
        // open for runtime-equivalent targets produced by real CATHODE routing.
        if (compositeStateTransitionCarrier && stableWorldTarget) {
            targetAllowed = true;
            if (ownerInputAllowed && !registry->IsCompositePublicRootActionKnown(
                    ownerCompositeInstance, guid, *currentId, delay, actionFlags)) {
                gStats.admissionManifestTargetBypassed.fetch_add(1U, std::memory_order_relaxed);
            }
        } else if (!ownerInputAllowed) {
            // The owner/input whitelist was explicitly opened. No exact static
            // port contract exists to consult, so stable runtime identity plus
            // the independently controlled target-domain gates are the fallback.
            targetAllowed = currentDomain.has_value() &&
                (*currentDomain == WorldDomain::World ||
                 (*currentDomain != WorldDomain::Unknown &&
                  !LocalTargetSecurityEnabled(*currentDomain)));
        } else {
            targetAllowed = registry->IsCompositePublicRootActionKnown(
                ownerCompositeInstance, guid, *currentId, delay, actionFlags);
            if (!targetAllowed &&
                !SecurityEnabled(AdmissionSecurityGate::ManifestTargetContract) &&
                currentDomain.has_value() &&
                (*currentDomain == WorldDomain::World ||
                 (*currentDomain != WorldDomain::Unknown &&
                  !LocalTargetSecurityEnabled(*currentDomain)))) {
                targetAllowed = true;
                gStats.admissionManifestTargetBypassed.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }
    if (!targetAllowed) {
        const auto domain = currentId ? registry->FindWorldEntityDomain(*currentId) : std::nullopt;
        Classify(domain && *domain != WorldDomain::World && *domain != WorldDomain::Unknown
            ? ActionClass::LocalOnly : (detachedCallback ? ActionClass::NeedsAdapter : ActionClass::Unknown));
        gStats.targetOwnerRejected.fetch_add(1U, std::memory_order_relaxed);
        if (guid == kDiagnosticProbeDirectShortGuid)
            gStats.probeTargetOwnerRejected.fetch_add(1U, std::memory_order_relaxed);
        if (!probeDirect) gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        invokeRetailFallback();
        return;
    }
    if (compositeStateTransitionCarrier)
        gStats.compositeCarrierTargetAdmitted.fetch_add(1U, std::memory_order_relaxed);
    (targetMatchesOwner ? gStats.targetOwnerMatched : gStats.targetOwnerDistinct)
        .fetch_add(1U, std::memory_order_relaxed);

    Classify(targetMatchesOwner ? ActionClass::GenericRoot : ActionClass::TargetedRoot);

    // Step 3 admission contract. SourcePlayer is causal attribution, not an
    // authority grant. Authority is considered only after the boundary/profile,
    // identity, context and target have all been certified. A non-Host Client
    // may submit a Player-origin World boundary, but it uses AwaitAuthority so
    // the originating process does not execute that World decision before the
    // Host sequences it. Autonomous Client World decisions are never proposed
    // as peer authority; once fully certified they are simply Host-owned.
    const bool multiplayerSession = SessionAuthorityBound();
    const bool localAuthorityHost = SessionAuthorityHost();
    const bool nonHostClient = multiplayerSession && !localAuthorityHost;

    // Patch 5.12: a compiler-certified Composite StateTransition carrier is
    // already the exact generic semantic boundary the Host publishes.  Mirror
    // that proven Host path on a non-Host Client instead of synthesizing a
    // Door-specific replay below it.  The Client remains local-first: retail
    // executes on the originating process, SubmitPublicRoot installs the local
    // observation echo slot, and the authenticated Host only sequences and
    // rebroadcasts the same ROOT kind/owner/input/target/context it already
    // accepts from its own runtime.
    const bool clientLocalFirstStateCarrier = nonHostClient &&
        compositeStateTransitionCarrier;
    // Patch 5.13: diagnostics from the first locked ChallengeMap16 transition
    // proved a generic directional ordering asymmetry, not a Door identity
    // problem: the same compiler-certified Composite StateTransition siblings
    // were observed in exact reverse order on Client vs Host, while the
    // EmittedOutput tail retained its position. Mark only this already-narrow
    // non-Host Client carrier class for local seal-time normalization. The hint
    // is never serialized and contains no map/entity/input special case.
    event.normalizeClientStateCarrierOrder = clientLocalFirstStateCarrier;
    const bool sourcePlayer = causalStamp.source == CausalSource::Player &&
        event.actorKind == world_interactions::PublicActorKind::SourcePlayer;
    event.flags = actionFlags;
    if (nonHostClient && sourcePlayer && !clientLocalFirstStateCarrier)
        event.flags |= kPublicRootFlagAwaitAuthority;
    cause.owner = ownerCompositeInstance;
    event.causalSerial = causalStamp.Valid() ? causalStamp.chainId : cause.invocation;
    event.segmentSerial = event.causalSerial; // replaced by sealed scheduler-turn segment

    // Re-run the common wire/manifest contract on the final event (including
    // AwaitAuthority) before any local suppression decision. This makes
    // "certified" distinct from merely seeing a WorldEffect in the profile.
    if (!world_interactions::Initialized() ||
        !PublicRootManifestAdmissionAllowed(*registry, ownerCompositeInstance, event)) {
        if (!probeDirect) gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 11U);
        invokeRetailFallback();
        return;
    }
    if (compositeStateTransitionCarrier)
        gStats.compositeCarrierManifestAdmitted.fetch_add(1U, std::memory_order_relaxed);

    // A non-Player autonomous World decision on a Client is not a legal Client
    // proposal (the Launcher Host rejects it). Once the exact ROOT is certified,
    // Host ownership itself is the independent reason this local decision must
    // not run. No queue/observation is created on the Client.
    // First in-game lifecycle gate: a certified WM08 ROOT is still retail work
    // until the real lobby authority is bound and a compatible WorldContext bus
    // is open. Loading/bootstrap observations must never poison recovery.
    if (!multiplayerSession || !WorldBusActive()) {
        if (compositeStateTransitionCarrier)
            gStats.compositeCarrierPreBusPreserved.fetch_add(1U, std::memory_order_relaxed);
        if (!probeDirect) gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 11U);
        invokeRetailFallback();
        return;
    }

    if (nonHostClient && !sourcePlayer && !clientLocalFirstStateCarrier) {
        // Patch 5.9: Host authority controls NETWORK publication, not whether
        // the non-Host process is allowed to run its own retail gameplay.
        // Patch 5.12 adds one proven exception to network suppression: a
        // manifest-certified Composite StateTransition carrier follows the
        // same local-first path already used by the Host.  Other autonomous
        // Client World decisions still remain local-only.
        gStats.authorityAutonomousSuppressed.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 11U);
        invokeRetailFallback();
        return;
    }

    if (!TryConsumeRootRateBudget()) {
        gStats.rootRateRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 11U);
        invokeRetailFallback();
        return;
    }
    if (compositeStateTransitionCarrier)
        gStats.compositeCarrierSubmitAttempt.fetch_add(1U, std::memory_order_relaxed);
    if (!world_interactions::SubmitPublicRoot(ownerCompositeInstance, event)) {
        // Local admission/transport queue rejection is still fail-open to retail.
        // Network/authority rejection after a successful enqueue belongs to the
        // later recovery stage and is intentionally not implemented here.
        gStats.queueRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 11U);
        invokeRetailFallback();
        return;
    }
    gStats.compositeInputRootsSent.fetch_add(1U, std::memory_order_relaxed);
    if (compositeStateTransitionCarrier)
        gStats.compositeCarrierSubmitSuccess.fetch_add(1U, std::memory_order_relaxed);
    gStats.causalRootCommitted.fetch_add(1U, std::memory_order_relaxed);
    gStats.rootsSent.fetch_add(1U, std::memory_order_relaxed);
    if (externalInvocation) gStats.externalRoots.fetch_add(1U, std::memory_order_relaxed);
    if (delay != 0.0) gStats.delayedRoots.fetch_add(1U, std::memory_order_relaxed);
    if (guid == kDiagnosticProbeDirectShortGuid)
        gStats.probeDirectRootsSent.fetch_add(1U, std::memory_order_relaxed);
    if (callbackValue != nullptr) gStats.callbackRoots.fetch_add(1U, std::memory_order_relaxed);
    if (detachedCallback) {
        gStats.callbackBoundaryRoots.fetch_add(1U, std::memory_order_relaxed);
        if (!probeDirect) {
            switch (causalStamp.source) {
            case CausalSource::Player:
                gStats.callbackBoundaryPlayerRoots.fetch_add(1U, std::memory_order_relaxed);
                break;
            case CausalSource::AI:
                gStats.callbackBoundaryAiRoots.fetch_add(1U, std::memory_order_relaxed);
                break;
            case CausalSource::UI:
                gStats.callbackBoundaryUiRoots.fetch_add(1U, std::memory_order_relaxed);
                break;
            case CausalSource::AIMP:
                gStats.callbackBoundaryAimpRoots.fetch_add(1U, std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }
    }
    cause.cause = event.observationId;
    cause.rootKind = static_cast<std::uint8_t>(event.rootKind);
    TraceContext(cause, 1U);
    if (nonHostClient && sourcePlayer && !clientLocalFirstStateCarrier) {
        // SubmitPublicRoot succeeded with AwaitAuthority. It deliberately did
        // not install a local-observation echo slot, so the Host mutation can
        // later execute on this initiating Client. Patch 5.12 state carriers
        // are explicitly local-first and therefore continue into retail below.
        gStats.authorityAwaitSubmitted.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityLocalSuppressed.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 12U);
        return;
    }
    if (workExecuteShape) StoreTaskCause(const_cast<std::uint32_t*>(input), cause, epoch);
    if (gFrame != nullptr && gFrame->epoch == epoch && gFrame->item == reinterpret_cast<std::uintptr_t>(input))
        gFrame->cause = cause;
    // Coverage belongs to this invocation, not to the current WorkItem or its
    // shared parent. Later sibling outputs remain candidates in the same cause.
    origin = Origin::LocalRoot;
    InvokePublicScoped(original, origin, epoch, self, instance, input, callback, delay, tail, &cause);
}

void RetainCausalScope(const CausalStamp stamp, std::uint32_t epoch) noexcept {
    std::lock_guard lock(gLineageMutex);
    if (epoch == gEpoch.load(std::memory_order_relaxed))
        static_cast<void>(gCausalGroups.AddRef(stamp.chainId));
}
void ReleaseCausalScope(const CausalStamp stamp, std::uint32_t epoch) noexcept {
    std::lock_guard lock(gLineageMutex);
    if (epoch == gEpoch.load(std::memory_order_relaxed) && gCausalGroups.ReleaseRef(stamp.chainId)) {
        gStats.causalChainsReleased.fetch_add(1U, std::memory_order_relaxed);
        gLiveCausalGroups.store(gCausalGroups.Size(), std::memory_order_release);
    }
}

void ObserveLocalEmittedOutput(
    void** instance, const std::uint32_t* input) noexcept {
    const auto observer = gLocalOutputObserver.load(std::memory_order_acquire);
    if (observer == nullptr || instance == nullptr || input == nullptr) return;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        auto* sharedRef = *instance;
        if (sharedRef == nullptr) return;
        const auto output = *input;
        if (output == 0U) return;
        auto* execution = *reinterpret_cast<void**>(
            static_cast<std::byte*>(sharedRef) + 0x0CU);
        if (execution == nullptr) return;
        observer(sharedRef, execution, output);
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
}

void OutputCapture(void* self, void** instance, const std::uint32_t* input,
    void** callback, double delay, std::uint32_t tail) {
    using namespace world_interactions;
    const auto original = reinterpret_cast<PublicFn>(gOutputTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return;
    ObserveLocalEmittedOutput(instance, input);
    // MissionSync owns only graph-compiled mission ingress ports. Its commits
    // replay the complete native fan-out under RemoteRoot, so WorldSync does
    // not also publish every consequence of the same mission decision.
    // Observe the existing lineage but never change its gameplay meaning:
    // a mission completion may currently be born inside WorldSync LocalRoot.
    const auto missionOrigin = CurrentOrigin();
    mission_sync::ObserveOutputAttempt(
        missionOrigin == Origin::LocalRoot, missionOrigin == Origin::RemoteRoot);
    if (missionOrigin == Origin::LocalRoot || missionOrigin == Origin::RemoteRoot) {
        void* scopedExecution{}; void* scopedCallback{};
        std::uint32_t scopedPort{}; bool scopedPayloadless{};
        if (ReadPublicArguments(instance, input, callback, scopedExecution,
                scopedPort, scopedCallback, scopedPayloadless))
            mission_sync::ProbeScopedOutput(
                scopedExecution, scopedPort, missionOrigin == Origin::LocalRoot);
    }
    // A mission completion often fires *inside* a WorldSync LocalRoot. The
    // previous Origin::None-only gate missed those proven frontiers entirely.
    // Capture a local mission decision once, then replay its full CATHODE
    // fan-out from the Host-ordered mission commit. A WorldSync RemoteRoot can
    // naturally reach the same frontier first; suppress only that exact
    // re-emission so it cannot double-execute the mission before its commit.
    // MissionSync::Apply has its own explicit scope to exempt the authoritative
    // replay from this interception.
    const bool worldRemoteMission = missionOrigin == Origin::RemoteRoot;
    const bool localMission = (missionOrigin == Origin::None ||
        missionOrigin == Origin::LocalRoot) && !replication_core::RemoteApplyActive();
    if (!mission_sync::ReplayActive() && CurrentRootKind() != kMissionRootKind
        && (localMission || worldRemoteMission)) {
        void* missionExecution{}; void* missionCallback{};
        std::uint32_t missionPort{}; bool payloadless{}; bool payloadTolerant{};
        if (ReadPublicArguments(instance, input, callback, missionExecution, missionPort,
                missionCallback, payloadless) &&
            TaskCause(missionCallback, gEpoch.load(std::memory_order_relaxed)).rootKind != kMissionRootKind &&
            mission_sync::Candidate(missionExecution, missionPort, payloadTolerant)) {
            // Previous builds captured a VariableBool output with native typed
            // data and replayed it with an EMPTY callback. Both peers returned
            // MISSION_SYNC_APPLIED, but no objective state was transferred.
            // A CATHODE data reference is not a portable bool/value. Do not
            // suppress the original fan-out or issue a false mission commit
            // unless the *observed* native output is actually data-free.
            // Exception: a Composite/Proxy interface pin (e.g. a generator's
            // `complete`) whose compiled targets are all control inputs. Its
            // data reference is a pass-through no receiver reads.
            if ((!payloadless && !payloadTolerant) || delay != 0.0 || tail != 0U) {
                mission_sync::NoteUnsafeShape(!payloadless, delay != 0.0, tail != 0U);
            } else {
                bool captured = false;
                for (std::uint8_t selector = 1U; selector <= 2U; ++selector) {
                    void* definition{};
                    if (OutputDefinition(missionExecution, selector, definition) && definition == self) {
                        // A Client's world transaction replayed on the Host can
                        // reach a frontier the Client itself suppressed as an
                        // autonomous decision (e.g. a cinematic's completion):
                        // nobody would ever commit it (black screen at the
                        // second BSP_TORRENS cinematic, build 493). The Host is
                        // the authority: it captures that output. A Client
                        // still leaves the fan-out to the Host's commit.
                        if (worldRemoteMission && !mission_sync::HostRole()) return;
                        captured = mission_sync::Capture(missionExecution, missionPort, selector,
                            CurrentCausalStamp(gEpoch.load(std::memory_order_relaxed)).source == CausalSource::Player);
                        if (captured) mission_sync::NoteCapturedShape(!payloadless, false, false);
                        break;
                    }
                }
                if (captured) return;
                mission_sync::Unsupported();
            }
        }
    }
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        original(self, instance, input, callback, delay, tail);
        return;
    }
    if (!gInstalled.load(std::memory_order_acquire)) {
        original(self, instance, input, callback, delay, tail);
        return;
    }
    auto* registry = gRegistry.load(std::memory_order_acquire);
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    const auto origin = CurrentOrigin();
    bool eventSidecar = false;
    if (origin == Origin::LocalRoot && registry != nullptr) {
        void* candidate{}; void* candidateParent{}; std::uint32_t port{}; bool payloadless{};
        if (ReadPublicArguments(instance, input, callback, candidate, port, candidateParent, payloadless)) {
            const auto id = registry->IdentifyRuntimeEntity(candidate);
            const auto profile = id ? registry->FindEmittedOutputCausalProfile(*id, port) : std::nullopt;
            eventSidecar = profile && profile->CertifiedEmittedEventCarrierBoundary() &&
                !CurrentRootOwnsEvent(profile->semanticTargetEntity, profile->semanticOperation);
        }
    }
    if ((origin != Origin::None && !eventSidecar) || replication_core::RemoteApplyActive() || !Enabled() ||
        registry == nullptr || !registry->RuntimeCaptureReady()) {
        original(self, instance, input, callback, delay, tail);
        return;
    }
    void* execution{}; void* parent{}; std::uint32_t output{}; bool ignoredPayloadless{};
    if (!ReadPublicArguments(instance, input, callback, execution, output, parent, ignoredPayloadless)) {
        original(self, instance, input, callback, delay, tail);
        return;
    }
    auto stamp = CurrentCausalStamp(epoch);
    const auto binding = ParentContext(parent, epoch);
    if (!stamp.Valid()) stamp = binding.causalStamp;
    if (!stamp.Valid()) stamp = FindCausalStamp(parent, epoch);
    bool newStamp = false;
    if (!stamp.Valid()) {
        const auto source = IdentifySourceCausality(*registry, instance);
        stamp = NewCausalStamp(source, epoch);
        newStamp = stamp.Valid();
        if (newStamp) RetainCausalScope(stamp, epoch);
        if (stamp.Valid() && parent != nullptr)
            BindParentContext(parent, Origin::None, stamp, ContextTrace{}, epoch);
    }
    const auto finishLocal = [&]() {
        InvokeEmissionScoped(original, stamp, epoch, self, instance, input, callback, delay, tail);
        if (newStamp) ReleaseCausalScope(stamp, epoch);
    };
    // Preserve ObserveLocalEmittedOutput AND causal seeding/scope above.
    // Those may be needed by a descendant after the World bus opens. Only the
    // expensive network candidate classification is skipped while offline.
    if (!SessionAuthorityBound() || !WorldBusActive()) {
        finishLocal();
        return;
    }
    const auto owner = registry->IdentifyRuntimeEntity(execution);
    if (!owner || !stamp.Valid() || !std::isfinite(delay) || delay < 0.0 || delay > 600.0 || tail != 0U) {
        finishLocal(); return;
    }
    const auto profile = registry->FindEmittedOutputCausalProfile(*owner, output);
    if (!profile) {
        gStats.causalBoundaryProfileMissing.fetch_add(1U, std::memory_order_relaxed);
        finishLocal(); return;
    }
    if (registry->FindWorldEntityDomain(*owner) != WorldDomain::World) {
        finishLocal(); return;
    }
    RecordRelevanceCandidate(*registry, *owner, output, PublicRootKind::EmittedOutput, *profile);
    const bool stateTransitionCarrier = profile->CertifiedStateTransitionCarrierBoundary();
    if (stateTransitionCarrier)
        gStats.carrierObserved.fetch_add(1U, std::memory_order_relaxed);
    if (!profile->Has(WorldCausalDependencyWorldEffect))
        gStats.causalBoundaryNoWorldEffect.fetch_add(1U, std::memory_order_relaxed);
    if (profile->Has(WorldCausalDependencyPlayer))
        gStats.causalBoundaryPlayerDependent.fetch_add(1U, std::memory_order_relaxed);
    if (profile->Has(WorldCausalDependencyUnknown))
        gStats.causalBoundaryUnknownDependency.fetch_add(1U, std::memory_order_relaxed);
    if (profile->Has(WorldCausalDependencyTruncated))
        gStats.causalBoundaryTruncated.fetch_add(1U, std::memory_order_relaxed);
    if (profile->Has(WorldCausalDependencyBranching))
        gStats.causalBoundaryBranching.fetch_add(1U, std::memory_order_relaxed);
    // Patch 5.3 moves Event authority to the earlier CATHODE scheduler edge.
    // Never publish an Event again from EmittedOutput; the local retail path
    // remains untouched and a SchedulerEdge ROOT owns the network copy.
    const bool eventCarrier = profile->CertifiedEmittedEventCarrierBoundary();
    if (profile->replicationContractKind == WorldReplicationContractKind::Event && !eventCarrier) {
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        finishLocal();
        return;
    }
    if (!profile->NetworkReplicableBoundary()) {
        if (profile->Has(WorldCausalDependencyWorldEffect))
            gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        finishLocal(); return;
    }
    if (stateTransitionCarrier)
        gStats.carrierNetworkEligible.fetch_add(1U, std::memory_order_relaxed);
    const auto finishCandidateLocal = [&]() {
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        finishLocal();
    };
    // The exact WM08 output profile contains the explicit emitted-port edges,
    // not 008BE0D0's runtime +11C -> +70 -> +394 forwarding association. Only
    // claim/suppress this outer invocation when the retail +11C implementation
    // proves that implicit leg cannot execute for this Entity right now.
    if (!EmittedOutputProfileCompleteAtRuntime(execution)) {
        gStats.emittedOutputForwardingRejected.fetch_add(1U, std::memory_order_relaxed);
        finishCandidateLocal(); return;
    }
    if (stateTransitionCarrier)
        gStats.carrierForwardingComplete.fetch_add(1U, std::memory_order_relaxed);
    CompositePublicInputEvent event{};
    event.retailRoot = true;
    event.rootKind = PublicRootKind::EmittedOutput;
    void* checkedDefinition{};
    for (std::uint8_t selector = 1U; selector <= 2U; ++selector) {
        if (OutputDefinition(execution, selector, checkedDefinition) && checkedDefinition == self) {
            event.interfaceSelector = selector; break;
        }
    }
    if (event.interfaceSelector == 0U) {
        if (stateTransitionCarrier)
            gStats.carrierSelectorRejected.fetch_add(1U, std::memory_order_relaxed);
        finishCandidateLocal(); return;
    }
    if (stateTransitionCarrier)
        gStats.carrierSelectorResolved.fetch_add(1U, std::memory_order_relaxed);
    event.publicInputShortGuid = output;
    event.targetEntity = *owner;
    event.relativeDelaySeconds = delay;
    event.flags = parent != nullptr ? kPublicRootDetachedCallback : kPublicRootExternalInvocation;
    event.causalSerial = stamp.chainId;
    event.segmentSerial = stamp.chainId;
    // Keep Events outside state-reconciliation transactions, just as the
    // scheduler Event boundary does. Submit assigns a fresh observation cause.
    if (eventCarrier) event.causalSerial = event.segmentSerial = 0U;
    auto cause = gFrame != nullptr && gFrame->epoch == epoch ? gFrame->cause : TaskCause(parent, epoch);
    event.actorKind = stamp.source == CausalSource::Player ? PublicActorKind::SourcePlayer : PublicActorKind::Authority;
    cause.actorKind = static_cast<std::uint8_t>(event.actorKind);
    cause.actor = {};
    cause.owner = cause.target = *owner;
    cause.input = output;
    cause.epoch = epoch;
    cause.causalChainId = stamp.chainId;
    cause.causalSource = static_cast<std::uint8_t>(stamp.source);
    cause.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
    cause.sourcePeer = gLocalPeer.load(std::memory_order_acquire);
    if (!CaptureContext(*registry, parent, event, cause)) {
        gStats.contextUnsupported.fetch_add(1U, std::memory_order_relaxed);
        finishCandidateLocal(); return;
    }
    if (stateTransitionCarrier)
        gStats.carrierContextCaptured.fetch_add(1U, std::memory_order_relaxed);
    const bool multiplayerSession = SessionAuthorityBound();
    const bool localAuthorityHost = SessionAuthorityHost();
    const bool nonHostClient = multiplayerSession && !localAuthorityHost;

    // Patch 5.12 mirrors the Host's certified EmittedOutput StateTransition
    // carrier path on the Client as well.  This remains local-first: the exact
    // Host-proven carrier is published, retail executes locally, and the Host
    // sequences the authoritative broadcast.  No Door-specific semantic call
    // is synthesized.
    const bool clientLocalFirstStateCarrier = nonHostClient && stateTransitionCarrier;
    const bool sourcePlayer = stamp.source == CausalSource::Player &&
        event.actorKind == PublicActorKind::SourcePlayer;
    if (nonHostClient && sourcePlayer && !clientLocalFirstStateCarrier)
        event.flags |= kPublicRootFlagAwaitAuthority;

    // Step 1's forwarding-completeness gate has already run. Only now may
    // authority policy act, after the final event also passes the common
    // wire/manifest admission contract.
    if (!Initialized() || !world_actions::PublicRootManifestAdmissionAllowed(*registry, *owner, event)) {
        finishCandidateLocal(); return;
    }
    if (stateTransitionCarrier)
        gStats.carrierManifestAdmitted.fetch_add(1U, std::memory_order_relaxed);
    if (!multiplayerSession || !WorldBusActive()) {
        if (stateTransitionCarrier)
            gStats.carrierPreBusPreserved.fetch_add(1U, std::memory_order_relaxed);
        finishCandidateLocal(); return;
    }
    if (nonHostClient && !sourcePlayer && !clientLocalFirstStateCarrier) {
        // Patch 5.9: autonomous Client outputs remain non-authoritative on the
        // wire, but the originating process must still execute retail locally.
        // Patch 5.12 permits only the already-certified StateTransition carrier
        // class to mirror the Host publication path.
        gStats.authorityAutonomousSuppressed.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 11U);
        finishCandidateLocal();
        return;
    }
    if (stateTransitionCarrier)
        gStats.carrierSubmitAttempt.fetch_add(1U, std::memory_order_relaxed);
    if (!SubmitPublicRoot(*owner, event)) {
        gStats.queueRejected.fetch_add(1U, std::memory_order_relaxed);
        finishCandidateLocal(); return;
    }
    if (stateTransitionCarrier)
        gStats.carrierSubmitSuccess.fetch_add(1U, std::memory_order_relaxed);
    cause.cause = event.observationId;
    cause.rootKind = static_cast<std::uint8_t>(event.rootKind);
    cause.contextKind = static_cast<std::uint8_t>(event.contextKind);
    gStats.emittedOutputRootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.rootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.causalRootCommitted.fetch_add(1U, std::memory_order_relaxed);
    TraceContext(cause, 1U);
    if (nonHostClient && sourcePlayer && !clientLocalFirstStateCarrier) {
        gStats.authorityAwaitSubmitted.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityLocalSuppressed.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 12U);
        if (newStamp) ReleaseCausalScope(stamp, epoch);
        return;
    }
    // Suppress only work actually born under this published output. The shared
    // parent and the current input may still emit other independent outputs.
    const auto savedStamp = gEmissionStamp;
    const auto savedEpoch = gEmissionEpoch;
    gEmissionStamp = stamp; gEmissionEpoch = epoch;
    InvokePublicScoped(original, Origin::LocalRoot, epoch, self, instance, input, callback, delay, tail, &cause);
    gEmissionStamp = savedStamp; gEmissionEpoch = savedEpoch;
    if (newStamp) ReleaseCausalScope(stamp, epoch);
}

void __fastcall PublicDetour(void* self, void*, void** instance, const std::uint32_t* input,
    void** callback, double delay, std::uint32_t tail) {
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        const auto original = reinterpret_cast<PublicFn>(
            gPublicTrampoline.load(std::memory_order_acquire));
        if (original != nullptr) {
            original(self, instance, input, callback, delay, tail);
        }
        return;
    }
    replication_core::BeginCausalTurn();
#if defined(_MSC_VER) && defined(_M_IX86)
    __try { PublicCapture(self, instance, input, callback, delay, tail); }
    __finally { replication_core::EndCausalTurn(); }
#else
    PublicCapture(self, instance, input, callback, delay, tail);
    replication_core::EndCausalTurn();
#endif
}

void __fastcall OutputDetour(void* self, void*, void** instance, const std::uint32_t* input,
    void** callback, double delay, std::uint32_t tail) {
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        OutputCapture(self, instance, input, callback, delay, tail);
        return;
    }
    // Patch 115.5 may install only this certified boundary before WorldSync is
    // armed. In that bootstrap state the detour is observation-only and must
    // not enter replication causal-turn machinery.
    if (!gInstalled.load(std::memory_order_acquire)) {
        OutputCapture(self, instance, input, callback, delay, tail);
        return;
    }
    replication_core::BeginCausalTurn();
#if defined(_MSC_VER) && defined(_M_IX86)
    __try { OutputCapture(self, instance, input, callback, delay, tail); }
    __finally { replication_core::EndCausalTurn(); }
#else
    OutputCapture(self, instance, input, callback, delay, tail);
    replication_core::EndCausalTurn();
#endif
}

// 0092CE20 drains scheduler work under its own retail critical section and
// restores manager time before returning (ret 4). Seal only on outer unwind;
// delayed callbacks keep the cause id and open a later, explicitly sealed segment.
void __fastcall SchedulerDrainDetour(void* manager, void*, std::uint32_t mode) {
    using DrainFn = void (__thiscall*)(void*, std::uint32_t);
    const auto original = reinterpret_cast<DrainFn>(gDrainTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        original(manager, mode);
        return;
    }
    replication_core::BeginCausalTurn();
#if defined(_MSC_VER) && defined(_M_IX86)
    __try { original(manager, mode); }
    __finally { replication_core::EndCausalTurn(); }
#else
    original(manager, mode);
    replication_core::EndCausalTurn();
#endif
}

void* __fastcall WorkBuildDetour(void* self, void*, void** source,
    const std::uint32_t* input, void* target, std::uint32_t flags, double time,
    void** parent, void** payload) {
    const auto original = reinterpret_cast<WorkBuildFn>(gBuildTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return self;
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        return original(self, source, input, target, flags, time, parent, payload);
    }
    Forget(self); // Every allocation/reuse boundary, including readiness gaps.
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr || !registry->RuntimeCaptureReady())
        return original(self, source, input, target, flags, time, parent, payload);
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    // Capture the initiating scope before calling retail. Helpers called by
    // the constructor must not retroactively turn their parent into a ROOT.
    void* retainedParent = ReadWorkHolder(parent);
    const auto sourceKind = IdentifySourceCausality(*registry, source);
    const bool independentPlayer = retainedParent == nullptr &&
        sourceKind == CausalSource::Player && gPublicOrigin == Origin::None &&
        !(gEmissionEpoch == epoch && gEmissionStamp.Valid());
    const auto parentContext = ParentContext(retainedParent, epoch);
    auto origin = FindOrigin(retainedParent, epoch);
    if (origin == Origin::None && gPublicEpoch == epoch && gPublicOrigin != Origin::None)
        origin = gPublicOrigin; // direct fan-out of the scoped native public call
    if (origin == Origin::None && retainedParent == nullptr && !independentPlayer)
        origin = CurrentOrigin();

    // 0.11.4.74: the retained callback itself is not tagged as a ROOT. Instead
    // its invocation-qualified parent binding propagates the already selected
    // origin/causal transaction to delayed and sibling WorkItems. This closes
    // the gap where retail builds a child from the shared callback after the
    // stack-local ROOT scope has already returned.
    CausalStamp causalStamp{};
    auto cause = TaskCause(retainedParent, epoch);
    MergeParentContext(parentContext, origin, causalStamp, cause);
    if (cause.invocation == 0U && gPublicEpoch == epoch && gPublicOrigin != Origin::None)
        cause = gPublicCause;
    if (retainedParent == nullptr && !independentPlayer && cause.invocation == 0U &&
        gFrame != nullptr && gFrame->epoch == epoch)
        cause = gFrame->cause;

    bool directCausalSeed = false;
    if (!causalStamp.Valid())
        causalStamp = FindCausalStamp(retainedParent, epoch);
    if (origin == Origin::None) {
        if (!causalStamp.Valid() && !independentPlayer)
            causalStamp = CurrentCausalStamp(epoch);
        if (!causalStamp.Valid()) {
            const auto seedSource = independentPlayer ? sourceKind : IdentifySourceCausality(*registry, source);
            if (seedSource != CausalSource::World && IsLocalCausalSource(seedSource)) {
                causalStamp = NewCausalStamp(seedSource, epoch);
                directCausalSeed = causalStamp.Valid();
            }
        }
    }
    const auto parentInvocation = cause.invocation;
    if (cause.cause == 0U && cause.actorKind == 0U) IdentifyActor(*registry, source, cause);
    cause.parentInvocation = parentInvocation;
    cause.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
    cause.epoch = epoch;
    cause.causalChainId = causalStamp.chainId;
    cause.causalSource = static_cast<std::uint8_t>(causalStamp.source);
    static_cast<void>(ReadPublicInput(input, cause.input));
    void* parentExecution{};
    static_cast<void>(ReadContextShape(ReadWorkHolder(parent), cause, parentExecution));
    if (retainedParent != nullptr && (origin != Origin::None || causalStamp.Valid() || cause.cause != 0U))
        BindParentContext(retainedParent, Origin::None, causalStamp, ContextTrace{}, epoch);
    void* result = original(self, source, input, target, flags, time, parent, payload);
    if (result != nullptr && epoch == gEpoch.load(std::memory_order_relaxed)) {
        StoreTaskCause(result, cause, epoch);
        cause.item = reinterpret_cast<std::uintptr_t>(result);
        if (cause.cause != 0U) TraceContext(cause, 5U);
        if (origin != Origin::None) {
            Tag(result, origin, epoch); // Tag this new child, never its parent.
            if (causalStamp.Valid())
                TagCausalStamp(result, causalStamp, epoch, false);
            gStats.lineageBuildInherit.fetch_add(1U, std::memory_order_relaxed);
        } else if (causalStamp.Valid()) {
            TagCausalStamp(result, causalStamp, epoch, directCausalSeed);
        }
    } else if (directCausalSeed) {
        DiscardUnreferencedCausalStamp(causalStamp, epoch);
    }
    return result;
}

// ScheduledInput admission is tri-state: retail-local, published+local, or
// authority-taken/suppressed. No native address enters the event.
std::uint8_t CaptureScheduledInput(WorldEntityRegistry& registry, void* manager,
    void** target, void** owner, void* item, CausalStamp& stamp, ContextTrace& cause,
    std::uint32_t epoch) {
    using namespace world_interactions;
    // Return 0: WS3 rejected this candidate and retail must run.
    // Return 1: ROOT submitted by Host/local authority and retail runs under LocalRoot.
    // Return 2: authority has taken responsibility and this local consumer is suppressed.
    // WorkExecuteDetour has already restored/tagged the retained lineage.
    // With a valid stamp and a closed bus, candidate building cannot change
    // that lineage or send a ROOT. Return 0, so the caller executes retail.
    // Do NOT take this shortcut for an unseeded item: the existing certified
    // path below may seed a World cause needed by a later descendant.
    if (stamp.Valid() && (!SessionAuthorityBound() || !WorldBusActive()))
        return 0U;

    // manager+0x10A is a retail debugger/assertion latch, not gameplay state.
    // A Host invocation observed while that latch is raised cannot be recreated
    // portably on another process, so do not declare it a supported ROOT.
    if (!ScheduledConsumerDebugLatchClear(manager) || target == nullptr || target != owner) {
        gStats.scheduledInputCertificationRejected.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }
    void* targetObject = ExecutionFromShared(ReadWorkHolder(target));
    void* ownerObject = ExecutionFromShared(ReadWorkHolder(owner));
    const auto targetId = registry.IdentifyRuntimeEntity(targetObject);
    const auto ownerId = registry.IdentifyRuntimeEntity(ownerObject);
    // STATIC CONFIRMED: every retail FUN_00926A60 -> FUN_00926680 call uses
    // the same holder for both consumer arguments, including the special
    // fan-out path. A mismatched pair is therefore outside the certified
    // ScheduledInput capture shape and must not become a replayable ROOT.
    if (!targetId || !ownerId || !(*targetId == *ownerId) ||
        registry.FindWorldEntityDomain(*targetId) != WorldDomain::World ||
        registry.FindWorldEntityDomain(*ownerId) != WorldDomain::World) {
        gStats.scheduledInputCertificationRejected.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }
    const auto profile = registry.FindScheduledInputCausalProfile(*targetId, cause.input);
    if (!profile) {
        gStats.causalBoundaryProfileMissing.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }
    RecordRelevanceCandidate(registry, *targetId, cause.input, PublicRootKind::ScheduledInput, *profile);
    if (!profile->Has(WorldCausalDependencyWorldEffect))
        gStats.causalBoundaryNoWorldEffect.fetch_add(1U, std::memory_order_relaxed);
    if (profile->Has(WorldCausalDependencyPlayer))
        gStats.causalBoundaryPlayerDependent.fetch_add(1U, std::memory_order_relaxed);
    if (profile->Has(WorldCausalDependencyUnknown))
        gStats.causalBoundaryUnknownDependency.fetch_add(1U, std::memory_order_relaxed);
    if (profile->Has(WorldCausalDependencyTruncated))
        gStats.causalBoundaryTruncated.fetch_add(1U, std::memory_order_relaxed);
    if (profile->Has(WorldCausalDependencyBranching))
        gStats.causalBoundaryBranching.fetch_add(1U, std::memory_order_relaxed);

    // Patch 5.3: Event replay belongs to the earlier QueueInput/SchedulerEdge
    // boundary. The retail consumer still executes locally, but it must never
    // become a second low-level network ROOT. This specifically removes the
    // PlayEnvironmentAnimation retained-consumer path that faulted after replay.
    if (profile->replicationContractKind == WorldReplicationContractKind::Event) {
        gStats.scheduledEventRootsSuppressed.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }

    // Patch 4.1 carrier ownership: an exact StateTransition ScheduledInput
    // whose semantic sink is already owned by a certified higher carrier must
    // never become a fallback network ROOT. Preserve retail locally so CATHODE
    // can continue naturally; only the network takeover is suppressed. This
    // is manifest-derived and does not disable ScheduledInput globally.
    const bool exactStateTransitionIntrinsic =
        profile->replicationContractKind == WorldReplicationContractKind::StateTransition &&
        profile->HasSemanticContract() &&
        profile->CertifiedExactIntrinsicBoundary();
    const bool coveredByCarrier = exactStateTransitionIntrinsic &&
        registry.HasCertifiedStateTransitionCarrier(
            profile->semanticTargetEntity, profile->semanticOperation);
    if (coveredByCarrier) {
        gStats.scheduledCoveredByCarrier.fetch_add(1U, std::memory_order_relaxed);
        gStats.scheduledCoveredByCarrierSuppressed.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }

    // Patch 4.2 quarantine: Patch 3 proved that an intrinsic ScheduledInput
    // StateTransition is not a safe remote apply boundary when its transition
    // consequences cannot be proven. If no certified higher carrier owns the
    // same semantic sink, do not publish this low boundary from the Host. The
    // local retail call is deliberately preserved (return 0), so this changes
    // only network authority admission and never writes/forces CATHODE state.
    // Keep non-Host proposal semantics unchanged; this quarantine is an
    // authority-Host policy for autonomous world ROOT publication.
    const bool authorityHost = SessionAuthorityBound() && SessionAuthorityHost();
    if (exactStateTransitionIntrinsic && !coveredByCarrier && authorityHost) {
        gStats.scheduledStateTransitionNoCarrier.fetch_add(1U, std::memory_order_relaxed);
        gStats.scheduledStateTransitionQuarantinedNoCarrier.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }

    // ScheduledInput is not globally disabled. WM08 merely prevents takeover
    // for scheduler/internal/deferred/unknown consumers. Those consumers return
    // 0 so WorkExecuteDetour invokes retail 00926680 locally and preserves the
    // causal lineage for a more specific descendant. Only Replicate* can ever
    // reach the Client suppression path below.
    if (!profile->NetworkReplicableBoundary()) {
        if (profile->Has(WorldCausalDependencyWorldEffect))
            gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }
    const auto rejectCandidateLocal = [&]() noexcept -> std::uint8_t {
        gStats.scheduledInputCertificationRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    };
    CompositePublicInputEvent event{};
    event.retailRoot = true;
    event.rootKind = PublicRootKind::ScheduledInput;
    event.publicInputShortGuid = cause.input;
    event.targetEntity = *targetId;
    event.flags = kPublicRootDetachedCallback;
    if (!CaptureContext(registry, item, event, cause)) return rejectCandidateLocal();
    if (!stamp.Valid()) {
        stamp = NewCausalStamp(CausalSource::World, epoch);
        if (!stamp.Valid()) return rejectCandidateLocal();
        TagCausalStamp(item, stamp, epoch, true);
        if (void* parent = ReadRetainedParent(item))
            BindParentContext(parent, Origin::None, stamp, ContextTrace{}, epoch);
    }
    event.causalSerial = event.segmentSerial = stamp.chainId;
    event.actorKind = stamp.source == CausalSource::Player ? PublicActorKind::SourcePlayer : PublicActorKind::Authority;
    const bool multiplayerSession = SessionAuthorityBound();
    const bool localAuthorityHost = SessionAuthorityHost();
    const bool nonHostClient = multiplayerSession && !localAuthorityHost;
    const bool sourcePlayer = stamp.source == CausalSource::Player &&
        event.actorKind == PublicActorKind::SourcePlayer;
    if (nonHostClient && sourcePlayer) event.flags |= kPublicRootFlagAwaitAuthority;

    // Step 2's latch/holder/context certification has completed. Re-check the
    // final wire/manifest contract before deciding whether authority may suppress.
    if (!Initialized() || !world_actions::PublicRootManifestAdmissionAllowed(registry, *ownerId, event))
        return rejectCandidateLocal();

    if (!multiplayerSession || !WorldBusActive())
        return rejectCandidateLocal();

    if (nonHostClient && !sourcePlayer) {
        // Patch 5.9: keep the autonomous ScheduledInput off the Client->Host
        // authority channel, but return 0 so WorkExecuteDetour runs the proven
        // retail consumer locally. Network authority must never veto local
        // trigger/interaction progression.
        gStats.authorityAutonomousSuppressed.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 11U);
        return 0U;
    }

    if (!SubmitPublicRoot(*ownerId, event)) {
        gStats.queueRejected.fetch_add(1U, std::memory_order_relaxed);
        return rejectCandidateLocal();
    }
    cause.cause = event.observationId;
    cause.rootKind = static_cast<std::uint8_t>(event.rootKind);
    cause.owner = *ownerId; cause.target = *targetId;
    cause.actorKind = static_cast<std::uint8_t>(event.actorKind); cause.actor = {};
    cause.causalChainId = stamp.chainId;
    cause.causalSource = static_cast<std::uint8_t>(stamp.source);
    cause.contextKind = static_cast<std::uint8_t>(event.contextKind);
    gStats.rootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.scheduledInputRootsSent.fetch_add(1U, std::memory_order_relaxed);
    if (nonHostClient && sourcePlayer) {
        gStats.authorityAwaitSubmitted.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityLocalSuppressed.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 12U);
        return 2U;
    }
    return 1U;
}

// Generic mission completion boundary for every map: the trigger of a retail
// level-exit primitive (GoToFrontend/SwitchLevel) is consumed here. In an AIMP
// gameplay session it is suppressed and reported; the Host returns everyone to
// the lobby once all participants are finished or dead.
bool LevelExitSuppressed(WorldEntityRegistry& registry, void** target, void* item) noexcept {
    if (!player_lifecycle::Installed() || !SessionAuthorityBound()) return false;
    std::uint32_t input{};
    if (!ReadPublicInput(static_cast<const std::uint32_t*>(item), input) ||
        input != player_lifecycle::kTriggerInput) return false;
    const auto id = registry.IdentifyRuntimeEntity(ExecutionFromShared(ReadWorkHolder(target)));
    if (!id) return false;
    if (registry.IsPlayerMissionCompleteEndpoint(*id)) {
        // This is an observation of a real executed CATHODE trigger, not a
        // Flash callback. A remote replay must never finish this local player.
        const auto epoch = gEpoch.load(std::memory_order_relaxed);
        const auto parent = ParentContext(ReadRetainedParent(item), epoch);
        auto origin = FindOrigin(item, epoch);
        CausalStamp stamp{};
        auto cause = TaskCause(item, epoch);
        MergeParentContext(parent, origin, stamp, cause);
        if (origin != Origin::RemoteRoot && CurrentOrigin() != Origin::RemoteRoot &&
            !replication_core::RemoteApplyActive() && !mission_sync::ReplayActive())
            player_lifecycle::ObserveMissionComplete();
        return false; // Keep the retail timer, score and camera consequences.
    }
    const auto primitive = registry.FindWorldEntityPrimitiveType(*id);
    if (primitive && *primitive == player_lifecycle::kSwitchLevelPrimitive
        && mission_sync::CampaignActive()) {
        const auto epoch = gEpoch.load(std::memory_order_relaxed);
        const auto cause = TaskCause(item, epoch);
        // The same authored transition runs on every participant. Preserve the
        // game's destination/checkpoint/spawn logic and keep the lobby alive.
        if (mission_sync::ReplayActive() || CurrentRootKind() == kMissionRootKind
            || cause.rootKind == kMissionRootKind) return false;
        bool ignored{};
        auto* execution = ExecutionFromShared(ReadWorkHolder(target));
        if (mission_sync::Candidate(execution, input, ignored, 4U)
            && mission_sync::Capture(execution, input, 4U)) return true;
    }
    return primitive && player_lifecycle::LevelExitPrimitive(*primitive) &&
        player_lifecycle::DecideLevelExit(*primitive);
}

// MissionSync scheduled commits (Checkpoint/SwitchLevel/GoToFrontend) are
// queued through the retail scheduler. Retail builds and delivers the WorkItem
// later; this engine-thread ledger exempts exactly that delivery from being
// captured again even if its lineage was not recorded.
struct PendingMissionSchedule { void* execution; std::uint32_t port; std::uint64_t epoch; };
std::array<PendingMissionSchedule, 16U> gPendingMissionSchedules{};

void AddPendingMissionSchedule(void* execution, const std::uint32_t port) noexcept {
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    for (auto& entry : gPendingMissionSchedules) {
        if (entry.execution == nullptr || entry.epoch != epoch) {
            entry = {execution, port, epoch};
            return;
        }
    }
}

bool ConsumePendingMissionSchedule(void* execution, const std::uint32_t port) noexcept {
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    for (auto& entry : gPendingMissionSchedules) {
        if (entry.execution == execution && entry.port == port && entry.epoch == epoch) {
            entry = {};
            return true;
        }
    }
    return false;
}

std::uint8_t __fastcall WorkExecuteDetour(void* manager, void*, void** target,
                                         void** owner, void** itemHolder) {
    const auto original = reinterpret_cast<WorkExecuteFn>(gExecuteTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    // Retail dereferences this holder immediately (009266BE/00926705).
    void* item = itemHolder == nullptr ? nullptr : *itemHolder;
    auto* registry = gRegistry.load(std::memory_order_acquire);
    // Spectate: keep the death fade/DOF off a dead player's screen while a
    // teammate is alive to be watched (PlayerLifecycle). The manifest function
    // type is looked up only while the local player is down.
    {
        std::uint32_t method{}, entity{}, type{}, primitive{};
        auto* execution = ExecutionFromShared(ReadWorkHolder(target));
        if (player_lifecycle::Installed() &&
            ReadPublicInput(static_cast<const std::uint32_t*>(item), method) &&
            ReadExecutionIdentity(execution, entity, type)) {
            if (player_lifecycle::LocalPlayerDown() && registry != nullptr && registry->RuntimeCaptureReady())
                if (const auto id = registry->IdentifyRuntimeEntity(execution))
                    primitive = registry->FindWorldEntityPrimitiveType(*id).value_or(0U);
            if (player_lifecycle::SuppressDeathPresentation(method, entity, type, primitive)) return 0U;
        }
    }
    if (registry == nullptr || !registry->RuntimeCaptureReady())
        return original(manager, target, owner, itemHolder);
    // Campaign engine actions (including save stations) are authored once in
    // the manifest. Resolve their scheduled input in each peer's own world;
    // never send a station pointer or invoke the UI's checkpoint update call.
    bool committedReplay = false;
    if (mission_sync::CampaignActive() && SessionAuthorityBound()) {
        const auto campaignEpoch = gEpoch.load(std::memory_order_relaxed);
        auto campaignOrigin = FindOrigin(item, campaignEpoch);
        CausalStamp campaignStamp{};
        auto campaignCause = TaskCause(item, campaignEpoch);
        MergeParentContext(ParentContext(ReadRetainedParent(item), campaignEpoch),
            campaignOrigin, campaignStamp, campaignCause);
        std::uint32_t campaignInput{};
        if (ReadPublicInput(static_cast<const std::uint32_t*>(item), campaignInput)) {
            auto* execution = ExecutionFromShared(ReadWorkHolder(target));
            committedReplay = ConsumePendingMissionSchedule(execution, campaignInput);
            bool tolerant{};
            if (!committedReplay && !mission_sync::ReplayActive()
                && CurrentRootKind() != kMissionRootKind
                && campaignCause.rootKind != kMissionRootKind
                && mission_sync::Candidate(execution, campaignInput, tolerant, 4U)
                && mission_sync::Capture(execution, campaignInput, 4U)) return 0U;
        }
    }
    if (!committedReplay && LevelExitSuppressed(*registry, target, item)) return 0U;
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        return original(manager, target, owner, itemHolder);
    }
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    void* retainedParent = ReadRetainedParent(item);
    const auto parentContext = ParentContext(retainedParent, epoch);
    auto origin = FindOrigin(item, epoch);
    CausalStamp causalStamp{};
    auto cause = TaskCause(item, epoch);
    MergeParentContext(parentContext, origin, causalStamp, cause);
    // Delayed remote descendants execute after the RemoteApply TLS scope has
    // returned. Use restored lineage as well as TLS to protect local controls.
    if (cause.rootKind != kMissionRootKind && CurrentRootKind() != kMissionRootKind
        && (origin == Origin::RemoteRoot || CurrentOrigin() == Origin::RemoteRoot ||
            replication_core::RemoteApplyActive())) {
        const auto targetId = registry->IdentifyRuntimeEntity(
            ExecutionFromShared(ReadWorkHolder(target)));
        const auto primitive = targetId
            ? registry->FindWorldEntityPrimitiveType(*targetId).value_or(0U) : 0U;
        if (SuppressRemotePlayerControl(primitive, true)) return 0U;
    }
    const auto itemStamp = FindCausalStamp(item, epoch);
    if (!causalStamp.Valid()) causalStamp = itemStamp;
    if (origin != Origin::None) {
        // Persist restored/inherited origin on the executing item before retail
        // can retain it for another frame or create children outside this TLS.
        Tag(item, origin, epoch);
        if (causalStamp.Valid() && !itemStamp.Valid())
            TagCausalStamp(item, causalStamp, epoch, false);
        gStats.lineageExecuteRestore.fetch_add(1U, std::memory_order_relaxed);
    }

    if (origin == Origin::None) {
        const bool needsCausalTag = !itemStamp.Valid();
        bool directCausalSeed = false;
        if (!causalStamp.Valid())
            causalStamp = CurrentCausalStamp(epoch);
        if (!causalStamp.Valid()) {
            // 00926680 receives owner/source and target holders separately. If
            // construction-time provenance was unavailable, the executing
            // owner is a second evidence-backed place to seed a local cause.
            const auto seedSource = IdentifySourceCausality(*registry, owner);
            if (seedSource != CausalSource::World && IsLocalCausalSource(seedSource)) {
                causalStamp = NewCausalStamp(seedSource, epoch);
                directCausalSeed = causalStamp.Valid();
            }
        }
        if (needsCausalTag && causalStamp.Valid() && item != nullptr)
            TagCausalStamp(item, causalStamp, epoch, directCausalSeed);
        else if (directCausalSeed && item == nullptr)
            DiscardUnreferencedCausalStamp(causalStamp, epoch);
    }
    if (cause.invocation == 0U) {
        cause.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
        cause.epoch = epoch;
        // An executing owner alone does not establish the gameplay actor.
        StoreTaskCause(item, cause, epoch);
    }
    cause.item = reinterpret_cast<std::uintptr_t>(item);
    cause.causalChainId = causalStamp.chainId;
    cause.causalSource = static_cast<std::uint8_t>(causalStamp.source);
    static_cast<void>(ReadPublicInput(static_cast<const std::uint32_t*>(item), cause.input));
    void* parentExecution{};
    static_cast<void>(ReadContextShape(retainedParent, cause, parentExecution));
    if (retainedParent != nullptr && (origin != Origin::None || causalStamp.Valid() || cause.cause != 0U))
        BindParentContext(retainedParent, Origin::None, causalStamp, ContextTrace{}, epoch);
    if (cause.cause != 0U) TraceContext(cause, 6U);
    if (origin == Origin::None && Enabled()) {
        const auto selected = CaptureScheduledInput(
            *registry, manager, target, owner, item, causalStamp, cause, epoch);
        if (selected == 2U) return 0U;
        if (selected == 1U) origin = Origin::LocalRoot;
    }
    ExecutionFrame frame{gFrame, reinterpret_cast<std::uintptr_t>(item), origin,
                         causalStamp, epoch, cause};
    gFrame = &frame;
    const bool remoteDescendant = origin == Origin::RemoteRoot &&
        cause.cause != 0U && cause.sourcePeer != 0U && cause.authoritySequence != 0U;
    if (remoteDescendant) TraceRemoteDescendant(cause, RootApplyTraceStage::DescendantExecute);
    std::uint8_t result{};
    const auto previousOrigin = gPublicOrigin;
    const auto previousEpoch = gPublicEpoch;
    const auto previousCause = gPublicCause;
    if (origin != Origin::None) { gPublicOrigin = origin; gPublicEpoch = epoch; gPublicCause = cause; }
#if defined(_MSC_VER) && defined(_M_IX86)
    __try { result = original(manager, target, owner, itemHolder); }
    __finally { gFrame = frame.previous; gPublicOrigin = previousOrigin; gPublicEpoch = previousEpoch; gPublicCause = previousCause; }
#else
    result = original(manager, target, owner, itemHolder);
    gFrame = frame.previous; gPublicOrigin = previousOrigin; gPublicEpoch = previousEpoch; gPublicCause = previousCause;
#endif
    if (remoteDescendant)
        TraceRemoteDescendant(cause, RootApplyTraceStage::DescendantCompleted, result);
    return result;
}

void __fastcall WorkDestroyDetour(void* self, void*) {
    const auto original = reinterpret_cast<WorkDestroyFn>(gDestroyTrampoline.load(std::memory_order_acquire));
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        if (original != nullptr) original(self);
        return;
    }
    const auto cause = TaskCause(self, gEpoch.load(std::memory_order_relaxed));
    if (cause.cause != 0U) TraceContext(cause, 7U);
    Forget(self);
    if (original != nullptr) original(self);
}

bool InvokeRemote(PublicFn original, void* definition, void* shared,
                  std::uint32_t input, double delay, void* parent, const ContextTrace& cause) noexcept {
    void* instance = shared;
    // Borrow a real receiver-local native parent, held by NativeCallbackLease.
    // Retail WorkItemBuild owns any references that survive this call.
    void* callback = parent;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        InvokePublicScoped(original, Origin::RemoteRoot,
            gEpoch.load(std::memory_order_relaxed), definition, &instance,
            &input, &callback, delay, 0U, &cause);
        return true;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

} // namespace

bool IsLocalPeer(std::uint64_t peer) noexcept {
    return peer != 0U && peer == gLocalPeer.load(std::memory_order_acquire);
}

void SetSessionAuthority(std::uint64_t localPeer, bool host) noexcept {
    if (localPeer == 0U) {
        ClearSessionAuthority();
        return;
    }
    // Every bind/rebind closes gameplay authority first. The Launcher opens the
    // bus only after WorldContext compatibility and recovery have been proven.
    gWorldBusActive.store(false, std::memory_order_release);
    gAuthorityHost.store(host, std::memory_order_release);
    gLocalPeer.store(localPeer, std::memory_order_release);
    gAuthorityBound.store(true, std::memory_order_release);
}

void ClearSessionAuthority() noexcept {
    gWorldBusActive.store(false, std::memory_order_release);
    gAuthorityBound.store(false, std::memory_order_release);
    gAuthorityHost.store(false, std::memory_order_release);
    gLocalPeer.store(0U, std::memory_order_release);
}

void SetWorldBusActive(bool active) noexcept {
    const auto allowed = active && gAuthorityBound.load(std::memory_order_acquire) &&
        gLocalPeer.load(std::memory_order_acquire) != 0U;
    gWorldBusActive.store(allowed, std::memory_order_release);
}

bool SessionAuthorityBound() noexcept {
    return gAuthorityBound.load(std::memory_order_acquire) &&
        gLocalPeer.load(std::memory_order_acquire) != 0U;
}

bool SessionAuthorityHost() noexcept {
    return SessionAuthorityBound() && gAuthorityHost.load(std::memory_order_acquire);
}

std::uint64_t SessionAuthorityPeer() noexcept {
    return SessionAuthorityBound() ? gLocalPeer.load(std::memory_order_acquire) : 0U;
}

bool WorldBusActive() noexcept {
    return SessionAuthorityBound() && gWorldBusActive.load(std::memory_order_acquire);
}

bool SchedulerEdgeLocalSidecarCaptureActive() noexcept {
    const auto rootKind = CurrentRootKind();
    return Installed() && !gSchedulerEdgeRetailScope && CurrentOrigin() == Origin::LocalRoot &&
        rootKind != static_cast<std::uint8_t>(world_interactions::PublicRootKind::SchedulerEdge) &&
        rootKind != static_cast<std::uint8_t>(world_interactions::PublicRootKind::CanonicalCompositeInput);
}

bool CurrentRootOwnsEvent(const WorldEntityId& target, const std::uint32_t input) noexcept {
    const auto* registry = gRegistry.load(std::memory_order_acquire);
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    const auto* cause = gPublicEpoch == epoch && gPublicCause.rootKind != 0U ? &gPublicCause :
        gFrame != nullptr && gFrame->epoch == epoch ? &gFrame->cause : nullptr;
    if (registry == nullptr || cause == nullptr ||
        cause->rootKind != static_cast<std::uint8_t>(world_interactions::PublicRootKind::EmittedOutput))
        return false;
    const auto profile = registry->FindEmittedOutputCausalProfile(cause->owner, cause->input);
    return profile && profile->CertifiedEmittedEventCarrierBoundary() &&
        profile->semanticTargetEntity == target && profile->semanticOperation == input;
}

bool SchedulerEdgeRemoteNaturalCaptureActive() noexcept {
    const auto rootKind = CurrentRootKind();
    return Installed() && !gSchedulerEdgeRetailScope && CurrentOrigin() == Origin::RemoteRoot &&
        rootKind != static_cast<std::uint8_t>(world_interactions::PublicRootKind::SchedulerEdge) &&
        rootKind != static_cast<std::uint8_t>(world_interactions::PublicRootKind::CanonicalCompositeInput);
}

void RememberRemoteNaturalSchedulerEdge(
    const WorldEntityId& source, const WorldEntityId& target,
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const double relativeDelaySeconds) noexcept {
    const auto parentAuthoritySequence = CurrentAuthoritySequence();
    if (!SchedulerEdgeRemoteNaturalCaptureActive() || parentAuthoritySequence == 0U ||
        !source.Valid() || !target.Valid() || schedulerParameterShortGuid == 0U ||
        targetInputShortGuid == 0U || !std::isfinite(relativeDelaySeconds) ||
        relativeDelaySeconds < 0.0 || relativeDelaySeconds > 600.0) return;

    const auto hash = SchedulerEdgeFingerprint(
        source, target, schedulerParameterShortGuid, targetInputShortGuid, relativeDelaySeconds);
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto start = static_cast<std::size_t>(hash) &
        (kSchedulerEdgeNaturalMatchCapacity - 1U);

    std::lock_guard lock(gSchedulerEdgeNaturalMatchMutex);
    for (std::size_t probe = 0U; probe < kSchedulerEdgeNaturalMatchCapacity; ++probe) {
        auto& slot = gSchedulerEdgeNaturalMatches[
            (start + probe) & (kSchedulerEdgeNaturalMatchCapacity - 1U)];
        if (slot.hash != 0U && slot.expiryTickMs > now) continue;
        slot.hash = hash;
        slot.expiryTickMs = now + kSchedulerEdgeNaturalMatchLifetimeMs;
        slot.parentAuthoritySequence = parentAuthoritySequence;
        gStats.schedulerEdgeRemoteNaturalRemembered.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
}

bool ConsumeRemoteNaturalSchedulerEdge(
    const WorldEntityId& source, const WorldEntityId& target,
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const double relativeDelaySeconds) noexcept {
    if (gRemoteSequence == 0U || !source.Valid() || !target.Valid() ||
        schedulerParameterShortGuid == 0U || targetInputShortGuid == 0U ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0 ||
        relativeDelaySeconds > 600.0) return false;

    const auto hash = SchedulerEdgeFingerprint(
        source, target, schedulerParameterShortGuid, targetInputShortGuid, relativeDelaySeconds);
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto start = static_cast<std::size_t>(hash) &
        (kSchedulerEdgeNaturalMatchCapacity - 1U);
    std::size_t best = kSchedulerEdgeNaturalMatchCapacity;
    std::uint64_t bestSequence = 0U;

    std::lock_guard lock(gSchedulerEdgeNaturalMatchMutex);
    for (std::size_t probe = 0U; probe < kSchedulerEdgeNaturalMatchCapacity; ++probe) {
        const auto index = (start + probe) & (kSchedulerEdgeNaturalMatchCapacity - 1U);
        auto& slot = gSchedulerEdgeNaturalMatches[index];
        if (slot.hash == 0U) continue;
        if (slot.expiryTickMs <= now) { slot = {}; continue; }
        if (slot.hash != hash || slot.parentAuthoritySequence == 0U ||
            slot.parentAuthoritySequence >= gRemoteSequence) continue;
        const auto delta = gRemoteSequence - slot.parentAuthoritySequence;
        if (delta > kSchedulerEdgeNaturalMatchMaximumSequenceDelta) continue;
        if (best == kSchedulerEdgeNaturalMatchCapacity ||
            slot.parentAuthoritySequence > bestSequence) {
            best = index;
            bestSequence = slot.parentAuthoritySequence;
        }
    }
    if (best == kSchedulerEdgeNaturalMatchCapacity) return false;
    gSchedulerEdgeNaturalMatches[best] = {};
    gStats.schedulerEdgeRemoteDedupConsumed.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool CanonicalCompositeClientCaptureActive() noexcept {
    // Runtime evidence from Patch 5.9: CanonicalCompositeInput Client roots did
    // not carry the actual door consequence and one speculative mapping forced
    // authority recovery. Keep the implementation available for postmortem, but
    // do not publish this class while the proven TypedMessage valve is active.
    return false;
}

CanonicalCompositeSubmission SubmitCanonicalCompositeRoot(
    const WorldEntityId& owner, const std::uint32_t publicInputShortGuid,
    ContextTrace& cause) noexcept {
    cause = {};
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!Enabled() || registry == nullptr || !registry->RuntimeCaptureReady() ||
        !owner.Valid() || publicInputShortGuid == 0U ||
        !registry->IsCompositePublicInputAllowed(owner, publicInputShortGuid)) {
        return CanonicalCompositeSubmission::RetailOnly;
    }
    // Patch 5.8 is specifically the missing Client-initiation valve. Host local
    // behavior keeps the already-stable WS3 carrier paths unchanged.
    if (!SessionAuthorityBound() || !WorldBusActive() || SessionAuthorityHost()) {
        return CanonicalCompositeSubmission::RetailOnly;
    }

    world_interactions::CompositePublicInputEvent event{};
    event.retailRoot = true;
    event.rootKind = world_interactions::PublicRootKind::CanonicalCompositeInput;
    event.publicInputShortGuid = publicInputShortGuid;
    event.targetEntity = owner;
    event.relativeDelaySeconds = 0.0;
    event.contextKind = world_interactions::PublicContextKind::Autonomous;
    event.actorKind = world_interactions::PublicActorKind::SourcePlayer;
    // Intentionally NO AwaitAuthority: WS2 proved this class must execute retail
    // locally first so the local player's trigger/prompt remains responsive.
    if (!TryConsumeRootRateBudget() ||
        !world_interactions::SubmitPublicRoot(owner, event)) {
        gStats.queueRejected.fetch_add(1U, std::memory_order_relaxed);
        return CanonicalCompositeSubmission::RetailOnly;
    }

    gStats.rootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.canonicalCompositeRootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.causalRootCommitted.fetch_add(1U, std::memory_order_relaxed);
    cause.cause = event.observationId;
    cause.rootKind = static_cast<std::uint8_t>(event.rootKind);
    cause.owner = owner;
    cause.target = owner;
    cause.input = publicInputShortGuid;
    cause.epoch = gEpoch.load(std::memory_order_relaxed);
    cause.causalChainId = event.causalSerial;
    cause.causalSource = static_cast<std::uint8_t>(CausalSource::Player);
    cause.actorKind = static_cast<std::uint8_t>(event.actorKind);
    cause.contextKind = static_cast<std::uint8_t>(event.contextKind);
    return CanonicalCompositeSubmission::Submitted;
}

void InvokeCanonicalCompositeRetailScoped(
    const Origin origin, const ContextTrace& cause,
    const CanonicalCompositeRetailThunk thunk, void* opaque) noexcept {
    if (thunk == nullptr || (origin != Origin::LocalRoot && origin != Origin::RemoteRoot)) return;
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    const auto previousOrigin = gPublicOrigin;
    const auto previousEpoch = gPublicEpoch;
    const auto previousCause = gPublicCause;
    gPublicOrigin = origin;
    gPublicEpoch = epoch;
    gPublicCause = cause;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try { thunk(opaque); }
    __finally {
        gPublicOrigin = previousOrigin;
        gPublicEpoch = previousEpoch;
        gPublicCause = previousCause;
    }
#else
    thunk(opaque);
    gPublicOrigin = previousOrigin;
    gPublicEpoch = previousEpoch;
    gPublicCause = previousCause;
#endif
}

SchedulerEdgeSubmission SubmitSchedulerEdgeRoot(
    void** sourceSharedRef, const WorldEntityId& source, const WorldEntityId& target,
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const double relativeDelaySeconds, ContextTrace& cause) noexcept {
    cause = {};
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!Enabled() || registry == nullptr || !registry->RuntimeCaptureReady() ||
        !source.Valid() || !target.Valid() || schedulerParameterShortGuid == 0U ||
        targetInputShortGuid == 0U || !std::isfinite(relativeDelaySeconds) ||
        relativeDelaySeconds < 0.0 || relativeDelaySeconds > 600.0) {
        return SchedulerEdgeSubmission::RetailOnly;
    }
    const bool sidecar = SchedulerEdgeLocalSidecarCaptureActive();
    if (sidecar)
        gStats.schedulerEdgeSidecarObserved.fetch_add(1U, std::memory_order_relaxed);

    // Before a real multiplayer WorldBus exists, QueueInput remains ordinary
    // retail work. SchedulerEdge must never turn loading/bootstrap activity
    // into authority traffic.
    if (!SessionAuthorityBound() || !WorldBusActive()) {
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return SchedulerEdgeSubmission::RetailOnly;
    }

    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    auto causalStamp = CurrentCausalStamp(epoch);
    const auto sourceKind = causalStamp.Valid()
        ? causalStamp.source : IdentifySourceCausality(*registry, sourceSharedRef);
    const bool nonHostClient = !SessionAuthorityHost();
    const bool sourcePlayer = sourceKind == CausalSource::Player;

    // Patch 5.9: keep autonomous Client edges Host-owned for NETWORK
    // authority, but do not veto the Client's retail call. Returning RetailOnly
    // makes QueueInputDetour execute the original edge locally and publish
    // nothing for this un-attributed autonomous edge.
    if (nonHostClient && !sourcePlayer) {
        gStats.authorityAutonomousSuppressed.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return SchedulerEdgeSubmission::RetailOnly;
    }

    world_interactions::CompositePublicInputEvent event{};
    event.retailRoot = true;
    event.rootKind = world_interactions::PublicRootKind::SchedulerEdge;
    event.publicInputShortGuid = targetInputShortGuid;
    event.schedulerParameterShortGuid = schedulerParameterShortGuid;
    event.targetEntity = target;
    event.relativeDelaySeconds = relativeDelaySeconds;
    event.contextKind = world_interactions::PublicContextKind::Autonomous;
    event.actorKind = nonHostClient
        ? world_interactions::PublicActorKind::SourcePlayer
        : world_interactions::PublicActorKind::Authority;
    if (nonHostClient) event.flags |= kPublicRootFlagAwaitAuthority;
    // Keep SchedulerEdge as an ordered singleton transaction. A scheduler edge
    // may be adjacent to a StateTransition carrier from the same gameplay
    // cause; grouping those heterogeneous boundaries would force the generic
    // transaction path to read all state before the Event executes. Let the
    // existing authority sequence preserve order without mixing reconciliation
    // models. SubmitPublicRoot assigns observationId as the causal serial.

    if (!TryConsumeRootRateBudget() ||
        !world_interactions::SubmitPublicRoot(source, event)) {
        gStats.queueRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return SchedulerEdgeSubmission::RetailOnly;
    }

    gStats.rootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.schedulerEdgeRootsSent.fetch_add(1U, std::memory_order_relaxed);
    if (sidecar)
        gStats.schedulerEdgeSidecarSubmitted.fetch_add(1U, std::memory_order_relaxed);
    gStats.causalRootCommitted.fetch_add(1U, std::memory_order_relaxed);
    if (relativeDelaySeconds != 0.0)
        gStats.delayedRoots.fetch_add(1U, std::memory_order_relaxed);

    cause.cause = event.observationId;
    cause.rootKind = static_cast<std::uint8_t>(event.rootKind);
    cause.owner = source;
    cause.target = target;
    cause.input = targetInputShortGuid;
    cause.epoch = epoch;
    cause.causalChainId = event.causalSerial;
    cause.causalSource = static_cast<std::uint8_t>(sourceKind);
    cause.actorKind = static_cast<std::uint8_t>(event.actorKind);
    cause.contextKind = static_cast<std::uint8_t>(event.contextKind);

    if (nonHostClient) {
        gStats.authorityAwaitSubmitted.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityLocalSuppressed.fetch_add(1U, std::memory_order_relaxed);
        return SchedulerEdgeSubmission::AwaitAuthority;
    }
    return SchedulerEdgeSubmission::ExecuteLocalRoot;
}

void InvokeSchedulerEdgeRetailScoped(
    const Origin origin, const ContextTrace& cause,
    const SchedulerEdgeRetailThunk thunk, void* opaque) noexcept {
    if (thunk == nullptr || (origin != Origin::LocalRoot && origin != Origin::RemoteRoot)) return;
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    const auto previousOrigin = gPublicOrigin;
    const auto previousEpoch = gPublicEpoch;
    const auto previousCause = gPublicCause;
    const auto previousSchedulerEdgeRetailScope = gSchedulerEdgeRetailScope;
    gPublicOrigin = origin;
    gPublicEpoch = epoch;
    gPublicCause = cause;
    gSchedulerEdgeRetailScope = true;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try { thunk(opaque); }
    __finally {
        gSchedulerEdgeRetailScope = previousSchedulerEdgeRetailScope;
        gPublicOrigin = previousOrigin;
        gPublicEpoch = previousEpoch;
        gPublicCause = previousCause;
    }
#else
    thunk(opaque);
    gSchedulerEdgeRetailScope = previousSchedulerEdgeRetailScope;
    gPublicOrigin = previousOrigin;
    gPublicEpoch = previousEpoch;
    gPublicCause = previousCause;
#endif
}


TypedMessageSubmission SubmitTypedMessageRoot(
    const WorldEntityId& source, const WorldEntityId& target,
    const std::uint32_t messageType, const std::span<const std::byte> payload,
    const double relativeDelaySeconds, ContextTrace& cause) noexcept {
    cause = {};
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!Enabled() || registry == nullptr || !registry->RuntimeCaptureReady() ||
        !source.Valid() || !target.Valid() ||
        payload.size() > world_interactions::kCompositePublicRootMaximumContextPayloadBytes ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0 ||
        relativeDelaySeconds > 600.0) {
        return TypedMessageSubmission::RetailOnly;
    }

    if (!SessionAuthorityBound() || !WorldBusActive()) {
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return TypedMessageSubmission::RetailOnly;
    }

    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    const auto causalStamp = CurrentCausalStamp(epoch);
    const auto observedSourceKind = causalStamp.Valid() ? causalStamp.source : CausalSource::World;
    const bool nonHostClient = !SessionAuthorityHost();

    // A non-empty TypedMessage attributed by the causal tracker to the local
    // Player may contain the sender's interaction/pose/Character context.
    // The receiving single-player engine can reinterpret that context as an
    // instruction to perform the same interaction, including repositioning
    // the observer's Player. Such messages are personal controls, not proven
    // shared-world state transitions. Keep retail on the originator only.
    // Do not guess that all Client World messages are player-owned: require a
    // positive Player causal stamp, so autonomous World->World messages retain
    // the existing replication contract.
    if (observedSourceKind == CausalSource::Player && !payload.empty()) {
        gStats.typedPlayerPayloadLocalOnly.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return TypedMessageSubmission::RetailOnly;
    }

    // Patch 5.10 restores the proven WS2 TypedMessage ordering under WS3
    // authority. A compatible Client is allowed to originate the same bounded
    // World->World TypedMessage class that WS2 published after retail local
    // execution. Host authority still authenticates, sequences and broadcasts
    // the occurrence, but it no longer requires the causal tracker to have
    // labelled the internal CATHODE descendant as Player before publication.
    //
    // This is intentionally generic: no Door GUID, level, trigger ShortGuid or
    // message id is special-cased here. Receiver-local interaction-control
    // messages are filtered by QueueTypedMessageDetour before this function.
    world_interactions::CompositePublicInputEvent event{};
    event.retailRoot = true;
    event.rootKind = world_interactions::PublicRootKind::TypedMessage;
    event.publicInputShortGuid = messageType;
    event.targetEntity = target;
    event.relativeDelaySeconds = relativeDelaySeconds;
    event.contextKind = world_interactions::PublicContextKind::Autonomous;
    event.actorKind = nonHostClient
        ? world_interactions::PublicActorKind::SourcePlayer
        : world_interactions::PublicActorKind::Authority;
    // WS2 local-first semantics: no AwaitAuthority on Client TypedMessages.
    // SubmitPublicRoot installs an observation echo slot for flags==0, so the
    // authoritative broadcast is consumed on the initiating Client instead of
    // executing the same retail message twice.
    event.flags = 0U;
    event.contextPayloadSize = static_cast<std::uint16_t>(payload.size());
    if (!payload.empty())
        std::copy(payload.begin(), payload.end(), event.contextPayload.begin());

    if (!TryConsumeRootRateBudget() ||
        !world_interactions::SubmitPublicRoot(source, event)) {
        gStats.queueRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return TypedMessageSubmission::RetailOnly;
    }

    gStats.rootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.typedMessageRootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.causalRootCommitted.fetch_add(1U, std::memory_order_relaxed);
    if (relativeDelaySeconds != 0.0)
        gStats.delayedRoots.fetch_add(1U, std::memory_order_relaxed);

    cause.cause = event.observationId;
    cause.rootKind = static_cast<std::uint8_t>(event.rootKind);
    cause.owner = source;
    cause.target = target;
    cause.input = messageType;
    cause.epoch = epoch;
    cause.causalChainId = event.causalSerial;
    cause.causalSource = static_cast<std::uint8_t>(
        nonHostClient ? CausalSource::Player : observedSourceKind);
    cause.actorKind = static_cast<std::uint8_t>(event.actorKind);
    cause.contextKind = static_cast<std::uint8_t>(event.contextKind);

    // Both Host and Client execute their originating retail TypedMessage. On a
    // Client the authoritative echo is suppressed by the observation id above.
    return TypedMessageSubmission::ExecuteLocalRoot;
}

void InvokeTypedMessageRetailScoped(
    const Origin origin, const ContextTrace& cause,
    const TypedMessageRetailThunk thunk, void* opaque) noexcept {
    if (thunk == nullptr || (origin != Origin::LocalRoot && origin != Origin::RemoteRoot)) return;
    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    const auto previousOrigin = gPublicOrigin;
    const auto previousEpoch = gPublicEpoch;
    const auto previousCause = gPublicCause;
    gPublicOrigin = origin;
    gPublicEpoch = epoch;
    gPublicCause = cause;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try { thunk(opaque); }
    __finally {
        gPublicOrigin = previousOrigin;
        gPublicEpoch = previousEpoch;
        gPublicCause = previousCause;
    }
#else
    thunk(opaque);
    gPublicOrigin = previousOrigin;
    gPublicEpoch = previousEpoch;
    gPublicCause = previousCause;
#endif
}

DoorLockStateSubmission SubmitDoorLockStateRoot(
    const WorldEntityId& source, const WorldEntityId& door,
    const std::uint32_t semanticInput, ContextTrace& cause) noexcept {
    cause = {};
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!Enabled() || registry == nullptr || !registry->RuntimeCaptureReady() ||
        !source.Valid() || !door.Valid() || source == door ||
        registry->FindWorldEntityDomain(source) != WorldDomain::World ||
        registry->FindWorldEntityDomain(door) != WorldDomain::World ||
        !world_animated::IsDoorLockStateInput(semanticInput) ||
        !SessionAuthorityBound() || !WorldBusActive() || SessionAuthorityHost()) {
        return DoorLockStateSubmission::RetailOnly;
    }

    // Patch 5.11.1 keeps the Client local-first but preserves the real retail
    // CATHODE provenance. The stable ROOT owner is the source entity and the
    // stable target is the Door. Raw context pointers never cross IPC.
    world_interactions::CompositePublicInputEvent event{};
    event.retailRoot = true;
    event.rootKind = world_interactions::PublicRootKind::DoorLockState;
    event.publicInputShortGuid = semanticInput;
    event.targetEntity = door;
    event.relativeDelaySeconds = 0.0;
    event.contextKind = world_interactions::PublicContextKind::Autonomous;
    event.actorKind = world_interactions::PublicActorKind::SourcePlayer;
    event.flags = 0U;

    if (!TryConsumeRootRateBudget() ||
        !world_interactions::SubmitPublicRoot(source, event)) {
        gStats.queueRejected.fetch_add(1U, std::memory_order_relaxed);
        gStats.authorityRetailPreserved.fetch_add(1U, std::memory_order_relaxed);
        return DoorLockStateSubmission::RetailOnly;
    }

    gStats.rootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.doorLockStateRootsSent.fetch_add(1U, std::memory_order_relaxed);
    gStats.causalRootCommitted.fetch_add(1U, std::memory_order_relaxed);

    const auto epoch = gEpoch.load(std::memory_order_relaxed);
    cause.cause = event.observationId;
    cause.rootKind = static_cast<std::uint8_t>(event.rootKind);
    cause.owner = source;
    cause.target = door;
    cause.input = semanticInput;
    cause.epoch = epoch;
    cause.causalChainId = event.causalSerial;
    cause.causalSource = static_cast<std::uint8_t>(CausalSource::Player);
    cause.actorKind = static_cast<std::uint8_t>(event.actorKind);
    cause.contextKind = static_cast<std::uint8_t>(event.contextKind);
    return DoorLockStateSubmission::Submitted;
}

bool RemoteRootOriginActive() noexcept {
    return Installed() && CurrentOrigin() == Origin::RemoteRoot;
}

void SetEmptyCallbackTest(bool enabled) noexcept {
    std::lock_guard lock(gEmptyCallbackConfigMutex);
    // Legacy IPC preference retained for compatibility with the existing UI.
    // ROOT v9 never permits this switch to detach an unsupported context.
    gEmptyCallbackTest.store(enabled, std::memory_order_release);
    gEmptyCallbackRuntimeConfigured = true;
}

bool PublicRootManifestAdmissionAllowed(const WorldEntityRegistry& registry,
    const WorldEntityId& owner, const world_interactions::CompositePublicInputEvent& event) noexcept {
    if (event.rootKind == world_interactions::PublicRootKind::CompositeInput) {
        const auto profile = registry.FindCompositePublicInputCausalProfile(owner, event.publicInputShortGuid);
        if (!profile || !profile->NetworkReplicableBoundary() ||
            registry.FindWorldEntityDomain(owner) != WorldDomain::World ||
            registry.FindWorldEntityDomain(event.targetEntity) != WorldDomain::World) {
            if (profile && !profile->NetworkReplicableBoundary())
                gStats.relevanceInboundRejected.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        // Patch 5.1 mirrors sender admission for a certified high-level cause.
        // The stable owner/input profile plus a stable World target is enough;
        // do not reject solely because the historical static target edge does
        // not enumerate the exact runtime-equivalent target selected by retail
        // CATHODE on this process. Wire/context invariants remain mandatory.
        if (profile->CertifiedCompositeStateTransitionCarrierBoundary()) {
            if (!world_interactions::ValidPublicRootContext(event) ||
                !std::isfinite(event.relativeDelaySeconds) ||
                event.relativeDelaySeconds < 0.0 || event.relativeDelaySeconds > 600.0 ||
                (event.flags & ~kPublicRootKnownFlags) != 0U ||
                (event.flags & 3U) == 3U) {
                return false;
            }
            if (!registry.IsCompositePublicRootActionKnown(owner, event.publicInputShortGuid,
                    event.targetEntity, event.relativeDelaySeconds, event.flags)) {
                gStats.admissionManifestTargetBypassed.fetch_add(1U, std::memory_order_relaxed);
            }
            return true;
        }

        return PublicRootManifestAdmissionAllowed(registry, owner, event.publicInputShortGuid,
            event.targetEntity, event.relativeDelaySeconds, event.flags);
    }
    if (event.rootKind == world_interactions::PublicRootKind::CanonicalCompositeInput) {
        // Patch 5.8: exact WS2 public-input canonicalization under WS3 authority.
        // The origin executes retail first; only the stable public semantic is
        // sequenced. No callback/WorkItem/RuntimeKey leaves the process.
        return world_interactions::ValidPublicRootContext(event) &&
            event.targetEntity == owner && event.relativeDelaySeconds == 0.0 &&
            registry.FindWorldEntityDomain(owner) == WorldDomain::World &&
            registry.IsCompositePublicInputAllowed(owner, event.publicInputShortGuid);
    }
    if (event.rootKind == world_interactions::PublicRootKind::DoorLockState) {
        // Patch 5.11.1: preserve the real stable CATHODE source -> Door edge.
        // Both endpoints must be manifest World entities and a Door self-edge
        // is rejected before any native replay. The target Door vtable is then
        // revalidated on the receiver.
        return world_interactions::ValidPublicRootContext(event) &&
            event.targetEntity != owner && event.relativeDelaySeconds == 0.0 &&
            world_animated::IsDoorLockStateInput(event.publicInputShortGuid) &&
            registry.FindWorldEntityDomain(owner) == WorldDomain::World &&
            registry.FindWorldEntityDomain(event.targetEntity) == WorldDomain::World;
    }
    if (event.rootKind == world_interactions::PublicRootKind::SchedulerEdge) {
        // Patch 5.3 absorbs the proven WS2 scheduler primitive into WS3. The
        // stable source is the ROOT owner, the stable target is carried in the
        // payload, and both must be manifest World entities. Replay itself is
        // performed through retail FUN_00921F70 -> FUN_009271C0.
        return world_interactions::ValidPublicRootContext(event) &&
            registry.FindWorldEntityDomain(owner) == WorldDomain::World &&
            registry.FindWorldEntityDomain(event.targetEntity) == WorldDomain::World;
    }
    if (event.rootKind == world_interactions::PublicRootKind::TypedMessage) {
        // Patch 5.4 absorbs the second proven WS2 generic CATHODE primitive.
        // Only the historically safe World->World class is portable. RuntimeKeys
        // are deliberately absent from the wire and are resolved locally later.
        return world_interactions::ValidPublicRootContext(event) &&
            registry.FindWorldEntityDomain(owner) == WorldDomain::World &&
            registry.FindWorldEntityDomain(event.targetEntity) == WorldDomain::World;
    }
    if (event.rootKind == world_interactions::PublicRootKind::ScheduledInput) {
        // 00926A60 only reaches 00926680 with one identical selected holder in
        // both argument positions. Preserve that proven consumer shape on the
        // receiver instead of accepting a synthetic owner/target split.
        if (!(owner == event.targetEntity) || !world_interactions::ValidPublicRootContext(event) ||
            registry.FindWorldEntityDomain(owner) != WorldDomain::World ||
            registry.FindWorldEntityDomain(event.targetEntity) != WorldDomain::World) return false;
        const auto profile = registry.FindScheduledInputCausalProfile(event.targetEntity, event.publicInputShortGuid);
        if (!profile || !profile->NetworkReplicableBoundary()) {
            if (profile) gStats.relevanceInboundRejected.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
    if (event.rootKind != world_interactions::PublicRootKind::EmittedOutput ||
        !(owner == event.targetEntity) || !world_interactions::ValidPublicRootContext(event) ||
        !std::isfinite(event.relativeDelaySeconds) || event.relativeDelaySeconds < 0.0 ||
        event.relativeDelaySeconds > 600.0 || registry.FindWorldEntityDomain(owner) != WorldDomain::World)
        return false;
    const auto profile = registry.FindEmittedOutputCausalProfile(owner, event.publicInputShortGuid);
    if (!profile || !profile->NetworkReplicableBoundary()) {
        if (profile) gStats.relevanceInboundRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool PublicRootManifestAdmissionAllowed(
    const WorldEntityRegistry& registry, const WorldEntityId& owner,
    const std::uint32_t input, const WorldEntityId& target,
    const double relativeDelaySeconds, const std::uint8_t flags) noexcept {
    // Hard protocol/runtime invariants are never live-bypassable.
    if (!owner.Valid() || !target.Valid() || input == 0U ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0 ||
        relativeDelaySeconds > 600.0 || (flags & ~kPublicRootKnownFlags) != 0U ||
        (flags & 3U) == 3U) {
        return false;
    }

    if (ai_sync::OwnsAI() && registry.FindWorldEntityDomain(target) == WorldDomain::AI) return false;

    // ProbeDirect is the controlled transport witness in 0.11.4.72. It must
    // obey the same live admission contract that already accepted it in
    // PublicDetour. The previous special case re-applied the exact static
    // owner/input/target manifest edge here and therefore rejected the one
    // ProbeDirect sample after it had already passed sender admission whenever
    // ManifestTargetContract was open. That made GenericRoot exceed RootSend by
    // exactly one and surfaced as a queue rejection. Keep the hard wire/runtime
    // invariants above, then use the common owner/target admission path below.
    const bool ownerInputKnown = registry.IsCompositePublicRootKnown(owner, input);
    if (!ownerInputKnown) {
        if (SecurityEnabled(AdmissionSecurityGate::ManifestOwnerInput)) return false;
        const auto domain = registry.FindWorldEntityDomain(target);
        return domain.has_value() &&
            (*domain == WorldDomain::World ||
             (*domain != WorldDomain::Unknown && !LocalTargetSecurityEnabled(*domain)));
    }

    if (registry.IsCompositePublicRootActionKnown(
            owner, input, target, relativeDelaySeconds, flags)) {
        return true;
    }
    if (SecurityEnabled(AdmissionSecurityGate::ManifestTargetContract)) return false;

    const auto domain = registry.FindWorldEntityDomain(target);
    return domain.has_value() &&
        (*domain == WorldDomain::World ||
         (*domain != WorldDomain::Unknown && !LocalTargetSecurityEnabled(*domain)));
}

bool Requested() noexcept { return true; }

bool PrepareLocalCompositeInputBoundary(
    const SignatureResolver& resolver) noexcept {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* publicTarget = resolver.ResolveMutable(SymbolId::CathodePublicCompositeAction);
    if (module == 0U || publicTarget == nullptr) return false;

    // Deliberately no hooks/registry/session state here. PlayerSync needs the
    // retail CompositeInput boundary before WorldSync's manifest lifecycle is
    // necessarily armed. Arm() later resolves the same certified symbol and
    // installs the capture detour; InvokeLocalCompositeInputNoCapture then
    // switches to the published trampoline automatically.
    gModule = module;
    gPublicTarget = publicTarget;
    return true;
}

bool PrepareLocalEmittedOutputObservation(
    const SignatureResolver& resolver, HookManager& hooks,
    const LocalEmittedOutputObserver observer, std::string& error) noexcept {
    error.clear();
    if (observer == nullptr) {
        error = "PlayerSync emitted-output observer is null";
        return false;
    }

    gLocalOutputObserver.store(observer, std::memory_order_release);
    if (gOutputTrampoline.load(std::memory_order_acquire) != nullptr) {
        return true;
    }

    const auto* spec = Retail2015Profile().Find(SymbolId::CathodeOutputPropagation);
    auto* target = resolver.ResolveMutable(SymbolId::CathodeOutputPropagation);
    if (spec == nullptr || target == nullptr) {
        gLocalOutputObserver.store(nullptr, std::memory_order_release);
        error = "PlayerSync retail EmittedOutput boundary did not resolve";
        return false;
    }

    HookRequest request{};
    request.name = "playersync_motion_tracker_output_observer";
    request.target = target;
    request.detour = reinterpret_cast<void*>(&OutputDetour);
    request.expected = spec->expected;
    request.patchLength = spec->approvedPatchLength;
    request.relocationSafePrefix = spec->trampolineRelocationSafe;
    request.trampolineOut = &gOutputRaw;
    request.publishedTrampolineOut = &gOutputTrampoline;
    if (!hooks.Install(request, error)) {
        gLocalOutputObserver.store(nullptr, std::memory_order_release);
        return false;
    }
    return true;
}

void ClearLocalEmittedOutputObserver(
    const LocalEmittedOutputObserver observer) noexcept {
    if (observer == nullptr) return;
    auto expected = observer;
    (void)gLocalOutputObserver.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
}

bool Arm(const SignatureResolver& resolver, HookManager& hooks,
         WorldEntityRegistry& registry, std::string& error) noexcept {
    error.clear();
    if (Installed()) return true;
    {
        std::lock_guard lock(gEmptyCallbackConfigMutex);
        if (!gEmptyCallbackRuntimeConfigured) {
            wchar_t emptyCallback[2]{};
            gEmptyCallbackTest.store(
                GetEnvironmentVariableW(L"AIMP_WS3_EMPTY_CALLBACK", emptyCallback, 2U) == 1U &&
                emptyCallback[0] == L'1', std::memory_order_release);
        }
    }
    gRegistry.store(&registry, std::memory_order_release);
    if (!PrepareLocalCompositeInputBoundary(resolver)) {
        error = "WS3 retail public CompositeInput boundary did not resolve";
        gStats.installFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    if (!RetailControlIdsMatch()) {
        error = "WS3 retail behavior-control ShortGuid constants do not match the certified build";
        gStats.installFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    const auto checkpoint = hooks.InstalledCount();
    struct Entry { SymbolId id; const char* name; void* detour; void** raw; std::atomic<void*>* published; };
    const Entry entries[] = {
        {SymbolId::CathodeSchedulerDrain, "worldsync3_causal_turn", reinterpret_cast<void*>(&SchedulerDrainDetour), &gDrainRaw, &gDrainTrampoline},
        {SymbolId::CathodeOutputPropagation, "worldsync3_emitted_output", reinterpret_cast<void*>(&OutputDetour), &gOutputRaw, &gOutputTrampoline},
        {SymbolId::CathodeWorkItemDestroy, "worldsync3_work_lifetime", reinterpret_cast<void*>(&WorkDestroyDetour), &gDestroyRaw, &gDestroyTrampoline},
        {SymbolId::CathodeWorkItemBuild, "worldsync3_work_lineage", reinterpret_cast<void*>(&WorkBuildDetour), &gBuildRaw, &gBuildTrampoline},
        {SymbolId::CathodeWorkItemExecute, "worldsync3_work_scope", reinterpret_cast<void*>(&WorkExecuteDetour), &gExecuteRaw, &gExecuteTrampoline},
        {SymbolId::CathodePublicCompositeAction, "worldsync3_public_root", reinterpret_cast<void*>(&PublicDetour), &gPublicRaw, &gPublicTrampoline},
    };
    for (const auto& entry : entries) {
        if (entry.id == SymbolId::CathodeOutputPropagation &&
            gOutputTrampoline.load(std::memory_order_acquire) != nullptr) {
            continue;
        }
        const auto* spec = Retail2015Profile().Find(entry.id);
        auto* target = resolver.ResolveMutable(entry.id);
        HookRequest request{};
        request.name = entry.name; request.target = target; request.detour = entry.detour;
        if (spec != nullptr) {
            request.expected = spec->expected;
            request.patchLength = spec->approvedPatchLength;
            request.relocationSafePrefix = spec->trampolineRelocationSafe;
        }
        request.trampolineOut = entry.raw; request.publishedTrampolineOut = entry.published;
        if (spec == nullptr || target == nullptr || !hooks.Install(request, error)) {
            gStats.installFailures.fetch_add(1U, std::memory_order_relaxed);
            std::string rollbackError;
            if (!hooks.UninstallFrom(checkpoint, rollbackError)) error += "; rollback: " + rollbackError;
            gEnabled.store(false, std::memory_order_release);
            return false;
        }
    }
    gInstalled.store(true, std::memory_order_release);
    ResetForGeneration();
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
bool Enabled() noexcept { return gEnabled.load(std::memory_order_acquire); }

bool InvokeLocalActionNoCapture(void(*action)(void*) noexcept, void* context) noexcept {
    if (action == nullptr) return false;
    const auto previousOrigin = gPublicOrigin;
    const auto previousEpoch = gPublicEpoch;
    const auto previousCause = gPublicCause;
    gPublicOrigin = Origin::RemoteRoot;
    gPublicEpoch = gEpoch.load(std::memory_order_acquire);
    gPublicCause = {};
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        __try { action(context); }
        __finally {
            gPublicOrigin = previousOrigin;
            gPublicEpoch = previousEpoch;
            gPublicCause = previousCause;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    action(context);
    gPublicOrigin = previousOrigin;
    gPublicEpoch = previousEpoch;
    gPublicCause = previousCause;
#endif
    return true;
}

bool MissionOutputReady() noexcept {
    return gOutputTrampoline.load(std::memory_order_acquire) != nullptr && gModule != 0U;
}

namespace {
// SEH boundary kept free of C++ unwinding (the callback lease lives in the caller).
LocalCompositeInputInvokeStatus InvokeMissionOutputGuarded(PublicFn original, void* definition,
    void* sharedRef, std::uint32_t port, void* callback) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        const auto epoch = gEpoch.load(std::memory_order_relaxed);
        ContextTrace cause{};
        cause.rootKind = kMissionRootKind;
        cause.epoch = epoch;
        cause.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
        cause.input = port;
        StoreTaskCause(callback, cause, epoch);
        BindParentContext(callback, Origin::RemoteRoot, {}, cause, epoch);
        InvokePublicScoped(original, Origin::RemoteRoot, epoch,
            definition, &sharedRef, &port, &callback, 0.0, 0U, &cause);
        return LocalCompositeInputInvokeStatus::Invoked;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return LocalCompositeInputInvokeStatus::NativeFault; }
#endif
}
}

LocalCompositeInputInvokeStatus InvokeMissionOutputNoCapture(
    const WorldEntityLease& lease, const std::uint32_t port, const std::uint8_t selector) noexcept {
    if (selector == 4U) {
        if (!lease || port == 0U) return LocalCompositeInputInvokeStatus::InvalidBoundary;
        // Scheduled frontiers are Checkpoint/SwitchLevel/GoToFrontend. Builds
        // 492-498 handed Checkpoint a WorkItem cloned by AIMP and called the
        // consumer FUN_00926680 directly. The save writer (FUN_00930A50 ->
        // FUN_00934FC0) then read a destroyed pause_context_trigger queue
        // (manager+4) and both peers crashed at 0x934FD3 on every save
        // station, with or without releasing that WorkItem. Queue the input
        // through the retail scheduler instead, like any authored edge, so
        // retail builds, owns and delivers its own WorkItem next drain.
        const auto epoch = gEpoch.load(std::memory_order_relaxed);
        ContextTrace cause{};
        cause.rootKind = kMissionRootKind; cause.epoch = epoch; cause.input = port;
        cause.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
        struct ScheduledReplay {
            const WorldEntityLease* lease;
            std::uint32_t port;
            cathode_primitives::ExecuteResult result;
        } replay{&lease, port, {}};
        const auto invoke = [](void* opaque) noexcept {
            auto& call = *static_cast<ScheduledReplay*>(opaque);
            call.result = cathode_primitives::ExecuteInputEdge(
                call.port, call.port, 0.0F, *call.lease, *call.lease);
        };
        AddPendingMissionSchedule(lease.Get(), port);
        InvokeSchedulerEdgeRetailScoped(Origin::RemoteRoot, cause, invoke, &replay);
        if (replay.result.status == cathode_primitives::ExecuteStatus::Queued && replay.result.queued)
            return LocalCompositeInputInvokeStatus::Invoked;
        static_cast<void>(ConsumePendingMissionSchedule(lease.Get(), port));
        return replay.result.status == cathode_primitives::ExecuteStatus::RuntimeUnavailable
            ? LocalCompositeInputInvokeStatus::RuntimeUnavailable
            : LocalCompositeInputInvokeStatus::NativeFault;
    }
    auto* sharedRef = WorldPublicActionLeaseAccess::SharedRef(lease);
    void* definition{};
    if (!lease || sharedRef == nullptr || port == 0U ||
        !(selector == 3U ? PublicDefinition(lease.Get(), definition)
                        : OutputDefinition(lease.Get(), selector, definition)))
        return LocalCompositeInputInvokeStatus::InvalidBoundary;
    const auto original = reinterpret_cast<PublicFn>((selector == 3U ? gPublicTrampoline : gOutputTrampoline)
        .load(std::memory_order_acquire));
    if (original == nullptr) return LocalCompositeInputInvokeStatus::RuntimeUnavailable;
    // v355 replayed with a null callback. Retail receivers dereference the
    // causal WorkItem (e.g. the activation handler FUN_00937670 -> 004DFE90
    // reads *context+0x20) and both peers crashed on the generator objective.
    // Root the fan-out in a real receiver-local callback on the frontier
    // entity, exactly like ItemSync's placed-completion replay. Retail
    // descendants retain their own references before the lease is released.
    cathode_primitives::NativeCallbackLease context;
    if (cathode_primitives::CreateNativeCallback(lease, 0x10000000U, {}, context) !=
            cathode_primitives::ExecuteStatus::Queued || context.Get() == nullptr)
        return LocalCompositeInputInvokeStatus::RuntimeUnavailable;
    return InvokeMissionOutputGuarded(original, definition, sharedRef, port, context.Get());
}

LocalCompositeInputInvokeStatus InvokeLocalCompositeInputNoCapture(
    void* executionObject,
    void* sharedRef,
    const std::uint32_t publicInputShortGuid,
    std::uintptr_t* resolvedDispatchOut) noexcept {
    if (resolvedDispatchOut != nullptr) *resolvedDispatchOut = 0U;
    if (executionObject == nullptr || sharedRef == nullptr ||
        publicInputShortGuid == 0U) {
        return LocalCompositeInputInvokeStatus::InvalidArguments;
    }
    if (gPublicTarget == nullptr || gModule == 0U) {
        return LocalCompositeInputInvokeStatus::RuntimeUnavailable;
    }

    // Patch 126: keep PublicDefinition() strict for network/WS3 identity
    // admission, but do not impose its exact-base-dispatch requirement on this
    // local PlayerSync provisioning call. The selected equipment archetype is created as a
    // retail specialized Entity. Its +0x70 getter can therefore return a
    // specialized Composite interface whose virtual +0x394 is a retail
    // override rather than the base 00965920 entry. Patch 122 already observed
    // this class family exposing an executable 008230A0 +0x394 override.
    //
    // The virtual slot has the same ABI as 00965920. Resolve it from the exact
    // interface returned by the retail +0x70 getter, require both getter and
    // dispatch to live inside AI.exe, and invoke the override under a
    // RemoteRoot scope. If the slot is the hooked base function itself, use the
    // trampoline exactly as before so this local equipment action never becomes
    // a WS3 public ROOT.
    void* definition = nullptr;
    PublicFn virtualDispatch = nullptr;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        const auto vtable = *static_cast<std::byte**>(executionObject);
        if (vtable == nullptr) {
            return LocalCompositeInputInvokeStatus::InvalidBoundary;
        }
        const auto getter = *reinterpret_cast<DefinitionFn*>(vtable + 0x70U);
        const auto getterAddress = reinterpret_cast<std::uintptr_t>(getter);
        if (getterAddress < gModule + 0x1000U ||
            getterAddress >= gModule + 0xD9E13BU) {
            return LocalCompositeInputInvokeStatus::DefinitionGetterRejected;
        }
        definition = getter(executionObject);
        if (definition == nullptr) {
            return LocalCompositeInputInvokeStatus::DefinitionNullRejected;
        }
        const auto definitionVtable = *static_cast<std::byte**>(definition);
        if (definitionVtable == nullptr) {
            return LocalCompositeInputInvokeStatus::DefinitionDispatchRejected;
        }
        virtualDispatch = *reinterpret_cast<PublicFn*>(definitionVtable + 0x394U);
        const auto dispatchAddress = reinterpret_cast<std::uintptr_t>(virtualDispatch);
        if (dispatchAddress < gModule + 0x1000U ||
            dispatchAddress >= gModule + 0xD9E13BU) {
            return LocalCompositeInputInvokeStatus::DefinitionDispatchRejected;
        }
        if (resolvedDispatchOut != nullptr) *resolvedDispatchOut = dispatchAddress;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return LocalCompositeInputInvokeStatus::DefinitionFaultRejected;
    }
#endif

    auto original = virtualDispatch;
    if (reinterpret_cast<void*>(virtualDispatch) == gPublicTarget) {
        const auto trampoline = reinterpret_cast<PublicFn>(
            gPublicTrampoline.load(std::memory_order_acquire));
        if (trampoline != nullptr) {
            original = trampoline;
        } else if (Installed()) {
            // The base target is patched once WS3 is installed. Never call the
            // patched address directly without its trampoline.
            return LocalCompositeInputInvokeStatus::RuntimeUnavailable;
        }
    }
    if (original == nullptr) {
        return LocalCompositeInputInvokeStatus::RuntimeUnavailable;
    }

    void* instance = sharedRef;
    void* callback = nullptr;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        InvokePublicScoped(
            original, Origin::RemoteRoot,
            gEpoch.load(std::memory_order_relaxed), definition, &instance,
            &publicInputShortGuid, &callback, 0.0, 0U);
        return LocalCompositeInputInvokeStatus::Invoked;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return LocalCompositeInputInvokeStatus::NativeFault;
    }
#else
    InvokePublicScoped(
        original, Origin::RemoteRoot,
        gEpoch.load(std::memory_order_relaxed), definition, &instance,
        &publicInputShortGuid, &callback, 0.0, 0U);
    return LocalCompositeInputInvokeStatus::Invoked;
#endif
}

bool DeferPublicInput(void* sharedRef, std::uint32_t input) noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!Enabled() || registry == nullptr || !registry->RuntimeCaptureReady() ||
        !registry->CouldBeCompositePublicInputShortGuid(input)) return false;
    void* execution{}; void* callback{}; void* callbackValue{};
    std::uint32_t guid{}; bool payloadless{};
    if (!ReadPublicArguments(&sharedRef, &input, &callback, execution, guid,
                             callbackValue, payloadless)) return false;
    const auto owner = registry->IdentifyRuntimeEntity(execution);
    void* definition{};
    if (!owner || !registry->IsCompositePublicInputAllowed(*owner, input) ||
        !PublicDefinition(execution, definition)) return false;

    // QueueInput should yield only to a higher public boundary that WS3 can
    // actually publish. Merely knowing a Composite port exists is insufficient:
    // Event/Unknown public inputs that are not NetworkReplicableBoundary() must
    // fall through to the generic SchedulerEdge valve rather than disappearing
    // between a deferred Composite and a suppressed lower consumer.
    const auto profile = registry->FindCompositePublicInputCausalProfile(*owner, input);
    return profile.has_value() && profile->NetworkReplicableBoundary();
}

bool SuppressDescendantCapture() noexcept {
    if (!Installed() || CurrentOrigin() == Origin::None) return false;
    gStats.ws2Bypassed.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool CallbackBridgeCaptureActive() noexcept {
    // The historical pre-Queue callback bridge is intentionally dormant in
    // WS3 v0.11.4.59. Keep the API as a compatibility no-op without leaving
    // constexpr-dead code that MSVC reports as C4702 under /W4 /WX.
    return false;
}

bool TryGetCallbackBridgeContext(
    WorldEntityId& owner, std::uint32_t& outerInput) noexcept {
    owner = {};
    outerInput = 0U;
    return false;
}

bool SubmitCallbackBridgeRoot(
    const WorldEntityId& owner, const std::uint32_t input) noexcept {
    static_cast<void>(owner);
    static_cast<void>(input);
    return false;
}

void ResetForGeneration() noexcept {
    gEpoch.fetch_add(1U, std::memory_order_acq_rel);
    {
        std::lock_guard lock(gLineageMutex);
        gLineage.Clear();
        gCausalProvenance.Clear();
        gCausalGroups.Clear();
        gTaskCauses.Clear();
        gParentContexts.Clear();
        gLiveTags.store(0U, std::memory_order_release);
        gLiveCausalTags.store(0U, std::memory_order_release);
        gLiveCausalGroups.store(0U, std::memory_order_release);
        gLiveParentContexts.store(0U, std::memory_order_release);
    }
    {
        std::lock_guard lock(gRelevanceProducerMutex);
        gRelevanceProducers.fill({});
    }
    {
        std::lock_guard lock(gSchedulerEdgeNaturalMatchMutex);
        gSchedulerEdgeNaturalMatches.fill({});
    }
    gEnabled.store(Installed(), std::memory_order_release);
}

void DrainDiagnosticFiles(aimp::Diagnostics& diagnostics) noexcept {
    try {
        RootApplyTraceRecord current{}, completed{};
        bool hasCurrent{}, hasCompleted{};
        {
            std::lock_guard lock(gRootApplyTraceMutex);
            current = gRootApplyTraceCurrent;
            completed = gRootApplyTraceCompleted;
            hasCurrent = gRootApplyTraceHasCurrent;
            hasCompleted = gRootApplyTraceHasCompleted;
        }
        const auto records = gDeferredRootJournal.Drain();
        const auto dropped = gDeferredRootJournal.TakeDropped();
        {
            std::lock_guard lock(gRootApplyFileMutex);
            for (const auto& record : records) AppendRootApplyJournalLocked(record);
            if (hasCurrent || hasCompleted)
                WriteRootApplyTraceLocked(current, hasCurrent, completed, hasCompleted);
        }
        if (dropped != 0U)
            diagnostics.Log("warning", "worldsync_diagnostic_records_dropped",
                "Diagnostic journal buffer overflow; gameplay events are unaffected.",
                {{"count", std::to_string(dropped)}});
    } catch (...) { }
}

void BeginLogicalShutdown() noexcept {
    gEnabled.store(false, std::memory_order_release);
    gEpoch.fetch_add(1U, std::memory_order_acq_rel);
    gRegistry.store(nullptr, std::memory_order_release);
    {
        std::lock_guard lock(gRootApplyFileMutex);
        if (gRootApplyTraceFile != INVALID_HANDLE_VALUE) {
            static_cast<void>(FlushFileBuffers(gRootApplyTraceFile));
            CloseHandle(gRootApplyTraceFile);
            gRootApplyTraceFile = INVALID_HANDLE_VALUE;
        }
        if (gRootApplyJournalFile != INVALID_HANDLE_VALUE) {
            static_cast<void>(FlushFileBuffers(gRootApplyJournalFile));
            CloseHandle(gRootApplyJournalFile);
            gRootApplyJournalFile = INVALID_HANDLE_VALUE;
        }
    }
    // HookManager owns code restoration; keep published trampolines alive.
}

Diagnostics Snapshot() noexcept {
    Diagnostics result{};
    for (std::size_t i = 0U; i < result.classified.size(); ++i)
        result.classified[i] = gStats.classified[i].load(std::memory_order_relaxed);
    result.targetOwnerDistinct = gStats.targetOwnerDistinct.load(std::memory_order_relaxed);
    result.externalRoots = gStats.externalRoots.load(std::memory_order_relaxed);
    result.delayedRoots = gStats.delayedRoots.load(std::memory_order_relaxed);
    result.installed = Installed(); result.enabled = Enabled();
    result.emptyCallbackTest = gEmptyCallbackTest.load(std::memory_order_acquire);
    result.admissionSecurityMask = 0U;
#define WS3_READ(name) result.name = gStats.name.load(std::memory_order_relaxed)
    WS3_READ(contextUnsupported); WS3_READ(contextReconstructed); WS3_READ(contextPending);
    WS3_READ(contextChainsCaptured); WS3_READ(contextFramesCaptured);
    WS3_READ(contextChainDepthPeak); WS3_READ(contextChainOverflow);
    WS3_READ(contextFramesReconstructed);
    WS3_READ(actorUnresolved); WS3_READ(causeTableOverflow);
    WS3_READ(seen); WS3_READ(publicCandidates); WS3_READ(rootsSent); WS3_READ(compositeInputRootsSent); WS3_READ(emittedOutputRootsSent); WS3_READ(scheduledInputRootsSent); WS3_READ(schedulerEdgeRootsSent); WS3_READ(typedMessageRootsSent); WS3_READ(typedMessageRootsApplied); WS3_READ(typedPlayerPayloadLocalOnly); WS3_READ(canonicalCompositeRootsSent); WS3_READ(canonicalCompositeRootsApplied); WS3_READ(doorLockStateRootsSent); WS3_READ(doorLockStateRootsApplied);
    WS3_READ(schedulerEdgeSidecarObserved); WS3_READ(schedulerEdgeSidecarSubmitted);
    WS3_READ(schedulerEdgeRemoteNaturalRemembered); WS3_READ(schedulerEdgeRemoteDedupConsumed);
    WS3_READ(scheduledEventRootsSuppressed);
    WS3_READ(descendantsSuppressed); WS3_READ(remoteSuppressed); WS3_READ(identityRejected);
    WS3_READ(contextRejected); WS3_READ(queueRejected); WS3_READ(playerCausalRejected);
    WS3_READ(playerCausalNodesScanned); WS3_READ(playerCausalNodesMapped);
    WS3_READ(targetOwnerRejected); WS3_READ(targetOwnerMatched);
    WS3_READ(probeTargetOwnerRejected); WS3_READ(causalUnknownRejected);
    WS3_READ(causalShapeRejected); WS3_READ(causalUnmappedNodes);
    WS3_READ(causalCycleRejected); WS3_READ(causalDepthRejected);
    WS3_READ(lineageOverflow);
    WS3_READ(lineageBuildInherit); WS3_READ(lineageExecuteRestore); WS3_READ(lineageCallbackRestore);
    WS3_READ(argRejected); WS3_READ(payloadRejected); WS3_READ(callbackRejected);
    WS3_READ(delayRejected); WS3_READ(tailRejected);
    WS3_READ(probeDirectSeen); WS3_READ(probeDirectArgRejected); WS3_READ(probeDirectPayload);
    WS3_READ(probeDirectCallback); WS3_READ(probeDirectDelay); WS3_READ(probeDirectTail);
    WS3_READ(probeDirectEligible);
    WS3_READ(applied); WS3_READ(applyRejected); WS3_READ(nativeFaults);
    WS3_READ(rootApplyTraceWrites); WS3_READ(rootApplyJournalWrites); WS3_READ(rootApplyTraceStarted);
    WS3_READ(rootApplyTraceCompleted);
    WS3_READ(rootApplyTraceLastStartedObservation);
    WS3_READ(rootApplyTraceLastStartedSequence);
    WS3_READ(rootApplyTraceLastCompletedObservation);
    WS3_READ(rootApplyTraceLastCompletedSequence);
    WS3_READ(rootApplyTraceCurrentStage);
    WS3_READ(ws2Bypassed); WS3_READ(callbackRoots);
    WS3_READ(callbackGameplayRejected);
    WS3_READ(callbackBoundaryCandidates); WS3_READ(callbackBoundaryRoots);
    WS3_READ(callbackBoundaryPlayerRoots); WS3_READ(callbackBoundaryAiRoots);
    WS3_READ(callbackBoundaryUiRoots); WS3_READ(callbackBoundaryAimpRoots);
    WS3_READ(callbackBoundaryApplied);
    WS3_READ(callbackWorldContinuationRejected); WS3_READ(callbackBoundaryUnknownRejected);
    WS3_READ(admissionForbiddenInputBypassed);
    WS3_READ(admissionGuidPrefilterRejected); WS3_READ(admissionGuidPrefilterBypassed);
    WS3_READ(admissionPayloadBypassed); WS3_READ(admissionCallbackShapeBypassed);
    WS3_READ(admissionTailBypassed);
    WS3_READ(admissionPlayerTargetBypassed); WS3_READ(admissionAiTargetBypassed);
    WS3_READ(admissionUiTargetBypassed); WS3_READ(admissionAimpTargetBypassed);
    WS3_READ(admissionWorldContinuationBypassed); WS3_READ(admissionUnknownCausalityBypassed);
    WS3_READ(admissionManifestOwnerInputBypassed); WS3_READ(admissionManifestTargetBypassed);
    WS3_READ(admissionDescendantBypassed);
    WS3_READ(causalProvenanceSeeded); WS3_READ(causalProvenanceInherited);
    WS3_READ(causalProvenanceBoundary); WS3_READ(causalProvenanceOverflow);
    WS3_READ(causalProvenancePlayer); WS3_READ(causalProvenanceAi);
    WS3_READ(causalProvenanceUi); WS3_READ(causalProvenanceAimp);
    WS3_READ(causalChainsCreated); WS3_READ(causalChainsReleased);
    result.causalGroupsLive = gLiveCausalGroups.load(std::memory_order_acquire);
    WS3_READ(causalGroupsLivePeak); WS3_READ(causalRootReserved);
    WS3_READ(causalRootCommitted); WS3_READ(causalRootReservationCanceled);
    WS3_READ(causalRootAlreadyClaimed); WS3_READ(causalRootMissing);
    WS3_READ(causalRootNonWorldLocal); WS3_READ(causalRootGroupOverflow);
    WS3_READ(causalRootSuppressedSiblings); WS3_READ(causalRootProbeStandalone);
    WS3_READ(causalBoundaryProfileMissing); WS3_READ(causalBoundaryNoWorldEffect);
    WS3_READ(causalBoundaryPlayerDependent); WS3_READ(causalBoundaryWorldCandidate);
    WS3_READ(causalBoundaryTruncated); WS3_READ(causalBoundaryUnknownDependency);
    WS3_READ(parentContextBound); WS3_READ(parentContextBoundLocalRoot);
    WS3_READ(parentContextBoundRemoteRoot); WS3_READ(parentContextInheritedOrigin);
    WS3_READ(parentContextInheritedCausal); WS3_READ(parentContextInheritedCause);
    WS3_READ(parentContextReleased); WS3_READ(parentContextStale);
    WS3_READ(parentContextOverflow);
    result.parentContextsLive = gLiveParentContexts.load(std::memory_order_acquire);
    result.parentContextsPeak = gStats.parentContextsLivePeak.load(std::memory_order_relaxed);
    WS3_READ(lineageGuardSuppressed);
    WS3_READ(probeCallbackExceptions); WS3_READ(probeCausalUnknownExceptions);
    WS3_READ(rootRateRejected);
    WS3_READ(authorityRetailPreserved);
    WS3_READ(authorityLocalSuppressed);
    WS3_READ(authorityAwaitSubmitted);
    WS3_READ(authorityAutonomousSuppressed);
    WS3_READ(causalBoundaryBranching);
    WS3_READ(emittedOutputForwardingRejected);
    WS3_READ(carrierObserved);
    WS3_READ(carrierNetworkEligible);
    WS3_READ(carrierForwardingComplete);
    WS3_READ(carrierSelectorResolved);
    WS3_READ(carrierSelectorRejected);
    WS3_READ(carrierContextCaptured);
    WS3_READ(carrierManifestAdmitted);
    WS3_READ(carrierPreBusPreserved);
    WS3_READ(carrierSubmitAttempt);
    WS3_READ(carrierSubmitSuccess);
    WS3_READ(scheduledCoveredByCarrier);
    WS3_READ(scheduledCoveredByCarrierSuppressed);
    WS3_READ(compositeCarrierObserved);
    WS3_READ(compositeCarrierNetworkEligible);
    WS3_READ(compositeCarrierTargetAdmitted);
    WS3_READ(compositeCarrierManifestAdmitted);
    WS3_READ(compositeCarrierPreBusPreserved);
    WS3_READ(compositeCarrierSubmitAttempt);
    WS3_READ(compositeCarrierSubmitSuccess);
    WS3_READ(scheduledStateTransitionNoCarrier);
    WS3_READ(scheduledStateTransitionQuarantinedNoCarrier);
    WS3_READ(scheduledInputCertificationRejected);
    WS3_READ(callbackBridgeScopes);
    WS3_READ(callbackBridgeSubmitted);
    WS3_READ(callbackBridgeOwnerRejected);
    WS3_READ(callbackBridgeSameInputRejected);
    WS3_READ(callbackBridgeAlreadyClaimedRejected);
    WS3_READ(callbackBridgeRateRejected);
    WS3_READ(callbackBridgeQueueRejected);
    WS3_READ(relevanceCandidates);
    WS3_READ(relevanceReplicateIntrinsic);
    WS3_READ(relevanceReplicateCarrier);
    WS3_READ(relevanceDefer);
    WS3_READ(relevanceNotRelevant);
    WS3_READ(relevanceUnknown);
    WS3_READ(relevanceReasonRouting);
    WS3_READ(relevanceReasonSchedulerMaintenance);
    WS3_READ(relevanceReasonInternalState);
    WS3_READ(relevanceReasonNoSink);
    WS3_READ(relevanceReasonBetterDescendant);
    WS3_READ(relevanceReasonCycleAmbiguous);
    WS3_READ(relevanceReasonHistorySensitive);
    WS3_READ(relevanceReasonStateTransitionCarrier);
    WS3_READ(relevanceReasonCompositeStateTransitionCarrier);
    WS3_READ(relevanceReasonCompositeRelevantFrontierRelaxed);
    WS3_READ(relevanceInboundRejected);
    WS3_READ(installFailures);
    WS3_READ(identityRuntimeKeyRejected); WS3_READ(identityRegistryLookupRejected);
    WS3_READ(identityBoundaryLayoutRejected); WS3_READ(probeIdentityBoundaryLayoutRejected);
    WS3_READ(identityBoundaryOwnerValid); WS3_READ(probeIdentityBoundaryOwnerValid);
    WS3_READ(probeDirectRootsSent);
    WS3_READ(identityPublicInputRejected); WS3_READ(identityDefinitionGetterRejected);
    WS3_READ(identityDirectOwnerValid); WS3_READ(identityInterfaceMapValid); WS3_READ(identityInterfaceMapRejected);
    WS3_READ(probeIdentityDirectOwnerValid); WS3_READ(probeIdentityInterfaceMapValid);
    WS3_READ(probeIdentityInterfaceMapRejected);
    WS3_READ(identityDefinitionNullRejected); WS3_READ(identityDefinitionDispatchRejected);
    WS3_READ(identityDefinitionFaultRejected); WS3_READ(identityDefinitionSelfRejected);
    WS3_READ(probeIdentityRuntimeKeyRejected); WS3_READ(probeIdentityRegistryLookupRejected);
    WS3_READ(probeIdentityPublicInputRejected); WS3_READ(probeIdentityDefinitionGetterRejected);
    WS3_READ(probeIdentityDefinitionNullRejected); WS3_READ(probeIdentityDefinitionDispatchRejected);
    WS3_READ(probeIdentityDefinitionFaultRejected); WS3_READ(probeIdentityDefinitionSelfRejected);
    result.identityCurrent = gStats.identityCurrent.Snapshot();
    result.identitySelf = gStats.identitySelf.Snapshot();
    result.probeIdentityCurrent = gStats.probeIdentityCurrent.Snapshot();
    result.probeIdentitySelf = gStats.probeIdentitySelf.Snapshot();
    result.identityOwner = gStats.identityOwner.Snapshot();
    result.probeIdentityOwner = gStats.probeIdentityOwner.Snapshot();
    {
        std::lock_guard lock(gProbeIdentityMutex);
        result.probeIdentityLast = gProbeIdentityLast;
    }
    {
        std::lock_guard lock(gRelevanceProducerMutex);
        std::array<const RelevanceProducerSlot*, kRelevanceProducerCapacity> ranked{};
        std::size_t count = 0U;
        for (const auto& slot : gRelevanceProducers)
            if (slot.occupied) ranked[count++] = &slot;
        std::sort(ranked.begin(), ranked.begin() + count,
            [](const RelevanceProducerSlot* left, const RelevanceProducerSlot* right) {
                return left->count > right->count;
            });
        const auto outputCount = (std::min)(count, result.relevanceTop.size());
        result.relevanceTopCount = static_cast<std::uint32_t>(outputCount);
        for (std::size_t index = 0U; index < outputCount; ++index) {
            const auto& slot = *ranked[index];
            result.relevanceTop[index] = RelevanceProducerDiagnostics{
                slot.count, slot.owner, slot.shortGuid, slot.primitiveType,
                slot.replicationRelevance, slot.rootKind};
        }
    }
#undef WS3_READ
    return result;
}

std::array<ContextTrace, 64U> ContextTraceSnapshot() noexcept {
    std::lock_guard lock(gTraceMutex);
    std::array<ContextTrace, 64U> result{};
    const auto count = (std::min)(gTraceSerial, static_cast<std::uint64_t>(result.size()));
    for (std::uint64_t i = 0; i < count; ++i)
        result[static_cast<std::size_t>(i)] = gContextTrace[static_cast<std::size_t>((gTraceSerial - count + i) % result.size())];
    return result;
}

void SetRemoteCauseEnvelope(std::uint64_t sourcePeer, std::uint64_t sequence) noexcept {
    gRemotePeer = sourcePeer;
    gRemoteSequence = sequence;
}

void RecordDependencyPending(const world_interactions::CompositePublicInputEvent& event,
    const WorldEntityId& owner, std::uint64_t peer, std::uint64_t sequence,
    std::uint8_t reason) noexcept {
    ContextTrace trace{};
    trace.cause = event.observationId;
    trace.sourcePeer = peer;
    trace.authoritySequence = sequence;
    trace.owner = owner;
    trace.target = event.targetEntity;
    trace.actor = event.actorEntity;
    trace.actorKind = static_cast<std::uint8_t>(event.actorKind);
    trace.contextRuntimeKey = event.contextFrameCount != 0U
        ? event.contextFrames[0].runtimeKey : CathodeRuntimeKey{};
    trace.parentInvocation = event.contextFrameCount != 0U
        ? event.contextFrames[0].invocation : 0U;
    trace.contextKind = static_cast<std::uint8_t>(event.contextKind);
    trace.input = event.publicInputShortGuid;
    trace.epoch = gEpoch.load(std::memory_order_relaxed);
    trace.pendingReason = reason; // 1 owner, 2 target, 3 context, 4 actor
    gStats.contextPending.fetch_add(1U, std::memory_order_relaxed);
    TraceContext(trace, 8U);
}

struct PreparedPublicRoot final {
    world_interactions::CompositePublicInputEvent event{};
    WorldEntityId owner{};
    WorldEntityLease ownerLease, targetLease, actorLease;
    CathodeRuntimeKey typedSourceRuntimeKey{}, typedTargetRuntimeKey{};
    cathode_primitives::NativeCallbackLease context;
    ContextTrace cause{};
    PublicFn original{};
    void* definition{};
    WorldEntityRegistry* registry{};
    bool invoked{};
    std::unique_ptr<RootApplyTraceScope> trace;
};

world_interactions::ExecuteResult Prepare(
    const world_interactions::CompositePublicInputEvent& event,
    const WorldEntityId& owner, WorldEntityRegistry& registry,
    std::shared_ptr<PreparedPublicRoot>& output) noexcept {
    using world_interactions::ExecuteStatus;
    if (output || !Installed()) return {ExecuteStatus::RuntimeUnavailable, false};
    std::shared_ptr<PreparedPublicRoot> prepared;
    try { prepared = std::make_shared<PreparedPublicRoot>(); }
    catch (...) { return {ExecuteStatus::RuntimeUnavailable, false}; }
    TraceRootIngress(event, owner);
    auto resolvedOwner = registry.Resolve(owner);
    if (!resolvedOwner.lease) return {resolvedOwner.status == WorldRegistryResolveStatus::NativeFault
        ? ExecuteStatus::NativeFault : ExecuteStatus::RuntimeUnavailable, false};
    prepared->ownerLease = std::move(resolvedOwner.lease);
    const auto& ownerLease = prepared->ownerLease;
    if (!event.retailRoot || !world_interactions::ValidPublicRootContext(event) ||
        !replication_core::RemoteApplyActive() ||
        !PublicRootManifestAdmissionAllowed(registry, owner, event)) {
        gStats.applyRejected.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::InvalidPayload, false};
    }
    try { prepared->trace = std::make_unique<RootApplyTraceScope>(event, owner, registry); }
    catch (...) { return {ExecuteStatus::RuntimeUnavailable, false}; }
    auto& rootTrace = *prepared->trace;
    void* definition{};
    const bool emitted = event.rootKind == world_interactions::PublicRootKind::EmittedOutput;
    const bool scheduled = event.rootKind == world_interactions::PublicRootKind::ScheduledInput;
    const bool schedulerEdge = event.rootKind == world_interactions::PublicRootKind::SchedulerEdge;
    const bool typedMessage = event.rootKind == world_interactions::PublicRootKind::TypedMessage;
    const bool canonicalComposite = event.rootKind == world_interactions::PublicRootKind::CanonicalCompositeInput;
    const bool doorLockState = event.rootKind == world_interactions::PublicRootKind::DoorLockState;
    if (!scheduled && !schedulerEdge && !typedMessage && !canonicalComposite && !doorLockState && (emitted ? !OutputDefinition(ownerLease.Get(), event.interfaceSelector, definition)
                : !PublicDefinition(ownerLease.Get(), definition))) {
        gStats.applyRejected.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::InvalidPayload, false};
    }
    if (!emitted && !scheduled && !schedulerEdge && !typedMessage && !canonicalComposite && !doorLockState) {
        void* boundaryOwner{};
        IdentityFailure boundaryFailure{};
        if (!ReadBoundaryOwner(definition, boundaryOwner, boundaryFailure) || boundaryOwner != ownerLease.Get())
            return {ExecuteStatus::InvalidPayload, false};
    }
    const auto original = (schedulerEdge || typedMessage || canonicalComposite || doorLockState) ? nullptr : reinterpret_cast<PublicFn>((emitted ? gOutputTrampoline : gPublicTrampoline)
        .load(std::memory_order_acquire));
    if (!schedulerEdge && !typedMessage && !canonicalComposite && !doorLockState && original == nullptr) return {ExecuteStatus::RuntimeUnavailable, false};

    // Keep both local references alive through retail fan-out. A missing target
    // never substitutes the owner and no partial invocation occurs.
    WorldRegistryResolveResult resolvedTarget{};
    const WorldEntityLease* targetLease = &ownerLease;
    if (!(event.targetEntity == owner)) {
        resolvedTarget = registry.Resolve(event.targetEntity);
        if (!resolvedTarget.lease) {
            gStats.applyRejected.fetch_add(1U, std::memory_order_relaxed);
            return {resolvedTarget.status == WorldRegistryResolveStatus::NativeFault
                ? ExecuteStatus::NativeFault : ExecuteStatus::RuntimeUnavailable, false};
        }
        targetLease = &resolvedTarget.lease;
    }
    // Check the live bindings again: streaming/reuse must not turn a valid
    // manifest key into the wrong local execution object.
    const auto liveOwner = registry.IdentifyRuntimeEntity(ownerLease.Get());
    const auto liveTarget = registry.IdentifyRuntimeEntity(targetLease->Get());
    if (!liveOwner || !liveTarget || !(*liveOwner == owner) || !(*liveTarget == event.targetEntity)) {
        gStats.applyRejected.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::InvalidPayload, false};
    }
    if (typedMessage &&
        (!registry.TryExtractRuntimeKey(ownerLease.Get(), prepared->typedSourceRuntimeKey) ||
         !registry.TryExtractRuntimeKey(targetLease->Get(), prepared->typedTargetRuntimeKey) ||
         prepared->typedSourceRuntimeKey.entityA == 0U ||
         prepared->typedTargetRuntimeKey.entityA == 0U)) {
        gStats.applyRejected.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::InvalidPayload, false};
    }

    ContextTrace cause{};
    cause.cause = event.observationId;
    cause.rootKind = static_cast<std::uint8_t>(event.rootKind);
    cause.sourcePeer = gRemotePeer;
    cause.authoritySequence = gRemoteSequence;
    cause.causalChainId = event.causalSerial;
    cause.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
    cause.parentInvocation = event.contextFrameCount != 0U
        ? event.contextFrames[0].invocation : 0U;
    cause.owner = owner;
    cause.target = event.targetEntity;
    cause.actor = event.actorEntity;
    cause.actorKind = static_cast<std::uint8_t>(event.actorKind);
    cause.contextKind = static_cast<std::uint8_t>(event.contextKind);
    cause.contextRuntimeKey = event.contextFrameCount != 0U
        ? event.contextFrames[0].runtimeKey : CathodeRuntimeKey{};
    cause.input = event.publicInputShortGuid;
    cause.epoch = gEpoch.load(std::memory_order_relaxed);
    TraceContext(cause, 3U);

    WorldRegistryResolveResult actorResolved{};
    if (event.actorKind == world_interactions::PublicActorKind::WorldEntity) {
        const auto domain = registry.FindWorldEntityDomain(event.actorEntity);
        if (domain != WorldDomain::World && domain != WorldDomain::AI)
            return {ExecuteStatus::InvalidPayload, false};
        actorResolved = registry.Resolve(event.actorEntity);
        if (!actorResolved.lease) return {actorResolved.status == WorldRegistryResolveStatus::NativeFault
            ? ExecuteStatus::NativeFault : ExecuteStatus::RuntimeUnavailable, false};
        const auto liveActor = registry.IdentifyRuntimeEntity(actorResolved.lease.Get());
        if (!liveActor || !(*liveActor == event.actorEntity)) return {ExecuteStatus::InvalidPayload, false};
    }
    // SourcePlayer identifies the authenticated sender for attribution only.
    // It is never substituted into the callback context until a real peer ->
    // remote native player binding exists. ROOT v9 reconstructs the complete
    // retained WorkItem ancestry from the oldest ancestor back to frame 0.
    cathode_primitives::NativeCallbackLease parent;
    if (event.contextKind == world_interactions::PublicContextKind::NativeContextChain) {
        rootTrace.Stage(RootApplyTraceStage::RebuildContext);
        for (std::size_t reverse = event.contextFrameCount; reverse != 0U; --reverse) {
            const auto frameIndex = reverse - 1U;
            const auto& frame = event.contextFrames[frameIndex];
            bool nativeFault = false;
            auto contextResolved = registry.ResolveRuntimeReference(frame.runtimeKey, nativeFault);
            if (!contextResolved) {
                if (!nativeFault) gStats.contextPending.fetch_add(1U, std::memory_order_relaxed);
                TraceContext(cause, nativeFault ? 9U : 8U);
                return {nativeFault ? ExecuteStatus::NativeFault : ExecuteStatus::RuntimeUnavailable, false};
            }
            // Never map a sender's local-player singleton to the receiver's
            // local player. A real sourcePeer -> remote-player native binding
            // is still required before Player frames can be portable.
            if (registry.IdentifyRuntimeEntityDomain(contextResolved.Get()) == WorldDomain::Player)
                return {ExecuteStatus::InvalidPayload, false};
            CathodeRuntimeKey liveKey{};
            if (!registry.TryExtractRuntimeKey(contextResolved.Get(), liveKey) ||
                !(liveKey == frame.runtimeKey)) return {ExecuteStatus::InvalidPayload, false};

            const auto payloadEnd = static_cast<std::size_t>(frame.payloadOffset) + frame.payloadSize;
            if (payloadEnd > event.contextPayloadSize)
                return {ExecuteStatus::InvalidPayload, false};
            const auto framePayload = std::span<const std::byte>(
                event.contextPayload.data() + frame.payloadOffset, frame.payloadSize);
            const void* expectedParent = parent.Get();
            cathode_primitives::NativeCallbackLease built;
            const auto status = cathode_primitives::CreateNativeContextFrame(
                contextResolved, frame.inputShortGuid, frame.constructorFlags,
                frame.descriptorDelayBits, framePayload,
                parent.Get() != nullptr ? &parent : nullptr, built);
            if (status != cathode_primitives::ExecuteStatus::Queued)
                return {status == cathode_primitives::ExecuteStatus::RuntimeUnavailable
                    ? ExecuteStatus::RuntimeUnavailable : status == cathode_primitives::ExecuteStatus::NativeFault
                        ? ExecuteStatus::NativeFault : ExecuteStatus::InvalidPayload, false};

            void* rebuiltExecution{};
            void* rebuiltParent{};
            std::uint32_t rebuiltInput{};
            std::uint32_t rebuiltFlags{};
            std::uint16_t rebuiltPayloadBytes{};
            const std::byte* rebuiltPayload{};
            std::uint32_t rebuiltDelayBits{};
            if (!ReadContextFrame(built.Get(), cause, rebuiltExecution, rebuiltParent,
                    rebuiltInput, rebuiltFlags, rebuiltPayloadBytes, rebuiltPayload,
                    &rebuiltDelayBits) ||
                rebuiltExecution != contextResolved.Get() ||
                rebuiltParent != expectedParent ||
                rebuiltInput != frame.inputShortGuid ||
                rebuiltFlags != frame.constructorFlags ||
                rebuiltDelayBits != frame.descriptorDelayBits ||
                rebuiltPayloadBytes != frame.payloadSize ||
                (rebuiltPayloadBytes != 0U &&
                    (rebuiltPayload == nullptr || !std::equal(rebuiltPayload,
                        rebuiltPayload + rebuiltPayloadBytes, framePayload.begin())))) {
                TraceContext(cause, 9U);
                gStats.nativeFaults.fetch_add(1U, std::memory_order_relaxed);
                return {ExecuteStatus::NativeFault, false};
            }

            // The newly built child has retained its parent through retail
            // FUN_00926D20. Release only our previous constructor owner.
            if (!parent.Reset()) {
                TraceContext(cause, 9U);
                gStats.nativeFaults.fetch_add(1U, std::memory_order_relaxed);
                return {ExecuteStatus::NativeFault, false};
            }
            parent = std::move(built);
            gStats.contextFramesReconstructed.fetch_add(1U, std::memory_order_relaxed);
        }

        gStats.contextReconstructed.fetch_add(1U, std::memory_order_relaxed);
        TraceContext(cause, 4U);
        rootTrace.Stage(RootApplyTraceStage::ContextReady);
        Tag(parent.Get(), Origin::RemoteRoot, cause.epoch);
        auto parentCause = cause;
        parentCause.invocation = gInvocationSerial.fetch_add(1U, std::memory_order_relaxed);
        parentCause.parentInvocation = event.contextFrameCount > 1U
            ? event.contextFrames[1].invocation : 0U;
        StoreTaskCause(parent.Get(), parentCause, cause.epoch);
        BindParentContext(parent.Get(), Origin::RemoteRoot, {}, parentCause, cause.epoch);
    }
    prepared->event = event;
    prepared->owner = owner;
    prepared->registry = &registry;
    prepared->definition = definition;
    prepared->original = original;
    prepared->targetLease = std::move(resolvedTarget.lease);
    prepared->actorLease = std::move(actorResolved.lease);
    prepared->context = std::move(parent);
    prepared->cause = cause;
    output = std::move(prepared);
    return {ExecuteStatus::Queued, false}; // prepared, no gameplay invoked
}

world_interactions::ExecuteResult ExecutePrepared(PreparedPublicRoot& prepared) noexcept {
    using world_interactions::ExecuteStatus;
    if (prepared.invoked || !replication_core::RemoteApplyActive() || !Installed() ||
        prepared.cause.epoch != gEpoch.load(std::memory_order_acquire))
        return {ExecuteStatus::InvalidPayload, false};
    prepared.invoked = true; // a possibly partial invocation is never retryable
    const auto& event = prepared.event;
    const auto& ownerLease = prepared.ownerLease;
    const auto* targetLease = event.targetEntity == prepared.owner ? &ownerLease : &prepared.targetLease;
    const auto original = prepared.original;
    void* definition = prepared.definition;
    const bool scheduled = event.rootKind == world_interactions::PublicRootKind::ScheduledInput;
    const bool schedulerEdge = event.rootKind == world_interactions::PublicRootKind::SchedulerEdge;
    const bool typedMessage = event.rootKind == world_interactions::PublicRootKind::TypedMessage;
    const bool canonicalComposite = event.rootKind == world_interactions::PublicRootKind::CanonicalCompositeInput;
    const bool doorLockState = event.rootKind == world_interactions::PublicRootKind::DoorLockState;
    auto& parent = prepared.context;
    auto& cause = prepared.cause;
    auto& rootTrace = *prepared.trace;
    rootTrace.Stage(RootApplyTraceStage::InjectExecute);
    if (doorLockState) {
        struct DoorLockReplay final {
            const world_interactions::CompositePublicInputEvent* event{};
            const WorldEntityId* source{};
            const WorldEntityLease* sourceLease{};
            const WorldEntityLease* targetLease{};
            world_animated::ExecuteResult result{};
        } replay{&event, &prepared.owner, &ownerLease, targetLease, {}};
        const auto invoke = [](void* opaque) noexcept {
            auto& call = *static_cast<DoorLockReplay*>(opaque);
            call.result = world_animated::ExecuteDoorLockState(
                call.event->publicInputShortGuid, *call.source, *call.sourceLease,
                call.event->targetEntity, *call.targetLease);
        };
        // Reuse the retail scope that marks RemoteRoot and disables scheduler
        // sidecar capture. The Door lock-state root is already the authoritative
        // semantic boundary; descendants must remain local to this replay.
        InvokeSchedulerEdgeRetailScoped(Origin::RemoteRoot, cause, invoke, &replay);
        const auto& applied = replay.result;
        rootTrace.Stage(RootApplyTraceStage::InvokeReturned, applied.queued ? 1U : 0U);
        rootTrace.Stage(RootApplyTraceStage::ReleaseContext);
        if (applied.status != world_animated::ExecuteStatus::Queued || !applied.queued) {
            if (applied.status == world_animated::ExecuteStatus::NativeFault)
                gStats.nativeFaults.fetch_add(1U, std::memory_order_relaxed);
            return {applied.status == world_animated::ExecuteStatus::RuntimeUnavailable
                    ? ExecuteStatus::RuntimeUnavailable
                    : applied.status == world_animated::ExecuteStatus::NativeFault
                        ? ExecuteStatus::NativeFault : ExecuteStatus::InvalidPayload,
                false};
        }
        gStats.applied.fetch_add(1U, std::memory_order_relaxed);
        gStats.doorLockStateRootsApplied.fetch_add(1U, std::memory_order_relaxed);
        rootTrace.Complete();
        return {ExecuteStatus::Queued, true};
    }
    if (canonicalComposite) {
        if (prepared.registry == nullptr) return {ExecuteStatus::RuntimeUnavailable, false};
        struct CanonicalReplay final {
            const world_interactions::CompositePublicInputEvent* event{};
            const WorldEntityId* owner{};
            const WorldEntityLease* ownerLease{};
            WorldEntityRegistry* registry{};
            world_interactions::ExecuteResult result{};
        } replay{&event, &prepared.owner, &ownerLease, prepared.registry, {}};
        const auto invoke = [](void* opaque) noexcept {
            auto& call = *static_cast<CanonicalReplay*>(opaque);
            call.result = world_interactions::ExecuteCanonicalCompositePublicInput(
                *call.event, *call.owner, *call.ownerLease, *call.registry);
        };
        InvokeCanonicalCompositeRetailScoped(Origin::RemoteRoot, cause, invoke, &replay);
        rootTrace.Stage(RootApplyTraceStage::InvokeReturned, replay.result.queued ? 1U : 0U);
        rootTrace.Stage(RootApplyTraceStage::ReleaseContext);
        if (replay.result.status != ExecuteStatus::Queued || !replay.result.queued)
            return replay.result;
        gStats.applied.fetch_add(1U, std::memory_order_relaxed);
        gStats.canonicalCompositeRootsApplied.fetch_add(1U, std::memory_order_relaxed);
        rootTrace.Complete();
        return replay.result;
    }
    if (typedMessage) {
        struct TypedReplay final {
            const world_interactions::CompositePublicInputEvent* event{};
            CathodeRuntimeKey sourceKey{};
            CathodeRuntimeKey targetKey{};
            cathode_primitives::ExecuteResult result{};
        } replay{&event, prepared.typedSourceRuntimeKey, prepared.typedTargetRuntimeKey, {}};
        const auto invoke = [](void* opaque) noexcept {
            auto& call = *static_cast<TypedReplay*>(opaque);
            call.result = cathode_primitives::ExecuteTypedMessage(
                call.event->publicInputShortGuid,
                std::span<const std::byte>(call.event->contextPayload.data(),
                    call.event->contextPayloadSize),
                call.event->relativeDelaySeconds,
                call.targetKey, call.sourceKey);
        };
        InvokeTypedMessageRetailScoped(Origin::RemoteRoot, cause, invoke, &replay);
        const auto& queued = replay.result;
        rootTrace.Stage(RootApplyTraceStage::InvokeReturned, queued.queued ? 1U : 0U);
        rootTrace.Stage(RootApplyTraceStage::ReleaseContext);
        if (queued.status != cathode_primitives::ExecuteStatus::Queued || !queued.queued) {
            if (queued.status == cathode_primitives::ExecuteStatus::NativeFault)
                gStats.nativeFaults.fetch_add(1U, std::memory_order_relaxed);
            return {queued.status == cathode_primitives::ExecuteStatus::RuntimeUnavailable
                    ? ExecuteStatus::RuntimeUnavailable
                    : queued.status == cathode_primitives::ExecuteStatus::NativeFault
                        ? ExecuteStatus::NativeFault : ExecuteStatus::InvalidPayload,
                false};
        }
        gStats.applied.fetch_add(1U, std::memory_order_relaxed);
        gStats.typedMessageRootsApplied.fetch_add(1U, std::memory_order_relaxed);
        rootTrace.Complete();
        return {ExecuteStatus::Queued, true};
    }
    if (schedulerEdge) {
        // A preceding higher RemoteRoot may already have regenerated this exact
        // scheduler edge through retail. In that case the sidecar is still an
        // authoritative ordered member, but replaying it again would duplicate
        // the visible effect. Consume exactly one sequence-qualified witness and
        // commit the ROOT without invoking QueueInput a second time.
        if (ConsumeRemoteNaturalSchedulerEdge(
                prepared.owner, event.targetEntity, event.schedulerParameterShortGuid,
                event.publicInputShortGuid, event.relativeDelaySeconds)) {
            rootTrace.Stage(RootApplyTraceStage::InvokeReturned, 2U);
            rootTrace.Stage(RootApplyTraceStage::ReleaseContext);
            gStats.applied.fetch_add(1U, std::memory_order_relaxed);
            rootTrace.Complete();
            return {ExecuteStatus::Queued, true};
        }

        // Recreate the edge through retail while RemoteRoot is active. The
        // scheduler constructs receiver-local WorkItems during this call; the
        // scope lets WorkBuildDetour persist RemoteRoot ownership onto them so
        // delayed descendants cannot echo back after RemoteApplyScope unwinds.
        struct SchedulerReplay final {
            const world_interactions::CompositePublicInputEvent* event{};
            const WorldEntityLease* target{};
            const WorldEntityLease* source{};
            cathode_primitives::ExecuteResult result{};
        } replay{&event, targetLease, &ownerLease, {}};
        const auto invoke = [](void* opaque) noexcept {
            auto& call = *static_cast<SchedulerReplay*>(opaque);
            call.result = cathode_primitives::ExecuteInputEdge(
                call.event->schedulerParameterShortGuid,
                call.event->publicInputShortGuid,
                static_cast<float>(call.event->relativeDelaySeconds),
                *call.target, *call.source);
        };
        InvokeSchedulerEdgeRetailScoped(Origin::RemoteRoot, cause, invoke, &replay);
        const auto& queued = replay.result;
        rootTrace.Stage(RootApplyTraceStage::InvokeReturned, queued.queued ? 1U : 0U);
        rootTrace.Stage(RootApplyTraceStage::ReleaseContext);
        if (queued.status != cathode_primitives::ExecuteStatus::Queued || !queued.queued) {
            if (queued.status == cathode_primitives::ExecuteStatus::NativeFault)
                gStats.nativeFaults.fetch_add(1U, std::memory_order_relaxed);
            return {queued.status == cathode_primitives::ExecuteStatus::RuntimeUnavailable
                    ? ExecuteStatus::RuntimeUnavailable
                    : queued.status == cathode_primitives::ExecuteStatus::NativeFault
                        ? ExecuteStatus::NativeFault : ExecuteStatus::InvalidPayload,
                false};
        }
        gStats.applied.fetch_add(1U, std::memory_order_relaxed);
        rootTrace.Complete();
        return {ExecuteStatus::Queued, true};
    }
    const bool invoked = scheduled
        ? cathode_primitives::ExecuteRetainedInput(ownerLease, *targetLease, parent)
        : InvokeRemote(original, definition, WorldPublicActionLeaseAccess::SharedRef(*targetLease),
            event.publicInputShortGuid, event.relativeDelaySeconds, parent.Get(), cause);
    rootTrace.Stage(RootApplyTraceStage::InvokeReturned, invoked ? 1U : 0U);
    rootTrace.Stage(RootApplyTraceStage::ReleaseContext);
    const bool released = parent.Reset(); // native descendants retain their own references
    if (!invoked || !released) {
        TraceContext(cause, 9U);
        gStats.nativeFaults.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::NativeFault, false}; // Never retry a possibly partial invocation.
    }
    gStats.applied.fetch_add(1U, std::memory_order_relaxed);
    if ((event.flags & world_interactions::kPublicRootDetachedCallback) != 0U)
        gStats.callbackBoundaryApplied.fetch_add(1U, std::memory_order_relaxed);
    rootTrace.Complete();
    return {ExecuteStatus::Queued, true};
}
world_interactions::ExecuteResult Execute(
    const world_interactions::CompositePublicInputEvent& event,
    const WorldEntityId& owner, const WorldEntityLease& ownerLease,
    WorldEntityRegistry& registry) noexcept {
    if (!ownerLease) return {world_interactions::ExecuteStatus::RuntimeUnavailable, false};
    std::shared_ptr<PreparedPublicRoot> prepared;
    const auto result = Prepare(event, owner, registry, prepared);
    return prepared ? ExecutePrepared(*prepared) : result;
}

} // namespace world_actions
} // namespace aimp
