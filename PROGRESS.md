# PROGRESS

## Done

- [x] ZHMModSDK source checkout at `S:\hitman-mcp\ZHMModSDK`
- [x] Build env: VS18 dev env + ninja, Rust nightly on S:, vcpkg baseline pin
      (fixes semver/sentry/protobuf API drift)
- [x] `MCPBridge` plugin: HTTP server + game-thread command queue + cheats +
      spawn + teleport + timescale + overlay `say`
- [x] `MCPBridge.dll` compiles & links (x64-Release)
- [x] Installed to `Retail/mods/`, enabled via `[mcpbridge]` in `mods.ini`
- [x] Node MCP bridge (`bridge/index.js`) — verified: `initialize` +
      `tools/list` round-trip over stdio, 8 tools advertised
- [x] Registered `hitman` server in `%APPDATA%\devin\mcp_config.json`

## Runtime verification — PASSED (Paris, "The Showstopper")

- [x] Mod loaded on game launch; `127.0.0.1:47847` LISTENING under HITMAN3.exe
- [x] `GET /state` returns live JSON (pos/cheats/timescale/frame)
- [x] `ping` round-trips through the game-thread queue
- [x] `say` renders overlay text in-game
- [x] `forward` 4m along facing — clean
- [x] `teleport` — works; ⚠️ relative +Y clipped player through floor
      (ground snap isn't resolved by SetObjectToWorldMatrixFromEditor).
      Prefer absolute teleports to known ground height.
- [x] `timescale` 0.3 ↔ 1.0 verified
- [x] `items` repository scan works — 1,470 items indexed
- [x] `spawn`→inventory verified (Remote Explosive RubberDuck delivered)
- [x] `spawn`→world verified (duck #2 at player feet)
- [x] `cheat` noreload + infiniteammo verified (cripple box + detour work)
- [ ] New Devin session picks up `mcp__hitman__*` tools (config registered;
      needs a fresh session to confirm)

## v1.0.0 polish (post-verification)

- [x] Ground-snap teleport via `ZCollisionManager::RayCastClosestHit`
      (verified: +25m pop landed on hedge top, stable)
- [x] `/state` reports `stale_ms` + `mod_version`
- [x] Port configurable via `mods/mcpbridge.ini` `[server] port=`
- [x] Unload safety: unregisters frame update, self-connect wakes accept(),
      `m_Unloading` bails in-flight dispatches
- [x] **SDK-level finding:** `reload`/`unload`/`unloadall` deadlock on game
      3.270.1 even for stock mods (`reload world` froze) — NOT our bug.
      Documented: restart to update.

## Release — DONE

- [x] Repo pushed: https://github.com/etherallax/hitman3-woa-mcp-server
- [x] GitHub release `v1.0.0` created with both assets:
      manual zip + SMF framework zip (zip layout fixed after SMF rejected
      flat manifest at root — needs `Etherallax.MCPBridge/` folder inside)
- [ ] npm publish of `hitman-mcp-bridge` (user choice; `npx` works from repo)
- [ ] Nexus upload — user opted to skip; `dist/README-NEXUS.txt` kept for later

## Known behaviors

- Commands dispatched while the game is PAUSED (or `~` console open) queue
  until unpause — `eUpdatePlayMode` doesn't tick. HTTP side will time out at
  10s but the command still executes later.
- First `items` call does a full THashMap walk — heavy but one-off.
- If game updates to 3.280+: rebuild SDK/plugin against matching version and
  bump SMF accordingly (see game MODDING-NOTES.md).
