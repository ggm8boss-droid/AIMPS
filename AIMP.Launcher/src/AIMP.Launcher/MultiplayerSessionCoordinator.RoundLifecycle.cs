using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

// Round lifecycle: each peer reports its own THE_PLAYER death to the Host, the
// Host ends the round once every participant is dead, then every peer shows the
// end message in PICKUPOVERLAY and returns to the frontend lobby through the
// retail "quit to main menu" path. The lobby connection itself stays alive.
internal sealed partial class MultiplayerSessionCoordinator
{
    private const string RoundEndTitle = "GAME OVER";
    private const string RoundEndMessage = "All players are dead.";
    private const string MissionCompleteReason = "MissionComplete";
    private const string MissionCompleteTitle = "MISSION COMPLETE";
    private const string MissionCompleteMessage = "Every player has finished.";
    private readonly record struct DeathReport(LobbyHost? Host, LobbyClient? Client, int Generation);
    private int _deathReportedGeneration;
    private int _roundEndingGeneration;
    private string? _frontendLobbyReturnRole;
    private RoundReturnFence? _roundReturnFence;

    internal sealed class RoundReturnFence(RoundEndedPayload round, bool recoverable = false)
    {
        internal RoundEndedPayload Round { get; } = round;
        internal bool Recoverable { get; } = recoverable;
        internal Guid Token { get; } = Guid.NewGuid();
        internal Guid ConnectionId { get; set; }
        internal bool Acknowledged { get; set; }
        internal bool ConnectionClosed { get; set; }
        internal bool AcknowledgementPending { get; set; }
        internal ScaleformBridgeEvent? Ready { get; set; }
        internal TaskCompletionSource<bool> Closed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource<bool> Retry { get; set; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        // Caller holds _stateGate. A live/locally replaced socket cannot be
        // abandoned: its movie may still be dispatching native callbacks.
        internal bool CanRenew => Recoverable && ConnectionClosed && !Acknowledged
            && !AcknowledgementPending && !Closed.Task.IsCompleted;

        internal bool TryBind(ScaleformBridgeEvent ready)
        {
            if (ready.ConnectionId == Guid.Empty || Closed.Task.IsCompleted || Acknowledged
                || AcknowledgementPending || ConnectionClosed
                || (ConnectionId != Guid.Empty && ConnectionId != ready.ConnectionId)) return false;
            ConnectionId = ready.ConnectionId;
            Ready = ready;
            AcknowledgementPending = true;
            return true;
        }

        internal bool CompleteIfReady() =>
            Acknowledged && ConnectionClosed && Closed.TrySetResult(true);

        internal bool AcceptPeerClosure(ScaleformBridgeConnectionClosed value)
        {
            if (value.Channel != "command" || !value.PeerClosed || ConnectionId == Guid.Empty
                || ConnectionId != value.ConnectionId) return false;
            ConnectionClosed = true;
            return true;
        }
    }

    private void OnNativePlayerLifecycleStateChanged(
        object? sender,
        NativePlayerLifecycleStatePayload value)
    {
        AcceptSpectateState(value);
        var (died, finished) = AcceptLifecycleEdges(value);
        if (died) _ = ReportLocalDeathAsync(value);
        if (finished) _ = ReportLocalMissionCompleteAsync($"native level exit #{value.LevelExitSequence}");
    }

    private async Task ReportLocalDeathAsync(NativePlayerLifecycleStatePayload value)
    {
        if (CaptureDeathReport() is not { } report) return;
        _diagnostics.Log("PlayerLifecycle",
            $"Local player death #{value.DeathSequence} reported for generation {report.Generation}.");
        try { await SendLocalDeathAsync(report).ConfigureAwait(false); }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException
            or InvalidOperationException or ProtocolException or OperationCanceledException)
        {
            _diagnostics.Log("PlayerLifecycle", $"Local death report failed: {exception.Message}");
        }
    }

    // One report per launch generation: there is no respawn, and a death seen
    // outside an active round (frontend, stopped session) is not a round event.
    private DeathReport? CaptureDeathReport()
    {
        lock (_stateGate)
        {
            if (_start is null || _deathReportedGeneration == _start.LevelGeneration
                || _missionCompleteReportedGeneration == _start.LevelGeneration) return null;
            if (_host is null && _client is null) return null;
            _deathReportedGeneration = _start.LevelGeneration;
            return new DeathReport(_host, _client, _start.LevelGeneration);
        }
    }

    private Task SendLocalDeathAsync(DeathReport report)
    {
        if (report.Host is not null)
            return report.Host.ReportHostPlayerDeathAsync(report.Generation, _lifetime.Token);
        return report.Client!.SendPlayerDeathAsync(report.Generation, _lifetime.Token);
    }

