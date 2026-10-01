#!/usr/bin/env node
// MCP stdio server bridging to the MCPBridge ZHM mod inside HITMAN3.exe.
// The mod exposes a tiny HTTP API on 127.0.0.1:47847; this server maps MCP tools to it.

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const BASE = "http://127.0.0.1:47847";

async function cmd(payload) {
  try {
    const res = await fetch(`${BASE}/cmd`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
      signal: AbortSignal.timeout(15000),
    });
    return await res.text();
  } catch (e) {
    return JSON.stringify({ ok: false, error: `game unreachable (is HITMAN3 running with MCPBridge loaded?): ${e.message}` });
  }
}

async function getState() {
  try {
    const res = await fetch(`${BASE}/state`, { signal: AbortSignal.timeout(5000) });
    return await res.text();
  } catch (e) {
    return JSON.stringify({ ok: false, error: `game unreachable (is HITMAN3 running with MCPBridge loaded?): ${e.message}` });
  }
}

const ok = (text) => ({ content: [{ type: "text", text }] });

const server = new McpServer({ name: "hitman", version: "1.0.0" });

server.tool(
  "hitman_state",
  "Get live game state: in_game flag, player position, active cheats, timescale",
  {},
  async () => ok(await getState())
);

server.tool(
  "hitman_teleport",
  "Teleport Agent 47. Absolute coordinates (x,y,z) or relative offsets (dx,dy,dz with relative=true). Y is up.",
  {
    x: z.number().describe("Target X (or delta if relative)"),
    y: z.number().describe("Target Y / up (or delta if relative)"),
    z: z.number().describe("Target Z (or delta if relative)"),
    relative: z.boolean().optional().describe("Treat x,y,z as offsets from current position"),
  },
  async ({ x, y, z, relative }) => ok(await cmd({ cmd: "teleport", x, y, z, relative: !!relative }))
);

server.tool(
  "hitman_move_forward",
  "Move 47 forward by N meters along his facing direction",
  { distance: z.number().describe("Meters to move forward (negative = backward)") },
  async ({ distance }) => ok(await cmd({ cmd: "forward", distance }))
);

server.tool(
  "hitman_spawn",
  "Spawn a repository item by ID, into 47's inventory or into the world at his feet. Use hitman_list_items to find IDs.",
  {
    id: z.string().describe("Repository ID, e.g. from hitman_list_items"),
    to_world: z.boolean().optional().describe("Drop in world instead of inventory"),
  },
  async ({ id, to_world }) => ok(await cmd({ cmd: "spawn", id, where: to_world ? "world" : "inventory" }))
);

server.tool(
  "hitman_list_items",
  "Search spawnable repository items (weapons, props, tools) by name substring",
  {
    filter: z.string().optional().describe("Case-insensitive name filter"),
    limit: z.number().optional().describe("Max results, default 50"),
  },
  async ({ filter, limit }) => ok(await cmd({ cmd: "items", filter: filter ?? "", limit: limit ?? 50 }))
);

server.tool(
  "hitman_cheat",
  "Toggle a cheat: invincible, invisible, infiniteammo, noreload",
  {
    name: z.enum(["invincible", "invisible", "infiniteammo", "noreload"]),
    on: z.boolean(),
  },
  async ({ name, on }) => ok(await cmd({ cmd: "cheat", name, on }))
);

server.tool(
  "hitman_timescale",
  "Set game speed multiplier (1.0 = normal, 0.5 = half speed, 2 = double)",
  { value: z.number() },
  async ({ value }) => ok(await cmd({ cmd: "timescale", value }))
);

server.tool(
  "hitman_say",
  "Show a text message on the in-game overlay (devin: <text>), visible to the player",
  {
    text: z.string(),
    duration: z.number().optional().describe("Seconds to display, default 5"),
  },
  async ({ text, duration }) => ok(await cmd({ cmd: "say", text, duration: duration ?? 5 }))
);

await server.connect(new StdioServerTransport());
