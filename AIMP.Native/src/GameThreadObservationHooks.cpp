#include "aimp/AISync.h"
#include "aimp/AISyncProfiling.h"
#include "aimp/VoiceSync.h"
#include "aimp/GameThreadObservationHooks.h"

#include "aimp/BuildProfile.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/HookManager.h"
#include "aimp/ItemSync.h"
#include "aimp/ThrowableSync.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/NativeLimits.h"
#include "aimp/PlayerSyncObserver.h"
#include "aimp/MultiplayerPause.h"
#include "aimp/RuntimeFeatureFlags.h"
#include "aimp/ReplicationCore.h"
#include "aimp/SignatureResolver.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace aimp {
namespace {

using MainThreadCallback = std::uint32_t(__cdecl*)();
using ManagerUpdate = void(__thiscall*)(void*);
using ReadinessProducer =
    void(__thiscall*)(void*, std::uint32_t, void*);
using ReadinessPublisher =
    void(__thiscall*)(void*, const void*, std::uint32_t);
using ReadinessBarrier = void(__thiscall*)(void*, const void*);

std::atomic<GameThreadRuntimeEvidence*> gEvidence{nullptr};
std::atomic<GameThreadDispatcher*> gDispatcher{nullptr};
std::atomic<LevelGenerationTracker*> gGenerations{nullptr};
std::atomic<PlayerSyncObserver*> gPlayerSync{nullptr};
std::atomic<void*> gMainThreadTrampoline{nullptr};
std::atomic<void*> gManagerUpdateTrampoline{nullptr};
std::atomic<void*> gReadinessProducerTrampoline{nullptr};
std::atomic<void*> gReadinessPublisherTrampoline{nullptr};
std::atomic<void*> gReadinessBarrierTrampoline{nullptr};
std::atomic<std::uint32_t> gActiveDetours{0};

class ActiveDetour final {
public:
    ActiveDetour() noexcept {
        gActiveDetours.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveDetour() {
        gActiveDetours.fetch_sub(1, std::memory_order_acq_rel);
    }
};

std::uint64_t PerformanceCounter() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t Generation() noexcept {
    const auto* generations =
        gGenerations.load(std::memory_order_acquire);
    return generations == nullptr ? 0 : generations->CurrentGeneration();
}

std::array<std::byte, 8> CopyReadyMessage(
    const void* message,
    bool& readable) noexcept {
    std::array<std::byte, 8> copy{};
    readable =
        message != nullptr &&
        crash_guard::TryCopy(copy.data(), message, copy.size());
    return copy;
}

bool ReadParticipantCountFromReadySink(
    const void* sink,
    std::uint32_t& count) noexcept {
    count = 0;
    if (sink == nullptr) {
        return false;
    }
    const auto sinkAddress = reinterpret_cast<std::uintptr_t>(sink);
    if (sinkAddress >
        (std::numeric_limits<std::uintptr_t>::max)() - 0x18U) {
        return false;
    }
    const auto wrapper = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(sinkAddress + 0x18U));
    if (!wrapper.has_value() || *wrapper == 0) {
        return false;
    }
    const auto owner = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*wrapper));
    if (!owner.has_value() || *owner == 0 ||
        *owner >
            (std::numeric_limits<std::uintptr_t>::max)() - 0xAC4U) {
        return false;
    }
    const auto observed = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(*owner + 0xAC4U));
    if (!observed.has_value() ||
        *observed > kNativeParticipantCapacity) {
        return false;
    }
    count = *observed;
    return true;
}

struct ReadyRecordSnapshot {
    std::uintptr_t record{};
    std::uint32_t readyCount{};
    std::uint8_t state{};
    bool recordReadable{};
    bool readyCountReadable{};
    bool stateReadable{};
};

