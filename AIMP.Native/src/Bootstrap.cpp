#include "aimp/AISync.h"
#include "aimp/VoiceSync.h"
#include "aimp/Bootstrap.h"
#include "aimp/AnimationSyncPropsRuntime.h"

#include "aimp/AuthorityController.h"
#include "aimp/BuildProfile.h"
#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/CapabilityMatrix.h"
#include "aimp/CaptureCatalog.h"
#include "aimp/ChannelInspector.h"
#include "aimp/CharacterObservationHooks.h"
#include "aimp/CharacterActionProbeDiagnostics.h"
#include "aimp/CharacterActionRuntime.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/HidingPresentationAudio.h"
#include "aimp/CharacterReplicationBinder.h"
#include "aimp/CrashGuard.h"
#include "aimp/DescriptorRegistry.h"
#include "aimp/Diagnostics.h"
#include "aimp/DummyPlayerController.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/GameCommandExecutor.h"
#include "aimp/GameThreadObservationHooks.h"
#include "aimp/HookManager.h"
#include "aimp/IdentityMapper.h"
#include "aimp/InboundDispatcher.h"
#include "aimp/InboundQueue.h"
#include "aimp/LauncherIPC.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/LifecycleCatalog.h"
#include "aimp/NativeCharacterLifecycle.h"
#include "aimp/NativeCharacterSpawner.h"
#include "aimp/NativeLimits.h"
#include "aimp/NativePublishRestorer.h"
#include "aimp/NativeTransportAdapter.h"
#include "aimp/NetZDiscovery.h"
#include "aimp/ParticipantRegistry.h"
#include "aimp/ProductVersion.h"
#include "aimp/PlayerSyncObserver.h"
#include "aimp/PlayerLifecycle.h"
#include "aimp/CampaignSaveRuntime.h"
#include "aimp/UIHeapGuard.h"
#include "aimp/LevelIntegrityGuard.h"
#include "aimp/LoadingContextGuard.h"
#include "aimp/PairedTransformGuard.h"
#include "aimp/TeardownReferenceGuard.h"
#include "aimp/MultiplayerPause.h"
#include "aimp/ItemSync.h"
#include "aimp/ThrowableSync.h"
#include "aimp/PeerManager.h"
#include "aimp/RangeProvider.h"
#include "aimp/SignatureResolver.h"
#include "aimp/SpawnCharacterTracker.h"
#include "aimp/ReplicationCore.h"
#include "aimp/WorldEntityRegistry.h"
#include "aimp/HidingDoorPresentation.h"
#include "aimp/AlienPvPDoors.h"
#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldAnimatedAdapters.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/WorldSemanticReconciliation.h"

#include <array>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aimp {
namespace {

enum class BootstrapState : LONG {
    Loaded = 0,
    Validating = 1,
    ProfileRejected = 2,
    Observing = 3,
    ShuttingDown = 4,
    Stopped = 5,
    Faulted = 6,
};

bool EnvironmentOptIn(const wchar_t* name) {
    wchar_t value[16]{};
    const auto length =
        GetEnvironmentVariableW(name, value, static_cast<DWORD>(_countof(value)));
    return length == 1 && value[0] == L'1';
}

bool ProcessScopedEventOptIn() {
    const auto eventName =
        std::wstring(L"Local\\AIMP.NetZ.ObservationHooks.") +
        std::to_wstring(GetCurrentProcessId());
    const auto eventHandle =
        OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
    if (eventHandle == nullptr) {
        return false;
    }

    CloseHandle(eventHandle);
    return true;
}

void LogResearchGates(Diagnostics& diagnostics) {
    constexpr std::array reports{
        NativeCharacterSpawner::Report(),
        CharacterReplicationBinder::Report(),
        NativeCharacterLifecycle::Report(),
        NativePublishRestorer::Report(),
        RangeProvider::Report(),
        InboundDispatcher::Report(),
        replication_core::Report(),
        cathode_primitives::Report(),
        world_interactions::Report(),
        world_animated::Report(),
        AuthorityController::Report(),
        IdentityMapper::Report(),
        PeerManager::Report(),
    };
    for (const auto& report : reports) {
        diagnostics.Log(
            "info",
            "module_status",
            report.reason,
            {{"module", std::string(report.name)},
             {"status", std::string(ToString(report.status))}});
    }
}

struct NaturalDestructionRuntimeProof {
    std::uint32_t threadId{};
    bool complete{};
};

NaturalDestructionRuntimeProof BuildNaturalDestructionRuntimeProof(
    const std::vector<LifecycleEvent>& lifecycle,
    const std::uint64_t generation) noexcept {
    constexpr std::uint32_t kFullBegin = 1U << 0U;
    constexpr std::uint32_t kRecordBegin = 1U << 1U;
    constexpr std::uint32_t kChannelRelease = 1U << 2U;
    constexpr std::uint32_t kContextRelease = 1U << 3U;
    constexpr std::uint32_t kRecordEnd = 1U << 4U;
    constexpr std::uint32_t kFullEnd = 1U << 5U;
    constexpr std::uint32_t kComplete =
        kFullBegin |
        kRecordBegin |
        kChannelRelease |
        kContextRelease |
        kRecordEnd |
        kFullEnd;

    NaturalDestructionRuntimeProof proof{};
    std::uint32_t observed = 0;
    std::uint64_t activeGeneration = 0;
    for (const auto& event : lifecycle) {
        if (generation != 0 &&
            event.levelGeneration != generation) {
            continue;
        }
        if (event.kind == LifecycleEventKind::CharacterFullDestroyBegin) {
            proof.threadId = event.threadId;
            activeGeneration = event.levelGeneration;
            observed = kFullBegin;
            continue;
        }
        if (proof.threadId == 0 ||
            event.threadId != proof.threadId ||
            event.levelGeneration != activeGeneration) {
            continue;
        }
        switch (event.kind) {
        case LifecycleEventKind::CharacterDestroyBegin:
            observed |= kRecordBegin;
            break;
        case LifecycleEventKind::ChannelReleased:
            observed |= kChannelRelease;
            break;
        case LifecycleEventKind::ReplicationContextReleased:
            observed |= kContextRelease;
            break;
        case LifecycleEventKind::CharacterDestroyEnd:
            observed |= kRecordEnd;
            break;
        case LifecycleEventKind::CharacterFullDestroyEnd:
            observed |= kFullEnd;
            proof.complete = observed == kComplete;
            if (proof.complete) {
                return proof;
            }
            proof.threadId = 0;
            activeGeneration = 0;
            observed = 0;
            break;
        case LifecycleEventKind::CharacterCreateBegin:
        case LifecycleEventKind::CharacterCreateEnd:
        case LifecycleEventKind::CharacterFinalizeBegin:
        case LifecycleEventKind::CharacterFinalizeEnd:
        case LifecycleEventKind::ParticipantAppendBegin:
        case LifecycleEventKind::ParticipantAppendEnd:
        case LifecycleEventKind::ReplicationContextAllocated:
        case LifecycleEventKind::ChannelAllocated:
        case LifecycleEventKind::CharacterFullDestroyBegin:
            break;
        }
    }
    return proof;
}

std::string ReadyMessageHex(
    const std::array<std::byte, 8>& message) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(message.size() * 2U, '0');
    for (std::size_t index = 0; index < message.size(); ++index) {
        const auto value = std::to_integer<std::uint8_t>(message[index]);
        output[index * 2U] = kHex[(value >> 4U) & 0x0FU];
        output[index * 2U + 1U] = kHex[value & 0x0FU];
    }
    return output;
}

} // namespace

Bootstrap& Bootstrap::Instance() noexcept {
    static Bootstrap instance;
    return instance;
}

void Bootstrap::SetModule(const HMODULE module) noexcept {
    module_ = module;
}

DWORD Bootstrap::Run() noexcept {
    try {
        return RunImpl();
    } catch (...) {
        InterlockedExchange(
            &state_,
            static_cast<LONG>(BootstrapState::Faulted));
        RequestStop();
        return 3;
    }
}

