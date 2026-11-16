# Steam relay hosting (unofficial)

This branch lets players host and join **through Steam's relay network**, so the
host does not need port forwarding, a VPN or a tool like Porthole. It also adds an
in-game **Friends** page (join friends, accept invites), an **Invite Friends** page
inside every lobby, and fills the in-game **Server Browser** with Steam lobbies.

**Steam is an extra way to connect, not a replacement.** A hosted lobby still listens
on its normal TCP port, so port forwarding, direct IP and **Direct Connection** work
exactly as before.

It uses Steamworks with app ID **480 (Spacewar)**, Valve's public test app. Every
Steam account can use it for free. The launcher activates Spacewar only after
Alien: Isolation's frontend and native runtime report that the menu is ready.
When the game is relaunched, the Steam client API is reinitialized at the new
process's ready menu. Steam's displayed game and real invitation routing still
need to be checked together during two-account testing.

## How it works

The lobby protocol is unchanged. Only the transport underneath it changes:

- **Host:** when you create a lobby, the launcher still listens on its normal TCP
  port, **and** accepts Steam relay connections, bridging each one to that port on
  `127.0.0.1`. It also sets Steam rich presence so friends see **Join Game**, and
  creates a Steam lobby (the "advert") with the visibility you picked, which is what
  the server browser lists and what in-game invites point to.
- **Client:** accepting a Steam invite or selecting **Join Game** opens a Steam
  connection and a one-shot loopback TCP listener. The normal lobby client
  completes its compatibility checks and opens the lobby automatically. Joining
  from the server browser, a friend or an in-game invite does the same after
  joining the Steam lobby, and always connects to the Steam-verified lobby owner.
- **Direct Connection** continues to use only the saved IP/hostname and port.
  Steam invitations never overwrite these settings, and `steam:` addresses are
  refused there.

Code: `AIMP.Launcher/src/AIMP.Launcher/SteamRelay.cs` (transport),
`SteamRelay.Lobbies.cs` (Steam lobbies, friends, invites, rich presence),
`MultiplayerSessionCoordinator.SteamRelay.cs` (host/join wiring) and
`MultiplayerSessionCoordinator.SteamLobbies.cs` (browser, friends and invite commands).
Menu: `Resources/UI/Source/NEWFRONTENDMENU/frontend/*.as`, compiled into
`Resources/UI/NEWFRONTENDMENU.GFX` (see "Rebuilding the menu").

## In-game menu

**Multiplayer → Create Server → Visibility** (default **Private**):

| Visibility | Who sees it in the Server Browser | How others join |
| --- | --- | --- |
| Private | Nobody | Steam friends by invite (Invite Friends), or anyone by direct IP |
| Friends Only | Your Steam friends | Steam friends only: browser, Friends page, invite, Join Game |
| Public | Everyone running this build | Anyone: browser, Friends page, invite, Join Game |

**Private = not listed.** The lobby keeps its 4 slots but is never shown in a server
browser (it is a private Steam lobby). Friends join it through **Invite Friends**, and
anyone can still join by direct IP. While nobody has joined, the host can press
**Start Game** right away and play solo (WorldSync is skipped for a solo round). Match
Type stays available: a solo Alien vs Survivors lobby lets the host play the Alien.

SteamIDs are public, so for Friends Only the host's relay only accepts connections from
the host's Steam friends (others are rejected and logged as
`Rejected Steam relay peer ...`). At most 16 relay connections are accepted at once.
Direct IP / port forwarding is not limited by this setting: anyone who can reach your
forwarded port can still connect, exactly as before.

