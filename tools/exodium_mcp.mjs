#!/usr/bin/env node
// What Claude Code runs for its browser tools. The tools themselves are Playwright's MCP
// server, connected to Exodium's debugging port. This sits between the two, passing
// everything along, and sees to the things Playwright can't know about Exodium:
//
// - Every agent gets windows of its own: the main conversation, and each subagent it
//   starts. They all call through this one process, so each has a Playwright server of its
//   own, which keeps its current page, its tabs, its console and its recording apart from
//   the others'. Two agents working at once would otherwise drive the same page. A hook,
//   exodium_hook.mjs, says which agent each call is from; without it, all are one.
// - An agent's windows close once it has finished with them: when a subagent's task ends, a
//   little while after the conversation's turn has ended, after ten minutes unused, or when
//   it calls browser_close. Its next call opens a new one, back at the address it was on.
// - An agent works in a window rather than a tab that's in use. Playwright left to itself
//   takes whichever page comes first. Exodium shows the window without the keyboard, behind
//   every other window, and closes it when the session ends.
// - Winamp is started if it isn't running, as it is for a link.
// - A new page is an Exodium window. Playwright's own way of making one gets a bare window
//   of the browser process's, outside Exodium. A window rather than a tab, because
//   Exodium hides a tab that isn't in front, and a hidden page can't be screenshotted.
// - A page a site opens itself is a real tab, but Playwright only sees those when it
//   connects. So it is connected again when there are pages it doesn't know about.
// - Playwright catches the file picker of every page it knows, which is every page there
//   is, so upload buttons in the user's own tabs did nothing while Claude was connected.
//   Only the page an agent is working in has its picker caught.
// - Playwright lists every tab there is in its answers. An agent is shown only its own:
//   the user's cost tokens, put their browsing in every transcript, and aren't Claude's to
//   work in, and another agent's aren't this one's.
// - Playwright records video of every page there is. Only the agent's own is recorded.
// - Playwright only reads and writes files in its output folder and the project. Claude
//   keeps its own in its scratchpad, under %TEMP%\claude, so files headed there are
//   passed through the output folder.
// - A console that repeats one message thousands of times is shown with each message once.
// - The user's password manager, Bitwarden, is theirs alone. Exodium gives Claude's windows a
//   browser profile of their own, which it isn't in, but a tab of the user's that Claude is
//   pointed at has it: its menus are frames of its own in the page, and its windows are the
//   extension's pages. Claude is kept out of both: it can't go to or into an extension's
//   page, and in a tab of the user's it can't click, type into or aim at an extension's frame.
//
// It also adds tools of its own, which Playwright has no equal for: browser_zoom, for a
// close look at part of a page; browser_batch, for several tools in one call;
// browser_emulate_device, for a phone or tablet; and browser_filmstrip, for frames of an
// animation.
//
// It goes in Winamp's Plugins folder, beside exodium_open.exe, exodium_start.html and
// exodium_hook.mjs.
import { execFile, spawn, spawnSync } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdirSync, readdirSync, readFileSync, statSync, unlinkSync, watch, writeFileSync } from "node:fs";
import { copyFile, mkdir, rename, rmdir, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, dirname, isAbsolute, join, relative, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const port = process.env.EXODIUM_PORT ?? "9333"; // remote_debugging_port in winamp.ini
const winampClass = process.env.EXODIUM_WINAMP_CLASS; // for a scratch Winamp started with /CLASS=
const playwright = "@playwright/mcp@0.0.83"; // pinned: what is parsed below is its output
const capabilities = "vision,pdf,devtools"; // the mouse by coordinates, PDF, video and tracing
const endpoint = `http://127.0.0.1:${port}`;
const startPage = pathToFileURL(join(here, "exodium_start.html")).href;
const pageWaitMs = 30000; // long enough for Winamp to start
const pollMs = 200;
const portHelp =
  `Exodium's debugging port (${port}) isn't answering. It needs remote_debugging_port=${port} ` +
  "under [Exodium] in winamp.ini, and Winamp restarted after that was set.";

const outputDir = join(tmpdir(), "exodium-mcp"); // shared by every session's Playwright
const outputBudget = 5 * 1024 * 1024 * 1024; // Playwright removes its oldest files past this
const claudeTemp = join(tmpdir(), "claude"); // Claude Code's scratchpads and task output
const readsFile = new Set(["browser_run_code_unsafe", "browser_set_storage_state"]); // the rest write theirs

// What the hook and this process tell each other, as files: call-<tool use>.json, from the
// hook, says which agent a call is from; open-<agent>, from here, that an agent has windows;
// and done-<agent>, from the hook, that it has finished. An agent is known there by its
// subagent id, or for the main conversation by Claude Code's session id.
const agentsDir = join(outputDir, "agents");
const msFromEnv = (name, otherwise) => (Number(process.env[name]) > 0 ? Number(process.env[name]) : otherwise);
const idleMs = msFromEnv("EXODIUM_IDLE_MS", 10 * 60 * 1000); // an agent's windows close after this long unused
const turnEndMs = msFromEnv("EXODIUM_TURN_END_MS", 2 * 60 * 1000); // the conversation's, this long after its turn ends
const minutes = ms => (ms % 60000 ? `${Math.round(ms / 1000)} seconds` : `${ms / 60000} minute${ms === 60000 ? "" : "s"}`);

const zoomLongEdge = 1400; // pixels along the long edge of a zoomed picture, at scale 1
const zoomMostTimes = 8; // no use blowing a few pixels up any further than this
const zoomWaitMs = 8000; // a page out of sight never draws, so it would never come

const consoleBudget = 20000; // characters of console messages shown at once
const consoleEntryMost = 3000; // and of any one of them, stack included

// Phones and the like, as Chrome's device toolbar has them. A page is shown their size, at
// their pixel density, with touch, and told it is on that device.
const iosAgent = device =>
  `Mozilla/5.0 (${device}; CPU ${device === "iPhone" ? "iPhone " : ""}OS 17_5 like Mac OS X) ` +
  "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1";
const androidAgent = model =>
  `Mozilla/5.0 (Linux; Android 14; ${model}) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Mobile Safari/537.36`;
const devices = {
  "iPhone 15": { width: 393, height: 852, scale: 3, mobile: true, touch: true, agent: iosAgent("iPhone"), platform: "iPhone", os: "iOS" },
  "iPhone SE": { width: 375, height: 667, scale: 2, mobile: true, touch: true, agent: iosAgent("iPhone"), platform: "iPhone", os: "iOS" },
  "Pixel 8": { width: 412, height: 915, scale: 2.625, mobile: true, touch: true, agent: androidAgent("Pixel 8"), platform: "Linux armv81", os: "Android", model: "Pixel 8" },
  "budget Android": { width: 360, height: 800, scale: 2, mobile: true, touch: true, agent: androidAgent("SM-A055F"), platform: "Linux armv81", os: "Android", model: "SM-A055F" },
  iPad: { width: 820, height: 1180, scale: 2, mobile: true, touch: true, agent: iosAgent("iPad"), platform: "iPad", os: "iOS" },
  laptop: { width: 1366, height: 768, scale: 1, mobile: false, touch: false },
};

const ownTools = [
  {
    name: "browser_zoom",
    description:
      "Take a magnified screenshot of one region of the page, for a closer look at small details such as " +
      "icons, text, or part of a canvas or 3D scene. The region is in the same pixels as the page's " +
      "screenshots and browser_mouse_click_xy. Page content is drawn afresh at the larger size; a canvas " +
      "or WebGL scene is enlarged from the pixels it has.",
    inputSchema: {
      type: "object",
      properties: {
        region: {
          type: "array",
          items: { type: "number" },
          minItems: 4,
          maxItems: 4,
          description: "(x0, y0, x1, y1): the rectangle to capture, in pixels from the top left of the page's viewport.",
        },
        scale: {
          type: "number",
          minimum: 0.1,
          maximum: 1,
          description: "Scale factor for the returned image. 1 (default) is the full size; 0.5 costs a quarter of the tokens.",
        },
      },
      required: ["region"],
    },
  },
  {
    name: "browser_batch",
    description:
      "Run a sequence of browser tools in ONE round trip. Each item is {name, input}, where input is exactly " +
      "what you would pass to that tool alone. They run in order and stop at the first error. Use this " +
      "whenever you can predict two or more steps ahead, such as navigate, click, type, screenshot. Results " +
      "come back in order, images included. It can't be nested.",
    inputSchema: {
      type: "object",
      properties: {
        actions: {
          type: "array",
          minItems: 1,
          description: "The tool calls to run, in order.",
          items: {
            type: "object",
            properties: {
              name: { type: "string", description: "The tool's name, such as browser_click." },
              input: { type: "object", description: "That tool's input." },
            },
            required: ["name"],
          },
        },
      },
      required: ["actions"],
    },
  },
  {
    name: "browser_emulate_device",
    description:
      "Make the page behave as on a phone, tablet or small laptop: its screen size and pixel density, touch " +
      "(browser_mouse_click_xy taps, (pointer: coarse) and (hover: none) match), the device's user agent, and a " +
      "CPU slowdown. A page without a viewport meta tag is laid out 980 pixels wide, as a phone's browser does. " +
      "Reload the page afterwards, so the site starts over as on that device. browser_resize then " +
      "changes the device's screen size, and screenshots and coordinates stay in its CSS pixels. Use " +
      'device "off" to go back to the normal window. The GPU stays this PC\'s (an RTX 3090), so WebGL frame ' +
      "rates are far above a real phone's: judge GPU cost by what is drawn (canvas size, draw calls), not by fps.",
    inputSchema: {
      type: "object",
      properties: {
        device: {
          type: "string",
          enum: [...Object.keys(devices), "off"],
          description: "The device. Its size, density, touch and agent can be changed with the options below.",
        },
        landscape: { type: "boolean", description: "Turn it sideways." },
        cpu: {
          type: "number",
          minimum: 1,
          maximum: 20,
          description: "CPU slowdown: 1 (default) is this PC, about 4 a mid-range phone, 6 to 8 a budget phone.",
        },
        width: { type: "number", description: "Screen width in CSS pixels, in place of the device's." },
        height: { type: "number", description: "Screen height in CSS pixels, in place of the device's." },
        scale: { type: "number", description: "Device pixels per CSS pixel (devicePixelRatio), in place of the device's." },
        mobile: { type: "boolean", description: "Lay the page out as a mobile browser does (the viewport meta tag counts)." },
        touch: { type: "boolean", description: "Touch in place of a mouse." },
      },
      required: ["device"],
    },
  },
  {
    name: "browser_filmstrip",
    description:
      "Watch the page for a while and get back frames of what it drew, evenly spaced in time: for animations, " +
      "transitions, loading sequences and anything else a single screenshot can't show. Give an action, such " +
      "as {name: \"browser_mouse_click_xy\", input: {x: 400, y: 300}} or {name: \"browser_navigate\", input: " +
      "{url: ...}}, to film what it sets off: filming starts half a second before it. Frames are scaled down " +
      "and their times are measured from the action. The page must be in view, as for screenshots.",
    inputSchema: {
      type: "object",
      properties: {
        duration: { type: "number", minimum: 100, maximum: 20000, description: "How long to watch after the action, in milliseconds. Default 2000." },
        frames: { type: "number", minimum: 2, maximum: 24, description: "How many frames to give back, spread over that time. Default 8." },
        action: {
          type: "object",
          description: "A tool call to run once filming has started, as in browser_batch: {name, input}.",
          properties: {
            name: { type: "string", description: "The tool's name, such as browser_click." },
            input: { type: "object", description: "That tool's input." },
          },
          required: ["name"],
        },
        size: { type: "number", minimum: 160, maximum: 1280, description: "Longest edge of each frame in pixels. Default 640." },
      },
    },
  },
];
// browser_zoom uses the last of these itself, but it is otherwise for the user to allow, and
// a batch would otherwise get it past them.
const notInBatch = new Set(["browser_batch", "browser_run_code_unsafe"]);
const notFilmed = new Set([...notInBatch, "browser_filmstrip"]);

// What Playwright's own tools are told besides.
const toolNotes = {
  browser_tabs:
    "Only your own tabs are listed, here and in what other tools show: the user's tabs, and those of any " +
    "other agent, are left out. Stay in your own window. Pass all: true to list them too, when the user " +
    "points you at one of them.",
  browser_close: "It closes your own windows in Exodium and nothing else. Call it once you have finished with the browser.",
  browser_start_video: "Only your own page is recorded.",
};

// What Claude is told of the tools as a whole.
const instructions =
  "Exodium is the user's own everyday browser. Every agent has windows of its own in it, the main " +
  "conversation and each subagent apart, and is shown only its own tabs, so agents working at the same " +
  "time don't get in each other's way. Your windows have a browser profile of their own, apart from the " +
  "user's: they aren't signed in to the user's sites and have none of their site data or extensions. " +
  "The user's password manager (Bitwarden) is not yours to use: its pages, and its menus in the user's " +
  "own tabs, are refused. Call browser_close when you have finished with the browser, and " +
  "close tabs you opened once you are done with them. Windows still open close by themselves: when a " +
  `subagent's task ends, ${minutes(turnEndMs)} after the conversation's turn ends, and after ` +
  `${minutes(idleMs)} unused. The next browser call then opens a new one, back at the address it was on.`;

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const clamp = (value, low, high, otherwise) => (Number.isFinite(Number(value)) ? Math.min(high, Math.max(low, Number(value))) : otherwise);

// ---- agents ----

// The main conversation, and each subagent it starts: whatever is kept for one of them.
class Agent {
  constructor(id) {
    this.id = id; // the subagent's, or "" for the main conversation
    this.session = ""; // Claude Code's session, which the conversation's turns end in
    this.server = null; // its Playwright server
    this.attached = false; // Playwright has connected, and seen the agent's page
    this.restarted = false; // and the device of the agent's page not yet given back to it
    this.ownPages = new Set(); // the pages of the windows opened for it
    this.current = null; // the page Playwright has it working in, when that is known
    this.pickersLeftToSort = false;
    this.tabIds = { list: "", ids: [] }; // for the last tab list that needed them
    this.video = null; // { file, ended } from start to stop
    this.emulated = new Map(); // the settings of each page given a device, by target
    this.emulating = false; // since a device was first set, browser_resize has to ask
    this.queue = Promise.resolve(); // its calls, one at a time
    this.waiting = 0; // calls in the queue, not yet started
    this.timer = null; // to close its windows when it leaves them
    this.lastCall = 0; // when its last call ended
    this.left = null; // { url, device, why }, once its windows were closed for it
    this.note = null; // to be told with the next answer
    this.marker = null; // its name in agentsDir while it is marked as having windows
    this.mouse = null; // { page, x, y }: where it last sent the mouse, by coordinates
  }
}

const agents = new Map(); // by subagent id
const agentFor = id => {
  if (!agents.has(id)) agents.set(id, new Agent(id));
  return agents.get(id);
};
const main = agentFor("");
const safeId = id => typeof id === "string" && /^[\w-]{1,100}$/.test(id);

// The agent a call is from, as the hook said before Claude Code sent it. With no hook, or
// none that has said, the call is the main conversation's.
function callerOf(message) {
  const call = message.params?._meta?.["claudecode/toolUseId"];
  if (!safeId(call)) return main;
  const file = join(agentsDir, `call-${call}.json`);
  let said;
  try {
    said = JSON.parse(readFileSync(file, "utf8"));
    unlinkSync(file);
  } catch {
    return main;
  }
  const agent = safeId(said.agent) ? agentFor(said.agent) : main;
  if (safeId(said.session)) agent.session = said.session;
  return agent;
}

// While an agent has windows the hook is to say when it has finished, which it only does
// for agents marked here, so that every turn ending in every session leaves nothing behind.
function mark(agent) {
  const name = agent.id || agent.session;
  if (!name || agent.marker === name) return;
  unmark(agent);
  try {
    writeFileSync(join(agentsDir, `open-${name}`), String(process.pid));
    agent.marker = name;
  } catch {}
}

function unmark(agent) {
  if (!agent.marker) return;
  try {
    unlinkSync(join(agentsDir, `open-${agent.marker}`));
  } catch {}
  agent.marker = null;
}

const alive = pid => {
  if (!(pid > 0)) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error.code === "EPERM";
  }
};

