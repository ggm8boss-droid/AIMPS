#pragma once
#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::alien_pvp_local {
bool InstallCameraHooks(std::uintptr_t base,HookManager& hooks,std::string& error);
// PlayerLifecycle spectate: third-person orbit around a living Character,
// driven by the mouse (wheel = distance), applied after the retail
// FollowBehavior update of exactly this behavior. target 0 turns it off.
// Engine thread.
void SetSpectateOrbit(std::uintptr_t base,std::uintptr_t followBehavior,std::uintptr_t target) noexcept;
struct SpectateOrbitStats {std::uint64_t frames{},collisions{},noWorld{};};
[[nodiscard]] SpectateOrbitStats SpectateOrbitSnapshot() noexcept;

// All entry points run on the certified engine thread. The local controller
// never accepts a network pointer, never changes the AI lease owner and never
// drives another retail Character.
struct Context {
    std::uintptr_t moduleBase{};
    std::uintptr_t alien{};
    std::uintptr_t technicalPlayer{};
    std::uint32_t nativeGeneration{};
    std::uint64_t ownerPeer{}, localPeer{};
    bool exclusiveLease{};
    bool possessionRequested{};
    // Living survivors' current positions on this PC (Host-certified
    // participants other than the Alien owner). Used only to rank vent exits.
    std::uint32_t survivorCount{};
    float survivors[4][3]{};
    // AvS round 10: each survivor proxy's facing (Character+0x174), held item
    // class (character_action_runtime::RemoteWeapon) and replicated input.
    float survivorYaw[4]{};
    std::uint8_t survivorWeapon[4]{};
    bool survivorAim[4]{}, survivorFiring[4]{};
    // Pass 20: survivor hidden in a locker/cabinet on this PC (player_hiding
    // occupied): never offered as a kill target.
    bool survivorHidden[4]{};
    // AvS round 10 survivor stun: this PC's own player and the Alien Character
    // as present here (pilot or replica), on every PC.
    std::uintptr_t selfPlayer{}, alienAny{};
};

struct Readiness final {
    bool inputIsolated{}, cameraBound{}, technicalHidden{}, moving{};
};
[[nodiscard]] Readiness Snapshot() noexcept;
struct VentInterface {bool open{},backstage{};int nearby{-1},selected{-1};bool freeCamera{};
    // Vent-exit picker (replaces free flight while backstage): candidate exits
    // near survivors, the selected position in that list and the list size.
    int pickerSlot{-1},pickerCount{};
    // A living survivor is inside the Host's attack shortlist (reach, height,
    // aim cone or contact) from the pilot's view: "Left click to kill".
    bool attackReady{};};
[[nodiscard]] VentInterface VentSnapshot() noexcept;
// Exit-camera aim (AvS 2026-11-25 round 3: grey view into the duct), safe to
// read from the diagnostics thread. mode 1 physics rays, 2 toward survivor (no
// world sampled), 3 kept mouse view; open = free distance seen from the exit
// (m, -1 unknown); aims = exits aimed so far; vent = last aimed exit index.
// Round 4: mode 4 = boxed in at the exit, probed 1.2 m above the floor below;
// out = metres the eye sits out from the exit, lift = eye height over the exit.
// Round 5: floorDrop = room floor found below the exit (m, 0 = floor-level
// exit), ventHidden = frames a vent in reach was hidden behind geometry (E not
// offered). mode 5 = eye pulled back to the verified-open probe point.
struct VentCameraStats {std::uint32_t aims{},mode{},worlds{};float open{};int vent{-1};float out{},lift{},floorDrop{};
    std::uint64_t ventHidden{};
    // Round 7: ventFaceHits = frames the ray hit within 1 m of the vent point
    // (its own grille, still offered); hiddenGap/faceGap = last such distance.
    std::uint64_t ventFaceHits{};float hiddenGap{-1.0F},faceGap{-1.0F};
    // inReachFrames = frames any vent was inside reach; tableCount = native
    // vent table size; reachWorlds = physics worlds the reach ray used.
    std::uint64_t inReachFrames{};std::uint32_t tableCount{},reachWorlds{};
    // Round 8: selfHits = ray hits within 1 m of its chest start (the Alien's
    // own body, ignored); blockedOffered = frames E was offered for a blocked
    // vent because no visible one was in reach; hitFromStart = last hit's
    // distance from the ray start (m).
    std::uint64_t selfHits{},blockedOffered{};float hitFromStart{-1.0F};
    // Round 10: axis = horizontal length (m) between the vent's opening point
    // and its native ExitPoint (0 = vertical/ceiling vent); side = +1/-1 the
    // axis direction chosen as "into the room" (0 = 16-direction probe);
    // sideOpen = free distance along the chosen side (m).
    float axis{};int side{};float sideOpen{-1.0F};
    // Pass 14: ceiling-vent floor levels tried / chosen (0 = first surface;
    // mode 9 = a lower level was used), upward clearance from that level (m),
    // native ExitPoint height relative to the vent point and read ok/fail.
    std::uint32_t levels{},level{};float upClear{-1.0F},throughDy{};
    std::uint64_t throughOk{},throughFail{};};
