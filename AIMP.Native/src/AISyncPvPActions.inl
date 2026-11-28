// Included once inside AISync.cpp's private namespace, after the lease lifetime
// helpers. Exact-build x86 adapters; never called by a transport worker.
// Native contracts and static evidence: docs/AVS-NATIVE-ACTIONS-v416.md.
namespace pvp_actions {
#ifdef AIMP_ALIEN_ACTION_TEST
#define AIMP_ACTION_TRY try
#define AIMP_ACTION_CATCH catch(...)
#else
#define AIMP_ACTION_TRY __try
#define AIMP_ACTION_CATCH __except(EXCEPTION_EXECUTE_HANDLER)
#endif
template<class Fn> Fn Native(const std::uintptr_t rva) noexcept {
#ifdef AIMP_ALIEN_ACTION_TEST
    return reinterpret_cast<Fn>(ActionTestFunction(rva));
#else
    return reinterpret_cast<Fn>(gBase+rva);
#endif
}
namespace ap=aimp::alien_pvp;
namespace policy=aimp::alien_pvp::actions;
using Phase=ap::ActionPhase;
using Reason=ap::ActionReason;
using Action=ap::Action;
struct VentSlot final {
    std::array<std::uint32_t,4> node{};
    ap::vent_node::Methods entryMethods{};
    std::uintptr_t frame{},manager{};
    ap::Scope scope{};std::uintptr_t character{};
    bool used{},started{},quarantined{},cancelling{};
    std::uint32_t selectedIndex{};
};
// Node storage must outlive the native frame. On unprovable destruction it is
// quarantined, never overwritten while an old frame could still refer to it.
std::array<VentSlot,8> ventSlots{};
struct Active final {
    ap::Context context{};ap::Frame receipt{};
    std::uint64_t began{},lastStep{},submittedAt{};
    PairIdentity victim{};
    std::uintptr_t chooser{};
    ap::attack::HeldTarget heldTarget{};
    VentSlot* vent{};
    bool used{},entered{},cancelled{},terminalSent{},hostEnded{},moveArrived{},entrySubmitted{},remoteVent{};
    bool presentationStarted{},ownerFinished{};
    std::uint64_t arrivedAt{};std::uint32_t cancelRefusals{};
    std::uint64_t ventProgressAt{};float ventBestDistance{std::numeric_limits<float>::infinity()};
    bool attackLatched{},retailArmed{},presented{};std::uint64_t latchedAt{},lastPairTrace{},armedAt{},presentedAt{};
    bool directSubmitted{};
    // AvS 2026-11-25 (round 3): settle before the direct pair, one transition
    // retry, and the observer's fallback presentation.
    bool settling{},transitionRetried{},observerFallback{};std::uint64_t settleAt{},observerWaitAt{};
    // Round 4: pilot PC's first PresentPair attempt (retried for 1.5 s while
    // an AISync replay still reserves the characters).
    std::uint64_t presentFirstAt{};std::uint32_t presentTries{};
    // Round 5: auto-chase until the retail query proves the kill, Started only
    // once the victim is really in its paired cycle (79), silent re-submits.
    bool sourceEntered{},stallTraced{};std::uint64_t sourceEnteredAt{},lastQueryAt{};std::uint32_t attackRetries{};
    std::uint32_t retargets{},flagConsumed{};
    std::uintptr_t attackAnimated{};bool shouldAttackSet{};
    ap::attack::HeldTarget brainTarget{};
    // Pass 20: first frame the chased victim was in contact while retail still
    // refused the kill (forced latch after kForcedLatchMs).
    std::uint64_t contactSince{};
};
Active active{};
policy::EdgeGate edges{};
std::uint64_t edgeEpoch{};

bool Live(const ap::Context& c) noexcept {
    return EngineThread() && PvPLeaseLiveIdentity() &&
        c.base==gBase && c.character==gPvPLease.ticket.character &&
        c.scope.nativeGeneration==gPvPLease.nativeGeneration && c.scope.worldGeneration==gPvPLease.worldGeneration &&
        c.scope.incarnation==gPvPLease.ticket.serial && c.scope.ownerPeer==gPvPLease.ownerPeer &&
        c.scope.cookie==gPvPLease.sessionCookie && c.scope.entityA==gPvPLease.entityA && c.scope.entityB==gPvPLease.entityB;
}
#include "AISyncPvPDonorMovement.inl"
bool Body(const std::uintptr_t character,policy::Point& point) noexcept {
    return Read(character+0x70U,point.x) && Read(character+0x74U,point.y) &&
        Read(character+0x78U,point.z) && policy::Finite(point);
}
bool Cycle(const std::uintptr_t character,std::uint8_t& state,bool& backstage) noexcept {
    std::uintptr_t cycle{},owner{};std::uint8_t hidden{},withdrawn{};
    if(!Read(character+0x36CU,cycle) || !cycle || !Read(cycle+4U,owner) || owner!=character ||
        !Read(cycle+0x1AU,state) || !Read(character+0x2DAU,hidden) || !Read(character+0x2DBU,withdrawn))return false;
    backstage=hidden!=0U || withdrawn!=0U;return true;
}
// Round 5: paired definition (kill variant) of the last submitted/presented
// selector: its kind and the victim bone name it attaches to. Diagnostics only.
std::array<std::atomic<char>,48> gDefinitionBone{};std::atomic<unsigned> gDefinitionKind{};
void RecordDefinition(const interactions::Definition& definition) noexcept {
    gDefinitionKind.store(definition.kind,std::memory_order_relaxed);
    bool ended{};
    for(std::size_t i=0;i+1<gDefinitionBone.size();++i) {
        char ch{};
        if(ended || !Read(definition.object+0x20U+i,ch) || ch=='\0' || ch<0x20 || ch>0x7E){ended=true;ch='\0';}
        gDefinitionBone[i].store(ch,std::memory_order_relaxed);
    }
}
std::string DefinitionBone() {
    std::string text;
    for(const auto& ch:gDefinitionBone){const char c=ch.load(std::memory_order_relaxed);if(!c)break;text.push_back(c);}
    return text;
}
#ifndef AIMP_ALIEN_ACTION_TEST
struct VentTelemetry final {
    std::atomic<std::uint64_t> revision{},sequence{},elapsed{};
    std::atomic<unsigned> action{},phase{},cycle{},frameStatus{};
    std::atomic<bool> host{},backstage{},cancelling{};
    std::atomic<float> x{},y{},z{};
    std::atomic<unsigned> arrivedNoPassage{},refusedCancels{}; // session counters
} ventTelemetry;
void TraceVent(unsigned frameStatus=0U) noexcept {
    if(!active.used || (active.receipt.action!=Action::EnterVent && active.receipt.action!=Action::ExitVent))return;
    if(frameStatus==3U)ventTelemetry.arrivedNoPassage.fetch_add(1U,std::memory_order_relaxed);
    if(frameStatus==4U)ventTelemetry.refusedCancels.fetch_add(1U,std::memory_order_relaxed);
    policy::Point body{};std::uint8_t state{};bool backstage{};
    if(!Body(active.context.character,body)||!Cycle(active.context.character,state,backstage))return;
    ventTelemetry.sequence=active.receipt.hostSequence;ventTelemetry.elapsed=GetTickCount64()-active.began;
    ventTelemetry.action=static_cast<unsigned>(active.receipt.action);ventTelemetry.phase=static_cast<unsigned>(active.receipt.phase);
    ventTelemetry.cycle=state;ventTelemetry.frameStatus=frameStatus;ventTelemetry.host=active.context.host;
    ventTelemetry.backstage=backstage;ventTelemetry.cancelling=active.cancelled;
    ventTelemetry.x=body.x;ventTelemetry.y=body.y;ventTelemetry.z=body.z;
    ventTelemetry.revision.fetch_add(1U,std::memory_order_release);
}
void LogVent(Diagnostics& diagnostics) {
    static std::uint64_t lastRevision{},lastAt{};
    const auto revision=ventTelemetry.revision.load(std::memory_order_acquire),now=GetTickCount64();
    if(!revision || revision==lastRevision || now-lastAt<1000U)return;
    lastRevision=revision;lastAt=now;
    diagnostics.Log("info","alien_vent_progress","Engine-thread vent observations; frameResult 0 pending, 1 arrived, 2 refused, 3 arrivedButNoPassage2.5s, 4 cancelRefusedDuringPassage, 5 pathLedAwayFromOpening (cancelled).",
        {{"host",Diagnostics::Boolean(ventTelemetry.host.load()),true},
         {"hostSequence",Diagnostics::Unsigned(ventTelemetry.sequence.load()),true},
         {"elapsedMs",Diagnostics::Unsigned(ventTelemetry.elapsed.load()),true},
         {"action",Diagnostics::Unsigned(ventTelemetry.action.load()),true},
         {"phase",Diagnostics::Unsigned(ventTelemetry.phase.load()),true},
         {"cycle",Diagnostics::Unsigned(ventTelemetry.cycle.load()),true},
         {"frameResult",Diagnostics::Unsigned(ventTelemetry.frameStatus.load()),true},
         {"backstage",Diagnostics::Boolean(ventTelemetry.backstage.load()),true},
         {"cancelling",Diagnostics::Boolean(ventTelemetry.cancelling.load()),true},
         {"arrivedNoPassageTotal",Diagnostics::Unsigned(ventTelemetry.arrivedNoPassage.load()),true},
         {"refusedCancelsTotal",Diagnostics::Unsigned(ventTelemetry.refusedCancels.load()),true},
         {"x",std::to_string(ventTelemetry.x.load()),true},
         {"y",std::to_string(ventTelemetry.y.load()),true},
         {"z",std::to_string(ventTelemetry.z.load()),true}});
}
// Host attack stages: 1 no AI selection, 2 no selected record, 3 target
// write fault, 4 query refused, 5 selector unproven, 6 cover ray pending,
// 7 pair submitted, 8 pair refused, 9 source entered cycle78, 10 entry
// attempted after the 400ms ray window.
struct AttackTelemetry final {
    std::atomic<std::uint64_t> revision{},sequence{},elapsed{},polls{};
    std::atomic<unsigned> stage{},selector{},cycle{};
    std::atomic<unsigned> pairStage{},pairPrepared{},victimState{},victimAcquired{};
    // Armed-window counters of the current attack (not overwritten by stage).
    std::atomic<unsigned> armedPolls{},flagConsumed{},retargets{},brainMissing{};
    // Retail ActionMeleeAttack starts/refusals for the armed Alien (probe
    // 00717D90), last node type, OR of attack-manager availability bytes
    // +8..+E (bit n = +8+n; +8 ANY, +D any proven) and source/victim +2B4.
    std::atomic<unsigned> meleeStarts{},meleeRefused{},meleeType{},availability{},sourceFlag2B4{},victimFlag2B4{},brainFaults{};
    // Direct pair (round 3): ms spent settling the Alien before submit, its
    // cycle at submit, and whether the transition was re-issued once.
    std::atomic<unsigned> settleMs{},settleCycle{},transitionRetried{};
    // Round 4: HostDirectPair guard mask (stage 36), whether a Client owns the
    // Alien, owner poses held back during settle/submit, and the pilot PC's
    // PresentPair attempts before it took (or gave up after 1.5 s).
    std::atomic<unsigned> directGuard{},remoteOwner{},posesHeld{},presentTries{};
    // Round 5: silent re-submits of the current attack, frames the pilot's PC
    // steered the Alien at its victim (session total).
    std::atomic<unsigned> retries{},chaseFrames{};
    // Diagnostics flush every ~5 s and kept only the last stage: a ring of
    // the most recent stage changes (stage<<24 | elapsedMs) shows the path.
    std::array<std::atomic<std::uint32_t>,16> history{};std::atomic<std::uint32_t> historyWrite{};
    // Session counters (never reset per attack).
    std::atomic<unsigned> ventHijacks{};
    // Pass 20: attacks refused because the victim hides in a locker/cabinet
    // (stage 42; the retail grab broke there and left the victim stuck in 79),
    // forced latches (stage 43: in contact 1.5 s while retail kept refusing, e.g.
    // a survivor crouched under a closet).
    std::atomic<unsigned> lockerRefused{},forcedLatches{};
    // Last NoTarget rejection: nearest certified survivor relative to the
    // Alien and the pilot's aim (debug point for "clicking does nothing").
    std::atomic<std::uint64_t> missRevision{},missSequence{};
    std::atomic<unsigned> missCandidates{},missLiving{},missPublished{};
    std::atomic<float> missDistance{-1.0F},missHeight{},missCosine{},missYaw{};
} attackTelemetry;
void TraceMiss(std::uint64_t sequence,unsigned candidates,unsigned living,unsigned published,
    float distance,float height,float cosine,float yaw) noexcept {
    attackTelemetry.missSequence=sequence;attackTelemetry.missCandidates=candidates;
    attackTelemetry.missLiving=living;attackTelemetry.missPublished=published;
    attackTelemetry.missDistance=distance;attackTelemetry.missHeight=height;
    attackTelemetry.missCosine=cosine;attackTelemetry.missYaw=yaw;
    attackTelemetry.missRevision.fetch_add(1U,std::memory_order_release);
}
void LogMiss(Diagnostics& diagnostics) {
    static std::uint64_t lastRevision{};
    const auto revision=attackTelemetry.missRevision.load(std::memory_order_acquire);
    if(!revision || revision==lastRevision)return;
    lastRevision=revision;
    diagnostics.Log("info","alien_attack_no_target",
        "Host rejected an Alien attack with NoTarget. Nearest participant: distanceM (flat), heightM, cosine of the angle to the pilot's aim (needs >= admission cosine), candidates/living/published counts.",
        {{"hostSequence",Diagnostics::Unsigned(attackTelemetry.missSequence.load()),true},
         {"participants",Diagnostics::Unsigned(attackTelemetry.missCandidates.load()),true},
         {"living",Diagnostics::Unsigned(attackTelemetry.missLiving.load()),true},
         {"pairPublished",Diagnostics::Unsigned(attackTelemetry.missPublished.load()),true},
         {"distanceM",std::to_string(attackTelemetry.missDistance.load()),true},
         {"heightM",std::to_string(attackTelemetry.missHeight.load()),true},
         {"aimCosine",std::to_string(attackTelemetry.missCosine.load()),true},
         {"aimYaw",std::to_string(attackTelemetry.missYaw.load()),true},
         {"reachM",std::to_string(policy::kAttackReach),true},
         {"admissionCosine",std::to_string(policy::kAttackCosine),true},
         {"ventHijacks",Diagnostics::Unsigned(attackTelemetry.ventHijacks.load()),true}});
}
void TraceAttack(unsigned stage,unsigned selector=0U) noexcept {
    std::uint8_t state{};bool backstage{};
    if(!Cycle(active.context.character,state,backstage))state=0xFFU;
    const bool fresh=attackTelemetry.stage.load()!=stage || attackTelemetry.sequence.load()!=active.receipt.hostSequence ||
        attackTelemetry.polls.load()==0U;
    attackTelemetry.sequence=active.receipt.hostSequence;attackTelemetry.elapsed=GetTickCount64()-active.began;
    if(fresh) {
        const auto slot=attackTelemetry.historyWrite.fetch_add(1U,std::memory_order_relaxed)%attackTelemetry.history.size();
        const auto elapsed=static_cast<std::uint32_t>(std::min<std::uint64_t>(attackTelemetry.elapsed.load(),0xFFFFFFU));
        attackTelemetry.history[slot].store((stage<<24U)|elapsed,std::memory_order_relaxed);
    }
    attackTelemetry.stage=stage;attackTelemetry.selector=selector;attackTelemetry.cycle=state;
    attackTelemetry.polls.fetch_add(1U,std::memory_order_relaxed);
    attackTelemetry.revision.fetch_add(1U,std::memory_order_release);
}
// After entry: source cycle +E0 stage, +E7 prepared, victim (+44) cycle state
// and +D9 acquired, the same fields AISyncInteraction proves for ownership.
void TracePair(unsigned stage) noexcept {
    std::uintptr_t cycle{},victim{},victimCycle{};std::uint32_t pairStage{0xFFFFFFFFU};
    std::uint8_t prepared{0xFFU},victimState{0xFFU},acquired{0xFFU};
    if(Read(active.context.character+0x36CU,cycle) && cycle) {
        static_cast<void>(Read(cycle+0xE0U,pairStage));static_cast<void>(Read(cycle+0xE7U,prepared));
        if(Read(cycle+0x44U,victim) && victim && Read(victim+0x36CU,victimCycle) && victimCycle) {
            static_cast<void>(Read(victimCycle+0x1AU,victimState));static_cast<void>(Read(victimCycle+0xD9U,acquired));
        }
    }
    attackTelemetry.pairStage=pairStage;attackTelemetry.pairPrepared=prepared;
    attackTelemetry.victimState=victimState;attackTelemetry.victimAcquired=acquired;
    TraceAttack(stage==12U && acquired==1U?11U:stage,active.receipt.selector);
}
void LogAttack(Diagnostics& diagnostics) {
    static std::uint64_t lastRevision{},lastSequence{},lastAt{};
    const auto revision=attackTelemetry.revision.load(std::memory_order_acquire),now=GetTickCount64();
    const auto sequence=attackTelemetry.sequence.load();
    if(!revision || revision==lastRevision || (sequence==lastSequence && now-lastAt<250U))return;
    lastRevision=revision;lastSequence=sequence;lastAt=now;
    // [[stage,elapsedMs],...] recorded since the previous line (oldest first).
    static std::uint32_t historyRead{};
    std::string recent="[";
    {
        const auto write=attackTelemetry.historyWrite.load(std::memory_order_relaxed);
        const auto size=static_cast<std::uint32_t>(attackTelemetry.history.size());
        if(write-historyRead>size)historyRead=write-size;
        for(bool comma{};historyRead!=write;++historyRead) {
            const auto v=attackTelemetry.history[historyRead%size].load(std::memory_order_relaxed);
            if(comma)recent+=",";
            comma=true;
            recent+="["+std::to_string(v>>24U)+","+std::to_string(v&0xFFFFFFU)+"]";
        }
    }
    recent+="]";
    diagnostics.Log("info","alien_attack_progress","Host attack preparation; stage 1 noSelection 2 noRecord 3 targetFault 4 queryRefused 5 selectorUnproven 6 rayPending 7 submitted 8 pairRefused 9 entered 10 enteredWithRayPending 11 pairRunning 12 victimNotAcquired4s 13 retailArmed 14 retailWindowExpired 15 retailOtherVictim 16 retailRetargeted(selector=count) 17 armedQueryRefused 18 brainTargetUnavailable 19 attackBranchConsumedFlag(selector=count) 24 retailNeverStartedMelee3s 25 retailChoseVentAborted(selector=cycle) 26 directPairNoDefinition 27 directPairInteractionRefused 28 directPairNativeFault 29 directPairDonorBusy 30 directPairNotEntered2s (7 = direct pair submitted; settleMs/settleCycle = wait for the Alien to stop before submit and its cycle then, 1 = idle; transitionRetried = transition re-issued at 600 ms) 20 clientPresented 21 clientPresentRefused 22 clientNotEntered 23 clientEntered; survivor (observer) PC: 31 waitingForPairReplay 32 fallbackPresented 33 fallbackRefused 34 killEnteredHere 35 hostEndedWithoutKillHere; 36 directPairGuard (directGuard bits: 1 lease not live, 2 scope stale, 4 not host, 8 releasing, 16 victim not live, 32 Alien pair-reserved, 64 victim pair-reserved); remoteOwner 1 = a Client pilots the Alien; posesHeld = owner poses held back on the Host during settle/submit (session total); recentStages = [stage, elapsedMs] since the previous line; presentTries = pilot-PC PresentPair attempts (stage 20 took, 21 gave up after 1.5 s). Round 5: 9 = source entered 78 (not yet a kill), 39 victim grabbed (its cycle 79) -> Started, 38 victim not grabbed within 1.5 s / pair ended without it -> re-submit, 30 not entered in 2 s -> re-submit, 37 grabbed but still paired after 9 s (stalled kill), 40 victim left the 6 m leash, 41 8 s chase window over; retries = re-submits of this attack; chaseFrames = pilot-PC frames steering at the victim (session); victimGate = OR of paired-victim availability checks on this PC since the last line (1 retail allowed, 2 not a victim view, 4 remote replica not ready, 8 local player not ready, 16 pair not present/matching, 32 AIMP allowed it, 64 local player; local-player detail 128 not the local client victim, 256 no published binding, 512 animation graph off, 1024 no player controller, 2048 cycle not 79, 4096 admission/source mismatch); bone/kind = paired definition of the selector.",
        {{"hostSequence",Diagnostics::Unsigned(sequence),true},
         {"elapsedMs",Diagnostics::Unsigned(attackTelemetry.elapsed.load()),true},
         {"stage",Diagnostics::Unsigned(attackTelemetry.stage.load()),true},
         {"selector",Diagnostics::Unsigned(attackTelemetry.selector.load()),true},
         {"cycle",Diagnostics::Unsigned(attackTelemetry.cycle.load()),true},
         {"polls",Diagnostics::Unsigned(attackTelemetry.polls.load()),true},
         {"pairStage",Diagnostics::Unsigned(attackTelemetry.pairStage.load()),true},
         {"pairPrepared",Diagnostics::Unsigned(attackTelemetry.pairPrepared.load()),true},
         {"victimState",Diagnostics::Unsigned(attackTelemetry.victimState.load()),true},
         {"victimAcquired",Diagnostics::Unsigned(attackTelemetry.victimAcquired.load()),true},
         {"armedPolls",Diagnostics::Unsigned(attackTelemetry.armedPolls.load()),true},
         {"flagConsumed",Diagnostics::Unsigned(attackTelemetry.flagConsumed.load()),true},
         {"retargets",Diagnostics::Unsigned(attackTelemetry.retargets.load()),true},
         {"brainMissing",Diagnostics::Unsigned(attackTelemetry.brainMissing.load()),true},
         {"meleeStarts",Diagnostics::Unsigned(attackTelemetry.meleeStarts.load()),true},
         {"meleeRefused",Diagnostics::Unsigned(attackTelemetry.meleeRefused.load()),true},
         {"meleeType",Diagnostics::Unsigned(attackTelemetry.meleeType.load()),true},
         {"availability",Diagnostics::Unsigned(attackTelemetry.availability.load()),true},
         {"source2B4",Diagnostics::Unsigned(attackTelemetry.sourceFlag2B4.load()),true},
         {"victim2B4",Diagnostics::Unsigned(attackTelemetry.victimFlag2B4.load()),true},
         {"brainFaults",Diagnostics::Unsigned(attackTelemetry.brainFaults.load()),true},
         {"settleMs",Diagnostics::Unsigned(attackTelemetry.settleMs.load()),true},
         {"settleCycle",Diagnostics::Unsigned(attackTelemetry.settleCycle.load()),true},
         {"transitionRetried",Diagnostics::Unsigned(attackTelemetry.transitionRetried.load()),true},
         {"directGuard",Diagnostics::Unsigned(attackTelemetry.directGuard.load()),true},
         {"remoteOwner",Diagnostics::Unsigned(attackTelemetry.remoteOwner.load()),true},
         {"posesHeld",Diagnostics::Unsigned(attackTelemetry.posesHeld.load()),true},
         {"presentTries",Diagnostics::Unsigned(attackTelemetry.presentTries.load()),true},
         {"retries",Diagnostics::Unsigned(attackTelemetry.retries.load()),true},
         {"chaseFrames",Diagnostics::Unsigned(attackTelemetry.chaseFrames.load()),true},
         {"lockerRefused",Diagnostics::Unsigned(attackTelemetry.lockerRefused.load()),true},
         {"forcedLatches",Diagnostics::Unsigned(attackTelemetry.forcedLatches.load()),true},
         {"victimGate",Diagnostics::Unsigned(gPairedVictimGateSeen.exchange(0U,std::memory_order_relaxed)),true},
         {"bone",DefinitionBone(),false},
         {"kind",Diagnostics::Unsigned(gDefinitionKind.load(std::memory_order_relaxed)),true},
         {"recentStages",recent,true}});
}
#else
void TraceVent(unsigned=0U) noexcept {}
void TraceAttack(unsigned,unsigned=0U) noexcept {}
void TracePair(unsigned) noexcept {}
void TraceMiss(std::uint64_t,unsigned,unsigned,unsigned,float,float,float,float) noexcept {}
#endif
bool Ordinary(const std::uint8_t state) noexcept {return state==1U || state==55U || state==56U || state==59U;}
bool Protected(const std::uint8_t state) noexcept {return state==18U || state==75U || state==77U || state==78U || state==79U;}
bool Same(const ap::Context& c) noexcept {return active.used && c.character==active.context.character && c.scope==active.context.scope;}
bool Quarantined(const ap::Context& c) noexcept {
    for(const auto& slot:ventSlots)if(slot.quarantined && slot.character==c.character && slot.scope==c.scope)return true;
    return false;
}
// A remote-owner vent receipt owns journal ordering, but no local native
// movement resource. Its replica must remain available to measured poses.
bool Busy(const ap::Context& c) noexcept {return (Same(c) && !active.remoteVent) || Quarantined(c);}
void Reject(ap::Frame& r,Reason reason) noexcept {
    r.phase=Phase::Rejected;r.reason=reason;r.resolutionRevision=1U;
}
// Scalar wrappers isolate SEH from functions with C++ destructors.
bool ExitPoint(std::uintptr_t descriptor,policy::Point& out) noexcept {
    AIMP_ACTION_TRY {
        using Fn=void*(__thiscall*)(void*,void*);
        Native<Fn>(0x0021CCC0U)(reinterpret_cast<void*>(descriptor),&out);
        return policy::Finite(out);
    } AIMP_ACTION_CATCH {return false;}
}
bool PortalIndex(const ap::Context& c,const ap::Frame& plan,std::uint32_t& index) noexcept {
    std::uintptr_t withdraw{},manager{},entries{};std::uint32_t count{};bool found{};
    if(!Live(c) || !Read(c.character+0x34F0U,withdraw) || !withdraw ||
        !Read(withdraw+0x24U,manager) || !manager || !Read(manager+8U,entries) || !entries ||
        !Read(manager+12U,count) || count>4096U)return false;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t descriptor{};policy::Point entrance{},exit{};
        if(!Read(entries+i*0x30U+0x28U,descriptor) || !descriptor || !Read(descriptor,entrance) || !ExitPoint(descriptor,exit))continue;
        constexpr float tolerance=policy::kVentMatchTolerance*policy::kVentMatchTolerance;
        if(policy::DistanceSquared(entrance,{plan.x,plan.y,plan.z})>tolerance ||
            policy::DistanceSquared(exit,{plan.resolvedX,plan.resolvedY,plan.resolvedZ})>tolerance)continue;
        if(found)return false;
        index=i;found=true;
    }
    return found;
}
// Donor vent.cpp::OnVentPickLeave/OnVentPickExitLeave, scoped to this
// AIMP-owned node call. Other Aliens and ordinary Coop keep retail selection.
thread_local VentSlot* selectedPortal{};
void* ventEnterOriginal{};void* ventExitOriginal{};
std::uintptr_t PickPortal(std::uintptr_t manager,int reason,bool exit) noexcept {
    auto* slot=selectedPortal;
    if(!slot || slot->manager!=manager || !Live(active.context) || reason<0 || reason>1)return 0U;
    std::uintptr_t entries{},descriptor{};std::uint32_t count{};
    if(!Read(manager+8U,entries) || !entries || !Read(manager+12U,count) || slot->selectedIndex>=count ||
        !Read(entries+slot->selectedIndex*0x30U+0x28U,descriptor) || !descriptor)return 0U;
    const auto offset=(exit?0x48U:0x40U)+static_cast<unsigned>(reason)*4U;
    AIMP_ACTION_TRY {
        *reinterpret_cast<std::uint32_t*>(manager+offset)=slot->selectedIndex;
        return entries+slot->selectedIndex*0x30U;
    } AIMP_ACTION_CATCH {return 0U;}
}
std::uintptr_t __fastcall PickEnterDetour(void* manager,void*,int reason) noexcept {
    const auto selected=PickPortal(reinterpret_cast<std::uintptr_t>(manager),reason,false);
    if(selected)return selected;
    return reinterpret_cast<std::uintptr_t(__thiscall*)(void*,int)>(ventEnterOriginal)(manager,reason);
}
std::uintptr_t __fastcall PickExitDetour(void* manager,void*,int reason) noexcept {
    const auto selected=PickPortal(reinterpret_cast<std::uintptr_t>(manager),reason,true);
    if(selected)return selected;
    return reinterpret_cast<std::uintptr_t(__thiscall*)(void*,int)>(ventExitOriginal)(manager,reason);
}
std::uintptr_t RetailEntry(std::uintptr_t manager) noexcept {
    if(!ventEnterOriginal)return 0;
    AIMP_ACTION_TRY {
        return reinterpret_cast<std::uintptr_t(__thiscall*)(void*,int)>(ventEnterOriginal)(reinterpret_cast<void*>(manager),0);
    } AIMP_ACTION_CATCH {return 0;}
}
bool RetreatReach(const policy::Point& body,const policy::Point& vent) noexcept {
    const float dy=vent.y-body.y;
    return std::hypot(vent.x-body.x,vent.z-body.z)<=ap::fear::kRetreatReach && dy>=-3.0F && dy<=6.0F;
}
// Retreat run on this PC (pilot flag) or a Host-side retreat command receipt.
bool RetreatEntry() noexcept {
    return active.receipt.action==Action::EnterVent &&
        (ap::fear::RetreatPitch(active.receipt.pitch) || alien_pvp_local::RetreatActive());
}
bool SelectPortal(const ap::Context& c,const ap::Frame& request,ap::Frame& result) noexcept {
    std::uintptr_t withdraw{},manager{},entries{};std::uint32_t count{};
    if(!Read(c.character+0x34F0U,withdraw) || !withdraw || !Read(withdraw+0x24U,manager) || !manager ||
        !Read(manager+8U,entries) || !Read(manager+12U,count) || !entries || count==0U || count>4096U)return false;
    policy::Point body{};if(!Body(c.character,body))return false;
    policy::Vent best{};bool found{};
    float bestDistance=std::numeric_limits<float>::infinity();
    const bool enter=request.action==Action::EnterVent;
    // AvS round 10: a flame retreat (pitch marker) may pick a vent up to 25 m
    // away; the native vent node walks the path there.
    const bool retreat=enter && ap::fear::RetreatPitch(request.pitch);
    for(std::uint32_t i=0U;i<count;++i) {
        std::uintptr_t descriptor{};policy::Vent vent{};vent.localIndex=i;
        if(!Read(entries+static_cast<std::uintptr_t>(i)*0x30U+0x28U,descriptor) || !descriptor ||
            !Read(descriptor,vent.entrance) || !policy::Finite(vent.entrance) || !ExitPoint(descriptor,vent.exit))continue;
        if(c.host) {
            if((request.flags&ap::VentSelected)!=0U) {
                constexpr float tolerance=policy::kVentMatchTolerance*policy::kVentMatchTolerance;
                if(policy::DistanceSquared(vent.entrance,{request.x,request.y,request.z})>tolerance)continue;
                if(enter && !(retreat?RetreatReach(body,vent.entrance):ap::vents::WithinReach({body.x,body.y,body.z},
                    {vent.entrance.x,vent.entrance.y,vent.entrance.z})))continue;
                if(found){result.reason=Reason::AmbiguousVent;return false;}
                best=vent;found=true;continue;
            }
            const auto p=enter?vent.entrance:vent.exit;const auto d=policy::DistanceSquared(body,p);
            if(enter) {
                if(!ap::vents::WithinReach({body.x,body.y,body.z},{p.x,p.y,p.z}))continue;
                if(!found || d<bestDistance){best=vent;found=true;bestDistance=d;}
            } else {
                // Donor vent.cpp::NearestUntried: nearest real opening to
                // the Alien, with a height penalty. Backstage is often above
                // the ceiling; a same-floor constraint prevents all exits.
                const auto score=std::hypot(body.x-vent.entrance.x,body.z-vent.entrance.z)+
                    std::abs(body.y-vent.entrance.y)*0.7F;
                if(std::isfinite(score) && (!found || score<bestDistance)) {best=vent;found=true;bestDistance=score;}
            }
        } else {
            const policy::Point entrance{request.x,request.y,request.z};
            const policy::Point exit{request.resolvedX,request.resolvedY,request.resolvedZ};
            constexpr float e=policy::kVentMatchTolerance*policy::kVentMatchTolerance;
            if(policy::DistanceSquared(entrance,vent.entrance)>e || policy::DistanceSquared(exit,vent.exit)>e)continue;
            if(found){result.reason=Reason::AmbiguousVent;return false;}
            best=vent;found=true;
        }
    }
    // Donor vent.cpp::PickGo keeps the retail path-aware choice when it is
    // almost as near as the closest opening. Forcing the nearest point can
    // choose the wrong side of a wall or an inaccessible approach.
    // AvS 2026-11-25 round 5: the pilot's E always carries VentSelected (the
    // straight-line nearest opening), so this never ran and the Alien was sent
    // ~11 m around a wall to an approach it then could not open. Apply it to
    // every entry; the retail choice must still be within reach of the body.
    if(c.host && enter && found && !retreat) {
        const auto original=RetailEntry(manager);
        if(original>=entries && (original-entries)%0x30U==0U && (original-entries)/0x30U<count) {
            policy::Vent vent{};std::uintptr_t descriptor{};
            vent.localIndex=static_cast<std::uint32_t>((original-entries)/0x30U);
            if(Read(original+0x28U,descriptor) && descriptor && Read(descriptor,vent.entrance) &&
                policy::Finite(vent.entrance) && ExitPoint(descriptor,vent.exit) &&
                ap::vents::WithinReach({body.x,body.y,body.z},{vent.entrance.x,vent.entrance.y,vent.entrance.z}) &&
                std::sqrt(policy::DistanceSquared(body,vent.entrance))<=std::sqrt(bestDistance)+3.0F)best=vent;
        }
    }
    std::uintptr_t current{},table{};std::uint32_t size{};
    if(!found || !Read(withdraw+0x24U,current) || current!=manager || !Read(manager+8U,table) || table!=entries ||
        !Read(manager+12U,size) || size!=count)return false;
    result.x=best.entrance.x;result.y=best.entrance.y;result.z=best.entrance.z;
    result.resolvedX=best.exit.x;result.resolvedY=best.exit.y;result.resolvedZ=best.exit.z;
    return true;
}
VentSlot* FreeVentSlot() noexcept {
    for(auto& slot:ventSlots)if(!slot.used && !slot.quarantined)return &slot;
    return nullptr;
}
bool VentFrameLive(const ap::Context& c,const VentSlot& slot) noexcept {
    std::uintptr_t node{},tree{},vtable{},withdraw{},manager{};
    return Live(c) && slot.used && !slot.quarantined && slot.frame &&
        Read(slot.frame,vtable) && vtable==gBase+0x00DBCD38U &&
        Read(slot.frame+0x10U,node) && node==reinterpret_cast<std::uintptr_t>(slot.node.data()) &&
        Read(slot.frame+0x14U,tree) && tree==gPvPLease.tree &&
        Read(c.character+0x34F0U,withdraw) && withdraw && Read(withdraw+0x24U,manager) && manager==slot.manager;
}
bool StartVent(const ap::Context& c,ap::Frame& result,VentSlot& slot) noexcept {
    std::uintptr_t withdraw{};std::uint32_t allocated{};
    if(!Live(c) || !Read(c.character+0x34F0U,withdraw) || !withdraw ||
        !Read(withdraw+0x24U,slot.manager) || !slot.manager ||
        !Read(gBase+0x012B1C20U,allocated) || allocated>=0x6F6U || !PortalIndex(c,result,slot.selectedIndex))return false;
    slot.used=true;slot.scope=c.scope;slot.character=c.character;
    AIMP_ACTION_TRY {
        using Constructor=void*(__thiscall*)(void*);
        using Create=std::uintptr_t(__thiscall*)(void*,std::uint32_t,std::uintptr_t);
        using Dispatch=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,std::uint32_t,std::uint32_t,std::uint32_t);
        const auto ctor=result.action==Action::EnterVent?0x0000F680U:0x0000FC50U;
        Native<Constructor>(ctor)(slot.node.data());
        if(result.action==Action::EnterVent) {
            if(!ap::vent_node::PilotEntryMethods(gBase,nullptr,
                [](void*,std::uintptr_t at,void* out,std::size_t n) noexcept {
                    return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),n);
                },slot.entryMethods)){slot={};return false;}
            slot.node[0]=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(slot.entryMethods.data()));
        }
        slot.frame=Native<Create>(0x002F15C0U)(slot.node.data(),1U,gPvPLease.tree);
        if(!slot.frame){slot.used=false;return false;}
        // Dispatch invokes the real Start method: frame+3C flags and the
        // vent-manager lock are established by CATHODE, not our own writes.
        Native<Dispatch>(0x002D0260U)(slot.node.data(),c.character,slot.frame,0U,0U,0U);
        slot.started=true;
        return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
