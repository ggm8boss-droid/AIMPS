# AI-MP — Alien: Isolation Multiplayer

**An experimental, community-driven multiplayer mod for _Alien: Isolation_.**

[![Status](https://img.shields.io/badge/status-experimental-orange)](#vision-and-project-status)
[![Platform](https://img.shields.io/badge/platform-Windows-blue)](#requirements)
[![Languages](https://img.shields.io/badge/languages-C%2B%2B%20%2F%20C%23-blue)](#architecture)

> **This repository contains the AI-MP backend and its Electron desktop shell.** It includes the launcher's local API, the protocol, the 32-bit injector, and the native DLL injected into the game. The web user interface loaded by Electron is maintained in a separate repository.
>
> AI-MP is neither an official product nor a modification supported by the creators of _Alien: Isolation_.

## Table of Contents

- [Vision and Project Status](#vision-and-project-status)
- [Development Philosophy](#development-philosophy)
- [Architecture](#architecture)
- [Main Modules](#main-modules)
- [Requirements](#requirements)
- [Building the Backend](#building-the-backend)
- [Developer Mode](#developer-mode)
- [Contributing](#contributing)
- [Safety, Compatibility, and Game Content](#safety-compatibility-and-game-content)

## Vision and Project Status

_Alien: Isolation_ was designed as a **single-player game**. Its engine, mission scripts, cutscenes, save system, and AI were not built to support multiple human players sharing the same session. AI-MP experiments with adding multiplayer by relying as much as possible on the **game engine's actual systems**, rather than running a parallel simulation of the entire game.

Our goal is easy to describe but difficult to achieve: **allow multiple players to experience survival horror together in a way that is consistent, stable, and faithful to the spirit of the original game.**

**Current status:** An experimental, playable build focused on **Survivor Mode**. The main campaign is **not** supported. Features may contain bugs, cause crashes, or become desynchronized. Some mechanics remain incomplete, especially when multiple systems interact or the number of participants increases.

To play or test the mod under real conditions, you need a legitimate installation of _Alien: Isolation_ **with all DLCs**, because multiplayer environment preparation relies on additional game content.

## Development Philosophy

### 1. Understand the Engine Before Working Around It

We first investigate **where and how the game actually performs an action**: creating a `Character`, making an AI decision, picking up an item, triggering a CATHODE interaction, changing a player's state, and so on.

A native modification must be based on reproducible observations, an identified engine contract, and explicit validation. We avoid artificially recreating behavior in the launcher when the game already provides a suitable mechanism.

### 2. Generic Solutions, Not Scenario-Specific Fixes

**Features should solve system-level problems.** A bug discovered while testing the Alien should, whenever possible, lead to a fix for a generic `AISync` rule—not an `if Alien` exception, a particular GUID, a specific map, or a hardcoded position.

Likewise, a player's identity must come from their session identity (`PeerId` and its associated context), **never** from the fact that they are `THE_PLAYER` in a local game instance. Native pointers and `Character` instances may be recreated after each load.

### 3. Clear Ownership for Every Module

Each module owns a specific gameplay domain and must not take over the responsibilities of other modules:

- `PlayerSync`: Remote player representations and actions.
- `AISync`: AI state and behavior.
- `WorldSync`: World / CATHODE events and states.
- `ItemSync`: Shared state of collectible items on the ground.
- `PlayerLifecycle`: Player life, death, and session / round continuity.

**If no existing module owns a gameplay domain, you may create a new one.**

**Example:** `WorldSync` must not make AI decisions on behalf of `AISync`. Modules can exchange events without taking ownership of one another's responsibilities.

### 4. The Host Is Authoritative

The reference model is **host-authoritative**. The host decides shared gameplay changes: AI decisions, important interactions, item pickups, damage, deaths, and session progression. Clients provide intentions or observations and apply the validated outcomes.

This does not mean the host must transmit every animation frame or every internal object. **Replicate the necessary decision or state, and let the local engine produce what it already knows how to produce.**

### 5. Synchronize Only What Matters

Do not attempt to synchronize “the entire game.” Prefer semantic events, state changes, and information that has a perceptible or functional impact. Account for message ordering, idempotency, late joiners, disconnections, and resynchronization.

Replication that works for two players but deteriorates with more participants is **still a problem to solve**, not a completed feature.

### 6. Robustness and Performance Before Complexity

The mod interacts with an older engine that was not designed for this use case. An incorrectly installed hook, a stale pointer, or a blocking operation on the game thread can crash the entire game.

Our rules: validate signatures and versions before execution; access memory cautiously; use bounded queues; never perform blocking network work on the game thread; keep instrumentation lightweight; and shut down or disable incompatible functionality safely (**fail-closed**).

### 7. Progress Through Testable Increments

Development should follow this path: **research → local prototype → minimal contract → networking → two-player validation → multiplayer testing → stabilization**. Do not introduce campaign features, cutscenes, or special cases before the underlying primitives are robust.

## Architecture

The backend consists of several processes and layers:

| Component                       | Technology                     | Responsibility                                                                                                                                      |
| ------------------------------- | ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `AIMP.Launcher`                 | **ASP.NET Core / .NET 8, x64** | Local API, game preparation, game launch, host/client sessions, orchestration, diagnostics, and networking.                                         |
| `AIMP.Protocol`                 | **C# / .NET 8**                | Contracts and messages exchanged between components.                                                                                                |
| `AIMP.Injector32`               | **C# / .NET 8, x86**           | Injection helper compatible with the game's 32-bit process.                                                                                         |
| `AIMP.Native` / `AIMP.NetZ.dll` | **C++20 / MSVC, x86**          | Access to the `AI.exe` engine, validated hooks, native events, game commands, entity representations, and execution on the appropriate game thread. |
| Frontend                        | **Separate repository**        | User interface; it must not own engine logic or authoritative gameplay logic.                                                                       |

### Synchronization Flow

**Local state:** `AI.exe` engine → observation by `AIMP.Native` → IPC → `AIMP.Launcher` → network → other participants.

**Remote action:** Network → validation/orchestration by `AIMP.Launcher` → IPC → `AIMP.Native` command → execution on the appropriate game-engine thread and objects.

This separation is intentional: **the DLL understands engine objects; the launcher understands sessions and transport**. Avoid reading `AI.exe` memory directly from the launcher to implement new gameplay logic.

### Repository Layout

- `AIMP.Native/src/` and `AIMP.Native/include/aimp/`: Native implementations and interfaces.
- `AIMP.Native/CMakeLists.txt` and `AIMP.Native/CMakePresets.json`: Native build configuration.
- `AIMP.Launcher/src/AIMP.Launcher/`: ASP.NET API, coordination, game preparation, networking, and diagnostics.
- `AIMP.Electron/`: Desktop shell, preload bridge, session authentication, and Windows packaging. Dependencies and staged binaries remain ignored.
- `AIMP.Launcher/src/AIMP.Protocol/`: Shared messages and contracts.
- `AIMP.Launcher/src/AIMP.Injector32/`: x86 injector.
- `AIMP.Launcher/tests/AIMP.Protocol.Tests/` and `AIMP.Native/tests/`: Tests and validations included in the source tree.

## Main Modules

| Module               | What It Does                                                                                             | Key Considerations                                                                   |
| -------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| **Game Preparation** | Automatically prepares the required resources and levels using game files and CathodeLib.                | A fresh installation should work **without manual edits in OpenCAGE**.               |
| **PlayerSync**       | Creates and tracks remote `Characters`; replicates movement, equipment, and actions.                     | Stable peer identity, `Character` lifecycle, interpolation, and avoiding duplicates. |
| **WorldSync 3**      | Replicates world interactions and states, including CATHODE events and doors.                            | Idempotent events, compatible manifests, loading, and state catch-up.                |
| **ItemSync**         | Replicates the state of collectible items on the ground.                                                 | A pickup must not result in conflicting collections by other players.                |
| **PlayerLifecycle**  | Handles participant deaths without blindly applying the single-player `Game Over` to the entire session. | Host death, surviving players, end-of-round behavior, and return to the lobby.       |
| **AISync**           | Aims to synchronize **all AI**, not just the Alien, using host authority.                                | Target identity, behavior transitions, attacks, and continuity after a death.        |
| **Diagnostics**      | Collects logs, versions, and state information needed to investigate incidents.                          | Avoid expensive telemetry and unnecessary sensitive data.                            |

### Hiding places (experimental)

The `feature/hiding-sync` branch (introduced in build 406, network/IPC protocol 30) sends the owner's hiding state with player movement. Once the retail `PLAYER_HIDING` flag is set, peers conceal the remote body, stop its locomotion and torch, and disable the occupied hiding interaction. Leaving reveals the body and releases the interaction. Character destruction, disconnection and level changes clear ownership. Entry and exit animations remain local.

Each stay latches the owner's native hiding search position. Peers resolve the nearest `AI_UI_INTERACT_HIDING` interaction within three metres, then retain its local instance key. This spatial lookup needs two-PC validation for adjacent lockers and low compartments. It does not reserve a locker before entry or arbitrate simultaneous entries; the interaction stays disabled until the final known occupant leaves. AI search/capture decisions are outside this change.

Two-PC acceptance: enter and leave both types of hiding place as host and client; check disappearance/reappearance, blocked use by another player, nearby lockers remaining usable, and release after a hidden player's disconnection or a level restart. Automated tests cover capture coherence, wire validation, ordered entry/exit, native visibility ABI and interaction lifetime using synthetic engine memory; they do not establish in-game rendering or AI behaviour.

Build 407 adds diagnostics for an Alien visibility report: `aisync_presentation.hidden` reads the backstage flag, while `invisible` independently reads render invisibility. `playerHidingHistory*` in the native runtime export retains requested hiding edges, successful native hide/show calls and their target Character addresses since DLL initialization, including after exit and level cleanup. These measurements distinguish a player-hiding write from an AISync vent or attack problem. The reported Alien vent divergence and stalled host attack remain under investigation.

## Requirements

To **build the backend**, you need:

- Windows 10/11.
- **Visual Studio 2026**, with the **Desktop development with C++** workload, MSVC, Windows SDK, and CMake tools.
- **CMake ≥ 3.25**.
- **.NET 8 SDK** and an ASP.NET Core build environment.
- Access to NuGet package restore (including `CathodeLib`, used by the launcher).

The currently provided CMake preset is named **`vs2026-win32`** and uses the `Visual Studio 18 2026` generator. Using a different Visual Studio version requires an appropriate CMake configuration; **do not replace `Win32` with `x64`**.

For **in-game testing**, you need a legally installed copy of _Alien: Isolation_ with all DLCs, ideally with two machines/instances so that host and client diagnostics can be compared.

## Building the Backend

Run the following commands **from the repository root** in PowerShell. They match the projects and presets provided in the source tree. **In-game behavior must be validated separately: a successful build does not guarantee that injection or synchronization will work.**

### 1. Native DLL (Release)

```powershell
cd AIMP.Native
cmake --preset vs2026-win32
cmake --build --preset release --parallel
cd ..
```

**Expected output:** `AIMP.Native/out/build/vs2026-win32/bin/Release/AIMP.NetZ.dll`.

For a native Debug build, run `cmake --build --preset debug --parallel` from `AIMP.Native`.

### 2. Launcher, Protocol, and Injector

```powershell
cd AIMP.Launcher
dotnet restore AIMP.Launcher.sln
dotnet build AIMP.Launcher.sln -c Release --no-restore
cd ..
```

The solution contains `AIMP.Protocol`, `AIMP.Injector32` (**x86**), and `AIMP.Launcher` (**x64**). The launcher project also builds the injector and copies it into the `Injector32/` output directory.

**Expected main output directory:** `AIMP.Launcher/src/AIMP.Launcher/bin/x64/Release/net8.0-windows/`.

For Debug, replace `Release` with `Debug`.

### 3. Assemble and Run a Local Build

To **test the backend you just built** with an existing AI-MP installation, you do not need to reinstall the mod:

1. Close AI-MP and make sure its local API process is no longer running.
2. Open the build output directory: `AIMP.Launcher/src/AIMP.Launcher/bin/x64/Release/net8.0-windows/` (or the corresponding `Debug` directory for a Debug build).
3. Copy **the contents of that directory** into `<AI-MP mod folder>\resources\api\`, **replacing the existing files** with the newly built ones.
4. Launch AI-MP again to test the updated backend.

If you also rebuilt the native module, make sure the launcher uses the **matching `AIMP.NetZ.dll`** from that same compatible revision, rather than an older DLL left in another directory.

- Use **Launcher / Native binaries built from the same compatible revision**.
- Use a compatible game installation; the launcher handles **automatic game preparation**.
- The Electron shell is included here; the web frontend it loads is maintained separately. Consult the frontend repository to change the web interface.

**Important:** Never add `AI.exe`, DLCs, game PAK files, or proprietary binaries to this repository just to “make building easier.”

### Desktop updates from GitHub Releases

The installed Windows NSIS launcher checks stable releases in [`Dropper7856/AIMP`](https://github.com/Dropper7856/AIMP/releases) at startup, before starting the local API. When a newer version is available, it downloads and verifies the installer, installs silently, and restarts. A small local window shows download progress. An offline connection, failed download, or a check exceeding 10 seconds lets the current version start normally; downloads time out after 5 minutes. Updates are deferred while `AI.exe` is running, or if its process state cannot be checked, to avoid replacing DLLs loaded by the game. Development, unpacked test builds, and the portable executable skip automatic installation. The portable version must be replaced manually.

For each release, increase `AIMP.Electron/package.json` and its lockfile to the same version, build the Windows targets, and attach **all three** files to a public, published GitHub Release tagged `v<version>` (for example `v1.0.2`):

- `AI-MP-Launcher-Setup-<version>-x64.exe`
- `AI-MP-Launcher-Setup-<version>-x64.exe.blockmap`
- `latest.yml` generated by that same build

The portable executable may be attached as an additional download. Upload the setup and blockmap before `latest.yml`, or prepare all assets in a draft and publish the completed release. Commits, drafts, and prereleases do not update the stable launcher. No GitHub token is distributed to users. Builds use `--publish never` so creating a local build does not publish a release. Existing versions without this updater require one manual installation of the first updater-enabled setup. See the [electron-builder v26 update guide](https://www.electron.build/v26/docs/features/auto-update/).

Versions 1.0.1 and 1.0.2 looked for an uninstaller named after the displayed product name instead of the configured executable name, so their installed builds skipped the update check. Version 1.0.3 derives the correct `Uninstall AIMP.Launcher.Desktop.exe` path from the running executable. Those older installations need one manual installation of the corrected setup; subsequent updates use GitHub automatically. The desktop version, executable path, and exact reason for skipping an update are recorded in `%APPDATA%\aimp-launcher-desktop\logs\aimp-electron.log`.

### Local API browser access

The API accepts connections from loopback addresses with a loopback `Host` only. Every request requires the `X-AIMP-Token` session header, except valid CORS preflights, which return no application data. Electron generates a fresh 256-bit random token at each launch, passes it to the API through `AIMP_API_TOKEN`, and adds it in the main process only to API requests from the official main frame. The token is never exposed through preload, renderer JavaScript, URLs, or configuration files, and is stripped from outgoing requests to other destinations, including redirects.

Browser requests must also use the exact HTTP/HTTPS origin supplied in `AIMP_FRONTEND_ORIGIN`; Electron sets it from its configured frontend URL. Opaque (`null`), `file:`, `app:`, and other origins receive HTTP 403 before any controller runs. Browser requests without `Origin` that carry fetch metadata or a referrer are also denied. Native clients, including Electron health/shutdown calls, require the same session token. Missing or invalid tokens receive HTTP 401; a missing or malformed startup token prevents the API from starting. Standalone development requires a random 32-byte hex `AIMP_API_TOKEN` and the intended frontend origin, with the token supplied in each client request header. Deploy the matching Electron and API versions together; the previous desktop client cannot authenticate to this API.

### Log files and retention

| Location                                         | Files                                                                                                  |
| ------------------------------------------------ | ------------------------------------------------------------------------------------------------------ |
| `%LOCALAPPDATA%\AIMP\logs`                       | One file per run: `AIMP.Launcher-*`, `AIMP.NetZ-*`, `AIMP.Crash-*`, `AIMP.WS3.*`, `AIMP.RemoteLocomotionFlight-*` |
| `%APPDATA%\aimp-launcher-desktop\logs`           | Appended across runs: `aimp-electron.log`, `aimp-launcher-api.log`                                      |

Logs are bounded automatically at launcher startup:

- **Per-run logs** (`%LOCALAPPDATA%\AIMP\logs`): every finished log is gzipped to `<name>.gz`, and only the newest 10 files of each kind are kept. Logs still open by a running game or launcher are never touched, so the current session's logs stay as plain text for **Export Diagnostics**.
- **Electron logs**: once a log reaches 1 MiB it is renamed to `<name>.<UTC time>.log`, a fresh file is started, and the rolled copy is gzipped. The newest 10 archives of each log are kept.

Compression is lossless: the original is deleted only after its `.gz` decompresses back to identical bytes (SHA-256 and length for per-run logs, byte comparison for Electron logs); otherwise the original is kept. `.gz` files open in 7-Zip, WinRAR, or `tar -xzf`.

## Developer Mode

Developer Mode exposes experimental tools for testing **other maps** and investigating **multiplayer support for the main campaign**, beyond the currently supported Survivor Mode.

### Enable Developer Mode

1. Start the **AI-MP launcher** with the `-dev` launch argument (for example, add it to the launcher's shortcut).
2. **Launch the game** from AI-MP.
3. Once in the game, open **Settings → Multiplayer** and enable the **Developer Mode** option.
4. **Exit the game**. A new **Debug** menu will then appear in the AI-MP launcher.

### Debug Menu

The launcher’s **Debug** menu includes:

- **Live Logs:** View diagnostic logs in real time.
- **Performance:** Enable or disable individual modules in real time to isolate performance issues and test their impact.
- **WorldManifest:** Select specific levels to generate their manifests and import the assets needed for multiplayer testing.

**Warning:** Developer Mode is intended for testing and debugging. Additional maps and the main campaign are **not officially supported for stable multiplayer gameplay**. Expect broken objectives, unsynchronized interactions, crashes, and other issues.

## Contributing

**Contributions are welcome**, whether they involve bug reports, diagnostics, engine research, performance improvements, documentation, or new features.

### Pull Requests

1. Create a dedicated branch for one issue. Avoid mixing bug fixes, refactoring, and new features in the same pull request.
2. Explain **the problem**, **the identified cause**, **the solution**, and **the affected modules**.
3. List the testing steps (host/client, participant count, map, scenario) and **the checks you actually performed**. Explicitly state what was not tested.
4. Attach relevant diagnostics **after removing sensitive information**, and summarize the important findings. Do not commit large diagnostic ZIP archives to the repository.
5. If the protocol, hooks, or build artifacts change, document **compatibility** and keep the relevant versions in sync.
6. Make sure no proprietary game files, secrets, personal paths, or build artifacts have been committed.

Authors and reviewers may request changes in scope to preserve module boundaries and make validation easier.

### Versions and Compatibility

The launcher explicitly checks the expected native version. When introducing a change that belongs to a new compatible mod version, review **all of the following together**:

- `AIMP.Launcher/src/AIMP.Launcher/ProductInfo.cs` (`LauncherVersion`, `ExpectedDllVersion`).
- `AIMP.Native/include/aimp/ProductVersion.h` (`kNativeVersion`).
- Network/IPC message formats and versions, if affected by the change.
- Content manifest and compatibility checks, where applicable.

**Never disable version or hash checks to hide an incompatibility.** A clear rejection is better than a corrupted session.

### Reporting a Bug

On Discord, create one post per issue in **#feedbacks** and **always provide the diagnostic ZIP archive** exported from **Settings** in the launcher. Do not close the launcher after the incident before exporting. For synchronization issues, **both host and client ZIP archives** are especially useful. Include reproduction steps, your host/client role, and the number of participants.

**You do not need to know how to code to help the project.** Reliable reproductions and good diagnostics save a great deal of time.

## Safety, Compatibility, and Game Content

- The project relies on reverse engineering and native interactions: **only run experimental builds on a game installation you can restore**.
- Offsets, signatures, ABIs, and version profiles are not universal. Unknown game builds must be rejected safely.
- `AI.exe` and `AIMP.NetZ.dll` are **32-bit**; the launcher is **64-bit**. This mix is intentional.
- Do not distribute the game's executable, DLCs, proprietary assets, or complete decompilations in the repository or pull requests.
- Never publicly share tokens, credentials, private addresses, or personal data extracted from logs.
- Before opening the repository for public contributions, make sure it has an **explicit license** and clear terms covering the code and contributions.

---

**AI-MP is not simply trying to place two players in the same room: it aims to build a genuine multiplayer foundation on top of a single-player engine without sacrificing its consistency.**

Thank you to everyone who tests, documents, reports bugs, or contributes to development. **See you in Sevastopol — together.** 👽
