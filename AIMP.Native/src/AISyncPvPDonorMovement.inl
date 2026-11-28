// AI-Playable-Alien control.cpp::MainTick / ActionMoveInDirection::Start.
// MIT (c) 2026 pirletik; see third_party/AI-Playable-Alien/LICENSE.
// Included in pvp_actions after Live(). Reuse AIMP's typed native request
// adapter for the donor's kind9 request and its strong Ref lifetime.
#ifndef AIMP_ALIEN_ACTION_TEST
struct DonorMovement final {
    std::unique_ptr<execution::NativeRequestAdapter> adapter{};
    ap::Context context{};
    transactions::Record command{};
    bool used{},fault{};
    ap::ResponsiveStop responsiveStop{};
    bool pilotMoving{};
    float pilotYaw{};
    // Stall watch (AvS 2026-11-25: "still stopping for no reason, getting
    // stuck on stuff" while every held frame was driven and Admitted).
    std::uint64_t stallAt{};float stallX{},stallZ{};float stallYaw{};
};
std::array<DonorMovement,4> donorMovements{};
struct DonorStallTelemetry final {
    std::atomic<std::uint64_t> stalls{},restarts{},requestsLost{},submits{},updates{};
    std::atomic<unsigned> lastCycle{};
    std::atomic<float> lastX{},lastZ{};
} donorStallTelemetry;
// 500 ms of driving with < 15 cm of travel. The retained kind9 request can
// finish (arrive/fail at an obstacle) while it is still "Admitted"; Update then
// edits a dead request and the Alien stands still. Restart it from scratch.
constexpr std::uint64_t kDonorStallMs=500U;constexpr float kDonorStallMetres=0.15F;
bool DonorPermission(void* opaque,const transactions::Record& r) noexcept {
    const auto& d=*static_cast<DonorMovement*>(opaque);
    const bool cleanup=r.opcode==transactions::Op::Cancel || r.opcode==transactions::Op::Release;
    return Live(d.context) && r.scope==d.command.scope && r.owner==d.command.owner &&
        r.commandId==d.command.commandId && r.nativeKind==9U &&
        (cleanup || (PvPLeaseScopeCurrent() && !gPvPLease.releasing));
}
bool DonorResolve(void* opaque,const transactions::OwnerKey& owner,execution::LocalActorBinding& actor) noexcept {
    const auto& d=*static_cast<DonorMovement*>(opaque);
    if(!Live(d.context) || owner!=d.command.owner)return false;
    actor={d.context.character,gPvPLease.runtimeId,true,gPvPLease.ticket.serial};return true;
}
DonorMovement* DonorFor(const ap::Context& c) noexcept {
    for(auto& d:donorMovements)
        if(d.used && d.context.character==c.character && d.context.scope==c.scope)return &d;
    return nullptr;
}
bool StopDonor(const ap::Context& c) noexcept {
    auto* d=DonorFor(c);if(!d)return true;
    if(!Live(c) || d->fault)return false; // retain strong Ref on unknown lifetime
    if(c.localOwner && d->pilotMoving) {
        std::array<float,3> position{};
        if(Read(c.character+0x70U,position))d->responsiveStop.Begin(GetTickCount64(),position[0],position[2]);
        d->pilotMoving=false;
        // Donor alienSnappy: request zero remaining distance before retiring.
        // This is only our retained kind9 request, never a vent/pair request.
        auto stationary=d->command;stationary.opcode=transactions::Op::Update;
        stationary.stopDistance=0.0F;++stationary.parameterRevision;++stationary.globalSeq;++stationary.ownerSeq;
        const auto outcome=d->adapter->Invoke(stationary);
        if(outcome==transactions::NativeOutcome::Unknown){d->fault=true;return false;}
        d->command=stationary;
    }
    auto cleanup=d->command;
    cleanup.flags=cleanup.mode=cleanup.gait=0U;
    cleanup.goalX=cleanup.goalY=cleanup.goalZ=cleanup.distance=cleanup.stopDistance=cleanup.aux=0.0F;
    cleanup.opcode=transactions::Op::Cancel;
    if(d->adapter->Invoke(cleanup)!=transactions::NativeOutcome::Admitted)return false;
    cleanup.opcode=transactions::Op::Release;
    if(d->adapter->Invoke(cleanup)!=transactions::NativeOutcome::Admitted)return false;
    d->used=false;return true;
}
bool DonorGait(const ap::Context& c,std::uint32_t gait) noexcept {
    // Exact writes performed by donor ActionMoveInDirection::Start; do not
    // force effective gait +198 or bypass the native transition animation.
    AIMP_ACTION_TRY {
        const auto loco=*reinterpret_cast<const std::uintptr_t*>(c.character+0x42A8U);
        if(!loco || gait>3U)return false;
        auto& requested=*reinterpret_cast<std::uint32_t*>(loco+0x194U);
        if(requested!=gait){requested=gait;*reinterpret_cast<std::uint32_t*>(loco+0x190U)=0U;}
        return true;
    } AIMP_ACTION_CATCH {return false;}
}
bool DriveDonor(const ap::Context& c,const ap::Frame* input) noexcept {
    if(!input || (input->forward==0.0F && input->strafe==0.0F))return StopDonor(c);
    if(!Live(c) || !PvPLeaseScopeCurrent() || gPvPLease.releasing || input->gait>3U ||
        !std::isfinite(input->yaw) || !std::isfinite(input->forward) || !std::isfinite(input->strafe) ||
        std::abs(input->forward)>1.0F || std::abs(input->strafe)>1.0F)return false;
    auto* d=DonorFor(c);
    if(!d) {
        for(auto& candidate:donorMovements)
            if(!candidate.used && !candidate.fault && (!candidate.adapter || candidate.adapter->ActiveCount()==0U)){d=&candidate;break;}
        if(!d)return false;
        d->context=c;d->command={};
        auto& r=d->command;
        std::memcpy(r.scope.session.data(),&c.scope.cookie,sizeof(c.scope.cookie));
        r.scope.authorityEpoch=c.scope.ownerPeer;r.scope.worldEpoch=c.scope.worldGeneration;
        r.scope.consumerEpoch=c.scope.nativeGeneration;
        r.owner={c.scope.entityA,c.scope.entityB,gPvPLease.runtimeId,1U,
            transactions::Capability::Movement,c.scope.incarnation};
        r.commandId=r.globalSeq=r.ownerSeq=1U;r.parameterRevision=1U;r.nativeKind=9U;
        if(!d->adapter)d->adapter=std::unique_ptr<execution::NativeRequestAdapter>(new(std::nothrow) execution::NativeRequestAdapter);
        if(!d->adapter || !d->adapter->Initialize(c.base,true,&DonorPermission,&DonorResolve,d))return false;
    }
    if(d->fault)return false;
    d->responsiveStop.Cancel();
    if(!DonorGait(c,input->gait))return false;
    if(c.localOwner && d->used && d->pilotMoving) {
        const auto now=GetTickCount64();
        std::array<float,3> body{};std::uintptr_t stallCycle{};std::uint8_t stallState{0xFFU};
        if(Read(c.character+0x36CU,stallCycle) && stallCycle)static_cast<void>(Read(stallCycle+0x1AU,stallState));
        if(Read(c.character+0x70U,body)) {
            const float wanted=ap::WrapYaw(input->yaw+std::atan2(input->strafe,input->forward));
            // A new heading is a fresh attempt; only a steady push can stall.
            // Start (55) / stop (59) cycles legitimately turn in place.
            if(!d->stallAt || stallState==55U || stallState==59U ||
                std::hypot(body[0]-d->stallX,body[2]-d->stallZ)>=kDonorStallMetres ||
                std::abs(ap::WrapYaw(wanted-d->stallYaw))>0.6F) {
                d->stallAt=now;d->stallX=body[0];d->stallZ=body[2];d->stallYaw=wanted;
            } else if(now-d->stallAt>=kDonorStallMs) {
                donorStallTelemetry.stalls.fetch_add(1U,std::memory_order_relaxed);
                donorStallTelemetry.lastCycle=stallState;
                donorStallTelemetry.lastX=body[0];donorStallTelemetry.lastZ=body[2];
                d->stallAt=0U;
                // Retire the wedged request. Next frame DonorFor() finds no
                // used slot and builds a brand-new kind9 request (fresh ids),
                // exactly like the first press after a stop.
                auto cleanup=d->command;
                cleanup.flags=cleanup.mode=cleanup.gait=0U;
                cleanup.goalX=cleanup.goalY=cleanup.goalZ=cleanup.distance=cleanup.stopDistance=cleanup.aux=0.0F;
                cleanup.opcode=transactions::Op::Cancel;
                if(d->adapter->Invoke(cleanup)==transactions::NativeOutcome::Admitted) {
                    cleanup.opcode=transactions::Op::Release;
                    if(d->adapter->Invoke(cleanup)==transactions::NativeOutcome::Admitted) {
                        d->used=false;d->pilotMoving=false;
                        donorStallTelemetry.restarts.fetch_add(1U,std::memory_order_relaxed);
                        return true;
                    }
                }
            }
        }
    }
    auto& r=d->command;
    r.opcode=d->used?transactions::Op::Update:transactions::Op::Submit;
    (d->used?donorStallTelemetry.updates:donorStallTelemetry.submits).fetch_add(1U,std::memory_order_relaxed);
    if(d->used){++r.parameterRevision;++r.globalSeq;++r.ownerSeq;}
    // Donor ReadInput worldYaw; native request +20 is pi/2 (not a speed),
    // +24 is refreshed to 10 metres and +34 rebases at the current body.
    r.aux=ap::WrapYaw(input->yaw+std::atan2(input->strafe,input->forward));
    r.distance=1.57079632679F;r.stopDistance=10.0F;
    d->used=true; // retain a partial Submit rather than resubmit it
    const auto outcome=d->adapter->Invoke(r);
    d->fault=outcome==transactions::NativeOutcome::Unknown;
    // A resource check can defer before the factory runs. It created no
    // retained request, so the next fresh input must retry Submit, not Update.
    if(!d->fault && d->adapter->ActiveCount()==0U) {
        if(r.opcode==transactions::Op::Update)donorStallTelemetry.requestsLost.fetch_add(1U,std::memory_order_relaxed);
        d->used=false;
    }
    if(c.localOwner && outcome==transactions::NativeOutcome::Admitted) {
        d->pilotMoving=true;d->pilotYaw=r.aux;
    }
    return outcome==transactions::NativeOutcome::Admitted;
}
// AvS round 10: retail reaction actions on the pilot's Alien. A kind-5 action
// request whose +1C is the action type (= the cycle number in our logs; the
// kill pair is 0x4E AT_ALIEN_MELEE_GRAPPLE, replayed the same way by AISync).
// 87 AT_AFFECTED_BY_FLAME_THROWER (retreat recoil), 80 AT_IMPACT_STUMBLE
// (flinch), 86 AT_MULTI_STAGE_STUNNED (shotgun stun). Retired after its window.
struct Reaction final {
    std::unique_ptr<execution::NativeRequestAdapter> adapter{};
    ap::Context context{};transactions::Record command{};
    bool used{},fault{};std::uint64_t until{};std::uint32_t nextId{1U};
} reaction{};
struct ReactionTelemetry final {
    std::atomic<std::uint64_t> submitted{},admitted{},refused{},unknown{},retired{};
    std::atomic<std::uint32_t> lastType{},lastOutcome{0xFFU};
} reactionTelemetry;
bool ReactionPermission(void* opaque,const transactions::Record& r) noexcept {
    const auto& d=*static_cast<Reaction*>(opaque);
    const bool cleanup=r.opcode==transactions::Op::Cancel || r.opcode==transactions::Op::Release;
    return Live(d.context) && r.scope==d.command.scope && r.owner==d.command.owner &&
        r.commandId==d.command.commandId && r.nativeKind==execution::kActionKind &&
        (cleanup || (PvPLeaseScopeCurrent() && !gPvPLease.releasing));
}
bool ReactionResolve(void* opaque,const transactions::OwnerKey& owner,execution::LocalActorBinding& actor) noexcept {
    const auto& d=*static_cast<Reaction*>(opaque);
    if(!Live(d.context) || owner!=d.command.owner)return false;
    actor={d.context.character,gPvPLease.runtimeId,true,gPvPLease.ticket.serial};return true;
}
bool RetireReaction(const ap::Context& c) noexcept {
    if(!reaction.used)return true;
    if(!Live(c) || reaction.fault)return false;
    auto cleanup=reaction.command;cleanup.opcode=transactions::Op::Cancel;
    if(reaction.adapter->Invoke(cleanup)!=transactions::NativeOutcome::Admitted)return false;
    cleanup.opcode=transactions::Op::Release;
    if(reaction.adapter->Invoke(cleanup)!=transactions::NativeOutcome::Admitted)return false;
    reaction.used=false;reactionTelemetry.retired.fetch_add(1U,std::memory_order_relaxed);return true;
}
bool PlayReaction(const ap::Context& c,std::uint32_t type,std::uint64_t windowMs) noexcept {
    if(!Live(c) || !c.localOwner || !PvPLeaseScopeCurrent() || gPvPLease.releasing || type>0xFFU ||
        type==execution::kActionNoState || reaction.fault)return false;
    if(!RetireReaction(c) || !StopDonor(c))return false;
    if(!reaction.adapter)reaction.adapter=std::unique_ptr<execution::NativeRequestAdapter>(new(std::nothrow) execution::NativeRequestAdapter);
    if(!reaction.adapter || (reaction.adapter->ActiveCount()==0U &&
        !reaction.adapter->Initialize(c.base,true,&ReactionPermission,&ReactionResolve,&reaction)))return false;
    reaction.context=c;reaction.command={};
    auto& r=reaction.command;
    std::memcpy(r.scope.session.data(),&c.scope.cookie,sizeof(c.scope.cookie));
    r.scope.authorityEpoch=c.scope.ownerPeer;r.scope.worldEpoch=c.scope.worldGeneration;
    r.scope.consumerEpoch=c.scope.nativeGeneration;
    r.owner={c.scope.entityA,c.scope.entityB,gPvPLease.runtimeId,1U,
        transactions::Capability::Movement,c.scope.incarnation};
    r.commandId=r.globalSeq=r.ownerSeq=reaction.nextId++;r.parameterRevision=1U;
    r.nativeKind=execution::kActionKind;r.mode=type;r.gait=0U;r.opcode=transactions::Op::Submit;
    reactionTelemetry.submitted.fetch_add(1U,std::memory_order_relaxed);
    reactionTelemetry.lastType.store(type,std::memory_order_relaxed);
    const auto outcome=reaction.adapter->Invoke(r);
    reactionTelemetry.lastOutcome.store(static_cast<std::uint32_t>(outcome),std::memory_order_relaxed);
    if(outcome==transactions::NativeOutcome::Unknown){reaction.fault=true;reactionTelemetry.unknown.fetch_add(1U,std::memory_order_relaxed);return false;}
    if(outcome!=transactions::NativeOutcome::Admitted || reaction.adapter->ActiveCount()==0U) {
        reactionTelemetry.refused.fetch_add(1U,std::memory_order_relaxed);return false;
    }
    reaction.used=true;reaction.until=GetTickCount64()+windowMs;
    reactionTelemetry.admitted.fetch_add(1U,std::memory_order_relaxed);
    return true;
}
// Called every pilot frame (ActionHooks::react with type 0).
bool PumpReaction(const ap::Context& c) noexcept {
    if(reaction.used && GetTickCount64()>=reaction.until)static_cast<void>(RetireReaction(c));
    return reaction.used;
}
// Keep SEH inside a scalar-only helper; the caller owns the native resource pin.
void PilotResponsiveWrite(DonorMovement& d,std::uintptr_t loco,std::uint8_t state) noexcept {
    const auto& c=d.context;
    AIMP_ACTION_TRY {
        if(d.used && d.pilotMoving) {
            // Donor ApplyFace: update velocity heading after native easing.
            *reinterpret_cast<float*>(loco+0x1CU)=d.pilotYaw;return;
        }
        std::array<float,2> target{};
        if(!d.responsiveStop.Target(GetTickCount64(),state,true,target))return;
        // Keep the retail stop/start state machine. Rebuilding its action as
        // idle can strand locomotion mode8 even with finished bits cleared.
        std::array<float,3> position{};float yaw{};
        if(Read(c.character+0x70U,position) && Read(c.character+0x174U,yaw) &&
            std::abs(position[0]-target[0])+std::abs(position[2]-target[1])>0.003F) {
            position[0]=target[0];position[2]=target[1];
            static_cast<void>(character_direct_step_runtime::ApplyAIRecoveryTransform(c.character,position.data(),yaw));
        }
    } AIMP_ACTION_CATCH {d.responsiveStop.Cancel();}
}
void PilotLocomotionAfter() noexcept {
    if(!EngineThread())return;
    for(auto& d:donorMovements) {
        const auto& c=d.context;
        if(!c.localOwner || !Live(c) || d.fault || !ap::ControlActive()) {d.responsiveStop.Cancel();continue;}
        std::uintptr_t cycle{},owner{},loco{};std::uint8_t state{},a{},b{};
        if(!Read(c.character+0x36CU,cycle) || !cycle || !Read(cycle+4U,owner) || owner!=c.character ||
            !Read(cycle+0x1AU,state) || !Read(c.character+0x42A8U,loco) || !loco ||
            !Read(c.character+0x2DAU,a) || !Read(c.character+0x2DBU,b)) {d.responsiveStop.Cancel();continue;}
        const bool eligible=!a && !b && !ap::VentPending() &&
            (state==1U || state==55U || state==56U || state==59U);
        if(!eligible){d.responsiveStop.Cancel();continue;}
        const character_direct_step_runtime::MovementResourceLease resources(c.character);
        if(!resources)continue;
        PilotResponsiveWrite(d,loco,state);
    }
}
void* pilotLocomotionOriginal{};
void __fastcall PilotLocomotionDetour(void* self,void*,std::uint32_t index,std::uint32_t delta) noexcept {
    reinterpret_cast<void(__thiscall*)(void*,std::uint32_t,std::uint32_t)>(pilotLocomotionOriginal)(self,index,delta);
    PilotLocomotionAfter();
}
void DonorDestroyed(std::uintptr_t character,std::uint32_t runtime,std::uint64_t serial,std::uint32_t generation) noexcept {
    for(auto& d:donorMovements) {
        if(!d.used || !d.adapter || d.context.character!=character || d.command.owner.birth!=runtime ||
            d.context.scope.incarnation!=serial || d.context.scope.nativeGeneration!=generation)continue;
        d.adapter->ConfirmActorDestroyed({character,runtime,true,serial});
        if(!d.adapter->BeginRetirement(d.command.scope))continue;
        const auto retired=d.adapter->DrainRetirement(
            [](void*,const execution::RetirementIdentity&) noexcept {return execution::Readiness::Rejected;},nullptr);
        if(retired.complete) {
            // Exact destructor return + zero retained Refs permits recycling
            // this adapter's storage; no calls target the destroyed actor.
            d.adapter.reset();d.used=d.fault=false;
        }
    }
}
#else
bool StopDonor(const ap::Context&) noexcept {return true;}
bool PlayReaction(const ap::Context&,std::uint32_t,std::uint64_t) noexcept {return false;}
bool PumpReaction(const ap::Context&) noexcept {return false;}
#endif
