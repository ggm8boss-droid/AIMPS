namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private AISyncShadowOpen? _shadowOpen;
    private AISyncShadowReceiverWindow? _shadowReceiver;
    private readonly HashSet<Guid> _shadowRetiredStreams = new();
    private int _shadowGeneration;
    private readonly object _shadowControlLock = new();
    private readonly Queue<ShadowControl> _shadowControls = new();
    private readonly SemaphoreSlim _shadowControlSignal = new(0, 1);
    private CancellationTokenSource? _shadowControlCancellation;
    private Task? _shadowControlSender;
    private int _shadowControlCount;
    private bool _shadowControlFailed;

    /// <summary>Verified value-only observations; never a request to enter the native runtime.</summary>
    public event EventHandler<AISyncShadowData>? AISyncShadowObservationBatchReceived;
    public event EventHandler<AISyncShadowRecovery>? AISyncShadowRecoveryRequired;

    private bool ShadowGenerationActive(AISyncShadowStreamId stream) => stream.SessionGuid == SessionId
        && Snapshot?.State == "Starting" && stream.WorldGeneration == (ulong)Snapshot.LevelGeneration;

    private async Task HandleAISyncShadowOpenAsync(ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        var open = AISyncShadowCodec.DecodeOpen(payload.Span);
        if (open.Stream.SessionGuid != SessionId)
            throw new ProtocolException("AISync shadow open belongs to another session.");
        if (!ShadowGenerationActive(open.Stream))
        {
            RaiseShadowRecovery(new(open.Stream, open.ConsumerGuid, AISyncShadowRecoveryReason.RestartRequired, 1));
            return;
        }
        if (_shadowGeneration != Snapshot!.LevelGeneration)
        {
            ClearAISyncShadow();
            _shadowGeneration = Snapshot.LevelGeneration;
        }
        if (_shadowOpen == open) return;
        if (_shadowRetiredStreams.Contains(open.Stream.StreamGuid)
            || _shadowOpen?.Stream == open.Stream || _shadowRetiredStreams.Count >= 1024)
        {
            RaiseShadowRecovery(new(open.Stream, open.ConsumerGuid, AISyncShadowRecoveryReason.RestartRequired, 1));
            return;
        }
        if (_shadowOpen is not null) _shadowRetiredStreams.Add(_shadowOpen.Stream.StreamGuid);
        _shadowReceiver = new AISyncShadowReceiverWindow(open);
        lock (_shadowControlLock)
        {
            _shadowOpen = open;
            _shadowControls.Clear();
            _shadowControlCount = 0;
            _shadowControlFailed = false;
            if (_shadowControlSender is null)
            {
                _shadowControlCancellation = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
                _shadowControlSender = Task.Run(() => SendShadowControlsLoopAsync(_shadowControlCancellation.Token));
            }
        }
        if (open.FirstOrdinal != 1)
            await SendShadowRecoveryAsync(new(open.Stream, open.ConsumerGuid,
                AISyncShadowRecoveryReason.RestartRequired, 1), token).ConfigureAwait(false);
    }

    private async Task HandleAISyncShadowDataAsync(ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        var scope = AISyncShadowCodec.DecodeDataScope(payload.Span);
        if (scope.Stream.SessionGuid != SessionId)
            throw new ProtocolException("AISync shadow data belongs to another session.");
        if (!ShadowGenerationActive(scope.Stream) || _shadowOpen?.Stream != scope.Stream
            || _shadowOpen.ConsumerGuid != scope.ConsumerGuid || _shadowReceiver is null)
        {
            RaiseShadowRecovery(new(scope.Stream, scope.ConsumerGuid, AISyncShadowRecoveryReason.RestartRequired, 1));
            return;
        }
        if (Volatile.Read(ref _shadowControlFailed)) return;
        AISyncShadowData data;
        try { data = AISyncShadowCodec.DecodeData(payload.Span); }
        catch (ProtocolException) when (_shadowOpen is not null && ShadowGenerationActive(_shadowOpen.Stream))
        {
            await SendShadowRecoveryAsync(new(_shadowOpen.Stream, _shadowOpen.ConsumerGuid,
                AISyncShadowRecoveryReason.DigestMismatch, (_shadowReceiver?.ReceivedOrdinal ?? 0) + 1), token)
                .ConfigureAwait(false);
            return;
        }
        var result = _shadowReceiver.Receive(data);
        if (result is AISyncShadowResult.Accepted or AISyncShadowResult.Duplicate)
        {
            // Snapshot the receipt before callbacks: subscribers own their byte arrays.
            var receipt = AISyncShadowCodec.EncodeReceipt(_shadowReceiver.CreateReceipt());
            if (result == AISyncShadowResult.Accepted)
            {
                try { AISyncShadowObservationBatchReceived?.Invoke(this, data); }
                catch (Exception) { }
            }
            await SendShadowControlAsync(MessageType.AISyncShadowReceipt, receipt, token).ConfigureAwait(false);
        }
        else await SendShadowRecoveryAsync(_shadowReceiver.CreateRecovery(_shadowReceiver.RecoveryReason), token)
            .ConfigureAwait(false);
    }

    private void HandleAISyncShadowRecovery(ReadOnlyMemory<byte> payload)
    {
        var recovery = AISyncShadowCodec.DecodeRecovery(payload.Span);
        if (recovery.Stream.SessionGuid != SessionId)
            throw new ProtocolException("AISync shadow recovery belongs to another session.");
        if (_shadowOpen?.Stream != recovery.Stream || _shadowOpen.ConsumerGuid != recovery.ConsumerGuid)
        {
            RaiseShadowRecovery(recovery);
            return;
        }
        // Terminal until a fresh stream opens; no claim of mid-flight resumption.
        _shadowReceiver = null;
        lock (_shadowControlLock)
        {
            _shadowControlFailed = true;
            _shadowControls.Clear();
            _shadowControlCount = 0;
        }
        RaiseShadowRecovery(recovery);
    }

    private async Task SendShadowRecoveryAsync(AISyncShadowRecovery recovery, CancellationToken token)
    {
        _shadowReceiver = null;
        RaiseShadowRecovery(recovery);
        await SendShadowControlAsync(MessageType.AISyncShadowRecovery,
            AISyncShadowCodec.EncodeRecovery(recovery), token).ConfigureAwait(false);
    }

    private Task SendShadowControlAsync(MessageType type, byte[] payload, CancellationToken token)
    {
        AISyncShadowRecovery? recovery = null;
        lock (_shadowControlLock)
        {
            if (token.IsCancellationRequested || _shadowOpen is null || _shadowControlCancellation is null
                || _shadowControlCancellation.IsCancellationRequested || _shadowControlFailed) return Task.CompletedTask;
            // Each control frame is <=128 bytes: the count cap also bounds bytes below 2 MiB.
            if (_shadowControlCount >= 2048)
            {
                recovery = FailShadowControlsLocked(AISyncShadowRecoveryReason.Overflow);
                type = MessageType.AISyncShadowRecovery;
                payload = AISyncShadowCodec.EncodeRecovery(recovery);
            }
            EnqueueShadowControlLocked(type, payload);
        }
        if (recovery is not null) RaiseShadowRecovery(recovery);
        return Task.CompletedTask;
    }

    private void EnqueueShadowControlLocked(MessageType type, byte[] bytes)
    {
        _shadowControls.Enqueue(new(type, bytes, _shadowOpen!, Environment.TickCount64));
        _shadowControlCount++;
        if (_shadowControlSignal.CurrentCount == 0) _shadowControlSignal.Release();
    }

    private AISyncShadowRecovery FailShadowControlsLocked(AISyncShadowRecoveryReason reason)
    {
        _shadowControlFailed = true;
        _shadowControls.Clear();
        _shadowControlCount = 0;
        return new(_shadowOpen!.Stream, _shadowOpen.ConsumerGuid, reason, 1);
    }

    private async Task SendShadowControlsLoopAsync(CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                await _shadowControlSignal.WaitAsync(token).ConfigureAwait(false);
                while (TakeShadowControl() is { } control)
                    await SendOneShadowControlAsync(control, token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
    }

    private ShadowControl? TakeShadowControl()
    {
        lock (_shadowControlLock)
            return _shadowControls.Count == 0 ? null : _shadowControls.Dequeue();
    }

    private async Task SendOneShadowControlAsync(ShadowControl control, CancellationToken token)
    {
        AISyncShadowRecovery? recovery = null;
        try
        {
            if (Environment.TickCount64 - control.ReadyTicks > 10000)
                throw new TimeoutException("AISync shadow receipt ready age exceeded.");
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            timeout.CancelAfter(TimeSpan.FromSeconds(10));
            await _connection.SendShadowAsync(control.Type, control.Bytes, timeout.Token).ConfigureAwait(false);
            lock (_shadowControlLock)
                if (control.Open == _shadowOpen && !_shadowControlFailed) _shadowControlCount--;
        }
        catch (Exception exception) when (exception is OperationCanceledException or TimeoutException or IOException
            or System.Net.Sockets.SocketException or ObjectDisposedException)
        {
            lock (_shadowControlLock)
            {
                if (!token.IsCancellationRequested && control.Open == _shadowOpen && !_shadowControlFailed)
                {
                    recovery = FailShadowControlsLocked(AISyncShadowRecoveryReason.RestartRequired);
                    EnqueueShadowControlLocked(MessageType.AISyncShadowRecovery, AISyncShadowCodec.EncodeRecovery(recovery));
                }
            }
        }
        if (recovery is not null) RaiseShadowRecovery(recovery);
    }

    private async Task StopShadowControlsAsync()
    {
        Task? sender;
        lock (_shadowControlLock)
        {
            sender = _shadowControlSender;
            _shadowControlCancellation?.Cancel();
            _shadowControls.Clear();
            _shadowControlCount = 0;
        }
        if (sender is not null) await sender.ConfigureAwait(false);
        lock (_shadowControlLock)
        {
            _shadowControlSender = null;
            _shadowControlCancellation?.Dispose();
            _shadowControlCancellation = null;
        }
    }

    private void RaiseShadowRecovery(AISyncShadowRecovery recovery)
    {
        try { AISyncShadowRecoveryRequired?.Invoke(this, recovery); }
        catch (Exception) { }
    }

    private void ClearAISyncShadow()
    {
        lock (_shadowControlLock)
        {
            _shadowControlFailed = true;
            _shadowControls.Clear();
            _shadowControlCount = 0;
            _shadowOpen = null;
        }
        _shadowReceiver = null;
        _shadowRetiredStreams.Clear();
        _shadowGeneration = 0;
    }

    private sealed record ShadowControl(MessageType Type, byte[] Bytes, AISyncShadowOpen Open, long ReadyTicks);
}
