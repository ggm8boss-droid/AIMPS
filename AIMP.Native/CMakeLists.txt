cmake_minimum_required(VERSION 3.25)

# AIMP uses no vcpkg dependencies. Apply this to every generated project,
# including tests, so machine-wide vcpkg integration does not invoke pwsh.
list(APPEND CMAKE_VS_GLOBALS "VcpkgEnabled=false" "VcpkgApplocalDeps=false")

project(AIMP.Native VERSION 0.11.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "AIMP.Native supports the MSVC x86 ABI only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure with -A Win32. AI.exe and AIMP.NetZ.dll are 32-bit.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(AIMP_CORE_SOURCES
    src/VoiceSync.cpp
    src/VoiceNativeAudio.cpp
    src/AISync.cpp
    src/AISyncHybridMotion.cpp
    src/AlienPvPControlRuntime.cpp
    src/AlienPvPLocalController.cpp
    src/AlienPvPInputIsolation.cpp
    src/CampaignSaveRuntime.cpp
    src/AlienPvPVisualIsolation.cpp
    src/AISyncTransactions.cpp
    src/AISyncMarkerIdentity.cpp
    src/AISyncMovement.cpp
    src/AISyncNativeMovement.cpp
    src/AISyncTransactionCapture.cpp
    src/AISyncTransactionExecutor.cpp
    src/AISyncExecutionCodec.cpp
    src/AISyncExecutionSession.cpp
    src/AISyncExecutionMailbox.cpp
    src/AISyncExecutionRuntime.cpp
    src/AISyncHostProducer.cpp
    src/AISyncHostProducerNative.cpp
    src/AISyncInteraction.cpp
    src/AISyncLifecycle.cpp
    src/AISyncOwnership.cpp
    src/AISyncNativeOwnership.cpp
    src/AISyncNativeRequestAdapter.cpp
    src/AISyncNativeRetirement.cpp
    src/AISyncRouteIdentity.cpp
    src/AISyncRouteCodec.cpp
    src/AISyncMeshStorage.cpp
    src/AISyncRouteRuntime.cpp
    src/AISyncRouteExecution.cpp
    src/AISyncNativeRoute.cpp
    src/AISyncInteractionRuntime.cpp
    src/AISyncPairRetirement.cpp
    src/AISyncHealth.cpp
    src/AISyncAnimation.cpp
    src/AISyncCombat.cpp
    src/AISyncNavigation.cpp
    src/BuildProfile.cpp
    src/CapabilityMatrix.cpp
    src/CaptureCatalog.cpp
    src/CathodeEquipmentGive.cpp
    src/CathodePrimitiveCodec.cpp
    src/CathodePrimitiveExecutor.cpp
    src/CharacterActionProbeDiagnostics.cpp
    src/CharacterActionRuntime.cpp
    src/ChannelInspector.cpp
    src/CharacterObservationHooks.cpp
    src/CharacterDirectStepRuntime.cpp
    src/CharacterIncarnation.cpp
    src/CrashGuard.cpp
    src/DescriptorRegistry.cpp
    src/Diagnostics.cpp
    src/DummyPlayerController.cpp
    src/ExperimentalProtocol.cpp
    src/GameThreadDispatcher.cpp
    src/GameCommandExecutor.cpp
    src/GameThreadObservationHooks.cpp
    src/HistoricalParticipantTransaction.cpp
    src/HookManager.cpp
    src/InboundQueue.cpp
    src/LauncherIPC.cpp
    src/LevelGenerationTracker.cpp
    src/LifecycleCatalog.cpp
    src/NativeTransportAdapter.cpp
    src/NetZDiscovery.cpp
    src/ParticipantRegistry.cpp
    src/PlayerSyncObserver.cpp
    src/PlayerPostureRuntime.cpp
    src/PlayerAnimationClipCapture.cpp
    src/PlayerTorchRuntime.cpp
    src/PlayerHidingRuntime.cpp
    src/PlayerHidingPresentationRuntime.cpp
    src/HidingPresentationAudio.cpp
    src/PlayerHidingLocalCapture.cpp
    src/AnimationSyncRuntime.cpp
    src/AnimationSyncTimeline.cpp
    src/AnimationSyncPropsRuntime.cpp
    src/HidingDoorPresentation.cpp
    src/MissionSync.cpp
    src/PlayerLifecycle.cpp
    src/LoadingContextGuard.cpp
    src/PairedTransformGuard.cpp
    src/TeardownReferenceGuard.cpp
    src/UIHeapGuard.cpp
    src/LevelIntegrityGuard.cpp
    src/MultiplayerPause.cpp
    src/ItemSync.cpp
    src/ThrowableSync.cpp
    src/RuntimeFeatureFlags.cpp
    src/Protocol.cpp
    src/ReadyReplayGate.cpp
    src/SignatureResolver.cpp
    src/SpawnCharacterTracker.cpp
    src/ReplicationCore.cpp
    src/WorldEntityRegistry.cpp
    src/WorldAnimatedAdapterCodec.cpp
    src/WorldAnimatedAdapters.cpp
    src/WorldInteractionAdapterCodec.cpp
    src/WorldInteractionAdapters.cpp
    src/WorldPublicActions.cpp
    src/WorldReplicationCodec.cpp
    src/WorldSemanticReconciliation.cpp
)

