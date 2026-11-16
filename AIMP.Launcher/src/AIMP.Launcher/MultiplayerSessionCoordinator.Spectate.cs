using System.Globalization;
using AIMP.Protocol;

namespace AIMP.Launcher;

// Spectate (adapted from alien-isolation-coop): native PlayerLifecycle moves the
// dead player's camera onto a living participant and reports which peer it
// watches. The Launcher owns display names, so it draws "SPECTATING <name>" in
// PICKUPOVERLAY. The banner is refreshed while active and hides itself when stale.
internal sealed partial class MultiplayerSessionCoordinator
{
    private ulong _spectatePeer;
    private int _spectateCandidates;
    private bool _spectateHudVisible;
    private DateTimeOffset _nextSpectateBanner;
    private int _spectateBannerRunning;

    private void AcceptSpectateState(NativePlayerLifecycleStatePayload value)
    {
        bool changed;
        lock (_stateGate)
        {
            changed = value.SpectatePeer != _spectatePeer;
            _spectatePeer = value.SpectatePeer;
            _spectateCandidates = value.SpectateCandidates;
            _nextSpectateBanner = DateTimeOffset.MinValue;
        }
        if (changed)
            _diagnostics.Log("PlayerLifecycle", value.SpectatePeer == 0
                ? "Spectate stopped."
                : $"Spectating peer {value.SpectatePeer} ({value.SpectateCandidates} living player(s)).");
    }

    private void TryScheduleSpectateBanner(DateTimeOffset now)
    {
        if (Volatile.Read(ref _disposed) != 0 || now < _nextSpectateBanner
            || Interlocked.CompareExchange(ref _spectateBannerRunning, 1, 0) != 0) return;
        _nextSpectateBanner = now.AddMilliseconds(500);
        _ = SendSpectateBannerAsync();
    }

    // Null while nothing is watched or the round-end screen owns the overlay.
    private Dictionary<string, string>? CaptureSpectateBanner()
    {
        lock (_stateGate)
        {
            if (_spectatePeer == 0 || _start is null || _roundEndingGeneration == _start.LevelGeneration) return null;
            var name = _lobby?.Players.FirstOrDefault(player => player.PeerId == _spectatePeer)?.DisplayName;
            return new Dictionary<string, string>
            {
                ["active"] = "1",
                ["name"] = string.IsNullOrWhiteSpace(name) ? "PLAYER" : name,
                ["others"] = _spectateCandidates.ToString(CultureInfo.InvariantCulture)
            };
        }
    }

    private async Task SendSpectateBannerAsync()
    {
        try
        {
            var banner = CaptureSpectateBanner();
            if (banner is null && !_spectateHudVisible) return;
            var sent = await _bridge.SendAsync("SPECTATE", banner ?? new Dictionary<string, string> { ["active"] = "0" },
                _lifetime.Token).ConfigureAwait(false);
            _spectateHudVisible = banner is not null && sent;
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception error) when (error is IOException or ObjectDisposedException or InvalidOperationException)
        {
            _diagnostics.Log("PlayerLifecycle", "Spectate banner deferred: " + error.Message);
        }
        finally { Interlocked.Exchange(ref _spectateBannerRunning, 0); }
    }
}
