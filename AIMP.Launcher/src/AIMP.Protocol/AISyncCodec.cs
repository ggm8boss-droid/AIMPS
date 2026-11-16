using System.Buffers.Binary;

namespace AIMP.Protocol;

// Generic high-level NPC authority, not a serialized Behaviour Tree, animation
// stream or WorldMutation. Fixed-size payloads carry no process addresses,
// ObjectRef pointers, clips, audio events or mutable BT state. Retail/CATHODE
// regenerate presentation locally; resolved paired health/outcomes come from Host.
public static class AISyncCodec
{
    public const uint StateVersion = 16;
    public const uint ImpactVersion = 2;
    public const int NativeStateBytes = 840;
    public const int NativeImpactBytes = 64;
    public const int ConfigurationBytes = 1096;
    public const int NetworkStateBytes = 16 + NativeStateBytes;
    public const int NetworkImpactBytes = 16 + NativeImpactBytes;
    public const int MaximumActors = 256;
    public const int MaximumParticipants = 32;
    private const uint PoseValid = 1U << 0;
    private const uint Moving = 1U << 1;
    private const uint ActionValid = 1U << 2;
    private const uint InteractionValid = 1U << 3;
    private const uint RoleTaskValid = 1U << 4;
    private const uint ExecutionValid = 1U << 5;
    public const uint ConsequenceEvent = 1U << 6;
    public const uint ParticipantConsequenceEvent = 1U << 7;
    public const uint ShotEvent = 1U << 8;
    public const uint NavigationEvent = 1U << 9;
    private const uint KnownStateFlags = PoseValid | Moving | ActionValid | InteractionValid | RoleTaskValid | ExecutionValid
        | ConsequenceEvent | ParticipantConsequenceEvent | ShotEvent | NavigationEvent;
    private const uint ExecutionTransition = 1U << 0;
    private const uint ExecutionCertified = 1U << 1;
    private const uint ExecutionPaired = 1U << 2;
    private const uint KnownExecutionFlags = ExecutionTransition | ExecutionCertified | ExecutionPaired;
    private const uint KnownLifecycleFlags = 63U; // includes Character backstage (bit 5)
    private const uint InteractionPairValid = 1U << 2;
    private const uint KnownInteractionFlags = 63U; // includes retained source cycle (bit 5)
    public const uint MotionValidSample = 1U << 0;
    public const uint MotionNativeActionOwnsMotion = 1U << 1;
    public const uint MotionDiscontinuity = 1U << 2;
    private const uint KnownMotionFlags = MotionValidSample | MotionNativeActionOwnsMotion | MotionDiscontinuity;

    public static ulong SessionCookie(Guid session)
    {
        Span<byte> bytes = stackalloc byte[16];
        session.TryWriteBytes(bytes);
        var cookie = BinaryPrimitives.ReadUInt64LittleEndian(bytes);
        return cookie == 0 ? 1UL : cookie;
    }

