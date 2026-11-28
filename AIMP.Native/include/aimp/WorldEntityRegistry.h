#pragma once

#include "aimp/WorldReplicationTypes.h"
#include "aimp/WorldPublicActionPolicy.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aimp {

class Diagnostics;
class LevelGenerationTracker;
class SignatureResolver;
class WorldEntityRegistry;
struct CathodePrimitiveLeaseAccess;
struct WorldInteractionLeaseAccess;
struct WorldPublicActionLeaseAccess;
struct ItemSyncLeaseAccess;

struct CathodeRuntimeKey final {
    std::uint32_t entityA{};
    std::uint32_t entityB{};

    [[nodiscard]] constexpr bool operator==(const CathodeRuntimeKey&) const noexcept = default;
};

// Owns the shared-ref returned by the retail CATHODE resolver. The execution
// object is valid only while this lease is alive. v12.3 deliberately does not
// cache the raw object pointer after releasing the native reference.
class WorldEntityLease final {
public:
    WorldEntityLease() noexcept = default;
    ~WorldEntityLease() noexcept;

    WorldEntityLease(const WorldEntityLease&) = delete;
    WorldEntityLease& operator=(const WorldEntityLease&) = delete;

    WorldEntityLease(WorldEntityLease&& other) noexcept;
    WorldEntityLease& operator=(WorldEntityLease&& other) noexcept;

    [[nodiscard]] void* Get() const noexcept { return executionObject_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return sharedRef_ != nullptr && executionObject_ != nullptr;
    }

    void Reset() noexcept;

private:
    friend class WorldEntityRegistry;
    friend struct CathodePrimitiveLeaseAccess;
    friend struct WorldInteractionLeaseAccess;
    friend struct WorldPublicActionLeaseAccess;
    friend struct ItemSyncLeaseAccess;

    WorldEntityLease(
        void* sharedRef,
        void* executionObject,
        const void* assignAddress) noexcept;

    void* sharedRef_{};
    void* executionObject_{};
    const void* assignAddress_{};
};

enum class WorldRegistryResolveStatus : std::uint8_t {
    Resolved = 0,
    ManifestNotLoaded,
    EntityNotInManifest,
    GenerationMismatch,
    RuntimeUnavailable,
    PendingUntilAvailable,
    NativeFault,
};

struct WorldRegistryResolveResult final {
    WorldRegistryResolveStatus status{WorldRegistryResolveStatus::ManifestNotLoaded};
    WorldEntityLease lease;
    CathodeRuntimeKey key{};
    std::uint32_t zoneId{};
};

struct WorldRegistryProbeSummary final {
    std::uint64_t eligible{};
    std::uint64_t activeEligible{};
    std::uint64_t probed{};
    std::uint64_t resolved{};
    std::uint64_t pending{};
    std::uint64_t nativeFaults{};

    // Provider contexts are the normal nested-composite lookup path used by
    // FUN_009236A0. The provider registry is scanned only by this explicit
    // one-shot diagnostic; there is no frame polling.
    std::uint64_t activeProviders{};
    std::uint64_t manifestActiveProviderContexts{};

    // Manager collections are fallback/special sets, not the world registry.
    // They remain useful for a bounded live control proving that the generic
    // retail resolver accepts identities extracted from real runtime objects.
    std::uint64_t runtimeRootEntries{};
    std::uint64_t runtimeLoadedEntries{};
    std::uint64_t runtimeUniqueKeys{};
    std::uint64_t runtimeManifestExact{};
    std::uint64_t controlResolverChecks{};
    std::uint64_t controlResolverResolved{};
    std::uint64_t controlResolverPending{};
    std::uint64_t controlNativeFaults{};
};

