using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

internal enum RemoteMovementAction
{
    None,
    Move,
    Stop
}

internal sealed record RemotePositionTrackSnapshot(
    PlayerPositionSnapshotPayload? Previous,
    PlayerPositionSnapshotPayload Latest,
    DateTimeOffset ReceivedAtUtc,
    bool ObservedMoving,
    float EstimatedVelocityX,
    float EstimatedVelocityZ,
    float RemoteStepDistance,
    bool CommandMoving,
    float LastCommandDirectionX,
    float LastCommandDirectionZ,
    float LastCommandIntensity,
    float LastCommandPositionX,
    float LastCommandPositionY,
    float LastCommandPositionZ,
    float LastCommandYaw,
    PlayerMoveType LastCommandMoveType,
    float LastCommandMoveSpeed,
    DateTimeOffset LastCommandAtUtc,
    PlayerPostureSnapshot? Posture = null,
    bool PosturePending = false,
    PlayerTorchSnapshot? Torch = null,
    bool TorchPending = false,
    PlayerHidingSnapshot? Hiding = null,
    bool HidingPending = false,
    bool HidingCanAnimate = false,
    PlayerAnimationSnapshot? Animation = null,
    bool AnimationPending = false);

internal sealed class RemotePositionTrack
{
    // v10.6 samples the source transform at ~60 Hz. Scale the displacement
    // thresholds with the shorter sample interval so SlowWalk still starts
    // reliably, while four stationary samples preserve roughly the same
    // 60-70 ms stop hysteresis as v10.5.
    private const float StartStepDistance = 0.005F;
    private const float StopStepDistance = 0.0015F;
    private const float DirectionSmoothing = 1.00F;
    private const int StationarySamplesToStop = 4;
    private const float DiscontinuityDistance = 2.0F;
    // Velocity is measured over a ~100 ms window of source samples. Per-sample
    // differences use millisecond UTC stamps (~16 ms steps) and swung by 2x.
    private const long VelocityWindowMinimumMilliseconds = 80;
    private const long VelocityWindowMaximumMilliseconds = 250;
    private const int VelocityHistoryCapacity = 32;
    private readonly Queue<(long SentAt, float X, float Z)> _velocityHistory = new();

    private PlayerPositionSnapshotPayload? _previous;
    private PlayerPositionSnapshotPayload? _latest;
    private readonly PlayerPositionReceiveWindow _receiveWindow = new();
    private readonly PlayerPostureDispatchQueue _postures = new();
    private PlayerPostureSnapshot? _lastReceivedPosture;
    private PlayerTorchSnapshot? _lastReceivedTorch;
    private PlayerHidingSnapshot? _lastReceivedHiding;
    private PlayerAnimationSnapshot? _lastReceivedAnimation;
    private long _lastReceivedAnimationAt;
    private uint _boundCharacter;
    private DateTimeOffset _receivedAtUtc;
    private float _estimatedVelocityX;
    private float _estimatedVelocityZ;
    private float _remoteStepDistance;
    private bool _observedMoving;
    private int _stationarySamples;
    private float _lastCommandDirectionX;
    private float _lastCommandDirectionZ;
    private float _lastCommandIntensity = RemoteMovementPlanner.BaseWalkIntensity;
    private float _lastCommandPositionX;
    private float _lastCommandPositionY;
    private float _lastCommandPositionZ;
    private float _lastCommandYaw = float.NaN;
    private PlayerMoveType _lastCommandMoveType = PlayerMoveType.Walk;
    private float _lastCommandMoveSpeed = RemoteMovementPlanner.LegacyWalkFallbackMagnitude;
    private DateTimeOffset _lastCommandAtUtc;
    private bool _hasSentBoundSnapshot;

    internal bool CommandMoving { get; private set; }

