"""Combine the four ME74 dashboards into one self-contained HTML file.

Each original dashboard runs unchanged inside its own tab (an iframe). A small
shim replaces navigator.serial inside each tab with a "virtual port" that the
main page feeds: the main page owns the one real USB connection, reads the
combined firmware's tagged lines ([GPS] ..., [IMU] ..., [LORA] ..., [ESPNOW] ...),
strips the tag and forwards each line to the matching tab. Commands typed in a
tab go back out through the same connection.

Usage (from the repo root):
  python ME74_combined/tools/build_dashboard.py gps-baseline/dashboard.html \
      gui/imu_dashboard.html gui/radio_dashboard.html <espnow_range_map.html> \
      ME74_combined/ME74_dashboard.html
"""
import json
import re
import sys

SOURCES = {
    "gps":    ("GPS",       sys.argv[1], "#connectBtn"),
    "imu":    ("IMU",       sys.argv[2], "#btnConnect"),
    "lora":   ("LoRa radio", sys.argv[3], "#connectBtn"),
    "espnow": ("ESP-NOW",   sys.argv[4], "#btnConnect"),
}
OUT = sys.argv[5]

SHIM = r"""<script>
/* ME74 combined dashboard: virtual serial port fed by the main page */
(function () {
  const MOD = "__MOD__";
  const enc = new TextEncoder(), dec = new TextDecoder();
  const listeners = {};
  let controller = null, isOpen = false;
  const port = {
    readable: null, writable: null,
    getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; },
    async open() {
      isOpen = true;
      port.readable = new ReadableStream({
        start(c) { controller = c; },
        cancel() { controller = null; }
      });
      port.writable = new WritableStream({
        write(chunk) {
          const text = typeof chunk === "string" ? chunk : dec.decode(chunk);
          parent.postMessage({ me74: 1, kind: "send", mod: MOD, text }, "*");
        }
      });
    },
    async close() {
      isOpen = false;
      try { if (controller) controller.close(); } catch (e) {}
      controller = null;
      port.readable = port.writable = null;
    },
    addEventListener() {}, removeEventListener() {}
  };
  const serial = {
    requestPort: async () => port,
    getPorts: async () => [port],
    addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
    removeEventListener(type, fn) { listeners[type] = (listeners[type] || []).filter((f) => f !== fn); }
  };
  try { Object.defineProperty(navigator, "serial", { value: serial, configurable: true }); } catch (e) {}

  window.addEventListener("message", (e) => {
    const m = e.data;
    if (!m || !m.me74) return;
    if (m.kind === "data") {
      if (controller) { try { controller.enqueue(enc.encode(m.text)); } catch (err) {} }
    } else if (m.kind === "connect") {
      if (!isOpen) { const b = document.querySelector(m.button); if (b) b.click(); }
    } else if (m.kind === "disconnect") {
      if (isOpen) (listeners.disconnect || []).forEach((f) => { try { f({ target: port }); } catch (err) {} });
      try { if (controller) controller.close(); } catch (err) {}
      controller = null;
    } else if (m.kind === "shown") {
      window.dispatchEvent(new Event("resize"));
    }
  });
})();
</script>
"""


def with_shim(html, mod):
    m = re.search(r"<head[^>]*>", html, re.I)
    if not m:
        raise SystemExit(f"no <head> in {mod}")
    return html[: m.end()] + "\n" + SHIM.replace("__MOD__", mod) + html[m.end():]


children = {}
for mod, (label, path, button) in SOURCES.items():
    children[mod] = {"label": label, "button": button, "html": with_shim(open(path, encoding="utf-8").read(), mod)}

