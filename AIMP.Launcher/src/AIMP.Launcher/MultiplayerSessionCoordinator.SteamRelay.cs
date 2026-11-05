using AIMP.Protocol;

namespace AIMP.Launcher;

// Steam relay (Spacewar 480) hosting/joining. The lobby protocol is unchanged:
// a host is reachable both on its TCP port and through Steam, and a client
// whose Host Address is "steam:<SteamID64>" connects through a loopback tunnel.
internal sealed partial class MultiplayerSessionCoordinator
{
    // Null when constructed without a relay (tests); Steam features are then off.
    private readonly SteamRelay? _steamRelay;

    private void InitializeSteamRelay()
    {
        if (_steamRelay is null) return;
        _steamRelay.JoinRequested += OnSteamJoinRequested;
        _steamRelay.Start();
    }

    private async Task StartSteamHostingAsync(LobbyHost host)
    {
        if (_steamRelay is null) return;
        try
        {
            if (!await _steamRelay.StartHostingAsync(host.BoundPort, "Hosting an AIMP lobby").ConfigureAwait(false))
            {
                _diagnostics.Log("Steam", "Steam is not running; this lobby is reachable by direct IP only.");
                return;
            }
            var address = SteamRelay.FormatAddress(_steamRelay.LocalSteamId);
            await SendStatusAsync($"Lobby created - friends can join with {address}").ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Steam", $"Could not start Steam relay hosting: {exception.Message}");
        }
    }

    private async Task<(string Host, int Port, string Label)> ResolveJoinEndpointAsync(LauncherSettings settings)
    {
        if (!SteamRelay.IsSteamAddress(settings.HostAddress))
            return (settings.HostAddress, settings.Port, $"{settings.HostAddress}:{settings.Port}");
        if (!SteamRelay.TryParseAddress(settings.HostAddress, out var hostSteamId))
            throw new ArgumentException(
                "A Steam host address must be steam:<SteamID64> (the 17-digit number from the host's Steam profile).");
        if (_steamRelay is null) throw new InvalidOperationException("Steam relay is not available.");
        await SendStatusAsync("Connecting through Steam relay...").ConfigureAwait(false);
        var port = await _steamRelay.OpenClientTunnelAsync(hostSteamId, _lifetime.Token).ConfigureAwait(false);
        return ("127.0.0.1", port, settings.HostAddress);
    }

    private void CloseSteamClientTunnels(string reason) => _steamRelay?.CloseAllTunnels(reason);

    private async Task StopSteamRelayAsync()
    {
        if (_steamRelay is null) return;
        _steamRelay.CloseAllTunnels("lobby closed");
        try { await _steamRelay.StopHostingAsync().ConfigureAwait(false); }
        catch (Exception exception) { _diagnostics.Log("Steam", $"Stop hosting failed: {exception.Message}"); }
    }

    // A Steam invite or "Join Game" only fills in the Host Address: joining
    // still goes through the menu so the normal lobby checks run unchanged.
    private async void OnSteamJoinRequested(string friendName, string address)
    {
        try
        {
            var settings = _settingsStore.Load();
            _settingsStore.Save(settings with { HostAddress = address });
            _diagnostics.Log("Steam", $"Steam join request from {friendName}: Host Address set to {address}.");
            await SendConfigAsync().ConfigureAwait(false);
            await SendStatusAsync($"Invite from {friendName} accepted - choose Direct Connection to join").ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Steam", $"Could not apply Steam join request: {exception.Message}");
        }
    }
}
