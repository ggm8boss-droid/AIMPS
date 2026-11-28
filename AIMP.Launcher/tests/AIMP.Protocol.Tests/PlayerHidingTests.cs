using AIMP.Launcher;
using System.Text;

namespace AIMP.Protocol.Tests;

internal static class PlayerHidingTests
{
    private static readonly PlayerHidingSnapshot Hidden = new(true, 10, 20, 30), Visible = new(false);
    private static readonly DateTimeOffset Now = DateTimeOffset.FromUnixTimeMilliseconds(1_800_000_000_000);
    private static PlayerPositionSnapshotPayload Position(ulong sequence, PlayerHidingSnapshot? hiding) =>
        new(Guid.Parse("d4b36fc5-c75c-479f-911b-bcc509efb874"), 2, 1, sequence,
            Now.ToUnixTimeMilliseconds() + (long)sequence, 10 + sequence, 20, 30, 0,
            PlayerMoveType.Walk, 1.15F, CharacterIncarnation: 1, Hiding: hiding);

    internal static Task WireAsync()
    {
        var presentation = new PlayerHidingPresentationSnapshot(PlayerHidingPhase.Entering,
            PlayerHidingKind.Tall, 0x5DC33E3E, 456, ulong.MaxValue, 25, 10, 20, 30, 0.5F);
        var presented = Hidden with { Presentation = presentation };
        var presentedPosition = Position(1, presented);
        ModelValidation.Validate(presentedPosition);
        AssertEx.Equal(presentedPosition, JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(JsonPayload.Serialize(presentedPosition)));
        AssertEx.True(PlayerHidingPresentationCodec.TryDecode(PlayerHidingPresentationCodec.Encode(presentation, true),
            out var decodedPresentation, out var animate));
        AssertEx.Equal(presentation, decodedPresentation!);
        AssertEx.True(animate);
        foreach (var invalid in new[] { presentation with { Transition = 0 }, presentation with { EntityShortGuid = 0 },
            presentation with { Phase = (PlayerHidingPhase)4 }, presentation with { Kind = (PlayerHidingKind)0 },
            presentation with { ElapsedMilliseconds = 60_001 }, presentation with { X = float.NaN },
            presentation with { Yaw = 3.4F } })
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(Position(1, Hidden with { Presentation = invalid })));
        foreach (var text in new[] { "", "0,0", "1,1,123,456,0,25,10,20,30,0.5,1",
            "1,1,123,456,1,25,10,20,30,0.5,2", "1,1,123,456,1,25,NaN,20,30,0.5,1" })
            AssertEx.False(PlayerHidingPresentationCodec.TryDecode(text, out _, out _));
        var queue = new PlayerPostureDispatchQueue();
        queue.Observe(null, hiding: presented);
        queue.Observe(null, hiding: presented with { Presentation = presentation with { ElapsedMilliseconds = 500 } });
        AssertEx.Equal(1, queue.Count);
        queue.Complete(null, hiding: queue.NextHiding);
        AssertEx.Equal(0, queue.Count);
        queue.Observe(null, hiding: presented with { Presentation = presentation with { Transition = 2 } });
        AssertEx.Equal(1, queue.Count);
        foreach (var state in new PlayerHidingSnapshot?[] { null, Visible, Hidden, new(true, 0, 0, 0) })
        {
            var position = Position(1, state);
            ModelValidation.Validate(position);
            AssertEx.Equal(position, JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(JsonPayload.Serialize(position)));
            if (state is null) continue;
            AssertEx.True(PlayerHidingCodec.TryDecode(PlayerHidingCodec.Encode(state), out var decoded));
            AssertEx.Equal(state, decoded!);
        }
        foreach (var state in new PlayerHidingSnapshot[] {
            new(true), new(true, 0, null, 1), new(true, float.NaN, 0, 0),
            new(true, 0, float.PositiveInfinity, 0), new(true, 1_000_001, 0, 0), new(false, 0, 0, 0) })
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(Position(1, state)));
        foreach (var text in new[] { "", "1", "true", "0,1,2,3", "1,1,2", "1,1,2,3,4", "1,NaN,0,0", "1,1000001,0,0" })
            AssertEx.False(PlayerHidingCodec.TryDecode(text, out _));
        foreach (var json in new[] { "{}", "{\"hidden\":null}", "{\"hidden\":1}", "{\"hidden\":false,\"extra\":0}" })
            AssertEx.Throws<ProtocolException>(() => JsonPayload.Deserialize<PlayerHidingSnapshot>(Encoding.UTF8.GetBytes(json)));
        var frame = FrameCodec.Encode(new AimpFrame(MessageType.PlayerPositionSnapshot, 0, 1,
            JsonPayload.Serialize(Position(1, Hidden))));
        frame[4] = 29; frame[5] = 0;
        AssertEx.Throws<ProtocolException>(() => FrameCodec.Decode(frame));
        return Task.CompletedTask;
    }

    internal static Task CaptureAsync()
    {
        const int module = 0x400000, character = 0x100000;
        byte flag = 0;
        var anchor = new float[] { 10, 20, 30 };
        bool tear = false, current = true, failPosition = false;
        var reader = new LocalPlayerHidingReader((address, output) =>
        {
            if (address == module + LocalPlayerHidingReader.HidingPositionRva && !failPosition)
            {
                anchor.SelectMany(BitConverter.GetBytes).ToArray().CopyTo(output, 0);
                if (tear) flag = 0;
                return true;
            }
            if (address != character + LocalPlayerHidingReader.PlayerHidingFlagOffset) return false;
            output[0] = flag;
            return true;
        });
        AssertEx.True(reader.TryCapture(module, character, 1, out var visible));
        AssertEx.Equal(Visible, visible!);
        flag = 1;
        AssertEx.True(reader.TryCapture(module, character, 1, out var hidden));
        AssertEx.Equal(Hidden, hidden!);
        anchor[0] = 50;
        AssertEx.True(reader.TryCapture(module, character, 1, out var latched));
        AssertEx.Equal(Hidden, latched!);
        AssertEx.True(reader.TryCapture(module, character, 2, out var newLevel));
        AssertEx.Equal(new PlayerHidingSnapshot(true, 50, 20, 30), newLevel!);
        reader.Reset(); tear = true;
        AssertEx.False(reader.TryCapture(module, character, 2, out var torn));
        AssertEx.True(torn is null);
        tear = false; flag = 1; current = false;
        AssertEx.False(reader.TryCapture(module, character, 2, out _, () => current));
        anchor[0] = 60; current = true;
        AssertEx.True(reader.TryCapture(module, character, 2, out var coherent, () => current));
        AssertEx.Equal(new PlayerHidingSnapshot(true, 60, 20, 30), coherent!);
        reader.Reset(); failPosition = true;
        AssertEx.False(reader.TryCapture(module, character, 2, out var missing));
        AssertEx.True(missing is null);
        var entering = new PlayerHidingPresentationSnapshot(PlayerHidingPhase.Entering,
            PlayerHidingKind.Tall, 0x5DC33E3E, 456, 1, 10, 10, 20, 30, 0.5F);
        var native = new NativePlayerHidingStatePayload(1, 2, character, 8,
            PlayerHidingPresentationCodec.Encode(entering, false));
        var runtime = NativeRuntimeSnapshot.Empty with { IpcConnected = true, LevelGeneration = 2,
            LocalCharacterGeneration = 2, LocalCharacterBase = character, LocalCharacterSerial = 8,
            LocalHiding = native, LocalHidingReceivedAt = Now };
        var early = LocalPlayerHidingReader.WithPresentation(Visible, runtime, character, Now.AddMilliseconds(20));
        AssertEx.False(early!.Hidden);
        AssertEx.Equal(entering with { ElapsedMilliseconds = 30 }, early.Presentation!);
        AssertEx.Equal(Visible, LocalPlayerHidingReader.WithPresentation(Visible,
            runtime with { IpcConnected = false }, character, Now)!);
        AssertEx.Equal(Visible, LocalPlayerHidingReader.WithPresentation(Visible,
            runtime with { LocalCharacterSerial = 9 }, character, Now)!);
        AssertEx.Equal(Visible, LocalPlayerHidingReader.WithPresentation(Visible,
            runtime with { LevelGeneration = 3 }, character, Now)!);
        AssertEx.Equal(Visible, LocalPlayerHidingReader.WithPresentation(Visible, runtime,
            character, Now.AddSeconds(2))!);
        AssertEx.Equal(Visible, LocalPlayerHidingReader.WithPresentation(Visible,
            runtime with { LocalHiding = native with { Presentation = "0" } }, character, Now)!);
        return Task.CompletedTask;
    }

    internal static Task MovementAsync()
    {
        var track = new RemotePositionTrack();
        track.TryAccept(Position(1, Visible), Now);
        track.BindCharacter(0x200000);
        void Complete()
        {
            var plan = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
            track.MarkCommand(plan, Now);
        }
        Complete();
        var entering = new PlayerHidingPresentationSnapshot(PlayerHidingPhase.Entering, PlayerHidingKind.Tall,
            0x5DC33E3E, 456, 1, 0, 10, 20, 30, 0.5F);
        track.TryAccept(Position(2, Visible with { Presentation = entering }), Now);
        var entryPlan = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
        AssertEx.Equal(RemoteMovementAction.Stop, entryPlan.Action);
        AssertEx.True(entryPlan.HidingCanAnimate);
        var entryText = RemoteMovementCommandFactory.Create(0x200000, entryPlan)["playerHidingPresentation"];
        AssertEx.True(PlayerHidingPresentationCodec.TryDecode(entryText, out var sentEntry, out var liveEntry));
        AssertEx.True(liveEntry);
        AssertEx.True(sentEntry!.ElapsedMilliseconds < 500);
        AssertEx.Equal(entering, sentEntry with { ElapsedMilliseconds = 0 });
        track.MarkCommand(entryPlan, Now);
        track.BindCharacter(0x210000);
        var reconciled = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
        AssertEx.False(reconciled.HidingCanAnimate);
        track.MarkCommand(reconciled, Now);
        track.TryAccept(Position(3, Visible), Now);
        Complete();
        // A settled locker hide (hidden flag + Tall presentation).
        var inLocker = Hidden with { Presentation = entering with { Phase = PlayerHidingPhase.Hidden, Transition = 2 } };
        track.TryAccept(Position(4, inLocker), Now);
        track.TryAccept(Position(5, null), Now); // unavailable samples preserve occupancy
        var enter = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
        AssertEx.Equal(RemoteMovementAction.Stop, enter.Action);
        AssertEx.Equal(inLocker, enter.Hiding!);
        // AvS pass 17: the hidden flag WITHOUT a presentation (crouched under a
        // table) is sent as visible, keeps the torch, and keeps walking.
        var open = RemoteMovementCommandFactory.Create(0x200000, enter with { Hiding = Hidden });
        AssertEx.Equal("0", open["playerHiding"]);
        AssertEx.Equal("0", open["playerHidingPresentation"]);
        var openTrack = new RemotePositionTrack();
        openTrack.TryAccept(Position(1, Visible), Now);
        openTrack.BindCharacter(0x400000);
        openTrack.TryAccept(Position(2, Hidden), Now.AddMilliseconds(50));
        AssertEx.True(RemoteMovementPlanner.Create(openTrack.Snapshot()!, Now.AddMilliseconds(60)).Action
            != RemoteMovementAction.Stop);
        var arguments = RemoteMovementCommandFactory.Create(0x200000, enter);
        var command = new GameCommandExecutePayload(1, Guid.NewGuid(), 1, Guid.NewGuid(),
            GameCommandNames.CharacterDirectMove, arguments);
        ModelValidation.Validate(command);
        AssertEx.Equal("1,10,20,30", arguments["playerHiding"]);
        AssertEx.Equal("0", arguments["playerTorch"]);
        var lit = RemoteMovementCommandFactory.Create(0x200000,
            enter with { Torch = new PlayerTorchSnapshot(true, 0, 0, 1) });
        AssertEx.Equal("0", lit["playerTorch"]);
        AssertEx.False(lit.ContainsKey("playerTorchDirection"));
        foreach (var key in new[] { "replicationPositionX", "replicationYaw", "moveType", "directionSpace" })
        {
            var invalid = new Dictionary<string, string>(arguments);
            invalid.Remove(key);
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command with { Arguments = invalid }));
        }
        // Rapid entry/exit must retain both edges while a command is in flight.
        track.TryAccept(Position(6, Visible), Now);
        track.MarkCommand(enter, Now);
        var exit = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
        AssertEx.Equal(Visible, exit.Hiding!);
        AssertEx.Equal("0", RemoteMovementCommandFactory.Create(0x200000, exit)["playerHiding"]);
        track.MarkCommand(exit, Now);
        track.TryAccept(Position(7, Hidden), Now);
        track.BindCharacter(0x300000);
        AssertEx.Equal(Hidden, track.Snapshot()!.Hiding!);
        track.TryAccept(Position(8, null) with { CharacterIncarnation = 2 }, Now);
        AssertEx.True(track.Snapshot()!.Hiding is null);
        return Task.CompletedTask;
    }
}
