#include "aimp/AlienPvPControlRuntime.h"
#include "aimp/AlienPvPControlPolicy.h"
#include "aimp/AlienPvPDelivery.h"
#include "aimp/AlienPvPSpatialPolicy.h"
#include "aimp/AlienPvPMovementMath.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

namespace aimp::alien_pvp {
namespace {
std::mutex gQueueMutex;
Mailbox<64> gIncoming,gOutgoing;
std::atomic<std::uint64_t> gReceived{},gRejected{},gExpired{},gDropped{},gInputs{},gCommits{},gPoses{},gCorrections{},gUnsupported{};
std::atomic<std::uint64_t> gHeld{},gDriven{},gBlockedAction{},gBlockedVent{},gBlockedStale{},gBlockedStage{},gDriveRefused{};
std::atomic<std::uint64_t> gChaseDriven{},gVentExpired{},gVentEpochCleared{},gStageRepaired{};
std::atomic<std::uint64_t> gCrawlPosesSent{},gCrawlMirror{},gCrawlHatch{},gOwnerBlend{},gCrawlFallback{};
// Pass 20: crawl tail (copy climbing out after the owner left the duct) and the
// observer blend running through a stuck action (avsfix19: frozen after a kill).
std::atomic<std::uint64_t> gCrawlTail{},gCrawlTailDone{},gBlendThroughBusy{},gBlendBlockedBusy{},gBlendInactive{};
std::atomic<std::uint64_t> gStunRoarAt{},gStunRoarTaken{};std::atomic<float> gStunRoarYaw{};
struct State final {
    Context context{};InputWindow window{};PredictionHistory history{};
    DeliveryInbox inbox{};DeliveryOutbox<> outbox{};bool deliveryFault{};std::uint64_t deliveryGeneration{};
    LocalInput local{};std::uint64_t capturedAt{};
    std::array<LocalInput,8> actions{};std::size_t actionRead{},actionCount{};
    std::uint64_t inputSequence{},poseSequence{},lastInputSent{},lastStatus{},lastPose{},lastTick{},lastAckAt{},lastRoar{};
    std::uint64_t appliedInput{},appliedHost{},correctionAt{};
    Frame correction{},lastObserved{},ventAuthority{},deferredTerminal{};
    std::uint64_t ventAuthorityAt{}; // when ventAuthority last changed
    bool hasDeferredTerminal{};
    bool moving{},hadPose{},ownsSlot{},expired{},recovering{},hasDeferredAction{},aligned{};
    Frame deferredAction{};std::uint64_t deferredActionAt{};
    std::uint64_t ventSettleSequence{},ventSettleAt{};
    float correctionX{},correctionZ{};bool predictionCorrection{};
    // Pass 15: observer of a Host-piloted Alien, replica stage != Host stage.
    std::uint64_t stageMismatchSince{},stageAttemptAt{};
    // Pass 17: observer replica stuck far from the Host pose (crawlspace hatch).
    std::uint64_t positionMismatchSince{},positionAttemptAt{};
    // Pass 19: observer replica driven by the owner's crawl (MirrorOwnerCrawl).
    bool crawlMirroring{};
    std::uint64_t crawlSeenAt{}; // pass 20: last fresh owner pose flagged Crawling
};
State g{}; // certified engine thread only
// A possession can restart on the SAME native Character/scope. A distinct
// delivery generation prevents old IDs/ACKs from aliasing the restarted stream.
// Advertised in Status.sequence; all reliable IDs use generation:counter32.
std::uint64_t gDeliveryGeneration{};

template<typename T> bool Read(std::uintptr_t at,T& value) noexcept {
    return at && crash_guard::TryCopy(&value,reinterpret_cast<const void*>(at),sizeof(T));
}
bool NativeStage(std::uintptr_t character,std::uint8_t& state,bool& backstage) noexcept {
    std::uintptr_t cycle{},owner{};std::uint8_t a{},b{};std::uint32_t dead{};
    if(!Read(character+0x2F90U,dead) || dead ||
        !Read(character+0x36CU,cycle) || !cycle || !Read(cycle+4U,owner) || owner!=character ||
        !Read(cycle+0x1AU,state) || !Read(character+0x2DAU,a) || !Read(character+0x2DBU,b))return false;
    backstage=spatial::Backstage(a,b);return true;
}
bool OrdinaryCycle(std::uintptr_t character) noexcept {
    std::uintptr_t cycle{},owner{};std::uint8_t state{};std::uint32_t dead{};
    return Read(character+0x2F90U,dead) && dead==0U &&
        Read(character+0x36CU,cycle) && cycle && Read(cycle+4U,owner) && owner==character &&
        Read(cycle+0x1AU,state) && (state==1U || state==55U || state==56U || state==57U || state==59U);
}
bool ReadPose(Frame& f) noexcept {
    if(!Read(g.context.character+0x70U,f.x) || !Read(g.context.character+0x74U,f.y) ||
        !Read(g.context.character+0x78U,f.z) || !Read(g.context.character+0x174U,f.yaw) ||
        !std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z) || !std::isfinite(f.yaw) ||
        std::abs(f.x)>100000.0F || std::abs(f.y)>100000.0F || std::abs(f.z)>100000.0F)return false;
    // Retail yaw is an angle, not necessarily already in the wire interval.
    // Normalize only the copied observation; never rewrite the Character here.
    f.yaw=WrapYaw(f.yaw);
    return true;
}
void Publish(Frame frame,std::uint64_t now) noexcept {
    frame.scope=g.context.scope;frame.sampledAtMs=now;
    if(!Valid(frame)){gRejected.fetch_add(1U,std::memory_order_relaxed);return;}
    if(Reliable(frame)) {
        if(!g.outbox.Push(frame,now)){g.deliveryFault=true;gDropped.fetch_add(1U,std::memory_order_relaxed);}
        return;
    }
    std::unique_lock lock(gQueueMutex,std::try_to_lock);
    if(!lock.owns_lock() || !gOutgoing.Push({frame,now}))gDropped.fetch_add(1U,std::memory_order_relaxed);
}
void FlushReliable(std::uint64_t now) noexcept {
    if(g.outbox.Expired(now))g.deliveryFault=true;
    Frame frame{};if(!g.outbox.Due(now,frame))return;
    std::unique_lock lock(gQueueMutex,std::try_to_lock);
    if(lock.owns_lock())static_cast<void>(gOutgoing.Push({frame,now}));
    // Retained even if the bounded output queue or lock was unavailable.
}
std::uint32_t Flags(std::uint64_t now) noexcept {
    return Bound|(g.context.localOwner?LocalOwner:0U)|(g.context.host?Host:0U)|
        (g.context.pilotReady&&!g.deliveryFault?PilotReady:0U)|(g.local.focused?Focused:0U)|
        (g.moving?Moving:0U)|(g.window.ActiveAt(now)?Active:0U)|(g.expired?Expired:0U);
}
// Source: the donor's ActAlienVoice/GameTick: NPC audio component +11A0,
// thiscall 00784230(type). No behavior tree, random auto-voice, or global hook.
// The caller already holds AIMP's exact-build, engine-thread incarnation proof.
bool Roar() noexcept {
#if defined(_MSC_VER) && !defined(AIMP_ALIEN_PVP_TEST)
    __try {
        reinterpret_cast<void(__thiscall*)(void*,int)>(g.context.base+0x00384230U)(
            reinterpret_cast<void*>(g.context.character+0x11A0U),1);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
bool ActionBusy() noexcept {
    return g.context.actions && g.context.actions->busy && g.context.actions->busy(g.context);
}
// AvS round 5 auto-chase state of the pending attack (see ActionHooks::chase).
int ChaseState(float& yaw) noexcept {
    return g.context.actions && g.context.actions->chase ? g.context.actions->chase(g.context,yaw) : 0;
}
// Busy for movement purposes: a chasing attack still lets the Alien move.
bool MovementBusy() noexcept {
    float yaw{};
    return ActionBusy() && ChaseState(yaw)==0;
}
void ReleaseCarrier() noexcept {
    if(g.ownsSlot && character_direct_step_runtime::ReleaseAIIntentOwnership(g.context.character))g.ownsSlot=false;
    g.moving=false;
}
void SetVentAuthority(const Frame& frame,std::uint64_t now) noexcept {
    g.ventAuthority=frame;g.ventAuthorityAt=now;
}
// AvS pass 10 (2026-11-26): the HUD stayed on "Entering ventilation... E / Q"
// and the Alien never moved again. An E press marks ventAuthority Preparing on
// the pilot's PC at once; the Host had just revoked the possession epoch (crawl
// first-person switch), so that intent was never committed and no Resolution
// ever arrived. Nothing cleared Preparing, which blocks all movement. A vent
// request without a Host answer now expires; the native action, when one is
// running here, keeps its own bounded timeouts (ActionBusy skips this).
void PublishAction(Frame result,std::uint64_t now) noexcept {
    if(g.context.host && (result.action==Action::EnterVent || result.action==Action::ExitVent))SetVentAuthority(result,now);
    if(g.context.localOwner && result.phase==ActionPhase::Completed &&
        (result.action==Action::EnterVent || result.action==Action::ExitVent) &&
        result.hostSequence==g.ventAuthority.hostSequence)SetVentAuthority(result,now);
    result.scope=g.context.scope;result.flags=Flags(now);
    result.kind=g.context.host && result.action!=Action::Roar?Kind::Resolution:Kind::ActionResult;
    if(result.phase==ActionPhase::Started || result.phase==ActionPhase::Completed) {
        result.flags|=ActionApplied;
        // Stationary backstage produces no locomotion step. Its measured pose
        // must still acknowledge the native action that changed its stage.
        g.appliedInput=std::max(g.appliedInput,result.sequence);
        g.appliedHost=std::max(g.appliedHost,result.hostSequence);
    }
    if(result.reason==ActionReason::NativeUnavailable) {
        result.flags|=UnsupportedAction;gUnsupported.fetch_add(1U,std::memory_order_relaxed);
    }
    if(result.kind==Kind::Resolution)g.window.AcknowledgeLocalResolution(result,now);
    Publish(result,now);
}
bool CancelRecovery() noexcept {
    const bool retired=!g.context.actions || !g.context.actions->cancelRecovery ||
        g.context.actions->cancelRecovery(g.context,true);
    g.recovering=!retired;return retired;
}
bool StopDonorMovement() noexcept {
    // Round 10: a running crawl path move is movement too; retire it first.
    const bool crawlStopped=!g.context.actions || !g.context.actions->crawl || g.context.actions->crawl(g.context,nullptr);
    return crawlStopped && (!g.context.actions || !g.context.actions->drive || g.context.actions->drive(g.context,nullptr));
}
bool VentAction(const Frame& frame) noexcept {
    return frame.action==Action::EnterVent || frame.action==Action::ExitVent;
}
constexpr std::uint64_t kVentRequestTimeoutMs=4000U,kVentRunTimeoutMs=30000U;
void ExpireVentAuthority(std::uint64_t now) noexcept {
    if(!VentAction(g.ventAuthority) || g.hasDeferredAction || ActionBusy())return;
    const auto phase=g.ventAuthority.phase;
    const auto limit=phase==ActionPhase::Preparing?kVentRequestTimeoutMs:
        phase==ActionPhase::Started?kVentRunTimeoutMs:0U;
    if(!limit)return;
    if(!g.ventAuthorityAt || now<g.ventAuthorityAt){g.ventAuthorityAt=now;return;}
    if(now-g.ventAuthorityAt<limit)return;
    g.ventAuthority.phase=ActionPhase::Failed;g.ventAuthority.reason=ActionReason::Timeout;g.ventAuthorityAt=now;
    gVentExpired.fetch_add(1U,std::memory_order_relaxed);
}
bool SettleVent(const Frame& frame,std::uint64_t now) noexcept {
    if(!VentAction(frame))return false;
    if(g.ventSettleSequence!=frame.hostSequence) {
        g.ventSettleSequence=frame.hostSequence;g.ventSettleAt=now;
    }
    // Donor vent.cpp::VentTick closes both gates for 300ms after the key.
    // Releasing a kind9 Ref alone does not settle the locomotor that frame.
    if(now-g.ventSettleAt>=300U)return false;
    if(!g.hasDeferredAction)g.deferredActionAt=now;
    g.deferredAction=frame;g.hasDeferredAction=true;
    static_cast<void>(StopDonorMovement());static_cast<void>(CancelRecovery());
    ReleaseCarrier();return true;
}
void ApplyAction(const Frame& command,std::uint64_t now) noexcept {
    if(command.action==Action::None || (command.flags&Focused)==0U)return;
    Frame result=command;result.resolutionRevision=1U;
    if(command.action==Action::CancelVent) {
        if(!g.context.host)return;
        if(g.hasDeferredAction && VentAction(g.deferredAction)) {
            result=g.deferredAction;g.hasDeferredAction=false;
            result.phase=ActionPhase::Rejected;result.reason=ActionReason::NativeBusy;
            result.resolutionRevision=1U;PublishAction(result,now);
        } else if(g.context.actions && g.context.actions->interruptVent &&
            g.context.actions->interruptVent(g.context,result))PublishAction(result,now);
        return;
    }
    if(command.action==Action::Roar && command.pitch>1.4F) {
        // Round 10 stun roar (Alien flamed during a chase): every PC gets the
        // pilot's Commit; the survivor in the yaw direction stuns itself.
        gStunRoarYaw.store(command.yaw,std::memory_order_relaxed);
        gStunRoarAt.store(now,std::memory_order_release);
    }
    if(command.action==Action::Roar) {
        result.phase=ActionPhase::Rejected;result.reason=ActionReason::NativeBusy;
        if((!g.lastRoar || now-g.lastRoar>=2000U) && !ActionBusy() && OrdinaryCycle(g.context.character) && Roar()) {
            result.phase=ActionPhase::Completed;result.reason=ActionReason::None;g.lastRoar=now;
        }
        PublishAction(result,now);return;
    }
    // Non-hosts never resolve targets or portals from their own local view.
    // Attack execution is carried by the existing typed AISync pair stream;
    // vent execution waits for the Host-selected real portal resolution.
    if(!g.context.host)return;
    if(!ActionBusy() && SettleVent(command,now))return;
    if(!StopDonorMovement() || !CancelRecovery()) {
        if(!g.hasDeferredAction) {g.deferredAction=command;g.deferredActionAt=now;g.hasDeferredAction=true;}
        else if(g.deferredAction.hostSequence!=command.hostSequence) {
            result.phase=ActionPhase::Rejected;result.reason=ActionReason::NativeBusy;PublishAction(result,now);
        }
        return;
    }
    if(!ActionBusy())ReleaseCarrier();
    if(!g.context.actions || !g.context.actions->begin || !g.context.actions->begin(g.context,command,result)) {
        result=command;result.resolutionRevision=1U;result.phase=ActionPhase::Rejected;
        result.reason=ActionReason::NativeUnavailable;
    }
    PublishAction(result,now);
}
void ApplyResolution(const Frame& plan,std::uint64_t now) noexcept {
    if(g.context.host)return;
    if(plan.action==Action::EnterVent || plan.action==Action::ExitVent)SetVentAuthority(plan,now);
    // Host may finish a short exit within our 300ms settling interval. Its
    // Completed receipt must not discard the queued local Started operation.
    if(VentAction(plan) && plan.phase==ActionPhase::Completed && g.hasDeferredAction &&
        g.deferredAction.kind==Kind::Resolution && g.deferredAction.phase==ActionPhase::Started &&
        g.deferredAction.hostSequence==plan.hostSequence) {
        g.deferredTerminal=plan;g.hasDeferredTerminal=true;return;
    }
    if(plan.phase==ActionPhase::Rejected || plan.phase==ActionPhase::Failed || plan.phase==ActionPhase::Cancelled)
        g.hasDeferredTerminal=false;
    if(plan.phase==ActionPhase::Started && !ActionBusy() && SettleVent(plan,now))return;
    if((!StopDonorMovement() || !CancelRecovery()) && (plan.phase==ActionPhase::Preparing || plan.phase==ActionPhase::Started)) {
        if(!g.hasDeferredAction || g.deferredAction.hostSequence==plan.hostSequence) {
            if(!g.hasDeferredAction)g.deferredActionAt=now;
            g.deferredAction=plan;g.hasDeferredAction=true;
        }
        return;
    }
    if(g.hasDeferredAction && g.deferredAction.hostSequence==plan.hostSequence)g.hasDeferredAction=false;
    if(plan.phase==ActionPhase::Started && !ActionBusy())ReleaseCarrier();
    Frame receipt=plan;
    if(g.context.actions && g.context.actions->begin && g.context.actions->begin(g.context,plan,receipt))
        PublishAction(receipt,now);
    if(plan.phase==ActionPhase::Started && !g.hasDeferredAction && g.hasDeferredTerminal &&
        g.deferredTerminal.hostSequence==plan.hostSequence) {
        const auto terminal=g.deferredTerminal;g.hasDeferredTerminal=false;
        ApplyResolution(terminal,now);
    }
}
void PollAction(std::uint64_t now) noexcept {
    if(g.hasDeferredAction) {
        if(!g.window.ActiveAt(now) || !Fresh(now,g.deferredActionAt,1500U)) {
            Frame failed=g.deferredAction;failed.phase=ActionPhase::Rejected;failed.reason=ActionReason::NativeBusy;
            failed.resolutionRevision=std::max(std::uint64_t{1},failed.resolutionRevision+1U);
            g.hasDeferredAction=false;g.hasDeferredTerminal=false;PublishAction(failed,now);
        } else if(StopDonorMovement() && CancelRecovery()) {
            const auto pending=g.deferredAction;g.hasDeferredAction=false;
            if(pending.kind==Kind::Resolution)ApplyResolution(pending,now);else ApplyAction(pending,now);
        }
    }
    if(!g.context.actions || !g.context.actions->poll)return;
    if(g.window.Granted() && !g.window.ActiveAt(now) && g.context.actions->cancel)
        g.context.actions->cancel(g.context,true);
    Frame result{};
    if(g.context.actions->poll(g.context,result))PublishAction(result,now);
}
void Drain(std::uint64_t now) noexcept {
    std::array<TimedFrame,32> pending{};std::size_t count{};
    {
        std::unique_lock lock(gQueueMutex,std::try_to_lock);
        if(!lock.owns_lock())return;
        while(count<pending.size() && gIncoming.Pop(pending[count]))++count;
    }
    for(std::size_t i=0;i<count;++i) {
        const auto& value=pending[i];const auto previousEpoch=g.window.Epoch();
        const auto& f=value.frame;
        if(f.scope!=g.context.scope){gRejected.fetch_add(1U,std::memory_order_relaxed);continue;}
        if(f.kind==Kind::Acknowledge){static_cast<void>(g.outbox.Acknowledge(f));continue;}
        if(f.deliveryId) {
            const auto verdict=g.inbox.Inspect(f.deliveryId);
            if(verdict==DeliveryInbox::Verdict::Gap)continue;
            if(verdict==DeliveryInbox::Verdict::Duplicate){Publish(Acknowledgement(f),now);continue;}
            // Complete delivery even if gameplay validation rejects a stale
            // epoch. Acknowledging receipt must never certify native execution.
            static_cast<void>(g.inbox.Commit(f.deliveryId));
            Publish(Acknowledgement(f),now);
        }
        if(f.deliveryId && f.sampledAtMs && f.kind!=Kind::Stop &&
            !Fresh(now,f.sampledAtMs,f.kind==Kind::Commit?500U:kDeliveryDeadlineMs)) {
            g.deliveryFault=true;gRejected.fetch_add(1U,std::memory_order_relaxed);continue;
        }
        if(!g.window.Accept(value.frame,value.at,now)) {
            gRejected.fetch_add(1U,std::memory_order_relaxed);continue;
        }
        if(f.kind==Kind::Stop){g.deliveryFault=false;g.hasDeferredAction=false;g.hasDeferredTerminal=false;}
        // Pass 10: a revoked/regranted epoch drops every uncommitted intent of
        // the old one, and cancel() below ends any native vent run here. A vent
        // request still marked Preparing/Started would never be answered.
        if((f.kind==Kind::Stop || (f.kind==Kind::Activate && previousEpoch!=f.epoch)) && VentAction(g.ventAuthority) &&
            (g.ventAuthority.phase==ActionPhase::Preparing || g.ventAuthority.phase==ActionPhase::Started)) {
            g.ventAuthority={};g.ventAuthorityAt=0U;gVentEpochCleared.fetch_add(1U,std::memory_order_relaxed);
        }
        if(f.kind==Kind::Stop || (f.kind==Kind::Activate && previousEpoch!=f.epoch)) {
            if(g.context.actions && g.context.actions->cancel)g.context.actions->cancel(g.context,true);
        }
        if(f.kind==Kind::Activate && previousEpoch!=f.epoch) {
            g.inputSequence=g.poseSequence=g.appliedInput=g.appliedHost=0U;
            g.ventSettleSequence=g.ventSettleAt=0U;
            g.history.Clear();g.lastInputSent=0U;g.lastAckAt=now;g.predictionCorrection=false;
            g.correction={};g.correctionAt=0U;g.lastObserved={};g.hadPose=false;g.lastPose=0U;
            g.actionRead=g.actionCount=0U;g.local.action=Action::None;g.hasDeferredAction=false;g.hasDeferredTerminal=false;
        } else if(f.kind==Kind::Commit) {
            gCommits.fetch_add(1U,std::memory_order_relaxed);g.lastAckAt=value.at;
            ApplyAction(f,now);
        } else if(f.kind==Kind::Resolution) {
            ApplyResolution(f,now);
        } else if(f.kind==Kind::Correction && (!g.context.host || (f.flags&OwnerMeasured)!=0U)) {
            // The owner already rendered this measured position. Feeding the
            // echoed sample back into its predictor creates a latency loop.
            if(g.context.localOwner && (f.flags&OwnerMeasured)!=0U)continue;
            std::uint8_t initialState{};bool initialBackstage{};
            const bool initialStage=NativeStage(g.context.character,initialState,initialBackstage);
            const bool sameStage=initialStage && initialBackstage==((f.flags&Backstage)!=0U);
            const bool initialIdle=OrdinaryCycle(g.context.character) ||
                (initialBackstage && initialState==77U);
            if(!g.aligned && (f.flags&(PoseValid|NativeBusy))==PoseValid &&
                sameStage && initialIdle && !ActionBusy()) {
                // AIMP fences ordinary spawn/lifecycle replay for the possessed
                // Alien. Apply the Host's first authenticated placement once,
                // before predicting any movement from a different local spawn.
                const float position[]{f.x,f.y,f.z};
                g.aligned=character_direct_step_runtime::ApplyAIRecoveryTransform(g.context.character,position,f.yaw);
                if(g.aligned){g.history.Clear();g.hadPose=false;g.predictionCorrection=false;}
            }
            g.correction=f;g.correctionAt=value.at;g.predictionCorrection=false;
            if(g.context.localOwner)
                g.predictionCorrection=g.history.Error(f.acknowledgedInput,f.x,f.y,f.z,g.correctionX,g.correctionZ);
        }
    }
}
void StopMovement() noexcept {
    if(!g.ownsSlot){g.moving=false;return;}
    if(OrdinaryCycle(g.context.character)) {
        std::string error;
        static_cast<void>(character_direct_step_runtime::HoldAIIntentIdle(g.context.character,1U,error));
    } else {
        // A cutscene, vent or paired capture owns its own native lifetime.
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(g.context.character));
        g.ownsSlot=false;
    }
    g.moving=false;
}
// Pass 15 (avsfix14): on a survivor PC with the Host piloting the Alien, the
// replica's stage only comes from generic replay, which can miss a vent exit
// or entry (seen: hidden backstage 40+ s beside the survivor). Once the Host
// has been idle in the other stage for kStageRepairMs, put the replica there.
constexpr std::uint64_t kStageRepairMs=1000U;
void RepairObserverStage(std::uint64_t now) noexcept {
    if(g.context.host || g.context.localOwner || !g.context.actions || !g.context.actions->reconcileStage ||
        !g.correctionAt || (g.correction.flags&OwnerMeasured)!=0U){g.stageMismatchSince=0U;return;}
    std::uint8_t state{};bool backstage{};
    if(!NativeStage(g.context.character,state,backstage) || (g.correction.flags&PoseValid)==0U ||
        backstage==((g.correction.flags&Backstage)!=0U)){g.stageMismatchSince=0U;return;}
    // Host still in a passage/pair (NativeBusy) or its pose is stale: wait.
    if((g.correction.flags&NativeBusy)!=0U || !Fresh(now,g.correctionAt,300U)){g.stageMismatchSince=0U;return;}
    if(!g.stageMismatchSince){g.stageMismatchSince=now;return;}
    if(now-g.stageMismatchSince<kStageRepairMs || (g.stageAttemptAt && now-g.stageAttemptAt<500U))return;
    g.stageAttemptAt=now;
    if(g.context.actions->reconcileStage(g.context,g.correction)) {
        gStageRepaired.fetch_add(1U,std::memory_order_relaxed);
        g.stageMismatchSince=0U;
    }
}
// Pass 17 (avsfix16 test): the replica's generic route cannot take a crawlspace
// hatch. It stood at the hatch while the Host crawled through, then walked the
// long way round (error 9-10 m for ~10 s). Same stage, Host pose fresh and not
// busy, replica > kSnapPlanarM away (or another height) for kSnapAfterMs ->
// place it at the Host pose (reconcileStage with an equal stage = snap).
constexpr float kSnapPlanarM=3.0F,kSnapVerticalM=1.2F;
constexpr std::uint64_t kSnapAfterMs=700U,kSnapRetryMs=400U;
void RepairObserverPosition(std::uint64_t now) noexcept {
    const auto reset=[]() noexcept {g.positionMismatchSince=0U;};
    if(g.context.host || g.context.localOwner || !g.context.actions || !g.context.actions->reconcileStage ||
        !g.correctionAt || (g.correction.flags&OwnerMeasured)!=0U){reset();return;}
    if((g.correction.flags&(PoseValid|NativeBusy|Backstage))!=PoseValid || !Fresh(now,g.correctionAt,500U)){reset();return;}
    std::uint8_t state{};bool backstage{};Frame here{};
    if(!NativeStage(g.context.character,state,backstage) || backstage || !ReadPose(here)){reset();return;}
    const float dx=g.correction.x-here.x,dz=g.correction.z-here.z,dy=g.correction.y-here.y;
    if(!std::isfinite(dx) || !std::isfinite(dz) || !std::isfinite(dy) ||
        (dx*dx+dz*dz<kSnapPlanarM*kSnapPlanarM && std::abs(dy)<kSnapVerticalM)){reset();return;}
    if(!g.positionMismatchSince){g.positionMismatchSince=now;return;}
    if(now-g.positionMismatchSince<kSnapAfterMs || (g.positionAttemptAt && now-g.positionAttemptAt<kSnapRetryMs))return;
    g.positionAttemptAt=now;
    if(g.context.actions->reconcileStage(g.context,g.correction))reset();
}
// Pass 19 (puppet Alien): while the owner's poses say Crawling (crawl mode or
// its hatch traversal), an observer moves the replica with the SAME retail
// crawl path move the owner uses, aimed at the owner pose. The retail path
// takes the crawlspace hatch link, so the replica plays the hatch climb (18)
// and the duct crawl instead of gliding through the wall. Its own traversal is
// never interrupted. More than kCrawlFallbackM behind: hand back to the
// ordinary owner-pose blend, which closes the gap.
// Pass 20 (avsfix19 test: the copy kept the crawl posture after the exit): the
// copy fell > 4 m behind twice (crawlFallback 2) and the blend dragged it out of
// the duct without its own exit climb. Now 8 m / 2.5 m height, and after the
// owner's Crawling flag clears the copy keeps the path move toward the owner for
// up to kCrawlTailMs, so it climbs out through the hatch itself (its own 18).
constexpr float kCrawlFallbackM=8.0F,kCrawlArriveM=0.4F,kCrawlTailArriveM=0.7F,kCrawlHeightM=2.5F;
constexpr std::uint64_t kCrawlTailMs=3000U;
bool MirrorOwnerCrawl(std::uint64_t now) noexcept {
    const auto stop=[]() noexcept {
        if(g.crawlMirroring && g.context.actions && g.context.actions->crawl)
            static_cast<void>(g.context.actions->crawl(g.context,nullptr));
        g.crawlMirroring=false;g.crawlSeenAt=0U;
    };
    const bool crawling=g.correctionAt && Fresh(now,g.correctionAt,400U) &&
        (g.correction.flags&(PoseValid|Crawling|Backstage))==(PoseValid|Crawling);
    if(crawling)g.crawlSeenAt=now;
    const bool tail=!crawling && g.crawlMirroring && g.crawlSeenAt && now-g.crawlSeenAt<kCrawlTailMs &&
        g.correctionAt && Fresh(now,g.correctionAt,400U) &&
        (g.correction.flags&(PoseValid|Backstage))==PoseValid;
    if(g.context.localOwner || !g.context.actions || !g.context.actions->crawl || (!crawling && !tail) ||
        ActionBusy()){stop();return false;}
    std::uint8_t state{};bool backstage{};Frame body{};
    if(!NativeStage(g.context.character,state,backstage) || backstage || !ReadPose(body)){stop();return false;}
    if(state==18U) {
        // The replica's own hatch traversal: let retail finish it.
        g.crawlMirroring=true;g.moving=true;gCrawlHatch.fetch_add(1U,std::memory_order_relaxed);return true;
    }
    const float lead=std::min(0.2F,static_cast<float>(now-g.correctionAt)/1000.0F+0.1F);
    const float dx=g.correction.x+g.correction.velocityX*lead-body.x;
    const float dz=g.correction.z+g.correction.velocityZ*lead-body.z;
    const float dist=std::hypot(dx,dz);
    if(!std::isfinite(dist) || dist>kCrawlFallbackM || std::abs(g.correction.y-body.y)>kCrawlHeightM) {
        if(g.crawlMirroring)gCrawlFallback.fetch_add(1U,std::memory_order_relaxed);
        stop();return false;
    }
    if(tail && dist<kCrawlTailArriveM) {
        // Out of the duct and caught up: hand back to the owner-pose blend.
        gCrawlTailDone.fetch_add(1U,std::memory_order_relaxed);stop();return false;
    }
    static_cast<void>(CancelRecovery());ReleaseCarrier();
    if(g.context.actions->drive)static_cast<void>(g.context.actions->drive(g.context,nullptr));
    g.crawlMirroring=true;
    if(tail)gCrawlTail.fetch_add(1U,std::memory_order_relaxed);
    if(dist<kCrawlArriveM) {
        static_cast<void>(g.context.actions->crawl(g.context,nullptr));g.moving=false;
    } else {
        Frame intent{};intent.forward=1.0F;intent.strafe=0.0F;intent.yaw=std::atan2(dx,dz);intent.gait=1U;
        g.moving=g.context.actions->crawl(g.context,&intent);
    }
    gCrawlMirror.fetch_add(1U,std::memory_order_relaxed);
    g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();
    return true;
}
void ApplyMovement(std::uint64_t now,float dt) noexcept {
    RepairObserverStage(now);
    RepairObserverPosition(now);
    if(!g.context.host && !g.aligned && (g.context.localOwner || !g.correctionAt ||
        (g.correction.flags&OwnerMeasured)==0U)){static_cast<void>(StopDonorMovement());ReleaseCarrier();return;}
    const bool active=g.window.ActiveAt(now) && !g.deliveryFault;
    const bool fresh=g.context.localOwner ?
        active && g.context.pilotReady && Fresh(now,g.capturedAt,150U) &&
            Fresh(now,g.lastAckAt,kAuthorityTimeoutMs) : g.window.InputFresh(now);
    const bool held=g.context.localOwner && g.local.focused && (g.local.forward!=0.0F || g.local.strafe!=0.0F);
    if(held)gHeld.fetch_add(1U,std::memory_order_relaxed);
    // Pass 20: an observer of an owner-measured Alien keeps following the owner
    // poses while a receipt is still open, unless its replica is really inside
    // a native action (pair 78/79, vent, traversal) or the owner pose says busy.
    // avsfix19: after a failed locker kill the survivor PC's receipt stayed open,
    // the blend never ran again and the copy stood 5 m off for over a minute.
    bool followThroughBusy=false;
    if(!g.context.localOwner && (ActionBusy() || g.hasDeferredAction) && g.correctionAt &&
        Fresh(now,g.correctionAt,250U) &&
        (g.correction.flags&(PoseValid|OwnerMeasured|NativeBusy))==(PoseValid|OwnerMeasured)) {
        followThroughBusy=OrdinaryCycle(g.context.character);
        (followThroughBusy?gBlendThroughBusy:gBlendBlockedBusy).fetch_add(1U,std::memory_order_relaxed);
    }
    if((ActionBusy() || g.hasDeferredAction) && !followThroughBusy) {
        float chaseYaw{};
        const int chase=g.hasDeferredAction?0:ChaseState(chaseYaw);
        if(chase!=0 && g.context.localOwner && g.context.actions && g.context.actions->drive) {
            // Round 5 auto-chase: run the Alien at the chosen survivor through
            // the same native direction request as WASD; hold when close.
            static_cast<void>(CancelRecovery());ReleaseCarrier();
            if(chase==1) {
                Frame intent{};intent.forward=1.0F;intent.strafe=0.0F;intent.yaw=chaseYaw;intent.gait=3U;
                g.moving=g.context.actions->drive(g.context,&intent);
                gChaseDriven.fetch_add(1U,std::memory_order_relaxed);
            } else {static_cast<void>(StopDonorMovement());g.moving=false;}
            return;
        }
        if(chase==0 || g.context.localOwner) {
            if(held)gBlockedAction.fetch_add(1U,std::memory_order_relaxed);
            static_cast<void>(StopDonorMovement());static_cast<void>(CancelRecovery());ReleaseCarrier();return;
        }
        // Host of a chasing Client pilot: keep following its measured poses.
    }
    if(g.context.localOwner && VentAction(g.ventAuthority) &&
        (g.ventAuthority.phase==ActionPhase::Preparing || g.ventAuthority.phase==ActionPhase::Started)) {
        if(held)gBlockedVent.fetch_add(1U,std::memory_order_relaxed);
        static_cast<void>(StopDonorMovement());static_cast<void>(CancelRecovery());ReleaseCarrier();return;
    }
    const bool remoteVent=!g.context.localOwner && VentAction(g.ventAuthority) &&
        g.ventAuthority.phase==ActionPhase::Started;
    if(!remoteVent && !followThroughBusy && MirrorOwnerCrawl(now))return;
    const bool ownerPose=!g.context.localOwner && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
        (g.correction.flags&(PoseValid|OwnerMeasured))==(PoseValid|OwnerMeasured) &&
        ((g.correction.flags&NativeBusy)==0U || (g.correction.flags&VentAnimating)!=0U) &&
        g.context.actions && g.context.actions->replicatePose;
    // Pass 20: a fresh owner-measured pose is itself the proof the pilot is live;
    // an inactive input window only means the committed input is not usable.
    if(ownerPose && !active)gBlendInactive.fetch_add(1U,std::memory_order_relaxed);
    if(ownerPose) {
        Frame body{};if(!ReadPose(body))return;
        auto target=g.correction;
        const float age=std::min(0.05F,static_cast<float>(now-g.correctionAt)/1000.0F);
        std::uint8_t state{};bool backstage{};
        const bool transition=NativeStage(g.context.character,state,backstage) && backstage!=((target.flags&Backstage)!=0U);
        const auto alpha=transition || std::hypot(target.x-body.x,target.z-body.z)>8.0F ? 1.0F : std::min(1.0F,dt*25.0F);
        target.x=body.x+(target.x+target.velocityX*age-body.x)*alpha;
        target.z=body.z+(target.z+target.velocityZ*age-body.z)*alpha;
        target.y=body.y+(target.y-body.y)*alpha;
        if(g.context.actions->drive) {
            const auto& input=g.window.Input();
            static_cast<void>(g.context.actions->drive(g.context,remoteVent || !active || followThroughBusy?nullptr:&input));
        }
        if(g.context.actions->replicatePose(g.context,target)) {
            gOwnerBlend.fetch_add(1U,std::memory_order_relaxed);
            g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();
            g.moving=std::hypot(target.velocityX,target.velocityZ)>0.05F;g.aligned=true;
        }
        return;
    }
    if(remoteVent){static_cast<void>(StopDonorMovement());ReleaseCarrier();return;}
    std::uint8_t cycle{};bool backstage{};
    const bool stageKnown=NativeStage(g.context.character,cycle,backstage);
    if(fresh && stageKnown && spatial::ManualBackstage(cycle,backstage,false)) {
        if(!StopDonorMovement()){ReleaseCarrier();return;}
        if(!CancelRecovery()){ReleaseCarrier();return;}
        ReleaseCarrier();
        const auto& committed=g.window.Input();
        const LocalInput input=g.context.localOwner?g.local:LocalInput{committed.forward,committed.strafe,
            committed.yaw,committed.pitch,committed.gait,Action::None,(committed.flags&Focused)!=0U};
        Frame body{};if(!ReadPose(body))return;
        float dx{},dz{};
        const bool moved=input.focused && spatial::BackstageStep(input.forward,input.strafe,input.yaw,input.gait,dt,dx,dz);
        bool corrected{};float cx{},cz{};
        if(!g.context.host && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
            (g.correction.flags&(PoseValid|Backstage))==(PoseValid|Backstage) &&
            (g.correction.flags&NativeBusy)==0U && std::abs(g.correction.y-body.y)<=1.0F) {
            if(g.context.localOwner && g.predictionCorrection) {
                corrected=spatial::BoundedStep(g.correctionX,g.correctionZ,4.0F*dt,cx,cz);
                g.correctionX-=cx;g.correctionZ-=cz;
            } else if(!g.context.localOwner)
                corrected=spatial::BoundedStep(g.correction.x-body.x,g.correction.z-body.z,8.0F*dt,cx,cz);
        }
        const float pos[]{body.x+dx+cx,body.y,body.z+dz+cz};
        if((moved || corrected) && character_direct_step_runtime::ApplyAIRecoveryTransform(
            g.context.character,pos,ApproachYaw(body.yaw,input.yaw,dt))) {
            g.moving=moved || corrected;g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();
            if(corrected)gCorrections.fetch_add(1U,std::memory_order_relaxed);
        }
        g.expired=false;return;
    }
    if(!fresh || !stageKnown || backstage || !OrdinaryCycle(g.context.character)) {
        if(held)(fresh?gBlockedStage:gBlockedStale).fetch_add(1U,std::memory_order_relaxed);
        static_cast<void>(StopDonorMovement());
        const bool expired=g.window.Granted() && !fresh;
        if(expired&&!g.expired)gExpired.fetch_add(1U,std::memory_order_relaxed);
        g.expired=expired;static_cast<void>(CancelRecovery());StopMovement();return;
    }
    g.expired=false;
    const auto& committed=g.window.Input();
    LocalInput input=g.context.localOwner?g.local:LocalInput{committed.forward,committed.strafe,
        committed.yaw,committed.pitch,committed.gait,Action::None,(committed.flags&Focused)!=0U};
    if(!input.focused){input.forward=input.strafe=0.0F;}
    Frame body{};
    if(!ReadPose(body)){StopMovement();return;}
    if(g.context.actions && g.context.actions->recover) {
        const bool ownerIdle=!g.context.localOwner || (input.forward==0.0F && input.strafe==0.0F);
        const bool ready=!g.context.host && ownerIdle && input.focused && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
            (g.correction.flags&PoseValid)!=0U && (g.correction.flags&(NativeBusy|Backstage))==0U;
        const float errorX=g.context.localOwner?g.correctionX:g.correction.x-body.x;
        const float errorZ=g.context.localOwner?g.correctionZ:g.correction.z-body.z;
        const bool substantial=ready && (!g.context.localOwner || g.predictionCorrection) && (std::hypot(errorX,errorZ)>4.0F ||
            std::abs(g.correction.y-body.y)>1.0F);
        if(substantial || g.recovering) {
            if(!ownerIdle) {
                // Fresh local steering takes priority over a path towards an
                // older Host pose. Retire recovery before resuming the donor.
                g.recovering=!CancelRecovery();
                if(g.recovering)return;
            } else {
                if(!StopDonorMovement()){ReleaseCarrier();return;}
                ReleaseCarrier();
                auto goal=g.correction;
                if(g.context.localOwner){goal.x=body.x+errorX;goal.z=body.z+errorZ;}
                g.recovering=g.context.actions->recover(g.context,ready?&goal:nullptr);
                if(g.recovering){g.moving=true;return;}
            }
        }
    }
    alien_pvp_movement::Plan plan{};
    const bool moving=alien_pvp_movement::TryPlan(input.forward,input.strafe,input.yaw,input.gait,plan);
    if(g.context.actions && g.context.actions->drive) {
        ReleaseCarrier();
        Frame intent{};intent.forward=input.forward;intent.strafe=input.strafe;
        intent.yaw=input.yaw;intent.gait=input.gait;
        // Feed bounded observer correction through the same native direction
        // request. The local owner keeps its immediate control and animation.
        if(!g.context.host && !g.context.localOwner && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
            (g.correction.flags&(PoseValid|NativeBusy|Backstage))==PoseValid) {
            const float dx=g.correction.x-body.x,dz=g.correction.z-body.z;
            if(std::hypot(dx,dz)>0.25F) {
                const float vx=(moving?plan.worldX:0.0F)+dx*0.5F;
                const float vz=(moving?plan.worldZ:0.0F)+dz*0.5F;
                intent.yaw=std::atan2(vx,vz);intent.forward=1.0F;intent.strafe=0.0F;
            }
        }
        if(g.context.localOwner && g.context.actions->crawl) {
            if(input.crawl && input.focused) {
                // Crawlspace: retail path move (takes the hatch links); the
                // kind9 direction request is retired while it runs.
                static_cast<void>(g.context.actions->drive(g.context,nullptr));
                g.moving=g.context.actions->crawl(g.context,&intent) && (intent.forward!=0.0F || intent.strafe!=0.0F);
                if(held)(g.moving?gDriven:gDriveRefused).fetch_add(1U,std::memory_order_relaxed);
                if(g.moving){g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();}
                return;
            }
            static_cast<void>(g.context.actions->crawl(g.context,nullptr));
        }
        g.moving=g.context.actions->drive(g.context,input.focused?&intent:nullptr) &&
            (intent.forward!=0.0F || intent.strafe!=0.0F) && input.focused;
        if(held)(g.moving?gDriven:gDriveRefused).fetch_add(1U,std::memory_order_relaxed);
        if(g.moving){g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();}
        return;
    }
    std::string error;bool applied{};
    // Progressive correction uses the already-existing no-snap native carrier,
    // exclusively in PvP. No Coop follower policy or transform writer changes.
    bool correction=false;float targetX=body.x,targetZ=body.z;
    if(!g.context.host && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
        (g.correction.flags&PoseValid)!=0U && (g.correction.flags&(NativeBusy|Backstage))==0U && input.focused) {
        if(g.context.localOwner && g.predictionCorrection) {
            float dx{},dz{};
            correction=spatial::BoundedStep(g.correctionX,g.correctionZ,0.5F,dx,dz);
            targetX+=dx;targetZ+=dz;
            // Consume only displacement observed since the previous sample,
            // not a requested correction that the locomotor may ignore.
            if(g.predictionCorrection && g.hadPose) {
                const float advancedX=body.x-g.lastObserved.x,advancedZ=body.z-g.lastObserved.z;
                const float d=std::hypot(g.correctionX,g.correctionZ);
                if(d>0.05F) {
                    const float progress=std::clamp((advancedX*g.correctionX+advancedZ*g.correctionZ)/d,0.0F,d);
                    g.correctionX-=g.correctionX/d*progress;g.correctionZ-=g.correctionZ/d*progress;
                }
            }
        } else if(!g.context.localOwner && std::abs(g.correction.y-body.y)<=1.0F) {
            const auto age=std::min(0.1F,static_cast<float>(now-g.correctionAt)/1000.0F);
            float dx{},dz{};
            correction=spatial::BoundedStep(g.correction.x+g.correction.velocityX*age-body.x,
                g.correction.z+g.correction.velocityZ*age-body.z,0.5F,dx,dz);
            targetX+=dx;targetZ+=dz;
        }
    }
    if(correction) {
        // Raw local intent remains the decision source on the owner. On an
        // observer the Host's measured physical velocity drives presentation.
        const float vx=g.context.localOwner?(moving?plan.worldX*plan.intensity:0.0F):g.correction.velocityX;
        const float vz=g.context.localOwner?(moving?plan.worldZ*plan.intensity:0.0F):g.correction.velocityZ;
        applied=character_direct_step_runtime::ArmAIHybridReplication(g.context.character,vx,vz,
            moving?plan.intensity:0.0F,targetX,body.y,targetZ,input.yaw,input.gait,error,now+kInputTimeoutMs);
        if(applied)gCorrections.fetch_add(1U,std::memory_order_relaxed);
    } else {
        applied=moving?character_direct_step_runtime::ArmAIIntentReplication(g.context.character,
            plan.worldX,plan.worldZ,plan.intensity,plan.gait,error):
            character_direct_step_runtime::HoldAIIntentIdle(g.context.character,input.gait,error);
        if(applied)
            static_cast<void>(character_direct_step_runtime::ApplyReplicatedYaw(g.context.character,
                ApproachYaw(body.yaw,input.yaw,dt),error));
    }
    if(applied) {
        if(!g.context.host){g.lastObserved=body;g.hadPose=true;}
        g.ownsSlot=true;g.moving=moving;
        g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();
    } else {StopMovement();}
}
void PublishInput(std::uint64_t now) noexcept {
    if(!g.context.localOwner || !g.context.pilotReady || g.deliveryFault || !g.window.Granted() || !g.window.Epoch())return;
    // Do not let a newer steering sequence overtake an unacknowledged click.
    if(g.outbox.Has(Kind::Input))return;
    if(g.lastInputSent && now-g.lastInputSent<16U && !g.actionCount)return;
    if(g.inputSequence==UINT64_MAX)return;
    Frame frame{};frame.kind=Kind::Input;frame.epoch=g.window.Epoch();frame.sequence=++g.inputSequence;
    frame.flags=Flags(now);frame.gait=g.local.gait;frame.forward=g.local.forward;frame.strafe=g.local.strafe;
    frame.yaw=g.local.yaw;frame.pitch=g.local.pitch;
    if(!Fresh(now,g.capturedAt,150U) || !g.local.focused) {
        frame.forward=frame.strafe=0.0F;frame.flags&=~Focused;
        g.actionRead=g.actionCount=0U;
    }
    LocalInput queued{};
    if(g.actionCount) {
        queued=g.actions[g.actionRead];frame.action=queued.action;
        // Round 10: an action carries its own pitch (the flame-retreat marker
        // on EnterVent); the steering pitch is otherwise always 0.
        frame.pitch=queued.pitch;
        g.actionRead=(g.actionRead+1U)%g.actions.size();--g.actionCount;
    }
    Frame body{};
    if(ReadPose(body)) {
        frame.x=body.x;frame.y=body.y;frame.z=body.z;frame.flags|=PoseValid;
        g.history.Record(frame.sequence,body.x,body.y,body.z);
    }
    if(queued.selectedVent && VentAction(frame)) {
        frame.flags|=VentSelected;frame.x=queued.ventX;frame.y=queued.ventY;frame.z=queued.ventZ;
    }
    Publish(frame,now);gInputs.fetch_add(1U,std::memory_order_relaxed);g.lastInputSent=now;
    if(frame.action==Action::EnterVent || frame.action==Action::ExitVent) {
        SetVentAuthority(frame,now);g.ventAuthority.phase=ActionPhase::Preparing;
    }
}
} // namespace

void CaptureLocal(const LocalInput& input) noexcept {
    g.local=input;g.capturedAt=GetTickCount64();
    if(input.action!=Action::None && g.window.Granted()) {
        if(g.actionCount<g.actions.size()) {
            g.actions[(g.actionRead+g.actionCount)%g.actions.size()]=input;++g.actionCount;
        } else gDropped.fetch_add(1U,std::memory_order_relaxed);
    }
    g.local.action=Action::None;
}
Frame VentAuthority() noexcept {return g.ventAuthority;}
bool ControlActive() noexcept {
    // A Client pilot aligns on the Host's first Correction. A Host pilot IS
    // the authority and never receives one, so it was never "aligned" (468:
    // permanent "Waiting for Alien control authorization", no free camera).
    return g.context.owned && g.context.localOwner && (g.aligned || g.context.host) && !g.deliveryFault &&
        g.window.ActiveAt(GetTickCount64());
}
bool VentPending() noexcept {
    return ActionBusy() || g.hasDeferredAction || g.ventAuthority.phase==ActionPhase::Preparing ||
        g.ventAuthority.phase==ActionPhase::Started;
}
// AvS round 6: VentPending() is "any action in flight" for movement/input
// gating. The HUD showed "Entering ventilation... E / Q" for a busy KILL on
// the Client pilot (its local attack record closed while the Host retried).
bool VentRequestPending() noexcept {
    return (g.hasDeferredAction && VentAction(g.deferredAction)) ||
        (VentAction(g.ventAuthority) && (g.ventAuthority.phase==ActionPhase::Preparing ||
            g.ventAuthority.phase==ActionPhase::Started));
}
bool NonVentActionBusy() noexcept {return ActionBusy() && !VentRequestPending();}
bool PlayReaction(std::uint32_t type,std::uint64_t windowMs) noexcept {
    return g.context.localOwner && g.context.actions && g.context.actions->react && !ActionBusy() &&
        g.context.actions->react(g.context,type,windowMs);
}
bool TakeStunRoar(float& yaw) noexcept {
    const auto at=gStunRoarAt.load(std::memory_order_acquire);
    if(!at || gStunRoarTaken.exchange(at,std::memory_order_acq_rel)==at)return false;
    yaw=gStunRoarYaw.load(std::memory_order_relaxed);return true;
}
void Reset(bool certifiedLivingCharacter) noexcept {
    if(g.context.actions && g.context.actions->cancel)g.context.actions->cancel(g.context,certifiedLivingCharacter);
    if(certifiedLivingCharacter && g.context.character && g.ownsSlot)
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(g.context.character));
    g={};
    // The engine thread never blocks behind the IPC worker. Stamped old-scope
    // frames which cannot be removed here will fail the next identity check.
    std::unique_lock lock(gQueueMutex,std::try_to_lock);
    if(lock.owns_lock()){gIncoming.Clear();gOutgoing.Clear();}
}
bool Owns(std::uintptr_t character,std::uint32_t generation) noexcept {
    return character && g.context.owned && g.context.character==character &&
        g.context.scope.nativeGeneration==generation;
}
bool MovingNow() noexcept { return g.moving; }
void Pump(const Context& context) noexcept {
    if(!context.owned || !context.base || !context.character || !Valid(context.scope)) {
        if(g.context.character)Reset(false);
        return;
    }
    if(g.context.character!=context.character || g.context.scope!=context.scope) {
        Reset(false);
        if(gDeliveryGeneration==UINT32_MAX)return;
        g.context=context;g.window.Reset(context.scope);
        g.deliveryGeneration=++gDeliveryGeneration;
        g.inbox.Clear(g.deliveryGeneration<<32U);g.outbox.Clear(g.deliveryGeneration<<32U);
    }
    g.context=context;
    if(g.context.localOwner && g.context.actions && g.context.actions->react)
        static_cast<void>(g.context.actions->react(g.context,0U,0U));
    const auto now=GetTickCount64();
    const auto dt=g.lastTick && now>=g.lastTick?
        std::clamp(static_cast<float>(now-g.lastTick)/1000.0F,0.001F,0.05F):0.016F;
    g.lastTick=now;
    Drain(now);PollAction(now);ExpireVentAuthority(now);PublishInput(now);FlushReliable(now);ApplyMovement(now,dt);
    if(!g.lastStatus || now-g.lastStatus>=250U) {
        Frame status{};status.kind=Kind::Status;status.sequence=g.deliveryGeneration;status.epoch=g.window.Epoch();status.flags=Flags(now);
        std::uint8_t state{};bool backstage{};
        const bool known=NativeStage(context.character,state,backstage);
        if(known && backstage)status.flags|=Backstage;
        // Round 5: a chasing attack is ordinary movement for the Host replica.
        if(MovementBusy() || !known || (!OrdinaryCycle(context.character) && !spatial::ManualBackstage(state,backstage,false)))status.flags|=NativeBusy;
        if(ReadPose(status))status.flags|=PoseValid;
        Publish(status,now);g.lastStatus=now;
    }
    if((context.host || context.localOwner) && g.window.Granted() && g.window.Epoch() &&
        (!g.lastPose || now-g.lastPose>=33U) && g.poseSequence!=UINT64_MAX) {
        Frame pose{};pose.kind=Kind::Pose;pose.epoch=g.window.Epoch();pose.sequence=++g.poseSequence;
        pose.flags=Flags(now);pose.hostSequence=g.appliedHost;pose.acknowledgedInput=g.appliedInput;
        pose.gait=g.window.Input().gait;
        std::uint8_t state{};bool backstage{};
        const bool known=NativeStage(context.character,state,backstage);
        if(known && backstage)pose.flags|=Backstage;
        // Pass 19: crawl mode (duct / C window) or the hatch traversal itself.
        if(context.localOwner && known && !backstage && (g.local.crawl || state==18U) && !ActionBusy()) {
            pose.flags|=Crawling;gCrawlPosesSent.fetch_add(1U,std::memory_order_relaxed);
        }
        if(MovementBusy() || !known || (!OrdinaryCycle(context.character) && !spatial::ManualBackstage(state,backstage,false)))pose.flags|=NativeBusy;
        if(context.localOwner && known && backstage && state==18U && ActionBusy() &&
            VentAction(g.ventAuthority) && g.ventAuthority.phase==ActionPhase::Started)
            pose.flags|=VentAnimating|NativeBusy;
        if(ReadPose(pose)) {
            pose.flags|=PoseValid;
            if(g.hadPose && g.lastPose && now>g.lastPose && now-g.lastPose<=200U) {
                const auto elapsed=static_cast<float>(now-g.lastPose)/1000.0F;
                pose.velocityX=(pose.x-g.lastObserved.x)/elapsed;pose.velocityZ=(pose.z-g.lastObserved.z)/elapsed;
                if(std::hypot(pose.velocityX,pose.velocityZ)>30.0F)pose.velocityX=pose.velocityZ=0.0F;
            }
            g.hadPose=true;g.lastObserved=pose;Publish(pose,now);gPoses.fetch_add(1U,std::memory_order_relaxed);
        } else g.hadPose=false;
        g.lastPose=now;
    }
}
bool QueueCommand(std::span<const std::byte> bytes) noexcept {
    Frame frame{};
    if(!Decode({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},frame) ||
        (frame.kind!=Kind::Activate && frame.kind!=Kind::Commit && frame.kind!=Kind::Correction && frame.kind!=Kind::Stop && frame.kind!=Kind::Resolution && frame.kind!=Kind::Acknowledge)) {
        gRejected.fetch_add(1U,std::memory_order_relaxed);return false;
    }
    const auto now=GetTickCount64();std::lock_guard lock(gQueueMutex);
    if(!gIncoming.Push({frame,now})){gDropped.fetch_add(1U,std::memory_order_relaxed);return false;}
    gReceived.fetch_add(1U,std::memory_order_relaxed);return true;
}
bool TryPopObservation(std::array<std::uint8_t,kWireBytes>& bytes) noexcept {
    std::lock_guard lock(gQueueMutex);TimedFrame value{};
    while(gOutgoing.Pop(value))if(Encode(value.frame,bytes))return true;
    return false;
}
bool LastCorrection(Frame& frame,std::uint64_t& ageMs) noexcept {
    if(!g.context.owned || g.context.localOwner || !g.correctionAt)return false;
    frame=g.correction;ageMs=GetTickCount64()-g.correctionAt;return true;
}
PuppetStats PuppetSnapshot() noexcept {
    return {gCrawlPosesSent.load(),gCrawlMirror.load(),gCrawlHatch.load(),gOwnerBlend.load(),gCrawlFallback.load(),
        gCrawlTail.load(),gCrawlTailDone.load(),gBlendThroughBusy.load(),gBlendBlockedBusy.load(),gBlendInactive.load()};
}
MovementStats MovementSnapshot() noexcept {
    return {gHeld.load(),gDriven.load(),gBlockedAction.load(),gBlockedVent.load(),gBlockedStale.load(),
        gBlockedStage.load(),gDriveRefused.load(),gChaseDriven.load(),gVentExpired.load(),gVentEpochCleared.load()};
}
Counters Snapshot() noexcept {
    return {gReceived.load(),gRejected.load(),gExpired.load(),gDropped.load(),gInputs.load(),gCommits.load(),
        gPoses.load(),gCorrections.load(),gUnsupported.load()};
}
} // namespace aimp::alien_pvp