bool StepVent(const ap::Context& c,VentSlot& slot,const ap::Frame& plan,int& result) noexcept {
    if(!VentFrameLive(c,slot))return false;
    const auto previous=selectedPortal;selectedPortal=&slot;
    AIMP_ACTION_TRY {
        // Run the donor's actual GoBackstage/ComeFrontstage node Update,
        // including its portal getter, cancel polling and native frame status.
        using Update=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t);
        Native<Update>(plan.action==Action::EnterVent?0x0000F5C0U:0x0000FB10U)(slot.node.data(),c.character,slot.frame);
        const auto status=*reinterpret_cast<const std::uint32_t*>(slot.frame+4U);
        result=status==9U?1:(status==8U?2:0);
        selectedPortal=previous;return true;
    } AIMP_ACTION_CATCH {selectedPortal=previous;slot.quarantined=true;return false;}
}
bool FinishVent(const ap::Context& c,VentSlot& slot,bool cancel,bool& done) noexcept {
    done=false;
    if(!VentFrameLive(c,slot))return false;
    AIMP_ACTION_TRY {
        using Two=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t);
        using RequestCancel=void(__stdcall*)(std::uintptr_t,std::uintptr_t);
        using PollCancel=std::uint32_t(__stdcall*)(std::uintptr_t,std::uintptr_t);
        using Destroy=void*(__thiscall*)(void*,std::uint32_t);
        if(cancel && !slot.cancelling) {
            Native<RequestCancel>(0x0030A7C0U)(c.character,slot.frame);
            slot.cancelling=true;
        }
        // Retail node Update uses 0070A5E0 while frame status is 7. A
        // cancellation request is NOT proof that scheduler/worker Refs retired.
        // Keep both the pooled frame AND stable node storage until this passes.
        if(slot.cancelling && (Native<PollCancel>(0x0030A5E0U)(
                c.character,slot.frame)&0xFFU)==0U)return true;
        if(slot.started)Native<Two>(0x0000FBC0U)(slot.node.data(),c.character,slot.frame);
        Native<Destroy>(0x002FA8D0U)(reinterpret_cast<void*>(slot.frame),1U);
        slot={};done=true;return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
// Donor Path -> ActionMoveToMostRecentSensedPosition (006ED2B0).
// Only its immutable node layout is reproduced. The destination is the last
// authenticated Host pose, never a client-selected target or an AI blackboard.
struct RecoverySlot final {
    std::array<std::uint32_t,3> node{};
    ap::Context context{};std::uintptr_t frame{};
    policy::Point goal{};std::uint64_t selectedAt{},steppedAt{},began{};
    bool used{},cancelling{},quarantined{};
};
std::array<RecoverySlot,8> recoveries{};
std::uint64_t recoveryRetryAt{};
RecoverySlot* RecoveryFor(const ap::Context& c) noexcept {
    for(auto& slot:recoveries)if(slot.used && slot.context.character==c.character && slot.context.scope==c.scope)return &slot;
    return nullptr;
}
bool RecoveryLive(const ap::Context& c,const RecoverySlot& slot) noexcept {
    std::uintptr_t vtable{},node{},tree{};
    return Live(c) && slot.used && !slot.quarantined && slot.frame &&
        Read(slot.frame,vtable) && vtable==gBase+0x00DBCD38U &&
        Read(slot.frame+0x10U,node) && node==reinterpret_cast<std::uintptr_t>(slot.node.data()) &&
        Read(slot.frame+0x14U,tree) && tree==gPvPLease.tree;
}
bool FinishRecovery(const ap::Context& c,RecoverySlot& slot,bool cancel) noexcept {
    if(!RecoveryLive(c,slot)){slot.quarantined=true;return false;}
    AIMP_ACTION_TRY {
        using Two=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t);
        using PollCancel=std::uint32_t(__stdcall*)(std::uintptr_t,std::uintptr_t);
        using Destroy=void*(__thiscall*)(void*,std::uint32_t);
        if(cancel && !slot.cancelling) {
            Native<Two>(0x0030A7C0U)(slot.node.data(),c.character,slot.frame);slot.cancelling=true;
        }
        if(slot.cancelling && (Native<PollCancel>(0x0030A5E0U)(c.character,slot.frame)&0xFFU)==0U)return false;
        // This node's Shutdown is 00718810 -> 0070AF80, NOT the vent unlock.
        Native<Two>(0x00318810U)(slot.node.data(),c.character,slot.frame);
        Native<Destroy>(0x002FA8D0U)(reinterpret_cast<void*>(slot.frame),1U);
        slot={};return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
bool CancelRecovery(const ap::Context& c,bool living) noexcept {
    auto* slot=RecoveryFor(c);if(!slot)return true;
    if(!living || !Live(c)){slot->quarantined=true;return false;}
    return FinishRecovery(c,*slot,true);
}
bool StartRecovery(const ap::Context& c,const ap::Frame& goal,RecoverySlot& slot) noexcept {
    std::uint32_t allocated{};
    if(!Live(c) || c.host || !c.owned || !gPvPLease.owned || gPvPLease.releasing ||
        !Read(gBase+0x012B1C20U,allocated) || allocated>=0x6F6U ||
        !ap::action_abi::Matches(gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t n) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),n);
        }))return false;
    slot.used=true;slot.context=c;
    // 006ED2B0 allocates 12 bytes: vtable, shutdown speed, movement type.
    slot.node={static_cast<std::uint32_t>(gBase+0x00EB84D0U),0U,std::min(goal.gait,3U)};
    slot.goal={goal.x,goal.y,goal.z};slot.began=slot.selectedAt=GetTickCount64();
    AIMP_ACTION_TRY {
        using Create=std::uintptr_t(__thiscall*)(void*,std::uint32_t,std::uintptr_t);
        using Dispatch=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,std::uint32_t,std::uint32_t,std::uint32_t);
        slot.frame=Native<Create>(0x002F15C0U)(slot.node.data(),1U,gPvPLease.tree);
        if(!slot.frame){slot={};return false;}
        // Frame factory supplies ordinary movement flags=2. Unlike a vent
        // request, no flag is changed and no manager lock is manufactured.
        Native<Dispatch>(0x002D0260U)(slot.node.data(),c.character,slot.frame,0U,0U,0U);
        return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