# JSON with every "<" escaped so nothing inside can close the <script> tag
DATA = json.dumps(children).replace("<", "\\u003c")

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ME74 Dashboard</title>
<style>
  :root {
    color-scheme: light;
    --bg: #F4F5F7; --bar: #FFFFFF; --text: #1B1F24; --muted: #667085; --line: #E2E5EA;
    --accent: #2563EB; --accent-text: #FFFFFF; --ok: #15803D; --warn: #B45309; --bad: #DC2626;
    --console: #0F1115; --console-text: #D6DAE1;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      color-scheme: dark;
      --bg: #0B0D11; --bar: #14171D; --text: #E6E8EB; --muted: #9AA4B2; --line: #262B35;
      --accent: #4F8CFF; --accent-text: #0B0D12; --ok: #3FB950; --warn: #E3A008; --bad: #F05252;
      --console: #07090C; --console-text: #C9CED6;
    }
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; height: 100%; }
  body { background: var(--bg); color: var(--text); display: flex; flex-direction: column;
         font: 14px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  header { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; padding: 10px 16px;
           background: var(--bar); border-bottom: 1px solid var(--line); }
  h1 { font-size: 17px; margin: 0 6px 0 0; }
  .pill { font-size: 12px; font-weight: 600; padding: 3px 10px; border-radius: 999px;
          border: 1px solid currentColor; color: var(--muted); }
  .pill.ok { color: var(--ok); } .pill.bad { color: var(--bad); }
  .spacer { flex: 1; }
  button { font: inherit; font-weight: 600; padding: 7px 14px; border-radius: 8px; cursor: pointer;
           border: 1px solid var(--line); background: var(--bar); color: var(--text); }
  button.primary { background: var(--accent); border-color: var(--accent); color: var(--accent-text); }
  button:disabled { opacity: .45; cursor: default; }
  #runbar { display: flex; flex-wrap: wrap; align-items: center; gap: 10px; padding: 10px 16px;
            background: var(--bar); border-bottom: 1px solid var(--line); }
  #runbar .go { min-width: 110px; }
  .mods { display: flex; flex-wrap: wrap; gap: 8px; margin-left: 6px; }
  .mod { display: flex; align-items: center; gap: 8px; padding: 5px 6px 5px 12px; border: 1px solid var(--line);
         border-radius: 10px; background: var(--bg); }
  .mod .name { font-weight: 700; cursor: pointer; }
  .mod .state { font-size: 12px; font-weight: 600; color: var(--muted); min-width: 70px; }
  .mod .state.run { color: var(--ok); } .mod .state.warn { color: var(--warn); }
  .mod button { padding: 4px 10px; font-size: 13px; }
  nav { display: flex; gap: 2px; padding: 0 12px; background: var(--bar); border-bottom: 1px solid var(--line);
        overflow-x: auto; }
  nav button { border: 0; border-bottom: 3px solid transparent; border-radius: 0; background: none;
               padding: 10px 14px; color: var(--muted); white-space: nowrap; }
  nav button.active { color: var(--text); border-bottom-color: var(--accent); }
  nav .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 7px;
             background: var(--line); vertical-align: 1px; }
  nav .dot.live { background: var(--ok); }
  main { flex: 1; position: relative; min-height: 0; }
  /* Hidden tabs stay laid out (visibility, not display:none) so their charts keep
     sizing and updating in the background */
  main iframe { position: absolute; inset: 0; width: 100%; height: 100%; border: 0; background: var(--bg);
                visibility: hidden; pointer-events: none; }
  main iframe.active { visibility: visible; pointer-events: auto; }
  #consoleView { position: absolute; inset: 0; display: none; flex-direction: column; padding: 14px 16px; gap: 10px;
                 background: var(--bg); z-index: 1; }
  #consoleView.active { display: flex; }
  .row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
  .row label { display: flex; gap: 5px; align-items: center; color: var(--muted); }
  #cmd { flex: 1; min-width: 200px; font: 13px ui-monospace, Menlo, Consolas, monospace; padding: 8px 10px;
         border-radius: 8px; border: 1px solid var(--line); background: var(--bar); color: var(--text); }
  #log { flex: 1; margin: 0; overflow: auto; padding: 12px; border-radius: 10px; background: var(--console);
         color: var(--console-text); font: 12px/1.45 ui-monospace, Menlo, Consolas, monospace; white-space: pre; }
  .help { color: var(--muted); font-size: 13px; margin: 0; }
  .help code { font: 12px ui-monospace, Menlo, Consolas, monospace; }
</style>
</head>
<body>
<header>
  <h1>ME74 Dashboard</h1>
  <span id="status" class="pill">Not connected</span>
  <span class="spacer"></span>
  <button id="btnConnect" class="primary">Connect board</button>
</header>
<section id="runbar">
  <button id="btnGoAll" class="primary go" disabled title="Start every module">&#9654; Go all</button>
  <button id="btnStopAll" disabled title="Stop every module">&#9632; Stop all</button>
  <div class="mods" id="mods"></div>
</section>
<nav id="tabs"></nav>
<main id="views">
  <section id="consoleView">
    <p class="help"><b>Go all</b> runs every module at once. To test one thing by itself, press <b>Only</b> on it
      (the others stop), or use each module's Start/Stop. One USB connection feeds every tab. Firmware: <code>ME74_combined</code>. Each tab is the original
      dashboard; its own Connect button is pressed for you when the board connects. Commands typed below go straight to
      the board (try <code>help</code> or <code>info</code>).</p>
    <div class="row">
      <input id="cmd" placeholder="Command, e.g. info, status, origin, c 3.5" autocomplete="off">
      <button id="btnSend">Send</button>
      <button data-cmd="info">info</button>
      <button data-cmd="help">help</button>
    </div>
    <div class="row" id="filters"><span style="color:var(--muted)">Show:</span></div>
    <pre id="log"></pre>
  </section>