struct WorldRegistryDiagnostics final {
    bool initialized{};
    bool manifestLoaded{};
    std::uint64_t manifestEntities{};
    std::uint64_t resolveRequests{};
    std::uint64_t resolveHits{};
    std::uint64_t pendingMisses{};
    std::uint64_t nativeFaults{};
    std::uint64_t invalidations{};
    std::uint64_t knownLoadedZones{};
    std::uint64_t knownUnloadedZones{};
    std::uint32_t levelGeneration{};
    WorldEntityId mapContentId{};
};

struct WorldRegistryContext final {
    bool manifestLoaded{};
    std::uint32_t levelGeneration{};
    WorldEntityId mapContentId{};
};

// Fix3.23 diagnostic-only opaque state capture. The snapshot is intentionally
// local-only: it never becomes a WorldMutation payload and never exposes raw
// process addresses. The provider/runtime object bytes are typed-neutral
// evidence used only to identify which already-discovered CATHODE context
// changed between two explicit operator snapshots.
inline constexpr std::size_t kWorldOpaqueStateObjectBytes = 128U;
inline constexpr std::size_t kWorldOpaqueStatePointeeBytes = 64U;
inline constexpr std::size_t kWorldOpaqueStateMaximumPointerProbes = 12U;
inline constexpr std::uint32_t kWorldOpaqueStateMaximumEntries = 65'536U;

enum class WorldOpaqueStateSource : std::uint8_t {
    ProviderRegistry = 1,
    ManagerLoadedCollection = 2,
    ManagerRootCollection = 3,
};

struct WorldOpaqueStatePointerProbe final {
    std::uint16_t ownerOffset{};
    std::uint16_t readableBytes{};
    std::uint16_t pointerWordMask{};
    std::uint16_t reserved{};
    std::uint64_t hash{};
    std::array<std::byte, kWorldOpaqueStatePointeeBytes> bytes{};
};

struct WorldOpaqueStateEntry final {
    WorldOpaqueStateSource source{WorldOpaqueStateSource::ProviderRegistry};
    std::uint32_t sourceIndex{};
    std::uint32_t providerContextKey{};
    CathodeRuntimeKey runtimeKey{};
    bool runtimeKeyValid{};
    WorldEntityId worldEntity{};
    bool worldEntityValid{};
    std::uint16_t objectReadableBytes{};
    std::uint32_t objectPointerWordMask{};
    std::uint64_t objectHash{};
    std::array<std::byte, kWorldOpaqueStateObjectBytes> objectBytes{};
    std::uint16_t pointerProbeCount{};
    std::array<WorldOpaqueStatePointerProbe,
               kWorldOpaqueStateMaximumPointerProbes> pointerProbes{};
};

struct WorldOpaqueStateSnapshot final {
    std::uint32_t levelGeneration{};
    std::uint64_t providerEntries{};
    std::uint64_t managerLoadedEntries{};
    std::uint64_t managerRootEntries{};
    std::uint64_t inspectedEntries{};
    std::uint64_t capturedEntries{};
    std::uint64_t runtimeKeyEntries{};
    std::uint64_t worldMappedEntries{};
    std::uint64_t objectReadFailures{};
    std::uint64_t objectPartialReads{};
    std::uint64_t pointerProbeReadFailures{};
    std::uint64_t pointerProbePartialReads{};
    std::uint64_t capacityDrops{};
    std::uint64_t elapsedMs{};
    std::vector<WorldOpaqueStateEntry> entries;
};

// Fix3.23 BuildFix01: resumable engine-thread capture cursor. The cursor owns
// only local diagnostic bytes and numeric collection positions; it never
// retains a raw native entity pointer between engine callbacks. This lets the
// full provider/runtime surface be captured in small bounded slices instead of
// blocking one engine callback for seconds.
enum class WorldOpaqueStateCapturePhase : std::uint8_t {
    ProviderRegistry = 0,
    ManagerLoadedCollection = 1,
    ManagerRootCollection = 2,
    Complete = 3,
    GenerationInvalidated = 4,
};