ReadyRecordSnapshot ReadRecordFromReadySink(
    const void* sink,
    const std::array<std::byte, 8>& message,
    const bool messageReadable) noexcept {
    ReadyRecordSnapshot snapshot{};
    if (sink == nullptr || !messageReadable) {
        return snapshot;
    }

    std::uint32_t spawnKey = 0;
    std::memcpy(&spawnKey, message.data(), sizeof(spawnKey));
    const auto sinkAddress = reinterpret_cast<std::uintptr_t>(sink);
    constexpr std::uintptr_t kMapCountOffset = 0x4354U;
    constexpr std::uintptr_t kMapEntriesOffset = 0x4358U;
    constexpr std::uintptr_t kRecordArrayOffset = 0x214U;
    constexpr std::uintptr_t kRecordStride = 0x1D0U;
    constexpr std::uint32_t kMaximumObservedRecords = 256;
    if (sinkAddress >
        (std::numeric_limits<std::uintptr_t>::max)() -
            kMapEntriesOffset) {
        return snapshot;
    }
    const auto mapCount = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(sinkAddress + kMapCountOffset));
    if (!mapCount.has_value() ||
        *mapCount == 0 ||
        *mapCount > kMaximumObservedRecords) {
        return snapshot;
    }

    std::uint32_t recordIndex = 0;
    bool found = false;
    for (std::uint32_t index = 0; index < *mapCount; ++index) {
        const auto entry =
            sinkAddress + kMapEntriesOffset +
            static_cast<std::uintptr_t>(index) * 8U;
        const auto observedKey = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(entry));
        const auto observedIndex = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(entry + 4U));
        if (!observedKey.has_value() || !observedIndex.has_value()) {
            return snapshot;
        }
        if (*observedKey == spawnKey) {
            recordIndex = *observedIndex;
            found = true;
            break;
        }
    }
    if (!found || recordIndex >= *mapCount) {
        return snapshot;
    }

    const auto recordOffset =
        kRecordArrayOffset +
        static_cast<std::uintptr_t>(recordIndex) * kRecordStride;
    if (sinkAddress >
        (std::numeric_limits<std::uintptr_t>::max)() -
            recordOffset - 0x14U) {
        return snapshot;
    }
    snapshot.record = sinkAddress + recordOffset;
    snapshot.recordReadable = true;
    const auto readyCount = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(snapshot.record + 0x10U));
    const auto state = crash_guard::SafeRead<std::uint8_t>(
        reinterpret_cast<const void*>(snapshot.record + 0x14U));
    if (readyCount.has_value()) {
        snapshot.readyCount = *readyCount;
        snapshot.readyCountReadable = true;
    }
    if (state.has_value()) {
        snapshot.state = *state;
        snapshot.stateReadable = true;
    }
    return snapshot;
}

std::uint32_t __cdecl MainThreadCharacterProcessingDetour() noexcept {
    const ai_sync::profiling::Timer timing(ai_sync::profiling::Section::EngineCallback);
    ai_sync::profiling::EngineCadence();
    ActiveDetour active;
    const auto threadId = GetCurrentThreadId();
    const auto performanceCounter = PerformanceCounter();
    if (auto* dispatcher = gDispatcher.load(std::memory_order_acquire);
        dispatcher != nullptr) {
        dispatcher->BindEngineThread(
            threadId,
            EngineThreadSource::MainThreadCharacterProcessing,
            performanceCounter);
    }
    if (runtime_features::Enabled(runtime_features::Feature::EngineThreadEvidence)) {
        if (auto* evidence = gEvidence.load(std::memory_order_acquire);
            evidence != nullptr) {
        evidence->ObserveMainThreadCharacterProcessing(
            threadId,
            performanceCounter);
        }
    }

    const auto original = reinterpret_cast<MainThreadCallback>(
        gMainThreadTrampoline.load(std::memory_order_acquire));
    const auto retailStart=ai_sync::profiling::Clock();
    const auto result = original == nullptr ? 0U : original();
    ai_sync::profiling::End(ai_sync::profiling::Section::EngineOriginal,retailStart);
    const ai_sync::profiling::Timer workTiming(ai_sync::profiling::Section::Dispatcher);

    // The loading hold skips CharacterManagerUpdate, including its creation
    // queue. Only advance that queue here; keep gameplay simulation held.
    if (runtime_features::Enabled(runtime_features::Feature::GameThreadPumps))
        multiplayer_pause::PumpLoadingCharacterReadiness(
            gReadinessProducerTrampoline.load(std::memory_order_acquire));

    if (runtime_features::Enabled(runtime_features::Feature::GameThreadPumps)) {
        static_cast<void>(replication_core::PumpDeferredRemoteApplyOnEngineThread());
        if (auto* dispatcher = gDispatcher.load(std::memory_order_acquire);
            dispatcher != nullptr) {
            dispatcher->DrainOnEngineThread(4);
        }
    }
    return result;
}