</main>

<script>
const CHILDREN = __DATA__;
const TAGS = { GPS: "gps", IMU: "imu", LORA: "lora", ESPNOW: "espnow" };
const $ = (id) => document.getElementById(id);

let port = null, reader = null, writer = null, pipeDone = null;
let writeChain = Promise.resolve();
const lastSeen = {};
const frames = {};
const show = { SYS: true, GPS: true, IMU: true, LORA: true, ESPNOW: true, other: true };

// ---------- Tabs ----------
function addTab(id, label) {
  const b = document.createElement("button");
  b.innerHTML = `<span class="dot" id="dot-${id}"></span>${label}`;
  b.onclick = () => selectTab(id);
  b.id = "tab-" + id;
  $("tabs").appendChild(b);
}
for (const [mod, c] of Object.entries(CHILDREN)) {
  addTab(mod, c.label);
  const f = document.createElement("iframe");
  f.title = c.label + " dashboard";
  f.srcdoc = c.html;
  $("views").appendChild(f);
  frames[mod] = f;
}
addTab("console", "Console");
$("dot-console").remove();

function selectTab(id) {
  for (const b of $("tabs").children) b.classList.toggle("active", b.id === "tab-" + id);
  for (const [mod, f] of Object.entries(frames)) f.classList.toggle("active", mod === id);
  $("consoleView").classList.toggle("active", id === "console");
  if (frames[id]) frames[id].contentWindow.postMessage({ me74: 1, kind: "shown" }, "*");
}
selectTab("console");

// ---------- Run controls (go / stop / only) ----------
const modState = {};   // mod -> {run, hw}
for (const [mod, c] of Object.entries(CHILDREN)) {
  const d = document.createElement("div");
  d.className = "mod";
  d.innerHTML = `<span class="name" title="Open tab">${c.label}</span><span class="state" id="st-${mod}">--</span>` +
                `<button id="tg-${mod}" disabled>Start</button>` +
                `<button id="only-${mod}" disabled title="Run only this module">Only</button>`;
  d.querySelector(".name").onclick = () => selectTab(mod);
  $("mods").appendChild(d);
}
function runCmd(cmd) { send(cmd); consoleLine("> " + cmd, "SYS"); }
for (const mod of Object.keys(CHILDREN)) {
  $("tg-" + mod).onclick = () => runCmd((modState[mod] && modState[mod].run ? "stop " : "go ") + mod);
  $("only-" + mod).onclick = () => { runCmd("only " + mod); selectTab(mod); };
}
$("btnGoAll").onclick = () => runCmd("go");
$("btnStopAll").onclick = () => runCmd("stop");

function onState(body) {
  // STATE lora=1 espnow=1 imu=1 gps=1 lora_hw=1 imu_hw=0 gps_hw=1
  const kv = Object.fromEntries(body.split(/\s+/).slice(1).map((p) => p.split("=")));
  for (const mod of Object.keys(CHILDREN)) {
    if (!(mod in kv)) continue;
    const hw = (mod + "_hw") in kv ? kv[mod + "_hw"] === "1" : true;
    modState[mod] = { run: kv[mod] === "1", hw };
  }
  renderRun();
}
function renderRun() {
  const on = !!port;
  $("btnGoAll").disabled = $("btnStopAll").disabled = !on;
  for (const mod of Object.keys(CHILDREN)) {
    const st = modState[mod], el = $("st-" + mod);
    $("tg-" + mod).disabled = $("only-" + mod).disabled = !on;
    $("tg-" + mod).textContent = st && st.run ? "Stop" : "Start";
    if (!on || !st) { el.textContent = "--"; el.className = "state"; continue; }
    if (st.run && !st.hw) { el.textContent = "No sensor"; el.className = "state warn"; }
    else if (st.run) { el.textContent = "Running"; el.className = "state run"; }
    else { el.textContent = "Stopped"; el.className = "state"; }
  }
}

// ---------- Console ----------
for (const k of Object.keys(show)) {
  const l = document.createElement("label");
  l.innerHTML = `<input type="checkbox" checked> ${k === "other" ? "untagged" : k}`;
  l.querySelector("input").onchange = (e) => { show[k] = e.target.checked; };
  $("filters").appendChild(l);
}
const logLines = [];
let logDirty = false;
function consoleLine(text, tag) {
  if (!show[tag in show ? tag : "other"]) return;
  logLines.push(text);
  if (logLines.length > 600) logLines.splice(0, logLines.length - 600);
  logDirty = true;
}
setInterval(() => {
  if (!logDirty) return;
  logDirty = false;
  const el = $("log");
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 30;
  el.textContent = logLines.join("\n");
  if (atBottom) el.scrollTop = el.scrollHeight;
}, 200);

