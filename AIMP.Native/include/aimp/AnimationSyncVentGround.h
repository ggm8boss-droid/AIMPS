#pragma once
#include <algorithm>
#include <cmath>

namespace aimp::animation_sync {
// Native vent entry can leave the physical NPC below the vent floor while
// the owner's script advances upward. Support its *rendered* feet only.
// Never change the physical Character, collision, or limb animation.
[[nodiscard]] inline float VentFootDepth(
    float idleFootY, float animatedFootY, float rootCorrectionY,
    float ownerRise, float progress) noexcept {
    if (!std::isfinite(idleFootY) || !std::isfinite(animatedFootY) ||
        !std::isfinite(rootCorrectionY) || !std::isfinite(ownerRise) ||
        !std::isfinite(progress)) return 0.0F;
    // Feet can trail the original floor early, then settle onto the duct
    // floor as the owner completes the traversal.
    const float weight = std::clamp((progress - 0.35F) / 0.60F, 0.0F, 1.0F);
    const float rise = std::clamp(ownerRise, 0.0F, 1.0F);
    return std::max(0.0F, idleFootY - animatedFootY - rootCorrectionY -
        rise * (1.0F - weight));
}
[[nodiscard]] inline float VentGroundLift(
    float idleFootY, float animatedFootY, float rootCorrectionY,
    float ownerRise, float progress, float previous) noexcept {
    if (!std::isfinite(previous)) previous = 0.0F;
    previous = std::clamp(previous, 0.0F, 0.45F);
    const float depth = VentFootDepth(idleFootY, animatedFootY,
        rootCorrectionY, ownerRise, progress);
    // This is a conservative floor aid, not an unconstrained forced offset.
    const float goal = std::clamp(depth, 0.0F, 0.45F);
    return std::clamp(goal, std::max(0.0F, previous - 0.035F), previous + 0.06F);
}
}