struct WorldOpaqueStateCaptureCursor final {
    WorldOpaqueStateSnapshot snapshot{};
    WorldOpaqueStateCapturePhase phase{
        WorldOpaqueStateCapturePhase::ProviderRegistry};
    std::uint32_t providerIndex{};
    std::uint32_t managerLoadedIndex{};
    std::uint32_t managerRootIndex{};
    std::uint32_t maximumEntries{kWorldOpaqueStateMaximumEntries};
    std::uint64_t startedTickMs{};
    bool initialized{};
    bool complete{};
};


struct CompositePublicInputManifestDiagnostics final {
    bool manifestReady{};
    bool guidKnown{};
    bool exactForwardKeyPresent{};
    bool exactForwardAllowed{};
    bool exactOwnerSeenForGuid{};
    std::uint32_t exactBridgeCount{};
    std::uint32_t exactPayloadlessBridgeCount{};
    std::uint32_t exactTypedBridgeCount{};
    std::uint32_t guidBridgeCount{};
    std::uint32_t guidOwnerCount{};
    WorldEntityId firstGuidOwner{};
    std::uint32_t firstGuidDataType{};
    bool firstGuidNoTypedData{};
};

class WorldEntityRegistry final {
    friend struct WorldManifestRuntimeTestAccess;
public:
    WorldEntityRegistry(
        const SignatureResolver& resolver,
        LevelGenerationTracker& generations,
        Diagnostics& diagnostics) noexcept;

    [[nodiscard]] bool Initialize(std::string& error) noexcept;
    // Starts/polls an isolated file/index worker. False + the exact
    // WORLD_MANIFEST_PREPARING sentinel means retry; other errors are terminal.
    // This never activates capture or binds the index to a live generation.
    [[nodiscard]] bool PrepareManifestRuntimeIndex(
        const std::filesystem::path& path,
        std::string& error) noexcept;
    [[nodiscard]] bool LoadManifestRuntimeIndex(
        const std::filesystem::path& path,
        std::uint32_t levelGeneration,
        std::string& error) noexcept;