add_library(AIMP.Core STATIC ${AIMP_CORE_SOURCES})
set_target_properties(AIMP.Core PROPERTIES
    VS_GLOBAL_VcpkgEnabled "false"
    VS_GLOBAL_VcpkgApplocalDeps "false"
)
target_include_directories(AIMP.Core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(AIMP.Core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0602
)
target_compile_options(AIMP.Core PRIVATE
    /W4
    /WX
    /permissive-
    /EHsc
    /sdl
    /utf-8
    /Zc:__cplusplus
)
target_link_libraries(AIMP.Core PUBLIC bcrypt version)

add_library(AIMP.NetZ SHARED
    src/AIMP.NetZ.def
    src/Bootstrap.cpp
    src/DllMain.cpp
)
target_link_libraries(AIMP.NetZ PRIVATE AIMP.Core)
target_compile_options(AIMP.NetZ PRIVATE
    /W4
    /WX
    /permissive-
    /EHsc
    /sdl
    /utf-8
    /Zc:__cplusplus
)
set_target_properties(AIMP.NetZ PROPERTIES
    OUTPUT_NAME "AIMP.NetZ"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    VS_GLOBAL_VcpkgEnabled "false"
    VS_GLOBAL_VcpkgApplocalDeps "false"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(AIMP.AlienPvPDonorControlTests tests/AlienPvPDonorControlTests.cpp
        src/AlienPvPControlRuntime.cpp src/CrashGuard.cpp)
    target_include_directories(AIMP.AlienPvPDonorControlTests PRIVATE include)
    target_compile_definitions(AIMP.AlienPvPDonorControlTests PRIVATE AIMP_ALIEN_PVP_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(AIMP.AlienPvPDonorControlTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AlienPvPDonorControlTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AlienPvPDonorControl COMMAND AIMP.AlienPvPDonorControlTests)
    add_executable(AIMP.WorldTransactionSkipTests tests/WorldTransactionSkipTests.cpp)
    target_link_libraries(AIMP.WorldTransactionSkipTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.WorldTransactionSkipTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.WorldTransactionSkipTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME WorldTransactionSkipAndContinue COMMAND AIMP.WorldTransactionSkipTests)
    add_executable(AIMP.WorldManifestTests tests/WorldManifestRuntimeTests.cpp)
    target_link_libraries(AIMP.WorldManifestTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.WorldManifestTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.WorldManifestTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME WorldManifestNativeRuntimeIndex COMMAND AIMP.WorldManifestTests)
    set_tests_properties(WorldManifestNativeRuntimeIndex PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(AIMP.AISyncMovementTests tests/AISyncMovementTests.cpp)
    target_link_libraries(AIMP.AISyncMovementTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncMovementTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncMovementTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncMovement COMMAND AIMP.AISyncMovementTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/aisync-execution-move-v2.hex")
    add_executable(AIMP.AISyncRouteCodecTests tests/AISyncRouteCodecTests.cpp
        src/AISyncRouteCodec.cpp src/AISyncRouteIdentity.cpp src/AISyncTransactions.cpp)
    target_include_directories(AIMP.AISyncRouteCodecTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncRouteCodecTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncRouteCodecTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncRouteCodec COMMAND AIMP.AISyncRouteCodecTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/aisync-route-v4.hex")
    add_executable(AIMP.AISyncNativeRouteTests tests/AISyncNativeRouteTests.cpp src/AISyncNativeRoute.cpp)
    target_link_libraries(AIMP.AISyncNativeRouteTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncNativeRouteTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncNativeRouteTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncNativeRoute COMMAND AIMP.AISyncNativeRouteTests)
    add_executable(AIMP.AISyncRouteHandoffTests tests/AISyncRouteHandoffTests.cpp)
    target_include_directories(AIMP.AISyncRouteHandoffTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncRouteHandoffTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncRouteHandoffTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncRouteHandoff COMMAND AIMP.AISyncRouteHandoffTests)
    add_executable(AIMP.AISyncRouteTrailTests tests/AISyncRouteTrailTests.cpp
        src/AISyncRouteCodec.cpp src/AISyncRouteIdentity.cpp src/AISyncTransactions.cpp)
    target_include_directories(AIMP.AISyncRouteTrailTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncRouteTrailTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncRouteTrailTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncRouteTrail COMMAND AIMP.AISyncRouteTrailTests)
    add_executable(AIMP.AISyncNativeGuidancePolicyTests tests/AISyncNativeGuidancePolicyTests.cpp)
    target_include_directories(AIMP.AISyncNativeGuidancePolicyTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncNativeGuidancePolicyTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncNativeGuidancePolicyTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncNativeGuidancePolicy COMMAND AIMP.AISyncNativeGuidancePolicyTests)
    add_executable(AIMP.AISyncLifecycleTests tests/AISyncLifecycleTests.cpp)
    target_link_libraries(AIMP.AISyncLifecycleTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncLifecycleTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncLifecycleTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncNativeLifecycle COMMAND AIMP.AISyncLifecycleTests)
    add_executable(AIMP.AISyncHostProducerTests tests/AISyncHostProducerTests.cpp
        src/AISyncHostProducer.cpp src/AISyncExecutionCodec.cpp src/AISyncTransactions.cpp src/AISyncMovement.cpp
        src/AISyncRouteCodec.cpp src/AISyncRouteIdentity.cpp)
    target_include_directories(AIMP.AISyncHostProducerTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncHostProducerTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncHostProducerTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncHostProducer COMMAND AIMP.AISyncHostProducerTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/aisync-action-reference-v1.hex")
    add_executable(AIMP.AISyncInteractionTests tests/AISyncInteractionTests.cpp)
    target_link_libraries(AIMP.AISyncInteractionTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncInteractionTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncInteractionTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncInteraction COMMAND AIMP.AISyncInteractionTests)
    add_executable(AIMP.AISyncInteractionRuntimeTests tests/AISyncInteractionRuntimeTests.cpp
        src/AISyncInteractionRuntime.cpp src/AISyncInteraction.cpp)
    target_include_directories(AIMP.AISyncInteractionRuntimeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(AIMP.AISyncInteractionRuntimeTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(AIMP.AISyncInteractionRuntimeTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncInteractionRuntimeTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncInteractionRuntime COMMAND AIMP.AISyncInteractionRuntimeTests)
    add_executable(AIMP.AISyncNativeRetirementTests tests/AISyncNativeRetirementTests.cpp)
    target_link_libraries(AIMP.AISyncNativeRetirementTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncNativeRetirementTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncNativeRetirementTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncNativeRetirement COMMAND AIMP.AISyncNativeRetirementTests)
    add_executable(AIMP.AISyncOwnershipTests tests/AISyncOwnershipTests.cpp src/AISyncOwnership.cpp)
    target_include_directories(AIMP.AISyncOwnershipTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncOwnershipTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncOwnershipTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncOwnershipHandoff COMMAND AIMP.AISyncOwnershipTests)
    add_executable(AIMP.AISyncNativeOwnershipTests tests/AISyncNativeOwnershipTests.cpp src/AISyncNativeOwnership.cpp)
    target_include_directories(AIMP.AISyncNativeOwnershipTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncNativeOwnershipTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncNativeOwnershipTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncNativeMovementBoundary COMMAND AIMP.AISyncNativeOwnershipTests)
    add_executable(AIMP.CharacterIncarnationTests tests/CharacterIncarnationTests.cpp src/CharacterIncarnation.cpp)
    target_include_directories(AIMP.CharacterIncarnationTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.CharacterIncarnationTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.CharacterIncarnationTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME CharacterIncarnationTickets COMMAND AIMP.CharacterIncarnationTests "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/native-character-incarnation-v1.hex")
    add_executable(AIMP.AISyncExecutionMailboxTests tests/AISyncExecutionMailboxTests.cpp)
    target_link_libraries(AIMP.AISyncExecutionMailboxTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncExecutionMailboxTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncExecutionMailboxTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncExecutionMailbox COMMAND AIMP.AISyncExecutionMailboxTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/aisync-execution-control-v1.hex")
    add_executable(AIMP.AISyncNativeIntentCodecTests tests/AISyncNativeIntentCodecTests.cpp
        src/AISyncTransactions.cpp src/AISyncMovement.cpp src/AISyncExecutionCodec.cpp)
    target_include_directories(AIMP.AISyncNativeIntentCodecTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncNativeIntentCodecTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncNativeIntentCodecTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncNativeIntentCodec COMMAND AIMP.AISyncNativeIntentCodecTests)
    add_executable(AIMP.AISyncMarkerCodecTests tests/AISyncMarkerCodecTests.cpp
        src/AISyncTransactions.cpp src/AISyncMovement.cpp src/AISyncExecutionCodec.cpp)
    target_include_directories(AIMP.AISyncMarkerCodecTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncMarkerCodecTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    add_test(NAME AISyncMarkerCodec COMMAND AIMP.AISyncMarkerCodecTests)
    add_executable(AIMP.AISyncMarkerIdentityTests tests/AISyncMarkerIdentityTests.cpp src/AISyncMarkerIdentity.cpp src/HookManager.cpp src/CrashGuard.cpp src/BuildProfile.cpp)
    target_include_directories(AIMP.AISyncMarkerIdentityTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncMarkerIdentityTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    target_link_libraries(AIMP.AISyncMarkerIdentityTests PRIVATE bcrypt version)
    target_compile_definitions(AIMP.AISyncMarkerIdentityTests PRIVATE AIMP_MARKER_IDENTITY_TESTING NOMINMAX)
    add_test(NAME AISyncMarkerIdentity COMMAND AIMP.AISyncMarkerIdentityTests "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe")
    add_executable(AIMP.AISyncNativeMarkerTests tests/AISyncNativeMarkerTests.cpp)
    target_link_libraries(AIMP.AISyncNativeMarkerTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncNativeMarkerTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    add_test(NAME AISyncNativeMarkers COMMAND AIMP.AISyncNativeMarkerTests)
    add_executable(AIMP.AISyncRouteRecoveryTests tests/AISyncRouteRecoveryTests.cpp)
    target_link_libraries(AIMP.AISyncRouteRecoveryTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncRouteRecoveryTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncRouteRecoveryTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncRouteRecovery COMMAND AIMP.AISyncRouteRecoveryTests)
    add_test(NAME AISyncTypedHostMarkers COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" typed-markers)
    add_executable(AIMP.AISyncTransactionExecutorTests
        tests/AISyncTransactionExecutorTests.cpp src/AISyncTransactionExecutor.cpp src/AISyncTransactions.cpp src/AISyncMovement.cpp)
    target_include_directories(AIMP.AISyncTransactionExecutorTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncTransactionExecutorTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncTransactionExecutorTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncTransactionExecutor COMMAND AIMP.AISyncTransactionExecutorTests)
    add_executable(AIMP.AISyncRouteIdentityTests tests/AISyncRouteIdentityTests.cpp src/AISyncRouteIdentity.cpp)
    target_include_directories(AIMP.AISyncRouteIdentityTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncRouteIdentityTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncRouteIdentityTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncRouteIdentity COMMAND AIMP.AISyncRouteIdentityTests)
    add_executable(AIMP.AISyncExecutionTests tests/AISyncExecutionTests.cpp)
    target_link_libraries(AIMP.AISyncExecutionTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncExecutionTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncExecutionTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncExecutionCodecAndSession COMMAND AIMP.AISyncExecutionTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/aisync-execution-v1.hex")
    add_test(NAME AISyncCommandAdmission COMMAND AIMP.AISyncExecutionTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/aisync-execution-v1.hex" --command-admission)
    add_test(NAME AISyncTypedHostHooks COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" typed)
    add_test(NAME AISyncTypedHostMovement COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" typed-movement)
    add_test(NAME AISyncTypedHostControls COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" typed-controls)
    add_test(NAME AISyncTypedHostHookRollback COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" typed-rollback)
    add_test(NAME AISyncTypedHostSharedHooks COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" typed-shared)
    add_executable(AIMP.AISyncTransactionTests
        tests/AISyncTransactionTests.cpp src/AISyncTransactions.cpp)
    target_include_directories(AIMP.AISyncTransactionTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncTransactionTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncTransactionTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncTransactionModel COMMAND AIMP.AISyncTransactionTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/aisync-transaction-v1.hex")
    add_executable(AIMP.AISyncTransactionCaptureTests tests/AISyncTransactionCaptureTests.cpp src/AISyncNativeRoute.cpp)
    target_link_libraries(AIMP.AISyncTransactionCaptureTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AISyncTransactionCaptureTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncTransactionCaptureTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncTransactionCapture COMMAND AIMP.AISyncTransactionCaptureTests)
    add_test(NAME AISyncOwnedMeshStorage COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" mesh-storage)
    add_test(NAME AISyncTransactionCaptureHooks COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" install)
    add_test(NAME AISyncTransactionCaptureRollback COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" rollback)
    add_test(NAME AISyncTransactionCaptureTransport COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" transport)
    add_test(NAME AISyncTransactionCaptureTransportOptIn COMMAND AIMP.AISyncTransactionCaptureTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" transport-inexact)
    set_tests_properties(AISyncTransactionCaptureHooks AISyncTransactionCaptureRollback PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(AIMP.EquipmentCommandTests tests/EquipmentCommandTests.cpp)
    target_link_libraries(AIMP.EquipmentCommandTests PRIVATE AIMP.Core)
    target_compile_definitions(AIMP.EquipmentCommandTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AIMP.EquipmentCommandTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.EquipmentCommandTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME EquipmentCommandAdmission COMMAND AIMP.EquipmentCommandTests)
    add_executable(AIMP.PlayerPostureTests tests/PlayerPostureTests.cpp)
    target_link_libraries(AIMP.PlayerPostureTests PRIVATE AIMP.Core)
    target_compile_definitions(AIMP.PlayerPostureTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AIMP.PlayerPostureTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.PlayerPostureTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME PlayerPostureAndTraversal COMMAND AIMP.PlayerPostureTests)
    add_executable(AIMP.PlayerPostureRuntimeTests tests/PlayerPostureRuntimeTests.cpp)
    target_link_libraries(AIMP.PlayerPostureRuntimeTests PRIVATE AIMP.Core)
    target_compile_definitions(AIMP.PlayerPostureRuntimeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AIMP.PlayerPostureRuntimeTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.PlayerPostureRuntimeTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME PlayerPostureRuntime COMMAND AIMP.PlayerPostureRuntimeTests)
    add_executable(AIMP.PlayerTorchRuntimeTests tests/PlayerTorchRuntimeTests.cpp src/PlayerTorchRuntime.cpp)
    target_include_directories(AIMP.PlayerTorchRuntimeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(AIMP.PlayerTorchRuntimeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AIMP.PlayerTorchRuntimeTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.PlayerTorchRuntimeTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME PlayerTorchRuntime COMMAND AIMP.PlayerTorchRuntimeTests)
    add_executable(AIMP.PlayerSpectatePolicyTests tests/PlayerSpectatePolicyTests.cpp)
    target_include_directories(AIMP.PlayerSpectatePolicyTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.PlayerSpectatePolicyTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.PlayerSpectatePolicyTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME PlayerSpectatePolicy COMMAND AIMP.PlayerSpectatePolicyTests)
    add_executable(AIMP.PlayerHidingRuntimeTests tests/PlayerHidingRuntimeTests.cpp)
    target_link_libraries(AIMP.PlayerHidingRuntimeTests PRIVATE AIMP.Core)
    target_include_directories(AIMP.PlayerHidingRuntimeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(AIMP.PlayerHidingRuntimeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AIMP.PlayerHidingRuntimeTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.PlayerHidingRuntimeTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME PlayerHidingRuntime COMMAND AIMP.PlayerHidingRuntimeTests)
    add_executable(AIMP.PlayerHidingPresentationRuntimeTests tests/PlayerHidingPresentationRuntimeTests.cpp)
    target_link_libraries(AIMP.PlayerHidingPresentationRuntimeTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.PlayerHidingPresentationRuntimeTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.PlayerHidingPresentationRuntimeTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME PlayerHidingPresentationRuntime COMMAND AIMP.PlayerHidingPresentationRuntimeTests)
    add_executable(AIMP.HidingDoorPresentationTests tests/HidingDoorPresentationTests.cpp)
    target_link_libraries(AIMP.HidingDoorPresentationTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.HidingDoorPresentationTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.HidingDoorPresentationTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME HidingDoorPresentation COMMAND AIMP.HidingDoorPresentationTests)
    add_executable(AIMP.HidingPresentationAudioTests tests/HidingPresentationAudioTests.cpp)
    target_link_libraries(AIMP.HidingPresentationAudioTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.HidingPresentationAudioTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.HidingPresentationAudioTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME HidingPresentationAudio COMMAND AIMP.HidingPresentationAudioTests)
    add_executable(AIMP.VoiceNativeAudioTests tests/VoiceNativeAudioTests.cpp)
    target_include_directories(AIMP.VoiceNativeAudioTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.VoiceNativeAudioTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.VoiceNativeAudioTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME VoiceNativeAudioContracts COMMAND AIMP.VoiceNativeAudioTests)
    add_executable(AIMP.AISyncHybridMotionTests tests/AISyncHybridMotionTests.cpp src/AISyncHybridMotion.cpp)
    target_include_directories(AIMP.AISyncHybridMotionTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncHybridMotionTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncHybridMotionTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncHybridMotion COMMAND AIMP.AISyncHybridMotionTests)
    add_executable(AIMP.AISyncTests tests/AISyncPortableTests.cpp)
    set_target_properties(AIMP.AISyncTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    target_include_directories(AIMP.AISyncTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    add_test(NAME AISyncOwnershipAndNavigation COMMAND AIMP.AISyncTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/aisync-participant-config-v3.hex")
    add_executable(AIMP.AnimationSyncTimelineTests tests/AnimationSyncTimelineTests.cpp)
    target_include_directories(AIMP.AnimationSyncTimelineTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AnimationSyncTimelineTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AnimationSyncTimelineTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AnimationSyncNativeTimeline COMMAND AIMP.AnimationSyncTimelineTests)
    add_executable(AIMP.AnimationSyncVentGroundTests tests/AnimationSyncVentGroundTests.cpp)
    target_include_directories(AIMP.AnimationSyncVentGroundTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AnimationSyncVentGroundTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AnimationSyncVentGroundTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AnimationSyncVentGround COMMAND AIMP.AnimationSyncVentGroundTests)
    add_executable(AIMP.AnimationSyncVisualRootTests tests/AnimationSyncVisualRootTests.cpp)
    target_include_directories(AIMP.AnimationSyncVisualRootTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AnimationSyncVisualRootTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AnimationSyncVisualRootTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AnimationSyncVisualRoot COMMAND AIMP.AnimationSyncVisualRootTests)
    add_executable(AIMP.AnimationSyncMovementPolicyTests tests/AnimationSyncMovementPolicyTests.cpp)
    target_include_directories(AIMP.AnimationSyncMovementPolicyTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AnimationSyncMovementPolicyTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AnimationSyncMovementPolicyTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AnimationSyncMovementPolicy COMMAND AIMP.AnimationSyncMovementPolicyTests)
    add_executable(AIMP.AnimationSyncPropsHookTests tests/AnimationSyncPropsHookTests.cpp)
    target_link_libraries(AIMP.AnimationSyncPropsHookTests PRIVATE AIMP.Core)
    target_compile_options(AIMP.AnimationSyncPropsHookTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AnimationSyncPropsHookTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AnimationSyncPropsHookInstallation COMMAND AIMP.AnimationSyncPropsHookTests)
    add_executable(AIMP.AISyncHookTests tests/AISyncHookInstallationTests.cpp)
    target_link_libraries(AIMP.AISyncHookTests PRIVATE AIMP.Core)
    target_compile_definitions(AIMP.AISyncHookTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AIMP.AISyncHookTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    set_target_properties(AIMP.AISyncHookTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    add_test(NAME AISyncHookInstallation COMMAND AIMP.AISyncHookTests "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe")
    set_tests_properties(AISyncHookInstallation PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME AISyncProductionInstallation COMMAND AIMP.AISyncHookTests "${CMAKE_CURRENT_SOURCE_DIR}/../AI.exe" production)
    set_tests_properties(AISyncProductionInstallation PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(AIMP.WorldActionTests
        tests/WorldActionPortableTests.cpp
        src/WorldInteractionAdapterCodec.cpp)
    set_target_properties(AIMP.WorldActionTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false"
        VS_GLOBAL_VcpkgApplocalDeps "false"
    )
    target_include_directories(AIMP.WorldActionTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(AIMP.WorldActionTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AIMP.WorldActionTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    add_test(NAME WorldActionLineageAndWire COMMAND AIMP.WorldActionTests)
endif()