**Multiplayer → Join Server** (Server Browser): lists up to 8 Steam lobbies (public
ones plus your friends' Friends Only lobbies). The last column says `Public`,
`Friend`, `In game` or `Other version`; the last two can't be joined. **Filter**
switches between All / Campaign / Survival and refreshes the list.

**Multiplayer → Friends**: your online Steam friends, those hosting a lobby you can
join first. At the top, **Accept Invite: <name>** rows show in-game invites you received
(they expire after 30 minutes). Pick a friend and **Join Their Lobby** (only enabled
when that friend hosts a Friends Only / Public lobby). Inviting is not done here.

**Invite Friends** (inside the lobby page, for the host and for joined players): lists
your online Steam friends; pick one to send an invite to the lobby you are in. With a
Steam lobby the friend gets an in-game
**Accept Invite** row plus a Steam chat invite; a player who joined through a Steam
**Join Game** / invite (no Steam lobby) sends a Steam game invite with the host's
address instead, which joins automatically when accepted. For a Friends Only lobby the
invited friend must also be the host's Steam friend.

When an invite arrives you get a notification; accept it under Friends. An invite
accepted in the Steam client (chat / overlay) joins automatically when the main menu
is open and you are not in a lobby; otherwise it waits under Friends.

## Requirements

- Both players run this modified launcher backend (same build).
- Both have the **Steam client running and signed in**, with **different** Steam
  accounts (Steam cannot relay an account to itself).

## Install

1. Close AI-MP (and any leftover `AIMP.Launcher` processes in Task Manager).
2. Back up `<AI-MP folder>\resources\api`.
3. Copy the contents of the published `api` folder over `<AI-MP folder>\resources\api`,
   replacing files. Keep the existing `AIMP.NetZ.dll` and `Injector32\` (they are unchanged).

To undo, restore the backup folder.

## Finding your SteamID64

The launcher logs it at startup: `[Steam] Steam relay ready as steam:7656119xxxxxxxxxx`.
It is also the 17-digit number in your Steam profile URL
(`steamcommunity.com/profiles/7656119...`). For custom profile URLs, Steam →
Account details shows "Steam ID".

## Two-player test

**Host (player A)**

1. Start Steam, then the AI-MP launcher, then the game from the launcher.
2. Multiplayer → Create Server → set **Visibility** to **Friends Only** or **Public**
   (or **Private** and invite B), then **Create Lobby**. The status line shows
   `Lobby created (listed for friends) - Invite Friends or share steam:<A's id>`.
3. In the lobby page → **Invite Friends** → pick B. (Or in the Steam friends list
   right-click B → **Invite to Game**.)

**Joiner (player B)**

1. Start Steam, the AI-MP launcher and the game.
2. Any of: Multiplayer → Friends → **Accept Invite: A**; Multiplayer → Friends → A →
   **Join Their Lobby**; Multiplayer → Join Server → pick A's lobby; accept A's
   invite in Steam or click A → **Join Game** in the Steam friends list.
3. The launcher joins A's lobby automatically and opens the lobby screen.
   No Direct Connection action is needed. If the game frontend is not ready,
   the invitation waits until the main menu becomes ready. Start the game from
   the launcher if it is closed. Leave any current lobby/game before accepting.
4. Confirm the saved IP address and port have not changed. Leave the lobby and
   test **Direct Connection** separately with an IP address.

**Check**

- Both see each other in the lobby; ready up and start a round; move, pick items up,
  use voice chat.
- Launcher logs (`%LOCALAPPDATA%\AIMP\logs`) should show `[Steam]` lines:
  host: `Steam lobby <id> created (Friends)`, `Invited B ...`,
  `Steam relay peer <B> connected; bridging to lobby port 28015.`;
  joiner: `Lobby invite from A`, `Joined Steam lobby <id>; host is steam:<A>.`,
  `Steam relay connected to steam:<A>.`
- `Server browser: N AIMP lobbies found` and `Friends page: N online friend(s)` are
  logged each time those pages refresh.
- Leaving the lobby logs `Left Steam lobby` and `Relay tunnel ... closed` on both sides.
- Solo check: create a **Private** lobby and press **Start Game** right away; the
  round should load without waiting, and dying should return you to the lobby.
- Private check: create a **Private** lobby; B joins by invite or direct IP.

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `Steam relay unavailable` in log | Steam not running or not signed in when the game menu became ready. Start Steam; the launcher retries every 10 s after menu activation. |
| Friends page says "No friends online" | Steam is not running/signed in, or none of your friends are online. |
| Friend's lobby isn't in the browser | It is Private (ask for an invite), the friend runs a different build, or the game already started. |
| `The lobby is full` when joining | The lobby already has 4 players. |
| `That lobby runs a different AIMP version` | Both players need the same build. |
| Host log: `Rejected Steam relay peer ...: not a Steam friend` | The lobby is Friends Only and that player isn't the host's Steam friend. Add them as a friend or make the lobby Public. |
| Log: `Rejected a command connection that is not from the game` | Something other than the game (for example a web page) tried to send commands to the launcher's UI bridge. It was ignored. |
| `Could not reach steam:... through Steam relay` | Host has no lobby open, host is not running this build, or wrong ID. |
| `That Steam ID is your own account` | Both PCs use the same Steam account; use two accounts. |
| Join Game in friends list launches Spacewar | The joiner's launcher was not open. Open the launcher first, then click Join Game. |
| Works by IP but not by Steam | Check both installs have `steam_api64.dll` and `Facepunch.Steamworks.Win64.dll` in `resources\api`. |

## Rebuilding the menu

The `.as` files are the menu scripts; the game loads the compiled `NEWFRONTENDMENU.GFX`.
Compile them with JPEXS FFDec, then keep only the compiled bytecode (FFDec's rewritten
display list must never be shipped):

```
ffdec-cli -importScript Resources\UI\NEWFRONTENDMENU.GFX compiled.GFX <dir containing scripts\frontend\*.as>
node tools/ui/merge-frontend-bytecode.cjs Resources\UI\NEWFRONTENDMENU.GFX compiled.GFX Resources\UI\NEWFRONTENDMENU.GFX
```

FFDec warns about an unreadable `PlaceObject2` tag; the shipped file has it too and the
merge step keeps it as-is.

## Limitations

- Both launchers must already be running when the invitation is accepted.
- An invitation does not interrupt an existing lobby or running round.
- Each peer's receive queue is capped at 128 messages of at most 64 KiB
  (8 MiB plus one active TCP write). Overflow disconnects the peer rather than
  dropping bytes from the reliable protocol stream.
- Spacewar initialization restores the launcher's original Steam environment;
  AI.exe cannot inherit Spacewar app IDs from this integration.
- An official AI-MP update replaces `resources\api` and removes this change.
- Steam relay adds the relay's latency (usually small; it routes via Valve's nearest PoPs).

