#pragma once

#include "aimp/Common.h"
#include "aimp/Protocol.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <Windows.h>

namespace aimp {

class Diagnostics;
class InboundQueue;
class CapabilityMatrix;
class CaptureCatalog;
class DummyPlayerController;
class GameThreadDispatcher;
class IGameCommandExecutor;
class IDummyNativeExecutor;
class LevelGenerationTracker;
class NetZDiscovery;
class PlayerSyncObserver;
class SpawnCharacterTracker;

struct PeerJoinControl {
    std::uint64_t peerId{};
    std::string launcherPlayerId;
    std::string displayName;
    bool isHost{};
    bool ready{};

    bool operator==(const PeerJoinControl&) const = default;
};

namespace ipc_validation {

bool DecodePeerJoin(
    std::span<const std::byte> payload,
    PeerJoinControl& output) noexcept;
bool DecodePeerLeave(
    std::span<const std::byte> payload,
    std::uint64_t& peerId) noexcept;

} // namespace ipc_validation

class LauncherIPC final {
public:
    LauncherIPC(Diagnostics& diagnostics, InboundQueue& inboundQueue) noexcept;
    ~LauncherIPC();

    LauncherIPC(const LauncherIPC&) = delete;
    LauncherIPC& operator=(const LauncherIPC&) = delete;

    [[nodiscard]] static constexpr ModuleStatus Status() noexcept {
        return ModuleStatus::PartiallyImplemented;
    }

    bool Start(
        std::uint32_t processId,
        bool buildValidated,
        const std::array<std::uint8_t, 32>& executableHash,
        std::string profileId,
        std::string resolutionMode);
    void AttachExperimentalServices(
        CapabilityMatrix& capabilities,
        CaptureCatalog& captures,
        LevelGenerationTracker& generations,
        GameThreadDispatcher& dispatcher,
        DummyPlayerController& dummy,
        IDummyNativeExecutor& executor) noexcept;
    void AttachDiscovery(NetZDiscovery& discovery) noexcept;
    void AttachGameCommandServices(IGameCommandExecutor& commands) noexcept;
    void AttachSpawnCharacterTracker(SpawnCharacterTracker& tracker) noexcept;
    void AttachPlayerSyncObserver(PlayerSyncObserver& observer) noexcept;
    void Stop() noexcept;
    void PublishRuntimeState();

    bool Queue(
        protocol::MessageType type,
        std::uint16_t flags,
        std::uint64_t sequence,
        std::span<const std::byte> payload);

    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] bool ShutdownRequested() const noexcept;
    [[nodiscard]] std::wstring PipeName() const;

private:
    struct ExperimentalCachedResponse {
        protocol::MessageType type{protocol::MessageType::ExperimentalError};
        std::vector<std::byte> requestPayload;
        std::vector<std::byte> responsePayload;
    };

    struct ExperimentalPendingRequest {
        protocol::MessageType type{protocol::MessageType::ExperimentalError};
        std::vector<std::byte> requestPayload;
        std::uint64_t epoch{};
    };

    enum class ExperimentalReservationState : std::uint8_t {
        Reserved,
        Cached,
        PendingDuplicate,
        Collision,
        CapacityReached,
    };

    struct ExperimentalReservation {
        ExperimentalReservationState state{
            ExperimentalReservationState::Collision};
        protocol::MessageType cachedType{
            protocol::MessageType::ExperimentalError};
        std::vector<std::byte> cachedResponse;
        std::uint64_t epoch{};
    };

    void Run();
    void PublishWorldReplicationTraffic();
    void PublishPlayerSyncTraffic();
    void PublishPlayerLifecycleTraffic();
    void PublishCharacterIncarnationTraffic();
    void PublishPlayerHidingTraffic();
    void PublishPlayerAnimationTraffic();
    void PublishItemSyncTraffic();
    void PublishMissionSyncTraffic();
    void PublishItemSyncSpawnTraffic();
    void PublishThrowableTraffic();
    void PublishAlienPvPTraffic();
    void PublishAISyncTraffic();
    void PublishAISyncTransactionTraffic();
    void PublishAISyncExecutionTraffic();
    void PublishAISyncHostTraffic();
    bool HandleAISyncFrame(const protocol::Frame& frame);
    bool HandleAISyncExecutionFrame(const protocol::Frame& frame);
    bool HandleAISyncHostFrame(const protocol::Frame& frame);
    bool TryConnect();
    void Disconnect(std::string_view reason) noexcept;
    bool ReceiveAvailable();
    bool FlushOutgoing(std::size_t maximumFrames);
    bool HandleFrame(protocol::Frame frame);
    bool WriteFrame(std::span<const std::byte> bytes);
    bool ReadExact(void* destination, std::size_t bytes);
    bool WriteExact(const void* source, std::size_t bytes);
    std::vector<std::byte> BuildHelloPayload() const;
    std::vector<std::byte> BuildInfoPayload() const;
    std::vector<std::byte> BuildObservationStatusPayload() const;
    bool QueueExperimental(
        protocol::MessageType type,
        std::span<const std::byte> payload);
    ExperimentalReservation ReserveExperimentalRequest(
        std::string_view requestId,
        protocol::MessageType responseType,
        std::span<const std::byte> requestPayload);
    bool CompleteExperimentalRequest(
        std::string_view requestId,
        std::uint64_t epoch,
        protocol::MessageType responseType,
        std::span<const std::byte> requestPayload,
        std::span<const std::byte> responsePayload);
    void ResetExperimentalRequests() noexcept;
    [[nodiscard]] std::uint64_t NextExperimentalDispatchId() noexcept;
    bool WaitStop(DWORD milliseconds) const noexcept;

