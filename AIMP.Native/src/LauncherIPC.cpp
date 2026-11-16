#include "aimp/LoadingContextGuard.h"
#include "aimp/AISync.h"
#include "aimp/AISyncTransactionCapture.h"
#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/AISyncRouteRuntime.h"
#include "aimp/AISyncHostProducer.h"
#include "aimp/VoiceSync.h"
#include "aimp/LauncherIPC.h"
#include "aimp/AlienPvPControlRuntime.h"
#include "aimp/PlayerAnimationClipCapture.h"
#include "aimp/IpcWriteBatch.h"
#include "aimp/WorldSemanticReconciliation.h"
#include "aimp/CharacterIncarnation.h"
#include "aimp/PlayerHidingLocalCapture.h"
#include "aimp/PlayerHidingPresentationRuntime.h"
#include "aimp/AnimationSyncRuntime.h"
#include "aimp/HidingDoorPresentation.h"
#include "aimp/HidingPresentationAudio.h"

#include "aimp/BuildProfile.h"
#include "aimp/CapabilityMatrix.h"
#include "aimp/CaptureCatalog.h"
#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/Diagnostics.h"
#include "aimp/DummyPlayerController.h"
#include "aimp/ExperimentalProtocol.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/GameCommandExecutor.h"
#include "aimp/InboundQueue.h"
#include "aimp/ItemSync.h"
#include "aimp/ThrowableSync.h"
#include "aimp/MissionSync.h"
#include "aimp/AlienPvPInputIsolation.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/MissionLevelObservation.h"
#include "aimp/MultiplayerPause.h"
#include "aimp/NetZDiscovery.h"
#include "aimp/ReplicationCore.h"
#include "aimp/RuntimeFeatureFlags.h"
#include "aimp/SpawnCharacterTracker.h"
#include "aimp/WorldReplicationCodec.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldAnimatedAdapters.h"
#include "aimp/ProductVersion.h"
#include "aimp/PlayerSyncObserver.h"
#include "aimp/PlayerLifecycle.h"
#include "aimp/CampaignSaveRuntime.h"
#include "aimp/UIHeapGuard.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <variant>

namespace aimp {
namespace {

std::vector<std::byte> JsonBytes(const std::string& json) {
    const auto* begin = reinterpret_cast<const std::byte*>(json.data());
    return std::vector<std::byte>(begin, begin + json.size());
}

using JsonScalar = std::variant<std::string, std::uint64_t, bool>;
using JsonObject = std::map<std::string, JsonScalar, std::less<>>;

class ClosedJsonParser final {
public:
    explicit ClosedJsonParser(const std::span<const std::byte> bytes) {
        text_.reserve(bytes.size());
        for (const auto value : bytes) {
            text_.push_back(static_cast<char>(std::to_integer<std::uint8_t>(value)));
        }
    }

    bool Parse(JsonObject& output) {
        output.clear();
        SkipWhitespace();
        if (!Consume('{')) {
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            SkipWhitespace();
            return cursor_ == text_.size();
        }

        for (;;) {
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }
            SkipWhitespace();

            JsonScalar value;
            if (!ParseScalar(value) ||
                output.find(key) != output.end()) {
                return false;
            }
            output.emplace(std::move(key), std::move(value));
            SkipWhitespace();
            if (Consume('}')) {
                SkipWhitespace();
                return cursor_ == text_.size();
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

private:
    bool ParseHex4(std::uint16_t& output) noexcept {
        if (cursor_ + 4 > text_.size()) {
            return false;
        }
        output = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const unsigned char value =
                static_cast<unsigned char>(text_[cursor_++]);
            std::uint16_t digit = 0;
            if (value >= '0' && value <= '9') {
                digit = static_cast<std::uint16_t>(value - '0');
            } else if (value >= 'a' && value <= 'f') {
                digit = static_cast<std::uint16_t>(value - 'a' + 10);
            } else if (value >= 'A' && value <= 'F') {
                digit = static_cast<std::uint16_t>(value - 'A' + 10);
            } else {
                return false;
            }
            output = static_cast<std::uint16_t>((output << 4U) | digit);
        }
        return true;
    }

    static bool AppendUtf8(std::string& output, const std::uint32_t codePoint) {
        if (codePoint <= 0x7FU) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0xFFFFU) {
            if (codePoint >= 0xD800U && codePoint <= 0xDFFFU) {
                return false;
            }
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0x10FFFFU) {
            output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else {
            return false;
        }
        return true;
    }

    void SkipWhitespace() noexcept {
        while (cursor_ < text_.size()) {
            const char value = text_[cursor_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++cursor_;
        }
    }

    bool Consume(const char expected) noexcept {
        if (cursor_ >= text_.size() || text_[cursor_] != expected) {
            return false;
        }
        ++cursor_;
        return true;
    }

    bool ParseString(std::string& output) {
        SkipWhitespace();
        if (!Consume('"')) {
            return false;
        }
        output.clear();
        while (cursor_ < text_.size()) {
            const unsigned char value =
                static_cast<unsigned char>(text_[cursor_++]);
            if (value == '"') {
                return true;
            }
            if (value < 0x20U) {
                return false;
            }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (cursor_ >= text_.size()) {
                return false;
            }
            const char escape = text_[cursor_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escape);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                std::uint16_t first = 0;
                if (!ParseHex4(first)) {
                    return false;
                }
                std::uint32_t codePoint = first;
                if (first >= 0xD800U && first <= 0xDBFFU) {
                    if (cursor_ + 2 > text_.size() ||
                        text_[cursor_] != '\\' ||
                        text_[cursor_ + 1] != 'u') {
                        return false;
                    }
                    cursor_ += 2;
                    std::uint16_t second = 0;
                    if (!ParseHex4(second) ||
                        second < 0xDC00U ||
                        second > 0xDFFFU) {
                        return false;
                    }
                    codePoint =
                        0x10000U +
                        ((static_cast<std::uint32_t>(first) - 0xD800U) << 10U) +
                        (static_cast<std::uint32_t>(second) - 0xDC00U);
                } else if (first >= 0xDC00U && first <= 0xDFFFU) {
                    return false;
                }
                if (!AppendUtf8(output, codePoint)) {
                    return false;
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseScalar(JsonScalar& output) {
        if (cursor_ >= text_.size()) {
            return false;
        }
        if (text_[cursor_] == '"') {
            std::string stringValue;
            if (!ParseString(stringValue)) {
                return false;
            }
            output = std::move(stringValue);
            return true;
        }
        if (text_.compare(cursor_, 4, "true") == 0) {
            cursor_ += 4;
            output = true;
            return true;
        }
        if (text_.compare(cursor_, 5, "false") == 0) {
            cursor_ += 5;
            output = false;
            return true;
        }
        if (!std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
            return false;
        }

        const auto numberBegin = cursor_;
        const bool leadingZero = text_[cursor_] == '0';
        std::uint64_t number = 0;
        while (cursor_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
            const auto digit = static_cast<std::uint64_t>(text_[cursor_] - '0');
            if (number >
                (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                return false;
            }
            number = number * 10U + digit;
            ++cursor_;
        }
        if (leadingZero && cursor_ - numberBegin != 1) {
            return false;
        }
        output = number;
        return true;
    }

    std::string text_;
    std::size_t cursor_{};
};

bool IsVisibleAscii(const std::string& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return character >= 0x20U && character <= 0x7EU;
    });
}

bool IsValidUtf8Text(
    const std::string& value,
    const std::size_t maximumUtf16CodeUnits) noexcept {
    std::size_t cursor = 0;
    std::size_t utf16CodeUnits = 0;
    while (cursor < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[cursor]);
        std::uint32_t codePoint = 0;
        std::size_t continuation = 0;
        if (first <= 0x7FU) {
            codePoint = first;
        } else if ((first & 0xE0U) == 0xC0U) {
            codePoint = first & 0x1FU;
            continuation = 1;
        } else if ((first & 0xF0U) == 0xE0U) {
            codePoint = first & 0x0FU;
            continuation = 2;
        } else if ((first & 0xF8U) == 0xF0U) {
            codePoint = first & 0x07U;
            continuation = 3;
        } else {
            return false;
        }
        if (cursor + continuation >= value.size()) {
            return false;
        }
        for (std::size_t index = 1; index <= continuation; ++index) {
            const auto next = static_cast<std::uint8_t>(value[cursor + index]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (next & 0x3FU);
        }
        if ((continuation == 1 && codePoint < 0x80U) ||
            (continuation == 2 && codePoint < 0x800U) ||
            (continuation == 3 && codePoint < 0x10000U) ||
            codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU) ||
            codePoint < 0x20U ||
            (codePoint >= 0x7FU && codePoint <= 0x9FU) ||
            codePoint == 0x00ADU ||
            codePoint == 0x061CU ||
            codePoint == 0x180EU ||
            (codePoint >= 0x200BU && codePoint <= 0x200FU) ||
            (codePoint >= 0x2028U && codePoint <= 0x202EU) ||
            (codePoint >= 0x2060U && codePoint <= 0x206FU) ||
            (codePoint >= 0xE000U && codePoint <= 0xF8FFU) ||
            codePoint == 0xFEFFU ||
            (codePoint >= 0xFFF9U && codePoint <= 0xFFFBU) ||
            (codePoint >= 0xF0000U && codePoint <= 0xFFFFDU) ||
            (codePoint >= 0x100000U && codePoint <= 0x10FFFDU)) {
            return false;
        }
        cursor += continuation + 1;
        utf16CodeUnits += codePoint > 0xFFFFU ? 2U : 1U;
        if (utf16CodeUnits > maximumUtf16CodeUnits) {
            return false;
        }
    }
    return utf16CodeUnits != 0;
}

bool IsValidPlayerName(const std::string& value) noexcept {
    return value.size() <= 96 &&
        IsValidUtf8Text(value, 24) &&
        !std::isspace(static_cast<unsigned char>(value.front())) &&
        !std::isspace(static_cast<unsigned char>(value.back()));
}

bool IsNonEmptyGuid(const std::string& value) noexcept {
    if (value.size() != 36 ||
        value[8] != '-' ||
        value[13] != '-' ||
        value[18] != '-' ||
        value[23] != '-') {
        return false;
    }
    bool nonZero = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const auto character = static_cast<unsigned char>(value[index]);
        if (!std::isxdigit(character)) {
            return false;
        }
        nonZero = nonZero || character != '0';
    }
    return nonZero;
}

std::string HexNonce(const std::array<std::uint8_t, 16>& nonce) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(nonce.size() * 2, '0');
    for (std::size_t index = 0; index < nonce.size(); ++index) {
        output[index * 2] = kHex[(nonce[index] >> 4U) & 0x0FU];
        output[index * 2 + 1] = kHex[nonce[index] & 0x0FU];
    }
    return output;
}

} // namespace

namespace ipc_validation {

bool DecodePeerJoin(
    const std::span<const std::byte> payload,
    PeerJoinControl& output) noexcept {
    try {
        output = {};
        if (payload.empty() || payload.size() > 4096) {
            return false;
        }
        JsonObject object;
        ClosedJsonParser parser(payload);
        if (!parser.Parse(object) || object.size() != 5) {
            return false;
        }

        const auto peerId = object.find("peerId");
        const auto launcherPlayerId = object.find("launcherPlayerId");
        const auto displayName = object.find("displayName");
        const auto isHost = object.find("isHost");
        const auto ready = object.find("ready");
        if (peerId == object.end() ||
            launcherPlayerId == object.end() ||
            displayName == object.end() ||
            isHost == object.end() ||
            ready == object.end() ||
            !std::holds_alternative<std::uint64_t>(peerId->second) ||
            !std::holds_alternative<std::string>(launcherPlayerId->second) ||
            !std::holds_alternative<std::string>(displayName->second) ||
            !std::holds_alternative<bool>(isHost->second) ||
            !std::holds_alternative<bool>(ready->second)) {
            return false;
        }

        const auto peer = std::get<std::uint64_t>(peerId->second);
        const auto& launcherId =
            std::get<std::string>(launcherPlayerId->second);
        const auto& display = std::get<std::string>(displayName->second);
        if (peer == 0 ||
            !IsNonEmptyGuid(launcherId) ||
            !IsValidPlayerName(display)) {
            return false;
        }

        output.peerId = peer;
        output.launcherPlayerId = launcherId;
        output.displayName = display;
        output.isHost = std::get<bool>(isHost->second);
        output.ready = std::get<bool>(ready->second);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool DecodePeerLeave(
    const std::span<const std::byte> payload,
    std::uint64_t& peerId) noexcept {
    try {
        peerId = 0;
        if (payload.empty() || payload.size() > 1024) {
            return false;
        }
        JsonObject object;
        ClosedJsonParser parser(payload);
        if (!parser.Parse(object) || object.size() != 1) {
            return false;
        }
        const auto peer = object.find("peerId");
        if (peer == object.end() ||
            !std::holds_alternative<std::uint64_t>(peer->second)) {
            return false;
        }
        peerId = std::get<std::uint64_t>(peer->second);
        return peerId != 0;
    } catch (...) {
        peerId = 0;
        return false;
    }
}

} // namespace ipc_validation

LauncherIPC::LauncherIPC(
    Diagnostics& diagnostics,
    InboundQueue& inboundQueue) noexcept
    : diagnostics_(&diagnostics), inboundQueue_(&inboundQueue) {}

LauncherIPC::~LauncherIPC() {
    Stop();
}

bool LauncherIPC::Start(
    const std::uint32_t processId,
    const bool buildValidated,
    const std::array<std::uint8_t, 32>& executableHash,
    std::string profileId,
    std::string resolutionMode) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }

