# Steam relay hosting (unofficial)

This branch lets players host and join **through Steam's relay network**, so the
host does not need port forwarding, a VPN or a tool like Porthole.

It uses Steamworks with app ID **480 (Spacewar)**, Valve's public test app. Every
Steam account can use it for free; while the launcher is open Steam shows you as
"playing Spacewar".

## How it works

The lobby protocol is unchanged. Only the transport underneath it changes:

- **Host:** when you create a lobby, the launcher still listens on its normal TCP
  port, **and** accepts Steam relay connections, bridging each one to that port on
  `127.0.0.1`. It also sets Steam rich presence so friends see **Join Game**.
- **Client:** if **Host Address** is `steam:<SteamID64>`, the launcher opens a Steam
  relay connection to that account and a one-shot loopback TCP listener, and the
  normal lobby client connects to that listener. A plain IP or host name still
  connects directly, exactly as before.

Code: `AIMP.Launcher/src/AIMP.Launcher/SteamRelay.cs` (transport) and
`MultiplayerSessionCoordinator.SteamRelay.cs` (host/join wiring).

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
2. Multiplayer → create a lobby. The status line shows
   `Lobby created - friends can join with steam:<A's id>`.
3. Either send that `steam:` address to B, or in the Steam friends list right-click B →
   **Invite to Game**.

**Joiner (player B)**

1. Start Steam, the AI-MP launcher and the game.
2. Either set **Host Address** to `steam:<A's id>` (port is ignored), or accept A's invite /
   click A → **Join Game** in the friends list (this fills Host Address automatically;
   the status says "Invite from A accepted").
3. Multiplayer → **Direct Connection**. Status shows "Connecting through Steam relay..."
   then `Connected to steam:<A's id>`.

**Check**

- Both see each other in the lobby; ready up and start a round; move, pick items up,
  use voice chat.
- Launcher logs (`%LOCALAPPDATA%\AIMP\logs`) should show `[Steam]` lines:
  host: `Steam relay peer <B> connected; bridging to lobby port 28015.`;
  joiner: `Steam relay connected to steam:<A>.`
- Leaving the lobby logs `Relay tunnel ... closed` on both sides.

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `Steam relay unavailable` in log | Steam not running or not signed in when the launcher started. Start Steam; the launcher retries every 10 s. |
| `Could not reach steam:... through Steam relay` | Host has no lobby open, host is not running this build, or wrong ID. |
| `That Steam ID is your own account` | Both PCs use the same Steam account; use two accounts. |
| Join Game in friends list launches Spacewar | The joiner's launcher was not open. Open the launcher first, then click Join Game. |
| Works by IP but not by Steam | Check both installs have `steam_api64.dll` and `Facepunch.Steamworks.Win64.dll` in `resources\api`. |

## Limitations

- Accepting an invite fills in Host Address only; the joiner still presses
  **Direct Connection** so the normal lobby checks run unchanged.
- An official AI-MP update replaces `resources\api` and removes this change.
- Steam relay adds the relay's latency (usually small; it routes via Valve's nearest PoPs).
