using System.Globalization;
using AIMP.Protocol;
using Steamworks;
using Steamworks.Data;

namespace AIMP.Launcher;

// Steam matchmaking lobbies (server browser), friends list and invites.
// The Steam lobby is only an advert + invite channel: the game traffic still
// goes through the relay tunnel to the host's normal lobby port.
internal sealed partial class SteamRelay
{
    private const string MarkerKey = "aimp";
    private const int MaxSearchResults = 50;
    private const int MaxFriends = 24;
    internal const int MaxInvites = 6;
    internal static readonly TimeSpan InviteLifetime = TimeSpan.FromMinutes(30);
    private static readonly TimeSpan InviteRepeatWindow = TimeSpan.FromSeconds(10);
    private static readonly string ProtocolTag = ProtocolConstants.Version.ToString(CultureInfo.InvariantCulture);

    // Pump thread only.
    private Lobby? _lobby;
    private bool _lobbyIsHost;
    private SteamLobbyVisibility _lobbyVisibility = SteamLobbyVisibility.Public;
    private SteamLobbyAdvert? _advert;
    private ulong _clientHostSteamId;

    private readonly List<SteamLobbyInvite> _invites = new(); // guarded by _gate

    public event Action<SteamLobbyInvite>? InviteReceived;

    internal static SteamLobbyVisibility ParseVisibility(string? value) =>
        value?.Trim().ToLowerInvariant() switch
        {
            "public" => SteamLobbyVisibility.Public,
            "friends" or "friendsonly" or "friends_only" => SteamLobbyVisibility.Friends,
            _ => SteamLobbyVisibility.Private
        };

    // Named handlers: Steam is re-initialized for every new game process.
    private void HookLobbyCallbacks()
    {
        SteamMatchmaking.OnLobbyInvite += OnLobbyInvite;
        SteamFriends.OnGameLobbyJoinRequested += OnGameLobbyJoinRequested;
    }

    private void UnhookSteamCallbacks()
    {
        SteamFriends.OnGameRichPresenceJoinRequested -= OnRichPresenceJoinRequested;
        SteamMatchmaking.OnLobbyInvite -= OnLobbyInvite;
        SteamFriends.OnGameLobbyJoinRequested -= OnGameLobbyJoinRequested;
    }

    private void OnLobbyInvite(Friend friend, Lobby lobby) =>
        AddInvite(friend.Id, friend.Name, lobby.Id, accepted: false);

    // Accepting a lobby invite from the Steam friends list / chat.
    private void OnGameLobbyJoinRequested(Lobby lobby, SteamId friendId) =>
        AddInvite(friendId, new Friend(friendId).Name, lobby.Id, accepted: true);

    // ---- hosting ---------------------------------------------------------

    /// <summary>Accepts Steam relay connections (bridged to the local lobby port) and
    /// advertises the lobby as a Steam lobby with the requested visibility.
    /// The normal TCP port is untouched, so direct IP / port forwarding keep working.</summary>
    public Task<bool> StartLobbyHostingAsync(int lobbyPort, SteamLobbyVisibility visibility, SteamLobbyAdvert advert) =>
        RunOnPumpAwaitAsync(async () =>
        {
            if (!EnsureInitialized(force: true)) return false;
            StartHostingOnPump(lobbyPort, visibility);
            await CreateHostedLobbyOnPumpAsync(visibility, advert);
            return true;
        });

    private async Task CreateHostedLobbyOnPumpAsync(SteamLobbyVisibility visibility, SteamLobbyAdvert advert)
    {
        _lobbyVisibility = visibility;
        _advert = advert;
        _lobbyIsHost = true;
        try
        {
            var created = await SteamMatchmaking.CreateLobbyAsync(Math.Max(2, advert.MaxPlayers));
            if (created is { } lobby) AdoptHostedLobby(lobby, visibility, advert);
            else _diagnostics.Log("Steam", "Could not create a Steam lobby; the lobby is not listed, but friends can still join from the Steam friends list.");
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Steam", $"Steam lobby creation failed: {exception.Message}");
        }
        ApplyPresence();
    }

    private void AdoptHostedLobby(Lobby lobby, SteamLobbyVisibility visibility, SteamLobbyAdvert advert)
    {
        if (_hostSocket is null) { lobby.Leave(); return; } // hosting stopped while Steam created it
        _lobby = lobby;
        // A private Steam lobby is never listed; only invited players can join it.
        _ = visibility switch
        {
            SteamLobbyVisibility.Public => lobby.SetPublic(),
            SteamLobbyVisibility.Private => lobby.SetPrivate(),
            _ => lobby.SetFriendsOnly()
        };
        lobby.SetJoinable(true);
        WriteAdvert(lobby, advert);
        _diagnostics.Log("Steam", $"Steam lobby {lobby.Id.Value} created ({visibility}).");
    }