// AvS round 10 crawlspaces: the pilot's kind9 direction move never takes the
// hatch nav link, so the Alien could walk inside a duct but not get out (log
// 2026-11-26: three stalls in cycle 56 right after a vent exit). In crawl mode
// the OWNER drives the same retail path node the recovery uses
// (ActionMoveToMostRecentSensedPosition, 0030A940) toward a short goal ahead of
// the input direction; the retail path follower plays the crawl and hatch
// traversals. Other PCs follow the owner's measured poses as usual.
RecoverySlot crawlSlot{};
struct CrawlTelemetry final {
    std::atomic<std::uint64_t> starts{},moves{},arrived{},failed{},stops{},refused{};
    std::atomic<int> lastResult{-1};std::atomic<float> lastGoalM{};
} crawlTelemetry;
bool StartPathSlot(const ap::Context& c,const policy::Point& goal,std::uint32_t gait,RecoverySlot& slot) noexcept {
    std::uint32_t allocated{};
    // Pass 19: observers also drive it (ControlRuntime MirrorOwnerCrawl) so the
    // replica takes the same hatch links as the owner; the lease must be ours.
    if(!Live(c) || !c.owned || !gPvPLease.owned || gPvPLease.releasing ||
        !Read(gBase+0x012B1C20U,allocated) || allocated>=0x6F6U ||
        !ap::action_abi::Matches(gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t n) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),n);
        }))return false;
    slot.used=true;slot.context=c;
    slot.node={static_cast<std::uint32_t>(gBase+0x00EB84D0U),0U,std::min(gait,3U)};
    slot.goal=goal;slot.began=slot.selectedAt=GetTickCount64();slot.steppedAt=0U;
    AIMP_ACTION_TRY {
        using Create=std::uintptr_t(__thiscall*)(void*,std::uint32_t,std::uintptr_t);
        using Dispatch=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,std::uint32_t,std::uint32_t,std::uint32_t);
        slot.frame=Native<Create>(0x002F15C0U)(slot.node.data(),1U,gPvPLease.tree);
        if(!slot.frame){slot={};return false;}
        Native<Dispatch>(0x002D0260U)(slot.node.data(),c.character,slot.frame,0U,0U,0U);
        return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