void __fastcall CharacterManagerUpdateDetour(
    void* manager,
    void*) noexcept {
    ActiveDetour active;
    const auto threadId = GetCurrentThreadId();
    const auto performanceCounter = PerformanceCounter();
    if (auto* dispatcher = gDispatcher.load(std::memory_order_acquire);
        dispatcher != nullptr) {
        dispatcher->BindEngineThread(
            threadId,
            EngineThreadSource::CharacterManagerUpdate,
            performanceCounter);
    }
    if (runtime_features::Enabled(runtime_features::Feature::EngineThreadEvidence)) {
        if (auto* evidence = gEvidence.load(std::memory_order_acquire);
            evidence != nullptr) {
        evidence->ObserveCharacterManagerUpdate(
            threadId,
            performanceCounter,
            reinterpret_cast<std::uintptr_t>(manager));
        }
    }
    // Apply queued replication commands before CharacterManager advances the
    // native animation graph. This lets the 0x01 -> 0x38 transition happen in
    // time for the same frame's ActNPCLocomotionCycle update instead of one
    // frame after the AI has already ticked.
    if (runtime_features::Enabled(runtime_features::Feature::GameThreadPumps)) {
        static_cast<void>(replication_core::PumpDeferredRemoteApplyOnEngineThread());
        if (auto* dispatcher = gDispatcher.load(std::memory_order_acquire);
            dispatcher != nullptr) {
            dispatcher->DrainOnEngineThread(4);
        }
        character_direct_step_runtime::PumpOnEngineThread(
            character_direct_step_runtime::EnginePumpPhase::PreCharacterManager);
    }

    ai_sync::PumpOnEngineThread();
    voice_sync::PumpOnEngineThread();
    item_sync::PumpOnEngineThread();
    throwable_sync::PumpOnEngineThread();
    const auto original = reinterpret_cast<ManagerUpdate>(
        gManagerUpdateTrampoline.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(manager);
    }

    // PlayerSync reads the authoritative local-player equipment state after
    // the retail CharacterManager tick. It never interprets input actions and
    // never depends on a CATHODE node being present in the loaded map.
    if (auto* playerSync = gPlayerSync.load(std::memory_order_acquire);
        playerSync != nullptr) {
        playerSync->ObserveOnGameThread();
    }

    // Commands can arrive while CharacterManager is executing, and CIVILIAN
    // code may attempt another state transition during the update. Drain once
    // more after the native tick. The Remote Movement POST phase is deliberately
    // cheap: it returns immediately unless a target changed during retail or a
    // native state loss was observed and actually needs reconciliation.
    if (runtime_features::Enabled(runtime_features::Feature::GameThreadPumps)) {
        static_cast<void>(replication_core::PumpDeferredRemoteApplyOnEngineThread());
        if (auto* dispatcher = gDispatcher.load(std::memory_order_acquire);
            dispatcher != nullptr) {
            dispatcher->DrainOnEngineThread(4);
        }
        character_direct_step_runtime::PumpOnEngineThread(
            character_direct_step_runtime::EnginePumpPhase::PostCharacterManager);
        ai_sync::ReconcileDirectPoseOnEngineThread();
    }
}

void __fastcall ReadinessProducerDetour(
    void* manager,
    void*,
    const std::uint32_t spawnKey,
    void* record) noexcept {
    ActiveDetour active;
    const auto threadId = GetCurrentThreadId();
    const auto performanceCounter = PerformanceCounter();
    if (auto* dispatcher = gDispatcher.load(std::memory_order_acquire);
        dispatcher != nullptr) {
        dispatcher->BindEngineThread(
            threadId,
            EngineThreadSource::CharacterReadinessProducer,
            performanceCounter);
    }
    if (runtime_features::Enabled(runtime_features::Feature::EngineThreadEvidence)) {
        if (auto* evidence = gEvidence.load(std::memory_order_acquire);
            evidence != nullptr) {
        evidence->ObserveReadinessProducer(
            threadId,
            performanceCounter,
            Generation(),
            reinterpret_cast<std::uintptr_t>(manager),
            spawnKey,
            reinterpret_cast<std::uintptr_t>(record));
        }
    }
    const auto original = reinterpret_cast<ReadinessProducer>(
        gReadinessProducerTrampoline.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(manager, spawnKey, record);
    }
    if (runtime_features::Enabled(runtime_features::Feature::GameThreadPumps)) {
        if (auto* dispatcher = gDispatcher.load(std::memory_order_acquire);
            dispatcher != nullptr) {
            dispatcher->DrainOnEngineThread(4);
        }
    }
}

void __fastcall ReadinessPublisherDetour(
    void* publisher,
    void*,
    const void* message,
    const std::uint32_t observedZero) noexcept {
    ActiveDetour active;
    const auto original = reinterpret_cast<ReadinessPublisher>(
        gReadinessPublisherTrampoline.load(std::memory_order_acquire));
    if (!runtime_features::Enabled(
            runtime_features::Feature::EngineThreadEvidence)) {
        if (original != nullptr) {
            original(publisher, message, observedZero);
        }
        return;
    }

    bool readable = false;
    const auto copy = CopyReadyMessage(message, readable);
    if (auto* evidence = gEvidence.load(std::memory_order_acquire);
        evidence != nullptr) {
        evidence->ObserveReadinessPublisher(
            GetCurrentThreadId(),
            PerformanceCounter(),
            Generation(),
            reinterpret_cast<std::uintptr_t>(publisher),
            copy,
            readable);
    }
    if (original != nullptr) {
        original(publisher, message, observedZero);
    }
}

