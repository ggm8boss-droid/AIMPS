#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/PlayerLocalControls.h"
#include "aimp/LocalObservationLedger.h"
#include "aimp/SelfClockedRootFilter.h"
#include "aimp/Diagnostics.h"
#include "aimp/RuntimeFeatureFlags.h"

#include "aimp/BuildProfile.h"
#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/HookManager.h"
#include "aimp/ReplicationCore.h"
#include "aimp/SignatureResolver.h"
#include "aimp/WorldEntityRegistry.h"
#include "aimp/HidingDoorPresentation.h"

#include <Windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <mutex>
#include <limits>
#include <utility>
#include <vector>

namespace aimp {

// Narrow lifetime bridge, matching the v12.5 primitive bridge. It is private to
// this translation unit; adapters cannot publish or cache shared refs elsewhere.
struct WorldInteractionLeaseAccess final {
    [[nodiscard]] static void* SharedRef(const WorldEntityLease& lease) noexcept {
        return lease.sharedRef_;
    }
};

namespace world_interactions {
namespace {

// STATIC CONFIRMED against the supplied retail AI.exe. IMPORTANT ABI NOTE:
// FUN_009271C0 ends in `ret 0x1C` and retail call-sites push 28 bytes. The
// trailing 32-bit argument is not referenced by the function body, but it is
// part of the x86 stack-cleanup contract. Omitting it corrupts ESP on every
// trampoline/original call. Keep it in both the detour and replay typedefs.
using QueueInputFn = void (__thiscall*)(
    void* manager,
    void** sourceSharedRef,
    const std::uint32_t* inputShortGuid,
    void* targetDescriptor,
    void* callbackRecord,
    double delaySeconds,
    std::uint32_t retailTailArgument);
using QueueTypedMessageFn = void (__thiscall*)(
    void* manager,
    const CathodeRuntimeKey* sourceKey,
    const CathodeRuntimeKey* targetKey,
    std::uint32_t messageType,
    std::size_t payloadSize,
    void* payloadData,
    double delaySeconds);
// STATIC CONFIRMED from retail call-sites (notably FUN_00930640):
// FUN_00926FF0 is a manager __thiscall with five stack arguments.
using BuildCallbackValueFn = void** (__thiscall*)(
    void* manager,
    void** destination,
    void** sourceSharedRef,
    std::uint32_t constructorFlags,
    std::size_t payloadSize,
    void* payloadData);
// STATIC CONFIRMED and already used by CathodePrimitiveExecutor:
// FUN_00921F70 constructs the 12-byte input-target descriptor from a retained
// target shared-ref, input ShortGuid and delay. Fix3.27 hooks this constructor
// before QueueInput without changing the retail ABI.
using BuildInputTargetFn = void* (__thiscall*)(
    void* targetDescriptor,
    void** targetSharedRef,
    const std::uint32_t* inputShortGuid,
    double delaySeconds);
using LogicSwitchInputFn = std::uint8_t (__thiscall*)(void*, void*, void*);
// STATIC CONFIRMED from the supplied AI.exe.c export for the Fix3.21
// candidate publication family: all five helpers are __thiscall void functions
// receiving (this, int* context, undefined1* value). The pointed value type is
// deliberately left uninterpreted.
using CompositeStateValueFn = void (__thiscall*)(void*, int*, std::uint8_t*);
// STATIC CONFIRMED from ai_exe_c_functions.tsv: FUN_009657F0 is
// `void __fastcall FUN_009657f0(undefined4 *param_1)`. Fix3.22 redirects only
// the single direct retail call from FUN_00965A90 and invokes this ABI intact.
using CompositeExecutionFn = void (__fastcall*)(std::uint32_t*);

struct RetailInputTarget final {
    std::uint32_t inputShortGuid{};
    float delaySeconds{};
    void* targetSharedRef{};
};

struct RuntimeWorldIdCacheSlot final {
    std::atomic<std::uintptr_t> execution{0U};
    std::atomic<std::uint64_t> high{0U};
    std::atomic<std::uint64_t> low{0U};
};

struct CompositeInputSemanticSlot final {
    WorldEntityId owner{};
    std::uint32_t publicInputShortGuid{};
    std::uint64_t lastLocalTickMs{};
    std::uint64_t localChainGuardUntilTickMs{};
    std::uint64_t remoteGuardUntilTickMs{};
};

static_assert(sizeof(void*) == 4, "World interaction adapters require the x86 retail ABI.");
static_assert(sizeof(RetailInputTarget) == 12U, "Unexpected CATHODE input target layout.");

constexpr std::size_t kSharedObjectOffset = 0x0CU;
constexpr std::ptrdiff_t kLogicSwitchNativeOffset = 0x14;
constexpr std::ptrdiff_t kLogicSwitchStateOffset = 0x0C;
constexpr std::size_t kEchoSlots = 1024U;
constexpr std::size_t kEchoProbeCount = 8U;
// STATIC CONFIRMED from retail strings + COMMANDS/OpenCAGE witness graph. This
// is a generic LogicSwitch input ShortGuid, not Door/Unlock semantics.
constexpr std::uint32_t kLogicSwitchSetFalseInputShortGuid = 0xD2B3CC2BU;
constexpr std::size_t kCompositeInputSemanticSlots = 64U;
constexpr std::uint64_t kCompositeInputLocalDedupeMs = 16U;
constexpr std::uint64_t kCompositeInputLocalChainGuardMs = 500U;
constexpr std::uint64_t kCompositeInputRemoteGuardMs = 500U;
constexpr std::uint32_t kCompositeInputMaximumSubmissionsPerSecond = 256U;
constexpr std::size_t kCompositeInputMaximumBridgeFanout = 256U;
constexpr std::size_t kLogicSwitchBindingCapacity = 16384U;
constexpr std::size_t kLogicSwitchBindingProbeCount = 32U;
constexpr std::uint64_t kLogicSwitchReplayContextMaximumAgeMs = 250U;
constexpr std::size_t kSchedulerCausalHistoryCapacity = 256U;
constexpr std::uint64_t kSchedulerCausalCorrelationWindowMs = 2000U;
constexpr std::uint64_t kCallbackClassificationProbeWindowMs = 5000U;
constexpr std::uint32_t kSchedulerCausalCallbackFlag = 0x1U;
constexpr std::uint32_t kSchedulerCausalCallbackExecutionReadableFlag = 0x2U;
constexpr std::uint32_t kSchedulerCausalCallbackWorldEntityFlag = 0x4U;
constexpr std::uint32_t kSchedulerCausalCallbackSameSourceFlag = 0x8U;
constexpr std::uint32_t kSchedulerCausalCallbackSameTargetFlag = 0x10U;
constexpr std::uint32_t kSchedulerCausalCallbackRawReadableFlag = 0x20U;
constexpr std::uint32_t kSchedulerCausalSourceRuntimeKeyFlag = 0x40U;
constexpr std::uint32_t kSchedulerCausalTargetRuntimeKeyFlag = 0x80U;
constexpr std::uint32_t kSchedulerCausalCallbackRuntimeKeyFlag = 0x100U;
constexpr std::uint32_t kSchedulerCausalCallbackExecutionPresentFlag = 0x200U;
constexpr std::size_t kCallbackRawWordCount = 10U;

// Fix3.8 diagnostic-only CATHODE runtime-key snapshot probe.
//
// STATIC CONFIRMED from the supplied retail AI.exe:
// - work-item +0x08 is a ref-counted CATHODE shared reference (FUN_009265C0
//   increments its shared refcount; FUN_004BF890 releases it);
// - the object behind a shared reference lives at shared+0x0C.
//
// RUNTIME CONFIRMED by Fix3.7 on the supplied Host diagnostic:
// - the temporal pair still produced stateMatchKind=4 and selected the same
//   LogicSwitch CLEAR state BCA779...0D50;
// - 232 live identity attempts produced no WorldEntityId for callback +0x08;
// - causally selected callback flags varied (0x40000000 and 0x04000000), while
//   the final tested door CLEAR selected 0x04000000. Flags therefore remain a
//   classifier, not a production gameplay rule.
//
// Fix3.8 asks the existing retail runtime-key extractor for (entityA, entityB)
// while source/target/callback objects are still owned by retail. It does this
// for callback-shaped pair matches regardless of the observed flag variant,
// under the same hard per-window cap. Only scalar keys are copied to the causal
// ring. No raw pointer survives the retail call and callback replay stays off.
constexpr std::uint32_t kCallbackAbiProbeCallerRva = 0x004BE280U;
constexpr std::uint32_t kCallbackTemporalProbeScheduler = 0x8FECEA29U; // Fix3.5/Fix3.6 runtime diagnostic only
constexpr std::uint32_t kCallbackTemporalProbeTargetInput = 0xD9D0CA29U; // Fix3.5/Fix3.6 runtime diagnostic only
constexpr std::uint32_t kCallbackWorkItemInputShortGuid = 0x6575CA9BU; // ShortGuid("callback")
// Fix3.11 diagnostic-only exact target. Fix3.8 proved that this runtime key is
// the LogicSwitch WorldEntity BCA779065D3AA56C94203C7A7FEB0D50 causally
// associated with the locked-door unlock in ChallengeMap16. It is a temporary
// microscope only: no networking/replay branch depends on this key.
constexpr std::uint32_t kFix311ExactStateKeyA = 0x18E35849U;
constexpr std::uint32_t kFix311ExactStateKeyB = 0xB3D0A4DEU;
// Fix3.12 diagnostic-only constructor-origin filters, proven by Fix3.11 on
// the exact live contextB callback of the causal unlock LogicSwitch.
constexpr std::uint32_t kFix312CharacterCallbackKeyA = 0x6168BE9CU;
constexpr std::uint32_t kFix312CharacterCallbackKeyB = 0xC3C36E09U;
constexpr std::uint32_t kFix312CharacterCallbackFlags = 0x12000000U;
constexpr std::uint32_t kFix312CharacterExecutionVtableRva = 0x00DD08D8U;
constexpr std::uint64_t kFix320ReplayWindowMs = 30000U;
constexpr std::uint64_t kFix320SemanticMatchLifetimeMs = 5000U;
constexpr std::uint64_t kFix320MaximumSubmissionsPerWindow = 64U;
constexpr std::uint64_t kFix320MaximumExecutionsPerWindow = 64U;
constexpr std::uint32_t kFix312BuildCallbackValueRva = 0x00526FF0U;
constexpr std::uint64_t kFix312OriginProbeWindowMs = 30000U;
// Fix3.13 diagnostic-only typed-message origin filters. Fix3.12 runtime proved
// that FUN_00930640 constructs the exact Character callback with constructor
// flags 0x12000000. STATIC CONFIRMED: FUN_00930640 ORs its incoming message
// flags with 0x10000000 before FUN_00926FF0. That does NOT uniquely prove
// whether the incoming value already carried that bit, so the probe accepts
// only values whose retail OR result is exactly the proven 0x12000000 and
// records the actual incoming value. The exact Character runtime key remains a
// microscope only; no production transport/replay decision consumes it.
constexpr std::uint32_t kFix313CallbackConstructorFlags = 0x12000000U;
constexpr std::uint32_t kFix313RetailInjectedCallbackFlag = 0x10000000U;
constexpr std::uint32_t kFix313TargetCharacterKeyA = 0x6168BE9CU;
constexpr std::uint32_t kFix313TargetCharacterKeyB = 0xC3C36E09U;
constexpr std::uint32_t kFix313QueueTypedMessageRva = 0x00530640U;
constexpr std::uint64_t kFix313OriginProbeWindowMs = 30000U;
// Fix3.14 receiver-side identity microscope. 0x02000000 is RUNTIME CONFIRMED
// by Fix3.13 as the incoming retail type for the current witness family; no
// gameplay meaning is assigned to it and no production replay branch uses it.
constexpr std::uint32_t kFix314WitnessMessageType = 0x02000000U;
constexpr std::uint64_t kFix314RemoteIdentityProbeWindowMs = 30000U;
// Fix3.15 diagnostic-only downstream fingerprint. The exact LogicSwitch key
// remains a temporary microscope; no production networking branch uses it.
constexpr std::uint64_t kFix315DownstreamProbeWindowMs = 30000U;
constexpr std::uint32_t kFix315QueueInputRva = 0x005271C0U;
constexpr std::uint64_t kFix315HashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFix315HashPrime = 1099511628211ULL;
// Fix3.16 diagnostic-only post-CLEAR scheduler microscope. Fix3.15 proved that
// the exact witness CLEAR does not synchronously call QueueInput. For a very
// short bounded window after that exact CLEAR returns, inspect QueueInput calls
// and retain only edges whose source or target RuntimeKey is the exact witness
// LogicSwitch. This is provenance only; no replay/transport branch consumes it.
constexpr std::uint64_t kFix316PostClearWindowMs = 250U;
constexpr std::uint64_t kFix316PostClearInspectLimit = 4096U;
constexpr std::uint64_t kFix316HashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFix316HashPrime = 1099511628211ULL;
// Fix3.17 diagnostic-only retail dispatch target probe. AI.exe.c statically
// shows both LogicSwitch SET and CLEAR unconditionally dispatch through
// vtable + 0x44 after their transition helper. Fix3.17 only reads that slot
// for the exact witness CLEAR and records its module-relative target.
constexpr std::uint32_t kFix317LogicSwitchDispatchSlot = 0x44U;
// Fix3.18 diagnostic-only code/thunk provenance. Fix3.17 runtime-confirmed the
// same vtable+0x44 target (RVA 0x001B4DD0) on Host and Client, but the supplied
// decompiler output has no named function boundary at VA 0x005B4DD0. Sample a
// bounded entry window and decode only exact x86 branch encodings. Numeric
// provenance only: no gameplay meaning is assigned and no target is invoked.
constexpr std::size_t kFix318CodeSampleBytes = 32U;
constexpr std::uint32_t kFix318MaxJumpDepth = 4U;
constexpr std::uint64_t kFix318HashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFix318HashPrime = 1099511628211ULL;
constexpr std::uint32_t kFix318BranchUnknown = 0U;
constexpr std::uint32_t kFix318BranchJmpRel32 = 1U;
constexpr std::uint32_t kFix318BranchJmpRel8 = 2U;
constexpr std::uint32_t kFix318BranchJmpAbsIndirect = 3U;
constexpr std::uint32_t kFix318BranchCallRel32 = 4U;
// Fix3.19 diagnostic-only causal differential flight recorder. The exact
// typed witness is only an anchor: once observed, a short one-second window
// records normalized TypedMessage, callback-constructor, QueueInput and
// LogicSwitch signatures. This is intended to identify a parallel Host-only
// CATHODE primitive in one Host/Client test
// instead of continuing the LogicSwitch micro-probe chain. No trace datum can
// enable, widen or parameterize replay.
constexpr std::uint64_t kFix319ArmWindowMs = 30000U;
constexpr std::uint64_t kFix319TraceWindowMs = 1000U;
constexpr std::size_t kFix319TraceSlotCapacity = kFix319CausalTraceMaximumEntries;
// BuildFix01: longer bounded probing accompanies the larger table so hash
// clustering is not mistaken for semantic capacity loss. The hot-path lookup
// still exits on the first empty/matching slot.
constexpr std::size_t kFix319TraceSlotProbeCount = 128U;
constexpr std::uint64_t kFix319HashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFix319HashPrime = 1099511628211ULL;
constexpr std::uint32_t kFix319AnchorLocalWitness = 1U;
constexpr std::uint32_t kFix319AnchorRemoteWitness = 2U;
constexpr std::uint32_t kFix319SemanticCallbackPresent = 0x1U;
constexpr std::uint32_t kFix319SemanticCallbackReadable = 0x2U;
constexpr std::uint32_t kFix319SemanticCallbackKeyValid = 0x4U;
constexpr std::uint32_t kFix319SemanticCallbackNestedPresent = 0x8U;
constexpr std::uint32_t kFix319SemanticCallbackPayloadPresent = 0x10U;
constexpr std::uint32_t kFix319ExecutionLocal = 0x1U;
constexpr std::uint32_t kFix319ExecutionRemoteApply = 0x2U;
static_assert((kFix319TraceSlotCapacity & (kFix319TraceSlotCapacity - 1U)) == 0U);
// Fix3.21 observation-only generic composite/state-value recorder. Function
// labels below are STATIC CONFIRMED VAs from AI.exe.c; constants are module-
// relative RVAs (preferred image base 0x00400000). Call-site redirection is
// accepted only when runtime E8 decoding reproduces the exact static call-graph
// multiplicities listed in InstallFix321CompositeStateCallRedirects().
constexpr std::uint64_t kFix321TraceWindowMs = 60000U;
constexpr std::size_t kFix321TraceSlotCapacity = kFix321CompositeStateTraceMaximumEntries;
constexpr std::size_t kFix321TraceSlotProbeCount = 128U;
constexpr std::uint64_t kFix321ObservationLimit = 65536U;
constexpr std::uint64_t kFix321HashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFix321HashPrime = 1099511628211ULL;
constexpr std::uint32_t kFix321IdentityRuntimeKey = 0x1U;
constexpr std::uint32_t kFix321IdentityWorldEntity = 0x2U;
constexpr std::uint32_t kFix321IdentityVtable = 0x4U;
constexpr std::uint32_t kFix321ReadWord0 = 0x1U;
constexpr std::uint32_t kFix321ReadWord1 = 0x2U;
constexpr std::uint32_t kFix321Helper04060Rva = 0x00504060U;
constexpr std::uint32_t kFix321Helper04200Rva = 0x00504200U;
constexpr std::uint32_t kFix321Helper042D0Rva = 0x005042D0U;
constexpr std::uint32_t kFix321Helper043A0Rva = 0x005043A0U;
constexpr std::uint32_t kFix321Helper04470Rva = 0x00504470U;
constexpr std::uint32_t kFix321Caller3AE0BeginRva = 0x001F3AE0U;
constexpr std::uint32_t kFix321Caller3AE0EndRva = 0x001F3B40U;
constexpr std::uint32_t kFix321Caller4200BeginRva = 0x001F4200U;
constexpr std::uint32_t kFix321Caller4200EndRva = 0x001F4310U;
constexpr std::uint32_t kFix321Caller4310BeginRva = 0x001F4310U;
constexpr std::uint32_t kFix321Caller4310EndRva = 0x001F4490U;
constexpr std::uint32_t kFix321Caller4580BeginRva = 0x001F4580U;
constexpr std::uint32_t kFix321Caller4580EndRva = 0x001F4700U;
constexpr std::uint64_t kFix321ExpectedRedirectCallSites = 19U;
static_assert((kFix321TraceSlotCapacity & (kFix321TraceSlotCapacity - 1U)) == 0U);
// Fix3.22 observation-only Composite execution before/after recorder.
constexpr std::uint64_t kFix322TraceWindowMs = 60000U;
constexpr std::size_t kFix322TraceSlotCapacity = kFix322CompositeDispatchTraceMaximumEntries;
constexpr std::size_t kFix322TraceSlotProbeCount = 128U;
constexpr std::size_t kFix322SnapshotWordCount = 16U;
constexpr std::size_t kFix322PublishedChangeCount = 4U;
constexpr std::uint64_t kFix322ObservationLimit = 65536U;
constexpr std::uint64_t kFix322HashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFix322HashPrime = 1099511628211ULL;
constexpr std::uint32_t kFix322Target657F0Rva = 0x005657F0U;
constexpr std::uint32_t kFix322Caller65A90BeginRva = 0x00565A90U;
constexpr std::uint32_t kFix322Caller65A90EndRva = 0x00565AC0U;
constexpr std::uint64_t kFix322ExpectedRedirectCallSites = 1U;
static_assert(kFix322SnapshotWordCount <= 32U);
static_assert((kFix322TraceSlotCapacity & (kFix322TraceSlotCapacity - 1U)) == 0U);
constexpr std::uint64_t kCallbackAbiProbeSampleLimit = 8U;
constexpr std::uint64_t kCallbackIdentityProbeResolveLimit = 1024U;
constexpr std::uint32_t kCallbackAbiShapeInputIsCallback = 0x01U;
constexpr std::uint32_t kCallbackAbiShapeNestedPresent = 0x02U;
constexpr std::uint32_t kCallbackAbiShapePayloadPresent = 0x04U;
constexpr std::uint32_t kCallbackAbiShapeTargetResolved = 0x08U;
constexpr std::uint32_t kCallbackAbiShapeTargetSameAsSource = 0x10U;
constexpr std::uint32_t kCallbackAbiShapeTargetSameAsTarget = 0x20U;
constexpr std::uint32_t kCallbackAbiShapeTargetSameAsState = 0x40U;
constexpr std::uint32_t kCallbackAbiMatchNone = 0U;
constexpr std::uint32_t kCallbackAbiMatchNested = 1U;
constexpr std::uint32_t kCallbackAbiMatchEndpointRecent = 2U;
constexpr std::uint32_t kCallbackAbiMatchRecentOnly = 3U;
constexpr std::uint32_t kCallbackAbiMatchTemporalSnapshot = 4U;
constexpr std::size_t kRuntimeWorldIdCacheCapacity = 4096U;
constexpr std::size_t kRuntimeWorldIdCacheProbeCount = 4U;

struct LogicSwitchRuntimeBinding final {
    std::uint64_t packedKey{};
    CathodeRuntimeKey key{};
    void* outerEntity{};
    void* nativeSwitch{};
    std::array<std::uint32_t, 4> contextA{};
    void* contextBObject{};
    bool contextAValid{};
    std::uint64_t contextCapturedTickMs{};
    std::uint64_t levelGeneration{};
    std::uint64_t lastSeenTickMs{};
};

struct SchedulerCausalRecord final {
    std::atomic<std::uint64_t> sequence{0U};
    std::atomic<std::uint64_t> tickMs{0U};
    std::atomic<std::uint64_t> sourceHigh{0U};
    std::atomic<std::uint64_t> sourceLow{0U};
    std::atomic<std::uint64_t> targetHigh{0U};
    std::atomic<std::uint64_t> targetLow{0U};
    std::atomic<std::uint32_t> schedulerParameterShortGuid{0U};
    std::atomic<std::uint32_t> targetInputShortGuid{0U};
    std::atomic<std::uint64_t> callbackHigh{0U};
    std::atomic<std::uint64_t> callbackLow{0U};
    std::atomic<std::uint32_t> callbackExecutionVtableRva{0U};
    std::atomic<std::uint32_t> sourceRuntimeKeyA{0U};
    std::atomic<std::uint32_t> sourceRuntimeKeyB{0U};
    std::atomic<std::uint32_t> targetRuntimeKeyA{0U};
    std::atomic<std::uint32_t> targetRuntimeKeyB{0U};
    std::atomic<std::uint32_t> callbackRuntimeKeyA{0U};
    std::atomic<std::uint32_t> callbackRuntimeKeyB{0U};
    std::atomic<std::uint32_t> queueCallerRva{0U};
    std::array<std::atomic<std::uint32_t>, kCallbackRawWordCount> callbackRawWords{};
    std::atomic<std::uint32_t> flags{0U};
};

struct SchedulerCausalSnapshot final {
    std::uint64_t sequence{};
    std::uint64_t tickMs{};
    WorldEntityId source{};
    WorldEntityId target{};
    std::uint32_t schedulerParameterShortGuid{};
    std::uint32_t targetInputShortGuid{};
    WorldEntityId callbackEntity{};
    CathodeRuntimeKey sourceRuntimeKey{};
    CathodeRuntimeKey targetRuntimeKey{};
    CathodeRuntimeKey callbackRuntimeKey{};
    std::uint32_t callbackExecutionVtableRva{};
    std::uint32_t queueCallerRva{};
    std::array<std::uint32_t, kCallbackRawWordCount> callbackRawWords{};
    bool callbackRecordPresent{};
    bool callbackRawReadable{};
    bool callbackExecutionReadable{};
    bool callbackExecutionPresent{};
    bool sourceRuntimeKeyValid{};
    bool targetRuntimeKeyValid{};
    bool callbackRuntimeKeyValid{};
    bool callbackWorldEntity{};
    bool callbackSameAsSource{};
    bool callbackSameAsTarget{};
};

struct ActiveSchedulerCausalContext final {
    bool active{};
    SchedulerCausalSnapshot snapshot{};
    // Process-local pointer used only while the retail FUN_009271C0 call is on
    // this thread's stack. It is never persisted, logged, or sent over network.
    void* callbackRecord{};
};

struct ExactLogicSwitchCallbackSnapshot final {
    std::array<std::uint32_t, kCallbackRawWordCount> words{};
    CathodeRuntimeKey callbackRuntimeKey{};
    std::uint32_t callbackExecutionVtableRva{};
    bool holderReadable{};
    bool callbackPresent{};
    bool rawReadable{};
    bool callbackExecutionPresent{};
    bool callbackExecutionReadable{};
    bool callbackRuntimeKeyValid{};
};

struct Fix318ClearCodeSnapshot final {
    std::uint32_t targetRva{};
    std::uint32_t word0{};
    std::uint32_t word1{};
    std::uint32_t branchKind{};
    std::uint32_t branchTargetRva{};
    std::uint32_t finalTargetRva{};
    std::uint32_t jumpDepth{};
    std::uint64_t hash{kFix318HashOffset};
    bool attempted{};
    bool readable{};
};

struct Fix317ClearDispatchSnapshot final {
    std::uint32_t vtableRva{};
    std::uint32_t targetRva{};
    Fix318ClearCodeSnapshot code{};
    bool attempted{};
    bool readable{};
};

struct Fix319TraceSemantic final {
    CausalTraceChannel channel{CausalTraceChannel::TypedMessage};
    std::uint32_t worldMask{};
    CathodeRuntimeKey sourceRuntimeKey{};
    CathodeRuntimeKey targetRuntimeKey{};
    WorldEntityId sourceWorldEntity{};
    WorldEntityId targetWorldEntity{};
    std::uint32_t primary{};
    std::uint32_t secondary{};
    std::uint32_t payloadBytes{};
    std::uint64_t payloadHash{};
    std::uint64_t delayBits{};
    std::uint32_t tailArgument{};
    std::uint32_t callbackInput{};
    std::uint32_t callbackFlags{};
    CathodeRuntimeKey callbackRuntimeKey{};
    std::uint32_t callbackExecutionVtableRva{};
    std::uint32_t semanticFlags{};
};

struct Fix319TraceSlot final {
    std::atomic<std::uint64_t> signatureHash{0U};
    std::atomic<std::uint32_t> ready{0U};
    Fix319TraceSemantic semantic{};
    std::atomic<std::uint64_t> count{0U};
    std::atomic<std::uint32_t> executionMask{0U};
    std::atomic<std::uint32_t> firstDeltaMs{0U};
    std::atomic<std::uint32_t> lastDeltaMs{0U};
    std::atomic<std::uint32_t> firstCallSiteRva{0U};
    std::atomic<std::uint32_t> lastCallSiteRva{0U};
    std::atomic<std::uint32_t> callSiteVaried{0U};
};


struct Fix321TraceSemantic final {
    std::uint32_t helperRva{};
    std::uint32_t callerRva{};
    CathodeRuntimeKey runtimeKey{};
    WorldEntityId worldEntity{};
    std::uint32_t executionVtableRva{};
    std::uint32_t identityMask{};
    std::uint32_t valueReadMask{};
    std::uint32_t valueWord0{};
    std::uint32_t contextReadMask{};
    std::uint32_t contextWord0{};
    std::uint32_t contextWord1{};
    std::uint32_t objectReadMask{};
    std::uint32_t objectWord0{};
    std::uint32_t objectWord1{};
};

struct Fix321TraceSlot final {
    std::atomic<std::uint64_t> signatureHash{0U};
    std::atomic<std::uint32_t> ready{0U};
    Fix321TraceSemantic semantic{};
    std::atomic<std::uint64_t> count{0U};
    std::atomic<std::uint32_t> executionMask{0U};
    std::atomic<std::uint32_t> firstDeltaMs{0U};
    std::atomic<std::uint32_t> lastDeltaMs{0U};
};

struct Fix321CallFamilySpec final {
    std::uint32_t callerBeginRva{};
    std::uint32_t callerEndRva{};
    std::uint32_t helperRva{};
    std::uint32_t expectedCount{};
    void* detour{};
};

struct Fix322TraceSemantic final {
    std::uint32_t targetRva{};
    std::uint32_t callerRva{};
    std::uint32_t firstPointerRva{};
    std::uint32_t firstPointerReadable{};
    std::uint64_t beforeHash{};
    std::uint64_t afterHash{};
    std::uint32_t changedWordMask{};
    std::uint32_t changedWordCount{};
    std::array<std::uint32_t, kFix322PublishedChangeCount> changedOffsets{};
    std::array<std::uint32_t, kFix322PublishedChangeCount> beforeWords{};
    std::array<std::uint32_t, kFix322PublishedChangeCount> afterWords{};
};

struct Fix322TraceSlot final {
    std::atomic<std::uint64_t> signatureHash{0U};
    std::atomic<std::uint32_t> ready{0U};
    Fix322TraceSemantic semantic{};
    std::atomic<std::uint64_t> count{0U};
    std::atomic<std::uint32_t> executionMask{0U};
    std::atomic<std::uint32_t> firstDeltaMs{0U};
    std::atomic<std::uint32_t> lastDeltaMs{0U};
};

std::atomic<bool> gInitialized{false};
std::atomic<bool> gHookInstalled{false};
std::atomic<WorldEntityRegistry*> gRegistry{nullptr};
const SignatureResolver* gResolver{};
HookManager* gHooks{};
std::mutex gArmMutex;
void* gQueueInputTrampolineRaw{};
std::atomic<void*> gQueueInputTrampoline{nullptr};
void* gBuildInputTargetTrampolineRaw{};
std::atomic<void*> gBuildInputTargetTrampoline{nullptr};
std::atomic<bool> gBuildInputTargetHookInstalled{false};
std::atomic<LocalInputTargetObserver> gLocalInputTargetObserver{nullptr};
void* gQueueTypedMessageTrampolineRaw{};
std::atomic<void*> gQueueTypedMessageTrampoline{nullptr};
std::atomic<bool> gTypedMessageHookInstalled{false};
void* gBuildCallbackValueTrampolineRaw{};
std::atomic<void*> gBuildCallbackValueTrampoline{nullptr};
std::atomic<bool> gCharacterCallbackOriginHookInstalled{false};
void* gLogicSwitchSetTrampolineRaw{};
std::atomic<void*> gLogicSwitchSetTrampoline{nullptr};
void* gLogicSwitchClearTrampolineRaw{};
std::atomic<void*> gLogicSwitchClearTrampoline{nullptr};
std::atomic<std::uintptr_t> gLogicSwitchVtable{0U};
std::atomic<bool> gLogicSwitchHookInstalled{false};
std::atomic<std::uint64_t> gVersion{0U};
std::atomic<std::uint64_t> gInitializationCount{0U};
std::atomic<std::uint64_t> gShutdownCount{0U};
std::atomic<std::uint64_t> gObservedSchedulerCalls{0U};
std::atomic<std::uint64_t> gRelevantSourceCalls{0U};
std::atomic<std::uint64_t> gSubmittedIntents{0U};
std::atomic<std::uint64_t> gUnresolvedSource{0U};
std::atomic<std::uint64_t> gUnresolvedTarget{0U};
std::atomic<std::uint64_t> gCallbackEdgesSkipped{0U};
std::atomic<std::uint64_t> gInvalidEdgesSkipped{0U};
std::atomic<std::uint64_t> gRemoteApplySuppressed{0U};
std::atomic<std::uint64_t> gOriginEchoSuppressed{0U};
std::atomic<std::uint64_t> gDiscardedPublicRootObservations{0U};
std::mutex gSelfClockedMutex;
SelfClockedRootFilter gSelfClockedRoots;
std::atomic<std::uint64_t> gSelfClockedKeys{0U};
std::atomic<std::uint64_t> gSelfClockedLocalRoots{0U};

// Locally executed by the caller when false: the retail loop keeps running on
// every peer, only its network copy is withheld.
bool SelfClockedLocalRoot(const WorldEntityId& owner, const CompositePublicInputEvent& event) noexcept {
    const auto key = (owner.high * 0x9E3779B97F4A7C15ULL) ^ owner.low ^
        (event.targetEntity.low * 0xC2B2AE3D27D4EB4FULL) ^ event.publicInputShortGuid;
    SelfClockedRootFilter::Decision decision{};
    {
        std::lock_guard lock(gSelfClockedMutex);
        decision = gSelfClockedRoots.Observe(key, GetTickCount64());
    }
    if (decision == SelfClockedRootFilter::Decision::Replicate) return false;
    if (decision == SelfClockedRootFilter::Decision::BecameLocal)
        gSelfClockedKeys.fetch_add(1U, std::memory_order_relaxed);
    gSelfClockedLocalRoots.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
std::atomic<std::uint64_t> gEchoTableFull{0U};
std::atomic<std::uint64_t> gIntentQueueRejected{0U};
std::atomic<std::uint64_t> gSourceClassMismatch{0U};
std::atomic<std::uint64_t> gExecuteAttempts{0U};
std::atomic<std::uint64_t> gQueuedRemoteEdges{0U};
// Patch 5.3.2 WS3 Event-boundary probes. Observation only; these never submit
// legacy WS2 operations and never change retail acceptance.
std::atomic<std::uint64_t> gWs3PreQueueSeen{0U};
std::atomic<std::uint64_t> gWs3PreQueueInputReadable{0U};
std::atomic<std::uint64_t> gWs3PreQueueTargetWorldResolved{0U};
std::atomic<std::uint64_t> gWs3PreQueueTargetWorldUnresolved{0U};
std::atomic<std::uint32_t> gWs3PreQueueLastInput{0U};
std::atomic<std::uint32_t> gWs3PreQueueLastCallerRva{0U};
std::atomic<std::uint64_t> gWs3QueueSeen{0U};
std::atomic<std::uint64_t> gWs3QueueDescendantSuppressed{0U};
std::atomic<std::uint64_t> gWs3QueueCallbackRejected{0U};
std::atomic<std::uint64_t> gWs3QueueDescribeRejected{0U};
std::atomic<std::uint64_t> gWs3QueueRemoteNatural{0U};
std::atomic<std::uint64_t> gWs3QueueRemoteApplySuppressed{0U};
std::atomic<std::uint64_t> gWs3QueueDeferredComposite{0U};
std::atomic<std::uint64_t> gWs3QueueStateOwned{0U};
std::atomic<std::uint64_t> gWs3QueueEventEligible{0U};
std::atomic<std::uint64_t> gWs3QueueUnknownEligible{0U};
std::atomic<std::uint64_t> gWs3QueueSubmitRetailOnly{0U};
std::atomic<std::uint64_t> gWs3QueueSubmitAwaitAuthority{0U};
std::atomic<std::uint64_t> gWs3QueueSubmitAuthoritySuppressed{0U};
std::atomic<std::uint64_t> gWs3QueueSubmitLocalRoot{0U};
std::atomic<std::uint32_t> gWs3QueueLastSchedulerParameter{0U};
std::atomic<std::uint32_t> gWs3QueueLastTargetInput{0U};
std::atomic<std::uint32_t> gWs3QueueLastCallerRva{0U};
std::atomic<std::uint32_t> gWs3QueueLastGate{0U};
std::atomic<std::uint64_t> gWs3TypedSeen{0U};
std::atomic<std::uint64_t> gWs3TypedDescendantSuppressed{0U};
std::atomic<std::uint64_t> gWs3TypedWorldPairResolved{0U};
std::atomic<std::uint64_t> gWs3TypedWorldPairUnresolved{0U};
std::atomic<std::uint64_t> gWs3TypedRootRetailOnly{0U};
std::atomic<std::uint64_t> gWs3TypedRootAwaitAuthority{0U};
std::atomic<std::uint64_t> gWs3TypedRootAuthoritySuppressed{0U};
std::atomic<std::uint64_t> gWs3TypedRootLocal{0U};
std::atomic<std::uint64_t> gWs3TypedReceiverLocalSuppressed{0U};
std::atomic<std::uint32_t> gWs3TypedLastReceiverLocalSuppressedType{0U};
std::atomic<std::uint32_t> gWs3TypedLastMessageType{0U};
std::atomic<std::uint32_t> gWs3TypedLastPayloadBytes{0U};
std::atomic<std::uint32_t> gWs3TypedLastWorldMask{0U};
std::atomic<std::uint32_t> gWs3TypedLastCallerRva{0U};
std::atomic<std::uint64_t> gWs3TypedInteractionPairSeen{0U};
std::atomic<std::uint64_t> gWs3TypedInteractionPair43{0U};
std::atomic<std::uint64_t> gWs3TypedInteractionPair44{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPair43LastCallerRva{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPair44LastCallerRva{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPair43LastCausalSource{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPair44LastCausalSource{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPairLastType{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPairLastCallerRva{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPairLastSourcePrimitive{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPairLastTargetPrimitive{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPairLastSourceDomain{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPairLastTargetDomain{0U};
std::atomic<std::uint32_t> gWs3TypedInteractionPairLastCausalSource{0U};
std::atomic<std::uint64_t> gWs3TypedInteractionPairLastSourceHigh{0U};
std::atomic<std::uint64_t> gWs3TypedInteractionPairLastSourceLow{0U};
std::atomic<std::uint64_t> gWs3TypedInteractionPairLastTargetHigh{0U};
std::atomic<std::uint64_t> gWs3TypedInteractionPairLastTargetLow{0U};
std::atomic<std::uint64_t> gCompositePublicInputMapped{0U};
std::atomic<std::uint64_t> gCompositePublicInputRuntimeEdgeMapped{0U};
std::atomic<std::uint64_t> gCompositePublicInputOwnerScopeRuntimeEdgeMapped{0U};
// Fix3.27 pre-QueueInput input-target constructor telemetry. v0.11.4.56-trace
// adds a bounded four-entry recorder ONLY for unknown ShortGuids observed while
// an already-certified callback BridgeScope is active. It is diagnostic-only,
// process-local, never scanned, never serialized, and never changes acceptance.
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderSeen{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderInputReadable{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderPublic{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderInternal{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderInternalExactMapped{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderInternalRuntimeMapped{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderInternalOwnerScopeMapped{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderUnknownGuid{0U};

struct PreQueueUnknownTraceSlot final {
    std::atomic<std::uint32_t> ready{0U};
    std::atomic<std::uint32_t> inputShortGuid{0U};
    std::atomic<std::uint32_t> callerRva{0U};
    std::atomic<std::uint32_t> targetRuntimeKeyA{0U};
    std::atomic<std::uint32_t> targetRuntimeKeyB{0U};
    std::atomic<std::uintptr_t> targetExecution{0U};
    std::atomic<std::uint64_t> targetIdHigh{0U};
    std::atomic<std::uint64_t> targetIdLow{0U};
    std::atomic<std::uint64_t> bridgeOwnerHigh{0U};
    std::atomic<std::uint64_t> bridgeOwnerLow{0U};
    std::atomic<std::uint32_t> outerPublicInput{0U};
};

std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderUnknownTraceCount{0U};
std::array<PreQueueUnknownTraceSlot, kPreQueueUnknownTraceCapacity>
    gCompositePublicInputPreQueueBuilderUnknownTrace{};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderMapped{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderOwnerMapped{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderInterfaceMapped{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderSubmitted{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderRemoteSkipped{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderDelayRejected{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderIdentityRejected{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderTypedRejected{0U};
std::atomic<std::uint64_t> gCompositePublicInputPreQueueBuilderRejected{0U};
std::atomic<std::uint64_t> gCompositePublicInputSubmitted{0U};
std::atomic<std::uint64_t> gCompositePublicInputRemoteAttempts{0U};
std::atomic<std::uint64_t> gCompositePublicInputBridgeQueued{0U};
std::atomic<std::uint64_t> gCompositePublicInputSourceFallbacks{0U};
std::atomic<std::uint64_t> gCompositePublicInputRuntimeUnavailable{0U};
std::atomic<std::uint64_t> gCompositePublicInputRejected{0U};
std::atomic<std::uint64_t> gNativeFaults{0U};
std::atomic<std::uint64_t> gActiveDetours{0U};
std::atomic<std::uint64_t> gObservedTypedMessages{0U};
std::atomic<std::uint64_t> gRelevantTypedMessages{0U};
std::atomic<std::uint64_t> gSubmittedTypedMessages{0U};
std::atomic<std::uint64_t> gTypedPayloadBytes{0U};
std::atomic<std::uint64_t> gOversizedTypedMessagesSkipped{0U};
std::atomic<std::uint64_t> gInvalidTypedMessagesSkipped{0U};
std::atomic<std::uint64_t> gUnresolvedTypedSource{0U};
std::atomic<std::uint64_t> gUnresolvedTypedTarget{0U};
std::atomic<std::uint64_t> gTypedRuntimeKeyAnchoredByTarget{0U};
std::atomic<std::uint64_t> gTypedRuntimeKeyAnchoredBySource{0U};
std::atomic<std::uint64_t> gTypedRuntimeKeyNoAnchorSkipped{0U};
std::atomic<std::uint64_t> gTypedRuntimeKeySafetyBlocked{0U};
std::atomic<std::uint32_t> gTypedBlockedMessageType{0U};
std::atomic<std::uint32_t> gTypedBlockedPayloadBytes{0U};
std::atomic<std::uint32_t> gTypedBlockedSourceA{0U};
std::atomic<std::uint32_t> gTypedBlockedSourceB{0U};
std::atomic<std::uint32_t> gTypedBlockedTargetA{0U};
std::atomic<std::uint32_t> gTypedBlockedTargetB{0U};
std::atomic<std::uint32_t> gTypedBlockedMissingMask{0U};
std::atomic<std::uint64_t> gTypedRemoteApplySuppressed{0U};
std::atomic<std::uint64_t> gTypedOriginEchoSuppressed{0U};
std::atomic<std::uint64_t> gTypedContentMatchSuppressed{0U};
std::atomic<std::uint64_t> gTypedIntentQueueRejected{0U};
std::atomic<std::uint64_t> gTypedExecuteAttempts{0U};
std::atomic<std::uint64_t> gQueuedRemoteTypedMessages{0U};
std::atomic<std::uint64_t> gTypedNativeFaults{0U};
std::atomic<std::uint64_t> gTypedMessageOriginProbeUntilTickMs{0U};
std::atomic<std::uint64_t> gTypedMessageOriginCandidates{0U};
std::atomic<std::uint64_t> gTypedMessageOriginExactMatches{0U};
std::atomic<std::uint64_t> gTypedMessageOriginCallVerified{0U};
std::atomic<std::uint64_t> gTypedMessageOriginReadFailures{0U};
std::atomic<std::uint32_t> gTypedMessageOriginMessageType{0U};
std::atomic<std::uint32_t> gTypedMessageOriginPayloadBytes{0U};
std::atomic<std::uint32_t> gTypedMessageOriginSourceA{0U};
std::atomic<std::uint32_t> gTypedMessageOriginSourceB{0U};
std::atomic<std::uint32_t> gTypedMessageOriginTargetA{0U};
std::atomic<std::uint32_t> gTypedMessageOriginTargetB{0U};
std::atomic<std::uint32_t> gTypedMessageOriginWorldMask{0U};
std::atomic<std::uint64_t> gTypedMessageOriginDelayBits{0U};
std::atomic<std::uint32_t> gTypedMessageOriginFirstCallSiteRva{0U};
std::atomic<std::uint32_t> gTypedMessageOriginLastCallSiteRva{0U};
std::atomic<std::uint32_t> gTypedMessageOriginCallSiteVaried{0U};
std::atomic<std::uint32_t> gTypedMessageOriginThreadId{0U};
std::atomic<std::uint64_t> gTypedMessageOriginTickMs{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityProbeUntilTickMs{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityCandidates{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityExactMatches{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentityMessageType{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentityPayloadBytes{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentitySourceWorldHigh{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentitySourceWorldLow{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityTargetWorldHigh{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityTargetWorldLow{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentitySenderSourceA{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentitySenderSourceB{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentitySenderTargetA{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentitySenderTargetB{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentityLocalSourceA{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentityLocalSourceB{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentityLocalTargetA{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentityLocalTargetB{0U};
std::atomic<std::uint32_t> gTypedRemoteIdentityMask{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityExecuteReached{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityQueued{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityRuntimeUnavailable{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityNativeFaults{0U};
std::atomic<std::uint64_t> gTypedRemoteIdentityInvalidPayloads{0U};
std::atomic<std::uint64_t> gLogicDownstreamProbeUntilTickMs{0U};
std::atomic<std::uint64_t> gLogicDownstreamScopes{0U};
std::atomic<std::uint64_t> gLogicDownstreamEdges{0U};
std::atomic<std::uint64_t> gLogicDownstreamValidEdges{0U};
std::atomic<std::uint64_t> gLogicDownstreamCallbackEdges{0U};
std::atomic<std::uint64_t> gLogicDownstreamReadFailures{0U};
std::atomic<std::uint32_t> gLogicDownstreamWorldMask{0U};
std::atomic<std::uint32_t> gLogicDownstreamSourceA{0U};
std::atomic<std::uint32_t> gLogicDownstreamSourceB{0U};
std::atomic<std::uint32_t> gLogicDownstreamTargetA{0U};
std::atomic<std::uint32_t> gLogicDownstreamTargetB{0U};
std::atomic<std::uint32_t> gLogicDownstreamSchedulerParameter{0U};
std::atomic<std::uint32_t> gLogicDownstreamTargetInput{0U};
std::atomic<std::uint32_t> gLogicDownstreamCallbackFlags{0U};
std::atomic<std::uint32_t> gLogicDownstreamFirstCallSiteRva{0U};
std::atomic<std::uint32_t> gLogicDownstreamLastCallSiteRva{0U};
std::atomic<std::uint32_t> gLogicDownstreamCallSiteVaried{0U};
std::atomic<std::uint64_t> gLogicDownstreamSequenceHash{kFix315HashOffset};
std::atomic<std::uint32_t> gLogicDownstreamThreadId{0U};
std::atomic<std::uint64_t> gLogicDownstreamTickMs{0U};
thread_local std::uint32_t gFix315ExactClearDepth = 0U;
std::atomic<std::uint64_t> gPostClearWindowUntilTickMs{0U};
std::atomic<std::uint64_t> gPostClearOriginTickMs{0U};
std::atomic<std::uint64_t> gPostClearArms{0U};
std::atomic<std::uint64_t> gPostClearSeen{0U};
std::atomic<std::uint64_t> gPostClearAnchorMatches{0U};
std::atomic<std::uint64_t> gPostClearValidEdges{0U};
std::atomic<std::uint64_t> gPostClearCallbackEdges{0U};
std::atomic<std::uint64_t> gPostClearReadFailures{0U};
std::atomic<std::uint64_t> gPostClearInspectLimitDrops{0U};
std::atomic<std::uint32_t> gPostClearAnchorRole{0U};
std::atomic<std::uint32_t> gPostClearWorldMask{0U};
std::atomic<std::uint32_t> gPostClearSourceA{0U};
std::atomic<std::uint32_t> gPostClearSourceB{0U};
std::atomic<std::uint32_t> gPostClearTargetA{0U};
std::atomic<std::uint32_t> gPostClearTargetB{0U};
std::atomic<std::uint32_t> gPostClearSchedulerParameter{0U};
std::atomic<std::uint32_t> gPostClearTargetInput{0U};
std::atomic<std::uint32_t> gPostClearCallbackFlags{0U};
std::atomic<std::uint32_t> gPostClearFirstCallSiteRva{0U};
std::atomic<std::uint32_t> gPostClearLastCallSiteRva{0U};
std::atomic<std::uint32_t> gPostClearCallSiteVaried{0U};
std::atomic<std::uint64_t> gPostClearSequenceHash{kFix316HashOffset};
std::atomic<std::uint32_t> gPostClearFirstDeltaMs{0U};
std::atomic<std::uint32_t> gPostClearLastDeltaMs{0U};
std::atomic<std::uint32_t> gPostClearThreadId{0U};
std::atomic<std::uint64_t> gPostClearTickMs{0U};
std::atomic<std::uint64_t> gClearDispatchCandidates{0U};
std::atomic<std::uint64_t> gClearDispatchExactMatches{0U};
std::atomic<std::uint64_t> gClearDispatchReadFailures{0U};
std::atomic<std::uint32_t> gClearDispatchVtableRva{0U};
std::atomic<std::uint32_t> gClearDispatchTargetRva{0U};
std::atomic<std::uint32_t> gClearDispatchFirstTargetRva{0U};
std::atomic<std::uint32_t> gClearDispatchLastTargetRva{0U};
std::atomic<std::uint32_t> gClearDispatchTargetVaried{0U};
std::atomic<std::uint32_t> gClearDispatchBeforeState{0U};
std::atomic<std::uint32_t> gClearDispatchAfterState{0U};
std::atomic<std::uint32_t> gClearDispatchThreadId{0U};
std::atomic<std::uint64_t> gClearDispatchTickMs{0U};
std::atomic<std::uint64_t> gClearCodeCandidates{0U};
std::atomic<std::uint64_t> gClearCodeExactMatches{0U};
std::atomic<std::uint64_t> gClearCodeReadFailures{0U};
std::atomic<std::uint32_t> gClearCodeTargetRva{0U};
std::atomic<std::uint32_t> gClearCodeWord0{0U};
std::atomic<std::uint32_t> gClearCodeWord1{0U};
std::atomic<std::uint32_t> gClearCodeBranchKind{0U};
std::atomic<std::uint32_t> gClearCodeBranchTargetRva{0U};
std::atomic<std::uint32_t> gClearCodeFinalTargetRva{0U};
std::atomic<std::uint32_t> gClearCodeJumpDepth{0U};
std::atomic<std::uint64_t> gClearCodeHash{kFix318HashOffset};
std::atomic<std::uint32_t> gClearCodeThreadId{0U};
std::atomic<std::uint64_t> gClearCodeTickMs{0U};
std::atomic<std::uint64_t> gFix319ArmUntilTickMs{0U};
std::atomic<std::uint64_t> gFix319WindowUntilTickMs{0U};
std::atomic<std::uint64_t> gFix319OriginTickMs{0U};
std::atomic<std::uint32_t> gFix319AnchorRole{0U};
std::atomic<std::uint64_t> gFix319Anchors{0U};
std::atomic<std::uint64_t> gFix319Inspected{0U};
std::atomic<std::uint64_t> gFix319Accepted{0U};
std::atomic<std::uint64_t> gFix319UniqueEntries{0U};
std::atomic<std::uint64_t> gFix319CapacityDrops{0U};
std::atomic<std::uint64_t> gFix319ReadFailures{0U};
std::array<Fix319TraceSlot, kFix319TraceSlotCapacity> gFix319TraceSlots{};
std::atomic<std::uint64_t> gLogicSwitchObserved{0U};
std::atomic<std::uint64_t> gLogicSwitchWorldStates{0U};
std::atomic<std::uint64_t> gLogicSwitchSubmitted{0U};
std::atomic<std::uint64_t> gLogicSwitchUnresolved{0U};
std::atomic<std::uint64_t> gLogicSwitchRemoteSuppressed{0U};
std::atomic<std::uint64_t> gLogicSwitchIntentRejected{0U};
std::atomic<std::uint64_t> gLogicSwitchExecuteAttempts{0U};
std::atomic<std::uint64_t> gLogicSwitchAppliedRemote{0U};
std::atomic<std::uint64_t> gLogicSwitchRuntimeUnavailable{0U};
std::atomic<std::uint64_t> gLogicSwitchNativeFaults{0U};
std::atomic<std::uint64_t> gLogicSwitchBindingRegistrations{0U};
std::atomic<std::uint64_t> gLogicSwitchBindingHits{0U};
std::atomic<std::uint64_t> gLogicSwitchBindingMisses{0U};
std::atomic<std::uint64_t> gLogicSwitchBindingDrops{0U};
std::atomic<std::uint64_t> gLogicSwitchContextReplays{0U};
std::atomic<std::uint64_t> gLogicSwitchSyntheticContextReplays{0U};
std::atomic<std::uint64_t> gLogicSwitchSafetyBlocked{0U};
std::atomic<std::uint64_t> gLogicSwitchContextWorld{0U};
std::atomic<std::uint64_t> gLogicSwitchContextNonWorld{0U};
std::atomic<std::uint32_t> gLogicSwitchLastContextA{0U};
std::atomic<std::uint32_t> gLogicSwitchLastContextB{0U};
std::atomic<std::uint32_t> gLogicSwitchLastStateA{0U};
std::atomic<std::uint32_t> gLogicSwitchLastStateB{0U};
std::atomic<std::uint32_t> gLogicSwitchLastEnabled{0U};
std::atomic<std::uint64_t> gLogicExactTargetMatches{0U};
std::atomic<std::uint64_t> gLogicExactHolderReadable{0U};
std::atomic<std::uint64_t> gLogicExactCallbackPresent{0U};
std::atomic<std::uint64_t> gLogicExactRawReadable{0U};
std::atomic<std::uint64_t> gLogicExactRawReadFailures{0U};
std::atomic<std::uint64_t> gLogicExactCallbackObjectPresent{0U};
std::atomic<std::uint64_t> gLogicExactCallbackKeyResolved{0U};
std::atomic<std::uint64_t> gLogicExactCallbackExecutionReadable{0U};
std::atomic<std::uint32_t> gLogicExactInput{0U};
std::atomic<std::uint32_t> gLogicExactFlags{0U};
std::atomic<std::uint32_t> gLogicExactRefCount{0U};
std::atomic<std::uint32_t> gLogicExactNestedPresent{0U};
std::atomic<std::uint32_t> gLogicExactPayloadPresent{0U};
std::atomic<std::uint32_t> gLogicExactCallbackKeyA{0U};
std::atomic<std::uint32_t> gLogicExactCallbackKeyB{0U};
std::atomic<std::uint32_t> gLogicExactCallbackExecutionVtableRva{0U};
std::atomic<std::uint32_t> gLogicExactContextSameAsState{0U};
std::atomic<std::uint32_t> gLogicExactEnabled{0U};
std::atomic<std::uint64_t> gCharacterCallbackOriginProbeUntilTickMs{0U};
std::atomic<std::uint64_t> gCharacterCallbackOriginCandidates{0U};
std::atomic<std::uint64_t> gCharacterCallbackOriginExactKeyMatches{0U};
std::atomic<std::uint64_t> gCharacterCallbackOriginCallVerified{0U};
std::atomic<std::uint64_t> gCharacterCallbackOriginReadFailures{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginSourceA{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginSourceB{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginExecutionVtableRva{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginFlags{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginPayloadBytes{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginFirstCallSiteRva{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginLastCallSiteRva{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginCallSiteVaried{0U};
std::atomic<std::uint32_t> gCharacterCallbackOriginThreadId{0U};
std::atomic<std::uint64_t> gCharacterCallbackOriginTickMs{0U};
std::atomic<std::uint64_t> gSchedulerCausalEdgesRecorded{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalNestedMatches{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalEndpointMatches{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalRecentMatches{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalCallbackMatches{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalNoMatches{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastAgeMs{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastSourceHigh{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastSourceLow{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastTargetHigh{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastTargetLow{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastStateHigh{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastStateLow{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastSchedulerParameter{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastTargetInput{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastFlags{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastEnabled{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastCallbackHigh{0U};
std::atomic<std::uint64_t> gLogicSwitchCausalLastCallbackLow{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastCallbackExecutionVtableRva{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastSourceRuntimeKeyA{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastSourceRuntimeKeyB{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastTargetRuntimeKeyA{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastTargetRuntimeKeyB{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastCallbackRuntimeKeyA{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastCallbackRuntimeKeyB{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastStateRuntimeKeyA{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastStateRuntimeKeyB{0U};
std::atomic<std::uint32_t> gLogicSwitchCausalLastQueueCallerRva{0U};
std::atomic<std::uintptr_t> gMainModuleBase{0U};
std::atomic<std::uint64_t> gCallbackRecordsObserved{0U};
std::atomic<std::uint64_t> gCallbackExecutionReadable{0U};
std::atomic<std::uint64_t> gCallbackWorldEntityResolved{0U};
std::atomic<std::uint64_t> gCallbackSameAsSource{0U};
std::atomic<std::uint64_t> gCallbackSameAsTarget{0U};
std::atomic<std::uint64_t> gCallbackRawReadable{0U};
std::atomic<std::uint64_t> gCallbackReferenceFamilyObserved{0U};
std::atomic<std::uint64_t> gCallbackReferencePortable{0U};
std::atomic<std::uint64_t> gCallbackReferenceSubmitted{0U};
std::atomic<std::uint64_t> gCallbackReferenceUnresolved{0U};
std::atomic<std::uint64_t> gCallbackReferenceExecuteAttempts{0U};
std::atomic<std::uint64_t> gCallbackReferenceAppliedRemote{0U};
std::atomic<std::uint64_t> gCallbackReferenceRuntimeUnavailable{0U};
std::atomic<std::uint64_t> gCallbackReferenceNativeFaults{0U};
std::atomic<std::uint64_t> gCallbackReferenceSettlingSkipped{0U};
std::atomic<std::uint64_t> gCallbackReferenceThrottled{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeCallerMatches{0U};
std::atomic<std::uint64_t> gCallbackAbiProbePairMatches{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeInputMatches{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeSamplesCaptured{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeSampleLimitDrops{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeRawReadFailures{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeIdentityAttempts{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeIdentityLimitDrops{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeRuntimeKeyAttempts{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeSourceKeyResolved{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeTargetKeyResolved{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeCallbackKeyResolved{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeCallbackObjectPresent{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeTargetResolved{0U};
std::atomic<std::uint32_t> gCallbackAbiProbeLastInputShortGuid{0U};
std::atomic<std::uint32_t> gCallbackAbiProbeFirstFlags{0U};
std::atomic<std::uint32_t> gCallbackAbiProbeLastFlags{0U};
std::atomic<std::uint32_t> gCallbackAbiProbeLastRefCount{0U};
std::atomic<std::uint32_t> gCallbackAbiProbeLastShapeFlags{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeLastTargetHigh{0U};
std::atomic<std::uint64_t> gCallbackAbiProbeLastTargetLow{0U};
std::atomic<std::uint32_t> gCallbackAbiProbeLastMatchKind{kCallbackAbiMatchNone};
std::atomic<bool> gCallbackAbiProbeFirstFlagsSet{false};
std::atomic<bool> gCallbackAbiProbeFlagsVaried{false};
std::array<std::atomic<std::uint32_t>, kCallbackRawWordCount> gLogicSwitchCausalLastCallbackRawWords{};
std::atomic<std::uint64_t> gCallbackClassificationUntilTickMs{0U};
std::atomic<std::uint64_t> gObservationSequence{1U};
std::atomic<std::uint64_t> gSchedulerCausalSequence{1U};
std::mutex gLocalObservationMutex;
LocalObservationLedger<4096U> gLocalObservations;
std::uint64_t gLocalObservationRemembered{}, gLocalObservationEchoed{}, gLocalObservationRetired{}, gLocalObservationMissing{};
std::array<std::atomic<std::uint64_t>, kEchoSlots> gTypedMatchHashes{};
std::array<std::atomic<std::uint64_t>, kEchoSlots> gTypedMatchExpiryMs{};
std::array<std::atomic<std::uint64_t>, kEchoSlots> gCharacterCallbackMatchHashes{};
std::array<std::atomic<std::uint64_t>, kEchoSlots> gCharacterCallbackMatchExpiryMs{};
std::atomic<std::uint64_t> gFix320ReplayUntilTickMs{0U};
std::atomic<std::uint64_t> gFix320Observed{0U};
std::atomic<std::uint64_t> gFix320Portable{0U};
std::atomic<std::uint64_t> gFix320ParentSuppressed{0U};
std::atomic<std::uint64_t> gFix320NestedSuppressed{0U};
std::atomic<std::uint64_t> gFix320Submitted{0U};
std::atomic<std::uint64_t> gFix320ContentSuppressed{0U};
std::atomic<std::uint64_t> gFix320ExecuteAttempts{0U};
std::atomic<std::uint64_t> gFix320Queued{0U};
std::atomic<std::uint64_t> gFix320RuntimeUnavailable{0U};
std::atomic<std::uint64_t> gFix320NativeFaults{0U};
std::atomic<std::uint64_t> gFix320Invalid{0U};
std::atomic<std::uint64_t> gFix320LimitDrops{0U};
std::atomic<std::uint64_t> gFix320WindowSubmissions{0U};
std::atomic<std::uint64_t> gFix320WindowExecutions{0U};
std::atomic<bool> gFix321CallRedirectsInstalled{false};
std::atomic<std::uint64_t> gFix321RedirectCallSites{0U};
std::atomic<std::uint64_t> gFix321InstallFailures{0U};
std::atomic<std::uint64_t> gFix321WindowUntilTickMs{0U};
std::atomic<std::uint64_t> gFix321OriginTickMs{0U};
std::atomic<std::uint64_t> gFix321Inspected{0U};
std::atomic<std::uint64_t> gFix321Accepted{0U};
std::atomic<std::uint64_t> gFix321UniqueEntries{0U};
std::atomic<std::uint64_t> gFix321CapacityDrops{0U};
std::atomic<std::uint64_t> gFix321ReadFailures{0U};
std::atomic<std::uint64_t> gFix321LimitDrops{0U};
std::array<Fix321TraceSlot, kFix321TraceSlotCapacity> gFix321TraceSlots{};
std::atomic<bool> gFix322CallRedirectInstalled{false};
std::atomic<std::uint64_t> gFix322RedirectCallSites{0U};
std::atomic<std::uint64_t> gFix322InstallFailures{0U};
std::atomic<std::uint64_t> gFix322WindowUntilTickMs{0U};
std::atomic<std::uint64_t> gFix322OriginTickMs{0U};
std::atomic<std::uint64_t> gFix322Inspected{0U};
std::atomic<std::uint64_t> gFix322Accepted{0U};
std::atomic<std::uint64_t> gFix322UniqueEntries{0U};
std::atomic<std::uint64_t> gFix322ChangedCalls{0U};
std::atomic<std::uint64_t> gFix322CapacityDrops{0U};
std::atomic<std::uint64_t> gFix322ReadFailures{0U};
std::atomic<std::uint64_t> gFix322LimitDrops{0U};
std::array<Fix322TraceSlot, kFix322TraceSlotCapacity> gFix322TraceSlots{};
std::array<LogicSwitchRuntimeBinding, kLogicSwitchBindingCapacity> gLogicSwitchBindings{};
std::array<SchedulerCausalRecord, kSchedulerCausalHistoryCapacity> gSchedulerCausalHistory{};
std::array<RuntimeWorldIdCacheSlot, kRuntimeWorldIdCacheCapacity> gRuntimeWorldIdCache{};
std::mutex gLogicSwitchBindingMutex;
std::mutex gCompositeInputSemanticMutex;
std::array<CompositeInputSemanticSlot, kCompositeInputSemanticSlots> gCompositeInputSemanticSlots{};
std::uint64_t gCompositeInputRateWindowTickMs{};
std::uint32_t gCompositeInputRateWindowCount{};
thread_local std::uint32_t gCompositeMappedQueueDepth = 0U;

// Patch 5.8 compatibility fallback: FUN_00921F70 can see the public
// Composite identity before QueueInput lowers it. 5.8 attempted to correlate
// the two boundaries by descriptor address; runtime proved that identity does
// not survive unchanged. Patch 5.8.1 publishes directly from FUN_00921F70 as
// WS2 did, so this process-local hint remains only as a dormant fallback.
// The descriptor pointer is NEVER serialized.
struct CanonicalCompositePreQueueHint final {
    void* targetDescriptor{};
    WorldCompositePublicInputDescriptor mapping{};
    bool valid{};
};
thread_local CanonicalCompositePreQueueHint gCanonicalCompositePreQueueHint{};
std::atomic<std::uint64_t> gWs3CanonicalPreQueueHintStored{0U};
std::atomic<std::uint64_t> gWs3CanonicalPreQueueHintConsumed{0U};
std::atomic<std::uint64_t> gWs3CanonicalPreQueueHintMissed{0U};
// Patch 5.8.1: the historical WS2 sender published the stable public Composite
// semantic directly from FUN_00921F70 after the retail descriptor constructor
// returned. These counters prove that WS3 is using that same semantic valve;
// no callback/work-item/descriptor pointer is transported.
std::atomic<std::uint64_t> gWs3CanonicalPreQueueSubmitAttempt{0U};
std::atomic<std::uint64_t> gWs3CanonicalPreQueueSubmitSuccess{0U};
std::atomic<std::uint64_t> gWs3CanonicalPreQueueAdmissionSuppressed{0U};
std::atomic<std::uint64_t> gWs3CanonicalPreQueueSubmitFailed{0U};
std::atomic<std::uint64_t> gWs3CanonicalQueueCandidateSeen{0U};
std::atomic<std::uint64_t> gWs3CanonicalQueueMapped{0U};
std::atomic<std::uint64_t> gWs3CanonicalQueueLocallyExecuted{0U};
std::atomic<std::uint64_t> gWs3CanonicalQueueAdmissionSuppressed{0U};

thread_local ActiveSchedulerCausalContext gActiveSchedulerCausalContext{};
// Non-zero only while a local typed message that will itself be replicated is
// executing retail. Exact callback descendants in this scope are causal
// consequences and are not redundantly networked by Fix3.20.
thread_local std::uint32_t gLocalReplicatedTypedDepth = 0U;
// Non-zero while an exact Fix3.20 callback edge is executing retail locally.
// Synchronous descendants are allowed to execute but are not redundantly
// submitted; replaying the root should regenerate them naturally on the peer.
thread_local std::uint32_t gLocalCharacterCallbackDepth = 0U;
std::uint64_t gObservationSalt{1U};


void TouchVersion() noexcept {
    gVersion.fetch_add(1U, std::memory_order_acq_rel);
}

std::uint32_t CallerRvaFromReturnAddress(const void* returnAddress) noexcept {
    const auto caller = reinterpret_cast<std::uintptr_t>(returnAddress);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (caller < module) return 0U;
    const auto delta = caller - module;
    return delta <= static_cast<std::uintptr_t>((std::numeric_limits<std::uint32_t>::max)())
        ? static_cast<std::uint32_t>(delta)
        : 0U;
}

void CapturePreQueueUnknownTrace(
    const std::uint32_t inputShortGuid,
    const std::uint32_t callerRva,
    const CathodeRuntimeKey& targetKey,
    void* const targetExecution,
    const WorldEntityId& targetId,
    const WorldEntityId& bridgeOwner,
    const std::uint32_t outerPublicInput) noexcept {
    const auto sequence = gCompositePublicInputPreQueueBuilderUnknownTraceCount.fetch_add(
        1U, std::memory_order_acq_rel);
    if (sequence >= kPreQueueUnknownTraceCapacity) return;
    auto& slot = gCompositePublicInputPreQueueBuilderUnknownTrace[
        static_cast<std::size_t>(sequence)];
    slot.inputShortGuid.store(inputShortGuid, std::memory_order_relaxed);
    slot.callerRva.store(callerRva, std::memory_order_relaxed);
    slot.targetRuntimeKeyA.store(targetKey.entityA, std::memory_order_relaxed);
    slot.targetRuntimeKeyB.store(targetKey.entityB, std::memory_order_relaxed);
    slot.targetExecution.store(
        reinterpret_cast<std::uintptr_t>(targetExecution), std::memory_order_relaxed);
    slot.targetIdHigh.store(targetId.high, std::memory_order_relaxed);
    slot.targetIdLow.store(targetId.low, std::memory_order_relaxed);
    slot.bridgeOwnerHigh.store(bridgeOwner.high, std::memory_order_relaxed);
    slot.bridgeOwnerLow.store(bridgeOwner.low, std::memory_order_relaxed);
    slot.outerPublicInput.store(outerPublicInput, std::memory_order_relaxed);
    slot.ready.store(1U, std::memory_order_release);
}

class ActiveDetour final {
public:
    ActiveDetour() noexcept { gActiveDetours.fetch_add(1U, std::memory_order_acq_rel); }
    ~ActiveDetour() { gActiveDetours.fetch_sub(1U, std::memory_order_acq_rel); }
};

bool ReadPointerSafely(const void* address, void*& output) noexcept {
    output = nullptr;
    if (address == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        output = *static_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    output = *static_cast<void* const*>(address);
    return true;
#endif
}


bool ReadU32Safely(const void* address, std::uint32_t& output) noexcept {
    output = 0U;
    if (address == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        output = *static_cast<const std::uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    output = *static_cast<const std::uint32_t*>(address);
    return true;
#endif
}

bool ReadU8Safely(const void* address, std::uint8_t& output) noexcept {
    output = 0U;
    if (address == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        output = *static_cast<const std::uint8_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    output = *static_cast<const std::uint8_t*>(address);
    return true;
#endif
}

bool ReadFloatSafely(const void* address, float& output) noexcept {
    std::uint32_t bits = 0U;
    if (!ReadU32Safely(address, bits)) return false;
    output = std::bit_cast<float>(bits);
    return true;
}


[[nodiscard]] void* ExecutionObjectFromShared(void* shared) noexcept {
    if (shared == nullptr) return nullptr;
    void* execution = nullptr;
    if (!ReadPointerSafely(
            static_cast<const std::byte*>(shared) + kSharedObjectOffset,
            execution)) {
        return nullptr;
    }
    return execution;
}

[[nodiscard]] std::size_t RuntimeWorldCacheStart(const void* execution) noexcept {
    auto value = reinterpret_cast<std::uintptr_t>(execution);
    value ^= value >> 17U;
    value *= static_cast<std::uintptr_t>(0x9E3779B1U);
    return static_cast<std::size_t>(value) & (kRuntimeWorldIdCacheCapacity - 1U);
}

[[nodiscard]] std::optional<WorldEntityId> IdentifyRuntimeWorldEntityCached(
    WorldEntityRegistry& registry,
    const void* execution) noexcept {
    if (execution == nullptr) return std::nullopt;
    const auto key = reinterpret_cast<std::uintptr_t>(execution);
    const auto start = RuntimeWorldCacheStart(execution);
    for (std::size_t probe = 0U; probe < kRuntimeWorldIdCacheProbeCount; ++probe) {
        auto& slot = gRuntimeWorldIdCache[(start + probe) & (kRuntimeWorldIdCacheCapacity - 1U)];
        if (slot.execution.load(std::memory_order_acquire) == key) {
            WorldEntityId id{};
            id.high = slot.high.load(std::memory_order_relaxed);
            id.low = slot.low.load(std::memory_order_relaxed);
            if (id.Valid()) return id;
        }
    }

    const auto resolved = registry.IdentifyRuntimeWorldEntity(execution);
    if (!resolved.has_value()) return std::nullopt;
    for (std::size_t probe = 0U; probe < kRuntimeWorldIdCacheProbeCount; ++probe) {
        auto& slot = gRuntimeWorldIdCache[(start + probe) & (kRuntimeWorldIdCacheCapacity - 1U)];
        auto expected = static_cast<std::uintptr_t>(0U);
        if (slot.execution.compare_exchange_strong(
                expected, key, std::memory_order_acq_rel, std::memory_order_acquire) ||
            expected == key) {
            slot.high.store(resolved->high, std::memory_order_relaxed);
            slot.low.store(resolved->low, std::memory_order_release);
            return resolved;
        }
    }
    // Cache saturation is non-fatal; the authoritative resolver result is still valid.
    return resolved;
}

void ClearRuntimeWorldIdCache() noexcept {
    for (auto& slot : gRuntimeWorldIdCache) {
        slot.high.store(0U, std::memory_order_relaxed);
        slot.low.store(0U, std::memory_order_relaxed);
        slot.execution.store(0U, std::memory_order_release);
    }
}

[[nodiscard]] bool ReadRuntimeKeySafely(
    const CathodeRuntimeKey* address,
    CathodeRuntimeKey& output) noexcept {
    output = {};
    if (address == nullptr) return false;
    return ReadU32Safely(address, output.entityA) &&
        ReadU32Safely(reinterpret_cast<const std::byte*>(address) + 4U, output.entityB) &&
        output.entityA != 0U;
}

bool CopyBytesSafely(
    const void* source,
    std::byte* destination,
    const std::size_t size) noexcept {
    if (size == 0U) return true;
    if (source == nullptr || destination == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, source, size);
    return true;
#endif
}

[[nodiscard]] bool ModuleRelativeRva(
    const void* address,
    std::uint32_t& rva) noexcept {
    rva = 0U;
    if (address == nullptr) return false;
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    if (moduleBase == 0U || value < moduleBase) return false;
    const auto relative = value - moduleBase;
    if (relative > std::numeric_limits<std::uint32_t>::max()) return false;
    rva = static_cast<std::uint32_t>(relative);
    return true;
}

[[nodiscard]] bool VerifyBuildCallbackDirectCallSite(
    const std::uintptr_t returnAddress,
    std::uint32_t& callSiteRva) noexcept {
    callSiteRva = 0U;
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase == 0U || returnAddress < moduleBase + 5U) return false;

    std::array<std::byte, 5> callBytes{};
    const auto callAddress = returnAddress - 5U;
    if (!CopyBytesSafely(
            reinterpret_cast<const void*>(callAddress),
            callBytes.data(), callBytes.size()) ||
        std::to_integer<std::uint8_t>(callBytes[0]) != 0xE8U) {
        return false;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, callBytes.data() + 1U, sizeof(displacement));
    const auto target = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(returnAddress) + displacement);
    if (target != moduleBase + kFix312BuildCallbackValueRva) return false;

    const auto relative = callAddress - moduleBase;
    if (relative > std::numeric_limits<std::uint32_t>::max()) return false;
    callSiteRva = static_cast<std::uint32_t>(relative);
    return true;
}

[[nodiscard]] bool VerifyQueueTypedMessageDirectCallSite(
    const std::uintptr_t returnAddress,
    std::uint32_t& callSiteRva) noexcept {
    callSiteRva = 0U;
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase == 0U || returnAddress < moduleBase + 5U) return false;

    std::array<std::byte, 5> callBytes{};
    const auto callAddress = returnAddress - 5U;
    if (!CopyBytesSafely(
            reinterpret_cast<const void*>(callAddress),
            callBytes.data(), callBytes.size()) ||
        std::to_integer<std::uint8_t>(callBytes[0]) != 0xE8U) {
        return false;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, callBytes.data() + 1U, sizeof(displacement));
    const auto target = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(returnAddress) + displacement);
    if (target != moduleBase + kFix313QueueTypedMessageRva) return false;

    const auto relative = callAddress - moduleBase;
    if (relative > std::numeric_limits<std::uint32_t>::max()) return false;
    callSiteRva = static_cast<std::uint32_t>(relative);
    return true;
}

[[nodiscard]] bool VerifyQueueInputDirectCallSite(
    const std::uintptr_t returnAddress,
    std::uint32_t& callSiteRva) noexcept {
    callSiteRva = 0U;
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase == 0U || returnAddress < moduleBase + 5U) return false;

    std::array<std::byte, 5> callBytes{};
    const auto callAddress = returnAddress - 5U;
    if (!CopyBytesSafely(
            reinterpret_cast<const void*>(callAddress),
            callBytes.data(), callBytes.size()) ||
        std::to_integer<std::uint8_t>(callBytes[0]) != 0xE8U) {
        return false;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, callBytes.data() + 1U, sizeof(displacement));
    const auto target = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(returnAddress) + displacement);
    if (target != moduleBase + kFix315QueueInputRva) return false;

    const auto relative = callAddress - moduleBase;
    if (relative > std::numeric_limits<std::uint32_t>::max()) return false;
    callSiteRva = static_cast<std::uint32_t>(relative);
    return true;
}

[[nodiscard]] bool Fix315DownstreamProbeActive() noexcept {
    const auto until = gLogicDownstreamProbeUntilTickMs.load(std::memory_order_relaxed);
    if (until == 0U) return false;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now > until) {
        gLogicDownstreamProbeUntilTickMs.store(0U, std::memory_order_relaxed);
        return false;
    }
    return true;
}

class Fix315ExactClearScope final {
public:
    explicit Fix315ExactClearScope(const bool active) noexcept : active_(active) {
        if (!active_) return;
        ++gFix315ExactClearDepth;
        gLogicDownstreamScopes.fetch_add(1U, std::memory_order_relaxed);
    }
    ~Fix315ExactClearScope() noexcept {
        if (active_ && gFix315ExactClearDepth != 0U) --gFix315ExactClearDepth;
    }
    Fix315ExactClearScope(const Fix315ExactClearScope&) = delete;
    Fix315ExactClearScope& operator=(const Fix315ExactClearScope&) = delete;
private:
    bool active_{};
};

void Fix315HashWord(const std::uint32_t word) noexcept {
    auto current = gLogicDownstreamSequenceHash.load(std::memory_order_relaxed);
    for (;;) {
        auto next = current;
        for (std::size_t i = 0; i < sizeof(word); ++i) {
            next ^= static_cast<std::uint8_t>((word >> (i * 8U)) & 0xFFU);
            next *= kFix315HashPrime;
        }
        if (gLogicDownstreamSequenceHash.compare_exchange_weak(
                current, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
}

void CaptureFix315DownstreamEdge(
    WorldEntityRegistry& registry,
    void** sourceSharedRef,
    const std::uint32_t* inputShortGuid,
    void* targetDescriptor,
    void* callbackRecord,
    const std::uintptr_t retailReturnAddress) noexcept {
    if (gFix315ExactClearDepth == 0U || !Fix315DownstreamProbeActive()) return;
    gLogicDownstreamEdges.fetch_add(1U, std::memory_order_relaxed);

    std::uint32_t schedulerParameter = 0U;
    std::uint32_t targetInput = 0U;
    void* sourceShared = nullptr;
    void* targetShared = nullptr;
    const auto* targetBytes = static_cast<const std::byte*>(targetDescriptor);
    if (sourceSharedRef == nullptr || inputShortGuid == nullptr || targetBytes == nullptr ||
        !ReadPointerSafely(sourceSharedRef, sourceShared) || sourceShared == nullptr ||
        !ReadU32Safely(inputShortGuid, schedulerParameter) || schedulerParameter == 0U ||
        !ReadU32Safely(targetBytes, targetInput) || targetInput == 0U ||
        !ReadPointerSafely(targetBytes + 8U, targetShared) || targetShared == nullptr) {
        gLogicDownstreamReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
    auto* targetExecution = ExecutionObjectFromShared(targetShared);
    CathodeRuntimeKey sourceKey{};
    CathodeRuntimeKey targetKey{};
    if (sourceExecution == nullptr || targetExecution == nullptr ||
        !registry.TryExtractRuntimeKey(sourceExecution, sourceKey) ||
        !registry.TryExtractRuntimeKey(targetExecution, targetKey)) {
        gLogicDownstreamReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    std::uint32_t worldMask = 0U;
    if (registry.FindRuntimeWorldEntityId(sourceKey).has_value()) worldMask |= 0x1U;
    if (registry.FindRuntimeWorldEntityId(targetKey).has_value()) worldMask |= 0x2U;

    std::uint32_t callbackFlags = 0U;
    if (callbackRecord != nullptr) {
        gLogicDownstreamCallbackEdges.fetch_add(1U, std::memory_order_relaxed);
        std::array<std::uint32_t, kCallbackRawWordCount> words{};
        if (CopyBytesSafely(callbackRecord, reinterpret_cast<std::byte*>(words.data()), sizeof(words))) {
            callbackFlags = words[8];
        } else {
            gLogicDownstreamReadFailures.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    std::uint32_t callSiteRva = 0U;
    if (retailReturnAddress != 0U &&
        VerifyQueueInputDirectCallSite(retailReturnAddress, callSiteRva)) {
        std::uint32_t expected = 0U;
        static_cast<void>(gLogicDownstreamFirstCallSiteRva.compare_exchange_strong(
            expected, callSiteRva, std::memory_order_relaxed));
        const auto previous = gLogicDownstreamLastCallSiteRva.exchange(
            callSiteRva, std::memory_order_relaxed);
        if (previous != 0U && previous != callSiteRva) {
            gLogicDownstreamCallSiteVaried.store(1U, std::memory_order_relaxed);
        }
    }

    gLogicDownstreamValidEdges.fetch_add(1U, std::memory_order_relaxed);
    gLogicDownstreamWorldMask.store(worldMask, std::memory_order_relaxed);
    gLogicDownstreamSourceA.store(sourceKey.entityA, std::memory_order_relaxed);
    gLogicDownstreamSourceB.store(sourceKey.entityB, std::memory_order_relaxed);
    gLogicDownstreamTargetA.store(targetKey.entityA, std::memory_order_relaxed);
    gLogicDownstreamTargetB.store(targetKey.entityB, std::memory_order_relaxed);
    gLogicDownstreamSchedulerParameter.store(schedulerParameter, std::memory_order_relaxed);
    gLogicDownstreamTargetInput.store(targetInput, std::memory_order_relaxed);
    gLogicDownstreamCallbackFlags.store(callbackFlags, std::memory_order_relaxed);
    gLogicDownstreamThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    gLogicDownstreamTickMs.store(static_cast<std::uint64_t>(GetTickCount64()), std::memory_order_relaxed);

    Fix315HashWord(sourceKey.entityA);
    Fix315HashWord(sourceKey.entityB);
    Fix315HashWord(targetKey.entityA);
    Fix315HashWord(targetKey.entityB);
    Fix315HashWord(schedulerParameter);
    Fix315HashWord(targetInput);
    Fix315HashWord(callbackRecord != nullptr ? 1U : 0U);
    Fix315HashWord(callbackFlags);
    Fix315HashWord(worldMask);
    Fix315HashWord(callSiteRva);
    TouchVersion();
}

[[nodiscard]] bool Fix316PostClearWindowActive(
    std::uint64_t& now,
    std::uint64_t& origin) noexcept {
    const auto until = gPostClearWindowUntilTickMs.load(std::memory_order_relaxed);
    if (until == 0U) return false;
    now = static_cast<std::uint64_t>(GetTickCount64());
    if (now > until) {
        gPostClearWindowUntilTickMs.store(0U, std::memory_order_relaxed);
        return false;
    }
    origin = gPostClearOriginTickMs.load(std::memory_order_relaxed);
    return origin != 0U && now >= origin;
}

void ArmFix316PostClearWindow() noexcept {
    if (!Fix315DownstreamProbeActive()) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    gPostClearOriginTickMs.store(now, std::memory_order_relaxed);
    gPostClearWindowUntilTickMs.store(now + kFix316PostClearWindowMs, std::memory_order_release);
    gPostClearArms.fetch_add(1U, std::memory_order_relaxed);
}

void Fix316HashWord(const std::uint32_t word) noexcept {
    auto current = gPostClearSequenceHash.load(std::memory_order_relaxed);
    for (;;) {
        auto next = current;
        for (std::size_t i = 0; i < sizeof(word); ++i) {
            next ^= static_cast<std::uint8_t>((word >> (i * 8U)) & 0xFFU);
            next *= kFix316HashPrime;
        }
        if (gPostClearSequenceHash.compare_exchange_weak(
                current, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
}

void CaptureFix316PostClearEdge(
    WorldEntityRegistry& registry,
    void** sourceSharedRef,
    const std::uint32_t* inputShortGuid,
    void* targetDescriptor,
    void* callbackRecord,
    const std::uintptr_t retailReturnAddress) noexcept {
    std::uint64_t now = 0U;
    std::uint64_t origin = 0U;
    if (!Fix316PostClearWindowActive(now, origin)) return;

    const auto seen = gPostClearSeen.fetch_add(1U, std::memory_order_relaxed);
    if (seen >= kFix316PostClearInspectLimit) {
        gPostClearInspectLimitDrops.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    std::uint32_t schedulerParameter = 0U;
    std::uint32_t targetInput = 0U;
    void* sourceShared = nullptr;
    void* targetShared = nullptr;
    const auto* targetBytes = static_cast<const std::byte*>(targetDescriptor);
    if (sourceSharedRef == nullptr || inputShortGuid == nullptr || targetBytes == nullptr ||
        !ReadPointerSafely(sourceSharedRef, sourceShared) || sourceShared == nullptr ||
        !ReadU32Safely(inputShortGuid, schedulerParameter) || schedulerParameter == 0U ||
        !ReadU32Safely(targetBytes, targetInput) || targetInput == 0U ||
        !ReadPointerSafely(targetBytes + 8U, targetShared) || targetShared == nullptr) {
        return;
    }

    auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
    auto* targetExecution = ExecutionObjectFromShared(targetShared);
    CathodeRuntimeKey sourceKey{};
    CathodeRuntimeKey targetKey{};
    const bool sourceKeyValid = sourceExecution != nullptr &&
        registry.TryExtractRuntimeKey(sourceExecution, sourceKey);
    const bool targetKeyValid = targetExecution != nullptr &&
        registry.TryExtractRuntimeKey(targetExecution, targetKey);

    std::uint32_t role = 0U;
    if (sourceKeyValid && sourceKey.entityA == kFix311ExactStateKeyA &&
        sourceKey.entityB == kFix311ExactStateKeyB) role |= 0x1U;
    if (targetKeyValid && targetKey.entityA == kFix311ExactStateKeyA &&
        targetKey.entityB == kFix311ExactStateKeyB) role |= 0x2U;
    if (role == 0U) return;

    gPostClearAnchorMatches.fetch_add(1U, std::memory_order_relaxed);
    if (!sourceKeyValid || !targetKeyValid) {
        gPostClearReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    std::uint32_t worldMask = 0U;
    if (registry.FindRuntimeWorldEntityId(sourceKey).has_value()) worldMask |= 0x1U;
    if (registry.FindRuntimeWorldEntityId(targetKey).has_value()) worldMask |= 0x2U;

    std::uint32_t callbackFlags = 0U;
    if (callbackRecord != nullptr) {
        gPostClearCallbackEdges.fetch_add(1U, std::memory_order_relaxed);
        std::array<std::uint32_t, kCallbackRawWordCount> words{};
        if (CopyBytesSafely(callbackRecord, reinterpret_cast<std::byte*>(words.data()), sizeof(words))) {
            callbackFlags = words[8];
        } else {
            gPostClearReadFailures.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    std::uint32_t callSiteRva = 0U;
    if (retailReturnAddress != 0U &&
        VerifyQueueInputDirectCallSite(retailReturnAddress, callSiteRva)) {
        std::uint32_t expected = 0U;
        static_cast<void>(gPostClearFirstCallSiteRva.compare_exchange_strong(
            expected, callSiteRva, std::memory_order_relaxed));
        const auto previous = gPostClearLastCallSiteRva.exchange(
            callSiteRva, std::memory_order_relaxed);
        if (previous != 0U && previous != callSiteRva) {
            gPostClearCallSiteVaried.store(1U, std::memory_order_relaxed);
        }
    }

    const auto delta64 = now - origin;
    const auto delta = static_cast<std::uint32_t>(delta64 >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
        ? std::numeric_limits<std::uint32_t>::max()
        : delta64);
    const auto validIndex = gPostClearValidEdges.fetch_add(1U, std::memory_order_relaxed);
    if (validIndex == 0U) {
        gPostClearFirstDeltaMs.store(delta, std::memory_order_relaxed);
    }
    gPostClearLastDeltaMs.store(delta, std::memory_order_relaxed);

    gPostClearAnchorRole.store(role, std::memory_order_relaxed);
    gPostClearWorldMask.store(worldMask, std::memory_order_relaxed);
    gPostClearSourceA.store(sourceKey.entityA, std::memory_order_relaxed);
    gPostClearSourceB.store(sourceKey.entityB, std::memory_order_relaxed);
    gPostClearTargetA.store(targetKey.entityA, std::memory_order_relaxed);
    gPostClearTargetB.store(targetKey.entityB, std::memory_order_relaxed);
    gPostClearSchedulerParameter.store(schedulerParameter, std::memory_order_relaxed);
    gPostClearTargetInput.store(targetInput, std::memory_order_relaxed);
    gPostClearCallbackFlags.store(callbackFlags, std::memory_order_relaxed);
    gPostClearThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    gPostClearTickMs.store(now, std::memory_order_relaxed);

    Fix316HashWord(sourceKey.entityA);
    Fix316HashWord(sourceKey.entityB);
    Fix316HashWord(targetKey.entityA);
    Fix316HashWord(targetKey.entityB);
    Fix316HashWord(schedulerParameter);
    Fix316HashWord(targetInput);
    Fix316HashWord(callbackRecord != nullptr ? 1U : 0U);
    Fix316HashWord(callbackFlags);
    Fix316HashWord(role);
    Fix316HashWord(worldMask);
    Fix316HashWord(callSiteRva);
    TouchVersion();
}

void CaptureFix319CallbackBuild(
    WorldEntityRegistry& registry,
    void** sourceSharedRef,
    std::uint32_t constructorFlags,
    std::size_t payloadSize,
    const void* payloadData,
    bool remoteApply,
    std::uintptr_t returnAddress) noexcept;

void** __fastcall BuildCallbackValueDetour(
    void* manager,
    void*,
    void** destination,
    void** sourceSharedRef,
    const std::uint32_t constructorFlags,
    const std::size_t payloadSize,
    void* payloadData) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t returnAddress = 0U;
#endif
    const auto original = reinterpret_cast<BuildCallbackValueFn>(
        gBuildCallbackValueTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return destination;
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }

    if (gInitialized.load(std::memory_order_acquire)) {
        auto* traceRegistry = gRegistry.load(std::memory_order_acquire);
        if (traceRegistry != nullptr && traceRegistry->RuntimeCaptureReady()) {
            CaptureFix319CallbackBuild(
                *traceRegistry, sourceSharedRef, constructorFlags, payloadSize, payloadData,
                replication_core::RemoteApplyActive(), returnAddress);
        }
    }

    const auto until = gCharacterCallbackOriginProbeUntilTickMs.load(
        std::memory_order_relaxed);
    if (!gInitialized.load(std::memory_order_acquire) || until == 0U ||
        constructorFlags != kFix312CharacterCallbackFlags || payloadSize != 0U) {
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }

    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now > until) {
        gCharacterCallbackOriginProbeUntilTickMs.store(0U, std::memory_order_relaxed);
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }

    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr || !registry->RuntimeCaptureReady()) {
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }

    void* sourceShared = nullptr;
    if (!ReadPointerSafely(sourceSharedRef, sourceShared) || sourceShared == nullptr) {
        gCharacterCallbackOriginReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }
    auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
    if (sourceExecution == nullptr) {
        gCharacterCallbackOriginReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }

    void* sourceVtable = nullptr;
    std::uint32_t sourceVtableRva = 0U;
    if (!ReadPointerSafely(sourceExecution, sourceVtable) ||
        !ModuleRelativeRva(sourceVtable, sourceVtableRva)) {
        gCharacterCallbackOriginReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }
    if (sourceVtableRva != kFix312CharacterExecutionVtableRva) {
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }

    gCharacterCallbackOriginCandidates.fetch_add(1U, std::memory_order_relaxed);
    CathodeRuntimeKey sourceKey{};
    if (!registry->TryExtractRuntimeKey(sourceExecution, sourceKey) ||
        sourceKey.entityA == 0U) {
        gCharacterCallbackOriginReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }

    gCharacterCallbackOriginSourceA.store(sourceKey.entityA, std::memory_order_relaxed);
    gCharacterCallbackOriginSourceB.store(sourceKey.entityB, std::memory_order_relaxed);
    gCharacterCallbackOriginExecutionVtableRva.store(sourceVtableRva, std::memory_order_relaxed);
    gCharacterCallbackOriginFlags.store(constructorFlags, std::memory_order_relaxed);
    gCharacterCallbackOriginPayloadBytes.store(
        static_cast<std::uint32_t>(payloadSize), std::memory_order_relaxed);
    gCharacterCallbackOriginThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    gCharacterCallbackOriginTickMs.store(now, std::memory_order_relaxed);

    const bool exactKey = sourceKey.entityA == kFix312CharacterCallbackKeyA &&
        sourceKey.entityB == kFix312CharacterCallbackKeyB;
    if (exactKey) {
        gCharacterCallbackOriginExactKeyMatches.fetch_add(1U, std::memory_order_relaxed);
    } else {
        TouchVersion();
        return original(manager, destination, sourceSharedRef, constructorFlags,
            payloadSize, payloadData);
    }

    std::uint32_t callSiteRva = 0U;
    if (returnAddress != 0U && VerifyBuildCallbackDirectCallSite(returnAddress, callSiteRva)) {
        gCharacterCallbackOriginCallVerified.fetch_add(1U, std::memory_order_relaxed);
        auto expectedFirst = 0U;
        static_cast<void>(gCharacterCallbackOriginFirstCallSiteRva.compare_exchange_strong(
            expectedFirst, callSiteRva, std::memory_order_relaxed, std::memory_order_relaxed));
        const auto previous = gCharacterCallbackOriginLastCallSiteRva.exchange(
            callSiteRva, std::memory_order_relaxed);
        if (previous != 0U && previous != callSiteRva) {
            gCharacterCallbackOriginCallSiteVaried.store(1U, std::memory_order_relaxed);
        }
    }
    TouchVersion();

    return original(manager, destination, sourceSharedRef, constructorFlags,
        payloadSize, payloadData);
}

void CommitCallbackAbiProbeSnapshot(
    const SchedulerCausalSnapshot& snapshot,
    const WorldEntityId& stateEntity) noexcept {
    if (!snapshot.callbackRecordPresent || !snapshot.callbackRawReadable ||
        snapshot.queueCallerRva != kCallbackAbiProbeCallerRva) {
        return;
    }

    // A state transition has now selected this previously captured, read-only
    // work-item snapshot. No process-local callback pointer is retained here.
    gCallbackAbiProbeInputMatches.fetch_add(1U, std::memory_order_relaxed);

    auto sampleCount = gCallbackAbiProbeSamplesCaptured.load(std::memory_order_relaxed);
    while (sampleCount < kCallbackAbiProbeSampleLimit &&
        !gCallbackAbiProbeSamplesCaptured.compare_exchange_weak(
            sampleCount,
            sampleCount + 1U,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
    }
    if (sampleCount >= kCallbackAbiProbeSampleLimit) {
        gCallbackAbiProbeSampleLimitDrops.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    const auto& words = snapshot.callbackRawWords;
    gCallbackRecordsObserved.fetch_add(1U, std::memory_order_relaxed);
    gCallbackRawReadable.fetch_add(1U, std::memory_order_relaxed);

    std::uint32_t shapeFlags = 0U;
    if (words[0] == kCallbackWorkItemInputShortGuid) {
        shapeFlags |= kCallbackAbiShapeInputIsCallback;
    }
    if (words[3] != 0U) {
        shapeFlags |= kCallbackAbiShapeNestedPresent;
    }
    if (words[4] != 0U || words[5] != 0U) {
        shapeFlags |= kCallbackAbiShapePayloadPresent;
    }

    // Exact-nested samples may already have resolved +0x08 while the retail
    // call was active. Temporal samples intentionally do not dereference the
    // process-local shared-ref after FUN_009271C0 has returned.
    if (snapshot.callbackWorldEntity) {
        shapeFlags |= kCallbackAbiShapeTargetResolved;
        gCallbackWorldEntityResolved.fetch_add(1U, std::memory_order_relaxed);
        gCallbackAbiProbeTargetResolved.fetch_add(1U, std::memory_order_relaxed);
        gCallbackAbiProbeLastTargetHigh.store(snapshot.callbackEntity.high, std::memory_order_relaxed);
        gCallbackAbiProbeLastTargetLow.store(snapshot.callbackEntity.low, std::memory_order_relaxed);
        if (snapshot.callbackSameAsSource) {
            shapeFlags |= kCallbackAbiShapeTargetSameAsSource;
            gCallbackSameAsSource.fetch_add(1U, std::memory_order_relaxed);
        }
        if (snapshot.callbackSameAsTarget) {
            shapeFlags |= kCallbackAbiShapeTargetSameAsTarget;
            gCallbackSameAsTarget.fetch_add(1U, std::memory_order_relaxed);
        }
        if (snapshot.callbackEntity == stateEntity) {
            shapeFlags |= kCallbackAbiShapeTargetSameAsState;
        }
    }

    const auto callbackFlags = words[8];
    bool expectedFirstFlagsSet = false;
    if (gCallbackAbiProbeFirstFlagsSet.compare_exchange_strong(
            expectedFirstFlagsSet, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        gCallbackAbiProbeFirstFlags.store(callbackFlags, std::memory_order_release);
    } else if (gCallbackAbiProbeFirstFlags.load(std::memory_order_acquire) != callbackFlags) {
        gCallbackAbiProbeFlagsVaried.store(true, std::memory_order_relaxed);
    }
    gCallbackAbiProbeLastInputShortGuid.store(words[0], std::memory_order_relaxed);
    gCallbackAbiProbeLastFlags.store(callbackFlags, std::memory_order_relaxed);
    gCallbackAbiProbeLastRefCount.store(words[9], std::memory_order_relaxed);
    gCallbackAbiProbeLastShapeFlags.store(shapeFlags, std::memory_order_release);
}

void CaptureCallbackAbiProbeSample(
    SchedulerCausalSnapshot& snapshot,
    void* callbackRecord,
    const WorldEntityId& stateEntity) noexcept {
    if (callbackRecord == nullptr || !snapshot.callbackRecordPresent ||
        snapshot.queueCallerRva != kCallbackAbiProbeCallerRva) {
        return;
    }

    std::array<std::uint32_t, kCallbackRawWordCount> words{};
    if (!CopyBytesSafely(
            callbackRecord,
            reinterpret_cast<std::byte*>(words.data()),
            sizeof(words))) {
        gCallbackAbiProbeRawReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    snapshot.callbackRawWords = words;
    snapshot.callbackRawReadable = true;

    // +0x08 is process-local. Resolve it only for an exact nested transition,
    // while the retail queue call and callback owner are still alive.
    const auto callbackSharedAddress = static_cast<std::uintptr_t>(words[2]);
    auto* registry = gRegistry.load(std::memory_order_relaxed);
    if (callbackSharedAddress != 0U && registry != nullptr && registry->RuntimeCaptureReady()) {
        auto* callbackExecution = ExecutionObjectFromShared(
            reinterpret_cast<void*>(callbackSharedAddress));
        if (callbackExecution != nullptr) {
            const auto callbackId = IdentifyRuntimeWorldEntityCached(*registry, callbackExecution);
            if (callbackId.has_value()) {
                snapshot.callbackEntity = *callbackId;
                snapshot.callbackWorldEntity = true;
                snapshot.callbackSameAsSource = *callbackId == snapshot.source;
                snapshot.callbackSameAsTarget = *callbackId == snapshot.target;
            }
        }
    }

    CommitCallbackAbiProbeSnapshot(snapshot, stateEntity);
}

[[nodiscard]] std::uint64_t TypedFingerprint(
    const ObservedTypedMessage& message,
    const std::span<const std::byte> data) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](const std::uint64_t value) {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            hash ^= (value >> shift) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    mix(message.messageType);
    mix(message.sourceRuntimeKey.entityA);
    mix(message.sourceRuntimeKey.entityB);
    mix(message.targetRuntimeKey.entityA);
    mix(message.targetRuntimeKey.entityB);
    mix(std::bit_cast<std::uint64_t>(message.relativeDelaySeconds));
    mix(data.size());
    for (const auto value : data) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

[[nodiscard]] std::uint64_t CharacterCallbackFingerprint(
    const ObservedCharacterCallbackInputEdge& edge,
    const WorldEntityId& targetEntity) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](const std::uint64_t value) {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            hash ^= (value >> shift) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    mix(targetEntity.high); mix(targetEntity.low);
    mix(edge.sourceEntity.high); mix(edge.sourceEntity.low);
    mix(edge.callbackEntity.high); mix(edge.callbackEntity.low);
    mix(edge.schedulerParameterShortGuid); mix(edge.targetInputShortGuid);
    mix(edge.callbackInputShortGuid); mix(edge.callbackConstructorFlags);
    mix(edge.callbackExecutionVtableRva); mix(edge.callbackFamilyCallerRva);
    mix(std::bit_cast<std::uint32_t>(edge.relativeDelaySeconds));
    return hash == 0U ? 1U : hash;
}

[[nodiscard]] bool RememberCharacterCallbackMatch(const std::uint64_t hash) noexcept {
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto start = static_cast<std::size_t>(hash) & (kEchoSlots - 1U);
    for (std::size_t probe = 0U; probe < kEchoProbeCount; ++probe) {
        const auto index = (start + probe) & (kEchoSlots - 1U);
        const auto current = gCharacterCallbackMatchHashes[index].load(std::memory_order_acquire);
        const auto expiry = gCharacterCallbackMatchExpiryMs[index].load(std::memory_order_acquire);
        if (current == hash || current == 0U || expiry <= now) {
            gCharacterCallbackMatchHashes[index].store(hash, std::memory_order_release);
            gCharacterCallbackMatchExpiryMs[index].store(
                now + kFix320SemanticMatchLifetimeMs, std::memory_order_release);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool ConsumeCharacterCallbackMatch(const std::uint64_t hash) noexcept {
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto start = static_cast<std::size_t>(hash) & (kEchoSlots - 1U);
    for (std::size_t probe = 0U; probe < kEchoProbeCount; ++probe) {
        const auto index = (start + probe) & (kEchoSlots - 1U);
        auto current = gCharacterCallbackMatchHashes[index].load(std::memory_order_acquire);
        if (current == 0U) continue;
        if (gCharacterCallbackMatchExpiryMs[index].load(std::memory_order_acquire) <= now) {
            gCharacterCallbackMatchHashes[index].compare_exchange_strong(
                current, 0U, std::memory_order_acq_rel, std::memory_order_acquire);
            continue;
        }
        if (current == hash && gCharacterCallbackMatchHashes[index].compare_exchange_strong(
                current, 0U, std::memory_order_acq_rel, std::memory_order_acquire)) {
            gCharacterCallbackMatchExpiryMs[index].store(0U, std::memory_order_release);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool Fix320ReplayActive() noexcept {
    if (world_actions::Installed()) {
        gFix320ReplayUntilTickMs.store(0U, std::memory_order_relaxed);
        return false;
    }
    const auto until = gFix320ReplayUntilTickMs.load(std::memory_order_relaxed);
    if (until == 0U) return false;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now <= until) return true;
    gFix320ReplayUntilTickMs.store(0U, std::memory_order_relaxed);
    return false;
}

[[nodiscard]] bool TryDescribeFix320CharacterCallbackEdge(
    WorldEntityRegistry& registry,
    void** sourceSharedRef,
    const std::uint32_t* inputShortGuid,
    void* targetDescriptor,
    void* callbackRecord,
    const double delaySeconds,
    const std::uintptr_t retailReturnAddress,
    ObservedCharacterCallbackInputEdge& edge,
    WorldEntityId& targetEntity) noexcept {
    edge = {};
    targetEntity = {};
    if (callbackRecord == nullptr || sourceSharedRef == nullptr || inputShortGuid == nullptr ||
        targetDescriptor == nullptr) return false;

    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase == 0U || retailReturnAddress < moduleBase) return false;
    const auto relative = retailReturnAddress - moduleBase;
    if (relative > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint32_t>(relative) != kCharacterCallbackFamilyCallerRva) return false;

    std::uint32_t schedulerParameter = 0U;
    std::uint32_t targetInput = 0U;
    float targetDelay = 0.0F;
    void* targetShared = nullptr;
    const auto* targetBytes = static_cast<const std::byte*>(targetDescriptor);
    if (!ReadU32Safely(inputShortGuid, schedulerParameter) || schedulerParameter == 0U ||
        !ReadU32Safely(targetBytes, targetInput) || targetInput == 0U ||
        !ReadFloatSafely(targetBytes + 4U, targetDelay) ||
        !ReadPointerSafely(targetBytes + 8U, targetShared) || targetShared == nullptr) return false;

    std::array<std::uint32_t, kCallbackRawWordCount> words{};
    void* callbackShared = nullptr;
    if (!CopyBytesSafely(callbackRecord, reinterpret_cast<std::byte*>(words.data()), sizeof(words)) ||
        words[0] != kCharacterCallbackInputShortGuid || words[3] != 0U ||
        words[4] != 0U || words[5] != 0U ||
        words[8] != kCharacterCallbackConstructorFlags ||
        !ReadPointerSafely(static_cast<const std::byte*>(callbackRecord) + 8U, callbackShared) ||
        callbackShared == nullptr) return false;

    void* sourceShared = nullptr;
    if (!ReadPointerSafely(sourceSharedRef, sourceShared) || sourceShared == nullptr) return false;
    auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
    auto* targetExecution = ExecutionObjectFromShared(targetShared);
    auto* callbackExecution = ExecutionObjectFromShared(callbackShared);
    if (sourceExecution == nullptr || targetExecution == nullptr || callbackExecution == nullptr) return false;

    void* callbackVtable = nullptr;
    std::uint32_t callbackVtableRva = 0U;
    if (!ReadPointerSafely(callbackExecution, callbackVtable) ||
        !ModuleRelativeRva(callbackVtable, callbackVtableRva) ||
        callbackVtableRva != kCharacterCallbackExecutionVtableRva) return false;

    const auto sourceId = IdentifyRuntimeWorldEntityCached(registry, sourceExecution);
    const auto targetId = IdentifyRuntimeWorldEntityCached(registry, targetExecution);
    CathodeRuntimeKey callbackKey{};
    if (!sourceId.has_value() || !targetId.has_value() ||
        !registry.TryExtractRuntimeKey(callbackExecution, callbackKey)) return false;
    const auto callbackId = registry.FindRuntimeWorldEntityId(callbackKey);
    if (!callbackId.has_value()) return false;

    const auto relativeDelay = static_cast<double>(targetDelay) + delaySeconds;
    if (!std::isfinite(relativeDelay) || relativeDelay < 0.0 || relativeDelay > 600.0) return false;

    edge.sourceCategory = SourceCategory::GenericWorld;
    edge.schedulerParameterShortGuid = schedulerParameter;
    edge.targetInputShortGuid = targetInput;
    edge.callbackInputShortGuid = kCharacterCallbackInputShortGuid;
    edge.callbackConstructorFlags = kCharacterCallbackConstructorFlags;
    edge.callbackExecutionVtableRva = kCharacterCallbackExecutionVtableRva;
    edge.callbackFamilyCallerRva = kCharacterCallbackFamilyCallerRva;
    edge.sourceEntity = *sourceId;
    edge.callbackEntity = *callbackId;
    edge.relativeDelaySeconds = static_cast<float>(relativeDelay);
    targetEntity = *targetId;
    return true;
}

[[nodiscard]] bool RememberTypedMatch(const std::uint64_t hash) noexcept {
    constexpr std::uint64_t kLifetimeMs = 5000U;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto start = static_cast<std::size_t>(hash) & (kEchoSlots - 1U);
    for (std::size_t probe = 0U; probe < kEchoProbeCount; ++probe) {
        const auto index = (start + probe) & (kEchoSlots - 1U);
        auto current = gTypedMatchHashes[index].load(std::memory_order_acquire);
        const auto expiry = gTypedMatchExpiryMs[index].load(std::memory_order_acquire);
        if (current != 0U && expiry > now) continue;
        if (current != 0U) {
            gTypedMatchHashes[index].compare_exchange_strong(
                current, 0U, std::memory_order_acq_rel, std::memory_order_acquire);
        }
        auto expected = std::uint64_t{0U};
        if (gTypedMatchHashes[index].compare_exchange_strong(
                expected, hash, std::memory_order_acq_rel, std::memory_order_acquire)) {
            gTypedMatchExpiryMs[index].store(now + kLifetimeMs, std::memory_order_release);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool ConsumeTypedMatch(const std::uint64_t hash) noexcept {
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto start = static_cast<std::size_t>(hash) & (kEchoSlots - 1U);
    for (std::size_t probe = 0U; probe < kEchoProbeCount; ++probe) {
        const auto index = (start + probe) & (kEchoSlots - 1U);
        auto current = gTypedMatchHashes[index].load(std::memory_order_acquire);
        if (current != hash) continue;
        if (gTypedMatchExpiryMs[index].load(std::memory_order_acquire) <= now) {
            gTypedMatchHashes[index].compare_exchange_strong(
                current, 0U, std::memory_order_acq_rel, std::memory_order_acquire);
            continue;
        }
        if (gTypedMatchHashes[index].compare_exchange_strong(
                current, 0U, std::memory_order_acq_rel, std::memory_order_acquire)) {
            gTypedMatchExpiryMs[index].store(0U, std::memory_order_release);
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint64_t NextObservationId() noexcept {
    // Skip the single zero encoding; keep every emitted ID invertible.
    for (;;) {
        const auto sequence = gObservationSequence.fetch_add(1U, std::memory_order_acq_rel);
        const auto value = gObservationSalt ^ (sequence * kObservationMultiplier);
        if (sequence != 0U && value != 0U) return value;
    }
}

[[nodiscard]] bool RememberLocalObservation(const std::uint64_t observationId) noexcept {
    if (observationId == 0U) return false;
    std::lock_guard lock(gLocalObservationMutex);
    const auto serial = ObservationSerial(observationId, gObservationSalt);
    const bool existed = gLocalObservations.Contains(serial);
    if (gLocalObservations.Remember(serial)) {
        if (!existed) ++gLocalObservationRemembered;
        return true;
    }
    gEchoTableFull.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return false;
}

[[nodiscard]] bool RemoveLocalObservation(
    const std::uint64_t observationId,
    const bool countAsEcho) noexcept {
    if (observationId == 0U) return false;
    std::lock_guard lock(gLocalObservationMutex);
    if (gLocalObservations.Consume(ObservationSerial(observationId, gObservationSalt))) {
        if (countAsEcho) {
            ++gLocalObservationEchoed;
            gOriginEchoSuppressed.fetch_add(1U, std::memory_order_acq_rel);
        } else ++gLocalObservationRetired;
        TouchVersion();
        return true;
    }
    ++gLocalObservationMissing;
    return false;
}

[[nodiscard]] bool SameCompositeInput(
    const CompositeInputSemanticSlot& slot,
    const WorldEntityId& owner,
    const std::uint32_t publicInputShortGuid) noexcept {
    return slot.owner == owner && slot.publicInputShortGuid == publicInputShortGuid;
}

[[nodiscard]] bool AllowLocalCompositePublicInput(
    const WorldEntityId& owner,
    const std::uint32_t publicInputShortGuid) noexcept {
    if (!owner.Valid() || publicInputShortGuid == 0U) return false;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    std::lock_guard lock(gCompositeInputSemanticMutex);

    CompositeInputSemanticSlot* reusable = nullptr;
    CompositeInputSemanticSlot* oldest = nullptr;
    for (auto& slot : gCompositeInputSemanticSlots) {
        if (SameCompositeInput(slot, owner, publicInputShortGuid)) {
            if ((slot.remoteGuardUntilTickMs != 0U && now <= slot.remoteGuardUntilTickMs) ||
                (slot.lastLocalTickMs != 0U && now >= slot.lastLocalTickMs &&
                 now - slot.lastLocalTickMs <= kCompositeInputLocalDedupeMs)) {
                return false;
            }
            reusable = &slot;
            break;
        }
        if (!slot.owner.Valid()) reusable = reusable == nullptr ? &slot : reusable;
        if (oldest == nullptr || slot.lastLocalTickMs < oldest->lastLocalTickMs) oldest = &slot;
    }

    if (gCompositeInputRateWindowTickMs == 0U || now < gCompositeInputRateWindowTickMs ||
        now - gCompositeInputRateWindowTickMs >= 1000U) {
        gCompositeInputRateWindowTickMs = now;
        gCompositeInputRateWindowCount = 0U;
    }
    if (gCompositeInputRateWindowCount >= kCompositeInputMaximumSubmissionsPerSecond) {
        return false;
    }
    ++gCompositeInputRateWindowCount;

    auto* slot = reusable != nullptr ? reusable : oldest;
    if (slot == nullptr) return false;
    if (!SameCompositeInput(*slot, owner, publicInputShortGuid)) {
        // A recycled slot may still carry a chain guard from a different
        // Composite input. Never let stale semantic state suppress a newly
        // admitted public input.
        *slot = {};
    }
    slot->owner = owner;
    slot->publicInputShortGuid = publicInputShortGuid;
    slot->lastLocalTickMs = now;
    slot->remoteGuardUntilTickMs = 0U;
    return true;
}

[[nodiscard]] bool CompositeLocalChainGuardActive(
    const WorldEntityId& owner,
    const std::uint32_t publicInputShortGuid) noexcept {
    if (!owner.Valid() || publicInputShortGuid == 0U) return false;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    std::lock_guard lock(gCompositeInputSemanticMutex);
    for (const auto& slot : gCompositeInputSemanticSlots) {
        if (SameCompositeInput(slot, owner, publicInputShortGuid)) {
            return slot.localChainGuardUntilTickMs != 0U &&
                now <= slot.localChainGuardUntilTickMs;
        }
    }
    return false;
}

void RememberCompositeLocalChainGuard(
    const WorldEntityId& owner,
    const std::uint32_t publicInputShortGuid) noexcept {
    if (!owner.Valid() || publicInputShortGuid == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    std::lock_guard lock(gCompositeInputSemanticMutex);
    CompositeInputSemanticSlot* reusable = nullptr;
    CompositeInputSemanticSlot* oldest = nullptr;
    for (auto& slot : gCompositeInputSemanticSlots) {
        if (SameCompositeInput(slot, owner, publicInputShortGuid)) {
            reusable = &slot;
            break;
        }
        if (!slot.owner.Valid()) reusable = reusable == nullptr ? &slot : reusable;
        if (oldest == nullptr || slot.localChainGuardUntilTickMs < oldest->localChainGuardUntilTickMs) {
            oldest = &slot;
        }
    }
    auto* slot = reusable != nullptr ? reusable : oldest;
    if (slot == nullptr) return;
    if (!SameCompositeInput(*slot, owner, publicInputShortGuid)) {
        *slot = {};
        slot->owner = owner;
        slot->publicInputShortGuid = publicInputShortGuid;
    }
    slot->localChainGuardUntilTickMs = now + kCompositeInputLocalChainGuardMs;
}

void RememberRemoteCompositePublicInput(
    const WorldEntityId& owner,
    const std::uint32_t publicInputShortGuid) noexcept {
    if (!owner.Valid() || publicInputShortGuid == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    std::lock_guard lock(gCompositeInputSemanticMutex);
    CompositeInputSemanticSlot* reusable = nullptr;
    CompositeInputSemanticSlot* oldest = nullptr;
    for (auto& slot : gCompositeInputSemanticSlots) {
        if (SameCompositeInput(slot, owner, publicInputShortGuid)) {
            reusable = &slot;
            break;
        }
        if (!slot.owner.Valid()) reusable = reusable == nullptr ? &slot : reusable;
        if (oldest == nullptr || slot.remoteGuardUntilTickMs < oldest->remoteGuardUntilTickMs) {
            oldest = &slot;
        }
    }
    auto* slot = reusable != nullptr ? reusable : oldest;
    if (slot == nullptr) return;
    if (!SameCompositeInput(*slot, owner, publicInputShortGuid)) {
        *slot = {};
        slot->owner = owner;
        slot->publicInputShortGuid = publicInputShortGuid;
    }
    slot->remoteGuardUntilTickMs = now + kCompositeInputRemoteGuardMs;
}

class CompositeMappedQueueScope final {
public:
    explicit CompositeMappedQueueScope(const bool active) noexcept : active_(active) {
        if (active_) ++gCompositeMappedQueueDepth;
    }
    ~CompositeMappedQueueScope() {
        if (active_ && gCompositeMappedQueueDepth != 0U) --gCompositeMappedQueueDepth;
    }
private:
    bool active_{};
};

[[nodiscard]] bool SubmitCompositePublicInput(
    WorldEntityRegistry& registry,
    const WorldCompositePublicInputDescriptor& mapping) noexcept {
    if (world_actions::Installed()) return false;
    if (!mapping.noTypedData || !mapping.ownerCompositeInstance.Valid() ||
        mapping.publicInputShortGuid == 0U ||
        !registry.IsCompositePublicInputAllowed(
            mapping.ownerCompositeInstance, mapping.publicInputShortGuid)) {
        return false;
    }
    if (!AllowLocalCompositePublicInput(
            mapping.ownerCompositeInstance, mapping.publicInputShortGuid)) {
        return false;
    }

    CompositePublicInputEvent event{};
    event.observationId = NextObservationId();
    event.publicInputShortGuid = mapping.publicInputShortGuid;
    WorldPayload payload{};
    if (!EncodeCompositePublicInputPayload(event, payload)) return false;
    if (!RememberLocalObservation(event.observationId)) return false;
    if (replication_core::SubmitLocalIntent(
            mapping.ownerCompositeInstance,
            WorldMutationKind::Event,
            kOperationCompositePublicInput,
            std::span<const std::byte>(payload.bytes.data(), payload.size))) {
        gCompositePublicInputSubmitted.fetch_add(1U, std::memory_order_relaxed);
        TouchVersion();
        return true;
    }
    static_cast<void>(RemoveLocalObservation(event.observationId, false));
    return false;
}

void SubmitCompositePublicInputForInternalEdge(
    WorldEntityRegistry& registry,
    const WorldEntityId& internalTarget,
    const std::uint32_t internalTargetInputShortGuid) noexcept {
    auto mapping = registry.FindCompositePublicInputForInternalEdge(
        internalTarget, internalTargetInputShortGuid);
    if (!mapping.has_value()) {
        // BuildFix05: the direct public bridge is not necessarily the runtime
        // endpoint that mutates. Fall back only to a compiler-generated unique
        // descendant anchor. The internal endpoint remains process-local; the
        // transported event is still owner CompositeInstance + public input.
        mapping = registry.FindCompositePublicInputForCanonicalAnchor(
            internalTarget, internalTargetInputShortGuid);
    }
    if (!mapping.has_value() || CompositeLocalChainGuardActive(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid)) {
        return;
    }
    gCompositePublicInputMapped.fetch_add(1U, std::memory_order_relaxed);
    if (SubmitCompositePublicInput(registry, *mapping)) {
        // The observed internal endpoint can be only the first visible leaf of
        // a larger asynchronous Composite fan-out. Keep the existing semantic
        // chain guard active so later leaves do not emit duplicate public
        // operations after the QueueInput stack has unwound.
        RememberCompositeLocalChainGuard(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid);
    }
}

void SubmitCompositePublicInputForRuntimeEdge(
    WorldEntityRegistry& registry,
    const CathodeRuntimeKey& internalTarget,
    const std::uint32_t internalTargetInputShortGuid) noexcept {
    const auto mapping = registry.FindCompositePublicInputForRuntimeEdge(
        internalTarget, internalTargetInputShortGuid);
    if (!mapping.has_value() || CompositeLocalChainGuardActive(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid)) {
        return;
    }
    gCompositePublicInputMapped.fetch_add(1U, std::memory_order_relaxed);
    gCompositePublicInputRuntimeEdgeMapped.fetch_add(1U, std::memory_order_relaxed);
    if (SubmitCompositePublicInput(registry, *mapping)) {
        RememberCompositeLocalChainGuard(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid);
    }
}

void SubmitCompositePublicInputForOwnerScopeRuntimeEdge(
    WorldEntityRegistry& registry,
    const CathodeRuntimeKey& internalTarget,
    const std::uint32_t internalTargetInputShortGuid) noexcept {
    const auto mapping = registry.FindCompositePublicInputForOwnerScopeRuntimeEdge(
        internalTarget, internalTargetInputShortGuid);
    if (!mapping.has_value() || CompositeLocalChainGuardActive(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid)) {
        return;
    }
    gCompositePublicInputMapped.fetch_add(1U, std::memory_order_relaxed);
    gCompositePublicInputOwnerScopeRuntimeEdgeMapped.fetch_add(
        1U, std::memory_order_relaxed);
    if (SubmitCompositePublicInput(registry, *mapping)) {
        RememberCompositeLocalChainGuard(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid);
    }
}

[[nodiscard]] bool HasLogicSwitchVtable(const void* nativeSwitch) noexcept {
    void* vtable = nullptr;
    return nativeSwitch != nullptr && ReadPointerSafely(nativeSwitch, vtable) &&
        reinterpret_cast<std::uintptr_t>(vtable) ==
            gLogicSwitchVtable.load(std::memory_order_acquire);
}

[[nodiscard]] constexpr std::uint64_t PackRuntimeKey(
    const CathodeRuntimeKey& key) noexcept {
    return static_cast<std::uint64_t>(key.entityA) |
        (static_cast<std::uint64_t>(key.entityB) << 32U);
}

[[nodiscard]] constexpr std::size_t LogicSwitchBindingStart(
    const std::uint64_t packedKey) noexcept {
    // SplitMix64 finalizer. The table size is a power of two, so the masked
    // result provides a stable bounded start position with no heap state.
    auto value = packedKey;
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return static_cast<std::size_t>(value) & (kLogicSwitchBindingCapacity - 1U);
}

[[maybe_unused]] [[nodiscard]] bool CaptureLogicSwitchBinding(
    WorldEntityRegistry& registry,
    void* nativeSwitch,
    void* contextA,
    void* contextBHolder,
    CathodeRuntimeKey& observedKey) noexcept {
    observedKey = {};
    if (!HasLogicSwitchVtable(nativeSwitch)) return false;

    auto* outerEntity = static_cast<std::byte*>(nativeSwitch) - kLogicSwitchNativeOffset;
    if (!registry.TryExtractRuntimeKey(outerEntity, observedKey) || observedKey.entityA == 0U) {
        observedKey = {};
        return false;
    }

    std::array<std::uint32_t, 4> contextSnapshot{};
    const bool contextAValid = contextA != nullptr && CopyBytesSafely(
        contextA,
        reinterpret_cast<std::byte*>(contextSnapshot.data()),
        sizeof(contextSnapshot));
    void* contextBObject = nullptr;
    if (contextBHolder != nullptr) {
        static_cast<void>(ReadPointerSafely(contextBHolder, contextBObject));
    }

    const auto packedKey = PackRuntimeKey(observedKey);
    const auto start = LogicSwitchBindingStart(packedKey);
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto levelGeneration = registry.DiagnosticsSnapshot().levelGeneration;
    {
        std::lock_guard lock(gLogicSwitchBindingMutex);
        LogicSwitchRuntimeBinding* target = nullptr;
        LogicSwitchRuntimeBinding* oldest = nullptr;
        auto oldestTick = ~std::uint64_t{0U};
        for (std::size_t probe = 0U; probe < kLogicSwitchBindingProbeCount; ++probe) {
            auto& binding = gLogicSwitchBindings[(start + probe) & (kLogicSwitchBindingCapacity - 1U)];
            if (binding.packedKey == 0U || binding.packedKey == packedKey) {
                target = &binding;
                break;
            }
            if (binding.lastSeenTickMs < oldestTick) {
                oldestTick = binding.lastSeenTickMs;
                oldest = &binding;
            }
        }
        if (target == nullptr) {
            target = oldest;
            if (target != nullptr) {
                gLogicSwitchBindingDrops.fetch_add(1U, std::memory_order_acq_rel);
            }
        }
        if (target != nullptr) {
            const bool sameRuntimeObject = target->packedKey == packedKey &&
                target->outerEntity == outerEntity && target->nativeSwitch == nativeSwitch;
            if (!sameRuntimeObject) {
                *target = {};
                target->packedKey = packedKey;
                target->key = observedKey;
                target->outerEntity = outerEntity;
                target->nativeSwitch = nativeSwitch;
            }
            target->lastSeenTickMs = now;
            target->levelGeneration = levelGeneration;
            if (contextAValid) {
                target->contextA = contextSnapshot;
                target->contextAValid = true;
                target->contextBObject = contextBObject;
                target->contextCapturedTickMs = now;
            }
            gLogicSwitchBindingRegistrations.fetch_add(1U, std::memory_order_acq_rel);
            return true;
        }
    }

    gLogicSwitchBindingDrops.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return false;
}

[[maybe_unused]] [[nodiscard]] bool ValidateLogicSwitchBinding(
    WorldEntityRegistry& registry,
    const CathodeRuntimeKey& expectedKey,
    const LogicSwitchRuntimeBinding& binding) noexcept {
    if (binding.packedKey != PackRuntimeKey(expectedKey) ||
        binding.outerEntity == nullptr || binding.nativeSwitch == nullptr ||
        static_cast<std::byte*>(binding.outerEntity) + kLogicSwitchNativeOffset != binding.nativeSwitch ||
        !HasLogicSwitchVtable(binding.nativeSwitch)) {
        return false;
    }

    const auto registryState = registry.DiagnosticsSnapshot();
    if (!registryState.manifestLoaded || registryState.levelGeneration == 0U ||
        binding.levelGeneration != registryState.levelGeneration) {
        return false;
    }

    CathodeRuntimeKey currentKey{};
    if (!registry.TryExtractRuntimeKey(binding.outerEntity, currentKey) || currentKey != expectedKey) {
        return false;
    }
    // The raw runtime object is usable only when the same CATHODE key belongs
    // to a World-domain entity in the currently armed manifest.
    return registry.FindRuntimeWorldEntityId(currentKey).has_value();
}

[[maybe_unused]] [[nodiscard]] bool FindLogicSwitchBinding(
    WorldEntityRegistry& registry,
    const CathodeRuntimeKey& key,
    LogicSwitchRuntimeBinding& output) noexcept {
    output = {};
    if (key.entityA == 0U || !registry.RuntimeCaptureReady()) {
        gLogicSwitchBindingMisses.fetch_add(1U, std::memory_order_acq_rel);
        return false;
    }

    const auto packedKey = PackRuntimeKey(key);
    const auto start = LogicSwitchBindingStart(packedKey);
    LogicSwitchRuntimeBinding candidate{};
    {
        std::lock_guard lock(gLogicSwitchBindingMutex);
        for (std::size_t probe = 0U; probe < kLogicSwitchBindingProbeCount; ++probe) {
            const auto& binding = gLogicSwitchBindings[(start + probe) & (kLogicSwitchBindingCapacity - 1U)];
            if (binding.packedKey == packedKey) {
                candidate = binding;
                break;
            }
            if (binding.packedKey == 0U) break;
        }
    }

    if (candidate.packedKey == 0U || !ValidateLogicSwitchBinding(registry, key, candidate)) {
        gLogicSwitchBindingMisses.fetch_add(1U, std::memory_order_acq_rel);
        return false;
    }

    output = candidate;
    gLogicSwitchBindingHits.fetch_add(1U, std::memory_order_acq_rel);
    return true;
}

void PublishSchedulerCausalRecord(const SchedulerCausalSnapshot& snapshot) noexcept {
    const auto sequence = gSchedulerCausalSequence.fetch_add(1U, std::memory_order_acq_rel);
    auto& slot = gSchedulerCausalHistory[sequence % kSchedulerCausalHistoryCapacity];
    slot.sequence.store(0U, std::memory_order_release);
    slot.tickMs.store(snapshot.tickMs, std::memory_order_relaxed);
    slot.sourceHigh.store(snapshot.source.high, std::memory_order_relaxed);
    slot.sourceLow.store(snapshot.source.low, std::memory_order_relaxed);
    slot.targetHigh.store(snapshot.target.high, std::memory_order_relaxed);
    slot.targetLow.store(snapshot.target.low, std::memory_order_relaxed);
    slot.schedulerParameterShortGuid.store(snapshot.schedulerParameterShortGuid, std::memory_order_relaxed);
    slot.targetInputShortGuid.store(snapshot.targetInputShortGuid, std::memory_order_relaxed);
    slot.callbackHigh.store(snapshot.callbackEntity.high, std::memory_order_relaxed);
    slot.callbackLow.store(snapshot.callbackEntity.low, std::memory_order_relaxed);
    slot.callbackExecutionVtableRva.store(snapshot.callbackExecutionVtableRva, std::memory_order_relaxed);
    slot.sourceRuntimeKeyA.store(snapshot.sourceRuntimeKey.entityA, std::memory_order_relaxed);
    slot.sourceRuntimeKeyB.store(snapshot.sourceRuntimeKey.entityB, std::memory_order_relaxed);
    slot.targetRuntimeKeyA.store(snapshot.targetRuntimeKey.entityA, std::memory_order_relaxed);
    slot.targetRuntimeKeyB.store(snapshot.targetRuntimeKey.entityB, std::memory_order_relaxed);
    slot.callbackRuntimeKeyA.store(snapshot.callbackRuntimeKey.entityA, std::memory_order_relaxed);
    slot.callbackRuntimeKeyB.store(snapshot.callbackRuntimeKey.entityB, std::memory_order_relaxed);
    slot.queueCallerRva.store(snapshot.queueCallerRva, std::memory_order_relaxed);
    for (std::size_t i = 0U; i < kCallbackRawWordCount; ++i) {
        slot.callbackRawWords[i].store(snapshot.callbackRawWords[i], std::memory_order_relaxed);
    }
    std::uint32_t flags = snapshot.callbackRecordPresent ? kSchedulerCausalCallbackFlag : 0U;
    if (snapshot.callbackRawReadable) flags |= kSchedulerCausalCallbackRawReadableFlag;
    if (snapshot.callbackExecutionReadable) flags |= kSchedulerCausalCallbackExecutionReadableFlag;
    if (snapshot.callbackExecutionPresent) flags |= kSchedulerCausalCallbackExecutionPresentFlag;
    if (snapshot.sourceRuntimeKeyValid) flags |= kSchedulerCausalSourceRuntimeKeyFlag;
    if (snapshot.targetRuntimeKeyValid) flags |= kSchedulerCausalTargetRuntimeKeyFlag;
    if (snapshot.callbackRuntimeKeyValid) flags |= kSchedulerCausalCallbackRuntimeKeyFlag;
    if (snapshot.callbackWorldEntity) flags |= kSchedulerCausalCallbackWorldEntityFlag;
    if (snapshot.callbackSameAsSource) flags |= kSchedulerCausalCallbackSameSourceFlag;
    if (snapshot.callbackSameAsTarget) flags |= kSchedulerCausalCallbackSameTargetFlag;
    slot.flags.store(flags, std::memory_order_relaxed);
    slot.sequence.store(sequence, std::memory_order_release);
    gSchedulerCausalEdgesRecorded.fetch_add(1U, std::memory_order_acq_rel);
}

[[nodiscard]] bool ReadSchedulerCausalRecord(
    const SchedulerCausalRecord& slot,
    SchedulerCausalSnapshot& output) noexcept {
    const auto before = slot.sequence.load(std::memory_order_acquire);
    if (before == 0U) return false;
    SchedulerCausalSnapshot snapshot{};
    snapshot.sequence = before;
    snapshot.tickMs = slot.tickMs.load(std::memory_order_relaxed);
    snapshot.source.high = slot.sourceHigh.load(std::memory_order_relaxed);
    snapshot.source.low = slot.sourceLow.load(std::memory_order_relaxed);
    snapshot.target.high = slot.targetHigh.load(std::memory_order_relaxed);
    snapshot.target.low = slot.targetLow.load(std::memory_order_relaxed);
    snapshot.schedulerParameterShortGuid = slot.schedulerParameterShortGuid.load(std::memory_order_relaxed);
    snapshot.targetInputShortGuid = slot.targetInputShortGuid.load(std::memory_order_relaxed);
    snapshot.callbackEntity.high = slot.callbackHigh.load(std::memory_order_relaxed);
    snapshot.callbackEntity.low = slot.callbackLow.load(std::memory_order_relaxed);
    snapshot.callbackExecutionVtableRva = slot.callbackExecutionVtableRva.load(std::memory_order_relaxed);
    snapshot.sourceRuntimeKey.entityA = slot.sourceRuntimeKeyA.load(std::memory_order_relaxed);
    snapshot.sourceRuntimeKey.entityB = slot.sourceRuntimeKeyB.load(std::memory_order_relaxed);
    snapshot.targetRuntimeKey.entityA = slot.targetRuntimeKeyA.load(std::memory_order_relaxed);
    snapshot.targetRuntimeKey.entityB = slot.targetRuntimeKeyB.load(std::memory_order_relaxed);
    snapshot.callbackRuntimeKey.entityA = slot.callbackRuntimeKeyA.load(std::memory_order_relaxed);
    snapshot.callbackRuntimeKey.entityB = slot.callbackRuntimeKeyB.load(std::memory_order_relaxed);
    snapshot.queueCallerRva = slot.queueCallerRva.load(std::memory_order_relaxed);
    for (std::size_t i = 0U; i < kCallbackRawWordCount; ++i) {
        snapshot.callbackRawWords[i] = slot.callbackRawWords[i].load(std::memory_order_relaxed);
    }
    const auto flags = slot.flags.load(std::memory_order_relaxed);
    snapshot.callbackRecordPresent = (flags & kSchedulerCausalCallbackFlag) != 0U;
    snapshot.callbackRawReadable = (flags & kSchedulerCausalCallbackRawReadableFlag) != 0U;
    snapshot.callbackExecutionReadable = (flags & kSchedulerCausalCallbackExecutionReadableFlag) != 0U;
    snapshot.callbackExecutionPresent = (flags & kSchedulerCausalCallbackExecutionPresentFlag) != 0U;
    snapshot.sourceRuntimeKeyValid = (flags & kSchedulerCausalSourceRuntimeKeyFlag) != 0U;
    snapshot.targetRuntimeKeyValid = (flags & kSchedulerCausalTargetRuntimeKeyFlag) != 0U;
    snapshot.callbackRuntimeKeyValid = (flags & kSchedulerCausalCallbackRuntimeKeyFlag) != 0U;
    snapshot.callbackWorldEntity = (flags & kSchedulerCausalCallbackWorldEntityFlag) != 0U;
    snapshot.callbackSameAsSource = (flags & kSchedulerCausalCallbackSameSourceFlag) != 0U;
    snapshot.callbackSameAsTarget = (flags & kSchedulerCausalCallbackSameTargetFlag) != 0U;
    const auto after = slot.sequence.load(std::memory_order_acquire);
    if (before != after || after == 0U) return false;
    output = snapshot;
    return true;
}

class SchedulerCausalScope final {
public:
    explicit SchedulerCausalScope(
        const SchedulerCausalSnapshot& snapshot,
        void* callbackRecord = nullptr) noexcept
        : previous_(gActiveSchedulerCausalContext) {
        gActiveSchedulerCausalContext.active = true;
        gActiveSchedulerCausalContext.snapshot = snapshot;
        gActiveSchedulerCausalContext.callbackRecord = callbackRecord;
    }

    ~SchedulerCausalScope() {
        gActiveSchedulerCausalContext = previous_;
    }

private:
    ActiveSchedulerCausalContext previous_{};
};

[[nodiscard]] bool CausalSnapshotTouchesEntity(
    const SchedulerCausalSnapshot& snapshot,
    const WorldEntityId& entity) noexcept {
    return snapshot.source == entity || snapshot.target == entity;
}

void StoreLastLogicSwitchCausalMatch(
    const SchedulerCausalSnapshot& snapshot,
    const std::uint64_t ageMs,
    const CathodeRuntimeKey& stateRuntimeKey) noexcept {
    gLogicSwitchCausalLastAgeMs.store(ageMs, std::memory_order_release);
    gLogicSwitchCausalLastSourceHigh.store(snapshot.source.high, std::memory_order_release);
    gLogicSwitchCausalLastSourceLow.store(snapshot.source.low, std::memory_order_release);
    gLogicSwitchCausalLastTargetHigh.store(snapshot.target.high, std::memory_order_release);
    gLogicSwitchCausalLastTargetLow.store(snapshot.target.low, std::memory_order_release);
    gLogicSwitchCausalLastSchedulerParameter.store(snapshot.schedulerParameterShortGuid, std::memory_order_release);
    gLogicSwitchCausalLastTargetInput.store(snapshot.targetInputShortGuid, std::memory_order_release);
    gLogicSwitchCausalLastCallbackHigh.store(snapshot.callbackEntity.high, std::memory_order_release);
    gLogicSwitchCausalLastCallbackLow.store(snapshot.callbackEntity.low, std::memory_order_release);
    gLogicSwitchCausalLastCallbackExecutionVtableRva.store(
        snapshot.callbackExecutionVtableRva, std::memory_order_release);
    gLogicSwitchCausalLastSourceRuntimeKeyA.store(snapshot.sourceRuntimeKey.entityA, std::memory_order_release);
    gLogicSwitchCausalLastSourceRuntimeKeyB.store(snapshot.sourceRuntimeKey.entityB, std::memory_order_release);
    gLogicSwitchCausalLastTargetRuntimeKeyA.store(snapshot.targetRuntimeKey.entityA, std::memory_order_release);
    gLogicSwitchCausalLastTargetRuntimeKeyB.store(snapshot.targetRuntimeKey.entityB, std::memory_order_release);
    gLogicSwitchCausalLastCallbackRuntimeKeyA.store(snapshot.callbackRuntimeKey.entityA, std::memory_order_release);
    gLogicSwitchCausalLastCallbackRuntimeKeyB.store(snapshot.callbackRuntimeKey.entityB, std::memory_order_release);
    gLogicSwitchCausalLastStateRuntimeKeyA.store(stateRuntimeKey.entityA, std::memory_order_release);
    gLogicSwitchCausalLastStateRuntimeKeyB.store(stateRuntimeKey.entityB, std::memory_order_release);
    gLogicSwitchCausalLastQueueCallerRva.store(snapshot.queueCallerRva, std::memory_order_release);
    for (std::size_t i = 0U; i < kCallbackRawWordCount; ++i) {
        gLogicSwitchCausalLastCallbackRawWords[i].store(snapshot.callbackRawWords[i], std::memory_order_release);
    }
    std::uint32_t flags = snapshot.callbackRecordPresent ? kSchedulerCausalCallbackFlag : 0U;
    if (snapshot.callbackRawReadable) flags |= kSchedulerCausalCallbackRawReadableFlag;
    if (snapshot.callbackExecutionReadable) flags |= kSchedulerCausalCallbackExecutionReadableFlag;
    if (snapshot.callbackExecutionPresent) flags |= kSchedulerCausalCallbackExecutionPresentFlag;
    if (snapshot.sourceRuntimeKeyValid) flags |= kSchedulerCausalSourceRuntimeKeyFlag;
    if (snapshot.targetRuntimeKeyValid) flags |= kSchedulerCausalTargetRuntimeKeyFlag;
    if (snapshot.callbackRuntimeKeyValid) flags |= kSchedulerCausalCallbackRuntimeKeyFlag;
    if (snapshot.callbackWorldEntity) flags |= kSchedulerCausalCallbackWorldEntityFlag;
    if (snapshot.callbackSameAsSource) flags |= kSchedulerCausalCallbackSameSourceFlag;
    if (snapshot.callbackSameAsTarget) flags |= kSchedulerCausalCallbackSameTargetFlag;
    gLogicSwitchCausalLastFlags.store(flags, std::memory_order_release);
}

void ObserveLogicSwitchCausalEvidence(
    const WorldEntityId& entity,
    const bool enabled,
    const CathodeRuntimeKey& stateRuntimeKey) noexcept {
    gLogicSwitchCausalLastStateHigh.store(entity.high, std::memory_order_release);
    gLogicSwitchCausalLastStateLow.store(entity.low, std::memory_order_release);
    gLogicSwitchCausalLastEnabled.store(enabled ? 1U : 0U, std::memory_order_release);
    if (gActiveSchedulerCausalContext.active) {
        auto snapshot = gActiveSchedulerCausalContext.snapshot;
        gLogicSwitchCausalNestedMatches.fetch_add(1U, std::memory_order_relaxed);
        if (snapshot.callbackRecordPresent) {
            gLogicSwitchCausalCallbackMatches.fetch_add(1U, std::memory_order_relaxed);
            CaptureCallbackAbiProbeSample(
                snapshot,
                gActiveSchedulerCausalContext.callbackRecord,
                entity);
        }
        gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchNested, std::memory_order_release);
        StoreLastLogicSwitchCausalMatch(snapshot, 0U, stateRuntimeKey);
        return;
    }

    const auto probeUntil = gCallbackClassificationUntilTickMs.load(std::memory_order_relaxed);
    if (probeUntil == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now > probeUntil) return;
    std::optional<SchedulerCausalSnapshot> bestEndpoint;
    std::optional<SchedulerCausalSnapshot> bestTemporalSnapshot;
    std::optional<SchedulerCausalSnapshot> bestCallbackRecent;
    std::optional<SchedulerCausalSnapshot> bestRecent;
    std::uint64_t bestEndpointAge = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t bestTemporalSnapshotAge = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t bestCallbackRecentAge = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t bestRecentAge = std::numeric_limits<std::uint64_t>::max();
    for (const auto& slot : gSchedulerCausalHistory) {
        SchedulerCausalSnapshot snapshot{};
        if (!ReadSchedulerCausalRecord(slot, snapshot) || snapshot.tickMs == 0U || now < snapshot.tickMs) continue;
        const auto age = now - snapshot.tickMs;
        if (age > kSchedulerCausalCorrelationWindowMs) continue;
        if (age < bestRecentAge) {
            bestRecentAge = age;
            bestRecent = snapshot;
        }
        if (snapshot.callbackRawReadable &&
            snapshot.queueCallerRva == kCallbackAbiProbeCallerRva &&
            age < bestTemporalSnapshotAge) {
            bestTemporalSnapshotAge = age;
            bestTemporalSnapshot = snapshot;
        }
        if (snapshot.callbackRecordPresent &&
            snapshot.queueCallerRva == kCallbackAbiProbeCallerRva &&
            age < bestCallbackRecentAge) {
            bestCallbackRecentAge = age;
            bestCallbackRecent = snapshot;
        }
        if (CausalSnapshotTouchesEntity(snapshot, entity) && age < bestEndpointAge) {
            bestEndpointAge = age;
            bestEndpoint = snapshot;
        }
    }

    if (bestEndpoint.has_value() && bestEndpoint->callbackRawReadable &&
        bestEndpoint->queueCallerRva == kCallbackAbiProbeCallerRva) {
        gLogicSwitchCausalEndpointMatches.fetch_add(1U, std::memory_order_acq_rel);
        gLogicSwitchCausalCallbackMatches.fetch_add(1U, std::memory_order_acq_rel);
        CommitCallbackAbiProbeSnapshot(*bestEndpoint, entity);
        gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchEndpointRecent, std::memory_order_release);
        StoreLastLogicSwitchCausalMatch(*bestEndpoint, bestEndpointAge, stateRuntimeKey);
        return;
    }
    if (bestTemporalSnapshot.has_value()) {
        gLogicSwitchCausalRecentMatches.fetch_add(1U, std::memory_order_acq_rel);
        gLogicSwitchCausalCallbackMatches.fetch_add(1U, std::memory_order_acq_rel);
        CommitCallbackAbiProbeSnapshot(*bestTemporalSnapshot, entity);
        gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchTemporalSnapshot, std::memory_order_release);
        StoreLastLogicSwitchCausalMatch(*bestTemporalSnapshot, bestTemporalSnapshotAge, stateRuntimeKey);
        return;
    }
    if (bestEndpoint.has_value()) {
        gLogicSwitchCausalEndpointMatches.fetch_add(1U, std::memory_order_acq_rel);
        if (bestEndpoint->callbackRecordPresent) {
            gLogicSwitchCausalCallbackMatches.fetch_add(1U, std::memory_order_acq_rel);
        }
        gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchEndpointRecent, std::memory_order_release);
        StoreLastLogicSwitchCausalMatch(*bestEndpoint, bestEndpointAge, stateRuntimeKey);
        return;
    }
    if (bestCallbackRecent.has_value()) {
        gLogicSwitchCausalRecentMatches.fetch_add(1U, std::memory_order_acq_rel);
        gLogicSwitchCausalCallbackMatches.fetch_add(1U, std::memory_order_acq_rel);
        gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchRecentOnly, std::memory_order_release);
        StoreLastLogicSwitchCausalMatch(*bestCallbackRecent, bestCallbackRecentAge, stateRuntimeKey);
        return;
    }
    if (bestRecent.has_value()) {
        gLogicSwitchCausalRecentMatches.fetch_add(1U, std::memory_order_acq_rel);
        if (bestRecent->callbackRecordPresent) {
            gLogicSwitchCausalCallbackMatches.fetch_add(1U, std::memory_order_acq_rel);
        }
        gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchRecentOnly, std::memory_order_release);
        StoreLastLogicSwitchCausalMatch(*bestRecent, bestRecentAge, stateRuntimeKey);
        return;
    }
    gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchNone, std::memory_order_release);
    gLogicSwitchCausalNoMatches.fetch_add(1U, std::memory_order_acq_rel);
}

[[maybe_unused]] [[nodiscard]] bool LogicSwitchReplayContextUsable(
    const LogicSwitchRuntimeBinding& binding) noexcept {
    if (!binding.contextAValid || binding.contextCapturedTickMs == 0U) return false;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now < binding.contextCapturedTickMs ||
        now - binding.contextCapturedTickMs > kLogicSwitchReplayContextMaximumAgeMs) {
        return false;
    }
    // ContextA is a copied retail call record. Treat pointer-looking words as
    // unsafe unless the target remains readable; small scalar values are
    // allowed. This is intentionally fail-closed because a stale CATHODE
    // context can corrupt engine state even when SEH catches the eventual
    // access violation.
    for (const auto word : binding.contextA) {
        if (word > 0x10000U) {
            void* ignored = nullptr;
            if (!ReadPointerSafely(
                    reinterpret_cast<const void*>(static_cast<std::uintptr_t>(word)),
                    ignored)) {
                return false;
            }
        }
    }
    if (binding.contextBObject != nullptr) {
        void* ignored = nullptr;
        if (!ReadPointerSafely(binding.contextBObject, ignored)) return false;
    }
    return true;
}

[[maybe_unused]] bool InvokeLogicSwitchSafely(
    const LogicSwitchInputFn function,
    void* nativeSwitch,
    const LogicSwitchRuntimeBinding& binding) noexcept {
    if (function == nullptr || nativeSwitch == nullptr ||
        !LogicSwitchReplayContextUsable(binding)) {
        return false;
    }
    auto contextA = binding.contextA;
    auto* contextBObject = binding.contextBObject;
    gLogicSwitchContextReplays.fetch_add(1U, std::memory_order_acq_rel);
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        static_cast<void>(function(nativeSwitch, contextA.data(), &contextBObject));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    static_cast<void>(function(nativeSwitch, contextA.data(), &contextBObject));
    return true;
#endif
}

[[nodiscard]] bool ExtractLogicSwitchContextRuntimeKey(
    WorldEntityRegistry& registry,
    void* contextA,
    CathodeRuntimeKey& key) noexcept {
    key = {};
    if (contextA == nullptr) return false;
    void* sharedRef = nullptr;
    if (!ReadPointerSafely(contextA, sharedRef) || sharedRef == nullptr) return false;
    auto* execution = ExecutionObjectFromShared(sharedRef);
    return execution != nullptr && registry.TryExtractRuntimeKey(execution, key) &&
        key.entityA != 0U;
}

[[nodiscard]] ExactLogicSwitchCallbackSnapshot CaptureExactLogicSwitchCallback(
    WorldEntityRegistry& registry,
    void* contextB) noexcept {
    ExactLogicSwitchCallbackSnapshot snapshot{};
    if (contextB == nullptr) return snapshot;

    void* callbackRecord = nullptr;
    if (!ReadPointerSafely(contextB, callbackRecord)) return snapshot;
    snapshot.holderReadable = true;
    if (callbackRecord == nullptr) return snapshot;
    snapshot.callbackPresent = true;

    if (!CopyBytesSafely(
            callbackRecord,
            reinterpret_cast<std::byte*>(snapshot.words.data()),
            sizeof(snapshot.words))) {
        return snapshot;
    }
    snapshot.rawReadable = true;

    const auto callbackSharedAddress =
        static_cast<std::uintptr_t>(snapshot.words[2]);
    if (callbackSharedAddress == 0U) return snapshot;

    auto* callbackExecution = ExecutionObjectFromShared(
        reinterpret_cast<void*>(callbackSharedAddress));
    if (callbackExecution == nullptr) return snapshot;
    snapshot.callbackExecutionPresent = true;

    if (registry.TryExtractRuntimeKey(
            callbackExecution, snapshot.callbackRuntimeKey) &&
        snapshot.callbackRuntimeKey.entityA != 0U) {
        snapshot.callbackRuntimeKeyValid = true;
    }

    void* callbackVtable = nullptr;
    if (ReadPointerSafely(callbackExecution, callbackVtable) &&
        callbackVtable != nullptr) {
        const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
        const auto vtableAddress =
            reinterpret_cast<std::uintptr_t>(callbackVtable);
        if (moduleBase != 0U && vtableAddress >= moduleBase) {
            const auto relative = vtableAddress - moduleBase;
            if (relative <= std::numeric_limits<std::uint32_t>::max()) {
                snapshot.callbackExecutionVtableRva =
                    static_cast<std::uint32_t>(relative);
                snapshot.callbackExecutionReadable = true;
            }
        }
    }
    return snapshot;
}

[[nodiscard]] bool IsFix311ExactStateKey(
    const CathodeRuntimeKey& key) noexcept {
    return key.entityA == kFix311ExactStateKeyA &&
        key.entityB == kFix311ExactStateKeyB;
}

void Fix319HashU64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        hash ^= static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        hash *= kFix319HashPrime;
    }
}

[[nodiscard]] std::uint64_t Fix319SemanticHash(
    const Fix319TraceSemantic& semantic) noexcept {
    auto hash = kFix319HashOffset;
    Fix319HashU64(hash, static_cast<std::uint32_t>(semantic.channel));
    Fix319HashU64(hash, semantic.worldMask);
    Fix319HashU64(hash, semantic.sourceRuntimeKey.entityA);
    Fix319HashU64(hash, semantic.sourceRuntimeKey.entityB);
    Fix319HashU64(hash, semantic.targetRuntimeKey.entityA);
    Fix319HashU64(hash, semantic.targetRuntimeKey.entityB);
    Fix319HashU64(hash, semantic.sourceWorldEntity.high);
    Fix319HashU64(hash, semantic.sourceWorldEntity.low);
    Fix319HashU64(hash, semantic.targetWorldEntity.high);
    Fix319HashU64(hash, semantic.targetWorldEntity.low);
    Fix319HashU64(hash, semantic.primary);
    Fix319HashU64(hash, semantic.secondary);
    Fix319HashU64(hash, semantic.payloadBytes);
    Fix319HashU64(hash, semantic.payloadHash);
    Fix319HashU64(hash, semantic.delayBits);
    Fix319HashU64(hash, semantic.tailArgument);
    Fix319HashU64(hash, semantic.callbackInput);
    Fix319HashU64(hash, semantic.callbackFlags);
    Fix319HashU64(hash, semantic.callbackRuntimeKey.entityA);
    Fix319HashU64(hash, semantic.callbackRuntimeKey.entityB);
    Fix319HashU64(hash, semantic.callbackExecutionVtableRva);
    Fix319HashU64(hash, semantic.semanticFlags);
    return hash == 0U ? 1U : hash;
}

[[nodiscard]] bool Fix319SemanticEqual(
    const Fix319TraceSemantic& lhs,
    const Fix319TraceSemantic& rhs) noexcept {
    return lhs.channel == rhs.channel &&
        lhs.worldMask == rhs.worldMask &&
        lhs.sourceRuntimeKey.entityA == rhs.sourceRuntimeKey.entityA &&
        lhs.sourceRuntimeKey.entityB == rhs.sourceRuntimeKey.entityB &&
        lhs.targetRuntimeKey.entityA == rhs.targetRuntimeKey.entityA &&
        lhs.targetRuntimeKey.entityB == rhs.targetRuntimeKey.entityB &&
        lhs.sourceWorldEntity.high == rhs.sourceWorldEntity.high &&
        lhs.sourceWorldEntity.low == rhs.sourceWorldEntity.low &&
        lhs.targetWorldEntity.high == rhs.targetWorldEntity.high &&
        lhs.targetWorldEntity.low == rhs.targetWorldEntity.low &&
        lhs.primary == rhs.primary &&
        lhs.secondary == rhs.secondary &&
        lhs.payloadBytes == rhs.payloadBytes &&
        lhs.payloadHash == rhs.payloadHash &&
        lhs.delayBits == rhs.delayBits &&
        lhs.tailArgument == rhs.tailArgument &&
        lhs.callbackInput == rhs.callbackInput &&
        lhs.callbackFlags == rhs.callbackFlags &&
        lhs.callbackRuntimeKey.entityA == rhs.callbackRuntimeKey.entityA &&
        lhs.callbackRuntimeKey.entityB == rhs.callbackRuntimeKey.entityB &&
        lhs.callbackExecutionVtableRva == rhs.callbackExecutionVtableRva &&
        lhs.semanticFlags == rhs.semanticFlags;
}

[[nodiscard]] bool Fix319ArmActive(const std::uint64_t now) noexcept {
    const auto until = gFix319ArmUntilTickMs.load(std::memory_order_relaxed);
    return until != 0U && now <= until;
}

[[nodiscard]] bool Fix319TraceActive(const std::uint64_t now) noexcept {
    const auto origin = gFix319OriginTickMs.load(std::memory_order_acquire);
    const auto until = gFix319WindowUntilTickMs.load(std::memory_order_acquire);
    return origin != 0U && until != 0U && now <= until;
}

void Fix321HashU64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        hash ^= static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        hash *= kFix321HashPrime;
    }
}

[[nodiscard]] std::uint64_t Fix321SemanticHash(
    const Fix321TraceSemantic& semantic) noexcept {
    auto hash = kFix321HashOffset;
    Fix321HashU64(hash, semantic.helperRva);
    Fix321HashU64(hash, semantic.callerRva);
    Fix321HashU64(hash, semantic.runtimeKey.entityA);
    Fix321HashU64(hash, semantic.runtimeKey.entityB);
    Fix321HashU64(hash, semantic.worldEntity.high);
    Fix321HashU64(hash, semantic.worldEntity.low);
    Fix321HashU64(hash, semantic.executionVtableRva);
    Fix321HashU64(hash, semantic.identityMask);
    Fix321HashU64(hash, semantic.valueReadMask);
    Fix321HashU64(hash, semantic.valueWord0);
    Fix321HashU64(hash, semantic.contextReadMask);
    Fix321HashU64(hash, semantic.contextWord0);
    Fix321HashU64(hash, semantic.contextWord1);
    Fix321HashU64(hash, semantic.objectReadMask);
    Fix321HashU64(hash, semantic.objectWord0);
    Fix321HashU64(hash, semantic.objectWord1);
    return hash == 0U ? 1U : hash;
}

[[nodiscard]] bool Fix321SemanticEqual(
    const Fix321TraceSemantic& lhs,
    const Fix321TraceSemantic& rhs) noexcept {
    return lhs.helperRva == rhs.helperRva &&
        lhs.callerRva == rhs.callerRva &&
        lhs.runtimeKey.entityA == rhs.runtimeKey.entityA &&
        lhs.runtimeKey.entityB == rhs.runtimeKey.entityB &&
        lhs.worldEntity.high == rhs.worldEntity.high &&
        lhs.worldEntity.low == rhs.worldEntity.low &&
        lhs.executionVtableRva == rhs.executionVtableRva &&
        lhs.identityMask == rhs.identityMask &&
        lhs.valueReadMask == rhs.valueReadMask &&
        lhs.valueWord0 == rhs.valueWord0 &&
        lhs.contextReadMask == rhs.contextReadMask &&
        lhs.contextWord0 == rhs.contextWord0 &&
        lhs.contextWord1 == rhs.contextWord1 &&
        lhs.objectReadMask == rhs.objectReadMask &&
        lhs.objectWord0 == rhs.objectWord0 &&
        lhs.objectWord1 == rhs.objectWord1;
}

[[nodiscard]] bool Fix321TraceActive(const std::uint64_t now) noexcept {
    const auto origin = gFix321OriginTickMs.load(std::memory_order_acquire);
    const auto until = gFix321WindowUntilTickMs.load(std::memory_order_acquire);
    return origin != 0U && until != 0U && now <= until;
}

void PublishFix321Trace(
    const Fix321TraceSemantic& semantic,
    const std::uint32_t executionMask,
    const std::uint64_t now) noexcept {
    const auto origin = gFix321OriginTickMs.load(std::memory_order_acquire);
    if (origin == 0U || now < origin || !Fix321TraceActive(now)) return;

    gFix321Accepted.fetch_add(1U, std::memory_order_relaxed);
    const auto delta64 = now - origin;
    const auto delta = static_cast<std::uint32_t>(delta64 >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
        ? std::numeric_limits<std::uint32_t>::max()
        : delta64);
    const auto signature = Fix321SemanticHash(semantic);
    const auto start = static_cast<std::size_t>(signature) &
        (kFix321TraceSlotCapacity - 1U);

    for (std::size_t probe = 0U; probe < kFix321TraceSlotProbeCount; ++probe) {
        auto& slot = gFix321TraceSlots[
            (start + probe) & (kFix321TraceSlotCapacity - 1U)];
        auto observed = slot.signatureHash.load(std::memory_order_acquire);
        if (observed == 0U) {
            if (slot.signatureHash.compare_exchange_strong(
                    observed, signature,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                slot.semantic = semantic;
                slot.count.store(1U, std::memory_order_relaxed);
                slot.executionMask.store(executionMask, std::memory_order_relaxed);
                slot.firstDeltaMs.store(delta, std::memory_order_relaxed);
                slot.lastDeltaMs.store(delta, std::memory_order_relaxed);
                slot.ready.store(1U, std::memory_order_release);
                gFix321UniqueEntries.fetch_add(1U, std::memory_order_relaxed);
                TouchVersion();
                return;
            }
        }
        if (observed != signature || slot.ready.load(std::memory_order_acquire) == 0U) {
            continue;
        }
        if (!Fix321SemanticEqual(slot.semantic, semantic)) continue;
        slot.count.fetch_add(1U, std::memory_order_relaxed);
        slot.executionMask.fetch_or(executionMask, std::memory_order_relaxed);
        slot.lastDeltaMs.store(delta, std::memory_order_relaxed);
        return;
    }

    gFix321CapacityDrops.fetch_add(1U, std::memory_order_relaxed);
}

[[nodiscard]] std::uint32_t Fix321CallSiteRva(
    const std::uintptr_t returnAddress) noexcept {
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase == 0U || returnAddress < moduleBase + 5U) return 0U;
    const auto callAddress = returnAddress - 5U;
    const auto relative = callAddress - moduleBase;
    if (relative > std::numeric_limits<std::uint32_t>::max()) return 0U;
    return static_cast<std::uint32_t>(relative);
}

void CaptureFix321CompositeStateValue(
    const std::uint32_t helperRva,
    void* execution,
    int* context,
    std::uint8_t* value,
    const std::uintptr_t returnAddress) noexcept {
    if (gFix321WindowUntilTickMs.load(std::memory_order_relaxed) == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (!gInitialized.load(std::memory_order_acquire) || !Fix321TraceActive(now)) return;
    const auto inspected = gFix321Inspected.fetch_add(1U, std::memory_order_relaxed);
    if (inspected >= kFix321ObservationLimit) {
        gFix321LimitDrops.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    Fix321TraceSemantic semantic{};
    semantic.helperRva = helperRva;
    semantic.callerRva = Fix321CallSiteRva(returnAddress);

    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry != nullptr && registry->RuntimeCaptureReady() && execution != nullptr) {
        if (registry->TryExtractRuntimeKey(execution, semantic.runtimeKey) &&
            semantic.runtimeKey.entityA != 0U) {
            semantic.identityMask |= kFix321IdentityRuntimeKey;
            const auto world = registry->FindRuntimeWorldEntityId(semantic.runtimeKey);
            if (world.has_value()) {
                semantic.worldEntity = *world;
                semantic.identityMask |= kFix321IdentityWorldEntity;
            }
        }
    }

    void* vtable = nullptr;
    if (execution != nullptr && ReadPointerSafely(execution, vtable) && vtable != nullptr &&
        ModuleRelativeRva(vtable, semantic.executionVtableRva)) {
        semantic.identityMask |= kFix321IdentityVtable;
    }

    if (ReadU32Safely(value, semantic.valueWord0)) {
        semantic.valueReadMask |= kFix321ReadWord0;
    }
    if (ReadU32Safely(context, semantic.contextWord0)) {
        semantic.contextReadMask |= kFix321ReadWord0;
    }
    if (ReadU32Safely(
            context == nullptr ? nullptr : reinterpret_cast<const std::byte*>(context) + 4U,
            semantic.contextWord1)) {
        semantic.contextReadMask |= kFix321ReadWord1;
    }
    if (ReadU32Safely(
            execution == nullptr ? nullptr : static_cast<const std::byte*>(execution) + 0x0CU,
            semantic.objectWord0)) {
        semantic.objectReadMask |= kFix321ReadWord0;
    }
    if (ReadU32Safely(
            execution == nullptr ? nullptr : static_cast<const std::byte*>(execution) + 0x10U,
            semantic.objectWord1)) {
        semantic.objectReadMask |= kFix321ReadWord1;
    }

    if (semantic.callerRva == 0U ||
        (semantic.identityMask == 0U && semantic.valueReadMask == 0U &&
         semantic.contextReadMask == 0U && semantic.objectReadMask == 0U)) {
        gFix321ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    PublishFix321Trace(
        semantic,
        replication_core::RemoteApplyActive()
            ? kFix319ExecutionRemoteApply
            : kFix319ExecutionLocal,
        now);
}

void InvokeFix321CompositeStateValue(
    const std::uint32_t helperRva,
    void* execution,
    int* context,
    std::uint8_t* value) noexcept {
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase == 0U) return;
    const auto original = reinterpret_cast<CompositeStateValueFn>(
        moduleBase + helperRva);
    original(execution, context, value);
}

void __fastcall Fix321Helper04060Redirect(
    void* execution, void*, int* context, std::uint8_t* value) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t returnAddress = 0U;
#endif
    CaptureFix321CompositeStateValue(
        kFix321Helper04060Rva, execution, context, value, returnAddress);
    InvokeFix321CompositeStateValue(kFix321Helper04060Rva, execution, context, value);
}

void __fastcall Fix321Helper04200Redirect(
    void* execution, void*, int* context, std::uint8_t* value) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t returnAddress = 0U;
#endif
    CaptureFix321CompositeStateValue(
        kFix321Helper04200Rva, execution, context, value, returnAddress);
    InvokeFix321CompositeStateValue(kFix321Helper04200Rva, execution, context, value);
}

void __fastcall Fix321Helper042D0Redirect(
    void* execution, void*, int* context, std::uint8_t* value) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t returnAddress = 0U;
#endif
    CaptureFix321CompositeStateValue(
        kFix321Helper042D0Rva, execution, context, value, returnAddress);
    InvokeFix321CompositeStateValue(kFix321Helper042D0Rva, execution, context, value);
}

void __fastcall Fix321Helper043A0Redirect(
    void* execution, void*, int* context, std::uint8_t* value) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t returnAddress = 0U;
#endif
    CaptureFix321CompositeStateValue(
        kFix321Helper043A0Rva, execution, context, value, returnAddress);
    InvokeFix321CompositeStateValue(kFix321Helper043A0Rva, execution, context, value);
}

void __fastcall Fix321Helper04470Redirect(
    void* execution, void*, int* context, std::uint8_t* value) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t returnAddress = 0U;
#endif
    CaptureFix321CompositeStateValue(
        kFix321Helper04470Rva, execution, context, value, returnAddress);
    InvokeFix321CompositeStateValue(kFix321Helper04470Rva, execution, context, value);
}

[[nodiscard]] bool InstallFix321CompositeStateCallRedirects(
    HookManager& hooks,
    std::string& error) {
    error.clear();
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase == 0U) {
        error = "Fix3.21 main module base is unavailable";
        return false;
    }

    const std::array<Fix321CallFamilySpec, 13> families{{
        {kFix321Caller3AE0BeginRva, kFix321Caller3AE0EndRva,
            kFix321Helper04060Rva, 1U, reinterpret_cast<void*>(&Fix321Helper04060Redirect)},
        {kFix321Caller4200BeginRva, kFix321Caller4200EndRva,
            kFix321Helper04200Rva, 1U, reinterpret_cast<void*>(&Fix321Helper04200Redirect)},
        {kFix321Caller4200BeginRva, kFix321Caller4200EndRva,
            kFix321Helper042D0Rva, 1U, reinterpret_cast<void*>(&Fix321Helper042D0Redirect)},
        {kFix321Caller4200BeginRva, kFix321Caller4200EndRva,
            kFix321Helper043A0Rva, 1U, reinterpret_cast<void*>(&Fix321Helper043A0Redirect)},
        {kFix321Caller4200BeginRva, kFix321Caller4200EndRva,
            kFix321Helper04470Rva, 1U, reinterpret_cast<void*>(&Fix321Helper04470Redirect)},
        {kFix321Caller4310BeginRva, kFix321Caller4310EndRva,
            kFix321Helper04200Rva, 1U, reinterpret_cast<void*>(&Fix321Helper04200Redirect)},
        {kFix321Caller4310BeginRva, kFix321Caller4310EndRva,
            kFix321Helper042D0Rva, 1U, reinterpret_cast<void*>(&Fix321Helper042D0Redirect)},
        {kFix321Caller4310BeginRva, kFix321Caller4310EndRva,
            kFix321Helper043A0Rva, 2U, reinterpret_cast<void*>(&Fix321Helper043A0Redirect)},
        {kFix321Caller4310BeginRva, kFix321Caller4310EndRva,
            kFix321Helper04470Rva, 2U, reinterpret_cast<void*>(&Fix321Helper04470Redirect)},
        {kFix321Caller4580BeginRva, kFix321Caller4580EndRva,
            kFix321Helper04200Rva, 2U, reinterpret_cast<void*>(&Fix321Helper04200Redirect)},
        {kFix321Caller4580BeginRva, kFix321Caller4580EndRva,
            kFix321Helper042D0Rva, 2U, reinterpret_cast<void*>(&Fix321Helper042D0Redirect)},
        {kFix321Caller4580BeginRva, kFix321Caller4580EndRva,
            kFix321Helper043A0Rva, 2U, reinterpret_cast<void*>(&Fix321Helper043A0Redirect)},
        {kFix321Caller4580BeginRva, kFix321Caller4580EndRva,
            kFix321Helper04470Rva, 2U, reinterpret_cast<void*>(&Fix321Helper04470Redirect)},
    }};

    struct Candidate final {
        void* callSite{};
        void* expectedTarget{};
        void* detour{};
    };
    std::array<Candidate, static_cast<std::size_t>(kFix321ExpectedRedirectCallSites)> candidates{};
    std::size_t candidateCount = 0U;

    for (const auto& family : families) {
        std::uint32_t found = 0U;
        for (std::uint32_t rva = family.callerBeginRva;
             rva + 5U <= family.callerEndRva;
             ++rva) {
            std::array<std::byte, 5> callBytes{};
            const auto callAddress = moduleBase + rva;
            if (!CopyBytesSafely(
                    reinterpret_cast<const void*>(callAddress),
                    callBytes.data(), callBytes.size()) ||
                std::to_integer<std::uint8_t>(callBytes[0]) != 0xE8U) {
                continue;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, callBytes.data() + 1U, sizeof(displacement));
            const auto target = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(callAddress + 5U) + displacement);
            if (target != moduleBase + family.helperRva) continue;
            ++found;
            if (candidateCount >= candidates.size()) {
                error = "Fix3.21 discovered more certified call-sites than the static call graph";
                return false;
            }
            candidates[candidateCount++] = {
                reinterpret_cast<void*>(callAddress),
                reinterpret_cast<void*>(target),
                family.detour};
        }
        if (found != family.expectedCount) {
            error = "Fix3.21 retail call-site multiplicity does not match AI.exe.c static evidence";
            return false;
        }
    }
    if (candidateCount != candidates.size()) {
        error = "Fix3.21 certified call-site total does not match static evidence";
        return false;
    }

    for (std::size_t index = 0U; index < candidateCount; ++index) {
        CallRedirectRequest request{};
        request.name = "worldsync2_fix321_composite_state_" + std::to_string(index);
        request.callSite = candidates[index].callSite;
        request.expectedTarget = candidates[index].expectedTarget;
        request.detour = candidates[index].detour;
        if (!hooks.InstallCallRedirect(request, error)) return false;
    }

    gFix321RedirectCallSites.store(candidateCount, std::memory_order_release);
    gFix321CallRedirectsInstalled.store(true, std::memory_order_release);
    return true;
}


void Fix322HashU64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        hash ^= static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        hash *= kFix322HashPrime;
    }
}

[[nodiscard]] std::uint64_t Fix322WordsHash(
    const std::array<std::uint32_t, kFix322SnapshotWordCount>& words) noexcept {
    auto hash = kFix322HashOffset;
    for (const auto word : words) Fix322HashU64(hash, word);
    return hash == 0U ? 1U : hash;
}

[[nodiscard]] std::uint64_t Fix322SemanticHash(
    const Fix322TraceSemantic& semantic) noexcept {
    auto hash = kFix322HashOffset;
    Fix322HashU64(hash, semantic.targetRva);
    Fix322HashU64(hash, semantic.callerRva);
    Fix322HashU64(hash, semantic.firstPointerRva);
    Fix322HashU64(hash, semantic.firstPointerReadable);
    Fix322HashU64(hash, semantic.beforeHash);
    Fix322HashU64(hash, semantic.afterHash);
    Fix322HashU64(hash, semantic.changedWordMask);
    Fix322HashU64(hash, semantic.changedWordCount);
    for (std::size_t i = 0U; i < kFix322PublishedChangeCount; ++i) {
        Fix322HashU64(hash, semantic.changedOffsets[i]);
        Fix322HashU64(hash, semantic.beforeWords[i]);
        Fix322HashU64(hash, semantic.afterWords[i]);
    }
    return hash == 0U ? 1U : hash;
}

[[nodiscard]] bool Fix322SemanticEqual(
    const Fix322TraceSemantic& lhs,
    const Fix322TraceSemantic& rhs) noexcept {
    return lhs.targetRva == rhs.targetRva &&
        lhs.callerRva == rhs.callerRva &&
        lhs.firstPointerRva == rhs.firstPointerRva &&
        lhs.firstPointerReadable == rhs.firstPointerReadable &&
        lhs.beforeHash == rhs.beforeHash && lhs.afterHash == rhs.afterHash &&
        lhs.changedWordMask == rhs.changedWordMask &&
        lhs.changedWordCount == rhs.changedWordCount &&
        lhs.changedOffsets == rhs.changedOffsets &&
        lhs.beforeWords == rhs.beforeWords && lhs.afterWords == rhs.afterWords;
}

[[nodiscard]] bool Fix322TraceActive(const std::uint64_t now) noexcept {
    const auto origin = gFix322OriginTickMs.load(std::memory_order_acquire);
    const auto until = gFix322WindowUntilTickMs.load(std::memory_order_acquire);
    return origin != 0U && now >= origin && now <= until;
}

void PublishFix322Trace(
    const Fix322TraceSemantic& semantic,
    const std::uint32_t executionMask,
    const std::uint64_t now) noexcept {
    const auto origin = gFix322OriginTickMs.load(std::memory_order_acquire);
    if (origin == 0U || now < origin || !Fix322TraceActive(now)) return;
    gFix322Accepted.fetch_add(1U, std::memory_order_relaxed);
    if (semantic.changedWordMask != 0U) {
        gFix322ChangedCalls.fetch_add(1U, std::memory_order_relaxed);
    }
    const auto delta64 = now - origin;
    const auto delta = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(delta64, std::numeric_limits<std::uint32_t>::max()));
    const auto signature = Fix322SemanticHash(semantic);
    const auto start = static_cast<std::size_t>(signature) &
        (kFix322TraceSlotCapacity - 1U);
    for (std::size_t probe = 0U; probe < kFix322TraceSlotProbeCount; ++probe) {
        auto& slot = gFix322TraceSlots[
            (start + probe) & (kFix322TraceSlotCapacity - 1U)];
        auto present = slot.signatureHash.load(std::memory_order_acquire);
        if (present == 0U) {
            if (slot.signatureHash.compare_exchange_strong(
                    present, signature, std::memory_order_acq_rel, std::memory_order_acquire)) {
                slot.semantic = semantic;
                slot.count.store(1U, std::memory_order_relaxed);
                slot.executionMask.store(executionMask, std::memory_order_relaxed);
                slot.firstDeltaMs.store(delta, std::memory_order_relaxed);
                slot.lastDeltaMs.store(delta, std::memory_order_relaxed);
                slot.ready.store(1U, std::memory_order_release);
                gFix322UniqueEntries.fetch_add(1U, std::memory_order_relaxed);
                return;
            }
        }
        if (present != signature || slot.ready.load(std::memory_order_acquire) == 0U ||
            !Fix322SemanticEqual(slot.semantic, semantic)) continue;
        slot.count.fetch_add(1U, std::memory_order_relaxed);
        slot.executionMask.fetch_or(executionMask, std::memory_order_relaxed);
        slot.lastDeltaMs.store(delta, std::memory_order_relaxed);
        return;
    }
    gFix322CapacityDrops.fetch_add(1U, std::memory_order_relaxed);
}

void CaptureFix322CompositeExecution(
    std::uint32_t* execution,
    const std::uintptr_t returnAddress,
    const std::array<std::uint32_t, kFix322SnapshotWordCount>& before,
    const std::array<std::uint32_t, kFix322SnapshotWordCount>& after) noexcept {
    if (gFix322WindowUntilTickMs.load(std::memory_order_relaxed) == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (!gInitialized.load(std::memory_order_acquire) || !Fix322TraceActive(now)) return;
    const auto inspected = gFix322Inspected.fetch_add(1U, std::memory_order_relaxed);
    if (inspected >= kFix322ObservationLimit) {
        gFix322LimitDrops.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    Fix322TraceSemantic semantic{};
    semantic.targetRva = kFix322Target657F0Rva;
    semantic.callerRva = Fix321CallSiteRva(returnAddress);
    semantic.beforeHash = Fix322WordsHash(before);
    semantic.afterHash = Fix322WordsHash(after);
    void* firstPointer = nullptr;
    if (execution != nullptr && ReadPointerSafely(execution, firstPointer)) {
        semantic.firstPointerReadable = 1U;
        if (firstPointer != nullptr) {
            static_cast<void>(ModuleRelativeRva(firstPointer, semantic.firstPointerRva));
        }
    }

    std::size_t published = 0U;
    for (std::size_t i = 0U; i < kFix322SnapshotWordCount; ++i) {
        if (before[i] == after[i]) continue;
        semantic.changedWordMask |= (1U << i);
        ++semantic.changedWordCount;
        if (published < kFix322PublishedChangeCount) {
            semantic.changedOffsets[published] = static_cast<std::uint32_t>(i * sizeof(std::uint32_t));
            semantic.beforeWords[published] = before[i];
            semantic.afterWords[published] = after[i];
            ++published;
        }
    }
    // A no-change call is still valuable evidence that the choke point ran,
    // but retaining its ambient raw hash would manufacture many signatures.
    // Normalize such calls to one identity bucket; changed calls retain the
    // full bounded before/after hashes and first four raw word transitions.
    if (semantic.changedWordMask == 0U) {
        semantic.beforeHash = 0U;
        semantic.afterHash = 0U;
    }

    if (semantic.callerRva == 0U) {
        gFix322ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    PublishFix322Trace(
        semantic,
        replication_core::RemoteApplyActive()
            ? kFix319ExecutionRemoteApply
            : kFix319ExecutionLocal,
        now);
}

void __fastcall Fix322CompositeExecutionRedirect(std::uint32_t* execution, void*) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t returnAddress = 0U;
#endif
    const auto activeBefore = gFix322WindowUntilTickMs.load(std::memory_order_relaxed) != 0U &&
        Fix322TraceActive(static_cast<std::uint64_t>(GetTickCount64()));
    std::array<std::uint32_t, kFix322SnapshotWordCount> before{};
    bool beforeReadable = false;
    if (activeBefore && execution != nullptr) {
        beforeReadable = CopyBytesSafely(
            execution, reinterpret_cast<std::byte*>(before.data()), sizeof(before));
    }
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase != 0U) {
        const auto original = reinterpret_cast<CompositeExecutionFn>(moduleBase + kFix322Target657F0Rva);
        original(execution);
    }
    if (!activeBefore) return;
    std::array<std::uint32_t, kFix322SnapshotWordCount> after{};
    if (!beforeReadable || execution == nullptr ||
        !CopyBytesSafely(execution, reinterpret_cast<std::byte*>(after.data()), sizeof(after))) {
        gFix322ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    CaptureFix322CompositeExecution(execution, returnAddress, before, after);
}

[[nodiscard]] bool InstallFix322CompositeExecutionCallRedirect(
    HookManager& hooks,
    std::string& error) {
    error.clear();
    const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
    if (moduleBase == 0U) {
        error = "Fix3.22 main module base is unavailable";
        return false;
    }
    void* callSite = nullptr;
    std::uint32_t found = 0U;
    for (std::uint32_t rva = kFix322Caller65A90BeginRva;
         rva + 5U <= kFix322Caller65A90EndRva; ++rva) {
        std::array<std::byte, 5> callBytes{};
        const auto callAddress = moduleBase + rva;
        if (!CopyBytesSafely(reinterpret_cast<const void*>(callAddress), callBytes.data(), callBytes.size()) ||
            std::to_integer<std::uint8_t>(callBytes[0]) != 0xE8U) continue;
        std::int32_t displacement = 0;
        std::memcpy(&displacement, callBytes.data() + 1U, sizeof(displacement));
        const auto target = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(callAddress + 5U) + displacement);
        if (target != moduleBase + kFix322Target657F0Rva) continue;
        ++found;
        callSite = reinterpret_cast<void*>(callAddress);
    }
    if (found != kFix322ExpectedRedirectCallSites || callSite == nullptr) {
        error = "Fix3.22 retail call-site multiplicity does not match AI.exe.c static evidence";
        return false;
    }
    CallRedirectRequest request{};
    request.name = "worldsync2_fix322_composite_execution";
    request.callSite = callSite;
    request.expectedTarget = reinterpret_cast<void*>(moduleBase + kFix322Target657F0Rva);
    request.detour = reinterpret_cast<void*>(&Fix322CompositeExecutionRedirect);
    if (!hooks.InstallCallRedirect(request, error)) return false;
    gFix322RedirectCallSites.store(found, std::memory_order_release);
    gFix322CallRedirectInstalled.store(true, std::memory_order_release);
    return true;
}

void StartFix319Trace(const std::uint32_t role) noexcept {
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (!Fix319ArmActive(now)) return;
    gFix319Anchors.fetch_add(1U, std::memory_order_relaxed);
    std::uint64_t expected = 0U;
    if (gFix319OriginTickMs.compare_exchange_strong(
            expected, now, std::memory_order_acq_rel, std::memory_order_acquire)) {
        gFix319AnchorRole.store(role, std::memory_order_release);
        gFix319WindowUntilTickMs.store(now + kFix319TraceWindowMs, std::memory_order_release);
    }
}

[[nodiscard]] std::uint32_t Fix319CallSiteRva(
    const std::uintptr_t returnAddress,
    const bool typedMessage) noexcept {
    std::uint32_t callSiteRva = 0U;
    if (returnAddress == 0U) return 0U;
    if (typedMessage) {
        static_cast<void>(VerifyQueueTypedMessageDirectCallSite(returnAddress, callSiteRva));
    } else {
        static_cast<void>(VerifyQueueInputDirectCallSite(returnAddress, callSiteRva));
    }
    return callSiteRva;
}

[[nodiscard]] bool HashFix319Payload(
    const void* payloadData,
    const std::size_t payloadSize,
    std::uint64_t& output) noexcept {
    output = kFix319HashOffset;
    if (payloadSize == 0U) return true;
    if (payloadData == nullptr || payloadSize > kObservedTypedMessageMaximumDataBytes) return false;
    std::array<std::byte, 256U> chunk{};
    auto* source = static_cast<const std::byte*>(payloadData);
    std::size_t offset = 0U;
    while (offset < payloadSize) {
        const auto bytes = (std::min)(chunk.size(), payloadSize - offset);
        if (!CopyBytesSafely(source + offset, chunk.data(), bytes)) return false;
        for (std::size_t i = 0U; i < bytes; ++i) {
            output ^= static_cast<std::uint8_t>(chunk[i]);
            output *= kFix319HashPrime;
        }
        offset += bytes;
    }
    return true;
}

void PublishFix319Trace(
    const Fix319TraceSemantic& semantic,
    const std::uint32_t executionMask,
    const std::uint32_t callSiteRva,
    const std::uint64_t now) noexcept {
    const auto origin = gFix319OriginTickMs.load(std::memory_order_acquire);
    if (origin == 0U || now < origin || !Fix319TraceActive(now)) return;
    gFix319Accepted.fetch_add(1U, std::memory_order_relaxed);
    const auto delta64 = now - origin;
    const auto delta = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
        delta64, (std::numeric_limits<std::uint32_t>::max)()));
    const auto signature = Fix319SemanticHash(semantic);
    const auto start = static_cast<std::size_t>(signature) & (kFix319TraceSlotCapacity - 1U);

    for (std::size_t probe = 0U; probe < kFix319TraceSlotProbeCount; ++probe) {
        auto& slot = gFix319TraceSlots[(start + probe) & (kFix319TraceSlotCapacity - 1U)];
        auto observed = slot.signatureHash.load(std::memory_order_acquire);
        if (observed == 0U) {
            std::uint64_t expected = 0U;
            if (slot.signatureHash.compare_exchange_strong(
                    expected, signature, std::memory_order_acq_rel, std::memory_order_acquire)) {
                slot.semantic = semantic;
                slot.count.store(1U, std::memory_order_relaxed);
                slot.executionMask.store(executionMask, std::memory_order_relaxed);
                slot.firstDeltaMs.store(delta, std::memory_order_relaxed);
                slot.lastDeltaMs.store(delta, std::memory_order_relaxed);
                slot.firstCallSiteRva.store(callSiteRva, std::memory_order_relaxed);
                slot.lastCallSiteRva.store(callSiteRva, std::memory_order_relaxed);
                slot.callSiteVaried.store(0U, std::memory_order_relaxed);
                slot.ready.store(1U, std::memory_order_release);
                gFix319UniqueEntries.fetch_add(1U, std::memory_order_relaxed);
                return;
            }
            observed = expected;
        }
        if (observed != signature) continue;
        if (slot.ready.load(std::memory_order_acquire) == 0U) continue;
        // A 64-bit hash collision must never merge two distinct semantic
        // records. Continue probing if the normalized payload differs.
        if (!Fix319SemanticEqual(slot.semantic, semantic)) continue;
        slot.count.fetch_add(1U, std::memory_order_relaxed);
        slot.executionMask.fetch_or(executionMask, std::memory_order_relaxed);
        slot.lastDeltaMs.store(delta, std::memory_order_relaxed);
        const auto firstCall = slot.firstCallSiteRva.load(std::memory_order_relaxed);
        slot.lastCallSiteRva.store(callSiteRva, std::memory_order_relaxed);
        if (firstCall != callSiteRva) slot.callSiteVaried.store(1U, std::memory_order_relaxed);
        return;
    }
    gFix319CapacityDrops.fetch_add(1U, std::memory_order_relaxed);
}

void CaptureFix319TypedMessage(
    WorldEntityRegistry& registry,
    const CathodeRuntimeKey& sourceKey,
    const CathodeRuntimeKey& targetKey,
    const std::uint32_t messageType,
    const std::size_t payloadSize,
    const void* payloadData,
    const double delaySeconds,
    const bool remoteApply,
    const std::uintptr_t returnAddress) noexcept {
    if (gFix319WindowUntilTickMs.load(std::memory_order_relaxed) == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (!Fix319TraceActive(now)) return;
    gFix319Inspected.fetch_add(1U, std::memory_order_relaxed);

    Fix319TraceSemantic semantic{};
    semantic.channel = CausalTraceChannel::TypedMessage;
    semantic.sourceRuntimeKey = sourceKey;
    semantic.targetRuntimeKey = targetKey;
    semantic.primary = messageType;
    semantic.secondary = static_cast<std::uint32_t>((std::min<std::size_t>)(
        payloadSize, (std::numeric_limits<std::uint32_t>::max)()));
    semantic.payloadBytes = semantic.secondary;
    semantic.delayBits = std::bit_cast<std::uint64_t>(delaySeconds);
    if (const auto sourceWorld = registry.FindRuntimeWorldEntityId(sourceKey); sourceWorld.has_value()) {
        semantic.worldMask |= 0x1U;
        semantic.sourceWorldEntity = *sourceWorld;
    }
    if (const auto targetWorld = registry.FindRuntimeWorldEntityId(targetKey); targetWorld.has_value()) {
        semantic.worldMask |= 0x2U;
        semantic.targetWorldEntity = *targetWorld;
    }
    if (!HashFix319Payload(payloadData, payloadSize, semantic.payloadHash)) {
        gFix319ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    PublishFix319Trace(
        semantic,
        remoteApply ? kFix319ExecutionRemoteApply : kFix319ExecutionLocal,
        Fix319CallSiteRva(returnAddress, true),
        now);
}

void CaptureFix319CallbackBuild(
    WorldEntityRegistry& registry,
    void** sourceSharedRef,
    const std::uint32_t constructorFlags,
    const std::size_t payloadSize,
    const void* payloadData,
    const bool remoteApply,
    const std::uintptr_t returnAddress) noexcept {
    if (gFix319WindowUntilTickMs.load(std::memory_order_relaxed) == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (!Fix319TraceActive(now)) return;
    gFix319Inspected.fetch_add(1U, std::memory_order_relaxed);

    void* sourceShared = nullptr;
    if (!ReadPointerSafely(sourceSharedRef, sourceShared) || sourceShared == nullptr) {
        gFix319ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
    if (sourceExecution == nullptr) {
        gFix319ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    Fix319TraceSemantic semantic{};
    semantic.channel = CausalTraceChannel::CallbackBuild;
    semantic.primary = constructorFlags;
    semantic.secondary = static_cast<std::uint32_t>((std::min<std::size_t>)(
        payloadSize, (std::numeric_limits<std::uint32_t>::max)()));
    semantic.payloadBytes = semantic.secondary;
    if (!HashFix319Payload(payloadData, payloadSize, semantic.payloadHash)) {
        gFix319ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (registry.TryExtractRuntimeKey(sourceExecution, semantic.sourceRuntimeKey)) {
        if (const auto sourceWorld = registry.FindRuntimeWorldEntityId(semantic.sourceRuntimeKey);
            sourceWorld.has_value()) {
            semantic.worldMask |= 0x1U;
            semantic.sourceWorldEntity = *sourceWorld;
        }
    }
    void* sourceVtable = nullptr;
    if (ReadPointerSafely(sourceExecution, sourceVtable)) {
        static_cast<void>(ModuleRelativeRva(
            sourceVtable, semantic.callbackExecutionVtableRva));
    }

    std::uint32_t callSiteRva = 0U;
    if (returnAddress != 0U) {
        static_cast<void>(VerifyBuildCallbackDirectCallSite(returnAddress, callSiteRva));
    }
    PublishFix319Trace(
        semantic,
        remoteApply ? kFix319ExecutionRemoteApply : kFix319ExecutionLocal,
        callSiteRva,
        now);
}

void CaptureFix319InputEdge(
    WorldEntityRegistry& registry,
    void** sourceSharedRef,
    const std::uint32_t* inputShortGuid,
    void* targetDescriptor,
    void* callbackRecord,
    const double delaySeconds,
    const std::uint32_t retailTailArgument,
    const bool remoteApply,
    const std::uintptr_t returnAddress) noexcept {
    if (gFix319WindowUntilTickMs.load(std::memory_order_relaxed) == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (!Fix319TraceActive(now)) return;
    gFix319Inspected.fetch_add(1U, std::memory_order_relaxed);

    std::uint32_t schedulerParameter = 0U;
    std::uint32_t targetInput = 0U;
    void* sourceShared = nullptr;
    void* targetShared = nullptr;
    const auto* targetBytes = static_cast<const std::byte*>(targetDescriptor);
    if (inputShortGuid == nullptr || targetBytes == nullptr ||
        !ReadU32Safely(inputShortGuid, schedulerParameter) ||
        !ReadU32Safely(targetBytes, targetInput) ||
        !ReadPointerSafely(sourceSharedRef, sourceShared) || sourceShared == nullptr ||
        !ReadPointerSafely(targetBytes + 8U, targetShared) || targetShared == nullptr) {
        gFix319ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    Fix319TraceSemantic semantic{};
    semantic.channel = CausalTraceChannel::InputEdge;
    semantic.primary = schedulerParameter;
    semantic.secondary = targetInput;
    semantic.delayBits = std::bit_cast<std::uint64_t>(delaySeconds);
    semantic.tailArgument = retailTailArgument;
    auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
    auto* targetExecution = ExecutionObjectFromShared(targetShared);
    const bool haveSourceKey = sourceExecution != nullptr &&
        registry.TryExtractRuntimeKey(sourceExecution, semantic.sourceRuntimeKey);
    const bool haveTargetKey = targetExecution != nullptr &&
        registry.TryExtractRuntimeKey(targetExecution, semantic.targetRuntimeKey);
    if (haveSourceKey) {
        if (const auto sourceWorld = registry.FindRuntimeWorldEntityId(semantic.sourceRuntimeKey);
            sourceWorld.has_value()) {
            semantic.worldMask |= 0x1U;
            semantic.sourceWorldEntity = *sourceWorld;
        }
    }
    if (haveTargetKey) {
        if (const auto targetWorld = registry.FindRuntimeWorldEntityId(semantic.targetRuntimeKey);
            targetWorld.has_value()) {
            semantic.worldMask |= 0x2U;
            semantic.targetWorldEntity = *targetWorld;
        }
    }

    if (callbackRecord != nullptr) {
        semantic.semanticFlags |= kFix319SemanticCallbackPresent;
        std::array<std::uint32_t, kCallbackRawWordCount> words{};
        if (CopyBytesSafely(
                callbackRecord,
                reinterpret_cast<std::byte*>(words.data()),
                sizeof(words))) {
            semantic.semanticFlags |= kFix319SemanticCallbackReadable;
            semantic.callbackInput = words[0];
            semantic.callbackFlags = words[8];
            if (words[3] != 0U) {
                semantic.semanticFlags |= kFix319SemanticCallbackNestedPresent;
            }
            if (words[4] != 0U) {
                semantic.semanticFlags |= kFix319SemanticCallbackPayloadPresent;
            }
            const auto sharedAddress = static_cast<std::uintptr_t>(words[2]);
            if (sharedAddress != 0U) {
                auto* callbackExecution = ExecutionObjectFromShared(
                    reinterpret_cast<void*>(sharedAddress));
                if (callbackExecution != nullptr) {
                    void* callbackVtable = nullptr;
                    if (ReadPointerSafely(callbackExecution, callbackVtable)) {
                        static_cast<void>(ModuleRelativeRva(
                            callbackVtable, semantic.callbackExecutionVtableRva));
                    }
                    if (registry.TryExtractRuntimeKey(
                            callbackExecution, semantic.callbackRuntimeKey)) {
                        semantic.semanticFlags |= kFix319SemanticCallbackKeyValid;
                    }
                }
            }
        } else {
            gFix319ReadFailures.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    // The flight recorder is broad enough to catch the historically dangerous
    // callback family, but avoids spending a trace slot on fully non-World,
    // callback-free scheduler noise.
    if (semantic.worldMask == 0U && callbackRecord == nullptr) return;
    PublishFix319Trace(
        semantic,
        remoteApply ? kFix319ExecutionRemoteApply : kFix319ExecutionLocal,
        Fix319CallSiteRva(returnAddress, false),
        now);
}

void CaptureFix319LogicSwitch(
    WorldEntityRegistry& registry,
    const CathodeRuntimeKey& contextRuntimeKey,
    const CathodeRuntimeKey& stateRuntimeKey,
    const WorldEntityId& stateWorldEntity,
    const bool enabled) noexcept {
    if (gFix319WindowUntilTickMs.load(std::memory_order_relaxed) == 0U) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (!Fix319TraceActive(now)) return;
    gFix319Inspected.fetch_add(1U, std::memory_order_relaxed);

    Fix319TraceSemantic semantic{};
    semantic.channel = CausalTraceChannel::LogicSwitch;
    semantic.sourceRuntimeKey = contextRuntimeKey;
    semantic.targetRuntimeKey = stateRuntimeKey;
    semantic.primary = enabled ? 1U : 0U;
    if (const auto contextWorld = registry.FindRuntimeWorldEntityId(contextRuntimeKey);
        contextWorld.has_value()) {
        semantic.worldMask |= 0x1U;
        semantic.sourceWorldEntity = *contextWorld;
    }
    if (stateWorldEntity.Valid()) {
        semantic.worldMask |= 0x2U;
        semantic.targetWorldEntity = stateWorldEntity;
    }
    PublishFix319Trace(
        semantic, kFix319ExecutionLocal, 0U, now);
}

[[nodiscard]] std::uint64_t HashFix318Bytes(
    const std::array<std::byte, kFix318CodeSampleBytes>& bytes) noexcept {
    auto hash = kFix318HashOffset;
    for (const auto value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= kFix318HashPrime;
    }
    return hash;
}

[[nodiscard]] bool DecodeFix318Branch(
    const void* entry,
    const std::byte* bytes,
    const std::size_t size,
    std::uint32_t& kind,
    void*& branchTarget) noexcept {
    kind = kFix318BranchUnknown;
    branchTarget = nullptr;
    if (entry == nullptr || bytes == nullptr || size < 2U) return false;

    const auto opcode = static_cast<std::uint8_t>(bytes[0]);
    const auto entryAddress = reinterpret_cast<std::intptr_t>(entry);
    if ((opcode == 0xE9U || opcode == 0xE8U) && size >= 5U) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 1U, sizeof(displacement));
        branchTarget = reinterpret_cast<void*>(
            entryAddress + 5 + static_cast<std::intptr_t>(displacement));
        kind = opcode == 0xE9U
            ? kFix318BranchJmpRel32
            : kFix318BranchCallRel32;
        return true;
    }
    if (opcode == 0xEBU) {
        std::int8_t displacement = 0;
        std::memcpy(&displacement, bytes + 1U, sizeof(displacement));
        branchTarget = reinterpret_cast<void*>(
            entryAddress + 2 + static_cast<std::intptr_t>(displacement));
        kind = kFix318BranchJmpRel8;
        return true;
    }
    if (opcode == 0xFFU && size >= 6U &&
        static_cast<std::uint8_t>(bytes[1]) == 0x25U) {
        std::uint32_t pointerSlotVa = 0U;
        std::memcpy(&pointerSlotVa, bytes + 2U, sizeof(pointerSlotVa));
        void* resolved = nullptr;
        if (pointerSlotVa == 0U || !ReadPointerSafely(
                reinterpret_cast<const void*>(
                    static_cast<std::uintptr_t>(pointerSlotVa)),
                resolved) || resolved == nullptr) {
            kind = kFix318BranchJmpAbsIndirect;
            return false;
        }
        kind = kFix318BranchJmpAbsIndirect;
        branchTarget = resolved;
        return true;
    }
    return false;
}

[[nodiscard]] Fix318ClearCodeSnapshot CaptureFix318ClearCode(
    void* target,
    const std::uint32_t targetRva) noexcept {
    Fix318ClearCodeSnapshot snapshot{};
    snapshot.attempted = target != nullptr;
    snapshot.targetRva = targetRva;
    snapshot.finalTargetRva = targetRva;
    if (target == nullptr) return snapshot;

    std::array<std::byte, kFix318CodeSampleBytes> bytes{};
    if (!CopyBytesSafely(target, bytes.data(), bytes.size())) return snapshot;

    std::memcpy(&snapshot.word0, bytes.data(), sizeof(snapshot.word0));
    std::memcpy(&snapshot.word1, bytes.data() + sizeof(snapshot.word0), sizeof(snapshot.word1));
    snapshot.hash = HashFix318Bytes(bytes);
    snapshot.readable = true;

    void* firstBranchTarget = nullptr;
    std::uint32_t firstKind = kFix318BranchUnknown;
    const bool haveFirstBranch = DecodeFix318Branch(
        target, bytes.data(), bytes.size(), firstKind, firstBranchTarget);
    snapshot.branchKind = firstKind;
    if (haveFirstBranch && firstBranchTarget != nullptr) {
        static_cast<void>(ModuleRelativeRva(
            firstBranchTarget, snapshot.branchTargetRva));
    }

    // CALL rel32 is recorded as provenance but not followed because execution
    // returns to this entry. Only unconditional JMP forms are followed, with a
    // strict depth limit and module-relative validation at every hop.
    if (!haveFirstBranch || firstBranchTarget == nullptr ||
        firstKind == kFix318BranchCallRel32) {
        return snapshot;
    }

    void* current = firstBranchTarget;
    std::uint32_t currentRva = 0U;
    for (std::uint32_t depth = 1U; depth <= kFix318MaxJumpDepth; ++depth) {
        if (!ModuleRelativeRva(current, currentRva)) break;
        snapshot.finalTargetRva = currentRva;
        snapshot.jumpDepth = depth;

        std::array<std::byte, 6> hopBytes{};
        if (!CopyBytesSafely(current, hopBytes.data(), hopBytes.size())) break;
        std::uint32_t hopKind = kFix318BranchUnknown;
        void* hopTarget = nullptr;
        const bool haveHop = DecodeFix318Branch(
            current, hopBytes.data(), hopBytes.size(), hopKind, hopTarget);
        if (!haveHop || hopTarget == nullptr ||
            hopKind == kFix318BranchCallRel32 ||
            hopKind == kFix318BranchUnknown) {
            break;
        }
        if (hopTarget == target || hopTarget == current) break;
        current = hopTarget;
    }
    return snapshot;
}

[[nodiscard]] Fix317ClearDispatchSnapshot CaptureFix317ClearDispatch(
    void* nativeSwitch) noexcept {
    Fix317ClearDispatchSnapshot snapshot{};
    snapshot.attempted = true;
    if (nativeSwitch == nullptr) return snapshot;

    void* vtable = nullptr;
    if (!ReadPointerSafely(nativeSwitch, vtable) || vtable == nullptr ||
        !ModuleRelativeRva(vtable, snapshot.vtableRva)) {
        return snapshot;
    }

    void* target = nullptr;
    if (!ReadPointerSafely(
            static_cast<const std::byte*>(vtable) + kFix317LogicSwitchDispatchSlot,
            target) || target == nullptr ||
        !ModuleRelativeRva(target, snapshot.targetRva)) {
        return snapshot;
    }

    snapshot.readable = true;
    snapshot.code = CaptureFix318ClearCode(target, snapshot.targetRva);
    return snapshot;
}

void CommitFix317ClearDispatch(
    const CathodeRuntimeKey& contextRuntimeKey,
    const CathodeRuntimeKey& stateRuntimeKey,
    const std::uint8_t before,
    const std::uint8_t after,
    const Fix317ClearDispatchSnapshot& snapshot) noexcept {
    if (!snapshot.attempted ||
        !IsFix311ExactStateKey(contextRuntimeKey) ||
        !IsFix311ExactStateKey(stateRuntimeKey) ||
        before == 0U || after != 0U) {
        return;
    }

    gClearDispatchExactMatches.fetch_add(1U, std::memory_order_relaxed);
    gClearDispatchBeforeState.store(before, std::memory_order_relaxed);
    gClearDispatchAfterState.store(after, std::memory_order_relaxed);
    gClearDispatchThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    gClearDispatchTickMs.store(
        static_cast<std::uint64_t>(GetTickCount64()), std::memory_order_relaxed);

    if (!snapshot.readable) {
        gClearDispatchReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    gClearDispatchVtableRva.store(snapshot.vtableRva, std::memory_order_relaxed);
    gClearDispatchTargetRva.store(snapshot.targetRva, std::memory_order_relaxed);
    auto expected = 0U;
    static_cast<void>(gClearDispatchFirstTargetRva.compare_exchange_strong(
        expected, snapshot.targetRva,
        std::memory_order_relaxed, std::memory_order_relaxed));
    const auto previous = gClearDispatchLastTargetRva.exchange(
        snapshot.targetRva, std::memory_order_relaxed);
    if (previous != 0U && previous != snapshot.targetRva) {
        gClearDispatchTargetVaried.store(1U, std::memory_order_relaxed);
    }

    gClearCodeExactMatches.fetch_add(1U, std::memory_order_relaxed);
    gClearCodeThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    gClearCodeTickMs.store(
        static_cast<std::uint64_t>(GetTickCount64()), std::memory_order_relaxed);
    if (!snapshot.code.attempted || !snapshot.code.readable) {
        gClearCodeReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    gClearCodeTargetRva.store(snapshot.code.targetRva, std::memory_order_relaxed);
    gClearCodeWord0.store(snapshot.code.word0, std::memory_order_relaxed);
    gClearCodeWord1.store(snapshot.code.word1, std::memory_order_relaxed);
    gClearCodeBranchKind.store(snapshot.code.branchKind, std::memory_order_relaxed);
    gClearCodeBranchTargetRva.store(
        snapshot.code.branchTargetRva, std::memory_order_relaxed);
    gClearCodeFinalTargetRva.store(
        snapshot.code.finalTargetRva, std::memory_order_relaxed);
    gClearCodeJumpDepth.store(snapshot.code.jumpDepth, std::memory_order_relaxed);
    gClearCodeHash.store(snapshot.code.hash, std::memory_order_relaxed);
}

void CommitExactLogicSwitchCallback(
    const CathodeRuntimeKey& contextRuntimeKey,
    const CathodeRuntimeKey& stateRuntimeKey,
    const bool enabled,
    const ExactLogicSwitchCallbackSnapshot& snapshot) noexcept {
    if (!IsFix311ExactStateKey(stateRuntimeKey)) return;

    gLogicExactTargetMatches.fetch_add(1U, std::memory_order_relaxed);
    gLogicExactContextSameAsState.store(
        contextRuntimeKey == stateRuntimeKey ? 1U : 0U,
        std::memory_order_relaxed);
    gLogicExactEnabled.store(enabled ? 1U : 0U, std::memory_order_relaxed);

    if (snapshot.holderReadable) {
        gLogicExactHolderReadable.fetch_add(1U, std::memory_order_relaxed);
    }
    if (snapshot.callbackPresent) {
        gLogicExactCallbackPresent.fetch_add(1U, std::memory_order_relaxed);
    }
    if (snapshot.rawReadable) {
        gLogicExactRawReadable.fetch_add(1U, std::memory_order_relaxed);
        gLogicExactInput.store(snapshot.words[0], std::memory_order_relaxed);
        gLogicExactNestedPresent.store(
            snapshot.words[3] != 0U ? 1U : 0U, std::memory_order_relaxed);
        gLogicExactPayloadPresent.store(
            (snapshot.words[4] != 0U || snapshot.words[5] != 0U) ? 1U : 0U,
            std::memory_order_relaxed);
        gLogicExactFlags.store(snapshot.words[8], std::memory_order_relaxed);
        gLogicExactRefCount.store(snapshot.words[9], std::memory_order_relaxed);
    } else if (snapshot.callbackPresent) {
        gLogicExactRawReadFailures.fetch_add(1U, std::memory_order_relaxed);
    }
    if (snapshot.callbackExecutionPresent) {
        gLogicExactCallbackObjectPresent.fetch_add(1U, std::memory_order_relaxed);
    }
    if (snapshot.callbackRuntimeKeyValid) {
        gLogicExactCallbackKeyResolved.fetch_add(1U, std::memory_order_relaxed);
        gLogicExactCallbackKeyA.store(
            snapshot.callbackRuntimeKey.entityA, std::memory_order_relaxed);
        gLogicExactCallbackKeyB.store(
            snapshot.callbackRuntimeKey.entityB, std::memory_order_relaxed);
    }
    if (snapshot.callbackExecutionReadable) {
        gLogicExactCallbackExecutionReadable.fetch_add(1U, std::memory_order_relaxed);
        gLogicExactCallbackExecutionVtableRva.store(
            snapshot.callbackExecutionVtableRva, std::memory_order_relaxed);
    }
    TouchVersion();
}

void RecordLogicSwitchSafetyObservation(
    WorldEntityRegistry& registry,
    const CathodeRuntimeKey& contextRuntimeKey,
    const CathodeRuntimeKey& stateRuntimeKey,
    const bool enabled) noexcept {
    if (registry.FindRuntimeWorldEntityId(contextRuntimeKey).has_value()) {
        gLogicSwitchContextWorld.fetch_add(1U, std::memory_order_relaxed);
    } else {
        gLogicSwitchContextNonWorld.fetch_add(1U, std::memory_order_relaxed);
    }
    gLogicSwitchSafetyBlocked.fetch_add(1U, std::memory_order_relaxed);
    gLogicSwitchLastContextA.store(contextRuntimeKey.entityA, std::memory_order_relaxed);
    gLogicSwitchLastContextB.store(contextRuntimeKey.entityB, std::memory_order_relaxed);
    gLogicSwitchLastStateA.store(stateRuntimeKey.entityA, std::memory_order_relaxed);
    gLogicSwitchLastStateB.store(stateRuntimeKey.entityB, std::memory_order_relaxed);
    gLogicSwitchLastEnabled.store(enabled ? 1U : 0U, std::memory_order_relaxed);
    TouchVersion();
}

[[nodiscard]] bool HistoricalLogicDiagnosticsActive() noexcept {
    const auto until = std::max({
        gCallbackClassificationUntilTickMs.load(std::memory_order_relaxed),
        gCharacterCallbackOriginProbeUntilTickMs.load(std::memory_order_relaxed),
        gLogicDownstreamProbeUntilTickMs.load(std::memory_order_relaxed),
        gFix319ArmUntilTickMs.load(std::memory_order_relaxed),
        gFix319WindowUntilTickMs.load(std::memory_order_relaxed)});
    return until != 0U && GetTickCount64() <= until;
}

std::uint8_t __fastcall LogicSwitchSetDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    const auto original = reinterpret_cast<LogicSwitchInputFn>(
        gLogicSwitchSetTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        return original(self, contextA, contextB);
    }
    if (world_actions::Installed() && !HistoricalLogicDiagnosticsActive()) {
        return original(self, contextA, contextB);
    }

    const bool remoteApply = replication_core::RemoteApplyActive();
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!gInitialized.load(std::memory_order_acquire) || registry == nullptr ||
        !registry->RuntimeCaptureReady() || !HasLogicSwitchVtable(self)) {
        return original(self, contextA, contextB);
    }
    if (remoteApply) {
        gLogicSwitchRemoteSuppressed.fetch_add(1U, std::memory_order_relaxed);
        return original(self, contextA, contextB);
    }

    std::uint8_t before = 0U;
    const bool haveBefore = ReadU8Safely(
        static_cast<const std::byte*>(self) + kLogicSwitchStateOffset, before);
    if (!haveBefore || before != 0U) {
        return original(self, contextA, contextB);
    }

    gLogicSwitchObserved.fetch_add(1U, std::memory_order_relaxed);
    CathodeRuntimeKey contextRuntimeKey{};
    const bool haveContextKey = ExtractLogicSwitchContextRuntimeKey(
        *registry, contextA, contextRuntimeKey);
    // Fix3.11: contextB is the exact callback holder passed into the retail
    // LogicSwitch dispatch. Fix3.10 BuildFix01 proved the causal unlock switch
    // is self-contextualized, so only touch contextB when contextA already has
    // the exact diagnostic key. This keeps the read-only probe off the global
    // LogicSwitch hot path.
    ExactLogicSwitchCallbackSnapshot exactCallbackSnapshot{};
    if (haveContextKey &&
        contextRuntimeKey.entityA == kFix311ExactStateKeyA &&
        contextRuntimeKey.entityB == kFix311ExactStateKeyB) {
        exactCallbackSnapshot = CaptureExactLogicSwitchCallback(*registry, contextB);
    }

    const auto result = original(self, contextA, contextB);
    std::uint8_t after = 0U;
    const bool haveAfter = ReadU8Safely(
        static_cast<const std::byte*>(self) + kLogicSwitchStateOffset, after);
    if (!haveAfter || after == 0U) return result;

    CathodeRuntimeKey stateRuntimeKey{};
    const auto entity = registry->IdentifyRuntimeWorldEntity(
        static_cast<std::byte*>(self) - kLogicSwitchNativeOffset, &stateRuntimeKey);
    if (!entity.has_value()) {
        gLogicSwitchUnresolved.fetch_add(1U, std::memory_order_relaxed);
        return result;
    }

    if (!haveContextKey) {
        gLogicSwitchUnresolved.fetch_add(1U, std::memory_order_relaxed);
        return result;
    }

    const auto probeUntil = gCallbackClassificationUntilTickMs.load(std::memory_order_relaxed);
    if (probeUntil != 0U && static_cast<std::uint64_t>(GetTickCount64()) <= probeUntil) {
        ObserveLogicSwitchCausalEvidence(*entity, true, stateRuntimeKey);
    }
    gLogicSwitchWorldStates.fetch_add(1U, std::memory_order_relaxed);
    CaptureFix319LogicSwitch(
        *registry, contextRuntimeKey, stateRuntimeKey, *entity, true);
    CommitExactLogicSwitchCallback(
        contextRuntimeKey, stateRuntimeKey, true, exactCallbackSnapshot);
    // Fix3.10 BuildFix01: global LogicSwitch replay is observation-only.
    // The Fix3.10 runtime-context experiment proved neither cross-process
    // semantic equivalence nor safe global event volume; do not submit a
    // WorldMutation until a bounded, evidence-backed replay class exists.
    RecordLogicSwitchSafetyObservation(*registry, contextRuntimeKey, stateRuntimeKey, true);
    return result;
}

std::uint8_t __fastcall LogicSwitchClearDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    const auto original = reinterpret_cast<LogicSwitchInputFn>(
        gLogicSwitchClearTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        return original(self, contextA, contextB);
    }
    if (world_actions::Installed() && !HistoricalLogicDiagnosticsActive()) {
        return original(self, contextA, contextB);
    }

    const bool remoteApply = replication_core::RemoteApplyActive();
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!gInitialized.load(std::memory_order_acquire) || registry == nullptr ||
        !registry->RuntimeCaptureReady() || !HasLogicSwitchVtable(self)) {
        return original(self, contextA, contextB);
    }
    if (remoteApply) {
        gLogicSwitchRemoteSuppressed.fetch_add(1U, std::memory_order_relaxed);
        return original(self, contextA, contextB);
    }

    std::uint8_t before = 0U;
    const bool haveBefore = ReadU8Safely(
        static_cast<const std::byte*>(self) + kLogicSwitchStateOffset, before);
    const bool transitionCandidate = haveBefore && before != 0U;

    CathodeRuntimeKey contextRuntimeKey{};
    bool haveContextKey = false;
    ExactLogicSwitchCallbackSnapshot exactCallbackSnapshot{};
    Fix317ClearDispatchSnapshot clearDispatchSnapshot{};
    if (transitionCandidate) {
        gLogicSwitchObserved.fetch_add(1U, std::memory_order_relaxed);
        haveContextKey = ExtractLogicSwitchContextRuntimeKey(
            *registry, contextA, contextRuntimeKey);
        // Fix3.11: contextB is the exact callback holder passed into the retail
        // LogicSwitch dispatch. Fix3.10 BuildFix01 proved the causal unlock switch
        // is self-contextualized, so only touch contextB when contextA already has
        // the exact diagnostic key. This keeps the read-only probe off the global
        // LogicSwitch hot path.
        if (haveContextKey &&
            contextRuntimeKey.entityA == kFix311ExactStateKeyA &&
            contextRuntimeKey.entityB == kFix311ExactStateKeyB) {
            exactCallbackSnapshot = CaptureExactLogicSwitchCallback(*registry, contextB);
            if (Fix315DownstreamProbeActive()) {
                gClearDispatchCandidates.fetch_add(1U, std::memory_order_relaxed);
                gClearCodeCandidates.fetch_add(1U, std::memory_order_relaxed);
                clearDispatchSnapshot = CaptureFix317ClearDispatch(self);
            }
        }
    }

    const bool fix315ScopeActive = transitionCandidate && haveContextKey &&
        IsFix311ExactStateKey(contextRuntimeKey) &&
        Fix315DownstreamProbeActive();
    Fix315ExactClearScope fix315Scope(fix315ScopeActive);
    const auto result = original(self, contextA, contextB);

    // Fix3.24 BuildFix03: canonicalize the PUBLIC INPUT INVOCATION, not only a
    // 1->0 mutation of the LogicSwitch state byte. COMMANDS proves that this
    // retail CLEAR entry point is the set_false endpoint for a public Composite
    // bridge; an invocation is therefore the relevant event even when the
    // switch byte was already false. The previous post-transition-only gate
    // could silently miss exactly such inputs, yielding ci24Map=0.
    //
    // A mapped QueueInput scope still suppresses this fallback so one public
    // action is not emitted both from QueueInput and from its resulting CLEAR.
    CathodeRuntimeKey stateRuntimeKey{};
    // BuildFix04: nested Composite internals are not guaranteed to be World-domain
    // manifest entities. Resolve a generic stable identity here; the public-input
    // reverse map remains exact and fail-closed, so this does not re-enable raw
    // LogicSwitch replay.
    const auto entity = registry->IdentifyRuntimeEntity(
        static_cast<std::byte*>(self) - kLogicSwitchNativeOffset, &stateRuntimeKey);
    if (!world_actions::Installed() && gCompositeMappedQueueDepth == 0U) {
        // Fix3.25 runtime evidence: the exact nested LogicSwitch RuntimeKey
        // survives even when the stable runtime entity classification does not.
        // Prefer the exact runtime-key reverse bridge, then retain the prior
        // stable-ID fallback for older/alternate runtime layouts.
        const auto runtimeMapping = registry->FindCompositePublicInputForRuntimeEdge(
            stateRuntimeKey, kLogicSwitchSetFalseInputShortGuid);
        if (runtimeMapping.has_value()) {
            SubmitCompositePublicInputForRuntimeEdge(
                *registry, stateRuntimeKey, kLogicSwitchSetFalseInputShortGuid);
        } else {
            const auto ownerScopeMapping =
                registry->FindCompositePublicInputForOwnerScopeRuntimeEdge(
                    stateRuntimeKey, kLogicSwitchSetFalseInputShortGuid);
            if (ownerScopeMapping.has_value()) {
                SubmitCompositePublicInputForOwnerScopeRuntimeEdge(
                    *registry, stateRuntimeKey, kLogicSwitchSetFalseInputShortGuid);
            } else if (entity.has_value()) {
                SubmitCompositePublicInputForInternalEdge(
                    *registry, *entity, kLogicSwitchSetFalseInputShortGuid);
            }
        }
    }

    // Historical LogicSwitch state/callback diagnostics remain transition-only.
    // Fix3.24 BuildFix03 changes only public-input canonicalization semantics.
    if (!transitionCandidate) return result;

    std::uint8_t after = 0U;
    const bool haveAfter = ReadU8Safely(
        static_cast<const std::byte*>(self) + kLogicSwitchStateOffset, after);
    if (!haveAfter || after != 0U) return result;

    if (!entity.has_value()) {
        gLogicSwitchUnresolved.fetch_add(1U, std::memory_order_relaxed);
        return result;
    }

    if (!haveContextKey) {
        gLogicSwitchUnresolved.fetch_add(1U, std::memory_order_relaxed);
        return result;
    }

    const auto probeUntil = gCallbackClassificationUntilTickMs.load(std::memory_order_relaxed);
    if (probeUntil != 0U && static_cast<std::uint64_t>(GetTickCount64()) <= probeUntil) {
        ObserveLogicSwitchCausalEvidence(*entity, false, stateRuntimeKey);
    }
    gLogicSwitchWorldStates.fetch_add(1U, std::memory_order_relaxed);
    CaptureFix319LogicSwitch(
        *registry, contextRuntimeKey, stateRuntimeKey, *entity, false);
    CommitExactLogicSwitchCallback(
        contextRuntimeKey, stateRuntimeKey, false, exactCallbackSnapshot);
    CommitFix317ClearDispatch(
        contextRuntimeKey, stateRuntimeKey, before, after, clearDispatchSnapshot);
    // Fix3.10 BuildFix01: global LogicSwitch replay is observation-only.
    // The Fix3.10 runtime-context experiment proved neither cross-process
    // semantic equivalence nor safe global event volume; do not submit a
    // WorldMutation until a bounded, evidence-backed replay class exists.
    RecordLogicSwitchSafetyObservation(*registry, contextRuntimeKey, stateRuntimeKey, false);
    // Fix3.16 starts only after the retail CLEAR and all local diagnostic
    // bookkeeping have completed. This keeps mod-side observation work out of
    // the post-CLEAR scheduler window.
    if (IsFix311ExactStateKey(contextRuntimeKey) &&
        IsFix311ExactStateKey(stateRuntimeKey) &&
        Fix315DownstreamProbeActive()) {
        ArmFix316PostClearWindow();
    }
    return result;
}

void ClearEchoIds() noexcept {
    {
        std::lock_guard lock(gLocalObservationMutex);
        gLocalObservations.Clear();
        gLocalObservationRemembered = gLocalObservationEchoed = gLocalObservationRetired = gLocalObservationMissing = 0U;
    }
    {
        std::lock_guard lock(gSelfClockedMutex);
        gSelfClockedRoots.Clear();
    }
    gSelfClockedKeys.store(0U, std::memory_order_relaxed);
    gSelfClockedLocalRoots.store(0U, std::memory_order_relaxed);
    for (auto& slot : gTypedMatchHashes) slot.store(0U, std::memory_order_release);
    for (auto& slot : gTypedMatchExpiryMs) slot.store(0U, std::memory_order_release);
    for (auto& slot : gCharacterCallbackMatchHashes) slot.store(0U, std::memory_order_release);
    for (auto& slot : gCharacterCallbackMatchExpiryMs) slot.store(0U, std::memory_order_release);
}

// Fix3.27 - Pre-Queue Input-Target Public Capture.
//
// STATIC CONFIRMED:
//   FUN_00921F70 receives the target shared-ref, input ShortGuid and delay and
//   constructs the 12-byte descriptor consumed by FUN_009271C0.
//
// INFERENCE UNDER TEST:
//   a CompositeInstance public input can still pass through this constructor
//   before childLink resolution erases the public ShortGuid. The patch is
//   functional when that inference holds, but remains fail-closed otherwise.
//
// The network contract remains owner CompositeInstance + public ShortGuid.
// Runtime pointers/RuntimeKeys never leave this function.
void* __fastcall BuildInputTargetDetour(
    void* targetDescriptor,
    void*,
    void** targetSharedRef,
    const std::uint32_t* inputShortGuid,
    const double delaySeconds) noexcept {
    const auto original = reinterpret_cast<BuildInputTargetFn>(
        gBuildInputTargetTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return targetDescriptor;

    // Patch 133 PlayerSync-local target capture. Observe the exact retail
    // target shared-ref before any WS3 classification. This lane is process-local
    // and does not alter QueueInput, the descriptor, or WorldSync publication.
    if (const auto observer =
            gLocalInputTargetObserver.load(std::memory_order_acquire);
        observer != nullptr) {
        std::uint32_t observedInputShortGuid = 0U;
        void* observedTargetSharedRef = nullptr;
        if (ReadU32Safely(inputShortGuid, observedInputShortGuid) &&
            observedInputShortGuid != 0U && targetSharedRef != nullptr &&
            ReadPointerSafely(targetSharedRef, observedTargetSharedRef) &&
            observedTargetSharedRef != nullptr) {
            observer(observedTargetSharedRef, observedInputShortGuid);
        }
    }

    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        return original(targetDescriptor, targetSharedRef, inputShortGuid, delaySeconds);
    }

    const bool ws3Installed = world_actions::Installed();
    const bool ws3CallbackBridge =
        ws3Installed && world_actions::CallbackBridgeCaptureActive();
    // Patch 5.10: the 5.8/5.8.1 CanonicalComposite sender is dormant after
    // runtime proved it was not the door consequence and could force recovery.
    const bool ws3CanonicalClientCapture = ws3Installed &&
        world_actions::CanonicalCompositeClientCaptureActive() &&
        world_actions::SessionAuthorityBound() && !world_actions::SessionAuthorityHost() &&
        world_actions::WorldBusActive() && !replication_core::RemoteApplyActive();
    if (ws3CanonicalClientCapture) gCanonicalCompositePreQueueHint = {};
    if (ws3Installed) {
        gWs3PreQueueSeen.fetch_add(1U, std::memory_order_relaxed);
        std::uint32_t callerRva = 0U;
#if defined(_MSC_VER) && defined(_M_IX86)
        callerRva = CallerRvaFromReturnAddress(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
        callerRva = CallerRvaFromReturnAddress(__builtin_return_address(0));
#endif
        gWs3PreQueueLastCallerRva.store(callerRva, std::memory_order_relaxed);
        std::uint32_t observedInput = 0U;
        if (ReadU32Safely(inputShortGuid, observedInput) && observedInput != 0U) {
            gWs3PreQueueInputReadable.fetch_add(1U, std::memory_order_relaxed);
            gWs3PreQueueLastInput.store(observedInput, std::memory_order_relaxed);
            auto* ws3Registry = gRegistry.load(std::memory_order_acquire);
            bool targetResolved = false;
            if (ws3Registry != nullptr && ws3Registry->RuntimeCaptureReady()) {
                void* targetShared = nullptr;
                if (targetSharedRef != nullptr &&
                    ReadPointerSafely(targetSharedRef, targetShared) &&
                    targetShared != nullptr) {
                    if (auto* execution = ExecutionObjectFromShared(targetShared);
                        execution != nullptr) {
                        targetResolved = ws3Registry->IdentifyRuntimeEntity(execution).has_value();
                    }
                }
            }
            if (targetResolved)
                gWs3PreQueueTargetWorldResolved.fetch_add(1U, std::memory_order_relaxed);
            else
                gWs3PreQueueTargetWorldUnresolved.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    // Patch 5.8.1 restores the exact historical WS2 sender boundary inside
    // WS3. Non-Host Clients run the proven Fix3.27 reverse-map here and, after
    // the retail descriptor constructor returns successfully, may publish only
    // the stable public Composite semantic as ROOT kind 6. The later QueueInput
    // remains normal local retail; no callback or descriptor pointer is sent.
    if (ws3Installed && !ws3CallbackBridge && !ws3CanonicalClientCapture) {
        return original(targetDescriptor, targetSharedRef, inputShortGuid, delaySeconds);
    }
    gCompositePublicInputPreQueueBuilderSeen.fetch_add(1U, std::memory_order_relaxed);

    // Fix3.27 BuildFix01: preserve the public semantic at the same confirmed
    // pre-QueueInput boundary, but recognize both stable identities already
    // present in the manifest descriptor. v0.11.4.56 additionally allows the
    // WS3 callback bridge to reverse-map a manifest-known INTERNAL leaf back to
    // its canonical public Composite input. This uses only existing exact,
    // ambiguity-rejecting manifest indexes; runtime pointers/RuntimeKeys remain
    // process-local and the wire is still owner + public ShortGuid.
    std::optional<WorldCompositePublicInputDescriptor> mapping{};
    bool publicCandidate = false;
    bool internalCandidate = false;
    bool rejectedCandidate = false;
    bool mappedViaOwner = false;
    bool mappedViaInterface = false;
    bool mappedViaInternalExact = false;
    bool mappedViaInternalRuntime = false;
    bool mappedViaInternalOwnerScope = false;
    auto* registry = gRegistry.load(std::memory_order_acquire);
    const bool remoteApply = replication_core::RemoteApplyActive();

    std::uint32_t observedInputShortGuid = 0U;
    if (gInitialized.load(std::memory_order_acquire) &&
        gHookInstalled.load(std::memory_order_acquire) &&
        registry != nullptr && registry->RuntimeCaptureReady() &&
        ReadU32Safely(inputShortGuid, observedInputShortGuid) &&
        observedInputShortGuid != 0U) {
        gCompositePublicInputPreQueueBuilderInputReadable.fetch_add(
            1U, std::memory_order_relaxed);

        const bool isPublicInput =
            registry->CouldBeCompositePublicInputShortGuid(observedInputShortGuid);
        const bool isInternalInput = (ws3CallbackBridge || ws3CanonicalClientCapture) &&
            !isPublicInput &&
            registry->CouldBeCompositeInternalInputShortGuid(observedInputShortGuid);
        publicCandidate = isPublicInput;
        internalCandidate = isInternalInput;

        if (publicCandidate) {
            gCompositePublicInputPreQueueBuilderPublic.fetch_add(
                1U, std::memory_order_relaxed);
        } else if (internalCandidate) {
            gCompositePublicInputPreQueueBuilderInternal.fetch_add(
                1U, std::memory_order_relaxed);
        } else if (ws3CallbackBridge) {
            gCompositePublicInputPreQueueBuilderUnknownGuid.fetch_add(
                1U, std::memory_order_relaxed);

            // Diagnostic-only bounded trace. The .56 logs proved that door
            // unlock reaches this boundary twice with a ShortGuid absent from
            // both public and internal manifest indexes. Capture the raw leaf
            // identity and its retail caller before making any further WS3
            // semantic change. Nothing in this block can submit a mutation.
            CathodeRuntimeKey traceTargetKey{};
            WorldEntityId traceTargetId{};
            WorldEntityId traceBridgeOwner{};
            std::uint32_t traceOuterInput = 0U;
            void* traceTargetExecution = nullptr;
            void* traceTargetShared = nullptr;
            if (targetSharedRef != nullptr &&
                ReadPointerSafely(targetSharedRef, traceTargetShared) &&
                traceTargetShared != nullptr) {
                traceTargetExecution = ExecutionObjectFromShared(traceTargetShared);
                if (traceTargetExecution != nullptr) {
                    const auto traceIdentity = registry->IdentifyRuntimeEntity(
                        traceTargetExecution, &traceTargetKey);
                    if (traceIdentity.has_value()) traceTargetId = *traceIdentity;
                }
            }
            static_cast<void>(world_actions::TryGetCallbackBridgeContext(
                traceBridgeOwner, traceOuterInput));
            std::uint32_t traceCallerRva = 0U;
#if defined(_MSC_VER) && defined(_M_IX86)
            traceCallerRva = CallerRvaFromReturnAddress(_ReturnAddress());
#endif
            CapturePreQueueUnknownTrace(
                observedInputShortGuid, traceCallerRva, traceTargetKey,
                traceTargetExecution, traceTargetId, traceBridgeOwner,
                traceOuterInput);
        }

        if (publicCandidate || internalCandidate) {
            if (!std::isfinite(delaySeconds) || delaySeconds != 0.0) {
                rejectedCandidate = true;
                gCompositePublicInputPreQueueBuilderDelayRejected.fetch_add(
                    1U, std::memory_order_relaxed);
            } else if (remoteApply) {
                // Remote receiver execution must never echo a public operation.
                gCompositePublicInputPreQueueBuilderRemoteSkipped.fetch_add(
                    1U, std::memory_order_relaxed);
            } else {
                void* targetShared = nullptr;
                if (targetSharedRef != nullptr &&
                    ReadPointerSafely(targetSharedRef, targetShared) &&
                    targetShared != nullptr) {
                    if (auto* targetExecution = ExecutionObjectFromShared(targetShared);
                        targetExecution != nullptr) {
                        CathodeRuntimeKey targetRuntimeKey{};
                        const auto targetIdentity = registry->IdentifyRuntimeEntity(
                            targetExecution, &targetRuntimeKey);

                        if (publicCandidate && targetIdentity.has_value()) {
                            mapping = registry->FindCompositePublicInputForTarget(
                                *targetIdentity, observedInputShortGuid);
                            if (mapping.has_value()) {
                                mappedViaOwner = true;
                            } else {
                                mapping = registry->FindCompositePublicInputForInterfaceTarget(
                                    *targetIdentity, observedInputShortGuid);
                                mappedViaInterface = mapping.has_value();
                            }
                        } else if (internalCandidate) {
                            // A callback-bearing public action may already have
                            // been resolved to its internal child input by the
                            // time FUN_00921F70 constructs the target. Reuse the
                            // exact reverse indexes built from COMMANDS/AIMPWM05
                            // to recover the canonical public input. Every index
                            // fails closed on ambiguity and payload type.
                            if (targetIdentity.has_value()) {
                                mapping = registry->FindCompositePublicInputForInternalEdge(
                                    *targetIdentity, observedInputShortGuid);
                                mappedViaInternalExact = mapping.has_value();
                            }
                            if (!mapping.has_value() && targetRuntimeKey.entityA != 0U) {
                                mapping = registry->FindCompositePublicInputForRuntimeEdge(
                                    targetRuntimeKey, observedInputShortGuid);
                                mappedViaInternalRuntime = mapping.has_value();
                            }
                            if (!mapping.has_value() && targetRuntimeKey.entityA != 0U &&
                                targetRuntimeKey.entityB != 0U) {
                                mapping = registry->FindCompositePublicInputForOwnerScopeRuntimeEdge(
                                    targetRuntimeKey, observedInputShortGuid);
                                mappedViaInternalOwnerScope = mapping.has_value();
                            }
                        }

                        if (mapping.has_value() && !mapping->noTypedData) {
                            mapping.reset();
                            mappedViaOwner = false;
                            mappedViaInterface = false;
                            mappedViaInternalExact = false;
                            mappedViaInternalRuntime = false;
                            mappedViaInternalOwnerScope = false;
                            rejectedCandidate = true;
                            gCompositePublicInputPreQueueBuilderTypedRejected.fetch_add(
                                1U, std::memory_order_relaxed);
                        }
                    }
                }
                if (!mapping.has_value() && !rejectedCandidate) {
                    rejectedCandidate = true;
                    gCompositePublicInputPreQueueBuilderIdentityRejected.fetch_add(
                        1U, std::memory_order_relaxed);
                }
            }
        }
    }

    // Preserve retail ordering/lifetime first. Any network observation is
    // published only after the original descriptor constructor succeeds.
    void* result = original(
        targetDescriptor, targetSharedRef, inputShortGuid, delaySeconds);

    if ((publicCandidate || internalCandidate) && rejectedCandidate) {
        gCompositePublicInputPreQueueBuilderRejected.fetch_add(
            1U, std::memory_order_relaxed);
    }
    if (!mapping.has_value()) return result;

    gCompositePublicInputPreQueueBuilderMapped.fetch_add(1U, std::memory_order_relaxed);
    if (mappedViaOwner) {
        gCompositePublicInputPreQueueBuilderOwnerMapped.fetch_add(
            1U, std::memory_order_relaxed);
    }
    if (mappedViaInterface) {
        gCompositePublicInputPreQueueBuilderInterfaceMapped.fetch_add(
            1U, std::memory_order_relaxed);
    }
    if (mappedViaInternalExact) {
        gCompositePublicInputPreQueueBuilderInternalExactMapped.fetch_add(
            1U, std::memory_order_relaxed);
    }
    if (mappedViaInternalRuntime) {
        gCompositePublicInputPreQueueBuilderInternalRuntimeMapped.fetch_add(
            1U, std::memory_order_relaxed);
        gCompositePublicInputRuntimeEdgeMapped.fetch_add(1U, std::memory_order_relaxed);
    }
    if (mappedViaInternalOwnerScope) {
        gCompositePublicInputPreQueueBuilderInternalOwnerScopeMapped.fetch_add(
            1U, std::memory_order_relaxed);
        gCompositePublicInputOwnerScopeRuntimeEdgeMapped.fetch_add(
            1U, std::memory_order_relaxed);
    }
    if (!ws3CanonicalClientCapture)
        gCompositePublicInputMapped.fetch_add(1U, std::memory_order_relaxed);

    if (ws3CanonicalClientCapture) {
        // Patch 5.8.1: mirror the proven WS2 ordering exactly. FUN_00921F70 has
        // already recovered an ambiguity-free public Composite semantic and the
        // retail descriptor constructor has returned. Publish that stable
        // semantic now, then let the caller continue to QueueInput normally.
        //
        // This is deliberately NOT an authority-before-retail proposal: the
        // Client's local trigger/prompt cannot be vetoed by transport. The ROOT
        // carries only owner CompositeInstance + public ShortGuid; the sender's
        // callback/work item/RuntimeKey/descriptor address remain process-local.
        gCompositePublicInputMapped.fetch_add(1U, std::memory_order_relaxed);
        if (!mapping->noTypedData ||
            !registry->IsCompositePublicInputAllowed(
                mapping->ownerCompositeInstance, mapping->publicInputShortGuid) ||
            CompositeLocalChainGuardActive(
                mapping->ownerCompositeInstance, mapping->publicInputShortGuid) ||
            !AllowLocalCompositePublicInput(
                mapping->ownerCompositeInstance, mapping->publicInputShortGuid)) {
            gWs3CanonicalPreQueueAdmissionSuppressed.fetch_add(
                1U, std::memory_order_relaxed);
            return result;
        }

        gWs3CanonicalPreQueueSubmitAttempt.fetch_add(1U, std::memory_order_relaxed);
        world_actions::ContextTrace canonicalCause{};
        const auto submission = world_actions::SubmitCanonicalCompositeRoot(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid,
            canonicalCause);
        if (submission == world_actions::CanonicalCompositeSubmission::Submitted) {
            gWs3CanonicalPreQueueSubmitSuccess.fetch_add(1U, std::memory_order_relaxed);
            gCompositePublicInputPreQueueBuilderSubmitted.fetch_add(
                1U, std::memory_order_relaxed);
            gCompositePublicInputSubmitted.fetch_add(1U, std::memory_order_relaxed);
            RememberCompositeLocalChainGuard(
                mapping->ownerCompositeInstance, mapping->publicInputShortGuid);
        } else {
            // The retail descriptor has already been built and QueueInput will
            // still execute normally. Network failure never blocks local play.
            gWs3CanonicalPreQueueSubmitFailed.fetch_add(1U, std::memory_order_relaxed);
        }
        return result;
    }

    if (ws3CallbackBridge) {
        // WS3 v0.11.4.56 bridge mode. The outer callback-bearing public call
        // remains local. Publish only one DIFFERENT public input for the SAME
        // World Composite through the v4 retail ROOT contract. The mapping
        // above proved either a stable public target or an exact, ambiguity-free
        // internal-to-public reverse mapping, with no typed data and zero delay.
        // SubmitCallbackBridgeRoot adds same-owner/same-input, rate and
        // one-per-scope gates. No WS2 operation is emitted.
        if (!CompositeLocalChainGuardActive(
                mapping->ownerCompositeInstance, mapping->publicInputShortGuid) &&
            world_actions::SubmitCallbackBridgeRoot(
                mapping->ownerCompositeInstance, mapping->publicInputShortGuid)) {
            gCompositePublicInputPreQueueBuilderSubmitted.fetch_add(
                1U, std::memory_order_relaxed);
            RememberCompositeLocalChainGuard(
                mapping->ownerCompositeInstance, mapping->publicInputShortGuid);
        } else {
            gCompositePublicInputPreQueueBuilderRejected.fetch_add(
                1U, std::memory_order_relaxed);
        }
        return result;
    }

    if (!CompositeLocalChainGuardActive(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid) &&
        SubmitCompositePublicInput(*registry, *mapping)) {
        gCompositePublicInputPreQueueBuilderSubmitted.fetch_add(
            1U, std::memory_order_relaxed);
        RememberCompositeLocalChainGuard(
            mapping->ownerCompositeInstance, mapping->publicInputShortGuid);
    }
    return result;
}

struct SchedulerEdgeCapture final {
    WorldEntityId source{};
    WorldEntityId target{};
    std::uint32_t schedulerParameter{};
    std::uint32_t targetInput{};
    double relativeDelay{};
    void* targetShared{};
};

[[nodiscard]] bool TryDescribeSchedulerEdge(
    WorldEntityRegistry& registry, void** sourceSharedRef,
    const std::uint32_t* inputShortGuid, void* targetDescriptor,
    const double delaySeconds, SchedulerEdgeCapture& edge) noexcept {
    edge = {};
    std::uint32_t schedulerParameter = 0U;
    std::uint32_t targetInput = 0U;
    float targetDelay = 0.0F;
    void* targetShared = nullptr;
    const auto* targetBytes = static_cast<const std::byte*>(targetDescriptor);
    if (inputShortGuid == nullptr || targetBytes == nullptr ||
        !ReadU32Safely(inputShortGuid, schedulerParameter) || schedulerParameter == 0U ||
        !ReadU32Safely(targetBytes, targetInput) || targetInput == 0U ||
        !ReadFloatSafely(targetBytes + 4U, targetDelay) ||
        !ReadPointerSafely(targetBytes + 8U, targetShared) || targetShared == nullptr) {
        return false;
    }

    void* sourceShared = nullptr;
    if (!ReadPointerSafely(sourceSharedRef, sourceShared) || sourceShared == nullptr) return false;
    auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
    auto* targetExecution = ExecutionObjectFromShared(targetShared);
    const auto sourceId = IdentifyRuntimeWorldEntityCached(registry, sourceExecution);
    const auto targetId = IdentifyRuntimeWorldEntityCached(registry, targetExecution);
    if (!sourceId.has_value() || !targetId.has_value()) {
        if (!sourceId.has_value()) gUnresolvedSource.fetch_add(1U, std::memory_order_relaxed);
        if (!targetId.has_value()) gUnresolvedTarget.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    const auto relativeDelay = static_cast<double>(targetDelay) + delaySeconds;
    if (!std::isfinite(relativeDelay) || relativeDelay < 0.0 || relativeDelay > 600.0) return false;

    edge.source = *sourceId;
    edge.target = *targetId;
    edge.schedulerParameter = schedulerParameter;
    edge.targetInput = targetInput;
    edge.relativeDelay = relativeDelay;
    edge.targetShared = targetShared;
    return true;
}

void __fastcall QueueInputDetour(
    void* manager,
    void*,
    void** sourceSharedRef,
    const std::uint32_t* inputShortGuid,
    void* targetDescriptor,
    void* callbackRecord,
    const double delaySeconds,
    const std::uint32_t retailTailArgument) noexcept {
    const auto original = reinterpret_cast<QueueInputFn>(
        gQueueInputTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return;
    // These private presentation edges travel with PlayerSync. Let retail run
    // its exact door/marker path once; keep it out of generic WorldSync ROOTs.
    if (hiding_door_presentation::IsHidingDoorInput(sourceSharedRef, inputShortGuid,
            targetDescriptor, delaySeconds)) {
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        static_cast<void>(hiding_door_presentation::ObserveAcceptedQueueInput(
            sourceSharedRef, inputShortGuid, targetDescriptor, delaySeconds));
        return;
    }
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto retailReturnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto retailReturnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t retailReturnAddress = 0U;
#endif
    gObservedSchedulerCalls.fetch_add(1U, std::memory_order_relaxed);

    const bool ws3Installed = world_actions::Installed();
    if (ws3Installed) {
        gWs3QueueSeen.fetch_add(1U, std::memory_order_relaxed);
        gWs3QueueLastCallerRva.store(
            CallerRvaFromReturnAddress(
                reinterpret_cast<const void*>(retailReturnAddress)),
            std::memory_order_relaxed);
    }
    const bool schedulerSidecar = world_actions::SchedulerEdgeLocalSidecarCaptureActive();
    const bool schedulerRemoteNatural = world_actions::SchedulerEdgeRemoteNaturalCaptureActive();
    if (!schedulerSidecar && !schedulerRemoteNatural &&
        world_actions::SuppressDescendantCapture()) {
        if (ws3Installed) {
            gWs3QueueDescendantSuppressed.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(1U, std::memory_order_relaxed);
        }
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    const bool remoteApply = replication_core::RemoteApplyActive();
    if (!gInitialized.load(std::memory_order_acquire)) {
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (remoteApply && registry != nullptr && registry->RuntimeCaptureReady()) {
        CaptureFix319InputEdge(
            *registry, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument, true, retailReturnAddress);
        // Historical callback-specific dedupe remains diagnostic-only while WS3
        // is installed, but preserve it for explicit rollback builds.
        if (Fix320ReplayActive()) {
            ObservedCharacterCallbackInputEdge naturalEdge{};
            WorldEntityId naturalTarget{};
            if (TryDescribeFix320CharacterCallbackEdge(
                    *registry, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                    delaySeconds, retailReturnAddress, naturalEdge, naturalTarget)) {
                static_cast<void>(RememberCharacterCallbackMatch(
                    CharacterCallbackFingerprint(naturalEdge, naturalTarget)));
            }
        }
    }
    if (registry == nullptr || !registry->RuntimeCaptureReady()) {
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    // Patch 5.3: absorb the proven v12.8 scheduler boundary directly into WS3.
    // Do not run the historical WS2 adapters/probes when WS3 is installed.
    // This path keeps only the generic parameterless World->World edge shape:
    // stable source/target IDs, both scheduler/input ShortGuids and delay.
    if (ws3Installed) {
        // Patch 5.8: restore the proven WS2 *semantic* valve without restoring
        // the WS2 protocol. Callback-bearing QueueInput is first reverse-mapped
        // through exact manifest indexes to owner Composite + public ShortGuid.
        // The callback remains local; only the canonical public input is a ROOT.
        if (world_actions::CanonicalCompositeClientCaptureActive() &&
            callbackRecord != nullptr && !remoteApply &&
            world_actions::SessionAuthorityBound() && !world_actions::SessionAuthorityHost() &&
            world_actions::WorldBusActive() && gCompositeMappedQueueDepth == 0U) {
            gWs3CanonicalQueueCandidateSeen.fetch_add(1U, std::memory_order_relaxed);
            std::uint32_t canonicalSchedulerParameter = 0U;
            std::uint32_t canonicalTargetInput = 0U;
            float canonicalTargetDelay = 0.0F;
            void* canonicalTargetShared = nullptr;
            const auto* canonicalTargetBytes = static_cast<const std::byte*>(targetDescriptor);
            if (inputShortGuid != nullptr && canonicalTargetBytes != nullptr &&
                ReadU32Safely(inputShortGuid, canonicalSchedulerParameter) && canonicalSchedulerParameter != 0U &&
                ReadU32Safely(canonicalTargetBytes, canonicalTargetInput) && canonicalTargetInput != 0U &&
                ReadFloatSafely(canonicalTargetBytes + 4U, canonicalTargetDelay) &&
                ReadPointerSafely(canonicalTargetBytes + 8U, canonicalTargetShared) && canonicalTargetShared != nullptr) {
                const auto canonicalDelay = static_cast<double>(canonicalTargetDelay) + delaySeconds;
                std::optional<WorldCompositePublicInputDescriptor> canonical{};
                bool viaRuntime = false;
                bool viaOwnerScope = false;

                // Prefer the earlier Fix3.27 identity when this QueueInput is
                // consuming the exact descriptor built on the same thread. This
                // is the boundary that made callback-bearing public Composite
                // inputs work in WS2 even after retail lowered the target/input.
                if (gCanonicalCompositePreQueueHint.valid) {
                    if (gCanonicalCompositePreQueueHint.targetDescriptor == targetDescriptor) {
                        canonical = gCanonicalCompositePreQueueHint.mapping;
                        gWs3CanonicalPreQueueHintConsumed.fetch_add(1U, std::memory_order_relaxed);
                        gCanonicalCompositePreQueueHint = {};
                    } else {
                        gWs3CanonicalPreQueueHintMissed.fetch_add(1U, std::memory_order_relaxed);
                        // A pre-Queue hint is an immediate same-thread handoff.
                        // Never retain a mismatched descriptor and risk a later
                        // address reuse being interpreted as the original edge.
                        gCanonicalCompositePreQueueHint = {};
                    }
                }

                if (!canonical.has_value() && std::isfinite(canonicalDelay) && canonicalDelay == 0.0) {
                    if (auto* targetExecution = ExecutionObjectFromShared(canonicalTargetShared);
                        targetExecution != nullptr) {
                        CathodeRuntimeKey targetKey{};
                        const auto targetId = registry->IdentifyRuntimeEntity(targetExecution, &targetKey);
                        if (registry->CouldBeCompositePublicInputShortGuid(canonicalTargetInput) && targetId.has_value()) {
                            canonical = registry->FindCompositePublicInputForTarget(*targetId, canonicalTargetInput);
                            if (!canonical.has_value())
                                canonical = registry->FindCompositePublicInputForInterfaceTarget(*targetId, canonicalTargetInput);
                        }
                        if (!canonical.has_value() &&
                            registry->CouldBeCompositeInternalInputShortGuid(canonicalTargetInput)) {
                            if (targetId.has_value())
                                canonical = registry->FindCompositePublicInputForInternalEdge(*targetId, canonicalTargetInput);
                            if (!canonical.has_value() && targetKey.entityA != 0U) {
                                canonical = registry->FindCompositePublicInputForRuntimeEdge(targetKey, canonicalTargetInput);
                                viaRuntime = canonical.has_value();
                            }
                            if (!canonical.has_value() && targetKey.entityA != 0U && targetKey.entityB != 0U) {
                                canonical = registry->FindCompositePublicInputForOwnerScopeRuntimeEdge(targetKey, canonicalTargetInput);
                                viaOwnerScope = canonical.has_value();
                            }
                        }
                    }
                }
                if (canonical.has_value() && canonical->noTypedData &&
                    registry->IsCompositePublicInputAllowed(
                        canonical->ownerCompositeInstance, canonical->publicInputShortGuid) &&
                    !CompositeLocalChainGuardActive(
                        canonical->ownerCompositeInstance, canonical->publicInputShortGuid)) {
                    gWs3CanonicalQueueMapped.fetch_add(1U, std::memory_order_relaxed);
                    // Preserve the exact WS2 ordering: execute the initiating
                    // player's retail interaction first. Network failure must
                    // never make a Client lose its local trigger/prompt.
                    struct RetailInvocation final {
                        QueueInputFn original{}; void* manager{}; void** sourceSharedRef{};
                        const std::uint32_t* inputShortGuid{}; void* targetDescriptor{};
                        void* callbackRecord{}; double delaySeconds{}; std::uint32_t retailTailArgument{};
                    } invocation{original, manager, sourceSharedRef, inputShortGuid, targetDescriptor,
                        callbackRecord, delaySeconds, retailTailArgument};
                    const auto invoke = [](void* opaque) noexcept {
                        auto& call = *static_cast<RetailInvocation*>(opaque);
                        CompositeMappedQueueScope mappedScope(true);
                        call.original(call.manager, call.sourceSharedRef, call.inputShortGuid,
                            call.targetDescriptor, call.callbackRecord, call.delaySeconds,
                            call.retailTailArgument);
                    };
                    world_actions::ContextTrace localCause{};
                    localCause.owner = canonical->ownerCompositeInstance;
                    localCause.target = canonical->ownerCompositeInstance;
                    localCause.input = canonical->publicInputShortGuid;
                    localCause.rootKind = static_cast<std::uint8_t>(
                        world_interactions::PublicRootKind::CanonicalCompositeInput);
                    localCause.causalSource = static_cast<std::uint8_t>(world_actions::CausalSource::Player);
                    localCause.actorKind = static_cast<std::uint8_t>(
                        world_interactions::PublicActorKind::SourcePlayer);
                    world_actions::InvokeCanonicalCompositeRetailScoped(
                        world_actions::Origin::LocalRoot, localCause, invoke, &invocation);
                    gWs3CanonicalQueueLocallyExecuted.fetch_add(1U, std::memory_order_relaxed);

                    gCompositePublicInputMapped.fetch_add(1U, std::memory_order_relaxed);
                    if (viaRuntime)
                        gCompositePublicInputRuntimeEdgeMapped.fetch_add(1U, std::memory_order_relaxed);
                    else if (viaOwnerScope)
                        gCompositePublicInputOwnerScopeRuntimeEdgeMapped.fetch_add(1U, std::memory_order_relaxed);

                    // Reuse the proven WS2 semantic fuse only AFTER local
                    // retail has run. Duplicate/remote-guard/rate rejection may
                    // suppress the network echo, but can never veto the local
                    // player's prompt/trigger. This also caps the newly reopened
                    // Client valve at the historical 256 submissions/second.
                    if (!AllowLocalCompositePublicInput(
                            canonical->ownerCompositeInstance,
                            canonical->publicInputShortGuid)) {
                        gWs3CanonicalQueueAdmissionSuppressed.fetch_add(
                            1U, std::memory_order_relaxed);
                        gWs3QueueLastGate.store(14U, std::memory_order_relaxed);
                        return;
                    }

                    // Only after retail has completed do we submit the stable
                    // canonical semantic to WS3 authority. No callback pointer,
                    // work-item or sender RuntimeKey is present in this ROOT.
                    world_actions::ContextTrace canonicalCause{};
                    const auto canonicalSubmission = world_actions::SubmitCanonicalCompositeRoot(
                        canonical->ownerCompositeInstance, canonical->publicInputShortGuid, canonicalCause);
                    if (canonicalSubmission == world_actions::CanonicalCompositeSubmission::Submitted) {
                        gCompositePublicInputSubmitted.fetch_add(1U, std::memory_order_relaxed);
                        RememberCompositeLocalChainGuard(
                            canonical->ownerCompositeInstance, canonical->publicInputShortGuid);
                        gWs3QueueLastGate.store(12U, std::memory_order_relaxed);
                    } else {
                        // Retail already ran. Fail open locally exactly like WS2;
                        // the missing send is diagnostic, never a gameplay veto.
                        gWs3QueueLastGate.store(13U, std::memory_order_relaxed);
                    }
                    return;
                }
            }
        }
        if (callbackRecord != nullptr) {
            gWs3QueueCallbackRejected.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(2U, std::memory_order_relaxed);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
            return;
        }

        SchedulerEdgeCapture edge{};
        if (!TryDescribeSchedulerEdge(
                *registry, sourceSharedRef, inputShortGuid, targetDescriptor, delaySeconds, edge)) {
            gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueDescribeRejected.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(3U, std::memory_order_relaxed);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
            return;
        }
        gWs3QueueLastSchedulerParameter.store(edge.schedulerParameter, std::memory_order_relaxed);
        gWs3QueueLastTargetInput.store(edge.targetInput, std::memory_order_relaxed);
        // This exact Event already belongs to the immediately preceding
        // certified output. Preserve retail without publishing a second root.
        if (world_actions::CurrentRootOwnsEvent(edge.target, edge.targetInput)) {
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
            return;
        }

        // A higher RemoteRoot is allowed to generate the edge naturally on the
        // receiver. Record it, but never publish it back to authority. The later
        // authoritative SchedulerEdge sidecar will consume this exact witness
        // and commit without replaying the effect twice.
        if (schedulerRemoteNatural) {
            gWs3QueueRemoteNatural.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(4U, std::memory_order_relaxed);
            world_actions::RememberRemoteNaturalSchedulerEdge(
                edge.source, edge.target, edge.schedulerParameter, edge.targetInput,
                edge.relativeDelay);
            if (remoteApply)
                gRemoteApplySuppressed.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
            return;
        }

        if (remoteApply) {
            gWs3QueueRemoteApplySuppressed.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(5U, std::memory_order_relaxed);
            gRemoteApplySuppressed.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
            return;
        }

        // Public Composite inputs remain the preferred high-level boundary only
        // for an unclaimed call. Once a StateTransition ROOT is already active,
        // lateral Event edges must remain independently observable; otherwise
        // the higher state carrier suppresses effects that it does not replay.
        if (!schedulerSidecar && retailTailArgument == 0U &&
            world_actions::DeferPublicInput(edge.targetShared, edge.targetInput)) {
            gWs3QueueDeferredComposite.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(6U, std::memory_order_relaxed);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
            return;
        }

        // Known State/StateTransition consumers remain owned by the existing
        // semantic carriers. Event and Unknown edges use the generic scheduler
        // valve when their runtime World identities are stable. Do not require
        // the compiler to have already classified every trigger consequence as
        // WorldEffect: that stricter gate is exactly what made WS3 narrower
        // than the proven v12.8 generic QueueInput boundary.
        const auto profile = registry->FindScheduledInputCausalProfile(edge.target, edge.targetInput);
        if (profile.has_value()) {
            const auto contract = profile->replicationContractKind;
            if (contract == WorldReplicationContractKind::State ||
                contract == WorldReplicationContractKind::StateTransition) {
                gWs3QueueStateOwned.fetch_add(1U, std::memory_order_relaxed);
                gWs3QueueLastGate.store(7U, std::memory_order_relaxed);
                original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                    delaySeconds, retailTailArgument);
                return;
            }
            if (contract == WorldReplicationContractKind::Event)
                gWs3QueueEventEligible.fetch_add(1U, std::memory_order_relaxed);
            else
                gWs3QueueUnknownEligible.fetch_add(1U, std::memory_order_relaxed);
        } else {
            gWs3QueueUnknownEligible.fetch_add(1U, std::memory_order_relaxed);
        }

        // SchedulerEdge is a native WS3 ROOT. Authority admission happens
        // before local retail, exactly like the higher Composite boundary. If
        // accepted on the Host, retail runs under LocalRoot so the WorkItems
        // constructed by FUN_009271C0 inherit ownership and cannot be emitted
        // again as lower EmittedOutput/ScheduledInput ROOTs. A non-Host player
        // proposal uses AwaitAuthority and therefore does not execute locally
        // until the authoritative echo arrives.
        world_actions::ContextTrace cause{};
        const auto submission = world_actions::SubmitSchedulerEdgeRoot(
            sourceSharedRef, edge.source, edge.target, edge.schedulerParameter, edge.targetInput,
            edge.relativeDelay, cause);
        if (submission == world_actions::SchedulerEdgeSubmission::RetailOnly) {
            gWs3QueueSubmitRetailOnly.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(8U, std::memory_order_relaxed);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
            return;
        }
        if (submission == world_actions::SchedulerEdgeSubmission::AwaitAuthority) {
            gWs3QueueSubmitAwaitAuthority.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(9U, std::memory_order_relaxed);
            return;
        }
        if (submission == world_actions::SchedulerEdgeSubmission::AuthoritySuppressed) {
            gWs3QueueSubmitAuthoritySuppressed.fetch_add(1U, std::memory_order_relaxed);
            gWs3QueueLastGate.store(10U, std::memory_order_relaxed);
            return;
        }
        gWs3QueueSubmitLocalRoot.fetch_add(1U, std::memory_order_relaxed);
        gWs3QueueLastGate.store(11U, std::memory_order_relaxed);

        struct RetailInvocation final {
            QueueInputFn original{};
            void* manager{};
            void** sourceSharedRef{};
            const std::uint32_t* inputShortGuid{};
            void* targetDescriptor{};
            void* callbackRecord{};
            double delaySeconds{};
            std::uint32_t retailTailArgument{};
        } invocation{original, manager, sourceSharedRef, inputShortGuid, targetDescriptor,
            callbackRecord, delaySeconds, retailTailArgument};
        const auto invoke = [](void* opaque) noexcept {
            auto& call = *static_cast<RetailInvocation*>(opaque);
            call.original(call.manager, call.sourceSharedRef, call.inputShortGuid,
                call.targetDescriptor, call.callbackRecord, call.delaySeconds,
                call.retailTailArgument);
        };
        world_actions::InvokeSchedulerEdgeRetailScoped(
            world_actions::Origin::LocalRoot, cause, invoke, &invocation);
        return;
    }

    // Fix3.19 one-second causal flight recorder. This executes only after the
    // exact witness has armed the window; callback-bearing edges remain retail
    // pass-through and are never replayed by this trace.
    CaptureFix319InputEdge(
        *registry, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
        delaySeconds, retailTailArgument, false, retailReturnAddress);

    // Fix3.15: synchronous observation only. Capture normalized QueueInput
    // identity while the exact witness LogicSwitch CLEAR remains on-stack.
    CaptureFix315DownstreamEdge(
        *registry, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
        retailReturnAddress);
    // Fix3.16: Fix3.15 proved there is no synchronous QueueInput while the
    // witness CLEAR is on-stack. For 250 ms after that exact CLEAR returns,
    // inspect a bounded number of QueueInput calls and retain only those whose
    // source or target RuntimeKey is the witness LogicSwitch.
    CaptureFix316PostClearEdge(
        *registry, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
        retailReturnAddress);

    const auto probeUntil = gCallbackClassificationUntilTickMs.load(std::memory_order_relaxed);
    std::uint64_t probeNow = 0U;
    bool probeActive = false;
    if (probeUntil != 0U) {
        probeNow = static_cast<std::uint64_t>(GetTickCount64());
        probeActive = probeNow <= probeUntil;
        if (!probeActive) gCallbackClassificationUntilTickMs.store(0U, std::memory_order_relaxed);
    }

    // Fix3.24 BuildFix04 must inspect the target descriptor before the legacy
    // callback safety pass-through. A public Composite invocation can inherit a
    // process-local callback from its parent work item; dropping every such edge
    // before reading targetInput made the canonical public boundary invisible.
    // The callback itself remains local-only and is never serialized.
    const bool fix320Active = callbackRecord != nullptr && Fix320ReplayActive();

    std::uint32_t schedulerParameter = 0U;
    std::uint32_t targetInput = 0U;
    float targetDelay = 0.0F;
    void* targetShared = nullptr;
    const auto* targetBytes = static_cast<const std::byte*>(targetDescriptor);
    if (inputShortGuid == nullptr || targetBytes == nullptr ||
        !ReadU32Safely(inputShortGuid, schedulerParameter) || schedulerParameter == 0U ||
        !ReadU32Safely(targetBytes, targetInput) || targetInput == 0U ||
        !ReadFloatSafely(targetBytes + 4U, targetDelay) ||
        !ReadPointerSafely(targetBytes + 8U, targetShared) || targetShared == nullptr) {
        gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    // A public-target edge is only a queued precursor. Its Method is captured
    // when retail actually executes it; sending both here and there duplicates
    // the ROOT. This checks the exact stable owner and retail definition only.
    if (world_actions::Installed() && callbackRecord == nullptr &&
        retailTailArgument == 0U && world_actions::DeferPublicInput(targetShared, targetInput)) {
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    // Fix3.24 BuildFix04: observe the PUBLIC Composite boundary directly.
    // CathodeLib/COMMANDS defines VariableEntity parameters as externally
    // addressable ports of an instanced Composite. At runtime that public event
    // is represented by QueueInput targeting the CompositeInstance with the
    // public target-input ShortGuid. This is a higher and more faithful boundary
    // than trying to infer the event later from an internal LogicSwitch CLEAR.
    //
    // Keep the lookup cheap on the hot callback path: first test only the
    // manifest's public-input ShortGuid set. Runtime-key extraction occurs only
    // for a possible public port. Typed public inputs and non-zero-delay edges
    // remain fail-closed. Explicit historical callback probes/Fix3.20 retain
    // their exact behavior when armed.
    const auto publicBoundaryDelay = static_cast<double>(targetDelay) + delaySeconds;
    std::optional<WorldCompositePublicInputDescriptor> compositePublicTarget{};
    if (!world_actions::Installed() && std::isfinite(publicBoundaryDelay) && publicBoundaryDelay == 0.0 &&
        registry->CouldBeCompositePublicInputShortGuid(targetInput)) {
        if (auto* targetExecution = ExecutionObjectFromShared(targetShared);
            targetExecution != nullptr) {
            const auto targetEntity = registry->IdentifyRuntimeEntity(targetExecution);
            if (targetEntity.has_value()) {
                compositePublicTarget = registry->FindCompositePublicInputForTarget(
                    *targetEntity, targetInput);
            }
        }
    }

    // Fix3.25: Cheat Engine + COMMANDS evidence shows a public Composite
    // invocation can be resolved into an internal CATHODE work item before this
    // scheduler boundary. The work item still carries the exact target
    // RuntimeKey and internal input ShortGuid. Mirror the manifest reverse map
    // by RuntimeKey and canonicalize that exact leaf back to its public
    // Composite input. This runs before the callback safety pass-through; the
    // callback remains process-local and is never serialized.
    std::optional<WorldCompositePublicInputDescriptor> compositeRuntimeEdge{};
    std::optional<WorldCompositePublicInputDescriptor> compositeOwnerScopeRuntimeEdge{};
    if (!world_actions::Installed() && !compositePublicTarget.has_value() && gCompositeMappedQueueDepth == 0U &&
        std::isfinite(publicBoundaryDelay) && publicBoundaryDelay == 0.0 &&
        registry->CouldBeCompositeInternalInputShortGuid(targetInput)) {
        if (auto* targetExecution = ExecutionObjectFromShared(targetShared);
            targetExecution != nullptr) {
            CathodeRuntimeKey targetRuntimeKey{};
            if (registry->TryExtractRuntimeKey(targetExecution, targetRuntimeKey)) {
                compositeRuntimeEdge = registry->FindCompositePublicInputForRuntimeEdge(
                    targetRuntimeKey, targetInput);
                if (!compositeRuntimeEdge.has_value()) {
                    // Fix3.26: runtime nested entities can preserve the owning
                    // CompositeInstance scope B even when the manifest's child
                    // entity uses a child-path-derived B. Keep A + input exact
                    // and accept only a unique payloadless owner-scope bridge.
                    compositeOwnerScopeRuntimeEdge =
                        registry->FindCompositePublicInputForOwnerScopeRuntimeEdge(
                            targetRuntimeKey, targetInput);
                }
            }
        }
    }

    const bool preserveHistoricalCallbackExperiment =
        callbackRecord != nullptr && (probeActive || fix320Active);
    const auto compositeCanonicalTarget = compositePublicTarget.has_value()
        ? compositePublicTarget
        : (compositeRuntimeEdge.has_value()
            ? compositeRuntimeEdge
            : compositeOwnerScopeRuntimeEdge);
    if (compositeCanonicalTarget.has_value() && !preserveHistoricalCallbackExperiment) {
        {
            CompositeMappedQueueScope mappedScope(true);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
        }
        gCompositePublicInputMapped.fetch_add(1U, std::memory_order_relaxed);
        if (compositeRuntimeEdge.has_value()) {
            gCompositePublicInputRuntimeEdgeMapped.fetch_add(1U, std::memory_order_relaxed);
        } else if (compositeOwnerScopeRuntimeEdge.has_value()) {
            gCompositePublicInputOwnerScopeRuntimeEdgeMapped.fetch_add(
                1U, std::memory_order_relaxed);
        }
        if (SubmitCompositePublicInput(*registry, *compositeCanonicalTarget)) {
            RememberCompositeLocalChainGuard(
                compositeCanonicalTarget->ownerCompositeInstance,
                compositeCanonicalTarget->publicInputShortGuid);
        }
        return;
    }

    // Fix3.6 keeps the Fix3.3 safety rollback as the permanent default for all
    // remaining callback-bearing InputEdges. BuildFix04 only exempts the exact
    // manifest-approved public Composite target above; broad callback replay is
    // still disabled.
    if (callbackRecord != nullptr && !probeActive && !fix320Active) {
        gCallbackEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    // Fix3.20 experimental narrow replay. Only the exact runtime-proven
    // World->World Character callback class is transportable. The shared helper
    // is also used during RemoteApply to remember naturally regenerated edges.
    if (callbackRecord != nullptr && Fix320ReplayActive()) {
        ObservedCharacterCallbackInputEdge edge{};
        WorldEntityId targetEntity{};
        if (TryDescribeFix320CharacterCallbackEdge(
                *registry, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailReturnAddress, edge, targetEntity)) {
            gFix320Observed.fetch_add(1U, std::memory_order_relaxed);
            edge.observationId = NextObservationId();
            WorldPayload payload{};
            if (EncodeObservedCharacterCallbackInputEdgePayload(edge, payload)) {
                gFix320Portable.fetch_add(1U, std::memory_order_relaxed);
                const bool nestedCharacterEdge = gLocalCharacterCallbackDepth != 0U;
                ++gLocalCharacterCallbackDepth;
                original(manager, sourceSharedRef, inputShortGuid, targetDescriptor,
                    callbackRecord, delaySeconds, retailTailArgument);
                --gLocalCharacterCallbackDepth;
                static_cast<void>(RememberCharacterCallbackMatch(
                    CharacterCallbackFingerprint(edge, targetEntity)));

                // A synchronously generated descendant of a local typed message
                // is not sent. The already-replicated parent message must
                // regenerate that graph naturally on the peer.
                if (gLocalReplicatedTypedDepth != 0U) {
                    gFix320ParentSuppressed.fetch_add(1U, std::memory_order_relaxed);
                    TouchVersion();
                    return;
                }
                // The same rule applies recursively to this narrow callback
                // family itself. If one exact callback edge synchronously
                // generates another, submit only the root. This preserves retail
                // ordering and lets the remote scheduler generate descendants in
                // the same order instead of receiving them during stack unwind.
                if (nestedCharacterEdge) {
                    gFix320NestedSuppressed.fetch_add(1U, std::memory_order_relaxed);
                    TouchVersion();
                    return;
                }

                const auto submissionIndex = gFix320WindowSubmissions.fetch_add(
                    1U, std::memory_order_acq_rel);
                if (submissionIndex >= kFix320MaximumSubmissionsPerWindow) {
                    gFix320LimitDrops.fetch_add(1U, std::memory_order_relaxed);
                    TouchVersion();
                    return;
                }
                if (!RememberLocalObservation(edge.observationId)) return;
                if (replication_core::SubmitLocalIntent(
                        targetEntity,
                        WorldMutationKind::Event,
                        kOperationObservedCharacterCallbackInputEdge,
                        std::span<const std::byte>(payload.bytes.data(), payload.size))) {
                    gFix320Submitted.fetch_add(1U, std::memory_order_relaxed);
                    TouchVersion();
                    return;
                }
                static_cast<void>(RemoveLocalObservation(edge.observationId, false));
                gIntentQueueRejected.fetch_add(1U, std::memory_order_relaxed);
                TouchVersion();
                return;
            }
        }
    }

    if (callbackRecord != nullptr && !probeActive) {
        gCallbackEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    std::uint32_t queueCallerRva = 0U;
    if (callbackRecord != nullptr) {
        // Fix3.6: the call-site remains the first pre-filter. The runtime pair is
        // never used to alter retail execution; it only decides whether a tiny
        // read-only 0x28-byte snapshot is copied into the bounded causal ring.
        // No WorldEntity resolution occurs on these thousands of caller matches.
        const auto queueReturnAddress = retailReturnAddress;
        const auto moduleBase = gMainModuleBase.load(std::memory_order_relaxed);
        if (moduleBase != 0U && queueReturnAddress >= moduleBase) {
            const auto relative = queueReturnAddress - moduleBase;
            if (relative <= std::numeric_limits<std::uint32_t>::max()) {
                queueCallerRva = static_cast<std::uint32_t>(relative);
            }
        }

        if (queueCallerRva != kCallbackAbiProbeCallerRva) {
            gCallbackEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
            return;
        }

        gCallbackReferenceFamilyObserved.fetch_add(1U, std::memory_order_relaxed);
        gCallbackAbiProbeCallerMatches.fetch_add(1U, std::memory_order_relaxed);

        SchedulerCausalSnapshot causal{};
        causal.tickMs = probeNow;
        causal.schedulerParameterShortGuid = schedulerParameter;
        causal.targetInputShortGuid = targetInput;
        causal.callbackRecordPresent = true;
        causal.queueCallerRva = queueCallerRva;

        // Fix3.8: copy the 0x28 bytes for the observed temporal pair first, then
        // ask retail for CATHODE runtime keys only while shared-ref ownership is
        // valid. Flags are observed classifiers, not gameplay rules. Extraction
        // is bounded to 1024 callback-shaped pair matches per armed window.
        if (schedulerParameter == kCallbackTemporalProbeScheduler &&
            targetInput == kCallbackTemporalProbeTargetInput) {
            gCallbackAbiProbePairMatches.fetch_add(1U, std::memory_order_relaxed);
            std::array<std::uint32_t, kCallbackRawWordCount> words{};
            if (CopyBytesSafely(
                    callbackRecord,
                    reinterpret_cast<std::byte*>(words.data()),
                    sizeof(words))) {
                causal.callbackRawWords = words;
                causal.callbackRawReadable = true;

                const bool callbackShaped =
                    words[0] == kCallbackWorkItemInputShortGuid &&
                    words[3] == 0U && words[4] == 0U && words[5] == 0U;
                if (callbackShaped) {
                    const auto identityAttempt =
                        gCallbackAbiProbeIdentityAttempts.fetch_add(1U, std::memory_order_relaxed);
                    if (identityAttempt < kCallbackIdentityProbeResolveLimit) {
                        gCallbackAbiProbeRuntimeKeyAttempts.fetch_add(1U, std::memory_order_relaxed);

                        void* sourceShared = nullptr;
                        if (ReadPointerSafely(sourceSharedRef, sourceShared) && sourceShared != nullptr) {
                            if (auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
                                sourceExecution != nullptr &&
                                registry->TryExtractRuntimeKey(sourceExecution, causal.sourceRuntimeKey)) {
                                causal.sourceRuntimeKeyValid = true;
                                gCallbackAbiProbeSourceKeyResolved.fetch_add(1U, std::memory_order_relaxed);
                                const auto sourceId = registry->FindRuntimeWorldEntityId(causal.sourceRuntimeKey);
                                if (sourceId.has_value()) causal.source = *sourceId;
                            }
                        }

                        if (auto* targetExecution = ExecutionObjectFromShared(targetShared);
                            targetExecution != nullptr &&
                            registry->TryExtractRuntimeKey(targetExecution, causal.targetRuntimeKey)) {
                            causal.targetRuntimeKeyValid = true;
                            gCallbackAbiProbeTargetKeyResolved.fetch_add(1U, std::memory_order_relaxed);
                            const auto targetId = registry->FindRuntimeWorldEntityId(causal.targetRuntimeKey);
                            if (targetId.has_value()) causal.target = *targetId;
                        }

                        const auto callbackSharedAddress = static_cast<std::uintptr_t>(words[2]);
                        if (callbackSharedAddress != 0U) {
                            auto* callbackExecution = ExecutionObjectFromShared(
                                reinterpret_cast<void*>(callbackSharedAddress));
                            if (callbackExecution != nullptr) {
                                causal.callbackExecutionPresent = true;
                                gCallbackAbiProbeCallbackObjectPresent.fetch_add(1U, std::memory_order_relaxed);
                                if (registry->TryExtractRuntimeKey(callbackExecution, causal.callbackRuntimeKey)) {
                                    causal.callbackRuntimeKeyValid = true;
                                    gCallbackAbiProbeCallbackKeyResolved.fetch_add(1U, std::memory_order_relaxed);
                                }
                                void* callbackVtable = nullptr;
                                if (ReadPointerSafely(callbackExecution, callbackVtable) &&
                                    callbackVtable != nullptr) {
                                    const auto vtableAddress = reinterpret_cast<std::uintptr_t>(callbackVtable);
                                    if (moduleBase != 0U && vtableAddress >= moduleBase) {
                                        const auto relative = vtableAddress - moduleBase;
                                        if (relative <= std::numeric_limits<std::uint32_t>::max()) {
                                            causal.callbackExecutionVtableRva =
                                                static_cast<std::uint32_t>(relative);
                                            causal.callbackExecutionReadable = true;
                                        }
                                    }
                                }
                                if (causal.callbackRuntimeKeyValid) {
                                    const auto callbackId =
                                        registry->FindRuntimeWorldEntityId(causal.callbackRuntimeKey);
                                    if (callbackId.has_value()) {
                                        causal.callbackEntity = *callbackId;
                                        causal.callbackWorldEntity = true;
                                        causal.callbackSameAsSource =
                                            causal.source.Valid() && *callbackId == causal.source;
                                        causal.callbackSameAsTarget =
                                            causal.target.Valid() && *callbackId == causal.target;
                                    }
                                }
                            }
                        }
                    } else {
                        gCallbackAbiProbeIdentityLimitDrops.fetch_add(1U, std::memory_order_relaxed);
                    }
                }
            } else {
                gCallbackAbiProbeRawReadFailures.fetch_add(1U, std::memory_order_relaxed);
            }
        }

        PublishSchedulerCausalRecord(causal);
        SchedulerCausalScope causalScope(causal, callbackRecord);
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);

        // Read-only probe only. Callback replay remains safety-disabled.
        gCallbackEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    void* sourceShared = nullptr;
    if (!ReadPointerSafely(sourceSharedRef, sourceShared) || sourceShared == nullptr) {
        gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }
    auto* sourceExecution = ExecutionObjectFromShared(sourceShared);
    auto* targetExecution = ExecutionObjectFromShared(targetShared);
    if (sourceExecution == nullptr || targetExecution == nullptr) {
        gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    const auto sourceId = IdentifyRuntimeWorldEntityCached(*registry, sourceExecution);
    const auto targetId = IdentifyRuntimeWorldEntityCached(*registry, targetExecution);
    if (!sourceId.has_value() || !targetId.has_value()) {
        if (!sourceId.has_value()) gUnresolvedSource.fetch_add(1U, std::memory_order_relaxed);
        if (!targetId.has_value()) gUnresolvedTarget.fetch_add(1U, std::memory_order_relaxed);
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }
    gRelevantSourceCalls.fetch_add(1U, std::memory_order_relaxed);

    const auto relativeDelay = static_cast<double>(targetDelay) + delaySeconds;
    if (!std::isfinite(relativeDelay) || relativeDelay < 0.0 || relativeDelay > 600.0) {
        gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
        original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
            delaySeconds, retailTailArgument);
        return;
    }

    // Fix3.24 canonicalization is deliberately payloadless and delay-free.
    // Non-zero-delay edges retain the pre-existing v12.8 path until public
    // input delay semantics are independently proven.
    const auto compositePublicInput = !world_actions::Installed() && relativeDelay == 0.0
        ? registry->FindCompositePublicInputForDirectEdge(
            *sourceId, schedulerParameter, *targetId, targetInput)
        : std::nullopt;

    SchedulerCausalSnapshot causal{};
    causal.tickMs = probeNow;
    causal.source = *sourceId;
    causal.target = *targetId;
    causal.schedulerParameterShortGuid = schedulerParameter;
    causal.targetInputShortGuid = targetInput;
    causal.callbackRecordPresent = false;
    causal.queueCallerRva = 0U;

    {
        CompositeMappedQueueScope mappedScope(compositePublicInput.has_value());
        if (probeActive) {
            PublishSchedulerCausalRecord(causal);
            SchedulerCausalScope causalScope(causal);
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
        } else {
            original(manager, sourceSharedRef, inputShortGuid, targetDescriptor, callbackRecord,
                delaySeconds, retailTailArgument);
        }
    }

    if (compositePublicInput.has_value()) {
        gCompositePublicInputMapped.fetch_add(1U, std::memory_order_relaxed);
        if (SubmitCompositePublicInput(*registry, *compositePublicInput)) {
            // A public QueueInput can generate a downstream LogicSwitch CLEAR
            // after this trampoline returns. Suppress only that fallback copy;
            // independent direct public QueueInputs are still admitted.
            RememberCompositeLocalChainGuard(
                compositePublicInput->ownerCompositeInstance,
                compositePublicInput->publicInputShortGuid);
        }
        return;
    }

    ObservedInputEdge edge{};
    edge.sourceCategory = SourceCategory::GenericWorld;
    edge.schedulerParameterShortGuid = schedulerParameter;
    edge.targetInputShortGuid = targetInput;
    edge.observationId = NextObservationId();
    edge.sourceEntity = *sourceId;
    edge.relativeDelaySeconds = static_cast<float>(relativeDelay);
    WorldPayload payload{};
    if (!EncodeObservedInputEdgePayload(edge, payload)) {
        gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    if (!RememberLocalObservation(edge.observationId)) return;
    if (replication_core::SubmitLocalIntent(
            *targetId,
            WorldMutationKind::Event,
            kOperationObservedInputEdge,
            std::span<const std::byte>(payload.bytes.data(), payload.size))) {
        gSubmittedIntents.fetch_add(1U, std::memory_order_relaxed);
        TouchVersion();
        return;
    }
    static_cast<void>(RemoveLocalObservation(edge.observationId, false));
    gIntentQueueRejected.fetch_add(1U, std::memory_order_relaxed);
    TouchVersion();
}

[[nodiscard]] bool TypedInteractionPairCandidate(
    const std::uint32_t messageType) noexcept {
    return messageType == 0x00000043U || messageType == 0x00000044U;
}

void ObserveTypedInteractionPairCandidate(
    WorldEntityRegistry& registry,
    const WorldEntityId& source,
    const WorldEntityId& target,
    const std::uint32_t messageType,
    const std::uint32_t callerRva) noexcept {
    if (!TypedInteractionPairCandidate(messageType)) return;
    gWs3TypedInteractionPairSeen.fetch_add(1U, std::memory_order_relaxed);
    if (messageType == 0x00000043U) {
        gWs3TypedInteractionPair43.fetch_add(1U, std::memory_order_relaxed);
        gWs3TypedInteractionPair43LastCallerRva.store(callerRva, std::memory_order_relaxed);
    } else {
        gWs3TypedInteractionPair44.fetch_add(1U, std::memory_order_relaxed);
        gWs3TypedInteractionPair44LastCallerRva.store(callerRva, std::memory_order_relaxed);
    }
    gWs3TypedInteractionPairLastType.store(messageType, std::memory_order_relaxed);
    gWs3TypedInteractionPairLastCallerRva.store(callerRva, std::memory_order_relaxed);
    gWs3TypedInteractionPairLastSourceHigh.store(source.high, std::memory_order_relaxed);
    gWs3TypedInteractionPairLastSourceLow.store(source.low, std::memory_order_relaxed);
    gWs3TypedInteractionPairLastTargetHigh.store(target.high, std::memory_order_relaxed);
    gWs3TypedInteractionPairLastTargetLow.store(target.low, std::memory_order_relaxed);
    gWs3TypedInteractionPairLastSourcePrimitive.store(
        registry.FindWorldEntityPrimitiveType(source).value_or(0U),
        std::memory_order_relaxed);
    gWs3TypedInteractionPairLastTargetPrimitive.store(
        registry.FindWorldEntityPrimitiveType(target).value_or(0U),
        std::memory_order_relaxed);
    gWs3TypedInteractionPairLastSourceDomain.store(
        static_cast<std::uint32_t>(registry.FindWorldEntityDomain(source).value_or(WorldDomain::Unknown)),
        std::memory_order_relaxed);
    gWs3TypedInteractionPairLastTargetDomain.store(
        static_cast<std::uint32_t>(registry.FindWorldEntityDomain(target).value_or(WorldDomain::Unknown)),
        std::memory_order_relaxed);
}

[[nodiscard]] constexpr bool ReceiverLocalInteractionControlMessage(
    const std::uint32_t messageType) noexcept {
    // Patch 5.5 / SALVAGEMODE1. These retail message ids belong to the
    // task/script interaction-control family. Replaying them on another peer
    // can make that peer enter the sender's interaction (observed as the Client
    // being pulled/teleported to the Host's terminal). They are control-plane
    // occurrences, not shared World consequences. Keep them receiver-local and
    // allow the resulting stable StateTransition/TypedMessage consequences to
    // replicate normally.
    switch (messageType) {
    case 0x00100001U:
    case 0x00100003U:
    case 0x00100004U:
    case 0x00100049U:
    // Paired local Character interaction handshake. The actual Survival
    // generator trace (both directions of a World->World pair) emitted these
    // as 0x43 / 0x44. They are not authored mission-completion decisions:
    // replaying them asks the receiver's local Player to perform the sender's
    // interaction, including snap-to-interaction/teleport behavior.
    // Keep the CATHODE controls retail-local on each process. MissionSync
    // owns the downstream authored progression decision.
    case 0x00000043U:
    case 0x00000044U:
    // ItemSync owns world pickups. The pickup manager's owner messages
    // (0x1E spawned, 0x1F collected) and the pickup use/range messages
    // (0x34 use, 0x36/0x37 in/out of range) describe one peer's local
    // spawn and collection. Replaying them elsewhere would mark the remote
    // owner collected without removing its pickup; ItemSync removes the
    // pickup itself through the pickup manager.
    case 0x0020001EU:
    case 0x0020001FU:
    case 0x00200034U:
    case 0x00200036U:
    case 0x00200037U:
    // Trigger-volume occupancy (Box/PlayerTriggerBox/Sphere -> Character):
    // 0x01000000 entered, 0x02000000 exited. The player Character has the same
    // WorldEntityId on every peer, so a replay moved the receiver's OWN player
    // in/out of the sender's volume (ChallengeMap11 lift climb-out: the Host's
    // exit fired the Client's on_exited -> Use.disable_use and removed its
    // prompt). Each peer's physics produces its own occupancy.
    case 0x01000000U:
    case 0x02000000U:
        return true;
    default:
        return false;
    }
}

// Player-input primitives are per-process HUD/pad state, independent of the
// message id. The Survival generator's ButtonMashPrompt sends itself
// 0x200021/0x20002D/0x200001 per mash cycle; replaying those made the Client's
// own Player enter the Host's generator interaction (snap/teleport). Each peer
// keeps its prompt local; MissionSync replicates the resulting milestone.
[[nodiscard]] constexpr bool ReceiverLocalPlayerInputPrimitive(
    const std::uint32_t primitiveType) noexcept {
    return world_actions::ReceiverLocalPlayerControlPrimitive(primitiveType);
}

static_assert(ReceiverLocalPlayerInputPrimitive(0x1728F297U));
static_assert(!ReceiverLocalPlayerInputPrimitive(0xB9B4A894U)); // ModelReference

// Guard the generic CATHODE interaction-control wire contract in native
// builds. Mission/environment messages remain eligible for WorldSync.
static_assert(ReceiverLocalInteractionControlMessage(0x00000043U));
static_assert(ReceiverLocalInteractionControlMessage(0x00000044U));
static_assert(ReceiverLocalInteractionControlMessage(0x00100001U));
static_assert(!ReceiverLocalInteractionControlMessage(0x00000041U));
static_assert(!ReceiverLocalInteractionControlMessage(0x00000053U));
static_assert(ReceiverLocalInteractionControlMessage(0x01000000U));
static_assert(ReceiverLocalInteractionControlMessage(0x02000000U));

void __fastcall QueueTypedMessageDetour(
    void* manager,
    void*,
    const CathodeRuntimeKey* sourceKeyAddress,
    const CathodeRuntimeKey* targetKeyAddress,
    const std::uint32_t messageType,
    const std::size_t payloadSize,
    void* payloadData,
    const double delaySeconds) noexcept {
    const auto original = reinterpret_cast<QueueTypedMessageFn>(
        gQueueTypedMessageTrampoline.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (!runtime_features::Enabled(runtime_features::Feature::WorldSyncHooks)) {
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    const auto retailReturnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__) || defined(__clang__)
    const auto retailReturnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#else
    const std::uintptr_t retailReturnAddress = 0U;
#endif
    const auto retailCallerRva = CallerRvaFromReturnAddress(
        reinterpret_cast<const void*>(retailReturnAddress));
    gObservedTypedMessages.fetch_add(1U, std::memory_order_relaxed);

    const bool ws3Installed = world_actions::Installed();
    if (ws3Installed) {
        gWs3TypedSeen.fetch_add(1U, std::memory_order_relaxed);
        gWs3TypedLastMessageType.store(messageType, std::memory_order_relaxed);
        const auto boundedPayload = static_cast<std::uint32_t>((std::min<std::size_t>)(
            payloadSize, static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
        gWs3TypedLastPayloadBytes.store(boundedPayload, std::memory_order_relaxed);
        gWs3TypedLastCallerRva.store(
            retailCallerRva, std::memory_order_relaxed);

        auto* ws3Registry = gRegistry.load(std::memory_order_acquire);
        CathodeRuntimeKey traceSource{};
        CathodeRuntimeKey traceTarget{};
        if (ws3Registry != nullptr && ws3Registry->RuntimeCaptureReady() &&
            ReadRuntimeKeySafely(sourceKeyAddress, traceSource) &&
            ReadRuntimeKeySafely(targetKeyAddress, traceTarget)) {
            std::uint32_t worldMask = 0U;
            if (ws3Registry->FindRuntimeWorldEntityId(traceSource).has_value()) worldMask |= 0x1U;
            if (ws3Registry->FindRuntimeWorldEntityId(traceTarget).has_value()) worldMask |= 0x2U;
            gWs3TypedLastWorldMask.store(worldMask, std::memory_order_relaxed);
            if (worldMask == 0x3U)
                gWs3TypedWorldPairResolved.fetch_add(1U, std::memory_order_relaxed);
            else
                gWs3TypedWorldPairUnresolved.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    if (world_actions::SuppressDescendantCapture()) {
        if (ws3Installed)
            gWs3TypedDescendantSuppressed.fetch_add(1U, std::memory_order_relaxed);
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }

    const bool remoteApply = replication_core::RemoteApplyActive();
    if (!gInitialized.load(std::memory_order_acquire)) {
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }

    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (remoteApply) {
        if (registry != nullptr && registry->RuntimeCaptureReady()) {
            CathodeRuntimeKey traceSource{};
            CathodeRuntimeKey traceTarget{};
            if (ReadRuntimeKeySafely(sourceKeyAddress, traceSource) &&
                ReadRuntimeKeySafely(targetKeyAddress, traceTarget)) {
                CaptureFix319TypedMessage(
                    *registry, traceSource, traceTarget, messageType, payloadSize,
                    payloadData, delaySeconds, true, retailReturnAddress);
            } else if (Fix319TraceActive(static_cast<std::uint64_t>(GetTickCount64()))) {
                gFix319ReadFailures.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        gTypedRemoteApplySuppressed.fetch_add(1U, std::memory_order_acq_rel);
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }
    if (registry == nullptr) {
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }
    if (!registry->RuntimeCaptureReady()) {
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }

    // Patch 5.4 absorbs the second generic WS2 primitive into the WS3 ROOT
    // pipeline. This path is deliberately limited to the runtime-proven safe
    // World->World class. Sender RuntimeKeys, callbacks and pointers never cross
    // IPC; the Client remaps both stable WorldEntityIds to its own RuntimeKeys.
    if (ws3Installed) {
        if (payloadSize > kCompositePublicRootMaximumContextPayloadBytes ||
            (payloadSize != 0U && payloadData == nullptr) ||
            !std::isfinite(delaySeconds) || delaySeconds < 0.0 || delaySeconds > 600.0) {
            gWs3TypedRootRetailOnly.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceKeyAddress, targetKeyAddress, messageType,
                payloadSize, payloadData, delaySeconds);
            return;
        }

        CathodeRuntimeKey sourceKey{};
        CathodeRuntimeKey targetKey{};
        if (!ReadRuntimeKeySafely(sourceKeyAddress, sourceKey) ||
            !ReadRuntimeKeySafely(targetKeyAddress, targetKey)) {
            gWs3TypedRootRetailOnly.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceKeyAddress, targetKeyAddress, messageType,
                payloadSize, payloadData, delaySeconds);
            return;
        }
        const auto sourceId = registry->FindRuntimeWorldEntityId(sourceKey);
        const auto targetId = registry->FindRuntimeWorldEntityId(targetKey);
        if (!sourceId.has_value() || !targetId.has_value()) {
            // Preserve the Fix3.9 BuildFix01 safety boundary: typed traffic with
            // any non-World endpoint remains local/observation-only.
            gWs3TypedRootRetailOnly.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceKeyAddress, targetKeyAddress, messageType,
                payloadSize, payloadData, delaySeconds);
            return;
        }

        ObserveTypedInteractionPairCandidate(
            *registry, *sourceId, *targetId, messageType, retailCallerRva);

        if (ReceiverLocalInteractionControlMessage(messageType) ||
            ReceiverLocalPlayerInputPrimitive(
                registry->FindWorldEntityPrimitiveType(*sourceId).value_or(0U)) ||
            ReceiverLocalPlayerInputPrimitive(
                registry->FindWorldEntityPrimitiveType(*targetId).value_or(0U))) {
            gWs3TypedReceiverLocalSuppressed.fetch_add(1U, std::memory_order_relaxed);
            gWs3TypedLastReceiverLocalSuppressedType.store(
                messageType, std::memory_order_relaxed);
            original(manager, sourceKeyAddress, targetKeyAddress, messageType,
                payloadSize, payloadData, delaySeconds);
            return;
        }

        std::array<std::byte, kCompositePublicRootMaximumContextPayloadBytes> portablePayload{};
        if (payloadSize != 0U && !CopyBytesSafely(
                payloadData, portablePayload.data(), payloadSize)) {
            gWs3TypedRootRetailOnly.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceKeyAddress, targetKeyAddress, messageType,
                payloadSize, payloadData, delaySeconds);
            return;
        }
        if (!cathode_primitives::PrepareTypedMessageForNetwork(messageType,
                std::span<std::byte>(portablePayload.data(), payloadSize))) {
            gWs3TypedRootRetailOnly.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceKeyAddress, targetKeyAddress, messageType,
                payloadSize, payloadData, delaySeconds);
            return;
        }

        world_actions::ContextTrace cause{};
        const auto submission = world_actions::SubmitTypedMessageRoot(
            *sourceId, *targetId, messageType,
            std::span<const std::byte>(portablePayload.data(), payloadSize),
            delaySeconds, cause);
        if (TypedInteractionPairCandidate(messageType)) {
            const auto causalSource = static_cast<std::uint32_t>(cause.causalSource);
            gWs3TypedInteractionPairLastCausalSource.store(
                causalSource, std::memory_order_relaxed);
            if (messageType == 0x00000043U)
                gWs3TypedInteractionPair43LastCausalSource.store(
                    causalSource, std::memory_order_relaxed);
            else
                gWs3TypedInteractionPair44LastCausalSource.store(
                    causalSource, std::memory_order_relaxed);
        }
        if (submission == world_actions::TypedMessageSubmission::RetailOnly) {
            gWs3TypedRootRetailOnly.fetch_add(1U, std::memory_order_relaxed);
            original(manager, sourceKeyAddress, targetKeyAddress, messageType,
                payloadSize, payloadData, delaySeconds);
            return;
        }
        if (submission == world_actions::TypedMessageSubmission::AwaitAuthority) {
            gWs3TypedRootAwaitAuthority.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        if (submission == world_actions::TypedMessageSubmission::AuthoritySuppressed) {
            gWs3TypedRootAuthoritySuppressed.fetch_add(1U, std::memory_order_relaxed);
            return;
        }

        struct TypedLocalReplay final {
            QueueTypedMessageFn original{};
            void* manager{};
            const CathodeRuntimeKey* source{};
            const CathodeRuntimeKey* target{};
            std::uint32_t messageType{};
            std::size_t payloadSize{};
            void* payloadData{};
            double delay{};
        } replay{original, manager, sourceKeyAddress, targetKeyAddress,
            messageType, payloadSize, payloadData, delaySeconds};
        const auto invoke = [](void* opaque) noexcept {
            auto& call = *static_cast<TypedLocalReplay*>(opaque);
            call.original(call.manager, call.source, call.target, call.messageType,
                call.payloadSize, call.payloadData, call.delay);
        };
        gWs3TypedRootLocal.fetch_add(1U, std::memory_order_relaxed);
        world_actions::InvokeTypedMessageRetailScoped(
            world_actions::Origin::LocalRoot, cause, invoke, &replay);
        return;
    }

    bool typedOriginCandidate = false;
    std::uint64_t typedOriginNow = 0U;
    const auto typedOriginUntil = gTypedMessageOriginProbeUntilTickMs.load(
        std::memory_order_relaxed);
    if (typedOriginUntil != 0U) {
        typedOriginNow = static_cast<std::uint64_t>(GetTickCount64());
        if (typedOriginNow > typedOriginUntil) {
            gTypedMessageOriginProbeUntilTickMs.store(0U, std::memory_order_relaxed);
        } else if ((messageType | kFix313RetailInjectedCallbackFlag) ==
                kFix313CallbackConstructorFlags &&
            payloadSize == 0U) {
            typedOriginCandidate = true;
            gTypedMessageOriginCandidates.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    if (payloadSize > kObservedTypedMessageMaximumDataBytes) {
        gOversizedTypedMessagesSkipped.fetch_add(1U, std::memory_order_acq_rel);
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }
    if ((payloadSize != 0U && payloadData == nullptr) ||
        !std::isfinite(delaySeconds) || delaySeconds < 0.0 || delaySeconds > 600.0) {
        gInvalidTypedMessagesSkipped.fetch_add(1U, std::memory_order_acq_rel);
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }

    CathodeRuntimeKey sourceKey{};
    CathodeRuntimeKey targetKey{};
    if (!ReadRuntimeKeySafely(sourceKeyAddress, sourceKey) ||
        !ReadRuntimeKeySafely(targetKeyAddress, targetKey)) {
        if (typedOriginCandidate) {
            gTypedMessageOriginReadFailures.fetch_add(1U, std::memory_order_relaxed);
        }
        gInvalidTypedMessagesSkipped.fetch_add(1U, std::memory_order_acq_rel);
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }


    const bool typedOriginExact = typedOriginCandidate &&
        targetKey.entityA == kFix313TargetCharacterKeyA &&
        targetKey.entityB == kFix313TargetCharacterKeyB;
    if (typedOriginExact) {
        StartFix319Trace(kFix319AnchorLocalWitness);
    }
    CaptureFix319TypedMessage(
        *registry, sourceKey, targetKey, messageType, payloadSize, payloadData,
        delaySeconds, false, retailReturnAddress);
    if (typedOriginExact) {
        gTypedMessageOriginExactMatches.fetch_add(1U, std::memory_order_relaxed);
        gTypedMessageOriginMessageType.store(messageType, std::memory_order_relaxed);
        gTypedMessageOriginPayloadBytes.store(
            static_cast<std::uint32_t>(payloadSize), std::memory_order_relaxed);
        gTypedMessageOriginSourceA.store(sourceKey.entityA, std::memory_order_relaxed);
        gTypedMessageOriginSourceB.store(sourceKey.entityB, std::memory_order_relaxed);
        gTypedMessageOriginTargetA.store(targetKey.entityA, std::memory_order_relaxed);
        gTypedMessageOriginTargetB.store(targetKey.entityB, std::memory_order_relaxed);
        gTypedMessageOriginDelayBits.store(
            std::bit_cast<std::uint64_t>(delaySeconds), std::memory_order_relaxed);
        gTypedMessageOriginThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
        gTypedMessageOriginTickMs.store(typedOriginNow, std::memory_order_relaxed);

        std::uint32_t callSiteRva = 0U;
        if (retailReturnAddress != 0U &&
            VerifyQueueTypedMessageDirectCallSite(retailReturnAddress, callSiteRva)) {
            gTypedMessageOriginCallVerified.fetch_add(1U, std::memory_order_relaxed);
            auto expectedFirst = 0U;
            static_cast<void>(gTypedMessageOriginFirstCallSiteRva.compare_exchange_strong(
                expectedFirst, callSiteRva,
                std::memory_order_relaxed, std::memory_order_relaxed));
            const auto previous = gTypedMessageOriginLastCallSiteRva.exchange(
                callSiteRva, std::memory_order_relaxed);
            if (previous != 0U && previous != callSiteRva) {
                gTypedMessageOriginCallSiteVaried.store(1U, std::memory_order_relaxed);
            }
        }
    }

    const auto sourceId = registry->FindRuntimeWorldEntityId(sourceKey);
    const auto targetId = registry->FindRuntimeWorldEntityId(targetKey);
    if (typedOriginExact) {
        std::uint32_t worldMask = 0U;
        if (sourceId.has_value()) worldMask |= 0x1U;
        if (targetId.has_value()) worldMask |= 0x2U;
        gTypedMessageOriginWorldMask.store(worldMask, std::memory_order_relaxed);
        TouchVersion();
    }
    if (!sourceId.has_value()) {
        gUnresolvedTypedSource.fetch_add(1U, std::memory_order_acq_rel);
    }
    if (!targetId.has_value()) {
        gUnresolvedTypedTarget.fetch_add(1U, std::memory_order_acq_rel);
    }

    if (!sourceId.has_value() || !targetId.has_value()) {
        // Fix3.9 BuildFix01 safety boundary. The retail blob is deep-copied, but
        // portability of the blob semantics and asynchronous side effects is not
        // proven for CompositeInstance/non-World endpoints. Keep this widened
        // runtime-key class observation-only until its exact message family is
        // classified. Never send it to another process.
        std::uint32_t missingMask = 0U;
        if (!sourceId.has_value()) missingMask |= 0x1U;
        if (!targetId.has_value()) missingMask |= 0x2U;
        gTypedRuntimeKeySafetyBlocked.fetch_add(1U, std::memory_order_acq_rel);
        gTypedBlockedMessageType.store(messageType, std::memory_order_release);
        gTypedBlockedPayloadBytes.store(
            static_cast<std::uint32_t>(payloadSize), std::memory_order_release);
        gTypedBlockedSourceA.store(sourceKey.entityA, std::memory_order_release);
        gTypedBlockedSourceB.store(sourceKey.entityB, std::memory_order_release);
        gTypedBlockedTargetA.store(targetKey.entityA, std::memory_order_release);
        gTypedBlockedTargetB.store(targetKey.entityB, std::memory_order_release);
        gTypedBlockedMissingMask.store(missingMask, std::memory_order_release);
        if (missingMask == 0x3U) {
            gTypedRuntimeKeyNoAnchorSkipped.fetch_add(1U, std::memory_order_acq_rel);
        }
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }

    // Preserve the stable v12.8 authority anchor: the target must be a manifested
    // WorldEntity. Normal traffic remains on the v2 typed payload; Fix3.14 may
    // add stable IDs only to the bounded witness message while its probe is armed.
    // No widened non-World endpoint is remotely replayed in this safety build.
    const WorldEntityId anchor = *targetId;
    gTypedRuntimeKeyAnchoredByTarget.fetch_add(1U, std::memory_order_acq_rel);
    gRelevantTypedMessages.fetch_add(1U, std::memory_order_acq_rel);

    ObservedTypedMessage message{};
    message.messageType = messageType;
    message.observationId = NextObservationId();
    message.sourceRuntimeKey = sourceKey;
    message.targetRuntimeKey = targetKey;
    message.sourceWorldEntity = *sourceId;
    message.targetWorldEntity = *targetId;
    const auto fix314Until =
        gTypedRemoteIdentityProbeUntilTickMs.load(std::memory_order_relaxed);
    const auto fix314Now = static_cast<std::uint64_t>(GetTickCount64());
    message.stableWorldIdentityIncluded = fix314Until != 0U &&
        fix314Now <= fix314Until && messageType == kFix314WitnessMessageType &&
        payloadSize == 0U;
    message.relativeDelaySeconds = delaySeconds;
    const auto typedHeaderBytes = message.stableWorldIdentityIncluded
        ? kObservedTypedMessageIdentityHeaderBytes
        : kObservedTypedMessageHeaderBytes;
    WorldPayload payload{};
    if (!PrepareObservedTypedMessagePayload(message, payloadSize, payload) ||
        !CopyBytesSafely(
            payloadData,
            payload.bytes.data() + typedHeaderBytes,
            payloadSize) ||
        !cathode_primitives::PrepareTypedMessageForNetwork(messageType,
            std::span<std::byte>(payload.bytes.data() + typedHeaderBytes, payloadSize))) {
        gInvalidTypedMessagesSkipped.fetch_add(1U, std::memory_order_acq_rel);
        original(manager, sourceKeyAddress, targetKeyAddress, messageType,
            payloadSize, payloadData, delaySeconds);
        return;
    }

    // Preserve retail semantics first. Fix3.20 marks this scope so exact
    // synchronous Character callback descendants are not redundantly sent
    // before their already-replicated parent typed message.
    ++gLocalReplicatedTypedDepth;
    original(manager, sourceKeyAddress, targetKeyAddress, messageType,
        payloadSize, payloadData, delaySeconds);
    --gLocalReplicatedTypedDepth;

    const auto data = std::span<const std::byte>(
        payload.bytes.data() + typedHeaderBytes, payloadSize);
    const auto fingerprint = TypedFingerprint(message, data);
    // Typed-message echo suppression is content-based rather than observation-ID
    // based. The runtime keys are the semantic identity; process-local shared
    // refs are never retained or serialized.
    static_cast<void>(RememberTypedMatch(fingerprint));

    if (replication_core::SubmitLocalIntent(
            anchor,
            WorldMutationKind::Event,
            kOperationObservedTypedMessage,
            std::span<const std::byte>(payload.bytes.data(), payload.size))) {
        gSubmittedTypedMessages.fetch_add(1U, std::memory_order_acq_rel);
        gTypedPayloadBytes.fetch_add(payloadSize, std::memory_order_acq_rel);
        TouchVersion();
        return;
    }
    // Keep the content fingerprint briefly when network submission fails: an
    // equivalent authoritative copy may still arrive from another peer.
    gTypedIntentQueueRejected.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
}

} // namespace

bool Initialize(
    const SignatureResolver& resolver,
    HookManager& hooks,
    WorldEntityRegistry& registry,
    std::string& error) noexcept {
    error.clear();
    bool expected = false;
    if (!gInitialized.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }

    const auto& profile = Retail2015Profile();
    const auto* inputTargetSpec = profile.Find(SymbolId::CathodeBuildInputTarget);
    auto* inputTargetTarget = resolver.ResolveMutable(SymbolId::CathodeBuildInputTarget);
    const auto* queueSpec = profile.Find(SymbolId::CathodeQueueInput);
    auto* queueTarget = resolver.ResolveMutable(SymbolId::CathodeQueueInput);
    const auto* typedSpec = profile.Find(SymbolId::CathodeQueueTypedMessage);
    auto* typedTarget = resolver.ResolveMutable(SymbolId::CathodeQueueTypedMessage);
    const auto* callbackBuildSpec = profile.Find(SymbolId::CathodeBuildCallbackValue);
    auto* callbackBuildTarget = resolver.ResolveMutable(SymbolId::CathodeBuildCallbackValue);
    const auto* switchSetSpec = profile.Find(SymbolId::CathodeLogicSwitchSet);
    auto* switchSetTarget = resolver.ResolveMutable(SymbolId::CathodeLogicSwitchSet);
    const auto* switchClearSpec = profile.Find(SymbolId::CathodeLogicSwitchClear);
    auto* switchClearTarget = resolver.ResolveMutable(SymbolId::CathodeLogicSwitchClear);
    const auto* switchVtable = resolver.Resolve(SymbolId::CathodeLogicSwitchVtable);

    if (inputTargetSpec == nullptr || inputTargetTarget == nullptr ||
        inputTargetSpec->approvedPatchLength < 5U || !inputTargetSpec->trampolineRelocationSafe ||
        queueSpec == nullptr || queueTarget == nullptr ||
        queueSpec->approvedPatchLength < 5U || !queueSpec->trampolineRelocationSafe ||
        typedSpec == nullptr || typedTarget == nullptr ||
        typedSpec->approvedPatchLength < 5U || !typedSpec->trampolineRelocationSafe ||
        callbackBuildSpec == nullptr || callbackBuildTarget == nullptr ||
        callbackBuildSpec->approvedPatchLength < 5U || !callbackBuildSpec->trampolineRelocationSafe ||
        switchSetSpec == nullptr || switchSetTarget == nullptr ||
        switchSetSpec->approvedPatchLength < 5U || !switchSetSpec->trampolineRelocationSafe ||
        switchClearSpec == nullptr || switchClearTarget == nullptr ||
        switchClearSpec->approvedPatchLength < 5U || !switchClearSpec->trampolineRelocationSafe ||
        switchVtable == nullptr) {
        gInitialized.store(false, std::memory_order_release);
        error = "v13.3 Fix3.27 pre-QueueInput public-input dependencies are unavailable";
        return false;
    }

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    gObservationSalt = static_cast<std::uint64_t>(counter.QuadPart) ^
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U) ^
        reinterpret_cast<std::uintptr_t>(&registry);
    if (gObservationSalt == 0U) gObservationSalt = 1U;
    ClearEchoIds();
    {
        std::lock_guard lock(gCompositeInputSemanticMutex);
        for (auto& slot : gCompositeInputSemanticSlots) slot = {};
        gCompositeInputRateWindowTickMs = 0U;
        gCompositeInputRateWindowCount = 0U;
    }
    ClearRuntimeWorldIdCache();
    {
        std::lock_guard lock(gLogicSwitchBindingMutex);
        for (auto& binding : gLogicSwitchBindings) binding = {};
    }
    gRegistry.store(&registry, std::memory_order_release);
    gMainModuleBase.store(
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),
        std::memory_order_release);
    gLogicSwitchVtable.store(reinterpret_cast<std::uintptr_t>(switchVtable), std::memory_order_release);
    gResolver = &resolver;
    gHooks = &hooks;

    // Fix1 stability rule: initialization is now hook-free. In particular the
    // experimental generated LogicSwitch constructor detour from v13.3 is no
    // longer installed. SET/CLEAR observation is armed only after a compatible
    // manifest is active, so WorldSync OFF leaves the environment hot path
    // completely untouched.
    gLogicSwitchHookInstalled.store(false, std::memory_order_release);
    gCharacterCallbackOriginHookInstalled.store(false, std::memory_order_release);
    gBuildInputTargetHookInstalled.store(false, std::memory_order_release);
    gHookInstalled.store(false, std::memory_order_release);
    gInitializationCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return true;
}

void SetLocalInputTargetObserver(
    const LocalInputTargetObserver observer) noexcept {
    gLocalInputTargetObserver.store(observer, std::memory_order_release);
}

bool LocalInputTargetObservationReady() noexcept {
    return gBuildInputTargetHookInstalled.load(std::memory_order_acquire) &&
        gBuildInputTargetTrampoline.load(std::memory_order_acquire) != nullptr;
}

bool ArmCapture(std::string& error) noexcept {
    error.clear();
    std::lock_guard lock(gArmMutex);
    if (!gInitialized.load(std::memory_order_acquire) || gRegistry.load(std::memory_order_acquire) == nullptr ||
        gResolver == nullptr || gHooks == nullptr) {
        error = "generic CATHODE capture is not initialized";
        return false;
    }
    if (gHookInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    const auto registryState = gRegistry.load(std::memory_order_acquire)->DiagnosticsSnapshot();
    if (!registryState.manifestLoaded || registryState.levelGeneration == 0U) {
        error = "generic CATHODE capture requires an active runtime manifest";
        return false;
    }

    const auto& profile = Retail2015Profile();
    const auto* inputTargetSpec = profile.Find(SymbolId::CathodeBuildInputTarget);
    auto* inputTargetTarget = gResolver->ResolveMutable(SymbolId::CathodeBuildInputTarget);
    const auto* queueSpec = profile.Find(SymbolId::CathodeQueueInput);
    auto* queueTarget = gResolver->ResolveMutable(SymbolId::CathodeQueueInput);
    const auto* typedSpec = profile.Find(SymbolId::CathodeQueueTypedMessage);
    auto* typedTarget = gResolver->ResolveMutable(SymbolId::CathodeQueueTypedMessage);
    const auto* callbackBuildSpec = profile.Find(SymbolId::CathodeBuildCallbackValue);
    auto* callbackBuildTarget = gResolver->ResolveMutable(SymbolId::CathodeBuildCallbackValue);
    const auto* switchSetSpec = profile.Find(SymbolId::CathodeLogicSwitchSet);
    auto* switchSetTarget = gResolver->ResolveMutable(SymbolId::CathodeLogicSwitchSet);
    const auto* switchClearSpec = profile.Find(SymbolId::CathodeLogicSwitchClear);
    auto* switchClearTarget = gResolver->ResolveMutable(SymbolId::CathodeLogicSwitchClear);
    if (inputTargetSpec == nullptr || inputTargetTarget == nullptr ||
        inputTargetSpec->approvedPatchLength < 5U || !inputTargetSpec->trampolineRelocationSafe ||
        queueSpec == nullptr || queueTarget == nullptr ||
        queueSpec->approvedPatchLength < 5U || !queueSpec->trampolineRelocationSafe ||
        typedSpec == nullptr || typedTarget == nullptr ||
        typedSpec->approvedPatchLength < 5U || !typedSpec->trampolineRelocationSafe ||
        callbackBuildSpec == nullptr || callbackBuildTarget == nullptr ||
        callbackBuildSpec->approvedPatchLength < 5U || !callbackBuildSpec->trampolineRelocationSafe ||
        switchSetSpec == nullptr || switchSetTarget == nullptr ||
        switchSetSpec->approvedPatchLength < 5U || !switchSetSpec->trampolineRelocationSafe ||
        switchClearSpec == nullptr || switchClearTarget == nullptr ||
        switchClearSpec->approvedPatchLength < 5U || !switchClearSpec->trampolineRelocationSafe) {
        error = "v13.3 Fix3.27 pre-QueueInput public-input capture or required CATHODE observer is unavailable while arming";
        return false;
    }

    const auto installedBefore = gHooks->InstalledCount();
    const auto rollback = [&] {
        std::string rollbackError;
        static_cast<void>(gHooks->UninstallFrom(installedBefore, rollbackError));
        gQueueInputTrampoline.store(nullptr, std::memory_order_release);
        gBuildInputTargetTrampoline.store(nullptr, std::memory_order_release);
        gBuildInputTargetHookInstalled.store(false, std::memory_order_release);
        gQueueTypedMessageTrampoline.store(nullptr, std::memory_order_release);
        gBuildCallbackValueTrampoline.store(nullptr, std::memory_order_release);
        gLogicSwitchSetTrampoline.store(nullptr, std::memory_order_release);
        gLogicSwitchClearTrampoline.store(nullptr, std::memory_order_release);
        gQueueInputTrampolineRaw = nullptr;
        gBuildInputTargetTrampolineRaw = nullptr;
        gQueueTypedMessageTrampolineRaw = nullptr;
        gBuildCallbackValueTrampolineRaw = nullptr;
        gLogicSwitchSetTrampolineRaw = nullptr;
        gLogicSwitchClearTrampolineRaw = nullptr;
        gFix321CallRedirectsInstalled.store(false, std::memory_order_release);
        gFix321RedirectCallSites.store(0U, std::memory_order_release);
        gFix322CallRedirectInstalled.store(false, std::memory_order_release);
        gFix322RedirectCallSites.store(0U, std::memory_order_release);
    };

    HookRequest inputTargetRequest{};
    inputTargetRequest.name = "worldsync2_capture_prequeue_composite_public_input";
    inputTargetRequest.target = inputTargetTarget;
    inputTargetRequest.detour = reinterpret_cast<void*>(&BuildInputTargetDetour);
    inputTargetRequest.patchLength = inputTargetSpec->approvedPatchLength;
    inputTargetRequest.expected = inputTargetSpec->expected;
    inputTargetRequest.relocationSafePrefix = inputTargetSpec->trampolineRelocationSafe;
    inputTargetRequest.trampolineOut = &gBuildInputTargetTrampolineRaw;
    inputTargetRequest.publishedTrampolineOut = &gBuildInputTargetTrampoline;
    if (!gHooks->Install(inputTargetRequest, error)) {
        rollback();
        return false;
    }
    gBuildInputTargetHookInstalled.store(true, std::memory_order_release);

    HookRequest typedRequest{};
    typedRequest.name = "worldsync2_observe_generic_cathode_typed_messages";
    typedRequest.target = typedTarget;
    typedRequest.detour = reinterpret_cast<void*>(&QueueTypedMessageDetour);
    typedRequest.patchLength = typedSpec->approvedPatchLength;
    typedRequest.expected = typedSpec->expected;
    typedRequest.relocationSafePrefix = typedSpec->trampolineRelocationSafe;
    typedRequest.trampolineOut = &gQueueTypedMessageTrampolineRaw;
    typedRequest.publishedTrampolineOut = &gQueueTypedMessageTrampoline;
    if (!gHooks->Install(typedRequest, error)) { rollback(); return false; }

    HookRequest inputRequest{};
    inputRequest.name = "worldsync2_observe_generic_cathode_input_edges";
    inputRequest.target = queueTarget;
    inputRequest.detour = reinterpret_cast<void*>(&QueueInputDetour);
    inputRequest.patchLength = queueSpec->approvedPatchLength;
    inputRequest.expected = queueSpec->expected;
    inputRequest.relocationSafePrefix = queueSpec->trampolineRelocationSafe;
    inputRequest.trampolineOut = &gQueueInputTrampolineRaw;
    inputRequest.publishedTrampolineOut = &gQueueInputTrampoline;
    if (!gHooks->Install(inputRequest, error)) {
        rollback();
        return false;
    }

    HookRequest callbackBuildRequest{};
    callbackBuildRequest.name = "worldsync2_probe_character_callback_origin";
    callbackBuildRequest.target = callbackBuildTarget;
    callbackBuildRequest.detour = reinterpret_cast<void*>(&BuildCallbackValueDetour);
    callbackBuildRequest.patchLength = callbackBuildSpec->approvedPatchLength;
    callbackBuildRequest.expected = callbackBuildSpec->expected;
    callbackBuildRequest.relocationSafePrefix = callbackBuildSpec->trampolineRelocationSafe;
    callbackBuildRequest.trampolineOut = &gBuildCallbackValueTrampolineRaw;
    callbackBuildRequest.publishedTrampolineOut = &gBuildCallbackValueTrampoline;
    if (!gHooks->Install(callbackBuildRequest, error)) {
        rollback();
        return false;
    }

    HookRequest switchSetRequest{};
    switchSetRequest.name = "worldsync2_observe_generic_logic_switch_set";
    switchSetRequest.target = switchSetTarget;
    switchSetRequest.detour = reinterpret_cast<void*>(&LogicSwitchSetDetour);
    switchSetRequest.patchLength = switchSetSpec->approvedPatchLength;
    switchSetRequest.expected = switchSetSpec->expected;
    switchSetRequest.relocationSafePrefix = switchSetSpec->trampolineRelocationSafe;
    switchSetRequest.trampolineOut = &gLogicSwitchSetTrampolineRaw;
    switchSetRequest.publishedTrampolineOut = &gLogicSwitchSetTrampoline;
    if (!gHooks->Install(switchSetRequest, error)) {
        rollback();
        return false;
    }

    HookRequest switchClearRequest{};
    switchClearRequest.name = "worldsync2_observe_generic_logic_switch_clear";
    switchClearRequest.target = switchClearTarget;
    switchClearRequest.detour = reinterpret_cast<void*>(&LogicSwitchClearDetour);
    switchClearRequest.patchLength = switchClearSpec->approvedPatchLength;
    switchClearRequest.expected = switchClearSpec->expected;
    switchClearRequest.relocationSafePrefix = switchClearSpec->trampolineRelocationSafe;
    switchClearRequest.trampolineOut = &gLogicSwitchClearTrampolineRaw;
    switchClearRequest.publishedTrampolineOut = &gLogicSwitchClearTrampoline;
    if (!gHooks->Install(switchClearRequest, error)) {
        rollback();
        return false;
    }

    // Fix3.21 is diagnostic-only and must never disable otherwise-stable
    // WorldSync capture. Redirect only the exact retail E8 call-sites proven by
    // the AI.exe.c call graph. If certification fails, remove any partial
    // Fix3.21 redirects and continue with the pre-existing generic observers.
    const auto fix321Checkpoint = gHooks->InstalledCount();
    std::string fix321Error;
    if (!InstallFix321CompositeStateCallRedirects(*gHooks, fix321Error)) {
        std::string fix321RollbackError;
        if (!gHooks->UninstallFrom(fix321Checkpoint, fix321RollbackError)) {
            rollback();
            error = "Fix3.21 optional redirect rollback failed: " + fix321RollbackError;
            return false;
        }
        gFix321CallRedirectsInstalled.store(false, std::memory_order_release);
        gFix321RedirectCallSites.store(0U, std::memory_order_release);
        gFix321InstallFailures.fetch_add(1U, std::memory_order_relaxed);
    }

    // Fix3.22 is likewise diagnostic-only. Its single direct-CALL redirect is
    // installed only when the runtime E8 target exactly matches the static
    // FUN_00965A90 -> FUN_009657F0 evidence. Failure is isolated from stable
    // WorldSync and from the pre-existing Fix3.21 recorder.
    const auto fix322Checkpoint = gHooks->InstalledCount();
    std::string fix322Error;
    if (!InstallFix322CompositeExecutionCallRedirect(*gHooks, fix322Error)) {
        std::string fix322RollbackError;
        if (!gHooks->UninstallFrom(fix322Checkpoint, fix322RollbackError)) {
            rollback();
            error = "Fix3.22 optional redirect rollback failed: " + fix322RollbackError;
            return false;
        }
        gFix322CallRedirectInstalled.store(false, std::memory_order_release);
        gFix322RedirectCallSites.store(0U, std::memory_order_release);
        gFix322InstallFailures.fetch_add(1U, std::memory_order_relaxed);
    }

    std::string ws3Error;
    const bool ws3Armed = world_actions::Arm(*gResolver, *gHooks,
        *gRegistry.load(std::memory_order_acquire), ws3Error);
    if (!ws3Armed || (world_actions::Requested() && !world_actions::Installed())) {
        rollback();
        gTypedMessageHookInstalled.store(false, std::memory_order_release);
        gCharacterCallbackOriginHookInstalled.store(false, std::memory_order_release);
        gLogicSwitchHookInstalled.store(false, std::memory_order_release);
        gHookInstalled.store(false, std::memory_order_release);
        error = ws3Error.empty()
            ? "WS3 strict was requested but the certified public ROOT hooks are not installed"
            : "WS3 strict arm failed: " + ws3Error;
        TouchVersion();
        return false;
    }

    // WS3 v6 uses only the generic public-action pipeline. The historical
    // Door semantic hooks from 0.11.4.57 remain dormant source for comparison,
    // but are deliberately NOT armed: object-specific capture would mask or
    // duplicate generic ROOT coverage.

    // WS3 is mandatory. A failed hook installation rolls the capture set
    // back; it never enables the historical WS2 producers.
    gTypedMessageHookInstalled.store(true, std::memory_order_release);
    gCharacterCallbackOriginHookInstalled.store(true, std::memory_order_release);
    gLogicSwitchHookInstalled.store(true, std::memory_order_release);
    gHookInstalled.store(true, std::memory_order_release);
    TouchVersion();
    return true;
}

bool Initialized() noexcept {
    return gInitialized.load(std::memory_order_acquire);
}

void ArmCallbackClassificationProbe() noexcept {
    for (auto& slot : gSchedulerCausalHistory) {
        slot.sequence.store(0U, std::memory_order_relaxed);
    }
    gSchedulerCausalSequence.store(1U, std::memory_order_relaxed);
    gCallbackAbiProbeCallerMatches.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbePairMatches.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeInputMatches.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeSamplesCaptured.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeSampleLimitDrops.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeRawReadFailures.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeIdentityAttempts.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeIdentityLimitDrops.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeRuntimeKeyAttempts.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeSourceKeyResolved.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeTargetKeyResolved.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeCallbackKeyResolved.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeCallbackObjectPresent.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeTargetResolved.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeLastInputShortGuid.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeFirstFlags.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeLastFlags.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeLastRefCount.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeLastShapeFlags.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeLastTargetHigh.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeLastTargetLow.store(0U, std::memory_order_relaxed);
    gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchNone, std::memory_order_relaxed);
    gCallbackAbiProbeFirstFlagsSet.store(false, std::memory_order_relaxed);
    gCallbackAbiProbeFlagsVaried.store(false, std::memory_order_relaxed);
    gLogicSwitchCausalLastAgeMs.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastSourceHigh.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastSourceLow.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastTargetHigh.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastTargetLow.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastStateHigh.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastStateLow.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastSchedulerParameter.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastTargetInput.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastFlags.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastEnabled.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastCallbackHigh.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastCallbackLow.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastCallbackExecutionVtableRva.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastSourceRuntimeKeyA.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastSourceRuntimeKeyB.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastTargetRuntimeKeyA.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastTargetRuntimeKeyB.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastCallbackRuntimeKeyA.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastCallbackRuntimeKeyB.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastStateRuntimeKeyA.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastStateRuntimeKeyB.store(0U, std::memory_order_relaxed);
    gLogicSwitchCausalLastQueueCallerRva.store(0U, std::memory_order_relaxed);
    for (auto& word : gLogicSwitchCausalLastCallbackRawWords) {
        word.store(0U, std::memory_order_relaxed);
    }
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    gCallbackClassificationUntilTickMs.store(
        now + kCallbackClassificationProbeWindowMs,
        std::memory_order_release);
}

void ArmCharacterCallbackOriginProbe() noexcept {
    if (!gInitialized.load(std::memory_order_acquire) ||
        !gCharacterCallbackOriginHookInstalled.load(std::memory_order_acquire)) {
        return;
    }
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    gCharacterCallbackOriginProbeUntilTickMs.store(
        now + kFix312OriginProbeWindowMs,
        std::memory_order_release);
    TouchVersion();
}

void ArmTypedMessageOriginProbe() noexcept {
    if (!gInitialized.load(std::memory_order_acquire) ||
        !gTypedMessageHookInstalled.load(std::memory_order_acquire)) {
        return;
    }
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    gTypedMessageOriginProbeUntilTickMs.store(
        now + kFix313OriginProbeWindowMs,
        std::memory_order_release);
    TouchVersion();
}

void ArmRemoteTypedIdentityProbe() noexcept {
    if (!gInitialized.load(std::memory_order_acquire)) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    gTypedRemoteIdentityProbeUntilTickMs.store(
        now + kFix314RemoteIdentityProbeWindowMs,
        std::memory_order_release);
    TouchVersion();
}

void ArmLogicDownstreamProbe() noexcept {
    if (!gInitialized.load(std::memory_order_acquire) ||
        !gLogicSwitchHookInstalled.load(std::memory_order_acquire) ||
        !gHookInstalled.load(std::memory_order_acquire)) {
        return;
    }
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto previous = gLogicDownstreamProbeUntilTickMs.load(std::memory_order_relaxed);
    if (previous == 0U || now > previous) {
        gLogicDownstreamScopes.store(0U, std::memory_order_relaxed);
        gLogicDownstreamEdges.store(0U, std::memory_order_relaxed);
        gLogicDownstreamValidEdges.store(0U, std::memory_order_relaxed);
        gLogicDownstreamCallbackEdges.store(0U, std::memory_order_relaxed);
        gLogicDownstreamReadFailures.store(0U, std::memory_order_relaxed);
        gLogicDownstreamWorldMask.store(0U, std::memory_order_relaxed);
        gLogicDownstreamSourceA.store(0U, std::memory_order_relaxed);
        gLogicDownstreamSourceB.store(0U, std::memory_order_relaxed);
        gLogicDownstreamTargetA.store(0U, std::memory_order_relaxed);
        gLogicDownstreamTargetB.store(0U, std::memory_order_relaxed);
        gLogicDownstreamSchedulerParameter.store(0U, std::memory_order_relaxed);
        gLogicDownstreamTargetInput.store(0U, std::memory_order_relaxed);
        gLogicDownstreamCallbackFlags.store(0U, std::memory_order_relaxed);
        gLogicDownstreamFirstCallSiteRva.store(0U, std::memory_order_relaxed);
        gLogicDownstreamLastCallSiteRva.store(0U, std::memory_order_relaxed);
        gLogicDownstreamCallSiteVaried.store(0U, std::memory_order_relaxed);
        gLogicDownstreamSequenceHash.store(kFix315HashOffset, std::memory_order_relaxed);
        gLogicDownstreamThreadId.store(0U, std::memory_order_relaxed);
        gLogicDownstreamTickMs.store(0U, std::memory_order_relaxed);
        gPostClearWindowUntilTickMs.store(0U, std::memory_order_relaxed);
        gPostClearOriginTickMs.store(0U, std::memory_order_relaxed);
        gPostClearArms.store(0U, std::memory_order_relaxed);
        gPostClearSeen.store(0U, std::memory_order_relaxed);
        gPostClearAnchorMatches.store(0U, std::memory_order_relaxed);
        gPostClearValidEdges.store(0U, std::memory_order_relaxed);
        gPostClearCallbackEdges.store(0U, std::memory_order_relaxed);
        gPostClearReadFailures.store(0U, std::memory_order_relaxed);
        gPostClearInspectLimitDrops.store(0U, std::memory_order_relaxed);
        gPostClearAnchorRole.store(0U, std::memory_order_relaxed);
        gPostClearWorldMask.store(0U, std::memory_order_relaxed);
        gPostClearSourceA.store(0U, std::memory_order_relaxed);
        gPostClearSourceB.store(0U, std::memory_order_relaxed);
        gPostClearTargetA.store(0U, std::memory_order_relaxed);
        gPostClearTargetB.store(0U, std::memory_order_relaxed);
        gPostClearSchedulerParameter.store(0U, std::memory_order_relaxed);
        gPostClearTargetInput.store(0U, std::memory_order_relaxed);
        gPostClearCallbackFlags.store(0U, std::memory_order_relaxed);
        gPostClearFirstCallSiteRva.store(0U, std::memory_order_relaxed);
        gPostClearLastCallSiteRva.store(0U, std::memory_order_relaxed);
        gPostClearCallSiteVaried.store(0U, std::memory_order_relaxed);
        gPostClearSequenceHash.store(kFix316HashOffset, std::memory_order_relaxed);
        gPostClearFirstDeltaMs.store(0U, std::memory_order_relaxed);
        gPostClearLastDeltaMs.store(0U, std::memory_order_relaxed);
        gPostClearThreadId.store(0U, std::memory_order_relaxed);
        gPostClearTickMs.store(0U, std::memory_order_relaxed);
        gClearDispatchCandidates.store(0U, std::memory_order_relaxed);
        gClearDispatchExactMatches.store(0U, std::memory_order_relaxed);
        gClearDispatchReadFailures.store(0U, std::memory_order_relaxed);
        gClearDispatchVtableRva.store(0U, std::memory_order_relaxed);
        gClearDispatchTargetRva.store(0U, std::memory_order_relaxed);
        gClearDispatchFirstTargetRva.store(0U, std::memory_order_relaxed);
        gClearDispatchLastTargetRva.store(0U, std::memory_order_relaxed);
        gClearDispatchTargetVaried.store(0U, std::memory_order_relaxed);
        gClearDispatchBeforeState.store(0U, std::memory_order_relaxed);
        gClearDispatchAfterState.store(0U, std::memory_order_relaxed);
        gClearDispatchThreadId.store(0U, std::memory_order_relaxed);
        gClearDispatchTickMs.store(0U, std::memory_order_relaxed);
        gClearCodeCandidates.store(0U, std::memory_order_relaxed);
        gClearCodeExactMatches.store(0U, std::memory_order_relaxed);
        gClearCodeReadFailures.store(0U, std::memory_order_relaxed);
        gClearCodeTargetRva.store(0U, std::memory_order_relaxed);
        gClearCodeWord0.store(0U, std::memory_order_relaxed);
        gClearCodeWord1.store(0U, std::memory_order_relaxed);
        gClearCodeBranchKind.store(0U, std::memory_order_relaxed);
        gClearCodeBranchTargetRva.store(0U, std::memory_order_relaxed);
        gClearCodeFinalTargetRva.store(0U, std::memory_order_relaxed);
        gClearCodeJumpDepth.store(0U, std::memory_order_relaxed);
        gClearCodeHash.store(kFix318HashOffset, std::memory_order_relaxed);
        gClearCodeThreadId.store(0U, std::memory_order_relaxed);
        gClearCodeTickMs.store(0U, std::memory_order_relaxed);
    }
    gLogicDownstreamProbeUntilTickMs.store(
        now + kFix315DownstreamProbeWindowMs,
        std::memory_order_release);
    TouchVersion();
}

void ArmCausalDifferentialTrace() noexcept {
    if (!gInitialized.load(std::memory_order_acquire) ||
        !gHookInstalled.load(std::memory_order_acquire)) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto previous = gFix319ArmUntilTickMs.load(std::memory_order_relaxed);
    const auto origin = gFix319OriginTickMs.load(std::memory_order_acquire);
    // Preserve a completed trace indefinitely for this diagnostic session.
    // A late second WORLD_INTERACTION_STATS must not erase the evidence merely
    // because the 30-second pre-anchor arm window expired. A new process/level
    // generation still clears the recorder through the existing reset paths.
    if (origin == 0U && (previous == 0U || now > previous)) {
        gFix319WindowUntilTickMs.store(0U, std::memory_order_relaxed);
        gFix319OriginTickMs.store(0U, std::memory_order_relaxed);
        gFix319AnchorRole.store(0U, std::memory_order_relaxed);
        gFix319Anchors.store(0U, std::memory_order_relaxed);
        gFix319Inspected.store(0U, std::memory_order_relaxed);
        gFix319Accepted.store(0U, std::memory_order_relaxed);
        gFix319UniqueEntries.store(0U, std::memory_order_relaxed);
        gFix319CapacityDrops.store(0U, std::memory_order_relaxed);
        gFix319ReadFailures.store(0U, std::memory_order_relaxed);
        for (auto& slot : gFix319TraceSlots) {
            slot.ready.store(0U, std::memory_order_relaxed);
            slot.signatureHash.store(0U, std::memory_order_relaxed);
            slot.count.store(0U, std::memory_order_relaxed);
            slot.executionMask.store(0U, std::memory_order_relaxed);
            slot.firstDeltaMs.store(0U, std::memory_order_relaxed);
            slot.lastDeltaMs.store(0U, std::memory_order_relaxed);
            slot.firstCallSiteRva.store(0U, std::memory_order_relaxed);
            slot.lastCallSiteRva.store(0U, std::memory_order_relaxed);
            slot.callSiteVaried.store(0U, std::memory_order_relaxed);
            slot.semantic = {};
        }
    }
    gFix319ArmUntilTickMs.store(now + kFix319ArmWindowMs, std::memory_order_release);
    TouchVersion();
}

void ArmCompositeStateDifferentialTrace() noexcept {
    if (!gInitialized.load(std::memory_order_acquire) ||
        !gHookInstalled.load(std::memory_order_acquire) ||
        !gFix321CallRedirectsInstalled.load(std::memory_order_acquire)) return;

    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto origin = gFix321OriginTickMs.load(std::memory_order_acquire);

    // BuildFix01: one single-shot trace per level generation. Once armed, keep
    // the snapshot even when it remains completely empty and even after the
    // bounded observation window expires. This makes a zero-entry Host/Client
    // result observable instead of letting the readout command erase/re-arm it.
    // ResetForGeneration() clears the recorder for the next level generation.
    if (origin == 0U) {
        gFix321WindowUntilTickMs.store(0U, std::memory_order_relaxed);
        gFix321OriginTickMs.store(0U, std::memory_order_relaxed);
        gFix321Inspected.store(0U, std::memory_order_relaxed);
        gFix321Accepted.store(0U, std::memory_order_relaxed);
        gFix321UniqueEntries.store(0U, std::memory_order_relaxed);
        gFix321CapacityDrops.store(0U, std::memory_order_relaxed);
        gFix321ReadFailures.store(0U, std::memory_order_relaxed);
        gFix321LimitDrops.store(0U, std::memory_order_relaxed);
        for (auto& slot : gFix321TraceSlots) {
            slot.ready.store(0U, std::memory_order_relaxed);
            slot.signatureHash.store(0U, std::memory_order_relaxed);
            slot.semantic = {};
            slot.count.store(0U, std::memory_order_relaxed);
            slot.executionMask.store(0U, std::memory_order_relaxed);
            slot.firstDeltaMs.store(0U, std::memory_order_relaxed);
            slot.lastDeltaMs.store(0U, std::memory_order_relaxed);
        }
        gFix321OriginTickMs.store(now, std::memory_order_release);
        gFix321WindowUntilTickMs.store(now + kFix321TraceWindowMs, std::memory_order_release);
        TouchVersion();
    }
}


void ArmCompositeDispatchDifferentialTrace() noexcept {
    if (!gInitialized.load(std::memory_order_acquire) ||
        !gHookInstalled.load(std::memory_order_acquire) ||
        !gFix322CallRedirectInstalled.load(std::memory_order_acquire)) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const auto origin = gFix322OriginTickMs.load(std::memory_order_acquire);
    // Single-shot per level generation, matching Fix3.21 BuildFix01. Preserve a
    // real zero-entry result instead of rearming on the readout command.
    if (origin != 0U) return;
    gFix322WindowUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix322Inspected.store(0U, std::memory_order_relaxed);
    gFix322Accepted.store(0U, std::memory_order_relaxed);
    gFix322UniqueEntries.store(0U, std::memory_order_relaxed);
    gFix322ChangedCalls.store(0U, std::memory_order_relaxed);
    gFix322CapacityDrops.store(0U, std::memory_order_relaxed);
    gFix322ReadFailures.store(0U, std::memory_order_relaxed);
    gFix322LimitDrops.store(0U, std::memory_order_relaxed);
    for (auto& slot : gFix322TraceSlots) {
        slot.ready.store(0U, std::memory_order_relaxed);
        slot.signatureHash.store(0U, std::memory_order_relaxed);
        slot.semantic = {};
        slot.count.store(0U, std::memory_order_relaxed);
        slot.executionMask.store(0U, std::memory_order_relaxed);
        slot.firstDeltaMs.store(0U, std::memory_order_relaxed);
        slot.lastDeltaMs.store(0U, std::memory_order_relaxed);
    }
    gFix322OriginTickMs.store(now, std::memory_order_release);
    gFix322WindowUntilTickMs.store(now + kFix322TraceWindowMs, std::memory_order_release);
    TouchVersion();
}

CompositeDispatchTraceSnapshot CompositeDispatchTraceDiagnosticsSnapshot() noexcept {
    CompositeDispatchTraceSnapshot result{};
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    result.hookInstalled = gFix322CallRedirectInstalled.load(std::memory_order_acquire);
    result.armed = gFix322OriginTickMs.load(std::memory_order_acquire) != 0U;
    result.active = Fix322TraceActive(now);
    result.windowMs = static_cast<std::uint32_t>(kFix322TraceWindowMs);
    result.redirectCallSites = gFix322RedirectCallSites.load(std::memory_order_acquire);
    result.installFailures = gFix322InstallFailures.load(std::memory_order_acquire);
    result.inspected = gFix322Inspected.load(std::memory_order_acquire);
    result.accepted = gFix322Accepted.load(std::memory_order_acquire);
    result.uniqueEntries = gFix322UniqueEntries.load(std::memory_order_acquire);
    result.changedCalls = gFix322ChangedCalls.load(std::memory_order_acquire);
    result.capacityDrops = gFix322CapacityDrops.load(std::memory_order_acquire);
    result.readFailures = gFix322ReadFailures.load(std::memory_order_acquire);
    result.limitDrops = gFix322LimitDrops.load(std::memory_order_acquire);
    result.originTickMs = gFix322OriginTickMs.load(std::memory_order_acquire);
    for (const auto& slot : gFix322TraceSlots) {
        if (slot.ready.load(std::memory_order_acquire) == 0U ||
            result.entryCount >= result.entries.size()) continue;
        auto& entry = result.entries[result.entryCount++];
        entry.signatureHash = slot.signatureHash.load(std::memory_order_relaxed);
        entry.count = slot.count.load(std::memory_order_relaxed);
        entry.targetRva = slot.semantic.targetRva;
        entry.callerRva = slot.semantic.callerRva;
        entry.firstPointerRva = slot.semantic.firstPointerRva;
        entry.firstPointerReadable = slot.semantic.firstPointerReadable;
        entry.beforeHash = slot.semantic.beforeHash;
        entry.afterHash = slot.semantic.afterHash;
        entry.changedWordMask = slot.semantic.changedWordMask;
        entry.changedWordCount = slot.semantic.changedWordCount;
        entry.changedOffsets = slot.semantic.changedOffsets;
        entry.beforeWords = slot.semantic.beforeWords;
        entry.afterWords = slot.semantic.afterWords;
        entry.executionMask = slot.executionMask.load(std::memory_order_relaxed);
        entry.firstDeltaMs = slot.firstDeltaMs.load(std::memory_order_relaxed);
        entry.lastDeltaMs = slot.lastDeltaMs.load(std::memory_order_relaxed);
    }
    return result;
}

CompositeStateTraceSnapshot CompositeStateTraceDiagnosticsSnapshot() noexcept {
    CompositeStateTraceSnapshot result{};
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    result.hookInstalled = gFix321CallRedirectsInstalled.load(std::memory_order_acquire);
    result.armed = gFix321OriginTickMs.load(std::memory_order_acquire) != 0U;
    result.active = Fix321TraceActive(now);
    result.windowMs = static_cast<std::uint32_t>(kFix321TraceWindowMs);
    result.redirectCallSites = gFix321RedirectCallSites.load(std::memory_order_acquire);
    result.installFailures = gFix321InstallFailures.load(std::memory_order_acquire);
    result.inspected = gFix321Inspected.load(std::memory_order_acquire);
    result.accepted = gFix321Accepted.load(std::memory_order_acquire);
    result.uniqueEntries = gFix321UniqueEntries.load(std::memory_order_acquire);
    result.capacityDrops = gFix321CapacityDrops.load(std::memory_order_acquire);
    result.readFailures = gFix321ReadFailures.load(std::memory_order_acquire);
    result.limitDrops = gFix321LimitDrops.load(std::memory_order_acquire);
    result.originTickMs = gFix321OriginTickMs.load(std::memory_order_acquire);

    for (const auto& slot : gFix321TraceSlots) {
        if (result.entryCount >= result.entries.size()) break;
        if (slot.ready.load(std::memory_order_acquire) == 0U) continue;
        auto& entry = result.entries[result.entryCount++];
        entry.signatureHash = slot.signatureHash.load(std::memory_order_acquire);
        entry.count = slot.count.load(std::memory_order_acquire);
        entry.helperRva = slot.semantic.helperRva;
        entry.callerRva = slot.semantic.callerRva;
        entry.runtimeKey = slot.semantic.runtimeKey;
        entry.worldEntity = slot.semantic.worldEntity;
        entry.executionVtableRva = slot.semantic.executionVtableRva;
        entry.identityMask = slot.semantic.identityMask;
        entry.valueReadMask = slot.semantic.valueReadMask;
        entry.valueWord0 = slot.semantic.valueWord0;
        entry.contextReadMask = slot.semantic.contextReadMask;
        entry.contextWord0 = slot.semantic.contextWord0;
        entry.contextWord1 = slot.semantic.contextWord1;
        entry.objectReadMask = slot.semantic.objectReadMask;
        entry.objectWord0 = slot.semantic.objectWord0;
        entry.objectWord1 = slot.semantic.objectWord1;
        entry.executionMask = slot.executionMask.load(std::memory_order_acquire);
        entry.firstDeltaMs = slot.firstDeltaMs.load(std::memory_order_acquire);
        entry.lastDeltaMs = slot.lastDeltaMs.load(std::memory_order_acquire);
    }
    return result;
}

void ArmCharacterCallbackReplayExperiment() noexcept {
    // Historical research-only replay is incompatible with WS3 strict mode.
    // Never allow a diagnostic action to add a second gameplay replication path.
    if (world_actions::Installed()) {
        gFix320ReplayUntilTickMs.store(0U, std::memory_order_release);
        return;
    }
    if (!gInitialized.load(std::memory_order_acquire) ||
        !gHookInstalled.load(std::memory_order_acquire)) return;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    gFix320WindowSubmissions.store(0U, std::memory_order_release);
    gFix320WindowExecutions.store(0U, std::memory_order_release);
    gFix320ReplayUntilTickMs.store(now + kFix320ReplayWindowMs, std::memory_order_release);
    TouchVersion();
}

CausalTraceSnapshot CausalTraceDiagnosticsSnapshot() noexcept {
    CausalTraceSnapshot result{};
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    result.armed = Fix319ArmActive(now);
    result.active = Fix319TraceActive(now);
    result.anchorRole = gFix319AnchorRole.load(std::memory_order_acquire);
    result.windowMs = static_cast<std::uint32_t>(kFix319TraceWindowMs);
    result.anchors = gFix319Anchors.load(std::memory_order_acquire);
    result.inspected = gFix319Inspected.load(std::memory_order_acquire);
    result.accepted = gFix319Accepted.load(std::memory_order_acquire);
    result.uniqueEntries = gFix319UniqueEntries.load(std::memory_order_acquire);
    result.capacityDrops = gFix319CapacityDrops.load(std::memory_order_acquire);
    result.readFailures = gFix319ReadFailures.load(std::memory_order_acquire);
    result.originTickMs = gFix319OriginTickMs.load(std::memory_order_acquire);

    for (const auto& slot : gFix319TraceSlots) {
        if (result.entryCount >= result.entries.size()) break;
        if (slot.ready.load(std::memory_order_acquire) == 0U) continue;
        CausalTraceEntry entry{};
        entry.signatureHash = slot.signatureHash.load(std::memory_order_acquire);
        entry.count = slot.count.load(std::memory_order_acquire);
        entry.channel = slot.semantic.channel;
        entry.worldMask = slot.semantic.worldMask;
        entry.sourceRuntimeKey = slot.semantic.sourceRuntimeKey;
        entry.targetRuntimeKey = slot.semantic.targetRuntimeKey;
        entry.sourceWorldEntity = slot.semantic.sourceWorldEntity;
        entry.targetWorldEntity = slot.semantic.targetWorldEntity;
        entry.primary = slot.semantic.primary;
        entry.secondary = slot.semantic.secondary;
        entry.payloadBytes = slot.semantic.payloadBytes;
        entry.payloadHash = slot.semantic.payloadHash;
        entry.delayBits = slot.semantic.delayBits;
        entry.tailArgument = slot.semantic.tailArgument;
        entry.callbackInput = slot.semantic.callbackInput;
        entry.callbackFlags = slot.semantic.callbackFlags;
        entry.callbackRuntimeKey = slot.semantic.callbackRuntimeKey;
        entry.callbackExecutionVtableRva = slot.semantic.callbackExecutionVtableRva;
        entry.semanticFlags = slot.semantic.semanticFlags;
        entry.executionMask = slot.executionMask.load(std::memory_order_acquire);
        entry.firstDeltaMs = slot.firstDeltaMs.load(std::memory_order_acquire);
        entry.lastDeltaMs = slot.lastDeltaMs.load(std::memory_order_acquire);
        entry.firstCallSiteRva = slot.firstCallSiteRva.load(std::memory_order_acquire);
        entry.lastCallSiteRva = slot.lastCallSiteRva.load(std::memory_order_acquire);
        entry.callSiteVaried = slot.callSiteVaried.load(std::memory_order_acquire) != 0U;
        result.entries[result.entryCount++] = entry;
    }
    std::sort(
        result.entries.begin(),
        result.entries.begin() + static_cast<std::ptrdiff_t>(result.entryCount),
        [](const CausalTraceEntry& lhs, const CausalTraceEntry& rhs) {
            if (lhs.firstDeltaMs != rhs.firstDeltaMs) return lhs.firstDeltaMs < rhs.firstDeltaMs;
            if (lhs.channel != rhs.channel) {
                return static_cast<std::uint32_t>(lhs.channel) <
                    static_cast<std::uint32_t>(rhs.channel);
            }
            return lhs.signatureHash < rhs.signatureHash;
        });
    return result;
}

namespace {
[[nodiscard]] bool Fix314RemoteProbeActive() noexcept {
    const auto until = gTypedRemoteIdentityProbeUntilTickMs.load(std::memory_order_relaxed);
    if (until == 0U) return false;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now > until) {
        gTypedRemoteIdentityProbeUntilTickMs.store(0U, std::memory_order_relaxed);
        return false;
    }
    return true;
}

[[nodiscard]] bool Fix314WitnessFamily(
    const ObservedTypedMessage& message,
    const std::size_t dataBytes) noexcept {
    return message.messageType == kFix314WitnessMessageType && dataBytes == 0U;
}
} // namespace

void ObserveRemoteTypedIdentity(
    const ObservedTypedMessage& message,
    const std::size_t dataBytes,
    const WorldEntityId& mutationEntity,
    const CathodeRuntimeKey& resolvedTargetKey,
    const bool rawGateAccepted) noexcept {
    if (!Fix314RemoteProbeActive() || !Fix314WitnessFamily(message, dataBytes)) return;
    gTypedRemoteIdentityCandidates.fetch_add(1U, std::memory_order_relaxed);

    const bool sourceIdValid = message.sourceWorldEntity.Valid();
    const bool targetIdValid = message.targetWorldEntity.Valid();
    if (!sourceIdValid || !targetIdValid) {
        TouchVersion();
        return;
    }

    gTypedRemoteIdentityExactMatches.fetch_add(1U, std::memory_order_relaxed);
    StartFix319Trace(kFix319AnchorRemoteWitness);
    gTypedRemoteIdentityMessageType.store(message.messageType, std::memory_order_relaxed);
    gTypedRemoteIdentityPayloadBytes.store(
        static_cast<std::uint32_t>(dataBytes), std::memory_order_relaxed);
    gTypedRemoteIdentitySourceWorldHigh.store(
        message.sourceWorldEntity.high, std::memory_order_relaxed);
    gTypedRemoteIdentitySourceWorldLow.store(
        message.sourceWorldEntity.low, std::memory_order_relaxed);
    gTypedRemoteIdentityTargetWorldHigh.store(
        message.targetWorldEntity.high, std::memory_order_relaxed);
    gTypedRemoteIdentityTargetWorldLow.store(
        message.targetWorldEntity.low, std::memory_order_relaxed);
    gTypedRemoteIdentitySenderSourceA.store(
        message.sourceRuntimeKey.entityA, std::memory_order_relaxed);
    gTypedRemoteIdentitySenderSourceB.store(
        message.sourceRuntimeKey.entityB, std::memory_order_relaxed);
    gTypedRemoteIdentitySenderTargetA.store(
        message.targetRuntimeKey.entityA, std::memory_order_relaxed);
    gTypedRemoteIdentitySenderTargetB.store(
        message.targetRuntimeKey.entityB, std::memory_order_relaxed);

    std::uint32_t mask = 0x1U | 0x2U;
    if (mutationEntity == message.targetWorldEntity) mask |= 0x4U;

    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry != nullptr) {
        const auto localSource = registry->FindRuntimeWorldEntityKey(message.sourceWorldEntity);
        const auto localTarget = registry->FindRuntimeWorldEntityKey(message.targetWorldEntity);
        if (localSource.has_value()) {
            mask |= 0x8U;
            gTypedRemoteIdentityLocalSourceA.store(
                localSource->entityA, std::memory_order_relaxed);
            gTypedRemoteIdentityLocalSourceB.store(
                localSource->entityB, std::memory_order_relaxed);
            if (*localSource == message.sourceRuntimeKey) mask |= 0x80U;
        }
        if (localTarget.has_value()) {
            mask |= 0x10U;
            gTypedRemoteIdentityLocalTargetA.store(
                localTarget->entityA, std::memory_order_relaxed);
            gTypedRemoteIdentityLocalTargetB.store(
                localTarget->entityB, std::memory_order_relaxed);
            if (*localTarget == message.targetRuntimeKey) mask |= 0x100U;
        }

        const auto senderSourceWorld =
            registry->FindRuntimeWorldEntityId(message.sourceRuntimeKey);
        const auto senderTargetWorld =
            registry->FindRuntimeWorldEntityId(message.targetRuntimeKey);
        if (senderSourceWorld.has_value() &&
            *senderSourceWorld == message.sourceWorldEntity) mask |= 0x20U;
        if (senderTargetWorld.has_value() &&
            *senderTargetWorld == message.targetWorldEntity) mask |= 0x40U;

        if (localSource.has_value() && localTarget.has_value() &&
            (mask & 0x4U) != 0U) {
            mask |= 0x400U;
            if (*localSource != message.sourceRuntimeKey ||
                *localTarget != message.targetRuntimeKey) {
                mask |= 0x800U;
            }
        }
    }

    if (resolvedTargetKey.entityA != 0U &&
        resolvedTargetKey == message.targetRuntimeKey) {
        // This bit records the exact target-key equality used by the current
        // ReplicationCore raw-key gate after mutation.entity resolution.
        mask |= 0x1000U;
    }
    if (rawGateAccepted) mask |= 0x200U;
    gTypedRemoteIdentityMask.store(mask, std::memory_order_relaxed);
    TouchVersion();
}

void ObserveRemoteTypedReplayResult(
    const ObservedTypedMessage& message,
    const std::size_t dataBytes,
    const ExecuteStatus status,
    const bool queued) noexcept {
    if (!Fix314RemoteProbeActive() || !Fix314WitnessFamily(message, dataBytes) ||
        !message.sourceWorldEntity.Valid() || !message.targetWorldEntity.Valid()) {
        return;
    }
    gTypedRemoteIdentityExecuteReached.fetch_add(1U, std::memory_order_relaxed);
    switch (status) {
    case ExecuteStatus::Queued:
        if (queued) gTypedRemoteIdentityQueued.fetch_add(1U, std::memory_order_relaxed);
        break;
    case ExecuteStatus::RuntimeUnavailable:
        gTypedRemoteIdentityRuntimeUnavailable.fetch_add(1U, std::memory_order_relaxed);
        break;
    case ExecuteStatus::NativeFault:
        gTypedRemoteIdentityNativeFaults.fetch_add(1U, std::memory_order_relaxed);
        break;
    case ExecuteStatus::InvalidPayload:
    default:
        gTypedRemoteIdentityInvalidPayloads.fetch_add(1U, std::memory_order_relaxed);
        break;
    }
    TouchVersion();
}

CompositePublicInputDiagnostics CompositePublicInputDiagnosticsSnapshot() noexcept {
    CompositePublicInputDiagnostics result{};
    result.initialized = Initialized();
    result.schedulerHookInstalled = gHookInstalled.load(std::memory_order_acquire);
    result.preQueueBuilderHookInstalled =
        gBuildInputTargetHookInstalled.load(std::memory_order_acquire);
    result.version = gVersion.load(std::memory_order_acquire);
    result.mapped = gCompositePublicInputMapped.load(std::memory_order_acquire);
    result.runtimeEdgeMapped = gCompositePublicInputRuntimeEdgeMapped.load(std::memory_order_acquire);
    result.ownerScopeRuntimeEdgeMapped =
        gCompositePublicInputOwnerScopeRuntimeEdgeMapped.load(std::memory_order_acquire);
    result.preQueueBuilderSeen =
        gCompositePublicInputPreQueueBuilderSeen.load(std::memory_order_acquire);
    result.preQueueBuilderInputReadable =
        gCompositePublicInputPreQueueBuilderInputReadable.load(std::memory_order_acquire);
    result.preQueueBuilderPublic =
        gCompositePublicInputPreQueueBuilderPublic.load(std::memory_order_acquire);
    result.preQueueBuilderInternal =
        gCompositePublicInputPreQueueBuilderInternal.load(std::memory_order_acquire);
    result.preQueueBuilderInternalExactMapped =
        gCompositePublicInputPreQueueBuilderInternalExactMapped.load(std::memory_order_acquire);
    result.preQueueBuilderInternalRuntimeMapped =
        gCompositePublicInputPreQueueBuilderInternalRuntimeMapped.load(std::memory_order_acquire);
    result.preQueueBuilderInternalOwnerScopeMapped =
        gCompositePublicInputPreQueueBuilderInternalOwnerScopeMapped.load(std::memory_order_acquire);
    result.preQueueBuilderUnknownGuid =
        gCompositePublicInputPreQueueBuilderUnknownGuid.load(std::memory_order_acquire);
    result.preQueueBuilderUnknownTraceCount =
        gCompositePublicInputPreQueueBuilderUnknownTraceCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < kPreQueueUnknownTraceCapacity; ++i) {
        const auto& slot = gCompositePublicInputPreQueueBuilderUnknownTrace[i];
        if (slot.ready.load(std::memory_order_acquire) == 0U) continue;
        auto& entry = result.preQueueBuilderUnknownTrace[i];
        entry.valid = true;
        entry.inputShortGuid = slot.inputShortGuid.load(std::memory_order_relaxed);
        entry.callerRva = slot.callerRva.load(std::memory_order_relaxed);
        entry.targetRuntimeKey.entityA = slot.targetRuntimeKeyA.load(std::memory_order_relaxed);
        entry.targetRuntimeKey.entityB = slot.targetRuntimeKeyB.load(std::memory_order_relaxed);
        entry.targetExecution = slot.targetExecution.load(std::memory_order_relaxed);
        entry.targetId.high = slot.targetIdHigh.load(std::memory_order_relaxed);
        entry.targetId.low = slot.targetIdLow.load(std::memory_order_relaxed);
        entry.bridgeOwner.high = slot.bridgeOwnerHigh.load(std::memory_order_relaxed);
        entry.bridgeOwner.low = slot.bridgeOwnerLow.load(std::memory_order_relaxed);
        entry.outerPublicInput = slot.outerPublicInput.load(std::memory_order_relaxed);
    }
    result.preQueueBuilderMapped =
        gCompositePublicInputPreQueueBuilderMapped.load(std::memory_order_acquire);
    result.preQueueBuilderOwnerMapped =
        gCompositePublicInputPreQueueBuilderOwnerMapped.load(std::memory_order_acquire);
    result.preQueueBuilderInterfaceMapped =
        gCompositePublicInputPreQueueBuilderInterfaceMapped.load(std::memory_order_acquire);
    result.preQueueBuilderSubmitted =
        gCompositePublicInputPreQueueBuilderSubmitted.load(std::memory_order_acquire);
    result.preQueueBuilderRemoteSkipped =
        gCompositePublicInputPreQueueBuilderRemoteSkipped.load(std::memory_order_acquire);
    result.preQueueBuilderDelayRejected =
        gCompositePublicInputPreQueueBuilderDelayRejected.load(std::memory_order_acquire);
    result.preQueueBuilderIdentityRejected =
        gCompositePublicInputPreQueueBuilderIdentityRejected.load(std::memory_order_acquire);
    result.preQueueBuilderTypedRejected =
        gCompositePublicInputPreQueueBuilderTypedRejected.load(std::memory_order_acquire);
    result.preQueueBuilderRejected =
        gCompositePublicInputPreQueueBuilderRejected.load(std::memory_order_acquire);
    result.submitted = gCompositePublicInputSubmitted.load(std::memory_order_acquire);
    result.remoteAttempts = gCompositePublicInputRemoteAttempts.load(std::memory_order_acquire);
    result.bridgeQueued = gCompositePublicInputBridgeQueued.load(std::memory_order_acquire);
    result.sourceFallbacks = gCompositePublicInputSourceFallbacks.load(std::memory_order_acquire);
    result.runtimeUnavailable = gCompositePublicInputRuntimeUnavailable.load(std::memory_order_acquire);
    result.rejected = gCompositePublicInputRejected.load(std::memory_order_acquire);
    return result;
}

RuntimeDiagnostics DiagnosticsSnapshot() noexcept {
    RuntimeDiagnostics result{};
    result.initialized = Initialized();
    result.hookInstalled = gHookInstalled.load(std::memory_order_acquire);
    result.version = gVersion.load(std::memory_order_acquire);
    result.initializationCount = gInitializationCount.load(std::memory_order_acquire);
    result.shutdownCount = gShutdownCount.load(std::memory_order_acquire);
    result.observedSchedulerCalls = gObservedSchedulerCalls.load(std::memory_order_acquire);
    result.relevantSourceCalls = gRelevantSourceCalls.load(std::memory_order_acquire);
    result.submittedIntents = gSubmittedIntents.load(std::memory_order_acquire);
    result.unresolvedSource = gUnresolvedSource.load(std::memory_order_acquire);
    result.unresolvedTarget = gUnresolvedTarget.load(std::memory_order_acquire);
    result.callbackEdgesSkipped = gCallbackEdgesSkipped.load(std::memory_order_acquire);
    result.invalidEdgesSkipped = gInvalidEdgesSkipped.load(std::memory_order_acquire);
    result.remoteApplySuppressed = gRemoteApplySuppressed.load(std::memory_order_acquire);
    result.originEchoSuppressed = gOriginEchoSuppressed.load(std::memory_order_acquire);
    result.discardedPublicRootObservations =
        gDiscardedPublicRootObservations.load(std::memory_order_acquire);
    result.echoTableFull = gEchoTableFull.load(std::memory_order_acquire);
    {
        std::lock_guard lock(gLocalObservationMutex);
        result.observationPending = gLocalObservations.Pending();
        result.observationHighWater = gLocalObservations.HighWater();
        result.observationRanges = gLocalObservations.Ranges();
        result.observationOldestSerial = gLocalObservations.OldestSerial();
        result.observationSplitRefused = gLocalObservations.SplitRefused();
        result.observationRemembered = gLocalObservationRemembered;
        result.observationEchoed = gLocalObservationEchoed;
        result.observationRetired = gLocalObservationRetired;
        result.observationMissing = gLocalObservationMissing;
    }
    result.intentQueueRejected = gIntentQueueRejected.load(std::memory_order_acquire);
    result.sourceClassMismatch = gSourceClassMismatch.load(std::memory_order_acquire);
    result.executeAttempts = gExecuteAttempts.load(std::memory_order_acquire);
    result.queuedRemoteEdges = gQueuedRemoteEdges.load(std::memory_order_acquire);
    result.ws3PreQueueSeen = gWs3PreQueueSeen.load(std::memory_order_acquire);
    result.ws3PreQueueInputReadable = gWs3PreQueueInputReadable.load(std::memory_order_acquire);
    result.ws3PreQueueTargetWorldResolved = gWs3PreQueueTargetWorldResolved.load(std::memory_order_acquire);
    result.ws3PreQueueTargetWorldUnresolved = gWs3PreQueueTargetWorldUnresolved.load(std::memory_order_acquire);
    result.ws3PreQueueLastInput = gWs3PreQueueLastInput.load(std::memory_order_acquire);
    result.ws3PreQueueLastCallerRva = gWs3PreQueueLastCallerRva.load(std::memory_order_acquire);
    result.ws3CanonicalPreQueueHintStored =
        gWs3CanonicalPreQueueHintStored.load(std::memory_order_acquire);
    result.ws3CanonicalPreQueueHintConsumed =
        gWs3CanonicalPreQueueHintConsumed.load(std::memory_order_acquire);
    result.ws3CanonicalPreQueueHintMissed =
        gWs3CanonicalPreQueueHintMissed.load(std::memory_order_acquire);
    result.ws3CanonicalPreQueueSubmitAttempt =
        gWs3CanonicalPreQueueSubmitAttempt.load(std::memory_order_acquire);
    result.ws3CanonicalPreQueueSubmitSuccess =
        gWs3CanonicalPreQueueSubmitSuccess.load(std::memory_order_acquire);
    result.ws3CanonicalPreQueueAdmissionSuppressed =
        gWs3CanonicalPreQueueAdmissionSuppressed.load(std::memory_order_acquire);
    result.ws3CanonicalPreQueueSubmitFailed =
        gWs3CanonicalPreQueueSubmitFailed.load(std::memory_order_acquire);
    result.ws3CanonicalQueueCandidateSeen =
        gWs3CanonicalQueueCandidateSeen.load(std::memory_order_acquire);
    result.ws3CanonicalQueueMapped =
        gWs3CanonicalQueueMapped.load(std::memory_order_acquire);
    result.ws3CanonicalQueueLocallyExecuted =
        gWs3CanonicalQueueLocallyExecuted.load(std::memory_order_acquire);
    result.ws3CanonicalQueueAdmissionSuppressed =
        gWs3CanonicalQueueAdmissionSuppressed.load(std::memory_order_acquire);
    result.ws3QueueSeen = gWs3QueueSeen.load(std::memory_order_acquire);
    result.ws3QueueDescendantSuppressed = gWs3QueueDescendantSuppressed.load(std::memory_order_acquire);
    result.ws3QueueCallbackRejected = gWs3QueueCallbackRejected.load(std::memory_order_acquire);
    result.ws3QueueDescribeRejected = gWs3QueueDescribeRejected.load(std::memory_order_acquire);
    result.ws3QueueRemoteNatural = gWs3QueueRemoteNatural.load(std::memory_order_acquire);
    result.ws3QueueRemoteApplySuppressed = gWs3QueueRemoteApplySuppressed.load(std::memory_order_acquire);
    result.ws3QueueDeferredComposite = gWs3QueueDeferredComposite.load(std::memory_order_acquire);
    result.ws3QueueStateOwned = gWs3QueueStateOwned.load(std::memory_order_acquire);
    result.ws3QueueEventEligible = gWs3QueueEventEligible.load(std::memory_order_acquire);
    result.ws3QueueUnknownEligible = gWs3QueueUnknownEligible.load(std::memory_order_acquire);
    result.ws3QueueSubmitRetailOnly = gWs3QueueSubmitRetailOnly.load(std::memory_order_acquire);
    result.ws3QueueSubmitAwaitAuthority = gWs3QueueSubmitAwaitAuthority.load(std::memory_order_acquire);
    result.ws3QueueSubmitAuthoritySuppressed = gWs3QueueSubmitAuthoritySuppressed.load(std::memory_order_acquire);
    result.ws3QueueSubmitLocalRoot = gWs3QueueSubmitLocalRoot.load(std::memory_order_acquire);
    result.ws3QueueLastSchedulerParameter = gWs3QueueLastSchedulerParameter.load(std::memory_order_acquire);
    result.ws3QueueLastTargetInput = gWs3QueueLastTargetInput.load(std::memory_order_acquire);
    result.ws3QueueLastCallerRva = gWs3QueueLastCallerRva.load(std::memory_order_acquire);
    result.ws3QueueLastGate = gWs3QueueLastGate.load(std::memory_order_acquire);
    result.ws3TypedSeen = gWs3TypedSeen.load(std::memory_order_acquire);
    result.ws3TypedDescendantSuppressed = gWs3TypedDescendantSuppressed.load(std::memory_order_acquire);
    result.ws3TypedWorldPairResolved = gWs3TypedWorldPairResolved.load(std::memory_order_acquire);
    result.ws3TypedWorldPairUnresolved = gWs3TypedWorldPairUnresolved.load(std::memory_order_acquire);
    result.ws3TypedRootRetailOnly = gWs3TypedRootRetailOnly.load(std::memory_order_acquire);
    result.ws3TypedRootAwaitAuthority = gWs3TypedRootAwaitAuthority.load(std::memory_order_acquire);
    result.ws3TypedRootAuthoritySuppressed = gWs3TypedRootAuthoritySuppressed.load(std::memory_order_acquire);
    result.ws3TypedRootLocal = gWs3TypedRootLocal.load(std::memory_order_acquire);
    result.ws3TypedReceiverLocalSuppressed = gWs3TypedReceiverLocalSuppressed.load(std::memory_order_acquire);
    result.ws3TypedLastReceiverLocalSuppressedType = gWs3TypedLastReceiverLocalSuppressedType.load(std::memory_order_acquire);
    result.ws3TypedLastMessageType = gWs3TypedLastMessageType.load(std::memory_order_acquire);
    result.ws3TypedLastPayloadBytes = gWs3TypedLastPayloadBytes.load(std::memory_order_acquire);
    result.ws3TypedLastWorldMask = gWs3TypedLastWorldMask.load(std::memory_order_acquire);
    result.ws3TypedLastCallerRva = gWs3TypedLastCallerRva.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairSeen = gWs3TypedInteractionPairSeen.load(std::memory_order_acquire);
    result.ws3TypedInteractionPair43 = gWs3TypedInteractionPair43.load(std::memory_order_acquire);
    result.ws3TypedInteractionPair44 = gWs3TypedInteractionPair44.load(std::memory_order_acquire);
    result.ws3TypedInteractionPair43LastCallerRva = gWs3TypedInteractionPair43LastCallerRva.load(std::memory_order_acquire);
    result.ws3TypedInteractionPair44LastCallerRva = gWs3TypedInteractionPair44LastCallerRva.load(std::memory_order_acquire);
    result.ws3TypedInteractionPair43LastCausalSource = gWs3TypedInteractionPair43LastCausalSource.load(std::memory_order_acquire);
    result.ws3TypedInteractionPair44LastCausalSource = gWs3TypedInteractionPair44LastCausalSource.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastType = gWs3TypedInteractionPairLastType.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastCallerRva = gWs3TypedInteractionPairLastCallerRva.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastSourcePrimitive = gWs3TypedInteractionPairLastSourcePrimitive.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastTargetPrimitive = gWs3TypedInteractionPairLastTargetPrimitive.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastSourceDomain = gWs3TypedInteractionPairLastSourceDomain.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastTargetDomain = gWs3TypedInteractionPairLastTargetDomain.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastCausalSource = gWs3TypedInteractionPairLastCausalSource.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastSourceHigh = gWs3TypedInteractionPairLastSourceHigh.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastSourceLow = gWs3TypedInteractionPairLastSourceLow.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastTargetHigh = gWs3TypedInteractionPairLastTargetHigh.load(std::memory_order_acquire);
    result.ws3TypedInteractionPairLastTargetLow = gWs3TypedInteractionPairLastTargetLow.load(std::memory_order_acquire);
    result.compositePublicInputMapped = gCompositePublicInputMapped.load(std::memory_order_acquire);
    result.compositePublicInputSubmitted = gCompositePublicInputSubmitted.load(std::memory_order_acquire);
    result.compositePublicInputRemoteAttempts = gCompositePublicInputRemoteAttempts.load(std::memory_order_acquire);
    result.compositePublicInputBridgeQueued = gCompositePublicInputBridgeQueued.load(std::memory_order_acquire);
    result.compositePublicInputSourceFallbacks = gCompositePublicInputSourceFallbacks.load(std::memory_order_acquire);
    result.compositePublicInputRuntimeUnavailable = gCompositePublicInputRuntimeUnavailable.load(std::memory_order_acquire);
    result.compositePublicInputRejected = gCompositePublicInputRejected.load(std::memory_order_acquire);
    result.nativeFaults = gNativeFaults.load(std::memory_order_acquire);
    result.activeDetours = gActiveDetours.load(std::memory_order_acquire);
    result.typedMessageHookInstalled = gTypedMessageHookInstalled.load(std::memory_order_acquire);
    result.observedTypedMessages = gObservedTypedMessages.load(std::memory_order_acquire);
    result.relevantTypedMessages = gRelevantTypedMessages.load(std::memory_order_acquire);
    result.submittedTypedMessages = gSubmittedTypedMessages.load(std::memory_order_acquire);
    result.typedPayloadBytes = gTypedPayloadBytes.load(std::memory_order_acquire);
    result.oversizedTypedMessagesSkipped = gOversizedTypedMessagesSkipped.load(std::memory_order_acquire);
    result.invalidTypedMessagesSkipped = gInvalidTypedMessagesSkipped.load(std::memory_order_acquire);
    result.unresolvedTypedSource = gUnresolvedTypedSource.load(std::memory_order_acquire);
    result.unresolvedTypedTarget = gUnresolvedTypedTarget.load(std::memory_order_acquire);
    result.typedRuntimeKeyAnchoredByTarget = gTypedRuntimeKeyAnchoredByTarget.load(std::memory_order_acquire);
    result.typedRuntimeKeyAnchoredBySource = gTypedRuntimeKeyAnchoredBySource.load(std::memory_order_acquire);
    result.typedRuntimeKeyNoAnchorSkipped = gTypedRuntimeKeyNoAnchorSkipped.load(std::memory_order_acquire);
    result.typedRuntimeKeySafetyBlocked = gTypedRuntimeKeySafetyBlocked.load(std::memory_order_acquire);
    result.typedBlockedMessageType = gTypedBlockedMessageType.load(std::memory_order_acquire);
    result.typedBlockedPayloadBytes = gTypedBlockedPayloadBytes.load(std::memory_order_acquire);
    result.typedBlockedSourceKey.entityA = gTypedBlockedSourceA.load(std::memory_order_acquire);
    result.typedBlockedSourceKey.entityB = gTypedBlockedSourceB.load(std::memory_order_acquire);
    result.typedBlockedTargetKey.entityA = gTypedBlockedTargetA.load(std::memory_order_acquire);
    result.typedBlockedTargetKey.entityB = gTypedBlockedTargetB.load(std::memory_order_acquire);
    result.typedBlockedMissingMask = gTypedBlockedMissingMask.load(std::memory_order_acquire);
    result.typedRemoteApplySuppressed = gTypedRemoteApplySuppressed.load(std::memory_order_acquire);
    result.typedOriginEchoSuppressed = gTypedOriginEchoSuppressed.load(std::memory_order_acquire);
    result.typedContentMatchSuppressed = gTypedContentMatchSuppressed.load(std::memory_order_acquire);
    result.typedIntentQueueRejected = gTypedIntentQueueRejected.load(std::memory_order_acquire);
    result.typedExecuteAttempts = gTypedExecuteAttempts.load(std::memory_order_acquire);
    result.queuedRemoteTypedMessages = gQueuedRemoteTypedMessages.load(std::memory_order_acquire);
    result.typedNativeFaults = gTypedNativeFaults.load(std::memory_order_acquire);
    const auto typedOriginUntil = gTypedMessageOriginProbeUntilTickMs.load(std::memory_order_acquire);
    const auto typedOriginNow = static_cast<std::uint64_t>(GetTickCount64());
    result.typedMessageOriginProbeActive =
        typedOriginUntil != 0U && typedOriginNow <= typedOriginUntil;
    result.typedMessageOriginCandidates =
        gTypedMessageOriginCandidates.load(std::memory_order_acquire);
    result.typedMessageOriginExactMatches =
        gTypedMessageOriginExactMatches.load(std::memory_order_acquire);
    result.typedMessageOriginCallVerified =
        gTypedMessageOriginCallVerified.load(std::memory_order_acquire);
    result.typedMessageOriginReadFailures =
        gTypedMessageOriginReadFailures.load(std::memory_order_acquire);
    result.typedMessageOriginMessageType =
        gTypedMessageOriginMessageType.load(std::memory_order_acquire);
    result.typedMessageOriginPayloadBytes =
        gTypedMessageOriginPayloadBytes.load(std::memory_order_acquire);
    result.typedMessageOriginSourceKey.entityA =
        gTypedMessageOriginSourceA.load(std::memory_order_acquire);
    result.typedMessageOriginSourceKey.entityB =
        gTypedMessageOriginSourceB.load(std::memory_order_acquire);
    result.typedMessageOriginTargetKey.entityA =
        gTypedMessageOriginTargetA.load(std::memory_order_acquire);
    result.typedMessageOriginTargetKey.entityB =
        gTypedMessageOriginTargetB.load(std::memory_order_acquire);
    result.typedMessageOriginWorldMask =
        gTypedMessageOriginWorldMask.load(std::memory_order_acquire);
    result.typedMessageOriginDelaySeconds = std::bit_cast<double>(
        gTypedMessageOriginDelayBits.load(std::memory_order_acquire));
    result.typedMessageOriginFirstCallSiteRva =
        gTypedMessageOriginFirstCallSiteRva.load(std::memory_order_acquire);
    result.typedMessageOriginLastCallSiteRva =
        gTypedMessageOriginLastCallSiteRva.load(std::memory_order_acquire);
    result.typedMessageOriginCallSiteVaried =
        gTypedMessageOriginCallSiteVaried.load(std::memory_order_acquire) != 0U;
    result.typedMessageOriginThreadId =
        gTypedMessageOriginThreadId.load(std::memory_order_acquire);
    result.typedMessageOriginTickMs =
        gTypedMessageOriginTickMs.load(std::memory_order_acquire);
    const auto typedRemoteUntil =
        gTypedRemoteIdentityProbeUntilTickMs.load(std::memory_order_acquire);
    const auto typedRemoteNow = static_cast<std::uint64_t>(GetTickCount64());
    result.typedRemoteIdentityProbeActive =
        typedRemoteUntil != 0U && typedRemoteNow <= typedRemoteUntil;
    result.typedRemoteIdentityCandidates =
        gTypedRemoteIdentityCandidates.load(std::memory_order_acquire);
    result.typedRemoteIdentityExactMatches =
        gTypedRemoteIdentityExactMatches.load(std::memory_order_acquire);
    result.typedRemoteIdentityMessageType =
        gTypedRemoteIdentityMessageType.load(std::memory_order_acquire);
    result.typedRemoteIdentityPayloadBytes =
        gTypedRemoteIdentityPayloadBytes.load(std::memory_order_acquire);
    result.typedRemoteIdentitySourceWorld.high =
        gTypedRemoteIdentitySourceWorldHigh.load(std::memory_order_acquire);
    result.typedRemoteIdentitySourceWorld.low =
        gTypedRemoteIdentitySourceWorldLow.load(std::memory_order_acquire);
    result.typedRemoteIdentityTargetWorld.high =
        gTypedRemoteIdentityTargetWorldHigh.load(std::memory_order_acquire);
    result.typedRemoteIdentityTargetWorld.low =
        gTypedRemoteIdentityTargetWorldLow.load(std::memory_order_acquire);
    result.typedRemoteIdentitySenderSourceKey.entityA =
        gTypedRemoteIdentitySenderSourceA.load(std::memory_order_acquire);
    result.typedRemoteIdentitySenderSourceKey.entityB =
        gTypedRemoteIdentitySenderSourceB.load(std::memory_order_acquire);
    result.typedRemoteIdentitySenderTargetKey.entityA =
        gTypedRemoteIdentitySenderTargetA.load(std::memory_order_acquire);
    result.typedRemoteIdentitySenderTargetKey.entityB =
        gTypedRemoteIdentitySenderTargetB.load(std::memory_order_acquire);
    result.typedRemoteIdentityLocalSourceKey.entityA =
        gTypedRemoteIdentityLocalSourceA.load(std::memory_order_acquire);
    result.typedRemoteIdentityLocalSourceKey.entityB =
        gTypedRemoteIdentityLocalSourceB.load(std::memory_order_acquire);
    result.typedRemoteIdentityLocalTargetKey.entityA =
        gTypedRemoteIdentityLocalTargetA.load(std::memory_order_acquire);
    result.typedRemoteIdentityLocalTargetKey.entityB =
        gTypedRemoteIdentityLocalTargetB.load(std::memory_order_acquire);
    result.typedRemoteIdentityMask =
        gTypedRemoteIdentityMask.load(std::memory_order_acquire);
    result.typedRemoteIdentityExecuteReached =
        gTypedRemoteIdentityExecuteReached.load(std::memory_order_acquire);
    result.typedRemoteIdentityQueued =
        gTypedRemoteIdentityQueued.load(std::memory_order_acquire);
    result.typedRemoteIdentityRuntimeUnavailable =
        gTypedRemoteIdentityRuntimeUnavailable.load(std::memory_order_acquire);
    result.typedRemoteIdentityNativeFaults =
        gTypedRemoteIdentityNativeFaults.load(std::memory_order_acquire);
    result.typedRemoteIdentityInvalidPayloads =
        gTypedRemoteIdentityInvalidPayloads.load(std::memory_order_acquire);
    const auto logicDownUntil = gLogicDownstreamProbeUntilTickMs.load(std::memory_order_acquire);
    const auto logicDownNow = static_cast<std::uint64_t>(GetTickCount64());
    result.logicDownstreamProbeActive = logicDownUntil != 0U && logicDownNow <= logicDownUntil;
    result.logicDownstreamScopes = gLogicDownstreamScopes.load(std::memory_order_acquire);
    result.logicDownstreamEdges = gLogicDownstreamEdges.load(std::memory_order_acquire);
    result.logicDownstreamValidEdges = gLogicDownstreamValidEdges.load(std::memory_order_acquire);
    result.logicDownstreamCallbackEdges = gLogicDownstreamCallbackEdges.load(std::memory_order_acquire);
    result.logicDownstreamReadFailures = gLogicDownstreamReadFailures.load(std::memory_order_acquire);
    result.logicDownstreamWorldMask = gLogicDownstreamWorldMask.load(std::memory_order_acquire);
    result.logicDownstreamSourceKey.entityA = gLogicDownstreamSourceA.load(std::memory_order_acquire);
    result.logicDownstreamSourceKey.entityB = gLogicDownstreamSourceB.load(std::memory_order_acquire);
    result.logicDownstreamTargetKey.entityA = gLogicDownstreamTargetA.load(std::memory_order_acquire);
    result.logicDownstreamTargetKey.entityB = gLogicDownstreamTargetB.load(std::memory_order_acquire);
    result.logicDownstreamSchedulerParameter = gLogicDownstreamSchedulerParameter.load(std::memory_order_acquire);
    result.logicDownstreamTargetInput = gLogicDownstreamTargetInput.load(std::memory_order_acquire);
    result.logicDownstreamCallbackFlags = gLogicDownstreamCallbackFlags.load(std::memory_order_acquire);
    result.logicDownstreamFirstCallSiteRva = gLogicDownstreamFirstCallSiteRva.load(std::memory_order_acquire);
    result.logicDownstreamLastCallSiteRva = gLogicDownstreamLastCallSiteRva.load(std::memory_order_acquire);
    result.logicDownstreamCallSiteVaried = gLogicDownstreamCallSiteVaried.load(std::memory_order_acquire) != 0U;
    result.logicDownstreamSequenceHash = gLogicDownstreamSequenceHash.load(std::memory_order_acquire);
    result.logicDownstreamThreadId = gLogicDownstreamThreadId.load(std::memory_order_acquire);
    result.logicDownstreamTickMs = gLogicDownstreamTickMs.load(std::memory_order_acquire);
    const auto postNow = static_cast<std::uint64_t>(GetTickCount64());
    const auto postUntil = gPostClearWindowUntilTickMs.load(std::memory_order_acquire);
    result.postClearWindowActive = postUntil != 0U && postNow <= postUntil;
    result.postClearArms = gPostClearArms.load(std::memory_order_acquire);
    result.postClearSeen = gPostClearSeen.load(std::memory_order_acquire);
    result.postClearAnchorMatches = gPostClearAnchorMatches.load(std::memory_order_acquire);
    result.postClearValidEdges = gPostClearValidEdges.load(std::memory_order_acquire);
    result.postClearCallbackEdges = gPostClearCallbackEdges.load(std::memory_order_acquire);
    result.postClearReadFailures = gPostClearReadFailures.load(std::memory_order_acquire);
    result.postClearInspectLimitDrops = gPostClearInspectLimitDrops.load(std::memory_order_acquire);
    result.postClearAnchorRole = gPostClearAnchorRole.load(std::memory_order_acquire);
    result.postClearWorldMask = gPostClearWorldMask.load(std::memory_order_acquire);
    result.postClearSourceKey.entityA = gPostClearSourceA.load(std::memory_order_acquire);
    result.postClearSourceKey.entityB = gPostClearSourceB.load(std::memory_order_acquire);
    result.postClearTargetKey.entityA = gPostClearTargetA.load(std::memory_order_acquire);
    result.postClearTargetKey.entityB = gPostClearTargetB.load(std::memory_order_acquire);
    result.postClearSchedulerParameter = gPostClearSchedulerParameter.load(std::memory_order_acquire);
    result.postClearTargetInput = gPostClearTargetInput.load(std::memory_order_acquire);
    result.postClearCallbackFlags = gPostClearCallbackFlags.load(std::memory_order_acquire);
    result.postClearFirstCallSiteRva = gPostClearFirstCallSiteRva.load(std::memory_order_acquire);
    result.postClearLastCallSiteRva = gPostClearLastCallSiteRva.load(std::memory_order_acquire);
    result.postClearCallSiteVaried = gPostClearCallSiteVaried.load(std::memory_order_acquire) != 0U;
    result.postClearSequenceHash = gPostClearSequenceHash.load(std::memory_order_acquire);
    result.postClearFirstDeltaMs = gPostClearFirstDeltaMs.load(std::memory_order_acquire);
    result.postClearLastDeltaMs = gPostClearLastDeltaMs.load(std::memory_order_acquire);
    result.postClearThreadId = gPostClearThreadId.load(std::memory_order_acquire);
    result.postClearTickMs = gPostClearTickMs.load(std::memory_order_acquire);
    result.clearDispatchProbeActive = Fix315DownstreamProbeActive();
    result.clearDispatchCandidates = gClearDispatchCandidates.load(std::memory_order_acquire);
    result.clearDispatchExactMatches = gClearDispatchExactMatches.load(std::memory_order_acquire);
    result.clearDispatchReadFailures = gClearDispatchReadFailures.load(std::memory_order_acquire);
    result.clearDispatchVtableRva = gClearDispatchVtableRva.load(std::memory_order_acquire);
    result.clearDispatchSlot = kFix317LogicSwitchDispatchSlot;
    result.clearDispatchTargetRva = gClearDispatchTargetRva.load(std::memory_order_acquire);
    result.clearDispatchFirstTargetRva = gClearDispatchFirstTargetRva.load(std::memory_order_acquire);
    result.clearDispatchLastTargetRva = gClearDispatchLastTargetRva.load(std::memory_order_acquire);
    result.clearDispatchTargetVaried = gClearDispatchTargetVaried.load(std::memory_order_acquire) != 0U;
    result.clearDispatchBeforeState = gClearDispatchBeforeState.load(std::memory_order_acquire);
    result.clearDispatchAfterState = gClearDispatchAfterState.load(std::memory_order_acquire);
    result.clearDispatchThreadId = gClearDispatchThreadId.load(std::memory_order_acquire);
    result.clearDispatchTickMs = gClearDispatchTickMs.load(std::memory_order_acquire);
    result.clearCodeProbeActive = Fix315DownstreamProbeActive();
    result.clearCodeCandidates = gClearCodeCandidates.load(std::memory_order_acquire);
    result.clearCodeExactMatches = gClearCodeExactMatches.load(std::memory_order_acquire);
    result.clearCodeReadFailures = gClearCodeReadFailures.load(std::memory_order_acquire);
    result.clearCodeTargetRva = gClearCodeTargetRva.load(std::memory_order_acquire);
    result.clearCodeWord0 = gClearCodeWord0.load(std::memory_order_acquire);
    result.clearCodeWord1 = gClearCodeWord1.load(std::memory_order_acquire);
    result.clearCodeBranchKind = gClearCodeBranchKind.load(std::memory_order_acquire);
    result.clearCodeBranchTargetRva = gClearCodeBranchTargetRva.load(std::memory_order_acquire);
    result.clearCodeFinalTargetRva = gClearCodeFinalTargetRva.load(std::memory_order_acquire);
    result.clearCodeJumpDepth = gClearCodeJumpDepth.load(std::memory_order_acquire);
    result.clearCodeHash = gClearCodeHash.load(std::memory_order_acquire);
    result.clearCodeThreadId = gClearCodeThreadId.load(std::memory_order_acquire);
    result.clearCodeTickMs = gClearCodeTickMs.load(std::memory_order_acquire);
    result.logicSwitchHookInstalled = gLogicSwitchHookInstalled.load(std::memory_order_acquire);
    result.logicSwitchObserved = gLogicSwitchObserved.load(std::memory_order_acquire);
    result.logicSwitchWorldStates = gLogicSwitchWorldStates.load(std::memory_order_acquire);
    result.logicSwitchSubmitted = gLogicSwitchSubmitted.load(std::memory_order_acquire);
    result.logicSwitchUnresolved = gLogicSwitchUnresolved.load(std::memory_order_acquire);
    result.logicSwitchRemoteSuppressed = gLogicSwitchRemoteSuppressed.load(std::memory_order_acquire);
    result.logicSwitchIntentRejected = gLogicSwitchIntentRejected.load(std::memory_order_acquire);
    result.logicSwitchExecuteAttempts = gLogicSwitchExecuteAttempts.load(std::memory_order_acquire);
    result.logicSwitchAppliedRemote = gLogicSwitchAppliedRemote.load(std::memory_order_acquire);
    result.logicSwitchRuntimeUnavailable = gLogicSwitchRuntimeUnavailable.load(std::memory_order_acquire);
    result.logicSwitchNativeFaults = gLogicSwitchNativeFaults.load(std::memory_order_acquire);
    result.logicSwitchBindingRegistrations = gLogicSwitchBindingRegistrations.load(std::memory_order_acquire);
    result.logicSwitchBindingHits = gLogicSwitchBindingHits.load(std::memory_order_acquire);
    result.logicSwitchBindingMisses = gLogicSwitchBindingMisses.load(std::memory_order_acquire);
    result.logicSwitchBindingDrops = gLogicSwitchBindingDrops.load(std::memory_order_acquire);
    result.logicSwitchContextReplays = gLogicSwitchContextReplays.load(std::memory_order_acquire);
    result.logicSwitchSyntheticContextReplays = gLogicSwitchSyntheticContextReplays.load(std::memory_order_acquire);
    result.logicSwitchSafetyBlocked = gLogicSwitchSafetyBlocked.load(std::memory_order_acquire);
    result.logicSwitchContextWorld = gLogicSwitchContextWorld.load(std::memory_order_acquire);
    result.logicSwitchContextNonWorld = gLogicSwitchContextNonWorld.load(std::memory_order_acquire);
    result.logicSwitchLastContextKey.entityA = gLogicSwitchLastContextA.load(std::memory_order_acquire);
    result.logicSwitchLastContextKey.entityB = gLogicSwitchLastContextB.load(std::memory_order_acquire);
    result.logicSwitchLastStateKey.entityA = gLogicSwitchLastStateA.load(std::memory_order_acquire);
    result.logicSwitchLastStateKey.entityB = gLogicSwitchLastStateB.load(std::memory_order_acquire);
    result.logicSwitchLastEnabled = gLogicSwitchLastEnabled.load(std::memory_order_acquire) != 0U;
    result.logicExactTargetMatches = gLogicExactTargetMatches.load(std::memory_order_acquire);
    result.logicExactHolderReadable = gLogicExactHolderReadable.load(std::memory_order_acquire);
    result.logicExactCallbackPresent = gLogicExactCallbackPresent.load(std::memory_order_acquire);
    result.logicExactRawReadable = gLogicExactRawReadable.load(std::memory_order_acquire);
    result.logicExactRawReadFailures = gLogicExactRawReadFailures.load(std::memory_order_acquire);
    result.logicExactCallbackObjectPresent = gLogicExactCallbackObjectPresent.load(std::memory_order_acquire);
    result.logicExactCallbackKeyResolved = gLogicExactCallbackKeyResolved.load(std::memory_order_acquire);
    result.logicExactCallbackExecutionReadable = gLogicExactCallbackExecutionReadable.load(std::memory_order_acquire);
    result.logicExactInput = gLogicExactInput.load(std::memory_order_acquire);
    result.logicExactFlags = gLogicExactFlags.load(std::memory_order_acquire);
    result.logicExactRefCount = gLogicExactRefCount.load(std::memory_order_acquire);
    result.logicExactNestedPresent = gLogicExactNestedPresent.load(std::memory_order_acquire) != 0U;
    result.logicExactPayloadPresent = gLogicExactPayloadPresent.load(std::memory_order_acquire) != 0U;
    result.logicExactCallbackKey.entityA = gLogicExactCallbackKeyA.load(std::memory_order_acquire);
    result.logicExactCallbackKey.entityB = gLogicExactCallbackKeyB.load(std::memory_order_acquire);
    result.logicExactCallbackExecutionVtableRva = gLogicExactCallbackExecutionVtableRva.load(std::memory_order_acquire);
    result.logicExactContextSameAsState = gLogicExactContextSameAsState.load(std::memory_order_acquire) != 0U;
    result.logicExactEnabled = gLogicExactEnabled.load(std::memory_order_acquire) != 0U;
    result.characterCallbackOriginHookInstalled =
        gCharacterCallbackOriginHookInstalled.load(std::memory_order_acquire);
    const auto originUntil = gCharacterCallbackOriginProbeUntilTickMs.load(std::memory_order_acquire);
    result.characterCallbackOriginProbeActive = originUntil != 0U &&
        static_cast<std::uint64_t>(GetTickCount64()) <= originUntil;
    result.characterCallbackOriginCandidates =
        gCharacterCallbackOriginCandidates.load(std::memory_order_acquire);
    result.characterCallbackOriginExactKeyMatches =
        gCharacterCallbackOriginExactKeyMatches.load(std::memory_order_acquire);
    result.characterCallbackOriginCallVerified =
        gCharacterCallbackOriginCallVerified.load(std::memory_order_acquire);
    result.characterCallbackOriginReadFailures =
        gCharacterCallbackOriginReadFailures.load(std::memory_order_acquire);
    result.characterCallbackOriginSourceKey.entityA =
        gCharacterCallbackOriginSourceA.load(std::memory_order_acquire);
    result.characterCallbackOriginSourceKey.entityB =
        gCharacterCallbackOriginSourceB.load(std::memory_order_acquire);
    result.characterCallbackOriginExecutionVtableRva =
        gCharacterCallbackOriginExecutionVtableRva.load(std::memory_order_acquire);
    result.characterCallbackOriginFlags =
        gCharacterCallbackOriginFlags.load(std::memory_order_acquire);
    result.characterCallbackOriginPayloadBytes =
        gCharacterCallbackOriginPayloadBytes.load(std::memory_order_acquire);
    result.characterCallbackOriginFirstCallSiteRva =
        gCharacterCallbackOriginFirstCallSiteRva.load(std::memory_order_acquire);
    result.characterCallbackOriginLastCallSiteRva =
        gCharacterCallbackOriginLastCallSiteRva.load(std::memory_order_acquire);
    result.characterCallbackOriginCallSiteVaried =
        gCharacterCallbackOriginCallSiteVaried.load(std::memory_order_acquire) != 0U;
    result.characterCallbackOriginThreadId =
        gCharacterCallbackOriginThreadId.load(std::memory_order_acquire);
    result.characterCallbackOriginTickMs =
        gCharacterCallbackOriginTickMs.load(std::memory_order_acquire);
    result.schedulerCausalEdgesRecorded = gSchedulerCausalEdgesRecorded.load(std::memory_order_acquire);
    result.logicSwitchCausalNestedMatches = gLogicSwitchCausalNestedMatches.load(std::memory_order_acquire);
    result.logicSwitchCausalEndpointMatches = gLogicSwitchCausalEndpointMatches.load(std::memory_order_acquire);
    result.logicSwitchCausalRecentMatches = gLogicSwitchCausalRecentMatches.load(std::memory_order_acquire);
    result.logicSwitchCausalCallbackMatches = gLogicSwitchCausalCallbackMatches.load(std::memory_order_acquire);
    result.logicSwitchCausalNoMatches = gLogicSwitchCausalNoMatches.load(std::memory_order_acquire);
    result.logicSwitchCausalLastAgeMs = gLogicSwitchCausalLastAgeMs.load(std::memory_order_acquire);
    result.logicSwitchCausalLastSource.high = gLogicSwitchCausalLastSourceHigh.load(std::memory_order_acquire);
    result.logicSwitchCausalLastSource.low = gLogicSwitchCausalLastSourceLow.load(std::memory_order_acquire);
    result.logicSwitchCausalLastTarget.high = gLogicSwitchCausalLastTargetHigh.load(std::memory_order_acquire);
    result.logicSwitchCausalLastTarget.low = gLogicSwitchCausalLastTargetLow.load(std::memory_order_acquire);
    result.logicSwitchCausalLastState.high = gLogicSwitchCausalLastStateHigh.load(std::memory_order_acquire);
    result.logicSwitchCausalLastState.low = gLogicSwitchCausalLastStateLow.load(std::memory_order_acquire);
    result.logicSwitchCausalLastSchedulerParameter = gLogicSwitchCausalLastSchedulerParameter.load(std::memory_order_acquire);
    result.logicSwitchCausalLastTargetInput = gLogicSwitchCausalLastTargetInput.load(std::memory_order_acquire);
    result.logicSwitchCausalLastCallback =
        (gLogicSwitchCausalLastFlags.load(std::memory_order_acquire) & kSchedulerCausalCallbackFlag) != 0U;
    result.logicSwitchCausalLastEnabled =
        gLogicSwitchCausalLastEnabled.load(std::memory_order_acquire) != 0U;
    result.logicSwitchCausalLastCallbackEntity.high =
        gLogicSwitchCausalLastCallbackHigh.load(std::memory_order_acquire);
    result.logicSwitchCausalLastCallbackEntity.low =
        gLogicSwitchCausalLastCallbackLow.load(std::memory_order_acquire);
    result.logicSwitchCausalLastCallbackExecutionVtableRva =
        gLogicSwitchCausalLastCallbackExecutionVtableRva.load(std::memory_order_acquire);
    result.logicSwitchCausalLastQueueCallerRva =
        gLogicSwitchCausalLastQueueCallerRva.load(std::memory_order_acquire);
    result.logicSwitchCausalLastSourceRuntimeKeyA = gLogicSwitchCausalLastSourceRuntimeKeyA.load(std::memory_order_acquire);
    result.logicSwitchCausalLastSourceRuntimeKeyB = gLogicSwitchCausalLastSourceRuntimeKeyB.load(std::memory_order_acquire);
    result.logicSwitchCausalLastTargetRuntimeKeyA = gLogicSwitchCausalLastTargetRuntimeKeyA.load(std::memory_order_acquire);
    result.logicSwitchCausalLastTargetRuntimeKeyB = gLogicSwitchCausalLastTargetRuntimeKeyB.load(std::memory_order_acquire);
    result.logicSwitchCausalLastCallbackRuntimeKeyA = gLogicSwitchCausalLastCallbackRuntimeKeyA.load(std::memory_order_acquire);
    result.logicSwitchCausalLastCallbackRuntimeKeyB = gLogicSwitchCausalLastCallbackRuntimeKeyB.load(std::memory_order_acquire);
    result.logicSwitchCausalLastStateRuntimeKeyA = gLogicSwitchCausalLastStateRuntimeKeyA.load(std::memory_order_acquire);
    result.logicSwitchCausalLastStateRuntimeKeyB = gLogicSwitchCausalLastStateRuntimeKeyB.load(std::memory_order_acquire);
    const auto causalFlags = gLogicSwitchCausalLastFlags.load(std::memory_order_acquire);
    result.logicSwitchCausalLastCallbackExecutionReadable =
        (causalFlags & kSchedulerCausalCallbackExecutionReadableFlag) != 0U;
    result.logicSwitchCausalLastCallbackExecutionPresent =
        (causalFlags & kSchedulerCausalCallbackExecutionPresentFlag) != 0U;
    result.logicSwitchCausalLastSourceRuntimeKeyValid =
        (causalFlags & kSchedulerCausalSourceRuntimeKeyFlag) != 0U;
    result.logicSwitchCausalLastTargetRuntimeKeyValid =
        (causalFlags & kSchedulerCausalTargetRuntimeKeyFlag) != 0U;
    result.logicSwitchCausalLastCallbackRuntimeKeyValid =
        (causalFlags & kSchedulerCausalCallbackRuntimeKeyFlag) != 0U;
    result.logicSwitchCausalLastCallbackWorldEntity =
        (causalFlags & kSchedulerCausalCallbackWorldEntityFlag) != 0U;
    result.logicSwitchCausalLastCallbackSameAsSource =
        (causalFlags & kSchedulerCausalCallbackSameSourceFlag) != 0U;
    result.logicSwitchCausalLastCallbackSameAsTarget =
        (causalFlags & kSchedulerCausalCallbackSameTargetFlag) != 0U;
    result.callbackRecordsObserved = gCallbackRecordsObserved.load(std::memory_order_acquire);
    result.callbackExecutionReadable = gCallbackExecutionReadable.load(std::memory_order_acquire);
    result.callbackWorldEntityResolved = gCallbackWorldEntityResolved.load(std::memory_order_acquire);
    result.callbackSameAsSource = gCallbackSameAsSource.load(std::memory_order_acquire);
    result.callbackSameAsTarget = gCallbackSameAsTarget.load(std::memory_order_acquire);
    result.callbackRawReadable = gCallbackRawReadable.load(std::memory_order_acquire);
    result.characterCallbackReplayActive = Fix320ReplayActive();
    result.characterCallbackReplayObserved = gFix320Observed.load(std::memory_order_acquire);
    result.characterCallbackReplayPortable = gFix320Portable.load(std::memory_order_acquire);
    result.characterCallbackReplayParentSuppressed = gFix320ParentSuppressed.load(std::memory_order_acquire);
    result.characterCallbackReplayNestedSuppressed = gFix320NestedSuppressed.load(std::memory_order_acquire);
    result.characterCallbackReplaySubmitted = gFix320Submitted.load(std::memory_order_acquire);
    result.characterCallbackReplayContentSuppressed = gFix320ContentSuppressed.load(std::memory_order_acquire);
    result.characterCallbackReplayExecuteAttempts = gFix320ExecuteAttempts.load(std::memory_order_acquire);
    result.characterCallbackReplayQueued = gFix320Queued.load(std::memory_order_acquire);
    result.characterCallbackReplayRuntimeUnavailable = gFix320RuntimeUnavailable.load(std::memory_order_acquire);
    result.characterCallbackReplayNativeFaults = gFix320NativeFaults.load(std::memory_order_acquire);
    result.characterCallbackReplayInvalid = gFix320Invalid.load(std::memory_order_acquire);
    result.characterCallbackReplayLimitDrops = gFix320LimitDrops.load(std::memory_order_acquire);
    result.callbackReferenceFamilyObserved = gCallbackReferenceFamilyObserved.load(std::memory_order_acquire);
    result.callbackReferencePortable = gCallbackReferencePortable.load(std::memory_order_acquire);
    result.callbackReferenceSubmitted = gCallbackReferenceSubmitted.load(std::memory_order_acquire);
    result.callbackReferenceUnresolved = gCallbackReferenceUnresolved.load(std::memory_order_acquire);
    result.callbackReferenceExecuteAttempts = gCallbackReferenceExecuteAttempts.load(std::memory_order_acquire);
    result.callbackReferenceAppliedRemote = gCallbackReferenceAppliedRemote.load(std::memory_order_acquire);
    result.callbackReferenceRuntimeUnavailable = gCallbackReferenceRuntimeUnavailable.load(std::memory_order_acquire);
    result.callbackReferenceNativeFaults = gCallbackReferenceNativeFaults.load(std::memory_order_acquire);
    result.callbackReferenceSettlingSkipped = gCallbackReferenceSettlingSkipped.load(std::memory_order_acquire);
    result.callbackReferenceThrottled = gCallbackReferenceThrottled.load(std::memory_order_acquire);
    const auto rawFlags = gLogicSwitchCausalLastFlags.load(std::memory_order_acquire);
    result.logicSwitchCausalLastCallbackRawReadable =
        (rawFlags & kSchedulerCausalCallbackRawReadableFlag) != 0U;
    for (std::size_t i = 0U; i < kCallbackRawWordCount; ++i) {
        result.logicSwitchCausalLastCallbackRawWords[i] =
            gLogicSwitchCausalLastCallbackRawWords[i].load(std::memory_order_acquire);
    }
    result.callbackAbiProbeCallerMatches = gCallbackAbiProbeCallerMatches.load(std::memory_order_acquire);
    result.callbackAbiProbePairMatches = gCallbackAbiProbePairMatches.load(std::memory_order_acquire);
    result.callbackAbiProbeInputMatches = gCallbackAbiProbeInputMatches.load(std::memory_order_acquire);
    result.callbackAbiProbeSamplesCaptured = gCallbackAbiProbeSamplesCaptured.load(std::memory_order_acquire);
    result.callbackAbiProbeSampleLimitDrops = gCallbackAbiProbeSampleLimitDrops.load(std::memory_order_acquire);
    result.callbackAbiProbeRawReadFailures = gCallbackAbiProbeRawReadFailures.load(std::memory_order_acquire);
    result.callbackAbiProbeIdentityAttempts = gCallbackAbiProbeIdentityAttempts.load(std::memory_order_acquire);
    result.callbackAbiProbeIdentityLimitDrops = gCallbackAbiProbeIdentityLimitDrops.load(std::memory_order_acquire);
    result.callbackAbiProbeRuntimeKeyAttempts = gCallbackAbiProbeRuntimeKeyAttempts.load(std::memory_order_acquire);
    result.callbackAbiProbeSourceKeyResolved = gCallbackAbiProbeSourceKeyResolved.load(std::memory_order_acquire);
    result.callbackAbiProbeTargetKeyResolved = gCallbackAbiProbeTargetKeyResolved.load(std::memory_order_acquire);
    result.callbackAbiProbeCallbackKeyResolved = gCallbackAbiProbeCallbackKeyResolved.load(std::memory_order_acquire);
    result.callbackAbiProbeCallbackObjectPresent = gCallbackAbiProbeCallbackObjectPresent.load(std::memory_order_acquire);
    result.callbackAbiProbeTargetResolved = gCallbackAbiProbeTargetResolved.load(std::memory_order_acquire);
    result.callbackAbiProbeLastInputShortGuid = gCallbackAbiProbeLastInputShortGuid.load(std::memory_order_acquire);
    result.callbackAbiProbeFirstFlags = gCallbackAbiProbeFirstFlags.load(std::memory_order_acquire);
    result.callbackAbiProbeLastFlags = gCallbackAbiProbeLastFlags.load(std::memory_order_acquire);
    result.callbackAbiProbeLastRefCount = gCallbackAbiProbeLastRefCount.load(std::memory_order_acquire);
    result.callbackAbiProbeLastMatchKind = gCallbackAbiProbeLastMatchKind.load(std::memory_order_acquire);
    result.callbackAbiProbeFlagsVaried = gCallbackAbiProbeFlagsVaried.load(std::memory_order_acquire);
    const auto callbackAbiShape = gCallbackAbiProbeLastShapeFlags.load(std::memory_order_acquire);
    result.callbackAbiProbeLastInputIsCallback =
        (callbackAbiShape & kCallbackAbiShapeInputIsCallback) != 0U;
    result.callbackAbiProbeLastNestedPresent =
        (callbackAbiShape & kCallbackAbiShapeNestedPresent) != 0U;
    result.callbackAbiProbeLastPayloadPresent =
        (callbackAbiShape & kCallbackAbiShapePayloadPresent) != 0U;
    result.callbackAbiProbeLastTargetResolved =
        (callbackAbiShape & kCallbackAbiShapeTargetResolved) != 0U;
    result.callbackAbiProbeLastTargetSameAsSource =
        (callbackAbiShape & kCallbackAbiShapeTargetSameAsSource) != 0U;
    result.callbackAbiProbeLastTargetSameAsTarget =
        (callbackAbiShape & kCallbackAbiShapeTargetSameAsTarget) != 0U;
    result.callbackAbiProbeLastTargetSameAsState =
        (callbackAbiShape & kCallbackAbiShapeTargetSameAsState) != 0U;
    result.callbackAbiProbeLastTargetEntity.high =
        gCallbackAbiProbeLastTargetHigh.load(std::memory_order_acquire);
    result.callbackAbiProbeLastTargetEntity.low =
        gCallbackAbiProbeLastTargetLow.load(std::memory_order_acquire);
    return result;
}

bool SubmitPublicRoot(
    const WorldEntityId& owner,
    const std::uint32_t input) noexcept {
    return SubmitPublicRoot(owner, owner, input, 0.0);
}

bool SubmitPublicRoot(
    const WorldEntityId& owner, const WorldEntityId& target,
    const std::uint32_t input, const double relativeDelaySeconds,
    const std::uint8_t flags) noexcept {
    // Compatibility helper can describe only a genuinely autonomous call.
    if ((flags & kPublicRootDetachedCallback) != 0U) return false;
    CompositePublicInputEvent event{};
    event.publicInputShortGuid = input;
    event.retailRoot = true;
    event.targetEntity = target;
    event.relativeDelaySeconds = relativeDelaySeconds;
    event.flags = flags;
    event.contextKind = PublicContextKind::Autonomous;
    return SubmitPublicRoot(owner, event);
}

bool SubmitPublicRoot(const WorldEntityId& owner,
    CompositePublicInputEvent& event) noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!Initialized() || registry == nullptr || !event.retailRoot || !ValidPublicRootContext(event)) {
        return false;
    }
    if (!world_actions::PublicRootManifestAdmissionAllowed(*registry, owner, event)) {
        return false;
    }
    if (SelfClockedLocalRoot(owner, event)) return false;
    // Allocate once before queueing; descendants and the echo use this same ID.
    if (event.observationId == 0U) event.observationId = NextObservationId();
    if (event.causalSerial == 0U) event.causalSerial = event.observationId;
    if (event.segmentSerial == 0U) event.segmentSerial = event.causalSerial;

    WorldPayload payload{};
    const bool localExecuted = (event.flags & world_actions::kPublicRootFlagAwaitAuthority) == 0U;
    if (!EncodeCompositePublicInputPayload(event, payload)) return false;
    if (localExecuted && !RememberLocalObservation(event.observationId)) {
        return false;
    }
    if (replication_core::SubmitLocalIntent(owner, WorldMutationKind::Event,
        kOperationCompositePublicInput,
        std::span<const std::byte>(payload.bytes.data(), payload.size))) {
        return true;
    }
    if (localExecuted) static_cast<void>(RemoveLocalObservation(event.observationId, false));
    return false;
}

bool DiscardPublicRootIntent(const WorldIntent& intent) noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (!Initialized() || registry == nullptr || !intent.entity.Valid() ||
        !intent.manifest.Valid() || !intent.payload.Valid() ||
        intent.levelGeneration == 0U || intent.clientSequence == 0U ||
        intent.operation != kOperationCompositePublicInput ||
        intent.requestedKind != WorldMutationKind::Event) return false;
    const auto context = registry->ReplicationContext();
    if (!context.manifestLoaded || context.levelGeneration != intent.levelGeneration ||
        context.mapContentId != intent.manifest.mapContentId) return false;
    CompositePublicInputEvent event{};
    if (!DecodeCompositePublicInputPayload(
            std::span<const std::byte>(intent.payload.bytes.data(), intent.payload.size), event) ||
        !event.retailRoot ||
        !world_actions::PublicRootManifestAdmissionAllowed(*registry, intent.entity, event)) return false;
    // A rejected proposal has no authoritative echo coming back. Retire only
    // its exact local observation, without counting an echo or touching retail.
    if ((event.flags & world_actions::kPublicRootFlagAwaitAuthority) == 0U &&
        !RemoveLocalObservation(event.observationId, false)) return false;
    gDiscardedPublicRootObservations.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return true;
}

bool HasLocalObservation(const std::uint64_t observationId) noexcept {
    if (observationId == 0U) return false;
    std::lock_guard lock(gLocalObservationMutex);
    return gLocalObservations.Contains(ObservationSerial(observationId, gObservationSalt));
}

bool ConsumeLocalObservation(const std::uint64_t observationId) noexcept {
    return RemoveLocalObservation(observationId, true);
}

void DrainObservationDiagnostics(aimp::Diagnostics& diagnostics) {
    static std::uint64_t lastSample{};
    const auto now = GetTickCount64();
    if (now - lastSample < 30000U || !world_actions::WorldBusActive()) return;
    lastSample = now;
    std::uint64_t pending{}, ranges{}, oldest{}, remembered{}, echoed{}, retired{}, missing{};
    {
        std::lock_guard lock(gLocalObservationMutex);
        pending = gLocalObservations.Pending(); ranges = gLocalObservations.Ranges();
        oldest = gLocalObservations.OldestSerial(); remembered = gLocalObservationRemembered;
        echoed = gLocalObservationEchoed; retired = gLocalObservationRetired; missing = gLocalObservationMissing;
    }
    const auto replication = replication_core::DiagnosticsSnapshot();
    diagnostics.Log("info", "worldsync_observation_backlog", "Worker sample of observation and transport lifetimes.",
        {{"tickMs", Diagnostics::Unsigned(now), true},
         {"pending", Diagnostics::Unsigned(pending), true},
         {"ranges", Diagnostics::Unsigned(ranges), true},
         {"oldestSerial", Diagnostics::Unsigned(oldest), true},
         {"remembered", Diagnostics::Unsigned(remembered), true},
         {"echoed", Diagnostics::Unsigned(echoed), true},
         {"retired", Diagnostics::Unsigned(retired), true},
         {"missing", Diagnostics::Unsigned(missing), true},
         {"selfClockedKeys", Diagnostics::Unsigned(gSelfClockedKeys.load(std::memory_order_relaxed)), true},
         {"selfClockedLocalRoots", Diagnostics::Unsigned(gSelfClockedLocalRoots.load(std::memory_order_relaxed)), true},
         {"outboundPending", Diagnostics::Unsigned(replication.outboundPendingCount), true},
         {"outboundReserved", Diagnostics::Unsigned(replication.outboundReservedCount), true},
           {"outboundCapacity", Diagnostics::Unsigned(replication.outboundCapacity), true},
           {"outboundHighWater", Diagnostics::Unsigned(replication.outboundHighWater), true},
           {"outboundCapacityRejected", Diagnostics::Unsigned(replication.outboundCapacityRejected), true},
         {"outboundPopped", Diagnostics::Unsigned(replication.outboundPoppedCount), true},
         {"received", Diagnostics::Unsigned(replication.receivedMutationCount), true},
         {"committed", Diagnostics::Unsigned(replication.causalLastCommittedSequence), true},
         {"deferred", Diagnostics::Unsigned(replication.deferredRemoteApplyCount), true}});
}

ExecuteResult Execute(
    const ObservedInputEdge& edge,
    const WorldEntityLease& target,
    const WorldEntityLease& source) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    if (!Initialized() || !target || !source ||
        WorldInteractionLeaseAccess::SharedRef(target) == nullptr ||
        WorldInteractionLeaseAccess::SharedRef(source) == nullptr) {
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (edge.schedulerParameterShortGuid == 0U || edge.targetInputShortGuid == 0U ||
        edge.observationId == 0U || !edge.sourceEntity.Valid() ||
        !std::isfinite(edge.relativeDelaySeconds) || edge.relativeDelaySeconds < 0.0F ||
        edge.relativeDelaySeconds > 600.0F) {
        gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return {ExecuteStatus::InvalidPayload, false};
    }
    if (edge.sourceCategory != SourceCategory::GenericWorld) {
        gSourceClassMismatch.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return {ExecuteStatus::InvalidPayload, false};
    }

    const auto result = cathode_primitives::ExecuteInputEdge(
        edge.schedulerParameterShortGuid,
        edge.targetInputShortGuid,
        edge.relativeDelaySeconds,
        target,
        source);
    switch (result.status) {
    case cathode_primitives::ExecuteStatus::Queued:
        gQueuedRemoteEdges.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return {ExecuteStatus::Queued, result.queued};
    case cathode_primitives::ExecuteStatus::RuntimeUnavailable:
        return {ExecuteStatus::RuntimeUnavailable, false};
    case cathode_primitives::ExecuteStatus::NativeFault:
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    case cathode_primitives::ExecuteStatus::InvalidPayload:
    case cathode_primitives::ExecuteStatus::UnsupportedOperation:
    default:
        return {ExecuteStatus::InvalidPayload, false};
    }
}

ExecuteResult ExecuteCompositePublicInputBridgeImpl(
    const CompositePublicInputEvent& event,
    const WorldEntityId& ownerCompositeInstance,
    const WorldEntityLease& target,
    WorldEntityRegistry& registry) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    gCompositePublicInputRemoteAttempts.fetch_add(1U, std::memory_order_relaxed);
    TouchVersion();
    if (event.observationId == 0U || event.publicInputShortGuid == 0U ||
        !ownerCompositeInstance.Valid() || !target ||
        !registry.IsCompositePublicInputAllowed(
            ownerCompositeInstance, event.publicInputShortGuid)) {
        gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_acq_rel);
        gCompositePublicInputRejected.fetch_add(1U, std::memory_order_relaxed);
        TouchVersion();
        return {ExecuteStatus::InvalidPayload, false};
    }

    // STATIC CONFIRMED (COMMANDS/CathodeLib): VariableEntity represents a
    // Composite parameter exposed outside the Composite, and its childLinks
    // encode thisParamID -> linkedEntityID/linkedParamID. Do not guess a
    // special runtime ABI for a Composite public port. Resolve the complete
    // manifest-generated bridge set and replay those exact local CATHODE input
    // edges through the already-proven FUN_009271C0 scheduler.
    const auto bridges = registry.FindCompositePublicInputBridges(
        ownerCompositeInstance, event.publicInputShortGuid);
    if (bridges.empty() || bridges.size() > kCompositeInputMaximumBridgeFanout) {
        gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_acq_rel);
        gCompositePublicInputRejected.fetch_add(1U, std::memory_order_relaxed);
        TouchVersion();
        return {ExecuteStatus::InvalidPayload, false};
    }

    struct ResolvedBridge final {
        WorldCompositePublicInputDescriptor descriptor{};
        WorldEntityLease source;
        WorldEntityLease target;
        bool useTargetAsSource{};
    };
    std::vector<ResolvedBridge> resolvedBridges;
    resolvedBridges.reserve(bridges.size());

    // Resolve every local endpoint before queueing any edge. This prevents a
    // partially available fan-out from mutating only half of a public input and
    // then being retried as a whole mutation.
    for (const auto& bridge : bridges) {
        if (!bridge.noTypedData ||
            !(bridge.ownerCompositeInstance == ownerCompositeInstance) ||
            bridge.publicInputShortGuid != event.publicInputShortGuid ||
            !bridge.interfaceVariableEntity.Valid() ||
            bridge.interfaceLinkParameter == 0U ||
            !bridge.internalTargetEntity.Valid() ||
            bridge.internalTargetInputShortGuid == 0U) {
            gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_acq_rel);
            gCompositePublicInputRejected.fetch_add(1U, std::memory_order_relaxed);
            TouchVersion();
            return {ExecuteStatus::InvalidPayload, false};
        }

        auto sourceResolved = registry.Resolve(bridge.interfaceVariableEntity);
        if (sourceResolved.status == WorldRegistryResolveStatus::NativeFault) {
            gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            return {ExecuteStatus::NativeFault, false};
        }
        const bool exactSourceResolved =
            sourceResolved.status == WorldRegistryResolveStatus::Resolved &&
            static_cast<bool>(sourceResolved.lease);

        auto targetResolved = registry.Resolve(bridge.internalTargetEntity);
        if (targetResolved.status != WorldRegistryResolveStatus::Resolved ||
            !targetResolved.lease) {
            if (targetResolved.status == WorldRegistryResolveStatus::NativeFault) {
                gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
                TouchVersion();
                return {ExecuteStatus::NativeFault, false};
            }
            gCompositePublicInputRuntimeUnavailable.fetch_add(1U, std::memory_order_relaxed);
            return {ExecuteStatus::RuntimeUnavailable, false};
        }

        ResolvedBridge resolved{};
        resolved.descriptor = bridge;
        if (exactSourceResolved) {
            resolved.source = std::move(sourceResolved.lease);
        } else {
            // STATIC CONFIRMED retail behavior inherited from v12.5: at least
            // one FUN_009271C0 call-site uses the same shared-ref as source and
            // target. The interface VariableEntity is a static COMMANDS node,
            // so if it is not materialized as a local runtime object, use that
            // proven source shape while preserving the exact manifest target,
            // scheduler parameter and target input.
            resolved.useTargetAsSource = true;
            gCompositePublicInputSourceFallbacks.fetch_add(1U, std::memory_order_relaxed);
        }
        resolved.target = std::move(targetResolved.lease);
        resolvedBridges.push_back(std::move(resolved));
    }

    // RemoteApply covers synchronous hook re-entry. The semantic guard covers
    // any delayed local result that reaches the LogicSwitch fallback after the
    // ReplicationCore apply scope has unwound.
    RememberRemoteCompositePublicInput(
        ownerCompositeInstance, event.publicInputShortGuid);

    bool queuedAny = false;
    for (auto& bridge : resolvedBridges) {
        const WorldEntityLease& effectiveSource =
            bridge.useTargetAsSource ? bridge.target : bridge.source;
        const auto primitiveResult = cathode_primitives::ExecuteInputEdge(
            bridge.descriptor.interfaceLinkParameter,
            bridge.descriptor.internalTargetInputShortGuid,
            0.0F,
            bridge.target,
            effectiveSource);
        switch (primitiveResult.status) {
        case cathode_primitives::ExecuteStatus::Queued:
            if (!primitiveResult.queued) {
                gCompositePublicInputRuntimeUnavailable.fetch_add(1U, std::memory_order_relaxed);
                return {ExecuteStatus::RuntimeUnavailable, false};
            }
            queuedAny = true;
            gQueuedRemoteEdges.fetch_add(1U, std::memory_order_acq_rel);
            gCompositePublicInputBridgeQueued.fetch_add(1U, std::memory_order_relaxed);
            TouchVersion();
            break;
        case cathode_primitives::ExecuteStatus::RuntimeUnavailable:
            gCompositePublicInputRuntimeUnavailable.fetch_add(1U, std::memory_order_relaxed);
            return {ExecuteStatus::RuntimeUnavailable, false};
        case cathode_primitives::ExecuteStatus::NativeFault:
            gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            return {ExecuteStatus::NativeFault, false};
        case cathode_primitives::ExecuteStatus::InvalidPayload:
        case cathode_primitives::ExecuteStatus::UnsupportedOperation:
        default:
            gInvalidEdgesSkipped.fetch_add(1U, std::memory_order_acq_rel);
            gCompositePublicInputRejected.fetch_add(1U, std::memory_order_relaxed);
            TouchVersion();
            return {ExecuteStatus::InvalidPayload, false};
        }
    }

    return queuedAny
        ? ExecuteResult{ExecuteStatus::Queued, true}
        : ExecuteResult{ExecuteStatus::InvalidPayload, false};
}

ExecuteResult ExecuteCanonicalCompositePublicInput(
    const CompositePublicInputEvent& event,
    const WorldEntityId& ownerCompositeInstance,
    const WorldEntityLease& target,
    WorldEntityRegistry& registry) noexcept {
    // Patch 5.8: a CanonicalCompositeInput ROOT is the WS3-native form of the
    // proven WS2 public-Composite valve. The callback/work-item stays strictly
    // process-local. Remote replay expands only compiler-manifested childLinks
    // and uses the retail scheduler for each exact local edge.
    if (!event.retailRoot ||
        event.rootKind != PublicRootKind::CanonicalCompositeInput ||
        event.publicInputShortGuid == 0U || event.observationId == 0U ||
        !ownerCompositeInstance.Valid() || !target ||
        !(event.targetEntity == ownerCompositeInstance) ||
        event.relativeDelaySeconds != 0.0 ||
        event.contextKind != PublicContextKind::Autonomous ||
        event.contextFrameCount != 0U || event.contextPayloadSize != 0U ||
        event.schedulerParameterShortGuid != 0U || event.interfaceSelector != 0U ||
        event.actorKind != PublicActorKind::SourcePlayer || event.flags != 0U) {
        gCompositePublicInputRejected.fetch_add(1U, std::memory_order_relaxed);
        TouchVersion();
        return {ExecuteStatus::InvalidPayload, false};
    }
    return ExecuteCompositePublicInputBridgeImpl(
        event, ownerCompositeInstance, target, registry);
}

ExecuteResult ExecuteCompositePublicInput(
    const CompositePublicInputEvent& event,
    const WorldEntityId& ownerCompositeInstance,
    const WorldEntityLease& target,
    WorldEntityRegistry& registry) noexcept {
    if (event.retailRoot) {
        if (event.rootKind == PublicRootKind::CanonicalCompositeInput) {
            return ExecuteCanonicalCompositePublicInput(
                event, ownerCompositeInstance, target, registry);
        }
        return world_actions::Execute(event, ownerCompositeInstance, target, registry);
    }
    // Do not let stale legacy WS2 payloads replay childLinks in a WS3 session.
    // Patch 5.8 uses ROOT kind 6 instead; this is not a WS2 fallback.
    if (world_actions::Installed()) return {ExecuteStatus::InvalidPayload, false};
    return ExecuteCompositePublicInputBridgeImpl(
        event, ownerCompositeInstance, target, registry);
}

ExecuteResult ExecuteCallbackInputEdge(
    const ObservedCallbackInputEdge& edge,
    const WorldEntityLease& target,
    const WorldEntityLease& source) noexcept {
    // Fix3.3 hard safety fuse. The runtime-observed FUN_008BDFB0 family still
    // lacks a proven cross-process callback construction ABI. Never call a
    // callback constructor/retain/queue sequence from replicated traffic.
    static_cast<void>(edge);
    static_cast<void>(target);
    static_cast<void>(source);
    gCallbackReferenceExecuteAttempts.fetch_add(1U, std::memory_order_relaxed);
    gCallbackReferenceRuntimeUnavailable.fetch_add(1U, std::memory_order_relaxed);
    return {ExecuteStatus::RuntimeUnavailable, false};
}

bool ConsumeLocalCharacterCallbackMatch(
    const ObservedCharacterCallbackInputEdge& edge,
    const WorldEntityId& targetEntity) noexcept {
    if (ConsumeCharacterCallbackMatch(CharacterCallbackFingerprint(edge, targetEntity))) {
        gFix320ContentSuppressed.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return true;
    }
    return false;
}

ExecuteResult ExecuteCharacterCallbackInputEdge(
    const ObservedCharacterCallbackInputEdge& edge,
    const WorldEntityLease& target,
    const WorldEntityLease& source,
    const WorldEntityLease& callback) noexcept {
    gFix320ExecuteAttempts.fetch_add(1U, std::memory_order_relaxed);
    if (!Fix320ReplayActive()) {
        gFix320RuntimeUnavailable.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (edge.sourceCategory != SourceCategory::GenericWorld ||
        edge.schedulerParameterShortGuid == 0U || edge.targetInputShortGuid == 0U ||
        edge.callbackInputShortGuid != kCharacterCallbackInputShortGuid ||
        edge.callbackConstructorFlags != kCharacterCallbackConstructorFlags ||
        edge.callbackExecutionVtableRva != kCharacterCallbackExecutionVtableRva ||
        edge.callbackFamilyCallerRva != kCharacterCallbackFamilyCallerRva ||
        edge.observationId == 0U || !edge.sourceEntity.Valid() ||
        !edge.callbackEntity.Valid() || !std::isfinite(edge.relativeDelaySeconds) ||
        edge.relativeDelaySeconds < 0.0F || edge.relativeDelaySeconds > 600.0F) {
        gFix320Invalid.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::InvalidPayload, false};
    }
    const auto executionIndex = gFix320WindowExecutions.fetch_add(
        1U, std::memory_order_acq_rel);
    if (executionIndex >= kFix320MaximumExecutionsPerWindow) {
        gFix320LimitDrops.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::InvalidPayload, false};
    }
    const auto primitive = cathode_primitives::ExecuteInputEdgeWithConstructedCallback(
        edge.schedulerParameterShortGuid,
        edge.targetInputShortGuid,
        edge.callbackConstructorFlags,
        0U,
        edge.relativeDelaySeconds,
        target,
        source,
        callback);
    switch (primitive.status) {
    case cathode_primitives::ExecuteStatus::Queued:
        gFix320Queued.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::Queued, primitive.queued};
    case cathode_primitives::ExecuteStatus::RuntimeUnavailable:
        gFix320RuntimeUnavailable.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::RuntimeUnavailable, false};
    case cathode_primitives::ExecuteStatus::NativeFault:
        gFix320NativeFaults.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::NativeFault, false};
    case cathode_primitives::ExecuteStatus::InvalidPayload:
    case cathode_primitives::ExecuteStatus::UnsupportedOperation:
    default:
        gFix320Invalid.fetch_add(1U, std::memory_order_relaxed);
        return {ExecuteStatus::InvalidPayload, false};
    }
}

bool ConsumeLocalTypedMessage(
    const ObservedTypedMessage& message,
    const std::span<const std::byte> data) noexcept {
    const auto fingerprint = TypedFingerprint(message, data);
    if (ConsumeTypedMatch(fingerprint)) {
        gTypedContentMatchSuppressed.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return true;
    }
    return false;
}

bool ConsumeLocalLogicSwitchState(const std::uint64_t observationId) noexcept {
    return RemoveLocalObservation(observationId, true);
}

ExecuteResult ExecuteLogicSwitchState(
    const ObservedLogicSwitchState& state,
    const CathodeRuntimeKey& targetKey,
    const WorldEntityLease& target) noexcept {
    static_cast<void>(state);
    static_cast<void>(targetKey);
    static_cast<void>(target);
    gLogicSwitchExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    gLogicSwitchRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    // Fix3.10 BuildFix01 safety gate: never execute the cross-process
    // LogicSwitch context-key replay. Fix3.10 showed that globally replaying
    // these states can flood the 128-entry engine dispatcher and can feed a
    // locally valid but semantically unrelated CATHODE context into retail.
    return {ExecuteStatus::RuntimeUnavailable, false};
}

ExecuteResult ExecuteTypedMessage(
    const ObservedTypedMessage& message,
    const std::span<const std::byte> data) noexcept {
    gTypedExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    if (!Initialized() || message.observationId == 0U ||
        message.sourceRuntimeKey.entityA == 0U || message.targetRuntimeKey.entityA == 0U ||
        data.size() > kObservedTypedMessageMaximumDataBytes ||
        !std::isfinite(message.relativeDelaySeconds) || message.relativeDelaySeconds < 0.0 ||
        message.relativeDelaySeconds > 600.0) {
        return {ExecuteStatus::InvalidPayload, false};
    }
    const auto result = cathode_primitives::ExecuteTypedMessage(
        message.messageType, data, message.relativeDelaySeconds,
        message.targetRuntimeKey, message.sourceRuntimeKey);
    switch (result.status) {
    case cathode_primitives::ExecuteStatus::Queued:
        gQueuedRemoteTypedMessages.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return {ExecuteStatus::Queued, result.queued};
    case cathode_primitives::ExecuteStatus::RuntimeUnavailable:
        return {ExecuteStatus::RuntimeUnavailable, false};
    case cathode_primitives::ExecuteStatus::NativeFault:
        gTypedNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    case cathode_primitives::ExecuteStatus::InvalidPayload:
    case cathode_primitives::ExecuteStatus::UnsupportedOperation:
    default:
        return {ExecuteStatus::InvalidPayload, false};
    }
}

void ResetSessionObservations() noexcept {
    ClearEchoIds();
}

void ResetForGeneration() noexcept {
    hiding_door_presentation::ResetForGeneration();
    world_actions::ResetForGeneration();
    gCompositePublicInputPreQueueBuilderUnknownTraceCount.store(0U, std::memory_order_relaxed);
    for (auto& slot : gCompositePublicInputPreQueueBuilderUnknownTrace) {
        slot.ready.store(0U, std::memory_order_relaxed);
        slot.inputShortGuid.store(0U, std::memory_order_relaxed);
        slot.callerRva.store(0U, std::memory_order_relaxed);
        slot.targetRuntimeKeyA.store(0U, std::memory_order_relaxed);
        slot.targetRuntimeKeyB.store(0U, std::memory_order_relaxed);
        slot.targetExecution.store(0U, std::memory_order_relaxed);
        slot.targetIdHigh.store(0U, std::memory_order_relaxed);
        slot.targetIdLow.store(0U, std::memory_order_relaxed);
        slot.bridgeOwnerHigh.store(0U, std::memory_order_relaxed);
        slot.bridgeOwnerLow.store(0U, std::memory_order_relaxed);
        slot.outerPublicInput.store(0U, std::memory_order_relaxed);
    }
    gFix320ReplayUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix319ArmUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix319WindowUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix319OriginTickMs.store(0U, std::memory_order_relaxed);
    gFix321WindowUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix321OriginTickMs.store(0U, std::memory_order_relaxed);
    gFix321Inspected.store(0U, std::memory_order_relaxed);
    gFix321Accepted.store(0U, std::memory_order_relaxed);
    gFix321UniqueEntries.store(0U, std::memory_order_relaxed);
    gFix321CapacityDrops.store(0U, std::memory_order_relaxed);
    gFix321ReadFailures.store(0U, std::memory_order_relaxed);
    gFix321LimitDrops.store(0U, std::memory_order_relaxed);
    for (auto& slot : gFix321TraceSlots) {
        slot.ready.store(0U, std::memory_order_relaxed);
        slot.signatureHash.store(0U, std::memory_order_relaxed);
        slot.semantic = {};
        slot.count.store(0U, std::memory_order_relaxed);
        slot.executionMask.store(0U, std::memory_order_relaxed);
        slot.firstDeltaMs.store(0U, std::memory_order_relaxed);
        slot.lastDeltaMs.store(0U, std::memory_order_relaxed);
    }
    gFix322WindowUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix322OriginTickMs.store(0U, std::memory_order_relaxed);
    gFix322Inspected.store(0U, std::memory_order_relaxed);
    gFix322Accepted.store(0U, std::memory_order_relaxed);
    gFix322UniqueEntries.store(0U, std::memory_order_relaxed);
    gFix322ChangedCalls.store(0U, std::memory_order_relaxed);
    gFix322CapacityDrops.store(0U, std::memory_order_relaxed);
    gFix322ReadFailures.store(0U, std::memory_order_relaxed);
    gFix322LimitDrops.store(0U, std::memory_order_relaxed);
    for (auto& slot : gFix322TraceSlots) {
        slot.ready.store(0U, std::memory_order_relaxed);
        slot.signatureHash.store(0U, std::memory_order_relaxed);
        slot.semantic = {};
        slot.count.store(0U, std::memory_order_relaxed);
        slot.executionMask.store(0U, std::memory_order_relaxed);
        slot.firstDeltaMs.store(0U, std::memory_order_relaxed);
        slot.lastDeltaMs.store(0U, std::memory_order_relaxed);
    }
    {
        std::lock_guard lock(gCompositeInputSemanticMutex);
        for (auto& slot : gCompositeInputSemanticSlots) slot = {};
        gCompositeInputRateWindowTickMs = 0U;
        gCompositeInputRateWindowCount = 0U;
    }
    ClearEchoIds();
    ClearRuntimeWorldIdCache();
    {
        std::lock_guard lock(gLogicSwitchBindingMutex);
        for (auto& binding : gLogicSwitchBindings) binding = {};
    }
    for (auto& record : gSchedulerCausalHistory) {
        record.sequence.store(0U, std::memory_order_release);
    }
    gLogicSwitchCausalLastAgeMs.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastSourceHigh.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastSourceLow.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastTargetHigh.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastTargetLow.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastStateHigh.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastStateLow.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastSchedulerParameter.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastTargetInput.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastFlags.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastEnabled.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastCallbackHigh.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastCallbackLow.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastCallbackExecutionVtableRva.store(0U, std::memory_order_release);
    gLogicSwitchCausalLastQueueCallerRva.store(0U, std::memory_order_release);
    gLogicSwitchLastContextA.store(0U, std::memory_order_release);
    gLogicSwitchLastContextB.store(0U, std::memory_order_release);
    gLogicSwitchLastStateA.store(0U, std::memory_order_release);
    gLogicSwitchLastStateB.store(0U, std::memory_order_release);
    gLogicSwitchLastEnabled.store(0U, std::memory_order_release);
    gLogicExactTargetMatches.store(0U, std::memory_order_release);
    gLogicExactHolderReadable.store(0U, std::memory_order_release);
    gLogicExactCallbackPresent.store(0U, std::memory_order_release);
    gLogicExactRawReadable.store(0U, std::memory_order_release);
    gLogicExactRawReadFailures.store(0U, std::memory_order_release);
    gLogicExactCallbackObjectPresent.store(0U, std::memory_order_release);
    gLogicExactCallbackKeyResolved.store(0U, std::memory_order_release);
    gLogicExactCallbackExecutionReadable.store(0U, std::memory_order_release);
    gLogicExactInput.store(0U, std::memory_order_release);
    gLogicExactFlags.store(0U, std::memory_order_release);
    gLogicExactRefCount.store(0U, std::memory_order_release);
    gLogicExactNestedPresent.store(0U, std::memory_order_release);
    gLogicExactPayloadPresent.store(0U, std::memory_order_release);
    gLogicExactCallbackKeyA.store(0U, std::memory_order_release);
    gLogicExactCallbackKeyB.store(0U, std::memory_order_release);
    gLogicExactCallbackExecutionVtableRva.store(0U, std::memory_order_release);
    gLogicExactContextSameAsState.store(0U, std::memory_order_release);
    gLogicExactEnabled.store(0U, std::memory_order_release);
    gCharacterCallbackOriginProbeUntilTickMs.store(0U, std::memory_order_release);
    gCharacterCallbackOriginCandidates.store(0U, std::memory_order_release);
    gCharacterCallbackOriginExactKeyMatches.store(0U, std::memory_order_release);
    gCharacterCallbackOriginCallVerified.store(0U, std::memory_order_release);
    gCharacterCallbackOriginReadFailures.store(0U, std::memory_order_release);
    gCharacterCallbackOriginSourceA.store(0U, std::memory_order_release);
    gCharacterCallbackOriginSourceB.store(0U, std::memory_order_release);
    gCharacterCallbackOriginExecutionVtableRva.store(0U, std::memory_order_release);
    gCharacterCallbackOriginFlags.store(0U, std::memory_order_release);
    gCharacterCallbackOriginPayloadBytes.store(0U, std::memory_order_release);
    gCharacterCallbackOriginFirstCallSiteRva.store(0U, std::memory_order_release);
    gCharacterCallbackOriginLastCallSiteRva.store(0U, std::memory_order_release);
    gCharacterCallbackOriginCallSiteVaried.store(0U, std::memory_order_release);
    gCharacterCallbackOriginThreadId.store(0U, std::memory_order_release);
    gCharacterCallbackOriginTickMs.store(0U, std::memory_order_release);
    gTypedMessageOriginProbeUntilTickMs.store(0U, std::memory_order_release);
    gTypedMessageOriginCandidates.store(0U, std::memory_order_release);
    gTypedMessageOriginExactMatches.store(0U, std::memory_order_release);
    gTypedMessageOriginCallVerified.store(0U, std::memory_order_release);
    gTypedMessageOriginReadFailures.store(0U, std::memory_order_release);
    gTypedMessageOriginMessageType.store(0U, std::memory_order_release);
    gTypedMessageOriginPayloadBytes.store(0U, std::memory_order_release);
    gTypedMessageOriginSourceA.store(0U, std::memory_order_release);
    gTypedMessageOriginSourceB.store(0U, std::memory_order_release);
    gTypedMessageOriginTargetA.store(0U, std::memory_order_release);
    gTypedMessageOriginTargetB.store(0U, std::memory_order_release);
    gTypedMessageOriginWorldMask.store(0U, std::memory_order_release);
    gTypedMessageOriginDelayBits.store(0U, std::memory_order_release);
    gTypedMessageOriginFirstCallSiteRva.store(0U, std::memory_order_release);
    gTypedMessageOriginLastCallSiteRva.store(0U, std::memory_order_release);
    gTypedMessageOriginCallSiteVaried.store(0U, std::memory_order_release);
    gTypedMessageOriginThreadId.store(0U, std::memory_order_release);
    gTypedMessageOriginTickMs.store(0U, std::memory_order_release);
    gTypedRemoteIdentityProbeUntilTickMs.store(0U, std::memory_order_release);
    gTypedRemoteIdentityCandidates.store(0U, std::memory_order_release);
    gTypedRemoteIdentityExactMatches.store(0U, std::memory_order_release);
    gTypedRemoteIdentityMessageType.store(0U, std::memory_order_release);
    gTypedRemoteIdentityPayloadBytes.store(0U, std::memory_order_release);
    gTypedRemoteIdentitySourceWorldHigh.store(0U, std::memory_order_release);
    gTypedRemoteIdentitySourceWorldLow.store(0U, std::memory_order_release);
    gTypedRemoteIdentityTargetWorldHigh.store(0U, std::memory_order_release);
    gTypedRemoteIdentityTargetWorldLow.store(0U, std::memory_order_release);
    gTypedRemoteIdentitySenderSourceA.store(0U, std::memory_order_release);
    gTypedRemoteIdentitySenderSourceB.store(0U, std::memory_order_release);
    gTypedRemoteIdentitySenderTargetA.store(0U, std::memory_order_release);
    gTypedRemoteIdentitySenderTargetB.store(0U, std::memory_order_release);
    gTypedRemoteIdentityLocalSourceA.store(0U, std::memory_order_release);
    gTypedRemoteIdentityLocalSourceB.store(0U, std::memory_order_release);
    gTypedRemoteIdentityLocalTargetA.store(0U, std::memory_order_release);
    gTypedRemoteIdentityLocalTargetB.store(0U, std::memory_order_release);
    gTypedRemoteIdentityMask.store(0U, std::memory_order_release);
    gTypedRemoteIdentityExecuteReached.store(0U, std::memory_order_release);
    gTypedRemoteIdentityQueued.store(0U, std::memory_order_release);
    gTypedRemoteIdentityRuntimeUnavailable.store(0U, std::memory_order_release);
    gTypedRemoteIdentityNativeFaults.store(0U, std::memory_order_release);
    gTypedRemoteIdentityInvalidPayloads.store(0U, std::memory_order_release);
    gLogicDownstreamProbeUntilTickMs.store(0U, std::memory_order_release);
    gLogicDownstreamScopes.store(0U, std::memory_order_release);
    gLogicDownstreamEdges.store(0U, std::memory_order_release);
    gLogicDownstreamValidEdges.store(0U, std::memory_order_release);
    gLogicDownstreamCallbackEdges.store(0U, std::memory_order_release);
    gLogicDownstreamReadFailures.store(0U, std::memory_order_release);
    gLogicDownstreamWorldMask.store(0U, std::memory_order_release);
    gLogicDownstreamSourceA.store(0U, std::memory_order_release);
    gLogicDownstreamSourceB.store(0U, std::memory_order_release);
    gLogicDownstreamTargetA.store(0U, std::memory_order_release);
    gLogicDownstreamTargetB.store(0U, std::memory_order_release);
    gLogicDownstreamSchedulerParameter.store(0U, std::memory_order_release);
    gLogicDownstreamTargetInput.store(0U, std::memory_order_release);
    gLogicDownstreamCallbackFlags.store(0U, std::memory_order_release);
    gLogicDownstreamFirstCallSiteRva.store(0U, std::memory_order_release);
    gLogicDownstreamLastCallSiteRva.store(0U, std::memory_order_release);
    gLogicDownstreamCallSiteVaried.store(0U, std::memory_order_release);
    gLogicDownstreamSequenceHash.store(kFix315HashOffset, std::memory_order_release);
    gLogicDownstreamThreadId.store(0U, std::memory_order_release);
    gLogicDownstreamTickMs.store(0U, std::memory_order_release);
    gFix315ExactClearDepth = 0U;
    gPostClearWindowUntilTickMs.store(0U, std::memory_order_release);
    gPostClearOriginTickMs.store(0U, std::memory_order_release);
    gPostClearArms.store(0U, std::memory_order_release);
    gPostClearSeen.store(0U, std::memory_order_release);
    gPostClearAnchorMatches.store(0U, std::memory_order_release);
    gPostClearValidEdges.store(0U, std::memory_order_release);
    gPostClearCallbackEdges.store(0U, std::memory_order_release);
    gPostClearReadFailures.store(0U, std::memory_order_release);
    gPostClearInspectLimitDrops.store(0U, std::memory_order_release);
    gPostClearAnchorRole.store(0U, std::memory_order_release);
    gPostClearWorldMask.store(0U, std::memory_order_release);
    gPostClearSourceA.store(0U, std::memory_order_release);
    gPostClearSourceB.store(0U, std::memory_order_release);
    gPostClearTargetA.store(0U, std::memory_order_release);
    gPostClearTargetB.store(0U, std::memory_order_release);
    gPostClearSchedulerParameter.store(0U, std::memory_order_release);
    gPostClearTargetInput.store(0U, std::memory_order_release);
    gPostClearCallbackFlags.store(0U, std::memory_order_release);
    gPostClearFirstCallSiteRva.store(0U, std::memory_order_release);
    gPostClearLastCallSiteRva.store(0U, std::memory_order_release);
    gPostClearCallSiteVaried.store(0U, std::memory_order_release);
    gPostClearSequenceHash.store(kFix316HashOffset, std::memory_order_release);
    gPostClearFirstDeltaMs.store(0U, std::memory_order_release);
    gPostClearLastDeltaMs.store(0U, std::memory_order_release);
    gPostClearThreadId.store(0U, std::memory_order_release);
    gPostClearTickMs.store(0U, std::memory_order_release);
    gClearDispatchCandidates.store(0U, std::memory_order_release);
    gClearDispatchExactMatches.store(0U, std::memory_order_release);
    gClearDispatchReadFailures.store(0U, std::memory_order_release);
    gClearDispatchVtableRva.store(0U, std::memory_order_release);
    gClearDispatchTargetRva.store(0U, std::memory_order_release);
    gClearDispatchFirstTargetRva.store(0U, std::memory_order_release);
    gClearDispatchLastTargetRva.store(0U, std::memory_order_release);
    gClearDispatchTargetVaried.store(0U, std::memory_order_release);
    gClearDispatchBeforeState.store(0U, std::memory_order_release);
    gClearDispatchAfterState.store(0U, std::memory_order_release);
    gClearDispatchThreadId.store(0U, std::memory_order_release);
    gClearDispatchTickMs.store(0U, std::memory_order_release);
    gClearCodeCandidates.store(0U, std::memory_order_release);
    gClearCodeExactMatches.store(0U, std::memory_order_release);
    gClearCodeReadFailures.store(0U, std::memory_order_release);
    gClearCodeTargetRva.store(0U, std::memory_order_release);
    gClearCodeWord0.store(0U, std::memory_order_release);
    gClearCodeWord1.store(0U, std::memory_order_release);
    gClearCodeBranchKind.store(0U, std::memory_order_release);
    gClearCodeBranchTargetRva.store(0U, std::memory_order_release);
    gClearCodeFinalTargetRva.store(0U, std::memory_order_release);
    gClearCodeJumpDepth.store(0U, std::memory_order_release);
    gClearCodeHash.store(kFix318HashOffset, std::memory_order_release);
    gClearCodeThreadId.store(0U, std::memory_order_release);
    gClearCodeTickMs.store(0U, std::memory_order_release);
    for (auto& word : gLogicSwitchCausalLastCallbackRawWords) {
        word.store(0U, std::memory_order_release);
    }
    gCallbackClassificationUntilTickMs.store(0U, std::memory_order_release);
    gCallbackAbiProbeCallerMatches.store(0U, std::memory_order_release);
    gCallbackAbiProbePairMatches.store(0U, std::memory_order_release);
    gCallbackAbiProbeInputMatches.store(0U, std::memory_order_release);
    gCallbackAbiProbeSamplesCaptured.store(0U, std::memory_order_release);
    gCallbackAbiProbeSampleLimitDrops.store(0U, std::memory_order_release);
    gCallbackAbiProbeRawReadFailures.store(0U, std::memory_order_release);
    gCallbackAbiProbeTargetResolved.store(0U, std::memory_order_release);
    gCallbackAbiProbeLastInputShortGuid.store(0U, std::memory_order_release);
    gCallbackAbiProbeFirstFlags.store(0U, std::memory_order_release);
    gCallbackAbiProbeLastFlags.store(0U, std::memory_order_release);
    gCallbackAbiProbeLastRefCount.store(0U, std::memory_order_release);
    gCallbackAbiProbeLastShapeFlags.store(0U, std::memory_order_release);
    gCallbackAbiProbeLastTargetHigh.store(0U, std::memory_order_release);
    gCallbackAbiProbeLastTargetLow.store(0U, std::memory_order_release);
    gCallbackAbiProbeLastMatchKind.store(kCallbackAbiMatchNone, std::memory_order_release);
    gCallbackAbiProbeFirstFlagsSet.store(false, std::memory_order_release);
    gCallbackAbiProbeFlagsVaried.store(false, std::memory_order_release);
    TouchVersion();
}

void BeginLogicalShutdown() noexcept {
    gLocalInputTargetObserver.store(nullptr, std::memory_order_release);
    world_actions::BeginLogicalShutdown();
    gFix319ArmUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix319WindowUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix319OriginTickMs.store(0U, std::memory_order_relaxed);
    gFix321WindowUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix321OriginTickMs.store(0U, std::memory_order_relaxed);
    gFix322WindowUntilTickMs.store(0U, std::memory_order_relaxed);
    gFix322OriginTickMs.store(0U, std::memory_order_relaxed);
    if (!gInitialized.exchange(false, std::memory_order_acq_rel)) return;
    ClearRuntimeWorldIdCache();
    gRegistry.store(nullptr, std::memory_order_release);
    gResolver = nullptr;
    gHooks = nullptr;
    {
        std::lock_guard lock(gCompositeInputSemanticMutex);
        for (auto& slot : gCompositeInputSemanticSlots) slot = {};
        gCompositeInputRateWindowTickMs = 0U;
        gCompositeInputRateWindowCount = 0U;
    }
    ClearEchoIds();
    gHookInstalled.store(false, std::memory_order_release);
    gBuildInputTargetHookInstalled.store(false, std::memory_order_release);
    gFix321CallRedirectsInstalled.store(false, std::memory_order_release);
    gFix322CallRedirectInstalled.store(false, std::memory_order_release);
    gTypedMessageHookInstalled.store(false, std::memory_order_release);
    gTypedMessageOriginProbeUntilTickMs.store(0U, std::memory_order_release);
    gTypedRemoteIdentityProbeUntilTickMs.store(0U, std::memory_order_release);
    gLogicDownstreamProbeUntilTickMs.store(0U, std::memory_order_release);
    gPostClearWindowUntilTickMs.store(0U, std::memory_order_release);
    gPostClearOriginTickMs.store(0U, std::memory_order_release);
    gClearDispatchCandidates.store(0U, std::memory_order_release);
    gClearDispatchExactMatches.store(0U, std::memory_order_release);
    gClearDispatchReadFailures.store(0U, std::memory_order_release);
    gClearDispatchVtableRva.store(0U, std::memory_order_release);
    gClearDispatchTargetRva.store(0U, std::memory_order_release);
    gClearDispatchFirstTargetRva.store(0U, std::memory_order_release);
    gClearDispatchLastTargetRva.store(0U, std::memory_order_release);
    gClearDispatchTargetVaried.store(0U, std::memory_order_release);
    gClearDispatchBeforeState.store(0U, std::memory_order_release);
    gClearDispatchAfterState.store(0U, std::memory_order_release);
    gClearDispatchThreadId.store(0U, std::memory_order_release);
    gClearDispatchTickMs.store(0U, std::memory_order_release);
    gClearCodeCandidates.store(0U, std::memory_order_release);
    gClearCodeExactMatches.store(0U, std::memory_order_release);
    gClearCodeReadFailures.store(0U, std::memory_order_release);
    gClearCodeTargetRva.store(0U, std::memory_order_release);
    gClearCodeWord0.store(0U, std::memory_order_release);
    gClearCodeWord1.store(0U, std::memory_order_release);
    gClearCodeBranchKind.store(0U, std::memory_order_release);
    gClearCodeBranchTargetRva.store(0U, std::memory_order_release);
    gClearCodeFinalTargetRva.store(0U, std::memory_order_release);
    gClearCodeJumpDepth.store(0U, std::memory_order_release);
    gClearCodeHash.store(kFix318HashOffset, std::memory_order_release);
    gClearCodeThreadId.store(0U, std::memory_order_release);
    gClearCodeTickMs.store(0U, std::memory_order_release);
    gFix315ExactClearDepth = 0U;
    gCharacterCallbackOriginHookInstalled.store(false, std::memory_order_release);
    gCharacterCallbackOriginProbeUntilTickMs.store(0U, std::memory_order_release);
    gLogicSwitchHookInstalled.store(false, std::memory_order_release);
    gShutdownCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    // Do not clear gQueueInputTrampoline here: an in-flight detour can still
    // need the trampoline until HookManager restores the prologue and retains
    // retired trampoline storage for process lifetime.
}

} // namespace world_interactions
} // namespace aimp