    private bool IsCurrentLobbySource(object? sender, Guid sessionId) =>
        sender is LobbyHost host && ReferenceEquals(host, _host) && host.SessionId == sessionId
        || sender is LobbyClient client && ReferenceEquals(client, _client) && client.SessionId == sessionId;

    private void OnRoundEnded(object? sender, RoundEndedPayload value)
    {
        lock (_stateGate)
            if (!IsCurrentLobbySource(sender, value.SessionId)) return;
        _ = HandleRoundEndedAsync(value);
    }

    private async Task HandleRoundEndedAsync(RoundEndedPayload value)
    {
        var fence = BeginRoundEnd(value);
        if (fence is null) return;
        _diagnostics.Log("PlayerLifecycle",
            $"Round {value.LevelGeneration} ended ({value.Reason}); minimum lobby return delay " +
            $"{value.ReturnDelayMilliseconds} ms, followed by HUD readiness/closure.");
        if (fence.Recoverable)
        {
            await RecoverSurvivalRoundReturnAsync(fence).ConfigureAwait(false);
            return;
        }
        try
        {
            if (!await ShowRoundEndAsync(fence).ConfigureAwait(false))
            {
                fence.Closed.TrySetResult(false);
                _diagnostics.Log("PlayerLifecycle", "Round return deferred: gameplay HUD is not connected.");
                return;
            }
            await Task.Delay(value.ReturnDelayMilliseconds, _lifetime.Token).ConfigureAwait(false);
            // The delay alone cannot establish that the old movie has stopped
            // its callbacks. Wait for READY and closure of that exact socket.
            if (!await fence.Closed.Task.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token).ConfigureAwait(false)) return;
            await ReturnToLobbyAsync(fence).ConfigureAwait(false);
        }
        catch (OperationCanceledException) { }
        catch (TimeoutException)
        {
            fence.Closed.TrySetResult(false);
            _diagnostics.Log("PlayerLifecycle",
                $"Round {value.LevelGeneration} return deferred: HUD readiness/connection closure timed out. " +
                "No native frontend request was issued; Disconnect remains available in Pause.");
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException)
        {
            fence.Closed.TrySetResult(false);
            _diagnostics.Log("PlayerLifecycle", $"Round return failed: {exception.Message}");
        }
    }

    private async Task RecoverSurvivalRoundReturnAsync(RoundReturnFence fence)
    {
        try
        {
            await NotifySurvivalReturnAsync(fence).ConfigureAwait(false);
            await Task.Delay(fence.Round.ReturnDelayMilliseconds, _lifetime.Token).ConfigureAwait(false);
            while (CurrentSurvivalReturnFence(fence) is { } current)
            {
                await NotifySurvivalReturnAsync(fence = current).ConfigureAwait(false);
                if (!await WaitForSurvivalReturnAsync(fence).ConfigureAwait(false)) continue;
                await ReturnToLobbyAsync(fence).ConfigureAwait(false);
                return;
            }
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException
            || (exception is OperationCanceledException && _lifetime.IsCancellationRequested))
        { _diagnostics.Log("PlayerLifecycle", $"Round return stopped: {exception.Message}"); }
    }

    private RoundReturnFence? CurrentSurvivalReturnFence(RoundReturnFence fence)
    {
        lock (_stateGate)
        {
            if (!ReferenceEquals(_roundReturnFence, fence) || _start is null
                || _start.SessionId != fence.Round.SessionId
                || _start.LevelGeneration != fence.Round.LevelGeneration) return null;
            if (!fence.CanRenew) return fence.Closed.Task.IsCompletedSuccessfully
                && !fence.Closed.Task.Result ? null : fence;
            // Renewal is allowed only after the old peer actually closed and
            // its ACK continuation settled. The new movie must prove readiness.
            _roundReturnFence = new RoundReturnFence(fence.Round, recoverable: true);
            fence.Closed.TrySetResult(false);
            return _roundReturnFence;
        }
    }

    private (bool Show, ScaleformBridgeEvent? Ready) ReserveSurvivalReturnNotification(RoundReturnFence fence)
    {
        lock (_stateGate)
        {
            if (!ReferenceEquals(_roundReturnFence, fence)) return (false, null);
            if (fence.ConnectionId == Guid.Empty) return (true, null);
            if (fence.Acknowledged || fence.ConnectionClosed || fence.AcknowledgementPending
                || fence.Ready is not { } ready) return (false, null);
            fence.AcknowledgementPending = true;
            return (false, ready);
        }
    }

    private async Task NotifySurvivalReturnAsync(RoundReturnFence fence)
    {
        var notification = ReserveSurvivalReturnNotification(fence);
        if (notification.Ready is { } ready) _ = AcknowledgeRoundReturnAsync(ready, fence);
        if (!notification.Show) return;
        try { await ShowRoundEndAsync(fence).ConfigureAwait(false); }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException)
        { _diagnostics.Log("PlayerLifecycle", $"Round HUD notification deferred: {exception.Message}"); }
    }

    private async Task<bool> WaitForSurvivalReturnAsync(RoundReturnFence fence)
    {
        try
        {
            var completed = await Task.WhenAny(fence.Closed.Task, fence.Retry.Task)
                .WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token).ConfigureAwait(false);
            if (completed == fence.Closed.Task) return await fence.Closed.Task.ConfigureAwait(false);
            lock (_stateGate) fence.Retry = new(TaskCreationOptions.RunContinuationsAsynchronously);
            await Task.Delay(250, _lifetime.Token).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            _diagnostics.Log("PlayerLifecycle",
                $"Round {fence.Round.LevelGeneration} return deferred: awaiting HUD readiness/peer closure; retry remains active.");
        }
        return false;
    }

    private RoundReturnFence? BeginRoundEnd(RoundEndedPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || _start.SessionId != value.SessionId || _start.LevelGeneration != value.LevelGeneration
                || _roundEndingGeneration == value.LevelGeneration) return null;
            _roundEndingGeneration = value.LevelGeneration;
            _roundReturnFence = new RoundReturnFence(value, LevelGameMode(_start.Level) == "Survival");
            return _roundReturnFence;
        }
    }

    private Task<bool> ShowRoundEndAsync(RoundReturnFence fence)
    {
        var value = fence.Round;
        var seconds = (value.ReturnDelayMilliseconds + 999) / 1000;
        var victory = value.Reason == MissionCompleteReason;
        var title = victory ? MissionCompleteTitle : RoundEndTitle;
        var message = victory ? MissionCompleteMessage : RoundEndMessage;
        if (value.Reason is "AlienWin" or "SurvivorsWin")
        {
            bool localAlien;
            lock (_stateGate)
                localAlien = _start?.AlienVsSurvivors == true
                    && _start.AlienPeerId == LocalPeerId();
            var alienWon = value.Reason == "AlienWin";
            title = alienWon ? "ALIEN VICTORY" : "SURVIVORS VICTORY";
            message = alienWon
                ? (localAlien ? "All survivors eliminated." : "The Alien eliminated all survivors.")
                : (localAlien ? "The survivors escaped." : "The survivors escaped the Alien.");
        }
        return _bridge.SendAsync("ROUND_END", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["title"] = title,
            ["message"] = message,
            ["seconds"] = seconds.ToString(CultureInfo.InvariantCulture),
            ["generation"] = value.LevelGeneration.ToString(CultureInfo.InvariantCulture),
            ["sessionId"] = value.SessionId.ToString("D"),
            ["returnToken"] = fence.Token.ToString("D"),
            ["retryReturn"] = fence.Recoverable ? "1" : "0"
        }, _lifetime.Token);
    }

    private void AcceptRoundReturnReady(ScaleformBridgeEvent value)
    {
        var message = value.Message;
        if (value.Channel != "command" || value.ConnectionId == Guid.Empty || value.Reply is null
            || message.Get("source") != "pickup_overlay" || message.Get("phase") != "gameplay"
            || !Guid.TryParse(message.Get("sessionId"), out var sessionId)
            || !Guid.TryParse(message.Get("returnToken"), out var token)
            || !int.TryParse(message.Get("generation"), NumberStyles.Integer, CultureInfo.InvariantCulture, out var generation)) return;
        RoundReturnFence fence;
        lock (_stateGate)
        {
            if (_roundReturnFence is not { } current || current.Token != token || current.Round.SessionId != sessionId
                || current.Round.LevelGeneration != generation || _start is null || _start.SessionId != sessionId
                || _start.LevelGeneration != generation || current.Closed.Task.IsCompleted) return;
            fence = current;
            if (!fence.TryBind(value)) return;
        }
        _diagnostics.Log("PlayerLifecycle", $"Round {generation}: HUD ready; acknowledging before its connection closes.");
        _ = AcknowledgeRoundReturnAsync(value, fence);
    }

    private async Task AcknowledgeRoundReturnAsync(ScaleformBridgeEvent value, RoundReturnFence fence)
    {
        try
        {
            if (!await value.Reply!("ROUND_RETURN_CLOSE", new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["sessionId"] = fence.Round.SessionId.ToString("D"),
                ["generation"] = fence.Round.LevelGeneration.ToString(CultureInfo.InvariantCulture),
                ["returnToken"] = fence.Token.ToString("D")
            }).ConfigureAwait(false)) return;
            lock (_stateGate)
            {
                if (!ReferenceEquals(_roundReturnFence, fence)) return;
                fence.Acknowledged = true;
                CompleteRoundReturnFence(fence);
            }
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException)
        { _diagnostics.Log("PlayerLifecycle", $"Round return acknowledgement failed: {exception.Message}"); }
        finally
        {
            lock (_stateGate)
            {
                fence.AcknowledgementPending = false;
                if (fence.Recoverable && !fence.Acknowledged) fence.Retry.TrySetResult(true);
            }
        }
    }

    // The ACK write continuation and reader termination can run in either order.
    // Both must complete for this authorized connection before native teardown.
    private void CompleteRoundReturnFence(RoundReturnFence fence)
    {
        if (fence.CompleteIfReady())
            _diagnostics.Log("PlayerLifecycle", $"Round {fence.Round.LevelGeneration}: acknowledged HUD connection closed.");
    }

    private void OnRoundBridgeConnectionClosed(object? sender, ScaleformBridgeConnectionClosed value)
    {
        if (value.Channel == "command") _steamFrontendReady = false;
        if (value.Channel != "command" || !value.PeerClosed) return;
        lock (_stateGate)
        {
            var fence = _roundReturnFence;
            if (fence is null || !fence.AcceptPeerClosure(value)) return;
            CompleteRoundReturnFence(fence);
        }
    }

    private async Task ReturnToLobbyAsync(RoundReturnFence fence)
    {
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            if (!EndRoundGameplayState(fence)) return;
            await CloseNativeWorldBusAsync("round-ended").ConfigureAwait(false);
            lock (_stateGate)
                if (!ReferenceEquals(_roundReturnFence, fence) || _start is not null) return;
            await RequestNativeFrontendAsync().ConfigureAwait(false);
            lock (_stateGate)
                if (ReferenceEquals(_roundReturnFence, fence)) _roundReturnFence = null;
            PublishState();
        }
        finally { _commandGate.Release(); }
    }

    // Keep the lobby connection and roster; drop every per-round replication
    // state exactly like a stop would, so the next LobbyStart starts clean.
    private bool EndRoundGameplayState(RoundReturnFence fence)
    {
        var value = fence.Round;
        lock (_stateGate)
        {
            if (!ReferenceEquals(_roundReturnFence, fence) || _start is null
                || _start.SessionId != value.SessionId || _start.LevelGeneration != value.LevelGeneration) return false;
            _start = null;
            _uiGameplayLoaded = false;
            _localGameplayReady = false;
            _localReadySending = false;
            _remoteGameplayReady = false;
            ResetWorldReplicationState(preserveCommittedCheckpoint: true);
            ResetRemoteSpawnState();
            _gameplayReadiness.Reset();
            _frontendLobbyReturnRole = _host is not null ? "host" : "client";
            return true;
        }
    }

    private async Task RequestNativeFrontendAsync()
    {
        await ReleaseSessionLoadingBestEffortAsync().ConfigureAwait(false);
        try
        {
            var result = await _controller.ExecuteCommandAsync(GameCommandNames.ReturnToFrontend,
                new Dictionary<string, string>(StringComparer.Ordinal), _lifetime.Token).ConfigureAwait(false);
            if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
            _diagnostics.Log("PlayerLifecycle",
                $"Return to frontend was rejected: {result.StatusCode} - {result.Message}");
        }
        catch (Exception exception)
        {
            _diagnostics.Log("PlayerLifecycle", $"Return to frontend failed: {exception.Message}");
        }
    }

    // The frontend movie reconnects after the level unloads. CONFIG and the
    // lobby snapshot were already re-sent by HELLO; LOBBY_RETURN asks the menu
    // to reopen the lobby page instead of leaving the player on the root menu.
    private async Task SendFrontendReadyAsync()
    {
        await SendMenuReadyStateAsync().ConfigureAwait(false);
        await SendStatusAsync("Launcher ready").ConfigureAwait(false);
        string? role;
        lock (_stateGate)
        {
            role = _host is null && _client is null ? null : _frontendLobbyReturnRole;
            _frontendLobbyReturnRole = null;
        }
        if (role is null) return;
        await _bridge.SendAsync("LOBBY_RETURN", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["role"] = role
        }).ConfigureAwait(false);
    }

    private void ClearRoundLifecycleState()
    {
        _roundReturnFence?.Closed.TrySetResult(false);
        _roundReturnFence = null;
        _deathReportedGeneration = 0;
        _roundEndingGeneration = 0;
        ClearMissionCompleteState();
        _spectatePeer = 0;
        _spectateCandidates = 0;
        _frontendLobbyReturnRole = null;
    }
}
