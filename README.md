# hitman-mcp

Let an AI agent *actually play* HITMAN 3 with you.

`hitman-mcp` is two things:

1. **MCPBridge** — a [ZHM Mod SDK](https://github.com/OrfeasZ/ZHMModSDK) plugin
   that runs a tiny HTTP server *inside the game* (localhost only), exposing
   player state, teleports, item spawning, cheats, and time control.
2. **hitman-mcp-bridge** — a Node.js MCP stdio server that turns those into
   MCP tools any MCP-capable agent (Devin CLI, Claude Code/Desktop, etc.) can
   call.

```
MCP client (your AI assistant)
    │ stdio
    ▼
hitman-mcp-bridge (node)
    │ HTTP 127.0.0.1:47847
    ▼
MCPBridge.dll inside HITMAN3.exe  →  game-thread command dispatch
```

Single-player, offline, your machine only. The server binds to `127.0.0.1`
and there is no authentication — do not expose the port.

## Features

- Live player position & game state (`in_game`, cheats, timescale, frame age)
- Absolute/relative teleport with **automatic ground snap** (raycast — no more
  falling through the floor; `snap:false` to override)
- Spawn any of ~1,500 repository items into 47's inventory or at his feet
- Search the item repository by name
- Cheats: invincible, invisible, infinite ammo, no reload
- Game speed multiplier
- `say` — overlay text messages rendered in-game

## Requirements

- HITMAN 3 / World of Assassination on PC (offline/single-player use)
- [ZHM Mod SDK](https://github.com/OrfeasZ/ZHMModSDK) installed into the game
  (`Retail/dinput8.dll` + `Retail/mods/`), version matching your game build
- Node.js ≥ 18 (for the MCP bridge)

> ⚠️ Game-version sensitivity is inherited from the SDK: the plugin must be
> built against an SDK release compatible with your game exe. Built and
> tested on game `3.270.1` + SDK `v4.0.2`.

## Install the mod

Prebuilt packages live on the
[Releases](https://github.com/etherallax/hitman3-woa-mcp-server/releases)
page.

**Option A — Simple Mod Framework (recommended for SMF users):**
grab `hitman-mcp-<ver>.framework.zip` and drop it into SMF's Mods folder like
any other framework mod, then Apply/Deploy. The deploy script copies the DLL
into `Retail/mods/` and enables it in `Retail/mods.ini` automatically. Mod
Manager will show its standard "mod contains scripts" warning — the script is
plain text in the zip (`deploy.ts`, ~50 lines) and does only the file copy +
ini edit.

**Option B — manual:**
1. Copy `MCPBridge.dll` into `<game>/Retail/mods/`.
2. Edit `<game>/Retail/mods.ini` and add a section:
   ```ini
   [mcpbridge]
   ```
3. Launch the game. In the SDK console (`~`) you should see:
   `MCPBridge: listening on 127.0.0.1:47847`
   A `MCPBRIDGE` tab also appears in the SDK menu bar showing the port.

> ⚠️ Uninstall note: removing the SMF package does not remove the DLL —
> delete `Retail/mods/MCPBridge.dll` and the `[mcpbridge]` section in
> `mods.ini` to fully uninstall (either install method).

Optional: create `<game>/Retail/mods/mcpbridge.ini` to change the port:

```ini
[server]
port = 47847
```

> ⚠️ Do **not** use `reload`/`unload` on this (or other) mods from the SDK
> console — the unload path deadlocks on some game builds. Restart the game
> to update the DLL.

## Try it without an agent

```bash
curl http://127.0.0.1:47847/state
curl -X POST http://127.0.0.1:47847/cmd -H "Content-Type: application/json" \
     -d '{"cmd":"say","text":"hello"}'
```

## Connect an agent (MCP)

Run the bridge: `npx hitman-mcp-bridge` (or `node bridge/index.js` from this
repo).

### Devin CLI

`%APPDATA%\devin\mcp_config.json`:

```json
{
  "mcpServers": {
    "hitman": { "command": "npx", "args": ["-y", "hitman-mcp-bridge"] }
  }
}
```

### Claude Desktop / other stdio MCP clients

Same shape — server entry `hitman` with `command: "npx"`, `args: ["-y",
"hitman-mcp-bridge"]`. Tools appear as `hitman_*`.

### Tools

| Tool | What it does |
|---|---|
| `hitman_state` | in_game, position, cheat flags, timescale, snapshot age |
| `hitman_teleport` | x/y/z absolute, or relative + ground snap |
| `hitman_move_forward` | nudge along facing direction |
| `hitman_spawn` | repository item → inventory or world |
| `hitman_list_items` | search item names |
| `hitman_cheat` | invincible / invisible / infiniteammo / noreload |
| `hitman_timescale` | game speed multiplier |
| `hitman_say` | in-game overlay message |

## HTTP API

- `GET /state` → `{in_game, pos, cheats, timescale, frame, stale_ms, mod_version}`
- `POST /cmd` → `{"cmd": "...", ...}`:
  `ping`, `teleport` (`x,y,z,relative,snap`), `forward` (`distance`),
  `spawn` (`id`, `where:"inventory"|"world"`), `items` (`filter`, `limit`),
  `cheat` (`name`, `on`), `timescale` (`value`), `say` (`text`, `duration`)
- `GET /items?filter=x` — alias for the items command

`stale_ms` grows when the game thread isn't ticking (pause menu, console
open, focus loss) — commands sent then will queue until unpaused.

## Building from source

1. Clone [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) (with submodules)
   at the version matching your game.
2. Copy `plugin/` into `ZHMModSDK/Mods/MCPBridge/`.
3. Add `MCPBridge` to the `MODS` list in the SDK root `CMakeLists.txt`.
4. Follow the SDK's normal build (CMake preset, MSVC + Ninja + Rust nightly +
   vcpkg — see their README).
5. Output: `_build/x64-Release/Mods/MCPBridge/MCPBridge.dll`.

The bridge needs `npm ci` in `bridge/` for local dev; published builds run
via `npx`.

## Caveats

- Commands are dispatched on the game thread once per frame — heavy commands
  (first `items` scan) can hitch a frame.
- Pausing/menu pauses the queue; requests time out at 10s but still execute
  on unpause.
- Teleporting into unloaded/out-of-bounds areas is your problem now (that's
  the fun part). `snap` keeps you on real collision.
- Offline single-player only — don't be weird with it.

## License

- `plugin/` (MCPBridge mod): **GPLv3** — it links against GPLv3 ZHMModSDK.
- `bridge/` (MCP bridge): **MIT**.

Not affiliated with IO Interactive. HITMAN is a trademark of IO Interactive A/S.
