#pragma once
#include <array>
#include <cmath>

namespace aimp::animation_sync {
// In native vent mode the full-body pose's root moves independently of the
// physical Character. Anchor only that root to the standing pose to avoid
// adding native root translation to the replicated owner's world trajectory.
// Other animations and hiding keep their additive visual-root behavior.
[[nodiscard]] inline std::array<float, 3> VisualRootPosition(
    const std::array<float, 3>& animatedRoot,
    const std::array<float, 3>& stableRoot,
    const std::array<float, 3>& localOffset,
    const float yaw,
    const bool stableAnchor) noexcept {
    const auto& root = stableAnchor ? stableRoot : animatedRoot;
    const auto c = std::cos(yaw), s = std::sin(yaw);
    return {root[0] * c + root[2] * s + localOffset[0],
        root[1] + localOffset[1],
        root[2] * c - root[0] * s + localOffset[2]};
}
}