    internal bool TryAccept(PlayerPositionSnapshotPayload value, DateTimeOffset receivedAtUtc)
    {
        if (_latest is not null && (value.SessionId != _latest.SessionId || value.PeerId != _latest.PeerId
            || value.LevelGeneration != _latest.LevelGeneration)) return false;
        if (!_receiveWindow.TryAccept(value)) return false;
        var changedLife = _latest is not null && _latest.CharacterIncarnation != value.CharacterIncarnation;
        if (changedLife)
        {
            _lastReceivedPosture = null; _lastReceivedTorch = null; _lastReceivedHiding = null;
            _lastReceivedAnimation = null;
            _postures.Bind(value.Posture, value.Torch, value.Hiding, value.Animation);
            CommandMoving = false; _lastCommandAtUtc = default; _lastCommandYaw = float.NaN;
            _hasSentBoundSnapshot = false;
        }
        if (value.Posture is not null) _lastReceivedPosture = value.Posture;
        if (value.Torch is not null) _lastReceivedTorch = value.Torch;
        if (value.Hiding is not null) _lastReceivedHiding = value.Hiding;
        if (value.Animation is not null) {
            _lastReceivedAnimation = value.Animation;
            _lastReceivedAnimationAt = Environment.TickCount64;
        }
        if (_boundCharacter != 0) _postures.Observe(value.Posture, value.Torch, value.Hiding, value.Animation);
        var previous = changedLife ? null : _latest;
        var previousReceivedAtUtc = _receivedAtUtc;
        _previous = previous;
        _latest = value;
        _receivedAtUtc = receivedAtUtc;
        if (previous is null) { ResetObservedMotion(); return true; }
        UpdateObservedMotion(previous, value, previousReceivedAtUtc, receivedAtUtc);
        return true;
    }

    internal RemotePositionTrackSnapshot? Snapshot()
    {
        var latest = _latest;
        return latest is null ? null : CreateSnapshot(latest);
    }

    internal void BindCharacter(uint character)
    {
        if (_boundCharacter == character) return;
        _boundCharacter = character;
        _postures.Bind(_lastReceivedPosture, _lastReceivedTorch, _lastReceivedHiding,
            PlayerAnimationCodec.Advance(_lastReceivedAnimation, Environment.TickCount64 - _lastReceivedAnimationAt));
        CommandMoving = false;
        _lastCommandAtUtc = default;
        _lastCommandYaw = float.NaN;
        _hasSentBoundSnapshot = false;
    }

    private RemotePositionTrackSnapshot CreateSnapshot(PlayerPositionSnapshotPayload latest) =>
        new(_previous, latest, _receivedAtUtc, _observedMoving,
            _estimatedVelocityX, _estimatedVelocityZ, _remoteStepDistance, CommandMoving,
            _lastCommandDirectionX, _lastCommandDirectionZ, _lastCommandIntensity,
            _lastCommandPositionX, _lastCommandPositionY, _lastCommandPositionZ,
            _lastCommandYaw, _lastCommandMoveType, _lastCommandMoveSpeed,
            _lastCommandAtUtc, _postures.Next, _postures.HasPending && _postures.Next is not null,
            _postures.NextTorch, _postures.HasPending && _postures.NextTorch is not null,
            _postures.NextHiding, _postures.HasPending && _postures.NextHiding is not null,
            _hasSentBoundSnapshot,
            _postures.NextAnimation, _postures.HasPending && _postures.NextAnimation is not null);

    internal void MarkCommand(RemoteMovementPlan plan, DateTimeOffset appliedAtUtc)
    {
        if (plan.Action != RemoteMovementAction.None)
        {
            _postures.Complete(plan.Posture, plan.Torch, plan.Hiding, plan.Animation);
            _hasSentBoundSnapshot = true;
        }
        if (plan.Action == RemoteMovementAction.Move)
        {
            MarkMoveCommand(plan, appliedAtUtc);
            return;
        }
        if (plan.Action == RemoteMovementAction.Stop) MarkStopCommand(plan, appliedAtUtc);
    }

    private void MarkMoveCommand(RemoteMovementPlan plan, DateTimeOffset appliedAtUtc)
    {
        CommandMoving = true;
        _lastCommandDirectionX = plan.DirectionX;
        _lastCommandDirectionZ = plan.DirectionZ;
        _lastCommandIntensity = plan.Intensity;
        _lastCommandPositionX = plan.PositionX;
        _lastCommandPositionY = plan.PositionY;
        _lastCommandPositionZ = plan.PositionZ;
        _lastCommandMoveType = plan.MoveType;
        _lastCommandMoveSpeed = plan.NativeMoveSpeed;
        MarkYaw(plan.Yaw, appliedAtUtc);
    }