    [[nodiscard]] WorldRegistryResolveResult Resolve(
        const WorldEntityId& id) noexcept;
    [[nodiscard]] std::optional<WorldEntityId> FindWorldEntityId(
        const CathodeRuntimeKey& key) const noexcept;
    [[nodiscard]] std::optional<WorldEntityId> FindRuntimeWorldEntityId(
        const CathodeRuntimeKey& key) const noexcept;
    // Fix3.24: manifest-generated direct public Composite input bridge.
    // Reverse lookup is used only to canonicalize a proven internal input;
    // forward lookup is used by the receiver to fail closed before QueueInput.
    [[nodiscard]] std::optional<WorldCompositePublicInputDescriptor>
        FindCompositePublicInputForDirectEdge(
            const WorldEntityId& interfaceVariable,
            std::uint32_t interfaceLinkParameter,
            const WorldEntityId& internalTarget,
            std::uint32_t internalTargetInputShortGuid) const noexcept;
    [[nodiscard]] std::optional<WorldCompositePublicInputDescriptor>
        FindCompositePublicInputForInternalEdge(
            const WorldEntityId& internalTarget,
            std::uint32_t internalTargetInputShortGuid) const noexcept;
    // Fix3.25: exact runtime-key mirror of the static internal-edge reverse map.
    // QueueInput can expose a nested CATHODE endpoint whose execution object is
    // not safely classifiable as a World-domain entity, while the retail
    // RuntimeKey is still exact. The runtime key is used only for local lookup;
    // the network payload remains stable CompositeInstance + public ShortGuid.
    [[nodiscard]] std::optional<WorldCompositePublicInputDescriptor>
        FindCompositePublicInputForRuntimeEdge(
            const CathodeRuntimeKey& internalTarget,
            std::uint32_t internalTargetInputShortGuid) const noexcept;
    // Fix3.26: runtime evidence shows that nested Composite internals may keep
    // the owning CompositeInstance's runtime scope key (entityB) instead of the
    // child-path-derived entityB serialized for the internal manifest entity.
    // Canonicalize only when COMMANDS gives one unique payloadless public input
    // for (owner runtime scope B, internal entity A, internal input). The
    // RuntimeKey remains process-local; the wire contract is unchanged.
    [[nodiscard]] std::optional<WorldCompositePublicInputDescriptor>
        FindCompositePublicInputForOwnerScopeRuntimeEdge(
            const CathodeRuntimeKey& internalTarget,
            std::uint32_t internalTargetInputShortGuid) const noexcept;
    // Fix3.24 BuildFix05: compiler-generated, fail-closed descendant anchor.
    // This is consulted only after the direct public->internal reverse map misses.
    [[nodiscard]] std::optional<WorldCompositePublicInputDescriptor>
        FindCompositePublicInputForCanonicalAnchor(
            const WorldEntityId& internalTarget,
            std::uint32_t internalTargetInputShortGuid) const noexcept;
    [[nodiscard]] bool IsCompositePublicInputAllowed(
        const WorldEntityId& ownerCompositeInstance,
        std::uint32_t publicInputShortGuid) const noexcept;
    // WS3 retail WS3 retail ROOT only: exact forward owner/input and one public
    // interface identity, regardless of static DataType/noTypedData metadata.
    // This does NOT prove an invocation payloadless. The sender must first
    // validate its actual callback payload words, delay, tail and callback mode.
    [[nodiscard]] bool IsCompositePublicRootKnown(
        const WorldEntityId& ownerCompositeInstance,
        std::uint32_t publicInputShortGuid) const noexcept;
    // One shared policy for submission, acknowledgements, mutation validation
    // and replay. Extended shapes require World endpoints and an exact target
    // relation in the manifest; they never infer target = owner.
    [[nodiscard]] bool IsCompositePublicRootActionKnown(
        const WorldEntityId& owner, std::uint32_t input,
        const WorldEntityId& target, double delay, std::uint8_t flags) const noexcept;
    // Diagnostic-only introspection of the already-loaded forward public-input index.
    // This never authorizes gameplay and is used only by the controlled WS3 ProbeDirect witness.
    [[nodiscard]] CompositePublicInputManifestDiagnostics InspectCompositePublicInputManifest(
        const WorldEntityId& ownerCompositeInstance,
        std::uint32_t publicInputShortGuid) const noexcept;
    // WS3 v6 compiler-generated semantic graph summary. This is a pure manifest
    // lookup: no runtime graph scan and no gameplay-specific ShortGuid policy.
    [[nodiscard]] std::optional<WorldCompositePublicInputCausalProfile>
        FindCompositePublicInputCausalProfile(
            const WorldEntityId& ownerCompositeInstance,
            std::uint32_t publicInputShortGuid) const noexcept;
    [[nodiscard]] std::optional<WorldCompositePublicInputCausalProfile>
        FindEmittedOutputCausalProfile(const WorldEntityId& owner, std::uint32_t output) const noexcept;
    [[nodiscard]] std::optional<WorldCompositePublicInputCausalProfile>
        FindScheduledInputCausalProfile(const WorldEntityId& target, std::uint32_t input) const noexcept;
    // Patch 4.1: manifest-derived inverse ownership index. A ScheduledInput
    // StateTransition covered by a certified higher ReplicateCarrier must not
    // become a fallback network ROOT merely because the carrier happened
    // before WorldBus activation or was otherwise preserved locally.
    [[nodiscard]] bool HasCertifiedStateTransitionCarrier(
        const WorldEntityId& semanticTarget, std::uint32_t semanticOperation) const noexcept;
    // Fix3.24 BuildFix04: canonicalize the actual retail boundary where a
    // QueueInput targets a CompositeInstance public port. The ShortGuid-only
    // prefilter prevents runtime-key extraction on unrelated hot-path edges.
    [[nodiscard]] bool CouldBeCompositePublicInputShortGuid(
        std::uint32_t publicInputShortGuid) const noexcept;
    // Fix3.25 hot-path prefilter for already-resolved internal work items.
    [[nodiscard]] bool CouldBeCompositeInternalInputShortGuid(
        std::uint32_t internalInputShortGuid) const noexcept;
    [[nodiscard]] std::optional<WorldCompositePublicInputDescriptor>
        FindCompositePublicInputForTarget(
            const WorldEntityId& ownerCompositeInstance,
            std::uint32_t publicInputShortGuid) const noexcept;
    // Fix3.27 BuildFix01: FUN_00921F70 can expose the runtime object of the
    // public VariableEntity rather than the owning CompositeInstance. The
    // manifest already carries that stable interfaceVariableEntity identity.
    // This lookup stays fail-closed on ambiguity and never serializes runtime
    // pointers/RuntimeKeys.
    [[nodiscard]] std::optional<WorldCompositePublicInputDescriptor>
        FindCompositePublicInputForInterfaceTarget(
            const WorldEntityId& interfaceVariableEntity,
            std::uint32_t publicInputShortGuid) const noexcept;
    // Fix3.24 BuildFix01: return the exact COMMANDS childLinks that implement a
    // canonical public Composite input. The wire remains owner+public input;
    // only process-local stable IDs/ShortGuids are returned to the receiver.
    [[nodiscard]] std::vector<WorldCompositePublicInputDescriptor>
        FindCompositePublicInputBridges(
            const WorldEntityId& ownerCompositeInstance,
            std::uint32_t publicInputShortGuid) const noexcept;
    // Fix3.14 diagnostic helper: returns the manifest-local CATHODE key for a
    // stable WorldEntityId without resolving/acquiring the runtime object.
    [[nodiscard]] std::optional<CathodeRuntimeKey> FindRuntimeWorldEntityKey(
        const WorldEntityId& id) const noexcept;
    // Local diagnostic metadata from the existing manifest entry. No runtime
    // resolution, new index, or manifest/cache format change.
    [[nodiscard]] std::optional<WorldEntityKind> FindWorldEntityKind(
        const WorldEntityId& id) const noexcept;
    // Local manifest metadata used by WS3 to classify the immediate retained
    // causal parent of a public action. This is process-local policy metadata.
    [[nodiscard]] std::optional<WorldDomain> FindWorldEntityDomain(
        const WorldEntityId& id) const noexcept;
    // WM09 fixed-width runtime metadata used for O(1) relevance diagnostics
    // and semantic-contract transport. PrimitiveType never decides by
    // owner/GUID at runtime; the compiler already encoded the semantic
    // disposition and contract in the causal profile.
    [[nodiscard]] bool IsPlayerMissionCompleteEndpoint(const WorldEntityId& id) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> FindWorldEntityPrimitiveType(
        const WorldEntityId& id) const noexcept;
    // AvS pass 18: every manifest entity of one primitive type (e.g. Door),
    // at most `limit`. Empty while no manifest is loaded.
    [[nodiscard]] std::vector<WorldEntityId> FindWorldEntitiesOfPrimitiveType(
        std::uint32_t primitiveType, std::size_t limit) const noexcept;
    [[nodiscard]] std::optional<WorldEntityId> IdentifyRuntimeEntity(
        const void* runtimeEntity,
        CathodeRuntimeKey* observedKey = nullptr) const noexcept;
    // Hot-path helper for WS3 causal provenance. Extracts the runtime key once
    // and returns the manifest domain with one registry lock; no pointer is
    // retained and no scan is performed.
    [[nodiscard]] std::optional<WorldDomain> IdentifyRuntimeEntityDomain(
        const void* runtimeEntity) const noexcept;
    // v12.7 Fix 1 hot-path filter: identifies only runtime-addressable
    // World-domain entities. This prevents the generic CATHODE observer from
    // turning AI/Player/UI graph traffic into environment replication.
    [[nodiscard]] std::optional<WorldEntityId> IdentifyRuntimeWorldEntity(
        const void* runtimeEntity,
        CathodeRuntimeKey* observedKey = nullptr) const noexcept;
    // Event-driven raw-key extraction used by generic runtime observers before
    // the manifest is armed. This performs no world scan and does not retain
    // the runtime pointer.
    [[nodiscard]] bool TryExtractRuntimeKey(
        const void* runtimeEntity,
        CathodeRuntimeKey& key) const noexcept;
    // v13.3 Fix3.10: transient resolver for a stable CATHODE runtime key that
    // is not required to belong to a manifest WorldEntity. The returned
    // shared-ref remains owned by the lease and must never escape its lifetime.
    // This is intentionally narrow: callers still need an independently
    // resolved manifest WorldEntity for the actual mutation target.
    [[nodiscard]] WorldEntityLease ResolveRuntimeReference(
        const CathodeRuntimeKey& key,
        bool& nativeFault) const noexcept;

