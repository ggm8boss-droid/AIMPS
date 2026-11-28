#pragma once
#include "aimp/AlienPvPWire.h"
#include "aimp/AlienPvPActionRuntime.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace aimp::alien_pvp {
struct Context final {
    Scope scope{};
    std::uintptr_t base{},character{};
    bool host{},localOwner{},owned{},pilotReady{};
    const ActionHooks* actions{};
};
struct LocalInput final {
    float forward{},strafe{},yaw{},pitch{};
    std::uint32_t gait{1U};
    Action action{Action::None};
    bool focused{};
    bool selectedVent{};
    float ventX{},ventY{},ventZ{};
    // AvS round 10: crawlspace mode (duct or C entry window): the owner drives
    // a retail path move instead of the kind9 direction move.
    bool crawl{};
};
// Engine-thread calls. No camera/input/character pointer crosses IPC.
void CaptureLocal(const LocalInput& input) noexcept;
void Pump(const Context& context) noexcept;
void Reset(bool certifiedLivingCharacter=false) noexcept;
bool Owns(std::uintptr_t character,std::uint32_t generation) noexcept;
bool MovingNow() noexcept;
Frame VentAuthority() noexcept;
bool VentPending() noexcept;
// HUD only: a vent request really in flight / a non-vent (attack) action busy.
bool VentRequestPending() noexcept;
bool NonVentActionBusy() noexcept;
// AvS round 10: retail reaction action on this PC's piloted Alien (owner only).
bool PlayReaction(std::uint32_t type,std::uint64_t windowMs) noexcept;
// AvS round 10: a stun roar Commit arrived (once per roar); yaw = direction
// from the Alien to the survivor it stuns.
bool TakeStunRoar(float& yaw) noexcept;
bool ControlActive() noexcept;
// Worker-thread calls. Queues hold copied frames, never native resources.
bool QueueCommand(std::span<const std::byte> bytes) noexcept;
bool TryPopObservation(std::array<std::uint8_t,kWireBytes>& bytes) noexcept;
struct Counters final {
    std::uint64_t received{},rejected{},expired{},dropped{},inputs{},commits{},poses{},corrections{},unsupportedActions{};
};
Counters Snapshot() noexcept;
// Engine thread: the last owner pose (Correction) this observer received and
// its age. False on the owner or before any correction. Debug only.
bool LastCorrection(Frame& frame,std::uint64_t& ageMs) noexcept;
// Debug point for "the Alien is hard to control / stops in place": frames in
// which the local pilot held a movement key, how many drove the Alien and why
// the others did not (session totals, any thread).
struct MovementStats final {
    std::uint64_t heldFrames{},drivenFrames{},blockedAction{},blockedVent{},blockedStale{},blockedStage{},driveRefused{};
    std::uint64_t chaseDriven{}; // AvS round 5: frames the auto-chase drove the Alien
    // Pass 10: vent requests dropped locally because no Host answer arrived
    // (Preparing > 4 s, Started > 30 s) or the possession epoch changed.
    std::uint64_t ventExpired{},ventEpochCleared{};
};
MovementStats MovementSnapshot() noexcept;
// AvS pass 19 owner presentation on observers (puppet Alien).
// crawlPosesSent = poses this pilot flagged Crawling; crawlMirrorFrames = observer
// frames driving the replica with the retail crawl path toward the owner pose;
// crawlHatchFrames = observer frames the replica was in its own hatch traversal
// (18) while mirroring; ownerBlendFrames = frames the owner-pose blend
// (committed input + replicatePose) ran; crawlFallback = mirroring dropped to the
// blend because the replica was > 4 m behind.
// Pass 20: crawlTailFrames = frames the copy kept crawling toward the owner after
// the owner left the duct (climbs out itself); crawlTailDone = tails that caught
// up; blendThroughBusy / blendBlockedBusy = observer frames with an open action
// receipt that still followed the owner pose / were held (replica in a native
// action); blendInactive = owner-pose blend frames with an inactive input window.
struct PuppetStats final {
    std::uint64_t crawlPosesSent{},crawlMirrorFrames{},crawlHatchFrames{},ownerBlendFrames{},crawlFallback{};
    std::uint64_t crawlTailFrames{},crawlTailDone{},blendThroughBusy{},blendBlockedBusy{},blendInactive{};
};
PuppetStats PuppetSnapshot() noexcept;
} // namespace aimp::alien_pvp