DWORD Bootstrap::RunImpl() {
#if !defined(_M_IX86)
#error AIMP.NetZ.dll must be compiled for MSVC x86.
#endif
    static_assert(sizeof(void*) == 4, "AIMP native ABI requires x86 pointers.");

    InterlockedExchange(
        &state_,
        static_cast<LONG>(BootstrapState::Validating));
    const auto stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr) {
        InterlockedExchange(
            &state_,
            static_cast<LONG>(BootstrapState::Faulted));
        return 1;
    }
    stopEvent_.store(stopEvent, std::memory_order_release);

    Diagnostics diagnostics;
    diagnostics.Initialize();
    const bool crashRecorderInstalled = crash_guard::InstallUnhandledExceptionRecorder();
    diagnostics.Log(
        crashRecorderInstalled ? "info" : "warning",
        "crash_recorder",
        crashRecorderInstalled
            ? "Unhandled-exception recorder installed for forensic correlation with WS3 ROOT and locomotion flight journals."
            : "Unhandled-exception recorder could not be installed; ROOT and locomotion journals remain active.",
        {{"processId", Diagnostics::Unsigned(GetCurrentProcessId()), true}});
    diagnostics.Log(
        "info",
        "bootstrap_started",
        std::string("AIMP.NetZ ") + kNativeVersion + " started. Patch 5.12 keeps the proven Patch 5.10 WS2 trigger/effect replication unchanged, retires active DoorLockState ROOT authoring, and restores exact Host-path parity for compiler-certified CompositeInput/EmittedOutput StateTransition carriers on non-Host Clients. Client carriers execute retail locally first, the Host authenticates/sequences the same generic ROOTs it already authors, and the authoritative echo is suppressed on the initiating Client. ForceOpen/ForceClose remain probe-only, CanonicalComposite Client publication remains disabled, and ScheduledInput Event plus synthetic Door self-edges remain fail-closed. ROOT v13/v12 decode is unchanged.",
        {{"architecture", "x86"},
         {"runtime", "static_msvc"},
         {"module", Diagnostics::Pointer(module_)}});
    // This tag comes from the same object file as the attachment hook fix.
    // Launcher and DLL revisions are delivered together and must match exactly.
    diagnostics.Log(
        "info",
        "animationsync_patch_build",
        animation_sync::props::BuildTag());
    LogResearchGates(diagnostics);

    const auto executablePath = CurrentExecutablePath();
    const auto& profile = Retail2015Profile();
    const auto validation = ValidateExecutable(executablePath, profile);
    const bool acceptedExecutable = validation.validCandidate;
    diagnostics.Log(
        acceptedExecutable ? "info" : "error",
        "build_validation",
        validation.reason,
        {{"profileId", std::string(profile.id)},
         {"actualSha256", Sha256Hex(validation.actual.sha256)},
         {"hashAvailable", Diagnostics::Boolean(validation.hashAvailable), true},
         {"hashMatch", Diagnostics::Boolean(validation.hashMatch), true},
         {"metadataMatch", Diagnostics::Boolean(validation.metadataMatch), true},
         {"sectionLayoutMatch",
          Diagnostics::Boolean(validation.sectionLayoutMatch),
          true},
         {"protectedSectionsMatch",
          Diagnostics::Boolean(validation.protectedSectionsMatch),
          true},
         {"normalizedHashMatch",
          Diagnostics::Boolean(validation.normalizedHashMatch),
          true},
         {"profileCompatible",
          Diagnostics::Boolean(validation.profileCompatible),
          true},
         {"recognized", Diagnostics::Boolean(validation.recognized), true},
         {"validCandidate",
          Diagnostics::Boolean(validation.validCandidate),
          true},
         {"acceptedExecutable",
          Diagnostics::Boolean(acceptedExecutable),
          true},
         {"fileSize",
          Diagnostics::Unsigned(validation.actual.fileSize),
          true},
         {"peTimestamp",
          Diagnostics::Unsigned(validation.actual.timeDateStamp),
          true},
         {"imageSize", Diagnostics::Unsigned(validation.actual.sizeOfImage), true},
         {"entryPoint",
          Diagnostics::Unsigned(validation.actual.entryPoint),
          true},
         {"preferredImageBase",
          Diagnostics::Unsigned(validation.actual.preferredImageBase),
          true},
         {"relocations",
          Diagnostics::Boolean(validation.actual.hasRelocations),
          true},
         {"codeView",
          Diagnostics::Boolean(validation.actual.hasCodeView),
          true}});

    InboundQueue inboundQueue;
    CapabilityMatrix capabilities;
    auto captureCatalog = std::make_unique<CaptureCatalog>();
    auto lifecycleCatalog = std::make_unique<LifecycleCatalog>();
    auto levelGenerations = std::make_unique<LevelGenerationTracker>();
    auto gameThreadEvidence =
        std::make_unique<GameThreadRuntimeEvidence>();
    auto gameThreadDispatcher =
        std::make_unique<GameThreadDispatcher>();
    auto spawnTracker =
        std::make_unique<SpawnCharacterTracker>(*levelGenerations, diagnostics);
    DummyPlayerController dummy;
    BlockedDummyExecutor dummyExecutor;

    ApplyFailClosedMutationPolicy(capabilities);

    LauncherIPC ipc(diagnostics, inboundQueue);
    ipc.AttachExperimentalServices(
        capabilities,
        *captureCatalog,
        *levelGenerations,
        *gameThreadDispatcher,
        dummy,
        dummyExecutor);
    ipc.AttachSpawnCharacterTracker(*spawnTracker);

    const auto moduleBase =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::unique_ptr<SignatureResolver> resolver;
    std::unique_ptr<SignatureResolver> fallbackResolver;
    std::unique_ptr<GameCommandExecutor> gameCommands;
    std::unique_ptr<PlayerSyncObserver> playerSyncObserver;
    std::unique_ptr<WorldEntityRegistry> worldRegistry;
    std::unique_ptr<DescriptorRegistry> descriptors;
    std::unique_ptr<ChannelInspector> channels;
    std::unique_ptr<ParticipantRegistry> participants;
    std::unique_ptr<NetZDiscovery> discovery;
    std::unique_ptr<HookManager> hooks;
    std::unique_ptr<CharacterObservationHooks> characterHooks;
    std::unique_ptr<GameThreadObservationHooks> gameThreadHooks;
    NativeTransportAdapter transportAdapter(diagnostics);
    NativeTransportController transportController(transportAdapter);

    const bool fixedRvaEligible = IsFixedRvaEligible(validation);
    bool signaturesValid = false;
    if (acceptedExecutable && moduleBase != 0) {
        resolver = std::make_unique<SignatureResolver>(
            reinterpret_cast<const void*>(moduleBase),
            validation.actual.sizeOfImage,
            profile,
            fixedRvaEligible);
        signaturesValid = fixedRvaEligible &&
            resolver->ValidateAllCodeSymbols();
        diagnostics.Log(
            signaturesValid ? "info" : "warning",
            "signature_validation",
            signaturesValid
                ? "Every fixed-RVA code signature matched; the executable hash was diagnostic only."
                : (fixedRvaEligible
                       ? "Some fixed-RVA signatures differ; commands remain individually gated and essential hooks may use unique signature fallback."
                       : "The historical RVA layout differs; essential hooks will use unique signature discovery."),
            {{"validated", Diagnostics::Boolean(signaturesValid), true},
             {"fixedRvaEligible",
              Diagnostics::Boolean(fixedRvaEligible),
              true},
             {"hashRequired", "false", true}});
        if (fixedRvaEligible) {
            fallbackResolver = std::make_unique<SignatureResolver>(
                reinterpret_cast<const void*>(moduleBase),
                validation.actual.sizeOfImage,
                profile,
                false);
        }
        if (!signaturesValid) {
            auto& genericResolver = fallbackResolver == nullptr
                ? *resolver : *fallbackResolver;
            std::map<SymbolId, SymbolResolution> genericEvidence;
            constexpr SymbolId genericSymbols[]{
                SymbolId::NetzDispatch,
                SymbolId::AllocateChannel,
                SymbolId::ReleaseChannel,
                SymbolId::BuildDescriptorRegistry,
                SymbolId::ParticipantBind,
                SymbolId::ParticipantRecordConstruct,
                SymbolId::ParticipantRecordCopy,
                SymbolId::ParticipantAppend,
                SymbolId::ParticipantDetach,
                SymbolId::CharacterCreate,
                SymbolId::CharacterDestroy,
                SymbolId::CharacterDestroyFull,
                SymbolId::MainThreadCharacterProcessing,
                SymbolId::CharacterManagerUpdate,
                SymbolId::CharacterReadinessProducer,
                SymbolId::CharacterReadinessPublisher,
                SymbolId::CharacterReadinessBarrier,
                SymbolId::ReplicationContextAllocate,
                SymbolId::ReplicationContextRelease,
                SymbolId::ReceiveSpawnRequestF0,
                SymbolId::ReceiveSpawnRequest130,
            };
            for (const auto symbol : genericSymbols) {
                const auto evidence =
                    genericResolver.ResolveWithEvidence(symbol);
                genericEvidence.emplace(symbol, evidence);
                const auto* spec = profile.Find(symbol);
                diagnostics.Log(
                    evidence.kind == ResolutionKind::GenericSignature
                        ? "info"
                        : "warning",
                    "generic_symbol_resolution",
                    evidence.kind == ResolutionKind::GenericSignature
                        ? "Unique masked signature candidate observed in the loaded build's executable .text section; ABI/call-graph/runtime invariants remain unvalidated."
                        : "No unique generic candidate was accepted.",
                    {{"symbol",
                      spec == nullptr
                          ? "unknown"
                          : std::string(spec->name)},
                     {"matches",
                      Diagnostics::Unsigned(evidence.matchCount),
                      true},
                     {"fixedBytes",
                      Diagnostics::Unsigned(evidence.fixedByteCount),
                      true},
                     {"section", evidence.sectionName}});
            }
            const auto observed =
                [&](const SymbolId symbol) {
                    const auto found = genericEvidence.find(symbol);
                    return found != genericEvidence.end() &&
                        found->second.kind ==
                            ResolutionKind::GenericSignature;
                };
            if (observed(SymbolId::NetzDispatch)) {
                capabilities.Set(
                    Capability::ObserveNetZ,
                    CapabilityState::Observed,
                    "generic_unique_text_signature_only_no_layout_or_runtime_validation");
            }
            if (observed(SymbolId::ReceiveSpawnRequestF0) &&
                observed(SymbolId::ReceiveSpawnRequest130)) {
                capabilities.Set(
                    Capability::ObserveCharacterRequests,
                    CapabilityState::Observed,
                    "generic_callback_candidates_layout_dependent_hooks_disabled");
            }
            if (observed(SymbolId::CharacterCreate) &&
                observed(SymbolId::CharacterDestroy)) {
                capabilities.Set(
                    Capability::ObserveCharacterLifecycle,
                    CapabilityState::Observed,
                    "generic_lifecycle_candidates_abi_not_validated");
            }
            if (observed(SymbolId::ParticipantBind)) {
                capabilities.Set(
                    Capability::ObserveParticipants,
                    CapabilityState::Observed,
                    "generic_function_candidate_layout_not_validated");
            }
            if (observed(SymbolId::AllocateChannel) &&
                observed(SymbolId::ReleaseChannel)) {
                capabilities.Set(
                    Capability::ObserveChannels,
                    CapabilityState::Observed,
                    "generic_function_candidates_channel_layout_not_validated");
            }
            if (observed(SymbolId::ReplicationContextAllocate) &&
                observed(SymbolId::ReplicationContextRelease)) {
                capabilities.Set(
                    Capability::ObserveReplicationContexts,
                    CapabilityState::Observed,
                    "generic_function_candidate_context_layout_not_validated");
            }
        }

        if (signaturesValid) {
            playerSyncObserver = std::make_unique<PlayerSyncObserver>(*resolver);
            if (playerSyncObserver->Available()) {
                ipc.AttachPlayerSyncObserver(*playerSyncObserver);
                diagnostics.Log(
                    "info",
                    "playersync_local_observer_ready",
                    "PlayerSync native local-player equipment observer is ready. It reads the retail Player equipment manager on the existing CharacterManagerUpdate engine tick and publishes only state changes; no map patch or input monitor is used.");
            } else {
                diagnostics.Log(
                    "warning",
                    "playersync_local_observer_rejected",
                    "PlayerSync could not resolve the retail GlobalStateRoot; local equipment observation is disabled while existing engine-thread hooks remain available.");
            }
        }

        if (signaturesValid) {
            worldRegistry = std::make_unique<WorldEntityRegistry>(
                *resolver,
                *levelGenerations,
                diagnostics);
            std::string worldRegistryError;
            if (worldRegistry->Initialize(worldRegistryError)) {
                diagnostics.Log(
                    "info",
                    "world_entity_registry_ready",
                    "WorldSync 2.0 clean v12.3 generic CATHODE resolver is ready. Each lookup owns the returned shared-ref for the lifetime of its lease; no raw execution pointer cache, constructor hook, global entity scan or frame polling is active.");
            } else {
                diagnostics.Log(
                    "warning",
                    "world_entity_registry_rejected",
                    worldRegistryError);
                worldRegistry.reset();
            }
        }

        if (signaturesValid) {
            gameCommands = std::make_unique<GameCommandExecutor>(
                *resolver,
                *captureCatalog,
                *levelGenerations,
                *gameThreadEvidence,
                *spawnTracker,
                diagnostics,
                validation.hashMatch);
            if (worldRegistry != nullptr) {
                gameCommands->AttachWorldEntityRegistry(*worldRegistry);
            }
            gameCommands->AttachGameThreadDispatcher(*gameThreadDispatcher);
            if (gameCommands->IsAvailable()) {
                ipc.AttachGameCommandServices(*gameCommands);
                diagnostics.Log(
                    "info",
                    "game_command_services_ready",
                    "Fixed-RVA signatures matched; game commands are available regardless of executable hash and may queue before the first engine callback.");
            } else {
                diagnostics.Log(
                    "error",
                    "game_command_services_unavailable",
                    "PopupMessage symbols were not resolved despite fixed-RVA signature validation.");
            }
        }
        if (signaturesValid) {
            const auto text = std::find_if(
                validation.actual.sections.begin(),
                validation.actual.sections.end(),
                [](const PeSectionIdentity& section) {
                    return section.name == ".text";
                });
            const auto textRva = text == validation.actual.sections.end()
                ? profile.textRva
                : text->virtualAddress;
            const auto textSize = text == validation.actual.sections.end()
                ? profile.textSize
                : (std::max)(text->virtualSize, text->rawSize);
            descriptors = std::make_unique<DescriptorRegistry>(
                reinterpret_cast<const void*>(moduleBase),
                validation.actual.sizeOfImage,
                textRva,
                textSize);
            channels = std::make_unique<ChannelInspector>(*descriptors);
            participants = std::make_unique<ParticipantRegistry>();
            discovery = std::make_unique<NetZDiscovery>(
                diagnostics,
                *resolver,
                *channels,
                *participants,
                moduleBase,
                validation.actual.sizeOfImage);
            ipc.AttachDiscovery(*discovery);
            capabilities.Set(
                Capability::ObserveNetZ,
                CapabilityState::StaticValidated,
                "compatible_profile_signatures_and_vtable_validated_runtime_instance_pending");
            capabilities.Set(
                Capability::ObserveCharacterRequests,
                CapabilityState::StaticValidated,
                "exact_callback_abi_and_copy_sizes_F0_0xF0_F130_0x130_validated_hook_pending");
            capabilities.Set(
                Capability::ObserveCharacterLifecycle,
                CapabilityState::StaticValidated,
                "exact_create_destroy_signatures_validated_full_lifecycle_detours_pending");
            capabilities.Set(
                Capability::ObserveParticipants,
                CapabilityState::StaticValidated,
                "compatible_profile_participant_layout_read_only");
            capabilities.Set(
                Capability::ObserveChannels,
                CapabilityState::StaticValidated,
                "compatible_profile_32_channel_layout_read_only");
            capabilities.Set(
                Capability::ObserveReplicationContexts,
                CapabilityState::StaticValidated,
                "compatible_profile_context_pool_root_read_only");
            capabilities.Set(
                Capability::ObserveOutboundDescriptors,
                CapabilityState::StaticValidated,
                "descriptor_registry_observation_only");
            capabilities.Set(
                Capability::ObserveLevelTransitions,
                CapabilityState::Observed,
                "root_tuple_generation_tracker_requires_three_coherent_polls");
        }
    }

    const bool hookOptIn =
        EnvironmentOptIn(L"AIMP_NETZ_ENABLE_OBSERVATION_HOOKS") ||
        ProcessScopedEventOptIn();
    hooks = std::make_unique<HookManager>(
        acceptedExecutable,
        true);

    {
        std::string heapError;
        const bool installed = validation.hashMatch &&
            ui_heap_guard::Install(moduleBase, *hooks, heapError);
        diagnostics.Log(installed ? "info" : "error", "ui_heap_guard",
            installed ? "All six retail GFx heap mutators now use their native critical sections (10 certified branches)."
                : (heapError.empty() ? "UI heap synchronization requires the exact executable hash." : heapError));
        if (installed && !heapError.empty()) diagnostics.Log("warning", "ui_heap_guard_protection", heapError);
    }
    {
        std::string gunError;
        const bool installed = validation.hashMatch &&
            ui_heap_guard::InstallGunLayerGuard(moduleBase, *hooks, gunError);
        diagnostics.Log(installed ? "info" : "error", "gun_layer_guard",
            installed ? "Retail gun-layer update skips a frame instead of crashing when a character's held item is missing (AvS new-match crash 00674921)."
                : (gunError.empty() ? "Gun layer guard requires the exact executable hash." : gunError));
    }

    {
        std::string integrityError;
        const bool installed = validation.hashMatch &&
            level_integrity_guard::Install(moduleBase, *hooks, integrityError);
        diagnostics.Log(installed ? "info" : "error", "level_integrity_guard",
            installed
                ? (level_integrity_guard::AlreadyBypassed()
                       ? "The executable already bypasses the MODELS_LEVEL.BIN and DLC fingerprints."
                       : "MODELS_LEVEL.BIN and DLC fingerprints bypassed in memory (2 retail CALL sites).")
                : (integrityError.empty()
                       ? "Prepared levels require the exact executable hash; retail will quit on load."
                       : integrityError),
            {{"currentGenScripts", std::string(level_integrity_guard::CurrentGenScriptsEnabled() ? "1" : "0")}});
    }

    {
        std::string contextError;
        const bool installed = validation.hashMatch &&
            loading_context_guard::Install(moduleBase, *hooks, contextError);
        diagnostics.Log(installed ? "info" : "error", "loading_context_guard_initialized",
            installed ? "Certified native loading context readiness guard installed." : contextError);
    }

    {
        std::string pairedError;
        const bool installed = validation.hashMatch &&
            paired_transform_guard::Install(moduleBase, *hooks, pairedError);
        diagnostics.Log(installed ? "info" : "error", "paired_transform_guard_initialized",
            installed ? "Paired animation partner transform guard installed." : pairedError);
    }

    {
        std::string teardownError;
        const bool installed = validation.hashMatch &&
            teardown_reference_guard::Install(moduleBase, *hooks, teardownError);
        diagnostics.Log(installed ? "info" : "error", "teardown_reference_guard_initialized",
            installed ? "Stale entity transform reference guard installed." : teardownError);
    }

    {
        std::string lifecycleError;
        const bool installed = signaturesValid && resolver != nullptr &&
            player_lifecycle::Install(profile, *resolver, *hooks,
                validation.hashMatch, lifecycleError);
        diagnostics.Log(installed ? "info" : "error", "playerlifecycle_initialized",
            installed
                ? "[PlayerLifecycle] initialized: local death cleanup preserved; automatic solo GameOver tail gated by THE_PLAYER and existing AIMP gameplay authority."
                : (lifecycleError.empty()
                    ? "[PlayerLifecycle] unavailable: exact executable/profile signatures required."
                    : lifecycleError));
    }

    {
        std::string campaignError;
        const bool installed = validation.hashMatch &&
            campaign_save::Install(moduleBase, campaignError);
        diagnostics.Log(installed ? "info" : "error", "campaign_save_initialized",
            installed ? (campaign_save::ProfileReloadInstalled()
                ? "Multiplayer campaign saves can be isolated from personal Steam saves; profile swaps reload the retail save profile."
                : "Multiplayer campaign saves can be isolated from personal Steam saves (retail profile reload unavailable).")
                : (campaignError.empty() ? "Campaign save isolation requires the exact executable hash." : campaignError));
    }

    {
        std::string pauseError;
        const bool installed = validation.hashMatch && levelGenerations != nullptr &&
            multiplayer_pause::Install(moduleBase, *hooks, *levelGenerations, pauseError);
        diagnostics.Log(installed ? "info" : "error", "multiplayer_pause_initialized",
            installed ? "Simulation queries ignore native menu/overlay pause reasons only in a stable multiplayer world; UI/input flags stay retail."
                : (pauseError.empty() ? "Multiplayer menu pause requires the exact executable hash." : pauseError));
        if (installed && !pauseError.empty()) diagnostics.Log("warning", "multiplayer_pause_protection", pauseError);
    }

    {
        std::string itemSyncError;
        const bool installed = validation.hashMatch && levelGenerations != nullptr && worldRegistry != nullptr &&
            item_sync::Install(moduleBase, *hooks, *levelGenerations, *worldRegistry, itemSyncError);
        diagnostics.Log(installed ? "info" : "error", "itemsync_initialized",
            installed ? "[ItemSync] initialized: InventoryItem collections are observed; spawned pickups use the pickup manager, placed pickups replay native completion without an inventory grant."
                : (itemSyncError.empty() ? "[ItemSync] unavailable: exact executable hash required." : itemSyncError));
    }

    {
        std::string throwableError;
        const bool installed = validation.hashMatch && levelGenerations != nullptr &&
            throwable_sync::Install(moduleBase, *hooks, *levelGenerations, *gameThreadDispatcher, throwableError);
        diagnostics.Log(installed ? "info" : "error", "throwables_initialized",
            installed ? "Native released item factories and Host lifetime replication installed."
                : (throwableError.empty() ? "Throwables require the exact executable hash." : throwableError));
    }

    if (signaturesValid && resolver != nullptr) {
        std::string spawnTrackerHookError;
        if (spawnTracker->Install(
                *resolver,
                *hooks,
                spawnTrackerHookError)) {
            diagnostics.Log(
                "info",
                "spawn_character_tracker_hook",
                "CharacterCreate hook installed; SpawnCharacter results will publish the local CharacterBase automatically.");
        } else {
            diagnostics.Log(
                "error",
                "spawn_character_tracker_hook_rejected",
                spawnTrackerHookError);
        }
    }

    if (signaturesValid && resolver != nullptr && moduleBase != 0U) {
        std::string characterMovementHookError;
        if (character_direct_step_runtime::Install(
                moduleBase,
                resolver->Resolve(SymbolId::GlobalStateRoot),
                *hooks,
                characterMovementHookError)) {
            diagnostics.Log(
                "info",
                "animationsync_props_hook_installed",
                "AnimationSync attachment observation is installed with persistent and atomic trampoline outputs; it was not skipped.",
                {{"buildId", animation_sync::props::BuildTag()}});
            diagnostics.Log(
                "info",
                "character_movement_realtime_hook",
                "Remote Player Controller installed: network commands publish desired state only; CharacterManagerUpdate owns native Move/Idle transitions. The +257900 Move entry and +25BDD0 default-Idle entry are detoured so bound remote Characters cannot reacquire or tear down locomotion through CIVILIAN AI. v10.8 restores the proven v10.6 direct follower and removes breadcrumb replay plus all writes to MovementAction private velocity fields. RUN is synchronized only through the retail requested/current move_type fields while ActNPCLocomotionCycle remains the physical driver. +28D6D0 remains the game's final locomotion submission; +3ECCA0 is reserved for one-time initial alignment only.");
        } else {
            diagnostics.Log(
                "warning",
                "character_movement_realtime_hook_rejected",
                characterMovementHookError);
        }
    }

    if (signaturesValid && resolver != nullptr) {
        std::string cathodePrimitiveError;
        if (cathode_primitives::Initialize(*resolver, cathodePrimitiveError)) {
            diagnostics.Log(
                "info",
                "worldsync2_cathode_primitives_initialized",
                "WorldSync v13.3 generic CATHODE primitive executor initialized. Event replay still uses FUN_00921F70/FUN_009271C0 and FUN_00930640; persistent LogicSwitch state uses the retail SET/CLEAR methods as replaceable WorldState.");
        } else {
            diagnostics.Log(
                "warning",
                "worldsync2_cathode_primitives_rejected",
                cathodePrimitiveError);
        }
    }

    {
        std::string replicationCoreError;
        if (worldRegistry != nullptr && replication_core::Initialize(
                *levelGenerations,
                *gameThreadDispatcher,
                *worldRegistry,
                replicationCoreError)) {
            diagnostics.Log(
                "info",
                "worldsync2_replication_core_initialized",
                "WorldSync v13.3 ReplicationCore initialized. Host ordering carries generic WS3 ROOTs and replayable StateTransition revisions. Patch 5.3.3 disables the 5.3.2 synthetic Door started_opening/started_closing SchedulerEdge publisher after the client-crash regression; no legacy DoorAction protocol is enabled.");
        } else {
            diagnostics.Log(
                "warning",
                "worldsync2_replication_core_rejected",
                replicationCoreError);
        }
    }

    if (signaturesValid && resolver != nullptr && worldRegistry != nullptr &&
        replication_core::Initialized() && cathode_primitives::Initialized()) {
        // v13.3 Fix1 keeps bootstrap hook-free. Generic input/typed-message
        // and LogicSwitch SET/CLEAR observers are installed only after
        // WORLD_MANIFEST_LOAD arms a compatible runtime world.
        std::string interactionAdapterError;
        if (world_interactions::Initialize(
                *resolver, *hooks, *worldRegistry, interactionAdapterError)) {
            diagnostics.Log(
                "info",
                "worldsync2_interaction_adapters_initialized",
                "WorldSync v13.3 Fix2 generic CATHODE event/state bus prepared hook-free. Input/typed-message and LogicSwitch SET/CLEAR observers arm only after manifest activation; bounded causal telemetry observes scheduler metadata only and adds no network traffic.");
        } else {
            diagnostics.Log(
                "warning",
                "worldsync2_interaction_adapters_rejected",
                interactionAdapterError);
        }

        std::string animatedAdapterError;
        std::string hidingPresentationError;
        if (!hiding_door_presentation::Initialize(*resolver, *worldRegistry, hidingPresentationError))
            diagnostics.Log("warning", "hiding_door_presentation_rejected", hidingPresentationError);
        std::string pvpDoorsError;
        if (!alien_pvp_doors::Initialize(*resolver, *worldRegistry, pvpDoorsError))
            diagnostics.Log("warning", "alien_pvp_doors_rejected", pvpDoorsError);
        if (world_animated::Initialize(
                *resolver, *hooks, *worldRegistry, animatedAdapterError)) {
            diagnostics.Log(
                "info",
                "worldsync3_semantic_world_adapter_prepared",
                "WorldSync specialized adapters initialize hook-free. Patch 5.3.3 keeps DoorNative started_opening/started_closing hooks dormant because the 5.3.2 callbackless self-edge replay regressed client stability. Persistent Door lock state remains on the generic WS3 StateTransition path; generic event-boundary probes remain diagnostic-only.");
        } else {
            diagnostics.Log(
                "warning",
                "worldsync2_animated_adapters_rejected",
                animatedAdapterError);
        }
    }

    if (acceptedExecutable && resolver != nullptr && playerSyncObserver == nullptr) {
        playerSyncObserver = std::make_unique<PlayerSyncObserver>(*resolver);
        if (playerSyncObserver->Available()) {
            ipc.AttachPlayerSyncObserver(*playerSyncObserver);
        }
    }
    if (acceptedExecutable && resolver != nullptr && playerSyncObserver != nullptr) {
        std::string engineHookError;
        gameThreadHooks = std::make_unique<GameThreadObservationHooks>(
            *gameThreadEvidence,
            *gameThreadDispatcher,
            *levelGenerations,
            *playerSyncObserver);
        const bool anyEngineHookInstalled =
            gameThreadHooks->InstallEngineThreadHooks(
                *resolver,
                *hooks,
                engineHookError);
        gameThreadDispatcher->SetHookAvailability(
            gameThreadHooks->MainThreadHookInstalled(),
            gameThreadHooks->ManagerUpdateHookInstalled(),
            gameThreadHooks->ReadinessProducerHookInstalled());
        diagnostics.Log(
            gameThreadHooks->MainThreadHookInstalled() ? "info" : "error",
            "main_thread_character_processing_hook",
            gameThreadHooks->MainThreadHookInstalled()
                ? "MainThreadCharacterProcessing hook installed automatically."
                : "MainThreadCharacterProcessing hook installation failed.",
            {{"installed",
              Diagnostics::Boolean(
                  gameThreadHooks->MainThreadHookInstalled()),
              true}});
        diagnostics.Log(
            gameThreadHooks->ManagerUpdateHookInstalled() ? "info" : "error",
            "character_manager_update_hook",
            gameThreadHooks->ManagerUpdateHookInstalled()
                ? "CharacterManagerUpdate fallback hook installed automatically."
                : "CharacterManagerUpdate fallback hook installation failed.",
            {{"installed",
              Diagnostics::Boolean(
                  gameThreadHooks->ManagerUpdateHookInstalled()),
              true}});
        diagnostics.Log(
            gameThreadHooks->ReadinessProducerHookInstalled()
                ? "info"
                : "error",
            "character_readiness_producer_hook",
            gameThreadHooks->ReadinessProducerHookInstalled()
                ? "CharacterReadinessProducer fallback hook installed automatically."
                : "CharacterReadinessProducer fallback hook installation failed.",
            {{"installed",
              Diagnostics::Boolean(
                  gameThreadHooks->ReadinessProducerHookInstalled()),
              true}});
        if (anyEngineHookInstalled) {
            capabilities.Set(
                Capability::ScheduleGameThreadWork,
                CapabilityState::Observed,
                "engine_thread_hooks_installed_waiting_for_engine_callback");
            if (!engineHookError.empty()) {
                diagnostics.Log(
                    "warning",
                    "engine_thread_hooks_partial",
                    engineHookError);
            }
        } else {
            capabilities.Set(
                Capability::ScheduleGameThreadWork,
                CapabilityState::Blocked,
                "engine_thread_hook_installation_failed");
            diagnostics.Log(
                "error",
                "engine_thread_hooks_rejected",
                engineHookError);
        }
    }

    if (hookOptIn && discovery != nullptr && resolver != nullptr) {
        std::string error;
        characterHooks = std::make_unique<CharacterObservationHooks>(
            *captureCatalog,
            *lifecycleCatalog,
            *levelGenerations,
            spawnTracker.get());
        if (!characterHooks->InstallExactProfile(
                *resolver,
                *hooks,
                error)) {
            diagnostics.Log(
                "error",
                "character_observation_hooks_rejected",
                error);
            capabilities.Set(
                Capability::ObserveCharacterRequests,
                CapabilityState::Blocked,
                "exact_callback_hook_installation_failed");
        } else {
            diagnostics.Log(
                "warning",
                "character_observation_hooks_enabled",
                "Exact-profile F0/F130, participant append, full/record Character destruction, channel and context callbacks feed bounded catalogs; detours perform no I/O or engine mutation.");
        }

        if (gameThreadHooks != nullptr && gameThreadHooks->Installed()) {
            error.clear();
            if (!gameThreadHooks->InstallOptionalReadinessHooks(
                    *resolver,
                    *hooks,
                    error)) {
                diagnostics.Log(
                    "warning",
                    "optional_readiness_hooks_rejected",
                    error);
            } else {
                diagnostics.Log(
                    "info",
                    "optional_readiness_hooks_enabled",
                    "Readiness publisher and barrier hooks are enabled for diagnostics only.");
            }
        }

        const auto* dispatch = profile.Find(SymbolId::NetzDispatch);
        error.clear();
        if (dispatch == nullptr ||
            !discovery->InstallOptionalDispatchObservationHook(
                *hooks,
                *dispatch,
                error)) {
            diagnostics.Log(
                "error",
                "observation_hook_rejected",
                error.empty() ? "dispatch profile entry missing" : error);
        } else {
            diagnostics.Log(
                "warning",
                "observation_hook_enabled",
                "User explicitly enabled the read-only NETZ_DISPATCH cadence hook. No gameplay data is patched.");
        }
    } else {
        diagnostics.Log(
            "info",
            "observation_hooks_disabled",
            hookOptIn
                ? "Optional hook opt-in was present, but exact build/signature validation failed."
                : "Optional Character/readiness/NetZ diagnostics hooks are disabled; essential engine-thread hooks remain automatic.");
    }

    bool aiSyncLifetimesReady = false;
    // Character Actions are a functional feature, not an optional RE probe.
    // Require both destruction notifications even when catalog hooks are off.
    if (validation.hashMatch && signaturesValid && resolver != nullptr &&
        spawnTracker->HookInstalled() && gameThreadHooks != nullptr &&
        (gameThreadHooks->MainThreadHookInstalled() || gameThreadHooks->ManagerUpdateHookInstalled())) {
        if (characterHooks == nullptr) {
            characterHooks = std::make_unique<CharacterObservationHooks>(
                *captureCatalog, *lifecycleCatalog, *levelGenerations, spawnTracker.get());
        }
        std::string actionError;
        aiSyncLifetimesReady = characterHooks->InstallActionLifetimeHooks(*resolver, *hooks, actionError);
        if (aiSyncLifetimesReady &&
            character_action_runtime::Install(*resolver, *hooks, *levelGenerations,
                *gameThreadDispatcher, diagnostics, validation.hashMatch, actionError)) {
            const auto tracked = spawnTracker->Snapshot();
            if (tracked.state == SpawnCharacterTrackingState::Ready) {
                character_action_runtime::ObserveCreated(tracked.characterAddress,
                    tracked.spawnKey, tracked.levelGeneration, true);
            }
            diagnostics.Log("info", "character_action_runtime_installed",
                "Persistent Aim/Primary input enabled at the natural equipment update; two-update release barrier active.");
        } else {
            diagnostics.Log("error", "character_action_runtime_unavailable", actionError);
        }
    } else {
        diagnostics.Log("warning", "character_action_runtime_unavailable",
            "Character Actions require the exact certified executable plus spawn, lifetime and engine-update hooks.");
    }

    { // Installed by default; live execution is controlled by AISyncConfigure.
        std::string aiError;
        const bool ready = validation.hashMatch && aiSyncLifetimesReady &&
            gameThreadHooks != nullptr && gameThreadHooks->ManagerUpdateHookInstalled();
        const bool installed = ready && ai_sync::Install(moduleBase, *hooks,
            *levelGenerations, *gameThreadDispatcher, aiError, worldRegistry.get());
        diagnostics.Log(installed ? "info" : "error", "aisync_v2_install",
            installed ? "AISync V2 installed by default; retail until multiplayer authority. Live toggle: Performance Isolation / AISync. High-level Character convergence, action identity and resolved interaction carriers are synchronized; retail/CATHODE regenerate animation, audio and consequences locally."
                : (aiError.empty() ? "AISync requires exact build, both destruction hooks and CharacterManagerUpdate." : aiError));
    }

    if (validation.hashMatch && ai_sync::Installed()) {
        std::string voiceError;
        const auto installed = voice_sync::Install(moduleBase, *hooks, *levelGenerations, diagnostics, voiceError);
        diagnostics.Log(installed ? "info" : "error", "voicesync_install",
            installed ? "VoiceSync native audio installed; inactive until an authorized multiplayer session." : voiceError);
    }

    const auto hashText = Sha256Hex(validation.actual.sha256);
    const bool staticProfileValidated = signaturesValid;
    const std::string selectedProfileId = signaturesValid
        ? std::string(profile.id)
        : (acceptedExecutable
               ? "discovered-" + hashText.substr(0, 12)
               : "invalid-target");
    const std::string resolutionMode = signaturesValid
        ? (validation.recognized
               ? "ExactProfile"
               : "LocalDiscoveredProfile")
        : "GenericDiscovery";
    const bool ipcStarted = ipc.Start(
        GetCurrentProcessId(),
        staticProfileValidated,
        validation.actual.sha256,
        selectedProfileId,
        resolutionMode);
    diagnostics.Log(
        ipcStarted ? "info" : "error",
        "ipc_start",
        ipcStarted
            ? "Named-pipe client worker started after build and signature classification."
            : "Named-pipe client worker failed to start; no launcher handshake is available.",
        {{"started", Diagnostics::Boolean(ipcStarted), true},
         {"staticProfileValidated",
          Diagnostics::Boolean(staticProfileValidated),
          true}});

    diagnostics.Log(
        "info",
        "transport_status",
        "Logger-only adapter is constructed but Phase 1 bootstrap never attaches it.",
        {{"attached", Diagnostics::Boolean(transportController.IsAttached()), true},
         {"nativeReplicationReady", "false", true}});

    InterlockedExchange(
        &state_,
        static_cast<LONG>(
            validation.validCandidate
                ? BootstrapState::Observing
                : BootstrapState::ProfileRejected));

    std::uint64_t lastLevelGeneration = 0;
    std::uint64_t lastCaptureSequence = 0;
    std::uint64_t lastLifecycleSequence = 0;
    std::uint64_t lastLoggedReadinessProducer = 0;
    std::uint64_t lastLoggedReadinessPublisher = 0;
    std::uint64_t lastLoggedReadinessBarrier = 0;
    std::uint64_t lastReadinessCounterLogTick = 0;
    std::uint64_t lastAISyncCounterLogTick = 0;
    std::uint64_t lastPauseCounterLogTick = 0;
    std::uint32_t lastLoggedEngineThreadId = 0;
    std::uint32_t lastLoggedMismatchedThreadId = 0;
    RuntimeObservationSnapshot lastRuntime{};
    while (WaitForSingleObject(stopEvent, 250) == WAIT_TIMEOUT) {
        player_lifecycle::DrainDiagnostics(diagnostics);
        if (ai_sync::Installed()) ai_sync::DrainLocomotionFlight(diagnostics);
        world_interactions::DrainObservationDiagnostics(diagnostics);
        world_actions::DrainDiagnosticFiles(diagnostics);
        semantic_reconciliation::DrainDiagnosticFiles();
        if (GetTickCount64() - lastPauseCounterLogTick >= 5000ULL) {
            multiplayer_pause::LogDiagnostics(diagnostics);
            loading_context_guard::LogDiagnostics(diagnostics);
            paired_transform_guard::LogDiagnostics(diagnostics);
            teardown_reference_guard::LogDiagnostics(diagnostics);
            item_sync::LogDiagnostics(diagnostics);
            hiding_presentation_audio::LogMetadataDiagnostics(diagnostics);
            lastPauseCounterLogTick = GetTickCount64();
        }
        if (ipc.ShutdownRequested()) {
            RequestStop();
            break;
        }
        if (discovery != nullptr) {
            try {
                discovery->Poll();
                discovery->CollectHookCounters();
                const auto runtime = discovery->RuntimeSnapshot();
                lastRuntime = runtime;
                const auto level = levelGenerations->Observe({
                    runtime.globalRoot,
                    runtime.netz,
                    runtime.characterManager,
                    runtime.participantWrapper,
                    runtime.contextPool});
                if (level.generation != lastLevelGeneration) {
                    lastLevelGeneration = level.generation;
                    captureCatalog->InvalidateGeneration(level.generation);
                    lifecycleCatalog->InvalidateGeneration(level.generation);
                    gameThreadDispatcher->SetGeneration(level.generation);
                    gameThreadEvidence->ResetGeneration(level.generation);
                    spawnTracker->ResetGeneration(level.generation);
                    dummy.InvalidateGeneration(level.generation);
                    replication_core::ResetForGeneration(level.generation);
                    if (worldRegistry != nullptr) {
                        worldRegistry->InvalidateAll();
                    }
                    capabilities.Set(
                        Capability::ObserveCharacterRequests,
                        CapabilityState::StaticValidated,
                        "new_generation_requires_new_natural_F0_and_130_captures");
                    capabilities.Set(
                        Capability::ObserveCharacterLifecycle,
                        CapabilityState::StaticValidated,
                        "new_generation_requires_new_lifecycle_observation");
                    diagnostics.Log(
                        "info",
                        "level_generation_changed",
                        "The observed engine-root tuple changed; level-bound commands and stale mappings were invalidated while the engine-thread binding was preserved.",
                        {{"levelGeneration",
                          Diagnostics::Unsigned(level.generation),
                          true}});
                }
                capabilities.Set(
                    Capability::ObserveLevelTransitions,
                    level.stability == LevelStability::Stable
                        ? CapabilityState::RuntimeValidated
                        : CapabilityState::Observed,
                    level.stability == LevelStability::Stable
                        ? "engine_root_tuple_stable_for_three_polls"
                        : "engine_root_tuple_transition_or_incomplete");
                capabilities.Set(
                    Capability::ObserveNetZ,
                    runtime.netz != 0
                        ? CapabilityState::RuntimeValidated
                        : CapabilityState::StaticValidated,
                    runtime.netz != 0
                        ? "exact_vtable_matched_live_netz_instance"
                        : "live_netz_instance_absent_or_transitioning");
                capabilities.Set(
                    Capability::ObserveParticipants,
                    runtime.participantRegistryAvailable
                        ? CapabilityState::RuntimeValidated
                        : CapabilityState::StaticValidated,
                    runtime.participantRegistryAvailable
                        ? "bounded_participant_snapshot_consistent_count_at_most_8"
                        : "participant_registry_snapshot_absent_or_inconsistent");
                capabilities.Set(
                    Capability::ObserveChannels,
                    runtime.channelSnapshotAvailable
                        ? CapabilityState::RuntimeValidated
                        : CapabilityState::StaticValidated,
                    runtime.channelSnapshotAvailable
                        ? "consistent_32_channel_snapshot_observed"
                        : "channel_snapshot_absent_or_inconsistent");
                capabilities.Set(
                    Capability::ObserveReplicationContexts,
                    runtime.contextPool != 0
                        ? CapabilityState::RuntimeValidated
                        : CapabilityState::StaticValidated,
                    runtime.contextPool != 0
                        ? "exact_profile_context_pool_root_non_null"
                        : "replication_context_pool_absent_or_transitioning");
            } catch (...) {
                diagnostics.Log(
                    "error",
                    "observation_fault",
                    "An exception escaped an observation pass; native pointers were invalidated.");
                discovery->Invalidate("observation_exception");
            }
        }
        captureCatalog->Pump();
        lifecycleCatalog->Pump();
        const auto captures = captureCatalog->Snapshot();
        for (const auto& capture : captures) {
            if (capture.sequence <= lastCaptureSequence ||
                capture.levelGeneration != lastLevelGeneration) {
                continue;
            }
            lastCaptureSequence =
                (std::max)(lastCaptureSequence, capture.sequence);
            diagnostics.Log(
                "info",
                "character_request_capture",
                "A bounded natural Character request was copied outside the hook and serialized into the local JSONL diagnostics.",
                 {{"kind",
                  capture.kind == CharacterRequestKind::SpawnF0
                      ? "F0"
                      : "130"},
                  {"captureSequence",
                  Diagnostics::Unsigned(capture.sequence),
                  true},
                 {"levelGeneration",
                  Diagnostics::Unsigned(capture.levelGeneration),
                  true},
                  {"threadId",
                   Diagnostics::Unsigned(capture.threadId),
                   true},
                  {"performanceCounter",
                   Diagnostics::Unsigned(
                       capture.performanceCounter),
                   true},
                 {"sourceRva",
                  Diagnostics::Unsigned(capture.sourceRva),
                  true},
                 {"size",
                  Diagnostics::Unsigned(capture.size),
                  true},
                 {"payloadHex", CharacterRequestHex(capture)}});
        }
        const auto captureSummary = captureCatalog->Summary();
        if (captureSummary.hasF0 && captureSummary.has130) {
            capabilities.Set(
                Capability::ObserveCharacterRequests,
                CapabilityState::RuntimeValidated,
                "natural_F0_and_130_requests_captured_in_current_generation");
        } else if (characterHooks != nullptr &&
                   characterHooks->Installed()) {
            capabilities.Set(
                Capability::ObserveCharacterRequests,
                CapabilityState::StaticValidated,
                "waiting_for_natural_F0_and_130_captures_in_current_generation");
        }
        const auto lifecycleEvents = lifecycleCatalog->SnapshotAll();
        for (const auto& event : lifecycleEvents) {
            if (event.sequence <= lastLifecycleSequence) {
                continue;
            }
            lastLifecycleSequence = event.sequence;
            capabilities.Set(
                Capability::ObserveCharacterLifecycle,
                CapabilityState::RuntimeValidated,
                "exact_profile_lifecycle_detour_event_observed");
            diagnostics.Log(
                "info",
                "character_lifecycle_observation",
                "Process-local lifecycle values were copied from an exact-profile detour; they are never portable identities.",
                {{"kind", ToString(event.kind)},
                 {"levelGeneration",
                  Diagnostics::Unsigned(event.levelGeneration),
                  true},
                 {"threadId",
                  Diagnostics::Unsigned(event.threadId),
                  true},
                 {"performanceCounter",
                  Diagnostics::Unsigned(
                      event.performanceCounter),
                  true},
                 {"owner",
                  Diagnostics::Pointer(
                      reinterpret_cast<const void*>(event.owner))},
                 {"first",
                  Diagnostics::Pointer(
                      reinterpret_cast<const void*>(event.first))},
                 {"second",
                  Diagnostics::Pointer(
                      reinterpret_cast<const void*>(event.second))},
                  {"result",
                   Diagnostics::Pointer(
                       reinterpret_cast<const void*>(event.result))}});
        }

        const auto destructionProof =
            BuildNaturalDestructionRuntimeProof(
                lifecycleEvents,
                0);
        const auto expiredCommands = gameThreadDispatcher->ExpirePending();
        if (expiredCommands != 0) {
            diagnostics.Log(
                "warning",
                "engine_commands_expired",
                "Queued engine commands expired before execution.",
                {{"count",
                  Diagnostics::Unsigned(expiredCommands),
                  true}});
        }
        const auto threadEvidence = gameThreadEvidence->Snapshot();
        const auto engineThread = gameThreadDispatcher->Snapshot();
        if (engineThread.ready &&
            engineThread.engineThreadId != lastLoggedEngineThreadId) {
            if (lastLoggedEngineThreadId == 0 && validation.hashMatch && moduleBase != 0) {
                auto* testLevelLog = &diagnostics;
                const auto testLevelBase = moduleBase;
                gameThreadDispatcher->Enqueue([testLevelLog, testLevelBase] {
                    for (const auto& level : level_integrity_guard::RegisterTestLevels(testLevelBase)) {
                        testLevelLog->Log(level.result >= 0 ? "info" : "error", "test_level_registered",
                            level.result == 1 ? "Developer test level registered in the engine level registry."
                            : level.result == 0 ? "Developer test level already registered (MAIN.PKG)."
                                                : "Developer test level registration refused.",
                            {{"level", level.name}, {"result", std::to_string(level.result)}});
                    }
                });
            }
            lastLoggedEngineThreadId = engineThread.engineThreadId;
            diagnostics.Log(
                "info",
                "engine_thread_detected",
                "The first valid engine callback bound and enabled the game-thread dispatcher.",
                {{"engineThreadId",
                  Diagnostics::Unsigned(engineThread.engineThreadId),
                  true},
                 {"source", std::string(ToString(engineThread.source))},
                 {"firstCallbackPerformanceCounter",
                  Diagnostics::Unsigned(
                      engineThread.firstCallbackPerformanceCounter),
                  true},
                 {"lastCallbackPerformanceCounter",
                  Diagnostics::Unsigned(
                      engineThread.lastCallbackPerformanceCounter),
                  true},
                 {"engineCallbackCount",
                  Diagnostics::Unsigned(engineThread.callbackCount),
                  true},
                 {"pendingCommandCount",
                  Diagnostics::Unsigned(engineThread.pendingCount),
                  true},
                 {"levelGeneration",
                  Diagnostics::Unsigned(engineThread.generation),
                  true},
                 {"mainThreadCharacterProcessingHookInstalled",
                  Diagnostics::Boolean(
                      engineThread.mainThreadHookInstalled),
                  true},
                 {"characterManagerUpdateHookInstalled",
                  Diagnostics::Boolean(
                      engineThread.managerUpdateHookInstalled),
                  true},
                 {"characterReadinessProducerHookInstalled",
                  Diagnostics::Boolean(
                      engineThread.readinessProducerHookInstalled),
                  true}});
        }
        if (engineThread.mismatchedThreadId != 0 &&
            engineThread.mismatchedThreadId !=
                lastLoggedMismatchedThreadId) {
            lastLoggedMismatchedThreadId =
                engineThread.mismatchedThreadId;
            diagnostics.Log(
                "warning",
                "engine_thread_callback_mismatch",
                "A later engine callback used another thread; the first binding remains active.",
                {{"engineThreadId",
                  Diagnostics::Unsigned(engineThread.engineThreadId),
                  true},
                 {"observedThreadId",
                  Diagnostics::Unsigned(
                      engineThread.mismatchedThreadId),
                  true},
                 {"source", std::string(ToString(engineThread.source))}});
        }
        if (engineThread.ready) {
            capabilities.Set(
                Capability::ScheduleGameThreadWork,
                CapabilityState::RuntimeValidated,
                "engine_thread_bound_" +
                    std::string(ToString(engineThread.source)));
        } else if (engineThread.mainThreadHookInstalled ||
                   engineThread.managerUpdateHookInstalled ||
                   engineThread.readinessProducerHookInstalled) {
            capabilities.Set(
                Capability::ScheduleGameThreadWork,
                CapabilityState::Observed,
                "engine_thread_hooks_installed_waiting_for_engine_callback");
        } else {
            capabilities.Set(
                Capability::ScheduleGameThreadWork,
                CapabilityState::Blocked,
                "engine_thread_hook_installation_failed");
        }

        const auto& readiness = threadEvidence.readiness;
        const auto readinessLogTick = GetTickCount64();
        if (ai_sync::Installed() && (lastAISyncCounterLogTick == 0U ||
                readinessLogTick - lastAISyncCounterLogTick >= 5000U)) {
            lastAISyncCounterLogTick = readinessLogTick;
            ai_sync::LogDiagnostics(diagnostics);
            voice_sync::LogDiagnostics(diagnostics);
        }
        const bool readinessEventChanged =
            readiness.publisher.calls != lastLoggedReadinessPublisher ||
            readiness.barrier.calls != lastLoggedReadinessBarrier;
        const bool producerCounterDue =
            readiness.producer.calls != lastLoggedReadinessProducer &&
            (lastReadinessCounterLogTick == 0 ||
             readinessLogTick - lastReadinessCounterLogTick >= 10000);
        if (readinessEventChanged || producerCounterDue) {
            lastLoggedReadinessProducer = readiness.producer.calls;
            lastLoggedReadinessPublisher = readiness.publisher.calls;
            lastLoggedReadinessBarrier = readiness.barrier.calls;
            lastReadinessCounterLogTick = readinessLogTick;
            diagnostics.Log(
                "info",
                "character_readiness_observation",
                "Engine-thread callback counters were sampled; readiness publisher and barrier remain diagnostics only.",
                {{"levelGeneration",
                  Diagnostics::Unsigned(threadEvidence.generation),
                  true},
                 {"mainThreadCharacterProcessingCalls",
                  Diagnostics::Unsigned(
                      threadEvidence.mainThreadCharacterProcessing.calls),
                  true},
                 {"characterManagerUpdateCalls",
                  Diagnostics::Unsigned(
                      threadEvidence.characterManagerUpdate.calls),
                  true},
                 {"producerCalls",
                  Diagnostics::Unsigned(readiness.producer.calls),
                  true},
                 {"publisherCalls",
                  Diagnostics::Unsigned(readiness.publisher.calls),
                  true},
                 {"barrierCalls",
                  Diagnostics::Unsigned(readiness.barrier.calls),
                  true},
                 {"producerLastQpc",
                  Diagnostics::Unsigned(
                      readiness.producer.lastPerformanceCounter),
                  true},
                 {"producerThreadId",
                  Diagnostics::Unsigned(
                      readiness.producer.primaryThreadId),
                  true},
                 {"publisherLastQpc",
                  Diagnostics::Unsigned(
                      readiness.publisher.lastPerformanceCounter),
                  true},
                 {"barrierLastQpc",
                  Diagnostics::Unsigned(
                      readiness.barrier.lastPerformanceCounter),
                  true},
                 {"producerThreadId",
                  Diagnostics::Unsigned(
                      readiness.producer.primaryThreadId),
                  true},
                 {"publisherThreadId",
                  Diagnostics::Unsigned(
                      readiness.publisher.primaryThreadId),
                  true},
                 {"barrierThreadId",
                  Diagnostics::Unsigned(
                      readiness.barrier.primaryThreadId),
                  true},
                 {"spawnKey",
                  Diagnostics::Unsigned(
                      readiness.lastProducerSpawnKey),
                  true},
                 {"publisherMessageReadable",
                  Diagnostics::Boolean(
                      readiness.publisherMessageReadable),
                  true},
                 {"publisherMessageHex",
                  ReadyMessageHex(readiness.lastPublisherMessage)},
                 {"barrierMessageReadable",
                  Diagnostics::Boolean(
                      readiness.barrierMessageReadable),
                  true},
                 {"barrierMessageHex",
                  ReadyMessageHex(readiness.lastBarrierMessage)},
                 {"participantCountBeforeReadable",
                  Diagnostics::Boolean(
                      readiness.participantCountBeforeReadable),
                  true},
                 {"participantCountBefore",
                  Diagnostics::Unsigned(
                      readiness.participantCountBeforeBarrier),
                  true},
                 {"participantCountAfterReadable",
                  Diagnostics::Boolean(
                      readiness.participantCountAfterReadable),
                  true},
                 {"participantCountAfter",
                  Diagnostics::Unsigned(
                      readiness.participantCountAfterBarrier),
                  true},
                 {"barrierRecordReadable",
                  Diagnostics::Boolean(
                      readiness.barrierRecordReadable),
                  true},
                 {"barrierRecord",
                  Diagnostics::Pointer(
                      reinterpret_cast<const void*>(
                          readiness.barrierRecord))},
                 {"readyCountBeforeReadable",
                  Diagnostics::Boolean(
                      readiness.readyCountBeforeReadable),
                  true},
                 {"readyCountBefore",
                  Diagnostics::Unsigned(
                      readiness.readyCountBeforeBarrier),
                  true},
                 {"readyCountAfterReadable",
                  Diagnostics::Boolean(
                      readiness.readyCountAfterReadable),
                  true},
                 {"readyCountAfter",
                  Diagnostics::Unsigned(
                      readiness.readyCountAfterBarrier),
                  true},
                 {"recordStateBeforeReadable",
                  Diagnostics::Boolean(
                      readiness.recordStateBeforeReadable),
                  true},
                 {"recordStateBefore",
                  Diagnostics::Unsigned(
                      readiness.recordStateBeforeBarrier),
                  true},
                 {"recordStateAfterReadable",
                  Diagnostics::Boolean(
                      readiness.recordStateAfterReadable),
                  true},
                 {"recordStateAfter",
                  Diagnostics::Unsigned(
                      readiness.recordStateAfterBarrier),
                  true},
                 {"manager",
                  Diagnostics::Pointer(
                      reinterpret_cast<const void*>(
                          readiness.lastProducerManager))},
                 {"record",
                  Diagnostics::Pointer(
                      reinterpret_cast<const void*>(
                          readiness.lastProducerRecord))}});
        }

        if (!gameThreadDispatcher->IsEnabled()) {
            capabilities.Set(
                Capability::CreateDummyParticipant,
                CapabilityState::Blocked,
                "validated_game_thread_required_before_participant_mutation");
        } else if (!lastRuntime.participantRegistryAvailable) {
            capabilities.Set(
                Capability::CreateDummyParticipant,
                CapabilityState::Blocked,
                "coherent_participant_registry_unavailable");
        } else if (lastRuntime.participantCount >=
                   kNativeParticipantCapacity) {
            capabilities.Set(
                Capability::CreateDummyParticipant,
                CapabilityState::Blocked,
                "participant_capacity_8_reached");
        } else {
            capabilities.Set(
                Capability::CreateDummyParticipant,
                CapabilityState::StaticValidated,
                "exact_participant_0x158_transaction_count_below_8_unique_identity_F4_and_guarded_last_slot_rollback_validated_offline_runtime_append_not_yet_exercised");
        }

        if (!captureSummary.hasF0 || !captureSummary.has130) {
            capabilities.Set(
                Capability::ReplayCharacterSpawn,
                CapabilityState::Blocked,
                "current_generation_natural_F0_and_130_capture_pair_missing");
        } else if (!gameThreadDispatcher->IsEnabled()) {
            capabilities.Set(
                Capability::ReplayCharacterSpawn,
                CapabilityState::Blocked,
                "validated_game_thread_required_before_0x130_replay");
        } else if (readiness.publisher.calls == 0 ||
                   readiness.barrier.calls == 0 ||
                   !readiness.publisherMessageReadable ||
                   !readiness.barrierMessageReadable ||
                   !readiness.readyCountBeforeReadable ||
                   !readiness.readyCountAfterReadable) {
            capabilities.Set(
                Capability::ReplayCharacterSpawn,
                CapabilityState::Blocked,
                "native_ready8_publisher_barrier_and_record_ready_count_observation_missing");
        } else {
            capabilities.Set(
                Capability::ReplayCharacterSpawn,
                CapabilityState::StaticValidated,
                "natural_ready8_record_transition_and_pointer_free_exactly_once_replay_gate_validated_offline_runtime_publisher_call_not_yet_exercised");
        }
        if (staticProfileValidated &&
            captureSummary.hasF0 &&
            captureSummary.has130) {
            capabilities.Set(
                Capability::BindDummyCharacter,
                CapabilityState::StaticValidated,
                "remote_flags_F4_lookup_request_plus_48_plus_4C_and_participant_bind_consumer_validated_dummy_binding_not_runtime_exercised");
        } else {
            capabilities.Set(
                Capability::BindDummyCharacter,
                CapabilityState::Blocked,
                "exact_profile_and_current_generation_capture_pair_required_for_remote_binding");
        }
        if (destructionProof.complete) {
            capabilities.Set(
                Capability::DestroyDummyCharacter,
                CapabilityState::Observed,
                "full_destroy_record_destroy_channel_release_and_context_release_observed_in_order_on_one_thread_participant_last_slot_pop_still_offline_only");
        } else if (staticProfileValidated) {
            capabilities.Set(
                Capability::DestroyDummyCharacter,
                CapabilityState::StaticValidated,
                "full_destroy_then_record_destroy_channel_context_release_and_guarded_last_slot_participant_rollback_validated_statically_runtime_order_trace_pending");
        } else {
            capabilities.Set(
                Capability::DestroyDummyCharacter,
                CapabilityState::Blocked,
                "exact_full_destroy_record_destroy_channel_and_context_release_symbols_required");
        }

        ipc.PublishRuntimeState();
        transportAdapter.DrainDiagnostics();
    }

    InterlockedExchange(
        &state_,
        static_cast<LONG>(BootstrapState::ShuttingDown));
    ipc.Stop();
    voice_sync::TransportLost();
    player_lifecycle::Shutdown();
    multiplayer_pause::Shutdown();
    item_sync::Shutdown();
    throwable_sync::Shutdown();
    player_lifecycle::DrainDiagnostics(diagnostics);
    character_action_runtime::RequestStopAll();
    world_actions::DrainDiagnosticFiles(diagnostics);
    semantic_reconciliation::DrainDiagnosticFiles();
    const auto actionReleaseDeadline = GetTickCount64() + 5500U;
    while (!character_action_runtime::InputsReleased() && GetTickCount64() < actionReleaseDeadline) {
        // Only the engine's normal update may apply STOP; this worker just waits.
        Sleep(10U);
    }
    const bool retainAISyncHooks = ai_sync::Installed();
    // The ItemSync detour runs on the engine thread like the pause shims: keep it for the process lifetime.
    const bool retainUiRuntime = ui_heap_guard::Installed() || ui_heap_guard::GunLayerGuardInstalled() ||
        multiplayer_pause::Installed() ||
        item_sync::Installed() || throwable_sync::Installed() || voice_sync::Installed() || loading_context_guard::Installed();
    ai_sync::BeginLogicalShutdown();
    animation_sync::props::BeginLogicalShutdown();
    const bool retainActionReleaseHook = character_action_runtime::BeginLogicalShutdown();
    character_action_probe::BeginLogicalShutdown();
    world_interactions::BeginLogicalShutdown();
    world_animated::BeginLogicalShutdown();
    hiding_door_presentation::BeginLogicalShutdown();
    alien_pvp_doors::Shutdown();
    replication_core::BeginLogicalShutdown();
    cathode_primitives::BeginLogicalShutdown();
    if (spawnTracker != nullptr) {
        spawnTracker->BeginLogicalShutdown();
    }
    if (characterHooks != nullptr) {
        characterHooks->BeginLogicalShutdown();
    }
    if (gameThreadHooks != nullptr) {
        gameThreadHooks->BeginLogicalShutdown();
    }
    gameThreadDispatcher->Clear();
    inboundQueue.Clear();

    bool safeToUnload = !retainActionReleaseHook && !retainAISyncHooks && !retainUiRuntime;
    if (retainUiRuntime) diagnostics.Log("info", "ui_runtime_process_lifetime",
        "GFx heap synchronization and disabled pause-query shims remain resident until process exit; Socket workers may outlive the Launcher.");
    if (retainAISyncHooks) diagnostics.Log("warning", "aisync_process_lifetime",
        "Experimental AISync hooks remain resident; pending locomotion releases on the engine thread and Client brains stay passive until process exit.");
    if (retainActionReleaseHook) {
        // A paused/stalled engine must not lose its pending STOP. Keep the
        // forward/release-only detours and their dependencies resident. No
        // native cleanup is performed by this bootstrap worker.
        diagnostics.Log("warning", "character_action_release_pending",
            "Input release is waiting for natural engine updates; hooks and DLL must remain loaded until process exit.");
    }
    if (hooks != nullptr && !retainActionReleaseHook && !retainAISyncHooks && !retainUiRuntime) {
        std::string error;
        if (!hooks->UninstallAll(error)) {
            safeToUnload = false;
            diagnostics.Log(
                "error",
                "hook_shutdown_fault",
                error);
        } else if (hooks->RetiredCount() != 0) {
            // A counter cannot close the small race where a thread has followed
            // the JMP but has not yet incremented the detour counter. Keep the
            // trampolines and DLL resident until process exit.
            safeToUnload = false;
            diagnostics.Log(
                "warning",
                "hook_trampolines_retained",
                "Hook prologues were restored, but generic FreeLibrary quiescence is not proven. Keep this DLL loaded until process exit.",
                {{"retiredTrampolines",
                  Diagnostics::Unsigned(hooks->RetiredCount()),
                  true},
                 {"activeCounterIsUnloadProof", "false", true}});
        }
    }
    if (discovery != nullptr) {
        discovery->Invalidate("bootstrap_shutdown");
    }

    diagnostics.Log(
        safeToUnload ? "info" : "critical",
        safeToUnload ? "bootstrap_stopped" : "bootstrap_unsafe_to_unload",
        safeToUnload
            ? "Observation services stopped and detour quiescence was proven. No transport, publisher, range, inbound callback, or Character mutation was active."
            : "A hook or in-flight detour could not be retired safely. Keep the DLL loaded until process exit.");
    const bool retainSpawnTrackerServices =
        spawnTracker != nullptr &&
        spawnTracker->ProcessLifetimeRequired();
    const bool retainCharacterServices =
        characterHooks != nullptr &&
        characterHooks->ProcessLifetimeRequired();
    const bool retainGameThreadServices =
        gameThreadHooks != nullptr &&
        gameThreadHooks->ProcessLifetimeRequired();
    if (retainSpawnTrackerServices) {
        spawnTracker.release();
    }
    if (retainCharacterServices) {
        // A thread can follow the old JMP just before prologue restoration and
        // increment its detour counter afterwards. Capture pointers are
        // already detached, but a pre-detach detour may have loaded them.
        // Retain these small bounded services until process exit.
        captureCatalog.release();
        lifecycleCatalog.release();
    }
    if (retainGameThreadServices || retainActionReleaseHook || retainAISyncHooks || retainUiRuntime) {
        // The main callback calls the original trampoline before attempting a
        // bounded drain. A pre-detach invocation may therefore still hold
        // these two pointers after the hook prologue is restored.
        gameThreadEvidence.release();
        gameThreadDispatcher.release();
    }
    if (retainSpawnTrackerServices || retainCharacterServices ||
        retainGameThreadServices || retainActionReleaseHook || retainAISyncHooks || retainUiRuntime) {
        levelGenerations.release();
    }
    if (retainActionReleaseHook || retainAISyncHooks || retainUiRuntime) {
        // HookManager::~HookManager would otherwise remove the pending-release
        // boundary. Small process-lifetime retention is deliberate and bounded.
        hooks.release();
    }
    if (retainAISyncHooks) {
        // A pre-shutdown scripted admission can still own a resolver lease.
        // Keep its resolver service alive with the resident AISync callbacks.
        worldRegistry.release();
    }
    if (safeToUnload) crash_guard::UninstallUnhandledExceptionRecorder();
    diagnostics.Shutdown();

    InterlockedExchange(
        &state_,
        static_cast<LONG>(
            safeToUnload
                ? BootstrapState::Stopped
                : BootstrapState::Faulted));
    return safeToUnload ? 0 : 2;
}

void Bootstrap::RequestStop() noexcept {
    const auto event = stopEvent_.load(std::memory_order_acquire);
    if (event != nullptr) {
        SetEvent(event);
    }
}

void Bootstrap::ReleaseStopEventOnDetach() noexcept {
    RequestStop();
    const auto state = State();
    if (state != static_cast<std::uint32_t>(BootstrapState::Stopped)) {
        return;
    }
    const auto event =
        stopEvent_.exchange(nullptr, std::memory_order_acq_rel);
    if (event != nullptr) {
        CloseHandle(event);
    }
}

std::uint32_t Bootstrap::State() const noexcept {
    return static_cast<std::uint32_t>(
        InterlockedCompareExchange(
            const_cast<volatile LONG*>(&state_),
            0,
            0));
}

} // namespace aimp