void __fastcall ReadinessBarrierDetour(
    void* sink,
    void*,
    const void* message) noexcept {
    ActiveDetour active;
    const auto original = reinterpret_cast<ReadinessBarrier>(
        gReadinessBarrierTrampoline.load(std::memory_order_acquire));
    if (!runtime_features::Enabled(
            runtime_features::Feature::EngineThreadEvidence)) {
        if (original != nullptr) {
            original(sink, message);
        }
        return;
    }

    bool messageReadable = false;
    const auto copy = CopyReadyMessage(message, messageReadable);
    std::uint32_t countBefore = 0;
    const bool countBeforeReadable =
        ReadParticipantCountFromReadySink(sink, countBefore);
    const auto recordBefore =
        ReadRecordFromReadySink(sink, copy, messageReadable);

    if (original != nullptr) {
        original(sink, message);
    }

    std::uint32_t countAfter = 0;
    const bool countAfterReadable =
        ReadParticipantCountFromReadySink(sink, countAfter);
    const auto recordAfter =
        ReadRecordFromReadySink(sink, copy, messageReadable);
    if (auto* evidence = gEvidence.load(std::memory_order_acquire);
        evidence != nullptr) {
        evidence->ObserveReadinessBarrier(
            GetCurrentThreadId(),
            PerformanceCounter(),
            Generation(),
            reinterpret_cast<std::uintptr_t>(sink),
            copy,
            messageReadable,
            countBefore,
            countBeforeReadable,
            countAfter,
            countAfterReadable,
            recordAfter.recordReadable
                ? recordAfter.record
                : recordBefore.record,
            recordBefore.readyCount,
            recordBefore.readyCountReadable,
            recordAfter.readyCount,
            recordAfter.readyCountReadable,
            recordBefore.state,
            recordBefore.stateReadable,
            recordAfter.state,
            recordAfter.stateReadable);
    }
}

} // namespace

void GameThreadRuntimeEvidence::AtomicSource::Reset() noexcept {
    calls.store(0, std::memory_order_release);
    primaryThreadId.store(0, std::memory_order_release);
    lastThreadId.store(0, std::memory_order_release);
    firstPerformanceCounter.store(0, std::memory_order_release);
    lastPerformanceCounter.store(0, std::memory_order_release);
    observedOnMultipleThreads.store(false, std::memory_order_release);
}

void GameThreadRuntimeEvidence::AtomicSource::Observe(
    const std::uint32_t threadId,
    const std::uint64_t performanceCounter) noexcept {
    if (threadId == 0 || performanceCounter == 0) {
        return;
    }
    std::uint32_t expected = 0;
    if (!primaryThreadId.compare_exchange_strong(
            expected,
            threadId,
            std::memory_order_acq_rel,
            std::memory_order_acquire) &&
        expected != threadId) {
        observedOnMultipleThreads.store(true, std::memory_order_release);
    }
    std::uint64_t noCounter = 0;
    firstPerformanceCounter.compare_exchange_strong(
        noCounter,
        performanceCounter,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    lastThreadId.store(threadId, std::memory_order_release);
    lastPerformanceCounter.store(
        performanceCounter,
        std::memory_order_release);
    calls.fetch_add(1, std::memory_order_acq_rel);
}

RuntimeHookSourceSnapshot
GameThreadRuntimeEvidence::AtomicSource::Snapshot() const noexcept {
    RuntimeHookSourceSnapshot snapshot{};
    snapshot.calls = calls.load(std::memory_order_acquire);
    snapshot.primaryThreadId =
        primaryThreadId.load(std::memory_order_acquire);
    snapshot.lastThreadId =
        lastThreadId.load(std::memory_order_acquire);
    snapshot.firstPerformanceCounter =
        firstPerformanceCounter.load(std::memory_order_acquire);
    snapshot.lastPerformanceCounter =
        lastPerformanceCounter.load(std::memory_order_acquire);
    snapshot.observedOnMultipleThreads =
        observedOnMultipleThreads.load(std::memory_order_acquire);
    return snapshot;
}

std::uint64_t GameThreadRuntimeEvidence::Pack(
    const std::array<std::byte, 8>& message) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, message.data(), message.size());
    return value;
}

std::array<std::byte, 8> GameThreadRuntimeEvidence::Unpack(
    const std::uint64_t value) noexcept {
    std::array<std::byte, 8> message{};
    std::memcpy(message.data(), &value, message.size());
    return message;
}

