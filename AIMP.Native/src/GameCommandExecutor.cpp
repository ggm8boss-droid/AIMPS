#include "aimp/GameCommandExecutor.h"
#include "aimp/AISync.h"
#include "aimp/ReplicationCore.h"
#include "aimp/PlayerTorchRuntime.h"
#include "aimp/PlayerHidingRuntime.h"
#include "aimp/PlayerHidingPresentationRuntime.h"
#include "aimp/AnimationSyncRuntime.h"
#include "aimp/HidingDoorPresentation.h"
#include "aimp/MissionSync.h"
#include "aimp/AlienPvPInputIsolation.h"

#include "aimp/BuildProfile.h"
#include "aimp/CaptureCatalog.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CharacterActionRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/GameThreadObservationHooks.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/ItemSync.h"
#include "aimp/NativeLimits.h"
#include "aimp/SignatureResolver.h"
#include "aimp/SpawnCharacterTracker.h"
#include "aimp/WorldEntityRegistry.h"
#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/WorldAnimatedAdapters.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <tuple>
#include <vector>
#include <string_view>
#include <utility>

namespace aimp {
namespace {

std::string WorldEntityIdText(const WorldEntityId& value) {
    std::array<char, 33> buffer{};
    std::snprintf(
        buffer.data(),
        buffer.size(),
        "%016llX%016llX",
        static_cast<unsigned long long>(value.high),
        static_cast<unsigned long long>(value.low));
    return buffer.data();
}

std::string ShortGuidText(const std::uint32_t value) {
    std::array<char, 11> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "0x%08X", value);
    return buffer.data();
}

const char* WorldEntityKindText(WorldEntityKind kind) noexcept {
    switch (kind) {
        case WorldEntityKind::Function: return "Function";
        case WorldEntityKind::Variable: return "Variable";
        case WorldEntityKind::Proxy: return "Proxy";
        case WorldEntityKind::Alias: return "Alias";
        case WorldEntityKind::CompositeInstance: return "CompositeInstance";
        case WorldEntityKind::Override: return "Override";
        default: return "Unknown";
    }
}

// Fix3.23 is deliberately a two-snapshot recorder instead of another
// function-by-function micro-probe. The first WORLD_INTERACTION_STATS captures
// the complete active provider/runtime surface exposed by WorldEntityRegistry;
// the second command captures it again and computes one normalized local diff.
// No raw process address is logged or transported.
enum class Fix323ChangeKind : std::uint8_t {
    Modified = 0,
    Appeared = 1,
    Disappeared = 2,
};

struct Fix323ScalarTransition final {
    std::uint16_t scope{}; // 0 = object prefix, 1 = one-hop pointee block.
    std::uint16_t ownerOffset{};
    std::uint16_t offset{};
    std::uint16_t reserved{};
    std::uint32_t before{};
    std::uint32_t after{};
};

struct Fix323PointeeChange final {
    std::uint16_t ownerOffset{};
    std::uint16_t beforeReadableBytes{};
    std::uint16_t afterReadableBytes{};
    std::uint16_t reserved{};
    std::uint64_t beforeHash{};
    std::uint64_t afterHash{};
};

struct Fix323SuppressedPointerSlot final {
    std::uint16_t scope{};
    std::uint16_t ownerOffset{};
    std::uint16_t offset{};
    std::uint16_t reserved{};
};

struct Fix323DeltaEntry final {
    Fix323ChangeKind changeKind{Fix323ChangeKind::Modified};
    WorldOpaqueStateSource source{WorldOpaqueStateSource::ProviderRegistry};
    std::uint32_t sourceIndex{};
    std::uint32_t providerContextKey{};
    CathodeRuntimeKey runtimeKey{};
    bool runtimeKeyValid{};
    WorldEntityId worldEntity{};
    bool worldEntityValid{};
    std::uint16_t beforeReadableBytes{};
    std::uint16_t afterReadableBytes{};
    std::uint64_t beforeHash{};
    std::uint64_t afterHash{};
    std::uint32_t objectChangedWords{};
    std::uint32_t pointeeChangedBlocks{};
    std::uint32_t suppressedPointerWords{};
    std::vector<Fix323ScalarTransition> transitions;
    std::vector<Fix323PointeeChange> pointeeChanges;
    std::vector<Fix323SuppressedPointerSlot> suppressedPointerSlots;
};

struct Fix323Diagnostics final {
    // 0 idle/unavailable, 1 baseline ready, 2 diff ready,
    // 10 baseline capture in progress, 11 after capture in progress.
    std::uint32_t phase{};
    std::uint32_t levelGeneration{};
    WorldOpaqueStateSnapshot baseline{};
    WorldOpaqueStateSnapshot after{};
    std::uint64_t unchanged{};
    std::uint64_t modified{};
    std::uint64_t appeared{};
    std::uint64_t disappeared{};
    std::uint64_t deltaEntries{};
    std::vector<Fix323DeltaEntry> deltas;
};

struct Fix323Identity final {
    std::uint8_t source{};
    std::uint8_t runtimeKeyValid{};
    std::uint16_t reserved{};
    std::uint32_t entityA{};
    std::uint32_t entityB{};
    std::uint32_t providerContextKey{};
    std::uint32_t sourceIndex{};

    [[nodiscard]] auto Tie() const noexcept {
        return std::tie(
            source,
            runtimeKeyValid,
            entityA,
            entityB,
            providerContextKey,
            sourceIndex);
    }
};

enum class Fix323CaptureMode : std::uint8_t {
    None = 0,
    Baseline = 1,
    After = 2,
};

std::mutex gFix323Mutex;
std::uint32_t gFix323Generation = 0U;
std::uint32_t gFix323Phase = 0U;
WorldOpaqueStateSnapshot gFix323Baseline;
Fix323Diagnostics gFix323Result;
WorldOpaqueStateCaptureCursor gFix323Cursor;
Fix323CaptureMode gFix323CaptureMode{Fix323CaptureMode::None};
bool gFix323CaptureTaskQueued = false;
std::uint64_t gFix323CaptureStepCount = 0U;
std::uint64_t gFix323NextStepTickMs = 0U;
// Fix3.28 reuses the proven Fix3.23 incremental state snapshot engine through
// dedicated commands that do not arm WORLD_INTERACTION_STATS historical probes.
// This flag changes only operator-facing diagnostics/popups; capture semantics
// remain local-only and memory-write-free.
bool gFix328StateDiscoveryActive = false;
std::uint64_t gFix328StateDiscoverySession = 0U;

constexpr std::uint32_t kFix323CaptureSlotsPerStep = 8U;
constexpr std::uint32_t kFix323CaptureStepBudgetMs = 1U;
constexpr std::uint64_t kFix323MinimumStepIntervalMs = 4ULL;

Fix323Identity Fix323EntryIdentity(const WorldOpaqueStateEntry& entry) noexcept {
    Fix323Identity result{};
    result.source = static_cast<std::uint8_t>(entry.source);
    result.runtimeKeyValid = entry.runtimeKeyValid ? 1U : 0U;
    if (entry.runtimeKeyValid) {
        result.entityA = entry.runtimeKey.entityA;
        result.entityB = entry.runtimeKey.entityB;
        // Source index is intentionally excluded for a proven RuntimeKey so a
        // collection reorder does not manufacture a state change.
    } else {
        result.providerContextKey = entry.providerContextKey;
        result.sourceIndex = entry.sourceIndex;
    }
    return result;
}

bool Fix323IdentityLess(
    const WorldOpaqueStateEntry* left,
    const WorldOpaqueStateEntry* right) noexcept {
    return Fix323EntryIdentity(*left).Tie() < Fix323EntryIdentity(*right).Tie();
}

bool Fix323IdentityEqual(
    const WorldOpaqueStateEntry& left,
    const WorldOpaqueStateEntry& right) noexcept {
    return Fix323EntryIdentity(left).Tie() == Fix323EntryIdentity(right).Tie();
}

std::uint32_t Fix323ReadWord(
    const std::byte* bytes,
    const std::size_t offset) noexcept {
    std::uint32_t value = 0U;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

void Fix323AddTransition(
    Fix323DeltaEntry& delta,
    const std::uint16_t scope,
    const std::uint16_t ownerOffset,
    const std::uint16_t offset,
    const std::uint32_t before,
    const std::uint32_t after) {
    delta.transitions.push_back({});
    auto& transition = delta.transitions.back();
    transition.scope = scope;
    transition.ownerOffset = ownerOffset;
    transition.offset = offset;
    transition.before = before;
    transition.after = after;
}

void Fix323AddSuppressedPointerSlot(
    Fix323DeltaEntry& delta,
    const std::uint16_t scope,
    const std::uint16_t ownerOffset,
    const std::uint16_t offset) {
    ++delta.suppressedPointerWords;
    delta.suppressedPointerSlots.push_back(
        Fix323SuppressedPointerSlot{scope, ownerOffset, offset, 0U});
}

void Fix323AddPointeeChange(
    Fix323DeltaEntry& delta,
    const WorldOpaqueStatePointerProbe* before,
    const WorldOpaqueStatePointerProbe* after,
    const std::uint16_t ownerOffset) {
    ++delta.pointeeChangedBlocks;
    delta.pointeeChanges.push_back({});
    auto& change = delta.pointeeChanges.back();
    change.ownerOffset = ownerOffset;
    if (before != nullptr) {
        change.beforeReadableBytes = before->readableBytes;
        change.beforeHash = before->hash;
    }
    if (after != nullptr) {
        change.afterReadableBytes = after->readableBytes;
        change.afterHash = after->hash;
    }

    if (before == nullptr || after == nullptr) return;
    const auto bytes = std::min<std::size_t>(
        before->readableBytes,
        after->readableBytes);
    const auto words = bytes / sizeof(std::uint32_t);
    for (std::size_t word = 0U; word < words; ++word) {
        const auto offset = word * sizeof(std::uint32_t);
        const auto oldValue = Fix323ReadWord(before->bytes.data(), offset);
        const auto newValue = Fix323ReadWord(after->bytes.data(), offset);
        if (oldValue == newValue) continue;
        const auto pointerMask = static_cast<std::uint16_t>(
            (1U << static_cast<std::uint32_t>(word)));
        if ((before->pointerWordMask & pointerMask) != 0U ||
            (after->pointerWordMask & pointerMask) != 0U) {
            Fix323AddSuppressedPointerSlot(
                delta,
                1U,
                ownerOffset,
                static_cast<std::uint16_t>(offset));
            continue;
        }
        Fix323AddTransition(
            delta,
            1U,
            ownerOffset,
            static_cast<std::uint16_t>(offset),
            oldValue,
            newValue);
    }
}

Fix323DeltaEntry Fix323BuildModifiedDelta(
    const WorldOpaqueStateEntry& before,
    const WorldOpaqueStateEntry& after) {
    Fix323DeltaEntry delta{};
    delta.changeKind = Fix323ChangeKind::Modified;
    delta.source = after.source;
    delta.sourceIndex = after.sourceIndex;
    delta.providerContextKey = after.providerContextKey != 0U
        ? after.providerContextKey
        : before.providerContextKey;
    delta.runtimeKey = after.runtimeKeyValid ? after.runtimeKey : before.runtimeKey;
    delta.runtimeKeyValid = after.runtimeKeyValid || before.runtimeKeyValid;
    delta.worldEntity = after.worldEntityValid ? after.worldEntity : before.worldEntity;
    delta.worldEntityValid = after.worldEntityValid || before.worldEntityValid;
    delta.beforeReadableBytes = before.objectReadableBytes;
    delta.afterReadableBytes = after.objectReadableBytes;
    delta.beforeHash = before.objectHash;
    delta.afterHash = after.objectHash;

    const auto objectBytes = std::min<std::size_t>(
        before.objectReadableBytes,
        after.objectReadableBytes);
    const auto objectWords = objectBytes / sizeof(std::uint32_t);
    for (std::size_t word = 0U; word < objectWords; ++word) {
        const auto offset = word * sizeof(std::uint32_t);
        const auto oldValue = Fix323ReadWord(before.objectBytes.data(), offset);
        const auto newValue = Fix323ReadWord(after.objectBytes.data(), offset);
        if (oldValue == newValue) continue;
        ++delta.objectChangedWords;
        const auto pointerMask = 1U << static_cast<std::uint32_t>(word);
        if ((before.objectPointerWordMask & pointerMask) != 0U ||
            (after.objectPointerWordMask & pointerMask) != 0U) {
            Fix323AddSuppressedPointerSlot(
                delta,
                0U,
                0U,
                static_cast<std::uint16_t>(offset));
            continue;
        }
        Fix323AddTransition(
            delta,
            0U,
            0U,
            static_cast<std::uint16_t>(offset),
            oldValue,
            newValue);
    }
    if (before.objectReadableBytes != after.objectReadableBytes) {
        ++delta.objectChangedWords;
    }

    std::size_t beforeProbe = 0U;
    std::size_t afterProbe = 0U;
    while (beforeProbe < before.pointerProbeCount ||
           afterProbe < after.pointerProbeCount) {
        const auto* oldProbe = beforeProbe < before.pointerProbeCount
            ? &before.pointerProbes[beforeProbe]
            : nullptr;
        const auto* newProbe = afterProbe < after.pointerProbeCount
            ? &after.pointerProbes[afterProbe]
            : nullptr;
        if (oldProbe != nullptr && newProbe != nullptr &&
            oldProbe->ownerOffset == newProbe->ownerOffset) {
            if (oldProbe->hash != newProbe->hash ||
                oldProbe->readableBytes != newProbe->readableBytes) {
                Fix323AddPointeeChange(
                    delta,
                    oldProbe,
                    newProbe,
                    oldProbe->ownerOffset);
            }
            ++beforeProbe;
            ++afterProbe;
            continue;
        }
        if (newProbe == nullptr ||
            (oldProbe != nullptr && oldProbe->ownerOffset < newProbe->ownerOffset)) {
            Fix323AddPointeeChange(
                delta,
                oldProbe,
                nullptr,
                oldProbe->ownerOffset);
            ++beforeProbe;
        } else {
            Fix323AddPointeeChange(
                delta,
                nullptr,
                newProbe,
                newProbe->ownerOffset);
            ++afterProbe;
        }
    }
    return delta;
}

Fix323DeltaEntry Fix323BuildPresenceDelta(
    const WorldOpaqueStateEntry& entry,
    const Fix323ChangeKind kind) {
    Fix323DeltaEntry delta{};
    delta.changeKind = kind;
    delta.source = entry.source;
    delta.sourceIndex = entry.sourceIndex;
    delta.providerContextKey = entry.providerContextKey;
    delta.runtimeKey = entry.runtimeKey;
    delta.runtimeKeyValid = entry.runtimeKeyValid;
    delta.worldEntity = entry.worldEntity;
    delta.worldEntityValid = entry.worldEntityValid;
    if (kind == Fix323ChangeKind::Appeared) {
        delta.afterReadableBytes = entry.objectReadableBytes;
        delta.afterHash = entry.objectHash;
    } else {
        delta.beforeReadableBytes = entry.objectReadableBytes;
        delta.beforeHash = entry.objectHash;
    }
    return delta;
}

bool Fix323EntryChanged(
    const WorldOpaqueStateEntry& before,
    const WorldOpaqueStateEntry& after) noexcept {
    if (before.objectHash != after.objectHash ||
        before.objectReadableBytes != after.objectReadableBytes ||
        before.pointerProbeCount != after.pointerProbeCount) {
        return true;
    }
    for (std::size_t index = 0U; index < before.pointerProbeCount; ++index) {
        const auto& oldProbe = before.pointerProbes[index];
        const auto& newProbe = after.pointerProbes[index];
        if (oldProbe.ownerOffset != newProbe.ownerOffset ||
            oldProbe.hash != newProbe.hash ||
            oldProbe.readableBytes != newProbe.readableBytes) {
            return true;
        }
    }
    return false;
}

void Fix323ComputeDelta(
    const WorldOpaqueStateSnapshot& before,
    const WorldOpaqueStateSnapshot& after,
    Fix323Diagnostics& result) {
    std::vector<const WorldOpaqueStateEntry*> oldEntries;
    std::vector<const WorldOpaqueStateEntry*> newEntries;
    oldEntries.reserve(before.entries.size());
    newEntries.reserve(after.entries.size());
    for (const auto& entry : before.entries) oldEntries.push_back(&entry);
    for (const auto& entry : after.entries) newEntries.push_back(&entry);
    std::sort(oldEntries.begin(), oldEntries.end(), Fix323IdentityLess);
    std::sort(newEntries.begin(), newEntries.end(), Fix323IdentityLess);

    result.deltas.clear();
    result.deltas.reserve(std::max(oldEntries.size(), newEntries.size()));
    std::size_t oldIndex = 0U;
    std::size_t newIndex = 0U;
    while (oldIndex < oldEntries.size() || newIndex < newEntries.size()) {
        if (oldIndex < oldEntries.size() && newIndex < newEntries.size() &&
            Fix323IdentityEqual(*oldEntries[oldIndex], *newEntries[newIndex])) {
            if (Fix323EntryChanged(*oldEntries[oldIndex], *newEntries[newIndex])) {
                ++result.modified;
                result.deltas.push_back(Fix323BuildModifiedDelta(
                    *oldEntries[oldIndex], *newEntries[newIndex]));
            } else {
                ++result.unchanged;
            }
            ++oldIndex;
            ++newIndex;
            continue;
        }

        if (newIndex >= newEntries.size() ||
            (oldIndex < oldEntries.size() &&
             Fix323EntryIdentity(*oldEntries[oldIndex]).Tie() <
                 Fix323EntryIdentity(*newEntries[newIndex]).Tie())) {
            ++result.disappeared;
            result.deltas.push_back(Fix323BuildPresenceDelta(
                *oldEntries[oldIndex], Fix323ChangeKind::Disappeared));
            ++oldIndex;
        } else {
            ++result.appeared;
            result.deltas.push_back(Fix323BuildPresenceDelta(
                *newEntries[newIndex], Fix323ChangeKind::Appeared));
            ++newIndex;
        }
    }
}

void Fix323CopySnapshotSummary(
    WorldOpaqueStateSnapshot& destination,
    const WorldOpaqueStateSnapshot& source) {
    destination = {};
    destination.levelGeneration = source.levelGeneration;
    destination.providerEntries = source.providerEntries;
    destination.managerLoadedEntries = source.managerLoadedEntries;
    destination.managerRootEntries = source.managerRootEntries;
    destination.inspectedEntries = source.inspectedEntries;
    destination.capturedEntries = source.capturedEntries;
    destination.runtimeKeyEntries = source.runtimeKeyEntries;
    destination.worldMappedEntries = source.worldMappedEntries;
    destination.objectReadFailures = source.objectReadFailures;
    destination.objectPartialReads = source.objectPartialReads;
    destination.pointerProbeReadFailures = source.pointerProbeReadFailures;
    destination.pointerProbePartialReads = source.pointerProbePartialReads;
    destination.capacityDrops = source.capacityDrops;
    destination.elapsedMs = source.elapsedMs;
}

std::string Fix323BuildEvidenceJson(const Fix323Diagnostics& result) {
    std::ostringstream json;
    json << '[';
    for (std::size_t index = 0U; index < result.deltas.size(); ++index) {
        if (index != 0U) json << ',';
        const auto& delta = result.deltas[index];
        json << '{'
             << "\"i\":" << index
             << ",\"kind\":" << static_cast<std::uint32_t>(delta.changeKind)
             << ",\"source\":" << static_cast<std::uint32_t>(delta.source)
             << ",\"sourceIndex\":" << delta.sourceIndex
             << ",\"contextKey\":\"" << ShortGuidText(delta.providerContextKey) << '\"'
             << ",\"runtimeKeyValid\":" << (delta.runtimeKeyValid ? 1 : 0)
             << ",\"runtimeKey\":\"" << ShortGuidText(delta.runtimeKey.entityA)
             << '/' << ShortGuidText(delta.runtimeKey.entityB) << '\"'
             << ",\"worldIdValid\":" << (delta.worldEntityValid ? 1 : 0)
             << ",\"worldId\":\"" << WorldEntityIdText(delta.worldEntity) << '\"'
             << ",\"beforeReadable\":" << delta.beforeReadableBytes
             << ",\"afterReadable\":" << delta.afterReadableBytes
             << ",\"beforeHash\":" << delta.beforeHash
             << ",\"afterHash\":" << delta.afterHash
             << ",\"objectChangedWords\":" << delta.objectChangedWords
             << ",\"pointeeChangedBlocks\":" << delta.pointeeChangedBlocks
             << ",\"suppressedPointerWords\":" << delta.suppressedPointerWords;

        json << ",\"scalars\":[";
        for (std::size_t i = 0U; i < delta.transitions.size(); ++i) {
            if (i != 0U) json << ',';
            const auto& transition = delta.transitions[i];
            json << '{'
                 << "\"scope\":" << transition.scope
                 << ",\"ownerOffset\":" << transition.ownerOffset
                 << ",\"offset\":" << transition.offset
                 << ",\"before\":" << transition.before
                 << ",\"after\":" << transition.after
                 << '}';
        }
        json << ']';

        json << ",\"pointees\":[";
        for (std::size_t i = 0U; i < delta.pointeeChanges.size(); ++i) {
            if (i != 0U) json << ',';
            const auto& change = delta.pointeeChanges[i];
            json << '{'
                 << "\"ownerOffset\":" << change.ownerOffset
                 << ",\"beforeReadable\":" << change.beforeReadableBytes
                 << ",\"afterReadable\":" << change.afterReadableBytes
                 << ",\"beforeHash\":" << change.beforeHash
                 << ",\"afterHash\":" << change.afterHash
                 << '}';
        }
        json << ']';

        json << ",\"pointerSlots\":[";
        for (std::size_t i = 0U; i < delta.suppressedPointerSlots.size(); ++i) {
            if (i != 0U) json << ',';
            const auto& slot = delta.suppressedPointerSlots[i];
            json << '{'
                 << "\"scope\":" << slot.scope
                 << ",\"ownerOffset\":" << slot.ownerOffset
                 << ",\"offset\":" << slot.offset
                 << '}';
        }
        json << "]}";
    }
    json << ']';
    return json.str();
}

void Fix323ResetForGenerationLocked(const std::uint32_t generation) {
    gFix323Generation = generation;
    gFix323Phase = 0U;
    gFix323Baseline = {};
    gFix323Result = {};
    gFix323Cursor = {};
    gFix323CaptureMode = Fix323CaptureMode::None;
    gFix323CaptureTaskQueued = false;
    gFix323CaptureStepCount = 0U;
    gFix323NextStepTickMs = 0U;
    gFix328StateDiscoveryActive = false;
}

void Fix323ShowReadyPopup(
    const GameCommandExecutor* owner,
    const Fix323CaptureMode mode) noexcept {
    if (owner == nullptr) return;
    bool stateDiscovery = false;
    {
        std::lock_guard lock(gFix323Mutex);
        stateDiscovery = gFix328StateDiscoveryActive;
    }
    PopupMessageCommand popup{};
    popup.headerText = stateDiscovery
        ? "WorldSync Fix3.28 State Discovery"
        : "WorldSync Fix3.23";
    popup.mainText = mode == Fix323CaptureMode::Baseline
        ? (stateDiscovery
            ? "Baseline complete - trigger exactly one witness action, then run After / Diff."
            : "Baseline capture complete - perform the witness interaction.")
        : (stateDiscovery
            ? "State diff complete - export diagnostics."
            : "After capture and full diff complete - diagnostics are ready to export.");
    popup.durationSeconds = 4.0F;
    try {
        (void)owner->ExecutePopupMessage(popup);
    } catch (...) {
    }
}

void ScheduleFix323CaptureStep(
    const GameCommandExecutor* owner,
    WorldEntityRegistry* registry,
    GameThreadDispatcher* dispatcher,
    Diagnostics* diagnostics);

void FinishFix323Capture(
    const GameCommandExecutor* owner,
    WorldEntityRegistry* registry,
    Diagnostics* diagnostics) {
    Fix323CaptureMode mode = Fix323CaptureMode::None;
    WorldOpaqueStateSnapshot snapshot{};
    bool stateDiscovery = false;
    std::uint64_t stateDiscoverySession = 0U;
    {
        std::lock_guard lock(gFix323Mutex);
        mode = gFix323CaptureMode;
        stateDiscovery = gFix328StateDiscoveryActive;
        stateDiscoverySession = gFix328StateDiscoverySession;
        snapshot = std::move(gFix323Cursor.snapshot);
        gFix323Cursor = {};
        gFix323CaptureMode = Fix323CaptureMode::None;
        gFix323CaptureTaskQueued = false;
        gFix323NextStepTickMs = 0U;
    }

    if (mode == Fix323CaptureMode::None) return;
    if (registry != nullptr &&
        registry->DiagnosticsSnapshot().levelGeneration != snapshot.levelGeneration) {
        std::lock_guard lock(gFix323Mutex);
        Fix323ResetForGenerationLocked(
            registry == nullptr ? 0U : registry->DiagnosticsSnapshot().levelGeneration);
        return;
    }

    if (mode == Fix323CaptureMode::Baseline) {
        {
            std::lock_guard lock(gFix323Mutex);
            gFix323Baseline = std::move(snapshot);
            gFix323Phase = 1U;
            gFix323Result = {};
            gFix323Result.phase = 1U;
            gFix323Result.levelGeneration = gFix323Generation;
            Fix323CopySnapshotSummary(gFix323Result.baseline, gFix323Baseline);
        }
        if (diagnostics != nullptr) {
            Fix323Diagnostics summary{};
            {
                std::lock_guard lock(gFix323Mutex);
                summary = gFix323Result;
            }
            diagnostics->Log(
                "info",
                stateDiscovery
                    ? "worldsync_fix328_state_discovery_baseline_complete"
                    : "worldsync_fix323_full_state_capture_complete",
                stateDiscovery
                    ? "Fix3.28 state-discovery baseline complete; local observation only and no historical probe was armed."
                    : "Fix3.23 BuildFix01 incremental baseline capture complete; the engine thread was never monopolized by one monolithic scan.",
                {
                    {"mode", "baseline"},
                    {"session", std::to_string(stateDiscovery ? stateDiscoverySession : 0U), true},
                    {"generation", std::to_string(summary.levelGeneration), true},
                    {"providers", std::to_string(summary.baseline.providerEntries), true},
                    {"captured", std::to_string(summary.baseline.capturedEntries), true},
                    {"inspected", std::to_string(summary.baseline.inspectedEntries), true},
                    {"elapsedMs", std::to_string(summary.baseline.elapsedMs), true},
                    {"steps", std::to_string(gFix323CaptureStepCount), true},
                });
        }
        Fix323ShowReadyPopup(owner, mode);
        return;
    }

    WorldOpaqueStateSnapshot baseline{};
    {
        std::lock_guard lock(gFix323Mutex);
        baseline = std::move(gFix323Baseline);
    }

    Fix323Diagnostics computed{};
    computed.phase = 2U;
    computed.levelGeneration = snapshot.levelGeneration;
    Fix323CopySnapshotSummary(computed.baseline, baseline);
    Fix323CopySnapshotSummary(computed.after, snapshot);
    Fix323ComputeDelta(baseline, snapshot, computed);
    computed.deltaEntries = computed.deltas.size();
    const auto evidenceJson = Fix323BuildEvidenceJson(computed);

    if (diagnostics != nullptr) {
        diagnostics->Log(
            "info",
            stateDiscovery
                ? "worldsync_fix328_state_discovery_summary"
                : "worldsync_fix323_full_state_summary",
            stateDiscovery
                ? "Fix3.28 generic CATHODE state differential complete; local observation only."
                : "Fix3.23 BuildFix01 incremental full active CATHODE differential complete; local observation only.",
            {
                {"phase", "2", true},
                {"session", std::to_string(stateDiscovery ? stateDiscoverySession : 0U), true},
                {"generation", std::to_string(computed.levelGeneration), true},
                {"baseProviders", std::to_string(computed.baseline.providerEntries), true},
                {"baseCaptured", std::to_string(computed.baseline.capturedEntries), true},
                {"afterProviders", std::to_string(computed.after.providerEntries), true},
                {"afterCaptured", std::to_string(computed.after.capturedEntries), true},
                {"unchanged", std::to_string(computed.unchanged), true},
                {"modified", std::to_string(computed.modified), true},
                {"appeared", std::to_string(computed.appeared), true},
                {"disappeared", std::to_string(computed.disappeared), true},
                {"deltaEntries", std::to_string(computed.deltaEntries), true},
                {"steps", std::to_string(gFix323CaptureStepCount), true},
            });
        diagnostics->Log(
            "info",
            stateDiscovery
                ? "worldsync_fix328_state_discovery_evidence"
                : "worldsync_fix323_full_state_evidence",
            stateDiscovery
                ? "Fix3.28 normalized CATHODE state evidence. Scalar transitions are local diagnostic values; raw pointer-shaped words remain suppressed."
                : "Fix3.23 BuildFix01 complete normalized state evidence. All scalar/pointee/pointer-slot details are packed into this single JSON array to avoid thousands of synchronous write-through log writes.",
            {
                {"session", std::to_string(stateDiscovery ? stateDiscoverySession : 0U), true},
                {"generation", std::to_string(computed.levelGeneration), true},
                {"deltaEntries", std::to_string(computed.deltaEntries), true},
                {"deltas", evidenceJson, true},
            });
    }

    computed.deltas.clear();
    computed.deltas.shrink_to_fit();
    {
        std::lock_guard lock(gFix323Mutex);
        gFix323Result = std::move(computed);
        gFix323Phase = 2U;
    }
    Fix323ShowReadyPopup(owner, mode);
}

void ScheduleFix323CaptureStep(
    const GameCommandExecutor* owner,
    WorldEntityRegistry* registry,
    GameThreadDispatcher* dispatcher,
    Diagnostics* diagnostics) {
    if (owner == nullptr || registry == nullptr || dispatcher == nullptr) {
        return;
    }
    {
        std::lock_guard lock(gFix323Mutex);
        if (gFix323CaptureMode == Fix323CaptureMode::None ||
            gFix323CaptureTaskQueued) {
            return;
        }
        gFix323CaptureTaskQueued = true;
    }

    const auto queued = dispatcher->Enqueue([owner, registry, dispatcher, diagnostics] {
        bool complete = false;
        bool doWork = false;
        {
            std::lock_guard lock(gFix323Mutex);
            gFix323CaptureTaskQueued = false;
            if (gFix323CaptureMode == Fix323CaptureMode::None) {
                return;
            }
            const auto now = static_cast<std::uint64_t>(GetTickCount64());
            doWork = now >= gFix323NextStepTickMs;
            if (doWork) {
                complete = registry->ContinueOpaqueRuntimeStateCapture(
                    gFix323Cursor,
                    kFix323CaptureSlotsPerStep,
                    kFix323CaptureStepBudgetMs);
                ++gFix323CaptureStepCount;
                gFix323NextStepTickMs = now + kFix323MinimumStepIntervalMs;
            }
        }

        if (complete) {
            FinishFix323Capture(owner, registry, diagnostics);
            return;
        }
        ScheduleFix323CaptureStep(owner, registry, dispatcher, diagnostics);
    });

    if (!queued) {
        {
            std::lock_guard lock(gFix323Mutex);
            gFix323CaptureTaskQueued = false;
            gFix323CaptureMode = Fix323CaptureMode::None;
            gFix323Cursor = {};
            gFix323Result.phase = gFix323Phase;
        }
        if (diagnostics != nullptr) {
            diagnostics->Log(
                "warning",
                "worldsync_fix323_capture_enqueue_failed",
                "Fix3.23 BuildFix01 could not enqueue the next bounded capture slice. The recorder was reset fail-closed and no gameplay state was modified.");
        }
    }
}

Fix323Diagnostics AdvanceFix323FullStateCapture(
    const GameCommandExecutor* owner,
    WorldEntityRegistry* registry,
    GameThreadDispatcher* dispatcher,
    Diagnostics* diagnostics) {
    if (registry == nullptr || dispatcher == nullptr ||
        !registry->RuntimeCaptureReady() || !dispatcher->IsEnabled()) {
        return {};
    }
    const auto generation = registry->DiagnosticsSnapshot().levelGeneration;

    Fix323CaptureMode modeToStart = Fix323CaptureMode::None;
    {
        std::lock_guard lock(gFix323Mutex);
        if (gFix323Generation != generation) {
            Fix323ResetForGenerationLocked(generation);
        }

        if (gFix323CaptureMode != Fix323CaptureMode::None) {
            auto current = gFix323Result;
            current.phase = gFix323CaptureMode == Fix323CaptureMode::Baseline
                ? 10U
                : 11U;
            current.levelGeneration = gFix323Generation;
            if (gFix323CaptureMode == Fix323CaptureMode::Baseline) {
                Fix323CopySnapshotSummary(current.baseline, gFix323Cursor.snapshot);
            } else {
                Fix323CopySnapshotSummary(current.after, gFix323Cursor.snapshot);
            }
            return current;
        }

        if (gFix323Phase >= 2U) {
            return gFix323Result;
        }
        modeToStart = gFix323Phase == 0U
            ? Fix323CaptureMode::Baseline
            : Fix323CaptureMode::After;
        gFix323CaptureMode = modeToStart;
        gFix323CaptureStepCount = 0U;
        gFix323NextStepTickMs = 0U;
        gFix323Cursor = {};
        registry->BeginOpaqueRuntimeStateCapture(gFix323Cursor);
        gFix323Result.phase = modeToStart == Fix323CaptureMode::Baseline
            ? 10U
            : 11U;
        gFix323Result.levelGeneration = gFix323Generation;
    }

    ScheduleFix323CaptureStep(owner, registry, dispatcher, diagnostics);

    std::lock_guard lock(gFix323Mutex);
    auto current = gFix323Result;
    if (modeToStart == Fix323CaptureMode::Baseline) {
        Fix323CopySnapshotSummary(current.baseline, gFix323Cursor.snapshot);
    } else {
        Fix323CopySnapshotSummary(current.after, gFix323Cursor.snapshot);
    }
    return current;
}

using FindNamedElementFn = void*(__thiscall*)(void*, const char*);
using PopupMessageFn = void(__thiscall*)(
    void*,
    const char*,
    const char*,
    float,
    const std::uint32_t*,
    std::uint32_t,
    const char*);
using SpawnRequest130ReceiverFn = std::uint32_t(__cdecl*)(void*, const void*);
constexpr std::size_t kSpawnKeyOffset = 0x00U;
constexpr std::size_t kEntityKeyOffset = 0x28U;
constexpr std::size_t kSelectionOffset = 0x38U;
constexpr std::size_t kParticipantLookupKeyOffset = 0x48U;
constexpr std::size_t kParticipantLookupEnabledOffset = 0x4CU;
constexpr std::size_t kPositionOffset = 0x50U;
constexpr std::size_t kCharacterClassOffset = 0xA4U;
constexpr std::size_t kExtendedSpawnKeyOffset = 0xF0U;
constexpr std::size_t kExtendedModeOffset = 0xF4U;
constexpr std::size_t kExtendedParameterOffset = 0xFCU;
// Spawn130 character-name fields (FNV-1a 32 of the crew name).
constexpr std::size_t kAnimSetOffset = 0x14U;
constexpr std::size_t kReferenceSkeletonOffset = 0x20U;
constexpr std::size_t kDisplayModelOffset = 0x24U;
constexpr std::uintptr_t kSpawn130SinkOffset = 0x14U;
constexpr std::size_t kParticipantRecordSize = 0x158U;
constexpr std::size_t kParticipantTableSize = 0xAC0U;
constexpr std::size_t kParticipantTablePointerOffset = 0xAC0U;
constexpr std::size_t kParticipantCountOffset = 0xAC4U;
constexpr std::size_t kParticipantNameOffset = 0x00U;
constexpr std::size_t kParticipantNameCapacity = 0x80U;
constexpr std::size_t kParticipantIdentityOffset = 0x80U;
constexpr std::size_t kParticipantFlagsOffset = 0xC8U;
constexpr std::size_t kParticipantVtableOffset = 0xCCU;
constexpr std::size_t kParticipantObjectOffset = 0xD0U;
constexpr std::size_t kParticipantValueOffset = 0xD4U;
constexpr std::size_t kParticipantBindingKeyOffset = 0xF4U;
constexpr std::size_t kParticipantTrailingIndexOffset = 0x150U;
constexpr std::uint32_t kRemoteUnboundFlags = 0x05U;
constexpr std::uint32_t kParticipantBoundBit = 0x08U;
constexpr std::uint32_t kMaximumParticipantCount = 8U;
constexpr float kMaximumCoordinateMagnitude = 100000.0F;
constexpr float kMinimumCharacterDirectMoveIntensity = 1.0F;
constexpr float kMaximumCharacterDirectMoveIntensity = 4.0F;
constexpr std::uint32_t kMaximumCharacterDirectMoveFrames = 3600U;
constexpr std::string_view kCivilianClass = "CIVILIAN";
constexpr std::string_view kCharacterNamePrefix = "Character: ";
constexpr std::uintptr_t kCharacterPositionOffset = 0x70U;
constexpr std::uintptr_t kCharacterControllerOwnerOffset = 0x35A4U;
constexpr float kMaximumCharacterDirectStep = 1.0F;
constexpr float kMaximumCharacterDirectYawStep = 1.0F;
constexpr std::uint32_t kMaximumCharacterDirectStepFrames = 600U;

bool IsValidText(const std::string_view value, const bool allowLineBreaks) noexcept {
    if (value.size() > 63U) {
        return false;
    }
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == 0U || byte == 0x7FU ||
            (byte < 0x20U &&
             !(allowLineBreaks &&
               (byte == '\r' || byte == '\n' || byte == '\t')))) {
            return false;
        }
    }
    return true;
}

