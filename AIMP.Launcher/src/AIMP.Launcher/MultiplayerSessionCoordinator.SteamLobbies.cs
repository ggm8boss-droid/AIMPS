using System.Globalization;
using AIMP.Protocol;

namespace AIMP.Launcher;

// On top of the Steam relay: hosted lobbies are advertised as Steam lobbies
// (Friends Only / Public) for the in-game server browser, the in-game Friends
// page lists Steam friends with Join and pending invites, and lobbies have an
// "Invite Friends" page. Private lobbies are not listed (invite or direct IP only)
// and the host may start one alone.
internal sealed partial class MultiplayerSessionCoordinator
{
    private const int MaxServerRows = 8;

    private LobbyHost? _steamAdvertisedHost;

    // Null when the relay has no lobby support (tests use a transport-only fake).
    private ISteamLobbies? SteamLobbies => _steamRelay as ISteamLobbies;

    // Solo: a Private lobby hosted with nobody else in it (no remote players to wait for).
    private bool IsSoloSessionLocked() => _host?.IsSolo == true;

    private bool IsSoloSession()
    {
        lock (_stateGate) return IsSoloSessionLocked();
    }

    private string WorldSyncSkippedMessage() => IsSoloSession()
        ? "Solo lobby: WorldSync is not needed for this game session; no world manifest will be compiled or loaded."
        : "WorldSync is disabled in Settings for this game session. Character spawn and movement replication remain active; no world manifest will be compiled or loaded.";

    private static SteamLobbyVisibility ReadLobbyVisibility(ScaleformBridgeMessage message) =>
        SteamRelay.ParseVisibility(message.Get("visibility", "private"));

    private Task<bool> StartRelayHostingAsync(LobbyHost host, LobbyHostOptions options, SteamLobbyVisibility visibility)
    {
        if (SteamLobbies is not { } lobbies)
            return _steamRelay!.StartHostingAsync(host.BoundPort, "Hosting an AIMP lobby");
        var advert = new SteamLobbyAdvert(options.ServerName, LevelGameMode(options.Level), options.Level,
            1, options.Capacity, Joinable: true);
        return lobbies.StartLobbyHostingAsync(host.BoundPort, visibility, advert);
    }

    private async Task OnSteamHostingStartedAsync(LobbyHost host, SteamLobbyVisibility visibility)
    {
        if (SteamLobbies is not null)
        {
            lock (_stateGate)
            {
                if (_steamAdvertisedHost is not null) _steamAdvertisedHost.SnapshotChanged -= OnSteamHostSnapshot;
                _steamAdvertisedHost = host;
            }
            host.SnapshotChanged += OnSteamHostSnapshot;
        }
        var address = SteamRelay.FormatAddress(_steamRelay!.LocalSteamId);
        await SendStatusAsync($"Lobby created ({DescribeVisibility(visibility)}) - Invite Friends or share {address}")
            .ConfigureAwait(false);
    }

    private static string DescribeVisibility(SteamLobbyVisibility visibility) =>
        visibility switch
        {
            SteamLobbyVisibility.Public => "listed publicly",
            SteamLobbyVisibility.Private => "not listed",
            _ => "listed for friends"
        };

    private void OnSteamHostSnapshot(object? sender, LobbySnapshotPayload snapshot)
    {
        var advert = new SteamLobbyAdvert(snapshot.ServerName, LevelGameMode(snapshot.Level), snapshot.Level,
            snapshot.Players.Count, snapshot.Capacity,
            Joinable: string.Equals(snapshot.State, "Lobby", StringComparison.Ordinal));
        _ = UpdateSteamAdvertAsync(advert);
    }

    private async Task UpdateSteamAdvertAsync(SteamLobbyAdvert advert)
    {
        try { if (SteamLobbies is { } lobbies) await lobbies.UpdateHostedLobbyAsync(advert).ConfigureAwait(false); }
        catch (Exception exception) { _diagnostics.Log("Steam", $"Steam lobby update failed: {exception.Message}"); }
    }

    private void DetachSteamAdvert()
    {
        LobbyHost? advertised;
        lock (_stateGate) { advertised = _steamAdvertisedHost; _steamAdvertisedHost = null; }
        if (advertised is not null) advertised.SnapshotChanged -= OnSteamHostSnapshot;
    }