bool Crawl(const ap::Context& c,const ap::Frame* intent) noexcept {
    auto& slot=crawlSlot;
    const bool wantStop=!intent || (intent->forward==0.0F && intent->strafe==0.0F);
    if(wantStop || !Live(c) || gPvPLease.releasing || Same(c)) {
        if(!slot.used)return true;
        if(!Live(c)){slot.quarantined=true;return false;}
        crawlTelemetry.stops.fetch_add(1U,std::memory_order_relaxed);
        return FinishRecovery(c,slot,true);
    }
    if(slot.quarantined){crawlTelemetry.refused.fetch_add(1U,std::memory_order_relaxed);return false;}
    std::uint8_t state{};bool backstage{};
    if(!Cycle(c.character,state,backstage) || backstage || state==18U || state==78U || state==79U) {
        if(slot.used)static_cast<void>(FinishRecovery(c,slot,true));
        return false;
    }
    if(slot.cancelling)return !FinishRecovery(c,slot,true);
    policy::Point body{};if(!Body(c.character,body))return false;
    const float yaw=intent->yaw+std::atan2(intent->strafe,intent->forward);
    const auto now=GetTickCount64();
    // Alternate the goal distance after a failed plan (wall right ahead).
    const float reach=(crawlTelemetry.failed.load(std::memory_order_relaxed)&1U)?1.2F:2.4F;
    const policy::Point goal{body.x+std::sin(yaw)*reach,body.y,body.z+std::cos(yaw)*reach};
    if(!slot.used) {
        if(!StartPathSlot(c,goal,1U,slot))return false;
        crawlTelemetry.starts.fetch_add(1U,std::memory_order_relaxed);
    }
    if(!RecoveryLive(c,slot)){slot.quarantined=true;return false;}
    if(now-slot.selectedAt>=150U){slot.goal=goal;slot.selectedAt=now;}
    if(slot.steppedAt && now-slot.steppedAt<33U)return true;
    slot.steppedAt=now;
    crawlTelemetry.lastGoalM.store(reach,std::memory_order_relaxed);
    AIMP_ACTION_TRY {
        using Move=int(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,const void*,std::uint32_t,float,float,std::uint32_t);
        const auto result=Native<Move>(0x0030A940U)(slot.node.data(),c.character,slot.frame,&slot.goal,0U,0.0F,0.3F,0U);
        crawlTelemetry.moves.fetch_add(1U,std::memory_order_relaxed);
        crawlTelemetry.lastResult.store(result,std::memory_order_relaxed);
        if(result==1){crawlTelemetry.arrived.fetch_add(1U,std::memory_order_relaxed);static_cast<void>(FinishRecovery(c,slot,false));}
        else if(result!=0){crawlTelemetry.failed.fetch_add(1U,std::memory_order_relaxed);static_cast<void>(FinishRecovery(c,slot,true));}
        return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
bool Recover(const ap::Context& c,const ap::Frame* goal) noexcept {
    auto* slot=RecoveryFor(c);
    if(!goal || c.host || !Live(c) || Same(c) || gPvPLease.releasing)
        return !CancelRecovery(c,Live(c));
    std::uint8_t state{};bool backstage{};
    if(!Cycle(c.character,state,backstage) || backstage || !Ordinary(state))return !CancelRecovery(c,true);
    if(slot && (slot->quarantined || slot->cancelling))return !CancelRecovery(c,true);
    policy::Point body{};if(!Body(c.character,body))return slot!=nullptr;
    const policy::Point observed{goal->x,goal->y,goal->z};
    if(!policy::Finite(observed))return !CancelRecovery(c,true);
    const float planar=std::hypot(observed.x-body.x,observed.z-body.z);
    const float vertical=std::abs(observed.y-body.y);
    if(slot && planar<=1.25F && vertical<=0.5F)return !CancelRecovery(c,true);
    const auto now=GetTickCount64();
    if(!slot) {
        if((planar<=4.0F && vertical<=1.0F) || now<recoveryRetryAt)return false;
        for(auto& candidate:recoveries)if(!candidate.used && !candidate.quarantined){slot=&candidate;break;}
        if(!slot || !StartRecovery(c,*goal,*slot)){recoveryRetryAt=now+500U;return slot && slot->used;}
    }
    if(!RecoveryLive(c,*slot)){slot->quarantined=true;return true;}
    // Stable goals avoid replaying a new plan every render frame. Only the
    // authenticated observations may advance the recovery destination.
    if(now-slot->selectedAt>=250U && policy::DistanceSquared(observed,slot->goal)>=1.0F) {
        slot->goal=observed;slot->selectedAt=now;
    }
    if(slot->steppedAt && now-slot->steppedAt<33U)return true;
    slot->steppedAt=now;
    AIMP_ACTION_TRY {
        using Move=int(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,const void*,std::uint32_t,float,float,std::uint32_t);
        const auto result=Native<Move>(0x0030A940U)(slot->node.data(),c.character,slot->frame,&slot->goal,0U,0.0F,0.5F,0U);
        if(result==1) {static_cast<void>(FinishRecovery(c,*slot,false));recoveryRetryAt=now+100U;}
        else if(result!=0 || now-slot->began>15000U) {
            static_cast<void>(FinishRecovery(c,*slot,true));recoveryRetryAt=now+500U;
        }
        // A failed path is retried with a new native request after retirement;
        // it is never converted into a teleport or a synthetic arrival.
        return true;
    } AIMP_ACTION_CATCH {slot->quarantined=true;return true;}
}
bool ResourcesPending() noexcept {
#ifndef AIMP_ALIEN_ACTION_TEST
    for(const auto& d:donorMovements)if(d.used && Live(d.context))return true;
#endif
    if(active.used)return true;
    for(const auto& slot:recoveries)if(slot.used && Live(slot.context))return true;
    return false;
}
ap::attack::Memory AttackMemory() noexcept {
    return {gBase,nullptr,
        [](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
        },
        [](void*,std::uintptr_t at,const void* in,std::size_t size) noexcept {
            return crash_guard::TryWrite(reinterpret_cast<void*>(at),in,size);
        }};
}
// The retail window stays below the protocol's 14s Preparing stall limit: 458 showed
// the BT running ~5s to reach the victim before the pair (cycle 78) starts.
constexpr std::uint64_t kClientAttackWaitMs=16000U,kRetailAttackWindowMs=10000U,kRetailNoStartMs=3000U;
bool HoldAttackTarget(const std::uintptr_t victim) noexcept {
    const ap::attack::Manager manager{active.chooser,0U,0U};
    return ap::attack::Hold(AttackMemory(),manager,victim,active.heldTarget)==ap::attack::HoldResult::Held;
}
// Retail alien_behave runs ATTACK_CORE_BRANCH (ConditionHasMeleeAttackAvailable
// OrIsAttacking -> ActionMeleeAttack) only while the logic character flag
// SHOULD_ATTACK (5) is set: byte AnimatedCharacter+423C+flag, written by
// 00785B10. The branch clears it after one try, so it is raised every poll.
constexpr std::uintptr_t kShouldAttackFlag=0x423CU+5U;
bool SetShouldAttack(const bool on) noexcept {
    if(!active.attackAnimated)return false;
    const std::uint8_t value=on?1U:0U;
    return crash_guard::TryWrite(reinterpret_cast<void*>(active.attackAnimated+kShouldAttackFlag),&value,sizeof(value));
}
// A stale level/lease may have freed the record: drop the hold, never write.
void ReleaseAttackTarget() noexcept {
    if(Live(active.context)) {
        if(active.heldTarget.record)ap::attack::Release(AttackMemory(),active.heldTarget);
        if(active.brainTarget.record)ap::attack::Release(AttackMemory(),active.brainTarget);
        if(active.shouldAttackSet)static_cast<void>(SetShouldAttack(false));
    }
    active.heldTarget={};active.brainTarget={};active.shouldAttackSet=false;
#ifndef AIMP_ALIEN_ACTION_TEST
    gPvPMeleeWatch.store(0U,std::memory_order_relaxed);
    DisarmPvPBrain();
#endif
}
// ActionMeleeAttack 00717D90 reads the target through AnimatedCharacter+34A8
// ->+10 (donor TargetOf/SetTargetOf). Hold it there too if it is not the
// record the attack manager query reads.
bool HoldBrainTarget(const std::uintptr_t victim) noexcept {
    std::uintptr_t brain{},record{},target{};std::uint32_t birth{},victimBirth{};
    if(!Read(active.attackAnimated+0x34A8U,brain) || !brain || !Read(brain+0x10U,record) || !record)return false;
    if(record==active.heldTarget.record)return true;
    if(!Read(record+0x42CU,target) || !Read(record+0x430U,birth) || !Read(victim+0x50U,victimBirth))return false;
    if(active.brainTarget.record!=record) {
        if(active.brainTarget.record)ap::attack::Release(AttackMemory(),active.brainTarget);
        active.brainTarget={record,target,birth,victim};
    }
    if(target==victim && birth==victimBirth)return true;
    return crash_guard::TryWrite(reinterpret_cast<void*>(record+0x430U),&victimBirth,sizeof(victimBirth)) &&
        crash_guard::TryWrite(reinterpret_cast<void*>(record+0x42CU),&victim,sizeof(victim));
}
bool AttackQuery(const ap::attack::Memory& memory,const ap::attack::Manager& manager,
    const std::uintptr_t victim,bool& ready,std::uint32_t& selector) noexcept {
    AIMP_ACTION_TRY {
        using Evaluate=std::uint8_t(__thiscall*)(void*,std::uintptr_t,float*);
        using Choose=std::uint32_t(__thiscall*)(void*,std::uintptr_t,std::uint32_t,std::uint32_t);
        float heading{};
        auto* chooser=reinterpret_cast<void*>(manager.address);
        if(!Native<Evaluate>(ap::attack::kEvaluateRva)(chooser,victim,&heading)){TraceAttack(4U);return true;}
        const auto chosen=Native<Choose>(ap::attack::kChooseRva)(chooser,victim,1U,15U);
        if(!ap::attack::Selected(memory,manager,chosen)){TraceAttack(5U,chosen);return true;}
        selector=chosen;ready=true;return true;
    } AIMP_ACTION_CATCH {return false;}
}
// Explicit-target retail Alien query; no fabricated hit or health write.
// The donor attack gate uses this same AlienAttackManager, not the separate
// human/android manager (012CC390, +800 cache). The query reads the selected
// AI target record, so the victim is held there (donor SetTargetOf) ONLY for
// this engine-thread call. 453 kept it held for the whole action: the leased
// Alien's retail brain (worker thread, not lease-fenced) then started its own
// kill, which AISync never captures, so the pilot's PC saw no animation.
// When the Host itself pilots the Alien, the only human the Alien could have
// selected is the hidden technical player, so the selector often has NO
// current record (+10) and Hold reports noRecord (468 test). Retail 0072AFB0
// (first half of the CATHODE set-target 005FBB40) makes the victim's record
// the selector's forced target (+C). Its 0072B880 refresh is the fenced
// coordinator, so instead install that record as current the donor's way
// (hugger.cpp EnsureTargetEntry): slots are smart pointers with the refcount
// at record+14, so take our own reference before storing it. Engine thread.
constexpr std::uintptr_t kTargetEntryTableRva=0x00EEE8B8U;
bool CallSetForcedTarget(std::uintptr_t selector,std::uintptr_t victim) noexcept {
    __try {
        reinterpret_cast<void(__thiscall*)(void*,std::uintptr_t)>(gBase+0x0032AFB0U)(reinterpret_cast<void*>(selector),victim);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool EnsureCurrentTargetRecord(const std::uintptr_t selector,const std::uintptr_t victim) noexcept {
    std::uintptr_t current{},forced{},forcedTarget{};
    if(!selector || !Read(selector+0x10U,current))return false;
    if(current)return true;
    if(!Read(selector+0xCU,forced) || !forced || !Read(forced+0x42CU,forcedTarget) || forcedTarget!=victim) {
        if(!CallSetForcedTarget(selector,victim))return false;
    }
    for(const auto offset:{0x0CU,0x14U,0x18U}) {
        std::uintptr_t entry{},table{};std::int32_t references{};
        if(!Read(selector+offset,entry) || !entry || !Read(entry,table) || table!=gBase+kTargetEntryTableRva ||
            !Read(entry+0x14U,references) || references<=0)continue;
        ++references;
        if(!crash_guard::TryWrite(reinterpret_cast<void*>(entry+0x14U),&references,sizeof(references)))return false;
        if(!crash_guard::TryWrite(reinterpret_cast<void*>(selector+0x10U),&entry,sizeof(entry)))return false;
        return true;
    }
    return false;
}
bool AttackAvailability(const ap::Context& c,const PairIdentity& victim,std::uintptr_t& chooser,
    bool& ready,std::uint32_t& selector) noexcept {
    ready=false;selector=0U;PairIdentity source{};
    const auto memory=AttackMemory();
    ap::attack::Manager manager{};
    if(!Live(c) || !PairIdentityLive(victim) || PublishedPairSource(c.character,source)==kActors ||
        !PairIdentityLive(source) || !ap::attack::Resolve(memory,c.character,source.animated,manager) ||
        (chooser && chooser!=manager.address))return false;
    chooser=manager.address;active.attackAnimated=source.animated;
    {
        std::uintptr_t context{},querySelector{},brainSelector{};
        if(Read(manager.address+4U,context) && context && Read(context+0xC8U,querySelector) && querySelector)
            static_cast<void>(EnsureCurrentTargetRecord(querySelector,victim.character));
        if(Read(source.animated+0x34A8U,brainSelector) && brainSelector && brainSelector!=querySelector)
            static_cast<void>(EnsureCurrentTargetRecord(brainSelector,victim.character));
    }
    switch(ap::attack::Hold(memory,manager,victim.character,active.heldTarget)) {
    case ap::attack::HoldResult::Held:break;
    case ap::attack::HoldResult::NoSelection:TraceAttack(1U);return true;
    case ap::attack::HoldResult::NoRecord:TraceAttack(2U);return true;
    default:TraceAttack(3U);ReleaseAttackTarget();return false;
    }
    const bool queried=AttackQuery(memory,manager,victim.character,ready,selector);
    ReleaseAttackTarget();
    return queried;
}
bool AttackQueryRetired(const std::uintptr_t chooser) noexcept {
    if(!chooser)return true;
    std::uintptr_t table{};
    if(!Read(chooser,table) || table!=gBase+ap::attack::kTableRva)return false;
    AIMP_ACTION_TRY {
        // Alien cover-ray handle lives at +1CC, not the android +20/+28 probe.
        // Wait for it to retire before releasing action bookkeeping.
        using Valid=std::uint8_t(__thiscall*)(void*);
        using Status=std::uint32_t(__thiscall*)(void*);
        auto* handle=reinterpret_cast<void*>(chooser+0x1CCU);
        return !Native<Valid>(0x003EB940U)(handle) || Native<Status>(0x003EB960U)(handle)!=1U;
    } AIMP_ACTION_CATCH {return false;}
}
bool SelectVictim(const ap::Context& c,const ap::Frame& command,PairIdentity& victim) noexcept {
    std::array<policy::Candidate,33> candidates{};std::size_t count{};
    policy::Point source{};if(!Body(c.character,source))return false;
    unsigned seen{},living{},published{};
    float nearest=-1.0F,nearestHeight{},nearestCosine{};
    const auto append=[&](std::uint64_t peer) noexcept {
        if(!peer || peer==c.scope.ownerPeer || count==candidates.size())return;
        for(std::size_t i=0;i<count;++i)if(candidates[i].peer==peer)return;
        const auto character=ParticipantForPeer(peer);PairIdentity p{};policy::Point point{};
        ++seen;
        if(!LivingCharacter(character))return;
        ++living;
        if(Body(character,point)) {
            const float dx=point.x-source.x,dz=point.z-source.z,d=std::sqrt(dx*dx+dz*dz);
            if(nearest<0.0F || d<nearest) {
                nearest=d;nearestHeight=point.y-source.y;
                nearestCosine=d>0.05F?(dx*std::sin(command.yaw)+dz*std::cos(command.yaw))/d:1.0F;
            }
        }
        if(!PublishedPairVictim(character,p) || !PairIdentityLive(p) || !Body(character,point))return;
        ++published;
        // This is only the candidate shortlist. Actual reachability is proved
        // by the explicit-target retail availability query before entry.
        candidates[count++]={peer,point,true,true,player_hiding::Hidden(character),true};
    };
    append(gCurrent.localPeer);
    for(std::uint32_t i=0;i<gCurrent.participantCount;++i)append(gCurrent.participants[i].peer);
    const auto peer=policy::SelectVictim(source,command.yaw,c.scope.ownerPeer,{candidates.data(),count});
    const bool selected=peer && PublishedPairVictim(ParticipantForPeer(peer),victim) && PairIdentityLive(victim) && victim.peer==peer;
    if(!selected)TraceMiss(command.hostSequence,seen,living,published,nearest,nearestHeight,nearestCosine,command.yaw);
    return selected;
}
bool VictimStillInReach(const ap::Context& c,const PairIdentity& victim,const ap::Frame& command) noexcept {
    if(!PairIdentityLive(victim) || !LivingCharacter(victim.character) || ParticipantForPeer(victim.peer)!=victim.character)return false;
    policy::Point source{},target{};
    if(!Body(c.character,source) || !Body(victim.character,target))return false;
    const policy::Candidate candidate{victim.peer,target,true,true,false,true};
    return policy::SelectVictim(source,command.yaw,c.scope.ownerPeer,{&candidate,1U})==victim.peer;
}
// Round 5 guaranteed kill: the chosen victim stays the target while alive,
// on (about) the same floor and within the leash; aim no longer matters.
bool VictimInLeash(const ap::Context& c,const PairIdentity& victim) noexcept {
    if(!PairIdentityLive(victim) || !LivingCharacter(victim.character) || ParticipantForPeer(victim.peer)!=victim.character)return false;
    policy::Point source{},target{};
    if(!Body(c.character,source) || !Body(victim.character,target))return false;
    return std::abs(target.y-source.y)<=policy::kAttackHeight &&
        std::hypot(target.x-source.x,target.z-source.z)<=policy::kAttackLeash;
}
// Pass 20: close enough that only retail's room/cover check can still refuse
// the kill (survivor crouched under furniture right next to the Alien).
constexpr float kForcedContactM=1.4F,kForcedContactHeightM=1.0F;
constexpr std::uint64_t kForcedLatchMs=1500U;
constexpr std::uint32_t kForcedSelector=0U; // standard front grab (kills seen with selector 0)
bool VictimInContact(const ap::Context& c,const PairIdentity& victim) noexcept {
    if(!PairIdentityLive(victim) || !LivingCharacter(victim.character))return false;
    policy::Point source{},target{};
    if(!Body(c.character,source) || !Body(victim.character,target))return false;
    return std::abs(target.y-source.y)<=kForcedContactHeightM &&
        std::hypot(target.x-source.x,target.z-source.z)<=kForcedContactM;
}
// The victim is in ITS paired cycle (79) of this Alien's pair: the kill grabbed it.
bool VictimInPair(const std::uintptr_t source,const std::uintptr_t victim) noexcept {
    std::uintptr_t cycle{},target{},victimCycle{},owner{};std::uint8_t state{};
    return victim && Read(source+0x36CU,cycle) && cycle && Read(cycle+0x44U,target) && target==victim &&
        Read(victim+0x36CU,victimCycle) && victimCycle && Read(victimCycle+4U,owner) && owner==victim &&
        Read(victimCycle+0x1AU,state) && state==0x4FU;
}
// Pilot-side presentation of the Host's retail kill (donor puppet.cpp model).
// Same registration/transition the AISync Client pair replay uses; AISync's
// replay cannot drive the locally piloted Alien (455: placement gate 1).
// Health and death stay Host-authoritative through participant lifecycle.
// AvS 2026-11-25 (round 3): also the survivor PC's fallback when AISync's
// replay of a Host-owned Alien's kill never enters (victim = local player).
bool PresentPair(const ap::Context& c,const PairIdentity& victim,const std::uint32_t selector) noexcept {
    if(!Live(c) || !PvPLeaseScopeCurrent() || c.host || gPvPLease.releasing ||
        !PairIdentityLive(victim) || PairReservedCharacter(c.character) || PairReservedCharacter(victim.character))return false;
#ifndef AIMP_ALIEN_ACTION_TEST
    if(!StopDonor(c))return false;
#endif
    interactions::Definition definition{};
    if(!animation::ResolvePairDefinition(c.character,gPvPLease.runtimeId,victim.character,victim.runtimeId,selector,definition))return false;
    RecordDefinition(definition);
    std::uintptr_t root{},manager{},cycle{};
    if(!Read(gBase+kInteractionManagerGlobalRva,root) || !root || !Read(root,manager) || !manager ||
        !Read(c.character+0x36CU,cycle) || !cycle)return false;
    AIMP_ACTION_TRY {
        const auto accepted=Native<CharacterInteraction>(0x00254820U)(
            reinterpret_cast<void*>(manager+0x90U),c.scope.entityA,c.scope.entityB,victim.entityA,victim.entityB,
            definition.victimAttachment>0?std::uint64_t{1U}:std::uint64_t{0U},definition.victimAttachment);
        if((accepted&0xFFU)==0U)return false;
        Native<PairedTransitionA>(0x0025C4E0U)(reinterpret_cast<void*>(cycle),victim.character,
            static_cast<std::int32_t>(selector),0U);
        return true;
    } AIMP_ACTION_CATCH {return false;}
}
// AvS 2026-11-25 (user decision: direct kill only, no fallback). Every armed
// retail-brain attempt since 453 ended stage 24: ATTACK_CORE never ran
// (flagConsumed 0, meleeStarts 0), the Alien idled 3 s and the pilot's input
// was frozen. The Host now starts the same native paired kill the observer
// side already presents (PresentPair): interaction registration + the paired
// transition with the selector proven by the explicit availability query.
// Return codes (attack stage): 0 submitted, 26 definition not resolved,
// 27 interaction refused, 28 native fault, 29 donor still retiring.
unsigned HostDirectPair(const ap::Context& c,const PairIdentity& victim,const std::uint32_t selector) noexcept {
    // AvS 2026-11-25 (round 4): this also required c.localOwner, so with a
    // Client piloting the Alien every Host attempt returned 27 at once (35
    // clicks, no kill). The Host runs the kill on its replica either way; the
    // pilot's PC presents it on Started (PresentPair).
    // Guard failures are stage 36 with a bit mask (directGuard) so a refusal
    // by our own checks is never confused with a native refusal (27) again.
    const unsigned guard=(Live(c)?0U:1U)|(PvPLeaseScopeCurrent()?0U:2U)|(c.host?0U:4U)|
        (gPvPLease.releasing?8U:0U)|(PairIdentityLive(victim)?0U:16U)|
        (PairReservedCharacter(c.character)?32U:0U)|(PairReservedCharacter(victim.character)?64U:0U);
#ifndef AIMP_ALIEN_ACTION_TEST
    attackTelemetry.directGuard=guard;
    attackTelemetry.remoteOwner=c.localOwner?0U:1U;
#endif
    if(guard)return 36U;
#ifndef AIMP_ALIEN_ACTION_TEST
    if(!StopDonor(c))return 29U;
    static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(c.character));
#endif
    interactions::Definition definition{};
    if(!animation::ResolvePairDefinition(c.character,gPvPLease.runtimeId,victim.character,victim.runtimeId,selector,definition))return 26U;
    RecordDefinition(definition);
    std::uintptr_t root{},manager{},cycle{};
    if(!Read(gBase+kInteractionManagerGlobalRva,root) || !root || !Read(root,manager) || !manager ||
        !Read(c.character+0x36CU,cycle) || !cycle)return 28U;
    AIMP_ACTION_TRY {
        const auto accepted=Native<CharacterInteraction>(0x00254820U)(
            reinterpret_cast<void*>(manager+0x90U),c.scope.entityA,c.scope.entityB,victim.entityA,victim.entityB,
            definition.victimAttachment>0?std::uint64_t{1U}:std::uint64_t{0U},definition.victimAttachment);
        if((accepted&0xFFU)==0U)return 27U;
        Native<PairedTransitionA>(0x0025C4E0U)(reinterpret_cast<void*>(cycle),victim.character,
            static_cast<std::int32_t>(selector),0U);
        return 0U;
    } AIMP_ACTION_CATCH {return 28U;}
}
// One re-issue of the paired transition on the Alien's CURRENT cycle (the
// registration above stays). Round 2's first attack was registered but the
// still-running locomotion replaced the cycle before it entered 78.
bool HostRetryTransition(const ap::Context& c,const PairIdentity& victim,const std::uint32_t selector) noexcept {
    if(!Live(c) || !c.host || !PairIdentityLive(victim))return false;
    std::uintptr_t cycle{};
    if(!Read(c.character+0x36CU,cycle) || !cycle)return false;
    AIMP_ACTION_TRY {
        Native<PairedTransitionA>(0x0025C4E0U)(reinterpret_cast<void*>(cycle),victim.character,
            static_cast<std::int32_t>(selector),0U);
        return true;
    } AIMP_ACTION_CATCH {return false;}
}
constexpr std::uint64_t kDirectSettleMs=450U,kDirectRetryMs=600U,kDirectEntryMs=2000U,kObserverReplayWaitMs=1200U;
// Round 5: the source in 78 must grab its victim (79) within this, else retry.
constexpr std::uint64_t kVictimAcquireMs=1500U,kPairStallMs=9000U,kChaseQueryMs=100U;
void Clear() noexcept {
    if(active.victim.character)player_hiding::FinishPvPSearch(active.victim.character);
    ReleaseAttackTarget();
    active={};
}
void Terminal(ap::Frame& result,Phase phase,Reason reason) noexcept {
    result=active.receipt;result.phase=phase;result.reason=reason;++result.resolutionRevision;
    active.receipt=result;active.terminalSent=true;
    TraceVent();
}
void Cancel(const ap::Context& c,bool living) noexcept {
    static_cast<void>(StopDonor(c));
    static_cast<void>(CancelRecovery(c,living));
    if(!Same(c))return;
    if(!living || !Live(c)) {
        if(active.vent)active.vent->quarantined=true;
        Clear();return;
    }
    active.cancelled=true;
    TraceVent();
    if(active.remoteVent && !active.vent){Clear();return;}
    if(active.victim.character)player_hiding::FinishPvPSearch(active.victim.character);
    if(active.vent) {
        bool done{};
        if(!FinishVent(c,*active.vent,true,done)) {active.vent->quarantined=true;Clear();return;}
        if(done)Clear();
        return;
    }
    if(active.moveArrived && (active.receipt.action==Action::EnterVent || active.receipt.action==Action::ExitVent)) {
        // The native node was already shut down and released. Stationary
        // backstage77 is not an outstanding traversal resource to retain.
        Clear();return;
    }
    // A registered pair belongs to the existing native retirement path. Never
    // force Idle/zero a cycle or mark a death complete on cancellation.
    if(!active.entrySubmitted && !active.entered && AttackQueryRetired(active.chooser))Clear();
}
bool InterruptVent(const ap::Context& c,ap::Frame& result) noexcept {
    if(!Same(c) || active.terminalSent ||
        (active.receipt.action!=Action::EnterVent && active.receipt.action!=Action::ExitVent))return false;
    // AvS 2026-11-24 (entry 8111): a cancel accepted after the passage began
    // left the Host Alien backstage while every replica rolled its own
    // traversal back and kept showing it. Once the opening animation (18) or,
    // for an entry, backstage has started, the passage must complete.
    std::uint8_t state{};bool backstage{};
    if(Cycle(c.character,state,backstage) &&
        (state==18U || (active.receipt.action==Action::EnterVent && (backstage || Protected(state))))) {
        ++active.cancelRefusals;TraceVent(4U);return false;
    }
    Terminal(result,Phase::Cancelled,Reason::NativeBusy);
    Cancel(c,true);return true;
}
bool Begin(const ap::Context& c,const ap::Frame& command,ap::Frame& result) noexcept {
    result=command;result.resolutionRevision=1U;
    if(!Live(c) || !PvPLeaseScopeCurrent() || gPvPLease.releasing){Reject(result,Reason::Stale);return true;}
    if(Quarantined(c)){Reject(result,Reason::Fault);return true;}
    if(!ap::action_abi::Matches(gBase,nullptr,
        [](void*,std::uintptr_t address,void* out,std::size_t size) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(address),size);
        })) {Reject(result,Reason::NativeUnavailable);return true;}
    if(!c.host) {
        // Client never invokes the attack selector. The existing AISync native
        // pair transaction, with its real Host outcome, is its sole executor.
        if(command.kind!=ap::Kind::Resolution)return false;
        if(command.phase==Phase::Rejected || command.phase==Phase::Failed || command.phase==Phase::Cancelled) {
            if(Same(c) && active.receipt.hostSequence==command.hostSequence)Cancel(c,true);
            return true;
        }
        if(command.phase==Phase::Completed) {
            if(Same(c) && active.receipt.hostSequence==command.hostSequence)active.hostEnded=true;
            return true;
        }
        if(Same(c)) {
            if(active.receipt.hostSequence==command.hostSequence && command.action==Action::Attack) {
                active.receipt=command;return true;
            }
            result.phase=Phase::Failed;result.reason=Reason::NativeBusy;return true;
        }
    } else {
        if(edgeEpoch!=command.epoch) {if(active.used){Reject(result,Reason::Stale);return true;}
            edgeEpoch=command.epoch;edges.Reset(edgeEpoch);}
        if(!edges.Consume(command.epoch,command.hostSequence,GetTickCount64(),command.action==Action::Attack,active.used)) {
            Reject(result,active.used?Reason::NativeBusy:Reason::Cooldown);return true;
        }
    }
    std::uint8_t state{};bool backstage{};
    if(!Cycle(c.character,state,backstage)){Reject(result,Reason::NativeUnavailable);return true;}
    if(command.action==Action::Attack) {
        if(c.host && (backstage || !Ordinary(state))){Reject(result,Reason::NativeBusy);return true;}
        PairIdentity victim{};
        if(c.host && !SelectVictim(c,command,victim)){Reject(result,Reason::NoTarget);return true;}
        // Host entity IDs are process-local; peer selects our certified victim.
        // The paired-animation transport remaps the physical execution itself.
        if(!c.host && (!PublishedPairVictim(ParticipantForPeer(command.targetPeer),victim) ||
            !PairIdentityLive(victim) || victim.peer!=command.targetPeer)) {Reject(result,Reason::NoTarget);return true;}
        active={};active.used=true;active.context=c;active.began=GetTickCount64();active.victim=victim;
        if(c.host) {
            policy::Point target{};
            if(!Body(victim.character,target)){Clear();Reject(result,Reason::Stale);return true;}
            result.targetPeer=victim.peer;result.targetA=victim.entityA;result.targetB=victim.entityB;
            result.x=target.x;result.y=target.y;result.z=target.z;result.selector=0U;
            result.phase=Phase::Preparing;result.reason=Reason::None;
        }
        active.receipt=result;return true;
    }
    if(command.action!=Action::EnterVent && command.action!=Action::ExitVent)return false;
    if((command.action==Action::EnterVent && backstage) || (command.action==Action::ExitVent && !backstage)) {
        Reject(result,Reason::WrongStage);return true;
    }
    if((!backstage && !Ordinary(state)) || state==18U || state==78U || state==79U) {
        Reject(result,Reason::NativeBusy);return true;
    }
    result.reason=Reason::NoVent;
    if(!SelectPortal(c,command,result)){Reject(result,result.reason);return true;}
    if(!c.localOwner) {
        // The owning player runs the donor traversal. Running another path
        // on the Host can circle/stall and cancel a successful owner entry.
        result.phase=Phase::Started;result.reason=Reason::None;
        active={};active.used=true;active.remoteVent=true;active.context=c;
        active.receipt=result;active.began=GetTickCount64();TraceVent();return true;
    }
    auto* slot=FreeVentSlot();
    if(!slot || !StartVent(c,result,*slot)){Reject(result,Reason::NativeUnavailable);return true;}
    result.phase=Phase::Started;result.reason=Reason::None;
    active={};active.used=true;active.context=c;active.receipt=result;active.vent=slot;active.began=GetTickCount64();
    active.ventProgressAt=active.began;
    TraceVent();
    return true;
}
bool Poll(const ap::Context& c,ap::Frame& result) noexcept {
    if(!Same(c))return false;
    if(!Live(c)){Cancel(c,false);return false;}
    const auto now=GetTickCount64();std::uint8_t state{};bool backstage{};
    if(!Cycle(c.character,state,backstage))return false;
    if(active.remoteVent) {
        if(active.vent) {
            int presentation{};
            if(!StepVent(c,*active.vent,active.receipt,presentation)) {
                active.vent->quarantined=true;Terminal(result,Phase::Failed,Reason::Fault);Clear();return true;
            }
            if(presentation!=0 || active.cancelled) {
                bool done{};
                if(!FinishVent(c,*active.vent,presentation==2 || active.cancelled,done)) {
                    active.vent->quarantined=true;Terminal(result,Phase::Failed,Reason::Fault);Clear();return true;
                }
                if(done)active.vent=nullptr;
            }
            if(!Cycle(c.character,state,backstage))return false;
        }
        if(active.cancelled){if(!active.vent)Clear();return false;}
        TraceVent();
        const bool expected=ap::spatial::VentStageReady(state,backstage,active.receipt.action==Action::EnterVent,true);
        if(expected && !active.vent && active.ownerFinished) {
            Terminal(result,Phase::Completed,Reason::None);Clear();return true;
        }
        if(now-active.began>(active.receipt.action==Action::EnterVent?12000U:27000U)) {
            Terminal(result,Phase::Failed,Reason::Timeout);Cancel(c,true);return true;
        }
        return false;
    }
    if(active.cancelled) {
        if(active.vent){Cancel(c,true);return false;}
        const auto* actor=Find(c.character);
        if(!Protected(state) && !PairSourceBusy(c.character) && !PairReservedCharacter(c.character) &&
            (!active.entrySubmitted || (actor && UnownedCleanupAllowed(*actor) && now-active.submittedAt>=500U)) &&
            AttackQueryRetired(active.chooser))Clear();
        return false;
    }
    if(active.receipt.action==Action::Attack) {
        if(!c.host && !c.localOwner) {
            // Survivor/observer PC of a Host-piloted Alien. AISync's Client pair
            // replay is the executor here: it binds the local victim and its
            // death outcome. AvS 2026-11-25: this path called the pilot-only
            // PresentPair (stage 21), and the replay itself was stuck on an older
            // unentered pair, so the survivor never died and the Host showed
            // him dead/invisible. Fallback after 1.2 s: present the Host's kill
            // directly, only while no AISync pair holds these characters.
            if(state==78U && !active.entered) {
                active.entered=true;
                std::uintptr_t cycle{},object{};interactions::Definition seen{};std::uint32_t index{};
                if(Read(c.character+0x36CU,cycle) && cycle && Read(cycle+0x4CU,object) && object &&
                    Read(object+8U,seen.kind)) {
                    seen.object=object;RecordDefinition(seen);
                    if(Read(object+4U,index) && index<25U)active.receipt.selector=index;
                }
                TracePair(34U);
            }
            if(active.receipt.phase==Phase::Started && !active.observerWaitAt) {
                active.observerWaitAt=now;TraceAttack(31U,active.receipt.selector);
            }
            if(!active.entered && !active.observerFallback && active.observerWaitAt &&
                now-active.observerWaitAt>kObserverReplayWaitMs && !active.hostEnded && !Protected(state) &&
                !PairReservedCharacter(c.character) && !PairReservedCharacter(active.victim.character)) {
                active.observerFallback=true;active.presentedAt=now;
                const bool presented=PresentPair(c,active.victim,active.receipt.selector);
                TraceAttack(presented?32U:33U,active.receipt.selector);
                if(!presented){Clear();return false;}
                active.presented=true;
            }
            if(active.entered && !Protected(state)){Clear();return false;}
            if(active.hostEnded && !active.entered && !Protected(state) &&
                (!active.presented || now-active.presentedAt>1500U)) {
                TraceAttack(35U,active.receipt.selector);Clear();return false;
            }
            if(now-active.began>kClientAttackWaitMs && !Protected(state))Clear();
            return false;
        }
        if(!c.host) {
            if(!active.entered && state!=78U && active.victim.character)
                static_cast<void>(player_hiding::PreparePvPSearch(active.victim.character,c.scope.nativeGeneration));
            // The victim replica is a PlayerSync proxy. AISync shields it from
            // network locomotion only through the Host stream, whose intake is
            // fenced while this Alien is pair-busy: 506 re-entered Move/Idle on
            // it under the presented pair, it never acquired and the Alien
            // stayed in 78. Hold it from the Host's Started until Clear.
            if(active.victim.character && (active.presented || active.receipt.phase==Phase::Started))
                PublishPairedParticipantVictim(active.victim.character,now+kPairedParticipantVictimHoldMs);
            // Donor model (puppet.cpp): each PC runs the Alien's native melee.
            // The Host's retail kill is authoritative; this Alien only plays
            // the same pair locally on its victim replica, once the Host has
            // really entered it (Started carries the retail selector).
            if(!active.presented && !active.entered && active.receipt.phase==Phase::Started && state!=78U) {
                // Round 4: one refusal used to end it (stage 21). An AISync
                // replay of the same Host pair may hold the characters for a
                // moment; retry each frame for up to 1.5 s.
                if(!active.presentFirstAt)active.presentFirstAt=now;
                ++active.presentTries;
#ifndef AIMP_ALIEN_ACTION_TEST
                attackTelemetry.presentTries=active.presentTries;
#endif
                if(PresentPair(c,active.victim,active.receipt.selector)) {
                    active.presented=true;active.presentedAt=now;TraceAttack(20U,active.receipt.selector);
                } else if(now-active.presentFirstAt>1500U) {
                    TraceAttack(21U,active.receipt.selector);Clear();return false;
                }
            }
            if(state==78U && !active.entered){active.entered=true;TracePair(23U);}
            if((active.entered && !Protected(state)) || (active.hostEnded && !Protected(state)))Clear();
            else if(active.presented && !active.entered && now-active.presentedAt>1500U && !Protected(state)) {
                TraceAttack(22U,active.receipt.selector);Clear();
            }
            // The pilot's input is held while this receipt is active; never
            // freeze it for the generic 30s if nothing native happens.
            else if(now-active.began>kClientAttackWaitMs && !Protected(state))Clear();
            return false;
        }
        if(active.receipt.phase==Phase::Preparing) {
            if(active.directSubmitted) {
                // AvS 2026-11-25 round 5: the source entering 78 is not a kill.
                // Two of three kills first ran a pair whose victim never left
                // idle (victimState 1); the survivor PC saw the Host end it
                // (stage 35) and the pilot had to click again (then stage 30
                // while that pair was still ending). Started now waits for the
                // victim's own paired cycle (79); a pair that ends or idles
                // without it is re-submitted silently (guaranteed kill).
                if(state==78U && !active.sourceEntered) {
                    active.sourceEntered=true;active.sourceEnteredAt=now;TracePair(9U);
                }
                if(state==78U && VictimInPair(c.character,active.victim.character)) {
                    std::uintptr_t cycle{},definition{};std::uint32_t index{};
                    if(Read(c.character+0x36CU,cycle) && cycle && Read(cycle+0x4CU,definition) && definition &&
                        Read(definition+4U,index) && index<25U)active.receipt.selector=index;
                    TracePair(39U);
                    active.entered=true;result=active.receipt;result.phase=Phase::Started;result.reason=Reason::None;
                    ++result.resolutionRevision;active.receipt=result;return true;
                }
#ifndef AIMP_ALIEN_ACTION_TEST
                PublishPairedParticipantVictim(active.victim.character,now+kPairedParticipantVictimHoldMs);
#endif
                const bool notEntered=!active.sourceEntered && now-active.submittedAt>kDirectEntryMs;
                const bool notAcquired=active.sourceEntered && (state!=78U || now-active.sourceEnteredAt>kVictimAcquireMs);
                if(notEntered || notAcquired) {
                    // Let a running (victimless) pair end before submitting again.
                    if(Protected(state))return false;
                    TracePair(notEntered?30U:38U);
                    if(active.attackRetries<policy::kAttackRetries && VictimInLeash(c,active.victim) &&
                        now-active.began<policy::kAttackChaseMs) {
                        ++active.attackRetries;
#ifndef AIMP_ALIEN_ACTION_TEST
                        attackTelemetry.retries=active.attackRetries;
#endif
                        active.attackLatched=active.settling=active.directSubmitted=false;
                        active.sourceEntered=active.transitionRetried=false;active.lastQueryAt=0U;
                        return false;
                    }
                    Terminal(result,Phase::Rejected,Reason::NativeRefused);active.cancelled=true;return true;
                }
                if(!active.sourceEntered && !active.transitionRetried && now-active.submittedAt>kDirectRetryMs && !Protected(state)) {
                    active.transitionRetried=true;
                    const bool retried=HostRetryTransition(c,active.victim,active.receipt.selector);
#ifndef AIMP_ALIEN_ACTION_TEST
                    attackTelemetry.transitionRetried=retried?1U:2U;
#else
                    static_cast<void>(retried);
#endif
                }
                return false;
            }
            if(active.retailArmed) {
                // The retail brain (worker thread) runs ActionMeleeAttack on
                // the held target, the only path that completed a kill (453).
                if(state==78U) {
                    std::uintptr_t cycle{},definition{},pairVictim{};std::uint32_t index{};
                    if(Read(c.character+0x36CU,cycle) && cycle && Read(cycle+0x4CU,definition) && definition &&
                        Read(definition+4U,index) && index<25U)active.receipt.selector=index;
                    TracePair(Read(cycle+0x44U,pairVictim) && pairVictim==active.victim.character?9U:15U);
#ifndef AIMP_ALIEN_ACTION_TEST
                    // The pair is native now (PairSourceBusy fences it). Return the
                    // decision slice to the PvP fence at once: 462 kept it open
                    // while the action stayed Started, the pilot's donor movement
                    // resumed, and the Host crashed in 00712D70 again.
                    DisarmPvPBrain();
#endif
                    active.entered=true;result=active.receipt;result.phase=Phase::Started;result.reason=Reason::None;
                    ++result.resolutionRevision;active.receipt=result;return true;
                }
                // AvS 2026-11-24 (both armed attacks, stage 24 cycle 18): with
                // its decision slice unfenced the retail brain never ran
                // ATTACK_CORE (flagConsumed 0, meleeStarts 0) and took the Alien
                // into a vent instead; the pilot then saw an "automatic" vent
                // entry while the survivor still saw the Alien standing there.
                // Hand the brain back the moment it picks any vent/withdraw cycle.
                if(state!=78U && (backstage || Protected(state))) {
                    attackTelemetry.ventHijacks.fetch_add(1U,std::memory_order_relaxed);
                    ReleaseAttackTarget();TraceAttack(25U,state);
                    Terminal(result,Phase::Rejected,Reason::NativeRefused);active.cancelled=true;return true;
                }
#ifndef AIMP_ALIEN_ACTION_TEST
                const auto starts=gPvPMeleeStarts.load(std::memory_order_relaxed);
                attackTelemetry.meleeStarts=starts;attackTelemetry.meleeRefused=gPvPMeleeRefused.load(std::memory_order_relaxed);
                attackTelemetry.meleeType=gPvPMeleeLastType.load(std::memory_order_relaxed);
                {
                    std::array<std::uint8_t,7> bytes{};unsigned mask{};std::uint8_t flag{};
                    if(active.chooser && crash_guard::TryCopy(bytes.data(),reinterpret_cast<const void*>(active.chooser+8U),bytes.size()))
                        for(std::size_t i=0;i<bytes.size();++i)if(bytes[i])mask|=1U<<i;
                    attackTelemetry.availability=attackTelemetry.availability.load()|mask;
                    if(Read(active.attackAnimated+0x2B4U,flag))attackTelemetry.sourceFlag2B4=flag;
                    if(Read(active.victim.character+0x2B4U,flag))attackTelemetry.victimFlag2B4=flag;
                }
                // The retail BT never even tried ActionMeleeAttack: do not hold
                // the pilot for the whole window (459: frozen 10s for nothing).
                if(!starts && now-active.armedAt>kRetailNoStartMs) {
                    ReleaseAttackTarget();TraceAttack(24U,active.receipt.selector);
                    Terminal(result,Phase::Rejected,Reason::NativeRefused);active.cancelled=true;return true;
                }
#endif
                if(now-active.armedAt>kRetailAttackWindowMs) {
                    ReleaseAttackTarget();TraceAttack(14U,active.receipt.selector);
                    Terminal(result,Phase::Rejected,Reason::NativeRefused);active.cancelled=true;return true;
                }
                // Raise SHOULD_ATTACK so retail alien_behave enters ATTACK_CORE_BRANCH
                // (its own HasMeleeAttackAvailable query, then ActionMeleeAttack),
                // and keep the victim on both target records: retail target
                // selection overwrites a single write. The branch clears the flag
                // after each try, so a cleared flag proves the branch ran. Our own
                // query is not re-run: it would re-arm the branch's cover ray.
                attackTelemetry.armedPolls.fetch_add(1U,std::memory_order_relaxed);
#ifndef AIMP_ALIEN_ACTION_TEST
                if(DonorFor(c))static_cast<void>(StopDonor(c));
                static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(c.character));
                attackTelemetry.brainFaults=gPvPArmedBrainFaults.load(std::memory_order_relaxed);
#endif
                std::uint8_t should{};
                if(active.shouldAttackSet && Read(active.attackAnimated+kShouldAttackFlag,should) && !should) {
                    attackTelemetry.flagConsumed=++active.flagConsumed;TraceAttack(19U,active.flagConsumed);
                }
                if(!SetShouldAttack(true)) {
                    ReleaseAttackTarget();Terminal(result,Phase::Failed,Reason::NativeUnavailable);active.cancelled=true;return true;
                }
                active.shouldAttackSet=true;
                std::uintptr_t current{};
                if(active.heldTarget.record && Read(active.heldTarget.record+0x42CU,current) &&
                    current!=active.victim.character) {
                    attackTelemetry.retargets=++active.retargets;TraceAttack(16U,active.retargets);
                }
                if(!HoldAttackTarget(active.victim.character)) {
                    ReleaseAttackTarget();Terminal(result,Phase::Failed,Reason::NativeUnavailable);active.cancelled=true;return true;
                }
                if(!HoldBrainTarget(active.victim.character)) {
                    attackTelemetry.brainMissing.fetch_add(1U,std::memory_order_relaxed);TraceAttack(18U,active.receipt.selector);
                }
                return false;
            }
            // Round 5: the click proved reach and aim (SelectVictim). From here
            // the Alien chases (Chase hook) and only the leash ends it.
            if(!VictimInLeash(c,active.victim)) {
                TraceAttack(40U,active.receipt.selector);
                Terminal(result,Phase::Rejected,Reason::NoTarget);active.cancelled=true;return true;
            }
            // Pass 20 (avsfix19 locker kill): a survivor inside a locker/cabinet
            // is not killable by the paired grab. The retail search pair entered
            // 78 for one frame and dropped, the victim replica stayed in 79
            // (invisible after the exit) and the survivor PC froze its Alien copy.
            // Lockers get their own sniff/hold-breath kill (next pass).
            if(player_hiding::Hidden(active.victim.character)) {
                attackTelemetry.lockerRefused.fetch_add(1U,std::memory_order_relaxed);TraceAttack(42U);
                Terminal(result,Phase::Rejected,Reason::NotReady);active.cancelled=true;return true;
            }
            // One explicit query proves the native attack is available on THIS
            // victim; latch it instead of re-arming the cover ray every frame.
            if(!active.attackLatched && now-active.lastQueryAt>=kChaseQueryMs) {
                active.lastQueryAt=now;
                bool ready{};std::uint32_t selector{};
                if(!AttackAvailability(c,active.victim,active.chooser,ready,selector)) {
                    Terminal(result,Phase::Failed,Reason::NativeUnavailable);active.cancelled=true;return true;
                }
                if(ready){active.attackLatched=true;active.latchedAt=now;active.receipt.selector=selector;active.contactSince=0U;}
                else if(VictimInContact(c,active.victim)) {
                    // Pass 20 (avsfix19: 26 s chase 0.6 m from a survivor crouched
                    // under a closet, availability 0 the whole time): retail
                    // refuses every kill there (no room / cover ray). After 1.5 s
                    // in contact submit the standard front grab anyway; the pair
                    // alignment pulls the victim out to the Alien.
                    if(!active.contactSince)active.contactSince=now;
                    if(now-active.contactSince>=kForcedLatchMs) {
                        active.attackLatched=true;active.latchedAt=now;active.receipt.selector=kForcedSelector;
                        attackTelemetry.forcedLatches.fetch_add(1U,std::memory_order_relaxed);
                        TraceAttack(43U,kForcedSelector);
                    }
                } else active.contactSince=0U;
            }
            if(active.attackLatched) {
#ifndef AIMP_ALIEN_ACTION_TEST
                // Keep the survivor proxy out of network locomotion while the
                // grab aligns (the Client presenter does the same).
                PublishPairedParticipantVictim(active.victim.character,now+kPairedParticipantVictimHoldMs);
                // AvS 2026-11-25 (round 2 first attack, stage 30 at cycle 59):
                // submitted while the Alien still ran; its locomotion replaced
                // the cycle and the kill never entered (but stayed registered).
                // Stop it first and submit once idle, or after 450 ms anyway.
                if(!active.settling) {
                    active.settling=true;active.settleAt=now;
                    static_cast<void>(StopDonor(c));
                    static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(c.character));
                }
                if(state!=1U && now-active.settleAt<kDirectSettleMs)return false;
                attackTelemetry.settleMs=static_cast<unsigned>(now-active.settleAt);attackTelemetry.settleCycle=state;
                attackTelemetry.transitionRetried=0U;
#endif
                const auto submitted=HostDirectPair(c,active.victim,active.receipt.selector);
                TraceAttack(submitted==0U?7U:submitted,active.receipt.selector);
                // Round 5: a refusal while the previous attempt's pair is still
                // retiring (27 / 36 reserved) is retried like a victimless pair.
                if(submitted!=0U && submitted!=26U && active.attackRetries<policy::kAttackRetries &&
                    now-active.began<policy::kAttackChaseMs) {
                    ++active.attackRetries;
#ifndef AIMP_ALIEN_ACTION_TEST
                    attackTelemetry.retries=active.attackRetries;
#endif
                    active.attackLatched=active.settling=false;active.lastQueryAt=now;
                    return false;
                }
                if(submitted!=0U) {
                    Terminal(result,Phase::Rejected,submitted==29U?Reason::NativeBusy:Reason::NativeRefused);
                    active.cancelled=true;return true;
                }
                active.directSubmitted=true;active.entrySubmitted=true;active.submittedAt=now;
                return false;
            }
            // The retail-brain arming of 453-575 (HoldAttackTarget + SHOULD_ATTACK
            // + ArmPvPBrain) is retired: see HostDirectPair. Its armed-branch
            // handling above stays for a receipt armed by an older peer only.
            if(now-active.began>policy::kAttackChaseMs) {
                TraceAttack(41U,active.receipt.selector);
                Terminal(result,Phase::Rejected,Reason::NativeRefused);active.cancelled=true;return true;
            }
            return false;
        }
#ifndef AIMP_ALIEN_ACTION_TEST
        if(active.directSubmitted && Protected(state))
            PublishPairedParticipantVictim(active.victim.character,now+kPairedParticipantVictimHoldMs);
#endif
        if(active.entered && Protected(state) && !active.stallTraced && active.sourceEnteredAt &&
            now-active.sourceEnteredAt>kPairStallMs) {
            // Round 5 (kill from behind): victim grabbed, but no kill marker for
            // 12+ s; the Alien stayed in 78. Recorded for the next log.
            active.stallTraced=true;TracePair(37U);
        }
        if(now-active.lastPairTrace>=250U) {
            active.lastPairTrace=now;
            TracePair(now-active.began>4000U?12U:11U);
        }
        // Retail-owned pair: AISync does not capture worker registrations, so
        // completion is the source leaving the protected paired cycle.
        if(active.entered && !Protected(state)) {
            Terminal(result,Phase::Completed,Reason::None);Clear();return true;
        }
        if(now-active.began>policy::kActionTimeoutMs && !active.terminalSent) {
            Terminal(result,Phase::Failed,Reason::Timeout);active.cancelled=true;return true;
        }
        return false;
    }
    if(!active.vent && !active.moveArrived)return false;
    if(active.lastStep && now-active.lastStep<33U)return false;
    active.lastStep=now;
    int nativeResult=active.moveArrived?1:0;
    if(!active.moveArrived && !StepVent(c,*active.vent,active.receipt,nativeResult)) {
        active.vent->quarantined=true;Terminal(result,Phase::Failed,Reason::Fault);Clear();return true;
    }
    if(nativeResult==1 && !active.moveArrived)active.arrivedAt=now;
    if(nativeResult==1)active.moveArrived=true;
    TraceVent(static_cast<unsigned>(nativeResult));
    // Retail shuts down a finished node immediately, releasing its request
    // and vent-manager lock. Waiting for the stage change while retaining
    // that lock can stall the transition. Stage confirmation remains separate.
    if(active.moveArrived && active.vent) {
        bool done{};
        if(!FinishVent(c,*active.vent,false,done) || !done) {
            active.vent->quarantined=true;Terminal(result,Phase::Failed,Reason::Fault);Clear();return true;
        }
        active.vent=nullptr;
        if(!Cycle(c.character,state,backstage))return false;
    }
    if(state==18U || state==75U || state==77U)active.entered=true;
    const bool expected=ap::spatial::VentStageReady(state,backstage,active.receipt.action==Action::EnterVent);
    if(nativeResult==1 && expected) {
        Terminal(result,Phase::Completed,Reason::None);
        Clear();return true;
    }
    // Reuse the donor's bounded traversal windows. A close entry that circles
    // without getting nearer is cancelled instead of capturing input for 30s.
    policy::Point body{};
    if(!active.moveArrived && Body(c.character,body)) {
        const policy::Point goal=active.receipt.action==Action::EnterVent?
            policy::Point{active.receipt.x,active.receipt.y,active.receipt.z}:
            policy::Point{active.receipt.resolvedX,active.receipt.resolvedY,active.receipt.resolvedZ};
        const float distance=std::sqrt(policy::DistanceSquared(body,goal));
        if(distance+0.25F<active.ventBestDistance) {active.ventBestDistance=distance;active.ventProgressAt=now;}
        // Round 5: an entry whose path leads well away from the opening is a
        // wrong-side approach ("the Alien started running around"). Give the
        // pilot control back at once instead of after the 10 s window.
        if(active.receipt.action==Action::EnterVent && !Protected(state) && std::isfinite(active.ventBestDistance) &&
            distance>active.ventBestDistance+(RetreatEntry()?10.0F:2.5F)) {
            TraceVent(5U);
            Terminal(result,Phase::Failed,Reason::NoVent);
            Cancel(c,true);return true;
        }
    }
    // AvS 2026-11-24 (entry 7394): the node reported arrival at 5.7 s but no
    // passage (cycle 56, frontstage) followed; the Alien idled in place until
    // the player gave up at 9 s. Fail an arrived entry that has not started
    // its passage within 2.5 s so the pilot gets control back and can retry.
    if(active.moveArrived && active.arrivedAt && !expected && !Protected(state) && !backstage &&
        active.receipt.action==Action::EnterVent && now-active.arrivedAt>2500U) {
        TraceVent(3U);
        Terminal(result,Phase::Failed,Reason::Timeout);
        Cancel(c,true);return true;
    }
    const bool retreating=RetreatEntry();
    const auto traversalLimit=active.receipt.action==Action::EnterVent?(retreating?20000U:10000U):25000U;
    const bool stalled=active.receipt.action==Action::EnterVent && !Protected(state) &&
        !active.moveArrived && now-active.ventProgressAt>(retreating?8000U:5000U);
    if(nativeResult==2 || stalled || now-active.began>traversalLimit) {
        Terminal(result,Phase::Failed,nativeResult==2?Reason::NativeRefused:Reason::Timeout);
        Cancel(c,true);return true;
    }
    return false;
}
#ifndef AIMP_ALIEN_ACTION_TEST
bool ReplicatePose(const ap::Context& c,const ap::Frame& pose) noexcept {
    if((pose.flags&ap::OwnerMeasured)==0U)return false;
    // Round 4: with a Client pilot the Host's Alien is placed by owner poses.
    // Each one rewrites its transform while the Host settles and submits the
    // paired kill, which can replace the cycle before it enters 78 (same
    // failure as round 2's moving submit). Hold them until entry or the end.
    if(Same(c) && c.host && !c.localOwner && active.receipt.action==Action::Attack && !active.cancelled &&
        !active.entered && (active.settling || active.directSubmitted)) {
        attackTelemetry.posesHeld.fetch_add(1U,std::memory_order_relaxed);
        return false;
    }
    if(Same(c) && active.remoteVent) {
        const bool entering=active.receipt.action==Action::EnterVent;
        if((pose.flags&ap::NativeBusy)==0U && ((pose.flags&ap::Backstage)!=0U)==entering)
            active.ownerFinished=true;
        if(entering)return ReplicatePvPPose(c,pose);
        if((pose.flags&ap::VentAnimating)!=0U) {
            if(active.presentationStarted || active.cancelled)return false;
            // Trigger the real exit node once the owner is in its exit passage.
            // Start at the approved opening, avoiding a second hidden path.
            if(!Live(c) || !StopDonor(c) || !CancelRecovery(c,true))return false;
            auto* slot=FreeVentSlot();if(!slot)return false;
            const float opening[]{active.receipt.x,active.receipt.y,active.receipt.z};
            if(!character_direct_step_runtime::ApplyAIRecoveryTransform(c.character,opening,pose.yaw))return false;
            if(!StartVent(c,active.receipt,*slot))return false;
            active.vent=slot;active.presentationStarted=true;return true;
        }
        if(active.vent)return false; // the native passage owns placement
    }
    if((pose.flags&ap::VentAnimating)!=0U)return false;
    return ReplicatePvPPose(c,pose);
}
// Pass 15: stage repair for an observer of a Host-piloted Alien. A local
// (non-remote) action owns its own stage transitions.
bool ReconcileStage(const ap::Context& c,const ap::Frame& pose) noexcept {
    if(Same(c) && !active.remoteVent)return false;
    return ReconcilePvPReplicaStage(c,pose);
}
#endif
void RetirePending() noexcept {
#ifndef AIMP_ALIEN_ACTION_TEST
    if(gPvPLease.releasing)for(auto& d:donorMovements)if(d.used && Live(d.context))
        static_cast<void>(StopDonor(d.context));
#endif
    for(auto& slot:recoveries)if(slot.used && slot.cancelling && !slot.quarantined)
        static_cast<void>(CancelRecovery(slot.context,Live(slot.context)));
    if(!active.used || !active.cancelled)return;
    ap::Frame ignored{};static_cast<void>(Poll(active.context,ignored));
}
// AvS 2026-11-25 round 5 auto-chase (user: "when clicking it should follow the
// player and execute the right animation"). The pilot's own PC steers the
// Alien at the chosen survivor while the attack is pending and not yet being
// submitted; the Host keeps querying retail availability and submits the kill
// that fits the angle as soon as it is proven. On the Host of a Client-piloted
// Alien it returns nonzero so the replica keeps following the owner's poses.
int Chase(const ap::Context& c,float& yaw) noexcept {
    if(!Same(c) || active.cancelled || active.receipt.action!=Action::Attack || active.entered ||
        active.presented || active.settling || active.directSubmitted || active.retailArmed)return 0;
    if(active.receipt.phase!=Phase::Preparing)return 0;
    if(!c.localOwner)return c.host?2:0;
    policy::Point source{},target{};
    if(!PairIdentityLive(active.victim) || !LivingCharacter(active.victim.character) ||
        !Body(c.character,source) || !Body(active.victim.character,target))return 0;
    const float dx=target.x-source.x,dz=target.z-source.z,distance=std::hypot(dx,dz);
    if(distance>policy::kAttackLeash || std::abs(target.y-source.y)>policy::kAttackHeight)return 0;
    if(distance<policy::kAttackChaseStop)return 2;
    yaw=std::atan2(dx,dz);
#ifndef AIMP_ALIEN_ACTION_TEST
    attackTelemetry.chaseFrames.fetch_add(1U,std::memory_order_relaxed);
#endif
    return 1;
}
// ActionHooks::react: type 0 pumps (retires an expired reaction).
bool React(const ap::Context& c,std::uint32_t type,std::uint64_t windowMs) noexcept {
    if(type==0U)return PumpReaction(c);
    // Never during a vent/attack/pair this module owns.
    if(Same(c))return false;
    return PlayReaction(c,type,windowMs);
}
const ap::ActionHooks hooks{&Begin,&Poll,&Busy,&Cancel,&Recover,&CancelRecovery
#ifndef AIMP_ALIEN_ACTION_TEST
    ,&DriveDonor,&ReplicatePose,&InterruptVent,&Chase,&Crawl,&React,&ReconcileStage
#else
    ,nullptr,nullptr,&InterruptVent,&Chase,&Crawl,&React,nullptr
#endif
};
#undef AIMP_ACTION_TRY
#undef AIMP_ACTION_CATCH
} // namespace pvp_actions
const alien_pvp::ActionHooks& PvPActionHooks() noexcept {return pvp_actions::hooks;}
void RetirePvPActions() noexcept {pvp_actions::RetirePending();}
// Donor camera.cpp: the audio listener transform comes from FUN_00689230
// (lea eax,[ecx+50]; ret 4), called at 006BE31F with ecx = player+3F0. While
// the local pilot drives the Alien, return the Alien's transform instead, so
// its footsteps, roars and kill audio are heard from the Alien's position.
std::uintptr_t __fastcall ListenerTransformDetour(std::uintptr_t animation,void*,std::uint32_t) noexcept {
    const auto alien=alien_pvp_local::ListenerCharacter();
    return (alien?alien+0x3F0U:animation)+0x50U;
}
bool InstallPvPDonorVentHooks(HookManager& hooks,std::string& error) {
    if(!MatchRetailBytes(0x00289230U,{0x8D,0x41,0x50,0xC2,0x04,0x00})) {
        error="alien pilot audio listener: FUN_00689230 bytes mismatch";return false;
    }
    CallRedirectRequest listener;listener.name="alien_pilot_audio_listener";
    listener.callSite=reinterpret_cast<void*>(gBase+0x002BE31FU);
    listener.expectedTarget=reinterpret_cast<void*>(gBase+0x00289230U);
    listener.detour=reinterpret_cast<void*>(&ListenerTransformDetour);
    if(!hooks.InstallCallRedirect(listener,error))return false;
    HookRequest locomotion;locomotion.name="alien_donor_responsive_locomotion";
    locomotion.target=reinterpret_cast<void*>(gBase+0x0004D4F0U);
    locomotion.detour=reinterpret_cast<void*>(&pvp_actions::PilotLocomotionDetour);
    locomotion.patchLength=5U;locomotion.relocationSafePrefix=true;
    locomotion.expected=MaskedBytes{{0x8B,0x11,0x8B,0x41,0x04},"xxxxx"};
    locomotion.trampolineOut=&pvp_actions::pilotLocomotionOriginal;
    if(!hooks.Install(locomotion,error))return false;
    for(const auto exit:{false,true}) {
        HookRequest request;request.name=exit?"alien_donor_pick_exit":"alien_donor_pick_entry";
        request.target=reinterpret_cast<void*>(gBase+(exit?0x00013310U:0x000132F0U));
        request.detour=exit?reinterpret_cast<void*>(&pvp_actions::PickExitDetour):reinterpret_cast<void*>(&pvp_actions::PickEnterDetour);
        request.patchLength=8U;request.relocationSafePrefix=true;
        request.expected=MaskedBytes{{0x8B,0x44,0x24,0x04,0x8B,0x44,0x81,static_cast<std::uint8_t>(exit?0x48:0x40)},"xxxxxxxx"};
        request.trampolineOut=exit?&pvp_actions::ventExitOriginal:&pvp_actions::ventEnterOriginal;
        if(!hooks.Install(request,error))return false;
    }
    return true;
}