    /// <summary>Keeps the hosted lobby's listing (players, map, in-game) current.</summary>
    public Task<bool> UpdateHostedLobbyAsync(SteamLobbyAdvert advert) => RunOnPumpAsync(() =>
    {
        if (!_ready || !_lobbyIsHost || advert == _advert) return false;
        _advert = advert;
        if (_lobby is { } lobby) WriteAdvert(lobby, advert);
        ApplyPresence();
        return true;
    });

    // Pump thread (relay OnConnecting). SteamIDs are public, so a Friends Only
    // lobby must not accept anyone who merely knows the host's ID: only the
    // host's Steam friends get through. Public lobbies take anyone.
    private string? RejectPeer(SteamId peer)
    {
        int hosted;
        lock (_gate) hosted = _tunnels.Count(tunnel => tunnel.ClientManager is null);
        if (hosted >= MaxHostTunnels) return "too many relay connections";
        if (_lobbyVisibility == SteamLobbyVisibility.Public) return null;
        return new Friend(peer).IsFriend ? null : $"not a Steam friend and the lobby is {_lobbyVisibility}";
    }

    private void WriteAdvert(Lobby lobby, SteamLobbyAdvert advert)
    {
        lobby.SetData(MarkerKey, "1");
        lobby.SetData("ver", ProtocolTag);
        lobby.SetData("host", LocalSteamId.ToString(CultureInfo.InvariantCulture));
        lobby.SetData("vis", _lobbyVisibility.ToString().ToLowerInvariant());
        lobby.SetData("name", Clip(advert.Name, 48));
        lobby.SetData("mode", advert.Mode);
        lobby.SetData("map", Clip(advert.Map, 64));
        lobby.SetData("players", advert.Players.ToString(CultureInfo.InvariantCulture));
        lobby.SetData("max", advert.MaxPlayers.ToString(CultureInfo.InvariantCulture));
        lobby.SetData("state", advert.Joinable ? "lobby" : "ingame");
    }

    // ---- joining ---------------------------------------------------------

    /// <summary>Joins an AIMP Steam lobby (browser, invite or friend) and returns the
    /// host's SteamID for the relay tunnel. Throws with a user-facing message.</summary>
    public Task<ulong> JoinLobbyAsync(ulong lobbyId) => RunOnPumpAwaitAsync(() => JoinLobbyOnPumpAsync(lobbyId));

    private async Task<ulong> JoinLobbyOnPumpAsync(ulong lobbyId)
    {
        if (!EnsureInitialized(force: true))
            throw new InvalidOperationException("Steam is not running or not signed in. Start Steam and try again.");
        LeaveLobbyOnPump();
        var joined = await SteamMatchmaking.JoinLobbyAsync(lobbyId);
        if (joined is not { } lobby)
            throw new InvalidOperationException("Could not join that lobby. It may be full, closed or private.");
        ulong host;
        try { host = ValidateJoinedLobby(lobby); }
        catch { lobby.Leave(); throw; }
        _lobby = lobby;
        _lobbyIsHost = false;
        _lobbyVisibility = ParseVisibility(lobby.GetData("vis"));
        ApplyPresence();
        _diagnostics.Log("Steam", $"Joined Steam lobby {lobby.Id.Value}; host is steam:{host}.");
        return host;
    }

    private ulong ValidateJoinedLobby(Lobby lobby)
    {
        if (lobby.GetData(MarkerKey) != "1")
            throw new InvalidOperationException("That Steam lobby is not an AIMP lobby.");
        if (lobby.GetData("ver") != ProtocolTag)
            throw new InvalidOperationException("That lobby runs a different AIMP version. Both players need the same build.");
        if (lobby.GetData("state") == "ingame")
            throw new InvalidOperationException("That lobby has already started a game. Try again when it returns to the lobby.");
        // Connect to the Steam-verified owner, never to a SteamID taken from lobby data.
        var host = lobby.Owner.Id.Value;
        if (host == 0 || lobby.GetData("host") != host.ToString(CultureInfo.InvariantCulture))
            throw new InvalidOperationException("That lobby's listing does not match its owner. It was not joined.");
        if (host == LocalSteamId)
            throw new InvalidOperationException("That is your own lobby.");
        return host;
    }

