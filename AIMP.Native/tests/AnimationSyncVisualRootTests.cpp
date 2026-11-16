#include "aimp/AnimationSyncVisualRoot.h"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
using aimp::animation_sync::VisualRootPosition;
static void RequireClose(float actual, float expected, const char* reason) {
    if (std::abs(actual - expected) > 0.0001F) throw std::runtime_error(reason);
}
int main() {
    constexpr std::array<float, 3> idle{0, 0, 0};
    constexpr std::array<float, 3> animated{0, -0.48F, 0.12F};
    constexpr std::array<float, 3> offset{0, 0.55F, -1.2F};
    const auto additive = VisualRootPosition(animated, idle, offset, 0.0F, false);
    const auto anchored = VisualRootPosition(animated, idle, offset, 0.0F, true);
    RequireClose(additive[1], 0.07F, "Original additive root follows the native downward displacement");
    RequireClose(anchored[1], 0.55F, "Anchored root must retain the owner's vertical correction");
    RequireClose(anchored[2], -1.2F, "Anchored root must retain the owner's forward correction");
    const auto turned = VisualRootPosition({1, 2, 3}, idle, {0, 0, 0},
        3.14159265358979323846F, false);
    RequireClose(turned[0], -1.0F, "Yaw transforms X");
    RequireClose(turned[2], -3.0F, "Yaw transforms Z");
    const auto unchanged = VisualRootPosition(animated, animated, {0, 0, 0}, 0.0F, true);
    RequireClose(unchanged[1], -0.48F, "Stable root matching the native pose must not move");
    std::cout << "AnimationSync visual root regression tests passed\n";
}