// The hook's files, and marks of wrappers that have gone, that nobody is left to take.
function tidyAgentsDir() {
  const now = Date.now();
  for (const file of readdirSync(agentsDir)) {
    const path = join(agentsDir, file);
    try {
      if (file.startsWith("open-")) {
        if (!alive(Number(readFileSync(path, "utf8")))) unlinkSync(path);
      } else if (now - statSync(path).mtimeMs > 60 * 60 * 1000) {
        unlinkSync(path);
      }
    } catch {}
  }
}

function watchAgents() {
  try {
    mkdirSync(agentsDir, { recursive: true });
    tidyAgentsDir();
    watch(agentsDir, (event, file) => {
      if (typeof file === "string" && file.startsWith("done-")) heardDone(file);
    }).on("error", () => {});
  } catch {}
}

// The hook says an agent of this process's has finished: a subagent's windows close at once,
// the conversation's a little while after its turn has ended, so that a quick answer from
// the user finds them as they were.
function heardDone(file) {
  const agent = [...agents.values()].find(candidate => candidate.marker === file.slice("done-".length));
  if (!agent) return;
  try {
    unlinkSync(join(agentsDir, file));
  } catch {
    return; // heard already
  }
  if (agent.id) {
    closeFor(agent, "when your task ended", false);
  } else {
    clearTimeout(agent.timer);
    agent.timer = setTimeout(() => closeFor(agent, "after your turn ended", true), turnEndMs);
  }
}

function leftUnused(agent) {
  clearTimeout(agent.timer);
  agent.timer = setTimeout(() => closeFor(agent, `after ${minutes(idleMs)} unused`, true), idleMs);
}

// Closes an agent's windows, in its turn, unless a call of its own is waiting. Unless it has
// finished for good, not while the user is in one of them: they may be signing in there, or
// looking at what Claude did. Then it is asked again later.
function closeFor(agent, why, unlessInUse) {
  agent.timer = null;
  agent.queue = agent.queue
    .then(async () => {
      if (agent.waiting) return;
      if (unlessInUse && (await inUse(agent))) {
        agent.timer = setTimeout(() => closeFor(agent, why, true), turnEndMs);
        return;
      }
      await putAway(agent, why);
    })
    .catch(() => {});
}

// Whether the user has been in one of an agent's windows lately: real input there, mouse
// or keys, since the agent's last call ended. A page can't say whether it has the
// keyboard, since Playwright tells every page it connects to that it has.
async function inUse(agent) {
  const pages = ((await listPages()) ?? []).filter(page => agent.ownPages.has(page.id));
  const said = await Promise.all(pages.map(page => askPage(page, "Runtime.evaluate", { expression: "window.__exodiumInputAt ?? 0", returnByValue: true })));
  const latest = Math.max(0, ...said.map(answer => Number(answer?.result?.value) || 0));
  return latest > agent.lastCall + 1000 && Date.now() - latest < turnEndMs;
}

