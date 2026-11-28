// Donor-derived movement/parking: AI-Playable-Alien, (c) 2026 pirletik.
// MIT license: third_party/AI-Playable-Alien/LICENSE.
#include "aimp/AlienPvPLocalController.h"
#include "aimp/AlienPvPInputIsolation.h"
#include "aimp/AlienPvPVisualIsolation.h"
#include "aimp/AlienPvPControlRuntime.h"
#include "aimp/AlienPvPControlPolicy.h"
#include "aimp/AlienPvPTechnicalPolicy.h"
#include "aimp/AlienPvPVents.h"
#include "aimp/AlienPvPFreeCamera.h"
#include "aimp/AlienPvPActionPolicy.h"
#include "aimp/AlienPvPFear.h"
#include "aimp/CharacterActionRuntime.h"

#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"
#include "aimp/MultiplayerPause.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <atomic>

namespace aimp::alien_pvp_local {
namespace {

// Certified retail Steam x86 RVAs; the caller supplies AISync's exact-build
// engine-thread context. Never scan the process for a camera or character.
constexpr std::uintptr_t kGlobalsRva=0x012F0C88U;
constexpr std::uintptr_t kCameraSetEntityRva=0x00029220U;
constexpr std::uintptr_t kCameraActivateRva=0x00032E10U;
constexpr std::uintptr_t kCharacterSkeletonOffset=0x718U;

struct CameraBinding final {
    std::uintptr_t root{}, manager{}, follow{}, head{}, previous{}, technical{}, alien{}, technicalSkeleton{};
    std::uint32_t generation{}, technicalRuntime{}, alienRuntime{};
    std::uintptr_t alienSkeleton{};
    bool bound{}, touched{}, firstPerson{};
    std::uintptr_t followBehavior{};
    std::array<float,6> originalFollow{};
    std::uint8_t originalFirstPerson{};
    bool followSaved{};
};
struct LocalState final {
    CameraBinding camera{};
    alien_pvp::technical::Parking parking{};
    std::uintptr_t character{};
    std::uint32_t generation{};
    bool moving{},visualReady{},middleDown{};
    bool attackDown{}, enterDown{}, exitDown{}, roarDown{};
    bool ventMapOpen{},wasBackstage{},ventLeft{},ventRight{},ventConfirm{};
    bool ventWasPending{},ventCancelSent{};std::uint64_t ventPendingAt{};
    // WASD keys held when a vent action became pending. Only a NEW press
    // cancels: holding W while pressing E used to cancel every entry 400 ms
    // later (AvS 2026-11-24: "runs around and doesn't enter the vent").
    unsigned ventMoveMask{};
    // Free camera chases the Alien along its real exit passage.
    bool exitTracking{};
    // AvS 2026-11-25: free flight inside the vents drifted out of the level
    // (alt-tab) and gave no sense of where to go. Backstage the camera now
    // jumps between real exits ranked by distance to the survivors.
    std::array<int,8> pickerVents{};int pickerCount{},pickerSlot{-1};
    std::uint64_t pickerBuiltAt{};bool pickPrev{},pickNext{};
    // AvS 2026-11-25 (round 3): the eye sat ON the vent point, facing the
    // duct wall (grey screen). FollowAfter probes the open side of the room
    // once per selected exit and keeps the mouse look relative to that aim.
    int pickerAimVent{-1};bool pickerAimPending{},pickerAimed{};
    std::array<float,3> pickerExit{},pickerEye{},pickerToward{};bool pickerHasToward{};
    // Round 10: horizontal unit axis opening<->native ExitPoint (sign unknown).
    std::array<float,3> pickerAxis{};float pickerAxisLength{};
    float pickerYaw{},pickerPitch{},orbitYaw0{},orbitPitch0{};
    bool hudHidden{};std::uint64_t hudAt{};
    // AvS 2026-11-25 round 4: "Left click to kill" hint. Same shortlist rule
    // the Host applies (reach, height, aim cone or body contact).
    bool attackReady{};
    int nearbyVent{-1},selectedVent{-1};
    float cameraDistance{3.0F},cameraHeight{2.0F},cameraSide{0.5F};
    bool duct{};unsigned ductRun{};std::uint64_t ductAt{};
    float collisionScale{1.0F};
    bool freeCamera{},eyeKnown{};
    std::array<float,3> freeEye{},lastEye{},freeForward{0.0F,0.0F,1.0F};
    std::uint64_t lastFrameMs{}, nextCameraAttemptMs{}, nextInputAttemptMs{};
};
LocalState gLocal{}; // certified game thread only
std::uintptr_t gCameraBase{};
std::atomic<DWORD> gPilotThread{};
void* gFollowTrampoline{};
void* gWorldTrampoline{};
void* gSubmitTrampoline{};
void* gPostBlendTrampoline{};
std::atomic<std::uint32_t> gCameraGeneration{};
struct WorldSample {std::atomic<std::uintptr_t> world{};std::atomic<std::uint64_t> at{};std::atomic<std::uint32_t> generation{};};
std::array<WorldSample,4> gWorlds{};
// Spectate orbit (PlayerLifecycle). Worlds sampled for it use this generation tag.
constexpr std::uint32_t kOrbitWorldGeneration=0xFFFFFFFFU;
std::atomic<std::uintptr_t> gOrbitBase{},gOrbitBehavior{},gOrbitTarget{};
std::atomic<std::uint64_t> gOrbitFrames{},gOrbitCollisions{},gOrbitNoWorld{};
struct OrbitState {std::uintptr_t target{};float yaw{},pitch{0.25F},distance{3.0F},shown{3.0F};};
OrbitState gOrbit{}; // engine thread only
struct AimStats final {
    std::atomic<std::uint32_t> aims{},mode{},worlds{};std::atomic<float> open{},out{},lift{},floorDrop{};std::atomic<int> vent{-1};
    std::atomic<float> axis{},sideOpen{-1.0F};std::atomic<int> side{};
} gAimStats; // written by the camera hook, read by diagnostics
std::atomic<std::uint64_t> gVentHidden{}; // frames a reachable vent was hidden behind geometry
std::atomic<std::uint64_t> gVentFaceHits{}; // frames the ray hit the vent's own face/grille (still offered)
std::atomic<float> gVentHiddenGap{-1.0F},gVentFaceGap{-1.0F}; // last hit -> vent point distance (m)
constexpr float kVentFaceTolerance=1.0F;
constexpr float kVentSelfClear=1.0F; // round 8: hits this close to the ray start are the Alien itself
std::atomic<std::uint64_t> gVentSelfHits{},gVentBlockedOffered{};
std::atomic<float> gVentHitFromStart{-1.0F};
std::atomic<std::uint64_t> gVentInReach{}; // on-foot frames with any vent inside WithinReach
std::atomic<std::uint32_t> gVentTableCount{},gVentWorlds{};
// Pass 14: crawl duct seen by TickCrawl (engine thread); ConfigureFollow uses
// the donor's close duct camera while it is set.
bool gCrawlClose{};
// Pass 14: the engine changed the active camera under the pilot (restored).
std::atomic<std::uint64_t> gCameraLost{};
// Pass 14 picker: ceiling-vent floor levels tried / chosen, upward clearance
// from the chosen eye, native ExitPoint reads (ok) and its height below the vent.
std::atomic<std::uint32_t> gAimLevels{},gAimLevel{};std::atomic<float> gAimUpClear{-1.0F},gAimThroughDy{};
std::atomic<std::uint64_t> gAimThroughOk{},gAimThroughFail{};

template<typename T>
bool Read(const std::uintptr_t at, T& out) noexcept {
    return at != 0U && crash_guard::TryCopy(&out,reinterpret_cast<const void*>(at),sizeof(T));
}
alien_pvp::technical::Identity TechnicalTicket(std::uintptr_t character,std::uint32_t generation) noexcept {
    alien_pvp::technical::Identity id{character,0U,0U,generation};std::uint32_t dead{};
    if(!character || !Read(character+0x50U,id.runtime) || !Read(character+0x718U,id.skeleton) ||
        !Read(character+0x2F90U,dead) || dead!=0U)return {};
    return id;
}
bool PrepareTechnicalHome(const Context& context) noexcept {
    const auto id=TechnicalTicket(context.technicalPlayer,context.nativeGeneration);
    std::array<float,3> home{};float yaw{};std::uint8_t hiding{},interaction{};
    if(!id.Valid() || !Read(context.technicalPlayer+0x70U,home) ||
        !Read(context.technicalPlayer+0x174U,yaw) || !Read(context.technicalPlayer+0x4249U,hiding) ||
        !Read(context.technicalPlayer+0x425BU,interaction))return false;
    return gLocal.parking.Prepare(id,home,alien_pvp::WrapYaw(yaw),hiding==0U && interaction==0U);
}
bool ParkTechnical(const Context& context) noexcept {
    const auto id=TechnicalTicket(context.technicalPlayer,context.nativeGeneration);
    std::array<float,3> target{};float yaw{};
    // Preserve mouse look: the technical Character supplies its current yaw.
    if(!gLocal.parking.Target(id,target) || !Read(context.technicalPlayer+0x174U,yaw) ||
        !std::isfinite(yaw))return false;
    if(!character_direct_step_runtime::ApplyAIRecoveryTransform(context.technicalPlayer,target.data(),alien_pvp::WrapYaw(yaw)))return false;
    gLocal.parking.Applied(id);return true;
}
void RestoreTechnicalHome(bool validWorld) noexcept {
    const auto camera=gLocal.camera;std::array<float,3> home{};float yaw{};
    if(validWorld && gLocal.parking.Restore(TechnicalTicket(camera.technical,camera.generation),home,yaw))
        static_cast<void>(character_direct_step_runtime::ApplyAIRecoveryTransform(camera.technical,home.data(),yaw));
    gLocal.parking.Clear();
}
bool LivingCameraTarget(const std::uintptr_t character) noexcept {
    std::uintptr_t skeleton{};
    return character != 0U && Read(character + kCharacterSkeletonOffset,skeleton) && skeleton != 0U;
}
bool CurrentCameraRoot(const std::uintptr_t base, CameraBinding& binding) noexcept {
    std::uintptr_t root{}, manager{}, follow{}, head{}, current{};
    if (base == 0U || !Read(base+kGlobalsRva,root) || !root ||
        !Read(root+0x58U,manager) || !manager ||
        !Read(manager+0x1DCU,follow) || !follow ||
        !Read(manager+0x1E0U,head) || !head ||
        !Read(manager+0x1D8U,current) || !current) return false;
    binding.root=root;binding.manager=manager;
    binding.follow=follow;binding.head=head;binding.previous=current;
    return true;
}
// Original camera behaviour list uses a deterministic name hash. Only
// touch the BoneAttached target on the currently selected retail head camera.
constexpr std::uint32_t BoneAttachedHash() noexcept {
    constexpr char name[]="BoneAttached";
    std::uint32_t h{};
    for (std::size_t i=0U,j=0U;i<256U;++i,j=(j+1U)%(sizeof(name)-1U))
        h+=static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[j]))<<(i&31U);
    return h;
}
constexpr std::uint32_t NameHash(const char* name,std::size_t size) noexcept {
    std::uint32_t h{};
    for(std::size_t i=0,j=0;i<256U;++i,j=(j+1U)%size)
        h+=static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[j]))<<(i&31U);
    return h;
}
std::uintptr_t FindFollow(const std::uintptr_t camera) noexcept {
    std::uintptr_t array{};std::uint32_t count{};
    if(!Read(camera+0xBCU,array) || !array || !Read(camera+0xC0U,count) || count>256U)return 0U;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t b{};std::uint32_t type{};
        if(Read(array+i*sizeof(std::uintptr_t),b) && b && Read(b+8U,type) &&
            type==NameHash("FollowBehavior",14U))return b;
    }
    return 0U;
}
template<class T> bool Write(std::uintptr_t at,const T& value) noexcept {
    return at && crash_guard::TryCopy(reinterpret_cast<void*>(at),&value,sizeof(value));
}
bool ConfigureFollow(CameraBinding& camera,bool firstPerson) noexcept {
    const auto b=FindFollow(camera.follow);
    if(!b || (camera.followSaved && camera.followBehavior!=b))return false;
    if(!camera.followSaved) {
        if(!Read(b+0x9CU,camera.originalFollow) || !Read(b+0xF6U,camera.originalFirstPerson))return false;
        camera.followBehavior=b;camera.followSaved=true;
    }
    // Donor camera.cpp::FollowFromCfg, DuctUpdate and FpKeeperTick.
    const std::array<float,6> values=(gLocal.duct || gCrawlClose)?
        std::array<float,6>{0.0F,0.35F,-0.9F,0.0F,0.3F,1.5F}:
        std::array<float,6>{gLocal.cameraSide,gLocal.cameraHeight,-gLocal.cameraDistance,
            0.0F,gLocal.cameraHeight-0.7F,2.0F};
    return Write(b+0x9CU,values) && Write(b+0xF6U,firstPerson?camera.originalFirstPerson:std::uint8_t{0});
}
bool IsBackstage(std::uintptr_t alien) noexcept {
    std::uint8_t a{},b{};
    return Read(alien+0x2DAU,a) && Read(alien+0x2DBU,b) && alien_pvp::spatial::Backstage(a,b);
}
// Donor Havok ray ABI and camera collision math. All native rays run on the
// current pilot's engine thread, using worlds observed in this generation.
bool Ray(std::uintptr_t world,const std::array<float,3>& from,const std::array<float,3>& to,
    std::array<float,3>& hit) noexcept {
    alignas(16) float a[4]{from[0],from[1],from[2],0},b[4]{to[0],to[1],to[2],0},p[4]{},n[4]{};
    alignas(16) std::uint8_t result[16]{};
#if defined(_MSC_VER)
    __try {
        using Fn=void(__thiscall*)(std::uintptr_t,void*,std::uintptr_t,const void*,const void*,void*,void*);
        reinterpret_cast<Fn>(gCameraBase+0x0028DAB0U)(0U,result,world,a,b,p,n);
        if(!result[0])return false;
        hit={p[0],p[1],p[2]};return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
#else
    return false;
#endif
}
// Physics worlds sampled by the camera hooks in this generation (< 1 s old).
unsigned CurrentWorlds(std::array<std::uintptr_t,4>& worlds) noexcept {
    const auto generation=gCameraGeneration.load(std::memory_order_acquire);
    const auto now=GetTickCount64();unsigned count{};
    if(!gCameraBase)return 0U;
    for(auto& sample:gWorlds) {
        if(sample.generation.load(std::memory_order_acquire)!=generation || now-sample.at.load()>1000U)continue;
        if(const auto world=sample.world.load())worlds[count++]=world;
    }
    return count;
}
// True when no sampled world blocks the segment (unknown world = not blocked).
bool SegmentClear(const std::array<std::uintptr_t,4>& worlds,unsigned count,
    const std::array<float,3>& from,const std::array<float,3>& to) noexcept {
    for(unsigned i=0;i<count;++i){std::array<float,3> hit{};if(Ray(worlds[i],from,to,hit))return false;}
    return true;
}
bool AttachHeadBone(const std::uintptr_t camera,const std::uintptr_t alien) noexcept {
    std::uintptr_t array{};
    std::uint32_t count{};
    if (!Read(camera+0xBCU,array) || !array ||
        !Read(camera+0xC0U,count) || count>256U) return false;
    for (std::uint32_t i=0U;i<count;++i) {
        std::uintptr_t behavior{};
        std::uint32_t type{};
        if (!Read(array+i*sizeof(std::uintptr_t),behavior) || !behavior ||
            !Read(behavior+8U,type)) return false;
        if (type!=BoneAttachedHash()) continue;
#if defined(_MSC_VER)
        __try {
            *reinterpret_cast<volatile std::uintptr_t*>(behavior+0xE4U)=alien;
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
        return false;
#endif
    }
    return false;
}
bool RetargetCamera(const std::uintptr_t base,const std::uintptr_t camera,
    const std::uintptr_t character) noexcept {
    if (!base || !camera || !LivingCameraTarget(character)) return false;
#if defined(_MSC_VER)
    __try {
        reinterpret_cast<void(__thiscall*)(void*,void*)>(base+kCameraSetEntityRva)(
            reinterpret_cast<void*>(camera),reinterpret_cast<void*>(character));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
bool CameraCall(const std::uintptr_t base,const std::uintptr_t manager,
    const std::uintptr_t camera,const std::uintptr_t character) noexcept {
    if (!manager || !RetargetCamera(base,camera,character)) return false;
#if defined(_MSC_VER)
    __try {
        static_cast<void>(reinterpret_cast<unsigned(__thiscall*)(void*,void*)>(base+kCameraActivateRva)(
            reinterpret_cast<void*>(manager),reinterpret_cast<void*>(camera)));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
// Camera restores use a fresh global-root proof and exact engine generation.
// Never activate a stored pointer after a loading-screen transition.
bool TechnicalIdentityCurrent(const CameraBinding& camera) noexcept;
// Pass 10: head camera handed back to the follow camera at restore / on a menu.
std::atomic<std::uint64_t> gCrawlHeadReleased{},gCrawlMenuExits{};
void RestoreLocalCamera(const bool validWorld) noexcept {
    const auto camera=gLocal.camera;
    const bool sameTechnical=validWorld && TechnicalIdentityCurrent(camera);
    std::uintptr_t alienSkeleton{};std::uint32_t alienRuntime{};
    const bool sameAlien=validWorld && camera.generation==gLocal.generation &&
        camera.alien==gLocal.character && camera.alienRuntime!=0U &&
        Read(camera.alien+0x50U,alienRuntime) && alienRuntime==camera.alienRuntime &&
        Read(camera.alien+kCharacterSkeletonOffset,alienSkeleton) && alienSkeleton==camera.alienSkeleton;
    // AvS pass 10 crash (AI.exe+0x62C011 <- 006B3100 "Head1" <- 0042CC0F): a
    // restart while the crawl first-person head camera was active left that
    // retail camera targeting the Alien; its next update read the dying Alien's
    // Head1 bone (+0x3F0 -> +0x328 null). Leave first person while the Alien is
    // still proven live, before any early return below.
    if (camera.firstPerson && sameAlien && gCameraBase && camera.manager && camera.follow &&
        CameraCall(gCameraBase,camera.manager,camera.follow,camera.alien))
        gCrawlHeadReleased.fetch_add(1U,std::memory_order_relaxed);
    alien_pvp_visual::RestoreAlienHead(camera.alien,camera.generation,sameAlien);
    alien_pvp_visual::RestoreTechnical(camera.technical,camera.generation,sameTechnical);
    RestoreTechnicalHome(sameTechnical);
    gLocal.camera={};
    if (!sameTechnical || !camera.touched || gCameraBase==0U) { gCameraBase=0U;return; }
    CameraBinding current{};
    std::uintptr_t active{};
    if (CurrentCameraRoot(gCameraBase,current) &&
        current.root==camera.root && current.manager==camera.manager &&
        current.follow==camera.follow && current.head==camera.head) {
        if(camera.followSaved && FindFollow(camera.follow)==camera.followBehavior) {
            static_cast<void>(Write(camera.followBehavior+0x9CU,camera.originalFollow));
            static_cast<void>(Write(camera.followBehavior+0xF6U,camera.originalFirstPerson));
        }
        // Retarget without activating: a cinematic may currently own the
        // camera manager, and we must never steal its view on PvP exit.
        static_cast<void>(AttachHeadBone(camera.head,camera.technical));
        static_cast<void>(RetargetCamera(gCameraBase,camera.head,camera.technical));
        static_cast<void>(RetargetCamera(gCameraBase,camera.follow,camera.technical));
        if (Read(camera.manager+0x1D8U,active) &&
            (active==camera.follow || active==camera.head))
            static_cast<void>(CameraCall(gCameraBase,camera.manager,
                camera.previous==camera.head?camera.head:camera.follow,camera.technical));
    }
    gCameraBase=0U;
}
bool BindCamera(const Context& context,const std::uint64_t now) noexcept {
    if (gLocal.camera.bound) return true;
    if (now<gLocal.nextCameraAttemptMs || !LivingCameraTarget(context.alien) ||
        !LivingCameraTarget(context.technicalPlayer)) return false;
    gLocal.nextCameraAttemptMs=now+1000U;
    CameraBinding camera{};
    if (!CurrentCameraRoot(context.moduleBase,camera)) return false;
    // A cinematic, loading camera or a third-party view is never hijacked.
    if (camera.previous!=camera.follow && camera.previous!=camera.head) return false;
    camera.technical=context.technicalPlayer;
    camera.alien=context.alien;
    if (!Read(camera.technical+0x50U,camera.technicalRuntime) ||
        !Read(camera.technical+kCharacterSkeletonOffset,camera.technicalSkeleton) ||
        !Read(camera.alien+0x50U,camera.alienRuntime) ||
        !Read(camera.alien+kCharacterSkeletonOffset,camera.alienSkeleton) ||
        !camera.technicalRuntime || !camera.technicalSkeleton || !camera.alienRuntime || !camera.alienSkeleton) return false;
    // Donor's parking requires a living, non-interacting Amanda. Capture home
    // before retargeting the camera; never use the Alien position as her home.
    if (!gLocal.parking.Parked() && camera.previous==camera.follow) {
        float eyes{},feet{};
        if(!Read(camera.follow+0x18U,eyes) || !Read(camera.technical+0x74U,feet) ||
            !std::isfinite(eyes) || !std::isfinite(feet))return false;
        const float height=eyes-feet;
        if(height>0.2F && height<1.3F)return false; // donor: crouched restore can fall through floor
    }
    if (!PrepareTechnicalHome(context)) return false;
    camera.generation=context.nativeGeneration;camera.touched=true;
    gLocal.camera=camera;gCameraBase=context.moduleBase; // record partial retarget for rollback
    if(!ConfigureFollow(gLocal.camera,false)) {RestoreLocalCamera(true);return false;}
    camera=gLocal.camera;
    if (!CameraCall(context.moduleBase,camera.manager,camera.follow,context.alien)) {
        RestoreLocalCamera(true);return false;
    }
    camera.bound=true;
    gLocal.camera=camera;
    gCameraBase=context.moduleBase;
    return true;
}
bool TechnicalIdentityCurrent(const CameraBinding& camera) noexcept {
    std::uintptr_t skeleton{};
    std::uint32_t runtime{};
    return camera.technical!=0U && camera.technicalRuntime!=0U &&
        Read(camera.technical+0x50U,runtime) && runtime==camera.technicalRuntime &&
        Read(camera.technical+kCharacterSkeletonOffset,skeleton) &&
        skeleton==camera.technicalSkeleton;
}
bool GameFocused() noexcept {
    DWORD process{};
    const auto foreground=GetForegroundWindow();
    return foreground!=nullptr && GetWindowThreadProcessId(foreground,&process)!=0U &&
        process==GetCurrentProcessId();
}
bool InputAllowed(const std::uintptr_t base) noexcept {
    if (!GameFocused()) return false;
    // Includes the 0x401 Pause_Game + pause_menu case the old 0x12000-only
    // test missed (AvS 2026-11-24: Alien could not quit/disconnect).
    return multiplayer_pause::MenuOpen(base)==0U;
}
bool CameraStillOwned(const Context& context) noexcept {
    CameraBinding current{};
    std::uintptr_t active{};
    return gLocal.camera.bound && CurrentCameraRoot(context.moduleBase,current) &&
        current.root==gLocal.camera.root && current.manager==gLocal.camera.manager &&
        Read(current.manager+0x1D8U,active) &&
        (active==gLocal.camera.follow || active==gLocal.camera.head);
}
bool Down(const int key) noexcept { return (GetAsyncKeyState(key)&0x8000)!=0; }
bool VentRead(std::uintptr_t at,void* out,std::size_t size) noexcept {
    return at && crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
}
// Round 10: the vent's native ExitPoint (descriptor thiscall 0021CCC0, the same
// call SelectPortal uses on both PCs). Opening -> ExitPoint is the vent's own
// through-the-wall axis; the picker camera uses it to face into the room.
bool VentExitPoint(const alien_pvp::vents::Table& table,std::uint32_t index,std::array<float,3>& out) noexcept {
    std::uintptr_t descriptor{};
    if(!gCameraBase || index>=table.count ||
        !VentRead(table.entries+index*0x30U+0x28U,&descriptor,sizeof(descriptor)) || !descriptor)return false;
    alignas(16) float p[4]{};
#if defined(_MSC_VER)
    __try {
        using Fn=void*(__thiscall*)(void*,void*);
        reinterpret_cast<Fn>(gCameraBase+0x0021CCC0U)(reinterpret_cast<void*>(descriptor),p);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
#else
    return false;
#endif
    for(unsigned i=0;i<3;++i)if(!std::isfinite(p[i]) || std::abs(p[i])>100000.0F)return false;
    out={p[0],p[1],p[2]};return true;
}
bool UpdateVentInterface(const Context& context,alien_pvp::vents::Table& table) noexcept {
    const bool backstage=IsBackstage(context.alien);
    gLocal.ventMapOpen=false;
    if(backstage && !gLocal.freeCamera) {
        gLocal.freeEye=gLocal.lastEye;
        if(!gLocal.eyeKnown && !Read(context.alien+0x70U,gLocal.freeEye))return false;
        gLocal.freeCamera=true;
    }
    if(!backstage && !alien_pvp::VentPending())gLocal.freeCamera=false;
    gLocal.wasBackstage=backstage;gLocal.nearbyVent=-1;
    if(!alien_pvp::vents::ReadTable(context.alien,&VentRead,table)){gVentTableCount.store(0U,std::memory_order_relaxed);return false;}
    alien_pvp::vents::Point body{};float best=100000.0F,nearest=100000.0F;int closest=-1;
    if(!Read(context.alien+0x70U,body))return false;
    if(gLocal.freeCamera)body=gLocal.freeEye;
    // AvS 2026-11-25 round 5: E picked the straight-line nearest vent even
    // through a wall; the native node then ran the Alien ~11 m around to its
    // approach and never opened it. On foot, only offer a vent the Alien can
    // see (chest -> just short of the opening, no physics hit).
    std::array<std::uintptr_t,4> worlds{};
    const unsigned worldCount=gLocal.freeCamera?0U:CurrentWorlds(worlds);
    bool anyInReach{};float bestBlocked=100000.0F;int blockedVent=-1;
    gVentTableCount.store(table.count,std::memory_order_relaxed);
    gVentWorlds.store(worldCount,std::memory_order_relaxed);
    struct InReachMark {const bool& any;~InReachMark(){if(any)gVentInReach.fetch_add(1U,std::memory_order_relaxed);}} mark{anyInReach};
    for(std::uint32_t i=0;i<table.count;++i) {
        alien_pvp::vents::Point point{};
        if(!alien_pvp::vents::ReadPoint(table,i,&VentRead,point))continue;
        const float distance=std::hypot(point[0]-body[0],point[2]-body[2])+std::abs(point[1]-body[1])*0.7F;
        if(distance<nearest){nearest=distance;closest=static_cast<int>(i);}
        bool reachable=gLocal.freeCamera?
            std::hypot(std::hypot(point[0]-body[0],point[2]-body[2]),point[1]-body[1])<=3.0F:
            alien_pvp::vents::WithinReach(body,point);
        if(reachable)anyInReach=true;
        bool visible=true;
        if(reachable && distance<best && worldCount) {
            // Round 7: the round-5 ray (stopping 0.45 m short) hid every vent:
            // the opening point sits behind its own grille/frame. Only a hit
            // well clear of the vent (a real wall between) hides it now; the
            // retail path-aware entry and the 2.5 m growth abort still catch
            // a vent on the far side of a thin wall.
            // Round 8: every frame hit ~2.1 m short of the vent with the Alien
            // inside 2.75 m, i.e. at the ray start (the Alien's own body).
            // Hits within kVentSelfClear of the start are ignored.
            const std::array<float,3> from{body[0],body[1]+1.0F,body[2]};
            float gap=-1.0F;
            for(unsigned w=0;w<worldCount;++w) {
                std::array<float,3> hit{};
                if(!Ray(worlds[w],from,point,hit))continue;
                const float fromStart=std::sqrt((hit[0]-from[0])*(hit[0]-from[0])+(hit[1]-from[1])*(hit[1]-from[1])+
                    (hit[2]-from[2])*(hit[2]-from[2]));
                gVentHitFromStart.store(fromStart,std::memory_order_relaxed);
                if(fromStart<kVentSelfClear){gVentSelfHits.fetch_add(1U,std::memory_order_relaxed);continue;}
                gap=std::max(gap,std::sqrt((hit[0]-point[0])*(hit[0]-point[0])+(hit[1]-point[1])*(hit[1]-point[1])+
                    (hit[2]-point[2])*(hit[2]-point[2])));
            }
            if(gap>kVentFaceTolerance) {
                visible=false;gVentHidden.fetch_add(1U,std::memory_order_relaxed);
                gVentHiddenGap.store(gap,std::memory_order_relaxed);
            } else if(gap>=0.0F) {
                gVentFaceHits.fetch_add(1U,std::memory_order_relaxed);
                gVentFaceGap.store(gap,std::memory_order_relaxed);
            }
        }
        // Visibility ranks, it never removes E: a blocked vent in reach is
        // still offered when no visible one is (path-aware entry and the
        // 2.5 m growth abort remain the wall guards).
        if(reachable && visible && distance<best){best=distance;gLocal.nearbyVent=static_cast<int>(i);}
        if(reachable && distance<bestBlocked){bestBlocked=distance;blockedVent=static_cast<int>(i);}
    }
    if(gLocal.nearbyVent<0 && blockedVent>=0){gLocal.nearbyVent=blockedVent;gVentBlockedOffered.fetch_add(1U,std::memory_order_relaxed);}
    alien_pvp::vents::Point selected{};
    if(gLocal.selectedVent<0 || !alien_pvp::vents::ReadPoint(table,static_cast<std::uint32_t>(gLocal.selectedVent),&VentRead,selected))
        gLocal.selectedVent=closest;
    return true;
}
// Rank exits: the two nearest each survivor (closest survivor first), then the
// exit nearest the Alien itself as a "leave here" fallback. Vent indices only;
// positions are re-read from the native table on every use.
void BuildVentPicker(const Context& context,const alien_pvp::vents::Table& table,std::uint64_t now) noexcept {
    const int previous=gLocal.pickerSlot>=0 && gLocal.pickerSlot<gLocal.pickerCount?gLocal.pickerVents[gLocal.pickerSlot]:-1;
    struct Entry {int vent;float score;};
    std::array<Entry,8> list{};int count{};
    const auto add=[&](int vent,float score) noexcept {
        if(vent<0)return;
        for(int i=0;i<count;++i)if(list[i].vent==vent){list[i].score=std::min(list[i].score,score);return;}
        if(count<static_cast<int>(list.size()))list[count++]={vent,score};
    };
    const auto nearest=[&](const float* from,int skip,float& best) noexcept {
        int found=-1;best=1.0e9F;
        for(std::uint32_t i=0;i<table.count;++i) {
            if(static_cast<int>(i)==skip)continue;
            alien_pvp::vents::Point p{};
            if(!alien_pvp::vents::ReadPoint(table,i,&VentRead,p))continue;
            const float d=std::hypot(p[0]-from[0],p[2]-from[2])+std::abs(p[1]-from[1])*1.5F;
            if(d<best){best=d;found=static_cast<int>(i);}
        }
        return found;
    };
    for(std::uint32_t s=0;s<context.survivorCount && s<4U;++s) {
        float d1{},d2{};
        const int first=nearest(context.survivors[s],-1,d1);
        add(first,d1);
        add(nearest(context.survivors[s],first,d2),d2);
    }
    float own{};alien_pvp::vents::Point body{};
    if(Read(context.alien+0x70U,body))add(nearest(body.data(),-1,own),1.0e6F+own);
    std::sort(list.begin(),list.begin()+count,[](const Entry& a,const Entry& b){return a.score<b.score;});
    gLocal.pickerCount=count;gLocal.pickerSlot=count?0:-1;
    for(int i=0;i<count;++i) {
        gLocal.pickerVents[i]=list[i].vent;
        if(list[i].vent==previous)gLocal.pickerSlot=i;
    }
    gLocal.pickerBuiltAt=now;
}
// Weapon/health HUD of the parked technical survivor ("the Alien has a pistol
// in its UI", AvS 2026-11-25). Donor alien_extra.cpp: the game's own
// force_hud_hidden/visible on the "weaponstuff" overlay, +40 = forced flag.
// "pickupOverlay" is NOT touched: it hosts AIMP's vent bar and prompts.
void SetGameHudHidden(const std::uintptr_t base,const bool hidden) noexcept {
    std::uintptr_t manager{},movies{};std::uint32_t count{};
    if(!base || !Read(base+0x0134A78CU,manager) || !manager || !Read(manager+0x38U,movies) || !movies ||
        !Read(manager+0x3CU,count) || count>256U)return;
    constexpr char kName[]="weaponstuff";
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t movie{},name{},view{},root{};
        if(!Read(movies+i*4U,movie) || !movie || !Read(movie+0x34U,name) || !name)continue;
        bool same=true;
        for(std::size_t k=0;k<sizeof(kName) && same;++k){char ch{};same=Read(name+k,ch) && ch==kName[k];}
        if(!same || !Read(movie+8U,view) || !view || !Read(movie+0x68U,root) || !root)continue;
        std::uint8_t forced{};
        if(hidden && Read(movie+0x40U,forced) && forced)continue; // still hidden
#if defined(_MSC_VER)
        __try {
            reinterpret_cast<void(__thiscall*)(void*,const char*)>(base+0x004081E0U)(
                reinterpret_cast<void*>(movie),hidden?"force_hud_hidden":"force_hud_visible");
        } __except(EXCEPTION_EXECUTE_HANDLER) {continue;}
#endif
        static_cast<void>(Write(movie+0x40U,std::uint8_t{hidden?1U:0U}));
    }
}
// ---- AvS round 10: crawlspaces (C to enter, first person inside, auto-exit) ----
struct CrawlState final {
    bool duct{},cDown{};unsigned run{};std::uint64_t at{},entryUntil{};
} gCrawl{}; // engine thread only
std::atomic<std::uint64_t> gCrawlEntries{},gCrawlDuctToggles{},gCrawlFirstPersonOn{},gCrawlFirstPersonFailed{},gCrawlFrames{};
std::atomic<bool> gCrawlDuct{};
std::atomic<float> gCrawlDuctX{},gCrawlDuctZ{};
bool SetFirstPerson(const Context& c,bool on) noexcept {
    auto& camera=gLocal.camera;
    if(!camera.bound)return false;
    if(camera.firstPerson==on)return true;
    if(on) {
        if(!camera.head || !AttachHeadBone(camera.head,c.alien) ||
            !CameraCall(c.moduleBase,camera.manager,camera.head,c.alien))return false;
        camera.firstPerson=true;
        static_cast<void>(alien_pvp_visual::HideAlienHead(c.alien,c.nativeGeneration));
        gCrawlFirstPersonOn.fetch_add(1U,std::memory_order_relaxed);
    } else {
        if(!CameraCall(c.moduleBase,camera.manager,camera.follow,c.alien))return false;
        camera.firstPerson=false;
        alien_pvp_visual::RestoreAlienHead(c.alien,c.nativeGeneration,true);
    }
    return true;
}
void TickCrawl(const Context& c,std::uint64_t now,bool focused,bool pendingVent,alien_pvp::LocalInput& input) noexcept {
    const bool backstage=IsBackstage(c.alien);
    std::array<float,3> body{};
    const bool haveBody=Read(c.alien+0x70U,body) && std::isfinite(body[0]) && std::isfinite(body[1]) && std::isfinite(body[2]);
    // Low ceiling over the Alien = inside a crawl duct (donor DuctUpdate ray,
    // 1.4 m instead of 1.9 m so a low pipe/table does not count). Own test:
    // the follow-camera hook that feeds gLocal.duct stops in first person.
    if(haveBody && !backstage && now-gCrawl.at>=150U) {
        gCrawl.at=now;
        std::array<std::uintptr_t,4> worlds{};const unsigned count=CurrentWorlds(worlds);
        if(count) {
            bool low{};
            for(unsigned i=0;i<count && !low;++i) {
                std::array<float,3> hit{};
                low=Ray(worlds[i],{body[0],body[1]+0.4F,body[2]},{body[0],body[1]+1.4F,body[2]},hit);
            }
            if(low==gCrawl.duct)gCrawl.run=0U;
            else if(++gCrawl.run>=4U) {
                gCrawl.duct=low;gCrawl.run=0U;
                gCrawlDuctToggles.fetch_add(1U,std::memory_order_relaxed);
                gCrawlDuct.store(low,std::memory_order_relaxed);
                gCrawlDuctX.store(body[0],std::memory_order_relaxed);gCrawlDuctZ.store(body[2],std::memory_order_relaxed);
            }
        }
    }
    if(backstage && gCrawl.duct){gCrawl.duct=false;gCrawl.run=0U;gCrawlDuct.store(false,std::memory_order_relaxed);}
    const bool cKey=focused && Down('C');
    if(cKey && !gCrawl.cDown && !backstage && !pendingVent && !gLocal.freeCamera && !gCrawl.duct) {
        gCrawl.entryUntil=now+3000U;gCrawlEntries.fetch_add(1U,std::memory_order_relaxed);
    }
    gCrawl.cDown=cKey;
    if(gCrawl.duct)gCrawl.entryUntil=0U; // inside: ordinary WASD steering
    const bool entering=now<gCrawl.entryUntil;
    const bool crawl=!backstage && !pendingVent && !gLocal.freeCamera && (gCrawl.duct || entering);
    // C at a hatch: push straight ahead into it for up to 3 s; the retail path
    // move plays the climb-in through the hatch link.
    if(entering && input.forward==0.0F && input.strafe==0.0F)input.forward=1.0F;
    input.crawl=crawl;
    if(crawl)gCrawlFrames.fetch_add(1U,std::memory_order_relaxed);
    // Pass 10: never stay in the head camera while a menu is up. Restart and
    // quit go through the pause menu, and a head camera still on the Alien when
    // the level is torn down crashed the game (AI.exe+0x62C011, "Head1").
    const bool menuOpen=multiplayer_pause::MenuOpen(c.moduleBase)!=0U;
    // Pass 14 (avsfix13 log: firstPersonOn == headReleased, +1 per second):
    // activating the retail head camera on the Alien made the next Pump lose
    // camera ownership, hand the view (and input) back to the hidden human and
    // rebind 1 s later, a human/Alien flicker. Crawlspaces now use the donor's
    // close duct follow camera (DuctUpdate offsets) and never the head camera.
    gCrawlClose=crawl && gCrawl.duct;
    const bool wantFirstPerson=false;
    if(gLocal.camera.bound && wantFirstPerson!=gLocal.camera.firstPerson) {
        if(!SetFirstPerson(c,wantFirstPerson))gCrawlFirstPersonFailed.fetch_add(1U,std::memory_order_relaxed);
        else if(menuOpen)gCrawlMenuExits.fetch_add(1U,std::memory_order_relaxed);
    }
}
// ---- AvS round 10: flamethrower fear, fire resistance, shotgun stun ----
// Runs on the Alien player's PC from the survivors' replicated weapon/trigger
// state (works whether the Alien player hosts or joins; no wire change).
alien_pvp::fear::State gFear{}; // engine thread only
std::uint32_t gFearGeneration{};
std::array<bool,4> gSurvivorWasFiring{};
struct RetreatRun final {
    bool sent{},sawPending{};std::uint64_t armedAt{},sentAt{},recoilMs{400U};unsigned tries{};
    std::array<int,3> tried{-1,-1,-1};bool haveFlamer{};float flamerX{},flamerZ{};
} gRetreat{};
std::atomic<std::uint64_t> gFearExitRefused{},gFearRetreatSent{},gFearRetreatAbandoned{},gFearFlamedFrames{},gFearThreatFrames{};
// Pass 20: kill clicks swallowed because the only survivor in reach hides in a locker.
std::atomic<std::uint64_t> gLockerClicks{};
std::atomic<float> gFearMeter{100.0F},gFearResistance{},gFearRetreatVentM{-1.0F},gFearFlamerAngle{-1.0F};
std::atomic<std::uint32_t> gFearPhase{},gFearRetreats{},gFearFlinches{},gFearStuns{},gFearBlockedClicks{},gFearChaseRetreats{};
std::atomic<std::uint32_t> gFearSurvivorWeapons{};
std::atomic<std::uint64_t> gFearReactions{},gFearStunRoars{},gSurvivorStunned{},gSurvivorStunMissed{};
std::uint64_t gSurvivorStunUntil{}; // engine thread only (survivor PC)
// Survivor PC: a stun roar from the Alien player. Stunned when this PC's
// player is within 8 m of the Alien and inside +-0.6 rad of the roar yaw.
void TickSurvivorStun(const Context& c,std::uint64_t now) noexcept {
    float yaw{};
    if(alien_pvp::TakeStunRoar(yaw) && c.selfPlayer && c.alienAny) {
        std::array<float,3> a{},p{};
        bool hit{};
        if(Read(c.alienAny+0x70U,a) && Read(c.selfPlayer+0x70U,p)) {
            const float dx=p[0]-a[0],dz=p[2]-a[2],d=std::hypot(dx,dz);
            float off=std::atan2(dx,dz)-yaw;
            while(off>3.14159265F)off-=6.28318531F;
            while(off<-3.14159265F)off+=6.28318531F;
            hit=d<=8.0F && std::abs(p[1]-a[1])<2.5F && std::abs(off)<=0.6F;
        }
        if(hit){gSurvivorStunUntil=now+1500U;gSurvivorStunned.fetch_add(1U,std::memory_order_relaxed);}
        else gSurvivorStunMissed.fetch_add(1U,std::memory_order_relaxed);
    }
} // 4 bits per survivor: weapon | aim<<2 | firing<<3
// Retail voice (donor ActAlienVoice): audio component +11A0, thiscall
// 00784230(type); 4 = burned, 6 = hurt. Local to the Alien player's PC.
void PlayAlienVoice(const Context& c,int type) noexcept {
#if defined(_MSC_VER)
    __try {
        reinterpret_cast<void(__thiscall*)(void*,int)>(c.moduleBase+0x00384230U)(reinterpret_cast<void*>(c.alien+0x11A0U),type);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
#endif
}
// Pick the retreat vent: within 25 m, preferring vents away from the flamer.
bool SendRetreat(const Context& c,const alien_pvp::vents::Table& vents,const std::array<float,3>& a,
    const alien_pvp::LocalInput& steering,std::uint64_t now) noexcept {
    int best=-1;float bestScore=1.0e9F,bestDistance{};alien_pvp::vents::Point bestPoint{};
    for(std::uint32_t i=0;i<vents.count;++i) {
        if(std::find(gRetreat.tried.begin(),gRetreat.tried.end(),static_cast<int>(i))!=gRetreat.tried.end())continue;
        alien_pvp::vents::Point p{};
        if(!alien_pvp::vents::ReadPoint(vents,i,&VentRead,p))continue;
        const float dx=p[0]-a[0],dz=p[2]-a[2],dy=p[1]-a[1],d=std::hypot(dx,dz);
        if(d>alien_pvp::fear::kRetreatReach || dy<-3.0F || dy>6.0F)continue;
        float toward{};
        if(gRetreat.haveFlamer && d>0.1F) {
            const float fx=gRetreat.flamerX-a[0],fz=gRetreat.flamerZ-a[2],fd=std::hypot(fx,fz);
            if(fd>0.1F)toward=(dx*fx+dz*fz)/(d*fd);
        }
        const float score=d+8.0F*std::max(0.0F,toward);
        if(score<bestScore){bestScore=score;best=static_cast<int>(i);bestDistance=d;bestPoint=p;}
    }
    if(best<0)return false;
    alien_pvp::LocalInput r=steering;
    r.forward=r.strafe=0.0F;r.focused=true;r.action=alien_pvp::Action::EnterVent;r.selectedVent=true;
    r.ventX=bestPoint[0];r.ventY=bestPoint[1];r.ventZ=bestPoint[2];r.pitch=alien_pvp::fear::kRetreatPitch;
    alien_pvp::CaptureLocal(r);
    gRetreat.tried[gRetreat.tries%gRetreat.tried.size()]=best;
    gRetreat.sent=true;gRetreat.sawPending=false;gRetreat.sentAt=now;
    gFearRetreatSent.fetch_add(1U,std::memory_order_relaxed);
    gFearRetreatVentM.store(bestDistance,std::memory_order_relaxed);
    static_cast<void>(c);
    return true;
}
// Pass 16 diagnostic (avsfix14: "hands up" when a survivor raised the
// flamethrower, but no flinch/reaction of ours and no cycle 87 in the 10 Hz
// locomotion log). Every change of the pilot Alien's primary action state
// (cycle+1A) with its time; oddCycles = changes into a non-locomotion state.
constexpr std::uint32_t kCycleTrail=12U;
std::atomic<std::uint32_t> gTrailState[kCycleTrail]{},gTrailHead{};
std::atomic<std::uint64_t> gTrailAt[kCycleTrail]{},gOddCycles{};
std::uint8_t gTrailLast{0xFFU};
void TraceCycle(std::uintptr_t alien,std::uint64_t now) noexcept {
    std::uintptr_t cycle{};std::uint8_t state{};
    if(!Read(alien+0x36CU,cycle) || !cycle || !Read(cycle+0x1AU,state) || state==gTrailLast)return;
    gTrailLast=state;
    const auto at=gTrailHead.load(std::memory_order_relaxed)%kCycleTrail;
    gTrailState[at].store(state,std::memory_order_relaxed);gTrailAt[at].store(now,std::memory_order_relaxed);
    gTrailHead.fetch_add(1U,std::memory_order_release);
    if(state!=1U && state!=55U && state!=56U && state!=57U && state!=59U)gOddCycles.fetch_add(1U,std::memory_order_relaxed);
}
void TickFear(const Context& c,const alien_pvp::vents::Table& vents,bool haveVents,std::uint64_t now,float dt,
    bool pendingVent,const alien_pvp::LocalInput& steering) noexcept {
    using alien_pvp::fear::Phase;
    TraceCycle(c.alien,now);
    if(c.nativeGeneration!=gFearGeneration) {
        // New match/level: meter full, resistance back to 0.
        gFear.Reset();gFearGeneration=c.nativeGeneration;gSurvivorWasFiring={};gRetreat={};
    }
    std::array<float,3> a{};
    if(!Read(c.alien+0x70U,a) || !std::isfinite(a[0]) || !std::isfinite(a[2]))return;
    const bool backstage=IsBackstage(c.alien);
    bool flamed{},threat{},shot{},chasing{};int flamer=-1;std::uint32_t packed{};
    for(std::uint32_t s=0;s<c.survivorCount && s<4U;++s) {
        const alien_pvp::fear::Survivor sv{c.survivors[s][0],c.survivors[s][1],c.survivors[s][2],c.survivorYaw[s],
            c.survivorWeapon[s],c.survivorAim[s],c.survivorFiring[s]};
        packed|=(static_cast<std::uint32_t>(sv.weapon&3U)|(sv.aim?4U:0U)|(sv.firing?8U:0U))<<(s*4U);
        if(!backstage) {
            if(alien_pvp::fear::Flamed(sv,a[0],a[1],a[2])){flamed=true;flamer=static_cast<int>(s);}
            if(alien_pvp::fear::Threatening(sv,a[0],a[1],a[2]))threat=true;
            if(sv.firing && !gSurvivorWasFiring[s] && alien_pvp::fear::ShotgunAimed(sv,a[0],a[1],a[2]))shot=true;
            if(sv.weapon==1U)gFearFlamerAngle.store(alien_pvp::fear::AngleTo(sv,a[0],a[2]),std::memory_order_relaxed);
            // Chase: sprinting at this survivor within 10 m.
            const float dx=sv.x-a[0],dz=sv.z-a[2],d=std::hypot(dx,dz);
            if(steering.gait==3U && steering.forward>0.0F && d<10.0F && d>0.1F) {
                float off=std::atan2(dx,dz)-steering.yaw;
                while(off>3.14159265F)off-=6.28318531F;
                while(off<-3.14159265F)off+=6.28318531F;
                if(std::abs(off)<0.6F)chasing=true;
            }
        }
        gSurvivorWasFiring[s]=sv.firing;
    }
    if(alien_pvp::NonVentActionBusy())chasing=true; // an attack in flight
    gFearSurvivorWeapons.store(packed,std::memory_order_relaxed);
    if(flamed)gFearFlamedFrames.fetch_add(1U,std::memory_order_relaxed);
    if(threat)gFearThreatFrames.fetch_add(1U,std::memory_order_relaxed);
    const float roll=static_cast<float>(((now*2654435761ULL)>>7U)%1000ULL)/1000.0F;
    // Retail reaction actions (kind-5 request, action type = cycle number):
    // 80 AT_IMPACT_STUMBLE flinch, 86 AT_MULTI_STAGE_STUNNED, 87 AT_AFFECTED_BY_FLAME_THROWER.
    if(gFear.Step(now,dt,flamed,threat,backstage,chasing,roll)) {
        PlayAlienVoice(c,4);
        if(alien_pvp::PlayReaction(80U,600U))gFearReactions.fetch_add(1U,std::memory_order_relaxed);
    }
    if(shot && gFear.Shot(now)) {
        PlayAlienVoice(c,6);
        if(alien_pvp::PlayReaction(86U,1000U))gFearReactions.fetch_add(1U,std::memory_order_relaxed);
    }
    if(gFear.retreatRequested) {
        gFear.retreatRequested=false;gRetreat={};gRetreat.armedAt=now;
        if(flamer>=0){gRetreat.haveFlamer=true;gRetreat.flamerX=c.survivors[flamer][0];gRetreat.flamerZ=c.survivors[flamer][2];}
        PlayAlienVoice(c,4);
        // Recoil first (the run to the vent waits for it), then run.
        if(alien_pvp::PlayReaction(87U,1400U)){gRetreat.recoilMs=1400U;gFearReactions.fetch_add(1U,std::memory_order_relaxed);}
        if(gFear.chaseRetreat && flamer>=0) {
            // Flamed during a chase: roar at the flamer; their PC stuns them.
            alien_pvp::LocalInput roar=steering;
            roar.forward=roar.strafe=0.0F;roar.focused=true;roar.action=alien_pvp::Action::Roar;
            roar.selectedVent=false;roar.pitch=1.5F;
            roar.yaw=alien_pvp::WrapYaw(std::atan2(c.survivors[flamer][0]-a[0],c.survivors[flamer][2]-a[2]));
            alien_pvp::CaptureLocal(roar);gFearStunRoars.fetch_add(1U,std::memory_order_relaxed);
        }
    }
    // Retreat run: after a short recoil beat, enter the chosen vent (the Host
    // accepts a far vent for a retreat and the native vent node paths there).
    if(gFear.phase==Phase::Retreating && !backstage) {
        if(pendingVent)gRetreat.sawPending=true;
        else if(now-gRetreat.armedAt>=gRetreat.recoilMs) {
            const bool waiting=gRetreat.sent && !gRetreat.sawPending && now-gRetreat.sentAt<1500U;
            if(!waiting) {
                if(gRetreat.sent)++gRetreat.tries;
                if(gRetreat.tries>=3U || !haveVents || !SendRetreat(c,vents,a,steering,now)) {
                    gFear.RetreatAbandoned();gFearRetreatAbandoned.fetch_add(1U,std::memory_order_relaxed);
                }
            }
        }
    }
    gFearMeter.store(gFear.meter,std::memory_order_relaxed);
    gFearResistance.store(gFear.resistance,std::memory_order_relaxed);
    gFearPhase.store(static_cast<std::uint32_t>(gFear.phase),std::memory_order_relaxed);
    gFearRetreats.store(gFear.retreats,std::memory_order_relaxed);
    gFearFlinches.store(gFear.flinches,std::memory_order_relaxed);
    gFearStuns.store(gFear.stuns,std::memory_order_relaxed);
    gFearBlockedClicks.store(gFear.blockedClicks,std::memory_order_relaxed);
    gFearChaseRetreats.store(gFear.chaseRetreats,std::memory_order_relaxed);
}
void TickInput(const Context& context,const std::uint64_t now) noexcept {
    if (now==gLocal.lastFrameMs) return;
    const float dt=gLocal.lastFrameMs && now>=gLocal.lastFrameMs?
        std::min(0.05F,static_cast<float>(now-gLocal.lastFrameMs)/1000.0F):0.016F;
    gLocal.lastFrameMs=now;
    const bool focused=InputAllowed(context.moduleBase);
    const auto authority=alien_pvp::VentAuthority();
    const bool pendingVent=(authority.action==alien_pvp::Action::EnterVent || authority.action==alien_pvp::Action::ExitVent) &&
        (authority.phase==alien_pvp::ActionPhase::Preparing || authority.phase==alien_pvp::ActionPhase::Started);
    const unsigned moveMask=focused?
        (Down('W')?1U:0U)|(Down('A')?2U:0U)|(Down('S')?4U:0U)|(Down('D')?8U:0U):0U;
    if(pendingVent && !gLocal.ventWasPending) {
        gLocal.ventPendingAt=now;gLocal.ventCancelSent=false;gLocal.ventMoveMask=moveMask;
    }
    if(!pendingVent)gLocal.ventCancelSent=false;
    // A key released during the pending action may cancel when pressed again.
    const unsigned freshMove=moveMask&~gLocal.ventMoveMask;
    gLocal.ventMoveMask=moveMask;
    gLocal.ventWasPending=pendingVent;
    alien_pvp::vents::Table vents{};
    const bool haveVents=UpdateVentInterface(context,vents);
    static_cast<void>(haveVents);
    const auto wheel=alien_pvp_input::TakeCameraWheel();
    const bool middle=focused && Down(VK_MBUTTON);
    if(focused && gLocal.camera.bound && !gLocal.camera.firstPerson && !gLocal.freeCamera) {
        if(middle && !gLocal.middleDown) {
            gLocal.cameraDistance=3.0F;gLocal.cameraHeight=2.0F;gLocal.cameraSide=0.5F;
        }
        const float steps=static_cast<float>(wheel)/120.0F;
        if(Down(VK_SHIFT))gLocal.cameraHeight=std::clamp(gLocal.cameraHeight+steps*0.1F,0.3F,4.5F);
        else if(Down(VK_CONTROL))gLocal.cameraSide=std::clamp(gLocal.cameraSide+steps*0.1F,-2.0F,2.0F);
        else gLocal.cameraDistance=std::clamp(gLocal.cameraDistance-steps*0.25F,1.0F,8.0F);
    }
    gLocal.middleDown=middle;
    int forward=0,side=0;
    if (focused && gLocal.camera.bound && !gLocal.freeCamera && !pendingVent) {
        forward=static_cast<int>(Down('W'))-static_cast<int>(Down('S'));
        side=static_cast<int>(Down('D'))-static_cast<int>(Down('A'));
    }
    std::uint32_t gait=1U;
    if (focused && Down(VK_SPACE)) gait=3U;
    else if (focused && Down(VK_SHIFT)) gait=2U;
    else if (focused && Down(VK_CONTROL)) gait=0U;
    float yaw{};
    if (!Read(context.technicalPlayer+0x174U,yaw) || !std::isfinite(yaw))
        if (!Read(context.alien+0x174U,yaw) || !std::isfinite(yaw)) yaw=0.0F;
    alien_pvp::LocalInput input{};
    input.forward=static_cast<float>(forward);input.strafe=static_cast<float>(side);
    input.yaw=alien_pvp::WrapYaw(yaw);input.gait=gait;input.focused=focused;
    TickCrawl(context,now,focused,pendingVent,input);
    TickFear(context,vents,haveVents,now,dt,pendingVent,input);
    if(gFear.Stunned(now) || gFear.phase==alien_pvp::fear::Phase::Retreating)input.forward=input.strafe=0.0F;
    // AvS 2026-11-24: during an exit the screen sat on "Exiting ventilation..."
    // at the spot where E was pressed while the Alien crawled the duct for up to
    // 25 s. Chase the real (backstage) Alien body instead; FollowAfter keeps this
    // look direction rather than the mouse orbit while tracking.
    gLocal.exitTracking=gLocal.freeCamera && authority.action==alien_pvp::Action::ExitVent &&
        authority.phase==alien_pvp::ActionPhase::Started;
    if(gLocal.exitTracking) {
        std::array<float,3> body{};float facing{};
        if(Read(context.alien+0x70U,body) && std::isfinite(body[0]) && std::isfinite(body[1]) && std::isfinite(body[2])) {
            if(!Read(context.alien+0x174U,facing) || !std::isfinite(facing))facing=yaw;
            const std::array<float,3> desired{body[0]-std::sin(facing)*2.8F,body[1]+1.6F,body[2]-std::cos(facing)*2.8F};
            const float blend=std::min(1.0F,dt*6.0F);
            const float jump=std::hypot(std::hypot(desired[0]-gLocal.freeEye[0],desired[2]-gLocal.freeEye[2]),desired[1]-gLocal.freeEye[1]);
            for(unsigned i=0;i<3;++i)gLocal.freeEye[i]=jump>20.0F?desired[i]:gLocal.freeEye[i]+(desired[i]-gLocal.freeEye[i])*blend;
            const std::array<float,3> look{body[0]-gLocal.freeEye[0],body[1]+0.6F-gLocal.freeEye[1],body[2]-gLocal.freeEye[2]};
            const float length=std::sqrt(look[0]*look[0]+look[1]*look[1]+look[2]*look[2]);
            if(length>0.05F)for(unsigned i=0;i<3;++i)gLocal.freeForward[i]=look[i]/length;
        }
        input.forward=input.strafe=0.0F;
    } else if(gLocal.freeCamera && alien_pvp::ControlActive()) {
        // Vent-exit picker. Never flies: the eye only ever sits on a real exit.
        if(haveVents && (!gLocal.pickerCount || now-gLocal.pickerBuiltAt>=1000U))
            BuildVentPicker(context,vents,now);
        const bool prev=focused && (Down('A') || Down(VK_LEFT));
        const bool next=focused && (Down('D') || Down(VK_RIGHT));
        if(gLocal.pickerCount>0) {
            if(prev && !gLocal.pickPrev)gLocal.pickerSlot=(gLocal.pickerSlot+gLocal.pickerCount-1)%gLocal.pickerCount;
            if(next && !gLocal.pickNext)gLocal.pickerSlot=(gLocal.pickerSlot+1)%gLocal.pickerCount;
            if(gLocal.pickerSlot<0)gLocal.pickerSlot=0;
            alien_pvp::vents::Point exit{};
            const int vent=gLocal.pickerVents[gLocal.pickerSlot];
            if(alien_pvp::vents::ReadPoint(vents,static_cast<std::uint32_t>(vent),&VentRead,exit)) {
                if(vent!=gLocal.pickerAimVent) {
                    // New exit: provisional eye at head height over the vent;
                    // the camera hook replaces it with the probed room view.
                    gLocal.pickerAimVent=vent;gLocal.pickerAimPending=true;gLocal.pickerAimed=false;
                    gLocal.pickerExit={exit[0],exit[1],exit[2]};
                    gLocal.pickerHasToward=false;float best=1.0e9F;
                    for(std::uint32_t s=0;s<context.survivorCount && s<4U;++s) {
                        const float dx=context.survivors[s][0]-exit[0],dz=context.survivors[s][2]-exit[2];
                        const float d=std::hypot(dx,dz);
                        if(d>0.2F && d<best){best=d;gLocal.pickerToward={dx/d,0.0F,dz/d};gLocal.pickerHasToward=true;}
                    }
                    gLocal.freeEye={exit[0],exit[1]+1.0F,exit[2]};
                    gLocal.pickerAxis={};gLocal.pickerAxisLength=0.0F;
                    std::array<float,3> through{};
                    if(!VentExitPoint(vents,static_cast<std::uint32_t>(vent),through))
                        gAimThroughFail.fetch_add(1U,std::memory_order_relaxed);
                    else {
                        gAimThroughOk.fetch_add(1U,std::memory_order_relaxed);
                        gAimThroughDy.store(through[1]-exit[1],std::memory_order_relaxed);
                        const float ax=through[0]-exit[0],az=through[2]-exit[2],length=std::hypot(ax,az);
                        gLocal.pickerAxisLength=length;
                        if(length>0.2F)gLocal.pickerAxis={ax/length,0.0F,az/length};
                    }
                }
                if(gLocal.pickerAimed)gLocal.freeEye=gLocal.pickerEye;
                gLocal.selectedVent=vent;
            }
        }
        gLocal.pickPrev=prev;gLocal.pickNext=next;
        // This is a local camera, never an Alien movement/teleport command.
        input.forward=input.strafe=0.0F;
        static_cast<void>(UpdateVentInterface(context,vents));
    }
    if(!gLocal.freeCamera){gLocal.pickerCount=0;gLocal.pickerSlot=-1;gLocal.pickerAimVent=-1;gLocal.pickerAimed=gLocal.pickerAimPending=false;}
    // Edges are queued independently of movement; holding a button does not
    // repeat a vent/attack command every frame. Native adapters explicitly
    // execute host-authorized native actions. No fabricated health effect.
    const auto edge=[&](int key,bool& previous,alien_pvp::Action action) noexcept {
        const bool pressed=focused && Down(key);
        if(pressed && !previous) {input.action=action;alien_pvp::CaptureLocal(input);}
        previous=pressed;
    };
    const bool interact=focused && Down('E'),exit=focused && Down('Q');
    const bool cancelKey=(interact && !gLocal.enterDown) || (exit && !gLocal.exitDown);
    const bool cancelMove=focused && !gLocal.freeCamera && pendingVent && now-gLocal.ventPendingAt>=400U &&
        freshMove!=0U;
    if(pendingVent && !gLocal.ventCancelSent && (cancelKey||cancelMove)) {
        input.action=alien_pvp::Action::CancelVent;alien_pvp::CaptureLocal(input);gLocal.ventCancelSent=true;
    }
    if((interact && !gLocal.enterDown) || (exit && !gLocal.exitDown)) {
        if(pendingVent) { /* Host resolves cancellation of the original traversal. */ }
        else if(gLocal.wasBackstage && interact && gLocal.nearbyVent>=0) {
            // Round 10: after a flame retreat the Alien stays in until the
            // fear meter is full again.
            if(gFear.ExitAllowed())static_cast<void>(GoToVent(static_cast<std::uint32_t>(gLocal.nearbyVent)));
            else gFearExitRefused.fetch_add(1U,std::memory_order_relaxed);
        }
        else if(interact && gLocal.nearbyVent>=0 && !alien_pvp::VentPending() &&
            !gFear.Stunned(now) && gFear.phase!=alien_pvp::fear::Phase::Retreating) {
            alien_pvp::vents::Point portal{};
            if(alien_pvp::vents::ReadPoint(vents,static_cast<std::uint32_t>(gLocal.nearbyVent),&VentRead,portal)) {
                input.action=alien_pvp::Action::EnterVent;input.selectedVent=true;
                input.ventX=portal[0];input.ventY=portal[1];input.ventZ=portal[2];
                alien_pvp::CaptureLocal(input);
            }
        }
    }
    gLocal.enterDown=interact;gLocal.exitDown=exit;
    alien_pvp_input::SetMapMode(false);
    // Left OR right click attacks (AvS 2026-11-24: players tried right click;
    // it was never bound). One edge for either button.
    const auto attackHeld=[&]() noexcept {return Down(VK_LBUTTON) || Down(VK_RBUTTON);};
    gLocal.attackReady=false;
    bool lockerOnly=false;
    if(!gLocal.freeCamera && !pendingVent && !IsBackstage(context.alien) && context.survivorCount) {
        alien_pvp::actions::Point source{};
        if(Read(context.alien+0x70U,source.x) && Read(context.alien+0x74U,source.y) && Read(context.alien+0x78U,source.z)) {
            std::array<alien_pvp::actions::Candidate,4> candidates{},hidden{};std::size_t count{},hiddenCount{};
            for(std::uint32_t s=0;s<context.survivorCount && s<4U;++s) {
                const alien_pvp::actions::Candidate candidate{s+1U,{context.survivors[s][0],context.survivors[s][1],context.survivors[s][2]},true,true,false,true};
                // Pass 20: a survivor in a locker is not a kill target (the
                // Host refuses it, stage 42); lockers get the sniff next pass.
                if(context.survivorHidden[s])hidden[hiddenCount++]=candidate;
                else candidates[count++]=candidate;
            }
            // Peer ids here are placeholders (index+1); the Alien "peer" is 0x7FFF.
            gLocal.attackReady=count && alien_pvp::actions::SelectVictim(source,input.yaw,0x7FFFU,{candidates.data(),count})!=0U;
            lockerOnly=!gLocal.attackReady && hiddenCount &&
                alien_pvp::actions::SelectVictim(source,input.yaw,0x7FFFU,{hidden.data(),hiddenCount})!=0U;
        }
    }
    // Round 10: a survivor facing the Alien with the flamethrower up close, a
    // stun/flinch or a retreat in progress: the kill click does nothing.
    const bool attackLocked=gFear.KillBlocked(now) || gFear.Stunned(now) ||
        gFear.phase!=alien_pvp::fear::Phase::Normal;
    if(attackLocked)gLocal.attackReady=false;
    if(!gLocal.freeCamera && !pendingVent) {
        const bool pressed=focused && attackHeld();
        if(pressed && !gLocal.attackDown && attackLocked)gFear.blockedClicks++;
        else if(pressed && !gLocal.attackDown && lockerOnly)gLockerClicks.fetch_add(1U,std::memory_order_relaxed);
        else if(pressed && !gLocal.attackDown){input.action=alien_pvp::Action::Attack;alien_pvp::CaptureLocal(input);}
        gLocal.attackDown=pressed;
        edge('G',gLocal.roarDown,alien_pvp::Action::Roar);
    } else {gLocal.attackDown=attackHeld();gLocal.roarDown=Down('G');}
    input.action=alien_pvp::Action::None;
    alien_pvp::CaptureLocal(input);
    gLocal.moving=alien_pvp::MovingNow();
}
bool CameraHookCurrent(std::uintptr_t behavior) noexcept {
    if(GetCurrentThreadId()!=gPilotThread.load(std::memory_order_acquire) ||
        !gCameraGeneration.load(std::memory_order_acquire))return false;
    CameraBinding current{};std::uint32_t runtime{};std::uintptr_t skeleton{};
    return gLocal.camera.bound && !gLocal.camera.firstPerson && gCameraBase &&
        CurrentCameraRoot(gCameraBase,current) && current.manager==gLocal.camera.manager &&
        current.previous==gLocal.camera.follow && FindFollow(current.follow)==behavior &&
        TechnicalIdentityCurrent(gLocal.camera) &&
        Read(gLocal.character+0x50U,runtime) && runtime==gLocal.camera.alienRuntime &&
        Read(gLocal.character+kCharacterSkeletonOffset,skeleton) && skeleton && skeleton==gLocal.camera.alienSkeleton;
}
void FollowBefore(std::uintptr_t behavior) noexcept {
    if(!CameraHookCurrent(behavior))return;
    // Donor OnFollowUpdateEnter: backstage actors do not tick the engine's
    // follow camera. Use the living technical actor to compute the orbit.
    const auto target=gLocal.freeCamera?gLocal.camera.technical:gLocal.character;
    std::uintptr_t current{},child{};
    if(Read(gLocal.camera.follow+0x148U,current) && current!=target)
        static_cast<void>(RetargetCamera(gCameraBase,gLocal.camera.follow,target));
    if(Read(behavior+0x34U,child) && child && child!=gLocal.camera.follow &&
        Read(child+0x148U,current) && current!=target)
        static_cast<void>(RetargetCamera(gCameraBase,child,target));
}
// Vent-exit picker view: find the open side of the room around the selected
// exit with this generation's physics rays (preferring the side facing the
// nearest survivor), put the eye just outside the opening at head height and
// look into the room. Without a sampled world, face the survivor instead.
void AimPickerCamera(const std::array<float,3>& orbitForward) noexcept {
    std::array<std::uintptr_t,4> worlds{};const unsigned worldCount=CurrentWorlds(worlds);
    const auto e=gLocal.pickerExit;
    const auto cast=[&](const std::array<float,3>& from,const std::array<float,3>& to) noexcept {
        float best=std::hypot(std::hypot(to[0]-from[0],to[2]-from[2]),to[1]-from[1]);
        for(unsigned i=0;i<worldCount;++i) {
            std::array<float,3> hit{};
            if(Ray(worlds[i],from,to,hit))
                best=std::min(best,std::hypot(std::hypot(hit[0]-from[0],hit[2]-from[2]),hit[1]-from[1]));
        }
        return best;
    };
    // AvS 2026-11-25 round 4: 0.7 m out still framed the duct ("seeing the
    // inside of the vent"; vent 17 open 0.54 m from exit+1 m). Probe from three
    // heights, then put the eye up to 2.5 m out into the open side, at up to
    // 1.8 m (ceiling-limited), so the view shows the room the Alien drops into.
    // If every probe is boxed in (exit inside the duct above a ceiling opening)
    // and a floor lies clearly below, probe again 1.2 m above that floor.
    // AvS 2026-11-25 round 5: two exits showed the room from far above and
    // one was grey. The floor probe only ran when every side was boxed in, so
    // a ceiling exit (open 8 m in the void above the ceiling) put the eye 1.8 m
    // over the hatch: above the ceiling, or inside its slab. Now the room floor
    // under the exit is found first; every candidate eye must be reachable from
    // its probe point, stand over a floor and see the exit (the room the Alien
    // drops into), otherwise the next direction/height is used.
    // AvS 2026-11-26 round 10 ("still see inside the vents"): the round-5 log
    // had openM < 1 m on every exit. Every far candidate failed the "must see
    // the exit" check (a ceiling vent point sits inside the slab, a wall vent
    // point inside the duct), so the winner was always the spot right at the
    // hatch, facing whatever lay within 1 m. That check is gone. The view now
    // stands on the room floor under/in front of the opening and looks into the
    // room: along the vent's own axis (opening -> native ExitPoint, the side
    // with the longer free run and a floor at room height = the room), or for
    // vertical (ceiling) vents along the most open of 16 directions.
    float yaw{},open{-1.0F},floorY{e[1]},drop{},sideOpen{-1.0F};int mode{},side{};
    std::array<float,3> eye{e[0],e[1]+1.0F,e[2]};
    if(worldCount) {
        const float down=cast({e[0],e[1]+0.2F,e[2]},{e[0],e[1]-8.0F,e[2]});
        if(down<7.9F){floorY=e[1]+0.2F-down;drop=std::max(0.0F,e[1]-floorY);}
        const auto floorAt=[&](const std::array<float,3>& p) noexcept {
            return cast(p,{p[0],p[1]-4.0F,p[2]});
        };
        // Eye height: 1.7 m over the room floor, kept 0.3 m under a ceiling.
        const auto standAt=[&](float x,float z) noexcept {
            const std::array<float,3> base{x,floorY+1.0F,z};
            const float up=cast(base,{x,base[1]+1.2F,z});
            return std::array<float,3>{x,floorY+std::clamp(1.0F+up-0.3F,0.6F,1.7F),z};
        };
        const auto toward=[&](float dx,float dz) noexcept {
            return gLocal.pickerHasToward?std::max(0.0F,dx*gLocal.pickerToward[0]+dz*gLocal.pickerToward[2]):0.0F;
        };
        constexpr float kReach=8.0F;
        if(gLocal.pickerAxisLength>0.2F) {
            const auto axis=gLocal.pickerAxis;
            float best=-1.0F;
            for(const int s:{1,-1}) {
                const float dx=axis[0]*static_cast<float>(s),dz=axis[2]*static_cast<float>(s);
                const std::array<float,3> from{e[0]+dx*0.8F,floorY+1.2F,e[2]+dz*0.8F};
                const float d=cast(from,{from[0]+dx*kReach,from[1],from[2]+dz*kReach});
                const float f=floorAt(from);
                // The room side has a floor ~1.2 m below; the duct side is the
                // wall slab or a duct floor right under the probe.
                if(f<0.9F || f>1.6F)continue;
                const float score=d+1.0F*toward(dx,dz);
                if(score>best){best=score;side=s;sideOpen=d;}
            }
            if(side!=0) {
                const float dx=axis[0]*static_cast<float>(side),dz=axis[2]*static_cast<float>(side);
                // Stand 0.8-1.5 m out from the opening (never past the far wall)
                // so the hatch is just behind/above and the room fills the view.
                const float out=std::clamp(sideOpen*0.25F,0.8F,1.5F);
                eye=standAt(e[0]+dx*out,e[2]+dz*out);
                yaw=std::atan2(dx,dz);open=sideOpen;mode=6;
            }
        }
        if(side==0) {
            // Vertical vent or axis unreadable: from under the opening, face
            // the most open of 16 directions (bias toward the survivors).
            constexpr int kDirections=16;
            const auto scan=[&](float floor,float& bestYaw) noexcept {
                const std::array<float,3> probe{e[0],floor+1.3F,e[2]};
                float best=-1.0e9F,bestOpen=0.0F;
                for(int i=0;i<kDirections;++i) {
                    const float a=static_cast<float>(i)*6.28318530718F/static_cast<float>(kDirections);
                    const float dx=std::sin(a),dz=std::cos(a);
                    const float d=cast(probe,{probe[0]+dx*kReach,probe[1],probe[2]+dz*kReach});
                    const float score=d+1.5F*toward(dx,dz);
                    if(score>best){best=score;bestYaw=a;bestOpen=d;}
                }
                return bestOpen;
            };
            open=scan(floorY,yaw);
            // Pass 14 (avsfix13: grey ceiling exits, openM 0.8, liftM -2.6,
            // floorDropM 4.3): the first surface under a ceiling vent's point
            // is the hatch grille at the bottom of its shaft, so the eye stood
            // 1.7 m over the grille inside the shaft. While boxed in, look
            // through that surface for the room floor below; accept it only if
            // it is much more open and clear straight up to just under the
            // surface above (a thin grille, not a floor slab of the level below).
            std::uint32_t levels=1U,level=0U;float upClear=-1.0F,top=floorY;
            for(std::uint32_t n=1U;n<3U && open<2.0F;++n) {
                const std::array<float,3> start{e[0],top-0.15F,e[2]};
                const float d=cast(start,{start[0],start[1]-8.0F,start[2]});
                if(d>=7.9F)break;
                const float lower=start[1]-d;++levels;
                float lowerYaw{};const float lowerOpen=scan(lower,lowerYaw);
                const std::array<float,3> probe{e[0],lower+1.3F,e[2]};
                const float need=top-0.3F-probe[1];
                const float up=need>0.0F?cast(probe,{e[0],top-0.3F,e[2]}):0.0F;
                if(lowerOpen>=open+1.0F && (need<=0.0F || up>=need-0.3F)) {
                    floorY=lower;open=lowerOpen;yaw=lowerYaw;level=n;upClear=up;
                    drop=std::max(0.0F,e[1]-floorY);
                }
                top=lower;
            }
            gAimLevels.store(levels,std::memory_order_relaxed);gAimLevel.store(level,std::memory_order_relaxed);
            gAimUpClear.store(upClear,std::memory_order_relaxed);
            const std::array<float,3> from{e[0],floorY+1.3F,e[2]};
            // Step back 0.4 m (if free) so the drop point is in front of the eye.
            const float bx=-std::sin(yaw),bz=-std::cos(yaw);
            const float back=std::clamp(cast(from,{from[0]+bx*1.0F,from[1],from[2]+bz*1.0F})-0.3F,0.0F,0.4F);
            eye=standAt(e[0]+bx*back,e[2]+bz*back);
            mode=level?9:drop>1.4F?7:8;
        }
    } else {
        yaw=gLocal.pickerHasToward?std::atan2(gLocal.pickerToward[0],gLocal.pickerToward[2]):
            std::atan2(orbitForward[0],orbitForward[2]);
        mode=gLocal.pickerHasToward?2:3;
        eye={e[0]+std::sin(yaw)*1.5F,e[1]+1.0F,e[2]+std::cos(yaw)*1.5F};
    }
    gLocal.pickerEye=eye;
    gAimStats.out.store(std::hypot(eye[0]-e[0],eye[2]-e[2]),std::memory_order_relaxed);
    gAimStats.lift.store(eye[1]-e[1],std::memory_order_relaxed);
    gAimStats.floorDrop.store(drop,std::memory_order_relaxed);
    gAimStats.axis.store(gLocal.pickerAxisLength,std::memory_order_relaxed);
    gAimStats.side.store(side,std::memory_order_relaxed);
    gAimStats.sideOpen.store(sideOpen,std::memory_order_relaxed);
    gLocal.orbitYaw0=std::atan2(orbitForward[0],orbitForward[2]);
    gLocal.orbitPitch0=std::asin(std::clamp(orbitForward[1],-1.0F,1.0F));
    gLocal.pickerYaw=yaw;gLocal.pickerPitch=mode==3?gLocal.orbitPitch0:-0.2F;
    gLocal.pickerAimed=true;gLocal.pickerAimPending=false;
    gLocal.freeEye=gLocal.pickerEye;
    gAimStats.mode.store(static_cast<std::uint32_t>(mode),std::memory_order_relaxed);
    gAimStats.open.store(open,std::memory_order_relaxed);
    gAimStats.worlds.store(worldCount,std::memory_order_relaxed);
    gAimStats.vent.store(gLocal.pickerAimVent,std::memory_order_relaxed);
    gAimStats.aims.fetch_add(1U,std::memory_order_release);
}
void FollowAfter(std::uintptr_t behavior) noexcept {
    if(!CameraHookCurrent(behavior))return;
    std::array<float,3> body{},base{},position{};
    if(!Read(gLocal.character+0x70U,body) || !Read(behavior+0x1C0U,base) || !Read(behavior+0x1CU,position))return;
    if(gLocal.freeCamera) {
        const float length=std::hypot(std::hypot(base[0]-position[0],base[2]-position[2]),base[1]-position[1]);
        std::array<float,3> orbit=gLocal.freeForward;
        if(length>0.01F)for(unsigned i=0;i<3;++i)orbit[i]=(base[i]-position[i])/length;
        if(!gLocal.exitTracking) {
            if(gLocal.pickerAimPending)AimPickerCamera(orbit);
            if(gLocal.pickerAimed) {
                // Mouse look stays live, relative to the probed room view.
                const float yaw=gLocal.pickerYaw+(std::atan2(orbit[0],orbit[2])-gLocal.orbitYaw0);
                const float pitch=std::clamp(gLocal.pickerPitch+
                    (std::asin(std::clamp(orbit[1],-1.0F,1.0F))-gLocal.orbitPitch0),-1.2F,1.2F);
                gLocal.freeForward={std::sin(yaw)*std::cos(pitch),std::sin(pitch),std::cos(yaw)*std::cos(pitch)};
            } else if(length>0.01F)gLocal.freeForward=orbit;
        }
        const auto out=gLocal.freeEye;
        std::array<float,3> target{};
        for(unsigned i=0;i<3;++i)target[i]=out[i]+gLocal.freeForward[i]*5.0F;
        static_cast<void>(Write(behavior+0x1C0U,target));static_cast<void>(Write(behavior+0x1CU,out));
        std::uintptr_t camera{};
        if(Read(behavior+0x34U,camera) && camera) {
            for(auto offset:{0x14U,0x3CU,0x64U})static_cast<void>(Write(camera+offset,out));
            static_cast<void>(Write(camera+0x10CU,target));
            static_cast<void>(Write(camera+0x153U,std::uint8_t{1}));
        }
        return;
    }
    gLocal.lastEye=position;gLocal.eyeKnown=true;
    const auto now=GetTickCount64();const auto generation=gCameraGeneration.load(std::memory_order_acquire);
    std::uintptr_t world{};
    // Select the donor's world containing the actor's floor; samples from a
    // previous generation are never candidates after a checkpoint reload.
    for(auto& sample:gWorlds) {
        if(sample.generation.load(std::memory_order_acquire)!=generation || now-sample.at.load()>1000U)continue;
        const auto candidate=sample.world.load();std::array<float,3> hit{};
        const std::array<float,3> from{body[0],body[1]+1.0F,body[2]},to{body[0],body[1]-5.0F,body[2]};
        if(candidate && Ray(candidate,from,to,hit)){world=candidate;break;}
    }
    if(!world)return;
    if(now-gLocal.ductAt>=150U) {
        gLocal.ductAt=now;std::array<float,3> hit{};
        const bool low=Ray(world,{body[0],body[1]+0.4F,body[2]},{body[0],body[1]+1.9F,body[2]},hit);
        if(low==gLocal.duct)gLocal.ductRun=0U;
        else if(++gLocal.ductRun>=5U){gLocal.duct=low;gLocal.ductRun=0U;}
        static_cast<void>(ConfigureFollow(gLocal.camera,false));
    }
    // Donor Collide soft mode: median of three rays, smooth inward/outward.
    const std::array<float,3> from{base[0],base[1]+1.2F,base[2]};
    const float dx=position[0]-from[0],dy=position[1]-from[1],dz=position[2]-from[2];
    const float length=std::max(0.05F,std::sqrt(dx*dx+dy*dy+dz*dz));
    const float ux=dx/length,uy=dy/length,uz=dz/length,hl=std::hypot(ux,uz);
    const float sx=hl>0.01F?uz/hl*0.3F:0.3F,sz=hl>0.01F?-ux/hl*0.3F:0.0F;
    const auto fraction=[&](float ox,float oz) noexcept {
        std::array<float,3> hit{};
        if(!Ray(world,from,{position[0]+ox+ux*0.3F,position[1]+uy*0.3F,position[2]+oz+uz*0.3F},hit))return 1.0F;
        const float x=hit[0]-from[0],y=hit[1]-from[1],z=hit[2]-from[2];
        return std::min(1.0F,(std::sqrt(x*x+y*y+z*z)-0.3F)/length);
    };
    const float t0=fraction(0,0),t1=fraction(sx,sz),t2=fraction(-sx,-sz);
    const float median=std::max(std::min(t0,t1),std::min(std::max(t0,t1),t2));
    const float target=std::min(1.0F,std::max(std::max(0.3F,1.0F/length),median));
    gLocal.collisionScale+=(target-gLocal.collisionScale)*(target<gLocal.collisionScale?0.25F:0.07F);
    if(std::abs(target-gLocal.collisionScale)<0.02F)gLocal.collisionScale=target;
    if(gLocal.collisionScale<0.999F) {
        const std::array<float,3> out{from[0]+dx*gLocal.collisionScale,from[1]+dy*gLocal.collisionScale,from[2]+dz*gLocal.collisionScale};
        static_cast<void>(Write(behavior+0x1CU,out));
    }
}
bool OrbitRay(std::uintptr_t base,std::uintptr_t world,const std::array<float,3>& from,const std::array<float,3>& to,
    std::array<float,3>& hit) noexcept {
    alignas(16) float a[4]{from[0],from[1],from[2],0},b[4]{to[0],to[1],to[2],0},p[4]{},n[4]{};
    alignas(16) std::uint8_t result[16]{};
#if defined(_MSC_VER)
    __try {
        using Fn=void(__thiscall*)(std::uintptr_t,void*,std::uintptr_t,const void*,const void*,void*,void*);
        reinterpret_cast<Fn>(base+0x0028DAB0U)(0U,result,world,a,b,p,n);
        if(!result[0])return false;
        hit={p[0],p[1],p[2]};return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
#else
    return false;
#endif
}
// Third-person orbit for a dead player watching a teammate. Same output
// fields as the PvP free camera (FollowAfter): behavior eye/look and the
// child camera transform, written after the retail update of this frame.
void SpectateOrbitAfter(std::uintptr_t behavior) noexcept {
    const auto base=gOrbitBase.load(std::memory_order_acquire);
    const auto target=gOrbitTarget.load(std::memory_order_acquire);
    if(!base || !target || behavior!=gOrbitBehavior.load(std::memory_order_acquire))return;
    std::array<float,3> body{};float facing{};
    if(!Read(target+0x70U,body) || !std::isfinite(body[0]) || !std::isfinite(body[1]) || !std::isfinite(body[2]))return;
    if(gOrbit.target!=target) {
        // Start behind the watched player, looking the way they face.
        gOrbit=OrbitState{};gOrbit.target=target;
        if(Read(target+0x174U,facing) && std::isfinite(facing))gOrbit.yaw=facing;
        static_cast<void>(alien_pvp_input::TakeSpectateMouse());
    }
    const auto mouse=alien_pvp_input::TakeSpectateMouse();
    constexpr float kSensitivity=0.0022F;
    gOrbit.yaw+=static_cast<float>(mouse.dx)*kSensitivity;
    gOrbit.pitch=std::clamp(gOrbit.pitch+static_cast<float>(mouse.dy)*kSensitivity,-0.55F,1.25F);
    gOrbit.distance=std::clamp(gOrbit.distance-static_cast<float>(mouse.wheel)/120.0F*0.35F,1.2F,6.0F);
    const std::array<float,3> pivot{body[0],body[1]+1.55F,body[2]};
    const float flat=std::cos(gOrbit.pitch);
    const std::array<float,3> dir{-std::sin(gOrbit.yaw)*flat,std::sin(gOrbit.pitch),-std::cos(gOrbit.yaw)*flat};
    // Collision: nearest hit over the physics worlds sampled this second.
    float allowed=gOrbit.distance;bool anyWorld{};
    const auto now=GetTickCount64();
    const std::array<float,3> reach{pivot[0]+dir[0]*(gOrbit.distance+0.3F),pivot[1]+dir[1]*(gOrbit.distance+0.3F),
        pivot[2]+dir[2]*(gOrbit.distance+0.3F)};
    for(auto& sample:gWorlds) {
        if(sample.generation.load(std::memory_order_acquire)!=kOrbitWorldGeneration || now-sample.at.load()>1000U)continue;
        const auto world=sample.world.load();if(!world)continue;
        anyWorld=true;std::array<float,3> hit{};
        if(OrbitRay(base,world,pivot,reach,hit)) {
            const float x=hit[0]-pivot[0],y=hit[1]-pivot[1],z=hit[2]-pivot[2];
            allowed=std::min(allowed,std::max(0.35F,std::sqrt(x*x+y*y+z*z)-0.3F));
        }
    }
    if(!anyWorld)gOrbitNoWorld.fetch_add(1U,std::memory_order_relaxed);
    if(allowed<gOrbit.distance)gOrbitCollisions.fetch_add(1U,std::memory_order_relaxed);
    // Pull in at once on a hit; ease back out so a doorway does not pop.
    gOrbit.shown=allowed<gOrbit.shown?allowed:gOrbit.shown+(allowed-gOrbit.shown)*0.12F;
    const std::array<float,3> eye{pivot[0]+dir[0]*gOrbit.shown,pivot[1]+dir[1]*gOrbit.shown,pivot[2]+dir[2]*gOrbit.shown};
    // The behavior transform block (+0C quaternion x,y,z,w, +1C position) is
    // copied into the rendered camera block by 0042ADA0 right after this
    // update. Basis +X right, +Y up, +Z forward; yaw as Character+0x174,
    // positive pitch looks down. Look from the eye at the pivot, no roll.
    const float lx=pivot[0]-eye[0],ly=pivot[1]-eye[1],lz=pivot[2]-eye[2];
    const float length=std::sqrt(lx*lx+ly*ly+lz*lz);
    if(!(length>0.05F))return;
    const float psi=std::atan2(lx,lz),theta=std::asin(std::clamp(-ly/length,-1.0F,1.0F));
    const float cy=std::cos(psi*0.5F),sy=std::sin(psi*0.5F),cp=std::cos(theta*0.5F),sp=std::sin(theta*0.5F);
    const std::array<float,4> rotation{cy*sp,sy*cp,-sy*sp,cy*cp};
    static_cast<void>(Write(behavior+0x0CU,rotation));
    static_cast<void>(Write(behavior+0x1CU,eye));
    gOrbitFrames.fetch_add(1U,std::memory_order_relaxed);
}
void __fastcall FollowDetour(void* self,void*,std::uint32_t a,std::uint32_t b) noexcept {
    FollowBefore(reinterpret_cast<std::uintptr_t>(self));
    reinterpret_cast<void(__thiscall*)(void*,std::uint32_t,std::uint32_t)>(gFollowTrampoline)(self,a,b);
    FollowAfter(reinterpret_cast<std::uintptr_t>(self));
    SpectateOrbitAfter(reinterpret_cast<std::uintptr_t>(self));
}
bool NoclipViewCurrent() noexcept {
    return GetCurrentThreadId()==gPilotThread.load(std::memory_order_acquire) &&
        gLocal.freeCamera && CameraHookCurrent(gLocal.camera.followBehavior);
}
void __fastcall SubmitDetour(void* self,void*) noexcept {
    reinterpret_cast<void(__thiscall*)(void*)>(gSubmitTrampoline)(self);
    if(!NoclipViewCurrent() || reinterpret_cast<std::uintptr_t>(self)!=gLocal.camera.manager)return;
    // Donor photo.cpp::OnCamSubmitLeave. Retail otherwise selects the room
    // of the parked technical actor. Let the renderer match the camera's
    // position instead. Outside the level, retain its existing room fallback.
    std::uintptr_t manager{},pov{};std::uint32_t zone{};
    if(!Read(gCameraBase+0x01366A00U,manager) || !manager ||
        !Read(manager+0x8CU,pov) || !pov || !Read(pov+0x330U,zone) || zone==0xFFFFFFFFU)return;
    static_cast<void>(Write(gCameraBase+0x01358144U,std::uint8_t{0}));
    static_cast<void>(Write(gCameraBase+0x01358145U,std::uint8_t{0}));
}
void __fastcall PostBlendDetour(void* self,void*) noexcept {
    reinterpret_cast<void(__thiscall*)(void*)>(gPostBlendTrampoline)(self);
    if(!NoclipViewCurrent())return;
    // Donor photo.cpp::OnPostBlendLeave: change only this frame's blended
    // output, never level settings. Retail recalculates these on every call,
    // so normal lighting resumes on the first frame outside noclip.
    const auto manager=reinterpret_cast<std::uintptr_t>(self);
    float brightness{};
    if(!Read(manager+0x1914U,brightness) || !std::isfinite(brightness))return;
    // Preserve the level's RGB/exposure. A forced minimum RGB multiplier of
    // four plus .25 brightness clipped the whole image to white in build 444.
    // Only lift very dark output slightly; never multiply the color channels.
    brightness=std::max(brightness,0.015F);
    static_cast<void>(Write(manager+0x1914U,brightness));
    // Clean, readable navigation: no vignette, grain, or motion blur.
    for(const auto offset:{0x1988U,0x19E8U,0x19ECU,0x19B0U,0x19B4U,0x19B8U,0x19D0U,0x19D4U,0x19D8U})
        static_cast<void>(Write(manager+offset,0.0F));
}
void __fastcall WorldDetour(void* self,void*,std::uint32_t a,std::uint32_t b) noexcept {
    auto generation=gCameraGeneration.load(std::memory_order_acquire);
    if(!generation && gOrbitTarget.load(std::memory_order_relaxed))generation=kOrbitWorldGeneration;
    if(generation) {
        const auto world=reinterpret_cast<std::uintptr_t>(self);const auto now=GetTickCount64();
        for(auto& sample:gWorlds) {
            if(sample.world.load()==world || sample.generation.load()!=generation || now-sample.at.load()>3000U) {
                sample.world.store(world);sample.at.store(now);sample.generation.store(generation,std::memory_order_release);break;
            }
        }
    }
    reinterpret_cast<void(__thiscall*)(void*,std::uint32_t,std::uint32_t)>(gWorldTrampoline)(self,a,b);
}
} // namespace

void SetSpectateOrbit(const std::uintptr_t base,const std::uintptr_t followBehavior,const std::uintptr_t target) noexcept {
    const bool on=base && followBehavior && target;
    if(on && !alien_pvp_input::Installed())static_cast<void>(alien_pvp_input::EnsureInstalled(base));
    alien_pvp_input::SetSpectateMouse(on);
    gOrbitBase.store(on?base:0U,std::memory_order_release);
    gOrbitBehavior.store(on?followBehavior:0U,std::memory_order_release);
    gOrbitTarget.store(on?target:0U,std::memory_order_release);
    if(!on)gOrbit=OrbitState{};
}
SpectateOrbitStats SpectateOrbitSnapshot() noexcept {
    return {gOrbitFrames.load(std::memory_order_relaxed),gOrbitCollisions.load(std::memory_order_relaxed),
        gOrbitNoWorld.load(std::memory_order_relaxed)};
}
bool InstallCameraHooks(std::uintptr_t base,HookManager& hooks,std::string& error) {
    // Both supplied x86 entries start with push ebp; mov ebp,esp;
    // and esp,-16. Six complete relocation-free instructions bytes.
    for(const auto world:{false,true}) {
        HookRequest request;
        request.name=world?"alien_donor_camera_world":"alien_donor_follow_camera";
        request.target=reinterpret_cast<void*>(base+(world?0x00748890U:0x0002E970U));
        request.detour=world?reinterpret_cast<void*>(&WorldDetour):reinterpret_cast<void*>(&FollowDetour);
        request.patchLength=6U;request.relocationSafePrefix=true;
        request.expected=MaskedBytes{{0x55,0x8B,0xEC,0x83,0xE4,0xF0},"xxxxxx"};
        request.trampolineOut=world?&gWorldTrampoline:&gFollowTrampoline;
        if(!hooks.Install(request,error))return false;
    }
    // Exact retail entries; neither prefix contains a relative instruction.
    HookRequest submit;
    submit.name="alien_noclip_render_room";
    submit.target=reinterpret_cast<void*>(base+0x00032300U);
    submit.detour=reinterpret_cast<void*>(&SubmitDetour);
    submit.patchLength=6U;submit.relocationSafePrefix=true;
    submit.expected=MaskedBytes{{0x55,0x8B,0xEC,0x83,0xE4,0xF0},"xxxxxx"};
    submit.trampolineOut=&gSubmitTrampoline;
    if(!hooks.Install(submit,error))return false;
    HookRequest post;
    post.name="alien_noclip_bright_view";
    post.target=reinterpret_cast<void*>(base+0x00608C50U);
    post.detour=reinterpret_cast<void*>(&PostBlendDetour);
    // push ecx; movq xmm0,[absolute address]. The loader relocates that
    // address, which remains valid in the trampoline (no EIP-relative data).
    post.patchLength=9U;post.relocationSafePrefix=true;
    post.expected=MaskedBytes{{0x51,0xF3,0x0F,0x7E,0x05,0,0,0,0},"xxxxx????"};
    post.trampolineOut=&gPostBlendTrampoline;
    if(!hooks.Install(post,error))return false;
    return true;
}

Readiness Snapshot() noexcept {
    return {alien_pvp_input::Installed(),gLocal.camera.bound,
        gLocal.visualReady,gLocal.moving};
}
VentInterface VentSnapshot() noexcept {
    return {false,gLocal.wasBackstage,gLocal.nearbyVent,gLocal.selectedVent,gLocal.freeCamera,
        gLocal.pickerSlot,gLocal.pickerCount,gLocal.attackReady};
}
FearStats FearSnapshot() noexcept {
    FearStats s{};
    s.meter=gFearMeter.load(std::memory_order_relaxed);s.resistance=gFearResistance.load(std::memory_order_relaxed);
    s.phase=gFearPhase.load(std::memory_order_relaxed);s.retreats=gFearRetreats.load(std::memory_order_relaxed);
    s.chaseRetreats=gFearChaseRetreats.load(std::memory_order_relaxed);s.flinches=gFearFlinches.load(std::memory_order_relaxed);
    s.stuns=gFearStuns.load(std::memory_order_relaxed);s.blockedClicks=gFearBlockedClicks.load(std::memory_order_relaxed);
    s.exitRefused=gFearExitRefused.load(std::memory_order_relaxed);s.retreatSent=gFearRetreatSent.load(std::memory_order_relaxed);
    s.retreatAbandoned=gFearRetreatAbandoned.load(std::memory_order_relaxed);
    s.flamedFrames=gFearFlamedFrames.load(std::memory_order_relaxed);s.threatFrames=gFearThreatFrames.load(std::memory_order_relaxed);
    s.retreatVentM=gFearRetreatVentM.load(std::memory_order_relaxed);s.flamerAngle=gFearFlamerAngle.load(std::memory_order_relaxed);
    s.survivorWeapons=gFearSurvivorWeapons.load(std::memory_order_relaxed);
    s.reactions=gFearReactions.load(std::memory_order_relaxed);s.stunRoars=gFearStunRoars.load(std::memory_order_relaxed);
    s.survivorStunned=gSurvivorStunned.load(std::memory_order_relaxed);s.survivorStunMissed=gSurvivorStunMissed.load(std::memory_order_relaxed);
    s.oddCycles=gOddCycles.load(std::memory_order_relaxed);
    s.lockerClicks=gLockerClicks.load(std::memory_order_relaxed);
    const std::uint32_t head=gTrailHead.load(std::memory_order_acquire);const std::uint64_t now=GetTickCount64();
    std::string trail="[";
    for(std::uint32_t i=head>kCycleTrail?head-kCycleTrail:0U;i<head;++i) {
        const auto at=i%kCycleTrail;const auto t=gTrailAt[at].load(std::memory_order_relaxed);
        if(trail.size()>1U)trail+=",";
        trail+="["+std::to_string(gTrailState[at].load(std::memory_order_relaxed))+","+std::to_string(now>=t?now-t:0U)+"]";
    }
    s.cycleTrail=trail+"]";
    return s;
}
CrawlStats CrawlSnapshot() noexcept {
    return {gCrawlDuct.load(std::memory_order_relaxed),gCrawlEntries.load(std::memory_order_relaxed),
        gCrawlDuctToggles.load(std::memory_order_relaxed),gCrawlFirstPersonOn.load(std::memory_order_relaxed),
        gCrawlFirstPersonFailed.load(std::memory_order_relaxed),gCrawlFrames.load(std::memory_order_relaxed),
        gCrawlDuctX.load(std::memory_order_relaxed),gCrawlDuctZ.load(std::memory_order_relaxed),
        gCrawlHeadReleased.load(std::memory_order_relaxed),gCrawlMenuExits.load(std::memory_order_relaxed),
        gCameraLost.load(std::memory_order_relaxed)};
}
bool RetreatActive() noexcept {
    return gFearPhase.load(std::memory_order_relaxed)==static_cast<std::uint32_t>(alien_pvp::fear::Phase::Retreating);
}
VentCameraStats VentCameraSnapshot() noexcept {
    return {gAimStats.aims.load(std::memory_order_acquire),gAimStats.mode.load(std::memory_order_relaxed),
        gAimStats.worlds.load(std::memory_order_relaxed),gAimStats.open.load(std::memory_order_relaxed),
        gAimStats.vent.load(std::memory_order_relaxed),gAimStats.out.load(std::memory_order_relaxed),
        gAimStats.lift.load(std::memory_order_relaxed),gAimStats.floorDrop.load(std::memory_order_relaxed),
        gVentHidden.load(std::memory_order_relaxed),gVentFaceHits.load(std::memory_order_relaxed),
        gVentHiddenGap.load(std::memory_order_relaxed),gVentFaceGap.load(std::memory_order_relaxed),
        gVentInReach.load(std::memory_order_relaxed),gVentTableCount.load(std::memory_order_relaxed),
        gVentWorlds.load(std::memory_order_relaxed),gVentSelfHits.load(std::memory_order_relaxed),
        gVentBlockedOffered.load(std::memory_order_relaxed),gVentHitFromStart.load(std::memory_order_relaxed),
        gAimStats.axis.load(std::memory_order_relaxed),gAimStats.side.load(std::memory_order_relaxed),
        gAimStats.sideOpen.load(std::memory_order_relaxed),
        gAimLevels.load(std::memory_order_relaxed),gAimLevel.load(std::memory_order_relaxed),
        gAimUpClear.load(std::memory_order_relaxed),gAimThroughDy.load(std::memory_order_relaxed),
        gAimThroughOk.load(std::memory_order_relaxed),gAimThroughFail.load(std::memory_order_relaxed)};
}
bool GoToVent(std::uint32_t index) noexcept {
    if(!gLocal.freeCamera || !gLocal.wasBackstage || alien_pvp::VentPending() || !alien_pvp::ControlActive() ||
        !gLocal.camera.bound || !InputAllowed(gCameraBase))return false;
    alien_pvp::vents::Table table{};alien_pvp::vents::Point point{};
    if(!alien_pvp::vents::ReadTable(gLocal.character,&VentRead,table) ||
        !alien_pvp::vents::ReadPoint(table,index,&VentRead,point))return false;
    if(std::hypot(std::hypot(point[0]-gLocal.freeEye[0],point[2]-gLocal.freeEye[2]),point[1]-gLocal.freeEye[1])>3.0F)return false;
    gLocal.selectedVent=static_cast<int>(index);
    alien_pvp::LocalInput input{};input.focused=true;
    input.action=alien_pvp::Action::ExitVent;input.selectedVent=true;
    input.ventX=point[0];input.ventY=point[1];input.ventZ=point[2];
    alien_pvp::CaptureLocal(input);return true;
}
bool Drives(const std::uintptr_t character,const std::uint32_t generation) noexcept {
    return character!=0U && gLocal.character==character &&
        gLocal.generation==generation && gLocal.camera.bound &&
        gLocal.visualReady && alien_pvp_input::Installed();
}
std::atomic<std::uintptr_t> gListenerCharacter{};
std::uintptr_t ListenerCharacter() noexcept {return gListenerCharacter.load(std::memory_order_acquire);}
void Reset(const bool sameCameraWorld,const bool certifiedLivingCharacter) noexcept {
    gCameraGeneration.store(0U,std::memory_order_release);
    gListenerCharacter.store(0U,std::memory_order_release);
    // The controller does not release AISync's exclusive lease: only the lease
    // owner does that after the certified request boundary is idle.
    alien_pvp_input::SetActive(false);
    // Overlays are recreated with a new level; only restore in the same world.
    if (gLocal.hudHidden && sameCameraWorld) SetGameHudHidden(gCameraBase,false);
    const auto camera=gLocal.camera;
    alien_pvp_visual::RestoreAlienHead(gLocal.character,gLocal.generation,
        certifiedLivingCharacter && sameCameraWorld);
    alien_pvp_visual::RestoreTechnical(camera.technical,camera.generation,
        sameCameraWorld && TechnicalIdentityCurrent(camera));
    RestoreLocalCamera(sameCameraWorld);
    gCrawlClose=false;
    if (certifiedLivingCharacter && gLocal.character!=0U)
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(gLocal.character));
    gLocal={};
    alien_pvp_input::Uninstall();
}
void Pump(const Context& context) noexcept {
    gPilotThread.store(GetCurrentThreadId(),std::memory_order_release);
    // Donor camera.cpp OnSoundListenerEnter: while the pilot drives the Alien's
    // camera, the room/audio listener follows the Alien (one frame behind).
    gListenerCharacter.store(Drives(gLocal.character,gLocal.generation)?gLocal.character:0U,std::memory_order_release);
    const bool current=context.exclusiveLease && context.moduleBase!=0U &&
        context.alien!=0U && context.technicalPlayer!=0U &&
        context.nativeGeneration!=0U && context.localPeer!=0U &&
        context.ownerPeer==context.localPeer;
    if (!current) {
        const bool waiting=context.possessionRequested && context.moduleBase && context.technicalPlayer &&
            context.nativeGeneration && context.ownerPeer==context.localPeer;
        if (waiting) {
            if (gLocal.generation && gLocal.generation!=context.nativeGeneration) Reset(false,false);
            gLocal.generation=context.nativeGeneration;
            const auto waitingNow=GetTickCount64();
            if (!alien_pvp_input::Installed() && waitingNow>=gLocal.nextInputAttemptMs) {
                gLocal.nextInputAttemptMs=waitingNow+3000U;
                static_cast<void>(alien_pvp_input::EnsureInstalled(context.moduleBase));
            }
            if (!alien_pvp_input::Installed()) return;
            alien_pvp_input::SetActive(InputAllowed(context.moduleBase));
            // Keep the original technical identity/home so cancellation also
            // restores a role which never obtained an Alien camera.
            if (!gLocal.camera.technical) {
                CameraBinding camera{};
                if (!CurrentCameraRoot(context.moduleBase,camera)) return;
                camera.technical=context.technicalPlayer;camera.generation=context.nativeGeneration;
                if (!Read(camera.technical+0x50U,camera.technicalRuntime) ||
                    !Read(camera.technical+kCharacterSkeletonOffset,camera.technicalSkeleton) ||
                    !TechnicalIdentityCurrent(camera) || !PrepareTechnicalHome(context)) return;
                gLocal.camera=camera;gCameraBase=context.moduleBase;
            }
            // Like the donor, park only after an Alien exists and its camera
            // can be attached. Moving THE_PLAYER earlier can disturb the map's
            // initial spawn triggers. The waiting overlay covers the camera.
            gLocal.visualReady=TechnicalIdentityCurrent(gLocal.camera) &&
                alien_pvp_visual::HideTechnical(context.technicalPlayer,context.nativeGeneration);
            alien_pvp::CaptureLocal({});
            return;
        }
        if (gLocal.character!=0U || gLocal.camera.technical!=0U)
            Reset(context.nativeGeneration!=0U && gLocal.generation==context.nativeGeneration,false);
        else {
            // Round 10 survivor stun: freeze this survivor's input for 1.5 s.
            const auto stunNow=GetTickCount64();
            TickSurvivorStun(context,stunNow);
            const bool stunned=stunNow<gSurvivorStunUntil;
            if(stunned && !alien_pvp_input::Installed())static_cast<void>(alien_pvp_input::EnsureInstalled(context.moduleBase));
            alien_pvp_input::SetActive(stunned && alien_pvp_input::Installed());
        }
        return;
    }
    if (gLocal.character!=0U && (gLocal.character!=context.alien ||
        gLocal.generation!=context.nativeGeneration))
        Reset(false,false);
    if (gLocal.character==0U) {
        gLocal.character=context.alien;
        gLocal.generation=context.nativeGeneration;
    }
    const auto now=GetTickCount64();
    if (!alien_pvp_input::Installed() && now>=gLocal.nextInputAttemptMs) {
        gLocal.nextInputAttemptMs=now+3000U;
        static_cast<void>(alien_pvp_input::EnsureInstalled(context.moduleBase));
    }
    if (!alien_pvp_input::Installed()) return; // Amanda input must be isolated first.
    if (gLocal.camera.bound && (gLocal.camera.alien!=context.alien ||
        gLocal.camera.technical!=context.technicalPlayer))
        RestoreLocalCamera(true);
    if (gLocal.camera.bound && !CameraStillOwned(context)) {
        // The engine selected a cinematic or another camera; never hijack it.
        gCameraLost.fetch_add(1U,std::memory_order_relaxed);
        alien_pvp_input::SetActive(false);
        RestoreLocalCamera(true);
    }
    const bool attached=BindCamera(context,now);
    gCameraGeneration.store(attached?context.nativeGeneration:0U,std::memory_order_release);
    if(attached)static_cast<void>(ConfigureFollow(gLocal.camera,gLocal.camera.firstPerson));
    // Retail can re-enable a submodel after a camera/animation transition.
    // Reassert head visibility only while we own the exact current Alien.
    if (attached && gLocal.camera.firstPerson &&
        !alien_pvp_visual::HideAlienHead(context.alien,context.nativeGeneration)) {
        const auto camera=gLocal.camera;
        if (CameraCall(context.moduleBase,camera.manager,camera.follow,context.alien)) {
            gLocal.camera.firstPerson=false;
            alien_pvp_visual::RestoreAlienHead(context.alien,context.nativeGeneration,true);
        }
    }
    gLocal.visualReady=attached && TechnicalIdentityCurrent(gLocal.camera) &&
        alien_pvp_visual::HideTechnical(context.technicalPlayer,context.nativeGeneration) &&
        ParkTechnical(context);
    if (!gLocal.visualReady && attached)
        alien_pvp_visual::RestoreTechnical(context.technicalPlayer,context.nativeGeneration,
            TechnicalIdentityCurrent(gLocal.camera));
    alien_pvp_input::SetActive(gLocal.visualReady && InputAllowed(context.moduleBase));
    // Re-assert every 250 ms: the game re-shows the overlay on its own events.
    if (gLocal.visualReady && now-gLocal.hudAt>=250U) {
        gLocal.hudAt=now;SetGameHudHidden(context.moduleBase,true);gLocal.hudHidden=true;
    } else if (!gLocal.visualReady && gLocal.hudHidden) {
        SetGameHudHidden(context.moduleBase,false);gLocal.hudHidden=false;
    }
    if (gLocal.visualReady) TickInput(context,now);
    else alien_pvp::CaptureLocal({});
}
} // namespace aimp::alien_pvp_local