// ---------- Serial ----------
function setStatus(text, cls) { $("status").textContent = text; $("status").className = "pill " + (cls || ""); }

async function connect() {
  if (!("serial" in navigator)) {
    setStatus("Use Chrome or Edge (Web Serial needed)", "bad");
    return;
  }
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
  } catch (e) {
    port = null;
    if (e.name !== "NotFoundError") {
      setStatus("Could not open the port", "bad");
      consoleLine("Could not open the port: " + e.message + " (close the Serial Monitor or any other program using it)", "SYS");
    }
    return;
  }
  writer = port.writable.getWriter();
  setStatus("Connected", "ok");
  $("btnConnect").textContent = "Disconnect";
  $("btnConnect").className = "";
  readLoop();
  for (const [mod, c] of Object.entries(CHILDREN)) {
    frames[mod].contentWindow.postMessage({ me74: 1, kind: "connect", button: c.button }, "*");
  }
  setTimeout(() => { send("info\n"); send("state\n"); }, 400);
  renderRun();
}

async function disconnect(reason) {
  for (const f of Object.values(frames)) f.contentWindow.postMessage({ me74: 1, kind: "disconnect" }, "*");
  try { if (reader) await reader.cancel(); } catch (e) {}
  try { if (pipeDone) await pipeDone; } catch (e) {}
  try { if (writer) writer.releaseLock(); } catch (e) {}
  try { if (port) await port.close(); } catch (e) {}
  port = reader = writer = pipeDone = null;
  setStatus(reason || "Not connected", reason ? "bad" : "");
  $("btnConnect").textContent = "Connect board";
  $("btnConnect").className = "primary";
  renderRun();
}

async function readLoop() {
  const decoder = new TextDecoderStream();
  pipeDone = port.readable.pipeTo(decoder.writable).catch(() => {});
  reader = decoder.readable.getReader();
  let buf = "";
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += value;
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, i).replace(/\r$/, "");
        buf = buf.slice(i + 1);
        if (line.trim()) routeLine(line);
      }
    }
  } catch (e) {
    if (port) disconnect("Board disconnected");
  } finally {
    try { reader.releaseLock(); } catch (e) {}
  }
}

function routeLine(line) {
  const m = line.match(/^\[(SYS|GPS|IMU|LORA|ESPNOW)\] ?(.*)$/);
  if (!m) { consoleLine(line, "other"); return; }
  const tag = m[1], body = m[2];
  if (tag === "SYS" && body.startsWith("STATE ")) { onState(body); return; }
  // The IMU's 50 Hz logging lines would flood the console; show them only in the IMU tab
  if (!(tag === "IMU" && body.startsWith("LOG,"))) consoleLine(line, tag);
  const mod = TAGS[tag];
  if (!mod) return;
  lastSeen[mod] = performance.now();
  frames[mod].contentWindow.postMessage({ me74: 1, kind: "data", text: body + "\n" }, "*");
}

function send(text) {
  if (!writer) return;
  if (!text.endsWith("\n")) text += "\n";   // the IMU tab sends single characters
  const data = new TextEncoder().encode(text);
  writeChain = writeChain.then(() => writer && writer.write(data)).catch(() => {});
}

window.addEventListener("message", (e) => {
  const m = e.data;
  if (!m || !m.me74 || m.kind !== "send") return;
  send(m.text);
});

navigator.serial?.addEventListener("disconnect", (e) => {
  if (port && e.target === port) disconnect("Board unplugged");
});

setInterval(() => {
  const now = performance.now();
  for (const mod of Object.keys(CHILDREN)) {
    $("dot-" + mod).classList.toggle("live", !!port && lastSeen[mod] && now - lastSeen[mod] < 3000);
  }
}, 500);

$("btnConnect").onclick = () => (port ? disconnect() : connect());
$("btnSend").onclick = () => { const v = $("cmd").value.trim(); if (v) { send(v); consoleLine("> " + v, "SYS"); $("cmd").value = ""; } };
$("cmd").addEventListener("keydown", (e) => { if (e.key === "Enter") $("btnSend").click(); });
for (const b of document.querySelectorAll("button[data-cmd]")) {
  b.onclick = () => { send(b.dataset.cmd); consoleLine("> " + b.dataset.cmd, "SYS"); };
}
if (!("serial" in navigator)) setStatus("Open in Chrome or Edge to connect", "bad");
</script>
</body>
</html>
"""

open(OUT, "w", encoding="utf-8").write(PAGE.replace("__DATA__", DATA))
print("wrote", OUT, len(PAGE) + len(DATA), "bytes")
