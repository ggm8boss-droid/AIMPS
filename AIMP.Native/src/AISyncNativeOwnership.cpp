#include "aimp/AISyncNativeOwnership.h"
#include "aimp/AISyncExecutionPolicy.h"
#include "aimp/AISyncExecutionCodec.h"
#include <cmath>

namespace aimp::ai_sync::execution {
namespace {
template<class T> bool Read(const OwnershipMemory& memory,std::uintptr_t at,T& value) noexcept {
    return memory.read&&at>=0x10000U&&at<=0xFFFFFFFFULL-sizeof(T)&&memory.read(memory.context,at,&value,sizeof(value));
}
struct Ref {std::uint32_t object{},control{};bool operator==(const Ref&) const noexcept=default;};
}
bool MatchesNativeExecution(const OwnershipMemory& memory,const NativeExecutionAnchor& anchor,
    const transactions::Scope& scope,const transactions::OwnerKey& owner,const ActorLifetime& actor) noexcept {
    if(anchor.scope!=scope||anchor.owner!=owner||anchor.actor!=actor||!actor.creationSerial||!anchor.cycle)return false;
    std::uint32_t cycle{},runtime{};std::uint8_t serial{};
    return Read(memory,actor.character+0x50U,runtime)&&runtime==actor.runtimeId&&
        Read(memory,actor.character+0x36CU,cycle)&&cycle==anchor.cycle&&
        Read(memory,cycle+0x19U,serial)&&serial==anchor.serial;
}
bool IsLiveNativePassage(const OwnershipMemory& memory,const NativeExecutionAnchor& anchor,
    const transactions::Scope& scope,const transactions::OwnerKey& owner,const ActorLifetime& actor) noexcept {
    if(memory.imageBase<0x10000U||memory.imageBase>0xFFFFFFFFULL-0xDBB360U||
        !actor.runtimeId||!actor.nativeGeneration||!MatchesNativeExecution(memory,anchor,scope,owner,actor))return false;
    std::uint32_t cycleOwner{},type{},health{};std::uint8_t state{},flags{},grappled{};
    const auto current=[&]() noexcept {
        return Read(memory,anchor.cycle+4U,cycleOwner)&&cycleOwner==actor.character&&
            Read(memory,anchor.cycle,type)&&Read(memory,anchor.cycle+0x1AU,state)&&
            ((type==memory.imageBase+0xDBB2A0U&&state==0x4BU)||
             (type==memory.imageBase+0xDBB360U&&state==0x4CU))&&
            Read(memory,anchor.cycle+8U,flags)&&(flags&0x06U)==0U&&
            Read(memory,actor.character+0x32CU,health)&&health<3U&&
            Read(memory,actor.character+0x297U,grappled)&&!grappled;
    };
    return current()&&MatchesNativeExecution(memory,anchor,scope,owner,actor)&&current();
}
bool SupersedesNativePassage(const NativeExecutionAnchor& anchor,std::uint64_t sequence,
    std::uint64_t command,const transactions::Record& record,transactions::NativeOutcome outcome) noexcept {
    if(!sequence||!command||outcome!=transactions::NativeOutcome::Admitted||
        record.scope!=anchor.scope||record.owner!=anchor.owner||record.globalSeq<=sequence||
        record.commandId==command||record.opcode!=transactions::Op::Submit)return false;
    if(record.nativeKind==1U&&record.admission==0U)return true;
    return record.nativeKind==kRelocateKind||record.nativeKind==kForcedRelocateKind||
        record.nativeKind==kDirectRelocateKind||record.nativeKind==kDirectForcedRelocateKind||
        record.nativeKind==kFrontstageKind||(record.nativeKind==kBackstageKind&&record.mode==0U);
}
static OwnershipCheck InspectBoundary(const OwnershipMemory& inputMemory,const ActorLifetime& inputActor,bool quiet,
    bool interaction,OwnershipBoundaryFailure* failure,bool initialBackstage=false,bool possession=false) noexcept {
    if(failure)*failure={};
    const auto memory=inputMemory;
    const auto actor=inputActor;
    if(memory.imageBase<0x10000U||memory.imageBase>0xFFFFFFFFULL-0xEABF80U||!actor.runtimeId||!actor.nativeGeneration)
        return OwnershipCheck::Reject;
    std::uint32_t runtime{},scheduler{},cycle{},owner{},type{},health{};
    std::uint8_t state{},serial{},grappled{};
    if(!Read(memory,actor.character+0x50U,runtime)||runtime!=actor.runtimeId||
        !Read(memory,actor.character+0x2CD0U,scheduler)||scheduler!=actor.character||
        !Read(memory,actor.character+0x36CU,cycle)||!cycle||!Read(memory,cycle+4U,owner)||owner!=actor.character||
        !Read(memory,cycle,type)||!Read(memory,cycle+0x19U,serial)||!Read(memory,cycle+0x1AU,state)||
        !Read(memory,actor.character+0x297U,grappled)||!Read(memory,actor.character+0x32CU,health))
        return OwnershipCheck::Unknown;
    bool cycleAllowed=type>=memory.imageBase&&(possession&&!initialBackstage?
        AlienPossessionDrainCycleAllowed(type-memory.imageBase,state):
        MovementDrainCycleAllowed(type-memory.imageBase,state));
    // 00401520 constructs a timed stationary hold. 00401580 destroys only
    // its base cycle; unlike75/76 it owns no traversal path/listener. Drain
    // the scheduler normally and leave this cycle to the next Host constructor.
    if(initialBackstage&&type==memory.imageBase+0xDBB1B8U&&state==0x4DU) {
        float deadline{};
        if(!Read(memory,cycle+0x44U,deadline))return OwnershipCheck::Unknown;
        cycleAllowed=std::isfinite(deadline);
    }
    if(interaction&&type>=memory.imageBase) {
        // 0065C4E0/0065BF60 dispatch the CURRENT cycle destructor before
        // constructing the pair. This is a native transition contract, not a
        // list of locomotion states or species. Existing paired resources keep
        // their separate owner until their own retirement proof.
        std::uint32_t destroy{},initialize{};
        if(!Read(memory,type,destroy)||!Read(memory,type+0x60U,initialize))return OwnershipCheck::Unknown;
        const auto code=[&](std::uint32_t address) noexcept {
            return address>=memory.imageBase+0x1000U&&address<memory.imageBase+0xD9F000U;
        };
        cycleAllowed=code(destroy)&&code(initialize)&&state!=0x42U&&state!=0x4EU&&state!=0x4FU;
    }
    if(grappled||health>=3U||!cycleAllowed) {
        if(failure)*failure=grappled?OwnershipBoundaryFailure{0x297U,grappled}:
            health>=3U?OwnershipBoundaryFailure{0x32CU,health}:OwnershipBoundaryFailure{0x36CU,type>=memory.imageBase?type-static_cast<std::uint32_t>(memory.imageBase):type};
        return OwnershipCheck::Waiting;
    }
    if(quiet) {
        if(state==0x38U){if(failure)*failure={0x36CU,state};return OwnershipCheck::Waiting;}
        std::uint32_t active{},retained{};Ref movement{},after{};
        if(!Read(memory,actor.character+0x2CD8U,active)||!Read(memory,actor.character+0x2CE4U,retained)||
            !Read(memory,actor.character+0x42A8U,movement)||!movement.object||!movement.control)
            return OwnershipCheck::Unknown;
        if(active||retained){if(failure)*failure=active?OwnershipBoundaryFailure{0x2CD8U,active}:OwnershipBoundaryFailure{0x2CE4U,retained};return OwnershipCheck::Waiting;}
        // M+C4 is the native navigation object. It may retain B/R/history after
        // the last scheduler request has been cancelled and its Ref released.
        const auto navigation=static_cast<std::uintptr_t>(movement.object)+0xC4U;
        for(const auto offset:{4U,0x14U,0x2CU,0x38U,0x40U,0x44U,0x5CU}) {
            std::uint32_t value{};
            if(!Read(memory,navigation+offset,value))return OwnershipCheck::Unknown;
            if(value){if(failure)*failure={0x10000U+offset,value};return OwnershipCheck::Waiting;}
        }
        if(!Read(memory,actor.character+0x42A8U,after)||movement!=after)return OwnershipCheck::Unknown;
    }
    std::uint32_t current{},currentCycle{},currentOwner{},currentType{},currentHealth{};std::uint8_t currentSerial{},currentState{},currentGrappled{};
    if(!Read(memory,actor.character+0x50U,current)||current!=runtime||
        !Read(memory,actor.character+0x36CU,currentCycle)||currentCycle!=cycle||
        !Read(memory,cycle+4U,currentOwner)||currentOwner!=owner||!Read(memory,cycle,currentType)||currentType!=type||
        !Read(memory,cycle+0x19U,currentSerial)||currentSerial!=serial||
        !Read(memory,cycle+0x1AU,currentState)||currentState!=state||
        !Read(memory,actor.character+0x32CU,currentHealth)||currentHealth!=health||
        !Read(memory,actor.character+0x297U,currentGrappled)||currentGrappled!=grappled)return OwnershipCheck::Unknown;
    return OwnershipCheck::Ready;
}
OwnershipCheck InspectMovementBoundary(const OwnershipMemory& memory,const ActorLifetime& actor,bool quiet,
    OwnershipBoundaryFailure* failure) noexcept {return InspectBoundary(memory,actor,quiet,false,failure);}
OwnershipCheck InspectInitialBackstageBoundary(const OwnershipMemory& memory,const ActorLifetime& actor,bool quiet,
    OwnershipBoundaryFailure* failure) noexcept {return InspectBoundary(memory,actor,quiet,false,failure,true);}
OwnershipCheck InspectAlienPossessionBoundary(const OwnershipMemory& memory,const ActorLifetime& actor,bool quiet,
    OwnershipBoundaryFailure* failure) noexcept {
    if(failure)*failure={};
    std::uint8_t a{},b{};
    if(!Read(memory,actor.character+0x2DAU,a)||!Read(memory,actor.character+0x2DBU,b))return OwnershipCheck::Unknown;
    return InspectBoundary(memory,actor,quiet,false,failure,a!=0U||b!=0U,true);
}
OwnershipCheck InspectInteractionBoundary(const OwnershipMemory& memory,const ActorLifetime& actor,
    OwnershipBoundaryFailure* failure) noexcept {return InspectBoundary(memory,actor,false,true,failure);}
OwnershipCheck InspectAuthoritativeExecutionBoundary(const OwnershipMemory& memory,const ActorLifetime& actor,bool quiet,
    OwnershipBoundaryFailure* failure) noexcept {return InspectBoundary(memory,actor,quiet,true,failure);}
}