void GameThreadRuntimeEvidence::ResetGeneration(
    const std::uint64_t generation) noexcept {
    mainThreadCharacterProcessing_.Reset();
    characterManagerUpdate_.Reset();
    lastCharacterManager_.store(0, std::memory_order_release);
    readinessProducer_.Reset();
    readinessPublisher_.Reset();
    readinessBarrier_.Reset();
    lastProducerGeneration_.store(0, std::memory_order_release);
    lastPublisherGeneration_.store(0, std::memory_order_release);
    lastBarrierGeneration_.store(0, std::memory_order_release);
    lastProducerSpawnKey_.store(0, std::memory_order_release);
    lastProducerManager_.store(0, std::memory_order_release);
    lastProducerRecord_.store(0, std::memory_order_release);
    lastPublisher_.store(0, std::memory_order_release);
    lastBarrierSink_.store(0, std::memory_order_release);
    lastPublisherMessage_.store(0, std::memory_order_release);
    lastBarrierMessage_.store(0, std::memory_order_release);
    participantCountBeforeBarrier_.store(0, std::memory_order_release);
    participantCountAfterBarrier_.store(0, std::memory_order_release);
    barrierRecord_.store(0, std::memory_order_release);
    readyCountBeforeBarrier_.store(0, std::memory_order_release);
    readyCountAfterBarrier_.store(0, std::memory_order_release);
    recordStateBeforeBarrier_.store(0, std::memory_order_release);
    recordStateAfterBarrier_.store(0, std::memory_order_release);
    publisherMessageReadable_.store(false, std::memory_order_release);
    barrierMessageReadable_.store(false, std::memory_order_release);
    participantCountBeforeReadable_.store(false, std::memory_order_release);
    participantCountAfterReadable_.store(false, std::memory_order_release);
    barrierRecordReadable_.store(false, std::memory_order_release);
    readyCountBeforeReadable_.store(false, std::memory_order_release);
    readyCountAfterReadable_.store(false, std::memory_order_release);
    recordStateBeforeReadable_.store(false, std::memory_order_release);
    recordStateAfterReadable_.store(false, std::memory_order_release);
    generation_.store(generation, std::memory_order_release);
}

void GameThreadRuntimeEvidence::ObserveMainThreadCharacterProcessing(
    const std::uint32_t threadId,
    const std::uint64_t performanceCounter) noexcept {
    mainThreadCharacterProcessing_.Observe(threadId, performanceCounter);
}

void GameThreadRuntimeEvidence::ObserveCharacterManagerUpdate(
    const std::uint32_t threadId,
    const std::uint64_t performanceCounter,
    const std::uintptr_t manager) noexcept {
    characterManagerUpdate_.Observe(threadId, performanceCounter);
    if (manager != 0U) {
        lastCharacterManager_.store(manager, std::memory_order_release);
    }
}

