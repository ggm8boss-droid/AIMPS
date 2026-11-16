#include "aimp/AnimationSyncMovementPolicy.h"
#include <cassert>
#include <initializer_list>
#include <iostream>

using aimp::animation_sync::MovementPolicy;
using aimp::animation_sync::SelectMovementPolicy;
using aimp::animation_sync::FollowNetworkPath;

static_assert(SelectMovementPolicy(false, false, false) == MovementPolicy::None);
static_assert(SelectMovementPolicy(false, true, true) == MovementPolicy::None);
static_assert(SelectMovementPolicy(true, true, false) == MovementPolicy::NativeRoot);
static_assert(SelectMovementPolicy(true, false, false) == MovementPolicy::FirstPersonPath);
static_assert(SelectMovementPolicy(true, true, true) == MovementPolicy::LockerVisual);
static_assert(SelectMovementPolicy(true, false, true) == MovementPolicy::LockerVisual);
static_assert(SelectMovementPolicy(true, true, false, true) == MovementPolicy::NativeVentVisual);
static_assert(SelectMovementPolicy(false, true, false, true) == MovementPolicy::None);
static_assert(SelectMovementPolicy(true, true, true, true) == MovementPolicy::LockerVisual);
static_assert(SelectMovementPolicy(true, false, false, true) == MovementPolicy::FirstPersonPath);
static_assert(!FollowNetworkPath(MovementPolicy::None));
static_assert(!FollowNetworkPath(MovementPolicy::NativeRoot));
static_assert(FollowNetworkPath(MovementPolicy::FirstPersonPath));
static_assert(!FollowNetworkPath(MovementPolicy::LockerVisual));
static_assert(!FollowNetworkPath(MovementPolicy::NativeVentVisual));

int main() {
    for (const bool owned : {false, true})
        for (const bool native : {false, true})
            for (const bool locker : {false, true})
                for (const bool vent : {false, true}) {
                    const auto policy = SelectMovementPolicy(owned, native, locker, vent);
                    assert(FollowNetworkPath(policy) == (owned && !native && !locker));
                    if (owned && native && vent && !locker)
                        assert(policy == MovementPolicy::NativeVentVisual);
                }
    std::cout << "AnimationSync movement policy tests passed\n";
}
