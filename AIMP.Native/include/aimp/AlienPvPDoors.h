#pragma once
#include <cstdint>
#include <string>

namespace aimp { class SignatureResolver; class WorldEntityRegistry; }

// AvS pass 18: automatic doors for the possessed Alien.
// Retail Door_Package doors open from a Box trigger or from a PathfindingWaitNode
// on an NPC's navigation path. The piloted Alien moves by direction requests,
// so neither fires. Every PC queues the door's own retail request_open input
// (same path as the legacy WS DoorAction replay) for unlocked doors near the
// Alien Character present on that PC. Locked doors (+4 & 0x200) are skipped.
namespace aimp::alien_pvp_doors {
bool Initialize(const SignatureResolver& resolver, WorldEntityRegistry& registry, std::string& error) noexcept;
void Shutdown() noexcept;
// Engine thread. alien = the Alien Character on this PC (pilot or replica), 0 = none.
void Pump(std::uintptr_t alien) noexcept;
struct Stats {
    std::uint64_t known{}, placed{}, opens{}, locked{}, failed{}, unresolved{};
    float lastDoorM{-1.0F};
    // Pass 20 probe (per level generation, registry only) for the next pass's
    // retail locker kill / vent killtrap: entities of the shared Hiding_logic
    // (Alien locker-kill CMD_PlayAnimation x4, the locker Box trigger,
    // CHR_HoldBreath) and of VentTraversalMarker (killtrap Alien animation)
    // found in this PC's registry, distinct instance hashes among the locker-kill
    // animations, and how many of the first ones resolved to a live object.
    std::uint64_t lockerKillAnims{}, lockerKillInstances{}, lockerKillResolved{}, lockerBoxes{}, holdBreath{};
    std::uint64_t killtrapAnims{}, killtrapResolved{}, playAnimationTotal{};
};
[[nodiscard]] Stats Snapshot() noexcept;
}
