#pragma once
#include <cstdint>

namespace aimp::animation_sync {

// Ordinary full-body clips retain native Character translation, FP fallbacks
// follow the player's network path and lockers retain their own visual path.
// Vent entry is special: the remote physical Character can be moved back by
// retail collision, while the owner is already traversing the vent.
enum class MovementPolicy : std::uint8_t {
    None = 0U,
    NativeRoot = 1U,
    FirstPersonPath = 2U,
    LockerVisual = 3U,
    NativeVentVisual = 4U,
};
[[nodiscard]] constexpr MovementPolicy SelectMovementPolicy(
    const bool ownsCycle, const bool nativeClip, const bool lockerClip,
    const bool nativeVentEntry = false) noexcept {
    if (!ownsCycle) return MovementPolicy::None;
    if (lockerClip) return MovementPolicy::LockerVisual;
    if (nativeClip && nativeVentEntry) return MovementPolicy::NativeVentVisual;
    return nativeClip ? MovementPolicy::NativeRoot : MovementPolicy::FirstPersonPath;
}
[[nodiscard]] constexpr bool FollowNetworkPath(const MovementPolicy policy) noexcept {
    return policy == MovementPolicy::FirstPersonPath;
}
}
