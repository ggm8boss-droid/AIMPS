using System.Buffers.Binary;

namespace AIMP.Protocol;

// Local IPC only. Incarnation is a local lifetime receipt, NEVER a peer ID.
public enum AlienPvPNativeKind : ushort
{ Status=1, Input=2, Pose=3, Activate=4, Commit=5, Correction=6, Stop=7, ActionResult=8, Resolution=9, Acknowledge=10 }
[Flags]
public enum AlienPvPNativeFlags : uint
{
    None=0, Bound=1, LocalOwner=2, PilotReady=4, Focused=8, Host=16,
    Moving=32, NativeBusy=64, Active=128, Expired=256,
    UnsupportedAction=512, PoseValid=1024, ActionApplied=2048, Backstage=4096, VentSelected=8192, OwnerMeasured=16384, VentAnimating=32768,
    // AvS pass 19: the pilot is in crawl mode or its crawlspace hatch traversal.
    Crawling=65536
}
public readonly record struct AlienPvPNativeScope(uint NativeGeneration,uint WorldGeneration,
    ulong Cookie,ulong OwnerPeer,ulong Incarnation,uint EntityA,uint EntityB);
public sealed record AlienPvPNativeFrame
{
    public AlienPvPNativeKind Kind { get; init; }
    public AlienPvPNativeScope Scope { get; init; }
    public ulong Epoch { get; init; }
    public ulong Sequence { get; init; }
    public ulong HostSequence { get; init; }
    public ulong AcknowledgedInput { get; init; }
    public ulong SampledAtMs { get; init; }
    public AlienPvPNativeFlags Flags { get; init; }
    public uint Gait { get; init; } = 1;
    public AlienAction Action { get; init; }
    public float Forward { get; init; }
    public float Strafe { get; init; }
    public float Yaw { get; init; } // radians on native IPC; peer intents use degrees.
    public float Pitch { get; init; }
    public float X { get; init; }
    public float Y { get; init; }
    public float Z { get; init; }
    public float VelocityX { get; init; }
    public float VelocityZ { get; init; }
    public ulong DeliveryId { get; init; }
    public ulong TargetPeer { get; init; }
    public uint TargetA { get; init; }
    public uint TargetB { get; init; }
    public uint Selector { get; init; }
    public AlienActionReason Reason { get; init; }
    public float ResolvedX { get; init; }
    public float ResolvedY { get; init; }
    public float ResolvedZ { get; init; }
    public AlienActionPhase Phase { get; init; }
    public ulong ResolutionRevision { get; init; }
}
public static class AlienPvPNativeCodec
{
    public const int Length=200;
    public const uint Magic=0x33535641;
    public static void Validate(AlienPvPNativeFrame f)
    {
        var s=f.Scope;
        if(s.NativeGeneration==0 || s.WorldGeneration==0 || s.Cookie==0 || s.OwnerPeer==0 ||
            s.Incarnation==0 || (s.EntityA==0 && s.EntityB==0) ||
            (ushort)f.Kind is <1 or >10 || ((uint)f.Flags & ~131071U)!=0 ||
            f.Gait>3 || (byte)f.Action>5 || (uint)f.Phase>6 || (uint)f.Reason>13)
            throw new ProtocolException("Invalid native PvP identity or kind.");
        foreach(var v in new[]{f.Forward,f.Strafe,f.Yaw,f.Pitch,f.X,f.Y,f.Z,f.VelocityX,f.VelocityZ,f.ResolvedX,f.ResolvedY,f.ResolvedZ})
            if(!float.IsFinite(v))throw new ProtocolException("Non-finite native PvP scalar.");
        if(Math.Abs(f.Forward)>1 || Math.Abs(f.Strafe)>1 || Math.Abs(f.Yaw)>MathF.PI+0.00001F ||
            Math.Abs(f.Pitch)>MathF.PI/2+0.00001F || Math.Abs(f.X)>100000 || Math.Abs(f.Y)>100000 ||
            Math.Abs(f.Z)>100000 || Math.Sqrt((double)f.VelocityX*f.VelocityX+(double)f.VelocityZ*f.VelocityZ)>30.01 ||
            Math.Abs(f.ResolvedX)>100000 || Math.Abs(f.ResolvedY)>100000 || Math.Abs(f.ResolvedZ)>100000 ||
            (f.Kind!=AlienPvPNativeKind.Status && f.Epoch==0) ||
            ((f.Kind is AlienPvPNativeKind.Input or AlienPvPNativeKind.Commit or AlienPvPNativeKind.Pose or
                AlienPvPNativeKind.Correction or AlienPvPNativeKind.ActionResult or AlienPvPNativeKind.Resolution) && f.Sequence==0) ||
            ((f.Kind is AlienPvPNativeKind.Commit or AlienPvPNativeKind.Resolution) && f.HostSequence==0) ||
            (f.Kind is not (AlienPvPNativeKind.Input or AlienPvPNativeKind.Commit or AlienPvPNativeKind.ActionResult or AlienPvPNativeKind.Resolution)
                && f.Action!=AlienAction.None))
            throw new ProtocolException("Invalid native PvP value or ordering identity.");
        if(f.Flags.HasFlag(AlienPvPNativeFlags.VentSelected) &&
            (f.Kind is not (AlienPvPNativeKind.Input or AlienPvPNativeKind.Commit) || f.Action is not (AlienAction.ExitVent or AlienAction.EnterVent)))
            throw new ProtocolException("Invalid native selected vent action.");
        if(f.Flags.HasFlag(AlienPvPNativeFlags.VentAnimating) &&
            (f.Kind is not (AlienPvPNativeKind.Pose or AlienPvPNativeKind.Correction) ||
             !f.Flags.HasFlag(AlienPvPNativeFlags.PoseValid|AlienPvPNativeFlags.Backstage|AlienPvPNativeFlags.NativeBusy)))
            throw new ProtocolException("Invalid native ventilation animation observation.");
        if(f.Kind==AlienPvPNativeKind.Acknowledge &&
            (f.DeliveryId==0 || f.Sequence!=0 || f.HostSequence!=0 || f.Flags!=0))
            throw new ProtocolException("Invalid native delivery acknowledgement.");
        if(f.Kind==AlienPvPNativeKind.Resolution &&
            (f.Action==AlienAction.None || f.Phase==AlienActionPhase.None || f.ResolutionRevision==0))
            throw new ProtocolException("Native action resolution lacks its action/phase/order.");
        if(f.Kind==AlienPvPNativeKind.Resolution) {
            var success=f.Phase is AlienActionPhase.Preparing or AlienActionPhase.Started or AlienActionPhase.Completed;
            if(f.Action==AlienAction.Roar || (f.Phase==AlienActionPhase.Preparing && f.Action!=AlienAction.Attack) ||
                success!=(f.Reason==AlienActionReason.None) ||
                (f.Action==AlienAction.Attack && success && (f.TargetPeer==0 || f.TargetPeer==s.OwnerPeer ||
                    (f.TargetA==0 && f.TargetB==0) || f.Selector>65535)) ||
                (f.Action!=AlienAction.Attack && (f.TargetPeer!=0 || f.TargetA!=0 || f.TargetB!=0 || f.Selector!=0)))
                throw new ProtocolException("Invalid native action result target/phase/reason.");
        }
        if(f.Kind is not (AlienPvPNativeKind.Resolution or AlienPvPNativeKind.ActionResult) &&
            (f.TargetPeer!=0 || f.TargetA!=0 || f.TargetB!=0 || f.Selector!=0 || f.Reason!=AlienActionReason.None ||
             f.Phase!=AlienActionPhase.None || f.ResolutionRevision!=0 || f.ResolvedX!=0 || f.ResolvedY!=0 || f.ResolvedZ!=0))
            throw new ProtocolException("Action fields are reserved outside a resolution/receipt.");
    }
    public static byte[] Encode(AlienPvPNativeFrame f)
    {
        Validate(f);var b=new byte[Length];var s=f.Scope;
        void U32(int o,uint v)=>BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(o,4),v);
        void U64(int o,ulong v)=>BinaryPrimitives.WriteUInt64LittleEndian(b.AsSpan(o,8),v);
        void F32(int o,float v)=>BinaryPrimitives.WriteSingleLittleEndian(b.AsSpan(o,4),v);
        U32(0,Magic);U32(4,3U|((uint)f.Kind<<16));
        U32(8,s.NativeGeneration);U32(12,s.WorldGeneration);U64(16,s.Cookie);U64(24,s.OwnerPeer);
        U64(32,f.Epoch);U64(40,s.Incarnation);U64(48,f.Sequence);U64(56,f.HostSequence);
        U64(64,f.AcknowledgedInput);U32(72,s.EntityA);U32(76,s.EntityB);
        U32(80,(uint)f.Flags);U32(84,f.Gait);U32(88,(uint)f.Action);
        F32(96,f.Forward);F32(100,f.Strafe);F32(104,f.Yaw);F32(108,f.Pitch);
        F32(112,f.X);F32(116,f.Y);F32(120,f.Z);F32(124,f.VelocityX);F32(128,f.VelocityZ);
        U64(136,f.SampledAtMs);U64(144,f.TargetPeer);U32(152,f.TargetA);U32(156,f.TargetB);
        U32(160,f.Selector);U32(164,(uint)f.Reason);F32(168,f.ResolvedX);F32(172,f.ResolvedY);F32(176,f.ResolvedZ);
        U32(180,(uint)f.Phase);U64(184,f.ResolutionRevision);U64(192,f.DeliveryId);return b;
    }
    public static AlienPvPNativeFrame Decode(ReadOnlySpan<byte> b)
    {
        if(b.Length!=Length || BinaryPrimitives.ReadUInt32LittleEndian(b)!=Magic ||
            BinaryPrimitives.ReadUInt16LittleEndian(b[4..])!=3 ||
            BinaryPrimitives.ReadUInt32LittleEndian(b[92..])!=0 ||
            BinaryPrimitives.ReadUInt32LittleEndian(b[132..])!=0)
            throw new ProtocolException("Invalid AVS3 native frame size/version/reserved fields.");
        // Static helpers avoid capturing a ref-like ReadOnlySpan in closures.
        static uint U32(ReadOnlySpan<byte> p,int o)=>BinaryPrimitives.ReadUInt32LittleEndian(p[o..]);
        static ulong U64(ReadOnlySpan<byte> p,int o)=>BinaryPrimitives.ReadUInt64LittleEndian(p[o..]);
        static float F32(ReadOnlySpan<byte> p,int o)=>BinaryPrimitives.ReadSingleLittleEndian(p[o..]);
        var f=new AlienPvPNativeFrame {
            Kind=(AlienPvPNativeKind)BinaryPrimitives.ReadUInt16LittleEndian(b[6..]),
            Scope=new(U32(b,8),U32(b,12),U64(b,16),U64(b,24),U64(b,40),U32(b,72),U32(b,76)),
            Epoch=U64(b,32),Sequence=U64(b,48),HostSequence=U64(b,56),AcknowledgedInput=U64(b,64),
            Flags=(AlienPvPNativeFlags)U32(b,80),Gait=U32(b,84),Action=(AlienAction)U32(b,88),
            Forward=F32(b,96),Strafe=F32(b,100),Yaw=F32(b,104),Pitch=F32(b,108),
            X=F32(b,112),Y=F32(b,116),Z=F32(b,120),VelocityX=F32(b,124),VelocityZ=F32(b,128),SampledAtMs=U64(b,136),
            TargetPeer=U64(b,144),TargetA=U32(b,152),TargetB=U32(b,156),Selector=U32(b,160),
            Reason=(AlienActionReason)U32(b,164),ResolvedX=F32(b,168),ResolvedY=F32(b,172),ResolvedZ=F32(b,176),
            Phase=(AlienActionPhase)U32(b,180),ResolutionRevision=U64(b,184),DeliveryId=U64(b,192)
        };
        // The network enum is a byte, but the IPC action is a full u32.
        if(U32(b,88)>5)throw new ProtocolException("Invalid native action width.");
        Validate(f);return f;
    }
}