bool IsValidCoordinate(const float value) noexcept {
    return std::isfinite(value) &&
        std::abs(value) <= kMaximumCoordinateMagnitude;
}

std::uint32_t MixEntityKey(
    std::uint32_t spawnKey,
    std::uint32_t nonce) noexcept;

bool IsZeroIdentity(const std::array<std::byte, 16>& identity) noexcept {
    return std::all_of(
        identity.begin(),
        identity.end(),
        [](const std::byte value) {
            return value == std::byte{0};
        });
}

std::array<std::byte, 16> BuildParticipantIdentity(
    const std::uint32_t participantKey,
    const std::uint32_t spawnKey,
    const std::uint32_t entityKey,
    const std::uint32_t nonce) noexcept {
    const std::array<std::uint32_t, 4> words{
        MixEntityKey(participantKey, spawnKey ^ 0x13579BDFU),
        MixEntityKey(spawnKey, entityKey ^ 0x2468ACE0U),
        MixEntityKey(entityKey, nonce ^ 0xA5A5A5A5U),
        MixEntityKey(nonce, participantKey ^ 0x5A5A5A5AU),
    };
    std::array<std::byte, 16> identity{};
    std::memcpy(identity.data(), words.data(), identity.size());
    return identity;
}

struct LiveParticipantTable {
    std::uintptr_t wrapper{};
    std::uintptr_t owner{};
    std::uint32_t count{};
    std::uint32_t localIndex{};
};

bool ResolveLiveParticipantTable(
    const void* globalStateRootAddress,
    LiveParticipantTable& output) noexcept {
    output = {};
    const auto globalRoot =
        crash_guard::SafeRead<std::uintptr_t>(globalStateRootAddress);
    if (!globalRoot.has_value() || *globalRoot == 0U) {
        return false;
    }
    const auto wrapper = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*globalRoot + 0x0CU));
    if (!wrapper.has_value() || *wrapper == 0U) {
        return false;
    }
    const auto owner = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*wrapper));
    const auto localIndex = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(*wrapper + 0x04U));
    if (!owner.has_value() || *owner == 0U || !localIndex.has_value()) {
        return false;
    }
    const auto table = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(
            *owner + kParticipantTablePointerOffset));
    const auto count = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(*owner + kParticipantCountOffset));
    if (!table.has_value() || *table != *owner ||
        !count.has_value() || *count == 0U ||
        *count >= kMaximumParticipantCount ||
        *localIndex >= *count ||
        !crash_guard::IsReadable(
            reinterpret_cast<const void*>(*owner),
            kParticipantTableSize) ||
        !crash_guard::IsWritable(
            reinterpret_cast<void*>(*owner),
            kParticipantCountOffset + sizeof(std::uint32_t))) {
        return false;
    }
    output.wrapper = *wrapper;
    output.owner = *owner;
    output.count = *count;
    output.localIndex = *localIndex;
    return true;
}

class LiveParticipantTransaction final {
public:
    ~LiveParticipantTransaction() {
        if (active_) {
            Rollback();
        }
    }

    bool Begin(
        const void* globalStateRootAddress,
        const std::uint32_t participantKey,
        const std::string_view displayName,
        const std::array<std::byte, 16>& identity,
        std::string& rejectionReason) {
        LiveParticipantTable table{};
        if (!ResolveLiveParticipantTable(globalStateRootAddress, table)) {
            rejectionReason = "participant_table_unavailable";
            return false;
        }
        for (std::uint32_t index = 0; index < table.count; ++index) {
            const auto entry = table.owner +
                static_cast<std::uintptr_t>(index) * kParticipantRecordSize;
            const auto key = crash_guard::SafeRead<std::uint32_t>(
                reinterpret_cast<const void*>(
                    entry + kParticipantBindingKeyOffset));
            if (!key.has_value()) {
                rejectionReason = "participant_table_unreadable";
                return false;
            }
            if (*key == participantKey) {
                rejectionReason = "participant_key_already_active";
                return false;
            }
        }

        std::array<std::byte, kParticipantRecordSize> local{};
        if (!crash_guard::TryCopy(
                local.data(),
                reinterpret_cast<const void*>(
                    table.owner +
                    static_cast<std::uintptr_t>(table.localIndex) *
                        kParticipantRecordSize),
                local.size()) ||
            !crash_guard::TryCopy(
                previousSlot_.data(),
                reinterpret_cast<const void*>(
                    table.owner +
                    static_cast<std::uintptr_t>(table.count) *
                        kParticipantRecordSize),
                previousSlot_.size())) {
            rejectionReason = "participant_record_unreadable";
            return false;
        }

        std::array<std::byte, kParticipantRecordSize> remote{};
        if (!GameCommandExecutor::PrepareRemoteParticipantRecord(
                local,
                remote,
                participantKey,
                displayName,
                identity)) {
            rejectionReason = "participant_record_invalid";
            return false;
        }

        owner_ = table.owner;
        index_ = table.count;
        countBefore_ = table.count;
        const auto slot = reinterpret_cast<void*>(
            owner_ + static_cast<std::uintptr_t>(index_) *
                kParticipantRecordSize);
        if (!crash_guard::TryWrite(slot, remote.data(), remote.size())) {
            rejectionReason = "participant_record_write_failed";
            Clear();
            return false;
        }
        const auto countAfter = countBefore_ + 1U;
        if (!crash_guard::TryWrite(
                reinterpret_cast<void*>(
                    owner_ + kParticipantCountOffset),
                &countAfter,
                sizeof(countAfter))) {
            crash_guard::TryWrite(
                slot,
                previousSlot_.data(),
                previousSlot_.size());
            rejectionReason = "participant_count_write_failed";
            Clear();
            return false;
        }
        active_ = true;
        if (!VerifyPrepared(participantKey)) {
            rejectionReason = "participant_append_verification_failed";
            Rollback();
            return false;
        }
        return true;
    }

    bool VerifyBound(const std::uint32_t participantKey) const noexcept {
        if (!active_) {
            return false;
        }
        const auto count = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(owner_ + kParticipantCountOffset));
        const auto entry = owner_ +
            static_cast<std::uintptr_t>(index_) * kParticipantRecordSize;
        const auto flags = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(entry + kParticipantFlagsOffset));
        const auto object = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(entry + kParticipantObjectOffset));
        const auto value = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(entry + kParticipantValueOffset));
        const auto key = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(entry + kParticipantBindingKeyOffset));
        return count.has_value() && *count == countBefore_ + 1U &&
            flags.has_value() && (*flags & kParticipantBoundBit) != 0U &&
            object.has_value() && *object != 0U &&
            value.has_value() && *value != 0U &&
            key.has_value() && *key == participantKey;
    }

    bool Rollback() noexcept {
        if (!active_ || owner_ == 0U) {
            Clear();
            return true;
        }
        const auto countRestored = crash_guard::TryWrite(
            reinterpret_cast<void*>(owner_ + kParticipantCountOffset),
            &countBefore_,
            sizeof(countBefore_));
        const auto slotRestored = crash_guard::TryWrite(
            reinterpret_cast<void*>(
                owner_ + static_cast<std::uintptr_t>(index_) *
                    kParticipantRecordSize),
            previousSlot_.data(),
            previousSlot_.size());
        Clear();
        return countRestored && slotRestored;
    }

    void Commit() noexcept {
        Clear();
    }

    [[nodiscard]] std::uint32_t Index() const noexcept {
        return index_;
    }

private:
    bool VerifyPrepared(const std::uint32_t participantKey) const noexcept {
        const auto count = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(owner_ + kParticipantCountOffset));
        const auto entry = owner_ +
            static_cast<std::uintptr_t>(index_) * kParticipantRecordSize;
        const auto flags = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(entry + kParticipantFlagsOffset));
        const auto object = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(entry + kParticipantObjectOffset));
        const auto key = crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(entry + kParticipantBindingKeyOffset));
        return count.has_value() && *count == countBefore_ + 1U &&
            flags.has_value() && *flags == kRemoteUnboundFlags &&
            object.has_value() && *object == 0U &&
            key.has_value() && *key == participantKey;
    }

    void Clear() noexcept {
        owner_ = 0U;
        index_ = 0U;
        countBefore_ = 0U;
        active_ = false;
        previousSlot_.fill(std::byte{0});
    }

    std::uintptr_t owner_{};
    std::uint32_t index_{};
    std::uint32_t countBefore_{};
    std::array<std::byte, kParticipantRecordSize> previousSlot_{};
    bool active_{};
};

bool IsValidCharacterObject(const std::uintptr_t address) noexcept {
    if (address < 0x10000U || (address & 0x0FU) != 0U) {
        return false;
    }

    const auto vtable = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(address));
    const auto owner = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(
            address + kCharacterControllerOwnerOffset));
    if (!vtable.has_value() || *vtable == 0U ||
        !owner.has_value() || *owner != address) {
        return false;
    }

    std::array<char, kCharacterNamePrefix.size()> name{};
    if (!crash_guard::TryCopy(
            name.data(),
            reinterpret_cast<const void*>(address + 0x10U),
            name.size()) ||
        !std::equal(
            kCharacterNamePrefix.begin(),
            kCharacterNamePrefix.end(),
            name.begin())) {
        return false;
    }

    std::array<float, 4> position{};
    if (!crash_guard::TryCopy(
            position.data(),
            reinterpret_cast<const void*>(
                address + kCharacterPositionOffset),
            sizeof(position))) {
        return false;
    }
    return std::all_of(
        position.begin(),
        position.begin() + 3,
        [](const float value) {
            return std::isfinite(value) &&
                std::abs(value) <= kMaximumCoordinateMagnitude;
        });
}

const char* IconName(const std::string_view icon) noexcept {
    if (icon == "Alert") {
        return "Alert";
    }
    if (icon == "Audiolog") {
        return "Audiolog";
    }
    if (icon == "None") {
        return "";
    }
    return nullptr;
}

template <typename T>
T ReadValue(
    const std::array<std::byte, 0x130>& payload,
    const std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    return value;
}

template <typename T>
void WriteValue(
    std::array<std::byte, 0x130>& payload,
    const std::size_t offset,
    const T value) noexcept {
    std::memcpy(payload.data() + offset, &value, sizeof(value));
}

bool IsCivilianTemplate(
    const std::array<std::byte, 0x130>& payload) noexcept {
    const auto* name = reinterpret_cast<const char*>(
        payload.data() + kCharacterClassOffset);
    return std::equal(
        kCivilianClass.begin(),
        kCivilianClass.end(),
        name) &&
        name[kCivilianClass.size()] == '\0';
}

std::uint32_t MixEntityKey(
    const std::uint32_t spawnKey,
    const std::uint32_t nonce) noexcept {
    auto value = spawnKey ^ (nonce * 0x9E3779B9U) ^ 0xA17C4A52U;
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    value *= 0x846CA68BU;
    value ^= value >> 16U;
    value |= 0x80000000U;
    return value == 0U ? 0xA17C4A52U : value;
}

