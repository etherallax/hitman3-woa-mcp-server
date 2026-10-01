# PRD — hitman-mcp

## Problem

An AI agent in a terminal cannot see or affect a running HITMAN 3 process.
The user wants the agent to be an in-game buddy: teleport them, hand them
items, flip cheats, control game speed, and post messages.

## Requirements

1. A ZHMModSDK plugin DLL loaded by HITMAN3.exe hosting a localhost server.
2. Commands must execute on the game thread (engine is not thread-safe).
3. An MCP stdio bridge translating tool calls to the plugin's HTTP API.
4. Initial command set: state read, teleport, spawn item, cheats, timescale.
5. Localhost-only; no network exposure.
6. Must not destabilize the game — failed commands return errors, never crash.

## Non-goals

- No online/multiplayer interaction.
- No generic remote shell into the game.
- No screenshots/video capture in v1 (agent plays "blind" via state reads).

## Success criteria

- `GET /state` returns live position while in a mission.
- POSTed commands produce observable in-game effects.
- MCP tools callable from a Devin session with the game running.