void GameThreadRuntimeEvidence::ObserveReadinessProducer(
    const std::uint32_t threadId,
    const std::uint64_t performanceCounter,
    const std::uint64_t generation,
    const std::uintptr_t manager,
    const std::uint32_t spawnKey,
    const std::uintptr_t record) noexcept {
    if (generation == 0 ||
        generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    readinessProducer_.Observe(threadId, performanceCounter);
    lastProducerGeneration_.store(generation, std::memory_order_release);
    lastProducerManager_.store(manager, std::memory_order_release);
    lastProducerSpawnKey_.store(spawnKey, std::memory_order_release);
    lastProducerRecord_.store(record, std::memory_order_release);
}

void GameThreadRuntimeEvidence::ObserveReadinessPublisher(
    const std::uint32_t threadId,
    const std::uint64_t performanceCounter,
    const std::uint64_t generation,
    const std::uintptr_t publisher,
    const std::array<std::byte, 8>& message,
    const bool messageReadable) noexcept {
    if (generation == 0 ||
        generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    readinessPublisher_.Observe(threadId, performanceCounter);
    lastPublisherGeneration_.store(generation, std::memory_order_release);
    lastPublisher_.store(publisher, std::memory_order_release);
    lastPublisherMessage_.store(Pack(message), std::memory_order_release);
    publisherMessageReadable_.store(
        messageReadable,
        std::memory_order_release);
}

void GameThreadRuntimeEvidence::ObserveReadinessBarrier(
    const std::uint32_t threadId,
    const std::uint64_t performanceCounter,
    const std::uint64_t generation,
    const std::uintptr_t sink,
    const std::array<std::byte, 8>& message,
    const bool messageReadable,
    const std::uint32_t participantCountBefore,
    const bool participantCountBeforeReadable,
    const std::uint32_t participantCountAfter,
    const bool participantCountAfterReadable,
    const std::uintptr_t barrierRecord,
    const std::uint32_t readyCountBefore,
    const bool readyCountBeforeReadable,
    const std::uint32_t readyCountAfter,
    const bool readyCountAfterReadable,
    const std::uint8_t recordStateBefore,
    const bool recordStateBeforeReadable,
    const std::uint8_t recordStateAfter,
    const bool recordStateAfterReadable) noexcept {
    if (generation == 0 ||
        generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    readinessBarrier_.Observe(threadId, performanceCounter);
    lastBarrierGeneration_.store(generation, std::memory_order_release);
    lastBarrierSink_.store(sink, std::memory_order_release);
    lastBarrierMessage_.store(Pack(message), std::memory_order_release);
    barrierMessageReadable_.store(
        messageReadable,
        std::memory_order_release);
    participantCountBeforeBarrier_.store(
        participantCountBefore,
        std::memory_order_release);
    participantCountAfterBarrier_.store(
        participantCountAfter,
        std::memory_order_release);
    barrierRecord_.store(barrierRecord, std::memory_order_release);
    readyCountBeforeBarrier_.store(
        readyCountBefore,
        std::memory_order_release);
    readyCountAfterBarrier_.store(
        readyCountAfter,
        std::memory_order_release);
    recordStateBeforeBarrier_.store(
        recordStateBefore,
        std::memory_order_release);
    recordStateAfterBarrier_.store(
        recordStateAfter,
        std::memory_order_release);
    participantCountBeforeReadable_.store(
        participantCountBeforeReadable,
        std::memory_order_release);
    participantCountAfterReadable_.store(
        participantCountAfterReadable,
        std::memory_order_release);
    barrierRecordReadable_.store(
        barrierRecord != 0,
        std::memory_order_release);
    readyCountBeforeReadable_.store(
        readyCountBeforeReadable,
        std::memory_order_release);
    readyCountAfterReadable_.store(
        readyCountAfterReadable,
        std::memory_order_release);
    recordStateBeforeReadable_.store(
        recordStateBeforeReadable,
        std::memory_order_release);
    recordStateAfterReadable_.store(
        recordStateAfterReadable,
        std::memory_order_release);
}

GameThreadRuntimeSnapshot GameThreadRuntimeEvidence::Snapshot() const noexcept {
    GameThreadRuntimeSnapshot snapshot{};
    snapshot.generation = generation_.load(std::memory_order_acquire);
    snapshot.mainThreadCharacterProcessing =
        mainThreadCharacterProcessing_.Snapshot();
    snapshot.characterManagerUpdate =
        characterManagerUpdate_.Snapshot();
    snapshot.lastCharacterManager =
        lastCharacterManager_.load(std::memory_order_acquire);
    snapshot.readiness.producer = readinessProducer_.Snapshot();
    snapshot.readiness.publisher = readinessPublisher_.Snapshot();
    snapshot.readiness.barrier = readinessBarrier_.Snapshot();
    snapshot.readiness.lastProducerGeneration =
        lastProducerGeneration_.load(std::memory_order_acquire);
    snapshot.readiness.lastPublisherGeneration =
        lastPublisherGeneration_.load(std::memory_order_acquire);
    snapshot.readiness.lastBarrierGeneration =
        lastBarrierGeneration_.load(std::memory_order_acquire);
    snapshot.readiness.lastProducerSpawnKey =
        lastProducerSpawnKey_.load(std::memory_order_acquire);
    snapshot.readiness.lastProducerManager =
        lastProducerManager_.load(std::memory_order_acquire);
    snapshot.readiness.lastProducerRecord =
        lastProducerRecord_.load(std::memory_order_acquire);
    snapshot.readiness.lastPublisher =
        lastPublisher_.load(std::memory_order_acquire);
    snapshot.readiness.lastBarrierSink =
        lastBarrierSink_.load(std::memory_order_acquire);
    snapshot.readiness.lastPublisherMessage =
        Unpack(lastPublisherMessage_.load(std::memory_order_acquire));
    snapshot.readiness.lastBarrierMessage =
        Unpack(lastBarrierMessage_.load(std::memory_order_acquire));
    snapshot.readiness.participantCountBeforeBarrier =
        participantCountBeforeBarrier_.load(std::memory_order_acquire);
    snapshot.readiness.participantCountAfterBarrier =
        participantCountAfterBarrier_.load(std::memory_order_acquire);
    snapshot.readiness.barrierRecord =
        barrierRecord_.load(std::memory_order_acquire);
    snapshot.readiness.readyCountBeforeBarrier =
        readyCountBeforeBarrier_.load(std::memory_order_acquire);
    snapshot.readiness.readyCountAfterBarrier =
        readyCountAfterBarrier_.load(std::memory_order_acquire);
    snapshot.readiness.recordStateBeforeBarrier =
        recordStateBeforeBarrier_.load(std::memory_order_acquire);
    snapshot.readiness.recordStateAfterBarrier =
        recordStateAfterBarrier_.load(std::memory_order_acquire);
    snapshot.readiness.publisherMessageReadable =
        publisherMessageReadable_.load(std::memory_order_acquire);
    snapshot.readiness.barrierMessageReadable =
        barrierMessageReadable_.load(std::memory_order_acquire);
    snapshot.readiness.participantCountBeforeReadable =
        participantCountBeforeReadable_.load(std::memory_order_acquire);
    snapshot.readiness.participantCountAfterReadable =
        participantCountAfterReadable_.load(std::memory_order_acquire);
    snapshot.readiness.barrierRecordReadable =
        barrierRecordReadable_.load(std::memory_order_acquire);
    snapshot.readiness.readyCountBeforeReadable =
        readyCountBeforeReadable_.load(std::memory_order_acquire);
    snapshot.readiness.readyCountAfterReadable =
        readyCountAfterReadable_.load(std::memory_order_acquire);
    snapshot.readiness.recordStateBeforeReadable =
        recordStateBeforeReadable_.load(std::memory_order_acquire);
    snapshot.readiness.recordStateAfterReadable =
        recordStateAfterReadable_.load(std::memory_order_acquire);
    return snapshot;
}

GameThreadObservationHooks::GameThreadObservationHooks(
    GameThreadRuntimeEvidence& evidence,
    GameThreadDispatcher& dispatcher,
    LevelGenerationTracker& generations,
    PlayerSyncObserver& playerSync) noexcept
    : evidence_(&evidence),
      dispatcher_(&dispatcher),
      generations_(&generations),
      playerSync_(&playerSync) {}

GameThreadObservationHooks::~GameThreadObservationHooks() {
    BeginLogicalShutdown();
}

bool GameThreadObservationHooks::InstallEngineThreadHooks(
    const SignatureResolver& resolver,
    HookManager& hooks,
    std::string& error) {
    if (MainThreadHookInstalled() ||
        ManagerUpdateHookInstalled() ||
        ReadinessProducerHookInstalled()) {
        error = "engine-thread hooks already installed or partially installed";
        return false;
    }
    const auto& profile = Retail2015Profile();
    GameThreadRuntimeEvidence* expected = nullptr;
    if (!gEvidence.compare_exchange_strong(
            expected,
            evidence_,
            std::memory_order_acq_rel)) {
        error = "another game-thread evidence service is active";
        return false;
    }
    gDispatcher.store(dispatcher_, std::memory_order_release);
    gGenerations.store(generations_, std::memory_order_release);
    gPlayerSync.store(playerSync_, std::memory_order_release);

    const auto install = [&](
        const SymbolId id,
        const char* name,
        void* detour,
        void** trampoline,
        std::atomic<void*>& published) {
        const auto* spec = profile.Find(id);
        auto* target = resolver.ResolveMutable(id);
        if (spec == nullptr || target == nullptr) {
            error = std::string(name) + " exact signature did not resolve";
            return false;
        }
        HookRequest request{};
        request.name = name;
        request.target = target;
        request.detour = detour;
        request.patchLength = spec->approvedPatchLength;
        request.expected = spec->expected;
        request.relocationSafePrefix = spec->trampolineRelocationSafe;
        request.trampolineOut = trampoline;
        request.publishedTrampolineOut = &published;
        if (!hooks.Install(request, error)) {
            return false;
        }
        processLifetimeRequired_ = true;
        return true;
    };

    mainThreadHookInstalled_ = install(
        SymbolId::MainThreadCharacterProcessing,
        "engine_thread_main_character_processing",
        reinterpret_cast<void*>(&MainThreadCharacterProcessingDetour),
        &mainThreadTrampoline_,
        gMainThreadTrampoline);
    const auto mainError = error;
    error.clear();
    managerUpdateHookInstalled_ = install(
        SymbolId::CharacterManagerUpdate,
        "engine_thread_character_manager_update",
        reinterpret_cast<void*>(&CharacterManagerUpdateDetour),
        &managerUpdateTrampoline_,
        gManagerUpdateTrampoline);
    const auto managerError = error;
    error.clear();
    readinessProducerHookInstalled_ = install(
        SymbolId::CharacterReadinessProducer,
        "engine_thread_character_readiness_producer",
        reinterpret_cast<void*>(&ReadinessProducerDetour),
        &readinessProducerTrampoline_,
        gReadinessProducerTrampoline);
    const auto producerError = error;
    if (!Installed()) {
        error = "no engine-thread hook could be installed; main=" +
            mainError + "; manager=" + managerError +
            "; producer=" + producerError;
        if (!processLifetimeRequired_) {
            gEvidence.store(nullptr, std::memory_order_release);
            gDispatcher.store(nullptr, std::memory_order_release);
            gGenerations.store(nullptr, std::memory_order_release);
            gPlayerSync.store(nullptr, std::memory_order_release);
        }
        return false;
    }
    if (!mainThreadHookInstalled_ ||
        !managerUpdateHookInstalled_ ||
        !readinessProducerHookInstalled_) {
        error = "engine-thread hook fallback is partial; main=" + mainError +
            "; manager=" + managerError +
            "; producer=" + producerError;
    } else {
        error.clear();
    }
    return true;
}

bool GameThreadObservationHooks::InstallOptionalReadinessHooks(
    const SignatureResolver& resolver,
    HookManager& hooks,
    std::string& error) {
    if (!Installed()) {
        error = "engine-thread hooks must be installed first";
        return false;
    }
    if (readinessPublisherHookInstalled_ ||
        readinessBarrierHookInstalled_) {
        error = "readiness hooks already installed or partially installed";
        return false;
    }
    const auto& profile = Retail2015Profile();
    constexpr std::array symbols{
        SymbolId::CharacterReadinessPublisher,
        SymbolId::CharacterReadinessBarrier,
    };
    for (const auto id : symbols) {
        const auto* spec = profile.Find(id);
        if (spec == nullptr || resolver.Resolve(id) == nullptr ||
            spec->approvedPatchLength < 5 ||
            !spec->trampolineRelocationSafe) {
            error = "optional readiness hook target is not certified";
            return false;
        }
    }

    const auto install = [&](
        const SymbolId id,
        const char* name,
        void* detour,
        void** trampoline,
        std::atomic<void*>& published) {
        const auto* spec = profile.Find(id);
        auto* target = resolver.ResolveMutable(id);
        if (spec == nullptr || target == nullptr) {
            error = std::string(name) + " exact signature did not resolve";
            return false;
        }
        HookRequest request{};
        request.name = name;
        request.target = target;
        request.detour = detour;
        request.patchLength = spec->approvedPatchLength;
        request.expected = spec->expected;
        request.relocationSafePrefix = spec->trampolineRelocationSafe;
        request.trampolineOut = trampoline;
        request.publishedTrampolineOut = &published;
        if (!hooks.Install(request, error)) {
            return false;
        }
        processLifetimeRequired_ = true;
        return true;
    };

    readinessPublisherHookInstalled_ = install(
        SymbolId::CharacterReadinessPublisher,
        "observe_character_readiness_publisher",
        reinterpret_cast<void*>(&ReadinessPublisherDetour),
        &readinessPublisherTrampoline_,
        gReadinessPublisherTrampoline);
    if (readinessPublisherHookInstalled_) {
        readinessBarrierHookInstalled_ = install(
            SymbolId::CharacterReadinessBarrier,
            "observe_character_readiness_barrier",
            reinterpret_cast<void*>(&ReadinessBarrierDetour),
            &readinessBarrierTrampoline_,
            gReadinessBarrierTrampoline);
    }
    return readinessPublisherHookInstalled_ &&
        readinessBarrierHookInstalled_;
}

void GameThreadObservationHooks::BeginLogicalShutdown() noexcept {
    gDispatcher.store(nullptr, std::memory_order_release);
    gEvidence.store(nullptr, std::memory_order_release);
    gGenerations.store(nullptr, std::memory_order_release);
    gPlayerSync.store(nullptr, std::memory_order_release);
}

bool GameThreadObservationHooks::Installed() const noexcept {
    return MainThreadHookInstalled() ||
        ManagerUpdateHookInstalled() ||
        ReadinessProducerHookInstalled();
}

bool GameThreadObservationHooks::MainThreadHookInstalled() const noexcept {
    return mainThreadHookInstalled_;
}

bool GameThreadObservationHooks::ManagerUpdateHookInstalled() const noexcept {
    return managerUpdateHookInstalled_;
}

bool GameThreadObservationHooks::ReadinessProducerHookInstalled() const noexcept {
    return readinessProducerHookInstalled_;
}

bool GameThreadObservationHooks::ReadinessHooksInstalled() const noexcept {
    return readinessProducerHookInstalled_ &&
        readinessPublisherHookInstalled_ &&
        readinessBarrierHookInstalled_;
}

bool GameThreadObservationHooks::ProcessLifetimeRequired() const noexcept {
    return processLifetimeRequired_;
}

std::uint32_t GameThreadObservationHooks::ActiveDetours() noexcept {
    return gActiveDetours.load(std::memory_order_acquire);
}

} // namespace aimp