bool InvokePopup(
    const void* functionAddress,
    void* popupManager,
    const char* header,
    const char* body,
    const float duration,
    const std::uint32_t* metadata,
    const std::uint32_t soundEvent,
    const char* icon) noexcept {
#if defined(_MSC_VER)
    __try {
        const auto function = reinterpret_cast<PopupMessageFn>(
            const_cast<void*>(functionAddress));
        function(
            popupManager,
            header,
            body,
            duration,
            metadata,
            soundEvent,
            icon);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    const auto function = reinterpret_cast<PopupMessageFn>(
        const_cast<void*>(functionAddress));
    function(
        popupManager,
        header,
        body,
        duration,
        metadata,
        soundEvent,
        icon);
    return true;
#endif
}

// 00793880: ECX = gameplay manager G, one byte argument, RET 4. Retail's
// confirmed "back to frontend" UI callback calls it with 1 on the engine thread.
using ReturnToFrontendFn = void(__thiscall*)(void* gameplayManager, std::uint32_t onlyIfFrontendAbsent);

bool InvokeReturnToFrontend(const void* functionAddress, void* gameplayManager) noexcept {
#if defined(_MSC_VER)
    __try {
        reinterpret_cast<ReturnToFrontendFn>(const_cast<void*>(functionAddress))(gameplayManager, 1U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    reinterpret_cast<ReturnToFrontendFn>(const_cast<void*>(functionAddress))(gameplayManager, 1U);
    return true;
#endif
}

bool InvokeSpawnRequest130(
    const void* functionAddress,
    void* sink,
    const std::array<std::byte, 0x130>& payload,
    std::uint32_t& callbackResult) noexcept {
#if defined(_MSC_VER)
    __try {
        const auto function = reinterpret_cast<SpawnRequest130ReceiverFn>(
            const_cast<void*>(functionAddress));
        callbackResult = function(sink, payload.data());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        callbackResult = 0;
        return false;
    }
#else
    const auto function = reinterpret_cast<SpawnRequest130ReceiverFn>(
        const_cast<void*>(functionAddress));
    callbackResult = function(sink, payload.data());
    return true;
#endif
}

void* FindNamedElement(
    const void* functionAddress,
    void* registry,
    const char* name) noexcept {
#if defined(_MSC_VER)
    __try {
        const auto function = reinterpret_cast<FindNamedElementFn>(
            const_cast<void*>(functionAddress));
        return function(registry, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    const auto function = reinterpret_cast<FindNamedElementFn>(
        const_cast<void*>(functionAddress));
    return function(registry, name);
#endif
}


bool TryNextNativeSpawnKey(
    void* counterAddress,
    std::uint32_t& spawnKey) noexcept {
    if (counterAddress == nullptr) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        const auto previous = InterlockedExchangeAdd(
            static_cast<volatile LONG*>(counterAddress),
            1);
        spawnKey = static_cast<std::uint32_t>(previous);
        return spawnKey != 0U &&
            spawnKey != (std::numeric_limits<std::uint32_t>::max)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spawnKey = 0;
        return false;
    }
#else
    const auto previous = InterlockedExchangeAdd(
        static_cast<volatile LONG*>(counterAddress),
        1);
    spawnKey = static_cast<std::uint32_t>(previous);
    return spawnKey != 0U &&
        spawnKey != (std::numeric_limits<std::uint32_t>::max)();
#endif
}

} // namespace

GameCommandExecutor::GameCommandExecutor(
    const SignatureResolver& resolver,
    CaptureCatalog& captures,
    LevelGenerationTracker& generations,
    const GameThreadRuntimeEvidence& runtimeEvidence,
    SpawnCharacterTracker& spawnTracker,
    Diagnostics& diagnostics,
    const bool exactExecutableHash) noexcept
    : uiRegistryRootAddress_(resolver.Resolve(SymbolId::UiRegistryRoot)),
      findNamedElementAddress_(resolver.Resolve(SymbolId::UiFindNamedElement)),
      popupMessageAddress_(resolver.Resolve(SymbolId::PopupMessageEnqueue)),
      spawnRequest130ReceiverAddress_(
          resolver.Resolve(SymbolId::ReceiveSpawnRequest130)),
      characterRuntimeIdCounterAddress_(
          resolver.ResolveMutable(SymbolId::CharacterRuntimeIdCounter)),
      globalStateRootAddress_(resolver.Resolve(SymbolId::GlobalStateRoot)),
      returnToFrontendAddress_(resolver.Resolve(SymbolId::GameplayReturnToFrontend)),
      captures_(&captures),
      generations_(&generations),
      runtimeEvidence_(&runtimeEvidence),
      spawnTracker_(&spawnTracker),
      diagnostics_(&diagnostics),
      exactExecutableHash_(exactExecutableHash) {}

bool GameCommandExecutor::PopupAvailable() const noexcept {
    return uiRegistryRootAddress_ != nullptr &&
        findNamedElementAddress_ != nullptr &&
        popupMessageAddress_ != nullptr;
}

bool GameCommandExecutor::SpawnAvailable() const noexcept {
    return spawnRequest130ReceiverAddress_ != nullptr &&
        characterRuntimeIdCounterAddress_ != nullptr &&
        captures_ != nullptr &&
        generations_ != nullptr &&
        runtimeEvidence_ != nullptr &&
        spawnTracker_ != nullptr;
}

bool GameCommandExecutor::NetZSpawnAvailable() const noexcept {
    return SpawnAvailable() && globalStateRootAddress_ != nullptr;
}

bool GameCommandExecutor::CharacterDirectMoveAvailable() const noexcept {
    return character_direct_step_runtime::Installed();
}

bool GameCommandExecutor::CharacterDirectStepAvailable() const noexcept {
    return character_direct_step_runtime::Installed();
}

bool GameCommandExecutor::CharacterActionTestAvailable() const noexcept {
    return exactExecutableHash_ && character_action_runtime::Installed();
}

bool GameCommandExecutor::RemoteEquipmentProvisionAvailable() const noexcept {
    return spawnTracker_ != nullptr;
}

bool GameCommandExecutor::ReturnToFrontendAvailable() const noexcept {
    return exactExecutableHash_ && globalStateRootAddress_ != nullptr &&
        returnToFrontendAddress_ != nullptr;
}


bool GameCommandExecutor::IsAvailable() const noexcept {
    return PopupAvailable() || SpawnAvailable() ||
        CharacterDirectMoveAvailable() || CharacterDirectStepAvailable() ||
        CharacterActionTestAvailable() || RemoteEquipmentProvisionAvailable() ||
        worldRegistry_ != nullptr;
}

bool GameCommandExecutor::CanExecute(const std::string_view command) const noexcept {
    if (command == "PopupMessage") {
        return PopupAvailable();
    }
    if (command == "SpawnCharacter") {
        return SpawnAvailable();
    }
    if (command == "SpawnNetZCharacter") {
        return NetZSpawnAvailable();
    }
    if (command == "CHARACTER_DIRECT_MOVE") {
        return CharacterDirectMoveAvailable();
    }
    if (command == "CHARACTER_DIRECT_STEP") {
        return CharacterDirectStepAvailable();
    }
    if (command == "CHARACTER_ACTION_TEST" || command == "CHARACTER_AMMO_REFILL") {
        return CharacterActionTestAvailable();
    }
    if (command == "REMOTE_EQUIPMENT_PROVISION") {
        return RemoteEquipmentProvisionAvailable();
    }
    if (command == "ALIEN_PVP_PROBE" || command == "ALIEN_PVP_BIND" ||
        command == "ALIEN_PVP_RELEASE" || command=="ALIEN_PVP_VENTS") {
        return ai_sync::Installed();
    }
    if (command == "RETURN_TO_FRONTEND") {
        return ReturnToFrontendAvailable();
    }
    if (command == "ITEM_SYNC_APPLY" || command == "ITEM_SYNC_SPAWN") {
        return item_sync::Installed();
    }
    if (command == "MISSION_SYNC_CONFIG" || command == "MISSION_SYNC_APPLY") return worldRegistry_ != nullptr;
    if (command == "CINEMATIC_SYNC_STATUS" || command == "CINEMATIC_SYNC_SKIP")
        return alien_pvp_input::Installed();
    if (command == "WORLD_REGISTRY_PROBE" ||
        command == "WORLD_MANIFEST_LOAD" ||
        command == "WORLD_NET_PROBE") {
        return worldRegistry_ != nullptr;
    }
    if (command == "WORLD_INTERACTION_STATS" ||
        command == "WORLD_COMPOSITE_INPUT_STATS" ||
        command == "WORLD_STATE_DISCOVERY_BASELINE" ||
        command == "WORLD_STATE_DISCOVERY_AFTER" ||
        command == "WORLD_STATE_DISCOVERY_STATUS") {
        // Diagnostic must remain callable even when the generic CATHODE bus
        // failed to arm; its flags distinguish installation failure from a
        // quiet world that simply emitted no replicated mutations yet.
        return worldRegistry_ != nullptr;
    }
    return false;
}

void GameCommandExecutor::AttachWorldEntityRegistry(
    WorldEntityRegistry& registry) noexcept {
    worldRegistry_ = &registry;
}

void GameCommandExecutor::AttachGameThreadDispatcher(
    GameThreadDispatcher& dispatcher) noexcept {
    gameThreadDispatcher_ = &dispatcher;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteWorldRegistryProbe(
    const WorldRegistryProbeCommand& command) const {
    GameCommandExecutionResult result{};
    if (worldRegistry_ == nullptr || generations_ == nullptr) {
        result.statusCode = "WORLD_REGISTRY_UNAVAILABLE";
        result.message = "WorldEntityRegistry is not initialized.";
        return result;
    }
    std::string error;
    if (!worldRegistry_->LoadManifestRuntimeIndex(
            std::filesystem::path(command.manifestPath),
            static_cast<std::uint32_t>(generations_->CurrentGeneration()),
            error)) {
        result.statusCode = error == "WORLD_MANIFEST_PREPARING"
            ? "WORLD_MANIFEST_PREPARING" : "WORLD_MANIFEST_REJECTED";
        result.message = error;
        return result;
    }

    constexpr std::uint32_t kProbeBudget = 128U;
    const auto startedAt = GetTickCount64();
    const auto summary = worldRegistry_->ProbeWorldEnvironment(kProbeBudget);
    const auto diagnostics = worldRegistry_->DiagnosticsSnapshot();
    const auto elapsedMs = GetTickCount64() - startedAt;

    const auto nativeInconsistent =
        summary.nativeFaults != 0U || summary.controlNativeFaults != 0U;
    const auto controlPass =
        summary.controlResolverChecks == 0U ||
        summary.controlResolverResolved != 0U;
    result.executed =
        summary.probed != 0U && summary.resolved != 0U &&
        controlPass && !nativeInconsistent;

    if (nativeInconsistent) {
        result.statusCode = "WORLD_REGISTRY_PROBE_NATIVE_FAULT";
    } else if (summary.probed == 0U) {
        result.statusCode = "WORLD_REGISTRY_PROBE_NO_ACTIVE_CANDIDATES";
    } else if (summary.resolved == 0U) {
        result.statusCode = "WORLD_REGISTRY_PROBE_NO_RUNTIME_MATCHES";
    } else if (!controlPass) {
        result.statusCode = "WORLD_REGISTRY_PROBE_CONTROL_UNRESOLVED";
    } else {
        result.statusCode = "WORLD_REGISTRY_PROBE_COMPLETE";
    }

    result.message =
        "eligible=" + std::to_string(summary.eligible) +
        " activeEligible=" + std::to_string(summary.activeEligible) +
        " probed=" + std::to_string(summary.probed) +
        " resolved=" + std::to_string(summary.resolved) +
        " pending=" + std::to_string(summary.pending) +
        " nativeFaults=" + std::to_string(summary.nativeFaults) +
        " activeProviders=" + std::to_string(summary.activeProviders) +
        " manifestProviderContexts=" +
            std::to_string(summary.manifestActiveProviderContexts) +
        " runtimeRoots=" + std::to_string(summary.runtimeRootEntries) +
        " runtimeLoaded=" + std::to_string(summary.runtimeLoadedEntries) +
        " runtimeUniqueKeys=" + std::to_string(summary.runtimeUniqueKeys) +
        " managerManifestExact=" + std::to_string(summary.runtimeManifestExact) +
        " controlChecks=" + std::to_string(summary.controlResolverChecks) +
        " controlResolved=" + std::to_string(summary.controlResolverResolved) +
        " controlPending=" + std::to_string(summary.controlResolverPending) +
        " controlNativeFaults=" + std::to_string(summary.controlNativeFaults) +
        " knownLoadedZones=" + std::to_string(diagnostics.knownLoadedZones) +
        " knownUnloadedZones=" + std::to_string(diagnostics.knownUnloadedZones) +
        " elapsedMs=" + std::to_string(elapsedMs);
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecutePopupMessage(
    const PopupMessageCommand& command) const {
    GameCommandExecutionResult result{};
    result.statusCode = "POPUP_COMMAND_BLOCKED";
    result.message = "La commande PopupMessage n'est pas disponible.";
    if (!PopupAvailable()) {
        return result;
    }
    if (!IsValidText(command.headerText, false) ||
        !IsValidText(command.mainText, true) ||
        (command.headerText.empty() && command.mainText.empty()) ||
        !std::isfinite(command.durationSeconds) ||
        command.durationSeconds < 0.5F ||
        command.durationSeconds > 120.0F ||
        command.soundEvent != 1U) {
        result.statusCode = "POPUP_INVALID_ARGUMENTS";
        result.message = "Les paramètres PopupMessage sont invalides.";
        return result;
    }
    const auto* icon = IconName(command.icon);
    if (icon == nullptr) {
        result.statusCode = "POPUP_INVALID_ICON";
        result.message = "L'icône PopupMessage n'est pas reconnue.";
        return result;
    }

    const auto registry = crash_guard::SafeRead<void*>(uiRegistryRootAddress_);
    if (!registry.has_value() || *registry == nullptr) {
        result.statusCode = "UI_REGISTRY_UNAVAILABLE";
        result.message = "Le registre UI du niveau n'est pas disponible.";
        return result;
    }
    auto* popupManager = FindNamedElement(
        findNamedElementAddress_,
        *registry,
        "popup_message");
    if (popupManager == nullptr) {
        result.statusCode = "POPUP_MANAGER_UNAVAILABLE";
        result.message = "Le gestionnaire UI popup_message est introuvable.";
        return result;
    }

    constexpr std::size_t kQueueCountOffset = 0x1DCU;
    const auto before = crash_guard::SafeRead<std::uint32_t>(
        static_cast<const std::byte*>(popupManager) + kQueueCountOffset);
    if (!before.has_value()) {
        result.statusCode = "POPUP_QUEUE_UNREADABLE";
        result.message = "La file PopupMessage est illisible.";
        return result;
    }
    if (*before >= 16U) {
        result.statusCode = "POPUP_QUEUE_FULL";
        result.message = "La file native contient déjà 16 messages.";
        return result;
    }

    const std::array<std::uint32_t, 2> metadata{};
    if (!InvokePopup(
            popupMessageAddress_,
            popupManager,
            command.headerText.c_str(),
            command.mainText.c_str(),
            command.durationSeconds,
            metadata.data(),
            command.soundEvent,
            icon)) {
        result.statusCode = "POPUP_NATIVE_FAULT";
        result.message = "L'appel natif PopupMessage a déclenché une exception protégée.";
        return result;
    }

    const auto after = crash_guard::SafeRead<std::uint32_t>(
        static_cast<const std::byte*>(popupManager) + kQueueCountOffset);
    if (!after.has_value() || *after != *before + 1U) {
        result.statusCode = "POPUP_NOT_QUEUED";
        result.message = "L'appel natif n'a pas ajouté le message à la file UI.";
        return result;
    }

    result.executed = true;
    result.statusCode = "POPUP_QUEUED";
    result.message = "PopupMessage ajouté à la file UI du jeu.";
    return result;
}


bool GameCommandExecutor::PrepareBootstrapCivilianPayload(
    std::array<std::byte, 0x130>& payload) noexcept {
    payload.fill(std::byte{0});

    // Canonical CIVILIAN/E_RIPLEY request fields captured from the exact
    // supported retail profile. All transient stack-backed string tails and
    // reserved bytes remain zero. Runtime identity, position and selection are
    // injected later by PrepareDynamicSpawnPayload.
    WriteValue(payload, 0x10U, 0x02000100U);
    WriteValue(payload, 0x14U, 0x21F92DEAU);
    WriteValue(payload, 0x18U, 0xF864FC82U);
    WriteValue(payload, 0x20U, 0x21F92DEAU);
    WriteValue(payload, 0x24U, 0x21F92DEAU);
    WriteValue(payload, 0x2CU, 0x5FDA4212U);
    WriteValue(payload, 0x30U, 3U);
    WriteValue(payload, 0x34U, 2U);
    WriteValue(payload, 0x3CU, 0xA21D4E0BU);
    WriteValue(payload, 0x40U, 0x5E5739EAU);
    WriteValue(payload, 0x44U, 0xA6CB83CFU);
    WriteValue(payload, 0x4CU, 0x33000000U);

    constexpr char kCivilian[] = "CIVILIAN";
    std::memcpy(
        payload.data() + kCharacterClassOffset,
        kCivilian,
        sizeof(kCivilian));

    WriteValue(payload, kExtendedModeOffset, std::uint8_t{0});
    WriteValue(payload, kExtendedParameterOffset, 0xD9A16D5BU);

    return IsCivilianTemplate(payload) &&
        ReadValue<std::uint32_t>(payload, 0x2CU) == 0x5FDA4212U &&
        ReadValue<std::uint32_t>(payload, kExtendedParameterOffset) ==
            0xD9A16D5BU;
}

bool GameCommandExecutor::PrepareDynamicSpawnPayload(
    std::array<std::byte, 0x130>& payload,
    const std::uint32_t spawnKey,
    const std::uint32_t entityKey,
    const std::uint32_t selection,
    const SpawnCharacterCommand& command) noexcept {
    if (spawnKey == 0U || entityKey == 0U || selection == 0U ||
        !IsValidCoordinate(command.x) ||
        !IsValidCoordinate(command.y) ||
        !IsValidCoordinate(command.z) ||
        !IsCivilianTemplate(payload)) {
        return false;
    }

    WriteValue(payload, kSpawnKeyOffset, spawnKey);
    WriteValue(payload, kEntityKeyOffset, entityKey);
    WriteValue(payload, kSelectionOffset, selection);
    WriteValue(payload, kPositionOffset + 0U, command.x);
    WriteValue(payload, kPositionOffset + 4U, command.y);
    WriteValue(payload, kPositionOffset + 8U, command.z);
    WriteValue(payload, kExtendedSpawnKeyOffset, spawnKey);
    // Crew identity: anim_set, reference_skeleton and display_model name hashes,
    // plus the DisplayModel composite in the extended parameter (mode 0). A
    // captured template may belong to another player's representation.
    if (!player_characters::IsRepresentation(command.characterHash)) return false;
    WriteValue(payload, kAnimSetOffset, command.characterHash);
    WriteValue(payload, kReferenceSkeletonOffset, command.characterHash);
    WriteValue(payload, kDisplayModelOffset, command.characterHash);
    if (ReadValue<std::uint8_t>(payload, kExtendedModeOffset) == 0U)
        WriteValue(payload, kExtendedParameterOffset, command.displayModelGuid);
    return ReadValue<std::uint32_t>(payload, kSpawnKeyOffset) == spawnKey &&
        ReadValue<std::uint32_t>(payload, kExtendedSpawnKeyOffset) == spawnKey;
}

bool GameCommandExecutor::PrepareNetZSpawnPayload(
    std::array<std::byte, 0x130>& payload,
    const std::uint32_t participantKey) noexcept {
    if (participantKey == 0U ||
        participantKey >
            static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
        !IsCivilianTemplate(payload)) {
        return false;
    }
    WriteValue(payload, kParticipantLookupKeyOffset, participantKey);
    WriteValue(
        payload,
        kParticipantLookupEnabledOffset,
        std::uint8_t{1});
    return ReadValue<std::uint32_t>(
               payload,
               kParticipantLookupKeyOffset) == participantKey &&
        ReadValue<std::uint8_t>(
            payload,
            kParticipantLookupEnabledOffset) == 1U;
}

bool GameCommandExecutor::PrepareRemoteParticipantRecord(
    const std::array<std::byte, 0x158>& localRecord,
    std::array<std::byte, 0x158>& remoteRecord,
    const std::uint32_t participantKey,
    const std::string_view displayName,
    const std::array<std::byte, 16>& identity) noexcept {
    std::uint32_t localVtable = 0;
    std::memcpy(
        &localVtable,
        localRecord.data() + kParticipantVtableOffset,
        sizeof(localVtable));
    if (participantKey == 0U ||
        participantKey >
            static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
        displayName.empty() ||
        displayName.size() >= kParticipantNameCapacity ||
        !IsValidText(displayName, false) ||
        IsZeroIdentity(identity) ||
        localVtable == 0U) {
        remoteRecord.fill(std::byte{0});
        return false;
    }

    remoteRecord = localRecord;
    std::fill_n(
        remoteRecord.begin() + kParticipantNameOffset,
        kParticipantNameCapacity,
        std::byte{0});
    std::memcpy(
        remoteRecord.data() + kParticipantNameOffset,
        displayName.data(),
        displayName.size());
    std::memcpy(
        remoteRecord.data() + kParticipantIdentityOffset,
        identity.data(),
        identity.size());

    const auto write = [&](const std::size_t offset, const auto value) {
        std::memcpy(
            remoteRecord.data() + offset,
            &value,
            sizeof(value));
    };
    write(kParticipantFlagsOffset, kRemoteUnboundFlags);
    write(kParticipantObjectOffset, std::uint32_t{0});
    write(kParticipantValueOffset, std::uint32_t{0});
    write(kParticipantBindingKeyOffset, participantKey);
    write(kParticipantTrailingIndexOffset, 0xFFFFFFFFU);

    std::uint32_t storedFlags = 0;
    std::uint32_t storedObject = 0;
    std::uint32_t storedKey = 0;
    std::memcpy(
        &storedFlags,
        remoteRecord.data() + kParticipantFlagsOffset,
        sizeof(storedFlags));
    std::memcpy(
        &storedObject,
        remoteRecord.data() + kParticipantObjectOffset,
        sizeof(storedObject));
    std::memcpy(
        &storedKey,
        remoteRecord.data() + kParticipantBindingKeyOffset,
        sizeof(storedKey));
    return storedFlags == kRemoteUnboundFlags &&
        storedObject == 0U &&
        storedKey == participantKey;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteWorldManifestLoad(
    const WorldManifestLoadCommand& command) const {
    GameCommandExecutionResult result{};
    if (worldRegistry_ == nullptr || generations_ == nullptr) {
        result.statusCode = "WORLD_REGISTRY_UNAVAILABLE";
        result.message = "WorldEntityRegistry is not initialized.";
        return result;
    }
    std::string error;
    const auto loadStarted = GetTickCount64();
    if (!worldRegistry_->LoadManifestRuntimeIndex(
            std::filesystem::path(command.manifestPath),
            static_cast<std::uint32_t>(generations_->CurrentGeneration()),
            error)) {
        result.statusCode = error == "WORLD_MANIFEST_PREPARING"
            ? "WORLD_MANIFEST_PREPARING" : "WORLD_MANIFEST_REJECTED";
        result.message = error;
        return result;
    }
    const auto indexLoaded = GetTickCount64();
    const auto diagnostics = worldRegistry_->DiagnosticsSnapshot();
    result.executed = diagnostics.manifestLoaded;
    std::string captureError;
    const bool captureArmed = result.executed &&
        world_interactions::ArmCapture(captureError);
    const auto captureFinished = GetTickCount64();
    if (result.executed && !captureArmed) {
        result.executed = false;
        result.statusCode = "WORLD_CAPTURE_ARM_FAILED";
        result.message = captureError;
        return result;
    }
    // Patch 5.3.3 invariant: Door started_opening/started_closing remains dormant.
    // Patch 5.6 passively observes only the certified DoorNative semantic INPUT
    // boundaries RequestLock/RequestUnlock/ForceOpen/ForceClose. Retail executes
    // unchanged and no DoorAction/ROOT is published by this probe matrix.
    const bool doorTransitionCaptureArmed = false;
    std::string doorSemanticProbeError;
    const bool doorSemanticProbeArmed = result.executed &&
        world_animated::ArmDoorSemanticInputProbe(doorSemanticProbeError);
    const auto probesFinished = GetTickCount64();
    result.statusCode = result.executed
        ? "WORLD_MANIFEST_LOADED"
        : "WORLD_MANIFEST_REJECTED";
    result.message = "entities=" + std::to_string(diagnostics.manifestEntities) +
        " generation=" + std::to_string(diagnostics.levelGeneration) +
        " mapContentId=" + WorldEntityIdText(diagnostics.mapContentId) +
        " genericCaptureArmed=" + std::to_string(captureArmed ? 1U : 0U) +
        " doorTransitionCaptureArmed=" +
            std::to_string(doorTransitionCaptureArmed ? 1U : 0U) +
        " doorTransitionCapturePolicy=disabled-regression-hotfix" +
        " doorSemanticProbeArmed=" + std::to_string(doorSemanticProbeArmed ? 1U : 0U) +
        " runtimeIndexLoadMs=" + std::to_string(indexLoaded - loadStarted) +
        " genericCaptureArmMs=" + std::to_string(captureFinished - indexLoaded) +
        " doorSemanticProbeArmMs=" + std::to_string(probesFinished - captureFinished) +
        (doorSemanticProbeError.empty() ? std::string{} :
            " doorSemanticProbeError=" + doorSemanticProbeError) +
        ". No runtime entity scan was performed.";
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteWorldNetProbe(
    const WorldNetProbeCommand& command) const {
    GameCommandExecutionResult result{};
    const WorldEntityId entity{command.entityHigh, command.entityLow};
    if (worldRegistry_ == nullptr || generations_ == nullptr || !entity.Valid()) {
        result.statusCode = "WORLD_NET_PROBE_INVALID";
        result.message = "WorldEntityId or registry is invalid.";
        return result;
    }
    const auto diagnostics = worldRegistry_->DiagnosticsSnapshot();
    if (!diagnostics.manifestLoaded ||
        diagnostics.levelGeneration != generations_->CurrentGeneration()) {
        result.statusCode = "WORLD_NET_CONTEXT_UNAVAILABLE";
        result.message = "Load a compatible manifest for the current LevelGeneration first.";
        return result;
    }
    if (!replication_core::SubmitLocalIntent(
            entity,
            WorldMutationKind::Event,
            kWorldNetworkProbeOperation)) {
        result.statusCode = "WORLD_NET_INTENT_QUEUE_REJECTED";
        result.message = "The bounded WorldIntent queue rejected the synthetic probe.";
        return result;
    }
    result.executed = true;
    result.statusCode = "WORLD_NET_PROBE_INTENT_QUEUED";
    result.message = "Synthetic no-op WorldIntent queued for " + WorldEntityIdText(entity) +
        ". No environment gameplay action was executed.";
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteWorldCompositeInputStats() const {
    GameCommandExecutionResult result{};
    const auto startedAt = GetTickCount64();
    const auto stats = world_interactions::CompositePublicInputDiagnosticsSnapshot();
    const auto ws3 = world_actions::Snapshot();
    const auto elapsedMs = GetTickCount64() - startedAt;

    result.executed = stats.initialized;
    result.statusCode = result.executed
        ? "WORLD_COMPOSITE_INPUT_STATS_COMPLETE"
        : "WORLD_COMPOSITE_INPUT_STATS_UNAVAILABLE";

    std::string ci27TraceText =
        " ci27TraceCount=" + std::to_string(stats.preQueueBuilderUnknownTraceCount);
    for (std::size_t i = 0; i < stats.preQueueBuilderUnknownTrace.size(); ++i) {
        const auto& trace = stats.preQueueBuilderUnknownTrace[i];
        if (!trace.valid) continue;
        const auto prefix = " ci27U" + std::to_string(i);
        ci27TraceText +=
            prefix + "Guid=" + ShortGuidText(trace.inputShortGuid) +
            prefix + "Caller=" + ShortGuidText(trace.callerRva) +
            prefix + "KeyA=" + ShortGuidText(trace.targetRuntimeKey.entityA) +
            prefix + "KeyB=" + ShortGuidText(trace.targetRuntimeKey.entityB) +
            prefix + "TargetPtr=" + ShortGuidText(
                static_cast<std::uint32_t>(trace.targetExecution)) +
            prefix + "TargetId=" + WorldEntityIdText(trace.targetId) +
            prefix + "Outer=" + ShortGuidText(trace.outerPublicInput) +
            prefix + "Owner=" + WorldEntityIdText(trace.bridgeOwner);
    }

    // Keep the live admission A/B signal at the FRONT of the bounded 1000-byte
    // command result. The full historical diagnostics continue below and in the
    // exported native_worldsync_snapshot, but these values must remain visible
    // immediately in the Launcher while testing a door in-game.
    result.message =
        std::string("ws3Adm=") + ShortGuidText(ws3.admissionSecurityMask) +
        " root=" + std::to_string(ws3.rootsSent) +
        " ctxR=" + std::to_string(ws3.contextRejected) +
        " pubInR=" + std::to_string(ws3.identityPublicInputRejected) +
        " tgtR=" + std::to_string(ws3.targetOwnerRejected) +
        " worldContR=" + std::to_string(ws3.callbackWorldContinuationRejected) +
        " admBehB=" + std::to_string(ws3.admissionForbiddenInputBypassed) +
        " admGuidR=" + std::to_string(ws3.admissionGuidPrefilterRejected) +
        " admGuidB=" + std::to_string(ws3.admissionGuidPrefilterBypassed) +
        " admPayloadB=" + std::to_string(ws3.admissionPayloadBypassed) +
        " admCbB=" + std::to_string(ws3.admissionCallbackShapeBypassed) +
        " admTailB=" + std::to_string(ws3.admissionTailBypassed) +
        " admPlayerB=" + std::to_string(ws3.admissionPlayerTargetBypassed) +
        " admAIB=" + std::to_string(ws3.admissionAiTargetBypassed) +
        " admUIB=" + std::to_string(ws3.admissionUiTargetBypassed) +
        " admAIMPB=" + std::to_string(ws3.admissionAimpTargetBypassed) +
        " admWorldB=" + std::to_string(ws3.admissionWorldContinuationBypassed) +
        " admUnknownB=" + std::to_string(ws3.admissionUnknownCausalityBypassed) +
        " admOwnerB=" + std::to_string(ws3.admissionManifestOwnerInputBypassed) +
        " admTargetB=" + std::to_string(ws3.admissionManifestTargetBypassed) +
        " admDescB=" + std::to_string(ws3.admissionDescendantBypassed) +
        " | scan=0 arm=0 fix323=0" +
        " hook=" + std::to_string(stats.schedulerHookInstalled ? 1U : 0U) +
        " ci27Hook=" + std::to_string(stats.preQueueBuilderHookInstalled ? 1U : 0U) +
        " version=" + std::to_string(stats.version) +
        " ci24Map=" + std::to_string(stats.mapped) +
        " ci25RuntimeMap=" + std::to_string(stats.runtimeEdgeMapped) +
        " ci26ScopeMap=" + std::to_string(stats.ownerScopeRuntimeEdgeMapped) +
        " ci27Seen=" + std::to_string(stats.preQueueBuilderSeen) +
        " ci27Input=" + std::to_string(stats.preQueueBuilderInputReadable) +
        " ci27Public=" + std::to_string(stats.preQueueBuilderPublic) +
        " ci27Internal=" + std::to_string(stats.preQueueBuilderInternal) +
        " ci27InternalExact=" + std::to_string(stats.preQueueBuilderInternalExactMapped) +
        " ci27InternalRuntime=" + std::to_string(stats.preQueueBuilderInternalRuntimeMapped) +
        " ci27InternalScope=" + std::to_string(stats.preQueueBuilderInternalOwnerScopeMapped) +
        " ci27UnknownGuid=" + std::to_string(stats.preQueueBuilderUnknownGuid) +
        ci27TraceText +
        " ci27Mapped=" + std::to_string(stats.preQueueBuilderMapped) +
        " ci27OwnerMap=" + std::to_string(stats.preQueueBuilderOwnerMapped) +
        " ci27IfaceMap=" + std::to_string(stats.preQueueBuilderInterfaceMapped) +
        " ci27Send=" + std::to_string(stats.preQueueBuilderSubmitted) +
        " ci27RemoteSkip=" + std::to_string(stats.preQueueBuilderRemoteSkipped) +
        " ci27DelayReject=" + std::to_string(stats.preQueueBuilderDelayRejected) +
        " ci27IdentityReject=" + std::to_string(stats.preQueueBuilderIdentityRejected) +
        " ci27TypedReject=" + std::to_string(stats.preQueueBuilderTypedRejected) +
        " ci27Reject=" + std::to_string(stats.preQueueBuilderRejected) +
        " ci24Send=" + std::to_string(stats.submitted) +
        " ci24Recv=" + std::to_string(stats.remoteAttempts) +
        " ci24Queue=" + std::to_string(stats.bridgeQueued) +
        " ci24SrcFallback=" + std::to_string(stats.sourceFallbacks) +
        " ci24Pending=" + std::to_string(stats.runtimeUnavailable) +
        " ci24Reject=" + std::to_string(stats.rejected) +
        " ws3Hook=" + std::to_string(ws3.installed) +
        " ws3Enabled=" + std::to_string(ws3.enabled) +
        " ws3EmptyCallbackTest=" + std::to_string(ws3.emptyCallbackTest) +
        " ws3AdmissionMask=" + ShortGuidText(ws3.admissionSecurityMask) +
        " ws3Seen=" + std::to_string(ws3.seen) +
        " ws3Public=" + std::to_string(ws3.publicCandidates) +
        " ws3RootSend=" + std::to_string(ws3.rootsSent) +
        " ws3GenericRoot=" + std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::GenericRoot)]) +
        " ws3TargetedRoot=" + std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::TargetedRoot)]) +
        " ws3PayloadRoot=" + std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::PayloadRoot)]) +
        " ws3NeedsAdapter=" + std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::NeedsAdapter)]) +
        " ws3LocalOnly=" + std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::LocalOnly)]) +
        " ws3Unsafe=" + std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::Unsafe)]) +
        " ws3Unknown=" + std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::Unknown)]) +
        " ws3TargetOwnerDistinct=" + std::to_string(ws3.targetOwnerDistinct) +
        " ws3ExternalRootSend=" + std::to_string(ws3.externalRoots) +
        " ws3DelayedRootSend=" + std::to_string(ws3.delayedRoots) +
        " ws3DescSkip=" + std::to_string(ws3.descendantsSuppressed) +
        " ws3RemoteSkip=" + std::to_string(ws3.remoteSuppressed) +
        " ws3IdentityReject=" + std::to_string(ws3.identityRejected) +
        " ws3IdentityRuntimeKeyReject=" + std::to_string(ws3.identityRuntimeKeyRejected) +
        " ws3IdentityRegistryLookupReject=" + std::to_string(ws3.identityRegistryLookupRejected) +
        " ws3IdentityPublicInputReject=" + std::to_string(ws3.identityPublicInputRejected) +
        " ws3IdentityDirectOwnerValid=" + std::to_string(ws3.identityDirectOwnerValid) +
        " ws3IdentityInterfaceMapValid=" + std::to_string(ws3.identityInterfaceMapValid) +
        " ws3IdentityInterfaceMapReject=" + std::to_string(ws3.identityInterfaceMapRejected) +
        " ws3ProbeIdentityDirectOwnerValid=" + std::to_string(ws3.probeIdentityDirectOwnerValid) +
        " ws3ProbeIdentityInterfaceMapValid=" + std::to_string(ws3.probeIdentityInterfaceMapValid) +
        " ws3ProbeIdentityInterfaceMapReject=" + std::to_string(ws3.probeIdentityInterfaceMapRejected) +
        " ws3IdentityDefinitionGetterReject=" + std::to_string(ws3.identityDefinitionGetterRejected) +
        " ws3IdentityDefinitionNullReject=" + std::to_string(ws3.identityDefinitionNullRejected) +
        " ws3IdentityDefinitionDispatchReject=" + std::to_string(ws3.identityDefinitionDispatchRejected) +
        " ws3IdentityDefinitionFaultReject=" + std::to_string(ws3.identityDefinitionFaultRejected) +
        " ws3IdentityDefinitionSelfReject=" + std::to_string(ws3.identityDefinitionSelfRejected) +
        " ws3IdentityCurrentRuntimeKeyValid=" + std::to_string(ws3.identityCurrent.runtimeKeyValid) +
        " ws3IdentityCurrentRuntimeKeyReject=" + std::to_string(ws3.identityCurrent.runtimeKeyReject) +
        " ws3IdentityCurrentCandidateValid=" + std::to_string(ws3.identityCurrent.candidateValid) +
        " ws3IdentityCurrentCandidateReject=" + std::to_string(ws3.identityCurrent.candidateReject) +
        " ws3IdentityCurrentPublicInputValid=" + std::to_string(ws3.identityCurrent.publicInputValid) +
        " ws3IdentityCurrentPublicInputReject=" + std::to_string(ws3.identityCurrent.publicInputReject) +
        " ws3IdentitySelfRuntimeKeyValid=" + std::to_string(ws3.identitySelf.runtimeKeyValid) +
        " ws3IdentitySelfRuntimeKeyReject=" + std::to_string(ws3.identitySelf.runtimeKeyReject) +
        " ws3IdentitySelfCandidateValid=" + std::to_string(ws3.identitySelf.candidateValid) +
        " ws3IdentitySelfCandidateReject=" + std::to_string(ws3.identitySelf.candidateReject) +
        " ws3IdentitySelfPublicInputValid=" + std::to_string(ws3.identitySelf.publicInputValid) +
        " ws3IdentitySelfPublicInputReject=" + std::to_string(ws3.identitySelf.publicInputReject) +
        " ws3ProbeIdentityCurrentRuntimeKeyValid=" + std::to_string(ws3.probeIdentityCurrent.runtimeKeyValid) +
        " ws3ProbeIdentityCurrentRuntimeKeyReject=" + std::to_string(ws3.probeIdentityCurrent.runtimeKeyReject) +
        " ws3ProbeIdentityCurrentCandidateValid=" + std::to_string(ws3.probeIdentityCurrent.candidateValid) +
        " ws3ProbeIdentityCurrentCandidateReject=" + std::to_string(ws3.probeIdentityCurrent.candidateReject) +
        " ws3ProbeIdentityCurrentPublicInputValid=" + std::to_string(ws3.probeIdentityCurrent.publicInputValid) +
        " ws3ProbeIdentityCurrentPublicInputReject=" + std::to_string(ws3.probeIdentityCurrent.publicInputReject) +
        " ws3ProbeIdentitySelfRuntimeKeyValid=" + std::to_string(ws3.probeIdentitySelf.runtimeKeyValid) +
        " ws3ProbeIdentitySelfRuntimeKeyReject=" + std::to_string(ws3.probeIdentitySelf.runtimeKeyReject) +
        " ws3ProbeIdentitySelfCandidateValid=" + std::to_string(ws3.probeIdentitySelf.candidateValid) +
        " ws3ProbeIdentitySelfCandidateReject=" + std::to_string(ws3.probeIdentitySelf.candidateReject) +
        " ws3ProbeIdentitySelfPublicInputValid=" + std::to_string(ws3.probeIdentitySelf.publicInputValid) +
        " ws3ProbeIdentitySelfPublicInputReject=" + std::to_string(ws3.probeIdentitySelf.publicInputReject) +
        " ws3ProbeIdentityRuntimeKeyReject=" + std::to_string(ws3.probeIdentityRuntimeKeyRejected) +
        " ws3ProbeIdentityRegistryLookupReject=" + std::to_string(ws3.probeIdentityRegistryLookupRejected) +
        " ws3ProbeIdentityPublicInputReject=" + std::to_string(ws3.probeIdentityPublicInputRejected) +
        " ws3ProbeIdentityDefinitionGetterReject=" + std::to_string(ws3.probeIdentityDefinitionGetterRejected) +
        " ws3ProbeIdentityDefinitionNullReject=" + std::to_string(ws3.probeIdentityDefinitionNullRejected) +
        " ws3ProbeIdentityDefinitionDispatchReject=" + std::to_string(ws3.probeIdentityDefinitionDispatchRejected) +
        " ws3ProbeIdentityDefinitionFaultReject=" + std::to_string(ws3.probeIdentityDefinitionFaultRejected) +
        " ws3ProbeIdentityDefinitionSelfReject=" + std::to_string(ws3.probeIdentityDefinitionSelfRejected) +
        " ws3IdentityBoundaryLayoutReject=" + std::to_string(ws3.identityBoundaryLayoutRejected) +
        " ws3IdentityBoundaryOwnerValid=" + std::to_string(ws3.identityBoundaryOwnerValid) +
        " ws3IdentityOwnerRuntimeKeyValid=" + std::to_string(ws3.identityOwner.runtimeKeyValid) +
        " ws3IdentityOwnerRuntimeKeyReject=" + std::to_string(ws3.identityOwner.runtimeKeyReject) +
        " ws3IdentityOwnerCandidateValid=" + std::to_string(ws3.identityOwner.candidateValid) +
        " ws3IdentityOwnerCandidateReject=" + std::to_string(ws3.identityOwner.candidateReject) +
        " ws3IdentityOwnerPublicInputValid=" + std::to_string(ws3.identityOwner.publicInputValid) +
        " ws3IdentityOwnerPublicInputReject=" + std::to_string(ws3.identityOwner.publicInputReject) +
        " ws3ProbeIdentityBoundaryLayoutReject=" + std::to_string(ws3.probeIdentityBoundaryLayoutRejected) +
        " ws3ProbeIdentityBoundaryOwnerValid=" + std::to_string(ws3.probeIdentityBoundaryOwnerValid) +
        " ws3ProbeIdentityOwnerRuntimeKeyValid=" + std::to_string(ws3.probeIdentityOwner.runtimeKeyValid) +
        " ws3ProbeIdentityOwnerRuntimeKeyReject=" + std::to_string(ws3.probeIdentityOwner.runtimeKeyReject) +
        " ws3ProbeIdentityOwnerCandidateValid=" + std::to_string(ws3.probeIdentityOwner.candidateValid) +
        " ws3ProbeIdentityOwnerCandidateReject=" + std::to_string(ws3.probeIdentityOwner.candidateReject) +
        " ws3ProbeIdentityOwnerPublicInputValid=" + std::to_string(ws3.probeIdentityOwner.publicInputValid) +
        " ws3ProbeIdentityOwnerPublicInputReject=" + std::to_string(ws3.probeIdentityOwner.publicInputReject) +
        " ws3ProbeDirectRootSend=" + std::to_string(ws3.probeDirectRootsSent) +
        " ws3ProbeIdentityLastObserved=" + std::to_string(ws3.probeIdentityLast.observed) +
        " ws3ProbeIdentityLastBoundaryOwnerValid=" + std::to_string(ws3.probeIdentityLast.boundaryOwnerValid) +
        " ws3ProbeIdentityLastOwnerMatchesCurrent=" + std::to_string(ws3.probeIdentityLast.ownerMatchesCurrent) +
        " ws3ProbeIdentityLastCurrentKind=" + WorldEntityKindText(ws3.probeIdentityLast.currentKind) +
        " ws3ProbeIdentityLastOwnerKind=" + WorldEntityKindText(ws3.probeIdentityLast.ownerKind) +
        " ws3ProbeIdentityLastCurrentKeyA=" + ShortGuidText(ws3.probeIdentityLast.currentKey.entityA) +
        " ws3ProbeIdentityLastCurrentKeyB=" + ShortGuidText(ws3.probeIdentityLast.currentKey.entityB) +
        " ws3ProbeIdentityLastOwnerKeyA=" + ShortGuidText(ws3.probeIdentityLast.ownerKey.entityA) +
        " ws3ProbeIdentityLastOwnerKeyB=" + ShortGuidText(ws3.probeIdentityLast.ownerKey.entityB) +
        " ws3ProbeIdentityLastCurrentId=" + WorldEntityIdText(ws3.probeIdentityLast.currentId) +
        " ws3ProbeIdentityLastOwnerId=" + WorldEntityIdText(ws3.probeIdentityLast.ownerId) +
        " ws3ProbeManifestReady=" + std::to_string(ws3.probeIdentityLast.manifestReady) +
        " ws3ProbeManifestGuidKnown=" + std::to_string(ws3.probeIdentityLast.manifestGuidKnown) +
        " ws3ProbeManifestExactForwardKeyPresent=" + std::to_string(ws3.probeIdentityLast.manifestExactForwardKeyPresent) +
        " ws3ProbeManifestExactForwardAllowed=" + std::to_string(ws3.probeIdentityLast.manifestExactForwardAllowed) +
        " ws3ProbeManifestExactOwnerSeenForGuid=" + std::to_string(ws3.probeIdentityLast.manifestExactOwnerSeenForGuid) +
        " ws3ProbeManifestExactBridgeCount=" + std::to_string(ws3.probeIdentityLast.manifestExactBridgeCount) +
        " ws3ProbeManifestExactPayloadlessBridgeCount=" + std::to_string(ws3.probeIdentityLast.manifestExactPayloadlessBridgeCount) +
        " ws3ProbeManifestExactTypedBridgeCount=" + std::to_string(ws3.probeIdentityLast.manifestExactTypedBridgeCount) +
        " ws3ProbeManifestGuidBridgeCount=" + std::to_string(ws3.probeIdentityLast.manifestGuidBridgeCount) +
        " ws3ProbeManifestGuidOwnerCount=" + std::to_string(ws3.probeIdentityLast.manifestGuidOwnerCount) +
        " ws3ProbeManifestFirstGuidOwner=" + WorldEntityIdText(ws3.probeIdentityLast.manifestFirstGuidOwner) +
        " ws3ProbeManifestFirstGuidDataType=" + std::to_string(ws3.probeIdentityLast.manifestFirstGuidDataType) +
        " ws3ProbeManifestFirstGuidNoTypedData=" + std::to_string(ws3.probeIdentityLast.manifestFirstGuidNoTypedData) +
        " ws3ContextUnsupported=" + std::to_string(ws3.contextUnsupported) +
        " ws3ContextReconstructed=" + std::to_string(ws3.contextReconstructed) +
        " ws3ContextPending=" + std::to_string(ws3.contextPending) +
        " ws3ContextChainsCaptured=" + std::to_string(ws3.contextChainsCaptured) +
        " ws3ContextFramesCaptured=" + std::to_string(ws3.contextFramesCaptured) +
        " ws3ContextChainDepthPeak=" + std::to_string(ws3.contextChainDepthPeak) +
        " ws3ContextChainOverflow=" + std::to_string(ws3.contextChainOverflow) +
        " ws3ContextFramesReconstructed=" + std::to_string(ws3.contextFramesReconstructed) +
        " ws3ActorUnresolved=" + std::to_string(ws3.actorUnresolved) +
        " ws3CauseTableOverflow=" + std::to_string(ws3.causeTableOverflow) +
        " ws3ContextReject=" + std::to_string(ws3.contextRejected) +
        " ws3ArgReject=" + std::to_string(ws3.argRejected) +
        " ws3PayloadReject=" + std::to_string(ws3.payloadRejected) +
        " ws3CallbackReject=" + std::to_string(ws3.callbackRejected) +
        " ws3DelayReject=" + std::to_string(ws3.delayRejected) +
        " ws3TailReject=" + std::to_string(ws3.tailRejected) +
        " ws3ProbeDirectSeen=" + std::to_string(ws3.probeDirectSeen) +
        " ws3ProbeDirectArgReject=" + std::to_string(ws3.probeDirectArgRejected) +
        " ws3ProbeDirectPayload=" + std::to_string(ws3.probeDirectPayload) +
        " ws3ProbeDirectCallback=" + std::to_string(ws3.probeDirectCallback) +
        " ws3ProbeDirectDelay=" + std::to_string(ws3.probeDirectDelay) +
        " ws3ProbeDirectTail=" + std::to_string(ws3.probeDirectTail) +
        " ws3ProbeDirectEligible=" + std::to_string(ws3.probeDirectEligible) +
        " ws3TargetOwnerReject=" + std::to_string(ws3.targetOwnerRejected) +
        " ws3TargetOwnerMatch=" + std::to_string(ws3.targetOwnerMatched) +
        " ws3ProbeTargetOwnerReject=" + std::to_string(ws3.probeTargetOwnerRejected) +
        " ws3PlayerCausalReject=" + std::to_string(ws3.playerCausalRejected) +
        " ws3CausalScanned=" + std::to_string(ws3.playerCausalNodesScanned) +
        " ws3CausalMapped=" + std::to_string(ws3.playerCausalNodesMapped) +
        " ws3CausalUnknownReject=" + std::to_string(ws3.causalUnknownRejected) +
        " ws3CausalShapeReject=" + std::to_string(ws3.causalShapeRejected) +
        " ws3CausalUnmappedNodes=" + std::to_string(ws3.causalUnmappedNodes) +
        " ws3CausalCycleReject=" + std::to_string(ws3.causalCycleRejected) +
        " ws3CausalDepthReject=" + std::to_string(ws3.causalDepthRejected) +
        " ws3QueueReject=" + std::to_string(ws3.queueRejected) +
        " ws3Overflow=" + std::to_string(ws3.lineageOverflow) +
        " ws3LineageBuildInherit=" + std::to_string(ws3.lineageBuildInherit) +
        " ws3LineageExecuteRestore=" + std::to_string(ws3.lineageExecuteRestore) +
        " ws3LineageCallbackRestore=" + std::to_string(ws3.lineageCallbackRestore) +
        " ws3Applied=" + std::to_string(ws3.applied) +
        " ws3ApplyReject=" + std::to_string(ws3.applyRejected) +
        " ws3Fault=" + std::to_string(ws3.nativeFaults) +
        " ws3Ws2Bypass=" + std::to_string(ws3.ws2Bypassed) +
        " ws3CallbackRoot=" + std::to_string(ws3.callbackRoots) +
        " ws3CallbackGameplayReject=" + std::to_string(ws3.callbackGameplayRejected) +
        " ws3CallbackBoundaryCandidate=" + std::to_string(ws3.callbackBoundaryCandidates) +
        " ws3CallbackBoundaryRoot=" + std::to_string(ws3.callbackBoundaryRoots) +
        " ws3CallbackBoundaryPlayer=" + std::to_string(ws3.callbackBoundaryPlayerRoots) +
        " ws3CallbackBoundaryAI=" + std::to_string(ws3.callbackBoundaryAiRoots) +
        " ws3CallbackBoundaryUI=" + std::to_string(ws3.callbackBoundaryUiRoots) +
        " ws3CallbackBoundaryAIMP=" + std::to_string(ws3.callbackBoundaryAimpRoots) +
        " ws3CallbackBoundaryApplied=" + std::to_string(ws3.callbackBoundaryApplied) +
        " ws3CallbackWorldContinuationReject=" + std::to_string(ws3.callbackWorldContinuationRejected) +
        " ws3CallbackBoundaryUnknownReject=" + std::to_string(ws3.callbackBoundaryUnknownRejected) +
        " ws3AdmissionForbiddenInputBypass=" + std::to_string(ws3.admissionForbiddenInputBypassed) +
        " ws3AdmissionGuidPrefilterReject=" + std::to_string(ws3.admissionGuidPrefilterRejected) +
        " ws3AdmissionGuidPrefilterBypass=" + std::to_string(ws3.admissionGuidPrefilterBypassed) +
        " ws3AdmissionPayloadBypass=" + std::to_string(ws3.admissionPayloadBypassed) +
        " ws3AdmissionCallbackShapeBypass=" + std::to_string(ws3.admissionCallbackShapeBypassed) +
        " ws3AdmissionTailBypass=" + std::to_string(ws3.admissionTailBypassed) +
        " ws3AdmissionPlayerTargetBypass=" + std::to_string(ws3.admissionPlayerTargetBypassed) +
        " ws3AdmissionAITargetBypass=" + std::to_string(ws3.admissionAiTargetBypassed) +
        " ws3AdmissionUITargetBypass=" + std::to_string(ws3.admissionUiTargetBypassed) +
        " ws3AdmissionAIMPTargetBypass=" + std::to_string(ws3.admissionAimpTargetBypassed) +
        " ws3AdmissionWorldContinuationBypass=" + std::to_string(ws3.admissionWorldContinuationBypassed) +
        " ws3AdmissionUnknownCausalityBypass=" + std::to_string(ws3.admissionUnknownCausalityBypassed) +
        " ws3AdmissionManifestOwnerInputBypass=" + std::to_string(ws3.admissionManifestOwnerInputBypassed) +
        " ws3AdmissionManifestTargetBypass=" + std::to_string(ws3.admissionManifestTargetBypassed) +
        " ws3AdmissionDescendantBypass=" + std::to_string(ws3.admissionDescendantBypassed) +
        " ws3CausalProvSeed=" + std::to_string(ws3.causalProvenanceSeeded) +
        " ws3CausalProvInherit=" + std::to_string(ws3.causalProvenanceInherited) +
        " ws3CausalProvBoundary=" + std::to_string(ws3.causalProvenanceBoundary) +
        " ws3CausalProvOverflow=" + std::to_string(ws3.causalProvenanceOverflow) +
        " ws3CausalProvPlayer=" + std::to_string(ws3.causalProvenancePlayer) +
        " ws3CausalProvAI=" + std::to_string(ws3.causalProvenanceAi) +
        " ws3CausalProvUI=" + std::to_string(ws3.causalProvenanceUi) +
        " ws3CausalProvAIMP=" + std::to_string(ws3.causalProvenanceAimp) +
        " ws3CausalChainsCreated=" + std::to_string(ws3.causalChainsCreated) +
        " ws3CausalChainsReleased=" + std::to_string(ws3.causalChainsReleased) +
        " ws3CausalGroupsLive=" + std::to_string(ws3.causalGroupsLive) +
        " ws3CausalGroupsPeak=" + std::to_string(ws3.causalGroupsLivePeak) +
        " ws3CausalRootReserved=" + std::to_string(ws3.causalRootReserved) +
        " ws3CausalRootCommitted=" + std::to_string(ws3.causalRootCommitted) +
        " ws3CompositeInputRootSend=" + std::to_string(ws3.compositeInputRootsSent) +
        " ws3EmittedOutputRootSend=" + std::to_string(ws3.emittedOutputRootsSent) +
        " ws3ScheduledInputRootSend=" + std::to_string(ws3.scheduledInputRootsSent) +
        " ws3SchedulerEdgeRootSend=" + std::to_string(ws3.schedulerEdgeRootsSent) +
        " ws3TypedMessageRootSend=" + std::to_string(ws3.typedMessageRootsSent) +
        " ws3TypedMessageRootApplied=" + std::to_string(ws3.typedMessageRootsApplied) +
        " ws3CanonicalCompositeRootSend=" + std::to_string(ws3.canonicalCompositeRootsSent) +
        " ws3CanonicalCompositeRootApplied=" + std::to_string(ws3.canonicalCompositeRootsApplied) +
        " ws3DoorLockStateRootSend=" + std::to_string(ws3.doorLockStateRootsSent) +
        " ws3DoorLockStateRootApplied=" + std::to_string(ws3.doorLockStateRootsApplied) +
        " ws3SchedulerEdgeSidecarObserved=" + std::to_string(ws3.schedulerEdgeSidecarObserved) +
        " ws3SchedulerEdgeSidecarSubmitted=" + std::to_string(ws3.schedulerEdgeSidecarSubmitted) +
        " ws3SchedulerEdgeRemoteNaturalRemembered=" + std::to_string(ws3.schedulerEdgeRemoteNaturalRemembered) +
        " ws3SchedulerEdgeRemoteDedupConsumed=" + std::to_string(ws3.schedulerEdgeRemoteDedupConsumed) +
        " ws3ScheduledEventRootSuppressed=" + std::to_string(ws3.scheduledEventRootsSuppressed) +
        " ws3CarrierObserved=" + std::to_string(ws3.carrierObserved) +
        " ws3CarrierNetworkEligible=" + std::to_string(ws3.carrierNetworkEligible) +
        " ws3CarrierForwardingComplete=" + std::to_string(ws3.carrierForwardingComplete) +
        " ws3CarrierSelectorResolved=" + std::to_string(ws3.carrierSelectorResolved) +
        " ws3CarrierSelectorRejected=" + std::to_string(ws3.carrierSelectorRejected) +
        " ws3CarrierContextCaptured=" + std::to_string(ws3.carrierContextCaptured) +
        " ws3CarrierManifestAdmitted=" + std::to_string(ws3.carrierManifestAdmitted) +
        " ws3CarrierPreBusPreserved=" + std::to_string(ws3.carrierPreBusPreserved) +
        " ws3CarrierSubmitAttempt=" + std::to_string(ws3.carrierSubmitAttempt) +
        " ws3CarrierSubmitSuccess=" + std::to_string(ws3.carrierSubmitSuccess) +
        " ws3ScheduledCoveredByCarrier=" + std::to_string(ws3.scheduledCoveredByCarrier) +
        " ws3ScheduledCoveredByCarrierSuppressed=" +
            std::to_string(ws3.scheduledCoveredByCarrierSuppressed) +
        " ws3CompositeCarrierObserved=" + std::to_string(ws3.compositeCarrierObserved) +
        " ws3CompositeCarrierNetworkEligible=" + std::to_string(ws3.compositeCarrierNetworkEligible) +
        " ws3CompositeCarrierTargetAdmitted=" + std::to_string(ws3.compositeCarrierTargetAdmitted) +
        " ws3CompositeCarrierManifestAdmitted=" + std::to_string(ws3.compositeCarrierManifestAdmitted) +
        " ws3CompositeCarrierPreBusPreserved=" + std::to_string(ws3.compositeCarrierPreBusPreserved) +
        " ws3CompositeCarrierSubmitAttempt=" + std::to_string(ws3.compositeCarrierSubmitAttempt) +
        " ws3CompositeCarrierSubmitSuccess=" + std::to_string(ws3.compositeCarrierSubmitSuccess) +
        " ws3ScheduledStateTransitionNoCarrier=" +
            std::to_string(ws3.scheduledStateTransitionNoCarrier) +
        " ws3ScheduledStateTransitionQuarantinedNoCarrier=" +
            std::to_string(ws3.scheduledStateTransitionQuarantinedNoCarrier) +
        " ws3RelevanceCandidate=" + std::to_string(ws3.relevanceCandidates) +
        " ws3RelevanceIntrinsic=" + std::to_string(ws3.relevanceReplicateIntrinsic) +
        " ws3RelevanceCarrier=" + std::to_string(ws3.relevanceReplicateCarrier) +
        " ws3RelevanceDefer=" + std::to_string(ws3.relevanceDefer) +
        " ws3RelevanceNotRelevant=" + std::to_string(ws3.relevanceNotRelevant) +
        " ws3RelevanceUnknown=" + std::to_string(ws3.relevanceUnknown) +
        " ws3RelevanceInboundReject=" + std::to_string(ws3.relevanceInboundRejected) +
        " ws3CausalRootReservationCanceled=" + std::to_string(ws3.causalRootReservationCanceled) +
        " ws3CausalRootAlreadyClaimed=" + std::to_string(ws3.causalRootAlreadyClaimed) +
        " ws3CausalRootMissing=" + std::to_string(ws3.causalRootMissing) +
        " ws3CausalRootNonWorldLocal=" + std::to_string(ws3.causalRootNonWorldLocal) +
        " ws3CausalRootGroupOverflow=" + std::to_string(ws3.causalRootGroupOverflow) +
        " ws3CausalRootSuppressedSiblings=" + std::to_string(ws3.causalRootSuppressedSiblings) +
        " ws3CausalRootProbeStandalone=" + std::to_string(ws3.causalRootProbeStandalone) +
        " ws3CausalBoundaryProfileMissing=" + std::to_string(ws3.causalBoundaryProfileMissing) +
        " ws3CausalBoundaryNoWorldEffect=" + std::to_string(ws3.causalBoundaryNoWorldEffect) +
        " ws3CausalBoundaryPlayerDependent=" + std::to_string(ws3.causalBoundaryPlayerDependent) +
        " ws3CausalBoundaryWorldCandidate=" + std::to_string(ws3.causalBoundaryWorldCandidate) +
        " ws3CausalBoundaryTruncated=" + std::to_string(ws3.causalBoundaryTruncated) +
        " ws3CausalBoundaryUnknownDependency=" + std::to_string(ws3.causalBoundaryUnknownDependency) +
        " ws3ParentContextBound=" + std::to_string(ws3.parentContextBound) +
        " ws3ParentContextBoundLocalRoot=" + std::to_string(ws3.parentContextBoundLocalRoot) +
        " ws3ParentContextBoundRemoteRoot=" + std::to_string(ws3.parentContextBoundRemoteRoot) +
        " ws3ParentContextInheritedOrigin=" + std::to_string(ws3.parentContextInheritedOrigin) +
        " ws3ParentContextInheritedCausal=" + std::to_string(ws3.parentContextInheritedCausal) +
        " ws3ParentContextInheritedCause=" + std::to_string(ws3.parentContextInheritedCause) +
        " ws3ParentContextReleased=" + std::to_string(ws3.parentContextReleased) +
        " ws3ParentContextStale=" + std::to_string(ws3.parentContextStale) +
        " ws3ParentContextOverflow=" + std::to_string(ws3.parentContextOverflow) +
        " ws3ParentContextsLive=" + std::to_string(ws3.parentContextsLive) +
        " ws3ParentContextsPeak=" + std::to_string(ws3.parentContextsPeak) +
        " ws3LineageGuardSuppressed=" + std::to_string(ws3.lineageGuardSuppressed) +
        " ws3ProbeCallbackException=" + std::to_string(ws3.probeCallbackExceptions) +
        " ws3ProbeCausalUnknownException=" + std::to_string(ws3.probeCausalUnknownExceptions) +
        " ws3RootRateReject=" + std::to_string(ws3.rootRateRejected) +
        " ws3BridgeScope=" + std::to_string(ws3.callbackBridgeScopes) +
        " ws3BridgeSend=" + std::to_string(ws3.callbackBridgeSubmitted) +
        " ws3BridgeOwnerReject=" + std::to_string(ws3.callbackBridgeOwnerRejected) +
        " ws3BridgeSameInputReject=" + std::to_string(ws3.callbackBridgeSameInputRejected) +
        " ws3BridgeClaimedReject=" + std::to_string(ws3.callbackBridgeAlreadyClaimedRejected) +
        " ws3BridgeRateReject=" + std::to_string(ws3.callbackBridgeRateRejected) +
        " ws3BridgeQueueReject=" + std::to_string(ws3.callbackBridgeQueueRejected) +
        " ws3InstallFail=" + std::to_string(ws3.installFailures) +
        " elapsedMs=" + std::to_string(elapsedMs);

    if (diagnostics_ != nullptr) {
        diagnostics_->Log("info", "worldsync3_context_contract",
            "ROOT v10: CompositeInput/EmittedOutput, causal segments, explicit actor/owner/target and retained context. "
            "Callback flags/payload are preserved; no absent context upgrade or player singleton fallback. Unsupported contexts remain "
            "coverage gaps. Stages: 1=capture, 2=unsupported, 3=receive, 4=parent constructed, "
            "5=work built, 6=work execute, 7=destroy, 8=pending, 9=fault, 10=host-owned, "
            "11=overload/resync required. Queued is scheduler admission, not deferred completion.",
            {{"contextUnsupported", std::to_string(ws3.contextUnsupported), true},
             {"contextReconstructed", std::to_string(ws3.contextReconstructed), true},
             {"contextPending", std::to_string(ws3.contextPending), true},
             {"actorUnresolved", std::to_string(ws3.actorUnresolved), true},
             {"causeTableOverflow", std::to_string(ws3.causeTableOverflow), true}});
        for (const auto& trace : world_actions::ContextTraceSnapshot()) {
            if (trace.serial == 0U) continue;
            diagnostics_->Log("info", "worldsync3_cause_context",
                "Bounded causal trace; item/parent addresses are local diagnostics only.",
                {{"serial", std::to_string(trace.serial), true},
                 {"stage", std::to_string(trace.stage), true},
                 {"epoch", std::to_string(trace.epoch), true},
                 {"cause", std::to_string(trace.cause), true},
                 {"sourcePeer", std::to_string(trace.sourcePeer), true},
                 {"authoritySequence", std::to_string(trace.authoritySequence), true},
                 {"invocation", std::to_string(trace.invocation), true},
                 {"parentInvocation", std::to_string(trace.parentInvocation), true},
                 {"causalChainId", std::to_string(trace.causalChainId), true},
                 {"causalSource", std::to_string(trace.causalSource), true},
                 {"owner", WorldEntityIdText(trace.owner), true},
                 {"target", WorldEntityIdText(trace.target), true},
                 {"actorKind", std::to_string(trace.actorKind), true},
                 {"actor", WorldEntityIdText(trace.actor), true},
                 {"contextKind", std::to_string(trace.contextKind), true},
                 {"pendingReason", std::to_string(trace.pendingReason), true},
                 {"contextRuntimeA", ShortGuidText(trace.contextRuntimeKey.entityA), true},
                 {"contextRuntimeB", ShortGuidText(trace.contextRuntimeKey.entityB), true},
                 {"input", ShortGuidText(trace.input), true},
                 {"item", std::to_string(trace.item), true},
                 {"parent", std::to_string(trace.parent), true},
                 {"parentReadable", std::to_string(trace.parentReadable), true},
                 {"parentInput", ShortGuidText(trace.parentInput), true},
                 {"parentFlags", ShortGuidText(trace.parentFlags), true},
                 {"parentPayload", std::to_string(trace.parentPayload), true},
                 {"parentPayloadBytes", std::to_string(trace.parentPayloadBytes), true}});
        }
        diagnostics_->Log(
            "info",
            "worldsync3_public_input_stats",
            "WS3 public ROOT counters. ContextReject counts each rejected invocation once; "
            "reason counters use first failure: Arg (invalid/unreadable arguments), "
            "Payload (current WorkItem +10 managed data reference), Callback (nonempty callback "
            "outside a certified local-domain->World Method boundary; only the "
            "explicit ProbeDirect witness may bypass this reason), "
            "Delay (nonfinite/outside 0..600), Tail (nonzero). ProbeDirect flags are independent "
            "observations in the active ROOT candidate context path; unreadable context "
            "counts only ArgReject when the id is readable. Eligible means context passed "
            "before identity/send. IdentityReject now validates the boundary owner and is "
            "partitioned by first failure: BoundaryLayout (certified CompositeInstance "
            "primary/secondary vtables or public dispatch mismatch), RuntimeKey "
            "(native extraction failed/zero key), RegistryLookup (key extracted but no "
            "WorldEntityId), PublicInput (owner/input forward manifest validation rejected), "
            "DefinitionGetter (uncertified getter or outside existing code range), DefinitionNull "
            "(getter returned null), DefinitionDispatch (slot +394 mismatch), "
            "DefinitionFault (protected native read/call fault), DefinitionSelf "
            "(validated definition differs from hook self). Candidate counters only "
            "cover context-eligible ROOT candidates: Current is the original execution "
            "pointer, Self is ECX observed through the same registry API. RuntimeKeyValid "
            "means native extraction succeeded, not independent object-type proof; "
            "CandidateValid means WorldEntityId obtained. PublicInputValid/Reject is "
            "counted only with an id and reports the original candidate's direct IsCompositePublicInputAllowed, including "
            "manifest/readiness and descriptor policy. ProbeIdentity is the same diagnostic "
            "restricted to ProbeDirect. DirectOwnerValid retains the passive Current direct check; "
            "InterfaceMapValid retains the passive unambiguous interface check with exact id/input, "
            "noTypedData and final owner authorization. InterfaceMapReject counts failed "
            "historical mapping observations. Neither lookup selects/vetoes the packet owner. "
            "BoundaryOwnerValid proves both CompositeInstance vtables and the Entity-to-interface "
            "getter round trip to this hook's self. Owner candidate counters follow only that "
            "proof; OwnerPublicInputValid checks the exact unambiguous WS3 retail ROOT forward port "
            "after runtime payload/context validation. Its static DataType/noTypedData is "
            "metadata only. Current/Self and ManifestExactForwardAllowed keep the historical "
            "v1 noTypedData policy, so they can remain false while the WS3 retail ROOT succeeds. "
            "WS3 v8 transports the verified native callback recipe and actor separately from owner/target. "
            "A callback is never detached into a null holder, including ProbeDirect. "
            "World effects captured in a session await host ordering on every peer, including their sender. "
            "Autonomous causes belong to the host; player causes use the authenticated source peer. "
            "Native callback flags and MemoryAllocation payload bytes are reconstructed through FUN_00926FF0. "
            "Player-bound or non-callback parent shapes remain coverage gaps, not silently upgraded. "
            "GenericRoot/TargetedRoot count classified candidates before rate/queue admission; "
            "PayloadRoot remains zero. NeedsAdapter/LocalOnly/Unsafe/Unknown describe refused "
            "candidates. ExternalRootSend/DelayedRootSend count successfully reserved roots. "
            "TargetOwnerReject now includes unbound targets or an ineligible extension, while "
            "TargetOwnerMatched and TargetOwnerDistinct distinguish accepted identity shapes. "
            "Current/Self identity hypotheses are collected only for ProbeDirect. The rate "
            "fuse and WS2 suppression remain active. Cyclic/over-depth retained lineage now suppresses "
            "only that branch; only actual lineage-table overflow disables new WS3 capture globally. "
            "ProbeDirectRootSend counts successful root "
            "enqueue for the diagnostic id, without a special sending path. Last* is the last "
            "context-eligible ProbeDirect sample; Kind is manifest metadata, KeyA is the native "
            "entity identifier and KeyB its native scope. LastObserved=0 means no sample; "
            "LastBoundaryOwnerValid=0 means OwnerMatchesCurrent is unknown. Keys and ids here "
            "are local diagnostics only, never additional gameplay payload. Counter groups overlap. "
            "Scalar-only; no scan or recorder is armed.",
            {
                {"scan", "0", true},
                {"arm", "0", true},
                {"fix323", "0", true},
                {"hook", std::to_string(stats.schedulerHookInstalled ? 1U : 0U), true},
                {"ci27Hook", std::to_string(stats.preQueueBuilderHookInstalled ? 1U : 0U), true},
                {"version", std::to_string(stats.version), true},
                {"ci24Map", std::to_string(stats.mapped), true},
                {"ci25RuntimeMap", std::to_string(stats.runtimeEdgeMapped), true},
                {"ci26ScopeMap", std::to_string(stats.ownerScopeRuntimeEdgeMapped), true},
                {"ci27Seen", std::to_string(stats.preQueueBuilderSeen), true},
                {"ci27Input", std::to_string(stats.preQueueBuilderInputReadable), true},
                {"ci27Public", std::to_string(stats.preQueueBuilderPublic), true},
                {"ci27Internal", std::to_string(stats.preQueueBuilderInternal), true},
                {"ci27InternalExact", std::to_string(stats.preQueueBuilderInternalExactMapped), true},
                {"ci27InternalRuntime", std::to_string(stats.preQueueBuilderInternalRuntimeMapped), true},
                {"ci27InternalScope", std::to_string(stats.preQueueBuilderInternalOwnerScopeMapped), true},
                {"ci27UnknownGuid", std::to_string(stats.preQueueBuilderUnknownGuid), true},
                {"ci27TraceCount", std::to_string(stats.preQueueBuilderUnknownTraceCount), true},
                {"ci27Mapped", std::to_string(stats.preQueueBuilderMapped), true},
                {"ci27OwnerMap", std::to_string(stats.preQueueBuilderOwnerMapped), true},
                {"ci27IfaceMap", std::to_string(stats.preQueueBuilderInterfaceMapped), true},
                {"ci27Send", std::to_string(stats.preQueueBuilderSubmitted), true},
                {"ci27RemoteSkip", std::to_string(stats.preQueueBuilderRemoteSkipped), true},
                {"ci27DelayReject", std::to_string(stats.preQueueBuilderDelayRejected), true},
                {"ci27IdentityReject", std::to_string(stats.preQueueBuilderIdentityRejected), true},
                {"ci27TypedReject", std::to_string(stats.preQueueBuilderTypedRejected), true},
                {"ci27Reject", std::to_string(stats.preQueueBuilderRejected), true},
                {"ci24Send", std::to_string(stats.submitted), true},
                {"ci24Recv", std::to_string(stats.remoteAttempts), true},
                {"ci24Queue", std::to_string(stats.bridgeQueued), true},
                {"ci24SrcFallback", std::to_string(stats.sourceFallbacks), true},
                {"ci24Pending", std::to_string(stats.runtimeUnavailable), true},
                {"ci24Reject", std::to_string(stats.rejected), true},
                {"ws3Hook", std::to_string(ws3.installed), true},
                {"ws3Enabled", std::to_string(ws3.enabled), true},
                {"ws3EmptyCallbackTest", std::to_string(ws3.emptyCallbackTest), true},
                {"ws3AdmissionMask", ShortGuidText(ws3.admissionSecurityMask), false},
                {"ws3Seen", std::to_string(ws3.seen), true},
                {"ws3Public", std::to_string(ws3.publicCandidates), true},
                {"ws3RootSend", std::to_string(ws3.rootsSent), true},
                {"ws3CompositeInputRootSend", std::to_string(ws3.compositeInputRootsSent), true},
                {"ws3GenericRoot", std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::GenericRoot)]), true},
                {"ws3TargetedRoot", std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::TargetedRoot)]), true},
                {"ws3PayloadRoot", std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::PayloadRoot)]), true},
                {"ws3NeedsAdapter", std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::NeedsAdapter)]), true},
                {"ws3LocalOnly", std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::LocalOnly)]), true},
                {"ws3Unsafe", std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::Unsafe)]), true},
                {"ws3Unknown", std::to_string(ws3.classified[static_cast<std::size_t>(world_actions::ActionClass::Unknown)]), true},
                {"ws3TargetOwnerDistinct", std::to_string(ws3.targetOwnerDistinct), true},
                {"ws3ExternalRootSend", std::to_string(ws3.externalRoots), true},
                {"ws3DelayedRootSend", std::to_string(ws3.delayedRoots), true},
                {"ws3DescSkip", std::to_string(ws3.descendantsSuppressed), true},
                {"ws3RemoteSkip", std::to_string(ws3.remoteSuppressed), true},
                {"ws3IdentityReject", std::to_string(ws3.identityRejected), true},
                {"ws3IdentityRuntimeKeyReject", std::to_string(ws3.identityRuntimeKeyRejected), true},
                {"ws3IdentityRegistryLookupReject", std::to_string(ws3.identityRegistryLookupRejected), true},
                {"ws3IdentityPublicInputReject", std::to_string(ws3.identityPublicInputRejected), true},
                {"ws3IdentityDirectOwnerValid", std::to_string(ws3.identityDirectOwnerValid), true},
                {"ws3IdentityInterfaceMapValid", std::to_string(ws3.identityInterfaceMapValid), true},
                {"ws3IdentityInterfaceMapReject", std::to_string(ws3.identityInterfaceMapRejected), true},
                {"ws3ProbeIdentityDirectOwnerValid", std::to_string(ws3.probeIdentityDirectOwnerValid), true},
                {"ws3ProbeIdentityInterfaceMapValid", std::to_string(ws3.probeIdentityInterfaceMapValid), true},
                {"ws3ProbeIdentityInterfaceMapReject", std::to_string(ws3.probeIdentityInterfaceMapRejected), true},
                {"ws3IdentityDefinitionGetterReject", std::to_string(ws3.identityDefinitionGetterRejected), true},
                {"ws3IdentityDefinitionNullReject", std::to_string(ws3.identityDefinitionNullRejected), true},
                {"ws3IdentityDefinitionDispatchReject", std::to_string(ws3.identityDefinitionDispatchRejected), true},
                {"ws3IdentityDefinitionFaultReject", std::to_string(ws3.identityDefinitionFaultRejected), true},
                {"ws3IdentityDefinitionSelfReject", std::to_string(ws3.identityDefinitionSelfRejected), true},
                {"ws3IdentityCurrentRuntimeKeyValid", std::to_string(ws3.identityCurrent.runtimeKeyValid), true},
                {"ws3IdentityCurrentRuntimeKeyReject", std::to_string(ws3.identityCurrent.runtimeKeyReject), true},
                {"ws3IdentityCurrentCandidateValid", std::to_string(ws3.identityCurrent.candidateValid), true},
                {"ws3IdentityCurrentCandidateReject", std::to_string(ws3.identityCurrent.candidateReject), true},
                {"ws3IdentityCurrentPublicInputValid", std::to_string(ws3.identityCurrent.publicInputValid), true},
                {"ws3IdentityCurrentPublicInputReject", std::to_string(ws3.identityCurrent.publicInputReject), true},
                {"ws3IdentitySelfRuntimeKeyValid", std::to_string(ws3.identitySelf.runtimeKeyValid), true},
                {"ws3IdentitySelfRuntimeKeyReject", std::to_string(ws3.identitySelf.runtimeKeyReject), true},
                {"ws3IdentitySelfCandidateValid", std::to_string(ws3.identitySelf.candidateValid), true},
                {"ws3IdentitySelfCandidateReject", std::to_string(ws3.identitySelf.candidateReject), true},
                {"ws3IdentitySelfPublicInputValid", std::to_string(ws3.identitySelf.publicInputValid), true},
                {"ws3IdentitySelfPublicInputReject", std::to_string(ws3.identitySelf.publicInputReject), true},
                {"ws3ProbeIdentityCurrentRuntimeKeyValid", std::to_string(ws3.probeIdentityCurrent.runtimeKeyValid), true},
                {"ws3ProbeIdentityCurrentRuntimeKeyReject", std::to_string(ws3.probeIdentityCurrent.runtimeKeyReject), true},
                {"ws3ProbeIdentityCurrentCandidateValid", std::to_string(ws3.probeIdentityCurrent.candidateValid), true},
                {"ws3ProbeIdentityCurrentCandidateReject", std::to_string(ws3.probeIdentityCurrent.candidateReject), true},
                {"ws3ProbeIdentityCurrentPublicInputValid", std::to_string(ws3.probeIdentityCurrent.publicInputValid), true},
                {"ws3ProbeIdentityCurrentPublicInputReject", std::to_string(ws3.probeIdentityCurrent.publicInputReject), true},
                {"ws3ProbeIdentitySelfRuntimeKeyValid", std::to_string(ws3.probeIdentitySelf.runtimeKeyValid), true},
                {"ws3ProbeIdentitySelfRuntimeKeyReject", std::to_string(ws3.probeIdentitySelf.runtimeKeyReject), true},
                {"ws3ProbeIdentitySelfCandidateValid", std::to_string(ws3.probeIdentitySelf.candidateValid), true},
                {"ws3ProbeIdentitySelfCandidateReject", std::to_string(ws3.probeIdentitySelf.candidateReject), true},
                {"ws3ProbeIdentitySelfPublicInputValid", std::to_string(ws3.probeIdentitySelf.publicInputValid), true},
                {"ws3ProbeIdentitySelfPublicInputReject", std::to_string(ws3.probeIdentitySelf.publicInputReject), true},
                {"ws3ProbeIdentityRuntimeKeyReject", std::to_string(ws3.probeIdentityRuntimeKeyRejected), true},
                {"ws3ProbeIdentityRegistryLookupReject", std::to_string(ws3.probeIdentityRegistryLookupRejected), true},
                {"ws3ProbeIdentityPublicInputReject", std::to_string(ws3.probeIdentityPublicInputRejected), true},
                {"ws3ProbeIdentityDefinitionGetterReject", std::to_string(ws3.probeIdentityDefinitionGetterRejected), true},
                {"ws3ProbeIdentityDefinitionNullReject", std::to_string(ws3.probeIdentityDefinitionNullRejected), true},
                {"ws3ProbeIdentityDefinitionDispatchReject", std::to_string(ws3.probeIdentityDefinitionDispatchRejected), true},
                {"ws3ProbeIdentityDefinitionFaultReject", std::to_string(ws3.probeIdentityDefinitionFaultRejected), true},
                {"ws3ProbeIdentityDefinitionSelfReject", std::to_string(ws3.probeIdentityDefinitionSelfRejected), true},
                {"ws3IdentityBoundaryLayoutReject", std::to_string(ws3.identityBoundaryLayoutRejected), true},
                {"ws3IdentityBoundaryOwnerValid", std::to_string(ws3.identityBoundaryOwnerValid), true},
                {"ws3IdentityOwnerRuntimeKeyValid", std::to_string(ws3.identityOwner.runtimeKeyValid), true},
                {"ws3IdentityOwnerRuntimeKeyReject", std::to_string(ws3.identityOwner.runtimeKeyReject), true},
                {"ws3IdentityOwnerCandidateValid", std::to_string(ws3.identityOwner.candidateValid), true},
                {"ws3IdentityOwnerCandidateReject", std::to_string(ws3.identityOwner.candidateReject), true},
                {"ws3IdentityOwnerPublicInputValid", std::to_string(ws3.identityOwner.publicInputValid), true},
                {"ws3IdentityOwnerPublicInputReject", std::to_string(ws3.identityOwner.publicInputReject), true},
                {"ws3ProbeIdentityBoundaryLayoutReject", std::to_string(ws3.probeIdentityBoundaryLayoutRejected), true},
                {"ws3ProbeIdentityBoundaryOwnerValid", std::to_string(ws3.probeIdentityBoundaryOwnerValid), true},
                {"ws3ProbeIdentityOwnerRuntimeKeyValid", std::to_string(ws3.probeIdentityOwner.runtimeKeyValid), true},
                {"ws3ProbeIdentityOwnerRuntimeKeyReject", std::to_string(ws3.probeIdentityOwner.runtimeKeyReject), true},
                {"ws3ProbeIdentityOwnerCandidateValid", std::to_string(ws3.probeIdentityOwner.candidateValid), true},
                {"ws3ProbeIdentityOwnerCandidateReject", std::to_string(ws3.probeIdentityOwner.candidateReject), true},
                {"ws3ProbeIdentityOwnerPublicInputValid", std::to_string(ws3.probeIdentityOwner.publicInputValid), true},
                {"ws3ProbeIdentityOwnerPublicInputReject", std::to_string(ws3.probeIdentityOwner.publicInputReject), true},
                {"ws3ProbeDirectRootSend", std::to_string(ws3.probeDirectRootsSent), true},
                {"ws3ProbeIdentityLastObserved", std::to_string(ws3.probeIdentityLast.observed), true},
                {"ws3ProbeIdentityLastBoundaryOwnerValid", std::to_string(ws3.probeIdentityLast.boundaryOwnerValid), true},
                {"ws3ProbeIdentityLastOwnerMatchesCurrent", std::to_string(ws3.probeIdentityLast.ownerMatchesCurrent), true},
                {"ws3ProbeIdentityLastCurrentKind", WorldEntityKindText(ws3.probeIdentityLast.currentKind)},
                {"ws3ProbeIdentityLastOwnerKind", WorldEntityKindText(ws3.probeIdentityLast.ownerKind)},
                {"ws3ProbeIdentityLastCurrentKeyA", ShortGuidText(ws3.probeIdentityLast.currentKey.entityA)},
                {"ws3ProbeIdentityLastCurrentKeyB", ShortGuidText(ws3.probeIdentityLast.currentKey.entityB)},
                {"ws3ProbeIdentityLastOwnerKeyA", ShortGuidText(ws3.probeIdentityLast.ownerKey.entityA)},
                {"ws3ProbeIdentityLastOwnerKeyB", ShortGuidText(ws3.probeIdentityLast.ownerKey.entityB)},
                {"ws3ProbeIdentityLastCurrentId", WorldEntityIdText(ws3.probeIdentityLast.currentId)},
                {"ws3ProbeIdentityLastOwnerId", WorldEntityIdText(ws3.probeIdentityLast.ownerId)},
                {"ws3ProbeManifestReady", std::to_string(ws3.probeIdentityLast.manifestReady), true},
                {"ws3ProbeManifestGuidKnown", std::to_string(ws3.probeIdentityLast.manifestGuidKnown), true},
                {"ws3ProbeManifestExactForwardKeyPresent", std::to_string(ws3.probeIdentityLast.manifestExactForwardKeyPresent), true},
                {"ws3ProbeManifestExactForwardAllowed", std::to_string(ws3.probeIdentityLast.manifestExactForwardAllowed), true},
                {"ws3ProbeManifestExactOwnerSeenForGuid", std::to_string(ws3.probeIdentityLast.manifestExactOwnerSeenForGuid), true},
                {"ws3ProbeManifestExactBridgeCount", std::to_string(ws3.probeIdentityLast.manifestExactBridgeCount), true},
                {"ws3ProbeManifestExactPayloadlessBridgeCount", std::to_string(ws3.probeIdentityLast.manifestExactPayloadlessBridgeCount), true},
                {"ws3ProbeManifestExactTypedBridgeCount", std::to_string(ws3.probeIdentityLast.manifestExactTypedBridgeCount), true},
                {"ws3ProbeManifestGuidBridgeCount", std::to_string(ws3.probeIdentityLast.manifestGuidBridgeCount), true},
                {"ws3ProbeManifestGuidOwnerCount", std::to_string(ws3.probeIdentityLast.manifestGuidOwnerCount), true},
                {"ws3ProbeManifestFirstGuidOwner", WorldEntityIdText(ws3.probeIdentityLast.manifestFirstGuidOwner)},
                {"ws3ProbeManifestFirstGuidDataType", std::to_string(ws3.probeIdentityLast.manifestFirstGuidDataType), true},
                {"ws3ProbeManifestFirstGuidNoTypedData", std::to_string(ws3.probeIdentityLast.manifestFirstGuidNoTypedData), true},
                {"ws3ContextReject", std::to_string(ws3.contextRejected), true},
                {"ws3ArgReject", std::to_string(ws3.argRejected), true},
                {"ws3PayloadReject", std::to_string(ws3.payloadRejected), true},
                {"ws3CallbackReject", std::to_string(ws3.callbackRejected), true},
                {"ws3DelayReject", std::to_string(ws3.delayRejected), true},
                {"ws3TailReject", std::to_string(ws3.tailRejected), true},
                {"ws3ProbeDirectSeen", std::to_string(ws3.probeDirectSeen), true},
                {"ws3ProbeDirectArgReject", std::to_string(ws3.probeDirectArgRejected), true},
                {"ws3ProbeDirectPayload", std::to_string(ws3.probeDirectPayload), true},
                {"ws3ProbeDirectCallback", std::to_string(ws3.probeDirectCallback), true},
                {"ws3ProbeDirectDelay", std::to_string(ws3.probeDirectDelay), true},
                {"ws3ProbeDirectTail", std::to_string(ws3.probeDirectTail), true},
                {"ws3ProbeDirectEligible", std::to_string(ws3.probeDirectEligible), true},
                {"ws3TargetOwnerReject", std::to_string(ws3.targetOwnerRejected), true},
                {"ws3TargetOwnerMatch", std::to_string(ws3.targetOwnerMatched), true},
                {"ws3ProbeTargetOwnerReject", std::to_string(ws3.probeTargetOwnerRejected), true},
                {"ws3PlayerCausalReject", std::to_string(ws3.playerCausalRejected), true},
                {"ws3CausalScanned", std::to_string(ws3.playerCausalNodesScanned), true},
                {"ws3CausalMapped", std::to_string(ws3.playerCausalNodesMapped), true},
                {"ws3CausalUnknownReject", std::to_string(ws3.causalUnknownRejected), true},
                {"ws3CausalShapeReject", std::to_string(ws3.causalShapeRejected), true},
                {"ws3CausalUnmappedNodes", std::to_string(ws3.causalUnmappedNodes), true},
                {"ws3CausalCycleReject", std::to_string(ws3.causalCycleRejected), true},
                {"ws3CausalDepthReject", std::to_string(ws3.causalDepthRejected), true},
                {"ws3QueueReject", std::to_string(ws3.queueRejected), true},
                {"ws3Overflow", std::to_string(ws3.lineageOverflow), true},
                {"ws3LineageBuildInherit", std::to_string(ws3.lineageBuildInherit), true},
                {"ws3LineageExecuteRestore", std::to_string(ws3.lineageExecuteRestore), true},
                {"ws3LineageCallbackRestore", std::to_string(ws3.lineageCallbackRestore), true},
                {"ws3Applied", std::to_string(ws3.applied), true},
                {"ws3ApplyReject", std::to_string(ws3.applyRejected), true},
                {"ws3Fault", std::to_string(ws3.nativeFaults), true},
                {"ws3Ws2Bypass", std::to_string(ws3.ws2Bypassed), true},
                {"ws3CallbackRoot", std::to_string(ws3.callbackRoots), true},
                {"ws3CallbackGameplayReject", std::to_string(ws3.callbackGameplayRejected), true},
                {"ws3CallbackBoundaryCandidate", std::to_string(ws3.callbackBoundaryCandidates), true},
                {"ws3CallbackBoundaryRoot", std::to_string(ws3.callbackBoundaryRoots), true},
                {"ws3CallbackBoundaryPlayer", std::to_string(ws3.callbackBoundaryPlayerRoots), true},
                {"ws3CallbackBoundaryAI", std::to_string(ws3.callbackBoundaryAiRoots), true},
                {"ws3CallbackBoundaryUI", std::to_string(ws3.callbackBoundaryUiRoots), true},
                {"ws3CallbackBoundaryAIMP", std::to_string(ws3.callbackBoundaryAimpRoots), true},
                {"ws3CallbackBoundaryApplied", std::to_string(ws3.callbackBoundaryApplied), true},
                {"ws3CallbackWorldContinuationReject", std::to_string(ws3.callbackWorldContinuationRejected), true},
                {"ws3CallbackBoundaryUnknownReject", std::to_string(ws3.callbackBoundaryUnknownRejected), true},
                {"ws3AdmissionForbiddenInputBypass", std::to_string(ws3.admissionForbiddenInputBypassed), true},
                {"ws3AdmissionGuidPrefilterReject", std::to_string(ws3.admissionGuidPrefilterRejected), true},
                {"ws3AdmissionGuidPrefilterBypass", std::to_string(ws3.admissionGuidPrefilterBypassed), true},
                {"ws3AdmissionPayloadBypass", std::to_string(ws3.admissionPayloadBypassed), true},
                {"ws3AdmissionCallbackShapeBypass", std::to_string(ws3.admissionCallbackShapeBypassed), true},
                {"ws3AdmissionTailBypass", std::to_string(ws3.admissionTailBypassed), true},
                {"ws3AdmissionPlayerTargetBypass", std::to_string(ws3.admissionPlayerTargetBypassed), true},
                {"ws3AdmissionAITargetBypass", std::to_string(ws3.admissionAiTargetBypassed), true},
                {"ws3AdmissionUITargetBypass", std::to_string(ws3.admissionUiTargetBypassed), true},
                {"ws3AdmissionAIMPTargetBypass", std::to_string(ws3.admissionAimpTargetBypassed), true},
                {"ws3AdmissionWorldContinuationBypass", std::to_string(ws3.admissionWorldContinuationBypassed), true},
                {"ws3AdmissionUnknownCausalityBypass", std::to_string(ws3.admissionUnknownCausalityBypassed), true},
                {"ws3AdmissionManifestOwnerInputBypass", std::to_string(ws3.admissionManifestOwnerInputBypassed), true},
                {"ws3AdmissionManifestTargetBypass", std::to_string(ws3.admissionManifestTargetBypassed), true},
                {"ws3AdmissionDescendantBypass", std::to_string(ws3.admissionDescendantBypassed), true},
                {"ws3CausalProvSeed", std::to_string(ws3.causalProvenanceSeeded), true},
                {"ws3CausalProvInherit", std::to_string(ws3.causalProvenanceInherited), true},
                {"ws3CausalProvBoundary", std::to_string(ws3.causalProvenanceBoundary), true},
                {"ws3CausalProvOverflow", std::to_string(ws3.causalProvenanceOverflow), true},
                {"ws3CausalProvPlayer", std::to_string(ws3.causalProvenancePlayer), true},
                {"ws3CausalProvAI", std::to_string(ws3.causalProvenanceAi), true},
                {"ws3CausalProvUI", std::to_string(ws3.causalProvenanceUi), true},
                {"ws3CausalProvAIMP", std::to_string(ws3.causalProvenanceAimp), true},
                {"ws3CausalChainsCreated", std::to_string(ws3.causalChainsCreated), true},
                {"ws3CausalChainsReleased", std::to_string(ws3.causalChainsReleased), true},
                {"ws3CausalGroupsLive", std::to_string(ws3.causalGroupsLive), true},
                {"ws3CausalGroupsPeak", std::to_string(ws3.causalGroupsLivePeak), true},
                {"ws3CausalRootReserved", std::to_string(ws3.causalRootReserved), true},
                {"ws3CausalRootCommitted", std::to_string(ws3.causalRootCommitted), true},
                {"ws3CausalRootReservationCanceled", std::to_string(ws3.causalRootReservationCanceled), true},
                {"ws3CausalRootAlreadyClaimed", std::to_string(ws3.causalRootAlreadyClaimed), true},
                {"ws3CausalRootMissing", std::to_string(ws3.causalRootMissing), true},
                {"ws3CausalRootNonWorldLocal", std::to_string(ws3.causalRootNonWorldLocal), true},
                {"ws3CausalRootGroupOverflow", std::to_string(ws3.causalRootGroupOverflow), true},
                {"ws3CausalRootSuppressedSiblings", std::to_string(ws3.causalRootSuppressedSiblings), true},
                {"ws3CausalRootProbeStandalone", std::to_string(ws3.causalRootProbeStandalone), true},
                {"ws3CausalBoundaryProfileMissing", std::to_string(ws3.causalBoundaryProfileMissing), true},
                {"ws3CausalBoundaryNoWorldEffect", std::to_string(ws3.causalBoundaryNoWorldEffect), true},
                {"ws3CausalBoundaryPlayerDependent", std::to_string(ws3.causalBoundaryPlayerDependent), true},
                {"ws3CausalBoundaryWorldCandidate", std::to_string(ws3.causalBoundaryWorldCandidate), true},
                {"ws3CausalBoundaryTruncated", std::to_string(ws3.causalBoundaryTruncated), true},
                {"ws3CausalBoundaryUnknownDependency", std::to_string(ws3.causalBoundaryUnknownDependency), true},
                {"ws3ParentContextBound", std::to_string(ws3.parentContextBound), true},
                {"ws3ParentContextBoundLocalRoot", std::to_string(ws3.parentContextBoundLocalRoot), true},
                {"ws3ParentContextBoundRemoteRoot", std::to_string(ws3.parentContextBoundRemoteRoot), true},
                {"ws3ParentContextInheritedOrigin", std::to_string(ws3.parentContextInheritedOrigin), true},
                {"ws3ParentContextInheritedCausal", std::to_string(ws3.parentContextInheritedCausal), true},
                {"ws3ParentContextInheritedCause", std::to_string(ws3.parentContextInheritedCause), true},
                {"ws3ParentContextReleased", std::to_string(ws3.parentContextReleased), true},
                {"ws3ParentContextStale", std::to_string(ws3.parentContextStale), true},
                {"ws3ParentContextOverflow", std::to_string(ws3.parentContextOverflow), true},
                {"ws3ParentContextsLive", std::to_string(ws3.parentContextsLive), true},
                {"ws3ParentContextsPeak", std::to_string(ws3.parentContextsPeak), true},
                {"ws3LineageGuardSuppressed", std::to_string(ws3.lineageGuardSuppressed), true},
                {"ws3ProbeCallbackException", std::to_string(ws3.probeCallbackExceptions), true},
                {"ws3ProbeCausalUnknownException", std::to_string(ws3.probeCausalUnknownExceptions), true},
                {"ws3RootRateReject", std::to_string(ws3.rootRateRejected), true},
                {"ws3BridgeScope", std::to_string(ws3.callbackBridgeScopes), true},
                {"ws3BridgeSend", std::to_string(ws3.callbackBridgeSubmitted), true},
                {"ws3BridgeOwnerReject", std::to_string(ws3.callbackBridgeOwnerRejected), true},
                {"ws3BridgeSameInputReject", std::to_string(ws3.callbackBridgeSameInputRejected), true},
                {"ws3BridgeClaimedReject", std::to_string(ws3.callbackBridgeAlreadyClaimedRejected), true},
                {"ws3BridgeRateReject", std::to_string(ws3.callbackBridgeRateRejected), true},
                {"ws3BridgeQueueReject", std::to_string(ws3.callbackBridgeQueueRejected), true},
                {"ws3RelevanceCandidate", std::to_string(ws3.relevanceCandidates), true},
                {"ws3RelevanceReplicateIntrinsic", std::to_string(ws3.relevanceReplicateIntrinsic), true},
                {"ws3RelevanceReplicateCarrier", std::to_string(ws3.relevanceReplicateCarrier), true},
                {"ws3CarrierObserved", std::to_string(ws3.carrierObserved), true},
                {"ws3CarrierNetworkEligible", std::to_string(ws3.carrierNetworkEligible), true},
                {"ws3CarrierForwardingComplete", std::to_string(ws3.carrierForwardingComplete), true},
                {"ws3CarrierSelectorResolved", std::to_string(ws3.carrierSelectorResolved), true},
                {"ws3CarrierSelectorRejected", std::to_string(ws3.carrierSelectorRejected), true},
                {"ws3CarrierContextCaptured", std::to_string(ws3.carrierContextCaptured), true},
                {"ws3CarrierManifestAdmitted", std::to_string(ws3.carrierManifestAdmitted), true},
                {"ws3CarrierPreBusPreserved", std::to_string(ws3.carrierPreBusPreserved), true},
                {"ws3CarrierSubmitAttempt", std::to_string(ws3.carrierSubmitAttempt), true},
                {"ws3CarrierSubmitSuccess", std::to_string(ws3.carrierSubmitSuccess), true},
                {"ws3ScheduledCoveredByCarrier", std::to_string(ws3.scheduledCoveredByCarrier), true},
                {"ws3ScheduledCoveredByCarrierSuppressed",
                 std::to_string(ws3.scheduledCoveredByCarrierSuppressed), true},
                {"ws3CompositeCarrierObserved", std::to_string(ws3.compositeCarrierObserved), true},
                {"ws3CompositeCarrierNetworkEligible", std::to_string(ws3.compositeCarrierNetworkEligible), true},
                {"ws3CompositeCarrierTargetAdmitted", std::to_string(ws3.compositeCarrierTargetAdmitted), true},
                {"ws3CompositeCarrierManifestAdmitted", std::to_string(ws3.compositeCarrierManifestAdmitted), true},
                {"ws3CompositeCarrierPreBusPreserved", std::to_string(ws3.compositeCarrierPreBusPreserved), true},
                {"ws3CompositeCarrierSubmitAttempt", std::to_string(ws3.compositeCarrierSubmitAttempt), true},
                {"ws3CompositeCarrierSubmitSuccess", std::to_string(ws3.compositeCarrierSubmitSuccess), true},
                {"ws3CanonicalCompositeRootSend", std::to_string(ws3.canonicalCompositeRootsSent), true},
                {"ws3CanonicalCompositeRootApplied", std::to_string(ws3.canonicalCompositeRootsApplied), true},
                {"ws3DoorLockStateRootSend", std::to_string(ws3.doorLockStateRootsSent), true},
                {"ws3DoorLockStateRootApplied", std::to_string(ws3.doorLockStateRootsApplied), true},
                {"ws3ScheduledStateTransitionNoCarrier",
                 std::to_string(ws3.scheduledStateTransitionNoCarrier), true},
                {"ws3ScheduledStateTransitionQuarantinedNoCarrier",
                 std::to_string(ws3.scheduledStateTransitionQuarantinedNoCarrier), true},
                {"ws3RelevanceDefer", std::to_string(ws3.relevanceDefer), true},
                {"ws3RelevanceNotRelevant", std::to_string(ws3.relevanceNotRelevant), true},
                {"ws3RelevanceUnknown", std::to_string(ws3.relevanceUnknown), true},
                {"ws3RelevanceReasonRouting", std::to_string(ws3.relevanceReasonRouting), true},
                {"ws3RelevanceReasonSchedulerMaintenance", std::to_string(ws3.relevanceReasonSchedulerMaintenance), true},
                {"ws3RelevanceReasonInternalState", std::to_string(ws3.relevanceReasonInternalState), true},
                {"ws3RelevanceReasonNoSink", std::to_string(ws3.relevanceReasonNoSink), true},
                {"ws3RelevanceReasonBetterDescendant", std::to_string(ws3.relevanceReasonBetterDescendant), true},
                {"ws3RelevanceReasonCycleAmbiguous", std::to_string(ws3.relevanceReasonCycleAmbiguous), true},
                {"ws3RelevanceReasonHistorySensitive", std::to_string(ws3.relevanceReasonHistorySensitive), true},
                {"ws3RelevanceReasonStateTransitionCarrier", std::to_string(ws3.relevanceReasonStateTransitionCarrier), true},
                {"ws3RelevanceReasonCompositeStateTransitionCarrier", std::to_string(ws3.relevanceReasonCompositeStateTransitionCarrier), true},
                {"ws3RelevanceReasonCompositeRelevantFrontierRelaxed", std::to_string(ws3.relevanceReasonCompositeRelevantFrontierRelaxed), true},
                {"ws3RelevanceInboundRejected", std::to_string(ws3.relevanceInboundRejected), true},
                {"ws3RelevanceTopCount", std::to_string(ws3.relevanceTopCount), true},
                {"ws3InstallFail", std::to_string(ws3.installFailures), true},
                {"elapsedMs", std::to_string(elapsedMs), true},
            });

        for (std::size_t i = 0; i < stats.preQueueBuilderUnknownTrace.size(); ++i) {
            const auto& trace = stats.preQueueBuilderUnknownTrace[i];
            if (!trace.valid) continue;
            diagnostics_->Log(
                "info",
                "ws3_prequeue_unknown_trace",
                "Bounded diagnostic capture of an unknown FUN_00921F70 input observed only while a certified callback BridgeScope was active. This event is not transmitted and does not alter WS3 acceptance.",
                {
                    {"slot", std::to_string(i), true},
                    {"inputShortGuid", ShortGuidText(trace.inputShortGuid)},
                    {"callerRva", ShortGuidText(trace.callerRva)},
                    {"targetKeyA", ShortGuidText(trace.targetRuntimeKey.entityA)},
                    {"targetKeyB", ShortGuidText(trace.targetRuntimeKey.entityB)},
                    {"targetExecution", ShortGuidText(static_cast<std::uint32_t>(trace.targetExecution))},
                    {"targetId", WorldEntityIdText(trace.targetId)},
                    {"outerPublicInput", ShortGuidText(trace.outerPublicInput)},
                    {"bridgeOwner", WorldEntityIdText(trace.bridgeOwner)},
                });
        }
    }
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteWorldStateDiscoveryBaseline() const {
    GameCommandExecutionResult result{};
    if (worldRegistry_ == nullptr || gameThreadDispatcher_ == nullptr ||
        !worldRegistry_->RuntimeCaptureReady() || !gameThreadDispatcher_->IsEnabled()) {
        result.statusCode = "WORLD_STATE_DISCOVERY_UNAVAILABLE";
        result.message = "The bounded CATHODE runtime-state capture surface is unavailable.";
        return result;
    }

    const auto generation = worldRegistry_->DiagnosticsSnapshot().levelGeneration;
    std::uint64_t session = 0U;
    {
        std::lock_guard lock(gFix323Mutex);
        if (gFix323CaptureMode != Fix323CaptureMode::None ||
            gFix323CaptureTaskQueued) {
            result.statusCode = "WORLD_STATE_DISCOVERY_BUSY";
            result.message = "A state capture is already in progress. Wait for its completion popup.";
            return result;
        }
        Fix323ResetForGenerationLocked(generation);
        gFix328StateDiscoveryActive = true;
        session = ++gFix328StateDiscoverySession;
    }

    const auto state = AdvanceFix323FullStateCapture(
        this,
        worldRegistry_,
        gameThreadDispatcher_,
        diagnostics_);
    result.executed = state.phase == 10U || state.phase == 1U;
    result.statusCode = result.executed
        ? "WORLD_STATE_DISCOVERY_BASELINE_STARTED"
        : "WORLD_STATE_DISCOVERY_BASELINE_REJECTED";
    result.message = "session=" + std::to_string(session) +
        " phase=" + std::to_string(state.phase) +
        " generation=" + std::to_string(generation) +
        ". Wait for the Fix3.28 baseline-complete popup before triggering exactly one witness action.";
    if (diagnostics_ != nullptr) {
        diagnostics_->Log(
            result.executed ? "info" : "warning",
            "worldsync_fix328_state_discovery_baseline_started",
            "Fix3.28 started a fresh local-only CATHODE state baseline. No network operation, gameplay write, or historical WorldSync probe was armed.",
            {
                {"session", std::to_string(session), true},
                {"phase", std::to_string(state.phase), true},
                {"generation", std::to_string(generation), true},
            });
    }
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteWorldStateDiscoveryAfter() const {
    GameCommandExecutionResult result{};
    if (worldRegistry_ == nullptr || gameThreadDispatcher_ == nullptr ||
        !worldRegistry_->RuntimeCaptureReady() || !gameThreadDispatcher_->IsEnabled()) {
        result.statusCode = "WORLD_STATE_DISCOVERY_UNAVAILABLE";
        result.message = "The bounded CATHODE runtime-state capture surface is unavailable.";
        return result;
    }

    const auto generation = worldRegistry_->DiagnosticsSnapshot().levelGeneration;
    std::uint64_t session = 0U;
    {
        std::lock_guard lock(gFix323Mutex);
        if (!gFix328StateDiscoveryActive || gFix323Generation != generation) {
            result.statusCode = "WORLD_STATE_DISCOVERY_NO_BASELINE";
            result.message = "Start a fresh Fix3.28 baseline for the current LevelGeneration first.";
            return result;
        }
        if (gFix323CaptureMode != Fix323CaptureMode::None ||
            gFix323CaptureTaskQueued) {
            result.statusCode = "WORLD_STATE_DISCOVERY_BUSY";
            result.message = "The baseline/after capture is still running. Wait for its completion popup.";
            return result;
        }
        if (gFix323Phase != 1U) {
            result.statusCode = gFix323Phase >= 2U
                ? "WORLD_STATE_DISCOVERY_DIFF_ALREADY_READY"
                : "WORLD_STATE_DISCOVERY_BASELINE_NOT_READY";
            result.message = gFix323Phase >= 2U
                ? "This discovery session already has a completed diff. Start Baseline to begin another witness."
                : "The Fix3.28 baseline is not ready yet.";
            return result;
        }
        session = gFix328StateDiscoverySession;
    }

    const auto state = AdvanceFix323FullStateCapture(
        this,
        worldRegistry_,
        gameThreadDispatcher_,
        diagnostics_);
    result.executed = state.phase == 11U || state.phase == 2U;
    result.statusCode = result.executed
        ? "WORLD_STATE_DISCOVERY_AFTER_STARTED"
        : "WORLD_STATE_DISCOVERY_AFTER_REJECTED";
    result.message = "session=" + std::to_string(session) +
        " phase=" + std::to_string(state.phase) +
        " generation=" + std::to_string(generation) +
        ". Wait for the Fix3.28 diff-complete popup, then export diagnostics.";
    if (diagnostics_ != nullptr) {
        diagnostics_->Log(
            result.executed ? "info" : "warning",
            "worldsync_fix328_state_discovery_after_started",
            "Fix3.28 started the post-witness state capture. The resulting diff remains local diagnostic evidence only.",
            {
                {"session", std::to_string(session), true},
                {"phase", std::to_string(state.phase), true},
                {"generation", std::to_string(generation), true},
            });
    }
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteWorldStateDiscoveryStatus() const {
    GameCommandExecutionResult result{};
    if (worldRegistry_ == nullptr) {
        result.statusCode = "WORLD_STATE_DISCOVERY_UNAVAILABLE";
        result.message = "WorldEntityRegistry is not initialized.";
        return result;
    }

    const auto generation = worldRegistry_->DiagnosticsSnapshot().levelGeneration;
    Fix323Diagnostics state{};
    std::uint64_t session = 0U;
    bool active = false;
    bool staleGeneration = false;
    {
        std::lock_guard lock(gFix323Mutex);
        active = gFix328StateDiscoveryActive;
        session = gFix328StateDiscoverySession;
        staleGeneration = active && gFix323Generation != generation;
        state = gFix323Result;
        state.levelGeneration = gFix323Generation;
        if (gFix323CaptureMode != Fix323CaptureMode::None) {
            state.phase = gFix323CaptureMode == Fix323CaptureMode::Baseline
                ? 10U
                : 11U;
            if (gFix323CaptureMode == Fix323CaptureMode::Baseline) {
                Fix323CopySnapshotSummary(state.baseline, gFix323Cursor.snapshot);
            } else {
                Fix323CopySnapshotSummary(state.after, gFix323Cursor.snapshot);
            }
        } else {
            state.phase = gFix323Phase;
        }
    }

    result.executed = true;
    if (!active) {
        result.statusCode = "WORLD_STATE_DISCOVERY_IDLE";
    } else if (staleGeneration) {
        result.statusCode = "WORLD_STATE_DISCOVERY_STALE_GENERATION";
    } else if (state.phase == 10U) {
        result.statusCode = "WORLD_STATE_DISCOVERY_BASELINE_IN_PROGRESS";
    } else if (state.phase == 1U) {
        result.statusCode = "WORLD_STATE_DISCOVERY_BASELINE_READY";
    } else if (state.phase == 11U) {
        result.statusCode = "WORLD_STATE_DISCOVERY_AFTER_IN_PROGRESS";
    } else if (state.phase >= 2U) {
        result.statusCode = "WORLD_STATE_DISCOVERY_DIFF_READY";
    } else {
        result.statusCode = "WORLD_STATE_DISCOVERY_IDLE";
    }
    result.message = "session=" + std::to_string(session) +
        " active=" + std::to_string(active ? 1U : 0U) +
        " phase=" + std::to_string(state.phase) +
        " generation=" + std::to_string(state.levelGeneration) +
        " currentGeneration=" + std::to_string(generation) +
        " baseCaptured=" + std::to_string(state.baseline.capturedEntries) +
        " afterCaptured=" + std::to_string(state.after.capturedEntries) +
        " modified=" + std::to_string(state.modified) +
        " appeared=" + std::to_string(state.appeared) +
        " disappeared=" + std::to_string(state.disappeared) +
        " deltaEntries=" + std::to_string(state.deltaEntries);
    if (diagnostics_ != nullptr) {
        diagnostics_->Log(
            "info",
            "worldsync_fix328_state_discovery_status",
            "Fix3.28 state-discovery status; this command is read-only and starts no capture/probe.",
            {
                {"session", std::to_string(session), true},
                {"active", std::to_string(active ? 1U : 0U), true},
                {"phase", std::to_string(state.phase), true},
                {"generation", std::to_string(state.levelGeneration), true},
                {"currentGeneration", std::to_string(generation), true},
                {"baseCaptured", std::to_string(state.baseline.capturedEntries), true},
                {"afterCaptured", std::to_string(state.after.capturedEntries), true},
                {"modified", std::to_string(state.modified), true},
                {"appeared", std::to_string(state.appeared), true},
                {"disappeared", std::to_string(state.disappeared), true},
                {"deltaEntries", std::to_string(state.deltaEntries), true},
            });
    }
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteWorldInteractionStats() const {
    GameCommandExecutionResult result{};
    {
        std::lock_guard lock(gFix323Mutex);
        if (gFix328StateDiscoveryActive && gFix323Phase < 2U) {
            result.statusCode = "WORLD_INTERACTION_STATS_BLOCKED_BY_STATE_DISCOVERY";
            result.message = "Fix3.28 state discovery is active. Use the dedicated Baseline / After / Status commands; no historical probe was armed.";
            return result;
        }
    }
    // Fix3.23 full-surface differential capture is advanced before arming any
    // of the historical witness probes so the baseline/after snapshots are not
    // polluted by the stats command itself. First call = baseline, second call
    // = normalized diff; no timing window is involved.
    const auto fullStateDiff = AdvanceFix323FullStateCapture(
        this,
        worldRegistry_,
        gameThreadDispatcher_,
        diagnostics_);
    // WORLD_INTERACTION_STATS may arm bounded read-only diagnostic windows,
    // but it must never change replication/replay behavior. In particular, the
    // historical Fix3.20 Character callback replay experiment is NOT armed here.
    world_interactions::ArmLogicDownstreamProbe();
    world_interactions::ArmCausalDifferentialTrace();
    world_interactions::ArmCompositeStateDifferentialTrace();
    world_interactions::ArmCompositeDispatchDifferentialTrace();
    world_interactions::ArmRemoteTypedIdentityProbe();
    world_interactions::ArmTypedMessageOriginProbe();
    world_interactions::ArmCharacterCallbackOriginProbe();
    const auto stats = world_interactions::DiagnosticsSnapshot();
    const auto causalTrace = world_interactions::CausalTraceDiagnosticsSnapshot();
    const auto compositeTrace = world_interactions::CompositeStateTraceDiagnosticsSnapshot();
    const auto compositeDispatchTrace = world_interactions::CompositeDispatchTraceDiagnosticsSnapshot();
    const auto legacy = world_animated::DiagnosticsSnapshot();

    if (diagnostics_ != nullptr && fullStateDiff.phase != 0U) {
        diagnostics_->Log(
            "info",
            "worldsync_fix323_full_state_status",
            "Fix3.23 BuildFix01 incremental full-state recorder status. Detailed phase-2 evidence is emitted once by the capture completion task in one packed JSON event.",
            {
                {"phase", std::to_string(fullStateDiff.phase), true},
                {"generation", std::to_string(fullStateDiff.levelGeneration), true},
                {"baseProviders", std::to_string(fullStateDiff.baseline.providerEntries), true},
                {"baseInspected", std::to_string(fullStateDiff.baseline.inspectedEntries), true},
                {"baseCaptured", std::to_string(fullStateDiff.baseline.capturedEntries), true},
                {"baseElapsedMs", std::to_string(fullStateDiff.baseline.elapsedMs), true},
                {"afterProviders", std::to_string(fullStateDiff.after.providerEntries), true},
                {"afterInspected", std::to_string(fullStateDiff.after.inspectedEntries), true},
                {"afterCaptured", std::to_string(fullStateDiff.after.capturedEntries), true},
                {"afterElapsedMs", std::to_string(fullStateDiff.after.elapsedMs), true},
                {"unchanged", std::to_string(fullStateDiff.unchanged), true},
                {"modified", std::to_string(fullStateDiff.modified), true},
                {"appeared", std::to_string(fullStateDiff.appeared), true},
                {"disappeared", std::to_string(fullStateDiff.disappeared), true},
            });
    }

    if (diagnostics_ != nullptr && causalTrace.anchors != 0U) {
        diagnostics_->Log(
            "info",
            "worldsync_fix319_causal_trace_summary",
            "Fix3.19 bounded CATHODE causal trace snapshot.",
            {
                {"role", std::to_string(causalTrace.anchorRole), true},
                {"anchors", std::to_string(causalTrace.anchors), true},
                {"inspected", std::to_string(causalTrace.inspected), true},
                {"accepted", std::to_string(causalTrace.accepted), true},
                {"unique", std::to_string(causalTrace.uniqueEntries), true},
                {"drops", std::to_string(causalTrace.capacityDrops), true},
                {"readFailures", std::to_string(causalTrace.readFailures), true},
                {"originTickMs", std::to_string(causalTrace.originTickMs), true},
                {"entryCount", std::to_string(causalTrace.entryCount), true},
                {"capacity", std::to_string(world_interactions::kFix319CausalTraceMaximumEntries), true},
            });
        for (std::size_t i = 0U; i < causalTrace.entryCount; ++i) {
            const auto& entry = causalTrace.entries[i];
            diagnostics_->Log(
                "info",
                "worldsync_fix319_causal_trace_entry",
                "Normalized Fix3.19 CATHODE trace entry; observation only.",
                {
                    {"index", std::to_string(i), true},
                    {"sig", std::to_string(entry.signatureHash), true},
                    {"count", std::to_string(entry.count), true},
                    {"channel", std::to_string(static_cast<std::uint32_t>(entry.channel)), true},
                    {"worldMask", std::to_string(entry.worldMask), true},
                    {"srcKey", ShortGuidText(entry.sourceRuntimeKey.entityA) + "/" +
                        ShortGuidText(entry.sourceRuntimeKey.entityB)},
                    {"dstKey", ShortGuidText(entry.targetRuntimeKey.entityA) + "/" +
                        ShortGuidText(entry.targetRuntimeKey.entityB)},
                    {"srcId", WorldEntityIdText(entry.sourceWorldEntity)},
                    {"dstId", WorldEntityIdText(entry.targetWorldEntity)},
                    {"primary", ShortGuidText(entry.primary)},
                    {"secondary", ShortGuidText(entry.secondary)},
                    {"payloadBytes", std::to_string(entry.payloadBytes), true},
                    {"payloadHash", std::to_string(entry.payloadHash), true},
                    {"delayBits", std::to_string(entry.delayBits), true},
                    {"tail", ShortGuidText(entry.tailArgument)},
                    {"cbInput", ShortGuidText(entry.callbackInput)},
                    {"cbFlags", ShortGuidText(entry.callbackFlags)},
                    {"cbKey", ShortGuidText(entry.callbackRuntimeKey.entityA) + "/" +
                        ShortGuidText(entry.callbackRuntimeKey.entityB)},
                    {"cbVtable", ShortGuidText(entry.callbackExecutionVtableRva)},
                    {"semanticFlags", std::to_string(entry.semanticFlags), true},
                    {"executionMask", std::to_string(entry.executionMask), true},
                    {"firstDt", std::to_string(entry.firstDeltaMs), true},
                    {"lastDt", std::to_string(entry.lastDeltaMs), true},
                    {"firstCall", ShortGuidText(entry.firstCallSiteRva)},
                    {"lastCall", ShortGuidText(entry.lastCallSiteRva)},
                    {"callVar", std::to_string(entry.callSiteVaried ? 1U : 0U), true},
                });
        }
    }
    if (diagnostics_ != nullptr &&
        (compositeDispatchTrace.hookInstalled || compositeDispatchTrace.armed ||
         compositeDispatchTrace.installFailures != 0U || compositeDispatchTrace.entryCount != 0U)) {
        diagnostics_->Log(
            "info",
            "worldsync_fix322_composite_dispatch_trace_summary",
            "Fix3.22 bounded Composite execution before/after trace; observation only.",
            {
                {"hook", std::to_string(compositeDispatchTrace.hookInstalled ? 1U : 0U), true},
                {"armed", std::to_string(compositeDispatchTrace.armed ? 1U : 0U), true},
                {"active", std::to_string(compositeDispatchTrace.active ? 1U : 0U), true},
                {"redirects", std::to_string(compositeDispatchTrace.redirectCallSites), true},
                {"installFailures", std::to_string(compositeDispatchTrace.installFailures), true},
                {"inspected", std::to_string(compositeDispatchTrace.inspected), true},
                {"accepted", std::to_string(compositeDispatchTrace.accepted), true},
                {"unique", std::to_string(compositeDispatchTrace.uniqueEntries), true},
                {"changedCalls", std::to_string(compositeDispatchTrace.changedCalls), true},
                {"drops", std::to_string(compositeDispatchTrace.capacityDrops), true},
                {"readFailures", std::to_string(compositeDispatchTrace.readFailures), true},
                {"limitDrops", std::to_string(compositeDispatchTrace.limitDrops), true},
                {"originTickMs", std::to_string(compositeDispatchTrace.originTickMs), true},
                {"entryCount", std::to_string(compositeDispatchTrace.entryCount), true},
                {"capacity", std::to_string(world_interactions::kFix322CompositeDispatchTraceMaximumEntries), true},
            });
        for (std::size_t i = 0U; i < compositeDispatchTrace.entryCount; ++i) {
            const auto& entry = compositeDispatchTrace.entries[i];
            diagnostics_->Log(
                "info",
                "worldsync_fix322_composite_dispatch_trace_entry",
                "Normalized Fix3.22 Composite execution snapshot; raw word changes only, no gameplay semantics assigned.",
                {
                    {"index", std::to_string(i), true},
                    {"sig", std::to_string(entry.signatureHash), true},
                    {"count", std::to_string(entry.count), true},
                    {"targetRva", ShortGuidText(entry.targetRva)},
                    {"callerRva", ShortGuidText(entry.callerRva)},
                    {"firstPointerRva", ShortGuidText(entry.firstPointerRva)},
                    {"firstPointerReadable", std::to_string(entry.firstPointerReadable), true},
                    {"beforeHash", std::to_string(entry.beforeHash), true},
                    {"afterHash", std::to_string(entry.afterHash), true},
                    {"changedMask", ShortGuidText(entry.changedWordMask)},
                    {"changedCount", std::to_string(entry.changedWordCount), true},
                    {"change0Offset", ShortGuidText(entry.changedOffsets[0])},
                    {"change0Before", ShortGuidText(entry.beforeWords[0])},
                    {"change0After", ShortGuidText(entry.afterWords[0])},
                    {"change1Offset", ShortGuidText(entry.changedOffsets[1])},
                    {"change1Before", ShortGuidText(entry.beforeWords[1])},
                    {"change1After", ShortGuidText(entry.afterWords[1])},
                    {"change2Offset", ShortGuidText(entry.changedOffsets[2])},
                    {"change2Before", ShortGuidText(entry.beforeWords[2])},
                    {"change2After", ShortGuidText(entry.afterWords[2])},
                    {"change3Offset", ShortGuidText(entry.changedOffsets[3])},
                    {"change3Before", ShortGuidText(entry.beforeWords[3])},
                    {"change3After", ShortGuidText(entry.afterWords[3])},
                    {"executionMask", std::to_string(entry.executionMask), true},
                    {"firstDt", std::to_string(entry.firstDeltaMs), true},
                    {"lastDt", std::to_string(entry.lastDeltaMs), true},
                });
        }
    }
    if (diagnostics_ != nullptr &&
        (compositeTrace.hookInstalled || compositeTrace.armed ||
         compositeTrace.installFailures != 0U || compositeTrace.entryCount != 0U)) {
        diagnostics_->Log(
            "info",
            "worldsync_fix321_composite_state_trace_summary",
            "Fix3.21 bounded generic CATHODE composite/state-value trace snapshot; observation only.",
            {
                {"hook", std::to_string(compositeTrace.hookInstalled ? 1U : 0U), true},
                {"armed", std::to_string(compositeTrace.armed ? 1U : 0U), true},
                {"active", std::to_string(compositeTrace.active ? 1U : 0U), true},
                {"redirects", std::to_string(compositeTrace.redirectCallSites), true},
                {"installFailures", std::to_string(compositeTrace.installFailures), true},
                {"inspected", std::to_string(compositeTrace.inspected), true},
                {"accepted", std::to_string(compositeTrace.accepted), true},
                {"unique", std::to_string(compositeTrace.uniqueEntries), true},
                {"drops", std::to_string(compositeTrace.capacityDrops), true},
                {"readFailures", std::to_string(compositeTrace.readFailures), true},
                {"limitDrops", std::to_string(compositeTrace.limitDrops), true},
                {"originTickMs", std::to_string(compositeTrace.originTickMs), true},
                {"entryCount", std::to_string(compositeTrace.entryCount), true},
                {"capacity", std::to_string(world_interactions::kFix321CompositeStateTraceMaximumEntries), true},
            });
        for (std::size_t i = 0U; i < compositeTrace.entryCount; ++i) {
            const auto& entry = compositeTrace.entries[i];
            diagnostics_->Log(
                "info",
                "worldsync_fix321_composite_state_trace_entry",
                "Normalized Fix3.21 state/value publication observation; no gameplay semantics assigned.",
                {
                    {"index", std::to_string(i), true},
                    {"sig", std::to_string(entry.signatureHash), true},
                    {"count", std::to_string(entry.count), true},
                    {"helperRva", ShortGuidText(entry.helperRva)},
                    {"callerRva", ShortGuidText(entry.callerRva)},
                    {"runtimeKey", ShortGuidText(entry.runtimeKey.entityA) + "/" +
                        ShortGuidText(entry.runtimeKey.entityB)},
                    {"worldId", WorldEntityIdText(entry.worldEntity)},
                    {"vtableRva", ShortGuidText(entry.executionVtableRva)},
                    {"identityMask", std::to_string(entry.identityMask), true},
                    {"valueReadMask", std::to_string(entry.valueReadMask), true},
                    {"valueWord0", ShortGuidText(entry.valueWord0)},
                    {"contextReadMask", std::to_string(entry.contextReadMask), true},
                    {"contextWord0", ShortGuidText(entry.contextWord0)},
                    {"contextWord1", ShortGuidText(entry.contextWord1)},
                    {"objectReadMask", std::to_string(entry.objectReadMask), true},
                    {"objectWord0", ShortGuidText(entry.objectWord0)},
                    {"objectWord1", ShortGuidText(entry.objectWord1)},
                    {"executionMask", std::to_string(entry.executionMask), true},
                    {"firstDt", std::to_string(entry.firstDeltaMs), true},
                    {"lastDt", std::to_string(entry.lastDeltaMs), true},
                });
        }
    }
    result.executed = stats.initialized;
    if (!result.executed) {
        result.statusCode = "WORLD_INTERACTION_STATS_UNAVAILABLE";
    } else if (fullStateDiff.phase == 10U) {
        result.statusCode = "WORLD_INTERACTION_STATS_BASELINE_CAPTURE_STARTED";
    } else if (fullStateDiff.phase == 11U) {
        result.statusCode = "WORLD_INTERACTION_STATS_AFTER_CAPTURE_STARTED";
    } else if (fullStateDiff.phase == 2U) {
        result.statusCode = "WORLD_INTERACTION_STATS_DIFF_READY";
    } else if (fullStateDiff.phase == 1U) {
        result.statusCode = "WORLD_INTERACTION_STATS_BASELINE_READY";
    } else {
        result.statusCode = "WORLD_INTERACTION_STATS_COMPLETE";
    }
    result.message =
        std::string("fs23Async=1 fs23Phase=") + std::to_string(fullStateDiff.phase) +
        " fs23Gen=" + std::to_string(fullStateDiff.levelGeneration) +
        " fs23BaseProviders=" + std::to_string(fullStateDiff.baseline.providerEntries) +
        " fs23BaseCaptured=" + std::to_string(fullStateDiff.baseline.capturedEntries) +
        " fs23BaseReadFail=" + std::to_string(
            fullStateDiff.baseline.objectReadFailures +
            fullStateDiff.baseline.pointerProbeReadFailures) +
        " fs23BasePartial=" + std::to_string(
            fullStateDiff.baseline.objectPartialReads +
            fullStateDiff.baseline.pointerProbePartialReads) +
        " fs23BaseDrop=" + std::to_string(fullStateDiff.baseline.capacityDrops) +
        " fs23AfterProviders=" + std::to_string(fullStateDiff.after.providerEntries) +
        " fs23AfterCaptured=" + std::to_string(fullStateDiff.after.capturedEntries) +
        " fs23AfterReadFail=" + std::to_string(
            fullStateDiff.after.objectReadFailures +
            fullStateDiff.after.pointerProbeReadFailures) +
        " fs23AfterPartial=" + std::to_string(
            fullStateDiff.after.objectPartialReads +
            fullStateDiff.after.pointerProbePartialReads) +
        " fs23AfterDrop=" + std::to_string(fullStateDiff.after.capacityDrops) +
        " fs23Modified=" + std::to_string(fullStateDiff.modified) +
        " fs23Appeared=" + std::to_string(fullStateDiff.appeared) +
        " fs23Disappeared=" + std::to_string(fullStateDiff.disappeared) +
        " fs23Delta=" + std::to_string(fullStateDiff.deltaEntries) +
        " runtimeKeyTransport=1 runtimeKeySafety=1" +
        // Fix3.24 BuildFix01 permanent functional-path health counters. They
        // are always collected and require no separately armed probe.
        " ci24Map=" + std::to_string(stats.compositePublicInputMapped) +
        " ci24Send=" + std::to_string(stats.compositePublicInputSubmitted) +
        " ci24Recv=" + std::to_string(stats.compositePublicInputRemoteAttempts) +
        " ci24Queue=" + std::to_string(stats.compositePublicInputBridgeQueued) +
        " ci24SrcFallback=" + std::to_string(stats.compositePublicInputSourceFallbacks) +
        " ci24Pending=" + std::to_string(stats.compositePublicInputRuntimeUnavailable) +
        " ci24Reject=" + std::to_string(stats.compositePublicInputRejected) +
        // Fix3.22 Composite execution snapshot is diagnostic-only and never
        // enables transport/replay.
        " cs22Hook=" + std::to_string(compositeDispatchTrace.hookInstalled ? 1U : 0U) +
        " cs22Armed=" + std::to_string(compositeDispatchTrace.armed ? 1U : 0U) +
        " cs22Active=" + std::to_string(compositeDispatchTrace.active ? 1U : 0U) +
        " cs22Calls=" + std::to_string(compositeDispatchTrace.redirectCallSites) +
        " cs22InstallFail=" + std::to_string(compositeDispatchTrace.installFailures) +
        " cs22Seen=" + std::to_string(compositeDispatchTrace.inspected) +
        " cs22Accepted=" + std::to_string(compositeDispatchTrace.accepted) +
        " cs22Unique=" + std::to_string(compositeDispatchTrace.uniqueEntries) +
        " cs22Changed=" + std::to_string(compositeDispatchTrace.changedCalls) +
        " cs22Drop=" + std::to_string(compositeDispatchTrace.capacityDrops) +
        " cs22ReadFail=" + std::to_string(compositeDispatchTrace.readFailures) +
        " cs22Limit=" + std::to_string(compositeDispatchTrace.limitDrops) +
        // Fix3.21 recorder is retained unchanged as an already-eliminated
        // observation family for regression evidence.
        " cs21Hook=" + std::to_string(compositeTrace.hookInstalled ? 1U : 0U) +
        " cs21Armed=" + std::to_string(compositeTrace.armed ? 1U : 0U) +
        " cs21Active=" + std::to_string(compositeTrace.active ? 1U : 0U) +
        " cs21Calls=" + std::to_string(compositeTrace.redirectCallSites) +
        " cs21InstallFail=" + std::to_string(compositeTrace.installFailures) +
        " cs21Seen=" + std::to_string(compositeTrace.inspected) +
        " cs21Accepted=" + std::to_string(compositeTrace.accepted) +
        " cs21Unique=" + std::to_string(compositeTrace.uniqueEntries) +
        " cs21Drop=" + std::to_string(compositeTrace.capacityDrops) +
        " cs21ReadFail=" + std::to_string(compositeTrace.readFailures) +
        " cs21Limit=" + std::to_string(compositeTrace.limitDrops) +
        // Fix3.20 experimental replay counters first so the bounded IPC result
        // always exposes whether the missing delayed family was sent/replayed.
        " cb20Armed=" + std::to_string(stats.characterCallbackReplayActive ? 1U : 0U) +
        " cb20Observed=" + std::to_string(stats.characterCallbackReplayObserved) +
        " cb20Portable=" + std::to_string(stats.characterCallbackReplayPortable) +
        " cb20ParentSkip=" + std::to_string(stats.characterCallbackReplayParentSuppressed) +
        " cb20NestedSkip=" + std::to_string(stats.characterCallbackReplayNestedSuppressed) +
        " cb20Submitted=" + std::to_string(stats.characterCallbackReplaySubmitted) +
        " cb20MatchSkip=" + std::to_string(stats.characterCallbackReplayContentSuppressed) +
        " cb20Exec=" + std::to_string(stats.characterCallbackReplayExecuteAttempts) +
        " cb20Queued=" + std::to_string(stats.characterCallbackReplayQueued) +
        " cb20Pending=" + std::to_string(stats.characterCallbackReplayRuntimeUnavailable) +
        " cb20Fault=" + std::to_string(stats.characterCallbackReplayNativeFaults) +
        " cb20Invalid=" + std::to_string(stats.characterCallbackReplayInvalid) +
        " cb20Limit=" + std::to_string(stats.characterCallbackReplayLimitDrops) +
        // Fix3.19 causal differential summary follows. Full normalized entries
        // are written to native diagnostics by this command, avoiding IPC
        // result-size pressure while requiring no additional operator command.
        " diffArmed=" + std::to_string(causalTrace.armed ? 1U : 0U) +
        " diffActive=" + std::to_string(causalTrace.active ? 1U : 0U) +
        " diffRole=" + std::to_string(causalTrace.anchorRole) +
        " diffAnchors=" + std::to_string(causalTrace.anchors) +
        " diffSeen=" + std::to_string(causalTrace.inspected) +
        " diffAccepted=" + std::to_string(causalTrace.accepted) +
        " diffUnique=" + std::to_string(causalTrace.uniqueEntries) +
        " diffCapacity=" + std::to_string(world_interactions::kFix319CausalTraceMaximumEntries) +
        " diffDrop=" + std::to_string(causalTrace.capacityDrops) +
        " diffReadFail=" + std::to_string(causalTrace.readFailures) +
        " diffWindow=" + std::to_string(causalTrace.windowMs) +
        " diffOrigin=" + std::to_string(causalTrace.originTickMs) +
        // Keep Fix3.18 code provenance next: GAME_COMMAND_RESULT is
        // intentionally bounded to 1000 UTF-8 bytes.
        " clearCodeArmed=" + std::to_string(stats.clearCodeProbeActive ? 1U : 0U) +
        " clearCodeCand=" + std::to_string(stats.clearCodeCandidates) +
        " clearCodeExact=" + std::to_string(stats.clearCodeExactMatches) +
        " clearCodeReadFail=" + std::to_string(stats.clearCodeReadFailures) +
        " clearCodeTarget=" + ShortGuidText(stats.clearCodeTargetRva) +
        " clearCodeWord0=" + ShortGuidText(stats.clearCodeWord0) +
        " clearCodeWord1=" + ShortGuidText(stats.clearCodeWord1) +
        " clearCodeKind=" + std::to_string(stats.clearCodeBranchKind) +
        " clearCodeBranch=" + ShortGuidText(stats.clearCodeBranchTargetRva) +
        " clearCodeFinal=" + ShortGuidText(stats.clearCodeFinalTargetRva) +
        " clearCodeDepth=" + std::to_string(stats.clearCodeJumpDepth) +
        " clearCodeHashHi=" + ShortGuidText(static_cast<std::uint32_t>(stats.clearCodeHash >> 32U)) +
        " clearCodeHashLo=" + ShortGuidText(static_cast<std::uint32_t>(stats.clearCodeHash & 0xFFFFFFFFULL)) +
        " clearCodeTid=" + std::to_string(stats.clearCodeThreadId) +
        " clearCodeTick=" + std::to_string(stats.clearCodeTickMs) +
        // Fix3.17 dispatch provenance follows.
        " clearDispArmed=" + std::to_string(stats.clearDispatchProbeActive ? 1U : 0U) +
        " clearDispCand=" + std::to_string(stats.clearDispatchCandidates) +
        " clearDispExact=" + std::to_string(stats.clearDispatchExactMatches) +
        " clearDispReadFail=" + std::to_string(stats.clearDispatchReadFailures) +
        " clearDispVtable=" + ShortGuidText(stats.clearDispatchVtableRva) +
        " clearDispSlot=" + ShortGuidText(stats.clearDispatchSlot) +
        " clearDispTarget=" + ShortGuidText(stats.clearDispatchTargetRva) +
        " clearDispFirst=" + ShortGuidText(stats.clearDispatchFirstTargetRva) +
        " clearDispLast=" + ShortGuidText(stats.clearDispatchLastTargetRva) +
        " clearDispVar=" + std::to_string(stats.clearDispatchTargetVaried ? 1U : 0U) +
        " clearDispBefore=" + std::to_string(stats.clearDispatchBeforeState) +
        " clearDispAfter=" + std::to_string(stats.clearDispatchAfterState) +
        " clearDispTid=" + std::to_string(stats.clearDispatchThreadId) +
        " clearDispTick=" + std::to_string(stats.clearDispatchTickMs) +
        // Fix3.16 post-CLEAR fields follow.
        " postWindow=" + std::to_string(stats.postClearWindowActive ? 1U : 0U) +
        " postArms=" + std::to_string(stats.postClearArms) +
        " postSeen=" + std::to_string(stats.postClearSeen) +
        " postAnchor=" + std::to_string(stats.postClearAnchorMatches) +
        " postValid=" + std::to_string(stats.postClearValidEdges) +
        " postCb=" + std::to_string(stats.postClearCallbackEdges) +
        " postReadFail=" + std::to_string(stats.postClearReadFailures) +
        " postLimit=" + std::to_string(stats.postClearInspectLimitDrops) +
        " postRole=" + ShortGuidText(stats.postClearAnchorRole) +
        " postWorld=" + ShortGuidText(stats.postClearWorldMask) +
        " postSrc=" + ShortGuidText(stats.postClearSourceKey.entityA) + "/" +
            ShortGuidText(stats.postClearSourceKey.entityB) +
        " postDst=" + ShortGuidText(stats.postClearTargetKey.entityA) + "/" +
            ShortGuidText(stats.postClearTargetKey.entityB) +
        " postSched=" + ShortGuidText(stats.postClearSchedulerParameter) +
        " postInput=" + ShortGuidText(stats.postClearTargetInput) +
        " postCbFlags=" + ShortGuidText(stats.postClearCallbackFlags) +
        " postFirstCall=" + ShortGuidText(stats.postClearFirstCallSiteRva) +
        " postLastCall=" + ShortGuidText(stats.postClearLastCallSiteRva) +
        " postCallVar=" + std::to_string(stats.postClearCallSiteVaried ? 1U : 0U) +
        " postFirstDt=" + std::to_string(stats.postClearFirstDeltaMs) +
        " postLastDt=" + std::to_string(stats.postClearLastDeltaMs) +
        " postHashHi=" + ShortGuidText(static_cast<std::uint32_t>(stats.postClearSequenceHash >> 32U)) +
        " postHashLo=" + ShortGuidText(static_cast<std::uint32_t>(stats.postClearSequenceHash)) +
        " postTid=" + std::to_string(stats.postClearThreadId) +
        " postTick=" + std::to_string(stats.postClearTickMs) +
        // Keep Fix3.15 synchronous downstream fields next.
        " downArmed=" + std::to_string(stats.logicDownstreamProbeActive ? 1U : 0U) +
        " downScopes=" + std::to_string(stats.logicDownstreamScopes) +
        " downEdges=" + std::to_string(stats.logicDownstreamEdges) +
        " downValid=" + std::to_string(stats.logicDownstreamValidEdges) +
        " downCb=" + std::to_string(stats.logicDownstreamCallbackEdges) +
        " downReadFail=" + std::to_string(stats.logicDownstreamReadFailures) +
        " downWorld=" + ShortGuidText(stats.logicDownstreamWorldMask) +
        " downSrc=" + ShortGuidText(stats.logicDownstreamSourceKey.entityA) + "/" +
            ShortGuidText(stats.logicDownstreamSourceKey.entityB) +
        " downDst=" + ShortGuidText(stats.logicDownstreamTargetKey.entityA) + "/" +
            ShortGuidText(stats.logicDownstreamTargetKey.entityB) +
        " downSched=" + ShortGuidText(stats.logicDownstreamSchedulerParameter) +
        " downInput=" + ShortGuidText(stats.logicDownstreamTargetInput) +
        " downCbFlags=" + ShortGuidText(stats.logicDownstreamCallbackFlags) +
        " downFirstCall=" + ShortGuidText(stats.logicDownstreamFirstCallSiteRva) +
        " downLastCall=" + ShortGuidText(stats.logicDownstreamLastCallSiteRva) +
        " downCallVar=" + std::to_string(stats.logicDownstreamCallSiteVaried ? 1U : 0U) +
        " downHashHi=" + ShortGuidText(static_cast<std::uint32_t>(stats.logicDownstreamSequenceHash >> 32U)) +
        " downHashLo=" + ShortGuidText(static_cast<std::uint32_t>(stats.logicDownstreamSequenceHash)) +
        " downTid=" + std::to_string(stats.logicDownstreamThreadId) +
        " downTick=" + std::to_string(stats.logicDownstreamTickMs) +
        // Fix3.14 remote identity fields follow.
        // intentionally bounded to 1000 UTF-8 bytes.
        " remoteTypedArmed=" + std::to_string(stats.typedRemoteIdentityProbeActive ? 1U : 0U) +
        " remoteTypedCand=" + std::to_string(stats.typedRemoteIdentityCandidates) +
        " remoteTypedExact=" + std::to_string(stats.typedRemoteIdentityExactMatches) +
        " remoteTypedMask=" + ShortGuidText(stats.typedRemoteIdentityMask) +
        " remoteTypedType=" + ShortGuidText(stats.typedRemoteIdentityMessageType) +
        " remoteTypedBytes=" + std::to_string(stats.typedRemoteIdentityPayloadBytes) +
        " remoteTypedSrcId=" + WorldEntityIdText(stats.typedRemoteIdentitySourceWorld) +
        " remoteTypedDstId=" + WorldEntityIdText(stats.typedRemoteIdentityTargetWorld) +
        " remoteTypedHostSrc=" + ShortGuidText(stats.typedRemoteIdentitySenderSourceKey.entityA) + "/" +
            ShortGuidText(stats.typedRemoteIdentitySenderSourceKey.entityB) +
        " remoteTypedHostDst=" + ShortGuidText(stats.typedRemoteIdentitySenderTargetKey.entityA) + "/" +
            ShortGuidText(stats.typedRemoteIdentitySenderTargetKey.entityB) +
        " remoteTypedLocalSrc=" + ShortGuidText(stats.typedRemoteIdentityLocalSourceKey.entityA) + "/" +
            ShortGuidText(stats.typedRemoteIdentityLocalSourceKey.entityB) +
        " remoteTypedLocalDst=" + ShortGuidText(stats.typedRemoteIdentityLocalTargetKey.entityA) + "/" +
            ShortGuidText(stats.typedRemoteIdentityLocalTargetKey.entityB) +
        " remoteTypedExecN=" + std::to_string(stats.typedRemoteIdentityExecuteReached) +
        " remoteTypedQueued=" + std::to_string(stats.typedRemoteIdentityQueued) +
        " remoteTypedPending=" + std::to_string(stats.typedRemoteIdentityRuntimeUnavailable) +
        " remoteTypedFault=" + std::to_string(stats.typedRemoteIdentityNativeFaults) +
        " remoteTypedInvalid=" + std::to_string(stats.typedRemoteIdentityInvalidPayloads) +
        // Fix3.13 provenance follows for Host-side cross-checking.
        " typedOriginArmed=" + std::to_string(stats.typedMessageOriginProbeActive ? 1U : 0U) +
        " typedOriginCand=" + std::to_string(stats.typedMessageOriginCandidates) +
        " typedOriginExact=" + std::to_string(stats.typedMessageOriginExactMatches) +
        " typedOriginCallN=" + std::to_string(stats.typedMessageOriginCallVerified) +
        " typedOriginReadFail=" + std::to_string(stats.typedMessageOriginReadFailures) +
        " typedOriginType=" + ShortGuidText(stats.typedMessageOriginMessageType) +
        " typedOriginBytes=" + std::to_string(stats.typedMessageOriginPayloadBytes) +
        " typedOriginSrc=" + ShortGuidText(stats.typedMessageOriginSourceKey.entityA) + "/" +
            ShortGuidText(stats.typedMessageOriginSourceKey.entityB) +
        " typedOriginDst=" + ShortGuidText(stats.typedMessageOriginTargetKey.entityA) + "/" +
            ShortGuidText(stats.typedMessageOriginTargetKey.entityB) +
        " typedOriginWorldMask=" + ShortGuidText(stats.typedMessageOriginWorldMask) +
        " typedOriginDelay=" + std::to_string(stats.typedMessageOriginDelaySeconds) +
        " typedOriginFirstCall=" + ShortGuidText(stats.typedMessageOriginFirstCallSiteRva) +
        " typedOriginLastCall=" + ShortGuidText(stats.typedMessageOriginLastCallSiteRva) +
        " typedOriginCallVar=" + std::to_string(stats.typedMessageOriginCallSiteVaried ? 1U : 0U) +
        " typedOriginTid=" + std::to_string(stats.typedMessageOriginThreadId) +
        " typedOriginTick=" + std::to_string(stats.typedMessageOriginTickMs) +
        " charOriginHook=" + std::to_string(stats.characterCallbackOriginHookInstalled ? 1U : 0U) +
        " charOriginArmed=" + std::to_string(stats.characterCallbackOriginProbeActive ? 1U : 0U) +
        " charOriginCand=" + std::to_string(stats.characterCallbackOriginCandidates) +
        " charOriginExact=" + std::to_string(stats.characterCallbackOriginExactKeyMatches) +
        " charOriginCallN=" + std::to_string(stats.characterCallbackOriginCallVerified) +
        " charOriginReadFail=" + std::to_string(stats.characterCallbackOriginReadFailures) +
        " charOriginKey=" + ShortGuidText(stats.characterCallbackOriginSourceKey.entityA) + "/" +
            ShortGuidText(stats.characterCallbackOriginSourceKey.entityB) +
        " charOriginExecRva=" + ShortGuidText(stats.characterCallbackOriginExecutionVtableRva) +
        " charOriginFlags=" + ShortGuidText(stats.characterCallbackOriginFlags) +
        " charOriginBytes=" + std::to_string(stats.characterCallbackOriginPayloadBytes) +
        " charOriginFirstCall=" + ShortGuidText(stats.characterCallbackOriginFirstCallSiteRva) +
        " charOriginLastCall=" + ShortGuidText(stats.characterCallbackOriginLastCallSiteRva) +
        " charOriginCallVar=" + std::to_string(stats.characterCallbackOriginCallSiteVaried ? 1U : 0U) +
        " charOriginTid=" + std::to_string(stats.characterCallbackOriginThreadId) +
        " charOriginTick=" + std::to_string(stats.characterCallbackOriginTickMs) +
        " typedHook=" + std::to_string(stats.typedMessageHookInstalled ? 1U : 0U) +
        " typedObserved=" + std::to_string(stats.observedTypedMessages) +
        " typedRelevant=" + std::to_string(stats.relevantTypedMessages) +
        " typedSubmitted=" + std::to_string(stats.submittedTypedMessages) +
        " typedPayloadBytes=" + std::to_string(stats.typedPayloadBytes) +
        " typedSrcNoWorld=" + std::to_string(stats.unresolvedTypedSource) +
        " typedDstNoWorld=" + std::to_string(stats.unresolvedTypedTarget) +
        " typedAnchorTarget=" + std::to_string(stats.typedRuntimeKeyAnchoredByTarget) +
        " typedAnchorSource=" + std::to_string(stats.typedRuntimeKeyAnchoredBySource) +
        " typedNoAnchor=" + std::to_string(stats.typedRuntimeKeyNoAnchorSkipped) +
        " typedSafetyBlocked=" + std::to_string(stats.typedRuntimeKeySafetyBlocked) +
        " typedBlockedType=" + ShortGuidText(stats.typedBlockedMessageType) +
        " typedBlockedBytes=" + std::to_string(stats.typedBlockedPayloadBytes) +
        " typedBlockedMissing=" + ShortGuidText(stats.typedBlockedMissingMask) +
        " typedBlockedSrc=" + ShortGuidText(stats.typedBlockedSourceKey.entityA) + "/" +
            ShortGuidText(stats.typedBlockedSourceKey.entityB) +
        " typedBlockedDst=" + ShortGuidText(stats.typedBlockedTargetKey.entityA) + "/" +
            ShortGuidText(stats.typedBlockedTargetKey.entityB) +
        " typedRemoteQueued=" + std::to_string(stats.queuedRemoteTypedMessages) +
        " typedRemoteSuppressed=" + std::to_string(stats.typedRemoteApplySuppressed) +
        " typedEchoSuppressed=" + std::to_string(stats.typedContentMatchSuppressed) +
        " typedQueueRejected=" + std::to_string(stats.typedIntentQueueRejected) +
        " typedInvalid=" + std::to_string(stats.invalidTypedMessagesSkipped) +
        " typedOversize=" + std::to_string(stats.oversizedTypedMessagesSkipped) +
        " typedNativeFaults=" + std::to_string(stats.typedNativeFaults) +
        " logicObserved=" + std::to_string(stats.logicSwitchObserved) +
        " logicWorld=" + std::to_string(stats.logicSwitchWorldStates) +
        " logicSubmitted=" + std::to_string(stats.logicSwitchSubmitted) +
        " logicUnresolved=" + std::to_string(stats.logicSwitchUnresolved) +
        " logicRemoteApplied=" + std::to_string(stats.logicSwitchAppliedRemote) +
        " logicRuntimePending=" + std::to_string(stats.logicSwitchRuntimeUnavailable) +
        " logicContextReplay=" + std::to_string(stats.logicSwitchContextReplays) +
        " logicSafetyBlocked=" + std::to_string(stats.logicSwitchSafetyBlocked) +
        " logicContextWorld=" + std::to_string(stats.logicSwitchContextWorld) +
        " logicContextNonWorld=" + std::to_string(stats.logicSwitchContextNonWorld) +
        " logicLastContext=" + ShortGuidText(stats.logicSwitchLastContextKey.entityA) + "/" +
            ShortGuidText(stats.logicSwitchLastContextKey.entityB) +
        " logicLastStateKey=" + ShortGuidText(stats.logicSwitchLastStateKey.entityA) + "/" +
            ShortGuidText(stats.logicSwitchLastStateKey.entityB) +
        " logicLastEnabled=" + std::to_string(stats.logicSwitchLastEnabled ? 1U : 0U) +
        " logicExactMatch=" + std::to_string(stats.logicExactTargetMatches) +
        " logicExactHolder=" + std::to_string(stats.logicExactHolderReadable) +
        " logicExactCb=" + std::to_string(stats.logicExactCallbackPresent) +
        " logicExactRaw=" + std::to_string(stats.logicExactRawReadable) +
        " logicExactReadFail=" + std::to_string(stats.logicExactRawReadFailures) +
        " logicExactObj=" + std::to_string(stats.logicExactCallbackObjectPresent) +
        " logicExactKeyN=" + std::to_string(stats.logicExactCallbackKeyResolved) +
        " logicExactExecN=" + std::to_string(stats.logicExactCallbackExecutionReadable) +
        " logicExactInput=" + ShortGuidText(stats.logicExactInput) +
        " logicExactFlags=" + ShortGuidText(stats.logicExactFlags) +
        " logicExactRef=" + std::to_string(stats.logicExactRefCount) +
        " logicExactNested=" + std::to_string(stats.logicExactNestedPresent ? 1U : 0U) +
        " logicExactPayload=" + std::to_string(stats.logicExactPayloadPresent ? 1U : 0U) +
        " logicExactCbKey=" + ShortGuidText(stats.logicExactCallbackKey.entityA) + "/" +
            ShortGuidText(stats.logicExactCallbackKey.entityB) +
        " logicExactExecRva=" + ShortGuidText(stats.logicExactCallbackExecutionVtableRva) +
        " logicExactSelfCtx=" + std::to_string(stats.logicExactContextSameAsState ? 1U : 0U) +
        " logicExactEnabled=" + std::to_string(stats.logicExactEnabled ? 1U : 0U) +
        " logicNativeFaults=" + std::to_string(stats.logicSwitchNativeFaults) +
        " callbackReplay=0" +
        " callbackSkipped=" + std::to_string(stats.callbackEdgesSkipped) +
        " inputNativeFaults=" + std::to_string(stats.nativeFaults) +
        " activeDetours=" + std::to_string(stats.activeDetours) +
        " ws3DoorSemanticHooks=" + std::to_string(legacy.doorHooksInstalled ? 1U : 0U) +
        " ws3DoorSemanticCalls=" + std::to_string(legacy.doorInputCalls) +
        " ws3DoorSemanticSent=" + std::to_string(legacy.doorObservedActions) +
        " ws3DoorSemanticUnresolved=" + std::to_string(legacy.doorUnresolved) +
        " ws3DoorSemanticRemoteSuppressed=" + std::to_string(legacy.doorRemoteSuppressed) +
        " ws3DoorSemanticRemoteQueued=" + std::to_string(legacy.queuedRemoteActions);
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteSpawnCharacter(
    const SpawnCharacterCommand& command) const {
    GameCommandExecutionResult result{};
    result.statusCode = "SPAWN_CHARACTER_BLOCKED";
    result.message = "La commande SpawnCharacter n'est pas disponible.";
    if (!SpawnAvailable()) {
        return result;
    }
    if (!spawnTracker_->HookInstalled()) {
        result.statusCode = "SPAWN_CHARACTER_TRACKER_UNAVAILABLE";
        result.message =
            "Le hook CharacterCreate requis pour récupérer CharacterBase n'est pas installé.";
        return result;
    }
    if (!IsValidCoordinate(command.x) ||
        !IsValidCoordinate(command.y) ||
        !IsValidCoordinate(command.z)) {
        result.statusCode = "SPAWN_CHARACTER_INVALID_ARGUMENTS";
        result.message = "Les coordonnées SpawnCharacter sont invalides.";
        return result;
    }

    const auto generation = generations_->CurrentGeneration();
    const auto captures = captures_->Snapshot();
    const auto civilian = std::find_if(
        captures.rbegin(),
        captures.rend(),
        [&](const CharacterRequestCapture& capture) {
            return capture.levelGeneration == generation &&
                capture.kind == CharacterRequestKind::Spawn130 &&
                capture.size == 0x130U &&
                capture.sink != 0U &&
                IsCivilianTemplate(capture.bytes);
        });

    std::array<std::byte, 0x130> payload{};
    void* sink = nullptr;
    std::uint32_t selectionBase = 1U;
    bool usedBootstrapTemplate = false;

    if (civilian != captures.rend()) {
        payload = civilian->bytes;
        sink = reinterpret_cast<void*>(civilian->sink);
        selectionBase =
            ReadValue<std::uint32_t>(payload, kSelectionOffset);
    } else {
        usedBootstrapTemplate = true;
        if (!PrepareBootstrapCivilianPayload(payload)) {
            result.statusCode = "SPAWN_CHARACTER_BOOTSTRAP_INVALID";
            result.message =
                "Le profil CIVILIAN intégré n'a pas pu être construit.";
            return result;
        }

        const auto runtime = runtimeEvidence_->Snapshot();
        if (runtime.generation == generation &&
            runtime.lastCharacterManager != 0U) {
            sink = reinterpret_cast<void*>(
                runtime.lastCharacterManager + kSpawn130SinkOffset);
        }

        if (sink == nullptr) {
            const auto carrier = std::find_if(
                captures.rbegin(),
                captures.rend(),
                [&](const CharacterRequestCapture& capture) {
                    return capture.levelGeneration == generation &&
                        capture.kind == CharacterRequestKind::Spawn130 &&
                        capture.size == 0x130U &&
                        capture.sink != 0U;
                });
            if (carrier != captures.rend()) {
                sink = reinterpret_cast<void*>(carrier->sink);
            }
        }

        const auto sinkVtable = sink == nullptr
            ? std::optional<void*>{}
            : crash_guard::SafeRead<void*>(sink);
        if (!sinkVtable.has_value() || *sinkVtable == nullptr) {
            result.statusCode = "SPAWN_CHARACTER_SINK_MISSING";
            result.message =
                "Le CharacterManager du niveau n'est pas encore disponible. "
                "Attendez que le niveau soit entièrement chargé puis réessayez.";
            return result;
        }
    }

    std::uint32_t spawnKey = 0;
    if (!TryNextNativeSpawnKey(
            characterRuntimeIdCounterAddress_,
            spawnKey)) {
        result.statusCode = "SPAWN_CHARACTER_ID_UNAVAILABLE";
        result.message = "Le compteur natif de Character n'est pas accessible.";
        return result;
    }

    const auto nonce = dynamicNonce_.fetch_add(1U, std::memory_order_relaxed);
    const auto entityKey = MixEntityKey(spawnKey, nonce);
    auto selection = selectionBase + nonce;
    if (selection == 0U) {
        selection = 1U;
    }

    if (!PrepareDynamicSpawnPayload(
            payload,
            spawnKey,
            entityKey,
            selection,
            command)) {
        result.statusCode = "SPAWN_CHARACTER_TEMPLATE_INVALID";
        result.message = "Le template CIVILIAN est incompatible.";
        return result;
    }

    spawnTracker_->Begin(generation, spawnKey);
    std::uint32_t callbackResult = 0;
    if (!InvokeSpawnRequest130(
            spawnRequest130ReceiverAddress_,
            sink,
            payload,
            callbackResult)) {
        spawnTracker_->Fail(generation, spawnKey);
        result.statusCode = "SPAWN_CHARACTER_NATIVE_FAULT";
        result.message =
            "La soumission native SpawnCharacter a déclenché une exception protégée.";
        return result;
    }

    result.executed = true;
    result.statusCode = usedBootstrapTemplate
        ? "SPAWN_CHARACTER_BOOTSTRAP_SUBMITTED"
        : "SPAWN_CHARACTER_SUBMITTED";
    if (usedBootstrapTemplate) {
        result.message = callbackResult == 0U
            ? "Requête CIVILIAN autonome soumise depuis le profil intégré. Validation visuelle requise."
            : "Requête CIVILIAN autonome acceptée par le callback natif.";
    } else {
        result.message = callbackResult == 0U
            ? "Requête dynamique CIVILIAN soumise au pipeline natif. Validation visuelle requise."
            : "Requête dynamique CIVILIAN acceptée par le callback natif.";
    }
    result.message += " Suivi automatique en attente pour spawnKey=" +
        std::to_string(spawnKey) + ".";
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteCharacterDirectMove(
    const CharacterDirectMoveCommand& command) const {
    GameCommandExecutionResult result{};
    result.statusCode = "CHARACTER_DIRECT_MOVE_BLOCKED";
    result.message =
        "La commande directionnelle CHARACTER_DIRECT_MOVE n'est pas disponible.";

    if (!CharacterDirectMoveAvailable()) {
        return result;
    }
    if (command.characterAddress == 0U ||
        !std::isfinite(command.directionX) ||
        !std::isfinite(command.directionZ) ||
        !std::isfinite(command.intensity) ||
        std::abs(command.directionX) >
            kMaximumCharacterDirectMoveDirectionMagnitude ||
        std::abs(command.directionZ) >
            kMaximumCharacterDirectMoveDirectionMagnitude ||
        command.directionX * command.directionX +
                command.directionZ * command.directionZ >
            kMaximumCharacterDirectMoveDirectionMagnitude *
                kMaximumCharacterDirectMoveDirectionMagnitude ||
        command.intensity < kMinimumCharacterDirectMoveIntensity ||
        command.intensity > kMaximumCharacterDirectMoveIntensity ||
        command.frameCount > kMaximumCharacterDirectMoveFrames ||
        (command.hasMoveType && command.moveType > 3U) ||
        (command.hasPlayerTorch && (!command.hasReplicationPosition ||
         !command.hasReplicationYaw || !command.hasMoveType || !command.worldSpace)) ||
        (command.hasPlayerTorchDirection && (!command.hasPlayerTorch || !command.playerTorchEnabled ||
         !player_torch::ValidDirection(command.playerTorchDirection))) ||
        (command.hasPlayerHiding && (!command.hasReplicationPosition ||
         !command.hasReplicationYaw || !command.hasMoveType || !command.worldSpace ||
         (command.playerHidden && !player_hiding::ValidAnchor(command.playerHidingAnchor)))) ||
        (command.hasPlayerHidingPresentation && (!command.hasPlayerHiding ||
         !player_hiding_presentation::Valid(command.playerHidingPresentation))) ||
        (command.hasPlayerAnimation && (!command.hasReplicationPosition ||
         !animation_sync::Valid(command.playerAnimation))) ||
        (command.hasPlayerPosture && (!command.hasReplicationPosition ||
         !command.hasReplicationYaw || !command.hasMoveType ||
         !player_posture::Valid(command.playerPosture))) ||
        (command.hasReplicationPosition &&
         (!command.hasReplicationYaw || !command.hasMoveType || !command.worldSpace ||
          !IsValidCoordinate(command.replicationPositionX) ||
          !IsValidCoordinate(command.replicationPositionY) ||
          !IsValidCoordinate(command.replicationPositionZ))) ||
        (command.hasReplicationYaw &&
         (!command.worldSpace || !std::isfinite(command.replicationYaw) ||
          command.replicationYaw < -3.3F || command.replicationYaw > 3.3F)) ||
        (command.frameCount != 0U &&
         command.directionX == 0.0F &&
         command.directionZ == 0.0F)) {
        result.statusCode = "CHARACTER_DIRECT_MOVE_INVALID_ARGUMENTS";
        result.message =
            "L'adresse, la direction X/Z, l'intensité, la position "
            "réseau ou le nombre de frames est invalide.";
        return result;
    }

    if (command.frameCount == 0U && command.hasReplicationYaw &&
        !IsValidCharacterObject(command.characterAddress)) {
        result.statusCode = "CHARACTER_DIRECT_MOVE_INVALID_CHARACTER";
        result.message =
            "L'adresse ne correspond pas à un objet Character valide de cette session.";
        return result;
    }

    // Admit movement first so rejected/stale Characters cannot allocate a
    // player-state mailbox. A full posture queue may retry without starving motion.
    const auto publishPlayerState = [&]() {
        if (command.hasPlayerTorch) {
            std::string torchError;
            if (!player_torch::Publish(command.characterAddress, command.playerTorchEnabled, torchError,
                    command.hasPlayerTorchDirection ? &command.playerTorchDirection : nullptr)) {
                result.statusCode = "PLAYER_TORCH_PUBLICATION_FAILED";
                result.message = torchError;
                return false;
            }
        }
        if (command.hasPlayerHiding) {
            std::string hidingError;
            if (!player_hiding::Publish(command.characterAddress, command.playerHidden,
                    command.playerHidingAnchor, hidingError,
                    command.hasPlayerHidingPresentation ? &command.playerHidingPresentation : nullptr)) {
                result.statusCode = "PLAYER_HIDING_PUBLICATION_FAILED";
                result.message = hidingError;
                return false;
            }
        }
        if (command.hasPlayerHidingPresentation) {
            std::string error;
            if (!player_hiding_presentation::Publish(command.characterAddress, command.playerHidingPresentation, error) ||
                !hiding_door_presentation::Publish(command.characterAddress, command.playerHidingPresentation, error)) {
                result.statusCode = "PLAYER_HIDING_PRESENTATION_FAILED";
                result.message = error;
                return false;
            }
        }
        if (command.hasPlayerAnimation) {
            std::string error;
              auto animation = command.playerAnimation;
              if (animation_sync::Active(animation)) {
                  // The movement carrier is newer than an IPC animation
                  // heartbeat: align to its current replicated transform.
                  animation.position = {command.replicationPositionX, command.replicationPositionY, command.replicationPositionZ};
                  animation.yaw = command.replicationYaw;
              }
              if (!animation_sync::Publish(command.characterAddress, animation, error)) {
                result.statusCode = "PLAYER_ANIMATION_PUBLICATION_FAILED";
                result.message = error;
                return false;
            }
        }
        if (!command.hasPlayerPosture) return true;
        std::string error;
        if (player_posture::Publish(command.characterAddress, command.playerPosture, error)) return true;
        result.statusCode = "PLAYER_POSTURE_PUBLICATION_FAILED";
        result.message = error;
        return false;
    };

    if (command.frameCount == 0U) {
        const auto holdNetworkIdle =
            command.hasReplicationPosition && command.hasReplicationYaw;
        if (holdNetworkIdle) {
            std::string idleError;
            if (!character_direct_step_runtime::HoldWorldReplicationIdle(
                    command.characterAddress,
                    command.replicationPositionX,
                    command.replicationPositionY,
                    command.replicationPositionZ,
                    command.replicationYaw,
                    command.moveType,
                    idleError)) {
                result.statusCode = "CHARACTER_DIRECT_MOVE_IDLE_HOLD_FAILED";
                result.message = idleError;
                return result;
            }
        } else if (!character_direct_step_runtime::Stop(command.characterAddress)) {
            result.statusCode = "CHARACTER_DIRECT_MOVE_STOP_FAILED";
            result.message =
                "La requête native de direction de ce Character n'a pas pu être remise à zéro.";
            return result;
        }
        if (command.hasReplicationYaw && !holdNetworkIdle) {
            std::string yawError;
            if (!character_direct_step_runtime::ApplyReplicatedYaw(
                    command.characterAddress, command.replicationYaw, yawError)) {
                result.statusCode = "CHARACTER_DIRECT_MOVE_YAW_FAILED";
                result.message = yawError;
                return result;
            }
        }
        if (!publishPlayerState()) return result;
        result.executed = true;
        result.statusCode = holdNetworkIdle
            ? "CHARACTER_DIRECT_MOVE_IDLE_HELD"
            : "CHARACTER_DIRECT_MOVE_STOPPED";
        if (command.hasReplicationPosition) {
            result.message =
                "Le Remote Player Controller conserve le Character en Idle natif; yaw et position restent publies au controleur CharacterManager sans ecriture cross-thread ni destination IA.";
        } else {
            result.message = command.hasReplicationYaw
                ? "La locomotion native a été arrêtée et le yaw répliqué a été appliqué."
                : "La locomotion native de ce Character a été arrêtée et libérée sans affecter les autres Characters répliqués.";
        }
        return result;
    }

    if (!IsValidCharacterObject(command.characterAddress)) {
        result.statusCode = "CHARACTER_DIRECT_MOVE_INVALID_CHARACTER";
        result.message =
            "L'adresse ne correspond pas à un objet Character valide de cette session.";
        return result;
    }

    std::string error;
    bool armed = false;
    if (command.worldSpace && command.hasReplicationPosition && command.hasReplicationYaw) {
        armed = character_direct_step_runtime::ArmWorldReplication(
            command.characterAddress,
            command.directionX,
            command.directionZ,
            command.intensity,
            command.replicationPositionX,
            command.replicationPositionY,
            command.replicationPositionZ,
            command.replicationYaw,
            command.moveType,
            command.frameCount,
            error);
    } else if (command.worldSpace) {
        armed = character_direct_step_runtime::ArmWorldDirection(
            command.characterAddress,
            command.directionX,
            command.directionZ,
            command.intensity,
            command.frameCount,
            error);
    } else {
        armed = character_direct_step_runtime::ArmDirection(
            command.characterAddress,
            command.directionX,
            command.directionZ,
            command.intensity,
            command.frameCount,
            error);
    }
    if (!armed) {
        result.statusCode = "CHARACTER_DIRECT_MOVE_ARM_FAILED";
        result.message = error;
        return result;
    }

    if (!publishPlayerState()) return result;
    result.executed = true;
    result.statusCode = "CHARACTER_DIRECT_MOVE_ARMED";
    result.message = command.hasReplicationPosition
        ? "Remote replication state published to the CharacterManager controller."
        : "Native directional locomotion state published.";
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteCharacterDirectStep(
    const CharacterDirectStepCommand& command) const {
    GameCommandExecutionResult result{};
    result.statusCode = "CHARACTER_DIRECT_STEP_BLOCKED";
    result.message =
        "La commande CHARACTER_DIRECT_STEP temps réel n'est pas disponible.";

    if (!CharacterDirectStepAvailable()) {
        return result;
    }
    if (command.characterAddress == 0U ||
        !std::isfinite(command.deltaX) ||
        !std::isfinite(command.deltaY) ||
        !std::isfinite(command.deltaZ) ||
        !std::isfinite(command.deltaYaw) ||
        std::abs(command.deltaX) > kMaximumCharacterDirectStep ||
        std::abs(command.deltaY) > kMaximumCharacterDirectStep ||
        std::abs(command.deltaZ) > kMaximumCharacterDirectStep ||
        std::abs(command.deltaYaw) > kMaximumCharacterDirectYawStep ||
        command.frameCount > kMaximumCharacterDirectStepFrames) {
        result.statusCode = "CHARACTER_DIRECT_STEP_INVALID_ARGUMENTS";
        result.message =
            "L'adresse, les vitesses ou le nombre de frames sont invalides.";
        return result;
    }

    if (command.frameCount == 0U) {
        if (!character_direct_step_runtime::Stop(command.characterAddress)) {
            result.statusCode = "CHARACTER_DIRECT_STEP_STOP_FAILED";
            result.message =
                "La requête native de direction de ce Character n'a pas pu être remise à zéro.";
            return result;
        }
        result.executed = true;
        result.statusCode = "CHARACTER_DIRECT_STEP_STOPPED";
        result.message =
            "La locomotion et les deltas physiques de ce Character ont été désarmés sans affecter les autres Characters répliqués.";
        return result;
    }

    if (!IsValidCharacterObject(command.characterAddress)) {
        result.statusCode = "CHARACTER_DIRECT_STEP_INVALID_CHARACTER";
        result.message =
            "L'adresse ne correspond pas à un objet Character valide de cette session.";
        return result;
    }

    std::string error;
    if (!character_direct_step_runtime::Arm(
            command.characterAddress,
            command.deltaX,
            command.deltaY,
            command.deltaZ,
            command.deltaYaw,
            command.frameCount,
            error)) {
        result.statusCode = "CHARACTER_DIRECT_STEP_ARM_FAILED";
        result.message = error;
        return result;
    }

    result.executed = true;
    result.statusCode = "CHARACTER_DIRECT_STEP_ARMED";
    result.message =
        "Le déplacement hybride est armé pendant " +
        std::to_string(command.frameCount) +
        " passages ActNPCLocomotionCycle : mod.main()+257900 réalise uniquement la première prise de contrôle, "
        "puis le hook post-IA +26D4D8 remplace les contrôles CIVILIAN avant que le cycle NPC actif avance l'animation et transmette l'entrée à mod.main()+28D6D0, "
        "puis les deltas physiques sont appliqués sur "
        "Character+0x1010/+0x1014/+0x1018/+0x1020. "
        "L'arrêt envoie ensuite une direction nulle; aucune destination IA n'est créée.";
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteCharacterAmmoRefill(
    const CharacterAmmoRefillCommand& command) const {
    if (!CharacterActionTestAvailable()) {
        return {false, "AMMO_REFILL_UNAVAILABLE",
            "Manual test refill requires the certified CharacterActionRuntime."};
    }
    const auto outcome = character_action_runtime::RefillTestAmmo(command.characterAddress);
    return {outcome.accepted, outcome.statusCode, outcome.message};
}

GameCommandExecutionResult GameCommandExecutor::ExecuteCharacterActionTest(
    const CharacterActionTestCommand& command) const {
    // This callback runs on the existing engine dispatcher. It publishes ONLY
    // AIMP input state: all native action application is at FUN_007CCFB0 entry.
    GameCommandExecutionResult result{};
    if (!CharacterActionTestAvailable()) {
        result.statusCode = "CHARACTER_ACTION_UNAVAILABLE";
        result.message = "CharacterActionRuntime is unavailable; check the exact-build hook installation result.";
        return result;
    }
    character_action_runtime::Result intent{};
    switch (command.action) {
    case CharacterActionKind::AimStart:
        intent = character_action_runtime::SetAim(command.characterAddress, true);
        break;
    case CharacterActionKind::AimStop:
        intent = character_action_runtime::SetAim(command.characterAddress, false);
        break;
    case CharacterActionKind::PrimaryStart:
        intent = character_action_runtime::SetPrimaryAction(command.characterAddress, true);
        break;
    case CharacterActionKind::PrimaryStop:
        intent = character_action_runtime::SetPrimaryAction(command.characterAddress, false);
        break;
    default:
        result.statusCode = "CHARACTER_ACTION_RETIRED";
        result.message = "Legacy Character Action probes are disabled. Use Aim START/STOP and Primary START/STOP.";
        return result;
    }
    result.executed = intent.accepted;
    result.statusCode = intent.statusCode;
    result.message = intent.message;
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteRemoteEquipmentProvision(
    const RemoteEquipmentProvisionCommand& command) const {
    GameCommandExecutionResult result{};
    if (!RemoteEquipmentProvisionAvailable()) {
        result.statusCode = "REMOTE_EQUIPMENT_BLOCKED";
        result.message = "The remote equipment provisioner is unavailable.";
        return result;
    }

    // A new equipment binding must never inherit a held action, including
    // when an allocator later reuses the same item address. Intent-only here;
    // the natural equipment update remains the sole native actuator boundary.
    static_cast<void>(character_action_runtime::SetAim(command.characterAddress, false));
    const auto provision = spawnTracker_->ProvisionRemoteEquipmentOnGameThread(
        command.characterAddress,
        command.compositeName,
        command.equipmentEntityShortGuid,
        command.equipmentOutputShortGuid,
        command.spawnSchedulerParameterShortGuid,
        command.spawnTargetInputShortGuid,
        command.equipmentSlot, command.activateHeld, command.releaseHeld);
    result.executed = provision.executed;
    // AvS round 10: remember which item class this survivor proxy now holds
    // (the Alien pilot's flame fear / kill block / shotgun stun read it).
    if (provision.executed)
        character_action_runtime::NoteRemoteWeapon(command.characterAddress, command.compositeName.c_str());
    if (!provision.executed && provision.retryable) {
        // LauncherIPC preserves REMOTE_EQUIPMENT_NOT_READY as a convergence result.
        // Keep the detailed native stage in the message for diagnostics.
        result.statusCode = "REMOTE_EQUIPMENT_NOT_READY";
        result.message = provision.statusCode + ": " + provision.message;
    } else {
        result.statusCode = provision.statusCode;
        result.message = provision.message;
    }
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteSpawnNetZCharacter(
    const SpawnNetZCharacterCommand& command) const {
    GameCommandExecutionResult result{};
    if (!SpawnAvailable()) {
        result.statusCode = "SPAWN_NETZ_CHARACTER_BLOCKED";
        result.message = "La commande SpawnNetZCharacter n'est pas disponible.";
        return result;
    }
    if (!IsValidCoordinate(command.x) ||
        !IsValidCoordinate(command.y) ||
        !IsValidCoordinate(command.z) ||
        command.participantKey == 0U ||
        command.participantKey >
            static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
        command.displayName.empty() ||
        !IsValidText(command.displayName, false)) {
        result.statusCode = "SPAWN_NETZ_CHARACTER_INVALID_ARGUMENTS";
        result.message =
            "Les paramètres SpawnNetZCharacter sont invalides.";
        return result;
    }

    // The live participant table cannot be made visible before the native
    // Character orchestrator reaches its participant lookup. Doing so was
    // observed to enter an incomplete multi-participant path and can tear down
    // the injected IPC session. The missing auxiliary resolver registration
    // must be restored before persistent count=2 mutation is safe.
    const auto spawn = ExecuteSpawnCharacter(
        SpawnCharacterCommand{command.x, command.y, command.z});
    if (!spawn.executed) {
        result.statusCode = "SPAWN_NETZ_FALLBACK_FAILED_" + spawn.statusCode;
        result.message =
            "Le spawn de secours a échoué. " + spawn.message;
        return result;
    }

    result.executed = true;
    result.statusCode = "SPAWN_NETZ_SAFE_FALLBACK_SUBMITTED";
    result.message =
        "Character créé par le pipeline natif sans modifier la table des "
        "participants. L'enregistrement NetZ persistant reste bloqué tant que "
        "le registre auxiliaire du participant distant n'est pas initialisé.";
    return result;
}


GameCommandExecutionResult GameCommandExecutor::ExecuteReturnToFrontend() const {
    GameCommandExecutionResult result{};
    if (!ReturnToFrontendAvailable()) {
        result.statusCode = "RETURN_TO_FRONTEND_UNAVAILABLE";
        result.message = "Return to frontend is not certified for this executable.";
        return result;
    }
    // Same G and mode guard as PlayerLifecycle: G = [GlobalStateRoot]+48,
    // gameplay mode at G+14 (0/3 = frontend or inactive), state at G+8.
    const auto root = crash_guard::SafeRead<std::uintptr_t>(globalStateRootAddress_);
    const auto manager = root && *root != 0U
        ? crash_guard::SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(*root + 0x48U))
        : std::optional<std::uintptr_t>{};
    const auto mode = manager && *manager != 0U
        ? crash_guard::SafeRead<std::int32_t>(reinterpret_cast<const void*>(*manager + 0x14U))
        : std::optional<std::int32_t>{};
    if (!mode) {
        result.statusCode = "RETURN_TO_FRONTEND_CONTEXT_UNAVAILABLE";
        result.message = "The gameplay manager is unreadable.";
        return result;
    }
    if (*mode == 0 || *mode == 3) {
        result.statusCode = "RETURN_TO_FRONTEND_NOT_IN_GAMEPLAY";
        result.message = "No gameplay level is active.";
        return result;
    }
    // Remote proxies are freed by the level unload. Drop every network
    // movement mailbox first so the engine-thread pump cannot write into
    // their memory while the frontend reuses the heap.
    // Retail can reuse all global root addresses on a same-map restart. Revoke
    // the old incarnation explicitly; pointer equality alone cannot preserve
    // caches or queued commands across this unload. Bootstrap owns invalidation
    // of every generation-bound registry on its next observation.
    if (generations_ != nullptr) static_cast<void>(generations_->BeginTransition());
    character_direct_step_runtime::ForgetAll();
    if (!InvokeReturnToFrontend(returnToFrontendAddress_, reinterpret_cast<void*>(*manager))) {
        result.statusCode = "RETURN_TO_FRONTEND_NATIVE_FAULT";
        result.message = "The native return-to-frontend call raised a protected exception.";
        return result;
    }
    result.executed = true;
    result.statusCode = "RETURN_TO_FRONTEND_REQUESTED";
    result.message = "Return to frontend requested from the gameplay manager.";
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteItemSyncSpawn(
    const ItemSyncSpawnCommand& command) const {
    GameCommandExecutionResult result{};
    item_sync::SpawnData data{};
    data.kind = command.drop ? item_sync::SpawnKind::Drop : item_sync::SpawnKind::Owner;
    data.ownerKey = command.ownerKey;
    data.spawn = command.spawn;
    if (command.path.size() >= sizeof(data.path)) {
        result.statusCode = "ITEM_SYNC_REJECTED";
        result.message = "Item path is too long.";
        return result;
    }
    std::memcpy(data.path, command.path.c_str(), command.path.size() + 1U);
    data.quantity = command.quantity;
    std::copy(command.transform.begin(), command.transform.end(), data.transform);
    const auto outcome = item_sync::ApplySpawnDecision(data);
    result.statusCode = item_sync::ApplyStatusCode(outcome);
    result.executed = outcome == item_sync::ApplyStatus::Applied ||
        outcome == item_sync::ApplyStatus::AlreadyApplied ||
        outcome == item_sync::ApplyStatus::Deferred;
    char ownerKey[17]{};
    std::snprintf(ownerKey, sizeof(ownerKey), "%016llX",
        static_cast<unsigned long long>(command.ownerKey));
    result.message = std::string(command.drop ? "Drop " : "Owner ") + ownerKey + ": " +
        (outcome == item_sync::ApplyStatus::Deferred
            ? "decision stored until the local owner runs."
            : outcome == item_sync::ApplyStatus::Applied
                ? (command.spawn ? "pickup spawn requested from the pickup manager."
                                 : "nothing spawns for this owner.")
                : outcome == item_sync::ApplyStatus::AlreadyApplied
                    ? "decision already applied."
                    : item_sync::ApplyStatusMessage(outcome));
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteMissionSync(
    const MissionSyncCommand& command, const bool configure) const {
    GameCommandExecutionResult result{};
    if (worldRegistry_ == nullptr) { result.statusCode = "MISSION_SYNC_NOT_READY"; return result; }
    if (configure) {
        result.executed = mission_sync::Configure(*worldRegistry_,
            std::filesystem::path(std::u8string(command.path.begin(), command.path.end())), command.role, result.message);
        result.statusCode = result.executed ? "MISSION_SYNC_CONFIGURED" : "MISSION_SYNC_NOT_READY";
    } else {
        const mission_sync::Signal signal{worldRegistry_->ReplicationContext().levelGeneration, command.sequence,
            WorldEntityId{command.entityHigh, command.entityLow}, command.signal, command.selector, command.occurrence};
        result.executed = mission_sync::Apply(signal, result.statusCode, result.message);
    }
    return result;
}

GameCommandExecutionResult GameCommandExecutor::ExecuteAlienPvPProbe() const {
    ai_sync::AlienPvPProbe probe{};
    if (!ai_sync::ProbeAlienPvPCandidate(probe))
        return {false, "ALIEN_PVP_NOT_READY",
            "Engine thread, AISync generation or Character registry is not ready."};
    if (!probe.unique)
        return {false, probe.candidates == 0U ? "ALIEN_PVP_NO_ALIEN" : "ALIEN_PVP_MULTIPLE_ALIENS",
            "Found " + std::to_string(probe.candidates) +
            " native Alien candidates; require exactly one before granting control."};
    return {true, "ALIEN_PVP_PROBE_READY",
        "Native Alien candidate verified: entityA=" + std::to_string(probe.entityA) +
        ", entityB=" + std::to_string(probe.entityB) +
        ", runtimeId=" + std::to_string(probe.runtimeId) +
        ", nativeGeneration=" + std::to_string(probe.nativeGeneration) +
        ", worldGeneration=" + std::to_string(probe.worldGeneration) +
        ". Read-only: no native control lease has been granted."};
}

GameCommandExecutionResult GameCommandExecutor::ExecuteAlienPvPBind(const std::uint64_t ownerPeer) const {
    ai_sync::AlienPvPLeaseStatus status{};
    const auto owned=ai_sync::AcquireAlienPvPLease(ownerPeer,status);
    std::string code;
    switch (status.phase) {
        case ai_sync::AlienPvPLeasePhase::Owned: code="ALIEN_PVP_OWNED";break;
        case ai_sync::AlienPvPLeasePhase::WaitingForAlien: code="ALIEN_PVP_WAITING_FOR_SPAWN";break;
        case ai_sync::AlienPvPLeasePhase::Ambiguous: code="ALIEN_PVP_MULTIPLE_ALIENS";break;
        case ai_sync::AlienPvPLeasePhase::AwaitingRetirement: code="ALIEN_PVP_OWNER_BUSY";break;
        case ai_sync::AlienPvPLeasePhase::Draining: code="ALIEN_PVP_DRAINING";break;
        case ai_sync::AlienPvPLeasePhase::Stale: code="ALIEN_PVP_STALE";break;
        default: code="ALIEN_PVP_NOT_READY";break;
    }
    return {owned,code,
        "Alien PvP native lease: ownerPeer="+std::to_string(status.ownerPeer)+
        " waitReason="+status.waitReason+
        " entityA="+std::to_string(status.entityA)+
        " entityB="+std::to_string(status.entityB)+
        " runtimeId="+std::to_string(status.runtimeId)+
        " boundaryOffset="+std::to_string(status.boundaryOffset)+
        " boundaryValue="+std::to_string(status.boundaryValue)+
        " retainedKind="+std::to_string(status.retainedKind)+
        " retainedStatus="+std::to_string(status.retainedStatus)+
        " retainedRetired="+std::to_string(status.retainedRetired)+
        " localOwner="+std::to_string(status.isLocalOwner?1:0)+
        " localPilotReady="+std::to_string(status.localPilotReady?1:0)+
        " pilotInputIsolated="+std::to_string(status.pilotInputIsolated?1:0)+
        " pilotCameraBound="+std::to_string(status.pilotCameraBound?1:0)+
        " pilotTechnicalHidden="+std::to_string(status.pilotTechnicalHidden?1:0)+
        " pilotMoving="+std::to_string(status.pilotMoving?1:0)+
        ". Owned means exclusive NPC authority, not verified local camera or locomotion. "+
        "The local pilot requires an isolated input path and a live camera target."};
}
GameCommandExecutionResult GameCommandExecutor::ExecuteAlienPvPRelease() const {
    ai_sync::AlienPvPLeaseStatus status{};
    const bool released=ai_sync::ReleaseAlienPvPLease(status);
    return {released,released?"ALIEN_PVP_RELEASED":"ALIEN_PVP_RELEASE_PENDING",
        released?"Alien PvP native lease released safely.":
            "Alien PvP release deferred while native requests are in flight."};
}
GameCommandExecutionResult GameCommandExecutor::ExecuteAlienPvPVents(std::uint32_t page,int goTo,std::uint64_t incarnation) const {
    std::string json;
    const bool ready=ai_sync::SnapshotAlienVents(page,json,goTo,incarnation);
    return {ready,ready?"ALIEN_PVP_VENTS_READY":"ALIEN_PVP_NOT_READY",ready?json:"Local Alien ventilation is not ready."};
}

GameCommandExecutionResult GameCommandExecutor::ExecuteItemSyncApply(
    const ItemSyncApplyCommand& command) const {
    GameCommandExecutionResult result{};
    const WorldEntityId entity{command.entityHigh, command.entityLow};
    const auto outcome = item_sync::ApplyRemoteCollected(entity);
    result.statusCode = item_sync::ApplyStatusCode(outcome);
    result.executed = outcome == item_sync::ApplyStatus::Applied ||
        outcome == item_sync::ApplyStatus::AlreadyApplied ||
        outcome == item_sync::ApplyStatus::Deferred;
    result.message = std::string(item_sync::ApplyStatusMessage(outcome)) + " Entity " +
        WorldEntityIdText(entity) + '.';
    return result;
}

} // namespace aimp