    public static void ValidateNative(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != NativeStateBytes || U32(bytes, 0) != StateVersion || U32(bytes, 4) == 0
            || BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == 0 || U32(bytes, 16) == 0
            || (U32(bytes, 20) == 0 && U32(bytes, 24) == 0) || U32(bytes, 32) == 0
            || U32(bytes, 36) == 0
            || !AllowedCharacterClass(U32(bytes, 28)))
            throw new ProtocolException("Invalid AISync authority snapshot v16.");
        ValidateAuthorityState(bytes);
        ValidateMotion(bytes);
    }

    // v16 appends the Host's GetTickCount64 sample at 816, actual physical
    // velocity XYZ at 824/828/832 and flags at 836. The tick is Host-local;
    // it does not establish a wall-clock delay or carry a native address.
    private static void ValidateMotion(ReadOnlySpan<byte> bytes)
    {
        var flags = U32(bytes, 836);
        if ((flags & ~KnownMotionFlags) != 0)
            throw new ProtocolException("Invalid AISync motion flags.");
        for (var offset = 824; offset <= 832; offset += 4)
            if (!BoundedFloat(bytes, offset, -128F, 128F))
                throw new ProtocolException("Invalid AISync sampled physical velocity.");
        if ((flags & MotionValidSample) == 0)
        {
            foreach (var value in bytes.Slice(816, 24))
                if (value != 0) throw new ProtocolException("Inactive AISync motion sample has a payload.");
        }
        else if (BinaryPrimitives.ReadUInt64LittleEndian(bytes[816..]) == 0 || (U32(bytes, 40) & PoseValid) == 0)
            throw new ProtocolException("AISync motion sample requires a Host tick and valid pose.");
    }

    // v15 block at 764: the Host's native cycle constructor call. The native
    // side owns the contract table; here the carrier must be canonical.
    private static void ValidateCycleCall(ReadOnlySpan<byte> bytes)
    {
        var rva = U32(bytes, 764);
        if (rva == 0)
        {
            foreach (var value in bytes.Slice(764, 52))
                if (value != 0) throw new ProtocolException("Invalid AISync cycle call padding.");
            return;
        }
        var kinds = U32(bytes, 768);
        if ((kinds >> 8) != 0) throw new ProtocolException("Invalid AISync cycle call arity.");
        for (var i = 0; i < 2; ++i)
        {
            var kind = (kinds >> (i * 4)) & 0xF;
            if (kind > 5 && kind != 15) throw new ProtocolException("Invalid AISync cycle call argument.");
        }
        for (var i = 0; i < 4; ++i)
            if (!float.IsFinite(BinaryPrimitives.ReadSingleLittleEndian(bytes[(796 + i * 4)..])))
                throw new ProtocolException("Invalid AISync cycle call vector.");
        if (U32(bytes, 812) > 0xFF) throw new ProtocolException("Invalid AISync cycle call serial.");
    }

    private static void ValidateAuthorityState(ReadOnlySpan<byte> bytes)
    {
        AISyncActionCorrelation.ValidateState(bytes);
        var causeSession=new Guid(bytes.Slice(644,16));
        var authority=BinaryPrimitives.ReadUInt64LittleEndian(bytes[660..]);
        var command=BinaryPrimitives.ReadUInt64LittleEndian(bytes[668..]);
        if(causeSession!=Guid.Empty||authority!=0||command!=0)
            if(causeSession==Guid.Empty||authority==0||command==0||(U32(bytes,40)&InteractionValid)==0||
                SessionCookie(causeSession)!=BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]))
                throw new ProtocolException("Invalid registration-time interaction authority seal.");
        ValidateNavigation(bytes);
        ValidateCycleCall(bytes);
        ValidateConsequences(bytes);
        ValidateScripted(bytes);
        ValidateCombat(bytes);
        var participantPeer = BinaryPrimitives.ReadUInt64LittleEndian(bytes[472..]);
        if (!ValidDamage(bytes, 480) || (participantPeer != 0) != (U32(bytes, 484) != 0)
            || (U32(bytes, 40) & ParticipantConsequenceEvent) != 0 && participantPeer == 0)
            throw new ProtocolException("Invalid AISync participant consequence.");
        var flags = U32(bytes, 40);
        if ((flags & NavigationEvent) != 0 && (U32(bytes, 260) == 0 || U32(bytes, 288) is not (4 or 7)))
            throw new ProtocolException("Invalid AISync ordered navigation event.");
        if (U32(bytes, 636) != 0 && (U32(bytes, 320) == 0
            || BinaryPrimitives.ReadUInt64LittleEndian(bytes[332..]) != 0 || U32(bytes, 352) == 0))
            throw new ProtocolException("Invalid AISync paired health provenance.");
        var locomotionIntensity = Float(bytes, 72);
        var locomotionRevision = U32(bytes, 76);
        if ((flags & ~KnownStateFlags) != 0 || U32(bytes, 68) > 3
            || !float.IsFinite(locomotionIntensity) || locomotionIntensity is < 0F or > 64F
            || ((flags & Moving) != 0 && locomotionRevision == 0)
            || U32(bytes, 92) > 10 || U32(bytes, 96) > 64
            || (flags & Moving) != 0 && (flags & PoseValid) == 0)
            throw new ProtocolException("Invalid AISync Character state.");
        ValidatePose(bytes);
        if ((flags & ActionValid) != 0
            && (U32(bytes, 80) == 0 || U32(bytes, 88) == 0 || U32(bytes, 96) == 0))
            throw new ProtocolException("Invalid AISync action identity.");
        var interactionVariant = I32(bytes, 132);
        var interactionFlags = U32(bytes, 112);
        var targetPeer = BinaryPrimitives.ReadUInt64LittleEndian(bytes[172..]);
        if(U32(bytes,640)!=0&&((flags&InteractionValid)==0||(interactionFlags&2U)==0||targetPeer==0))
            throw new ProtocolException("Participant incarnation requires an identified player interaction.");
        var transitionVariant = I32(bytes, 180);
        if ((interactionFlags & ~KnownInteractionFlags) != 0
            || U32(bytes, 116) > 2 || (U32(bytes, 120) & ~0xFFFFU) != 0
            || interactionVariant is < -1 or > 0xFFFF || transitionVariant is < -1 or > 0xFFFF)
            throw new ProtocolException("Invalid AISync Character interaction.");
        for (var offset = 136; offset <= 144; offset += 4)
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000)
                throw new ProtocolException("Invalid AISync interaction target transform.");
        if ((flags & InteractionValid) != 0
            && (U32(bytes, 100) == 0 || U32(bytes, 104) == 0 && U32(bytes, 108) == 0
                || U32(bytes, 184) == 0 || (interactionFlags & InteractionPairValid) == 0
                || ((interactionFlags & 2U) != 0) != (targetPeer != 0)))
            throw new ProtocolException("Invalid AISync interaction identity.");
        if ((flags & InteractionValid) != 0 && ((interactionFlags & 2U) != 0
                ? U32(bytes, 544) != 0 || U32(bytes, 548) != 0
                : U32(bytes, 544) == 0 || !AllowedCharacterClass(U32(bytes, 548))))
            throw new ProtocolException("Invalid AISync NPC victim incarnation.");
        if ((flags & InteractionValid) == 0 && U32(bytes, 116) != 0)
            throw new ProtocolException("AISync transition requires an interaction event.");
        if ((interactionFlags & 32U) != 0 && ((flags & InteractionValid) == 0
            || U32(bytes, 116) == 0 || (interactionFlags & InteractionPairValid) == 0))
            throw new ProtocolException("AISync retained source requires an identified paired transition.");
        if (U32(bytes, 116) != 0 && U32(bytes, 188) == 0)
            throw new ProtocolException("AISync transition requires its own revision.");
        if ((flags & RoleTaskValid) != 0
            && (U32(bytes, 148) == 0 || U32(bytes, 152) == 0))
            throw new ProtocolException("Invalid AISync role task identity.");
        if ((U32(bytes, 168) & ~KnownLifecycleFlags) != 0
            || U32(bytes, 168) != 0 && U32(bytes, 164) == 0)
            throw new ProtocolException("Invalid AISync lifecycle flags.");
        var outcomeRevision = U32(bytes, 192);
        var outcomeFlags = U32(bytes, 196);
        var kind = U32(bytes, 204);
        var hpBefore = U32(bytes, 208);
        var hpPrepared = U32(bytes, 212);
        var hpReaction = U32(bytes, 216);
        var healthReaction = U32(bytes, 220);
        if ((outcomeFlags & ~63U) != 0 || U32(bytes, 200) > 0xFFFF
            || kind is not (0 or 1 or 3) || hpBefore > 0xFFFF || hpPrepared > 0xFFFF
            || hpReaction > 0xFFFF || healthReaction > 4
            || (outcomeRevision == 0) != (outcomeFlags == 0)
            || (outcomeFlags & 32U) != 0 && (outcomeFlags & 2U) == 0
            || ((outcomeFlags & 16U) != 0) != (outcomeRevision != 0 && hpReaction == 0 && healthReaction == 4)
            || outcomeRevision != 0 && ((flags & InteractionValid) == 0
                || targetPeer == 0 && (U32(bytes, 544) == 0 || !AllowedCharacterClass(U32(bytes, 548)))
                || U32(bytes, 116) == 0 || (interactionFlags & 16U) == 0
                || (outcomeFlags & 1U) == 0 || hpBefore == 0 || hpPrepared > hpBefore))
            throw new ProtocolException("Invalid AISync native attack outcome.");
        for (var offset = 224; offset <= 236; offset += 4)
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000)
                throw new ProtocolException("Invalid AISync native outcome geometry.");
        var executionRevision = U32(bytes, 240);
        var executionType = U32(bytes, 244);
        var executionState = U32(bytes, 248);
        var executionFlags = U32(bytes, 252);
        if (executionState > 0xFF || (executionFlags & ~KnownExecutionFlags) != 0
            || executionFlags != 0 && (flags & ExecutionValid) == 0
            || (executionFlags & ExecutionCertified) != 0 && (executionFlags & ExecutionTransition) == 0
            || (flags & ExecutionValid) != 0 && (executionRevision == 0 || executionType == 0))
            throw new ProtocolException("Invalid AISync execution-cycle authority.");
    }

    private static void ValidateNavigation(ReadOnlySpan<byte> bytes)
    {
        var flags = U32(bytes, 260);
        var kind = U32(bytes, 288);
        var stop = Float(bytes, 276);
        var heading = Float(bytes, 292);
        var tolerance = Float(bytes, 296);
        var distance = Float(bytes, 300);
        if ((flags & ~3U) != 0 || U32(bytes, 280) > 3 || !float.IsFinite(stop) || stop is < -1F or > 1000F
            || !float.IsFinite(heading) || Math.Abs(heading) > 6.4F
            || !float.IsFinite(tolerance) || tolerance is < 0F or > 6.4F
            || !float.IsFinite(distance) || distance is < 0F or > 100000F
            || flags != 0 && (U32(bytes, 256) == 0 || U32(bytes, 284) == 0))
            throw new ProtocolException("Invalid AISync native navigation parameters.");
        foreach (var offset in new[] { 264, 268, 272, 304, 308, 312 })
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000F)
                throw new ProtocolException("Invalid AISync native navigation coordinates.");
        var positionZero = ZeroFloats(bytes, 264, 3);
        var originZero = ZeroFloats(bytes, 304, 3);
        var valid = flags == 0
            ? kind == 0 && U32(bytes, 280) == 0 && stop == 0 && heading == 0 && tolerance == 0
                && distance == 0 && positionZero && originZero
            : kind switch
            {
                1 => flags == 1 && heading == 0 && tolerance == 0 && distance == 0 && originZero,
                4 => (flags & 1) != 0 && stop == 0 && distance == 0 && originZero
                    && ((flags & 2) != 0 ? heading == 0 : positionZero),
                7 => flags == 1 && U32(bytes, 280) <= 2 && stop == 0 && heading == 0 && tolerance == 0
                    && distance == 0 && positionZero && originZero,
                9 => (flags & 1) != 0 && ((flags & 2) != 0 || positionZero),
                _ => false
            };
        if (!valid) throw new ProtocolException("Invalid AISync native navigation request kind.");
    }

    private static bool ZeroFloats(ReadOnlySpan<byte> bytes, int offset, int count)
    {
        for (var index = 0; index < count; ++index)
            if (Float(bytes, offset + index * 4) != 0) return false;
        return true;
    }

    private static void ValidateScripted(ReadOnlySpan<byte> bytes)
    {
        if (U32(bytes, 420) == 0)
        {
            for (var offset = 424; offset < 472; offset += 4)
                if (U32(bytes, offset) != 0) throw new ProtocolException("Inactive AISync scripted animation has parameters.");
            return;
        }
        if (U32(bytes, 420) != 1 || U32(bytes, 424) == 0 || U32(bytes, 428) == 0
            || (U32(bytes, 468) & ~0x0302BE7FU) != 0 || (U32(bytes, 40) & ExecutionValid) == 0
            || U32(bytes, 244) != 0x00EADB58U || U32(bytes, 248) != 0x12U
            || I32(bytes, 432) is < -1 or > 1000000 || I32(bytes, 436) is < -1 or > 1000000
            || I32(bytes, 440) is < -1 or > 65535
            || !BoundedFloat(bytes, 444, 0F, 64F) || !BoundedFloat(bytes, 448, 0F, 60F)
            || !BoundedFloat(bytes, 456, -1F, 1000000F) || !BoundedFloat(bytes, 460, 0F, 60F)
            || !BoundedFloat(bytes, 464, 0F, 64F))
            throw new ProtocolException("Invalid AISync native scripted animation parameters.");
    }

    private static bool BoundedFloat(ReadOnlySpan<byte> bytes, int offset, float minimum, float maximum)
    {
        var value = Float(bytes, offset);
        return float.IsFinite(value) && value >= minimum && value <= maximum;
    }

    private static bool ValidCombatState(ReadOnlySpan<byte> bytes, int offset)
    {
        var flags = U32(bytes, offset + 4);
        if ((flags & ~7U) != 0 || U32(bytes, offset + 8) > 10 || U32(bytes, offset + 12) > 0xFFFF
            || U32(bytes, offset + 32) > 7 || !BoundedFloat(bytes, offset + 28, 0F, 3.141593F)) return false;
        for (var index = 16; index <= 24; index += 4)
            if (!BoundedFloat(bytes, offset + index, -100000F, 100000F)) return false;
        if (flags == 0)
        {
            for (var index = 4; index < 36; index += 4)
                if (U32(bytes, offset + index) != 0) return false;
            return true;
        }
        return (flags & 1) != 0 && U32(bytes, offset) != 0
            && ((flags & 4) != 0 || ZeroFloats(bytes, offset + 16, 3) && Float(bytes, offset + 28) == 0)
            && ((flags & 2) == 0 || (flags & 4) != 0);
    }

    private static void ValidateCombat(ReadOnlySpan<byte> bytes)
    {
        if (!ValidCombatState(bytes, 552) || !ValidCombatState(bytes, 596))
            throw new ProtocolException("Invalid AISync combat presentation state.");
        var shotFlags = U32(bytes, 592);
        if (shotFlags == 0)
        {
            if (U32(bytes, 588) != 0 || U32(bytes, 596) != 0 || U32(bytes, 600) != 0 || U32(bytes, 632) != 0)
                throw new ProtocolException("Inactive AISync shot event has a payload.");
        }
        else if (shotFlags != 1 || U32(bytes, 588) == 0 || (U32(bytes, 600) & 5U) != 5U
            || U32(bytes, 632) is 0 or > 0xFFFF)
            throw new ProtocolException("Invalid AISync authoritative shot.");
        if ((U32(bytes, 40) & ShotEvent) != 0 && shotFlags == 0)
            throw new ProtocolException("AISync live shot is missing its event.");
    }

    private static bool ValidHealth(ReadOnlySpan<byte> bytes, int offset)
    {
        var revision = U32(bytes, offset);
        var flags = U32(bytes, offset + 4);
        var hp = U32(bytes, offset + 8);
        var phase = U32(bytes, offset + 12);
        return flags == 0 ? revision == 0 && hp == 0 && phase == 0
            : flags == 1 && revision != 0 && hp <= 0xFFFF && phase <= 4 && ((hp == 0) == (phase == 4));
    }

    private static bool ValidDamage(ReadOnlySpan<byte> bytes, int offset)
    {
        if (U32(bytes, offset + 4) == 0)
        {
            for (var index = 0; index < 64; index += 4)
                if (U32(bytes, offset + index) != 0) return false;
            return true;
        }
        var hp = U32(bytes, offset + 40);
        var phase = U32(bytes, offset + 44);
        if (U32(bytes, offset + 4) == 3)
            for (var index = 8; index <= 36; index += 4)
                if (U32(bytes, offset + index) != 0) return false;
        return (U32(bytes, offset + 4) is 1 or 3) && U32(bytes, offset) != 0
            && U32(bytes, offset + 8) <= 0xFFFF && U32(bytes, offset + 16) <= 255
            && U32(bytes, offset + 20) <= 255 && U32(bytes, offset + 24) <= 255
            && U32(bytes, offset + 28) <= 255 && U32(bytes, offset + 32) <= 255 && U32(bytes, offset + 36) <= 255
            && hp <= 0xFFFF && phase <= 4 && ((hp == 0) == (phase == 4))
            && U32(bytes, offset + 52) == 1 && ValidHealth(bytes, offset + 48);
    }

    private static void ValidateConsequences(ReadOnlySpan<byte> bytes)
    {
        var causePeer = BinaryPrimitives.ReadUInt64LittleEndian(bytes[332..]);
        var npcCause = U32(bytes, 340) != 0 || U32(bytes, 344) != 0;
        var eventFlags = U32(bytes, 360);
        if (!ValidHealth(bytes, 316) || !ValidDamage(bytes, 356)
            || causePeer != 0 && (npcCause || U32(bytes, 348) != 0 || U32(bytes, 352) != 0)
            || npcCause != (U32(bytes, 352) != 0)
            || (npcCause ? !AllowedCharacterClass(U32(bytes, 348)) : U32(bytes, 348) != 0)
            || (causePeer != 0 || npcCause) && U32(bytes, 320) == 0
            || (U32(bytes, 40) & ConsequenceEvent) != 0 && eventFlags == 0)
            throw new ProtocolException("Invalid AISync health authority or cause.");
        if (eventFlags == 0)
        {
            for (var offset = 356; offset < 420; offset += 4)
                if (U32(bytes, offset) != 0) throw new ProtocolException("Inactive AISync damage event has a payload.");
            return;
        }
        if (U32(bytes, 320) == 0 || U32(bytes, 404) > U32(bytes, 316))
            throw new ProtocolException("Invalid AISync native damage event.");
    }

    private static void ValidatePose(ReadOnlySpan<byte> bytes)
    {
        for (var offset = 44; offset <= 52; offset += 4)
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000)
                throw new ProtocolException("Invalid AISync Character transform.");
        var yaw = Float(bytes, 56);
        var velocityX = Float(bytes, 60);
        var velocityZ = Float(bytes, 64);
        if (!float.IsFinite(yaw) || Math.Abs(yaw) > 3.3F
            || !float.IsFinite(velocityX) || Math.Abs(velocityX) > 20F
            || !float.IsFinite(velocityZ) || Math.Abs(velocityZ) > 20F)
            throw new ProtocolException("Invalid AISync Character motion.");
    }

    public static bool BelongsTo(ReadOnlySpan<byte> bytes, Guid session, int worldGeneration) =>
        bytes.Length == NativeStateBytes && session != Guid.Empty && worldGeneration > 0
        && BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == SessionCookie(session)
        && MatchesSnapshotSessions(bytes,session)
        && U32(bytes, 16) == (uint)worldGeneration;

    public static ulong ActorKey(ReadOnlySpan<byte> bytes) =>
        ((ulong)U32(bytes, 20) << 32) | U32(bytes, 24);

    public static (uint Birth, uint Revision) ActorRevision(ReadOnlySpan<byte> bytes) =>
        (U32(bytes, 32), U32(bytes, 36));

    // Irreversible native results cannot be merged like motion snapshots.
    public static (uint Birth, uint Pair, uint Revision) OutcomeKey(ReadOnlySpan<byte> bytes) =>
        (U32(bytes, 32), U32(bytes, 184), U32(bytes, 192));

    public static (uint Birth, uint Revision) DamageKey(ReadOnlySpan<byte> bytes) =>
        (U32(bytes, 32), (U32(bytes, 40) & ConsequenceEvent) != 0 ? U32(bytes, 356) : 0);

    public static (uint Birth, uint Revision) ParticipantDamageKey(ReadOnlySpan<byte> bytes) =>
        (U32(bytes, 32), (U32(bytes, 40) & ParticipantConsequenceEvent) != 0 ? U32(bytes, 480) : 0);

    public static (uint Birth, uint Revision) ShotKey(ReadOnlySpan<byte> bytes) =>
        (U32(bytes, 32), (U32(bytes, 40) & ShotEvent) != 0 ? U32(bytes, 588) : 0);

    public static (uint Birth, uint Revision) NavigationKey(ReadOnlySpan<byte> bytes) =>
        (U32(bytes, 32), (U32(bytes, 40) & NavigationEvent) != 0 ? U32(bytes, 256) : 0);

    public static byte[] EncodeNetwork(Guid session, ReadOnlySpan<byte> native)
    {
        ValidateNative(native);
        if (session == Guid.Empty || BinaryPrimitives.ReadUInt64LittleEndian(native[8..]) != SessionCookie(session)||!MatchesSnapshotSessions(native,session))
            throw new ProtocolException("AISync session identity mismatch.");
        var bytes = new byte[NetworkStateBytes];
        session.TryWriteBytes(bytes.AsSpan(0, 16));
        native.CopyTo(bytes.AsSpan(16));
        return bytes;
    }

    public static ReadOnlyMemory<byte> DecodeNetwork(ReadOnlyMemory<byte> bytes, Guid session)
    {
        if (bytes.Length != NetworkStateBytes || new Guid(bytes.Span[..16]) != session)
            throw new ProtocolException("AISync network session mismatch.");
        var native = bytes[16..];
        ValidateNative(native.Span);
        if (BinaryPrimitives.ReadUInt64LittleEndian(native.Span[8..]) != SessionCookie(session)||!MatchesSnapshotSessions(native.Span,session))
            throw new ProtocolException("AISync cookie mismatch.");
        return native;
    }
    internal static bool MatchesSnapshotSessions(ReadOnlySpan<byte> bytes,Guid session)
    {
        var cause=new Guid(bytes.Slice(644,16));var action=new Guid(bytes.Slice(676,16));
        return (cause==Guid.Empty||cause==session)&&(action==Guid.Empty||action==session);
    }

    public static void ValidateNativeImpact(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != NativeImpactBytes || U32(bytes, 0) != ImpactVersion || U32(bytes, 4) == 0
            || BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == 0 || U32(bytes, 16) == 0
            || U32(bytes, 20) == 0 || BinaryPrimitives.ReadUInt64LittleEndian(bytes[24..]) == 0
            || (U32(bytes, 32) == 0 && U32(bytes, 36) == 0) || !AllowedCharacterClass(U32(bytes, 40))
            || U32(bytes, 44) == 0 || !ValidImpactPayload(U32(bytes, 48), U32(bytes, 52), U32(bytes, 56), U32(bytes, 60)))
            throw new ProtocolException("Invalid AISync client impact.");
    }

    // Channels 1/2 carry HP damage. Channel 3 is the exact zero-damage stimulus
    // (flame repel): damage 0, 16-bit descriptor, reserved = zone | pellets << 8.
    private static bool ValidImpactPayload(uint damage, uint descriptor, uint channel, uint reserved) =>
        channel switch
        {
            1 or 2 => damage is > 0 and <= 0xFFFF && reserved == 0,
            3 => damage == 0 && descriptor <= 0xFFFF && (reserved & 0xFF) < 0x80
                && (reserved >> 8) is >= 1 and <= 16,
            _ => false
        };

    public static bool ImpactBelongsTo(ReadOnlySpan<byte> bytes, Guid session, int worldGeneration) =>
        bytes.Length == NativeImpactBytes && session != Guid.Empty && worldGeneration > 0
        && BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == SessionCookie(session)
        && U32(bytes, 16) == (uint)worldGeneration;

    public static ulong ImpactSourcePeer(ReadOnlySpan<byte> bytes) =>
        BinaryPrimitives.ReadUInt64LittleEndian(bytes[24..]);

    public static uint ImpactSequence(ReadOnlySpan<byte> bytes) => U32(bytes, 20);

    public static byte[] EncodeImpactNetwork(Guid session, ReadOnlySpan<byte> native)
    {
        ValidateNativeImpact(native);
        if (session == Guid.Empty || BinaryPrimitives.ReadUInt64LittleEndian(native[8..]) != SessionCookie(session))
            throw new ProtocolException("AISync impact session identity mismatch.");
        var bytes = new byte[NetworkImpactBytes];
        session.TryWriteBytes(bytes.AsSpan(0, 16));
        native.CopyTo(bytes.AsSpan(16));
        return bytes;
    }

    public static ReadOnlyMemory<byte> DecodeImpactNetwork(ReadOnlyMemory<byte> bytes, Guid session)
    {
        if (bytes.Length != NetworkImpactBytes || new Guid(bytes.Span[..16]) != session)
            throw new ProtocolException("AISync impact network session mismatch.");
        var native = bytes[16..];
        ValidateNativeImpact(native.Span);
        if (BinaryPrimitives.ReadUInt64LittleEndian(native.Span[8..]) != SessionCookie(session))
            throw new ProtocolException("AISync impact cookie mismatch.");
        return native;
    }

    public static byte[] Configuration(uint role, int nativeGeneration, int worldGeneration, Guid session,
        ulong localPeer, IReadOnlyDictionary<ulong, uint> participants, bool alienEnabled = true,
        AISyncParticipantBinding? localBinding = null,IReadOnlyDictionary<ulong,AISyncParticipantBinding>? certifiedBindings = null, bool alienVsSurvivors = false)
    {
        if (role > 3 || nativeGeneration < 0 || worldGeneration < 0)
            throw new ArgumentOutOfRangeException(nameof(role));
        if (role != 0 && (session == Guid.Empty || localPeer == 0 ||
            (role != 3 && (nativeGeneration == 0 || worldGeneration == 0))))
            throw new ProtocolException("AISync configuration needs a live generation.");
        if (participants.Count > MaximumParticipants)
            throw new ProtocolException("Too many AISync participant bindings.");
        var bytes = new byte[ConfigurationBytes];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 3);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), role);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), (uint)nativeGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), (uint)worldGeneration);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16), role == 0 ? 0 : SessionCookie(session));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), localPeer);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(32), (uint)participants.Count);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(36), role == 0 ? 0U :
            (!alienEnabled ? 1U : 0U) | (alienVsSurvivors ? 2U : 0U));
        var offset = 72;
        var characters = new HashSet<uint>();
        if(localBinding is not null)
        {
            if(localBinding.Incarnation==0)throw new ProtocolException("Local binding needs its portable incarnation.");
            AISyncParticipantBindingCodec.Write(bytes.AsSpan(40,32),localPeer,localBinding);
            characters.Add(localBinding.Character);
        }
        if(certifiedBindings is not null && certifiedBindings.Keys.Any(peer=>!participants.ContainsKey(peer)))
            throw new ProtocolException("Construction binding has no participant mapping.");
        foreach (var binding in participants.OrderBy(value => value.Key))
        {
            if (binding.Key == 0 || binding.Key == localPeer || binding.Value < 0x10000
                || !characters.Add(binding.Value))
                throw new ProtocolException("Invalid local AISync participant binding.");
            var proof=certifiedBindings is not null&&certifiedBindings.TryGetValue(binding.Key,out var stored)
                ? stored : new AISyncParticipantBinding(binding.Value,0,0,0);
            if(proof.Character!=binding.Value)throw new ProtocolException("Participant address differs from its captured construction.");
            AISyncParticipantBindingCodec.Write(bytes.AsSpan(offset,32),binding.Key,proof);
            offset += 32;
        }
        return bytes;
    }

    private static bool AllowedCharacterClass(uint value) => value is 1 or 2 or 3 or 4 or 6 or 7 or 9;

    private static uint U32(ReadOnlySpan<byte> bytes, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]);
    private static int I32(ReadOnlySpan<byte> bytes, int offset) =>
        BinaryPrimitives.ReadInt32LittleEndian(bytes[offset..]);
    private static float Float(ReadOnlySpan<byte> bytes, int offset) =>
        BitConverter.Int32BitsToSingle((int)U32(bytes, offset));
}
