namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    private const int ShadowOutboxRecords = 2048;
    private const int ShadowOutboxBytes = 2 * 1024 * 1024;
    private static readonly TimeSpan ShadowSendTimeout = TimeSpan.FromSeconds(10); // peer-timeout parity: a cancelled write drops the peer
    private readonly Dictionary<ulong, ShadowPeer> _shadowPeers = new();
    private AISyncShadowJournal? _shadowJournal;
    private AISyncShadowStreamId _shadowStream;
    private long _shadowRetentionStart;
    private bool _shadowRetentionExpired;
    private Task _shadowStopTask = Task.CompletedTask;

    public event EventHandler<AISyncShadowReceipt>? AISyncShadowReceiptReceived;
    public event EventHandler<AISyncShadowRecovery>? AISyncShadowRecoveryRequired;

    /// <summary>Explicit observation-only opt-in. Every source binding starts an empty history.</summary>
    public AISyncShadowStreamId StartAISyncShadow(int levelGeneration)
    {
        lock (_stateLock)
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            if (_state != "Starting" || levelGeneration != _levelGeneration)
                throw new InvalidOperationException("AISync shadow requires the active starting generation.");
            if (!_shadowStopTask.IsCompleted)
                throw new InvalidOperationException("AISync shadow is still stopping.");
            _shadowStream = new(SessionId, (ulong)levelGeneration, Guid.NewGuid());
            _shadowJournal = new AISyncShadowJournal(_shadowStream);
            _shadowRetentionStart = 0;
            _shadowRetentionExpired = false;
            foreach (var state in _clients.Values)
            {
                if (!_shadowPeers.TryGetValue(state.PeerId, out var peer))
                {
                    peer = new ShadowPeer(state, _shutdown.Token);
                    _shadowPeers.Add(state.PeerId, peer);
                    peer.Sender = Task.Run(() => SendShadowLoopAsync(peer));
                }
                peer.Reset(_shadowJournal.CreateOpen(Guid.NewGuid()));
                QueueShadowLocked(peer, MessageType.AISyncShadowOpen,
                    AISyncShadowCodec.EncodeOpen(peer.Open!), 0, 0);
            }
            return _shadowStream;
        }
    }

    /// <summary>Validates and copies bytes, then returns without awaiting any peer's socket.</summary>
    public Task<AISyncShadowResult> PublishAISyncShadowAsync(AISyncShadowStreamId stream,
        ReadOnlyMemory<byte> observation, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        List<AISyncShadowRecovery> recoveries = new();
        AISyncShadowResult result;
        lock (_stateLock)
        {
            if (_disposed != 0 || _state != "Starting" || stream != _shadowStream
                || stream.SessionGuid != SessionId || stream.WorldGeneration != (ulong)_levelGeneration
                || _shadowJournal is null)
                return Task.FromResult(AISyncShadowResult.Rejected);
            AISyncShadowCodec.ValidateNativeObservation(observation.Span, stream);
            if (_shadowJournal.RecoveryRequired || _shadowRetentionExpired)
                return Task.FromResult(AISyncShadowResult.RecoveryRequired);
            _shadowRetentionExpired = _shadowJournal.Count != 0
                && Environment.TickCount64 - _shadowRetentionStart >= 60_000;
            ulong ordinal = 0;
            try { if (!_shadowRetentionExpired) ordinal = _shadowJournal.Append(observation.Span); }
            catch (ProtocolException) when (_shadowJournal.RecoveryRequired) { }
            if (_shadowJournal.Count == 1 && ordinal != 0) _shadowRetentionStart = Environment.TickCount64;
            result = _shadowJournal.RecoveryRequired || _shadowRetentionExpired
                ? AISyncShadowResult.RecoveryRequired : AISyncShadowResult.Accepted;
            foreach (var peer in _shadowPeers.Values)
            {
                if (peer.RecoveryRequired) continue;
                if (result == AISyncShadowResult.RecoveryRequired)
                {
                    recoveries.Add(RecoverShadowLocked(peer, _shadowRetentionExpired
                        ? AISyncShadowRecoveryReason.RestartRequired : AISyncShadowRecoveryReason.Overflow));
                    continue;
                }
                var data = _shadowJournal.CreateData(peer.Open!.ConsumerGuid, ordinal, 1);
                var payload = AISyncShadowCodec.EncodeData(data);
                if (peer.Records + 1 > ShadowOutboxRecords || peer.Bytes + payload.Length > ShadowOutboxBytes)
                    recoveries.Add(RecoverShadowLocked(peer, AISyncShadowRecoveryReason.Overflow));
                else QueueShadowLocked(peer, MessageType.AISyncShadowData, payload, 1, ordinal);
            }
        }
        foreach (var recovery in recoveries) RaiseShadowRecovery(recovery);
        return Task.FromResult(result);
    }

    private static void QueueShadowLocked(ShadowPeer peer, MessageType type, byte[] bytes, int records, ulong last)
    {
        peer.Queue.Enqueue(new ShadowSend(type, bytes, records, last, peer.Open!.Stream, Environment.TickCount64));
        peer.Records += records;
        peer.Bytes += bytes.Length;
        if (peer.Signal.CurrentCount == 0) peer.Signal.Release();
    }

    private AISyncShadowRecovery RecoverShadowLocked(ShadowPeer peer, AISyncShadowRecoveryReason reason)
    {
        peer.RecoveryRequired = true;
        peer.Queue.Clear();
        peer.Records = peer.Bytes = 0;
        var recovery = new AISyncShadowRecovery(peer.Open!.Stream, peer.Open.ConsumerGuid, reason,
            peer.LastReceipt + 1);
        QueueShadowLocked(peer, MessageType.AISyncShadowRecovery, AISyncShadowCodec.EncodeRecovery(recovery), 0, 0);
        return recovery;
    }

    private async Task SendShadowLoopAsync(ShadowPeer peer)
    {
        try
        {
            while (!peer.Cancellation.IsCancellationRequested)
            {
                await peer.Signal.WaitAsync(peer.Cancellation.Token).ConfigureAwait(false);
                while (TakeShadow(peer) is { } send)
                    await SendShadowAsync(peer, send).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (peer.Cancellation.IsCancellationRequested) { }
    }

    private ShadowSend? TakeShadow(ShadowPeer peer)
    {
        lock (_stateLock)
        {
            if (peer.Cancellation.IsCancellationRequested || peer.Queue.Count == 0) return null;
            var send = peer.Queue.Dequeue();
            // An acknowledgement can arrive as soon as the socket write starts.
            if (send.LastOrdinal != 0) peer.HighestSending = send.LastOrdinal;
            return send;
        }
    }

    private async Task SendShadowAsync(ShadowPeer peer, ShadowSend send)
    {
        AISyncShadowRecovery? recovery = null;
        try
        {
            if (Environment.TickCount64 - send.ReadyTicks > ShadowSendTimeout.TotalMilliseconds)
                throw new TimeoutException("AISync shadow ready age exceeded.");
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(peer.Cancellation.Token);
            timeout.CancelAfter(ShadowSendTimeout);
            await peer.State.Connection.SendShadowAsync(send.Type, send.Bytes, timeout.Token).ConfigureAwait(false);
            lock (_stateLock)
            {
                if (peer.Open?.Stream == send.Stream && !peer.RecoveryRequired)
                {
                    peer.Records -= send.Records;
                    peer.Bytes -= send.Bytes.Length;
                }
            }
        }
        catch (Exception exception) when (exception is OperationCanceledException or TimeoutException
            or IOException or System.Net.Sockets.SocketException or ObjectDisposedException)
        {
            lock (_stateLock)
            {
                if (!peer.Cancellation.IsCancellationRequested && peer.Open?.Stream == send.Stream
                    && !peer.RecoveryRequired)
                    recovery = RecoverShadowLocked(peer, AISyncShadowRecoveryReason.RestartRequired);
            }
        }
        if (recovery is not null) RaiseShadowRecovery(recovery);
    }

    private async Task HandleAISyncShadowReceiptAsync(ClientState state, ReadOnlyMemory<byte> payload,
        CancellationToken token)
    {
        AISyncShadowReceipt receipt;
        try { receipt = AISyncShadowCodec.DecodeReceipt(payload.Span); }
        catch (ProtocolException)
        {
            await SendErrorAsync(state.Connection, "AISYNC_SHADOW_RECEIPT_REJECTED",
                "Malformed observation receipt; applied claims are forbidden.", token).ConfigureAwait(false);
            return;
        }
        var accepted = false;
        AISyncShadowRecovery? recovery = null;
        lock (_stateLock)
        {
            if (ActiveShadowPeerLocked(state, receipt.Stream, receipt.ConsumerGuid, out var peer)
                && !peer!.RecoveryRequired && receipt.ReceivedOrdinal >= peer.LastReceipt
                && receipt.ReceivedOrdinal <= peer.HighestSending)
            {
                var result = _shadowJournal!.AcceptReceipt(receipt);
                accepted = result is AISyncShadowResult.Accepted or AISyncShadowResult.Duplicate;
                if (accepted) peer.LastReceipt = receipt.ReceivedOrdinal;
                else recovery = RecoverShadowLocked(peer, AISyncShadowRecoveryReason.DigestMismatch);
            }
        }
        if (recovery is not null) RaiseShadowRecovery(recovery);
        if (accepted) RaiseShadowReceipt(receipt);
        else await SendErrorAsync(state.Connection, "AISYNC_SHADOW_RECEIPT_REJECTED",
            "The observation receipt has a stale scope, invalid order or unverified digest.", token).ConfigureAwait(false);
    }

    private void HandleAISyncShadowRecovery(ClientState state, ReadOnlyMemory<byte> payload)
    {
        var request = AISyncShadowCodec.DecodeRecovery(payload.Span);
        AISyncShadowRecovery? recovery = null;
        lock (_stateLock)
        {
            if (!ActiveShadowPeerLocked(state, request.Stream, request.ConsumerGuid, out var peer)
                || request.ExpectedOrdinal > _shadowJournal!.LastOrdinal + 1)
                throw new ProtocolException("Invalid AISync shadow recovery scope or ordinal.");
            if (!peer!.RecoveryRequired) recovery = RecoverShadowLocked(peer, request.Reason);
        }
        if (recovery is not null) RaiseShadowRecovery(recovery);
    }

    private bool ActiveShadowPeerLocked(ClientState state, AISyncShadowStreamId stream, Guid consumer,
        out ShadowPeer? peer)
    {
        peer = null;
        return _state == "Starting" && _shadowJournal is not null && stream == _shadowStream
            && stream.SessionGuid == SessionId && stream.WorldGeneration == (ulong)_levelGeneration
            && _shadowPeers.TryGetValue(state.PeerId, out peer) && ReferenceEquals(peer.State, state)
            && peer.Open?.ConsumerGuid == consumer;
    }

    // Diagnostics must not fault the connection reader or the dedicated sender.
    private void RaiseShadowRecovery(AISyncShadowRecovery recovery)
    {
        try { AISyncShadowRecoveryRequired?.Invoke(this, recovery); }
        catch (Exception) { }
    }

    private void RaiseShadowReceipt(AISyncShadowReceipt receipt)
    {
        try { AISyncShadowReceiptReceived?.Invoke(this, receipt); }
        catch (Exception) { }
    }

    public Task StopAISyncShadowAsync()
    {
        lock (_stateLock)
        {
            if (!_shadowStopTask.IsCompleted) return _shadowStopTask;
            var peers = _shadowPeers.Values.ToArray();
            _shadowPeers.Clear();
            _shadowJournal = null;
            foreach (var peer in peers) peer.Cancellation.Cancel();
            var notify = _state == "Starting" && _disposed == 0 && !_shutdown.IsCancellationRequested;
            return _shadowStopTask = StopShadowPeersAsync(peers, notify);
        }
    }

    private async Task StopShadowPeersAsync(ShadowPeer[] peers, bool notify)
    {
        await Task.WhenAll(peers.Select(peer => peer.Sender)).ConfigureAwait(false);
        if (notify)
            await Task.WhenAll(peers.Select(NotifyShadowStoppedAsync)).ConfigureAwait(false);
        await DrainShadowPeersAsync(peers).ConfigureAwait(false);
    }

    private async Task NotifyShadowStoppedAsync(ShadowPeer peer)
    {
        var recovery = new AISyncShadowRecovery(peer.Open!.Stream, peer.Open.ConsumerGuid,
            AISyncShadowRecoveryReason.RestartRequired, peer.LastReceipt + 1);
        RaiseShadowRecovery(recovery);
        try
        {
            using var timeout = new CancellationTokenSource(ShadowSendTimeout);
            await peer.State.Connection.SendShadowAsync(MessageType.AISyncShadowRecovery,
                AISyncShadowCodec.EncodeRecovery(recovery), timeout.Token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is OperationCanceledException or IOException
            or System.Net.Sockets.SocketException or ObjectDisposedException) { }
    }

    private async Task RemoveShadowPeerAsync(ulong peerId)
    {
        ShadowPeer? peer;
        lock (_stateLock)
        {
            if (!_shadowPeers.Remove(peerId, out peer)) return;
            if (peer.Open is not null) _shadowJournal?.RemoveConsumer(peer.Open.ConsumerGuid);
            peer.Cancellation.Cancel();
        }
        await DrainShadowPeersAsync(new[] { peer }).ConfigureAwait(false);
    }

    private static async Task DrainShadowPeersAsync(ShadowPeer[] peers)
    {
        foreach (var peer in peers)
        {
            await peer.Sender.ConfigureAwait(false);
            peer.Cancellation.Dispose();
            peer.Signal.Dispose();
        }
    }

    private sealed record ShadowSend(MessageType Type, byte[] Bytes, int Records, ulong LastOrdinal,
        AISyncShadowStreamId Stream, long ReadyTicks);

    private sealed class ShadowPeer(ClientState state, CancellationToken shutdown)
    {
        public ClientState State { get; } = state;
        public CancellationTokenSource Cancellation { get; } = CancellationTokenSource.CreateLinkedTokenSource(shutdown);
        public SemaphoreSlim Signal { get; } = new(0, 1);
        public Queue<ShadowSend> Queue { get; } = new();
        public Task Sender { get; set; } = Task.CompletedTask;
        public AISyncShadowOpen? Open { get; private set; }
        public bool RecoveryRequired { get; set; }
        public int Records { get; set; }
        public int Bytes { get; set; }
        public ulong HighestSending { get; set; }
        public ulong LastReceipt { get; set; }

        public void Reset(AISyncShadowOpen open)
        {
            Open = open;
            Queue.Clear();
            Records = Bytes = 0;
            HighestSending = LastReceipt = 0;
            RecoveryRequired = false;
        }
    }
}