    private void MarkStopCommand(RemoteMovementPlan plan, DateTimeOffset appliedAtUtc)
    {
        CommandMoving = false;
        _lastCommandDirectionX = 0F;
        _lastCommandDirectionZ = 0F;
        _lastCommandIntensity = RemoteMovementPlanner.BaseWalkIntensity;
        _lastCommandPositionX = plan.PositionX;
        _lastCommandPositionY = plan.PositionY;
        _lastCommandPositionZ = plan.PositionZ;
        _lastCommandMoveType = plan.MoveType;
        _lastCommandMoveSpeed = plan.NativeMoveSpeed;
        MarkYaw(plan.Yaw, appliedAtUtc);
    }

    private void MarkYaw(float yaw, DateTimeOffset appliedAtUtc)
    {
        _lastCommandYaw = yaw;
        _lastCommandAtUtc = appliedAtUtc;
    }

    private void UpdateObservedMotion(
        PlayerPositionSnapshotPayload previous,
        PlayerPositionSnapshotPayload latest,
        DateTimeOffset previousReceivedAtUtc,
        DateTimeOffset receivedAtUtc)
    {
        var stepX = latest.X - previous.X;
        var stepZ = latest.Z - previous.Z;
        var step = Length(stepX, stepZ);
        _remoteStepDistance = step;
        if (!IsContinuousStep(step)) { ResetObservedMotion(); return; }
        RecordVelocitySample(latest);
        if (step >= MovementThreshold())
        {
            ObserveMovement(stepX, stepZ, previous, latest, previousReceivedAtUtc, receivedAtUtc);
            return;
        }
        ObserveStationarySample();
    }

    private bool IsContinuousStep(float step) =>
        float.IsFinite(step) && step < DiscontinuityDistance;

    private float MovementThreshold() => _observedMoving ? StopStepDistance : StartStepDistance;

    private void ObserveMovement(
        float stepX,
        float stepZ,
        PlayerPositionSnapshotPayload previous,
        PlayerPositionSnapshotPayload latest,
        DateTimeOffset previousReceivedAtUtc,
        DateTimeOffset receivedAtUtc)
    {
        _observedMoving = true;
        _stationarySamples = 0;
        UpdateVelocity(stepX, stepZ, previous, latest, previousReceivedAtUtc, receivedAtUtc);
    }

    private void ObserveStationarySample()
    {
        if (!_observedMoving)
        {
            _estimatedVelocityX = 0F;
            _estimatedVelocityZ = 0F;
            return;
        }
        if (++_stationarySamples >= StationarySamplesToStop) ResetObservedMotion();
    }

    private void UpdateVelocity(
        float stepX,
        float stepZ,
        PlayerPositionSnapshotPayload previous,
        PlayerPositionSnapshotPayload latest,
        DateTimeOffset previousReceivedAtUtc,
        DateTimeOffset receivedAtUtc)
    {
        if (TryWindowVelocity(latest, out var windowX, out var windowZ))
        {
            ApplyVelocity(windowX, windowZ);
            return;
        }
        var seconds = SampleSeconds(previous, latest, previousReceivedAtUtc, receivedAtUtc);
        if (seconds <= 0F) return;
        var velocityX = stepX / seconds;
        var velocityZ = stepZ / seconds;
        if (!float.IsFinite(velocityX) || !float.IsFinite(velocityZ)) return;
        ApplyVelocity(velocityX, velocityZ);
    }

    private void ApplyVelocity(float velocityX, float velocityZ)
    {
        if (Length(_estimatedVelocityX, _estimatedVelocityZ) <= float.Epsilon)
        {
            _estimatedVelocityX = velocityX;
            _estimatedVelocityZ = velocityZ;
            return;
        }
        var dot = VelocityDot(velocityX, velocityZ);
        var alpha = dot < -0.25F ? 1F : DirectionSmoothing;
        _estimatedVelocityX += (velocityX - _estimatedVelocityX) * alpha;
        _estimatedVelocityZ += (velocityZ - _estimatedVelocityZ) * alpha;
    }