    processId_ = processId;
    buildValidated_ = buildValidated;
    executableHash_ = executableHash;
    profileId_ = std::move(profileId);
    resolutionMode_ = std::move(resolutionMode);
    const auto safeIdentifier = [](const std::string& value) {
        return !value.empty() &&
            value.size() <= 128 &&
            std::all_of(
                value.begin(),
                value.end(),
                [](const unsigned char character) {
                    return std::isalnum(character) ||
                        character == '-' ||
                        character == '_' ||
                        character == '.';
                });
    };
    if (!safeIdentifier(profileId_)) {
        profileId_ = "unresolved";
    }
    if (!experimental::IsResolutionMode(resolutionMode_)) {
        resolutionMode_ = "GenericDiscovery";
    }
    std::array<std::uint8_t, 16> randomNonce{};
    if (BCryptGenRandom(
            nullptr,
            randomNonce.data(),
            static_cast<ULONG>(randomNonce.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    nonce_ = HexNonce(randomNonce);
    shutdownRequested_.store(false, std::memory_order_release);
    helloAcknowledged_.store(false, std::memory_order_release);
    sessionConfigured_.store(false, std::memory_order_release);
    netzReadySent_.store(false, std::memory_order_release);
    experimentalNegotiated_.store(false, std::memory_order_release);
    lastPublishedGeneration_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedCaptureCount_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedCaptureGeneration_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedStability_.store(0xFFU, std::memory_order_release);
    lastPublishedGameThreadValidated_.store(
        0xFFU,
        std::memory_order_release);
    lastPublishedGameThreadCandidateId_.store(
        (std::numeric_limits<std::uint32_t>::max)(),
        std::memory_order_release);
    lastPublishedGameThreadSourceCount_.store(
        (std::numeric_limits<std::uint32_t>::max)(),
        std::memory_order_release);
    lastPublishedScheduleState_.store(0xFFU, std::memory_order_release);
    lastPublishedPendingCommandCount_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedSpawnTrackerVersion_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedPlayerSyncRevision_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    localPeerId_ = 0;
    worldSyncAuthoritySessionId_.clear();
    worldSyncAuthorityGeneration_ = 0U;
    worldSyncAuthorityBound_ = false;
    worldSyncAuthorityHost_ = false;
    worldSyncWorldBusActive_ = false;
    world_actions::ClearSessionAuthority();
    localLauncherPlayerId_.clear();
    {
        std::lock_guard lock(experimentalStateMutex_);
        negotiatedDummyId_.clear();
        negotiationRequestId_.clear();
    }
    remotePeers_.clear();
    ResetExperimentalRequests();
    pipeName_ = L"\\\\.\\pipe\\AIMP.NetZ." + std::to_wstring(processId_);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    try {
        thread_ = std::thread(&LauncherIPC::Run, this);
    } catch (...) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void LauncherIPC::AttachExperimentalServices(
    CapabilityMatrix& capabilities,
    CaptureCatalog& captures,
    LevelGenerationTracker& generations,
    GameThreadDispatcher& dispatcher,
    DummyPlayerController& dummy,
    IDummyNativeExecutor& executor) noexcept {
    capabilities_ = &capabilities;
    captures_ = &captures;
    generations_ = &generations;
    dispatcher_ = &dispatcher;
    dummy_ = &dummy;
    dummyExecutor_ = &executor;
}

void LauncherIPC::AttachDiscovery(NetZDiscovery& discovery) noexcept {
    discovery_ = &discovery;
}

void LauncherIPC::AttachGameCommandServices(
    IGameCommandExecutor& commands) noexcept {
    gameCommands_ = &commands;
}

void LauncherIPC::AttachSpawnCharacterTracker(
    SpawnCharacterTracker& tracker) noexcept {
    spawnTracker_ = &tracker;
}

void LauncherIPC::AttachPlayerSyncObserver(
    PlayerSyncObserver& observer) noexcept {
    playerSyncObserver_ = &observer;
}

void LauncherIPC::PublishAISyncTransactionTraffic() {
    if (!sessionConfigured_.load(std::memory_order_acquire) ||
        !worldSyncAuthorityBound_ || !worldSyncAuthorityHost_ || !worldSyncWorldBusActive_) return;
    ai_sync::transaction_capture::TransportEnvelope bytes{};
    for (std::size_t i = 0; i < 32U && ai_sync::transaction_capture::PeekTransport(bytes); ++i) {
        // Queue keeps all bytes and the original local cookie/epochs. Launcher
        // binds the outer full session identity and rejects stale cookie/world
        // values. No native object is read or executed on this worker.
        if (!Queue(protocol::MessageType::AISyncTransactionObservation, 0U,
                localSequence_.fetch_add(1U, std::memory_order_relaxed), std::as_bytes(std::span(bytes)))) break;
        ai_sync::transaction_capture::CommitTransport();
    }
}
void LauncherIPC::PublishAISyncExecutionTraffic() {
    if (!sessionConfigured_.load(std::memory_order_acquire) || !worldSyncAuthorityBound_ ||
        worldSyncAuthorityHost_ || !worldSyncWorldBusActive_) return;
    ai_sync::execution::MailboxPacket packet{};
    for (std::size_t i = 0; i < 32 && ai_sync::execution_runtime::Peek(packet); ++i) {
        ai_sync::transactions::SessionId session{};
        std::copy_n(packet.bytes.begin() + 16, session.size(), session.begin());
        std::uint64_t world{};
        for (std::size_t n = 0; n < 8; ++n) world |= static_cast<std::uint64_t>(packet.bytes[40 + n]) << (8U * n);
        if (!ai_sync::execution::SessionTextMatches(session, worldSyncAuthoritySessionId_) || world != worldSyncAuthorityGeneration_) {
            // WorldSyncLifecycle already requested local retirement. Drop
            // only this copied old-scope disposition so it cannot obstruct
            // the new binding's Ready response.
            if (!ai_sync::execution_runtime::Commit(packet.token)) break;
            continue;
        }
        if (!Queue(packet.receipt ? protocol::MessageType::AISyncExecutionReceipt : protocol::MessageType::AISyncExecutionBinding,
            0, localSequence_.fetch_add(1, std::memory_order_relaxed), std::as_bytes(std::span(packet.bytes).first(packet.size)))) break;
        static_cast<void>(ai_sync::execution_runtime::Commit(packet.token));
    }
}

void LauncherIPC::PublishAISyncHostTraffic() {
    if (!sessionConfigured_.load(std::memory_order_acquire) || !worldSyncAuthorityBound_ ||
        !worldSyncAuthorityHost_ || !worldSyncWorldBusActive_) return;
    ai_sync::host_producer::HostPacket packet{};
    for (std::size_t i = 0; i < 32 && ai_sync::host_producer::PeekHost(packet); ++i) {
        // A packet carries its original full binding. The IPC thread never
        // reads Characters or relabels output from a retired Host incarnation.
        if (packet.size < (packet.route ? ai_sync::route::kHeaderBytes : ai_sync::execution::kHeaderSize) || packet.size > packet.bytes.size()) {
            ai_sync::host_producer::RetireHost(); break;
        }
        ai_sync::transactions::SessionId session{};
        std::copy_n(packet.bytes.begin() + (packet.route ? 32 : 16), session.size(), session.begin());
        std::uint64_t world{};
        for (std::size_t n = 0; n < 8; ++n) world |= static_cast<std::uint64_t>(packet.bytes[(packet.route ? 56 : 40) + n]) << (8U * n);
        if (!ai_sync::execution::SessionTextMatches(session, worldSyncAuthoritySessionId_) || world != worldSyncAuthorityGeneration_) {
            // The lifecycle boundary already retired the old source. Its
            // retained bytes cannot be rebound and must not block a new Offer.
            if (!ai_sync::host_producer::CommitHost(packet.token)) break;
            continue;
        }
        if (!Queue(packet.state ? protocol::MessageType::AISyncHostState : packet.route ? protocol::MessageType::AISyncHostRoute :
            (packet.control ? protocol::MessageType::AISyncHostControl : protocol::MessageType::AISyncHostBatch),
            0, localSequence_.fetch_add(1, std::memory_order_relaxed), std::as_bytes(std::span(packet.bytes).first(packet.size)))) break;
        if (!ai_sync::host_producer::CommitHost(packet.token)) break;
    }
}

void LauncherIPC::PublishAlienPvPTraffic() {
    if (!sessionConfigured_.load(std::memory_order_acquire) ||
        !worldSyncAuthorityBound_ || !worldSyncWorldBusActive_) return;
    std::array<std::uint8_t,alien_pvp::kWireBytes> bytes{};
    for(std::size_t i=0;i<32U && alien_pvp::TryPopObservation(bytes);++i) {
        if(!Queue(protocol::MessageType::AlienPvPNative,0U,
            localSequence_.fetch_add(1U,std::memory_order_relaxed),std::as_bytes(std::span(bytes)))) break;
    }
}

void LauncherIPC::PublishAISyncTraffic() {
    ai_sync::ImpactEvent impact{};
    for (std::size_t i = 0U; i < 16U && ai_sync::TryPopObservedImpact(impact); ++i) {
        if (!sessionConfigured_.load(std::memory_order_acquire) ||
            !worldSyncAuthorityBound_ || worldSyncAuthorityHost_ || !worldSyncWorldBusActive_) continue;
        if (!Queue(protocol::MessageType::AISyncImpact, 0U,
                localSequence_.fetch_add(1U, std::memory_order_relaxed),
                std::as_bytes(std::span(&impact, 1U)))) break;
    }
}

void LauncherIPC::PublishPlayerLifecycleTraffic() {
    const auto sequence = player_lifecycle::LocalDeathSequence();
    const auto levelExit = player_lifecycle::LocalLevelExitSequence();
    if (!sessionConfigured_.load(std::memory_order_acquire) ||
        !netzReadySent_.load(std::memory_order_acquire)) {
        // A death or level exit outside an active session is never a round
        // event; do not replay it to a Launcher that connects later.
        publishedDeathSequence_ = sequence;
        publishedLevelExitSequence_ = levelExit;
        publishedSpectatePeer_ = 0U;
        publishedSpectateCandidates_ = 0U;
        return;
    }
    // Spectating only follows a suppressed death, so a state carrying it
    // always has the death edge the Launcher requires.
    const auto spectate = sequence != 0U ? player_lifecycle::SpectateSnapshot() : player_lifecycle::SpectateState{};
    if (sequence == publishedDeathSequence_ && levelExit == publishedLevelExitSequence_ &&
        spectate.targetPeer == publishedSpectatePeer_ && spectate.candidates == publishedSpectateCandidates_) return;

    // Retried every worker tick until queued: the Host needs these exact edges
    // to decide that every participant of the round is dead or finished.
    std::ostringstream json;
    json << "{\"schemaVersion\":3"
         << ",\"deathSequence\":" << sequence
         << ",\"localPlayerDead\":" << (sequence != 0U ? "true" : "false")
         << ",\"levelExitSequence\":" << levelExit
         << ",\"spectatePeer\":" << spectate.targetPeer
         << ",\"spectateCandidates\":" << spectate.candidates
         << '}';
    if (Queue(
            protocol::MessageType::PlayerLifecycleState,
            0,
            localSequence_.fetch_add(1U, std::memory_order_relaxed),
            JsonBytes(json.str()))) {
        publishedDeathSequence_ = sequence;
        publishedLevelExitSequence_ = levelExit;
        publishedSpectatePeer_ = spectate.targetPeer;
        publishedSpectateCandidates_ = spectate.candidates;
    }
}

void LauncherIPC::PublishCharacterIncarnationTraffic() {
    if(!sessionConfigured_.load(std::memory_order_acquire)||!netzReadySent_.load(std::memory_order_acquire)) {
        characterIncarnationPublished_=false;return;
    }
    const auto ticket=character_incarnations::NativeRegistry().Local();
    std::array<std::uint8_t,character_incarnations::kLocalStateBytes> bytes{};
    if(!character_incarnations::EncodeLocalState(ticket,bytes)||
        (characterIncarnationPublished_&&bytes==publishedCharacterIncarnation_))return;
    if(Queue(protocol::MessageType::CharacterIncarnationState,0,localSequence_.fetch_add(1U,std::memory_order_relaxed),std::as_bytes(std::span(bytes)))) {
        publishedCharacterIncarnation_=bytes;characterIncarnationPublished_=true;
    }
}

void LauncherIPC::PublishPlayerHidingTraffic() {
    if (!sessionConfigured_.load(std::memory_order_acquire) || !netzReadySent_.load(std::memory_order_acquire)) {
        publishedHiding_.clear(); lastHidingPublication_ = 0; return;
    }
    const auto now = GetTickCount64();
    if (now - lastHidingPublication_ < 16U) return;
    const auto capture = player_hiding_local::Read();
    if (!capture.valid || now < capture.capturedAt || now - capture.capturedAt > 500U ||
        !character_incarnations::NativeRegistry().Current(capture.ticket)) return;
    const auto& state = capture.state;
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<float>::max_digits10);
    if (state.phase == 0) text << '0';
    else text << state.phase << ',' << state.kind << ',' << state.entityShortGuid << ',' << state.hierarchyHash
        << ',' << state.transition << ',' << state.elapsedMilliseconds << ',' << state.position[0]
        << ',' << state.position[1] << ',' << state.position[2] << ',' << state.yaw << ",0";
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"nativeGeneration\":" << capture.ticket.generation
        << ",\"character\":" << capture.ticket.character << ",\"serial\":" << capture.ticket.serial
        << ",\"presentation\":\"" << text.str() << "\"}";
    const auto payload = json.str();
    if (payload == publishedHiding_ && now - lastHidingPublication_ < 250U) return;
    if (Queue(protocol::MessageType::PlayerHidingState, 0,
            localSequence_.fetch_add(1U, std::memory_order_relaxed), JsonBytes(payload))) {
        publishedHiding_ = payload; lastHidingPublication_ = now;
    }
}

// AnimationSync: the local THE_PLAYER's current scripted clip ("0" = none).
// Same cadence and identity guards as the hiding observation.
void LauncherIPC::PublishPlayerAnimationTraffic() {
    if (!sessionConfigured_.load(std::memory_order_acquire) || !netzReadySent_.load(std::memory_order_acquire)) {
        publishedAnimation_.clear(); lastAnimationPublication_ = 0; return;
    }
    const auto now = GetTickCount64();
    if (now - lastAnimationPublication_ < 16U) return;
    const auto capture = animation_sync::ReadLocal();
    if (!capture.valid || now < capture.capturedAt || now - capture.capturedAt > 500U ||
        !character_incarnations::NativeRegistry().Current(capture.ticket)) return;
    // Identity without the advancing clock: a new clip or a clear is sent at
    // once, an unchanged clip only refreshes its elapsed time every 250 ms, or
    // every 50 ms while it carries the owner's moving body effectors.
    std::ostringstream key;
    key << capture.ticket.generation << ':' << capture.ticket.serial << ':'
        << capture.state.transition << ':' << capture.state.animation;
    for (const auto attachment : capture.state.attachments) key << ':' << attachment;
    if (key.str() == publishedAnimation_ &&
        now - lastAnimationPublication_ < (capture.state.posed ? 50U : 250U)) return;
    auto state = capture.state;
    if (animation_sync::Active(state)) state.elapsedMilliseconds = static_cast<std::uint32_t>(now - capture.capturedAt);
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"nativeGeneration\":" << capture.ticket.generation
        << ",\"character\":" << capture.ticket.character << ",\"serial\":" << capture.ticket.serial
        << ",\"sampledAtTickMilliseconds\":" << now
        << ",\"animation\":\"" << animation_sync::Encode(state) << "\"}";
    const auto payload = json.str();
    if (Queue(protocol::MessageType::PlayerAnimationState, 0,
            localSequence_.fetch_add(1U, std::memory_order_relaxed), JsonBytes(payload))) {
        publishedAnimation_ = key.str(); lastAnimationPublication_ = now;
    }
}

void LauncherIPC::PublishMissionSyncTraffic() {
    mission_sync::Signal signal{};
    for (std::size_t i = 0U; i < 16U && mission_sync::TryPeek(signal); ++i) {
        if (!sessionConfigured_.load(std::memory_order_acquire)) break;
        std::ostringstream json;
        json << "{\"levelGeneration\":" << signal.generation
             << ",\"sequence\":" << signal.sequence
             << ",\"entityId\":\"" << item_sync::EntityIdHex(signal.entity) << '"'
             << ",\"signal\":" << signal.port
             << ",\"interfaceSelector\":" << static_cast<unsigned>(signal.selector)
             << ",\"occurrence\":" << signal.occurrence << '}';
        if (!Queue(protocol::MessageType::MissionSignalProduced, 0,
                localSequence_.fetch_add(1U, std::memory_order_relaxed), JsonBytes(json.str()))) break;
        mission_sync::Pop();
    }
}

void LauncherIPC::PublishItemSyncTraffic() {
    item_sync::PickupEvent pickup{};
    for (std::size_t i = 0U; i < 16U && item_sync::TryPeekObservedPickup(pickup); ++i) {
        // A pickup outside a bound session is a solo event: drop it.
        if (!sessionConfigured_.load(std::memory_order_acquire) ||
            !netzReadySent_.load(std::memory_order_acquire) ||
            !worldSyncAuthorityBound_) {
            item_sync::PopObservedPickup();
            continue;
        }
        std::ostringstream json;
        json << "{\"schemaVersion\":1"
             << ",\"sequence\":" << pickup.sequence
             << ",\"entityId\":\"" << item_sync::EntityIdHex(pickup.entity) << '"'
             << '}';
        // Kept at the head of the ring until queued: the Host must see every
        // pickup to remove it for the other participants.
        if (!Queue(
                protocol::MessageType::ItemSyncPickup,
                0,
                localSequence_.fetch_add(1U, std::memory_order_relaxed),
                JsonBytes(json.str()))) break;
        item_sync::PopObservedPickup();
    }
}

void LauncherIPC::PublishItemSyncSpawnTraffic() {
    item_sync::SpawnEvent event{};
    for (std::size_t i = 0U; i < 16U && item_sync::TryPeekObservedSpawn(event); ++i) {
        if (!worldSyncAuthorityBound_) {
            item_sync::PopObservedSpawn();
            continue;
        }
        // Owner spawns run while the level loads: keep them until the
        // session is ready instead of losing the Host decision request.
        if (!sessionConfigured_.load(std::memory_order_acquire) ||
            !netzReadySent_.load(std::memory_order_acquire)) break;
        const auto& data = event.data;
        // Item paths are printable ASCII ("Pickups\..."): escape the JSON
        // specials and drop anything else rather than emit invalid JSON.
        std::string path;
        for (const char* cursor = data.spawn ? data.path : ""; *cursor != '\0'; ++cursor) {
            const auto character = static_cast<unsigned char>(*cursor);
            if (character < 0x20U || character > 0x7EU) continue;
            if (character == '"' || character == '\\') path += '\\';
            path += static_cast<char>(character);
        }
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << std::setprecision(9);
        char ownerKey[17]{};
        std::snprintf(ownerKey, sizeof(ownerKey), "%016llX",
            static_cast<unsigned long long>(data.ownerKey));
        json << "{\"schemaVersion\":1"
             << ",\"sequence\":" << event.sequence
             << ",\"kind\":\"" << (data.kind == item_sync::SpawnKind::Drop ? "drop" : "owner") << '"'
             << ",\"ownerKey\":\"" << ownerKey << '"'
             << ",\"spawn\":" << (data.spawn ? "true" : "false")
             << ",\"path\":\"" << path << '"'
             << ",\"quantity\":" << (data.spawn ? data.quantity : 0)
             << ",\"transform\":[";
        for (std::size_t index = 0U; index < 12U; ++index)
            json << (index == 0U ? "" : ",") << (data.spawn ? data.transform[index] : 0.0F);
        json << "]}";
        if (!Queue(
                protocol::MessageType::ItemSyncSpawn,
                0,
                localSequence_.fetch_add(1U, std::memory_order_relaxed),
                JsonBytes(json.str()))) break;
        item_sync::PopObservedSpawn();
    }
}

void LauncherIPC::PublishThrowableTraffic() {
    throwable_sync::Event event{};
    std::uint64_t ticket{};
    for (std::size_t count = 0U; count < 32U && throwable_sync::TryPeekObserved(event, ticket); ++count) {
        if (!worldSyncAuthorityBound_ || !worldSyncWorldBusActive_) break;
        const auto bytes = std::as_bytes(std::span{&event, 1U});
        if (!Queue(protocol::MessageType::ThrowableEvent, 0,
            localSequence_.fetch_add(1U, std::memory_order_relaxed),
            std::vector<std::byte>(bytes.begin(), bytes.end()))) break;
        throwable_sync::PopObserved(ticket);
    }
}

void LauncherIPC::PublishPlayerSyncTraffic() {
    if (playerSyncObserver_ == nullptr) {
        return;
    }

    if (!sessionConfigured_.load(std::memory_order_acquire) ||
        !netzReadySent_.load(std::memory_order_acquire)) {
        // Edge events only have meaning inside the active multiplayer session.
        // Drop stale offline edges while retaining the durable snapshot, which
        // will converge the current Aim/Primary state when a session starts.
        PlayerSyncActionEvent ignored{};
        while (playerSyncObserver_->TryPopAction(ignored)) {}
        return;
    }

    PlayerEquipmentSnapshot snapshot{};
    if (!playerSyncObserver_->TrySnapshot(snapshot) || snapshot.revision == 0U) {
        return;
    }

    const auto previous = lastPublishedPlayerSyncRevision_.load(
        std::memory_order_acquire);
    if (previous != snapshot.revision) {
        std::ostringstream json;
        json << "{\"schemaVersion\":2"
             << ",\"revision\":" << snapshot.revision
             << ",\"valid\":" << (snapshot.valid ? "true" : "false")
             << ",\"currentEquipmentIndex\":" << snapshot.currentEquipmentIndex
             << ",\"previousEquipmentIndex\":" << snapshot.previousEquipmentIndex
             << ",\"equipmentActive\":" << (snapshot.equipmentActive ? "true" : "false")
             << ",\"presentationState\":" << snapshot.presentationState
             << ",\"aimHeld\":" << (snapshot.aimHeld ? "true" : "false")
             << ",\"primaryHeld\":" << (snapshot.primaryHeld ? "true" : "false")
             << '}';
        const auto payload = JsonBytes(json.str());
        if (Queue(
                protocol::MessageType::PlayerSyncState,
                0,
                localSequence_.fetch_add(1U, std::memory_order_relaxed),
                payload)) {
            lastPublishedPlayerSyncRevision_.store(
                snapshot.revision,
                std::memory_order_release);
        }
    }

    PlayerSyncActionEvent action{};
    for (std::size_t count = 0U; count < 32U &&
         playerSyncObserver_->TryPopAction(action); ++count) {
        std::ostringstream actionJson;
        actionJson << "{\"schemaVersion\":1"
                   << ",\"eventSequence\":" << action.sequence
                   << ",\"action\":" << static_cast<unsigned>(action.action)
                   << ",\"active\":" << (action.active ? "true" : "false")
                   << ",\"currentEquipmentIndex\":" << action.currentEquipmentIndex
                   << ",\"presentationState\":" << action.presentationState
                   << '}';
        if (!Queue(
                protocol::MessageType::PlayerSyncAction,
                0,
                localSequence_.fetch_add(1U, std::memory_order_relaxed),
                JsonBytes(actionJson.str()))) {
            break;
        }
    }
}

void LauncherIPC::PublishWorldReplicationTraffic() {
    if (!sessionConfigured_.load(std::memory_order_acquire) ||
        !netzReadySent_.load(std::memory_order_acquire)) {
        return;
    }

    // WorldSync gameplay traffic must not wait for the 250 ms bootstrap /
    // diagnostics cadence. The IPC worker is the sole consumer of these two
    // bounded queues so clientSequence order is preserved exactly. Drain a
    // bounded batch each worker pass; the worker writes the whole outgoing
    // batch before sleeping again.
    WorldIntent intent{};
    for (std::size_t count = 0U; count < 128U &&
         replication_core::TryPopOutboundIntent(intent); ++count) {
        workerTrafficObserved_ = true;
        std::vector<std::byte> payload;
        if (!world_replication_codec::EncodeIntent(intent, payload) ||
            !Queue(
                protocol::MessageType::WorldIntent,
                0,
                localSequence_.fetch_add(1U, std::memory_order_relaxed),
                payload)) {
            // The ROOT was already published to the native outbound ring but
            // never entered the Launcher pipe. AwaitAuthority cannot be replayed
            // asynchronously, so make the loss an explicit recovery-required
            // state instead of silently discarding a taken-over World decision.
            replication_core::MarkOutboundIntentTransportFailure(intent);
            static_cast<void>(world_interactions::DiscardPublicRootIntent(intent));
            diagnostics_->Log(
                "warning",
                "world_intent_ipc_queue_rejected",
                "A bounded WorldIntent could not be queued to the launcher.");
            break;
        }
    }

    WorldMutationResult mutationResult{};
    for (std::size_t count = 0U; count < 128U &&
         replication_core::TryPopMutationResult(mutationResult); ++count) {
        workerTrafficObserved_ = true;
        std::vector<std::byte> payload;
        if (mutationResult.nativeDispatchId == 0U ||
            !world_replication_codec::EncodeResult(mutationResult, payload) ||
            !Queue(
                protocol::MessageType::WorldMutationResult,
                0,
                mutationResult.nativeDispatchId,
                payload)) {
            diagnostics_->Log(
                "warning",
                "world_result_ipc_queue_rejected",
                "A WorldMutation result could not be queued to the launcher.");
            break;
        }
    }
}

void LauncherIPC::PublishRuntimeState() {

    if (!sessionConfigured_.load(std::memory_order_acquire) ||
        !netzReadySent_.load(std::memory_order_acquire) ||
        capabilities_ == nullptr ||
        generations_ == nullptr ||
        captures_ == nullptr ||
        dispatcher_ == nullptr) {
        return;
    }
    experimental::RequestEnvelope request;
    {
        std::lock_guard lock(experimentalStateMutex_);
        request.requestId = negotiationRequestId_;
        request.portableDummyId = negotiatedDummyId_;
    }
    if (request.requestId.empty() ||
        request.portableDummyId.empty()) {
        return;
    }

    const auto level = generations_->Snapshot();
    SpawnCharacterTrackingSnapshot spawnTracking{};
    if (spawnTracker_ != nullptr) {
        spawnTracker_->ResetGeneration(level.generation);
        spawnTracking = spawnTracker_->Snapshot();
    }
    request.levelGeneration = static_cast<std::uint32_t>(
        (std::min<std::uint64_t>)(
            level.generation,
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::int32_t>::max)())));
    const auto engineThread = dispatcher_->Snapshot();
    const auto schedule =
        capabilities_->Get(Capability::ScheduleGameThreadWork);
    const auto scheduleState = static_cast<std::uint8_t>(schedule.state);
    if (lastPublishedScheduleState_.load(std::memory_order_acquire) !=
        scheduleState) {
        const auto capabilitySnapshot = capabilities_->Snapshot();
        std::vector<CapabilityRecord> records(
            capabilitySnapshot.begin(),
            capabilitySnapshot.end());
        std::vector<std::string> blocked;
        for (const auto& record : records) {
            if (record.state == CapabilityState::Blocked ||
                record.state == CapabilityState::Unresolved ||
                record.state == CapabilityState::Disabled) {
                blocked.emplace_back(ToString(record.capability));
            }
        }
        const auto nextAction = engineThread.ready
            ? "Engine thread ready."
            : ((engineThread.mainThreadHookInstalled ||
                engineThread.managerUpdateHookInstalled ||
                engineThread.readinessProducerHookInstalled)
                   ? "Waiting for the first engine callback."
                   : "Engine-thread hooks are unavailable.");
        const auto payload = experimental::EncodeCapabilityState(
            request,
            profileId_,
            resolutionMode_,
            buildValidated_,
            engineThread.ready,
            records,
            blocked,
            nextAction);
        if (!QueueExperimental(
                protocol::MessageType::CapabilityState,
                payload)) {
            return;
        }
        lastPublishedScheduleState_.store(
            scheduleState,
            std::memory_order_release);
    }
    const auto previousGeneration =
        lastPublishedGeneration_.load(std::memory_order_acquire);
    const auto previousStability =
        lastPublishedStability_.load(std::memory_order_acquire);
    const auto selectedLevelName = level.stability == LevelStability::Stable
        ? mission_level_observation::ReadSelectedLevelName(
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))) : std::string{};
    const bool levelStateChanged = lastPublishedLevelName_ != selectedLevelName ||
        previousGeneration != level.generation ||
        previousStability !=
            static_cast<std::uint8_t>(level.stability);
    if (levelStateChanged) {
        const auto payload = experimental::EncodeLevelState(
            request,
            selectedLevelName,
            level.stability != LevelStability::Stable,
            engineThread.ready);
        if (!QueueExperimental(
                protocol::MessageType::LevelState,
                payload)) {
            return;
        }
        lastPublishedGeneration_.store(
            level.generation,
            std::memory_order_release);
        lastPublishedLevelName_ = selectedLevelName;
        lastPublishedStability_.store(
            static_cast<std::uint8_t>(level.stability),
            std::memory_order_release);
    }

    const auto summary = captures_->Summary();
    const auto previousCaptures =
        lastPublishedCaptureCount_.load(std::memory_order_acquire);
    const auto previousCaptureGeneration =
        lastPublishedCaptureGeneration_.load(std::memory_order_acquire);
    const auto gameThreadValidated = engineThread.ready;
    const auto gameThreadCandidateId = engineThread.engineThreadId;
    const auto gameThreadSourceCount = engineThread.ready ? 1U : 0U;
    const auto previousGameThreadValidated =
        lastPublishedGameThreadValidated_.load(std::memory_order_acquire);
    const auto previousGameThreadCandidateId =
        lastPublishedGameThreadCandidateId_.load(
            std::memory_order_acquire);
    const auto previousGameThreadSourceCount =
        lastPublishedGameThreadSourceCount_.load(
            std::memory_order_acquire);
    const auto previousPendingCommands =
        lastPublishedPendingCommandCount_.load(std::memory_order_acquire);
    const auto previousSpawnTrackerVersion =
        lastPublishedSpawnTrackerVersion_.load(std::memory_order_acquire);
    const bool spawnTrackerChanged =
        previousSpawnTrackerVersion != spawnTracking.version ||
        lastPublishedSpawnCreationSerial_.load(std::memory_order_acquire) != spawnTracking.creation.serial;
    const bool gameThreadStateChanged =
        previousGameThreadValidated !=
            static_cast<std::uint8_t>(gameThreadValidated ? 1U : 0U) ||
        previousGameThreadCandidateId != gameThreadCandidateId ||
        previousGameThreadSourceCount != gameThreadSourceCount ||
        previousPendingCommands != engineThread.pendingCount;
    if (previousCaptures == summary.accepted &&
        previousCaptureGeneration == level.generation &&
        !levelStateChanged &&
        !gameThreadStateChanged &&
        !spawnTrackerChanged) {
        return;
    }
    const auto runtime = discovery_ != nullptr
        ? discovery_->RuntimeSnapshot()
        : RuntimeObservationSnapshot{};
    const auto payload = experimental::EncodeCaptureState(
        request,
        summary.hasF0,
        summary.has130,
        runtime.participantRegistryAvailable,
        runtime.participantCount,
        runtime.contextPool != 0,
        runtime.channelSnapshotAvailable &&
            runtime.freeChannelMask != 0,
        gameThreadValidated,
        (std::min)(summary.currentCaptureCount, 256U),
        spawnTrackerChanged
            ? "remote_character_state_changed"
            : (gameThreadStateChanged
                   ? (gameThreadValidated
                          ? "engine_thread_ready"
                          : "waiting_for_engine_thread")
                   : (previousGeneration != level.generation
                          ? "level_generation_changed"
                          : (previousStability !=
                                     static_cast<std::uint8_t>(level.stability)
                                 ? "level_stability_changed"
                                 : "character_request_captured"))),
        gameThreadCandidateId,
        gameThreadSourceCount,
        ToString(engineThread.source),
        engineThread.firstCallbackPerformanceCounter,
        engineThread.lastCallbackPerformanceCounter,
        engineThread.pendingCount,
        engineThread.mainThreadHookInstalled,
        engineThread.managerUpdateHookInstalled,
        engineThread.readinessProducerHookInstalled,
        ToString(spawnTracking.state),
        spawnTracking.spawnKey,
        spawnTracking.characterAddress,
        spawnTracking.recordAddress,
        spawnTracking.hookInstalled, spawnTracking.creation.serial, spawnTracking.creation.runtimeId);
    if (QueueExperimental(protocol::MessageType::CaptureState, payload)) {
        lastPublishedCaptureCount_.store(
            summary.accepted,
            std::memory_order_release);
        lastPublishedCaptureGeneration_.store(
            level.generation,
            std::memory_order_release);
        lastPublishedGameThreadValidated_.store(
            static_cast<std::uint8_t>(
                gameThreadValidated ? 1U : 0U),
            std::memory_order_release);
        lastPublishedGameThreadCandidateId_.store(
            gameThreadCandidateId,
            std::memory_order_release);
        lastPublishedGameThreadSourceCount_.store(
            gameThreadSourceCount,
            std::memory_order_release);
        lastPublishedPendingCommandCount_.store(
            engineThread.pendingCount,
            std::memory_order_release);
        lastPublishedSpawnTrackerVersion_.store(
            spawnTracking.version,
            std::memory_order_release);
        lastPublishedSpawnCreationSerial_.store(spawnTracking.creation.serial, std::memory_order_release);
    }
}

void LauncherIPC::Stop() noexcept {
    const bool wasRunning =
        running_.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning &&
        !thread_.joinable() &&
        stopEvent_ == nullptr) {
        return;
    }
    if (stopEvent_ != nullptr) {
        SetEvent(stopEvent_);
    }
    const auto pipe = pipe_.load(std::memory_order_acquire);
    if (pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe, nullptr);
    }
    if (thread_.joinable() &&
        thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    Disconnect("ipc_stopped");
    if (stopEvent_ != nullptr) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    std::lock_guard lock(outgoingMutex_);
    outgoing_.clear();
    outgoingBytes_ = 0;
}

bool LauncherIPC::Queue(
    const protocol::MessageType type,
    const std::uint16_t flags,
    const std::uint64_t sequence,
    const std::span<const std::byte> payload) {
    std::vector<std::byte> encoded;
    if (!protocol::EncodeFrame(type, flags, sequence, payload, encoded)) {
        return false;
    }

    constexpr std::size_t kMaximumQueuedFrames = 64;
    constexpr std::size_t kMaximumQueuedBytes = 4U * 1024U * 1024U;
    if (encoded.size() > kMaximumQueuedBytes) return false;
    for (;;) {
        {
            std::lock_guard lock(outgoingMutex_);
            if (!connected_.load(std::memory_order_acquire)) {
                // A publisher racing Disconnect cannot repopulate the old epoch.
                return false;
            }
            if (outgoing_.size() < kMaximumQueuedFrames &&
                outgoingBytes_ <= kMaximumQueuedBytes - encoded.size()) {
                outgoingBytes_ += encoded.size();
                outgoing_.push_back(std::move(encoded));
                return true;
            }
        }
        // HandleFrame returns Queue's result for valid replies, including the
        // heartbeat. A full queue must not become a protocol-validation failure.
        // Only the pipe worker may drain it; engine-thread publishers stay bounded
        // and never wait on pipe I/O. Preserve FIFO order and the existing limits.
        if (GetCurrentThreadId() != workerThreadId_.load(std::memory_order_acquire) ||
            !FlushOutgoing(kMaximumQueuedFrames)) return false;
    }
}

bool LauncherIPC::IsConnected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

bool LauncherIPC::ShutdownRequested() const noexcept {
    return shutdownRequested_.load(std::memory_order_acquire);
}

std::wstring LauncherIPC::PipeName() const {
    return pipeName_;
}

void LauncherIPC::Run() {
    workerThreadId_.store(GetCurrentThreadId(), std::memory_order_release);
    try {
        diagnostics_->Log(
            "info",
            "ipc_worker_started",
            "Named-pipe client started; it cannot invoke engine callbacks.");

        while (running_.load(std::memory_order_acquire)) {
            workerTrafficObserved_ = false;
            if (!connected_.load(std::memory_order_acquire)) {
                if (!TryConnect()) {
                    WaitStop(500);
                    continue;
                }

                const auto sendControl = [&](const protocol::MessageType type,
                                             const std::vector<std::byte>& payload) {
                    std::vector<std::byte> wire;
                    return protocol::EncodeFrame(
                               type,
                               0,
                               localSequence_.fetch_add(1, std::memory_order_relaxed),
                               payload,
                               wire) &&
                        WriteFrame(wire);
                };
                if (!sendControl(
                        protocol::MessageType::Hello,
                        BuildHelloPayload())) {
                    Disconnect("initial_handshake_write_failed");
                    continue;
                }
            }

            if (!ReceiveAvailable()) {
                continue;
            }

            PublishWorldReplicationTraffic();
            PublishPlayerSyncTraffic();
            PublishPlayerLifecycleTraffic();
            PublishCharacterIncarnationTraffic();
            PublishPlayerHidingTraffic();
            PublishPlayerAnimationTraffic();
            PublishItemSyncTraffic();
            PublishMissionSyncTraffic();
            PublishItemSyncSpawnTraffic();
            PublishThrowableTraffic();
            PublishAlienPvPTraffic();
            PublishAISyncTraffic();
            PublishAISyncTransactionTraffic();
            PublishAISyncExecutionTraffic();
            PublishAISyncHostTraffic();

            if (!FlushOutgoing(256U)) continue;

            // Poll at 1 ms while traffic is flowing, back off to 10 ms when
            // idle. All protocol families still receive a turn on every pass.
            WaitStop(workerTrafficObserved_ ? 1U : 10U);
        }

        Disconnect("ipc_worker_exit");
        diagnostics_->Log(
            "info",
            "ipc_worker_stopped",
            "Named-pipe client stopped.");
    } catch (...) {
        running_.store(false, std::memory_order_release);
        Disconnect("ipc_worker_exception");
        try {
            diagnostics_->Log(
                "error",
                "ipc_worker_fault",
                "Unhandled IPC worker exception was contained; no engine callback was invoked.");
        } catch (...) {
        }
    }
    workerThreadId_.store(0U, std::memory_order_release);
}

bool LauncherIPC::FlushOutgoing(const std::size_t maximumFrames) {
    std::vector<std::byte> outgoing;
    for (std::size_t count = 0U; count < maximumFrames;) {
        std::size_t batchFrames{};
        {
            std::lock_guard lock(outgoingMutex_);
            if (outgoing_.empty()) break;
            batchFrames = TakeIpcWriteBatch(outgoing_, outgoingBytes_, outgoing,
                maximumFrames - count, kMaximumIpcPayload + protocol::kWireHeaderBytes);
        }
        if (batchFrames == 0U) return false;
        if (!WriteFrame(outgoing)) {
            Disconnect("queued_write_failed");
            return false;
        }
        count += batchFrames;
    }
    return true;
}

bool LauncherIPC::TryConnect() {
    HANDLE pipe = CreateFileW(
        pipeName_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        CloseHandle(pipe);
        return false;
    }

    pipe_.store(pipe, std::memory_order_release);
    connected_.store(true, std::memory_order_release);
    characterIncarnationPublished_=false;
    helloAcknowledged_.store(false, std::memory_order_release);
    sessionConfigured_.store(false, std::memory_order_release);
    netzReadySent_.store(false, std::memory_order_release);
    experimentalNegotiated_.store(false, std::memory_order_release);
    localPeerId_ = 0;
    worldSyncAuthoritySessionId_.clear();
    worldSyncAuthorityGeneration_ = 0U;
    worldSyncAuthorityBound_ = false;
    worldSyncAuthorityHost_ = false;
    worldSyncWorldBusActive_ = false;
    world_actions::ClearSessionAuthority();
    localLauncherPlayerId_.clear();
    {
        std::lock_guard lock(experimentalStateMutex_);
        negotiatedDummyId_.clear();
        negotiationRequestId_.clear();
    }
    remotePeers_.clear();
    ResetExperimentalRequests();
    diagnostics_->Log(
        "info",
        "ipc_connected",
        "Connected to the launcher pipe.",
        {{"processId", Diagnostics::Unsigned(processId_), true}});
    return true;
}