    private void LeaveLobbyOnPump()
    {
        if (_lobby is { } lobby)
        {
            try { lobby.Leave(); }
            catch (Exception exception) { _diagnostics.Log("Steam", $"Leaving Steam lobby: {exception.Message}"); }
            _diagnostics.Log("Steam", $"Left Steam lobby {lobby.Id.Value}.");
        }
        _lobby = null;
        _lobbyIsHost = false;
        _advert = null;
        _clientHostSteamId = 0;
        if (_ready) ApplyPresence();
    }

    // ---- server browser --------------------------------------------------

    /// <summary>Public AIMP lobbies plus lobbies friends host or advertise to friends.</summary>
    public Task<IReadOnlyList<SteamLobbyListing>> FindLobbiesAsync() => RunOnPumpAwaitAsync(FindLobbiesOnPumpAsync);

    private async Task<IReadOnlyList<SteamLobbyListing>> FindLobbiesOnPumpAsync()
    {
        if (!EnsureInitialized(force: true))
            throw new InvalidOperationException("Steam is not running. Start Steam to browse lobbies.");
        var found = await SteamMatchmaking.LobbyList
            .WithKeyValue(MarkerKey, "1")
            .FilterDistanceWorldwide()
            .WithMaxResults(MaxSearchResults)
            .RequestAsync();
        var raw = found ?? Array.Empty<Lobby>();
        var listings = ReadSearchListings(raw, out var unreadable);
        var fromSearch = listings.Count;
        AddFriendListings(listings);
        _diagnostics.Log("Steam", $"Lobby search: Steam returned {raw.Length} lobb{(raw.Length == 1 ? "y" : "ies")} " +
            $"({unreadable} unreadable, {fromSearch} listed), {listings.Count - fromSearch} more from friends' presence.");
        return SortListings(listings.Values);
    }

    // Own lobby is skipped; lobbies without a readable AIMP listing are counted for the log.
    private Dictionary<ulong, SteamLobbyListing> ReadSearchListings(IEnumerable<Lobby> found, out int unreadable)
    {
        var listings = new Dictionary<ulong, SteamLobbyListing>();
        unreadable = 0;
        foreach (var lobby in found)
            if (ReadListing(lobby) is not { } listing) unreadable++;
            else if (listing.HostSteamId != LocalSteamId) listings[listing.LobbyId] = listing;
        return listings;
    }

    // Friends-only lobbies never show up in a search; friends advertise them in rich presence.
    private static void AddFriendListings(IDictionary<ulong, SteamLobbyListing> listings)
    {
        foreach (var friend in SteamFriends.GetFriends())
            if (ReadFriendListing(friend) is { } listing)
                listings[listing.LobbyId] = listing;
    }

    private static IReadOnlyList<SteamLobbyListing> SortListings(IEnumerable<SteamLobbyListing> listings) =>
        listings.OrderByDescending(l => l.Joinable && l.Compatible)
            .ThenByDescending(l => l.FromFriend)
            .ThenByDescending(l => l.Players)
            .ToArray();

    private static SteamLobbyListing? ReadListing(Lobby lobby)
    {
        if (lobby.GetData(MarkerKey) != "1") return null;
        if (!ulong.TryParse(lobby.GetData("host"), NumberStyles.None, CultureInfo.InvariantCulture, out var host)) return null;
        return new SteamLobbyListing(lobby.Id.Value, host,
            Fallback(lobby.GetData("name"), "AIMP Lobby"), Fallback(lobby.GetData("mode"), "Survival"),
            Fallback(lobby.GetData("map"), "Unknown map"), ParseInt(lobby.GetData("players"), lobby.MemberCount),
            ParseInt(lobby.GetData("max"), lobby.MaxMembers), lobby.GetData("state") != "ingame",
            lobby.GetData("ver") == ProtocolTag, FromFriend: false);
    }

    private static SteamLobbyListing? ReadFriendListing(Friend friend)
    {
        if (!friend.IsPlayingThisGame || friend.GetRichPresence(MarkerKey) != "hosting") return null;
        if (!ulong.TryParse(friend.GetRichPresence("aimp_lobby"), NumberStyles.None, CultureInfo.InvariantCulture, out var lobbyId)
            || lobbyId == 0) return null;
        return new SteamLobbyListing(lobbyId, friend.Id.Value,
            Fallback(friend.GetRichPresence("aimp_name"), $"{friend.Name}'s Lobby"),
            Fallback(friend.GetRichPresence("aimp_mode"), "Survival"),
            Fallback(friend.GetRichPresence("aimp_map"), "Unknown map"),
            ParseInt(friend.GetRichPresence("aimp_players"), 1), ParseInt(friend.GetRichPresence("aimp_max"), 4),
            friend.GetRichPresence("aimp_state") != "ingame", friend.GetRichPresence("aimp_ver") == ProtocolTag,
            FromFriend: true);
    }