    [[nodiscard]] WorldRegistryProbeSummary ProbeWorldEnvironment(
        std::uint32_t maximumEntities = 128U) noexcept;
    // Fix3.23: enumerate the full active provider registry plus both manager
    // runtime collections in one explicit, bounded local snapshot. No native
    // reference is retained and no pointer address leaves this method.
    [[nodiscard]] WorldOpaqueStateSnapshot CaptureOpaqueRuntimeStateSnapshot(
        std::uint32_t maximumEntries = kWorldOpaqueStateMaximumEntries) const noexcept;
    // BuildFix01 non-blocking equivalent used by WORLD_INTERACTION_STATS.
    // Each call processes at most maximumSlots and also obeys maximumStepMs
    // after at least one slot, then returns to the engine immediately.
    void BeginOpaqueRuntimeStateCapture(
        WorldOpaqueStateCaptureCursor& cursor,
        std::uint32_t maximumEntries = kWorldOpaqueStateMaximumEntries) const noexcept;
    [[nodiscard]] bool ContinueOpaqueRuntimeStateCapture(
        WorldOpaqueStateCaptureCursor& cursor,
        std::uint32_t maximumSlots = 8U,
        std::uint32_t maximumStepMs = 1U) const noexcept;
    [[nodiscard]] WorldRegistryDiagnostics DiagnosticsSnapshot() const noexcept;
    // O(1) transport context; no diagnostic zone enumeration per mutation.
    [[nodiscard]] WorldRegistryContext ReplicationContext() const noexcept;
    // Hot-path readiness check for generic CATHODE observers. Unlike
    // DiagnosticsSnapshot(), this is lock-free and performs no zone iteration.
    [[nodiscard]] bool RuntimeCaptureReady() const noexcept {
        return captureReady_.load(std::memory_order_acquire);
    }