void LauncherIPC::Disconnect(const std::string_view reason) noexcept {
    // Worker owns the consumer; Stop also calls this after joining the worker.
    ai_sync::host_producer::RetireHost();
    ai_sync::transaction_capture::DiscardTransport();
    throwable_sync::TransportLost();
    voice_sync::TransportLost();
    multiplayer_pause::ClearLoadingHold();
    ai_sync::TransportLost();
    const auto pipe =
        pipe_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    const bool wasConnected =
        connected_.exchange(false, std::memory_order_acq_rel);
    helloAcknowledged_.store(false, std::memory_order_release);
    sessionConfigured_.store(false, std::memory_order_release);
    netzReadySent_.store(false, std::memory_order_release);
    experimentalNegotiated_.store(false, std::memory_order_release);
    lastPublishedGeneration_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedCaptureCount_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedCaptureGeneration_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedStability_.store(0xFFU, std::memory_order_release);
    lastPublishedGameThreadValidated_.store(
        0xFFU,
        std::memory_order_release);
    lastPublishedGameThreadCandidateId_.store(
        (std::numeric_limits<std::uint32_t>::max)(),
        std::memory_order_release);
    lastPublishedGameThreadSourceCount_.store(
        (std::numeric_limits<std::uint32_t>::max)(),
        std::memory_order_release);
    lastPublishedScheduleState_.store(0xFFU, std::memory_order_release);
    lastPublishedPendingCommandCount_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedSpawnTrackerVersion_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    lastPublishedPlayerSyncRevision_.store(
        (std::numeric_limits<std::uint64_t>::max)(),
        std::memory_order_release);
    localPeerId_ = 0;
    worldSyncAuthoritySessionId_.clear();
    worldSyncAuthorityGeneration_ = 0U;
    worldSyncAuthorityBound_ = false;
    worldSyncAuthorityHost_ = false;
    worldSyncWorldBusActive_ = false;
    world_actions::ClearSessionAuthority();
    localLauncherPlayerId_.clear();
    {
        std::lock_guard lock(experimentalStateMutex_);
        negotiatedDummyId_.clear();
        negotiationRequestId_.clear();
    }
    remotePeers_.clear();
    ResetExperimentalRequests();
    {
        // Frames are scoped to one pipe connection. Retaining a response
        // across reconnect could place it ahead of the new HELLO handshake
        // and could expose state from an obsolete level generation.
        std::lock_guard lock(outgoingMutex_);
        outgoing_.clear();
        outgoingBytes_ = 0;
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe, nullptr);
        CloseHandle(pipe);
    }
    if (wasConnected && diagnostics_ != nullptr) {
        try {
            diagnostics_->Log(
                "warning",
                "ipc_disconnected",
                reason);
        } catch (...) {
        }
    }
}

bool LauncherIPC::HandleAISyncExecutionFrame(const protocol::Frame& frame) {
    if (!sessionConfigured_.load(std::memory_order_acquire)) return false;
    if (!worldSyncAuthorityBound_ || worldSyncAuthorityHost_ || !worldSyncWorldBusActive_ || generations_ == nullptr) return true;
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(frame.payload.data()), frame.payload.size());
    if(frame.type==protocol::MessageType::AISyncClientState) {
        static_cast<void>(ai_sync::execution_runtime::QueueCarrier(bytes));return true;
    }
    if(frame.type==protocol::MessageType::AISyncClientRoute) {
        ai_sync::route::Key key{};
        if(ai_sync::route::DecodeKey(bytes,key)!=ai_sync::route::Result::Ok||
            !ai_sync::execution::SessionTextMatches(key.scope.session,worldSyncAuthoritySessionId_)||
            key.scope.worldEpoch!=worldSyncAuthorityGeneration_)return true;
        static_cast<void>(ai_sync::execution_runtime::QueueRoute(bytes));return true;
    }
    if (frame.type == protocol::MessageType::AISyncExecutionBinding) {
        ai_sync::execution::Control control{};
        if (!ai_sync::execution::DecodeControl(bytes, control) ||
            !ai_sync::execution::SessionTextMatches(control.scope.session, worldSyncAuthoritySessionId_) ||
            control.scope.worldEpoch != worldSyncAuthorityGeneration_ || control.kind == ai_sync::execution::ControlKind::Ready) return true;
        const auto nativeGeneration = generations_->CurrentGeneration();
        const auto generation32 = nativeGeneration <= UINT32_MAX ? static_cast<std::uint32_t>(nativeGeneration) : 0U;
        if (!ai_sync::execution_runtime::QueueBinding(bytes, generation32) &&
            control.kind == ai_sync::execution::ControlKind::Offer) {
            std::array<std::uint8_t, ai_sync::execution::kHeaderSize> refusal{};
            control.kind = ai_sync::execution::ControlKind::Recovery;
            control.value = static_cast<std::uint32_t>(ai_sync::execution::Recovery::NativeUnavailable);
            if (ai_sync::execution::EncodeControl(control, refusal))
                static_cast<void>(Queue(protocol::MessageType::AISyncExecutionBinding, 0,
                    localSequence_.fetch_add(1, std::memory_order_relaxed), std::as_bytes(std::span(refusal))));
        }
    } else static_cast<void>(ai_sync::execution_runtime::QueueBatch(bytes, frame.type == protocol::MessageType::AISyncExecutionRetry));
    return true;
}

bool LauncherIPC::HandleAISyncHostFrame(const protocol::Frame& frame) {
    if (!sessionConfigured_.load(std::memory_order_acquire)) return false;
    if (!worldSyncAuthorityBound_ || !worldSyncAuthorityHost_ || !worldSyncWorldBusActive_ || generations_ == nullptr) return true;
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(frame.payload.data()), frame.payload.size());
    ai_sync::execution::Control control{};
    if (!ai_sync::execution::DecodeControl(bytes, control) ||
        !ai_sync::execution::SessionTextMatches(control.scope.session, worldSyncAuthoritySessionId_) ||
        control.scope.worldEpoch != worldSyncAuthorityGeneration_ || control.scope.consumerEpoch != 1 ||
        control.kind == ai_sync::execution::ControlKind::Ready) return true;
    const auto generation = generations_->CurrentGeneration();
    const auto generation32 = generation <= UINT32_MAX ? static_cast<std::uint32_t>(generation) : 0U;
    const bool routeProvider = (control.value & 8U) == 0U || ai_sync::route_runtime::Available();
    if ((!routeProvider || !ai_sync::host_producer::QueueBindingHost(bytes, generation32)) &&
        control.kind == ai_sync::execution::ControlKind::Offer) {
        std::array<std::uint8_t, ai_sync::execution::kHeaderSize> refusal{};
        control.kind = ai_sync::execution::ControlKind::Recovery;
        control.value = static_cast<std::uint32_t>(ai_sync::execution::Recovery::NativeUnavailable);
        if (ai_sync::execution::EncodeControl(control, refusal))
            static_cast<void>(Queue(protocol::MessageType::AISyncHostControl, 0,
                localSequence_.fetch_add(1, std::memory_order_relaxed), std::as_bytes(std::span(refusal))));
    }
    return true;
}
bool LauncherIPC::HandleAISyncFrame(const protocol::Frame& frame) {
    if (!sessionConfigured_.load(std::memory_order_acquire)) return false;
    bool accepted{};
    if (!ai_sync::Installed()) {
        accepted = false;
    } else if (frame.type == protocol::MessageType::AISyncConfigure) {
        accepted = ai_sync::Configure(frame.payload);
    } else if (!worldSyncAuthorityBound_ || !worldSyncWorldBusActive_) {
        return true; // A stale application packet cannot bind gameplay authority.
    } else if (frame.type == protocol::MessageType::AISyncState) {
        if (worldSyncAuthorityHost_) return true;
        return true; // Retired independent state lane; AISE uses the execution mailbox.
    } else {
        if (!worldSyncAuthorityHost_) return true;
        accepted = ai_sync::QueueRemoteImpact(frame.payload);
    }
    if (!accepted) {
        // A decoded AISync frame can fail validation or bounded admission.
        // Neither is a broken pipe/header: keep other modules and diagnostics
        // connected. Invalid data never reaches native gameplay execution.
        ++aiSyncRejectedFrames_;
        if (aiSyncRejectedFrames_ <= 4U || aiSyncRejectedFrames_ % 128U == 0U)
            diagnostics_->Log("warning", "aisync_ipc_payload_rejected",
                "AISync payload refused without closing the shared IPC; inspect validation and queue counters.",
                {{"messageType", Diagnostics::Unsigned(static_cast<std::uint16_t>(frame.type)), true},
                 {"sequence", Diagnostics::Unsigned(frame.sequence), true},
                 {"payloadBytes", Diagnostics::Unsigned(frame.payload.size()), true},
                 {"rejectedFrames", Diagnostics::Unsigned(aiSyncRejectedFrames_), true},
                 {"aisyncInstalled", Diagnostics::Boolean(ai_sync::Installed()), true}});
    }
    return true;
}

bool LauncherIPC::ReceiveAvailable() {
    constexpr std::size_t kMaximumReceiveBatch = 128U;
    for (std::size_t batch = 0U; batch < kMaximumReceiveBatch; ++batch) {
        const auto pipe = pipe_.load(std::memory_order_acquire);
        if (pipe == INVALID_HANDLE_VALUE) {
            return false;
        }

        std::array<std::byte, protocol::kWireHeaderBytes> header{};
        DWORD bytesRead = 0;
        DWORD bytesAvailable = 0;
        if (!PeekNamedPipe(
                pipe,
                header.data(),
                static_cast<DWORD>(header.size()),
                &bytesRead,
                &bytesAvailable,
                nullptr)) {
            Disconnect("PeekNamedPipe_failed");
            return false;
        }
        if (bytesAvailable < protocol::kWireHeaderBytes ||
            bytesRead < protocol::kWireHeaderBytes) {
            return true;
        }

        protocol::MessageType type{};
        std::uint16_t flags = 0;
        std::uint32_t payloadLength = 0;
        std::uint64_t sequence = 0;
        const auto headerResult = protocol::DecodeHeader(
            header,
            type,
            flags,
            payloadLength,
            sequence);
        if (headerResult != protocol::DecodeResult::Complete) {
            Disconnect("invalid_ipc_header");
            return false;
        }

        const auto total =
            protocol::kWireHeaderBytes + static_cast<std::size_t>(payloadLength);
        if (bytesAvailable < total) {
            return true;
        }

        std::vector<std::byte> wire(total);
        if (!ReadExact(wire.data(), wire.size())) {
            Disconnect("ipc_read_failed");
            return false;
        }

        protocol::Frame frame{};
        std::size_t consumed = 0;
        if (protocol::DecodeFrame(wire, frame, consumed) !=
                protocol::DecodeResult::Complete ||
            consumed != wire.size()) {
            Disconnect("ipc_frame_decode_failed");
            return false;
        }
        if (!HandleFrame(std::move(frame))) {
            Disconnect("ipc_message_validation_failed");
            return false;
        }
        workerTrafficObserved_ = true;
    }
    return true;
}

