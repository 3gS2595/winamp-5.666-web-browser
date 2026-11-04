#!/usr/bin/env node
// What Claude Code runs for its browser tools. The tools themselves are Playwright's MCP
// server, connected to Exodium's debugging port. This sits between the two, passing
// everything along, and sees to the things Playwright can't know about Exodium:
//
// - Claude gets a window of its own, so it never works in a tab that's in use. Playwright
//   left to itself takes whichever page comes first. Exodium shows it without the
//   keyboard, behind every other window, and it closes when the session ends.
// - Winamp is started if it isn't running, as it is for a link.
// - A new page is an Exodium window. Playwright's own way of making one gets a bare window
//   of the browser process's, outside Exodium. A window rather than a tab, because
//   Exodium hides a tab that isn't in front, and a hidden page can't be screenshotted.
// - A page a site opens itself is a real tab, but Playwright only sees those when it
//   connects. So it is connected again when there are pages it doesn't know about.
// - Playwright catches the file picker of every page it knows, which is every page there
//   is, so upload buttons in the user's own tabs did nothing while Claude was connected.
//   Only the page Claude is working in has its picker caught.
// - Playwright lists every tab there is in its answers. The user's own are left out of what
//   Claude is shown: they cost tokens, put the user's browsing in every transcript, and
//   aren't Claude's to work in.
// - Playwright records video of every page there is. Only Claude's is recorded.
// - Playwright only reads and writes files in its output folder and the project. Claude
//   keeps its own in its scratchpad, under %TEMP%\claude, so files headed there are
//   passed through the output folder.
// - A console that repeats one message thousands of times is shown with each message once.
//
// It also adds tools of its own, which Playwright has no equal for: browser_zoom, for a
// close look at part of a page; browser_batch, for several tools in one call;
// browser_emulate_device, for a phone or tablet; and browser_filmstrip, for frames of an
// animation.
//
// It goes in Winamp's Plugins folder, beside exodium_open.exe and exodium_start.html.
import { execFile, spawn, spawnSync } from "node:child_process";
import { randomUUID } from "node:crypto";
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
    "The user's own tabs are left out of the list, and out of the list other tools show: stay in your own " +
    "window. Pass all: true to list them too, when the user points you at one of them.",
  browser_start_video: "Only Claude's own page is recorded.",
};

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const clamp = (value, low, high, otherwise) => (Number.isFinite(Number(value)) ? Math.min(high, Math.max(low, Number(value))) : otherwise);

// ---- Playwright's server ----

let server = null;
let initializeParams = null;
let nextRequest = 1;
const awaited = new Map(); // answers something here is waiting for, by request id

function startServer() {
  // Through a shell because npx is a script on Windows.
  const options = ["--cdp-endpoint", endpoint, "--caps", capabilities, "--output-dir", outputDir, "--output-max-size", String(outputBudget)];
  server = spawn("npx", ["-y", playwright, ...options], { shell: true, stdio: ["pipe", "pipe", "inherit"] });
  createInterface({ input: server.stdout }).on("line", line => {
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      return;
    }
    const taker = awaited.get(message.id);
    if (!taker) {
      process.stdout.write(line + "\n");
      return;
    }
    awaited.delete(message.id);
    taker(message, line);
  });
}

function stopServer() {
  // The whole tree: the shell, npx, and the server that holds the connection.
  if (server) spawnSync("taskkill", ["/pid", String(server.pid), "/t", "/f"], { stdio: "ignore" });
  server = null;
}

function request(method, params) {
  const id = `exodium-${nextRequest++}`;
  return new Promise(resolve => {
    awaited.set(id, resolve);
    server.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
  });
}

// What the new connection loses, the recording and the devices, is seen to on either side.
async function restartServer() {
  await refuseIfSilent(); // the connection there is now is better than none
  await endVideo("when the connection to Exodium was renewed");
  attached = false;
  stopServer();
  startServer();
  await request("initialize", initializeParams);
  server.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");
  restarted = true;
}
let restarted = false; // and the device of Claude's page not yet given back to it