// Closes an agent's windows for it, and remembers where it was, so that its next call can
// take a new window back there. A subagent's Playwright server goes too.
async function putAway(agent, why) {
  clearTimeout(agent.timer);
  agent.timer = null;
  if (agent.ownPages.size) {
    const where = await whereIs(agent).catch(() => ({}));
    await endVideo(agent, why);
    await closeClaudePages(agent);
    agent.left = { ...where, why };
  }
  agent.emulated.clear();
  if (agent.id) stopServer(agent);
  unmark(agent);
}

// The address an agent's page is on, and the device it was given. Only ever one of its own
// windows': Playwright's current page may be anyone's once the agent's has gone.
async function whereIs(agent) {
  const pages = ((await listPages()) ?? []).filter(page => agent.ownPages.has(page.id));
  let page = pages.find(candidate => candidate.id === agent.current);
  if (!page && agent.server) {
    const current = listedTabs(await toolText(agent, "browser_tabs", { action: "list" })).find(tab => tab.current);
    page = pages.find(candidate => candidate.url === current?.url);
  }
  if (!page) return {};
  return { url: page.url.includes("exodium_start.html") ? undefined : page.url, device: agent.emulated.get(page.id) };
}

// A new window after the old ones were closed for the agent: the device it had is set
// again, and unless the call goes somewhere itself, it goes back to the address it was on.
async function comeBack(agent, goesSomewhere) {
  const { url, device, why } = agent.left;
  agent.left = null;
  const said = [`Your window had been closed ${why}, so this is a new one.`];
  if (url && !goesSomewhere) {
    const went = await request(agent, "tools/call", { name: "browser_navigate", arguments: { url } });
    said.push(went.error || went.result?.isError ? `Going back to ${url} failed.` : `It went back to ${url}; whatever had been done in the page is gone.`);
  }
  if (device) {
    try {
      const { id } = await runCode(agent, deviceCode(device));
      agent.emulated.set(id, device);
      agent.emulating = true;
      said.push("The device it had was set again.");
    } catch {
      said.push("Setting the device it had again failed.");
    }
  }
  return said.join(" ");
}

// Whether a call goes to an address of its own, so that going back first would be wasted.
function goesSomewhere(name, args = {}) {
  if (name === "browser_navigate") return true;
  if (name === "browser_tabs") return args.action === "new";
  if (name === "browser_batch") return Boolean(args.actions?.[0]) && goesSomewhere(args.actions[0].name, args.actions[0].input);
  if (name === "browser_filmstrip") return Boolean(args.action) && goesSomewhere(args.action.name, args.action.input);
  return false;
}

// ---- Playwright's servers ----

let initializeParams = null;
let nextRequest = 1;
const awaited = new Map(); // { server, take } for answers something here is waiting for, by request id
const passedOn = new Map(); // { server, id } for requests of a server's that Claude Code is to answer

function startServer(agent) {
  // Through a shell because npx is a script on Windows.
  const options = ["--cdp-endpoint", endpoint, "--caps", capabilities, "--output-dir", outputDir, "--output-max-size", String(outputBudget)];
  const server = spawn("npx", ["-y", playwright, ...options], { shell: true, stdio: ["pipe", "pipe", "inherit"] });
  agent.server = server;
  server.stdin.on("error", () => {});
  createInterface({ input: server.stdout }).on("line", line => fromServer(server, line));
  server.on("exit", () => {
    for (const [id, waiting] of awaited) {
      if (waiting.server !== server) continue;
      awaited.delete(id);
      waiting.take({ jsonrpc: "2.0", id, error: { code: -32000, message: "Playwright's server stopped." } }, "");
    }
    for (const [id, asked] of passedOn) if (asked.server === server) passedOn.delete(id);
    if (agent.server === server) {
      agent.server = null;
      agent.attached = false;
    }
  });
}

function fromServer(server, line) {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }
  const waiting = message.method === undefined ? awaited.get(message.id) : undefined;
  if (waiting) {
    awaited.delete(message.id);
    waiting.take(message, line);
  } else if (message.method !== undefined && message.id !== undefined) {
    // One of its own for Claude Code, such as for the roots. Each server numbers its own,
    // so the number is made unique before it goes on, and put back for the answer.
    const id = `exodium-asks-${nextRequest++}`;
    passedOn.set(id, { server, id: message.id });
    process.stdout.write(JSON.stringify({ ...message, id }) + "\n");
  } else {
    process.stdout.write(line + "\n");
  }
}

function stopServer(agent) {
  // The whole tree: the shell, npx, and the server that holds the connection.
  const server = agent.server;
  agent.server = null;
  agent.attached = false;
  if (server) spawnSync("taskkill", ["/pid", String(server.pid), "/t", "/f"], { stdio: "ignore" });
}

function request(agent, method, params) {
  const id = `exodium-${nextRequest++}`;
  const server = agent.server;
  if (!server) return Promise.resolve({ jsonrpc: "2.0", id, error: { code: -32000, message: "Playwright's server isn't running." } });
  return new Promise(resolve => {
    awaited.set(id, { server, take: resolve });
    server.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
  });
}

// The main conversation's server is started with this process, and introduced by Claude
// Code itself. Any other, and any started again, is introduced here as Claude Code did it.
async function ensureServer(agent) {
  if (agent.server) return;
  startServer(agent);
  await request(agent, "initialize", initializeParams);
  agent.server?.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");
  agent.restarted = true;
}

// What the new connection loses, the recording and the devices, is seen to on either side.
async function restartServer(agent) {
  await refuseIfSilent(agent); // the connection there is now is better than none
  await endVideo(agent, "when the connection to Exodium was renewed");
  stopServer(agent);
  await ensureServer(agent);
}

async function toolText(agent, name, args) {
  const answer = await request(agent, "tools/call", { name, arguments: args });
  return (answer.result?.content ?? []).map(part => part.text ?? "").join("\n") || (answer.error?.message ?? "");
}