[[nodiscard]] VentCameraStats VentCameraSnapshot() noexcept;
// AvS round 10 flamethrower fear (Alien player's PC). phase 0 normal,
// 1 retreating to a vent, 2 recovering backstage (exit refused below 100%).
// survivorWeapons: 4 bits per survivor = weapon(2 bits: 1 flamethrower,
// 2 shotgun) | aim<<2 | firing<<3. flamerAngle = last angle (rad) between a
// flamethrower survivor's facing and the Alien (yaw convention check).
struct FearStats {float meter{100.0F},resistance{};std::uint32_t phase{},retreats{},chaseRetreats{},flinches{},stuns{},blockedClicks{};
    std::uint64_t exitRefused{},retreatSent{},retreatAbandoned{},flamedFrames{},threatFrames{};
    float retreatVentM{-1.0F},flamerAngle{-1.0F};std::uint32_t survivorWeapons{};
    std::uint64_t reactions{},stunRoars{},survivorStunned{},survivorStunMissed{};
    // Pass 16: pilot Alien action-state changes into non-locomotion states, and
    // the last 12 changes as [[state,msAgo],...] (oldest first).
    std::uint64_t oddCycles{};std::string cycleTrail;
    // Pass 20: kill clicks ignored because the only survivor in reach is in a locker.
    std::uint64_t lockerClicks{};};
[[nodiscard]] FearStats FearSnapshot() noexcept;
// AvS round 10 crawlspaces (Alien PC): duct = low ceiling now (hysteresis),
// entries = C presses, frames = frames in crawl mode, firstPerson* = head
// camera switches, ductX/Z = where duct last toggled.
// Pass 10: headReleased = head camera handed back on restore (teardown),
// menuExits = first person left because a menu (pause/restart) opened.
struct CrawlStats {bool duct{};std::uint64_t entries{},ductToggles{},firstPersonOn{},firstPersonFailed{},frames{};float ductX{},ductZ{};
    std::uint64_t headReleased{},menuExits{};
    // Pass 14: crawlspaces use the close follow camera (no head camera);
    // cameraLost = the engine switched the active camera away from the pilot.
    std::uint64_t cameraLost{};};
[[nodiscard]] CrawlStats CrawlSnapshot() noexcept;
// True while this PC's Alien runs a flame retreat (vent entry may go far).
[[nodiscard]] bool RetreatActive() noexcept;
bool GoToVent(std::uint32_t index) noexcept;
void Pump(const Context& context) noexcept;
// clearCurrentWorld is false during teardown/physical generation rollover:
// never dereference old Characters or resurrect a camera from the old world.
// Only the AISync lease-release path may prove a living native identity and
// permit a write to its movement slot. Ordinary teardown restores camera
// targets but must never touch an incarnation which may have been recycled.
void Reset(bool sameCameraWorld, bool certifiedLivingCharacter = false) noexcept;
[[nodiscard]] bool Drives(std::uintptr_t character, std::uint32_t nativeGeneration) noexcept;
// The Alien whose camera the local pilot currently drives (0 otherwise),
// published by Pump for the audio thread's listener redirect.
[[nodiscard]] std::uintptr_t ListenerCharacter() noexcept;

} // namespace aimp::alien_pvp_local