    // ---- invites -----------------------------------------------------------

    private void OnSteamInviteReceived(SteamLobbyInvite invite)
    {
        lock (_steamInviteGate)
        {
            if (Volatile.Read(ref _disposed) != 0) return;
            _steamInviteTask = ProcessSteamActionAfterAsync(_steamInviteTask, () => HandleLobbyInviteAsync(invite));
        }
    }

    // An invite accepted in the Steam client joins right away (like upstream's Join Game)
    // when the menu is ready and no lobby/game is open; otherwise it waits under Friends.
    private async Task HandleLobbyInviteAsync(SteamLobbyInvite invite)
    {
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            if (invite.AcceptedInSteam && CanAutoJoinSteamInvite() && SteamLobbies!.TakeInvite(invite.FromSteamId) is { } taken)
                await JoinSteamLobbyAsync(taken.LobbyId, $"{taken.FromName}'s lobby").ConfigureAwait(false);
            else
                await ShowLobbyInviteAsync(invite).ConfigureAwait(false);
        }
        finally { _commandGate.Release(); }
    }

    private async Task ShowLobbyInviteAsync(SteamLobbyInvite invite)
    {
        await _bridge.SendAsync("INVITE_RECEIVED", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["from"] = invite.FromSteamId.ToString(CultureInfo.InvariantCulture),
            ["name"] = invite.FromName
        }).ConfigureAwait(false);
        await SendStatusAsync($"{invite.FromName} invited you - Multiplayer > Friends to accept").ConfigureAwait(false);
    }

    private bool CanAutoJoinSteamInvite()
    {
        if (!_steamFrontendReady || !_controller.Snapshot.IpcConnected) return false;
        lock (_stateGate) return _host is null && _client is null && !_uiGameplayLoaded;
    }

    // ---- bridge commands ---------------------------------------------------

    // Server browser and friends commands. The browser lists Steam lobbies when the
    // Steam relay is available; without it, the original empty list / error is kept.
    private Task HandleBrowserCommandAsync(ScaleformBridgeMessage message) =>
        SteamLobbies is not null ? DispatchSteamBridgeMessageAsync(message)
        : string.Equals(message.Type, "REFRESH_SERVERS", StringComparison.OrdinalIgnoreCase)
            ? SendEmptyServerListAsync()
            : SendErrorAsync("Server discovery is disabled; use Direct Connection.");

    private Task DispatchSteamBridgeMessageAsync(ScaleformBridgeMessage message) =>
        message.Type.Trim().ToUpperInvariant() switch
        {
            "REFRESH_SERVERS" => SendSteamServerListAsync(message.Get("filter", "all")),
            "JOIN_SERVER" => JoinSteamServerAsync(message.Get("serverId")),
            "REFRESH_FRIENDS" => SendSteamFriendListAsync(),
            "INVITE_FRIEND" => InviteSteamFriendAsync(message.Get("friendId")),
            "JOIN_FRIEND" => JoinSteamFriendAsync(message.Get("friendId")),
            "ACCEPT_INVITE" => AcceptLobbyInviteAsync(message.Get("from")),
            _ => LogOnlyAsync($"No multiplayer action is attached to {message.Type}.")
        };

    private ISteamLobbies RequireSteam() => SteamLobbies
        ?? throw new InvalidOperationException("Steam features are not available in this launcher.");

    private async Task SendSteamServerListAsync(string requestedFilter)
    {
        var filter = NormalizeFilter(requestedFilter);
        var lobbies = await TryFindSteamLobbiesAsync().ConfigureAwait(false);
        var shown = (lobbies ?? Array.Empty<SteamLobbyListing>())
            .Where(lobby => MatchesFilter(lobby, filter)).Take(MaxServerRows).ToArray();
        await _bridge.SendAsync("SERVER_LIST", BuildServerListValues(shown)).ConfigureAwait(false);
        if (lobbies is null) return;
        _diagnostics.Log("Steam", $"Server browser: {lobbies.Count} AIMP lobb{(lobbies.Count == 1 ? "y" : "ies")} found, {shown.Length} shown (filter {filter}).");
        await SendStatusAsync(shown.Length == 0
            ? "No Steam lobbies found. Friends can still invite you."
            : $"Found {shown.Length} Steam lobb{(shown.Length == 1 ? "y" : "ies")}").ConfigureAwait(false);
    }

    private async Task<IReadOnlyList<SteamLobbyListing>?> TryFindSteamLobbiesAsync()
    {
        try { return await RequireSteam().FindLobbiesAsync().ConfigureAwait(false); }
        catch (Exception exception)
        {
            await SendStatusAsync(exception.Message).ConfigureAwait(false);
            return null;
        }
    }

    // Only known filters reach matching and the log (the value comes from the bridge).
    private static string NormalizeFilter(string filter) =>
        filter.Trim().ToLowerInvariant() is "campaign" or "survival" ? filter.Trim().ToLowerInvariant() : "all";

    private static bool MatchesFilter(SteamLobbyListing lobby, string filter) =>
        filter == "all" || string.Equals(lobby.Mode, filter, StringComparison.OrdinalIgnoreCase);

    private static Dictionary<string, string> BuildServerListValues(IReadOnlyList<SteamLobbyListing> shown)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["count"] = shown.Count.ToString(CultureInfo.InvariantCulture)
        };
        for (var index = 0; index < shown.Count; index++) AddServerValues(values, $"s{index}_", shown[index]);
        return values;
    }

    private static void AddServerValues(Dictionary<string, string> values, string prefix, SteamLobbyListing lobby)
    {
        values[prefix + "id"] = lobby.LobbyId.ToString(CultureInfo.InvariantCulture);
        values[prefix + "name"] = lobby.Name;
        values[prefix + "mode"] = lobby.Mode;
        values[prefix + "players"] = lobby.Players.ToString(CultureInfo.InvariantCulture);
        values[prefix + "maxPlayers"] = lobby.MaxPlayers.ToString(CultureInfo.InvariantCulture);
        values[prefix + "ping"] = DescribeListing(lobby);
        values[prefix + "locked"] = lobby.Joinable && lobby.Compatible ? "0" : "1";
        values[prefix + "mapId"] = lobby.Map;
        values[prefix + "mapName"] = lobby.Map;
    }

    // Shown in the row's last column (the original menu labels it "ping").
    private static string DescribeListing(SteamLobbyListing lobby) =>
        !lobby.Compatible ? "Other version" : !lobby.Joinable ? "In game" : lobby.FromFriend ? "Friend" : "Public";

    private Task JoinSteamServerAsync(string serverId)
    {
        if (!TryParseSteamId(serverId, out var lobbyId))
            throw new InvalidOperationException("Select a lobby from the list first.");
        return JoinSteamLobbyAsync(lobbyId, "Steam lobby");
    }

    private async Task SendSteamFriendListAsync()
    {
        var steam = RequireSteam();
        var friends = await TryGetSteamFriendsAsync(steam).ConfigureAwait(false);
        var invites = steam.GetInvites();
        _diagnostics.Log("Steam", $"Friends page: {friends.Count} online friend(s), " +
            $"{friends.Count(friend => friend.RunningAimp)} running AIMP, {invites.Count} pending invite(s).");
        await _bridge.SendAsync("FRIEND_LIST", BuildFriendListValues(friends, invites)).ConfigureAwait(false);
    }

    private async Task<IReadOnlyList<SteamFriendEntry>> TryGetSteamFriendsAsync(ISteamLobbies steam)
    {
        try { return await steam.GetFriendsAsync().ConfigureAwait(false); }
        catch (Exception exception)
        {
            await SendStatusAsync(exception.Message).ConfigureAwait(false);
            return Array.Empty<SteamFriendEntry>();
        }
    }

    private static Dictionary<string, string> BuildFriendListValues(
        IReadOnlyList<SteamFriendEntry> friends, IReadOnlyList<SteamLobbyInvite> invites)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["count"] = friends.Count.ToString(CultureInfo.InvariantCulture),
            ["icount"] = invites.Count.ToString(CultureInfo.InvariantCulture)
        };
        for (var index = 0; index < friends.Count; index++) AddFriendValues(values, $"f{index}_", friends[index]);
        for (var index = 0; index < invites.Count; index++)
        {
            values[$"i{index}_from"] = invites[index].FromSteamId.ToString(CultureInfo.InvariantCulture);
            values[$"i{index}_name"] = invites[index].FromName;
        }
        return values;
    }

    private static void AddFriendValues(Dictionary<string, string> values, string prefix, SteamFriendEntry friend)
    {
        values[prefix + "id"] = friend.SteamId.ToString(CultureInfo.InvariantCulture);
        values[prefix + "name"] = friend.Name;
        values[prefix + "status"] = friend.Status;
        values[prefix + "aimp"] = friend.RunningAimp ? "1" : "0";
        values[prefix + "join"] = friend.CanJoin ? "1" : "0";
    }

    // Invites are only sent from inside an active lobby (host or joined player).
    private async Task InviteSteamFriendAsync(string friendId)
    {
        if (!TryParseSteamId(friendId, out var id)) throw new InvalidOperationException("Select a friend first.");
        bool inLobby;
        lock (_stateGate) inLobby = _host is not null || _client is not null;
        if (!inLobby) throw new InvalidOperationException("Create or join a lobby first, then invite friends from the lobby.");
        var name = await RequireSteam().InviteFriendAsync(id).ConfigureAwait(false);
        await SendStatusAsync($"Invite sent to {name}").ConfigureAwait(false);
    }

    private async Task JoinSteamFriendAsync(string friendId)
    {
        if (!TryParseSteamId(friendId, out var id)) throw new InvalidOperationException("Select a friend first.");
        var lobbyId = await RequireSteam().GetFriendLobbyAsync(id).ConfigureAwait(false);
        if (lobbyId == 0)
            throw new InvalidOperationException("That friend is not hosting a lobby you can join. Ask them for an invite.");
        await JoinSteamLobbyAsync(lobbyId, "friend's lobby").ConfigureAwait(false);
    }

    private async Task AcceptLobbyInviteAsync(string from)
    {
        if (!TryParseSteamId(from, out var id)) throw new InvalidOperationException("Select an invite first.");
        var invite = RequireSteam().TakeInvite(id)
            ?? throw new InvalidOperationException("That invite has expired. Ask your friend to invite you again.");
        await JoinSteamLobbyAsync(invite.LobbyId, $"{invite.FromName}'s lobby").ConfigureAwait(false);
    }

    // Joins the Steam lobby, then connects the normal LobbyClient through the
    // relay tunnel to the host's lobby port (the same transport as Steam invites).
    private async Task JoinSteamLobbyAsync(ulong lobbyId, string label)
    {
        var steam = RequireSteam();
        _pendingSteamInvite = null;
        EnsureMenuConnectionAvailable();
        await StopNetworkAsync("Joining a Steam lobby").ConfigureAwait(false);
        await SendStatusAsync($"Joining {label} through Steam...").ConfigureAwait(false);
        var hostSteamId = await steam.JoinLobbyAsync(lobbyId).ConfigureAwait(false);
        try { await ConnectThroughSteamAsync(hostSteamId).ConfigureAwait(false); }
        catch
        {
            _steamRelay!.CloseAllTunnels("join failed");
            try { await _steamRelay.StopHostingAsync().ConfigureAwait(false); } catch { /* leaving the Steam lobby is best effort */ }
            throw;
        }
        await SendMenuLobbyOpenedAsync("client").ConfigureAwait(false);
        await SendStatusAsync($"Connected to {label}").ConfigureAwait(false);
    }

    private async Task ConnectThroughSteamAsync(ulong hostSteamId)
    {
        var port = await _steamRelay!.OpenClientTunnelAsync(hostSteamId, _lifetime.Token).ConfigureAwait(false);
        var settings = _settingsStore.Load();
        var options = new LobbyClientOptions("127.0.0.1", port,
            settings.PlayerId, settings.PlayerName, LobbyCompatibility.Create(settings));
        var client = await LobbyClient.ConnectAsync(options, _lifetime.Token).ConfigureAwait(false);
        await CompleteMenuJoinAsync(client).ConfigureAwait(false);
    }

    private static bool TryParseSteamId(string? value, out ulong id) =>
        ulong.TryParse(value?.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out id) && id != 0;
}