    private float VelocityDot(float velocityX, float velocityZ)
    {
        var previousLength = Length(_estimatedVelocityX, _estimatedVelocityZ);
        var currentLength = Length(velocityX, velocityZ);
        return previousLength > float.Epsilon && currentLength > float.Epsilon
            ? (_estimatedVelocityX * velocityX + _estimatedVelocityZ * velocityZ)
                / (previousLength * currentLength)
            : 1F;
    }

    private void RecordVelocitySample(PlayerPositionSnapshotPayload latest)
    {
        if (_velocityHistory.Count > 0 && latest.SentAtUnixMilliseconds <= LastVelocitySampleTime())
            return;
        _velocityHistory.Enqueue((latest.SentAtUnixMilliseconds, latest.X, latest.Z));
        while (_velocityHistory.Count > VelocityHistoryCapacity ||
            (_velocityHistory.Count > 1 && latest.SentAtUnixMilliseconds - _velocityHistory.Peek().SentAt
                > VelocityWindowMaximumMilliseconds))
            _velocityHistory.Dequeue();
    }

    private long LastVelocitySampleTime()
    {
        long last = 0;
        foreach (var sample in _velocityHistory) last = sample.SentAt;
        return last;
    }

    private bool TryWindowVelocity(PlayerPositionSnapshotPayload latest, out float velocityX, out float velocityZ)
    {
        velocityX = velocityZ = 0F;
        (long SentAt, float X, float Z)? origin = null;
        foreach (var sample in _velocityHistory)
        {
            var age = latest.SentAtUnixMilliseconds - sample.SentAt;
            if (age < VelocityWindowMinimumMilliseconds) break;
            origin = sample;
        }
        if (origin is not { } start) return false;
        var seconds = (latest.SentAtUnixMilliseconds - start.SentAt) / 1000F;
        if (seconds <= 0F) return false;
        velocityX = (latest.X - start.X) / seconds;
        velocityZ = (latest.Z - start.Z) / seconds;
        return float.IsFinite(velocityX) && float.IsFinite(velocityZ);
    }

    private static float SampleSeconds(
        PlayerPositionSnapshotPayload previous,
        PlayerPositionSnapshotPayload latest,
        DateTimeOffset previousReceivedAtUtc,
        DateTimeOffset receivedAtUtc)
    {
        var sourceMilliseconds = latest.SentAtUnixMilliseconds - previous.SentAtUnixMilliseconds;
        if (sourceMilliseconds is >= 5 and <= 500) return sourceMilliseconds / 1000F;
        var receiveSeconds = (float)(receivedAtUtc - previousReceivedAtUtc).TotalSeconds;
        return receiveSeconds is > 0F and <= 0.5F ? receiveSeconds : 0F;
    }

    private void ResetObservedMotion()
    {
        _velocityHistory.Clear();
        _observedMoving = false;
        _stationarySamples = 0;
        _estimatedVelocityX = 0F;
        _estimatedVelocityZ = 0F;
        _remoteStepDistance = 0F;
    }

    private static float Length(float x, float z) => MathF.Sqrt(x * x + z * z);
}

internal sealed record RemoteMovementPlan(
    RemoteMovementAction Action,
    float DirectionX,
    float DirectionZ,
    float Intensity,
    float PositionX,
    float PositionY,
    float PositionZ,
    float Yaw,
    float RemoteStepDistance,
    PlayerMoveType MoveType,
    float NativeMoveSpeed,
    PlayerPostureSnapshot? Posture = null,
    PlayerTorchSnapshot? Torch = null,
    PlayerHidingSnapshot? Hiding = null,
    bool HidingCanAnimate = false,
    PlayerAnimationSnapshot? Animation = null);

