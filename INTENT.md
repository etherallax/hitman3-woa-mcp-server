# INTENT

## Why this exists

The user plays HITMAN 3 WoA offline, single-player, on their own PC, for fun.
They wanted console/debug/noclip/spawn capabilities ("it's offline and only for
my PC so I can have fun") — delivered via SMF + ZHMModSDK. Then they asked:

> "what if we make a mcp server mod? ;)" — "Yes. We can be buddies even more then"

So the goal is to let the agent (Devin) actually interact with the running game:
read state, teleport the player, spawn items, flip cheats, set timescale, and
post overlay messages — a co-op partner living in the terminal.

## Scope boundaries

- Offline, local, single-player only. Server binds to 127.0.0.1.
- No interaction with online services, leaderboards, or other players.
- No anti-cheat concerns apply (emulated Steam env, offline RIP install).
- Don't break the working SMF/ZHM mod stack — additive changes only.
