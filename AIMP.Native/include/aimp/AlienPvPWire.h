#pragma once
// Local IPC only. Portable identities and copied scalar values, never pointers.
// The peer network uses the authenticated JSON contracts in AlienControlModels.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aimp::alien_pvp {
inline constexpr std::size_t kWireBytes=200U;
inline constexpr std::uint32_t kWireMagic=0x33535641U; // AVS3
inline constexpr float kPi=3.14159265358979323846F;
enum class Kind : std::uint16_t {
    Status=1, Input=2, Pose=3, Activate=4, Commit=5, Correction=6, Stop=7, ActionResult=8, Resolution=9, Acknowledge=10
};
enum Flag : std::uint32_t {
    Bound=1U, LocalOwner=2U, PilotReady=4U, Focused=8U, Host=16U,
    Moving=32U, NativeBusy=64U, Active=128U, Expired=256U,
    UnsupportedAction=512U, PoseValid=1024U, ActionApplied=2048U, Backstage=4096U, VentSelected=8192U, OwnerMeasured=16384U,
    VentAnimating=32768U,
    // AvS pass 19: pilot in crawl mode or its crawlspace hatch traversal (18).
    Crawling=65536U
};
inline constexpr std::uint32_t kKnownFlags=131071U;
enum class Action : std::uint32_t { None, Attack, EnterVent, ExitVent, Roar, CancelVent };
enum class ActionPhase : std::uint32_t { None, Started, Completed, Rejected, Failed, Cancelled, Preparing };
enum class ActionReason : std::uint32_t { None, NativeBusy, Cooldown, NoTarget, NotReady,
    NativeUnavailable, NativeRefused, NoVent, AmbiguousVent, WrongStage, Stale, Timeout, Fault, Occluded };