bool LauncherIPC::HandleFrame(protocol::Frame frame) {
    if (frame.type != protocol::MessageType::HelloAck &&
        !helloAcknowledged_.load(std::memory_order_acquire)) {
        return false;
    }
    switch (frame.type) {
    case protocol::MessageType::AISyncTransactionObservation:
        // Outbound-only observation export has no native input/replay path.
        return false;
    case protocol::MessageType::HelloAck: {
        if (helloAcknowledged_.load(std::memory_order_acquire) ||
            frame.payload.size() > 4096) {
            return false;
        }
        JsonObject object;
        ClosedJsonParser parser(frame.payload);
        if (!parser.Parse(object) || object.size() != 4) {
            return false;
        }
        const auto protocolValue = object.find("protocolVersion");
        const auto launcherVersion = object.find("launcherVersion");
        const auto processValue = object.find("processId");
        const auto nonceValue = object.find("nonce");
        if (protocolValue == object.end() ||
            launcherVersion == object.end() ||
            processValue == object.end() ||
            nonceValue == object.end() ||
            !std::holds_alternative<std::uint64_t>(protocolValue->second) ||
            !std::holds_alternative<std::string>(launcherVersion->second) ||
            !std::holds_alternative<std::uint64_t>(processValue->second) ||
            !std::holds_alternative<std::string>(nonceValue->second)) {
            return false;
        }
        const auto& version =
            std::get<std::string>(launcherVersion->second);
        if (std::get<std::uint64_t>(protocolValue->second) != kProtocolVersion ||
            std::get<std::uint64_t>(processValue->second) != processId_ ||
            std::get<std::string>(nonceValue->second) != nonce_ ||
            version.empty() ||
            version.size() > 64 ||
            !IsVisibleAscii(version)) {
            return false;
        }
        helloAcknowledged_.store(true, std::memory_order_release);
        diagnostics_->Log(
            "info",
            "ipc_hello_ack",
            "Launcher ACK matched protocol, target PID, and the DLL nonce.",
            {{"launcherVersion", version}});
        const auto buildInfo = BuildInfoPayload();
        return Queue(
            protocol::MessageType::BuildInfo,
            0,
            localSequence_.fetch_add(1, std::memory_order_relaxed),
            buildInfo);
    }
    case protocol::MessageType::SessionConfig: {
        if (!helloAcknowledged_.load(std::memory_order_acquire) ||
            sessionConfigured_.load(std::memory_order_acquire) ||
            frame.payload.empty() ||
            frame.payload.size() > 64U * 1024U) {
            return false;
        }
        JsonObject sessionObject;
        ClosedJsonParser sessionParser(frame.payload);
        if (!sessionParser.Parse(sessionObject) || sessionObject.size() != 9) {
            return false;
        }

        const auto sessionId = sessionObject.find("sessionId");
        const auto peerId = sessionObject.find("peerId");
        const auto launcherPlayerId = sessionObject.find("launcherPlayerId");
        const auto displayName = sessionObject.find("displayName");
        const auto isHost = sessionObject.find("isHost");
        const auto hostAddress = sessionObject.find("hostAddress");
        const auto hostPort = sessionObject.find("hostPort");
        const auto levelGeneration = sessionObject.find("levelGeneration");
        const auto nativeReplication =
            sessionObject.find("nativeReplicationEnabled");
        if (sessionId == sessionObject.end() ||
            peerId == sessionObject.end() ||
            launcherPlayerId == sessionObject.end() ||
            displayName == sessionObject.end() ||
            isHost == sessionObject.end() ||
            hostAddress == sessionObject.end() ||
            hostPort == sessionObject.end() ||
            levelGeneration == sessionObject.end() ||
            nativeReplication == sessionObject.end() ||
            !std::holds_alternative<std::string>(sessionId->second) ||
            !std::holds_alternative<std::uint64_t>(peerId->second) ||
            !std::holds_alternative<std::string>(launcherPlayerId->second) ||
            !std::holds_alternative<std::string>(displayName->second) ||
            !std::holds_alternative<bool>(isHost->second) ||
            !std::holds_alternative<std::string>(hostAddress->second) ||
            !std::holds_alternative<std::uint64_t>(hostPort->second) ||
            !std::holds_alternative<std::uint64_t>(levelGeneration->second) ||
            !std::holds_alternative<bool>(nativeReplication->second)) {
            return false;
        }

        const auto peer = std::get<std::uint64_t>(peerId->second);
        const auto port = std::get<std::uint64_t>(hostPort->second);
        const auto generation =
            std::get<std::uint64_t>(levelGeneration->second);
        const auto& display = std::get<std::string>(displayName->second);
        const auto& host = std::get<std::string>(hostAddress->second);
        if (!IsNonEmptyGuid(std::get<std::string>(sessionId->second)) ||
            !IsNonEmptyGuid(std::get<std::string>(launcherPlayerId->second)) ||
            peer == 0 ||
            !IsValidPlayerName(display) ||
            host.size() > 255 ||
            !IsValidUtf8Text(host, 255) ||
            port == 0 ||
            port > 65535 ||
            generation > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int32_t>::max()) ||
            std::get<bool>(nativeReplication->second)) {
            return false;
        }
        // SessionConfig is IPC bootstrap only. It must never freeze gameplay
        // authority before a real LobbyHost/LobbyClient exists. The dedicated
        // WorldSyncLifecycle message binds the actual multiplayer PeerId/role.
        ai_sync::host_producer::RetireHost();
        ai_sync::transaction_capture::DiscardTransport();
        localPeerId_ = 0U;
        world_actions::ClearSessionAuthority();
        localLauncherPlayerId_ =
            std::get<std::string>(launcherPlayerId->second);
        {
            // Generic runtime publications use the normal session identity;
            // TestModeConfigure may still replace it for legacy Dummy tools.
            std::lock_guard lock(experimentalStateMutex_);
            negotiationRequestId_ =
                std::get<std::string>(sessionId->second);
            negotiatedDummyId_ = localLauncherPlayerId_;
        }
        lastPublishedScheduleState_.store(0xFFU, std::memory_order_release);
        lastPublishedPendingCommandCount_.store(
            (std::numeric_limits<std::uint64_t>::max)(),
            std::memory_order_release);
        sessionConfigured_.store(true, std::memory_order_release);
        diagnostics_->Log(
            "info",
            "ipc_session_config",
            "Validated IPC bootstrap configuration received; WS3 multiplayer authority remains unbound.",
            {{"bootstrapPeerId", Diagnostics::Unsigned(peer), true},
             {"bootstrapIsHost",
              Diagnostics::Boolean(std::get<bool>(isHost->second)),
              true},
             {"authorityBound", "false", true},
             {"worldBusActive", "false", true},
             {"levelGeneration",
              Diagnostics::Unsigned(generation),
              true},
             {"nativeReplicationEnabled", "false", true}});
        const auto status = BuildObservationStatusPayload();
        const bool queued = Queue(
            protocol::MessageType::NetzReady,
            0,
            localSequence_.fetch_add(1, std::memory_order_relaxed),
            status);
        netzReadySent_.store(queued, std::memory_order_release);
        return queued;
    }
    case protocol::MessageType::AlienPvPCommand:
        if(sessionConfigured_.load(std::memory_order_acquire) &&
            worldSyncAuthorityBound_ && worldSyncWorldBusActive_)
            static_cast<void>(alien_pvp::QueueCommand(frame.payload));
        return true; // stale input expires; it never tears down other IPC systems.
    case protocol::MessageType::AISyncExecutionBinding:
    case protocol::MessageType::AISyncExecutionBatch:
    case protocol::MessageType::AISyncClientState:
    case protocol::MessageType::AISyncClientRoute:
    case protocol::MessageType::AISyncExecutionRetry:
        return HandleAISyncExecutionFrame(frame);
    case protocol::MessageType::AISyncHostBinding:
        return HandleAISyncHostFrame(frame);
    case protocol::MessageType::AISyncConfigure:
    case protocol::MessageType::AISyncState:
    case protocol::MessageType::AISyncImpact:
        return HandleAISyncFrame(frame);
    case protocol::MessageType::ThrowableConfigure:
        if (sessionConfigured_.load(std::memory_order_acquire))
            static_cast<void>(throwable_sync::Configure(frame.payload));
        return true;
    case protocol::MessageType::ThrowableEvent:
        if (sessionConfigured_.load(std::memory_order_acquire) && worldSyncAuthorityBound_ && worldSyncWorldBusActive_)
            static_cast<void>(throwable_sync::QueueRemote(frame.payload));
        return true;
    case protocol::MessageType::VoiceConfigure:
        if (!sessionConfigured_.load(std::memory_order_acquire)) return true;
        if (frame.payload.size() == 48U) {
            const auto flags = std::to_integer<unsigned int>(frame.payload[12]);
            if ((flags & 1U) == 0U || (worldSyncAuthorityBound_ && worldSyncWorldBusActive_))
                static_cast<void>(voice_sync::Configure(frame.payload, worldSyncAuthorityHost_));
        }
        return true; // Malformed/dropped audio never tears down the shared pipe.
    case protocol::MessageType::VoicePcmFrame:
        if (sessionConfigured_.load(std::memory_order_acquire) && worldSyncAuthorityBound_ && worldSyncWorldBusActive_)
            static_cast<void>(voice_sync::QueueFrame(frame.payload));
        else voice_sync::NoteGatedFrame();
        return true;
    case protocol::MessageType::WorldSyncLifecycle: {
        if (!sessionConfigured_.load(std::memory_order_acquire) ||
            frame.payload.empty() || frame.payload.size() > 16U * 1024U) {
            return false;
        }
        JsonObject object;
        ClosedJsonParser parser(frame.payload);
        if (!parser.Parse(object) || object.size() != 7U) return false;
        const auto sessionId = object.find("sessionId");
        const auto peerId = object.find("peerId");
        const auto isHost = object.find("isHost");
        const auto levelGeneration = object.find("levelGeneration");
        const auto authorityBound = object.find("authorityBound");
        const auto worldBusActive = object.find("worldBusActive");
        const auto reason = object.find("reason");
        if (sessionId == object.end() || peerId == object.end() ||
            isHost == object.end() || levelGeneration == object.end() ||
            authorityBound == object.end() || worldBusActive == object.end() ||
            reason == object.end() ||
            !std::holds_alternative<std::string>(sessionId->second) ||
            !std::holds_alternative<std::uint64_t>(peerId->second) ||
            !std::holds_alternative<bool>(isHost->second) ||
            !std::holds_alternative<std::uint64_t>(levelGeneration->second) ||
            !std::holds_alternative<bool>(authorityBound->second) ||
            !std::holds_alternative<bool>(worldBusActive->second) ||
            !std::holds_alternative<std::string>(reason->second)) {
            return false;
        }
        const auto& session = std::get<std::string>(sessionId->second);
        const auto peer = std::get<std::uint64_t>(peerId->second);
        const auto host = std::get<bool>(isHost->second);
        const auto generation = std::get<std::uint64_t>(levelGeneration->second);
        const auto bound = std::get<bool>(authorityBound->second);
        const auto busActive = std::get<bool>(worldBusActive->second);
        const auto& bindReason = std::get<std::string>(reason->second);
        if (!IsNonEmptyGuid(session) || peer == 0U ||
            generation > static_cast<std::uint64_t>(
                std::numeric_limits<std::int32_t>::max()) ||
            bindReason.empty() || bindReason.size() > 128U ||
            !IsValidUtf8Text(bindReason, 128U) || (busActive && !bound)) {
            return false;
        }

        // A different live session/peer/role must first issue an explicit
        // unbind. This prevents an arbitrary Host<->Client flip while gameplay
        // authority is active. Generation/bus-state updates for the same bound
        // identity are legal.
        if (worldSyncAuthorityBound_) {
            const bool sameIdentity = worldSyncAuthoritySessionId_ == session &&
                localPeerId_ == peer && worldSyncAuthorityHost_ == host;
            if (!sameIdentity) return false;
        }

        if (!bound || !worldSyncAuthorityBound_ || worldSyncAuthorityGeneration_ != generation ||
            worldSyncWorldBusActive_ != busActive) {
            ai_sync::execution_runtime::Retire();
            ai_sync::host_producer::RetireHost();
            ai_sync::transaction_capture::DiscardTransport();
        }
        if (!bound) {
            world_actions::ClearSessionAuthority();
            localPeerId_ = 0U;
            worldSyncAuthoritySessionId_.clear();
            worldSyncAuthorityGeneration_ = 0U;
            worldSyncAuthorityBound_ = false;
            worldSyncAuthorityHost_ = false;
            worldSyncWorldBusActive_ = false;
            diagnostics_->Log(
                "info",
                "ws3_native_authority_unbound",
                "WS3 native multiplayer authority unbound; retail remains authoritative locally until a new real lobby binding is installed.",
                {{"session", session, true},
                 {"peerId", Diagnostics::Unsigned(peer), true},
                 {"role", host ? "Host" : "Client", true},
                 {"generation", Diagnostics::Unsigned(generation), true},
                 {"reason", bindReason, true}});
            return true;
        }

        // Authority transactions restart at 1 for every lobby session. A
        // committed checkpoint from an earlier session would otherwise make
        // every lower sequence look already applied and be acknowledged
        // without execution. Rebinding the same session (recovery/reconnect)
        // keeps its checkpoint so retransmissions stay idempotent.
        if (session != worldSyncLastBoundSessionId_) {
            replication_core::ResetAuthorityOrdering();
            worldSyncLastBoundSessionId_ = session;
        }
        world_actions::SetSessionAuthority(peer, host);
        world_actions::SetWorldBusActive(busActive);
        localPeerId_ = peer;
        worldSyncAuthoritySessionId_ = session;
        worldSyncAuthorityGeneration_ = generation;
        worldSyncAuthorityBound_ = true;
        worldSyncAuthorityHost_ = host;
        worldSyncWorldBusActive_ = busActive;
        diagnostics_->Log(
            "info",
            "ws3_native_authority_bound",
            busActive
                ? "WS3 native authority bound and compatible World bus opened."
                : "WS3 native authority bound; compatible World bus remains closed.",
            {{"session", session},
             {"peerId", Diagnostics::Unsigned(peer), true},
             {"role", host ? "Host" : "Client"},
             {"generation", Diagnostics::Unsigned(generation), true},
             {"worldBusActive", Diagnostics::Boolean(busActive), true},
             {"reason", bindReason}});
        return true;
    }
    case protocol::MessageType::TestModeConfigure: {
        experimental::TestModeConfigureRequest request;
        if (!sessionConfigured_.load(std::memory_order_acquire) ||
            !netzReadySent_.load(std::memory_order_acquire) ||
            !experimental::DecodeTestModeConfigure(frame.payload, request)) {
            return false;
        }
        if (!request.enabled ||
            capabilities_ == nullptr ||
            captures_ == nullptr ||
            generations_ == nullptr ||
            dispatcher_ == nullptr ||
            dummy_ == nullptr ||
            dummyExecutor_ == nullptr) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                "TEST_MODE_UNAVAILABLE",
                "Native experimental services were not initialized.",
                {"native_experimental_services"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }
        if (experimentalNegotiated_.load(std::memory_order_acquire) &&
            negotiatedDummyId_ != request.portableDummyId) {
            return false;
        }
        {
            std::lock_guard lock(experimentalStateMutex_);
            negotiatedDummyId_ = request.portableDummyId;
            negotiationRequestId_ = request.requestId;
        }
        lastPublishedGeneration_.store(
            (std::numeric_limits<std::uint64_t>::max)(),
            std::memory_order_release);
        lastPublishedCaptureCount_.store(
            (std::numeric_limits<std::uint64_t>::max)(),
            std::memory_order_release);
        lastPublishedCaptureGeneration_.store(
            (std::numeric_limits<std::uint64_t>::max)(),
            std::memory_order_release);
        lastPublishedStability_.store(0xFFU, std::memory_order_release);
        lastPublishedGameThreadValidated_.store(
            0xFFU,
            std::memory_order_release);
        lastPublishedGameThreadCandidateId_.store(
            (std::numeric_limits<std::uint32_t>::max)(),
            std::memory_order_release);
        lastPublishedGameThreadSourceCount_.store(
            (std::numeric_limits<std::uint32_t>::max)(),
            std::memory_order_release);
        lastPublishedScheduleState_.store(0xFFU, std::memory_order_release);
        lastPublishedPendingCommandCount_.store(
            (std::numeric_limits<std::uint64_t>::max)(),
            std::memory_order_release);

        const auto snapshot = capabilities_->Snapshot();
        std::vector<CapabilityRecord> records(
            snapshot.begin(),
            snapshot.end());
        std::vector<std::string> blocked;
        for (const auto& record : records) {
            if (record.state == CapabilityState::Blocked ||
                record.state == CapabilityState::Unresolved ||
                record.state == CapabilityState::Disabled) {
                blocked.emplace_back(ToString(record.capability));
            }
        }
        const auto capabilities = experimental::EncodeCapabilityState(
            request,
            profileId_,
            resolutionMode_,
            buildValidated_,
            dispatcher_->IsEnabled(),
            records,
            blocked,
            "Chargez une partie et attendez les captures Character F0 et 130; les mutations restent bloquées jusqu'à validation runtime du thread moteur et du cleanup.");
        if (!QueueExperimental(
                protocol::MessageType::CapabilityState,
                capabilities)) {
            return false;
        }

        const auto captureSummary = captures_->Summary();
        const auto runtime = discovery_ != nullptr
            ? discovery_->RuntimeSnapshot()
            : RuntimeObservationSnapshot{};
        const auto captureCount =
            (std::min)(captureSummary.currentCaptureCount, 256U);
        const auto engineThread = dispatcher_->Snapshot();
        const auto capture = experimental::EncodeCaptureState(
            request,
            captureSummary.hasF0,
            captureSummary.has130,
            runtime.participantRegistryAvailable,
            runtime.participantCount,
            runtime.contextPool != 0,
            runtime.channelSnapshotAvailable &&
                runtime.freeChannelMask != 0,
            engineThread.ready,
            captureCount,
            "experimental_mode_negotiated",
            engineThread.engineThreadId,
            engineThread.ready ? 1U : 0U,
            ToString(engineThread.source),
            engineThread.firstCallbackPerformanceCounter,
            engineThread.lastCallbackPerformanceCounter,
            engineThread.pendingCount,
            engineThread.mainThreadHookInstalled,
            engineThread.managerUpdateHookInstalled,
            engineThread.readinessProducerHookInstalled);
        const bool captureQueued = QueueExperimental(
            protocol::MessageType::CaptureState,
            capture);
        experimentalNegotiated_.store(true, std::memory_order_release);
        return captureQueued || !capabilities.empty();
    }
    case protocol::MessageType::CapabilityQuery: {
        experimental::CapabilityQueryRequest request;
        if (!sessionConfigured_.load(std::memory_order_acquire) ||
            !netzReadySent_.load(std::memory_order_acquire) ||
            !experimental::DecodeCapabilityQuery(frame.payload, request)) {
            return false;
        }
        const auto currentGeneration = generations_->CurrentGeneration();
        if (request.levelGeneration != 0 &&
            request.levelGeneration != currentGeneration) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                "LEVEL_GENERATION_MISMATCH",
                "The capability query targets an obsolete level generation.",
                {"current_level_generation"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }
        request.levelGeneration = static_cast<std::uint32_t>(
            (std::min<std::uint64_t>)(
                currentGeneration,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)())));
        const auto snapshot = capabilities_->Snapshot();
        std::vector<CapabilityRecord> records(
            snapshot.begin(),
            snapshot.end());
        std::vector<std::string> blocked;
        for (const auto& record : records) {
            if (record.state == CapabilityState::Blocked ||
                record.state == CapabilityState::Unresolved ||
                record.state == CapabilityState::Disabled) {
                blocked.emplace_back(ToString(record.capability));
            }
        }
        std::string_view nextAction =
            "Waiting for the first engine callback.";
        if (dispatcher_->IsEnabled()) {
            nextAction =
                "Engine thread ready.";
        }
        const auto payload = experimental::EncodeCapabilityState(
            request,
            profileId_,
            resolutionMode_,
            buildValidated_,
            dispatcher_->IsEnabled(),
            records,
            blocked,
            nextAction);
        if (!QueueExperimental(
            protocol::MessageType::CapabilityState,
            payload)) {
            return false;
        }
        const auto captureSummary = captures_->Summary();
        const auto runtime = discovery_ != nullptr
            ? discovery_->RuntimeSnapshot()
            : RuntimeObservationSnapshot{};
        const auto engineThread = dispatcher_->Snapshot();
        const auto capture = experimental::EncodeCaptureState(
            request,
            captureSummary.hasF0,
            captureSummary.has130,
            runtime.participantRegistryAvailable,
            runtime.participantCount,
            runtime.contextPool != 0,
            runtime.channelSnapshotAvailable &&
                runtime.freeChannelMask != 0,
            engineThread.ready,
            (std::min)(
                captureSummary.currentCaptureCount,
                256U),
            "capability_query",
            engineThread.engineThreadId,
            engineThread.ready ? 1U : 0U,
            ToString(engineThread.source),
            engineThread.firstCallbackPerformanceCounter,
            engineThread.lastCallbackPerformanceCounter,
            engineThread.pendingCount,
            engineThread.mainThreadHookInstalled,
            engineThread.managerUpdateHookInstalled,
            engineThread.readinessProducerHookInstalled);
        return QueueExperimental(
            protocol::MessageType::CaptureState,
            capture);
    }
    case protocol::MessageType::DummyCreate: {
        experimental::DummyCreateRequest request;
        if (!experimentalNegotiated_.load(std::memory_order_acquire) ||
            !experimental::DecodeDummyCreate(frame.payload, request) ||
            request.portableDummyId != negotiatedDummyId_) {
            return false;
        }
        if (request.levelGeneration !=
            generations_->CurrentGeneration()) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                "LEVEL_GENERATION_MISMATCH",
                "La création vise une génération de niveau obsolète.",
                {"current_level_generation"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }
        const auto reservation = ReserveExperimentalRequest(
            request.requestId,
            protocol::MessageType::DummyCreateResult,
            frame.payload);
        if (reservation.state ==
            ExperimentalReservationState::Cached) {
            return QueueExperimental(
                reservation.cachedType,
                reservation.cachedResponse);
        }
        if (reservation.state ==
            ExperimentalReservationState::PendingDuplicate) {
            return true;
        }
        if (reservation.state ==
                ExperimentalReservationState::Collision ||
            reservation.state ==
                ExperimentalReservationState::CapacityReached) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                reservation.state ==
                        ExperimentalReservationState::Collision
                    ? "REQUEST_ID_REUSE"
                    : "EXPERIMENTAL_QUEUE_FULL",
                reservation.state ==
                        ExperimentalReservationState::Collision
                    ? "RequestId already belongs to another command."
                    : "The bounded experimental queue is full.",
                {"unique_request_id"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }

        constexpr Capability required[]{
            Capability::ScheduleGameThreadWork,
            Capability::CreateDummyParticipant,
            Capability::ReplayCharacterSpawn,
            Capability::BindDummyCharacter,
            Capability::DestroyDummyCharacter,
        };
        std::vector<std::string> blocked;
        for (const auto capability : required) {
            if (!capabilities_->IsEnabled(capability)) {
                blocked.emplace_back(ToString(capability));
            }
        }
        if (!dispatcher_->IsEnabled() &&
            std::find(
                blocked.begin(),
                blocked.end(),
                "ScheduleGameThreadWork") == blocked.end()) {
            blocked.emplace_back("validated_game_thread_dispatcher");
        }
        if (!blocked.empty()) {
            DummyOperationResult result{};
            result.state = dummy_->State();
            result.achieved = dummy_->Achievement();
            result.statusCode = "DUMMY_CREATE_BLOCKED";
            result.message =
                "Aucune mutation moteur n'a été exécutée : au moins une gate n'est pas runtime-validée pour cette génération.";
            result.blockedGates = std::move(blocked);
            const auto payload =
                experimental::EncodeDummyOperationResult(
                    request,
                    result);
            diagnostics_->Log(
                "warning",
                "dummy_create_blocked",
                result.message,
                {{"levelGeneration",
                  Diagnostics::Unsigned(request.levelGeneration),
                  true},
                 {"portableDummyId", request.portableDummyId}});
            return CompleteExperimentalRequest(
                request.requestId,
                reservation.epoch,
                protocol::MessageType::DummyCreateResult,
                frame.payload,
                payload);
        }

        struct PendingCreate final {
            experimental::DummyCreateRequest request;
            std::vector<std::byte> requestPayload;
            std::uint64_t epoch{};
        };
        auto pending = std::make_shared<PendingCreate>(
            PendingCreate{
                request,
                frame.payload,
                reservation.epoch});
        GameThreadCommand command{};
        command.requestId = NextExperimentalDispatchId();
        command.levelGeneration = request.levelGeneration;
        command.expiresAtTickMs = GetTickCount64() + 15000;
        command.kind = GameThreadCommandKind::CreateDummy;
        command.execute = [this, pending] {
            const auto result = dummy_->Create(
                pending->request.identity,
                pending->request.levelGeneration,
                DummySpawnOffset{
                    pending->request.offsetX,
                    pending->request.offsetY,
                    pending->request.offsetZ},
                *dummyExecutor_);
            const auto payload =
                experimental::EncodeDummyOperationResult(
                    pending->request,
                    result);
            CompleteExperimentalRequest(
                pending->request.requestId,
                pending->epoch,
                protocol::MessageType::DummyCreateResult,
                pending->requestPayload,
                payload);
            if (result.state == DummyState::Active) {
                const auto binding =
                    experimental::EncodeDummyBindingState(
                        pending->request,
                        result.achieved,
                        {});
                QueueExperimental(
                    protocol::MessageType::DummyBindingState,
                    binding);
            }
        };
        command.completion = [this, pending](
                                 const DispatchCompletion completion) {
            if (completion == DispatchCompletion::Executed) {
                return;
            }
            DummyOperationResult result{};
            result.state = dummy_->State();
            result.achieved = dummy_->Achievement();
            result.statusCode =
                completion == DispatchCompletion::Expired
                    ? "DUMMY_CREATE_EXPIRED"
                    : (completion ==
                               DispatchCompletion::GenerationInvalidated
                           ? "LEVEL_GENERATION_MISMATCH"
                           : (completion == DispatchCompletion::Cleared
                                  ? "DISPATCHER_STOPPED"
                                  : "DUMMY_CREATE_TASK_FAULT"));
            result.message =
                "La commande n'a pas exécuté le pipeline natif sur le thread moteur validé.";
            result.blockedGates.emplace_back(
                completion ==
                        DispatchCompletion::GenerationInvalidated
                    ? "current_level_generation"
                    : "validated_game_thread_dispatcher");
            const auto payload =
                experimental::EncodeDummyOperationResult(
                    pending->request,
                    result);
            CompleteExperimentalRequest(
                pending->request.requestId,
                pending->epoch,
                protocol::MessageType::DummyCreateResult,
                pending->requestPayload,
                payload);
        };
        const auto queued = dispatcher_->Enqueue(std::move(command));
        if (queued == DispatchEnqueueResult::Queued) {
            diagnostics_->Log(
                "info",
                "dummy_create_queued",
                "La création du Dummy est en attente du tick moteur validé.",
                {{"levelGeneration",
                  Diagnostics::Unsigned(request.levelGeneration),
                  true},
                 {"portableDummyId", request.portableDummyId}});
            return true;
        }

        DummyOperationResult result{};
        result.state = dummy_->State();
        result.achieved = dummy_->Achievement();
        result.statusCode =
            queued == DispatchEnqueueResult::CreateAlreadyPending
                ? "DUMMY_CREATE_IN_PROGRESS"
                : (queued == DispatchEnqueueResult::GenerationMismatch
                       ? "LEVEL_GENERATION_MISMATCH"
                       : (queued == DispatchEnqueueResult::CapacityReached
                              ? "DISPATCH_QUEUE_FULL"
                              : (queued == DispatchEnqueueResult::Expired
                                     ? "DUMMY_CREATE_EXPIRED"
                                     : "DUMMY_CREATE_BLOCKED")));
        result.message =
            "La commande n'a pas été acceptée par la file bornée du thread moteur.";
        result.blockedGates.emplace_back(
            queued == DispatchEnqueueResult::GenerationMismatch
                ? "current_level_generation"
                : "validated_game_thread_dispatcher");
        const auto payload =
            experimental::EncodeDummyOperationResult(request, result);
        return CompleteExperimentalRequest(
            request.requestId,
            reservation.epoch,
            protocol::MessageType::DummyCreateResult,
            frame.payload,
            payload);
    }
    case protocol::MessageType::DummyRemove: {
        experimental::DummyRemoveRequest request;
        if (!experimentalNegotiated_.load(std::memory_order_acquire) ||
            !experimental::DecodeDummyRemove(frame.payload, request) ||
            request.portableDummyId != negotiatedDummyId_) {
            return false;
        }
        if (request.levelGeneration !=
            generations_->CurrentGeneration()) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                "LEVEL_GENERATION_MISMATCH",
                "La suppression vise une génération de niveau obsolète.",
                {"current_level_generation"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }
        const auto reservation = ReserveExperimentalRequest(
            request.requestId,
            protocol::MessageType::DummyRemoveResult,
            frame.payload);
        if (reservation.state ==
            ExperimentalReservationState::Cached) {
            return QueueExperimental(
                reservation.cachedType,
                reservation.cachedResponse);
        }
        if (reservation.state ==
            ExperimentalReservationState::PendingDuplicate) {
            return true;
        }
        if (reservation.state ==
                ExperimentalReservationState::Collision ||
            reservation.state ==
                ExperimentalReservationState::CapacityReached) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                reservation.state ==
                        ExperimentalReservationState::Collision
                    ? "REQUEST_ID_REUSE"
                    : "EXPERIMENTAL_QUEUE_FULL",
                "La commande Remove n'a pas été réservée.",
                {"unique_request_id"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }
        DummyOperationResult result{};
        result.state = dummy_->State();
        result.achieved = dummy_->Achievement();
        if (result.state == DummyState::Absent) {
            result.statusCode = "DUMMY_ABSENT";
            result.message =
                "No native dummy player is active; removal is idempotent.";
            const auto payload =
                experimental::EncodeDummyOperationResult(
                    request,
                    result);
            return CompleteExperimentalRequest(
                request.requestId,
                reservation.epoch,
                protocol::MessageType::DummyRemoveResult,
                frame.payload,
                payload);
        }

        constexpr Capability required[]{
            Capability::ScheduleGameThreadWork,
            Capability::DestroyDummyCharacter,
        };
        for (const auto capability : required) {
            if (!capabilities_->IsEnabled(capability)) {
                result.blockedGates.emplace_back(ToString(capability));
            }
        }
        if (!dispatcher_->IsEnabled()) {
            result.blockedGates.emplace_back(
                "validated_game_thread_dispatcher");
        }
        if (!result.blockedGates.empty()) {
            result.statusCode = "DUMMY_REMOVE_BLOCKED";
            result.message =
                "La suppression moteur exige le dispatcher et le cleanup runtime-validés.";
            const auto payload =
                experimental::EncodeDummyOperationResult(
                    request,
                    result);
            return CompleteExperimentalRequest(
                request.requestId,
                reservation.epoch,
                protocol::MessageType::DummyRemoveResult,
                frame.payload,
                payload);
        }

        struct PendingRemove final {
            experimental::DummyRemoveRequest request;
            std::vector<std::byte> requestPayload;
            std::uint64_t epoch{};
        };
        auto pending = std::make_shared<PendingRemove>(
            PendingRemove{
                request,
                frame.payload,
                reservation.epoch});
        GameThreadCommand command{};
        command.requestId = NextExperimentalDispatchId();
        command.levelGeneration = request.levelGeneration;
        command.expiresAtTickMs = GetTickCount64() + 15000;
        command.kind = GameThreadCommandKind::RemoveDummy;
        command.execute = [this, pending] {
            const auto operation = dummy_->Remove(
                pending->request.levelGeneration,
                *dummyExecutor_);
            const auto payload =
                experimental::EncodeDummyOperationResult(
                    pending->request,
                    operation);
            CompleteExperimentalRequest(
                pending->request.requestId,
                pending->epoch,
                protocol::MessageType::DummyRemoveResult,
                pending->requestPayload,
                payload);
            const auto binding =
                experimental::EncodeDummyBindingState(
                    pending->request,
                    operation.achieved,
                    operation.state == DummyState::Absent
                        ? std::string_view{}
                        : std::string_view(operation.message));
            QueueExperimental(
                protocol::MessageType::DummyBindingState,
                binding);
        };
        command.completion = [this, pending](
                                 const DispatchCompletion completion) {
            if (completion == DispatchCompletion::Executed) {
                return;
            }
            DummyOperationResult operation{};
            operation.state = dummy_->State();
            operation.achieved = dummy_->Achievement();
            operation.statusCode =
                completion == DispatchCompletion::Expired
                    ? "DUMMY_REMOVE_EXPIRED"
                    : (completion ==
                               DispatchCompletion::GenerationInvalidated
                           ? "LEVEL_GENERATION_MISMATCH"
                           : (completion == DispatchCompletion::Cleared
                                  ? "DISPATCHER_STOPPED"
                                  : "DUMMY_REMOVE_TASK_FAULT"));
            operation.message =
                "Le cleanup natif n'a pas été exécuté sur le thread moteur validé.";
            operation.blockedGates.emplace_back(
                completion ==
                        DispatchCompletion::GenerationInvalidated
                    ? "current_level_generation"
                    : "validated_game_thread_dispatcher");
            const auto payload =
                experimental::EncodeDummyOperationResult(
                    pending->request,
                    operation);
            CompleteExperimentalRequest(
                pending->request.requestId,
                pending->epoch,
                protocol::MessageType::DummyRemoveResult,
                pending->requestPayload,
                payload);
        };
        const auto queued = dispatcher_->Enqueue(std::move(command));
        if (queued == DispatchEnqueueResult::Queued) {
            return true;
        }

        result.statusCode =
            queued == DispatchEnqueueResult::GenerationMismatch
                ? "LEVEL_GENERATION_MISMATCH"
                : (queued == DispatchEnqueueResult::CapacityReached
                       ? "DISPATCH_QUEUE_FULL"
                       : (queued == DispatchEnqueueResult::Expired
                              ? "DUMMY_REMOVE_EXPIRED"
                              : "DUMMY_REMOVE_BLOCKED"));
        result.message =
            "La suppression n'a pas été acceptée par la file bornée du thread moteur.";
        result.blockedGates.emplace_back(
            queued == DispatchEnqueueResult::GenerationMismatch
                ? "current_level_generation"
                : "validated_game_thread_dispatcher");
        const auto payload =
            experimental::EncodeDummyOperationResult(request, result);
        return CompleteExperimentalRequest(
            request.requestId,
            reservation.epoch,
            protocol::MessageType::DummyRemoveResult,
            frame.payload,
            payload);
    }
    case protocol::MessageType::DummyMove: {
        experimental::DummyMoveRequest request;
        if (!experimentalNegotiated_.load(std::memory_order_acquire) ||
            !experimental::DecodeDummyMove(frame.payload, request) ||
            request.portableDummyId != negotiatedDummyId_) {
            return false;
        }
        if (request.levelGeneration !=
            generations_->CurrentGeneration()) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                "LEVEL_GENERATION_MISMATCH",
                "Le mouvement vise une génération de niveau obsolète.",
                {"current_level_generation"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }
        const auto reservation = ReserveExperimentalRequest(
            request.requestId,
            protocol::MessageType::DummyMoveResult,
            frame.payload);
        if (reservation.state ==
            ExperimentalReservationState::Cached) {
            return QueueExperimental(
                reservation.cachedType,
                reservation.cachedResponse);
        }
        if (reservation.state ==
            ExperimentalReservationState::PendingDuplicate) {
            return true;
        }
        if (reservation.state ==
                ExperimentalReservationState::Collision ||
            reservation.state ==
                ExperimentalReservationState::CapacityReached) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                reservation.state ==
                        ExperimentalReservationState::Collision
                    ? "REQUEST_ID_REUSE"
                    : "EXPERIMENTAL_QUEUE_FULL",
                "La commande Move n'a pas été réservée.",
                {"unique_request_id"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }

        std::vector<std::string> blocked;
        constexpr Capability required[]{
            Capability::ScheduleGameThreadWork,
            Capability::BindDummyCharacter,
        };
        for (const auto capability : required) {
            if (!capabilities_->IsEnabled(capability)) {
                blocked.emplace_back(ToString(capability));
            }
        }
        if (!dispatcher_->IsEnabled()) {
            blocked.emplace_back("validated_game_thread_dispatcher");
        }
        if (dummy_->State() != DummyState::Active ||
            (dummy_->Achievement() != DummyAchievement::C &&
             dummy_->Achievement() != DummyAchievement::D)) {
            blocked.emplace_back("active_dummy_level_C_binding");
        }
        if (!blocked.empty()) {
            const auto payload = experimental::EncodeDummyMoveResult(
                request,
                "MOVEMENT_BLOCKED",
                "Aucun Dummy de niveau C et aucun chemin natif de mouvement ne sont runtime-validés.",
                "NotApplied",
                false);
            return CompleteExperimentalRequest(
                request.requestId,
                reservation.epoch,
                protocol::MessageType::DummyMoveResult,
                frame.payload,
                payload);
        }

        struct PendingMove final {
            experimental::DummyMoveRequest request;
            std::vector<std::byte> requestPayload;
            std::uint64_t epoch{};
        };
        auto pending = std::make_shared<PendingMove>(
            PendingMove{
                request,
                frame.payload,
                reservation.epoch});
        GameThreadCommand command{};
        command.requestId = NextExperimentalDispatchId();
        command.levelGeneration = request.levelGeneration;
        command.expiresAtTickMs = GetTickCount64() + 15000;
        command.kind = GameThreadCommandKind::MoveDummy;
        command.execute = [this, pending] {
            const auto operation = dummy_->Move(
                pending->request.levelGeneration,
                pending->request.command,
                pending->request.amount,
                pending->request.durationMilliseconds,
                *dummyExecutor_);
            const bool applied =
                operation.statusCode == "MOVEMENT_APPLIED";
            const auto payload =
                experimental::EncodeDummyMoveResult(
                    pending->request,
                    operation.statusCode,
                    applied
                        ? "La commande de mouvement a été appliquée."
                        : std::string_view(operation.message),
                    applied
                        ? std::string_view(operation.message)
                        : std::string_view("NotApplied"),
                    applied);
            CompleteExperimentalRequest(
                pending->request.requestId,
                pending->epoch,
                protocol::MessageType::DummyMoveResult,
                pending->requestPayload,
                payload);
            if (applied) {
                const auto binding =
                    experimental::EncodeDummyBindingState(
                        pending->request,
                        operation.achieved,
                        {});
                QueueExperimental(
                    protocol::MessageType::DummyBindingState,
                    binding);
            }
        };
        command.completion = [this, pending](
                                 const DispatchCompletion completion) {
            if (completion == DispatchCompletion::Executed) {
                return;
            }
            const auto status =
                completion == DispatchCompletion::Expired
                    ? std::string_view("MOVEMENT_EXPIRED")
                    : (completion ==
                               DispatchCompletion::GenerationInvalidated
                           ? std::string_view(
                                 "LEVEL_GENERATION_MISMATCH")
                           : (completion == DispatchCompletion::Cleared
                                  ? std::string_view(
                                        "DISPATCHER_STOPPED")
                                  : std::string_view(
                                        "MOVEMENT_TASK_FAULT")));
            const auto payload =
                experimental::EncodeDummyMoveResult(
                    pending->request,
                    status,
                    "La commande n'a pas été exécutée sur le thread moteur validé.",
                    "NotApplied",
                    false);
            CompleteExperimentalRequest(
                pending->request.requestId,
                pending->epoch,
                protocol::MessageType::DummyMoveResult,
                pending->requestPayload,
                payload);
        };
        const auto queued = dispatcher_->Enqueue(std::move(command));
        if (queued == DispatchEnqueueResult::Queued) {
            return true;
        }

        const auto status =
            queued == DispatchEnqueueResult::GenerationMismatch
                ? std::string_view("LEVEL_GENERATION_MISMATCH")
                : (queued == DispatchEnqueueResult::CapacityReached
                       ? std::string_view("DISPATCH_QUEUE_FULL")
                       : (queued == DispatchEnqueueResult::Expired
                              ? std::string_view("MOVEMENT_EXPIRED")
                              : std::string_view("MOVEMENT_BLOCKED")));
        const auto payload = experimental::EncodeDummyMoveResult(
            request,
            status,
            "La commande n'a pas été acceptée par la file bornée du thread moteur.",
            "NotApplied",
            false);
        return CompleteExperimentalRequest(
            request.requestId,
            reservation.epoch,
            protocol::MessageType::DummyMoveResult,
            frame.payload,
            payload);
    }
    case protocol::MessageType::GameCommandExecute: {
        experimental::GameCommandRequest request;
        if (!sessionConfigured_.load(std::memory_order_acquire) ||
            !netzReadySent_.load(std::memory_order_acquire) ||
            !experimental::DecodeGameCommand(frame.payload, request)) {
            return false;
        }
        const bool ws3Setting = request.command == "WORLD_WS3_EMPTY_CALLBACK_SET";
        const bool performanceSetting = request.command == "PERFORMANCE_ISOLATION_SET";
        const bool loadingSetting = request.command == "SESSION_LOADING_HOLD";
        const bool roundBoundary = request.command == "SESSION_ROUND_BOUNDARY";
        const bool campaignProfile = request.command == "CAMPAIGN_PROFILE";
        const bool processSetting = ws3Setting || performanceSetting || loadingSetting || roundBoundary
            || campaignProfile;
        if (processSetting && request.portableDummyId != localLauncherPlayerId_) return false;
        const auto currentGeneration = processSetting ? 0ULL : generations_->CurrentGeneration();
        if (!processSetting && request.levelGeneration == 0) {
            request.levelGeneration = static_cast<std::uint32_t>(
                (std::min<std::uint64_t>)(
                    currentGeneration,
                    static_cast<std::uint64_t>(
                        (std::numeric_limits<std::int32_t>::max)())));
        }
        const auto reservation = ReserveExperimentalRequest(
            request.requestId,
            protocol::MessageType::GameCommandResult,
            frame.payload);
        if (reservation.state == ExperimentalReservationState::Cached) {
            return QueueExperimental(
                reservation.cachedType,
                reservation.cachedResponse);
        }
        if (reservation.state == ExperimentalReservationState::PendingDuplicate) {
            return true;
        }
        if (reservation.state == ExperimentalReservationState::Collision ||
            reservation.state == ExperimentalReservationState::CapacityReached) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                reservation.state == ExperimentalReservationState::Collision
                    ? "REQUEST_ID_REUSE"
                    : "EXPERIMENTAL_QUEUE_FULL",
                reservation.state == ExperimentalReservationState::Collision
                    ? "RequestId already belongs to another command."
                    : "The bounded experimental queue is full.",
                {"unique_request_id"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }

        // A runtime option changes only an atomic, so it is applied and
        // acknowledged on this authenticated IPC connection before engine
        // readiness/dispatcher gates. Reuse normal request reservation/replies.
        if (loadingSetting) {
            GameCommandExecutionResult result{};
            result.executed = multiplayer_pause::SetLoadingHold(request.sessionLoadingActive,
                request.sessionLoadingSessionId, request.sessionLoadingGeneration);
            result.statusCode = result.executed ? "SESSION_LOADING_APPLIED" : "SESSION_LOADING_REJECTED";
            result.message = result.executed ? "Session loading simulation hold updated." : "Loading token is obsolete or native simulation hold is unavailable.";
            const auto payload = experimental::EncodeGameCommandResult(request, request.command, result);
            return CompleteExperimentalRequest(request.requestId, reservation.epoch,
                protocol::MessageType::GameCommandResult, frame.payload, payload);
        }

        // Retail can reload a level, or return from it, while every observed
        // engine root keeps its address. Each lobby start therefore revokes
        // the previous incarnation explicitly, exactly like the mod-owned
        // RETURN_TO_FRONTEND does; Bootstrap then invalidates every
        // generation-bound registry (WS3 ordering, AISync world, captures).
        // Idempotent per session so a retried request cannot bump twice.
        if (roundBoundary) {
            GameCommandExecutionResult result{};
            const bool fresh = request.sessionLoadingSessionId != lastRoundBoundarySessionId_
                || request.sessionLoadingGeneration > lastRoundBoundaryNetworkGeneration_;
            if (fresh && generations_ != nullptr) {
                lastRoundBoundarySessionId_ = request.sessionLoadingSessionId;
                lastRoundBoundaryNetworkGeneration_ = request.sessionLoadingGeneration;
                const auto previous = generations_->CurrentGeneration();
                const auto next = generations_->BeginTransition();
                diagnostics_->Log(
                    "info",
                    "session_round_boundary",
                    "A lobby start revoked the previous level incarnation; the next observation opens a new generation.",
                    {{"session", request.sessionLoadingSessionId},
                     {"previousGeneration", Diagnostics::Unsigned(previous), true},
                     {"transitionGeneration", Diagnostics::Unsigned(next.generation), true}});
            }
            result.executed = generations_ != nullptr;
            result.statusCode = result.executed ? "SESSION_ROUND_BOUNDARY_APPLIED" : "SESSION_ROUND_BOUNDARY_REJECTED";
            result.message = result.executed
                ? (fresh ? "Previous level incarnation revoked." : "Round boundary already applied for this session.")
                : "Level generation tracking is unavailable.";
            const auto payload = experimental::EncodeGameCommandResult(request, request.command, result);
            return CompleteExperimentalRequest(request.requestId, reservation.epoch,
                protocol::MessageType::GameCommandResult, frame.payload, payload);
        }

        // Profile switches change only isolated file routing; the cached retail
        // save catalogue is re-enumerated by the main-thread storage poll.
        if (campaignProfile) {
            GameCommandExecutionResult result{};
            std::string error;
            bool applied = true;
            if (request.campaignProfileAction == "configure")
                applied = campaign_save::Configure(request.sessionLoadingSessionId, request.campaignProfilePath, error);
            else if (request.campaignProfileAction == "clear")
                applied = campaign_save::Clear(request.sessionLoadingSessionId, error);
            auto status = campaign_save::Snapshot();
            if (!error.empty()) status.error = error;
            result.executed = applied;
            result.statusCode = applied ? "CAMPAIGN_PROFILE_STATUS" : "CAMPAIGN_PROFILE_REJECTED";
            result.message = campaign_save::EncodeStatus(status);
            if (diagnostics_ != nullptr && request.campaignProfileAction != "status")
                diagnostics_->Log(applied ? "info" : "warning", "campaign_profile",
                    applied ? "Campaign save profile updated." : "Campaign save profile change rejected.",
                    {{"action", request.campaignProfileAction}, {"session", request.sessionLoadingSessionId},
                     {"error", error}});
            const auto payload = experimental::EncodeGameCommandResult(request, request.command, result);
            return CompleteExperimentalRequest(request.requestId, reservation.epoch,
                protocol::MessageType::GameCommandResult, frame.payload, payload);
        }

        if (performanceSetting) {
            GameCommandExecutionResult result{};
            if (!runtime_features::SetByName(
                    request.performanceIsolationModule,
                    request.performanceIsolationEnabled)) {
                result.statusCode = "PERFORMANCE_MODULE_INVALID";
                result.message = "Unknown performance isolation module.";
            } else {
                result.executed = true;
                result.statusCode = "PERFORMANCE_MODULE_APPLIED";
                std::ostringstream message;
                message << "module=" << request.performanceIsolationModule
                        << " enabled=" << (request.performanceIsolationEnabled ? 1 : 0)
                        << " mask=" << runtime_features::Mask();
                result.message = message.str();
                if (diagnostics_ != nullptr &&
                    runtime_features::Enabled(runtime_features::Feature::NativeDiagnostics)) {
                    diagnostics_->Log("info", "performance_isolation_configured",
                        "Launcher performance isolation setting applied to the injected process.",
                        {{"module", request.performanceIsolationModule},
                         {"enabled", request.performanceIsolationEnabled ? "1" : "0", true},
                         {"featureMask", Diagnostics::Unsigned(runtime_features::Mask()), true},
                         {"source", "launcher-ipc"}, {"requestId", request.requestId}});
                }
            }
            const auto payload = experimental::EncodeGameCommandResult(request, request.command, result);
            return CompleteExperimentalRequest(request.requestId, reservation.epoch,
                protocol::MessageType::GameCommandResult, frame.payload, payload);
        }

        if (ws3Setting) {
            world_actions::SetEmptyCallbackTest(request.worldSync3EmptyCallbackEnabled);
            GameCommandExecutionResult result{};
            result.executed = true;
            result.statusCode = "WS3_EMPTY_CALLBACK_APPLIED";
            result.message = request.worldSync3EmptyCallbackEnabled
                ? "ws3EmptyCallbackTest=1" : "ws3EmptyCallbackTest=0";
            if (diagnostics_ != nullptr) {
                diagnostics_->Log("info", "ws3_empty_callback_configured",
                    "Launcher IPC ProbeDirect callback diagnostic setting applied to the injected process; gameplay callback ROOTs remain blocked.",
                    {{"pid", Diagnostics::Unsigned(GetCurrentProcessId()), true},
                     {"ws3EmptyCallbackTest", request.worldSync3EmptyCallbackEnabled ? "1" : "0", true},
                     {"source", "launcher-ipc"}, {"requestId", request.requestId}});
            }
            const auto payload = experimental::EncodeGameCommandResult(request, request.command, result);
            return CompleteExperimentalRequest(request.requestId, reservation.epoch,
                protocol::MessageType::GameCommandResult, frame.payload, payload);
        }



        if (request.levelGeneration != 0 &&
            request.levelGeneration != currentGeneration) {
            GameCommandExecutionResult result{};
            result.statusCode = "EXPIRED";
            result.message =
                "The command targets an obsolete level generation.";
            const auto payload = experimental::EncodeGameCommandResult(
                request,
                request.command,
                result);
            return CompleteExperimentalRequest(
                request.requestId,
                reservation.epoch,
                protocol::MessageType::GameCommandResult,
                frame.payload,
                payload);
        }



        if (gameCommands_ == nullptr ||
            !gameCommands_->CanExecute(request.command) ||
            dispatcher_ == nullptr) {
            diagnostics_->Log(
                "warning",
                "game_command_unresolved",
                "The requested command has no validated native function.",
                {{"command", request.command},
                 {"gameCommandServicesAttached",
                  Diagnostics::Boolean(gameCommands_ != nullptr),
                  true},
                 {"dispatcherAttached",
                  Diagnostics::Boolean(dispatcher_ != nullptr),
                  true},
                 {"profileId", profileId_},
                 {"resolutionMode", resolutionMode_}});
            GameCommandExecutionResult result{};
            result.statusCode = "INVALID_COMMAND";
            result.message =
                "The command is invalid or its native function is unresolved.";
            const auto payload = experimental::EncodeGameCommandResult(
                request,
                request.command,
                result);
            return CompleteExperimentalRequest(
                request.requestId,
                reservation.epoch,
                protocol::MessageType::GameCommandResult,
                frame.payload,
                payload);
        }
        const auto engineThreadAtSubmission = dispatcher_->Snapshot();
        if (!engineThreadAtSubmission.mainThreadHookInstalled &&
            !engineThreadAtSubmission.managerUpdateHookInstalled &&
            !engineThreadAtSubmission.readinessProducerHookInstalled) {
            GameCommandExecutionResult result{};
            result.statusCode = "TASK_FAULT";
            result.message = "Engine hooks are unavailable.";
            const auto payload = experimental::EncodeGameCommandResult(
                request,
                request.command,
                result);
            return CompleteExperimentalRequest(
                request.requestId,
                reservation.epoch,
                protocol::MessageType::GameCommandResult,
                frame.payload,
                payload);
        }

        const auto realtimeMovement =
            request.command == "CHARACTER_DIRECT_MOVE";
        const auto realtimeReplication = realtimeMovement &&
            request.characterDirectMove.worldSpace &&
            request.characterDirectMove.hasReplicationPosition &&
            request.characterDirectMove.hasReplicationYaw &&
            request.characterDirectMove.hasMoveType;

        // Remote-player replication is state publication, not a transactional
        // engine action. ArmWorldReplication/HoldWorldReplicationIdle publish
        // atomics that CharacterManager consumes on its next native tick.
        // Queuing every 16-33 ms snapshot onto the game thread made the engine
        // perform validation, JSON response work and WRITE_THROUGH diagnostics
        // in its hot callback. It also added a full dispatch round-trip before
        // each newest snapshot could replace the previous one. Execute only
        // this validated network-replication form on the IPC worker thread; all
        // actual Character memory writes, animation transitions and locomotion
        // still occur on CharacterManager through PumpOnEngineThread/hooks.
        if (realtimeReplication && engineThreadAtSubmission.ready) {
            auto result = gameCommands_->ExecuteCharacterDirectMove(
                request.characterDirectMove);
            const auto nativeStatusCode = result.statusCode;
            const auto nativeMessage = result.message;
            result.statusCode = result.executed ? "EXECUTED" : "TASK_FAULT";
            result.message = result.executed
                ? "Real-time replication state published (" +
                    nativeStatusCode + "): " + nativeMessage
                : "Real-time replication publication failed (" +
                    nativeStatusCode + "): " + nativeMessage;
            if (!result.executed) {
                diagnostics_->Log(
                    "warning",
                    "realtime_replication_failed",
                    result.message,
                    {{"command", request.command},
                     {"nativeStatusCode", nativeStatusCode},
                     {"levelGeneration",
                      Diagnostics::Unsigned(request.levelGeneration),
                      true}});
            }
            const auto payload = experimental::EncodeGameCommandResult(
                request,
                request.command,
                result);
            return CompleteExperimentalRequest(
                request.requestId,
                reservation.epoch,
                protocol::MessageType::GameCommandResult,
                frame.payload,
                payload);
        }

        struct PendingGameCommand final {
            PendingGameCommand(
                experimental::GameCommandRequest requestValue,
                std::vector<std::byte> payloadValue,
                const std::uint64_t epochValue)
                : request(std::move(requestValue)),
                  requestPayload(std::move(payloadValue)),
                  epoch(epochValue) {}

            experimental::GameCommandRequest request;
            std::vector<std::byte> requestPayload;
            std::uint64_t epoch{};
            std::mutex responseMutex;
            bool finalSent{};
        };
        auto pending = std::make_shared<PendingGameCommand>(
            request,
            frame.payload,
            reservation.epoch);
        const bool waitingForEngineThread =
            !engineThreadAtSubmission.ready;
        GameThreadCommand command{};
        command.requestId = NextExperimentalDispatchId();
        command.levelGeneration = request.levelGeneration;
        // Remote movement is real-time state publication. Keep the normal
        // 10 s window for transactional commands, but drop stale movement
        // work after a fraction of a second.
        command.expiresAtTickMs = GetTickCount64() +
            (realtimeMovement ? 250ULL : 10000ULL);
        command.kind = GameThreadCommandKind::ExecuteGameCommand;
        command.execute = [this, pending] {
            GameCommandExecutionResult result{};
            if (pending->request.command == "SpawnCharacter") {
                result = gameCommands_->ExecuteSpawnCharacter(
                    pending->request.spawnCharacter);
            } else if (
                pending->request.command == "SpawnNetZCharacter") {
                result = gameCommands_->ExecuteSpawnNetZCharacter(
                    pending->request.spawnNetZCharacter);
            } else if (
                pending->request.command == "CHARACTER_DIRECT_MOVE") {
                result = gameCommands_->ExecuteCharacterDirectMove(
                    pending->request.characterDirectMove);
            } else if (
                pending->request.command == "CHARACTER_DIRECT_STEP") {
                result = gameCommands_->ExecuteCharacterDirectStep(
                    pending->request.characterDirectStep);
            } else if (
                pending->request.command == "CHARACTER_ACTION_TEST") {
                result = gameCommands_->ExecuteCharacterActionTest(
                    pending->request.characterActionTest);
            } else if (
                pending->request.command == "CHARACTER_AMMO_REFILL") {
                result = gameCommands_->ExecuteCharacterAmmoRefill(
                    pending->request.characterAmmoRefill);
            } else if (
                pending->request.command == "REMOTE_EQUIPMENT_PROVISION") {
                result = gameCommands_->ExecuteRemoteEquipmentProvision(
                    pending->request.remoteEquipmentProvision);
            } else if (
                pending->request.command == "WORLD_REGISTRY_PROBE") {
                result = gameCommands_->ExecuteWorldRegistryProbe(
                    pending->request.worldRegistryProbe);
            } else if (
                pending->request.command == "WORLD_MANIFEST_LOAD") {
                result = gameCommands_->ExecuteWorldManifestLoad(
                    pending->request.worldManifestLoad);
            } else if (
                pending->request.command == "WORLD_NET_PROBE") {
                result = gameCommands_->ExecuteWorldNetProbe(
                    pending->request.worldNetProbe);
            } else if (
                pending->request.command == "WORLD_INTERACTION_STATS") {
                result = gameCommands_->ExecuteWorldInteractionStats();
            } else if (
                pending->request.command == "WORLD_COMPOSITE_INPUT_STATS") {
                result = gameCommands_->ExecuteWorldCompositeInputStats();
            } else if (
                pending->request.command == "WORLD_STATE_DISCOVERY_BASELINE") {
                result = gameCommands_->ExecuteWorldStateDiscoveryBaseline();
            } else if (
                pending->request.command == "WORLD_STATE_DISCOVERY_AFTER") {
                result = gameCommands_->ExecuteWorldStateDiscoveryAfter();
            } else if (
                pending->request.command == "WORLD_STATE_DISCOVERY_STATUS") {
                result = gameCommands_->ExecuteWorldStateDiscoveryStatus();
            } else if (
                pending->request.command == "ALIEN_PVP_PROBE") {
                result = gameCommands_->ExecuteAlienPvPProbe();
            } else if (
                pending->request.command == "ALIEN_PVP_BIND") {
                result = gameCommands_->ExecuteAlienPvPBind(pending->request.alienPvPOwnerPeer);
            } else if (
                pending->request.command == "ALIEN_PVP_RELEASE") {
                result = gameCommands_->ExecuteAlienPvPRelease();
            } else if(pending->request.command=="ALIEN_PVP_VENTS") {
                result=gameCommands_->ExecuteAlienPvPVents(pending->request.alienPvPVentPage,pending->request.alienPvPVentGo,pending->request.alienPvPVentIncarnation);
            } else if (
                pending->request.command == "RETURN_TO_FRONTEND") {
                result = gameCommands_->ExecuteReturnToFrontend();
            } else if (
                pending->request.command == "CINEMATIC_SYNC_STATUS") {
                const auto stage=alien_pvp_input::PollCinematic();
                result.executed=stage!=alien_pvp_input::CinematicSignal::Unavailable;
                switch (stage) {
                case alien_pvp_input::CinematicSignal::HostSkip:
                    result.statusCode="CINEMATIC_SYNC_HOST_SKIP";break;
                case alien_pvp_input::CinematicSignal::ClientBlocked:
                    result.statusCode="CINEMATIC_SYNC_CLIENT_BLOCKED";break;
                case alien_pvp_input::CinematicSignal::Playing:
                    result.statusCode="CINEMATIC_SYNC_PLAYING";break;
                case alien_pvp_input::CinematicSignal::Idle:
                    result.statusCode="CINEMATIC_SYNC_IDLE";break;
                default:result.statusCode="CINEMATIC_SYNC_UNAVAILABLE";break;
                }
                result.message=result.statusCode;
            } else if (pending->request.command=="CINEMATIC_SYNC_SKIP") {
                result.executed=alien_pvp_input::ApplyCinematicSkip();
                result.statusCode=result.executed
                    ? "CINEMATIC_SYNC_SKIP_APPLIED" : "CINEMATIC_SYNC_SKIP_NOT_ACTIVE";
                result.message=result.statusCode;
            } else if (
                pending->request.command == "MISSION_SYNC_CONFIG" || pending->request.command == "MISSION_SYNC_APPLY") {
                result = gameCommands_->ExecuteMissionSync(pending->request.missionSync,
                    pending->request.command == "MISSION_SYNC_CONFIG");
            } else if (
                pending->request.command == "ITEM_SYNC_APPLY") {
                result = gameCommands_->ExecuteItemSyncApply(
                    pending->request.itemSyncApply);
            } else if (
                pending->request.command == "ITEM_SYNC_SPAWN") {
                result = gameCommands_->ExecuteItemSyncSpawn(
                    pending->request.itemSyncSpawn);
            } else {
                result = gameCommands_->ExecutePopupMessage(
                    pending->request.popupMessage);
            }
            const auto nativeStatusCode = result.statusCode;
            const auto nativeMessage = result.message;
            const bool remoteEquipmentRetry =
                !result.executed &&
                pending->request.command == "REMOTE_EQUIPMENT_PROVISION" &&
                nativeStatusCode == "REMOTE_EQUIPMENT_NOT_READY";
            const bool remoteEquipmentTerminal =
                !result.executed &&
                pending->request.command == "REMOTE_EQUIPMENT_PROVISION" &&
                nativeStatusCode.rfind("REMOTE_EQUIPMENT_", 0U) == 0U;
            // WORLD_MANIFEST_PREPARING is a retryable state (the manifest worker
            // is still building its runtime index), and ItemSync statuses tell
            // the Launcher whether to retry. Collapsing them into TASK_FAULT
            // made the Launcher abandon WorldSync activation for good.
            const bool preservedStatus =
                !result.executed &&
                ((pending->request.command == "WORLD_MANIFEST_LOAD" &&
                  nativeStatusCode == "WORLD_MANIFEST_PREPARING") ||
                 nativeStatusCode.rfind("ITEM_SYNC_", 0U) == 0U ||
                 nativeStatusCode.rfind("ALIEN_PVP_", 0U) == 0U ||
                 nativeStatusCode.rfind("CINEMATIC_SYNC_", 0U) == 0U);
            if (remoteEquipmentRetry || remoteEquipmentTerminal || preservedStatus ||
                pending->request.command=="CINEMATIC_SYNC_STATUS" ||
                pending->request.command=="CINEMATIC_SYNC_SKIP") {
                // Preserve the native equipment provisioning stage/status so
                // the Launcher can distinguish convergence from terminal
                // fail-closed errors without collapsing them into TASK_FAULT.
                result.statusCode = nativeStatusCode;
                result.message = nativeMessage;
            } else {
                result.statusCode = result.executed ? "EXECUTED" : "TASK_FAULT";
                result.message = result.executed
                    ? "Command executed on the engine thread (" +
                        nativeStatusCode + "): " + nativeMessage
                    : "The native command failed (" + nativeStatusCode +
                        "): " + nativeMessage;
            }
            const auto payload = experimental::EncodeGameCommandResult(
                pending->request,
                pending->request.command,
                result);
            {
                std::lock_guard lock(pending->responseMutex);
                pending->finalSent = true;
                CompleteExperimentalRequest(
                    pending->request.requestId,
                    pending->epoch,
                    protocol::MessageType::GameCommandResult,
                    pending->requestPayload,
                    payload);
            }
            if ((pending->request.command != "CHARACTER_DIRECT_MOVE" && pending->request.command!="ALIEN_PVP_VENTS") ||
                !result.executed) {
                diagnostics_->Log(
                    result.executed ? "info" : "warning",
                    "game_command_completed",
                    result.message,
                    {{"command", pending->request.command},
                     {"statusCode", result.statusCode},
                     {"nativeStatusCode", nativeStatusCode},
                     {"levelGeneration",
                      Diagnostics::Unsigned(pending->request.levelGeneration),
                      true}});
            }
        };
        command.completion = [this, pending](
                                 const DispatchCompletion completion) {
            if (completion == DispatchCompletion::Executed) {
                return;
            }
            GameCommandExecutionResult result{};
            result.statusCode =
                completion == DispatchCompletion::Expired
                    ? "EXPIRED"
                    : (completion == DispatchCompletion::GenerationInvalidated
                           ? "EXPIRED"
                           : "TASK_FAULT");
            result.message =
                "The command was not executed on the engine thread.";
            const auto payload = experimental::EncodeGameCommandResult(
                pending->request,
                pending->request.command,
                result);
            std::lock_guard lock(pending->responseMutex);
            pending->finalSent = true;
            CompleteExperimentalRequest(
                pending->request.requestId,
                pending->epoch,
                protocol::MessageType::GameCommandResult,
                pending->requestPayload,
                payload);
        };
        const auto queued = dispatcher_->Enqueue(std::move(command));
        if (queued == DispatchEnqueueResult::Queued) {
            if (!realtimeMovement) {
                diagnostics_->Log(
                    "info",
                    "game_command_queued",
                    waitingForEngineThread
                        ? "Command queued while waiting for the engine thread."
                        : "Command queued for the next engine callback.",
                    {{"command", request.command},
                     {"levelGeneration",
                      Diagnostics::Unsigned(request.levelGeneration),
                      true}});
            }
            if (waitingForEngineThread) {
                std::lock_guard lock(pending->responseMutex);
                if (!pending->finalSent) {
                    GameCommandExecutionResult result{};
                    result.statusCode =
                        "QUEUED_WAITING_FOR_ENGINE_THREAD";
                    result.message = "Command queued.";
                    const auto payload =
                        experimental::EncodeGameCommandResult(
                            request,
                            request.command,
                            result);
                    if (!QueueExperimental(
                            protocol::MessageType::GameCommandResult,
                            payload)) {
                        return false;
                    }
                }
            }
            return true;
        }

        GameCommandExecutionResult result{};
        result.statusCode =
            queued == DispatchEnqueueResult::GenerationMismatch
                ? "EXPIRED"
                : (queued == DispatchEnqueueResult::CapacityReached
                       ? "QUEUE_FULL"
                       : (queued == DispatchEnqueueResult::Expired
                              ? "EXPIRED"
                              : "INVALID_COMMAND"));
        result.message =
            "The engine-thread queue rejected the command.";
        const auto payload = experimental::EncodeGameCommandResult(
            request,
            request.command,
            result);
        return CompleteExperimentalRequest(
            request.requestId,
            reservation.epoch,
            protocol::MessageType::GameCommandResult,
            frame.payload,
            payload);
    }
    case protocol::MessageType::DiagnosticSnapshotRequest: {
        ai_sync::LogDiagnostics(*diagnostics_);
        voice_sync::LogDiagnostics(*diagnostics_);
        item_sync::LogDiagnostics(*diagnostics_);
        throwable_sync::LogDiagnostics(*diagnostics_);
        mission_sync::LogDiagnostics(*diagnostics_);
        loading_context_guard::LogDiagnostics(*diagnostics_);
        experimental::DiagnosticSnapshotRequest request;
        if (!sessionConfigured_.load(std::memory_order_acquire) ||
            !netzReadySent_.load(std::memory_order_acquire) ||
            !experimental::DecodeDiagnosticSnapshotRequest(
                frame.payload,
                request)) {
            return false;
        }
        const auto currentGeneration = generations_->CurrentGeneration();
        if (request.levelGeneration != 0 &&
            request.levelGeneration != currentGeneration) {
            const auto error = experimental::EncodeExperimentalError(
                request,
                "LEVEL_GENERATION_MISMATCH",
                "The diagnostics request targets an obsolete level generation.",
                {"current_level_generation"});
            return QueueExperimental(
                protocol::MessageType::ExperimentalError,
                error);
        }
        request.levelGeneration = static_cast<std::uint32_t>(
            (std::min<std::uint64_t>)(
                currentGeneration,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)())));
        const auto engineThread = dispatcher_ == nullptr
            ? GameThreadDispatcherSnapshot{}
            : dispatcher_->Snapshot();
        const auto locomotion = character_direct_step_runtime::Snapshot();
        const auto hidingHistory = player_hiding::History();
        const auto hidingPresentation = player_hiding_presentation::Snapshot(locomotion.characterAddress);
        const auto hidingDoor = hiding_door_presentation::Diagnostics();
        const auto hidingLocal = player_hiding_local::Diagnostics();
        const auto hidingFollow = character_direct_step_runtime::HidingFollowSnapshot();
        const auto hidingAudio = hiding_presentation_audio::Snapshot(locomotion.characterAddress);
        diagnostics_->Log("info", "native_hiding_presentation_snapshot", "Remote hiding presentation state.",
            {{"audioStage", hidingAudio.stage, false},
             {"audioEvent", hidingAudio.eventName, false},
             {"audioPosted", Diagnostics::Unsigned(hidingAudio.posted), true},
             {"audioFailed", Diagnostics::Unsigned(hidingAudio.failed), true},
             {"audioPlayingId", Diagnostics::Unsigned(hidingAudio.playingId), true},
             {"audioFilterInstalled", Diagnostics::Boolean(hidingAudio.filterInstalled), true},
             {"audioMetadataObserved", Diagnostics::Unsigned(hidingAudio.metadataObserved), true},
             {"audioMetadataSuppressed", Diagnostics::Unsigned(hidingAudio.metadataSuppressed), true},
             {"localEntering", Diagnostics::Unsigned(hidingLocal.entering), true},
             {"localHidden", Diagnostics::Unsigned(hidingLocal.hidden), true},
             {"localExiting", Diagnostics::Unsigned(hidingLocal.exiting), true},
             {"localUnavailable", Diagnostics::Unsigned(hidingLocal.unavailable), true},
             {"exitRevealDeferredFrames", Diagnostics::Unsigned(hidingHistory.exitRevealDeferredFrames), true},
             {"clipFollowFrames", Diagnostics::Unsigned(hidingFollow.frames), true},
             {"clipFollowRejected", Diagnostics::Unsigned(hidingFollow.rejected), true},
             {"hiddenExitHeadingFrames", Diagnostics::Unsigned(hidingFollow.exitHeadingFrames), true},
             {"entryVisualPublished", Diagnostics::Unsigned(hidingFollow.frames), true},
             {"entryVisualApplied", Diagnostics::Unsigned(hidingFollow.refAlignFrames), true},
             {"entryEntityMovedMm", Diagnostics::Unsigned(hidingFollow.maxPullMm), true},
             {"phase", Diagnostics::Unsigned(hidingPresentation.phase), true},
             {"transition", Diagnostics::Unsigned(hidingPresentation.transition), true},
             {"stage", hidingPresentation.stage, false},
             {"applied", Diagnostics::Unsigned(hidingPresentation.applied), true},
             {"failed", Diagnostics::Unsigned(hidingPresentation.failed), true},
             {"stale", Diagnostics::Unsigned(hidingPresentation.stale), true},
             {"completed", Diagnostics::Unsigned(hidingPresentation.completed), true},
             {"active", Diagnostics::Boolean(hidingPresentation.active), true},
             {"resource", Diagnostics::Unsigned(hidingPresentation.resourceToken), true},
             {"clip", Diagnostics::Unsigned(hidingPresentation.animationToken), true},
             {"doorCaptured", Diagnostics::Unsigned(hidingDoor.captured), true},
             {"doorQueued", Diagnostics::Unsigned(hidingDoor.queued), true},
             {"doorUnresolved", Diagnostics::Unsigned(hidingDoor.unresolved), true},
             {"doorRejected", Diagnostics::Unsigned(hidingDoor.rejected), true},
             {"doorFaults", Diagnostics::Unsigned(hidingDoor.nativeFaults), true},
             {"doorTarget", Diagnostics::Unsigned(hidingDoor.lastTargetShortGuid), true},
             {"doorHierarchy", Diagnostics::Unsigned(hidingDoor.lastHierarchyHash), true}});
        player_animation_clip_capture::Export(*diagnostics_);
        animation_sync::Export(*diagnostics_, locomotion.characterAddress);
        diagnostics_->Log(
            "info",
            "native_runtime_snapshot",
            "Native runtime state captured for diagnostics export.",
            {{"engineHooksInstalled",
              Diagnostics::Boolean(
                  engineThread.mainThreadHookInstalled ||
                  engineThread.managerUpdateHookInstalled ||
                  engineThread.readinessProducerHookInstalled),
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
              true},
             {"engineThreadReady",
              Diagnostics::Boolean(engineThread.ready),
              true},
             {"engineThreadId",
              Diagnostics::Unsigned(engineThread.engineThreadId),
              true},
             {"engineThreadSource",
              std::string(ToString(engineThread.source))},
             {"firstEngineCallbackPerformanceCounter",
              Diagnostics::Unsigned(
                  engineThread.firstCallbackPerformanceCounter),
              true},
             {"lastEngineCallbackPerformanceCounter",
              Diagnostics::Unsigned(
                  engineThread.lastCallbackPerformanceCounter),
              true},
             {"engineCallbackCount",
              Diagnostics::Unsigned(engineThread.callbackCount),
              true},
             {"engineCallbackMismatchCount",
              Diagnostics::Unsigned(engineThread.mismatchCount),
              true},
             {"pendingCommandCount",
              Diagnostics::Unsigned(engineThread.pendingCount),
              true},
             {"lastExecutionTickMs",
              Diagnostics::Unsigned(engineThread.lastExecutionTickMs),
              true},
             {"currentLevelGeneration",
              Diagnostics::Unsigned(engineThread.generation),
              true},
             {"remoteLocomotionCharacter",
              Diagnostics::Pointer(reinterpret_cast<void*>(
                  locomotion.characterAddress))},
             {"remoteLocomotionCycle",
              Diagnostics::Pointer(reinterpret_cast<void*>(
                  locomotion.lastStateAddress))},
             {"remoteLocomotionLastHookCharacter",
              Diagnostics::Pointer(reinterpret_cast<void*>(
                  locomotion.lastCharacter))},
             {"remoteLocomotionArmed",
              Diagnostics::Boolean(locomotion.armed),
              true},
             {"remoteLocomotionMoveActive",
              Diagnostics::Boolean(locomotion.moveActive),
              true},
             {"remoteLocomotionNativeCycleState",
              Diagnostics::Unsigned(locomotion.nativeCycleState),
              true},
             {"remoteLocomotionNativeStateActive",
              Diagnostics::Boolean(locomotion.nativeStateActive),
              true},
             {"remoteLocomotionEnterMoveTransitions",
              Diagnostics::Unsigned(locomotion.enterMoveTransitions),
              true},
             {"remoteLocomotionUpdateMoveTransitions",
              Diagnostics::Unsigned(locomotion.updateMoveTransitions),
              true},
             {"remoteLocomotionExitMoveTransitions",
              Diagnostics::Unsigned(locomotion.exitMoveTransitions),
              true},
             {"remoteLocomotionIdleHoldApplications",
              Diagnostics::Unsigned(locomotion.idleHoldApplications),
              true},
             {"remoteLocomotionEnginePumpApplications",
              Diagnostics::Unsigned(locomotion.enginePumpApplications),
              true},
             {"remoteLocomotionAiActivationSuppressions",
              Diagnostics::Unsigned(locomotion.aiActivationSuppressions),
              true},
             {"remoteLocomotionAiIdleSuppressions",
              Diagnostics::Unsigned(locomotion.aiIdleSuppressions),
              true},
             {"remoteLocomotionNativeIdleApplications",
              Diagnostics::Unsigned(locomotion.nativeIdleApplications),
              true},
             {"remoteLocomotionFollowerApplications",
              Diagnostics::Unsigned(locomotion.followerApplications),
              true},
             {"remoteLocomotionEffectiveMoveTypeApplications",
              Diagnostics::Unsigned(
                  locomotion.effectiveMoveTypeApplications),
              true},
             {"remoteLocomotionActivationAttempts",
              Diagnostics::Unsigned(locomotion.activationAttempts),
              true},
             {"remoteLocomotionActivationApplications",
              Diagnostics::Unsigned(locomotion.activationApplications),
              true},
             {"remoteLocomotionActivationReuses",
              Diagnostics::Unsigned(locomotion.activationReuses),
              true},
             {"remoteLocomotionActivationFailures",
              Diagnostics::Unsigned(locomotion.activationFailures),
              true},
             {"remoteLocomotionNativeStateLosses",
              Diagnostics::Unsigned(locomotion.nativeStateLosses),
              true},
             {"remoteLocomotionReacquireAttempts",
              Diagnostics::Unsigned(
                  locomotion.nativeStateReacquireAttempts),
              true},
             {"remoteLocomotionReacquireSuccesses",
              Diagnostics::Unsigned(
                  locomotion.nativeStateReacquireSuccesses),
              true},
             {"remoteLocomotionPostAiHookHits",
              Diagnostics::Unsigned(locomotion.hookHits),
              true},
             {"remoteLocomotionControlApplications",
              Diagnostics::Unsigned(locomotion.requestApplications),
              true},
             {"remoteLocomotionFinalSubmissions",
              Diagnostics::Unsigned(locomotion.finalSubmissions),
              true},
             {"remoteLocomotionTransformApplications",
              Diagnostics::Unsigned(locomotion.transformApplications),
              true},
             {"remoteLocomotionTeleportApplications",
              Diagnostics::Unsigned(locomotion.teleportApplications),
              true},
             {"remoteLocomotionHardResyncAttempts",
              Diagnostics::Unsigned(locomotion.hardResyncAttempts),
              true},
             {"remoteLocomotionHardResyncApplications",
              Diagnostics::Unsigned(locomotion.hardResyncApplications),
              true},
             {"remoteLocomotionStopApplications",
              Diagnostics::Unsigned(locomotion.stopApplications),
              true},
             {"remoteLocomotionProtectedFailures",
              Diagnostics::Unsigned(locomotion.protectedFailures),
              true},
             {"remotePostureApplications", Diagnostics::Unsigned(locomotion.posture.postureApplications), true},
             {"remotePostureFailures", Diagnostics::Unsigned(locomotion.posture.postureFailures), true},
             {"remoteCrouchTransitions", Diagnostics::Unsigned(locomotion.posture.crouchTransitions), true},
             {"remoteCrouchDesired", Diagnostics::Boolean(locomotion.posture.desiredCrouched), true},
             {"remoteDeepCrouchDesired", Diagnostics::Boolean(locomotion.posture.desiredDeepCrouched), true},
             {"remoteCrouchApplied", Diagnostics::Boolean(locomotion.posture.appliedCrouched), true},
             {"remoteAnimationContextRepairs", Diagnostics::Unsigned(locomotion.posture.animationContextRepairs), true},
             {"remoteAnimationRefreshes", Diagnostics::Unsigned(locomotion.posture.animationRefreshes), true},
             {"remoteAnimationRefreshFailures", Diagnostics::Unsigned(locomotion.posture.animationRefreshFailures), true},
             {"remoteCrouchedAimApplications", Diagnostics::Unsigned(locomotion.posture.crouchedAimApplications), true},
             {"remoteLastAimTree", Diagnostics::Unsigned(locomotion.posture.lastAimTree), true},
             {"remoteLastAimStance", Diagnostics::Unsigned(locomotion.posture.lastAimStance), true},
             {"remoteAnimationContextReadable", Diagnostics::Boolean(locomotion.posture.animationContextReadable), true},
             {"remoteAnimationDatabase", Diagnostics::Unsigned(locomotion.posture.animationDatabase), true},
             {"remoteAnimationContext", Diagnostics::Unsigned(locomotion.posture.animationContext), true},
             {"remoteAnimationContextReferences", Diagnostics::Unsigned(locomotion.posture.animationContextReferences), true},
             {"remoteAnimationIdlePath", Diagnostics::Unsigned(locomotion.posture.animationIdlePath), true},
             {"remoteAnimationWalkPath", Diagnostics::Unsigned(locomotion.posture.animationWalkPath), true},
             {"remoteLastCrouchDatabase", Diagnostics::Unsigned(locomotion.posture.lastCrouchDatabase), true},
             {"remoteLastCrouchContext", Diagnostics::Unsigned(locomotion.posture.lastCrouchContext), true},
             {"remoteLastCrouchReferences", Diagnostics::Unsigned(locomotion.posture.lastCrouchReferences), true},
             {"remoteLastCrouchIdlePath", Diagnostics::Unsigned(locomotion.posture.lastCrouchIdlePath), true},
             {"remoteLastCrouchWalkPath", Diagnostics::Unsigned(locomotion.posture.lastCrouchWalkPath), true},
             {"remoteDeepCrouchApplied", Diagnostics::Boolean(locomotion.posture.appliedDeepCrouched), true},
             {"remotePostureStage", locomotion.posture.stage, false},
             {"remoteTraversalApplications", Diagnostics::Unsigned(locomotion.posture.traversalApplications), true},
             {"remoteTraversalRejections", Diagnostics::Unsigned(locomotion.posture.traversalRejections), true},
             {"remoteTraversalActiveRevision", Diagnostics::Unsigned(locomotion.posture.activeRevision), true},
             {"remoteTraversalPending", Diagnostics::Unsigned(locomotion.posture.pendingTraversals), true},
             {"remoteTorchApplications", Diagnostics::Unsigned(locomotion.torch.applications), true},
             {"remoteTorchFailures", Diagnostics::Unsigned(locomotion.torch.failures), true},
             {"remoteTorchCreations", Diagnostics::Unsigned(locomotion.torch.creations), true},
             {"remoteTorchDesired", locomotion.torch.desired ? "true" : "false", true},
             {"remoteTorchEmitted", locomotion.torch.emitted ? "true" : "false", true},
             {"remoteTorchTransformSubmitted", Diagnostics::Boolean(locomotion.torch.transformSubmitted), true},
             {"remoteTorchSocketOrigin", Diagnostics::Boolean(locomotion.torch.socketOrigin), true},
             {"remoteTorchHasDirection", Diagnostics::Boolean(locomotion.torch.hasDirection), true},
             {"remoteTorchRenderer", Diagnostics::Unsigned(locomotion.torch.renderer), true},
             {"remoteTorchLastOnRenderer", Diagnostics::Unsigned(locomotion.torch.lastOnRenderer), true},
             {"remoteTorchOnFrames", Diagnostics::Unsigned(locomotion.torch.onFrames), true},
             {"remoteTorchRenderOnFrames", Diagnostics::Unsigned(locomotion.torch.renderOnFrames), true},
             {"remoteTorchLastOnStage", locomotion.torch.lastOnStage, false},
             {"remoteTorchStage", locomotion.torch.stage, false},
             {"remoteHidingDesired", Diagnostics::Boolean(locomotion.hiding.desired), true},
             {"remoteHidingConcealed", Diagnostics::Boolean(locomotion.hiding.concealed), true},
             {"remoteHidingSpotLocked", Diagnostics::Boolean(locomotion.hiding.spotLocked), true},
             {"remoteHidingHides", Diagnostics::Unsigned(locomotion.hiding.hides), true},
             {"remoteHidingShows", Diagnostics::Unsigned(locomotion.hiding.shows), true},
             {"remoteHidingFailures", Diagnostics::Unsigned(locomotion.hiding.failures), true},
             {"remoteHidingSpotsLocked", Diagnostics::Unsigned(locomotion.hiding.spotsLocked), true},
             {"remoteHidingSpotsReleased", Diagnostics::Unsigned(locomotion.hiding.spotsReleased), true},
             {"remoteHidingSpotsMissing", Diagnostics::Unsigned(locomotion.hiding.spotsMissing), true},
             {"remoteHidingStage", locomotion.hiding.stage, false},
             {"playerHidingHistoryHiddenEdges", Diagnostics::Unsigned(hidingHistory.hiddenEdges), true},
             {"playerHidingHistoryVisibleEdges", Diagnostics::Unsigned(hidingHistory.visibleEdges), true},
             {"playerHidingHistoryHides", Diagnostics::Unsigned(hidingHistory.hides), true},
             {"playerHidingHistoryShows", Diagnostics::Unsigned(hidingHistory.shows), true},
             {"playerHidingHistoryFailures", Diagnostics::Unsigned(hidingHistory.failures), true},
             {"playerHidingHistoryLastHiddenRequestedCharacter", Diagnostics::Pointer(
                  reinterpret_cast<const void*>(hidingHistory.lastHiddenRequestedCharacter)), false},
             {"playerHidingHistoryLastHiddenCharacter", Diagnostics::Pointer(
                  reinterpret_cast<const void*>(hidingHistory.lastHiddenCharacter)), false},
             {"playerHidingHistoryLastShownCharacter", Diagnostics::Pointer(
                  reinterpret_cast<const void*>(hidingHistory.lastShownCharacter)), false}});
        const auto ws3 = world_actions::Snapshot();
        const auto replication = replication_core::DiagnosticsSnapshot();
        const auto interactions = world_interactions::DiagnosticsSnapshot();
        const auto primitives = cathode_primitives::DiagnosticsSnapshot();
        const auto animated = world_animated::DiagnosticsSnapshot();
        diagnostics_->Log(
            "info",
            "native_worldsync_snapshot",
            "WorldSync counters captured for diagnostics export.",
            {{"currentLevelGeneration", Diagnostics::Unsigned(currentGeneration), true},
             {"ws3Installed", Diagnostics::Boolean(ws3.installed), true},
             {"ws3Enabled", Diagnostics::Boolean(ws3.enabled), true},
             {"ws3AuthorityBound", Diagnostics::Boolean(world_actions::SessionAuthorityBound()), true},
             {"ws3AuthorityPeerId", Diagnostics::Unsigned(world_actions::SessionAuthorityPeer()), true},
             {"ws3AuthorityIsHost", Diagnostics::Boolean(world_actions::SessionAuthorityHost()), true},
             {"ws3WorldBusActive", Diagnostics::Boolean(world_actions::WorldBusActive()), true},
             {"ws3EmptyCallbackTest", Diagnostics::Boolean(ws3.emptyCallbackTest), true},
             {"ws3AdmissionMask", Diagnostics::Unsigned(ws3.admissionSecurityMask), true},
             {"ws3Seen", Diagnostics::Unsigned(ws3.seen), true},
             {"ws3PublicCandidates", Diagnostics::Unsigned(ws3.publicCandidates), true},
             {"ws3RootSend", Diagnostics::Unsigned(ws3.rootsSent), true},
             {"ws3CompositeInputRootSend", Diagnostics::Unsigned(ws3.compositeInputRootsSent), true},
             {"ws3RelevanceCandidate", Diagnostics::Unsigned(ws3.relevanceCandidates), true},
             {"ws3RelevanceReplicateIntrinsic", Diagnostics::Unsigned(ws3.relevanceReplicateIntrinsic), true},
             {"ws3RelevanceReplicateCarrier", Diagnostics::Unsigned(ws3.relevanceReplicateCarrier), true},
             {"ws3RelevanceDefer", Diagnostics::Unsigned(ws3.relevanceDefer), true},
             {"ws3RelevanceNotRelevant", Diagnostics::Unsigned(ws3.relevanceNotRelevant), true},
             {"ws3RelevanceUnknown", Diagnostics::Unsigned(ws3.relevanceUnknown), true},
             {"ws3RelevanceReasonRouting", Diagnostics::Unsigned(ws3.relevanceReasonRouting), true},
             {"ws3RelevanceReasonSchedulerMaintenance", Diagnostics::Unsigned(ws3.relevanceReasonSchedulerMaintenance), true},
             {"ws3RelevanceReasonInternalState", Diagnostics::Unsigned(ws3.relevanceReasonInternalState), true},
             {"ws3RelevanceReasonNoSink", Diagnostics::Unsigned(ws3.relevanceReasonNoSink), true},
             {"ws3RelevanceReasonBetterDescendant", Diagnostics::Unsigned(ws3.relevanceReasonBetterDescendant), true},
             {"ws3RelevanceReasonCycleAmbiguous", Diagnostics::Unsigned(ws3.relevanceReasonCycleAmbiguous), true},
             {"ws3RelevanceReasonHistorySensitive", Diagnostics::Unsigned(ws3.relevanceReasonHistorySensitive), true},
             {"ws3RelevanceReasonStateTransitionCarrier", Diagnostics::Unsigned(ws3.relevanceReasonStateTransitionCarrier), true},
             {"ws3RelevanceReasonCompositeStateTransitionCarrier", Diagnostics::Unsigned(ws3.relevanceReasonCompositeStateTransitionCarrier), true},
             {"ws3RelevanceReasonCompositeRelevantFrontierRelaxed", Diagnostics::Unsigned(ws3.relevanceReasonCompositeRelevantFrontierRelaxed), true},
             {"ws3RelevanceInboundRejected", Diagnostics::Unsigned(ws3.relevanceInboundRejected), true},
             {"ws3RelevanceTopCount", Diagnostics::Unsigned(ws3.relevanceTopCount), true},
             {"ws3GenericRoot", Diagnostics::Unsigned(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::GenericRoot)]), true},
             {"ws3TargetedRoot", Diagnostics::Unsigned(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::TargetedRoot)]), true},
             {"ws3PayloadRoot", Diagnostics::Unsigned(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::PayloadRoot)]), true},
             {"ws3NeedsAdapter", Diagnostics::Unsigned(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::NeedsAdapter)]), true},
             {"ws3LocalOnly", Diagnostics::Unsigned(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::LocalOnly)]), true},
             {"ws3Unsafe", Diagnostics::Unsigned(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::Unsafe)]), true},
             {"ws3Unknown", Diagnostics::Unsigned(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::Unknown)]), true},
             {"ws3TargetOwnerDistinct", Diagnostics::Unsigned(ws3.targetOwnerDistinct), true},
             {"ws3TargetOwnerReject", Diagnostics::Unsigned(ws3.targetOwnerRejected), true},
             {"ws3TargetOwnerMatch", Diagnostics::Unsigned(ws3.targetOwnerMatched), true},
             {"ws3ContextUnsupported", Diagnostics::Unsigned(ws3.contextUnsupported), true},
             {"ws3ContextChainsCaptured", Diagnostics::Unsigned(ws3.contextChainsCaptured), true},
             {"ws3ContextFramesCaptured", Diagnostics::Unsigned(ws3.contextFramesCaptured), true},
             {"ws3ContextChainDepthPeak", Diagnostics::Unsigned(ws3.contextChainDepthPeak), true},
             {"ws3ContextChainOverflow", Diagnostics::Unsigned(ws3.contextChainOverflow), true},
             {"ws3ContextFramesReconstructed", Diagnostics::Unsigned(ws3.contextFramesReconstructed), true},
             {"ws3ActorUnresolved", Diagnostics::Unsigned(ws3.actorUnresolved), true},
             {"ws3ArgReject", Diagnostics::Unsigned(ws3.argRejected), true},
             {"ws3PayloadReject", Diagnostics::Unsigned(ws3.payloadRejected), true},
             {"ws3CallbackReject", Diagnostics::Unsigned(ws3.callbackRejected), true},
             {"ws3DelayReject", Diagnostics::Unsigned(ws3.delayRejected), true},
             {"ws3TailReject", Diagnostics::Unsigned(ws3.tailRejected), true},
             {"ws3ExternalRootSend", Diagnostics::Unsigned(ws3.externalRoots), true},
             {"ws3DelayedRootSend", Diagnostics::Unsigned(ws3.delayedRoots), true},
             {"ws3DescSkip", Diagnostics::Unsigned(ws3.descendantsSuppressed), true},
             {"ws3RemoteSkip", Diagnostics::Unsigned(ws3.remoteSuppressed), true},
             {"ws3IdentityReject", Diagnostics::Unsigned(ws3.identityRejected), true},
             {"ws3IdentityOwnerPublicInputValid",
              Diagnostics::Unsigned(ws3.identityOwner.publicInputValid), true},
             {"ws3ContextReject", Diagnostics::Unsigned(ws3.contextRejected), true},
             {"ws3QueueReject", Diagnostics::Unsigned(ws3.queueRejected), true},
             {"ws3PlayerCausalReject", Diagnostics::Unsigned(ws3.playerCausalRejected), true},
             {"ws3PlayerCausalScanNodes", Diagnostics::Unsigned(ws3.playerCausalNodesScanned), true},
             {"ws3PlayerCausalMappedNodes", Diagnostics::Unsigned(ws3.playerCausalNodesMapped), true},
             {"ws3LineageOverflow", Diagnostics::Unsigned(ws3.lineageOverflow), true},
             {"ws3LineageBuildInherit", Diagnostics::Unsigned(ws3.lineageBuildInherit), true},
             {"ws3LineageExecuteRestore", Diagnostics::Unsigned(ws3.lineageExecuteRestore), true},
             {"ws3LineageCallbackRestore", Diagnostics::Unsigned(ws3.lineageCallbackRestore), true},
             {"ws3Applied", Diagnostics::Unsigned(ws3.applied), true},
             {"ws3ApplyReject", Diagnostics::Unsigned(ws3.applyRejected), true},
             {"ws3Fault", Diagnostics::Unsigned(ws3.nativeFaults), true},
             {"ws3RootTraceWrites", Diagnostics::Unsigned(ws3.rootApplyTraceWrites), true},
             {"ws3RootJournalWrites", Diagnostics::Unsigned(ws3.rootApplyJournalWrites), true},
             {"ws3RootTraceStarted", Diagnostics::Unsigned(ws3.rootApplyTraceStarted), true},
             {"ws3RootTraceCompleted", Diagnostics::Unsigned(ws3.rootApplyTraceCompleted), true},
             {"ws3RootTraceLastStartedObservation", Diagnostics::Unsigned(ws3.rootApplyTraceLastStartedObservation), true},
             {"ws3RootTraceLastStartedSequence", Diagnostics::Unsigned(ws3.rootApplyTraceLastStartedSequence), true},
             {"ws3RootTraceLastCompletedObservation", Diagnostics::Unsigned(ws3.rootApplyTraceLastCompletedObservation), true},
             {"ws3RootTraceLastCompletedSequence", Diagnostics::Unsigned(ws3.rootApplyTraceLastCompletedSequence), true},
             {"ws3RootTraceCurrentStage", Diagnostics::Unsigned(ws3.rootApplyTraceCurrentStage), true},
             {"ws3ProbeDirectSeen", Diagnostics::Unsigned(ws3.probeDirectSeen), true},
             {"ws3ProbeDirectEligible", Diagnostics::Unsigned(ws3.probeDirectEligible), true},
             {"ws3ProbeDirectRootSend", Diagnostics::Unsigned(ws3.probeDirectRootsSent), true},
             {"ws3ProbeIdentityOwnerPublicInputValid",
              Diagnostics::Unsigned(ws3.probeIdentityOwner.publicInputValid), true},
             {"ws3ProbeIdentityOwnerPublicInputReject",
              Diagnostics::Unsigned(ws3.probeIdentityOwner.publicInputReject), true},
             {"ws3ProbeIdentityBoundaryLayoutReject",
              Diagnostics::Unsigned(ws3.probeIdentityBoundaryLayoutRejected), true},
             {"ws3ProbeDirectArgReject", Diagnostics::Unsigned(ws3.probeDirectArgRejected), true},
             {"ws3ProbeDirectPayload", Diagnostics::Unsigned(ws3.probeDirectPayload), true},
             {"ws3ProbeDirectCallback", Diagnostics::Unsigned(ws3.probeDirectCallback), true},
             {"ws3ProbeDirectDelay", Diagnostics::Unsigned(ws3.probeDirectDelay), true},
             {"ws3ProbeDirectTail", Diagnostics::Unsigned(ws3.probeDirectTail), true},
             {"ws3CallbackGameplayReject",
              Diagnostics::Unsigned(ws3.callbackGameplayRejected), true},
             {"ws3CallbackBoundaryCandidate", Diagnostics::Unsigned(ws3.callbackBoundaryCandidates), true},
             {"ws3CallbackBoundaryRoot", Diagnostics::Unsigned(ws3.callbackBoundaryRoots), true},
             {"ws3CallbackBoundaryPlayer", Diagnostics::Unsigned(ws3.callbackBoundaryPlayerRoots), true},
             {"ws3CallbackBoundaryAI", Diagnostics::Unsigned(ws3.callbackBoundaryAiRoots), true},
             {"ws3CallbackBoundaryUI", Diagnostics::Unsigned(ws3.callbackBoundaryUiRoots), true},
             {"ws3CallbackBoundaryAIMP", Diagnostics::Unsigned(ws3.callbackBoundaryAimpRoots), true},
             {"ws3CallbackBoundaryApplied", Diagnostics::Unsigned(ws3.callbackBoundaryApplied), true},
             {"ws3CallbackWorldContinuationReject",
              Diagnostics::Unsigned(ws3.callbackWorldContinuationRejected), true},
             {"ws3CallbackBoundaryUnknownReject",
              Diagnostics::Unsigned(ws3.callbackBoundaryUnknownRejected), true},
             {"ws3AdmissionForbiddenInputBypass", Diagnostics::Unsigned(ws3.admissionForbiddenInputBypassed), true},
             {"ws3AdmissionGuidPrefilterReject", Diagnostics::Unsigned(ws3.admissionGuidPrefilterRejected), true},
             {"ws3AdmissionGuidPrefilterBypass", Diagnostics::Unsigned(ws3.admissionGuidPrefilterBypassed), true},
             {"ws3AdmissionPayloadBypass", Diagnostics::Unsigned(ws3.admissionPayloadBypassed), true},
             {"ws3AdmissionCallbackShapeBypass", Diagnostics::Unsigned(ws3.admissionCallbackShapeBypassed), true},
             {"ws3AdmissionTailBypass", Diagnostics::Unsigned(ws3.admissionTailBypassed), true},
             {"ws3AdmissionPlayerTargetBypass", Diagnostics::Unsigned(ws3.admissionPlayerTargetBypassed), true},
             {"ws3AdmissionAITargetBypass", Diagnostics::Unsigned(ws3.admissionAiTargetBypassed), true},
             {"ws3AdmissionUITargetBypass", Diagnostics::Unsigned(ws3.admissionUiTargetBypassed), true},
             {"ws3AdmissionAIMPTargetBypass", Diagnostics::Unsigned(ws3.admissionAimpTargetBypassed), true},
             {"ws3AdmissionWorldContinuationBypass", Diagnostics::Unsigned(ws3.admissionWorldContinuationBypassed), true},
             {"ws3AdmissionUnknownCausalityBypass", Diagnostics::Unsigned(ws3.admissionUnknownCausalityBypassed), true},
             {"ws3AdmissionManifestOwnerInputBypass", Diagnostics::Unsigned(ws3.admissionManifestOwnerInputBypassed), true},
             {"ws3AdmissionManifestTargetBypass", Diagnostics::Unsigned(ws3.admissionManifestTargetBypassed), true},
             {"ws3AdmissionDescendantBypass", Diagnostics::Unsigned(ws3.admissionDescendantBypassed), true},
             {"ws3CausalProvSeed", Diagnostics::Unsigned(ws3.causalProvenanceSeeded), true},
             {"ws3CausalProvInherit", Diagnostics::Unsigned(ws3.causalProvenanceInherited), true},
             {"ws3CausalProvBoundary", Diagnostics::Unsigned(ws3.causalProvenanceBoundary), true},
             {"ws3CausalProvOverflow", Diagnostics::Unsigned(ws3.causalProvenanceOverflow), true},
             {"ws3CausalProvPlayer", Diagnostics::Unsigned(ws3.causalProvenancePlayer), true},
             {"ws3CausalProvAI", Diagnostics::Unsigned(ws3.causalProvenanceAi), true},
             {"ws3CausalProvUI", Diagnostics::Unsigned(ws3.causalProvenanceUi), true},
             {"ws3CausalProvAIMP", Diagnostics::Unsigned(ws3.causalProvenanceAimp), true},
             {"ws3CausalChainsCreated", Diagnostics::Unsigned(ws3.causalChainsCreated), true},
             {"ws3CausalChainsReleased", Diagnostics::Unsigned(ws3.causalChainsReleased), true},
             {"ws3CausalGroupsLive", Diagnostics::Unsigned(ws3.causalGroupsLive), true},
             {"ws3CausalGroupsPeak", Diagnostics::Unsigned(ws3.causalGroupsLivePeak), true},
             {"ws3CausalRootReserved", Diagnostics::Unsigned(ws3.causalRootReserved), true},
             {"ws3CausalRootCommitted", Diagnostics::Unsigned(ws3.causalRootCommitted), true},
             {"ws3CausalRootReservationCanceled", Diagnostics::Unsigned(ws3.causalRootReservationCanceled), true},
             {"ws3CausalRootAlreadyClaimed", Diagnostics::Unsigned(ws3.causalRootAlreadyClaimed), true},
             {"ws3CausalRootMissing", Diagnostics::Unsigned(ws3.causalRootMissing), true},
             {"ws3CausalRootNonWorldLocal", Diagnostics::Unsigned(ws3.causalRootNonWorldLocal), true},
             {"ws3CausalRootGroupOverflow", Diagnostics::Unsigned(ws3.causalRootGroupOverflow), true},
             {"ws3CausalRootSuppressedSiblings", Diagnostics::Unsigned(ws3.causalRootSuppressedSiblings), true},
             {"ws3CausalRootProbeStandalone", Diagnostics::Unsigned(ws3.causalRootProbeStandalone), true},
             {"ws3CausalBoundaryProfileMissing", Diagnostics::Unsigned(ws3.causalBoundaryProfileMissing), true},
             {"ws3CausalBoundaryNoWorldEffect", Diagnostics::Unsigned(ws3.causalBoundaryNoWorldEffect), true},
             {"ws3CausalBoundaryPlayerDependent", Diagnostics::Unsigned(ws3.causalBoundaryPlayerDependent), true},
             {"ws3CausalBoundaryWorldCandidate", Diagnostics::Unsigned(ws3.causalBoundaryWorldCandidate), true},
             {"ws3CausalBoundaryTruncated", Diagnostics::Unsigned(ws3.causalBoundaryTruncated), true},
             {"ws3CausalBoundaryUnknownDependency", Diagnostics::Unsigned(ws3.causalBoundaryUnknownDependency), true},
             {"ws3ParentContextBound", Diagnostics::Unsigned(ws3.parentContextBound), true},
             {"ws3ParentContextBoundLocalRoot", Diagnostics::Unsigned(ws3.parentContextBoundLocalRoot), true},
             {"ws3ParentContextBoundRemoteRoot", Diagnostics::Unsigned(ws3.parentContextBoundRemoteRoot), true},
             {"ws3ParentContextInheritedOrigin", Diagnostics::Unsigned(ws3.parentContextInheritedOrigin), true},
             {"ws3ParentContextInheritedCausal", Diagnostics::Unsigned(ws3.parentContextInheritedCausal), true},
             {"ws3ParentContextInheritedCause", Diagnostics::Unsigned(ws3.parentContextInheritedCause), true},
             {"ws3ParentContextReleased", Diagnostics::Unsigned(ws3.parentContextReleased), true},
             {"ws3ParentContextStale", Diagnostics::Unsigned(ws3.parentContextStale), true},
             {"ws3ParentContextOverflow", Diagnostics::Unsigned(ws3.parentContextOverflow), true},
             {"ws3ParentContextsLive", Diagnostics::Unsigned(ws3.parentContextsLive), true},
             {"ws3ParentContextsPeak", Diagnostics::Unsigned(ws3.parentContextsPeak), true},
             {"ws3LineageGuardSuppressed", Diagnostics::Unsigned(ws3.lineageGuardSuppressed), true},
             {"ws3ProbeCallbackException",
              Diagnostics::Unsigned(ws3.probeCallbackExceptions), true},
             {"ws3ProbeCausalUnknownException",
              Diagnostics::Unsigned(ws3.probeCausalUnknownExceptions), true},
             {"ws3RootRateReject",
              Diagnostics::Unsigned(ws3.rootRateRejected), true},
             {"ws3AuthorityRetailPreserved", Diagnostics::Unsigned(ws3.authorityRetailPreserved), true},
             {"ws3AuthorityLocalSuppressed", Diagnostics::Unsigned(ws3.authorityLocalSuppressed), true},
             {"ws3AuthorityAwaitSubmitted", Diagnostics::Unsigned(ws3.authorityAwaitSubmitted), true},
             {"ws3AuthorityAutonomousSuppressed", Diagnostics::Unsigned(ws3.authorityAutonomousSuppressed), true},
             {"ws3CausalBoundaryBranching", Diagnostics::Unsigned(ws3.causalBoundaryBranching), true},
             {"ws3EmittedForwardingReject", Diagnostics::Unsigned(ws3.emittedOutputForwardingRejected), true},
             {"ws3ScheduledCertificationReject", Diagnostics::Unsigned(ws3.scheduledInputCertificationRejected), true},
             {"ws3BridgeScope", Diagnostics::Unsigned(ws3.callbackBridgeScopes), true},
             {"ws3BridgeSend", Diagnostics::Unsigned(ws3.callbackBridgeSubmitted), true},
             {"ws3BridgeOwnerReject", Diagnostics::Unsigned(ws3.callbackBridgeOwnerRejected), true},
             {"ws3BridgeSameInputReject", Diagnostics::Unsigned(ws3.callbackBridgeSameInputRejected), true},
             {"ws3BridgeClaimedReject", Diagnostics::Unsigned(ws3.callbackBridgeAlreadyClaimedRejected), true},
             {"ws3BridgeRateReject", Diagnostics::Unsigned(ws3.callbackBridgeRateRejected), true},
             {"ws3BridgeQueueReject", Diagnostics::Unsigned(ws3.callbackBridgeQueueRejected), true},
             {"worldOutboundIntents", Diagnostics::Unsigned(replication.outboundIntentCount), true},
             {"worldReceivedMutations", Diagnostics::Unsigned(replication.receivedMutationCount), true},
             {"worldAcceptedMutations", Diagnostics::Unsigned(replication.acceptedMutationCount), true},
             {"worldQueuedMutations", Diagnostics::Unsigned(replication.queuedMutationCount), true},
             {"worldAppliedMutations", Diagnostics::Unsigned(replication.appliedMutationCount), true},
             {"worldEchoSuppressed", Diagnostics::Unsigned(replication.echoSuppressedCount), true},
             {"ws3DiscardedObservations",
              Diagnostics::Unsigned(interactions.discardedPublicRootObservations), true},
             {"worldDuplicateRejected", Diagnostics::Unsigned(replication.duplicateRejectedCount), true},
             {"worldStaleRejected", Diagnostics::Unsigned(replication.staleRejectedCount), true},
             {"worldGenerationRejected", Diagnostics::Unsigned(replication.generationRejectedCount), true},
             {"worldManifestRejected", Diagnostics::Unsigned(replication.manifestRejectedCount), true},
             {"worldEntityRejected", Diagnostics::Unsigned(replication.entityRejectedCount), true},
             {"worldQueueRejected", Diagnostics::Unsigned(replication.queueRejectedCount), true},
             {"worldRuntimePending", Diagnostics::Unsigned(replication.runtimePendingCount), true},
             {"worldNativeFault", Diagnostics::Unsigned(replication.nativeFaultCount), true},
             {"worldDeferredRemoteApply", Diagnostics::Unsigned(replication.deferredRemoteApplyCount), true},
             {"worldDeferredQueued", Diagnostics::Unsigned(replication.deferredQueuedCount), true},
             {"worldDeferredRetry", Diagnostics::Unsigned(replication.deferredRetryCount), true},
             {"worldDeferredExecuted", Diagnostics::Unsigned(replication.deferredExecutedCount), true},
             {"worldDeferredExpired", Diagnostics::Unsigned(replication.deferredExpiredCount), true},
             {"ws3EmittedOutputRootSend", Diagnostics::Unsigned(ws3.emittedOutputRootsSent), true},
             {"ws3ScheduledInputRootSend", Diagnostics::Unsigned(ws3.scheduledInputRootsSent), true},
             {"ws3SchedulerEdgeRootSend", Diagnostics::Unsigned(ws3.schedulerEdgeRootsSent), true},
             {"ws3TypedMessageRootSend", Diagnostics::Unsigned(ws3.typedMessageRootsSent), true},
             {"ws3TypedMessageRootApplied", Diagnostics::Unsigned(ws3.typedMessageRootsApplied), true},
             {"ws3CanonicalCompositeRootSend", Diagnostics::Unsigned(ws3.canonicalCompositeRootsSent), true},
             {"ws3CanonicalCompositeRootApplied", Diagnostics::Unsigned(ws3.canonicalCompositeRootsApplied), true},
             {"ws3DoorLockStateRootSend", Diagnostics::Unsigned(ws3.doorLockStateRootsSent), true},
             {"ws3DoorLockStateRootApplied", Diagnostics::Unsigned(ws3.doorLockStateRootsApplied), true},
             {"ws3SchedulerEdgeSidecarObserved", Diagnostics::Unsigned(ws3.schedulerEdgeSidecarObserved), true},
             {"ws3SchedulerEdgeSidecarSubmitted", Diagnostics::Unsigned(ws3.schedulerEdgeSidecarSubmitted), true},
             {"ws3SchedulerEdgeRemoteNaturalRemembered", Diagnostics::Unsigned(ws3.schedulerEdgeRemoteNaturalRemembered), true},
             {"ws3SchedulerEdgeRemoteDedupConsumed", Diagnostics::Unsigned(ws3.schedulerEdgeRemoteDedupConsumed), true},
             {"ws3DoorTransitionHooksArmed", Diagnostics::Boolean(animated.doorHooksInstalled), true},
             {"ws3DoorUnlockProbeArmed", Diagnostics::Boolean(animated.doorUnlockProbeInstalled), true},
             {"ws3DoorUnlockProbeCalls", Diagnostics::Unsigned(animated.doorUnlockProbeCalls), true},
             {"ws3DoorUnlockProbeSuccess", Diagnostics::Unsigned(animated.doorUnlockProbeSuccess), true},
             {"ws3DoorUnlockProbeResolved", Diagnostics::Unsigned(animated.doorUnlockProbeResolved), true},
             {"ws3DoorUnlockProbeUnresolved", Diagnostics::Unsigned(animated.doorUnlockProbeUnresolved), true},
             {"ws3DoorUnlockProbeLastEntityHigh", Diagnostics::Unsigned(animated.doorUnlockProbeLastEntityHigh), true},
             {"ws3DoorUnlockProbeLastEntityLow", Diagnostics::Unsigned(animated.doorUnlockProbeLastEntityLow), true},
             {"ws3DoorSemanticProbeArmed", Diagnostics::Boolean(animated.doorSemanticProbeInstalled), true},
             {"ws3DoorRequestLockProbeCalls", Diagnostics::Unsigned(animated.doorRequestLockProbeCalls), true},
             {"ws3DoorRequestLockProbeSuccess", Diagnostics::Unsigned(animated.doorRequestLockProbeSuccess), true},
             {"ws3DoorRequestLockProbeResolved", Diagnostics::Unsigned(animated.doorRequestLockProbeResolved), true},
             {"ws3DoorForceOpenProbeCalls", Diagnostics::Unsigned(animated.doorForceOpenProbeCalls), true},
             {"ws3DoorForceOpenProbeSuccess", Diagnostics::Unsigned(animated.doorForceOpenProbeSuccess), true},
             {"ws3DoorForceOpenProbeResolved", Diagnostics::Unsigned(animated.doorForceOpenProbeResolved), true},
             {"ws3DoorForceCloseProbeCalls", Diagnostics::Unsigned(animated.doorForceCloseProbeCalls), true},
             {"ws3DoorForceCloseProbeSuccess", Diagnostics::Unsigned(animated.doorForceCloseProbeSuccess), true},
             {"ws3DoorForceCloseProbeResolved", Diagnostics::Unsigned(animated.doorForceCloseProbeResolved), true},
             {"ws3DoorSemanticProbeLastAction", Diagnostics::Unsigned(animated.doorSemanticProbeLastAction), true},
             {"ws3DoorSemanticProbeLastEntityHigh", Diagnostics::Unsigned(animated.doorSemanticProbeLastEntityHigh), true},
             {"ws3DoorSemanticProbeLastEntityLow", Diagnostics::Unsigned(animated.doorSemanticProbeLastEntityLow), true},
             {"ws3DoorLockStateSourceResolved", Diagnostics::Unsigned(animated.doorLockStateSourceResolved), true},
             {"ws3DoorLockStateSourceUnresolved", Diagnostics::Unsigned(animated.doorLockStateSourceUnresolved), true},
             {"ws3DoorLockStateSelfSourceRejected", Diagnostics::Unsigned(animated.doorLockStateSelfSourceRejected), true},
             {"ws3DoorLockStateLastSourceHigh", Diagnostics::Unsigned(animated.doorLockStateLastSourceHigh), true},
             {"ws3DoorLockStateLastSourceLow", Diagnostics::Unsigned(animated.doorLockStateLastSourceLow), true},
             {"ws3DoorTransitionOutputCalls", Diagnostics::Unsigned(animated.doorOutputCalls), true},
             {"ws3DoorTransitionRootAttempts", Diagnostics::Unsigned(animated.doorTransitionRootAttempts), true},
             {"ws3DoorTransitionRootSubmitted", Diagnostics::Unsigned(animated.doorTransitionRootSubmitted), true},
             {"ws3DoorTransitionRootRetailOnly", Diagnostics::Unsigned(animated.doorTransitionRootRetailOnly), true},
             {"ws3DoorTransitionRootAwaitAuthority", Diagnostics::Unsigned(animated.doorTransitionRootAwaitAuthority), true},
             {"ws3DoorTransitionRootAuthoritySuppressed", Diagnostics::Unsigned(animated.doorTransitionRootAuthoritySuppressed), true},
             {"ws3PreQueueSeen", Diagnostics::Unsigned(interactions.ws3PreQueueSeen), true},
             {"ws3PreQueueInputReadable", Diagnostics::Unsigned(interactions.ws3PreQueueInputReadable), true},
             {"ws3PreQueueTargetWorldResolved", Diagnostics::Unsigned(interactions.ws3PreQueueTargetWorldResolved), true},
             {"ws3PreQueueTargetWorldUnresolved", Diagnostics::Unsigned(interactions.ws3PreQueueTargetWorldUnresolved), true},
             {"ws3PreQueueLastInput", Diagnostics::Unsigned(interactions.ws3PreQueueLastInput), true},
             {"ws3PreQueueLastCallerRva", Diagnostics::Unsigned(interactions.ws3PreQueueLastCallerRva), true},
             {"ws3CanonicalPreQueueHintStored", Diagnostics::Unsigned(interactions.ws3CanonicalPreQueueHintStored), true},
             {"ws3CanonicalPreQueueHintConsumed", Diagnostics::Unsigned(interactions.ws3CanonicalPreQueueHintConsumed), true},
             {"ws3CanonicalPreQueueHintMissed", Diagnostics::Unsigned(interactions.ws3CanonicalPreQueueHintMissed), true},
             {"ws3CanonicalPreQueueSubmitAttempt", Diagnostics::Unsigned(interactions.ws3CanonicalPreQueueSubmitAttempt), true},
             {"ws3CanonicalPreQueueSubmitSuccess", Diagnostics::Unsigned(interactions.ws3CanonicalPreQueueSubmitSuccess), true},
             {"ws3CanonicalPreQueueAdmissionSuppressed", Diagnostics::Unsigned(interactions.ws3CanonicalPreQueueAdmissionSuppressed), true},
             {"ws3CanonicalPreQueueSubmitFailed", Diagnostics::Unsigned(interactions.ws3CanonicalPreQueueSubmitFailed), true},
             {"ws3CanonicalQueueCandidateSeen", Diagnostics::Unsigned(interactions.ws3CanonicalQueueCandidateSeen), true},
             {"ws3CanonicalQueueMapped", Diagnostics::Unsigned(interactions.ws3CanonicalQueueMapped), true},
             {"ws3CanonicalQueueLocallyExecuted", Diagnostics::Unsigned(interactions.ws3CanonicalQueueLocallyExecuted), true},
             {"ws3CanonicalQueueAdmissionSuppressed", Diagnostics::Unsigned(interactions.ws3CanonicalQueueAdmissionSuppressed), true},
             {"ws3QueueInputSeen", Diagnostics::Unsigned(interactions.ws3QueueSeen), true},
             {"ws3QueueInputDescendantSuppressed", Diagnostics::Unsigned(interactions.ws3QueueDescendantSuppressed), true},
             {"ws3QueueInputCallbackRejected", Diagnostics::Unsigned(interactions.ws3QueueCallbackRejected), true},
             {"ws3QueueInputDescribeRejected", Diagnostics::Unsigned(interactions.ws3QueueDescribeRejected), true},
             {"ws3QueueInputUnresolvedSource", Diagnostics::Unsigned(interactions.unresolvedSource), true},
             {"ws3QueueInputUnresolvedTarget", Diagnostics::Unsigned(interactions.unresolvedTarget), true},
             {"ws3QueueInputRemoteNatural", Diagnostics::Unsigned(interactions.ws3QueueRemoteNatural), true},
             {"ws3QueueInputRemoteApplySuppressed", Diagnostics::Unsigned(interactions.ws3QueueRemoteApplySuppressed), true},
             {"ws3QueueInputDeferredComposite", Diagnostics::Unsigned(interactions.ws3QueueDeferredComposite), true},
             {"ws3QueueInputStateOwned", Diagnostics::Unsigned(interactions.ws3QueueStateOwned), true},
             {"ws3QueueInputEventEligible", Diagnostics::Unsigned(interactions.ws3QueueEventEligible), true},
             {"ws3QueueInputUnknownEligible", Diagnostics::Unsigned(interactions.ws3QueueUnknownEligible), true},
             {"ws3QueueInputSubmitRetailOnly", Diagnostics::Unsigned(interactions.ws3QueueSubmitRetailOnly), true},
             {"ws3QueueInputSubmitAwaitAuthority", Diagnostics::Unsigned(interactions.ws3QueueSubmitAwaitAuthority), true},
             {"ws3QueueInputSubmitAuthoritySuppressed", Diagnostics::Unsigned(interactions.ws3QueueSubmitAuthoritySuppressed), true},
             {"ws3QueueInputSubmitLocalRoot", Diagnostics::Unsigned(interactions.ws3QueueSubmitLocalRoot), true},
             {"ws3QueueInputLastSchedulerParameter", Diagnostics::Unsigned(interactions.ws3QueueLastSchedulerParameter), true},
             {"ws3QueueInputLastTargetInput", Diagnostics::Unsigned(interactions.ws3QueueLastTargetInput), true},
             {"ws3QueueInputLastCallerRva", Diagnostics::Unsigned(interactions.ws3QueueLastCallerRva), true},
             {"ws3QueueInputLastGate", Diagnostics::Unsigned(interactions.ws3QueueLastGate), true},
             {"ws3TypedBoundarySeen", Diagnostics::Unsigned(interactions.ws3TypedSeen), true},
             {"ws3TypedBoundaryDescendantSuppressed", Diagnostics::Unsigned(interactions.ws3TypedDescendantSuppressed), true},
             {"ws3TypedBoundaryWorldPairResolved", Diagnostics::Unsigned(interactions.ws3TypedWorldPairResolved), true},
             {"ws3TypedBoundaryWorldPairUnresolved", Diagnostics::Unsigned(interactions.ws3TypedWorldPairUnresolved), true},
             {"ws3TypedRootRetailOnly", Diagnostics::Unsigned(interactions.ws3TypedRootRetailOnly), true},
             {"ws3TypedRootAwaitAuthority", Diagnostics::Unsigned(interactions.ws3TypedRootAwaitAuthority), true},
             {"ws3TypedRootAuthoritySuppressed", Diagnostics::Unsigned(interactions.ws3TypedRootAuthoritySuppressed), true},
             {"ws3TypedRootLocal", Diagnostics::Unsigned(interactions.ws3TypedRootLocal), true},
             {"ws3TypedCharacterEventsEncoded", Diagnostics::Unsigned(primitives.typedCharacterEventsEncoded), true},
             {"ws3TypedCharacterEventsRebuilt", Diagnostics::Unsigned(primitives.typedCharacterEventsRebuilt), true},
             {"ws3TypedCharacterEventsRejected", Diagnostics::Unsigned(primitives.typedCharacterEventsRejected), true},
             {"ws3TypedCharacterEventLastKind", Diagnostics::Unsigned(primitives.lastTypedCharacterEventKind), true},
             {"ws3TypedReceiverLocalSuppressed", Diagnostics::Unsigned(interactions.ws3TypedReceiverLocalSuppressed), true},
             {"ws3TypedLastReceiverLocalSuppressedType", Diagnostics::Unsigned(interactions.ws3TypedLastReceiverLocalSuppressedType), true},
             {"ws3TypedBoundaryLastMessageType", Diagnostics::Unsigned(interactions.ws3TypedLastMessageType), true},
             {"ws3TypedBoundaryLastPayloadBytes", Diagnostics::Unsigned(interactions.ws3TypedLastPayloadBytes), true},
             {"ws3TypedBoundaryLastWorldMask", Diagnostics::Unsigned(interactions.ws3TypedLastWorldMask), true},
             {"ws3TypedBoundaryLastCallerRva", Diagnostics::Unsigned(interactions.ws3TypedLastCallerRva), true},
             {"ws3TypedInteractionPairSeen", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairSeen), true},
             {"ws3TypedInteractionPair43", Diagnostics::Unsigned(interactions.ws3TypedInteractionPair43), true},
             {"ws3TypedInteractionPair44", Diagnostics::Unsigned(interactions.ws3TypedInteractionPair44), true},
             {"ws3TypedInteractionPair43LastCallerRva", Diagnostics::Unsigned(interactions.ws3TypedInteractionPair43LastCallerRva), true},
             {"ws3TypedInteractionPair44LastCallerRva", Diagnostics::Unsigned(interactions.ws3TypedInteractionPair44LastCallerRva), true},
             {"ws3TypedInteractionPair43LastCausalSource", Diagnostics::Unsigned(interactions.ws3TypedInteractionPair43LastCausalSource), true},
             {"ws3TypedInteractionPair44LastCausalSource", Diagnostics::Unsigned(interactions.ws3TypedInteractionPair44LastCausalSource), true},
             {"ws3TypedInteractionPairLastType", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastType), true},
             {"ws3TypedInteractionPairLastCallerRva", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastCallerRva), true},
             {"ws3TypedInteractionPairLastSourcePrimitive", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastSourcePrimitive), true},
             {"ws3TypedInteractionPairLastTargetPrimitive", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastTargetPrimitive), true},
             {"ws3TypedInteractionPairLastSourceDomain", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastSourceDomain), true},
             {"ws3TypedInteractionPairLastTargetDomain", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastTargetDomain), true},
             {"ws3TypedInteractionPairLastCausalSource", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastCausalSource), true},
             {"ws3TypedInteractionPairLastSourceHigh", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastSourceHigh), true},
             {"ws3TypedInteractionPairLastSourceLow", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastSourceLow), true},
             {"ws3TypedInteractionPairLastTargetHigh", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastTargetHigh), true},
             {"ws3TypedInteractionPairLastTargetLow", Diagnostics::Unsigned(interactions.ws3TypedInteractionPairLastTargetLow), true},
             {"ws3ScheduledEventRootSuppressed", Diagnostics::Unsigned(ws3.scheduledEventRootsSuppressed), true},
             {"ws3CarrierObserved", Diagnostics::Unsigned(ws3.carrierObserved), true},
             {"ws3CarrierNetworkEligible", Diagnostics::Unsigned(ws3.carrierNetworkEligible), true},
             {"ws3CarrierForwardingComplete", Diagnostics::Unsigned(ws3.carrierForwardingComplete), true},
             {"ws3CarrierSelectorResolved", Diagnostics::Unsigned(ws3.carrierSelectorResolved), true},
             {"ws3CarrierSelectorRejected", Diagnostics::Unsigned(ws3.carrierSelectorRejected), true},
             {"ws3CarrierContextCaptured", Diagnostics::Unsigned(ws3.carrierContextCaptured), true},
             {"ws3CarrierManifestAdmitted", Diagnostics::Unsigned(ws3.carrierManifestAdmitted), true},
             {"ws3CarrierPreBusPreserved", Diagnostics::Unsigned(ws3.carrierPreBusPreserved), true},
             {"ws3CarrierSubmitAttempt", Diagnostics::Unsigned(ws3.carrierSubmitAttempt), true},
             {"ws3CarrierSubmitSuccess", Diagnostics::Unsigned(ws3.carrierSubmitSuccess), true},
             {"ws3ScheduledCoveredByCarrier", Diagnostics::Unsigned(ws3.scheduledCoveredByCarrier), true},
             {"ws3ScheduledCoveredByCarrierSuppressed",
              Diagnostics::Unsigned(ws3.scheduledCoveredByCarrierSuppressed), true},
             {"ws3CompositeCarrierObserved", Diagnostics::Unsigned(ws3.compositeCarrierObserved), true},
             {"ws3CompositeCarrierNetworkEligible", Diagnostics::Unsigned(ws3.compositeCarrierNetworkEligible), true},
             {"ws3CompositeCarrierTargetAdmitted", Diagnostics::Unsigned(ws3.compositeCarrierTargetAdmitted), true},
             {"ws3CompositeCarrierManifestAdmitted", Diagnostics::Unsigned(ws3.compositeCarrierManifestAdmitted), true},
             {"ws3CompositeCarrierPreBusPreserved", Diagnostics::Unsigned(ws3.compositeCarrierPreBusPreserved), true},
             {"ws3CompositeCarrierSubmitAttempt", Diagnostics::Unsigned(ws3.compositeCarrierSubmitAttempt), true},
             {"ws3CompositeCarrierSubmitSuccess", Diagnostics::Unsigned(ws3.compositeCarrierSubmitSuccess), true},
             {"ws3ScheduledStateTransitionNoCarrier",
              Diagnostics::Unsigned(ws3.scheduledStateTransitionNoCarrier), true},
             {"ws3ScheduledStateTransitionQuarantinedNoCarrier",
              Diagnostics::Unsigned(ws3.scheduledStateTransitionQuarantinedNoCarrier), true},
             {"ws3SegmentsSealed", Diagnostics::Unsigned(replication.causalSegmentsSealed), true},
             {"ws3SegmentsApplied", Diagnostics::Unsigned(replication.causalSegmentsApplied), true},
             {"ws3SegmentsFailed", Diagnostics::Unsigned(replication.causalSegmentsFailed), true},
             {"ws3SegmentsSkipped", Diagnostics::Unsigned(replication.causalSegmentsSkipped), true},
             {"ws3SequenceGaps", Diagnostics::Unsigned(replication.causalSequenceGaps), true},
             {"ws3LastCommittedSequence", Diagnostics::Unsigned(replication.causalLastCommittedSequence), true},
             {"ws3RecoveryRequired", Diagnostics::Boolean(replication.causalRecoveryRequired), true},
             {"ws3DivergedAuthorityTransaction", Diagnostics::Unsigned(replication.causalDivergedAuthorityTransaction), true},
             {"ws3OrderedCarrierTransactions", Diagnostics::Unsigned(replication.orderedCarrierTransactionCount), true},
             {"ws3OrderedCarrierMembers", Diagnostics::Unsigned(replication.orderedCarrierMemberCount), true},
             {"ws3OrderedCarrierCommits", Diagnostics::Unsigned(replication.orderedCarrierCommitCount), true},
             {"ws3OrderedCarrierFailures", Diagnostics::Unsigned(replication.orderedCarrierFailureCount), true},
             {"ws3SemanticProbe", Diagnostics::Unsigned(replication.semanticProbeCount), true},
             {"ws3SemanticContractedProbe", Diagnostics::Unsigned(replication.semanticContractedProbeCount), true},
             {"ws3SemanticUnknownContract", Diagnostics::Unsigned(replication.semanticUnknownContractCount), true},
             {"ws3SemanticEventContract", Diagnostics::Unsigned(replication.semanticEventContractCount), true},
             {"ws3SemanticStateContract", Diagnostics::Unsigned(replication.semanticStateContractCount), true},
             {"ws3SemanticStateTransitionContract", Diagnostics::Unsigned(replication.semanticStateTransitionContractCount), true},
             {"ws3SemanticReaderResolved", Diagnostics::Unsigned(replication.semanticReaderResolvedCount), true},
             {"ws3SemanticReaderRuntimeUnavailable", Diagnostics::Unsigned(replication.semanticReaderRuntimeUnavailableCount), true},
             {"ws3SemanticReaderUnsupported", Diagnostics::Unsigned(replication.semanticReaderUnsupportedCount), true},
             {"ws3SemanticReaderInvalidState", Diagnostics::Unsigned(replication.semanticReaderInvalidStateCount), true},
             {"ws3SemanticReaderNativeFault", Diagnostics::Unsigned(replication.semanticReaderNativeFaultCount), true},
             {"ws3SemanticCandidateApply", Diagnostics::Unsigned(replication.semanticCandidateApplyCount), true},
             {"ws3SemanticCandidateSkip", Diagnostics::Unsigned(replication.semanticCandidateSkipCount), true},
             {"ws3SemanticCandidateMismatch", Diagnostics::Unsigned(replication.semanticCandidateMismatchCount), true},
             {"ws3SemanticCandidateAlreadyDesiredTransitionUnproven",
              Diagnostics::Unsigned(replication.semanticCandidateAlreadyDesiredTransitionUnprovenCount), true},
             {"ws3SemanticCandidateUnknown", Diagnostics::Unsigned(replication.semanticCandidateUnknownCount), true},
             {"ws3SemanticActionLegacyApply", Diagnostics::Unsigned(replication.semanticActionLegacyApplyCount), true},
             {"ws3SemanticActionApply", Diagnostics::Unsigned(replication.semanticActionApplyCount), true},
             {"ws3SemanticActionSkip", Diagnostics::Unsigned(replication.semanticActionSkipCount), true},
             {"ws3SemanticActionRetry", Diagnostics::Unsigned(replication.semanticActionRetryCount), true},
             {"ws3SemanticActionReject", Diagnostics::Unsigned(replication.semanticActionRejectCount), true},
             {"ws3SemanticActionTransitionUnprovenReject",
              Diagnostics::Unsigned(replication.semanticActionTransitionUnprovenRejectCount), true},
             {"ws3SemanticActionStatefulMultiRootReject",
              Diagnostics::Unsigned(replication.semanticActionStatefulMultiRootRejectCount), true},
             {"ws3SemanticActionCarrierTransitionApply",
              Diagnostics::Unsigned(replication.semanticActionCarrierTransitionApplyCount), true},
             {"ws3SemanticTraceWrites", Diagnostics::Unsigned(replication.semanticTraceWriteCount), true},
             {"ws3SemanticHasLast", Diagnostics::Boolean(replication.semanticHasLast), true},
             {"ws3SemanticLastAuthoritySequence", Diagnostics::Unsigned(replication.semanticLastAuthoritySequence), true},
             {"ws3SemanticLastObservationId", Diagnostics::Unsigned(replication.semanticLastObservationId), true},
             {"ws3SemanticLastAuthorityTransaction", Diagnostics::Unsigned(replication.semanticLastAuthorityTransaction), true},
             {"ws3SemanticLastApplyBoundaryHigh", Diagnostics::Unsigned(replication.semanticLastApplyBoundaryHigh), true},
             {"ws3SemanticLastApplyBoundaryLow", Diagnostics::Unsigned(replication.semanticLastApplyBoundaryLow), true},
             {"ws3SemanticLastApplyBoundaryOperation", Diagnostics::Unsigned(replication.semanticLastApplyBoundaryOperation), true},
             {"ws3SemanticLastRootKind", Diagnostics::Unsigned(replication.semanticLastRootKind), true},
             {"ws3SemanticLastContract", Diagnostics::Unsigned(replication.semanticLastContract), true},
             {"ws3SemanticLastTargetHigh", Diagnostics::Unsigned(replication.semanticLastTargetHigh), true},
             {"ws3SemanticLastTargetLow", Diagnostics::Unsigned(replication.semanticLastTargetLow), true},
             {"ws3SemanticLastTargetPrimitiveType", Diagnostics::Unsigned(replication.semanticLastTargetPrimitiveType), true},
             {"ws3SemanticLastOperation", Diagnostics::Unsigned(replication.semanticLastOperation), true},
             {"ws3SemanticLastReadStatus", Diagnostics::Unsigned(replication.semanticLastReadStatus), true},
             {"ws3SemanticLastCandidateDecision", Diagnostics::Unsigned(replication.semanticLastCandidateDecision), true},
             {"ws3SemanticLastAction", Diagnostics::Unsigned(replication.semanticLastAction), true},
             {"ws3SemanticLastDesiredKnown", Diagnostics::Boolean(replication.semanticLastDesiredKnown), true},
             {"ws3SemanticLastDesired", Diagnostics::Boolean(replication.semanticLastDesired), true},
             {"ws3SemanticLastExpectedBeforeKnown", Diagnostics::Boolean(replication.semanticLastExpectedBeforeKnown), true},
             {"ws3SemanticLastExpectedBefore", Diagnostics::Boolean(replication.semanticLastExpectedBefore), true},
             {"ws3SemanticLastLocalBeforeKnown", Diagnostics::Boolean(replication.semanticLastLocalBeforeKnown), true},
             {"ws3SemanticLastLocalBefore", Diagnostics::Boolean(replication.semanticLastLocalBefore), true},
             {"ws3SemanticLastCarrierBoundary", Diagnostics::Boolean(replication.semanticLastCarrierBoundary), true},
             {"worldDeferredOverflow", Diagnostics::Unsigned(replication.deferredOverflowCount), true},
             {"worldControlledRemoteApplyProbeOnly",
              Diagnostics::Boolean(replication.controlledRemoteApplyProbeOnly), true},
             {"worldControlledRemoteApplyFirstInteraction",
              Diagnostics::Boolean(replication.controlledRemoteApplyFirstInteraction), true},
             {"worldControlledRemoteApplyArmed",
              Diagnostics::Boolean(replication.controlledRemoteApplyArmed), true},
             {"worldControlledRemoteApplyTargetHigh",
              Diagnostics::Unsigned(replication.controlledRemoteApplyTargetHigh), true},
             {"worldControlledRemoteApplyTargetLow",
              Diagnostics::Unsigned(replication.controlledRemoteApplyTargetLow), true},
             {"worldControlledRemoteApplyTargetRootCount",
              Diagnostics::Unsigned(replication.controlledRemoteApplyTargetRootCount), true},
             {"worldControlledRemoteApplyArmCount",
              Diagnostics::Unsigned(replication.controlledRemoteApplyArmCount), true},
             {"worldControlledRemoteApplyGameplayCandidate",
              Diagnostics::Unsigned(replication.controlledRemoteApplyGameplayCandidateCount), true},
             {"worldControlledRemoteApplyGameplaySelected",
              Diagnostics::Unsigned(replication.controlledRemoteApplyGameplaySelectedCount), true},
             {"worldControlledRemoteApplyUnsafeContextSuppressed",
              Diagnostics::Unsigned(replication.controlledRemoteApplyUnsafeContextSuppressedCount), true},
             {"worldControlledRemoteApplyNotArmedSuppressed",
              Diagnostics::Unsigned(replication.controlledRemoteApplyNotArmedSuppressedCount), true},
             {"worldControlledRemoteApplyOtherTargetSuppressed",
              Diagnostics::Unsigned(replication.controlledRemoteApplyOtherTargetSuppressedCount), true},
             {"worldControlledRemoteApplyCrossTargetSelected",
              Diagnostics::Unsigned(replication.controlledRemoteApplyCrossTargetSelectedCount), true},
             {"worldControlledRemoteApplyBudgetSuppressed",
              Diagnostics::Unsigned(replication.controlledRemoteApplyBudgetSuppressedCount), true},
             {"worldControlledRemoteApplyLastSelectedInput",
              Diagnostics::Unsigned(replication.controlledRemoteApplyLastSelectedInput), true},
             {"worldControlledRemoteApplyLastSelectedTargetHigh",
              Diagnostics::Unsigned(replication.controlledRemoteApplyLastSelectedTargetHigh), true},
             {"worldControlledRemoteApplyLastSelectedTargetLow",
              Diagnostics::Unsigned(replication.controlledRemoteApplyLastSelectedTargetLow), true},
             {"worldControlledRemoteApplyLastSelectedSequence",
              Diagnostics::Unsigned(replication.controlledRemoteApplyLastSelectedSequence), true},
             {"worldControlledRemoteApplyParsed",
              Diagnostics::Unsigned(replication.controlledRemoteApplyParsedCount), true},
             {"worldControlledRemoteApplySelected",
              Diagnostics::Unsigned(replication.controlledRemoteApplySelectedCount), true},
             {"worldControlledRemoteApplySuppressed",
              Diagnostics::Unsigned(replication.controlledRemoteApplySuppressedCount), true},
             {"worldOriginEchoSuppressed", Diagnostics::Unsigned(interactions.originEchoSuppressed), true},
             {"worldEchoTableFull", Diagnostics::Unsigned(interactions.echoTableFull), true},
             {"observationPending", Diagnostics::Unsigned(interactions.observationPending), true},
             {"observationHighWater", Diagnostics::Unsigned(interactions.observationHighWater), true},
             {"observationRanges", Diagnostics::Unsigned(interactions.observationRanges), true},
             {"observationRangeCapacity", "4096", true},
             {"observationOldestSerial", Diagnostics::Unsigned(interactions.observationOldestSerial), true},
             {"observationRemembered", Diagnostics::Unsigned(interactions.observationRemembered), true},
             {"observationEchoed", Diagnostics::Unsigned(interactions.observationEchoed), true},
             {"observationRetired", Diagnostics::Unsigned(interactions.observationRetired), true},
             {"observationMissing", Diagnostics::Unsigned(interactions.observationMissing), true},
             {"observationSplitRefused", Diagnostics::Unsigned(interactions.observationSplitRefused), true},
             {"worldOutboundPending", Diagnostics::Unsigned(replication.outboundPendingCount), true},
             {"worldOutboundReserved", Diagnostics::Unsigned(replication.outboundReservedCount), true},
              {"worldOutboundCapacity", Diagnostics::Unsigned(replication.outboundCapacity), true},
              {"worldOutboundHighWater", Diagnostics::Unsigned(replication.outboundHighWater), true},
              {"worldOutboundCapacityRejected", Diagnostics::Unsigned(replication.outboundCapacityRejected), true},
             {"worldOutboundPopped", Diagnostics::Unsigned(replication.outboundPoppedCount), true},
             {"worldIntentQueueRejected", Diagnostics::Unsigned(interactions.intentQueueRejected), true}});
        world_actions::DrainDiagnosticFiles(*diagnostics_);
        semantic_reconciliation::DrainDiagnosticFiles();
        for (std::uint32_t index = 0U; index < ws3.relevanceTopCount; ++index) {
            const auto& producer = ws3.relevanceTop[index];
            diagnostics_->Log(
                "info",
                "native_worldsync_relevance_top",
                "Bounded WM08 replication-relevance producer aggregate.",
                {{"rank", Diagnostics::Unsigned(index + 1U), true},
                 {"count", Diagnostics::Unsigned(producer.count), true},
                 {"rootKind", Diagnostics::Unsigned(producer.rootKind), true},
                 {"primitiveType", Diagnostics::Unsigned(producer.primitiveType), true},
                 {"shortGuid", Diagnostics::Unsigned(producer.shortGuid), true},
                 {"replicationRelevance", Diagnostics::Unsigned(producer.replicationRelevance), true},
                 {"relevanceDisposition", Diagnostics::Unsigned(
                     producer.replicationRelevance & kWorldReplicationDispositionMask), true},
                 {"semanticClass", Diagnostics::Unsigned(
                     (producer.replicationRelevance & kWorldReplicationSemanticClassMask) >>
                         kWorldReplicationSemanticClassShift), true},
                 {"ownerHigh", Diagnostics::Unsigned(producer.owner.high), true},
                 {"ownerLow", Diagnostics::Unsigned(producer.owner.low), true}});
        }
        const auto path = diagnostics_->Path();
        const auto required = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            path.data(),
            static_cast<int>(path.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        std::string utf8Path;
        if (required > 0) {
            utf8Path.resize(static_cast<std::size_t>(required));
            WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                path.data(),
                static_cast<int>(path.size()),
                utf8Path.data(),
                required,
                nullptr,
                nullptr);
        }
        const auto payload =
            experimental::EncodeDiagnosticSnapshotResult(
                request,
                 nonce_,
                 utf8Path,
                 "SNAPSHOT_READY",
                 request.flush
                    ? "The native log is current; the launcher can assemble the diagnostics archive."
                    : "The current native log path was returned.");
        return QueueExperimental(
            protocol::MessageType::DiagnosticSnapshotResult,
            payload);
    }
    case protocol::MessageType::WorldIntentRejected: {
        if (!sessionConfigured_.load(std::memory_order_acquire)) return false;
        WorldIntent intent{};
        if (!world_replication_codec::DecodeIntent(frame.payload, intent)) return false;
        // A stale/already-retired disposition is harmless. This IPC message
        // acknowledges non-acceptance; it never applies a WorldMutation.
        static_cast<void>(world_interactions::DiscardPublicRootIntent(intent));
        return true;
    }
    case protocol::MessageType::WorldIntentAbandoned: {
        if (!sessionConfigured_.load(std::memory_order_acquire)) return false;
        WorldIntent intent{};
        if (!world_replication_codec::DecodeIntent(frame.payload, intent)) return false;
        // Pre-bus/local-context abandonment is not an authority rejection. It
        // only retires the native observation/reservation if one exists. Most
        // startup observations never pass the native world-bus gate at all;
        // an already queued item may also race a later bus close. In neither
        // case may this local lifecycle disposition be re-labelled HostReject.
        static_cast<void>(world_interactions::DiscardPublicRootIntent(intent));
        return true;
    }
    case protocol::MessageType::WorldMutation: {
        if (!sessionConfigured_.load(std::memory_order_acquire)) return false;
        WorldMutation mutation{};
        if (!world_replication_codec::DecodeMutation(frame.payload, mutation) || frame.sequence == 0U) {
            return false;
        }
        // Step 5: Frame::sequence is the exact local dispatch identity assigned
        // by this Launcher process. It never crosses the multiplayer wire.
        mutation.nativeDispatchId = frame.sequence;
        // Semantic mutation rejection is reported asynchronously through
        // WorldMutationResult; the IPC frame itself was decoded successfully.
        static_cast<void>(replication_core::ReceiveAuthoritativeMutation(mutation));
        return true;
    }
    case protocol::MessageType::InboundNetzFrame: {
        if (!sessionConfigured_.load(std::memory_order_acquire)) {
            return false;
        }
        InboundItem item{};
        item.type = frame.type;
        item.flags = frame.flags;
        item.sequence = frame.sequence;
        item.payload = std::move(frame.payload);
        if (!inboundQueue_->Enqueue(std::move(item))) {
            diagnostics_->Log(
                "warning",
                "inbound_frame_rejected",
                "The bounded quarantine queue rejected an opaque frame.");
        }
        return true;
    }
    case protocol::MessageType::Heartbeat:
        if (!frame.payload.empty()) {
            return false;
        }
        return Queue(
            protocol::MessageType::Heartbeat,
            0,
            localSequence_.fetch_add(1, std::memory_order_relaxed),
            {});
    case protocol::MessageType::Shutdown:
        if (!frame.payload.empty()) {
            return false;
        }
        shutdownRequested_.store(true, std::memory_order_release);
        diagnostics_->Log(
            "info",
            "ipc_shutdown_requested",
            "Launcher requested a clean DLL service shutdown.");
        return true;
    case protocol::MessageType::PeerJoin: {
        if (!sessionConfigured_.load(std::memory_order_acquire)) {
            return false;
        }
        PeerJoinControl peer;
        if (!ipc_validation::DecodePeerJoin(frame.payload, peer) ||
            peer.peerId == localPeerId_ ||
            peer.launcherPlayerId == localLauncherPlayerId_ ||
            remotePeers_.contains(peer.peerId) ||
            remotePeers_.size() >= 7 ||
            std::any_of(
                remotePeers_.begin(),
                remotePeers_.end(),
                [&](const auto& existing) {
                    return existing.second.launcherPlayerId ==
                        peer.launcherPlayerId;
                })) {
            return false;
        }
        const auto peerId = peer.peerId;
        const auto displayName = peer.displayName;
        const auto isHost = peer.isHost;
        const auto ready = peer.ready;
        remotePeers_.emplace(peerId, std::move(peer));
        diagnostics_->Log(
            "info",
            "ipc_peer_join_quarantined",
            "Validated remote launcher identity recorded; no native participant mutation is implemented.",
            {{"peerId", Diagnostics::Unsigned(peerId), true},
             {"displayName", displayName},
             {"isHost", Diagnostics::Boolean(isHost), true},
             {"ready", Diagnostics::Boolean(ready), true}});
        return true;
    }
    case protocol::MessageType::PeerLeave: {
        if (!sessionConfigured_.load(std::memory_order_acquire)) {
            return false;
        }
        std::uint64_t peerId = 0;
        if (!ipc_validation::DecodePeerLeave(frame.payload, peerId) ||
            remotePeers_.erase(peerId) != 1) {
            return false;
        }
        diagnostics_->Log(
            "info",
            "ipc_peer_leave_quarantined",
            "Known remote launcher identity removed; no native participant mutation was implemented.",
            {{"peerId", Diagnostics::Unsigned(peerId), true}});
        return true;
    }
    case protocol::MessageType::PeerBind:
    case protocol::MessageType::LevelChanged:
    case protocol::MessageType::ResyncRequest: {
        if (frame.payload.empty() || frame.payload.size() > 64U * 1024U) {
            return false;
        }
        JsonObject controlObject;
        ClosedJsonParser controlParser(frame.payload);
        if (!controlParser.Parse(controlObject) || controlObject.empty()) {
            return false;
        }
        diagnostics_->Log(
            "info",
            "ipc_control_quarantined",
            "Control message recorded; no native mutation is implemented.",
            {{"messageType",
              Diagnostics::Unsigned(static_cast<std::uint16_t>(frame.type)),
              true}});
        return true;
    }
    default:
        diagnostics_->Log(
            "warning",
            "ipc_unexpected_message",
            "The launcher sent a message not accepted in the DLL-client direction.",
            {{"messageType",
              Diagnostics::Unsigned(static_cast<std::uint16_t>(frame.type)),
              true}});
        return true;
    }
}