internal static class RemoteMovementPlanner
{
    internal static readonly TimeSpan SnapshotTimeout = TimeSpan.FromMilliseconds(200);
    // The native follower runs every engine frame and keeps consuming the
    // latest network target.  Normal target refreshes therefore do not need to
    // cross Launcher -> DLL -> engine at 60 Hz.  Sharp direction changes and
    // gait/speed changes still bypass this cap above, preserving responsiveness.
    internal static readonly TimeSpan MinimumCommandInterval = TimeSpan.FromMilliseconds(25);
    internal static readonly TimeSpan MovementHeartbeat = TimeSpan.FromMilliseconds(33);
    internal static readonly TimeSpan OrientationHeartbeat = TimeSpan.FromMilliseconds(33);

    internal const float BaseWalkIntensity = 1.100F;
    private const float MaximumReplicatedSpeed = 8.0F;
    // Fallback only. v10.4 reads the retail source Character's requested
    // move_type and its own +0x4C speed table instead of assuming WALK.
    internal const float LegacyWalkFallbackMagnitude = 1.15073F;
    private const float DirectionResendDot = 0.997F;
    private const float ImmediateDirectionResendDot = 0.75F;
    private const float PositionResendDistance = 0.008F;
    private const float YawResendRadians = 0.00872665F; // 0.5 degree.

    internal static RemoteMovementPlan Create(
        RemotePositionTrackSnapshot track,
        DateTimeOffset now)
    {
        var age = now - track.ReceivedAtUtc;
        if (age > SnapshotTimeout) return StalePlan(track);
        // A hidden representation is backstage at the locker; never walk it.
        if (track.Hiding is { Hidden: true } or { Presentation: not null }) return IdlePlan(track, now);
        if (track.Previous is null || !track.ObservedMoving) return IdlePlan(track, now);
        var velocityLength = Length(track.EstimatedVelocityX, track.EstimatedVelocityZ);
        if (velocityLength <= float.Epsilon) return IdlePlan(track, now);
        return MovingPlan(track, now, velocityLength);
    }

    private static RemoteMovementPlan MovingPlan(
        RemotePositionTrackSnapshot track,
        DateTimeOffset now,
        float velocityLength)
    {
        var motionX = track.EstimatedVelocityX / velocityLength;
        var motionZ = track.EstimatedVelocityZ / velocityLength;
        var nativeMoveSpeed = track.Latest.NativeMoveSpeed;
        // Travel at the source's measured speed. The NPC gait table (3.45 m/s
        // FastWalk) is slower than a sprinting player: the Host replica fell
        // >1.5 m behind and was teleported 7 times in 30 s (diag 182335).
        var speed = float.IsFinite(velocityLength) && velocityLength > 0.05F
            ? Math.Clamp(velocityLength, 0.25F, MaximumReplicatedSpeed) : nativeMoveSpeed;
        var directionX = motionX * speed;
        var directionZ = motionZ * speed;
        var action = ShouldDispatchMove(track, directionX, directionZ, now)
            ? RemoteMovementAction.Move : RemoteMovementAction.None;
        return new(action, directionX, directionZ, BaseWalkIntensity,
            track.Latest.X, track.Latest.Y, track.Latest.Z,
            track.Latest.Yaw, track.RemoteStepDistance,
            track.Latest.MoveType, nativeMoveSpeed, track.Posture, track.Torch, track.Hiding, track.HidingCanAnimate,
            track.Animation);
    }

    private static bool ShouldDispatchMove(
        RemotePositionTrackSnapshot track,
        float directionX,
        float directionZ,
        DateTimeOffset now)
    {
        if (track.PosturePending || track.TorchPending || track.HidingPending || track.AnimationPending
            || !track.CommandMoving || track.LastCommandAtUtc == default) return true;
        if (track.Latest.MoveType != track.LastCommandMoveType ||
            MathF.Abs(track.Latest.NativeMoveSpeed - track.LastCommandMoveSpeed) >= 0.005F)
            return true;
        var sinceLastCommand = now - track.LastCommandAtUtc;
        var dot = DirectionDot(track, directionX, directionZ);
        if (dot < ImmediateDirectionResendDot) return true;
        if (sinceLastCommand < MinimumCommandInterval) return false;
        if (sinceLastCommand >= MovementHeartbeat) return true;
        return PositionMoved(track)
            || dot < DirectionResendDot || YawChanged(track, track.Latest.Yaw);
    }

