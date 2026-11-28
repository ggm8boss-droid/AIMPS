#pragma once

#include <cstdint>
#include <string>

namespace aimp {
class Diagnostics;
class GameThreadDispatcher;
class HookManager;
class LevelGenerationTracker;
class SignatureResolver;

namespace character_action_runtime {

enum class Phase : std::uint8_t { Idle, PendingStart, Held, Releasing, Faulted };

struct Result {
    bool accepted{};
    std::string statusCode;
    std::string message;
};

// Installed only for the exact certified executable, after the shared spawn,
// destruction and engine-thread hooks. Does not create an extra engine update.
bool Install(const SignatureResolver& resolver, HookManager& hooks,
    LevelGenerationTracker& generations, GameThreadDispatcher& dispatcher,
    Diagnostics& diagnostics, bool exactExecutableHash, std::string& error);
[[nodiscard]] bool Installed() noexcept;

// Engine-thread commands publish intent ONLY. No native setter, item accessor,
// ShootingRequest factory or native update is called by these two functions.
[[nodiscard]] Result SetAim(std::uintptr_t character, bool enabled);
[[nodiscard]] Result SetPrimaryAction(std::uintptr_t character, bool enabled);

// Explicit manual test command, executed once on the certified engine dispatcher.
// Refuses held/pending/releasing/faulted input. Does not publish an intention,
// refill automatically, change capacity, or synthesize the Reload action.
[[nodiscard]] Result RefillTestAmmo(std::uintptr_t character);

// Lifecycle notifications from the existing shared hooks. Notifications never
// invoke native code. Only AIMP-attributed spawns may acquire an action session.
void ObserveCreated(std::uintptr_t character, std::uint32_t spawnKey,
    std::uint64_t generation, bool aimpOwned) noexcept;
void ForgetCharacter(std::uintptr_t character) noexcept;
// AvS round 10 (flamethrower fear / kill block / shotgun stun on the Alien
// pilot's PC): read-only view of a remote survivor proxy's replicated input.
// aim/primary = the intent last published by the launcher's PlayerSync replay.
// weapon = item class of the last successful remote equipment provision on
// this Character: 0 unknown/other, 1 flamethrower, 2 shotgun.
enum RemoteWeapon : std::uint8_t { WeaponUnknown=0, WeaponFlamethrower=1, WeaponShotgun=2 };
struct RemoteIntent final { bool known{}, aim{}, primary{}; std::uint8_t weapon{}; std::int32_t slot{-1}; };
[[nodiscard]] RemoteIntent QueryRemoteIntent(std::uintptr_t character) noexcept;
void NoteRemoteWeapon(std::uintptr_t character, const char* compositeName) noexcept;
void ForgetSpawnKey(std::uint32_t spawnKey) noexcept;
void InvalidateGeneration(std::uint64_t generation) noexcept;

// Disable future presses and request release on the SAME natural update hook.
// Shutdown never calls native functions on the bootstrap/IPC worker thread.
void RequestStopAll() noexcept;
[[nodiscard]] bool InputsReleased() noexcept;
// Returns true if the functional hook must remain resident until pending
// releases can be consumed. Diagnostics are detached in either case.
[[nodiscard]] bool BeginLogicalShutdown() noexcept;
[[nodiscard]] const char* ToString(Phase phase) noexcept;

} // namespace character_action_runtime
} // namespace aimp