bool LauncherIPC::WriteFrame(const std::span<const std::byte> bytes) {
    return WriteExact(bytes.data(), bytes.size());
}

bool LauncherIPC::ReadExact(void* destination, const std::size_t bytes) {
    const auto pipe = pipe_.load(std::memory_order_acquire);
    if (pipe == INVALID_HANDLE_VALUE || bytes > kMaximumIpcPayload + protocol::kWireHeaderBytes) {
        return false;
    }

    auto* cursor = static_cast<std::byte*>(destination);
    std::size_t offset = 0;
    while (offset < bytes) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr) {
            return false;
        }
        const auto request = static_cast<DWORD>(
            (std::min<std::size_t>)(
                bytes - offset,
                std::numeric_limits<DWORD>::max()));
        DWORD transferred = 0;
        const BOOL immediate = ReadFile(
            pipe,
            cursor + offset,
            request,
            &transferred,
            &operation);
        bool complete = immediate != FALSE;
        if (!complete && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[] = {stopEvent_, operation.hEvent};
            const auto wait = WaitForMultipleObjects(2, waits, FALSE, 5000);
            complete =
                wait == WAIT_OBJECT_0 + 1 &&
                GetOverlappedResult(pipe, &operation, &transferred, FALSE);
            if (!complete) {
                CancelIoEx(pipe, &operation);
                DWORD cancelledBytes = 0;
                GetOverlappedResult(
                    pipe,
                    &operation,
                    &cancelledBytes,
                    TRUE);
            }
        }
        CloseHandle(operation.hEvent);
        if (!complete || transferred == 0 || transferred > request) {
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool LauncherIPC::WriteExact(const void* source, const std::size_t bytes) {
    const auto pipe = pipe_.load(std::memory_order_acquire);
    if (pipe == INVALID_HANDLE_VALUE ||
        bytes > kMaximumIpcPayload + protocol::kWireHeaderBytes) {
        return false;
    }

    const auto* cursor = static_cast<const std::byte*>(source);
    std::size_t offset = 0;
    while (offset < bytes) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr) {
            return false;
        }
        const auto request = static_cast<DWORD>(
            (std::min<std::size_t>)(
                bytes - offset,
                std::numeric_limits<DWORD>::max()));
        DWORD transferred = 0;
        const BOOL immediate = WriteFile(
            pipe,
            cursor + offset,
            request,
            &transferred,
            &operation);
        bool complete = immediate != FALSE;
        if (!complete && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[] = {stopEvent_, operation.hEvent};
            const auto wait = WaitForMultipleObjects(2, waits, FALSE, 5000);
            complete =
                wait == WAIT_OBJECT_0 + 1 &&
                GetOverlappedResult(pipe, &operation, &transferred, FALSE);
            if (!complete) {
                CancelIoEx(pipe, &operation);
                DWORD cancelledBytes = 0;
                GetOverlappedResult(
                    pipe,
                    &operation,
                    &cancelledBytes,
                    TRUE);
            }
        }
        CloseHandle(operation.hEvent);
        if (!complete || transferred == 0 || transferred > request) {
            return false;
        }
        offset += transferred;
    }
    return true;
}