struct Scope final {
    std::uint32_t nativeGeneration{}, worldGeneration{};
    std::uint64_t cookie{}, ownerPeer{}, incarnation{};
    std::uint32_t entityA{}, entityB{};
    bool operator==(const Scope&) const noexcept=default;
};
struct Frame final {
    Kind kind{Kind::Status};
    Scope scope{};
    std::uint64_t epoch{}, sequence{}, hostSequence{}, acknowledgedInput{}, sampledAtMs{};
    std::uint32_t flags{}, gait{1U};
    Action action{Action::None};
    float forward{}, strafe{}, yaw{}, pitch{}, x{}, y{}, z{}, velocityX{}, velocityZ{};
    // Resolution only: player identity or actual vent endpoints; no local indices.
    std::uint64_t targetPeer{}, resolutionRevision{};
    std::uint32_t targetA{},targetB{},selector{};
    ActionReason reason{ActionReason::None};
    float resolvedX{},resolvedY{},resolvedZ{};
    ActionPhase phase{ActionPhase::None};
    // Ordered, reliable LOCAL transport stream. Independent of gameplay revisions.
    std::uint64_t deliveryId{};
};
inline bool Valid(const Scope& s) noexcept {
    return s.nativeGeneration && s.worldGeneration && s.cookie && s.ownerPeer &&
        s.incarnation && (s.entityA || s.entityB);
}
inline bool Valid(const Frame& f) noexcept {
    if (!Valid(f.scope) || static_cast<unsigned>(f.kind)<1U ||
        static_cast<unsigned>(f.kind)>10U || (f.flags&~kKnownFlags) || f.gait>3U ||
        static_cast<unsigned>(f.action)>5U) return false;
    for (const auto v:{f.forward,f.strafe,f.yaw,f.pitch,f.x,f.y,f.z,f.velocityX,f.velocityZ,f.resolvedX,f.resolvedY,f.resolvedZ})
        if (!std::isfinite(v)) return false;
    if (std::abs(f.forward)>1.0F || std::abs(f.strafe)>1.0F ||
        std::abs(f.yaw)>kPi+0.00001F || std::abs(f.pitch)>kPi/2.0F+0.00001F ||
        std::abs(f.x)>100000.0F || std::abs(f.y)>100000.0F || std::abs(f.z)>100000.0F ||
        std::hypot(f.velocityX,f.velocityZ)>30.01F ||
        std::abs(f.resolvedX)>100000.0F || std::abs(f.resolvedY)>100000.0F || std::abs(f.resolvedZ)>100000.0F ||
        static_cast<unsigned>(f.phase)>6U || static_cast<unsigned>(f.reason)>13U) return false;
    if (f.kind!=Kind::Status && !f.epoch) return false;
    if((f.flags&VentAnimating) && ((f.kind!=Kind::Pose && f.kind!=Kind::Correction) ||
        (f.flags&(PoseValid|Backstage|NativeBusy))!=(PoseValid|Backstage|NativeBusy)))return false;
    if (f.kind==Kind::Acknowledge && (!f.deliveryId || f.sequence || f.hostSequence || f.flags)) return false;
    if ((f.kind==Kind::Input || f.kind==Kind::Commit || f.kind==Kind::Pose ||
        f.kind==Kind::Correction || f.kind==Kind::ActionResult || f.kind==Kind::Resolution) && !f.sequence) return false;
    if ((f.kind==Kind::Commit || f.kind==Kind::Resolution) && !f.hostSequence) return false;
    if (f.kind!=Kind::Input && f.kind!=Kind::Commit && f.kind!=Kind::ActionResult && f.kind!=Kind::Resolution &&
        f.action!=Action::None) return false;
    if((f.flags&VentSelected) &&
        ((f.kind!=Kind::Input && f.kind!=Kind::Commit) || (f.action!=Action::ExitVent && f.action!=Action::EnterVent)))return false;
    if(f.kind==Kind::Resolution && (f.action==Action::None || f.phase==ActionPhase::None || !f.resolutionRevision))return false;
    if(f.kind==Kind::Resolution) {
        if(f.action==Action::Roar || (f.phase==ActionPhase::Preparing && f.action!=Action::Attack))return false;
        const bool success=f.phase==ActionPhase::Preparing || f.phase==ActionPhase::Started || f.phase==ActionPhase::Completed;
        if(success!=(f.reason==ActionReason::None))return false;
        if(f.action==Action::Attack && success && (!f.targetPeer || f.targetPeer==f.scope.ownerPeer ||
            (!f.targetA && !f.targetB) || f.selector>65535U))return false;
        if(f.action!=Action::Attack && (f.targetPeer || f.targetA || f.targetB || f.selector))return false;
    }
    if(f.kind!=Kind::Resolution && f.kind!=Kind::ActionResult &&
        (f.targetPeer||f.targetA||f.targetB||f.selector||f.reason!=ActionReason::None||
         f.resolutionRevision||f.phase!=ActionPhase::None||f.resolvedX!=0||f.resolvedY!=0||f.resolvedZ!=0))return false;
    return true;
}
inline std::uint32_t Read32(std::span<const std::uint8_t> b,std::size_t o) noexcept {
    std::uint32_t v{};for(std::size_t i=0;i<4U;++i)v|=std::uint32_t(b[o+i])<<(i*8U);return v;
}
inline std::uint64_t Read64(std::span<const std::uint8_t> b,std::size_t o) noexcept {
    return std::uint64_t(Read32(b,o))|(std::uint64_t(Read32(b,o+4U))<<32U);
}
inline void Write32(std::span<std::uint8_t> b,std::size_t o,std::uint32_t v) noexcept {
    for(std::size_t i=0;i<4U;++i)b[o+i]=static_cast<std::uint8_t>(v>>(i*8U));
}
inline void Write64(std::span<std::uint8_t> b,std::size_t o,std::uint64_t v) noexcept {
    Write32(b,o,static_cast<std::uint32_t>(v));Write32(b,o+4U,static_cast<std::uint32_t>(v>>32U));
}
inline bool Encode(const Frame& f,std::span<std::uint8_t> b) noexcept {
    if(b.size()!=kWireBytes || !Valid(f))return false;
    for(auto& v:b)v=0U;
    Write32(b,0,kWireMagic);Write32(b,4,3U|(std::uint32_t(f.kind)<<16U));
    Write32(b,8,f.scope.nativeGeneration);Write32(b,12,f.scope.worldGeneration);
    Write64(b,16,f.scope.cookie);Write64(b,24,f.scope.ownerPeer);Write64(b,32,f.epoch);
    Write64(b,40,f.scope.incarnation);Write64(b,48,f.sequence);Write64(b,56,f.hostSequence);
    Write64(b,64,f.acknowledgedInput);Write32(b,72,f.scope.entityA);Write32(b,76,f.scope.entityB);
    Write32(b,80,f.flags);Write32(b,84,f.gait);Write32(b,88,std::uint32_t(f.action));
    const std::array<float,9> v{f.forward,f.strafe,f.yaw,f.pitch,f.x,f.y,f.z,f.velocityX,f.velocityZ};
    for(std::size_t i=0;i<v.size();++i)Write32(b,96U+4U*i,std::bit_cast<std::uint32_t>(v[i]));
    Write64(b,136,f.sampledAtMs);Write64(b,144,f.targetPeer);
    Write32(b,152,f.targetA);Write32(b,156,f.targetB);Write32(b,160,f.selector);Write32(b,164,std::uint32_t(f.reason));
    Write32(b,168,std::bit_cast<std::uint32_t>(f.resolvedX));Write32(b,172,std::bit_cast<std::uint32_t>(f.resolvedY));
    Write32(b,176,std::bit_cast<std::uint32_t>(f.resolvedZ));Write32(b,180,std::uint32_t(f.phase));
    Write64(b,184,f.resolutionRevision);Write64(b,192,f.deliveryId);return true;
}
inline bool Decode(std::span<const std::uint8_t> b,Frame& f) noexcept {
    f={};
    if(b.size()!=kWireBytes || Read32(b,0)!=kWireMagic ||
        (Read32(b,4)&0xFFFFU)!=3U || Read32(b,92)!=0U || Read32(b,132)!=0U)return false;
    f.kind=static_cast<Kind>(Read32(b,4)>>16U);
    f.scope={Read32(b,8),Read32(b,12),Read64(b,16),Read64(b,24),Read64(b,40),Read32(b,72),Read32(b,76)};
    f.epoch=Read64(b,32);f.sequence=Read64(b,48);f.hostSequence=Read64(b,56);f.acknowledgedInput=Read64(b,64);
    f.flags=Read32(b,80);f.gait=Read32(b,84);f.action=static_cast<Action>(Read32(b,88));
    f.forward=std::bit_cast<float>(Read32(b,96));f.strafe=std::bit_cast<float>(Read32(b,100));
    f.yaw=std::bit_cast<float>(Read32(b,104));f.pitch=std::bit_cast<float>(Read32(b,108));
    f.x=std::bit_cast<float>(Read32(b,112));f.y=std::bit_cast<float>(Read32(b,116));
    f.z=std::bit_cast<float>(Read32(b,120));f.velocityX=std::bit_cast<float>(Read32(b,124));
    f.velocityZ=std::bit_cast<float>(Read32(b,128));f.sampledAtMs=Read64(b,136);
    f.targetPeer=Read64(b,144);f.targetA=Read32(b,152);f.targetB=Read32(b,156);f.selector=Read32(b,160);
    f.reason=static_cast<ActionReason>(Read32(b,164));f.resolvedX=std::bit_cast<float>(Read32(b,168));
    f.resolvedY=std::bit_cast<float>(Read32(b,172));f.resolvedZ=std::bit_cast<float>(Read32(b,176));
    f.phase=static_cast<ActionPhase>(Read32(b,180));f.resolutionRevision=Read64(b,184);f.deliveryId=Read64(b,192);
    return Valid(f);
}
} // namespace aimp::alien_pvp
