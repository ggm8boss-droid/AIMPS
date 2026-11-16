#pragma once

#include "aimp/AnimationSync.h"
#include "aimp/AnimationSyncMovementPolicy.h"
#include "aimp/CharacterIncarnation.h"
#include <cstdint>
#include <string>

namespace aimp { class Diagnostics; class HookManager; }

namespace aimp::animation_sync {

// Pass CharacterDirectStepRuntime's certified original Idle trampoline, as
// for the hiding presentation. Zero disables playback; capture still works.
void Initialize(std::uintptr_t moduleBase, std::uintptr_t nativeIdleTransition = 0U) noexcept;

// --- Owner side -------------------------------------------------------------
// Engine thread, before the character manager. Samples the actual local
// THE_PLAYER cycle: any CATHODE scripted animation (constructor 006568B0,
// vtable EADB58, state 0x12) is captured with its native parameters, including
// the actual locker entry, idle and exit clips.
void PumpLocalOnEngineThread() noexcept;
struct LocalSnapshot final {
    character_incarnations::Ticket ticket{};
    State state{};
    std::uint64_t capturedAt{};
    bool valid{};
};
[[nodiscard]] LocalSnapshot ReadLocal() noexcept;
// Clip-resolver hook feed (any thread): aliases the local player's own clip
// database resolves, kept for the owner observation diagnostic.
void ObserveResolvedAlias(std::uint32_t database, std::uint32_t name, std::uint32_t path) noexcept;

// --- Observer side ----------------------------------------------------------
// Value-only mailbox (IPC thread). An inactive State clears the stay.
bool Publish(std::uintptr_t character, const State&, std::string& error) noexcept;
// Engine thread, before movement/turn/pose correction of the representations.
void PumpAllOnEngineThread() noexcept;
// After CharacterManager: publish each representation's bone-mask state to the
// layer-blend redirect (idle pose kept on bones the replayed clip does not drive).
void ApplyMasksOnEngineThread() noexcept;
// Redirects the layer-composition CALL (0x2A5026 -> 0x2A25A0). Optional: on
// failure clips simply play unmasked.
bool InstallMaskHook(HookManager& hooks, std::uintptr_t moduleBase, std::string& error) noexcept;
// True while a replicated clip owns the representation (movement must hold).
[[nodiscard]] bool OwnsMovement(std::uintptr_t character) noexcept;
// Exact owned cycle only: native body root, FP network follower, or visual locker.
// Unadmitted/deferred states return None, never freezing normal locomotion.
[[nodiscard]] MovementPolicy MovementPolicyFor(std::uintptr_t character) noexcept;
// Our clip is the current cycle: carry the actor along the owner's path.
[[nodiscard]] bool ClipPlaying(std::uintptr_t character) noexcept;// Keep the actor visible through native locker entry/exit, but allow the
// occupancy runtime to conceal a settled hidden idle (which may loop).
[[nodiscard]] bool HidingTransitionPlaying(std::uintptr_t character) noexcept;
// Only true for an already constructed and owned native animation cycle.
[[nodiscard]] bool HidingEntryPlaying(std::uintptr_t character) noexcept;
[[nodiscard]] bool HidingExitPlaying(std::uintptr_t character) noexcept;
// Engine thread. Render-only displacement of the representation's body in its
// own model space (Y up, heading 0 = +Z): the final pose's skeleton roots are
// turned by `yaw` about the Character origin, then moved by `offset`. The
// Character entity, its physics and AI position stay where retail keeps them.
// stableRoot=true (native vent only) uses the rig's idle root translation as
// its visual anchor instead of adding owner motion to a moving native root.
bool SetVisualOffset(std::uintptr_t character, bool active, const std::array<float, 3>& offset,
    float yaw, bool stableRoot = false) noexcept;
[[nodiscard]] std::uint64_t VisualOffsetFrames(std::uintptr_t character) noexcept;
struct VisualRootStats final {
    std::uint64_t renderedFrames{}, stableFrames{}, idleReferences{}, liveReferences{};
    float maxSuppressedRootDeltaY{};
    std::uint64_t ventGroundFrames{}, ventGroundSkipped{}, ventGroundMapped{};
    float ventMaxGroundLift{}, ventMaxDepthBeforeGround{}, ventMaxHipDrop{};
    float ventLastFootY{}, ventIdleFootY{};
};
[[nodiscard]] VisualRootStats VisualRootSnapshot(std::uintptr_t character) noexcept;
// Native clock of an owned locker entry clip, 0..1. Engine thread only.
[[nodiscard]] bool HidingEntryProgress(std::uintptr_t character, float& progress) noexcept;
// One of our locker entry/idle/exit clips is the current cycle.
[[nodiscard]] bool HidingClipOwned(std::uintptr_t character) noexcept;
// Engine thread. Where the owner's first-person Character stood at the time
// our locker clip has reached (its real entry/exit path), or its latest
// transform while hidden. False when there is nothing to show yet.
[[nodiscard]] bool HidingTarget(std::uintptr_t character, std::array<float, 3>& position, float& yaw) noexcept;
void ClearHidingTrack(std::uintptr_t character) noexcept;
[[nodiscard]] bool HidingExitProgress(std::uintptr_t character, float& progress) noexcept;
// The looped hidden idle (crouched in a small locker) is our current cycle.
[[nodiscard]] bool HidingIdlePlaying(std::uintptr_t character) noexcept;
// Height kept while the clip plays: representation ground minus owner position.
[[nodiscard]] float HeightOffset(std::uintptr_t character, float ownerY) noexcept;
// A hiding exit uses its authored outward heading, not the owner network yaw.
[[nodiscard]] float PlaybackYaw(std::uintptr_t character, float ownerYaw) noexcept;
// Value-only lifecycle cleanup; pinned clip requests drain on the next pump.
void Forget(std::uintptr_t character) noexcept;
void ForgetAll() noexcept;

void Export(Diagnostics& diagnostics, std::uintptr_t remoteCharacter);
}