std::vector<std::byte> LauncherIPC::BuildHelloPayload() const {
    std::ostringstream json;
    json << "{\"protocolVersion\":" << kProtocolVersion
         << ",\"dllVersion\":\"" << kNativeVersion << "\""
         << ",\"processId\":" << processId_
         << ",\"observationOnly\":true"
         << ",\"aiSyncV1Available\":" << (ai_sync::Installed() ? "true" : "false")
         << ",\"uiHeapSynchronized\":" << (ui_heap_guard::Installed() ? "true" : "false")
         << ",\"nonce\":\"" << nonce_ << "\"}";
    return JsonBytes(json.str());
}

std::vector<std::byte> LauncherIPC::BuildInfoPayload() const {
    std::ostringstream json;
    json << "{\"aiSha256\":\"" << Sha256Hex(executableHash_)
         << "\",\"profileId\":\"" << profileId_
         << "\",\"profileValidated\":" << (buildValidated_ ? "true" : "false")
         << '}';
    return JsonBytes(json.str());
}

std::vector<std::byte> LauncherIPC::BuildObservationStatusPayload() const {
    return JsonBytes(
        "{\"mode\":\"observation\","
        "\"nativeReplicationReady\":false,"
        "\"blockedGates\":["
        "\"outbound_payload\","
        "\"range_provider\","
        "\"inbound_framing_sink_reader\","
        "\"validated_game_thread\","
        "\"remote_character_runtime_lifecycle\"]}");
}

