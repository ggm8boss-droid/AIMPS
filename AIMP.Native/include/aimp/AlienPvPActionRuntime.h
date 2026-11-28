#pragma once
#include "aimp/AlienPvPWire.h"
#include <cstdint>
namespace aimp::alien_pvp {
struct Context;
// Local callbacks installed only by the certified AISync engine-thread owner.
// No pointer is serialized. A simulated host can exercise the real controller
// through the same seam without claiming that the retail ABI was executed.
struct ActionHooks final {
    bool (*begin)(const Context&,const Frame&,Frame& result) noexcept{};
    bool (*poll)(const Context&,Frame& result) noexcept{};
    bool (*busy)(const Context&) noexcept{};
    void (*cancel)(const Context&,bool living) noexcept{};
    // Authenticated pose recovery uses a native navigation request, not a
    // position write. A null goal asks to retire it and returns while busy.
    bool (*recover)(const Context&,const Frame* goal) noexcept{};
    bool (*cancelRecovery)(const Context&,bool living) noexcept{};
    // Donor control.cpp: retained native kind9 MoveInDirection request.
    // Null intent retires it before a vent, pair, recovery or lease release.
    bool (*drive)(const Context&,const Frame* intent) noexcept{};
    bool (*replicatePose)(const Context&,const Frame&) noexcept{};
    bool (*interruptVent)(const Context&,Frame&) noexcept{};
    // AvS round 5 auto-chase. While a pending attack has not been submitted:
    // 0 not chasing, 1 run toward the victim along yaw, 2 close enough, hold.
    int (*chase)(const Context&,float& yaw) noexcept{};
    // AvS round 10 crawlspaces: owner-only retail path move toward a short goal
    // along the intent direction (crawl ducts + hatch traversals). Null retires.
    bool (*crawl)(const Context&,const Frame* intent) noexcept{};
    // AvS round 10: owner-only retail reaction action (kind-5 request, action
    // type e.g. 87 flame recoil) for windowMs; type 0 pumps/retires it.
    bool (*react)(const Context&,std::uint32_t type,std::uint64_t windowMs) noexcept{};
    // Pass 15: observer of a Host-piloted Alien (Host poses carry no
    // OwnerMeasured, so replicatePose never runs there). Puts the replica in
    // the Host pose's stage (backstage/frontstage) at that pose.
    bool (*reconcileStage)(const Context&,const Frame& hostPose) noexcept{};
};
} // namespace aimp::alien_pvp