    // Zone support remains event-driven. v12.3 never polls unloaded entities.
    void NotifyZoneUnloaded(std::uint32_t zoneId) noexcept;
    void NotifyZoneLoaded(std::uint32_t zoneId) noexcept;
    void InvalidateAll() noexcept;

private:
    struct RuntimeEntry final {
        WorldEntityId id{};
        CathodeRuntimeKey key{};
        std::uint32_t zoneId{};
        std::uint16_t flags{};
        WorldDomain domain{WorldDomain::Unknown};
        WorldEntityKind kind{WorldEntityKind::Unknown};
        std::uint32_t primitiveType{};
    };

    struct WorldEntityIdHash final {
        [[nodiscard]] std::size_t operator()(const WorldEntityId& value) const noexcept;
    };

    struct CompositeInputKey final {
        WorldEntityId entity{};
        std::uint32_t inputShortGuid{};
        [[nodiscard]] bool operator==(const CompositeInputKey& other) const noexcept = default;
    };

    struct CompositeInputKeyHash final {
        [[nodiscard]] std::size_t operator()(const CompositeInputKey& value) const noexcept;
    };

    struct CompositeRuntimeInputKey final {
        CathodeRuntimeKey entity{};
        std::uint32_t inputShortGuid{};
        [[nodiscard]] bool operator==(const CompositeRuntimeInputKey& other) const noexcept = default;
    };