bool LauncherIPC::QueueExperimental(
    const protocol::MessageType type,
    const std::span<const std::byte> payload) {
    return Queue(
        type,
        0,
        localSequence_.fetch_add(1, std::memory_order_relaxed),
        payload);
}

LauncherIPC::ExperimentalReservation
LauncherIPC::ReserveExperimentalRequest(
    const std::string_view requestId,
    const protocol::MessageType responseType,
    const std::span<const std::byte> requestPayload) {
    std::lock_guard lock(experimentalStateMutex_);
    const auto cached = experimentalResponses_.find(requestId);
    if (cached != experimentalResponses_.end()) {
        const bool sameRequest =
            cached->second.type == responseType &&
            cached->second.requestPayload.size() == requestPayload.size() &&
            std::equal(
                requestPayload.begin(),
                requestPayload.end(),
                cached->second.requestPayload.begin());
        if (!sameRequest) {
            return {
                ExperimentalReservationState::Collision,
                responseType,
                {},
                experimentalEpoch_.load(std::memory_order_acquire)};
        }
        return {
            ExperimentalReservationState::Cached,
            cached->second.type,
            cached->second.responsePayload,
            experimentalEpoch_.load(std::memory_order_acquire)};
    }

    const auto pending = experimentalPending_.find(requestId);
    if (pending != experimentalPending_.end()) {
        const bool sameRequest =
            pending->second.type == responseType &&
            pending->second.requestPayload.size() == requestPayload.size() &&
            std::equal(
                requestPayload.begin(),
                requestPayload.end(),
                pending->second.requestPayload.begin());
        return {
            sameRequest
                ? ExperimentalReservationState::PendingDuplicate
                : ExperimentalReservationState::Collision,
            responseType,
            {},
            pending->second.epoch};
    }
    constexpr std::size_t kMaximumPendingExperimentalRequests = 64;
    if (experimentalPending_.size() >=
        kMaximumPendingExperimentalRequests) {
        return {
            ExperimentalReservationState::CapacityReached,
            responseType,
            {},
            experimentalEpoch_.load(std::memory_order_acquire)};
    }

    const auto epoch =
        experimentalEpoch_.load(std::memory_order_acquire);
    experimentalPending_.emplace(
        std::string(requestId),
        ExperimentalPendingRequest{
            responseType,
            std::vector<std::byte>(
                requestPayload.begin(),
                requestPayload.end()),
            epoch});
    return {
        ExperimentalReservationState::Reserved,
        responseType,
        {},
        epoch};
}