    // ---- friends + invites -----------------------------------------------

    /// <summary>Online Steam friends; friends hosting a joinable AIMP lobby first.</summary>
    public Task<IReadOnlyList<SteamFriendEntry>> GetFriendsAsync() => RunOnPumpAsync<IReadOnlyList<SteamFriendEntry>>(() =>
    {
        if (!EnsureInitialized(force: true))
            throw new InvalidOperationException("Steam is not running. Start Steam to see your friends.");
        return SteamFriends.GetFriends()
            .Where(friend => friend.IsOnline)
            .Select(DescribeFriend)
            .OrderByDescending(entry => entry.CanJoin)
            .ThenByDescending(entry => entry.RunningAimp)
            .ThenBy(entry => entry.Name, StringComparer.CurrentCultureIgnoreCase)
            .Take(MaxFriends)
            .ToArray();
    });

    private static SteamFriendEntry DescribeFriend(Friend friend)
    {
        var aimp = friend.IsPlayingThisGame ? friend.GetRichPresence(MarkerKey) : null;
        var listing = ReadFriendListing(friend);
        var canJoin = listing is { Joinable: true, Compatible: true };
        return new SteamFriendEntry(friend.Id.Value, Clip(friend.Name, 32), FriendStatus(friend, aimp, listing),
            !string.IsNullOrEmpty(aimp), listing?.LobbyId ?? 0, canJoin);
    }

    private static string FriendStatus(Friend friend, string? aimp, SteamLobbyListing? listing) => listing is not null
        ? listing.Joinable ? $"Hosting {listing.Map} {listing.Players}/{listing.MaxPlayers}" : "Hosting - in game"
        : aimp switch
        {
            "hosting" => "Hosting a lobby",
            "joined" => "In a lobby",
            "menu" => "In AIMP",
            _ when friend.IsPlayingThisGame => "Online",
            _ when friend.GameInfo is not null => "In another game",
            _ when friend.IsBusy => "Busy",
            _ when friend.IsAway || friend.IsSnoozing => "Away",
            _ => "Online"
        };

    /// <summary>Lobby a friend is hosting and advertising, or 0.</summary>
    public Task<ulong> GetFriendLobbyAsync(ulong friendId) => RunOnPumpAsync(() =>
        _ready && ReadFriendListing(new Friend(friendId)) is { Joinable: true } listing ? listing.LobbyId : 0UL);

    public Task<string> InviteFriendAsync(ulong friendId) => RunOnPumpAsync(() => InviteFriendOnPump(friendId));

    // Invites are only sent from inside a lobby. With a Steam lobby the friend gets a
    // lobby invite (in-game "Accept Invite" row and Steam chat); otherwise (a player who
    // joined from a Steam invite / Join Game) a Steam game invite with the host's address.
    private string InviteFriendOnPump(ulong friendId)
    {
        if (!EnsureInitialized(force: true))
            throw new InvalidOperationException("Steam is not running. Start Steam to invite friends.");
        var friend = new Friend(friendId);
        var name = Clip(friend.Name, 32);
        if (!friend.IsFriend) throw new InvalidOperationException("You can only invite your Steam friends.");
        var sent = _lobby is { } lobby ? lobby.InviteFriend(friendId) : InviteToHostGame(friend);
        if (!sent) throw new InvalidOperationException($"Steam could not send the invite to {name}.");
        _diagnostics.Log("Steam", _lobby is { } invited
            ? $"Invited {name} ({friendId}) to Steam lobby {invited.Id.Value}."
            : $"Invited {name} ({friendId}) to join the lobby through Steam.");
        return name;
    }

    private bool InviteToHostGame(Friend friend)
    {
        var host = _hostSocket is not null ? LocalSteamId : _clientHostSteamId;
        if (host == 0) throw new InvalidOperationException("Create or join a lobby first, then invite friends.");
        return friend.InviteToGame(FormatAddress(host));
    }

