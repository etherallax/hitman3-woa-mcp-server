# PROJECT

## Components

### MCPBridge (ZHMModSDK plugin)

`plugin/` — C++ mod, builds to `MCPBridge.dll`. This is the source of truth;
`ZHMModSDK/Mods/MCPBridge` is a **directory junction** to `plugin/` so the
SDK build picks it up (verify junctions work if the tree is re-cloned).

- `HttpServer.{h,cpp}` — blocking loopback HTTP/1.1 server, one request per
  connection, serial accepts. Winsock2, no external deps.
- `MCPBridge.{h,cpp}` — plugin interface impl:
  - `Init()` starts the HTTP thread (port 47847).
  - `OnEngineInitialized()` registers a frame update → `OnFrameUpdate`.
  - `OnFrameUpdate` refreshes `m_StateJson` and drains the command queue —
    all engine access happens on the game thread.
  - `DispatchOnGameThread` (HTTP thread) enqueues `{id, body}` and waits on
    `m_ResultCv` up to 10s.
  - Detours: `ZEntitySceneContext_ClearScene` (reset cheat entities),
    `ZHM5ItemWeapon_SetBulletsInMagazine` (no-reload, lifted from Player mod).
  - ImGui overlay draws pending `say` messages.

### HTTP API (localhost:47847)

- `GET /state` → `{in_game, pos:[x,y,z], cheats:{...}, timescale, frame}`
- `POST /cmd` with JSON body:
  `{"cmd":"ping"|"teleport"|"forward"|"spawn"|"items"|"cheat"|"timescale"|"say", ...}`
- `GET /items?filter=x` convenience alias.

### bridge/ (Node MCP stdio server)

`bridge/index.js` — `@modelcontextprotocol/sdk` server exposing `hitman_*`
tools; each maps to a `/cmd` POST or `/state` GET. Diagnostics/errors are
returned as tool text; nothing logs to stdout except protocol frames.

## Build environment

- MSVC cl via VS 18 Community (`vcvarsall x64`), cmake 4.2.1 + ninja
- Rust **nightly** (quickentity-rs uses `#![feature]` gates) at
  `S:\hitman-mcp\.rustup`
- vcpkg baseline pinned (mid-2025) in `ZHMModSDK/vcpkg.json` — head-of-master
  ports break the SDK (semver/sentry API drift). Do not unpin.
- Everything lives on `S:` because `C:` is full.

## Deployment

- DLL → `<game>/Retail/mods/MCPBridge.dll`, section `[mcpbridge]` in
  `Retail/mods.ini`
- Bridge → `mcpServers.hitman` in `%APPDATA%\devin\mcp_config.json`