    struct CompositeRuntimeInputKeyHash final {
        [[nodiscard]] std::size_t operator()(const CompositeRuntimeInputKey& value) const noexcept;
    };

    struct CompositeOwnerScopeRuntimeInputKey final {
        std::uint32_t ownerScopeB{};
        std::uint32_t internalEntityA{};
        std::uint32_t inputShortGuid{};
        [[nodiscard]] bool operator==(
            const CompositeOwnerScopeRuntimeInputKey& other) const noexcept = default;
    };

    struct CompositeOwnerScopeRuntimeInputKeyHash final {
        [[nodiscard]] std::size_t operator()(
            const CompositeOwnerScopeRuntimeInputKey& value) const noexcept;
    };

    struct CompositeInputReverseEntry final {
        WorldCompositePublicInputDescriptor descriptor{};
        bool ambiguous{};
    };

    struct CompositeDirectInputKey final {
        WorldEntityId interfaceVariable{};
        std::uint32_t interfaceLinkParameter{};
        WorldEntityId internalTarget{};
        std::uint32_t internalTargetInputShortGuid{};
        [[nodiscard]] bool operator==(const CompositeDirectInputKey& other) const noexcept = default;
    };

    struct CompositeDirectInputKeyHash final {
        [[nodiscard]] std::size_t operator()(const CompositeDirectInputKey& value) const noexcept;
    };

    [[nodiscard]] WorldEntityLease ResolveNative(
        const CathodeRuntimeKey& key,
        bool& nativeFault) const noexcept;
    [[nodiscard]] bool ExtractRuntimeKeyNative(
        const void* runtimeEntity,
        CathodeRuntimeKey& key) const noexcept;

    const SignatureResolver* resolver_{};
    LevelGenerationTracker* generations_{};
    Diagnostics* diagnostics_{};
    const void* managerRootAddress_{};
    const void* resolveEntityAddress_{};
    const void* extractRuntimeKeyAddress_{};
    const void* assignAddress_{};
    const void* providerRegistryRootAddress_{};

    mutable std::mutex mutex_;
    bool initialized_{};
    bool manifestLoaded_{};
    std::atomic<bool> captureReady_{false};
    std::uint32_t manifestGeneration_{};
    WorldEntityId mapContentId_{};
    std::unordered_map<WorldEntityId, RuntimeEntry, WorldEntityIdHash> entries_;
    std::unordered_map<std::uint64_t, WorldEntityId> idsByRuntimeKey_;
    std::unordered_map<CompositeDirectInputKey, CompositeInputReverseEntry, CompositeDirectInputKeyHash> compositeInputDirect_;
    std::unordered_map<CompositeInputKey, CompositeInputReverseEntry, CompositeInputKeyHash> compositeInputReverse_;
    std::unordered_map<CompositeRuntimeInputKey, CompositeInputReverseEntry, CompositeRuntimeInputKeyHash> compositeInputRuntimeReverse_;
    std::unordered_map<CompositeOwnerScopeRuntimeInputKey, CompositeInputReverseEntry, CompositeOwnerScopeRuntimeInputKeyHash> compositeInputOwnerScopeRuntimeReverse_;
    std::unordered_map<CompositeInputKey, CompositeInputReverseEntry, CompositeInputKeyHash> compositeInputCanonicalAnchors_;
    std::unordered_map<CompositeInputKey, CompositeInputReverseEntry, CompositeInputKeyHash> compositeInputInterfaceTargets_;
    std::unordered_map<CompositeInputKey, bool, CompositeInputKeyHash> compositeInputForward_;
    // Load-time ambiguity cache for the existing FORWARD keys, never a reverse
    // owner lookup. Several childLinks of one public interface remain valid.
    std::unordered_set<CompositeInputKey, CompositeInputKeyHash> compositePublicRootAmbiguous_;
    world_actions::PublicRootIndex compositePublicRoots_;
    std::unordered_map<CompositeInputKey, std::vector<WorldCompositePublicInputDescriptor>, CompositeInputKeyHash> compositeInputForwardBridges_;
    std::unordered_map<CompositeInputKey, WorldCompositePublicInputCausalProfile, CompositeInputKeyHash> compositeInputCausalProfiles_, emittedOutputCausalProfiles_, scheduledInputCausalProfiles_;
    std::unordered_set<CompositeInputKey, CompositeInputKeyHash> certifiedStateTransitionCarrierSemantics_;
    std::unordered_set<std::uint32_t> compositePublicInputShortGuids_;
    std::unordered_set<std::uint32_t> compositeInternalInputShortGuids_;
    std::unordered_map<std::uint32_t, bool> loadedZones_;