// Runs code in the agent's Playwright server on its current page and gives back what it
// returned. Playwright waits half a second after any code before it answers, so this is
// kept for what nothing else can do.
async function runCode(agent, code) {
  const answer = await request(agent, "tools/call", { name: "browser_run_code_unsafe", arguments: { code } });
  const text = (answer.result?.content ?? []).map(part => part.text ?? "").join("\n");
  if (answer.error || answer.result?.isError) throw new Error(answer.error?.message ?? text);
  const result = text.match(/### Result\n([\s\S]*?)(?:\n### |(?![\s\S]))/)?.[1];
  return result === undefined ? undefined : JSON.parse(result);
}

// Playwright's list of tabs: "- 1: (current) [Title](url)".
function listedTabs(text) {
  return [...text.matchAll(/^- (\d+): (\(current\) )?\[.*\]\((.*)\)( \[crashed\])?\s*$/gm)].map(match => ({
    index: Number(match[1]),
    current: match[2] !== undefined,
    url: match[3],
  }));
}

// Every page there is, or nothing if the port isn't answering.
async function listPages() {
  try {
    const answer = await fetch(`${endpoint}/json/list`, { signal: AbortSignal.timeout(2000) });
    return (await answer.json()).filter(target => target.type === "page");
  } catch {
    return null;
  }
}

// One question on a connection of its own, to the browser or to one page. Undefined if no
// answer comes in time, and null if there is no connection to ask on.
async function askOver(url, method, params, ms) {
  let socket = null;
  try {
    socket = new WebSocket(url);
    return await new Promise(resolve => {
      const timer = setTimeout(() => resolve(undefined), ms);
      socket.onerror = () => {
        clearTimeout(timer);
        resolve(null);
      };
      socket.onopen = () => socket.send(JSON.stringify({ id: 1, method, params }));
      socket.onmessage = event => {
        const message = JSON.parse(event.data);
        if (message.id !== 1) return;
        clearTimeout(timer);
        resolve(message);
      };
    });
  } catch {
    return null;
  } finally {
    try {
      socket?.close();
    } catch {}
  }
}

async function askBrowser(method, params = {}) {
  const version = await (await fetch(`${endpoint}/json/version`, { signal: AbortSignal.timeout(2000) })).json();
  const answer = await askOver(version.webSocketDebuggerUrl, method, params, 3000);
  if (!answer) throw new Error(`no answer to ${method}`);
  if (answer.error) throw new Error(answer.error.message);
  return answer.result;
}

const askPage = async (page, method, params, ms = 2000) => (await askOver(page.webSocketDebuggerUrl, method, params, ms))?.result;

// The target of each of Playwright's tabs, in the order it lists them, which is the order
// of the context's pages. Asked for only when addresses can't tell pages apart, since it
// costs Playwright's half second.
const tabIdsCode = `async page => {
  const context = page.context();
  const ids = context.exodiumIds ??= new WeakMap();
  const out = [];
  for (const tab of context.pages()) {
    if (!ids.has(tab)) {
      const session = await context.newCDPSession(tab);
      try { ids.set(tab, (await session.send("Target.getTargetInfo")).targetInfo.targetId); }
      finally { await session.detach().catch(() => {}); }
    }
    out.push(ids.get(tab));
  }
  return out;
}`;

// Kept for as long as the tabs, and the pages there are, stay the same.
async function tabTargets(agent, tabs, pageIds) {
  const list = `${tabs.map(tab => tab.url).join("\n")}\n${[...pageIds].sort().join(" ")}`;
  if (agent.tabIds.list !== list) agent.tabIds = { list, ids: await runCode(agent, tabIdsCode) };
  return agent.tabIds.ids;
}

// Where a page is in an agent's tab list. Its address says, unless another page has the same.
async function indexOfPage(agent, tabs, pages, id) {
  const url = pages.find(page => page.id === id)?.url;
  const same = tabs.filter(tab => tab.url === url);
  if (url === undefined || !same.length) return undefined;
  if (same.length === 1 && pages.filter(page => page.url === url).length === 1) return same[0].index;
  try {
    const index = (await tabTargets(agent, tabs, pages.map(page => page.id))).indexOf(id);
    return index < 0 ? undefined : index;
  } catch {
    return same.at(-1).index; // the newest is the likeliest
  }
}

// The page of one of an agent's tabs, if it can be told.
async function pageOfTab(agent, tabs, index) {
  const tab = tabs.find(candidate => candidate.index === index);
  const pages = await listPages();
  if (!tab || !pages) return null;
  const same = pages.filter(page => page.url === tab.url);
  if (same.length === 1 && tabs.filter(candidate => candidate.url === tab.url).length === 1) return same[0].id;
  try {
    return (await tabTargets(agent, tabs, pages.map(page => page.id)))[index] ?? null;
  } catch {
    return null;
  }
}

// Makes a page the agent's current one in Playwright. It shows up there shortly after it
// does on the port. The address is asked for each time, since it may be on its way through
// a redirect. The time allowed starts with Playwright's first answer: connecting, at the
// first call, takes longer the more pages there are.
async function selectPage(agent, id) {
  let deadline = null;
  for (;;) {
    const pages = (await listPages()) ?? [];
    const text = await toolText(agent, "browser_tabs", { action: "list" });
    if (failedToConnect(text)) throw new Error(await whyNoConnection(agent, text));
    deadline ??= Date.now() + pageWaitMs;
    const index = await indexOfPage(agent, listedTabs(text), pages, id);
    if (index !== undefined) {
      await toolText(agent, "browser_tabs", { action: "select", index });
      await catchPickersOnlyHere(agent);
      agent.attached = true;
      agent.current = id;
      return;
    }
    if (Date.now() > deadline) throw new Error("Playwright never saw the page opened for it.");
    await sleep(pollMs);
  }
}

// ---- pages that hold up connecting ----

// Playwright, as it connects, waits for every page there is to answer, and gives up after
// half a minute if one doesn't. A page showing a dialog (an alert, a confirm, "leave
// site?") doesn't answer until it is answered itself, nor does one stuck in a script. So
// before Playwright connects, each page is asked something trivial, and if one stays
// silent Claude is told which, at once, rather than after a minute of waiting.
const silenceMs = 3000;

async function silentPages() {
  const pages = ((await listPages()) ?? []).filter(page => page.webSocketDebuggerUrl);
  const answered = await Promise.all(pages.map(page => askOver(page.webSocketDebuggerUrl, "Runtime.evaluate", { expression: "1" }, silenceMs)));
  return pages.filter((page, at) => answered[at] === undefined); // a page that can't be asked, Playwright will find out about
}

function silenceHelp(agent, silent) {
  const whose = page =>
    agent.ownPages.has(page.id) ? ", one of your own" : [...agents.values()].some(other => other.ownPages.has(page.id)) ? ", another agent's" : "";
  const named = silent.map(page => `"${page.title || "untitled"}" (${page.url})${whose(page)}`);
  return (
    "The browser tools can't connect to Exodium while a page isn't answering, as a page showing a dialog " +
    `(an alert, a confirm, "leave site?") or stuck in a script doesn't. Not answering: ${named.join("; ")}. ` +
    "If it is one of the user's tabs, ask them to answer the dialog or close the tab, then try again."
  );
}

async function refuseIfSilent(agent) {
  const silent = await silentPages();
  if (silent.length) throw new Error(silenceHelp(agent, silent));
}

const failedToConnect = text => /initializeServer|connectOverCDP/.test(text);

async function whyNoConnection(agent, text) {
  const silent = await silentPages();
  return silent.length ? silenceHelp(agent, silent) : text;
}

// Playwright stops a page's file picker from opening, and tells Claude about it instead, for
// browser_file_upload to answer, for as long as the page has a "filechooser" listener. Its
// server puts one on every page it knows of, the user's own tabs included, so a picker
// opened there went nowhere. Those listeners are set aside on every page but the one the
// agent is working in, which is the `page` this code is given, and on every page that
// turns up later. Taking a page's last listener away is what lets its pickers open again.
// Run in the agent's Playwright server, as browser_zoom's code is, after anything that
// changes its page.
//
// The same code has the page note the last real input it gets, for inUse: from then on, in
// every document it loads.
const inputWatch = `(() => {
  if (window.__exodiumInputWatched) return;
  window.__exodiumInputWatched = true;
  for (const type of ["pointerdown", "pointermove", "keydown", "wheel", "touchstart"]) {
    addEventListener(type, event => { if (event.isTrusted) window.__exodiumInputAt = Date.now(); }, { capture: true, passive: true });
  }
})()`;
const pickerCode = `async page => {
  if (!page.exodiumWatched) {
    page.exodiumWatched = true;
    await page.addInitScript({ content: ${JSON.stringify(inputWatch)} }).catch(() => {});
    await page.evaluate(${JSON.stringify(inputWatch)}).catch(() => {});
  }
  const context = page.context();
  const pickers = context.exodiumPickers ??= { claude: null, setAside: new WeakMap() };
  const setAside = tab => {
    const listeners = tab.listeners("filechooser");
    if (!listeners.length) return;
    pickers.setAside.set(tab, [...(pickers.setAside.get(tab) ?? []), ...listeners]);
    for (const listener of listeners) tab.off("filechooser", listener);
  };
  const putBack = tab => {
    for (const listener of pickers.setAside.get(tab) ?? []) tab.on("filechooser", listener);
    pickers.setAside.delete(tab);
  };
  if (!pickers.watching) {
    pickers.watching = true;
    context.on("page", tab => { if (tab !== pickers.claude) setAside(tab); });
  }
  pickers.claude = page;
  for (const tab of context.pages()) (tab === page ? putBack : setAside)(tab);
  return "";
}`;

// Playwright won't run it while the agent's page has a picker or a dialog waiting for an
// answer, and Claude may have just gone back to such a page. It is then run again after
// each of the agent's calls until it goes through, which is once the answer has been given.
async function catchPickersOnlyHere(agent) {
  const answer = await request(agent, "tools/call", { name: "browser_run_code_unsafe", arguments: { code: pickerCode } });
  agent.pickersLeftToSort = Boolean(answer.error || answer.result?.isError);
}

async function sortPickersIfLeft(agent) {
  if (agent.pickersLeftToSort) await catchPickersOnlyHere(agent);
}

// ---- Exodium ----

// Opens a window as a link would, starting Winamp if need be.
function openWindow(url) {
  const option = winampClass ? `--class=${winampClass} ` : "";
  const line = `${option}"${url.replaceAll('"', "%22")}"`;
  return new Promise((resolve, reject) => {
    execFile(join(here, "exodium_open.exe"), [line], { windowsVerbatimArguments: true }, error =>
      error ? reject(new Error(`exodium_open.exe failed: ${error.message}`)) : resolve(),
    );
  });
}

// Opens a window on `url` for an agent and returns its page, or nothing if none came. `isIt`
// tells it from whatever else turns up: Winamp starting up brings its own first window along.
async function openPage(agent, url, isIt) {
  const known = new Set((await listPages())?.map(page => page.id));
  await openWindow(url);
  const deadline = Date.now() + pageWaitMs;
  while (Date.now() < deadline) {
    const fresh = (await listPages())?.find(page => !known.has(page.id) && isIt(page));
    if (fresh) {
      agent.ownPages.add(fresh.id);
      return fresh;
    }
    await sleep(pollMs);
  }
  return null;
}

let portStayedShut = false; // a window was opened and the port never showed it

// A window on the start page. Its address is its own, because Playwright only knows pages
// by their addresses, and another agent may be opening one just now. It names this
// process, which Exodium watches, to close the window when the session ends.
async function openStartPage(agent) {
  const url = `${startPage}#pid=${process.pid}&${randomUUID()}`;
  return openPage(agent, url, candidate => candidate.url === url);
}

// The agent's window, opened at its first call, and again if it has gone since: closed
// for it, or by the user, or with Winamp. A port that won't open is only found out once;
// opening a window at every call won't help it. True if the window is a new one.
async function ensureWindow(agent) {
  await ensureServer(agent);
  const pages = await listPages();
  const own = pages?.findLast(page => agent.ownPages.has(page.id));
  if (own && agent.attached) {
    // Playwright moves on to the next page there is when the current one closes, and that
    // may be another agent's or a tab in use.
    if (agent.current && !pages.some(page => page.id === agent.current)) await returnToOwnPage(agent);
    return false;
  }
  if (own) {
    // Opened, but Playwright couldn't connect to go to it, last time.
    await refuseIfSilent(agent);
    await selectPage(agent, own.id);
    return false;
  }
  if (!pages && portStayedShut) throw new Error(portHelp);

  const lost = agent.ownPages.size > 0;
  agent.ownPages.clear();
  agent.current = null;
  const page = await openStartPage(agent);
  portStayedShut = !page;
  if (!page) throw new Error(portHelp);
  if (lost) await restartServer(agent); // its connection may have gone with Winamp
  else if (!agent.attached) await refuseIfSilent(agent);
  await selectPage(agent, page.id);
  return true;
}

// A site opened a page that Playwright doesn't know about.
async function seePopups(agent) {
  const pages = await listPages();
  const tabs = listedTabs(await toolText(agent, "browser_tabs", { action: "list" }));
  if (!pages || pages.length <= tabs.length) return;
  const currentUrl = tabs.find(tab => tab.current)?.url;
  const current = agent.current ?? pages.find(page => page.url === currentUrl && agent.ownPages.has(page.id))?.id;
  await restartServer(agent);
  // Either way the new connection has the agent's page picked, and its file pickers caught.
  if (current && (await listPages())?.some(page => page.id === current)) await selectPage(agent, current);
  else await returnToOwnPage(agent);
}

// Playwright moves on to the next page when its current one closes, and that may be a tab
// that is in use. An agent stays in its own windows; if none is left, its next call opens one.
async function returnToOwnPage(agent) {
  agent.current = null;
  const page = (await listPages())?.findLast(candidate => agent.ownPages.has(candidate.id));
  if (page) await selectPage(agent, page.id);
}

// An agent's windows, and whatever pages of theirs opened.
async function closeClaudePages(agent) {
  let ids = [...agent.ownPages];
  try {
    ids = (await claudeTargets(agent)).map(target => target.targetId);
  } catch {}
  await Promise.all(ids.map(id => fetch(`${endpoint}/json/close/${id}`, { signal: AbortSignal.timeout(2000) }).catch(() => {})));
  agent.ownPages.clear();
  agent.current = null;
}

// ---- what an agent is shown of the other tabs ----

const pageTargets = async () => (await askBrowser("Target.getTargets")).targetInfos.filter(target => target.type === "page");

// An agent's pages: the windows opened for it, and the pages that pages of theirs opened.
async function claudeTargets(agent, pages) {
  pages ??= await pageTargets();
  const claude = new Set(pages.filter(target => agent.ownPages.has(target.targetId)).map(target => target.targetId));
  for (let grew = true; grew; ) {
    grew = false;
    for (const target of pages) {
      if (!claude.has(target.targetId) && claude.has(target.openerId)) {
        claude.add(target.targetId);
        grew = true;
      }
    }
  }
  return pages.filter(target => claude.has(target.targetId));
}

// Which of the listed tabs the agent is shown: its own, and whichever it is in.
async function tabsToShow(agent, tabs) {
  const keep = new Set(tabs.filter(tab => tab.current).map(tab => tab.index));
  let claude;
  let pages;
  try {
    pages = await pageTargets();
    claude = await claudeTargets(agent, pages);
  } catch {
    return null; // the browser can't say, so nothing is left out
  }
  const unsure = [];
  for (const tab of tabs) {
    const theirs = claude.filter(target => target.url === tab.url).length;
    if (!theirs) continue;
    if (theirs >= tabs.filter(other => other.url === tab.url).length) keep.add(tab.index);
    else unsure.push(tab); // the user, or another agent, has the same address open
  }
  if (unsure.length) {
    try {
      const ids = await tabTargets(agent, tabs, pages.map(page => page.targetId));
      const own = new Set(claude.map(target => target.targetId));
      for (const tab of unsure) if (own.has(ids[tab.index])) keep.add(tab.index);
    } catch {
      for (const tab of unsure) keep.add(tab.index); // better one too many than its own missing
    }
  }
  return keep;
}

// Leaves the other tabs out of a tab list in an answer: the "Open tabs" section of any
// tool's, and browser_tabs' own.
async function trimTabText(agent, text, isTabsTool) {
  const sections = text.split(/^(?=### )/m);
  for (const [at, section] of sections.entries()) {
    const title = section.slice(0, section.indexOf("\n"));
    if (title !== "### Open tabs" && !(isTabsTool && title === "### Result")) continue;
    const tabs = listedTabs(section);
    if (!tabs.length) continue;
    const keep = await tabsToShow(agent, tabs);
    if (!keep) continue;
    const left = tabs.length - keep.size;
    if (!left) continue;
    const lines = section.split("\n").filter(line => {
      const index = line.match(/^- (\d+): /)?.[1];
      return index === undefined || keep.has(Number(index));
    });
    const note = `- (and ${left} other tab${left === 1 ? "" : "s"}, not yours, not shown: the user's own, and any other agent's)`;
    const end = lines.findLastIndex(line => /^- \d+: /.test(line));
    lines.splice(end + 1, 0, note);
    sections[at] = lines.join("\n");
  }
  return sections.join("");
}

async function trimTabs(agent, name, args, result) {
  if (!result?.content || (name === "browser_tabs" && args.all)) return result;
  for (const part of result.content) {
    if (part.type === "text" && /^- \d+: /m.test(part.text)) part.text = await trimTabText(agent, part.text, name === "browser_tabs");
  }
  return result;
}

// ---- the user's password manager ----

// An extension's page (Bitwarden's popup and windows, and the frames of its menus, have
// these addresses), or the browser's own page that manages extensions.
const isExtensionUrl = url =>
  /^(view-source:)?chrome-extension:/i.test(String(url ?? "").trim()) ||
  /^(view-source:)?(chrome|edge):\/\/extensions/i.test(String(url ?? "").trim());

const notYours = what => textResult(`${what}: the user's password manager, or another of their browser extensions, which isn't yours to use.`, true);

// What a call aims at that could land in an extension's frame: elements, points on the page,
// and the keyboard (keys go where the focus is). Nothing for a call that aims at none of them.
function aimsOf(agent, name, args) {
  const fields = Array.isArray(args.fields) ? args.fields.map(field => field?.target) : [];
  const targets = [args.target, args.startTarget, args.endTarget, ...fields].filter(target => typeof target === "string" && target);
  const points = [];
  const point = (x, y) => {
    if (Number.isFinite(Number(x)) && Number.isFinite(Number(y))) points.push([Number(x), Number(y)]);
  };
  if (name === "browser_mouse_click_xy") point(args.x, args.y);
  if (name === "browser_mouse_drag_xy") {
    point(args.startX, args.startY);
    point(args.endX, args.endY);
  }
  // A button goes down, or comes up, wherever the mouse was last sent.
  if ((name === "browser_mouse_down" || name === "browser_mouse_up") && agent.mouse?.page === agent.current) {
    point(agent.mouse.x, agent.mouse.y);
  }
  const keys = ["browser_press_key", "browser_keydown", "browser_keyup", "browser_press_sequentially"].includes(name);
  return targets.length || points.length || keys ? { targets, points, keys } : null;
}

// Says whether the aims of a call land in an extension's frame, in the agent's page: null if
// they don't, or the page has no such frame. Run in the agent's Playwright server, which knows
// every frame of the page, Bitwarden's in their closed shadow roots included.
const frameGuardCode = aims => `async page => {
  const aims = ${JSON.stringify(aims)};
  const isExtension = url => /^chrome-extension:/i.test(url || "");
  const frames = page.frames().filter(frame => isExtension(frame.url()));
  if (!frames.length) return null;
  const inExtension = frame => {
    for (let at = frame; at; at = at.parentFrame()) if (isExtension(at.url())) return true;
    return false;
  };
  const boxes = [];
  for (const frame of frames) {
    const owner = await frame.frameElement().catch(() => null);
    const box = owner && (await owner.boundingBox().catch(() => null));
    if (box) boxes.push(box);
  }
  const onFrame = ([x, y]) => boxes.some(box => x >= box.x && x <= box.x + box.width && y >= box.y && y <= box.y + box.height);
  for (const target of aims.targets) {
    let handle = null;
    try {
      const locator = /^(f\\d+)?e\\d+$/.test(target) ? page.locator("aria-ref=" + target) : page.locator(target);
      handle = await locator.first().elementHandle({ timeout: 2000 });
    } catch {}
    if (!handle) return "unresolved";
    const frame = await handle.ownerFrame();
    // Playwright clicks an element in the middle, and an extension's frame there takes the
    // click: Bitwarden's frames sit in an element of the page's that is as big as they are.
    const box = await handle.boundingBox().catch(() => null);
    await handle.dispose();
    if (!frame || inExtension(frame)) return "element";
    if (box && onFrame([box.x + box.width / 2, box.y + box.height / 2])) return "covered";
  }
  if (aims.points.some(onFrame)) return "point";
  if (aims.keys) {
    for (const frame of frames) {
      if (await frame.evaluate(() => document.hasFocus()).catch(() => false)) return "keyboard";
    }
  }
  return null;
}`;

const frameRefusals = {
  element: "That element is in a frame of a browser extension's",
  covered: "That element is under a frame of a browser extension's, which would take the click",
  point: "That point is on a frame of a browser extension's",
  keyboard: "The keyboard is in a frame of a browser extension's (click into the page itself first)",
  unresolved: "That target couldn't be checked against the browser extension frames in this page (use a ref from a fresh snapshot)",
};

// Refuses a call that would take the agent to, into or onto an extension's page or frame.
async function refuseExtensions(agent, name, args) {
  const goesTo = name === "browser_navigate" || (name === "browser_tabs" && args.action === "new");
  if (goesTo && isExtensionUrl(args.url)) return notYours("That address is a browser extension's page");
  if (name === "browser_tabs" && args.action === "select") {
    const tab = listedTabs(await toolText(agent, "browser_tabs", { action: "list" })).find(listed => listed.index === args.index);
    return tab && isExtensionUrl(tab.url) ? notYours("That tab is a browser extension's page") : null;
  }
  if (name === "browser_tabs" || name === "browser_close") return null;

  // Already in one, as when the user's tab the agent is in went there: only leaving is allowed.
  const pages = await listPages();
  const current = pages?.find(page => page.id === agent.current);
  if (current && isExtensionUrl(current.url)) {
    return name === "browser_navigate" ? null : notYours("The page you are in is a browser extension's (go elsewhere, or select another tab)");
  }

  if (name === "browser_mouse_click_xy" || name === "browser_mouse_move_xy") agent.mouse = { page: agent.current, x: Number(args.x), y: Number(args.y) };
  if (name === "browser_mouse_drag_xy") agent.mouse = { page: agent.current, x: Number(args.endX), y: Number(args.endY) };
  const aims = aimsOf(agent, name, args);
  if (!aims) return null;
  // The agent's own pages are in its own profile, where there are no extensions.
  if (current) {
    try {
      if ((await claudeTargets(agent)).some(target => target.targetId === current.id)) return null;
    } catch {}
  }
  let found;
  try {
    found = await runCode(agent, frameGuardCode(aims));
  } catch (error) {
    return textResult(`Couldn't check that this keeps clear of the user's password manager in their tab: ${error.message}`, true);
  }
  return found ? notYours(frameRefusals[found] ?? "That is in a frame of a browser extension's") : null;
}

// Leaves extensions' pages out of a tab list in an answer, even one with the user's tabs.
function hideExtensionTabs(result) {
  for (const part of result?.content ?? []) {
    if (part.type !== "text" || !part.text.includes("chrome-extension:")) continue;
    let hidden = 0;
    part.text = part.text
      .split("\n")
      .filter(line => {
        const url = line.match(/^- \d+: (?:\(current\) )?\[.*\]\((.*)\)/)?.[1];
        if (url === undefined || !isExtensionUrl(url)) return true;
        hidden++;
        return false;
      })
      .join("\n");
    if (hidden) part.text += `\n(${hidden} page${hidden === 1 ? "" : "s"} of the user's browser extensions not shown: not yours to use)`;
  }
  return result;
}

// ---- files in Claude's scratchpad ----

const inside = (folder, file) => {
  const path = relative(folder.toLowerCase(), resolve(file).toLowerCase());
  return path === "" || (!path.startsWith("..") && !isAbsolute(path));
};
const forClaudeTemp = file => typeof file === "string" && isAbsolute(file) && inside(claudeTemp, file);

// A file to be read is copied into the output folder and read from there. One to be written
// is written there, and moved to where it was asked for once it has been.
async function passFiles(name, args) {
  const moves = [];
  const sent = { ...args };
  const copyIn = async file => {
    if (!forClaudeTemp(file)) return file;
    const copy = join(outputDir, "from-claude", randomUUID(), basename(file));
    await mkdir(dirname(copy), { recursive: true });
    await copyFile(file, copy);
    return copy;
  };
  if (forClaudeTemp(args.filename)) {
    if (readsFile.has(name)) {
      sent.filename = await copyIn(args.filename);
    } else {
      sent.filename = join(outputDir, "for-claude", randomUUID(), basename(args.filename));
      moves.push({ from: sent.filename, to: resolve(args.filename) });
    }
  }
  if (Array.isArray(args.paths)) sent.paths = await Promise.all(args.paths.map(copyIn));
  return { sent, moves };
}

async function finishMoves(moves, result) {
  for (const { from, to } of moves) {
    try {
      await stat(from);
    } catch {
      continue; // nothing was written
    }
    await mkdir(dirname(to), { recursive: true });
    await rename(from, to).catch(async () => {
      await copyFile(from, to);
    });
    await rmdir(dirname(from)).catch(() => {});
    for (const part of result?.content ?? []) {
      if (part.type === "text") part.text = part.text.replaceAll(from, to).replaceAll(relative(process.cwd(), from), to);
    }
  }
}

// ---- console ----

// Each message once, with how often it came, and no more of them than is worth reading:
// the first few, which may say what started it, and the latest.
function collapseMessages(body) {
  const lines = body.split("\n");
  const blank = lines.indexOf("");
  const header = blank < 0 ? [] : lines.slice(0, blank + 1);
  const entries = [];
  for (const line of lines.slice(header.length)) {
    if (/^\s/.test(line) && entries.length) entries[entries.length - 1] += "\n" + line;
    else if (line) entries.push(line);
  }
  const counted = new Map();
  for (const entry of entries) counted.set(entry, (counted.get(entry) ?? 0) + 1);
  let shown = [...counted].map(([entry, times]) => {
    const cut = entry.length > consoleEntryMost ? entry.slice(0, consoleEntryMost) + " …(cut short)" : entry;
    if (times === 1) return cut;
    const end = cut.indexOf("\n");
    return end < 0 ? `${cut} (${times} times)` : `${cut.slice(0, end)} (${times} times)${cut.slice(end)}`;
  });
  const repeats = entries.length - counted.size;
  if (repeats) header.splice(Math.max(0, header.length - 1), 0, `${repeats} repeats left out; each message is shown once, with how many times it came.`);

  const size = list => list.reduce((sum, entry) => sum + entry.length + 1, 0);
  if (size(shown) > consoleBudget) {
    const first = shown.slice(0, 5);
    const last = [];
    let room = consoleBudget - size(first);
    for (let i = shown.length - 1; i >= first.length && room - shown[i].length - 1 > 0; i--) {
      room -= shown[i].length + 1;
      last.unshift(shown[i]);
    }
    const missed = shown.length - first.length - last.length;
    shown = [...first, `… ${missed} more messages between these, left out. Pass filename to save them all.`, ...last];
  }
  return [...header, ...shown].join("\n");
}

function collapseConsole(result) {
  for (const part of result?.content ?? []) {
    if (part.type !== "text" || !part.text.includes("Total messages:")) continue;
    part.text = part.text.replace(/(### Result\n)([\s\S]*?)(?=\n### |(?![\s\S]))/, (_, title, body) => title + collapseMessages(body));
  }
}

// ---- tools of our own ----

function textResult(text, isError = false) {
  return { content: [{ type: "text", text }], ...(isError ? { isError } : {}) };
}

// Asked of Playwright's page because that is the one the agent is working in, which the
// port can't say. What is run is this and nothing else: the numbers are checked before
// they get here.
//
// The browser takes the picture by briefly changing the page's viewport, unseen by the page,
// and leaves it changed if the session goes before the picture is done. A page out of
// view never finishes one, so it isn't asked to, and the session always waits for the end.
async function zoom(agent, { region, scale = 1 }) {
  const [left, top, right, bottom] = (region ?? []).map(Number);
  const width = right - left;
  const height = bottom - top;
  if (!(width > 0 && height > 0)) return textResult("region has to be [x0, y0, x1, y1], with x1 > x0 and y1 > y0.", true);

  const times = Math.min(zoomMostTimes, zoomLongEdge / Math.max(width, height)) * Math.min(1, Math.max(0.1, Number(scale)));
  const code = `async page => {
    if (await page.evaluate("document.visibilityState") !== "visible") return "";
    const ratio = await page.evaluate("devicePixelRatio");
    const clip = { x: ${left}, y: ${top}, width: ${width}, height: ${height}, scale: ${times} / ratio };
    const session = await page.context().newCDPSession(page);
    const picture = session.send("Page.captureScreenshot", { format: "png", clip });
    const finished = () => session.detach().catch(() => {});
    picture.then(finished, finished);
    const tooLong = new Promise(resolve => setTimeout(() => resolve({}), ${zoomWaitMs}));
    return (await Promise.race([picture, tooLong])).data ?? "";
  }`;
  const text = await toolText(agent, "browser_run_code_unsafe", { code });
  const data = text.match(/[A-Za-z0-9+/=]{100,}/)?.[0];
  if (!data) {
    return textResult("No picture came back. A page only draws while it is in view: its window may be minimized or its tab hidden.", true);
  }
  return {
    content: [
      { type: "text", text: `Region ${left},${top} to ${right},${bottom}, magnified ${times.toFixed(1)} times.` },
      { type: "image", data, mimeType: "image/png" },
    ],
  };
}

async function batch(agent, { actions }) {
  const refused = actions?.find(action => notInBatch.has(action.name));
  if (refused) return textResult(`${refused.name} can't be used in a batch.`, true);

  const content = [];
  for (const [index, action] of (actions ?? []).entries()) {
    const result = await runTool(agent, action.name, action.input ?? {});
    content.push({ type: "text", text: `[${index + 1}/${actions.length}] ${action.name}` }, ...(result.content ?? []));
    if (result.isError) return { content, isError: true };
  }
  return { content };
}

// Video, made by the same means as Playwright's, of the agent's page alone.
async function startVideo(agent, { filename, size, fps, cursor }) {
  if (agent.video && !agent.video.ended) return textResult("Video recording has already been started.", true);
  const file = filename
    ? resolve(process.cwd(), filename)
    : join(outputDir, `video-${new Date().toISOString().replace(/[:.]/g, "-")}.webm`);
  const allowed = [outputDir, process.cwd(), claudeTemp];
  if (!allowed.some(folder => inside(folder, file))) {
    return textResult(`File access denied: ${file} is outside allowed roots. Allowed roots: ${allowed.join(", ")}`, true);
  }
  await mkdir(dirname(file), { recursive: true });
  const options = { path: file };
  if (size) options.size = { width: Number(size.width), height: Number(size.height) };
  if (fps) options.fps = Number(fps);
  await runCode(
    agent,
    `async page => {
    await page.screencast.start(${JSON.stringify(options)});
    page.context().exodiumVideo = page;
    ${cursor ? 'await page.screencast.showActions({ cursor: "pointer", duration: 800, style: { title: "display: none" } });' : ""}
    return "";
  }`,
  );
  agent.video = { file, ended: null };
  return textResult("Video recording started. Only your own page is recorded.");
}

const stopVideoCode = `async page => {
  const context = page.context();
  const recorded = context.exodiumVideo;
  delete context.exodiumVideo;
  if (recorded) await recorded.screencast.stop().catch(() => {});
  return "";
}`;

// The recording stops early when its connection, or its page, is about to go.
async function endVideo(agent, when) {
  if (!agent.video || agent.video.ended) return;
  await runCode(agent, stopVideoCode).catch(() => {});
  agent.video.ended = when;
}

async function stopVideo(agent) {
  if (!agent.video) return textResult("No videos were recorded.");
  if (!agent.video.ended) await runCode(agent, stopVideoCode);
  const { file, ended } = agent.video;
  agent.video = null;
  return textResult(`### Result\n- [Video](${file})${ended ? `\n- The recording stopped early, ${ended}.` : ""}`);
}

// Devices. The browser keeps a device's settings for as long as the connection that set
// them stays, so each page has one of its own in the agent's Playwright server, kept there.
function deviceSettings(args) {
  const preset = devices[args.device];
  if (!preset && args.device !== "off") throw new Error(`device has to be one of: ${[...Object.keys(devices), "off"].join(", ")}.`);
  if (!preset) return null;
  const settings = { ...preset };
  for (const key of ["width", "height", "scale"]) if (Number(args[key]) > 0) settings[key] = Number(args[key]);
  for (const key of ["mobile", "touch"]) if (typeof args[key] === "boolean") settings[key] = args[key];
  if (args.landscape) [settings.width, settings.height] = [settings.height, settings.width];
  settings.landscape = Boolean(args.landscape);
  settings.cpu = clamp(args.cpu, 1, 20, 1);
  return settings;
}

// Sets `settings` on the agent's page, or takes its device away for null, and says what the
// page sees now.
const deviceCode = settings => `async page => {
  const settings = ${JSON.stringify(settings)};
  const context = page.context();
  const sessions = context.exodiumDevices ??= new WeakMap();
  let session = sessions.get(page);
  if (!settings) {
    sessions.delete(page);
    if (session) await session.detach().catch(() => {});
    session = null;
  } else {
    if (!session) {
      session = await context.newCDPSession(page);
      sessions.set(page, session);
    }
    session.exodiumSettings = settings;
    await session.send("Emulation.setDeviceMetricsOverride", {
      width: Math.round(settings.width), height: Math.round(settings.height),
      deviceScaleFactor: settings.scale, mobile: settings.mobile,
      screenOrientation: settings.landscape ? { type: "landscapePrimary", angle: 90 } : { type: "portraitPrimary", angle: 0 },
    });
    await session.send("Emulation.setTouchEmulationEnabled", { enabled: settings.touch, maxTouchPoints: settings.touch ? 5 : 1 });
    if (settings.agent) {
      await session.send("Emulation.setUserAgentOverride", {
        userAgent: settings.agent, platform: settings.platform,
        userAgentMetadata: {
          brands: settings.os === "Android" ? [{ brand: "Chromium", version: "156" }, { brand: "Google Chrome", version: "156" }] : [],
          platform: settings.os, platformVersion: settings.os === "Android" ? "14.0.0" : "17.5.0",
          architecture: "", model: settings.model ?? "", mobile: settings.mobile,
        },
      });
    }
    await session.send("Emulation.setCPUThrottlingRate", { rate: settings.cpu });
  }
  const probe = session ?? await context.newCDPSession(page);
  const id = (await probe.send("Target.getTargetInfo")).targetInfo.targetId;
  if (probe !== session) await probe.detach().catch(() => {});
  const seen = await page.evaluate(() => ({
    width: innerWidth, height: innerHeight, devicePixelRatio,
    coarsePointer: matchMedia("(pointer: coarse)").matches, maxTouchPoints: navigator.maxTouchPoints,
  }));
  return { id, seen };
}`;

async function emulateDevice(agent, args) {
  let settings;
  try {
    settings = deviceSettings(args);
  } catch (error) {
    return textResult(error.message, true);
  }
  const { id, seen } = await runCode(agent, deviceCode(settings));
  if (settings) {
    agent.emulated.set(id, settings);
    agent.emulating = true;
  } else {
    agent.emulated.delete(id);
  }
  const what = settings
    ? `${args.device}${settings.landscape ? ", sideways" : ""}${settings.cpu > 1 ? `, CPU ${settings.cpu} times slower` : ""}. ` +
      "Reload the page so the site starts over on it."
    : "The window's own size, mouse and CPU again.";
  return textResult(`### Result\n${what}\nThe page now sees: ${JSON.stringify(seen)}`);
}

// While the agent's page has a device, its size is the device's screen.
async function resizeDevice(agent, { width, height }) {
  const resized = await runCode(
    agent,
    `async page => {
    const session = page.context().exodiumDevices?.get(page);
    if (!session) return null;
    const settings = session.exodiumSettings;
    settings.width = ${Number(width)}; settings.height = ${Number(height)};
    await session.send("Emulation.setDeviceMetricsOverride", {
      width: Math.round(settings.width), height: Math.round(settings.height),
      deviceScaleFactor: settings.scale, mobile: settings.mobile,
      screenOrientation: settings.landscape ? { type: "landscapePrimary", angle: 90 } : { type: "portraitPrimary", angle: 0 },
    });
    return { id: (await session.send("Target.getTargetInfo")).targetInfo.targetId, settings };
  }`,
  );
  if (!resized) return null;
  agent.emulated.set(resized.id, resized.settings);
  return textResult(`### Result\nThe device's screen is now ${width}×${height}.`);
}

// On a touch device a click at a point is a tap: touches, from which the page makes its own
// click, as a phone's browser does. Turning the mouse into touches, as Chrome's device
// toolbar does, leaves Playwright waiting forever for its mouse events to be done with.
async function tap(agent, { x, y, button, clickCount }) {
  if ((button && button !== "left") || (clickCount && clickCount !== 1)) return null;
  const tapped = await runCode(
    agent,
    `async page => {
    const session = page.context().exodiumDevices?.get(page);
    if (!session?.exodiumSettings.touch) return false;
    await session.send("Input.dispatchTouchEvent", { type: "touchStart", touchPoints: [{ x: ${Number(x)}, y: ${Number(y)} }] });
    await session.send("Input.dispatchTouchEvent", { type: "touchEnd", touchPoints: [] });
    return true;
  }`,
  );
  return tapped ? textResult(`### Result\nTapped at (${x}, ${y}).`) : null;
}

// A new connection to Playwright has none of the old one's devices: the agent's page is
// given its own back.
async function restoreDevice(agent) {
  if (!agent.restarted) return;
  agent.restarted = false;
  if (!agent.emulated.size) return;
  const id = await runCode(
    agent,
    `async page => {
    const session = await page.context().newCDPSession(page);
    try { return (await session.send("Target.getTargetInfo")).targetInfo.targetId; }
    finally { await session.detach().catch(() => {}); }
  }`,
  ).catch(() => null);
  if (agent.emulated.has(id)) await runCode(agent, deviceCode(agent.emulated.get(id))).catch(() => {});
}

// Frames of what the page draws, sent by the browser as it draws them, and picked out at
// even times afterwards. Each is the last drawn by then: a page that draws nothing new
// gives the same frame again.
async function filmstrip(agent, { duration, frames, action, size }) {
  duration = clamp(duration, 100, 20000, 2000);
  frames = Math.round(clamp(frames, 2, 24, 8));
  size = Math.round(clamp(size, 160, 1280, 640));
  if (action && notFilmed.has(action.name)) return textResult(`${action.name} can't be filmed.`, true);

  const started = await runCode(
    agent,
    `async page => {
    if (await page.evaluate("document.visibilityState") !== "visible") return false;
    const context = page.context();
    const session = await context.newCDPSession(page);
    const film = context.exodiumFilm = { session, frames: [] };
    session.on("Page.screencastFrame", frame => {
      film.frames.push({ data: frame.data, at: frame.metadata.timestamp * 1000 });
      session.send("Page.screencastFrameAck", { sessionId: frame.sessionId }).catch(() => {});
    });
    await session.send("Page.startScreencast", { format: "jpeg", quality: 70, maxWidth: ${size}, maxHeight: ${size} });
    return true;
  }`,
  );
  if (!started) return textResult("The page isn't in view, so it draws nothing: its window may be minimized or its tab hidden.", true);

  const from = Date.now();
  let acted = null;
  if (action) acted = await runTool(agent, action.name, action.input ?? {});
  await sleep(Math.max(0, from + duration - Date.now()));

  const film = await runCode(
    agent,
    `async page => {
    const context = page.context();
    const film = context.exodiumFilm;
    delete context.exodiumFilm;
    if (!film) return null;
    await film.session.send("Page.stopScreencast").catch(() => {});
    await film.session.detach().catch(() => {});
    const drawn = film.frames;
    const picked = [];
    for (let i = 0; i < ${frames}; i++) {
      const at = ${from} + (${duration} * i) / ${frames - 1};
      const frame = drawn.findLast(candidate => candidate.at <= at) ?? drawn[0];
      if (frame) picked.push({ at: Math.round(at - ${from}), data: frame.data, same: picked.at(-1)?.data === frame.data });
    }
    const during = drawn.filter(frame => frame.at >= ${from});
    return { picked, drawn: during.length };
  }`,
  );
  if (!film?.picked.length) return textResult("No frames came: the page drew nothing while it was watched.", true);

  const content = [];
  if (acted) {
    const said = (acted.content ?? []).filter(part => part.type === "text").map(part => part.text).join("\n");
    content.push({ type: "text", text: `${action.name}${acted.isError ? " failed" : ""}: ${said.split("\n### ")[0].slice(0, 600)}` });
  }
  content.push({
    type: "text",
    text:
      `${film.picked.length} frames over ${duration} ms${action ? " from the action" : ""}. The browser sent ${film.drawn} ` +
      `frames in that time, which is fewer than the page draws when it is busy. Frames are scaled to at most ${size} ` +
      "pixels, so their pixels aren't the page's.",
  });
  for (const frame of film.picked) {
    content.push({ type: "text", text: `+${frame.at} ms${frame.same ? " (nothing new drawn since the last)" : ""}` });
    content.push({ type: "image", data: frame.data, mimeType: "image/jpeg" });
  }
  return { content };
}

// ---- Claude Code ----

// The agent's call, as it came if nothing in it changed, so that it keeps its id.
function forward(agent, message, line, args) {
  const sent = args === message.params.arguments ? line : JSON.stringify({ ...message, params: { ...message.params, arguments: args } });
  const server = agent.server;
  if (!server) return Promise.resolve({ error: { code: -32000, message: "Playwright's server isn't running." } });
  return new Promise(resolve => {
    awaited.set(message.id, { server, take: answer => resolve(answer) });
    server.stdin.write(sent + "\n");
  });
}

// Whatever has to happen around a tool call. Gives back the result if that was all of it,
// and nothing if the call is Playwright's to answer.
async function prepareCall(agent, name, args) {
  const tabs = name === "browser_tabs";
  if (name === "browser_close") {
    const filmed = agent.video && !agent.video.ended ? agent.video.file : null;
    await endVideo(agent, "when your windows were closed");
    await closeClaudePages(agent);
    agent.left = null;
    agent.emulated.clear();
    return textResult(`Closed your windows in Exodium.${filmed ? ` The video being recorded is in ${filmed}.` : ""}`);
  }
  const fresh = await ensureWindow(agent);
  await restoreDevice(agent);
  if (fresh && agent.left) agent.note = await comeBack(agent, goesSomewhere(name, args));
  const refused = await refuseExtensions(agent, name, args);
  if (refused) return refused;
  if (name === "browser_zoom") return zoom(agent, args);
  if (name === "browser_batch") return batch(agent, args);
  if (name === "browser_emulate_device") return emulateDevice(agent, args);
  if (name === "browser_filmstrip") return filmstrip(agent, args);
  if (name === "browser_start_video") return startVideo(agent, args);
  if (name === "browser_stop_video") return stopVideo(agent);
  if (name === "browser_resize" && agent.emulating) {
    const resized = await resizeDevice(agent, args);
    if (resized) return resized;
  }
  if (name === "browser_mouse_click_xy" && agent.emulating) {
    const tapped = await tap(agent, args);
    if (tapped) return tapped;
  }
  if (tabs && args.action === "new") {
    // On the start page first, so that Exodium knows the window for one of Claude's. A
    // window this call has just opened is the new one already.
    if (!fresh) {
      const page = await openStartPage(agent);
      if (!page) throw new Error(`Exodium opened no window for ${args.url ?? "a new page"}.`);
      await selectPage(agent, page.id);
    }
    if (args.url) {
      const went = await request(agent, "tools/call", { name: "browser_navigate", arguments: { url: args.url } });
      if (went.error || went.result?.isError) return went.result ?? textResult(went.error.message, true);
    }
    return textResult(await toolText(agent, "browser_tabs", { action: "list" }));
  }
  if (tabs) await seePopups(agent);
  if (tabs && args.action === "select") {
    const answer = await request(agent, "tools/call", { name, arguments: { action: "select", index: args.index } });
    await catchPickersOnlyHere(agent);
    const text = (answer.result?.content ?? []).map(part => part.text ?? "").join("\n");
    agent.current = answer.result && !answer.result.isError ? await pageOfTab(agent, listedTabs(text), args.index) : agent.current;
    return answer.result ?? textResult(answer.error?.message ?? "No answer.", true);
  }
  if (tabs && args.action === "close") {
    await toolText(agent, "browser_tabs", { action: "close", index: args.index });
    // Closing another of its tabs leaves it where it was.
    if (!(agent.current && (await listPages())?.some(page => page.id === agent.current))) await returnToOwnPage(agent);
    return textResult(await toolText(agent, "browser_tabs", { action: "list" }));
  }
  return null;
}

// What is to be told with an answer, ahead of it.
function withNote(agent, result) {
  if (!agent.note) return result;
  const note = agent.note;
  agent.note = null;
  return { ...result, content: [{ type: "text", text: note }, ...(result.content ?? [])] };
}

// A call of an agent's, from start to finish. `send` gets Playwright's answer to the call as
// it is to go to Playwright, for the calls that are Playwright's.
async function callTool(agent, name, args, send) {
  try {
    let result = await prepareCall(agent, name, args);
    if (!result) {
      const { sent, moves } = await passFiles(name, name === "browser_tabs" ? { action: args.action, index: args.index, url: args.url } : args);
      const answer = await send(sent);
      await sortPickersIfLeft(agent);
      if (answer.error) return { error: answer.error };
      result = answer.result ?? textResult("No answer.", true);
      const said = (result.content ?? []).map(part => part.text ?? "").join("\n");
      if (result.isError && failedToConnect(said)) return { result: withNote(agent, textResult(await whyNoConnection(agent, said), true)) };
      await finishMoves(moves, result);
      if (name === "browser_console_messages") collapseConsole(result);
    }
    return { result: withNote(agent, hideExtensionTabs(await trimTabs(agent, name, args, result))) };
  } catch (error) {
    return { result: withNote(agent, textResult(error.message, true)) };
  }
}

// One tool of a batch or a filmstrip, whose answer goes into theirs.
async function runTool(agent, name, args) {
  const { result, error } = await callTool(agent, name, args, async sent => request(agent, "tools/call", { name, arguments: sent }));
  return result ?? textResult(error?.message ?? "No answer.", true);
}

async function handleCall(agent, message, line) {
  agent.waiting--;
  clearTimeout(agent.timer);
  agent.timer = null;
  mark(agent);
  const { name, arguments: args = {} } = message.params;
  const { result, error } = await callTool(agent, name, args, sent => forward(agent, message, line, sent));
  process.stdout.write(JSON.stringify({ jsonrpc: "2.0", id: message.id, ...(error ? { error } : { result }) }) + "\n");
  agent.lastCall = Date.now();
  leftUnused(agent);
}

// Playwright's tools as Claude is shown them.
function listTools(list) {
  if (!list.result?.tools) return list;
  for (const tool of list.result.tools) {
    if (toolNotes[tool.name]) tool.description = `${tool.description.replace(/\.?\s*$/, ".")} ${toolNotes[tool.name]}`;
    if (tool.name === "browser_tabs") {
      tool.inputSchema.properties.all = { type: "boolean", description: "For list: the user's own tabs, and other agents', too." };
    }
  }
  list.result.tools.push(...ownTools);
  return list;
}

function introduce(answer) {
  if (answer.result) answer.result.instructions = [answer.result.instructions, instructions].filter(Boolean).join("\n\n");
  return answer;
}

watchAgents();
startServer(main);

// Each agent's calls one at a time, since each may have to change what the next is
// working on. Different agents' go side by side.
const input = createInterface({ input: process.stdin });
input.on("line", line => {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }
  if (message.method === "tools/call") {
    const agent = callerOf(message);
    agent.waiting++;
    agent.queue = agent.queue.then(() => handleCall(agent, message, line)).catch(() => {});
    return;
  }
  if (message.method === undefined && passedOn.has(message.id)) {
    // Claude Code's answer to a server's own request.
    const { server, id } = passedOn.get(message.id);
    passedOn.delete(message.id);
    server.stdin.write(JSON.stringify({ ...message, id }) + "\n");
    return;
  }
  if (message.method === "initialize") {
    initializeParams = message.params;
    awaited.set(message.id, { server: main.server, take: answer => process.stdout.write(JSON.stringify(introduce(answer)) + "\n") });
  }
  if (message.method === "tools/list") {
    awaited.set(message.id, { server: main.server, take: list => process.stdout.write(JSON.stringify(listTools(list)) + "\n") });
  }
  if (message.method?.startsWith("notifications/") && message.method !== "notifications/initialized") {
    for (const agent of agents.values()) agent.server?.stdin.write(line + "\n"); // such as cancelling a call, wherever it is
    return;
  }
  main.server?.stdin.write(line + "\n");
});

// Claude Code ends a session by closing the wrapper's input, and closing its console sends
// a hangup. Claude's windows go with the session. Exodium closes them as well once this
// process has gone, however it went, but the Exodium running may be one from before that.
let ending = false;
async function endSession() {
  if (ending) return;
  ending = true;
  const tidy = () =>
    Promise.all(
      [...agents.values()].map(async agent => {
        await endVideo(agent, "when the session ended");
        await closeClaudePages(agent);
      }),
    );
  await Promise.race([tidy(), sleep(5000)]).catch(() => {});
  for (const agent of agents.values()) {
    unmark(agent);
    stopServer(agent);
  }
  process.exit(0);
}
input.on("close", endSession);
process.on("SIGHUP", endSession);