## Build 464 validation

- Eight automated Steam tests pass without requiring Steam: bounded reliable
  receive order/overflow, environment restoration, address validation/disposal,
  automatic lobby handshake, queued invite and failed-join cleanup, IP-only
  connection, concurrent invitations, and callback cancellation on shutdown.
- The complete managed suite reports 169/178 passing. All nine failing cases
  also fail on unmodified main `eca2e35`: MissionSync survival frontier/catalog
  barrier, Protocol models, posture/torch protocol version assertions, WS3
  wire/authority/manifest, launcher reset scope, native DLL priority, and the
  existing launcher body-length architecture check.
- Release native x86 and self-contained API x64/Injector32 x86 publication
  succeeded. API artifacts were delivered to both test PCs without deleting
  destination-only files. SHA-256 equality was checked for the three AIMP DLLs
  and both Steam dependencies.
- Live Steam invitation acceptance, actual Valve transport, game UI transitions,
  and a complete two-account game round still require manual in-game validation.

## Build 465: activate Spacewar at the ready game menu

- Opening the desktop launcher no longer initializes Steam immediately.
- The first native-ready frontend HELLO or MENU_STATE activates Spacewar after
  AI.exe has started. Repeated menu heartbeats keep the existing Steam session.
- A new game PID reinitializes Steam at its ready menu, even when the launcher
  remains open between game launches. Gameplay updates do not activate Steam.
- Logs include `Game menu ready for PID=...; activating Spacewar relay after
  Alien: Isolation startup.` followed by the initialization result.
- Eleven Steam tests plus the posture/torch wire tests pass (13 total). New
  coverage checks deferred activation, repeat heartbeats, a new game process,
  gameplay suppression and a worker that cannot initialize Steam before a menu.
- Both PCs require build 465. Actual Steam presence priority and invitation
  routing with both games open require another in-game test.

## Friends page, server browser, Invite Friends and solo lobbies (on build 465)

- Ported from `AIMPSteamWork-1.06_F` on top of build 465 without changing its
  transport, deferred Spacewar activation or automatic invite joining.
- Steam lobbies, the friends list and in-game invites live in `SteamRelay.Lobbies.cs`
  behind `ISteamLobbies`; the coordinator falls back to the plain relay when a relay
  has no lobby support (the Steam tests' fake), so build 465's tests are unchanged.
- Hardening: the UI bridge drops connections whose first line is HTTP or binary
  (web pages could otherwise send menu commands to 127.0.0.1:12345); Friends Only
  relays accept only the host's Steam friends; at most 16 relay connections.
- New tests: UI bridge rejects browser traffic / accepts game traffic, solo lobby
  starts alone / rejects joins, multiplayer lobby still needs a client. The managed
  suite reports 180/186; the 6 failures are the same as on unmodified build 465
  (MissionSync frontier and catalog barrier, Protocol models, WS3, native DLL
  priority, launcher body length; no new body-length entries).
- Not yet tested in-game or with a second Steam account.