    private static RemoteMovementPlan IdlePlan(
        RemotePositionTrackSnapshot track,
        DateTimeOffset now)
    {
        var shouldSend = track.PosturePending || track.TorchPending || track.HidingPending || track.AnimationPending
            || track.CommandMoving || PositionMoved(track)
            || ShouldDispatchOrientation(track, now);
        return new(shouldSend ? RemoteMovementAction.Stop : RemoteMovementAction.None,
            0F, 0F, BaseWalkIntensity,
            track.Latest.X, track.Latest.Y, track.Latest.Z,
            track.Latest.Yaw, track.RemoteStepDistance,
            track.Latest.MoveType, track.Latest.NativeMoveSpeed, track.Posture, track.Torch, track.Hiding, track.HidingCanAnimate,
            track.Animation);
    }

    private static RemoteMovementPlan StalePlan(RemotePositionTrackSnapshot track) =>
        new(track.PosturePending || track.TorchPending || track.HidingPending || track.AnimationPending || track.CommandMoving
                ? RemoteMovementAction.Stop : RemoteMovementAction.None,
            0F, 0F, BaseWalkIntensity,
            track.Latest.X, track.Latest.Y, track.Latest.Z,
            track.Latest.Yaw, track.RemoteStepDistance,
            track.Latest.MoveType, track.Latest.NativeMoveSpeed, track.Posture, track.Torch, track.Hiding, false,
            track.Animation);

    private static bool ShouldDispatchOrientation(
        RemotePositionTrackSnapshot track,
        DateTimeOffset now)
    {
        if (track.LastCommandAtUtc == default || !float.IsFinite(track.LastCommandYaw)) return true;
        if (YawChanged(track, track.Latest.Yaw)) return true;
        return now - track.LastCommandAtUtc >= OrientationHeartbeat;
    }

    private static bool YawChanged(RemotePositionTrackSnapshot track, float yaw) =>
        AngularDistance(track.LastCommandYaw, yaw) >= YawResendRadians;

    internal static float AngularDistance(float first, float second)
    {
        if (!float.IsFinite(first) || !float.IsFinite(second)) return float.PositiveInfinity;
        var delta = MathF.IEEERemainder(second - first, MathF.Tau);
        return MathF.Abs(delta);
    }

    private static float DirectionDot(
        RemotePositionTrackSnapshot track,
        float directionX,
        float directionZ)
    {
        var lastLength = Length(track.LastCommandDirectionX, track.LastCommandDirectionZ);
        var currentLength = Length(directionX, directionZ);
        return lastLength > float.Epsilon && currentLength > float.Epsilon
            ? (track.LastCommandDirectionX * directionX + track.LastCommandDirectionZ * directionZ)
                / (lastLength * currentLength)
            : -1F;
    }

    private static bool PositionMoved(RemotePositionTrackSnapshot track)
    {
        var x = track.Latest.X - track.LastCommandPositionX;
        var y = track.Latest.Y - track.LastCommandPositionY;
        var z = track.Latest.Z - track.LastCommandPositionZ;
        return MathF.Sqrt(x * x + y * y + z * z) >= PositionResendDistance;
    }

    private static float Length(float x, float z) => MathF.Sqrt(x * x + z * z);
}

internal static class RemoteMovementCommandFactory
{
    private const string SafetyFrames = "3600";

    internal static IReadOnlyDictionary<string, string> Create(
        uint characterBase,
        RemoteMovementPlan plan) =>
        AddPlayerState(plan.Action == RemoteMovementAction.Stop
            ? CreateStop(characterBase, plan)
            : CreateMove(characterBase, plan), plan.Posture, plan.Torch, plan.Hiding, plan.HidingCanAnimate, plan.Animation);