    public IReadOnlyList<SteamLobbyInvite> GetInvites()
    {
        lock (_gate)
        {
            _invites.RemoveAll(invite => DateTime.UtcNow - invite.ReceivedUtc > InviteLifetime);
            return _invites.ToArray();
        }
    }

    /// <summary>Removes and returns the invite from that friend, if still pending.</summary>
    public SteamLobbyInvite? TakeInvite(ulong fromSteamId)
    {
        lock (_gate)
        {
            var invite = _invites.FirstOrDefault(value => value.FromSteamId == fromSteamId);
            if (invite is not null) _invites.Remove(invite);
            return invite;
        }
    }

    private void AddInvite(SteamId from, string fromName, SteamId lobbyId, bool accepted)
    {
        var invite = new SteamLobbyInvite(from.Value, Clip(fromName, 32), lobbyId.Value, DateTime.UtcNow, accepted);
        lock (_gate)
        {
            // The same invite repeated within a few seconds is not re-announced (notification spam).
            if (!accepted && _invites.Any(value => value.FromSteamId == invite.FromSteamId && value.LobbyId == invite.LobbyId
                    && invite.ReceivedUtc - value.ReceivedUtc < InviteRepeatWindow)) return;
            _invites.RemoveAll(value => value.FromSteamId == invite.FromSteamId);
            _invites.Insert(0, invite);
            if (_invites.Count > MaxInvites) _invites.RemoveRange(MaxInvites, _invites.Count - MaxInvites);
        }
        _diagnostics.Log("Steam", $"Lobby invite from {invite.FromName} ({(accepted ? "accepted in Steam" : "in-game")}), lobby {invite.LobbyId}.");
        var handler = InviteReceived;
        if (handler is not null) Task.Run(() => handler(invite));
    }

    // ---- rich presence ---------------------------------------------------

    // Friends see "aimp" = menu / hosting / joined. While hosting a Friends Only or
    // Public lobby, the Steam "Join Game" connect string and listing details are
    // published (Private lobbies are never listed; friends join them by invite).
    private void ApplyPresence()
    {
        var role = _hostSocket is not null ? "hosting" : _lobby is not null || _clientHostSteamId != 0 ? "joined" : "menu";
        var advert = _advert;
        var listed = role == "hosting" && advert is not null && _lobbyVisibility != SteamLobbyVisibility.Private;
        SteamFriends.SetRichPresence(MarkerKey, role);
        SteamFriends.SetRichPresence("aimp_ver", ProtocolTag);
        SteamFriends.SetRichPresence("status", PresenceStatus(role, advert));
        SetListingPresence(listed ? advert : null);
    }

    private static string PresenceStatus(string role, SteamLobbyAdvert? advert) => role switch
    {
        "hosting" when advert is not null => $"Hosting an AIMP lobby ({advert.Players}/{advert.MaxPlayers})",
        "hosting" => "Hosting an AIMP lobby",
        "joined" => "In an AIMP lobby",
        _ => "In the AIMP menu"
    };

    // A null advert clears every listing key (Steam removes keys set to "").
    private void SetListingPresence(SteamLobbyAdvert? advert)
    {
        string Value(Func<SteamLobbyAdvert, string> read) => advert is null ? string.Empty : read(advert);
        SteamFriends.SetRichPresence("connect", Value(_ => FormatAddress(LocalSteamId)));
        SteamFriends.SetRichPresence("aimp_lobby", Value(_ => _lobby?.Id.Value.ToString(CultureInfo.InvariantCulture) ?? string.Empty));
        SteamFriends.SetRichPresence("aimp_name", Value(a => Clip(a.Name, 48)));
        SteamFriends.SetRichPresence("aimp_mode", Value(a => a.Mode));
        SteamFriends.SetRichPresence("aimp_map", Value(a => Clip(a.Map, 64)));
        SteamFriends.SetRichPresence("aimp_players", Value(a => a.Players.ToString(CultureInfo.InvariantCulture)));
        SteamFriends.SetRichPresence("aimp_max", Value(a => a.MaxPlayers.ToString(CultureInfo.InvariantCulture)));
        SteamFriends.SetRichPresence("aimp_state", Value(a => a.Joinable ? "lobby" : "ingame"));
    }

    private static string Clip(string? value, int max)
    {
        var text = (value ?? string.Empty).Replace('|', '/').Replace('\n', ' ').Replace('\r', ' ').Trim();
        return text.Length <= max ? text : text[..max];
    }

    private static string Fallback(string? value, string fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : Clip(value, 64);

    private static int ParseInt(string? value, int fallback) =>
        int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed) ? parsed : fallback;
}
