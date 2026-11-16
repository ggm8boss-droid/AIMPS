#include "aimp/AISyncHostProducer.h"
#include "aimp/AISyncTransactionCapture.h"
#include "aimp/AISyncRouteRuntime.h"
#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/HookManager.h"
#include "aimp/CharacterResourceAdmission.h"
#include <Windows.h>
#include <cmath>
#include <cstring>

// Static provenance: 00d6 AI.exe, retained v260/native/006777e0,
// 0046d8f0,006838d0; phase2/native-commands/00683700,00688510;
// existing Turn/Stance native adapter profiles. No game execution is implied.
namespace aimp::ai_sync::host_producer {
namespace {
struct Ref { std::uintptr_t object{}, control{}; };
struct Lease {
    transactions::Scope scope{};
    transactions::OwnerKey owner{};
    Ref ref{};
    Ref companion{};
    std::uintptr_t character{};
    std::uint32_t runtime{};
    Command parameters{};
    std::uint32_t pendingTerminal{};
    // Native kind actually observed. Kind2 has no certified executable request
    // contract here and is never flattened into an ordinary kind1 Move.
    std::uint32_t sourceKind{};
    std::uint32_t markerRetainedBefore{};
    bool markerInitialized{};
    std::uint64_t order{};
    std::uintptr_t actionCycle{};
    std::uint32_t actionType{};
    std::uint8_t actionState{},actionSerial{};
    bool actionKnown{},actionRevoked{};
    bool used{}, submitted{}, unknown{}, primaryAccepted{}, companionAccepted{}, producerActive{}, admissionActive{};
    // Every Ref in this lease was acquired by a successful Strong(). A fenced
    // (unknown) lease of the live world whose Refs are all exactly held can be
    // retired like a live one; a world fence or a failed decrement cannot.
    bool refsHeld{}, worldFenced{};
};
struct MoveFrame {
    std::uintptr_t character{},state{},primary{};
    bool bound{};
};
std::array<MoveFrame,16> gMoveFrames{};
std::size_t gMoveDepth{},gMoveOverflow{};
std::uint64_t gAdmissionOrder{};
std::array<Lease,Producer::kCommands> gLeases{};
std::uintptr_t gBase{};
NativeContext gContext{};
NativeGuard gGuard{};
NativeMarkerCapture gMarkerCapture{};
std::atomic<void*> gMarkerInitializeOriginal{};
void* gMarkerInitializeTrampoline{};
NativeRetirementGuard gRetirementGuard{};
std::atomic<void*> gStatusOriginal{}, gTickOriginal{};
std::atomic<void*> gRetentionOriginal{};
std::atomic<NativeSchedulerObserver> gSchedulerObserver{};
std::atomic<void*> gAppendOriginal{}, gFrontOriginal{};
std::atomic<void*> gMoveOriginal{};
std::atomic<void*> gStopUpdateOriginal{};
void* gStatusTrampoline{};
void* gRetentionTrampoline{};
void* gTickTrampoline{};
void* gAppendTrampoline{};
void* gFrontTrampoline{};
void* gMoveTrampoline{};
void* gStopUpdateTrampoline{};
bool gInstalled{};
std::atomic<std::uint64_t> gNativeCallDepth{};
// Scheduler kinds without an executable contract. They are counted, never a
// reason to close the whole source: their Character keeps its other commands.
std::array<std::atomic<std::uint64_t>,16> gUncoveredKinds{};
// Every closure of the source records its exact site (source line) so a game
// session measures coverage instead of only reporting that it was lost.
std::atomic<std::uint64_t> gCoverageLosses{};
std::atomic<std::uint32_t> gLastLossSite{}, gFirstLossSite{};
std::atomic<std::uint64_t> gBootstrapCancels{};
std::mutex gMovementFailureGate;
MovementCaptureFailure gMovementFailure{};
void Lost(std::uint32_t site, execution::Recovery reason = execution::Recovery::NativeUnavailable) noexcept {
    if (EngineProducer().Bound()) {
        gCoverageLosses.fetch_add(1U,std::memory_order_relaxed);
        std::uint32_t none{};
        static_cast<void>(gFirstLossSite.compare_exchange_strong(none,site,std::memory_order_relaxed));
        gLastLossSite.store(site,std::memory_order_relaxed);
    }
    EngineProducer().LoseCoverage(reason);
}
struct NativeCallScope {
    NativeCallScope() noexcept {gNativeCallDepth.fetch_add(1,std::memory_order_acq_rel);}
    ~NativeCallScope(){gNativeCallDepth.fetch_sub(1,std::memory_order_release);}
};
template<class T> bool Read(std::uintptr_t at, T& value) noexcept {
    __try { value = *reinterpret_cast<const T*>(at); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void MovementFailure(const Lease& lease,const Command& parameters,std::uint32_t stage) noexcept {
    const std::lock_guard lock(gMovementFailureGate);
    if(gMovementFailure.stage)return;
    auto& f=gMovementFailure;f.stage=stage;f.kind=lease.sourceKind?lease.sourceKind:parameters.kind;
    f.request=lease.ref.object;f.bundle=parameters.movement;f.gait=parameters.movement.gait;
    static_cast<void>(Read(f.request,f.type));static_cast<void>(Read(f.request+0x10U,f.observedKind));
    static_cast<void>(Read(f.request+0xCU,f.status));
    if(f.kind==1U) {
        static_cast<void>(Read(f.request+0x68U,f.phase));static_cast<void>(Read(f.request+0xB8U,f.flags));
    }
}
bool Strong(std::uintptr_t request, Ref& ref) noexcept {
    __try {
        using Lock = Ref*(__thiscall*)(void*,Ref*);
        reinterpret_cast<Lock>(gBase + 0x6D8F0U)(reinterpret_cast<void*>(request + 4),&ref);
        return ref.object == request && ref.control != 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// 00283DB0: the retail scheduler Cancel used by the Client adapter retirement.
bool CancelNative(std::uintptr_t request) noexcept {
    __try { reinterpret_cast<void(__thiscall*)(void*)>(gBase + 0x283DB0U)(reinterpret_cast<void*>(request)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReleaseRef(Ref& ref) noexcept {
    // A Ref whose decrement already faulted is poisoned (control=0) and is
    // never decremented again.
    if (!ref.control) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(Ref*)>(gBase + 0xD770U)(&ref);
        ref = {}; return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { ref.control = 0; return false; }
}
bool Snapshot(std::uintptr_t request, std::uintptr_t& character, std::uint32_t& runtime, Command& p) noexcept {
    std::uintptr_t scheduler{}, type{};
    std::uint8_t retained{};
    if (!Read(request,type) || !Read(request + 0x10,p.kind) ||
        !Read(request + 0x14,scheduler) || !Read(scheduler,character) || !character ||
        scheduler != character + 0x2CD0U || !Read(character + 0x50,runtime) || !runtime ||
        !Read(request + 0x18,retained) || retained > 1) return false;
    p = Command{p.kind};
    p.scalarRetention=retained?3U:0U;
    if (scheduler_control::Supported(p.kind)) return type == gBase + scheduler_control::kTypeRva;
    if (p.kind == execution::kPlacementKind) {
        std::uint8_t branch{};
        p.point=true;
        if(type!=gBase+0xEB199CU||!Read(request+0x1C,p.x)||!Read(request+0x20,p.y)||
            !Read(request+0x24,p.z)||!Read(request+0x28,p.heading)||!Read(request+0x2C,branch)||branch>1U)return false;
        p.mode=branch;
        return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.heading)&&
            std::abs(p.x)<=100000.0F&&std::abs(p.y)<=100000.0F&&std::abs(p.z)<=100000.0F&&std::abs(p.heading)<=6.4F;
    }
    if (p.kind == 7 && type == gBase + 0xEB19BCU) return Read(request + 0x1C,p.mode);
    if (p.kind == execution::kActionKind && type == gBase + 0xEB198CU) {
        // 0067EC30 layout: +1C cycle state, +20/+21/+22 bytes (00713166..184).
        std::uint8_t interrupt{}, b21{}, b22{};
        if (!Read(request + 0x1C,p.mode) || p.mode > 0xFFU || p.mode == execution::kActionNoState ||
            !Read(request + 0x20,interrupt) || !Read(request + 0x21,b21) || !Read(request + 0x22,b22) ||
            interrupt > 1 || b21 > 1 || b22 > 1) return false;
        p.actionFlags = interrupt | (static_cast<std::uint32_t>(b21) << 1U) | (static_cast<std::uint32_t>(b22) << 2U);
        return true;
    }
    if (p.kind == execution::kDirectedKind && type == gBase + 0xEB19ACU) {
        // 00686860 writes these three values; +28 origin is Host-local.
        return Read(request + 0x1C,p.heading) && Read(request + 0x20,p.tolerance) &&
            Read(request + 0x24,p.x) && std::isfinite(p.heading) && std::abs(p.heading) <= 6.4F &&
            std::isfinite(p.tolerance) && p.tolerance >= 0.0F && p.tolerance <= 6.4F &&
            std::isfinite(p.x) && p.x >= 0.0F && p.x <= 1000.0F;
    }
    if (p.kind != 4 || type != gBase + 0xEB19CCU) return false;
    std::uint32_t mode{}, style{}; std::uint8_t aim{};
    if (!Read(request + 0x30,mode) || mode > 1 || !Read(request + 0x34,style) || style > 1 ||
        !Read(request + 0x38,aim) || aim > 1 || !Read(request + 0x2C,p.tolerance)) return false;
    p.mode = style | (static_cast<std::uint32_t>(aim) << 1U); p.point = mode == 0;
    if (p.point) return Read(request + 0x1C,p.x) && Read(request + 0x20,p.y) && Read(request + 0x24,p.z);
    return Read(request + 0x28,p.heading);
}
bool SnapshotMovement(std::uintptr_t request,std::uint32_t kind,movement::Parameters& p,bool pendingGoal) noexcept {
    std::uintptr_t type{};std::uint32_t actual{},phase{};std::uint8_t retained{},f90{},f91{};
    const auto block=request+(kind==1?0x28U:0x1CU);
    if(!Read(request,type)||type!=gBase+(kind==1?0xEB1A70U:(kind==2?0xEB1A60U:0xEB1A50U))||
        !Read(request+0x10,actual)||actual!=kind||!Read(request+0x18,retained)||
        !Read(block+0x50,p.style)||!Read(block+0x2C,p.stoppingDistance)||!Read(block+0x30,p.stopDistance)||
        !Read(block+0x90,f90)||!Read(block+0x91,f91))return false;
    // 00461280 also consumes bit2 when building the native query mask.
    // Bit6 remains the local pending-goal cursor, outside authored flags.
    p.retained=retained;p.flags90=f90&0x3FU;p.flags91=f91&0x31U;
    auto goal=block;
    if(pendingGoal&&kind==1&&(f90&0x40)&&Read(request+0x68,phase)&&phase==3)goal=request+0x34;
    for(std::size_t i=0;i<3;++i)if(!Read(goal+i*4,p.goal[i]))return false;
    for(std::size_t i=0;i<9;++i)if(!Read(block+0x58+i*4,p.query[i]))return false;
    return true;
}
std::uint32_t SourceKind(const Lease& lease) noexcept { return lease.sourceKind ? lease.sourceKind : lease.parameters.kind; }
bool SnapshotGait(std::uintptr_t character,std::uint32_t& gait) noexcept {
    std::uintptr_t movement{};return Read(character+0x42A8,movement)&&movement&&Read(movement+0x194,gait)&&gait<=3;
}
Lease* Find(std::uintptr_t request) noexcept {
    // A null request never names a lease: every lease without a companion has
    // companion.object==0, so Find(0) used to return an arbitrary live lease.
    if (!request) return nullptr;
    for (auto& item : gLeases) if (item.used && (item.ref.object == request || item.companion.object == request)) return &item;
    return nullptr;
}
bool Live(Lease& lease) noexcept {
    transactions::OwnerKey owner{}; transactions::Scope scope{};
    std::uint32_t runtime{}, strong{};
    return gGuard && gGuard() && HostScope(scope) && scope == lease.scope &&
        gContext(lease.character,owner) && owner == lease.owner &&
        Read(lease.character + 0x50,runtime) && runtime == lease.runtime &&
        Read(lease.ref.control + 4,strong) && strong != 0 &&
        (!lease.companion.object||(Read(lease.companion.control+4,strong)&&strong!=0));
}
bool RecoverableFence(const Lease& lease) noexcept {
    return lease.unknown && lease.refsHeld && !lease.worldFenced && lease.ref.object && lease.ref.control &&
        (!lease.companion.object || lease.companion.control);
}
Lease* Capture(std::uintptr_t request) noexcept {
    if (auto* lease = Find(request)) return lease;
    if (!gInstalled || !gGuard || !gGuard() || !EngineProducer().Bound()) return nullptr;
    // Qualify the owner first: a foreign Character's request never concerns
    // the source, whatever its kind.
    transactions::Scope scope{}; transactions::OwnerKey owner{};
    std::uintptr_t character{}; std::uint32_t runtime{}; Command parameters{};
    std::uintptr_t scheduler{}; std::uint32_t kind{};
    if (!Read(request + 0x14,scheduler) || !Read(scheduler,character) ||
        scheduler != character + 0x2CD0U || !gContext(character,owner)) return nullptr;
    // StatusDetour and ObserveNativeAdmission can capture a request even if
    // BeforeNativeAdmission declined it. Suppress all ordinary navigation at
    // this central fallback too, not just the pre-admission hook. Otherwise
    // status1/2 recreates a command lease and restarts Client pathfinding.
    if(!Read(request+0x10,kind) ||
       kind==1U||kind==2U||kind==3U||kind==4U||kind==6U||
       kind==7U||kind==8U||kind==9U||kind==11U)return nullptr;
    if(kind==execution::kPlacementKind&&(!execution::PlacementDedupReady()||
        (EngineProducer().Profiles()&execution::kProfilePlacement)==0)) {
        gUncoveredKinds[kind].fetch_add(1U,std::memory_order_relaxed);return nullptr;
    }
    if(kind==execution::kPlacementKind) {
        std::uint8_t branch{};
        if(!Read(request+0x2C,branch)){Lost(__LINE__);return nullptr;}
        // An unsupported semantic variant is an uncovered command, not loss
        // of the entire source. Never normalize its native byte to a bool.
        if(branch>1U){gUncoveredKinds[kind].fetch_add(1U,std::memory_order_relaxed);return nullptr;}
    }
    if (!Read(request + 0x10,kind) || execution::ProfileForKind(kind) == 0) {
        gUncoveredKinds[kind & 15U].fetch_add(1U,std::memory_order_relaxed);
        return nullptr;
    }
    if (!Snapshot(request,character,runtime,parameters) || !HostScope(scope)) {
        Lost(__LINE__); return nullptr;
    }
    Lease* lease{};
    for (auto& candidate : gLeases) if (!candidate.used) { lease = &candidate; break; }
    if (!lease) { Lost(__LINE__,execution::Recovery::Capacity); return nullptr; }
    *lease = {}; lease->used = true; lease->scope = scope; lease->owner = owner;
    if(gAdmissionOrder==UINT64_MAX){lease->unknown=true;Lost(__LINE__,execution::Recovery::Capacity);return nullptr;}
    lease->order=++gAdmissionOrder;
    lease->character = character; lease->runtime = runtime; lease->parameters = parameters;
    if (!Strong(request,lease->ref)) { lease->unknown = true; Lost(__LINE__); return nullptr; }
    lease->refsHeld = true;
    if (!Live(*lease)) {
        lease->unknown = true; Lost(__LINE__); return nullptr;
    }
    return lease;
}
bool FinishMovement(Lease& lease) noexcept {
    if(lease.unknown||!lease.submitted||!Live(lease))return false;
    std::uint32_t primary{},companion=3;
    if(!Read(lease.ref.object+0xC,primary)||(lease.companion.object&&!Read(lease.companion.object+0xC,companion))) {
        lease.unknown=true;Lost(__LINE__);return false;
    }
    if(primary<3||primary>6||companion<3||companion>6)return false;
    const auto request=lease.ref.object;
    const auto result=primary==3?EngineProducer().Complete(request):EngineProducer().Cancel(request);
    if(result!=SourceResult::Ok){lease.unknown=true;return false;}
    // Drop independent observer refs only. Never cancel Host execution here.
    if(lease.companion.object) {
        lease.unknown=true;
        if(!ReleaseRef(lease.companion)){Lost(__LINE__);return false;}
        if(!Live(lease)){Lost(__LINE__);return false;}
    }
    lease.unknown=true;
    if(!ReleaseRef(lease.ref)){Lost(__LINE__);return false;}
    lease={};
    if(EngineProducer().Release(request)!=SourceResult::Ok)Lost(__LINE__);
    return true;
}
void FinalizeMovement(Lease& lease) noexcept {
    if(lease.admissionActive||(lease.parameters.kind==2&&!lease.markerInitialized))return;
    if(lease.unknown||!Live(lease)){Lost(__LINE__);return;}
    if(!lease.submitted) {
        if(!lease.primaryAccepted) {
            // No admitted Host execution exists. Consume only our provisional
            // observer Ref; a partial decrement remains fenced, never retried.
            lease.unknown=true;
            if(lease.companion.object||!ReleaseRef(lease.ref)){Lost(__LINE__);return;}
            lease={};return;
        }
        if(lease.companion.object) {
            movement::Parameters after{};std::uint8_t flags{};
            if(!lease.companionAccepted||!SnapshotMovement(lease.companion.object,3,after,false)||
                !Read(lease.companion.object+0xB8,flags)||(flags&7U)!=lease.parameters.movement.companionFlags) {
                lease.unknown=true;Lost(__LINE__);return;
            }
            lease.parameters.movement.companionRetainedAfter=after.retained;
            after.retained=lease.parameters.movement.companion.retained;
            if(!movement::Same(after,lease.parameters.movement.companion)) {
                lease.unknown=true;Lost(__LINE__);return;
            }
        }
        if(!SnapshotGait(lease.character,lease.parameters.movement.gait)||
            !SnapshotMovement(lease.ref.object,SourceKind(lease),lease.parameters.movement.after,true)) {
            lease.unknown=true;Lost(__LINE__);return;
        }
        // Admission may initialize/progress local path state synchronously.
        // The authoritative initial goal remains the captured producer input.
        if(lease.parameters.kind==2) {
            const auto retained=lease.parameters.movement.after.retained;
            lease.parameters.movement.after=lease.parameters.movement.before;
            lease.parameters.movement.before.retained=lease.markerRetainedBefore;
            lease.parameters.movement.after.retained=retained;
        }
        const auto pendingGoal=lease.parameters.movement.after.goal;
        lease.parameters.movement.after.goal=lease.parameters.movement.before.goal;
        if(lease.parameters.kind==3) {
            const auto retained=lease.parameters.movement.after.retained;
            lease.parameters.movement.after=lease.parameters.movement.before;
            lease.parameters.movement.after.retained=retained;
        }
        if(EngineProducer().Admit(lease.owner,lease.ref.object,lease.parameters)!=SourceResult::Ok) {
            if(!movement::Validate(lease.parameters.movement))MovementFailure(lease,lease.parameters,5U);
            lease.unknown=true;Lost(__LINE__);return;
        }
        lease.submitted=true;
        if(SourceKind(lease)==1&&pendingGoal!=lease.parameters.movement.after.goal) {
            std::uint8_t flags{};std::uint32_t phase{};
            if(Read(lease.ref.object+0xB8,flags)&&Read(lease.ref.object+0x68,phase)&&(flags&0x40)&&phase==3) {
                auto parameters=lease.parameters;parameters.movement.after.goal=pendingGoal;
                if(EngineProducer().Update(lease.ref.object,parameters)!=SourceResult::Ok){
                    if(!movement::Validate(parameters.movement))MovementFailure(lease,parameters,6U);
                    lease.unknown=true;return;
                }
                lease.parameters=parameters;
            }
        }
    } else {
        if(lease.parameters.kind==2)return; // Marker updates have a dedicated immutable-geometry path.
        auto parameters=lease.parameters;std::uint8_t flags{};std::uint32_t phase{};
        std::uint32_t failed{};
        if(!SnapshotGait(lease.character,parameters.movement.gait))failed=1U;
        else if(!SnapshotMovement(lease.ref.object,SourceKind(lease),parameters.movement.after,true))failed=2U;
        else if(SourceKind(lease)==1&&!Read(lease.ref.object+0xB8,flags))failed=3U;
        else if(SourceKind(lease)==1&&!Read(lease.ref.object+0x68,phase))failed=4U;
        if(failed) {
            MovementFailure(lease,parameters,failed);
            lease.unknown=true;Lost(__LINE__);return;
        }
        // Only an accepted pending-goal write is a new producer destination.
        // The native cursor/projected goal cannot replace that causal input.
        if(!(flags&0x40)||phase!=3)parameters.movement.after.goal=lease.parameters.movement.after.goal;
        if(EngineProducer().Update(lease.ref.object,parameters)!=SourceResult::Ok) {
            if(!movement::Validate(parameters.movement))MovementFailure(lease,parameters,6U);
            lease.unknown=true;Lost(__LINE__);return;
        }
        lease.parameters=parameters;
    }
    // The full producer bundle and its latest parameter revision must precede
    // any route packet in the shared causal outbox. A synchronous native path
    // install can only be recovered now, after Admit established its identity.
    Ref movement{};
    if(!Live(lease)||!Read(lease.character+0x42A8U,movement)||!movement.object||!movement.control) {
        lease.unknown=true;Lost(__LINE__);return;
    }
    route_runtime::ObserveHostNavigation(movement.object+0xC4U,
        lease.ref.object+(SourceKind(lease)==1?0x28U:0x1CU));
    if(!Live(lease)){lease.unknown=true;Lost(__LINE__);return;}
    if(lease.companion.object) {
        // Observe can publish through callbacks. Qualify the second part from
        // the current owning lease and current M, independently of the first.
        if(!Read(lease.character+0x42A8U,movement)||!movement.object||!movement.control) {
            lease.unknown=true;Lost(__LINE__);return;
        }
        route_runtime::ObserveHostNavigation(movement.object+0xC4U,lease.companion.object+0x1CU);
    }
    static_cast<void>(FinishMovement(lease));
}
void ObserveStatus(std::uintptr_t request, std::uintptr_t character, std::uint32_t next) noexcept {
    if (!gGuard || !gGuard() || !EngineProducer().Bound()) return;
    auto* lease = Find(request);
    if (!lease && (next == 1 || next == 2)) lease = Capture(request);
    if (!lease) return;
    if (lease->unknown || character != lease->character || !Live(*lease)) { Lost(__LINE__); return; }
    if((lease->parameters.kind==1||lease->parameters.kind==2||lease->parameters.kind==3)) {
        if(lease->submitted)static_cast<void>(FinishMovement(*lease));
        return;
    }
    if (next == 3 || next == 4 || next == 5 || next == 6) {
        if (!lease->submitted) { lease->pendingTerminal = next; return; }
        const auto result = next == 3 ? EngineProducer().Complete(request) : EngineProducer().Cancel(request);
        if (result != SourceResult::Ok) {
            lease->unknown = true; return;
        }
        // This callback holds a retail strong Ref. Drop only our independent
        // observer Ref after original status dispatch, without reading after.
        lease->unknown = true; // Before the destructor-capable decrement.
        if (!ReleaseRef(lease->ref)) { Lost(__LINE__); return; }
        *lease = {};
        // The wire Release records the observed local Ref boundary. A failed
        // decrement must never publish successful authoritative cleanup.
        if (EngineProducer().Release(request) != SourceResult::Ok) Lost(__LINE__);
    }
}
void FinalizeScalar(Lease& lease) noexcept {
    if(lease.unknown||lease.admissionActive||!Live(lease))return;
    if(!lease.primaryAccepted) {
        lease.unknown=true;if(!ReleaseRef(lease.ref)){Lost(__LINE__);return;}lease={};return;
    }
    std::uintptr_t character{};std::uint32_t runtime{},status{};Command after{};
    if(!Snapshot(lease.ref.object,character,runtime,after)||character!=lease.character||runtime!=lease.runtime||
        !Read(lease.ref.object+0xC,status)){lease.unknown=true;Lost(__LINE__);return;}
    after.admission=lease.parameters.admission;
    after.scalarRetention=(lease.parameters.scalarRetention&1)|(after.scalarRetention&2);
    lease.parameters.scalarRetention=after.scalarRetention;
    if(EngineProducer().Admit(lease.owner,lease.ref.object,lease.parameters)!=SourceResult::Ok){lease.unknown=true;return;}
    lease.submitted=true;
    if(after!=lease.parameters) {
        if(EngineProducer().Update(lease.ref.object,after)!=SourceResult::Ok){lease.unknown=true;return;}
        lease.parameters=after;
    }
    // The retention handler can already have changed terminal3/5 into6.
    // Keep the exact pre-retention disposition captured at that boundary.
    const auto terminal=lease.pendingTerminal?lease.pendingTerminal:status;
    if(terminal>=3&&terminal<=6)ObserveStatus(lease.ref.object,lease.character,terminal);
}
void FinalizePending() noexcept {
    for(std::size_t budget=0;budget<gLeases.size();++budget) {
        Lease* first{};
        for(auto& lease:gLeases)if(lease.used&&!lease.submitted&&!lease.unknown&&(!first||lease.order<first->order))first=&lease;
        if(!first||first->admissionActive||first->producerActive)return;
        if(first->parameters.kind==2&&!first->markerInitialized) {
            std::uint32_t status{};
            if(!Read(first->ref.object+0xC,status)){first->unknown=true;Lost(__LINE__);return;}
            if(!first->primaryAccepted||(status>=3&&status<=6)) {
                first->unknown=true;if(!ReleaseRef(first->ref)){Lost(__LINE__);return;}*first={};continue;
            }
            return; // Geometry is captured when the retail initializer actually runs.
        }
        if(first->parameters.kind==1||first->parameters.kind==2||first->parameters.kind==3)FinalizeMovement(*first);else FinalizeScalar(*first);
        if(first->used&&!first->submitted)return;
    }
}
// 006777E0: ECX=request, Character,old,new stack words; RET 0C.
// Preserve 00776120 dispatch when new==2 exactly once before observation.
void __fastcall StatusDetour(void* request, void*, std::uintptr_t character,
    std::uint32_t previous, std::uint32_t next) noexcept {
    const NativeCallScope frame;
    using Status = void(__thiscall*)(void*,std::uintptr_t,std::uint32_t,std::uint32_t);
    const auto original = reinterpret_cast<Status>(gStatusOriginal.load(std::memory_order_acquire));
    if (!original) return;
    // Pin before original's Character callback can reenter or replace the
    // request. A provisional Ref is not an accepted Host command.
    if (next == 1 || next == 2) static_cast<void>(Capture(reinterpret_cast<std::uintptr_t>(request)));
    original(request,character,previous,next);
    ObserveStatus(reinterpret_cast<std::uintptr_t>(request),character,next);
}
// 00683470: ECX=scheduler, strong Ref* on stack, RET4. The caller has already
// removed the request and assigned3/4/5. The original may overwrite it with6.
// Unlike the status virtual, this boundary also sees the base-only controls.
void __fastcall RetentionDetour(void* scheduler,void*,const Ref* reference) noexcept {
    const NativeCallScope frame;
    const auto original=reinterpret_cast<void(__thiscall*)(void*,const Ref*)>(gRetentionOriginal.load(std::memory_order_acquire));
    if(!original)return;
    std::uintptr_t request{};
    if(Read(reinterpret_cast<std::uintptr_t>(reference),request))
        BeforeNativeRetention(reinterpret_cast<std::uintptr_t>(scheduler),request);
    original(scheduler,reference);
}
std::uint8_t __fastcall AppendDetour(void* request, void*) noexcept {
    const NativeCallScope frame;
    using Admission = std::uint8_t(__thiscall*)(void*);
    const auto original = reinterpret_cast<Admission>(gAppendOriginal.load(std::memory_order_acquire));
    if (!original) return 0;
    BeforeNativeAdmission(reinterpret_cast<std::uintptr_t>(request),true);
    const auto accepted = original(request);
    ObserveNativeAdmission(reinterpret_cast<std::uintptr_t>(request),accepted != 0); return accepted;
}
std::uint8_t __fastcall FrontDetour(void* request, void*) noexcept {
    const NativeCallScope frame;
    using Admission = std::uint8_t(__thiscall*)(void*);
    const auto original = reinterpret_cast<Admission>(gFrontOriginal.load(std::memory_order_acquire));
    if (!original) return 0;
    BeforeNativeAdmission(reinterpret_cast<std::uintptr_t>(request),2);
    const auto accepted = original(request);
    ObserveNativeAdmission(reinterpret_cast<std::uintptr_t>(request),accepted != 0); return accepted;
}
// Keep the exact producer context through retirement and successor callbacks.
// This leaf owns no RAII state; the finally only restores value-only observer
// context, and never catches or suppresses an exception from the native call.
void CallObservedTick(void* scheduler,void(__thiscall* original)(void*),NativeSchedulerObserver observer) {
    if(observer)observer(reinterpret_cast<std::uintptr_t>(scheduler),true);
    __try {original(scheduler);}
    __finally {if(observer)observer(reinterpret_cast<std::uintptr_t>(scheduler),false);}
}
void __fastcall TickDetour(void* scheduler, void*) noexcept {
    const NativeCallScope frame;
    using Tick = void(__thiscall*)(void*);
    const auto original = reinterpret_cast<Tick>(gTickOriginal.load(std::memory_order_acquire));
    if (!original) return;
    // Capture effective scalar mutations before the scheduler consumes them.
    // Unconsumed intermediate writes are not independent native commands.
    PumpNative(); CallObservedTick(scheduler,original,gGuard&&gGuard()?gSchedulerObserver.load(std::memory_order_acquire):nullptr);
    ObserveNativeActionExecution(reinterpret_cast<std::uintptr_t>(scheduler));
}
using MoveFn=std::uint32_t(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,
    std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
std::uint32_t __fastcall MoveDetour(void* action,void*,std::uintptr_t character,std::uintptr_t state,
    std::uint32_t p4,std::uint32_t p5,std::uint32_t p6,std::uint32_t p7,std::uint32_t p8) noexcept {
    const auto original=reinterpret_cast<MoveFn>(gMoveOriginal.load(std::memory_order_acquire));if(!original)return 0;
    BeginNativeMove(character,state);const auto result=original(action,character,state,p4,p5,p6,p7,p8);EndNativeMove();return result;
}
void __fastcall StopUpdateDetour(void* request,void*,float distance,std::uint32_t mode) noexcept {
    const NativeCallScope frame;
    using Setter=void(__thiscall*)(void*,float,std::uint32_t);
    const auto original=reinterpret_cast<Setter>(gStopUpdateOriginal.load(std::memory_order_acquire));if(!original)return;
    original(request,distance,mode);ObserveNativeMutation(reinterpret_cast<std::uintptr_t>(request));
}
void __fastcall GoalUpdateDetour(void* request,void*,const void* goal) noexcept {
    const NativeCallScope frame;
    reinterpret_cast<void(__thiscall*)(void*,const void*)>(gBase+0x2C7550)(request,goal);
    ObserveNativeMutation(reinterpret_cast<std::uintptr_t>(request));
}
// Exact 00681810 entry, ECX=request, Character stack word, RET4. The
// original Host initializer stays native. An owned Client uses captured Host
// geometry before native insertion, so local player context cannot author it.
void __fastcall MarkerInitializeDetour(void* request,void*,std::uintptr_t character) noexcept {
    const NativeCallScope frame;
    const auto ownership=execution_runtime::InitializeMarker(reinterpret_cast<std::uintptr_t>(request),character);
    if(ownership!=execution::MarkerInitialization::Unowned)return;
    const auto original=reinterpret_cast<void(__thiscall*)(void*,std::uintptr_t)>(gMarkerInitializeOriginal.load(std::memory_order_acquire));
    if(original)original(request,character);
}
// This wrapper is only the CALL0068192F inside the genuine kind2 initializer.
// Snapshot before0044D190 changes B.state/list links; no broad offset matching.
void __fastcall MarkerInsertDetour(void* navigation,void*,std::uintptr_t block) noexcept {
    const NativeCallScope frame;
    BeforeNativeMarkerInsertion(reinterpret_cast<std::uintptr_t>(navigation),block);
    reinterpret_cast<void(__thiscall*)(void*,std::uintptr_t)>(gBase+0x4D190U)(navigation,block);
    if(gGuard&&gGuard()&&EngineProducer().Bound())FinalizePending();
}
}
void BeforeNativeMarkerInsertion(std::uintptr_t navigation,std::uintptr_t block) noexcept {
    if(!gInstalled||!gGuard||!gGuard()||!EngineProducer().Bound())return;
    auto* lease=block>=0x1CU?Find(block-0x1CU):nullptr;
    if(lease&&lease->parameters.kind==2&&!lease->unknown&&gGuard&&gGuard()&&Live(*lease)) {
        Ref movement{};movement::Parameters authored{};std::uint32_t marker{};native_intent::Payload key{};
        auto expected=lease->parameters.intent;expected.words[12]=expected.words[13]=0;
        if(!Read(lease->ref.object+0xB0,marker)||!gMarkerCapture||!gMarkerCapture(marker,key)||key!=expected||
            !Read(lease->character+0x42A8U,movement)||!movement.object||
            navigation!=movement.object+0xC4U||
            !SnapshotMovement(lease->ref.object,2,authored,false)) {lease->unknown=true;Lost(__LINE__);}
        else {lease->parameters.movement.before=authored;lease->markerInitialized=true;}
    }
}
void SetNativeMarkerCapture(NativeMarkerCapture capture) noexcept {gMarkerCapture=capture;}
void SetNativeSchedulerObserver(NativeSchedulerObserver observer) noexcept {
    gSchedulerObserver.store(observer,std::memory_order_release);
}
void BeforeNativeRetention(std::uintptr_t scheduler,std::uintptr_t request) noexcept {
    if(!gInstalled||!gGuard||!gGuard()||!EngineProducer().Bound())return;
    const auto* lease=Find(request);std::uintptr_t owner{};std::uint32_t status{};
    if(lease&&(scheduler_control::Supported(lease->parameters.kind)||lease->parameters.kind==2||lease->parameters.kind==execution::kPlacementKind)&&
        Read(request+0x14U,owner)&&owner==scheduler&&Read(request+0xCU,status)&&status>=3U&&status<=5U)
        ObserveStatus(request,lease->character,status);
}
void BeginNativeMove(std::uintptr_t character,std::uintptr_t state) noexcept {
    if(gMoveOverflow||gMoveDepth==gMoveFrames.size()){++gMoveOverflow;return;}
    auto& frame=gMoveFrames[gMoveDepth++];frame={};frame.character=character;frame.state=state;
    transactions::OwnerKey owner{};
    frame.bound=gInstalled&&gGuard&&gGuard()&&EngineProducer().Bound()&&gContext(character,owner);
    if(!frame.bound)return;
    std::uintptr_t request{};
    if(!Read(state+0x2C,request)){Lost(__LINE__);frame.bound=false;return;}
    if(auto* existing=Find(request);existing&&existing->parameters.kind==1&&existing->character==character) {
        frame.primary=request;existing->producerActive=true;
    }
}
void BeforeNativeAdmission(std::uintptr_t request,std::uint32_t admission) noexcept {
    if(!gInstalled||!gGuard||!gGuard()||!EngineProducer().Bound())return;
    std::uint32_t kind{};if(!Read(request+0x10,kind))return;
    // Direct NPC locomotion: do not publish local pathfinding, steering,
    // placement or scheduler hold instructions to clients. They are per-brain
    // decisions and can never converge spatially. Kind 5 (native action) and
    // all observed state/passage/lifecycle events keep their ordered channel.
    if(kind==1U||kind==2U||kind==3U||kind==4U||kind==6U||
       kind==7U||kind==8U||kind==9U||kind==11U)return;
    std::uintptr_t scheduler{},character{};transactions::OwnerKey owner{};
    if(!Read(request+0x14,scheduler)||!Read(scheduler,character)||scheduler!=character+0x2CD0||!gContext(character,owner))return;
    if(admission>2){Lost(__LINE__);return;}
    if(kind==2) {
        // A raw Host marker index is process-local. Capture its exact loaded
        // asset/ordinal key BEFORE native admission; unresolved identity skips
        // only this request, never closes unrelated actors' publication.
        native_intent::Payload key{};std::uint32_t marker{};std::uint8_t opaque{},retained{};
        if((EngineProducer().Profiles()&execution::kProfileMarker)==0||!gMarkerCapture||!Read(request+0xB0,marker)||marker==UINT32_MAX||
            !Read(request+0xB4,opaque)||opaque>1||!Read(request+0x18,retained)||retained>1||
            !gMarkerCapture(marker,key)) {gUncoveredKinds[2].fetch_add(1U,std::memory_order_relaxed);return;}
        if(Find(request)){gUncoveredKinds[2].fetch_add(1U,std::memory_order_relaxed);return;}
        Lease* lease{};for(auto& item:gLeases)if(!item.used){lease=&item;break;}
        if(!lease){Lost(__LINE__,execution::Recovery::Capacity);return;}
        *lease={};lease->used=true;lease->owner=owner;lease->character=character;
        lease->parameters.kind=lease->sourceKind=2;lease->parameters.admission=admission;
        lease->parameters.intent=key;lease->parameters.intent.words[12]=opaque;lease->parameters.intent.words[13]=opaque;
        lease->markerRetainedBefore=retained;lease->admissionActive=true;
        lease->parameters.movement.version=1;lease->parameters.movement.parts=1;
        lease->parameters.movement.navigation=movement::Navigation::HostRoute;
        if(gAdmissionOrder==UINT64_MAX){lease->unknown=true;Lost(__LINE__,execution::Recovery::Capacity);return;}
        lease->order=++gAdmissionOrder;
        if(!HostScope(lease->scope)||!Read(character+0x50,lease->runtime)||!Strong(request,lease->ref)) {
            lease->unknown=true;Lost(__LINE__);return;
        }
        lease->refsHeld=true;
        if(!Live(*lease)){lease->unknown=true;Lost(__LINE__);}
        return;
    }
    if(kind==4||kind==7||kind==execution::kActionKind||kind==execution::kPlacementKind||kind==execution::kDirectedKind||scheduler_control::Supported(kind)) {
        auto* lease=Capture(request);if(!lease||lease->unknown)return;
        if(lease->submitted){Lost(__LINE__);return;}
        // A kind5 admission can precede execution. Its cycle identity is
        // observed at the retail scheduler boundary, never guessed here from
        // the cycle left by the preceding command.
        lease->parameters.admission=admission;lease->admissionActive=true;return;
    }
    if(kind!=1&&kind!=3) {
        gUncoveredKinds[(std::min)(kind,15U)].fetch_add(1U,std::memory_order_relaxed);return;
    }
    auto* frame=(!gMoveOverflow&&gMoveDepth&&gMoveFrames[gMoveDepth-1].bound&&
        gMoveFrames[gMoveDepth-1].character==character)?&gMoveFrames[gMoveDepth-1]:nullptr;
    if(kind!=3||admission!=1||!frame) {
        if(Find(request)){Lost(__LINE__);return;}
        Lease* lease{};for(auto& candidate:gLeases)if(!candidate.used){lease=&candidate;break;}
        if(!lease){Lost(__LINE__,execution::Recovery::Capacity);return;}
        *lease={};lease->used=true;lease->owner=owner;lease->character=character;
        lease->parameters.kind=kind;lease->sourceKind=kind;
        if(gAdmissionOrder==UINT64_MAX){lease->unknown=true;Lost(__LINE__,execution::Recovery::Capacity);return;}
        lease->order=++gAdmissionOrder;lease->parameters.admission=admission;
        lease->admissionActive=true;
        auto& b=lease->parameters.movement;b.version=1;b.parts=1;
        // Replay the actual producer destination/stop/gait inputs. Replacing
        // a long Host Move with a short physical-carrier target starts native
        // stop mode6; 0044D320 then discards goal edits even with a live tail.
        // Destination equality alone does not preserve the Host corridor or
        // its vent links. Import its certified native route; missing geometry
        // must wait/fail through the route barrier rather than solve locally.
        b.navigation=movement::Navigation::HostRoute;
        if(!HostScope(lease->scope)||!Read(character+0x50,lease->runtime)||
            !SnapshotMovement(request,kind,b.before,false)||!Strong(request,lease->ref)) {
            lease->unknown=true;Lost(__LINE__);return;
        }
        lease->refsHeld=true;
        if(!Live(*lease)){lease->unknown=true;Lost(__LINE__);return;}
        if(kind==3) {
            std::uint8_t flags{};
            if(!Read(request+0xB8,flags)){lease->unknown=true;Lost(__LINE__);return;}
            b.primaryFlags=flags&7U;
        }
        if(frame&&kind==1){frame->primary=request;lease->producerActive=true;}
    } else {
        auto* lease=frame?Find(frame->primary):nullptr;
        // Companion of a Move admitted while unbound: as untracked as its
        // primary, which BootstrapNativeActor cancels.
        if(!lease&&frame&&!frame->primary){gUncoveredKinds[3].fetch_add(1U,std::memory_order_relaxed);return;}
        if(admission!=1||!lease||lease->unknown||lease->companion.object||!Live(*lease)||
            !SnapshotMovement(request,3,lease->parameters.movement.companion,false)) {
            if(lease)lease->unknown=true;Lost(__LINE__);return;
        }
        if(!Strong(request,lease->companion)){lease->refsHeld=false;lease->unknown=true;Lost(__LINE__);return;}
        if(!Live(*lease)){lease->unknown=true;Lost(__LINE__);return;}
        lease->parameters.movement.parts=2;
        lease->admissionActive=true;
        std::uint8_t flags{};
        if(!Read(request+0xB8,flags)) {
            lease->unknown=true;Lost(__LINE__);return;
        }
        lease->parameters.movement.companionFlags=flags&7U;
    }
}
void EndNativeMove() noexcept {
    if(gMoveOverflow){--gMoveOverflow;return;}if(!gMoveDepth)return;
    const auto frame=gMoveFrames[--gMoveDepth];gMoveFrames[gMoveDepth]={};
    if(!frame.bound||!gGuard||!gGuard())return;
    auto* lease=Find(frame.primary);if(!lease)return;
    if(lease->parameters.kind!=1&&lease->parameters.kind!=2&&lease->parameters.kind!=3)return;
    // The Move frame has returned even when a recovery unbound the source in
    // between. Leaving this frame bit set made RetireNative skip the lease
    // forever, so the next authority Offer could never qualify (Host stream
    // permanently silent after a same-world recovery).
    lease->producerActive=false;
    if(!EngineProducer().Bound())return;
    if(lease->submitted)FinalizeMovement(*lease);else FinalizePending();
}
void ObserveNativeMutation(std::uintptr_t request) noexcept {
    if(!gInstalled||!gGuard||!gGuard()||!EngineProducer().Bound())return;
    auto* lease=Find(request);
    if(lease&&lease->parameters.kind==execution::kPlacementKind) {
        if(!execution::PlacementDedupReady()||lease->unknown||!lease->submitted||lease->admissionActive)return;
        std::uintptr_t character{};std::uint32_t runtime{},status{};Command parameters{};
        if(!Live(*lease)||!Snapshot(request,character,runtime,parameters)||character!=lease->character||runtime!=lease->runtime||
            !Read(request+0xC,status)) {lease->unknown=true;Lost(__LINE__);return;}
        if(status!=1U&&status!=2U)return;
        parameters.admission=lease->parameters.admission;
        parameters.scalarRetention=(lease->parameters.scalarRetention&1U)|(parameters.scalarRetention&2U);
        if(parameters!=lease->parameters) {
            if(EngineProducer().Update(request,parameters)!=SourceResult::Ok){lease->unknown=true;return;}
            lease->parameters=parameters;
        }
        return;
    }
    if(!lease||lease->parameters.kind!=1||lease->unknown||!lease->submitted||lease->producerActive||lease->admissionActive)return;
    FinalizeMovement(*lease);
}
bool InstallNative(std::uintptr_t base, HookManager& hooks, NativeContext context, NativeGuard guard,
    NativeRetirementGuard retirement, std::string& error) {
    if (gInstalled || !base || !context || !guard || !retirement || !hooks.ExactBuildValidated()) return false;
    gBase = base; gContext = context; gGuard = guard; gRetirementGuard = retirement;
    const auto before = hooks.InstalledCount();
    const bool sharedAdmission = transaction_capture::HasAdmissionHooks(base);
    const auto install = [&](const char* name, std::uintptr_t rva, void* detour,
        std::initializer_list<std::uint8_t> bytes, void** trampoline, std::atomic<void*>* published) {
        HookRequest request{}; request.name = name; request.target = reinterpret_cast<void*>(base + rva);
        request.detour = detour; request.patchLength = bytes.size(); request.relocationSafePrefix = true;
        request.trampolineOut = trampoline; request.publishedTrampolineOut = published;
        for (auto byte : bytes) { request.expected.bytes.push_back(byte); request.expected.mask.push_back('x'); }
        if(rva==0x30A940U) {
            const auto handler=static_cast<std::uint32_t>(base+0xD29798U);
            std::memcpy(request.expected.bytes.data()+3,&handler,4);
        }
        return hooks.Install(request,error);
    };
    if (!install("AISync marker initializer",0x281810U,reinterpret_cast<void*>(&MarkerInitializeDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0},&gMarkerInitializeTrampoline,&gMarkerInitializeOriginal) ||
        !install("AISync typed Host status",0x2777E0U,reinterpret_cast<void*>(&StatusDetour),
            {0x83,0x7C,0x24,0x0C,0x02},&gStatusTrampoline,&gStatusOriginal) ||
        !install("AISync typed Host request retention",0x283470U,reinterpret_cast<void*>(&RetentionDetour),
            {0x53,0x8B,0x5C,0x24,0x08},&gRetentionTrampoline,&gRetentionOriginal) ||
        !install("AISync typed Host scheduler",0x288510U,reinterpret_cast<void*>(&TickDetour),
            {0x83,0xEC,0x10,0x56,0x8B,0xF1},&gTickTrampoline,&gTickOriginal) ||
        (!sharedAdmission && (!install("AISync typed Host Move",0x30A940U,reinterpret_cast<void*>(&MoveDetour),
            {0x6A,0xFF,0x68,0,0,0,0},&gMoveTrampoline,&gMoveOriginal) ||
        !install("AISync typed Host append",0x283C10U,reinterpret_cast<void*>(&AppendDetour),
            {0x64,0xA1,0,0,0,0},&gAppendTrampoline,&gAppendOriginal) ||
        !install("AISync typed Host front",0x283550U,reinterpret_cast<void*>(&FrontDetour),
            {0x64,0xA1,0,0,0,0},&gFrontTrampoline,&gFrontOriginal) ||
        !install("AISync typed Host stop update",0x27D0D0U,reinterpret_cast<void*>(&StopUpdateDetour),
            {0xF3,0x0F,0x10,0x49,0x58},&gStopUpdateTrampoline,&gStopUpdateOriginal)))) {
        std::string rollback; if (!hooks.UninstallFrom(before,rollback)) error += "; " + rollback;
        return false;
    }
    if(!sharedAdmission)for(const auto site:{0x30AAEAU,0x362417U}) {
        CallRedirectRequest call{"AISync typed Host goal update",reinterpret_cast<void*>(base+site),
            reinterpret_cast<void*>(base+0x2C7550),reinterpret_cast<void*>(&GoalUpdateDetour)};
        if(!hooks.InstallCallRedirect(call,error)) {
            std::string rollback;if(!hooks.UninstallFrom(before,rollback))error+="; "+rollback;return false;
        }
    }
    const CallRedirectRequest markerInsert{"AISync marker authored parameters",reinterpret_cast<void*>(base+0x28192FU),
        reinterpret_cast<void*>(base+0x4D190U),reinterpret_cast<void*>(&MarkerInsertDetour)};
    if(!hooks.InstallCallRedirect(markerInsert,error)) {
        std::string rollback;if(!hooks.UninstallFrom(before,rollback))error+="; "+rollback;return false;
    }
    gInstalled = true; return true;
}
void ObserveNativeAdmission(std::uintptr_t request, bool accepted) noexcept {
    if (!gGuard || !gGuard()) return;
    if (!EngineProducer().Bound()) {
        // Same boundary as EndNativeMove: the native admission has returned,
        // so an unbound source must not keep this lease pinned as in-flight.
        // Nothing is published; RetireNative releases it normally.
        if(auto* move=Find(request))move->admissionActive=false;
        return;
    }
    if(auto* move=Find(request)) {
        if(request==move->ref.object)move->primaryAccepted=accepted;else move->companionAccepted=accepted;
        if(move->parameters.kind==2) {
            std::uint8_t opaque{};
            if(!Read(request+0xB4,opaque)||opaque>1){move->unknown=true;Lost(__LINE__);return;}
            move->parameters.intent.words[13]=opaque;
        }
        move->admissionActive=false;
        return;
    }
    if(!accepted)return;
    // Bootstrap-only scalar checkpoint or a missing certified admission seam.
    auto* lease = Capture(request);
    if (!lease || lease->unknown || lease->submitted) return;
    std::uintptr_t character{}; std::uint32_t runtime{}; Command parameters{};
    if (!Snapshot(request,character,runtime,parameters) || character != lease->character || runtime != lease->runtime || !Live(*lease)) {
        lease->unknown = true; Lost(__LINE__); return;
    }
    lease->parameters=parameters;lease->primaryAccepted=true;
}
void PumpNative() noexcept {
    if (!gInstalled || !gGuard || !gGuard() || !EngineProducer().Bound()) return;
    const NativeCallScope frame;
    FinalizePending();
    for (auto& lease : gLeases) {
        if (!lease.used || lease.unknown) continue;
        if(!lease.submitted)continue;
        if (!Live(lease)) { lease.unknown = true; Lost(__LINE__); return; }
        if((lease.parameters.kind==1||lease.parameters.kind==2||lease.parameters.kind==3)) {
            if(lease.producerActive||lease.admissionActive)continue;
            // Producer-return boundaries capture changes; scheduler execution
            // cursors/flags never create synthetic Host updates.
            if(FinishMovement(lease))continue;
            if(lease.parameters.kind==2) {
                auto parameters=lease.parameters;std::uint8_t opaque{},retained{};std::uint32_t marker{};
                native_intent::Payload key{};
                if(!Read(lease.ref.object+0xB0,marker)||!Read(lease.ref.object+0xB4,opaque)||opaque>1||
                    !Read(lease.ref.object+0x18,retained)||retained>1||!gMarkerCapture||!gMarkerCapture(marker,key)) {
                    lease.unknown=true;Lost(__LINE__);continue;
                }
                auto expected=lease.parameters.intent;expected.words[12]=expected.words[13]=0;
                if(key!=expected){lease.unknown=true;Lost(__LINE__);continue;}
                parameters.intent.words[13]=opaque;parameters.movement.after.retained=retained;
                if(parameters!=lease.parameters) {
                    if(EngineProducer().Update(lease.ref.object,parameters)!=SourceResult::Ok){lease.unknown=true;continue;}
                    lease.parameters=parameters;
                }
                continue;
            }
            std::uint32_t gait{};
            if(!SnapshotGait(lease.character,gait)){lease.unknown=true;Lost(__LINE__);return;}
            if(gait!=lease.parameters.movement.gait) {
                auto parameters=lease.parameters;parameters.movement.gait=gait;
                if(EngineProducer().Update(lease.ref.object,parameters)!=SourceResult::Ok){lease.unknown=true;return;}
                lease.parameters=parameters;
            }
            continue;
        }
        std::uintptr_t character{}; std::uint32_t runtime{}, status{}; Command parameters{};
        if (!Snapshot(lease.ref.object,character,runtime,parameters) || character != lease.character || runtime != lease.runtime ||
            !Read(lease.ref.object + 0xC,status)) { lease.unknown = true; Lost(__LINE__); return; }
        if (status >= 3 && status <= 6) { ObserveStatus(lease.ref.object,character,status); continue; }
        parameters.admission=lease.parameters.admission;
        parameters.scalarRetention=(lease.parameters.scalarRetention&1)|(parameters.scalarRetention&2);
        if (parameters != lease.parameters) {
            if (EngineProducer().Update(lease.ref.object,parameters) != SourceResult::Ok) { lease.unknown = true; return; }
            lease.parameters = parameters;
        }
    }
}
void RetireNative(bool liveWorld, bool retireSource) noexcept {
    if(gNativeCallDepth.load(std::memory_order_acquire)!=0||gMoveDepth||gMoveOverflow){if(retireSource)RetireHost();return;}
    const NativeCallScope frame;
    // Never chase an old Character after a world barrier. An unresolved Ref
    // remains visible and prevents a new source from claiming clean coverage.
    for (auto& lease : gLeases) if (lease.used) {
        if (!liveWorld) { lease.unknown = true; lease.worldFenced = true; continue; }
        // Only this top-level engine-tick path recovers a same-world fence:
        // a destructor reentering mid-decrement would still see control!=0.
        if (lease.unknown && !RecoverableFence(lease)) continue;
        if(lease.producerActive||lease.admissionActive)continue;
        if (!gRetirementGuard || !gRetirementGuard(lease.scope,lease.owner,lease.character,lease.runtime)) continue;
        const character_direct_step_runtime::MovementResourceLease resources(lease.character);
        if (!resources || !gRetirementGuard(lease.scope,lease.owner,lease.character,lease.runtime)) continue;
        // Mark before entry. A destructor/reentrant callback cannot retry a
        // decrement whose native outcome is uncertain.
        lease.unknown = true;
        if(lease.companion.object) {
            if(!ReleaseRef(lease.companion)||!gRetirementGuard(lease.scope,lease.owner,lease.character,lease.runtime))continue;
        }
        if (ReleaseRef(lease.ref)) lease = {};
    }
    if(retireSource)RetireHost();
}
bool NativeIdle() noexcept { for (const auto& lease : gLeases) if (lease.used) return false; return true; }
bool NativeNeedsLiveWorldRetirement() noexcept {
    // A retained old-world strong Ref is *not* a live request. Calling
    // RetireHost for it every frame clears the new Pending Offer before
    // PumpHost can configure the next world, so no Ready can ever be sent.
    // A same-world capture fence whose Refs are exactly held still needs (and
    // allows) retirement: otherwise one capture failure silences the Host
    // source for the rest of the round (diag 054751, loss site 339).
    for(const auto& lease:gLeases)if(lease.used&&(!lease.unknown||RecoverableFence(lease)))return true;
    return false;
}
// A world transition retires the source BEFORE the prior CATHODE requests
// necessarily release their independent strong Refs. RetireNative(false)
// explicitly fences those observations as unknown without dereferencing old
// Characters. Their slots must stay pinned, but they cannot represent an
// active command in a strictly newer Host world.
bool NativeReadyForNewWorld(const transactions::Scope& scope) noexcept {
    if(!gInstalled||scope.worldEpoch==0U||scope.consumerEpoch!=1U||
        EngineProducer().Bound()||
        gNativeCallDepth.load(std::memory_order_acquire)!=0U||gMoveDepth||gMoveOverflow)return false;
    for(const auto& lease:gLeases)if(lease.used) {
        // producerActive/admissionActive are historical bits of the dead
        // world's request; at a completed engine-thread generation boundary
        // they cannot grant a new-world native callback. The global native
        // call/Move-frame barriers above still forbid any in-flight call.
        // A fenced lease from an earlier lobby session is necessarily older:
        // sessions never overlap and a new lobby always loads a new world.
        // Keeping it pinned is enough; it must not veto every later session.
        if(!lease.unknown)return false;
        if(lease.scope.session==scope.session&&
            (lease.scope.worldEpoch==0U||lease.scope.worldEpoch>=scope.worldEpoch))return false;
    }
    // Retained references are *not* released, recycled, or declared quiet:
    // future source leases occupy distinct free slots and retain the new
    // scope/lifetime. Exhaustion continues to fail closed at kCommands.
    return true;
}
std::uint32_t RetainedNativeLeases() noexcept {
    std::uint32_t total{};
    for(const auto& lease:gLeases)if(lease.used)++total;
    return total;
}
std::uint32_t FencedOldNativeLeases() noexcept {
    std::uint32_t total{};
    for(const auto& lease:gLeases)if(lease.used&&lease.unknown)++total;
    return total;
}
CoverageDiagnostics Coverage() noexcept {
    return {gCoverageLosses.load(std::memory_order_relaxed),gFirstLossSite.load(std::memory_order_relaxed),
        gLastLossSite.load(std::memory_order_relaxed)};
}
MovementCaptureFailure FirstMovementCaptureFailure() noexcept {
    const std::lock_guard lock(gMovementFailureGate);return gMovementFailure;
}
std::uint64_t UncoveredKind(std::uint32_t kind) noexcept {
    return kind < gUncoveredKinds.size() ? gUncoveredKinds[kind].load(std::memory_order_relaxed) : 0U;
}
void BeginNativeMutation() noexcept {gNativeCallDepth.fetch_add(1,std::memory_order_acq_rel);}
void EndNativeMutation() noexcept {gNativeCallDepth.fetch_sub(1,std::memory_order_release);}
bool BeforeNativeActorDestruction(std::uintptr_t character,std::uint32_t runtime) noexcept {
    bool complete=true;
    for(auto& lease:gLeases)if(lease.used&&lease.character==character&&lease.runtime==runtime) {
        // First close the source stream; observer retirement is not an
        // observed Host Complete/Cancel/Release operation.
        Lost(__LINE__);
        if(gNativeCallDepth.load(std::memory_order_acquire)!=0||gMoveDepth||gMoveOverflow||
            lease.unknown||lease.producerActive||lease.admissionActive||!gRetirementGuard||
            !gRetirementGuard(lease.scope,lease.owner,character,runtime)) {complete=false;continue;}
        const NativeCallScope frame;
        const character_direct_step_runtime::MovementResourceLease resources(character);
        if(!resources||!gRetirementGuard(lease.scope,lease.owner,character,runtime)){complete=false;continue;}
        lease.unknown=true;
        if(lease.companion.object&&(!ReleaseRef(lease.companion)||
            !gRetirementGuard(lease.scope,lease.owner,character,runtime))){complete=false;continue;}
        if(!ReleaseRef(lease.ref)){complete=false;continue;}
        lease={};
    }
    return complete;
}
bool IdentifyNativeNavigation(std::uintptr_t navigation,std::uintptr_t block,CommandIdentity& output,std::uint32_t& part) noexcept {
    if(!gGuard||!gGuard()||!navigation||!block)return false;
    for(auto& lease:gLeases)if(lease.used&&lease.submitted&&!lease.unknown&&
        (lease.parameters.kind==1||lease.parameters.kind==2||lease.parameters.kind==3)) {
        const auto primary=lease.ref.object+(SourceKind(lease)==1?0x28U:0x1CU);
        const auto secondary=lease.companion.object?lease.companion.object+0x1CU:0;
        if(block!=primary&&block!=secondary)continue;
        Ref before{},after{};CommandIdentity identity{};
        if(!Live(lease)||!Read(lease.character+0x42A8U,before)||!before.object||
            navigation!=before.object+0xC4U||!EngineProducer().Identify(lease.ref.object,identity)||
            identity.scope!=lease.scope||identity.owner!=lease.owner||!Live(lease)||
            !Read(lease.character+0x42A8U,after)||before.object!=after.object||before.control!=after.control)return false;
        output=identity;part=block==primary?0U:1U;return true;
    }
    return false;
}
bool IdentifyNativePlacement(std::uintptr_t scheduler,NativePlacementIdentity& output) noexcept {
    if(!gInstalled||!execution::PlacementDedupReady()||!gGuard||!gGuard()||!EngineProducer().Bound())return false;
    std::uintptr_t character{},head{},first{},request{},control{},requestScheduler{},type{};
    std::uint32_t count{},kind{},status{};
    if(!Read(scheduler,character)||!character||scheduler!=character+0x2CD0U||
        !Read(scheduler+4,head)||!head||!Read(scheduler+8,count)||!count||
        !Read(head,first)||!first||first==head||!Read(first+8,request)||!Read(first+12,control)||!control||
        !Read(request,type)||type!=gBase+0xEB199CU||!Read(request+0x10,kind)||kind!=execution::kPlacementKind||
        !Read(request+0x14,requestScheduler)||requestScheduler!=scheduler||
        !Read(request+0xC,status)||(status!=1U&&status!=2U))return false;
    auto* lease=Find(request);
    if(!lease||lease->unknown||!lease->submitted||lease->admissionActive||lease->character!=character||
        lease->parameters.kind!=execution::kPlacementKind||lease->ref.control!=control||!Live(*lease))return false;
    std::uintptr_t observedCharacter{};std::uint32_t runtime{};Command parameters{};CommandIdentity identity{};
    if(!Snapshot(request,observedCharacter,runtime,parameters)||observedCharacter!=character||runtime!=lease->runtime)return false;
    parameters.admission=lease->parameters.admission;
    parameters.scalarRetention=(lease->parameters.scalarRetention&1U)|(parameters.scalarRetention&2U);
    if(parameters!=lease->parameters||!EngineProducer().Identify(request,identity)||identity.scope!=lease->scope||
        identity.owner!=lease->owner||identity.nativeKind!=execution::kPlacementKind||!Live(*lease))return false;
    output={identity,scheduler,request,character,runtime,parameters.mode};return true;
}
void ObserveNativeActionExecution(std::uintptr_t scheduler) noexcept {
    if(!gInstalled||!gGuard||!gGuard())return;
    std::uintptr_t character{},head{},first{},request{},requestScheduler{};
    std::uint32_t count{},status{};
    if(!Read(scheduler,character)||scheduler!=character+0x2CD0U||
        !Read(scheduler+4,head)||!head||!Read(scheduler+8,count)||!count||
        !Read(head,first)||!first||first==head||!Read(first+8,request))return;
    auto* lease=Find(request);
    if(!lease||lease->parameters.kind!=execution::kActionKind||lease->unknown||lease->actionRevoked||
        !lease->submitted||lease->admissionActive||lease->character!=character||!Live(*lease)||
        !Read(request+0x14,requestScheduler)||requestScheduler!=scheduler||
        !Read(request+0xC,status)||(status!=1U&&status!=2U))return;
    std::uintptr_t cycle{},owner{},type{};std::uint8_t state{},serial{};
    if(!Read(character+0x36C,cycle)||!cycle||!Read(cycle+4,owner)||owner!=character||
        !Read(cycle,type)||type<=gBase||type-gBase>0x02000000U||!Read(cycle+0x1A,state)||
        state!=lease->parameters.mode||!Read(cycle+0x19,serial))return;
    if(lease->actionKnown) {
        if(lease->actionCycle!=cycle||lease->actionType!=type-gBase||lease->actionSerial!=serial) {
            lease->actionKnown=false;lease->actionRevoked=true;
        }
        return;
    }
    // Native 00687190/00687690 select scheduler.head; the accepted, strongly
    // retained request supplies the state. Admission itself need not have
    // executed it synchronously. Never associate a waiting non-head request.
    lease->actionCycle=cycle;lease->actionType=static_cast<std::uint32_t>(type-gBase);
    lease->actionState=state;lease->actionSerial=serial;lease->actionKnown=true;
}
bool IdentifyNativeAction(std::uintptr_t character,std::uintptr_t cycle,std::uint32_t typeRva,
    std::uint8_t state,std::uint8_t serial,std::uint32_t revision,action_correlation::Reference& output) noexcept {
    if(!gInstalled||!gGuard||!gGuard()||!character||!cycle||!revision)return false;
    for(const auto& lease:gLeases)if(lease.used&&lease.character==character&&lease.parameters.kind==execution::kActionKind&&
        lease.actionKnown&&lease.actionCycle==cycle&&lease.actionType==typeRva&&lease.actionState==state&&lease.actionSerial==serial&&
        (lease.admissionActive||!lease.submitted))return false;
    action_correlation::Reference candidate{};Lease* selected{};std::size_t matches{};
    for(auto& lease:gLeases)if(lease.used&&lease.character==character&&lease.parameters.kind==execution::kActionKind&&
        lease.submitted&&!lease.unknown&&!lease.admissionActive&&lease.actionKnown) {
        // An observed mismatch permanently revokes this lease's association.
        // Never rebind it when a later cycle returns to the same state/serial.
        std::uintptr_t current{},type{},owner{};std::uint8_t currentState{},currentSerial{};
        if(!Live(lease)||!Read(character+0x36C,current)||current!=lease.actionCycle||
            !Read(current,type)||type!=gBase+lease.actionType||!Read(current+4,owner)||owner!=character||
            !Read(current+0x1A,currentState)||currentState!=lease.actionState||
            !Read(current+0x19,currentSerial)||currentSerial!=lease.actionSerial) {lease.actionKnown=false;lease.actionRevoked=true;continue;}
        if(cycle!=current||typeRva!=lease.actionType||state!=currentState||serial!=currentSerial||state!=lease.parameters.mode)continue;
        std::uint32_t status{};if(!Read(lease.ref.object+0xC,status)||(status!=1&&status!=2))continue;
        action_correlation::Reference found{};
        if(!EngineProducer().ActionReference(lease.ref.object,revision,found)||found.source!=lease.scope||
            found.ownerEpoch!=lease.owner.ownerEpoch||!Live(lease))continue;
        candidate=found;selected=&lease;++matches;
    }
    if(matches!=1) {
        if(matches>1)for(auto& lease:gLeases)if(lease.used&&lease.character==character&&lease.actionCycle==cycle&&
            lease.actionType==typeRva&&lease.actionState==state&&lease.actionSerial==serial)lease.actionKnown=false;
        return false;
    }
    action_correlation::Reference currentReference{};std::uintptr_t current{},type{},cycleOwner{};std::uint8_t currentState{},currentSerial{};
    if(!selected||!selected->used||selected->unknown||!selected->actionKnown||!Live(*selected)||
        !EngineProducer().ActionReference(selected->ref.object,revision,currentReference)||currentReference!=candidate||
        !Read(character+0x36C,current)||current!=cycle||!Read(current,type)||type!=gBase+typeRva||
        !Read(current+4,cycleOwner)||cycleOwner!=character||!Read(current+0x1A,currentState)||currentState!=state||
        !Read(current+0x19,currentSerial)||currentSerial!=serial)return false;
    output=candidate;return true;
}
void InvalidateNativeActor(std::uintptr_t character, std::uint32_t runtime) noexcept {
    bool found = false;
    for (auto& lease : gLeases) if (lease.used && lease.character == character && lease.runtime == runtime) {
        lease.unknown = true; found = true;
    }
    if (found) Lost(__LINE__);
}
bool BootstrapNativeActor(std::uintptr_t character) noexcept {
    if (!gInstalled || !gGuard || !gGuard() || !EngineProducer().Bound()) return false;
    std::uintptr_t owner{}, sentinel{}, first{}, request{}; std::uint32_t count{}, status{};
    if (!Read(character + 0x2CD0U,owner) || owner != character ||
        !Read(character + 0x2CD8U,count) || !Read(character + 0x2CD4U,sentinel) ||
        !Read(sentinel,first)) { Lost(__LINE__); return false; }
    if (count == 0 && first == sentinel) return true;
    if (count > 64U || first == sentinel) { Lost(__LINE__); return false; }
    // MSVC std::list node: +0 next, +8 request. A request admitted while the
    // source was unbound has no observed cause. Only a lone active scalar head
    // is checkpointed; anything else is cancelled natively, one per call, so
    // the Host brain reissues it through the observed admission seams and the
    // Clients start from the same empty baseline. Retried every AI update.
    bool primed = true;
    auto node = first;
    for (std::uint32_t i = 0; i < count && node != sentinel; ++i) {
        std::uint32_t kind{};
        if (!Read(node + 8,request) || !request || !Read(request + 0xC,status) || !Read(request + 0x10,kind)) {
            Lost(__LINE__); return false;
        }
        if (Find(request) || status < 1 || status > 2) {
            primed = primed && Find(request) != nullptr;
            if (!Read(node,node)) { Lost(__LINE__); return false; }
            continue;
        }
        if (count == 1 && kind != 1 && kind != 2 && kind != 3 && execution::ProfileForKind(kind) != 0) {
            ObserveNativeAdmission(request,true);
            FinalizePending();
            const auto* lease = Find(request);
            return lease && lease->submitted && !lease->unknown && EngineProducer().Bound();
        }
        if (execution::ProfileForKind(kind) == 0) {
            // Not replicated by any profile: left to the Host, never cancelled.
            if (!Read(node,node)) { Lost(__LINE__); return false; }
            continue;
        }
        if (gMoveDepth || gMoveOverflow) return false;
        const NativeCallScope frame;
        if (!CancelNative(request)) { Lost(__LINE__); return false; }
        gBootstrapCancels.fetch_add(1U,std::memory_order_relaxed);
        return false;
    }
    return primed;
}
std::uint64_t BootstrapCancels() noexcept { return gBootstrapCancels.load(std::memory_order_relaxed); }
} // namespace aimp::ai_sync::host_producer