    Diagnostics* diagnostics_{};
    InboundQueue* inboundQueue_{};
    std::wstring pipeName_;
    std::array<std::uint8_t, 32> executableHash_{};
    std::uint32_t processId_{};
    std::uint64_t aiSyncRejectedFrames_{}; // IPC-worker only, retained across reconnects.
    std::uint64_t publishedDeathSequence_{}; // IPC-worker only.
    std::uint64_t publishedLevelExitSequence_{}; // IPC-worker only.
    std::uint64_t publishedSpectatePeer_{}; // IPC-worker only.
    std::uint32_t publishedSpectateCandidates_{}; // IPC-worker only.
    std::array<std::uint8_t,40> publishedCharacterIncarnation_{}; // IPC-worker only.
    bool characterIncarnationPublished_{};
    std::string publishedHiding_; // IPC worker only.
    std::uint64_t lastHidingPublication_{};
    std::string publishedAnimation_; // IPC worker only.
    std::uint64_t lastAnimationPublication_{};
    bool buildValidated_{};
    std::string profileId_;
    std::string resolutionMode_;
    std::string nonce_;
    std::uint64_t localPeerId_{};
    std::string worldSyncAuthoritySessionId_;
    std::string worldSyncLastBoundSessionId_;
    std::string lastRoundBoundarySessionId_;
    std::uint32_t lastRoundBoundaryNetworkGeneration_{};
    std::uint64_t worldSyncAuthorityGeneration_{};
    bool worldSyncAuthorityBound_{};
    bool worldSyncAuthorityHost_{};
    bool worldSyncWorldBusActive_{};
    std::string localLauncherPlayerId_;
    std::string negotiatedDummyId_;
    std::string negotiationRequestId_;
    std::map<std::uint64_t, PeerJoinControl> remotePeers_;
    CapabilityMatrix* capabilities_{};
    CaptureCatalog* captures_{};
    LevelGenerationTracker* generations_{};
    GameThreadDispatcher* dispatcher_{};
    DummyPlayerController* dummy_{};
    IDummyNativeExecutor* dummyExecutor_{};
    NetZDiscovery* discovery_{};
    IGameCommandExecutor* gameCommands_{};
    SpawnCharacterTracker* spawnTracker_{};
    PlayerSyncObserver* playerSyncObserver_{};
    std::map<
        std::string,
        ExperimentalCachedResponse,
        std::less<>> experimentalResponses_;
    std::map<
        std::string,
        ExperimentalPendingRequest,
        std::less<>> experimentalPending_;

    HANDLE stopEvent_{nullptr};
    std::atomic<HANDLE> pipe_{INVALID_HANDLE_VALUE};
    std::thread thread_;
    std::atomic<DWORD> workerThreadId_{0U};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> shutdownRequested_{false};
    std::atomic<bool> helloAcknowledged_{false};
    std::atomic<bool> sessionConfigured_{false};
    std::atomic<bool> netzReadySent_{false};
    std::atomic<bool> experimentalNegotiated_{false};
    std::atomic<std::uint64_t> lastPublishedGeneration_{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::atomic<std::uint64_t> lastPublishedCaptureCount_{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::atomic<std::uint64_t> lastPublishedCaptureGeneration_{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::atomic<std::uint8_t> lastPublishedStability_{0xFFU};
    std::string lastPublishedLevelName_;
    std::atomic<std::uint8_t> lastPublishedGameThreadValidated_{0xFFU};
    std::atomic<std::uint32_t> lastPublishedGameThreadCandidateId_{
        (std::numeric_limits<std::uint32_t>::max)()};
    std::atomic<std::uint32_t> lastPublishedGameThreadSourceCount_{
        (std::numeric_limits<std::uint32_t>::max)()};
    std::atomic<std::uint8_t> lastPublishedScheduleState_{0xFFU};
    std::atomic<std::uint64_t> lastPublishedPendingCommandCount_{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::atomic<std::uint64_t> lastPublishedSpawnTrackerVersion_{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::atomic<std::uint64_t> lastPublishedSpawnCreationSerial_{0};
    std::atomic<std::uint64_t> lastPublishedPlayerSyncRevision_{
        (std::numeric_limits<std::uint64_t>::max)()};

    mutable std::mutex outgoingMutex_;
    mutable std::mutex experimentalStateMutex_;
    std::deque<std::vector<std::byte>> outgoing_;
    std::size_t outgoingBytes_{};
    // Adaptive polling state owned by the worker.
    bool workerTrafficObserved_{};
    std::atomic<std::uint64_t> localSequence_{1};
    std::atomic<std::uint64_t> experimentalEpoch_{1};
    std::atomic<std::uint64_t> experimentalDispatchSequence_{1};
};

} // namespace aimp