    private static IReadOnlyDictionary<string, string> AddPlayerState(
        IReadOnlyDictionary<string, string> arguments, PlayerPostureSnapshot? posture,
        PlayerTorchSnapshot? torch, PlayerHidingSnapshot? hiding, bool animate, PlayerAnimationSnapshot? animation)
    {
        if (posture is null && torch is null && hiding is null && animation is null) return arguments;
        // The remote torch uses a separate render entity. Concealing the body
        // alone must not leave a floating beam while an off sample is delayed.
        if (hiding is { Hidden: true } or { Presentation: not null }) torch = new PlayerTorchSnapshot(false);
        var result = new Dictionary<string, string>(arguments, StringComparer.Ordinal);
        if (posture is not null) result["playerPosture"] = PlayerPostureCodec.Encode(posture);
        if (torch is not null) result["playerTorch"] = torch.Enabled ? "1" : "0";
        if (hiding is not null)
        {
            result["playerHiding"] = PlayerHidingCodec.Encode(hiding);
            result["playerHidingPresentation"] = hiding.Presentation is { } presentation
                ? PlayerHidingPresentationCodec.Encode(presentation, animate) : "0";
        }
        if (torch is { Enabled: true, AimX: not null, AimY: not null, AimZ: not null })
            result["playerTorchDirection"] = PlayerTorchCodec.EncodeDirection(torch);
        // AnimationSync requires the world-space replication position (DLL rule).
        if (animation is not null && result.ContainsKey("replicationPositionX"))
            result["playerAnimation"] = PlayerAnimationCodec.Encode(animation);
        return result;
    }

    internal static IReadOnlyDictionary<string, string> CreateStop(uint characterBase) =>
        CreateBaseArguments(characterBase, 0F, 0F,
            RemoteMovementPlanner.BaseWalkIntensity, "0");

    internal static IReadOnlyDictionary<string, string> CreateStop(
        uint characterBase,
        float yaw)
    {
        var arguments = new Dictionary<string, string>(CreateStop(characterBase), StringComparer.Ordinal)
        {
            ["replicationYaw"] = FormatYaw(yaw)
        };
        return arguments;
    }

    private static IReadOnlyDictionary<string, string> CreateStop(
        uint characterBase,
        RemoteMovementPlan plan)
    {
        var arguments = new Dictionary<string, string>(
            CreateStop(characterBase, plan.Yaw), StringComparer.Ordinal)
        {
            ["replicationPositionX"] = Format(plan.PositionX),
            ["replicationPositionY"] = Format(plan.PositionY),
            ["replicationPositionZ"] = Format(plan.PositionZ),
            ["moveType"] = ((int)plan.MoveType).ToString(CultureInfo.InvariantCulture)
        };
        return arguments;
    }

    private static IReadOnlyDictionary<string, string> CreateMove(
        uint characterBase,
        RemoteMovementPlan plan)
    {
        var arguments = new Dictionary<string, string>(
            CreateBaseArguments(characterBase, plan.DirectionX, plan.DirectionZ,
                plan.Intensity, SafetyFrames), StringComparer.Ordinal)
        {
            ["replicationPositionX"] = Format(plan.PositionX),
            ["replicationPositionY"] = Format(plan.PositionY),
            ["replicationPositionZ"] = Format(plan.PositionZ),
            ["replicationYaw"] = FormatYaw(plan.Yaw),
            ["moveType"] = ((int)plan.MoveType).ToString(CultureInfo.InvariantCulture)
        };
        return arguments;
    }

    private static string Format(float value) =>
        value.ToString("0.000000", CultureInfo.InvariantCulture);

    private static string FormatYaw(float value) =>
        value.ToString("0.000000", CultureInfo.InvariantCulture);

    private static IReadOnlyDictionary<string, string> CreateBaseArguments(
        uint characterBase,
        float directionX,
        float directionZ,
        float intensity,
        string frames) => new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["characterAddress"] = $"0x{characterBase:X8}",
            ["directionX"] = Format(directionX),
            ["directionZ"] = Format(directionZ),
            ["intensity"] = intensity.ToString("0.000", CultureInfo.InvariantCulture),
            ["frameCount"] = frames,
            ["directionSpace"] = "world"
        };
}