async function toolText(name, args) {
  const answer = await request("tools/call", { name, arguments: args });
  return (answer.result?.content ?? []).map(part => part.text ?? "").join("\n");
}

// Runs code in Playwright's server on Claude's current page and gives back what it returned.
// Playwright waits half a second after any code before it answers, so this is kept for
// what nothing else can do.
async function runCode(code) {
  const answer = await request("tools/call", { name: "browser_run_code_unsafe", arguments: { code } });
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

// One question to the browser itself, on a connection of its own.
async function askBrowser(method, params = {}) {
  const version = await (await fetch(`${endpoint}/json/version`, { signal: AbortSignal.timeout(2000) })).json();
  const socket = new WebSocket(version.webSocketDebuggerUrl);
  try {
    return await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`no answer to ${method}`)), 3000);
      socket.onerror = () => reject(new Error("no connection to the browser"));
      socket.onopen = () => socket.send(JSON.stringify({ id: 1, method, params }));
      socket.onmessage = event => {
        const message = JSON.parse(event.data);
        if (message.id !== 1) return;
        clearTimeout(timer);
        if (message.error) reject(new Error(message.error.message));
        else resolve(message.result);
      };
    });
  } finally {
    socket.close();
  }
}

// Makes a page Playwright's current one. It shows up there shortly after it does on the
// port, and only tabs' addresses say which is which. The newest goes first, and the
// address is asked for each time, since it may be on its way through a redirect. The time
// allowed starts with Playwright's first answer: connecting, at the first call, takes
// longer the more pages there are.
async function selectPage(id) {
  let deadline = null;
  for (;;) {
    const url = (await listPages())?.find(page => page.id === id)?.url;
    const text = await toolText("browser_tabs", { action: "list" });
    if (failedToConnect(text)) throw new Error(await whyNoConnection(text));
    deadline ??= Date.now() + pageWaitMs;
    const found = listedTabs(text).findLast(tab => tab.url === url);
    if (found) {
      await toolText("browser_tabs", { action: "select", index: found.index });
      await catchPickersOnlyHere();
      attached = true;
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
let attached = false; // Playwright has connected, and seen Claude's page

async function silentPages() {
  const pages = (await listPages()) ?? [];
  const silent = await Promise.all(
    pages.map(
      page =>
        new Promise(resolve => {
          let socket = null;
          const done = answered => {
            clearTimeout(timer);
            try {
              socket?.close();
            } catch {}
            resolve(answered ? null : page);
          };
          const timer = setTimeout(() => done(false), silenceMs);
          try {
            socket = new WebSocket(page.webSocketDebuggerUrl);
            socket.onopen = () => socket.send(JSON.stringify({ id: 1, method: "Runtime.evaluate", params: { expression: "1" } }));
            socket.onmessage = event => JSON.parse(event.data).id === 1 && done(true);
            socket.onerror = () => done(true); // can't tell; Playwright will find out
          } catch {
            done(true);
          }
        }),
    ),
  );
  return silent.filter(Boolean);
}

function silenceHelp(silent) {
  const named = silent.map(page => `"${page.title || "untitled"}" (${page.url})${ownPages.has(page.id) ? ", one of Claude's own" : ""}`);
  return (
    "The browser tools can't connect to Exodium while a page isn't answering, as a page showing a dialog " +
    `(an alert, a confirm, "leave site?") or stuck in a script doesn't. Not answering: ${named.join("; ")}. ` +
    "If it is one of the user's tabs, ask them to answer the dialog or close the tab, then try again."
  );
}

async function refuseIfSilent() {
  const silent = await silentPages();
  if (silent.length) throw new Error(silenceHelp(silent));
}

const failedToConnect = text => /initializeServer|connectOverCDP/.test(text);

async function whyNoConnection(text) {
  const silent = await silentPages();
  return silent.length ? silenceHelp(silent) : text;
}

// Playwright stops a page's file picker from opening, and tells Claude about it instead, for
// browser_file_upload to answer, for as long as the page has a "filechooser" listener. Its
// server puts one on every page it knows of, the user's own tabs included, so a picker
// opened there went nowhere. Those listeners are set aside on every page but the one Claude
// is working in, which is the `page` this code is given, and on every page that turns up
// later. Taking a page's last listener away is what lets its pickers open again. Run in
// Playwright's server, as browser_zoom's code is, after anything that changes Claude's page.
const pickerCode = `async page => {
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

// Playwright won't run it while Claude's page has a picker or a dialog waiting for an
// answer, and Claude may have just gone back to such a page. It is then run again after
// each of Claude's calls until it goes through, which is once the answer has been given.
let pickersLeftToSort = false;

async function catchPickersOnlyHere() {
  const answer = await request("tools/call", { name: "browser_run_code_unsafe", arguments: { code: pickerCode } });
  pickersLeftToSort = Boolean(answer.error || answer.result?.isError);
}

async function sortPickersIfLeft() {
  if (pickersLeftToSort) await catchPickersOnlyHere();
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

// Opens a window on `url` and returns its page, or nothing if none came. `isIt` tells it
// from whatever else turns up: Winamp starting up brings its own first window along.
async function openPage(url, isIt) {
  const known = new Set((await listPages())?.map(page => page.id));
  await openWindow(url);
  const deadline = Date.now() + pageWaitMs;
  while (Date.now() < deadline) {
    const fresh = (await listPages())?.find(page => !known.has(page.id) && isIt(page));
    if (fresh) {
      ownPages.add(fresh.id);
      return fresh;
    }
    await sleep(pollMs);
  }
  return null;
}

const ownPages = new Set(); // the pages of the windows opened here
let portStayedShut = false; // a window was opened and the port never showed it

// A window on the start page. Its address is its own, because Playwright only knows pages
// by their addresses, and another Claude session may be opening one just now. It names
// this process, which Exodium watches, to close the window when the session ends.
async function openStartPage() {
  const url = `${startPage}#pid=${process.pid}&${randomUUID()}`;
  return openPage(url, candidate => candidate.url === url);
}

// Claude's window, opened at the first call, and again if it has gone since: the user
// may have closed it, or Winamp. A port that won't open is only found out once; opening
// a window at every call won't help it.
async function ensureWindow() {
  const pages = await listPages();
  const own = pages?.findLast(page => ownPages.has(page.id));
  if (own && attached) return;
  if (own) {
    // Opened, but Playwright couldn't connect to go to it, last time.
    await refuseIfSilent();
    await selectPage(own.id);
    return;
  }
  if (!pages && portStayedShut) throw new Error(portHelp);

  const lost = ownPages.size > 0;
  ownPages.clear();
  const page = await openStartPage();
  portStayedShut = !page;
  if (!page) throw new Error(portHelp);
  if (lost) await restartServer(); // its connection may have gone with Winamp
  else if (!attached) await refuseIfSilent();
  await selectPage(page.id);
}

// A site opened a page that Playwright doesn't know about.
async function seePopups() {
  const pages = await listPages();
  const tabs = listedTabs(await toolText("browser_tabs", { action: "list" }));
  if (!pages || pages.length <= tabs.length) return;
  const current = tabs.find(tab => tab.current);
  await restartServer();
  const page = pages.find(candidate => candidate.url === current?.url);
  // Either way the new connection has Claude's page picked, and its file pickers caught.
  if (page) await selectPage(page.id);
  else await returnToOwnPage();
}

// Playwright moves on to the next page when its current one closes, and that may be a tab
// that is in use. Claude stays in its own windows; if none is left, the next call opens one.
async function returnToOwnPage() {
  const page = (await listPages())?.findLast(candidate => ownPages.has(candidate.id));
  if (page) await selectPage(page.id);
}

// Claude's windows, and whatever pages of theirs opened.
async function closeClaudePages() {
  let ids = [...ownPages];
  try {
    ids = (await claudeTargets()).map(target => target.targetId);
  } catch {}
  await Promise.all(ids.map(id => fetch(`${endpoint}/json/close/${id}`, { signal: AbortSignal.timeout(2000) }).catch(() => {})));
  ownPages.clear();
}

// ---- what Claude is shown of the user's tabs ----

// Claude's pages: the windows opened here, and the pages that pages of theirs opened.
async function claudeTargets() {
  const { targetInfos } = await askBrowser("Target.getTargets");
  const pages = targetInfos.filter(target => target.type === "page");
  const claude = new Set(pages.filter(target => ownPages.has(target.targetId)).map(target => target.targetId));
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

// The target of each of Playwright's tabs, in the order it lists them, which is the order
// of the context's pages. Asked for only when addresses can't tell Claude's tabs from the
// user's, since it costs Playwright's half second.
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
let tabIdsKnown = { list: "", ids: [] }; // for the last list that needed them

// Which of the listed tabs Claude is shown: its own, and whichever it is in.
async function tabsToShow(tabs) {
  const keep = new Set(tabs.filter(tab => tab.current).map(tab => tab.index));
  let claude;
  try {
    claude = await claudeTargets();
  } catch {
    return null; // the browser can't say, so nothing is left out
  }
  const unsure = [];
  for (const tab of tabs) {
    const theirs = claude.filter(target => target.url === tab.url).length;
    if (!theirs) continue;
    if (theirs >= tabs.filter(other => other.url === tab.url).length) keep.add(tab.index);
    else unsure.push(tab); // the user has the same address open
  }
  if (unsure.length) {
    const list = tabs.map(tab => tab.url).join("\n");
    try {
      if (tabIdsKnown.list !== list) tabIdsKnown = { list, ids: await runCode(tabIdsCode) };
      const ids = new Set(claude.map(target => target.targetId));
      for (const tab of unsure) if (ids.has(tabIdsKnown.ids[tab.index])) keep.add(tab.index);
    } catch {
      for (const tab of unsure) keep.add(tab.index); // better one too many than Claude's own missing
    }
  }
  return keep;
}

// Leaves the user's tabs out of a tab list in an answer: the "Open tabs" section of any
// tool's, and browser_tabs' own.
async function trimTabText(text, isTabsTool) {
  const sections = text.split(/^(?=### )/m);
  for (const [at, section] of sections.entries()) {
    const title = section.slice(0, section.indexOf("\n"));
    if (title !== "### Open tabs" && !(isTabsTool && title === "### Result")) continue;
    const tabs = listedTabs(section);
    if (!tabs.length) continue;
    const keep = await tabsToShow(tabs);
    if (!keep) continue;
    const left = tabs.length - keep.size;
    if (!left) continue;
    const lines = section.split("\n").filter(line => {
      const index = line.match(/^- (\d+): /)?.[1];
      return index === undefined || keep.has(Number(index));
    });
    const note = `- (and ${left} of the user's own tabs, not shown)`;
    const end = lines.findLastIndex(line => /^- \d+: /.test(line));
    lines.splice(end + 1, 0, note);
    sections[at] = lines.join("\n");
  }
  return sections.join("");
}

async function trimTabs(name, args, result) {
  if (!result?.content || (name === "browser_tabs" && args.all)) return result;
  for (const part of result.content) {
    if (part.type === "text" && /^- \d+: /m.test(part.text)) part.text = await trimTabText(part.text, name === "browser_tabs");
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

// Asked of Playwright's page because that is the one Claude is working in, which the port
// can't say. What is run is this and nothing else: the numbers are checked before they get here.
//
// The browser takes the picture by briefly changing the page's viewport, unseen by the page,
// and leaves it changed if the session goes before the picture is done. A page out of
// view never finishes one, so it isn't asked to, and the session always waits for the end.
async function zoom({ region, scale = 1 }) {
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
  const text = await toolText("browser_run_code_unsafe", { code });
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

async function batch({ actions }) {
  const refused = actions?.find(action => notInBatch.has(action.name));
  if (refused) return textResult(`${refused.name} can't be used in a batch.`, true);

  const content = [];
  for (const [index, action] of (actions ?? []).entries()) {
    const result = await runTool(action.name, action.input ?? {});
    content.push({ type: "text", text: `[${index + 1}/${actions.length}] ${action.name}` }, ...(result.content ?? []));
    if (result.isError) return { content, isError: true };
  }
  return { content };
}

// Video, made by the same means as Playwright's, of Claude's page alone.
let video = null; // { file, ended } from start to stop

async function startVideo({ filename, size, fps, cursor }) {
  if (video && !video.ended) return textResult("Video recording has already been started.", true);
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
  await runCode(`async page => {
    await page.screencast.start(${JSON.stringify(options)});
    page.context().exodiumVideo = page;
    ${cursor ? 'await page.screencast.showActions({ cursor: "pointer", duration: 800, style: { title: "display: none" } });' : ""}
    return "";
  }`);
  video = { file, ended: null };
  return textResult("Video recording started. Only Claude's page is recorded.");
}

const stopVideoCode = `async page => {
  const context = page.context();
  const recorded = context.exodiumVideo;
  delete context.exodiumVideo;
  if (recorded) await recorded.screencast.stop().catch(() => {});
  return "";
}`;

// The recording stops early when its connection is about to go.
async function endVideo(when) {
  if (!video || video.ended) return;
  await runCode(stopVideoCode).catch(() => {});
  video.ended = when;
}

async function stopVideo() {
  if (!video) return textResult("No videos were recorded.");
  if (!video.ended) await runCode(stopVideoCode);
  const { file, ended } = video;
  video = null;
  return textResult(`### Result\n- [Video](${file})${ended ? `\n- The recording stopped early, ${ended}.` : ""}`);
}

// Devices. The browser keeps a device's settings for as long as the connection that set
// them stays, so each page has one of its own in Playwright's server, kept there.
const emulated = new Map(); // the settings of each page given a device, by target
let emulating = false; // since a device was first set, browser_resize has to ask

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

// Sets `settings` on Claude's page, or takes its device away for null, and says what the
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

async function emulateDevice(args) {
  let settings;
  try {
    settings = deviceSettings(args);
  } catch (error) {
    return textResult(error.message, true);
  }
  const { id, seen } = await runCode(deviceCode(settings));
  if (settings) {
    emulated.set(id, settings);
    emulating = true;
  } else {
    emulated.delete(id);
  }
  const what = settings
    ? `${args.device}${settings.landscape ? ", sideways" : ""}${settings.cpu > 1 ? `, CPU ${settings.cpu} times slower` : ""}. ` +
      "Reload the page so the site starts over on it."
    : "The window's own size, mouse and CPU again.";
  return textResult(`### Result\n${what}\nThe page now sees: ${JSON.stringify(seen)}`);
}

// While Claude's page has a device, its size is the device's screen.
async function resizeDevice({ width, height }) {
  const resized = await runCode(`async page => {
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
  }`);
  if (!resized) return null;
  emulated.set(resized.id, resized.settings);
  return textResult(`### Result\nThe device's screen is now ${width}×${height}.`);
}

// On a touch device a click at a point is a tap: touches, from which the page makes its own
// click, as a phone's browser does. Turning the mouse into touches, as Chrome's device
// toolbar does, leaves Playwright waiting forever for its mouse events to be done with.
async function tap({ x, y, button, clickCount }) {
  if ((button && button !== "left") || (clickCount && clickCount !== 1)) return null;
  const tapped = await runCode(`async page => {
    const session = page.context().exodiumDevices?.get(page);
    if (!session?.exodiumSettings.touch) return false;
    await session.send("Input.dispatchTouchEvent", { type: "touchStart", touchPoints: [{ x: ${Number(x)}, y: ${Number(y)} }] });
    await session.send("Input.dispatchTouchEvent", { type: "touchEnd", touchPoints: [] });
    return true;
  }`);
  return tapped ? textResult(`### Result\nTapped at (${x}, ${y}).`) : null;
}

// A new connection to Playwright has none of the old one's devices: Claude's page is given
// its own back.
async function restoreDevice() {
  if (!restarted) return;
  restarted = false;
  if (!emulated.size) return;
  const id = await runCode(`async page => {
    const session = await page.context().newCDPSession(page);
    try { return (await session.send("Target.getTargetInfo")).targetInfo.targetId; }
    finally { await session.detach().catch(() => {}); }
  }`).catch(() => null);
  if (emulated.has(id)) await runCode(deviceCode(emulated.get(id))).catch(() => {});
}

// Frames of what the page draws, sent by the browser as it draws them, and picked out at
// even times afterwards. Each is the last drawn by then: a page that draws nothing new
// gives the same frame again.
async function filmstrip({ duration, frames, action, size }) {
  duration = clamp(duration, 100, 20000, 2000);
  frames = Math.round(clamp(frames, 2, 24, 8));
  size = Math.round(clamp(size, 160, 1280, 640));
  if (action && notFilmed.has(action.name)) return textResult(`${action.name} can't be filmed.`, true);

  const started = await runCode(`async page => {
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
  }`);
  if (!started) return textResult("The page isn't in view, so it draws nothing: its window may be minimized or its tab hidden.", true);

  const from = Date.now();
  let acted = null;
  if (action) acted = await runTool(action.name, action.input ?? {});
  await sleep(Math.max(0, from + duration - Date.now()));

  const film = await runCode(`async page => {
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
  }`);
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

function reply(id, result) {
  process.stdout.write(JSON.stringify({ jsonrpc: "2.0", id, result }) + "\n");
}

// Claude's own call, as it came if nothing in it changed, so that it keeps its id.
function forward(message, line, args) {
  const sent = args === message.params.arguments ? line : JSON.stringify({ ...message, params: { ...message.params, arguments: args } });
  return new Promise(resolve => {
    awaited.set(message.id, answer => resolve(answer));
    server.stdin.write(sent + "\n");
  });
}

// Whatever has to happen around a tool call. Gives back the result if that was all of it,
// and nothing if the call is Playwright's to answer.
async function prepareCall(name, args) {
  const tabs = name === "browser_tabs";
  if (name === "browser_close") {
    const filmed = video && !video.ended ? video.file : null;
    await endVideo("when Claude's windows were closed");
    await closeClaudePages();
    return textResult(`Closed Claude's windows in Exodium.${filmed ? ` The video being recorded is in ${filmed}.` : ""}`);
  }
  await ensureWindow();
  await restoreDevice();
  if (name === "browser_zoom") return zoom(args);
  if (name === "browser_batch") return batch(args);
  if (name === "browser_emulate_device") return emulateDevice(args);
  if (name === "browser_filmstrip") return filmstrip(args);
  if (name === "browser_start_video") return startVideo(args);
  if (name === "browser_stop_video") return stopVideo();
  if (name === "browser_resize" && emulating) {
    const resized = await resizeDevice(args);
    if (resized) return resized;
  }
  if (name === "browser_mouse_click_xy" && emulating) {
    const tapped = await tap(args);
    if (tapped) return tapped;
  }
  if (tabs && args.action === "new") {
    // On the start page first, so that Exodium knows the window for one of Claude's.
    const page = await openStartPage();
    if (!page) throw new Error(`Exodium opened no window for ${args.url ?? "a new page"}.`);
    await selectPage(page.id);
    if (args.url) {
      const went = await request("tools/call", { name: "browser_navigate", arguments: { url: args.url } });
      if (went.error || went.result?.isError) return went.result ?? textResult(went.error.message, true);
    }
    return textResult(await toolText("browser_tabs", { action: "list" }));
  }
  if (tabs) await seePopups();
  if (tabs && args.action === "select") {
    const answer = await request("tools/call", { name, arguments: { action: "select", index: args.index } });
    await catchPickersOnlyHere();
    return answer.result ?? textResult(answer.error?.message ?? "No answer.", true);
  }
  if (tabs && args.action === "close") {
    await toolText("browser_tabs", { action: "close", index: args.index });
    await returnToOwnPage();
    return textResult(await toolText("browser_tabs", { action: "list" }));
  }
  return null;
}

// A call of Claude's, from start to finish. `send` gets Playwright's answer to the call as
// it is to go to Playwright, for the calls that are Playwright's.
async function callTool(name, args, send) {
  try {
    let result = await prepareCall(name, args);
    if (!result) {
      const { sent, moves } = await passFiles(name, name === "browser_tabs" ? { action: args.action, index: args.index, url: args.url } : args);
      const answer = await send(sent);
      await sortPickersIfLeft();
      if (answer.error) return { error: answer.error };
      result = answer.result ?? textResult("No answer.", true);
      const said = (result.content ?? []).map(part => part.text ?? "").join("\n");
      if (result.isError && failedToConnect(said)) return { result: textResult(await whyNoConnection(said), true) };
      await finishMoves(moves, result);
      if (name === "browser_console_messages") collapseConsole(result);
    }
    return { result: await trimTabs(name, args, result) };
  } catch (error) {
    return { result: textResult(error.message, true) };
  }
}

// One tool of a batch or a filmstrip, whose answer goes into theirs.
async function runTool(name, args) {
  const { result, error } = await callTool(name, args, async sent => request("tools/call", { name, arguments: sent }));
  return result ?? textResult(error?.message ?? "No answer.", true);
}

async function handleCall(message, line) {
  const { name, arguments: args = {} } = message.params;
  const { result, error } = await callTool(name, args, sent => forward(message, line, sent));
  process.stdout.write(JSON.stringify({ jsonrpc: "2.0", id: message.id, ...(error ? { error } : { result }) }) + "\n");
}

// Playwright's tools as Claude is shown them.
function listTools(list) {
  if (!list.result?.tools) return list;
  for (const tool of list.result.tools) {
    if (toolNotes[tool.name]) tool.description = `${tool.description.replace(/\.?\s*$/, ".")} ${toolNotes[tool.name]}`;
    if (tool.name === "browser_tabs") {
      tool.inputSchema.properties.all = { type: "boolean", description: "For list: the user's own tabs too." };
    }
  }
  list.result.tools.push(...ownTools);
  return list;
}

startServer();

// One tool call at a time: each may have to change what the next is working on.
let turn = Promise.resolve();
const input = createInterface({ input: process.stdin });
input.on("line", line => {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }
  if (message.method === "initialize") initializeParams = message.params;
  if (message.method === "tools/list") {
    awaited.set(message.id, list => process.stdout.write(JSON.stringify(listTools(list)) + "\n"));
  }
  if (message.method === "tools/call") turn = turn.then(() => handleCall(message, line));
  else server.stdin.write(line + "\n");
});
// Claude Code ends a session by closing the wrapper's input, and closing its console sends
// a hangup. Claude's windows go with the session. Exodium closes them as well once this
// process has gone, however it went, but the Exodium running may be one from before that.
let ending = false;
async function endSession() {
  if (ending) return;
  ending = true;
  const tidy = async () => {
    await endVideo("when the session ended");
    await closeClaudePages();
  };
  await Promise.race([tidy(), sleep(5000)]).catch(() => {});
  stopServer();
  process.exit(0);
}
input.on("close", endSession);
process.on("SIGHUP", endSession);
