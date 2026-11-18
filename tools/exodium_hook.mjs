#!/usr/bin/env node
// Claude Code runs this as a hook (see "Letting Claude Code drive it" in the README), to tell
// the browser tools' wrapper, exodium_mcp.mjs, what it can't otherwise know:
//
// - PreToolUse, for the browser tools: which agent the call is from. The main conversation
//   and every subagent it starts call through the same wrapper, and the call itself doesn't
//   say. A file named after the call's tool_use_id says, for the wrapper to read as the call
//   comes in.
// - SubagentStop and Stop: that a subagent's task, or the main conversation's turn, has
//   ended, so that its windows can be closed. Only for an agent that has windows open, which
//   the wrapper marks with a file of its own, so that a session that never used the browser
//   leaves nothing behind.
//
// It goes in Winamp's Plugins folder, beside exodium_mcp.mjs. It never stops a call or a stop:
// whatever goes wrong, it says nothing and exits 0.
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, unlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const folder = join(tmpdir(), "exodium-mcp", "agents"); // as in exodium_mcp.mjs
const callFileMs = 10 * 60 * 1000; // one the wrapper never took, as an older one won't, goes after this
const safe = id => typeof id === "string" && /^[\w-]{1,100}$/.test(id);

try {
  const hook = JSON.parse(readFileSync(0, "utf8").replace(/^﻿/, ""));
  if (hook.hook_event_name === "PreToolUse" && safe(hook.tool_use_id)) {
    mkdirSync(folder, { recursive: true });
    const said = { agent: safe(hook.agent_id) ? hook.agent_id : "", session: safe(hook.session_id) ? hook.session_id : "" };
    writeFileSync(join(folder, `call-${hook.tool_use_id}.json`), JSON.stringify(said));
    const now = Date.now();
    for (const file of readdirSync(folder)) {
      if (!file.startsWith("call-")) continue;
      try {
        if (now - statSync(join(folder, file)).mtimeMs > callFileMs) unlinkSync(join(folder, file));
      } catch {}
    }
  } else if (hook.hook_event_name === "SubagentStop" || hook.hook_event_name === "Stop") {
    const id = hook.hook_event_name === "Stop" ? hook.session_id : hook.agent_id;
    if (safe(id) && existsSync(join(folder, `open-${id}`))) writeFileSync(join(folder, `done-${id}`), hook.hook_event_name);
  }
} catch {}