    struct PreparedManifest final {
        WorldEntityId mapContentId{};
        // Identity obtained from the same read-locked file used by the parser.
        std::array<std::uint32_t, 7> fileIdentity{};
        decltype(entries_) entries_;
        decltype(idsByRuntimeKey_) idsByRuntimeKey_;
        decltype(compositeInputDirect_) compositeInputDirect_;
        decltype(compositeInputReverse_) compositeInputReverse_;
        decltype(compositeInputRuntimeReverse_) compositeInputRuntimeReverse_;
        decltype(compositeInputOwnerScopeRuntimeReverse_) compositeInputOwnerScopeRuntimeReverse_;
        decltype(compositeInputCanonicalAnchors_) compositeInputCanonicalAnchors_;
        decltype(compositeInputInterfaceTargets_) compositeInputInterfaceTargets_;
        decltype(compositeInputForward_) compositeInputForward_;
        decltype(compositePublicRootAmbiguous_) compositePublicRootAmbiguous_;
        decltype(compositePublicRoots_) compositePublicRoots_;
        decltype(compositeInputForwardBridges_) compositeInputForwardBridges_;
        decltype(compositeInputCausalProfiles_) compositeInputCausalProfiles_;
        decltype(emittedOutputCausalProfiles_) emittedOutputCausalProfiles_;
        decltype(scheduledInputCausalProfiles_) scheduledInputCausalProfiles_;
        decltype(certifiedStateTransitionCarrierSemantics_) certifiedStateTransitionCarrierSemantics_;
        decltype(compositePublicInputShortGuids_) compositePublicInputShortGuids_;
        decltype(compositeInternalInputShortGuids_) compositeInternalInputShortGuids_;
    };
    struct ManifestPreparation final {
        std::shared_ptr<PreparedManifest> index;
        std::string error;
    };
    [[nodiscard]] static bool ParseManifestRuntimeIndex(
        const std::filesystem::path& path, PreparedManifest& prepared,
        std::string& error) noexcept;
    // Polling is serialized separately from live lookup/capture locks. The
    // worker captures only values and never references this registry or engine.
    std::mutex preparationMutex_;
    std::future<ManifestPreparation> preparationWorker_;
    std::filesystem::path preparationPath_;
    std::shared_ptr<PreparedManifest> preparedManifest_;
    std::shared_ptr<PreparedManifest> retiredManifest_;
    std::string preparationError_;
    std::filesystem::path activeManifestPath_;
    std::array<std::uint32_t, 7> activeManifestFileIdentity_{};

    std::uint64_t resolveRequests_{};
    std::uint64_t resolveHits_{};
    std::uint64_t pendingMisses_{};
    std::uint64_t nativeFaults_{};
    std::uint64_t invalidations_{};
};

} // namespace aimp