bool LauncherIPC::CompleteExperimentalRequest(
    const std::string_view requestId,
    const std::uint64_t epoch,
    const protocol::MessageType responseType,
    const std::span<const std::byte> requestPayload,
    const std::span<const std::byte> responsePayload) {
    {
        std::lock_guard lock(experimentalStateMutex_);
        if (experimentalEpoch_.load(std::memory_order_acquire) != epoch) {
            return false;
        }
        const auto pending = experimentalPending_.find(requestId);
        if (pending == experimentalPending_.end() ||
            pending->second.epoch != epoch ||
            pending->second.type != responseType ||
            pending->second.requestPayload.size() !=
                requestPayload.size() ||
            !std::equal(
                requestPayload.begin(),
                requestPayload.end(),
                pending->second.requestPayload.begin())) {
            return false;
        }
        experimentalPending_.erase(pending);
        constexpr std::size_t kMaximumCachedExperimentalResponses = 64;
        if (experimentalResponses_.size() >=
            kMaximumCachedExperimentalResponses) {
            experimentalResponses_.erase(
                experimentalResponses_.begin());
        }
        experimentalResponses_.insert_or_assign(
            std::string(requestId),
            ExperimentalCachedResponse{
                responseType,
                std::vector<std::byte>(
                    requestPayload.begin(),
                    requestPayload.end()),
                std::vector<std::byte>(
                    responsePayload.begin(),
                    responsePayload.end())});
    }
    return QueueExperimental(responseType, responsePayload);
}

void LauncherIPC::ResetExperimentalRequests() noexcept {
    experimentalEpoch_.fetch_add(1, std::memory_order_acq_rel);
    try {
        std::lock_guard lock(experimentalStateMutex_);
        experimentalPending_.clear();
        experimentalResponses_.clear();
    } catch (...) {
    }
}

std::uint64_t LauncherIPC::NextExperimentalDispatchId() noexcept {
    constexpr std::uint64_t kExperimentalNamespace =
        0x8000000000000000ULL;
    constexpr std::uint64_t kSequenceMask =
        0x7FFFFFFFFFFFFFFFULL;
    auto sequence = experimentalDispatchSequence_.fetch_add(
        1,
        std::memory_order_relaxed) & kSequenceMask;
    if (sequence == 0) {
        sequence = experimentalDispatchSequence_.fetch_add(
            1,
            std::memory_order_relaxed) & kSequenceMask;
        if (sequence == 0) {
            sequence = 1;
        }
    }
    return kExperimentalNamespace | sequence;
}

bool LauncherIPC::WaitStop(const DWORD milliseconds) const noexcept {
    return stopEvent_ != nullptr &&
        WaitForSingleObject(stopEvent_, milliseconds) == WAIT_OBJECT_0;
}

} // namespace aimp
