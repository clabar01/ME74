"""Combine the four ME74 dashboards into one self-contained HTML file.

Each original dashboard runs unchanged inside its own tab (an iframe). A small
shim replaces navigator.serial inside each tab with a "virtual port" that the
main page feeds: the main page owns the one real USB connection, reads the
combined firmware's tagged lines ([GPS] ..., [IMU] ..., [LORA] ..., [ESPNOW] ...),
strips the tag and forwards each line to the matching tab. Commands typed in a
tab go back out through the same connection.

Usage (from the repo root):
  Everything (ME74_combined):
    python ME74_combined/tools/build_dashboard.py ME74_combined/ME74_dashboard.html "ME74 Dashboard" ME74_combined \
        gps=gps-baseline/dashboard.html imu=gui/imu_dashboard.html \
        lora=gui/radio_dashboard.html espnow=<espnow_range_map.html>
  Radios only (ME74_radios):
    python ME74_combined/tools/build_dashboard.py ME74_radios/ME74_radio_dashboard.html "ME74 Radio Dashboard" \
        ME74_radios lora=gui/radio_dashboard.html espnow=<espnow_range_map.html>
"""
import json
import re
import sys

# Usage: build_dashboard.py OUT.html "Title" FIRMWARE gps=PATH imu=PATH lora=PATH espnow=PATH
# Any subset of modules works (e.g. just lora= and espnow= for the radios-only dashboard).
KNOWN = {
    "gps":    ("GPS",        "#connectBtn"),
    "imu":    ("IMU",        "#btnConnect"),
    "imu2":   ("IMU 2",      "#btnConnect"),
    "lora":   ("LoRa radio", "#connectBtn"),
    "espnow": ("ESP-NOW",    "#btnConnect"),
}
OUT, TITLE, FIRMWARE = sys.argv[1], sys.argv[2], sys.argv[3]
paths = dict(a.split("=", 1) for a in sys.argv[4:])
SOURCES = {m: (KNOWN[m][0], paths[m], KNOWN[m][1]) for m in KNOWN if m in paths}

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
<title>__TITLE__</title>
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
  #runbar .files { margin-left: auto; display: flex; gap: 8px; align-items: center; }
  #runbar .files .count { color: var(--muted); font-size: 12px; }
  nav #saveTab { margin-left: auto; align-self: center; border: 1px solid var(--line); border-radius: 8px;
                 padding: 5px 12px; color: var(--text); font-size: 13px; white-space: nowrap; }
  #homeView { position: absolute; inset: 0; display: none; overflow: auto; padding: 16px; background: var(--bg); z-index: 1; }
  #homeView.active { display: block; }
  .home-grid { display: grid; grid-template-columns: repeat(__NCARDS__, minmax(0, 1fr)); gap: 14px; max-width: 1400px; margin: 0 auto; }
  @media (max-width: 1100px) { .home-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
  @media (max-width: 600px) { .home-grid { grid-template-columns: 1fr; } }
  .card { background: var(--bar); border: 1px solid var(--line); border-radius: 12px; padding: 14px 16px; min-width: 0; }
  .card h2 { margin: 0; font-size: 13px; text-transform: uppercase; letter-spacing: .06em; color: var(--muted);
             display: flex; align-items: center; gap: 8px; }
  .card h2 .swatch { width: 10px; height: 10px; border-radius: 3px; }
  .card .big { font-size: 40px; font-weight: 800; line-height: 1.1; margin: 10px 0 2px; font-variant-numeric: tabular-nums; }
  .card .big small { font-size: 18px; color: var(--muted); font-weight: 600; margin-left: 3px; }
  .card .method { color: var(--muted); font-size: 12px; margin-bottom: 8px; }
  .card .detail { font-size: 13px; font-variant-numeric: tabular-nums; min-height: 2.8em; }
  .card .hint { color: var(--warn); font-size: 12px; font-weight: 600; margin-top: 4px; min-height: 1.4em; }
  .card .foot { display: flex; justify-content: space-between; align-items: center; margin-top: 10px;
                padding-top: 8px; border-top: 1px solid var(--line); font-size: 12px; color: var(--muted); }
  .card .foot a { color: var(--accent); cursor: pointer; text-decoration: none; font-weight: 600; }
  .card label.cal { display: inline-flex; gap: 4px; align-items: center; font-size: 12px; color: var(--muted); margin-right: 8px; }
  .card input.small { width: 58px; font: inherit; padding: 2px 5px; border-radius: 6px; border: 1px solid var(--line);
                      background: var(--bg); color: var(--text); }
  .chart-card { grid-column: 1 / -1; }
  .home-top { grid-column: 1 / -1; display: flex; flex-wrap: wrap; align-items: center; gap: 12px;
              background: var(--bar); border: 1px solid var(--line); border-radius: 12px; padding: 12px 16px; }
  .home-top label { display: flex; align-items: center; gap: 8px; font-weight: 600; }
  .home-top input { font: inherit; width: 260px; padding: 7px 10px; border-radius: 8px; border: 1px solid var(--line);
                    background: var(--bg); color: var(--text); }
  .home-top .help { flex: 1; min-width: 220px; }
  .card .zero-row { display: flex; align-items: center; justify-content: space-between; gap: 8px; margin-top: 6px;
                    font-size: 13px; font-variant-numeric: tabular-nums; }
  .card .zero-row button { padding: 4px 12px; font-size: 13px; }
  .chart-head { display: flex; flex-wrap: wrap; align-items: center; gap: 14px; margin-bottom: 8px; }
  .chart-head .legend { display: flex; flex-wrap: wrap; gap: 14px; font-size: 13px; }
  .chart-head .legend i { display: inline-block; width: 14px; height: 3px; border-radius: 2px; margin-right: 6px; vertical-align: middle; }
  .chart-head label { margin-left: auto; display: flex; gap: 6px; align-items: center; font-size: 13px; color: var(--muted); }
  #homeChart { width: 100%; height: 300px; display: block; }
  .help code { font: 12px ui-monospace, Menlo, Consolas, monospace; }
</style>
</head>
<body>
<header>
  <h1>__TITLE__</h1>
  <span id="status" class="pill">Not connected</span>
  <span class="spacer"></span>
  <button id="btnConnect" class="primary">Connect board</button>
</header>
<section id="runbar">
  <button id="btnGoAll" class="primary go" disabled title="Start every module">&#9654; Go all</button>
  <button id="btnStopAll" disabled title="Stop every module">&#9632; Stop all</button>
  <div class="mods" id="mods"></div>
  <div class="files">
    <span class="count" id="recCount">No data recorded yet</span>
    <button id="btnSaveAll" disabled title="Every module's CSV plus the overview, in one ZIP file">&#10515; Save all CSV</button>
    <button id="btnClearData" disabled title="Forget all recorded data">Clear data</button>
  </div>
</section>
<nav id="tabs"></nav>
<main id="views">
  <section id="homeView">
    <div class="home-grid">
      <div class="home-top">
        <label for="runLabel">Test name</label>
        <input id="runLabel" placeholder="e.g. walking 0-10m" autocomplete="off">
        <span class="help">Goes in every saved file name and in a <code>test</code> column, so each run is labeled.</span>
        <button id="btnZeroAll" class="primary" disabled title="Set every distance to zero right now">Zero all</button>
      </div>
__CARDS__      <div class="card chart-card">
        <div class="chart-head">
          <h2 style="margin:0;font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted)">Distance over the last 5 minutes</h2>
          <div class="legend">
            __LEGEND__
          </div>
          <label>Actual distance (tape), m <input class="small" id="tape" type="number" step="0.01" min="0" placeholder="--"></label>
        </div>
        <canvas id="homeChart"></canvas>
        <p class="help" style="margin-top:8px">The overview CSV saves one row per second with every technology's
          distance side by side, the distances since zero, and the actual distance you type here, so you can compare them.</p>
      </div>
    </div>
  </section>
  <section id="consoleView">
    <p class="help"><b>Go all</b> runs every module at once. To test one thing by itself, press <b>Only</b> on it
      (the others stop), or use each module's Start/Stop. One USB connection feeds every tab. Firmware: <code>__FIRMWARE__</code>. Each tab is the original
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
const TAGS = { GPS: "gps", IMU: "imu", IMU2: "imu2", LORA: "lora", ESPNOW: "espnow" };
const $ = (id) => document.getElementById(id);

let port = null, reader = null, writer = null, pipeDone = null;
let writeChain = Promise.resolve();
const lastSeen = {};
const frames = {};
const show = { SYS: true, GPS: true, IMU: true, IMU2: true, LORA: true, ESPNOW: true, other: true };

// ---------- Tabs ----------
function addTab(id, label) {
  const b = document.createElement("button");
  b.innerHTML = `<span class="dot" id="dot-${id}"></span>${label}`;
  b.onclick = () => selectTab(id);
  b.id = "tab-" + id;
  $("tabs").appendChild(b);
}
addTab("home", "Home");
$("dot-home").remove();
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
const saveTabBtn = document.createElement("button");
saveTabBtn.id = "saveTab";
$("tabs").appendChild(saveTabBtn);
let currentTab = "home";

function selectTab(id) {
  for (const b of $("tabs").children) b.classList.toggle("active", b.id === "tab-" + id);
  for (const [mod, f] of Object.entries(frames)) f.classList.toggle("active", mod === id);
  $("consoleView").classList.toggle("active", id === "console");
  $("homeView").classList.toggle("active", id === "home");
  if (frames[id]) frames[id].contentWindow.postMessage({ me74: 1, kind: "shown" }, "*");
  currentTab = id;
  updateSaveButtons();
  if (id === "home") drawHome();
}

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

// ---------- Recording: every module's data, kept for the CSV files ----------
const REC = {
  home:   { label: "Overview", file: "overview",
            cols: ["pc_time", "elapsed_s", "test", "tape_m", "gps_dist_m", "gps_state", "gps_sats", "gps_hdop",
                   "lora_dist_m", "lora_rssi_dbm", "lora_snr_db", "espnow_dist_m", "espnow_rssi_dbm",
                   "espnow_loss_pct", "imu_accel_total", "imu_motion_accel", "imu_horizontal", "imu_vertical"], rows: [] },
  gps:    { label: "GPS", file: "gps",
            cols: ["pc_time", "elapsed_s", "test", "type", "state", "sats", "hdop", "lat", "lon", "speed_mps", "utc",
                   "origin_lat", "origin_lon", "dist_m", "true_m", "gps_m", "error_m", "samples", "tape_m"], rows: [] },
  imu:    { label: "IMU", file: "imu",
            cols: ["pc_time", "elapsed_s", "test", "t_us", "ax", "ay", "az", "gx", "gy", "gz", "mx", "my", "mz",
                   "roll_deg", "pitch_deg", "heading_deg", "dt_ms", "overruns",
                   "accel_total", "motion_accel", "horizontal", "vertical",
                   "traveled_m", "from_zero_m", "speed_mps", "tape_m"], rows: [] },
  lora:   { label: "LoRa", file: "lora",
            cols: ["pc_time", "elapsed_s", "test", "type", "from_node", "rssi_dbm", "snr_db", "firmware_dist_m",
                   "ref_dist_m", "ref_rssi_dbm", "tape_m"], rows: [] },
  espnow: { label: "ESP-NOW", file: "espnow",
            cols: ["pc_time", "elapsed_s", "test", "board_ms", "status", "rssi_raw_dbm", "rssi_smoothed_dbm",
                   "peer_hears_me_dbm", "packet_loss_pct", "packets_rx", "packets_missed", "est_distance_m",
                   "cal_rssi_1m_dbm", "cal_env_factor", "tape_m"], rows: [] },
  console: { label: "Console", file: "console_log", cols: ["pc_time", "line"], rows: [] },
};
REC.imu2 = { label: "IMU 2", file: "imu2", cols: [...REC.imu.cols], rows: [] };
for (const m of ["gps", "imu", "imu2", "lora", "espnow"]) if (!(m in CHILDREN)) delete REC[m];
const HAS = (m) => m in CHILDREN;
let recT0 = null;
const live = { gps: {}, lora: {}, espnow: {}, imu: {}, imu2: {} };
const loraDev = { refDist: null, refRssi: null, calibrated: false };
let loraPending = null;
const DEG = 180 / Math.PI, G0 = 9.80665;

function stamp() {
  const now = new Date();
  if (!recT0) recT0 = now;
  return [now.toISOString(), ((now - recT0) / 1000).toFixed(3), runName()];
}
const runName = () => $("runLabel").value.trim();
const runSlug = () => runName().replace(/\s+/g, "_").replace(/[^A-Za-z0-9._-]/g, "").slice(0, 60);
const labelPart = () => (runSlug() ? "_" + runSlug() : "");

// ---------- Zero points ----------
// GPS: position at zero. LoRa / ESP-NOW: distance at zero. IMU: restart dead reckoning.
const zero = { gps: null, lora: null, espnow: null };
const gpsFixes = [];   // recent {t, lat, lon} for averaging the zero point
const events = [];     // written into the next overview row
function haversine(lat1, lon1, lat2, lon2) {
  const R = 6371008.8, r = Math.PI / 180;
  const dp = (lat2 - lat1) * r, dl = (lon2 - lon1) * r;
  const a = Math.sin(dp / 2) ** 2 + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin(dl / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(a));
}

// ---------- IMU dead reckoning ----------
// Integrates horizontal acceleration (gravity removed, in world axes) twice. Whenever the
// sensor is still for a moment, velocity is reset to zero, which stops most of the drift.
const makeNav = () => ({ vx: 0, vy: 0, px: 0, py: 0, traveled: 0, speed: 0, still: 0, bx: 0, by: 0, lastT: null, recent: [], active: false });
const NAVS = { imu: makeNav(), imu2: makeNav() };
const IMUS = ["imu", "imu2"];
function navUpdate(nav, wx, wy, lin, gyroMag, tSec) {
  nav.recent.push([wx, wy]);
  if (nav.recent.length > 100) nav.recent.shift();
  if (nav.lastT === null) { nav.lastT = tSec; return; }
  const dt = tSec - nav.lastT;
  nav.lastT = tSec;
  if (!(dt > 0 && dt < 0.25) || !nav.active) return;
  const still = lin < 0.3 && (gyroMag == null || gyroMag < 0.2);
  nav.still = still ? nav.still + dt : 0;
  let ax = wx - nav.bx, ay = wy - nav.by;
  if (Math.hypot(ax, ay) < 0.08) { ax = 0; ay = 0; }        // ignore sensor noise
  if (nav.still > 0.25) { nav.vx = 0; nav.vy = 0; }         // standing still: no velocity
  else { nav.vx += ax * dt; nav.vy += ay * dt; }
  nav.px += nav.vx * dt; nav.py += nav.vy * dt;
  nav.speed = Math.hypot(nav.vx, nav.vy);
  nav.traveled += nav.speed * dt;
}
function navZero(nav) {
  const r = nav.recent.slice(-50);   // last ~1 s: taken as the sensor's offset (hold still when zeroing)
  nav.bx = r.length ? r.reduce((a, x) => a + x[0], 0) / r.length : 0;
  nav.by = r.length ? r.reduce((a, x) => a + x[1], 0) / r.length : 0;
  Object.assign(nav, { vx: 0, vy: 0, px: 0, py: 0, traveled: 0, speed: 0, still: 0, active: true });
}

function zeroModule(mod) {
  const msg = (t) => consoleLine("[HOME] " + t, "SYS");
  if (mod === "gps") {
    const recent = gpsFixes.filter((f) => f.t > performance.now() - 3000);
    if (!recent.length) { msg("GPS zero skipped: no fix yet"); return false; }
    zero.gps = { lat: recent.reduce((a, f) => a + f.lat, 0) / recent.length, lon: recent.reduce((a, f) => a + f.lon, 0) / recent.length };
  } else if (mod === "lora" || mod === "espnow") {
    const d = live[mod].dist;
    if (d == null || !fresh(mod, mod === "lora" ? 8000 : 3000)) { msg(`${mod} zero skipped: no distance yet`); return false; }
    zero[mod] = d;
  } else if (IMUS.includes(mod)) {
    navZero(NAVS[mod]);
  }
  events.push("zero_" + mod);
  msg("Zeroed " + mod);
  return true;
}
function sinceZero(mod) {
  if (mod === "gps") {
    const g = live.gps;
    return zero.gps && fresh("gps") && g.lat != null ? haversine(zero.gps.lat, zero.gps.lon, g.lat, g.lon) : null;
  }
  if (IMUS.includes(mod)) return NAVS[mod].active && fresh(mod) ? NAVS[mod].traveled : null;
  const d = live[mod].dist;
  return zero[mod] != null && d != null && fresh(mod, mod === "lora" ? 8000 : 3000) ? d - zero[mod] : null;
}
const num = (v) => { const x = parseFloat(v); return isFinite(x) ? x : null; };
const tapeVal = () => num($("tape").value);
function push(mod, row) { REC[mod].rows.push([...stamp(), ...row]); }

function espnowDist(rssi) {
  const ref = num($("enRef").value) ?? -45, n = num($("enN").value) ?? 2.5;
  return Math.pow(10, (ref - rssi) / (10 * n));
}

// Sensor -> world rotation (same convention as the firmware), for gravity removal
function motionAccel(ax, ay, az, roll, pitch, heading) {
  const f = roll / DEG, th = pitch / DEG, ps = -heading / DEG;
  const cf = Math.cos(f), sf = Math.sin(f), ct = Math.cos(th), st = Math.sin(th), cp = Math.cos(ps), sp = Math.sin(ps);
  const wx = cp * ct * ax + (cp * st * sf - sp * cf) * ay + (cp * st * cf + sp * sf) * az;
  const wy = sp * ct * ax + (sp * st * sf + cp * cf) * ay + (sp * st * cf - cp * sf) * az;
  const wz = -st * ax + ct * sf * ay + ct * cf * az - G0;
  return { total: Math.hypot(ax, ay, az), lin: Math.hypot(wx, wy, wz), hor: Math.hypot(wx, wy), ver: wz, wx, wy };
}

function recordLine(mod, body) {
  const now = performance.now();
  if (mod === "gps") {
    let m; try { m = JSON.parse(body); } catch (e) { return; }
    if (m.type === "status") {
      live.gps = { t: now, dist: m.dist, state: m.state, sats: m.sats, hdop: m.hdop, origin: m.origin_lat != null,
                   lat: m.state === "fix" ? m.lat : null, lon: m.state === "fix" ? m.lon : null };
      if (m.state === "fix" && m.lat != null) {
        gpsFixes.push({ t: now, lat: m.lat, lon: m.lon });
        while (gpsFixes.length && gpsFixes[0].t < now - 10000) gpsFixes.shift();
      }
      push("gps", ["status", m.state, m.sats, m.hdop, m.lat, m.lon, m.speed, m.utc, m.origin_lat, m.origin_lon, m.dist,
                   "", "", "", "", tapeVal()]);
    } else if (m.type === "result") {
      push("gps", ["result_" + m.mode + (m.ok ? "" : "_failed"), "", "", "", m.lat, m.lon, "", "", "", "", "",
                   m.true_m, m.gps_m, m.error_m, m.samples, tapeVal()]);
    }
  } else if (IMUS.includes(mod)) {
    const nav = NAVS[mod];
    let ax, ay, az, roll, pitch, heading, rest, gyroMag = null, tSec = now / 1000;
    if (body.startsWith("LOG,")) {
      const p = body.split(",");
      if (p.length !== 16) return;
      const n = p.slice(1).map(Number);
      if (n.some(Number.isNaN)) return;
      [ax, ay, az] = [n[1], n[2], n[3]]; [roll, pitch, heading] = [n[10], n[11], n[12]];
      gyroMag = Math.hypot(n[4], n[5], n[6]);
      tSec = n[0] / 1e6;
      rest = p.slice(1);
    } else if (body.startsWith("Ax:")) {
      const kv = Object.fromEntries(body.split(",").map((x) => x.split(":")));
      [ax, ay, az, roll, pitch, heading] = ["Ax", "Ay", "Az", "Roll", "Pitch", "Heading"].map((k) => num(kv[k]));
      if ([ax, ay, az, roll, pitch, heading].some((v) => v === null)) return;
      rest = ["", ax, ay, az, "", "", "", "", "", "", roll, pitch, heading, num(kv.dt_ms), num(kv.Overruns)];
    } else return;
    const a = motionAccel(ax, ay, az, roll, pitch, heading);
    navUpdate(nav, a.wx, a.wy, a.lin, gyroMag, tSec);
    live[mod] = { t: now, ...a, roll, pitch, heading };
    push(mod, [...rest, a.total.toFixed(4), a.lin.toFixed(4), a.hor.toFixed(4), a.ver.toFixed(4),
                 nav.active ? nav.traveled.toFixed(3) : "", nav.active ? Math.hypot(nav.px, nav.py).toFixed(3) : "",
                 nav.active ? nav.speed.toFixed(3) : "", tapeVal()]);
  } else if (mod === "lora") {
    let m;
    const r = (s) => new RegExp("^" + s.replace(/#/g, "(-?\\d+(?:\\.\\d+)?)"));
    if ((m = body.match(r("RX from node #")))) { loraPending = { from: +m[1] }; return; }
    if (loraPending && (m = body.match(r("RSSI: # dBm")))) { loraPending.rssi = +m[1]; return; }
    if (loraPending && (m = body.match(r("SNR: # dB")))) { loraPending.snr = +m[1]; return; }
    const finish = (dist) => {
      const p = loraPending; loraPending = null;
      if (p.rssi == null) return;
      live.lora = { t: now, rssi: p.rssi, snr: p.snr, dist };
      push("lora", ["packet", p.from, p.rssi, p.snr, dist, loraDev.refDist, loraDev.refRssi, tapeVal()]);
    };
    if (loraPending && (m = body.match(r("Distance: # m")))) { finish(+m[1]); return; }
    if (loraPending && body === "Not calibrated.") { finish(null); return; }
    if ((m = body.match(r("Known distance: # m")))) { loraDev.refDist = +m[1]; loraDev.calibrated = false; return; }
    if ((m = body.match(r("Calibration: #/# RSSI=#")))) {
      live.lora = { ...live.lora, t: now, rssi: +m[3], calibrating: `${m[1]}/${m[2]}` };
      push("lora", ["calibration", "", +m[3], "", "", loraDev.refDist, "", tapeVal()]);
      return;
    }
    if (body === "CALIBRATION COMPLETE") { loraDev.calibrated = true; live.lora.calibrating = null; return; }
    if ((m = body.match(r("Reference RSSI: #")))) { loraDev.refRssi = +m[1]; return; }
    if ((m = body.match(r("Reference distance: #")))) { loraDev.refDist = +m[1]; return; }
    if ((m = body.match(/^Calibrated: (YES|NO)/))) { loraDev.calibrated = m[1] === "YES"; return; }
  } else if (mod === "espnow") {
    if (!body.startsWith("DATA,")) return;
    const f = body.split(",");
    if (f.length < 9) return;
    const smoothed = num(f[4]);
    const dist = smoothed === null ? null : espnowDist(smoothed);
    live.espnow = { t: now, status: f[2], rssi: smoothed, peer: num(f[5]), loss: num(f[6]), dist };
    push("espnow", [num(f[1]), f[2], num(f[3]), smoothed, num(f[5]), num(f[6]), num(f[7]), num(f[8]),
                    dist === null ? null : dist.toFixed(3), num($("enRef").value), num($("enN").value), tapeVal()]);
  }
}

// Overview: one row per second with every technology side by side
const history = [];   // {t, gps, lora, espnow}
function fresh(mod, ms = 3000) { return live[mod].t && performance.now() - live[mod].t < ms; }
const f3 = (v) => (v == null ? null : (+v).toFixed(3));
const OV = [
  ["gps_from_zero_m", "gps", () => f3(sinceZero("gps"))],
  ["gps_dist_m", "gps", (s) => s.g.dist], ["gps_state", "gps", (s) => s.g.state],
  ["gps_sats", "gps", (s) => s.g.sats], ["gps_hdop", "gps", (s) => s.g.hdop],
  ["lora_dist_m", "lora", (s) => s.l.dist], ["lora_since_zero_m", "lora", () => f3(sinceZero("lora"))],
  ["lora_rssi_dbm", "lora", (s) => s.l.rssi], ["lora_snr_db", "lora", (s) => s.l.snr],
  ["espnow_dist_m", "espnow", (s) => s.e.dist == null ? null : s.e.dist.toFixed(3)],
  ["espnow_since_zero_m", "espnow", () => f3(sinceZero("espnow"))],
  ["espnow_rssi_dbm", "espnow", (s) => s.e.rssi], ["espnow_loss_pct", "espnow", (s) => s.e.loss],
  ...IMUS.flatMap((k) => [
    [`${k}_traveled_m`, k, () => f3(sinceZero(k))],
    [`${k}_from_zero_m`, k, () => (NAVS[k].active && fresh(k) ? f3(Math.hypot(NAVS[k].px, NAVS[k].py)) : null)],
    [`${k}_speed_mps`, k, () => (NAVS[k].active && fresh(k) ? f3(NAVS[k].speed) : null)],
  ]),
].filter((c) => HAS(c[1]));
REC.home.cols = ["pc_time", "elapsed_s", "test", "tape_m", "event", ...OV.map((c) => c[0])];
setInterval(() => {
  if (!port) return;
  const snap = {
    g: fresh("gps") ? live.gps : {}, l: fresh("lora", 8000) ? live.lora : {},
    e: fresh("espnow") && live.espnow.status === "OK" ? live.espnow : {}, i: fresh("imu") ? live.imu : {},
  };
  push("home", [tapeVal(), events.splice(0).join(";"), ...OV.map((c) => c[2](snap) ?? "")]);
  history.push({ t: Date.now(), gps: gpsShown(), lora: snap.l.dist ?? null, espnow: snap.e.dist ?? null,
                 imu: sinceZero("imu"), imu2: sinceZero("imu2"), tape: tapeVal() });
  while (history.length && history[0].t < Date.now() - 300000) history.shift();
}, 1000);

// ---------- Home page ----------
const fmtM = (v) => (v == null ? "--" : v < 10 ? v.toFixed(2) : v.toFixed(1));
function ago(mod) {
  if (!live[mod].t) return "no data";
  const s = (performance.now() - live[mod].t) / 1000;
  return s < 1.5 ? "updated just now" : `updated ${s.toFixed(0)} s ago`;
}
function stateText(mod) {
  const st = modState[mod];
  if (!port || !st) return ["", ""];
  if (!st.run) return ["Stopped", ""];
  if (!st.hw) return ["No sensor", "warn"];
  return ["Running", "run"];
}
function renderHome() {
  for (const mod of Object.keys(CHILDREN)) {
    const [t, cls] = stateText(mod);
    const el = $("hs-" + mod);
    el.textContent = t; el.className = "state " + cls;
    el.style.cssText = "text-transform:none;letter-spacing:0;font-size:12px;margin-left:auto;font-weight:600;color:" +
      (cls === "run" ? "var(--ok)" : cls === "warn" ? "var(--warn)" : "var(--muted)");
    $("ha-" + mod).textContent = ago(mod);
  }
  const zr = (mod, text) => { const el = $("hz-" + mod); if (el) el.textContent = text; };
  const signed = (v) => (v == null ? "--" : (v >= 0 ? "+" : "") + v.toFixed(2) + " m");
  if (HAS("gps")) {
  const g = live.gps, gOk = fresh("gps");
  const gv = gpsShown();
  $("hd-gps").innerHTML = (gv != null ? fmtM(gv) : "--") + "<small>m</small>";
  $("hm-gps").textContent = zero.gps ? "Straight-line distance from your zero point" : "Distance from the GPS origin (press Zero to start from here)";
  zr("gps", zero.gps ? "Zero set at your position" : "Not zeroed");
  $("hx-gps").textContent = gOk ? `${g.state === "fix" ? "Fix" : g.state}, ${g.sats ?? 0} satellites` +
    (g.state === "fix" ? `, HDOP ${Number(g.hdop).toFixed(2)}` : "") : "--";
  $("hh-gps").textContent = !gOk ? "" : g.state === "no_data" ? "No data from the GPS module" :
    g.state !== "fix" ? "Waiting for a fix (go outside)" : !g.origin && !zero.gps ? "Press Zero to measure from here" : "";
  }
  if (HAS("lora")) {
  const l = live.lora, lOk = fresh("lora", 8000);
  $("hd-lora").innerHTML = (lOk && l.dist != null ? fmtM(l.dist) : "--") + "<small>m</small>";
  $("hx-lora").textContent = lOk ? `RSSI ${l.rssi} dBm` + (l.snr != null ? ` · SNR ${l.snr.toFixed(1)} dB` : "") : "--";
  $("hh-lora").textContent = l.calibrating ? `Calibrating ${l.calibrating}...` :
    lOk && l.dist == null ? "Not calibrated: calibrate in the LoRa tab" : "";
  zr("lora", zero.lora != null ? `Since zero: ${signed(sinceZero("lora"))}` : "Not zeroed");
  }
  if (HAS("espnow")) {
  const e = live.espnow, eOk = fresh("espnow");
  const eLive = eOk && e.status === "OK";
  $("hd-espnow").innerHTML = (eLive && e.rssi != null ? fmtM(espnowDist(e.rssi)) : "--") + "<small>m</small>";
  $("hx-espnow").textContent = !eOk ? "--" : e.status !== "OK" ? "No signal from the other board" :
    `RSSI ${e.rssi?.toFixed(1)} dBm · peer hears ${e.peer ?? "--"} dBm · loss ${e.loss?.toFixed(1)}%`;
  zr("espnow", zero.espnow != null ? `Since zero: ${signed(sinceZero("espnow"))}` : "Not zeroed");
  }
  for (const k of IMUS.filter(HAS)) {
  const i = live[k], iOk = fresh(k), nav = NAVS[k];
  $("hd-" + k).innerHTML = (nav.active && iOk ? fmtM(nav.traveled) : "--") + "<small>m</small>";
  $("hx-" + k).textContent = !iOk ? "--" : !nav.active ? `Acceleration ${i.lin.toFixed(2)} m/s² (press Zero to start measuring)` :
    `Straight-line from zero ${fmtM(Math.hypot(nav.px, nav.py))} m · speed ${nav.speed.toFixed(2)} m/s`;
  $("hh-" + k).textContent = iOk && !nav.active ? "Hold the IMU still, then press Zero" : "";
  zr(k, nav.active ? (nav.still > 0.25 ? "Zeroed · still" : "Zeroed · moving") : "Not zeroed");
  }
  $("btnZeroAll").disabled = !port;
  for (const mod of Object.keys(CHILDREN)) { const b = $("zb-" + mod); if (b) b.disabled = !port; }
  if (currentTab === "home") drawHome();
}
setInterval(renderHome, 500);
function gpsShown() {
  const z = sinceZero("gps");
  if (z != null) return z;
  return !zero.gps && fresh("gps") && live.gps.dist != null ? live.gps.dist : null;
}
for (const mod of Object.keys(CHILDREN)) {
  const b = $("zb-" + mod);
  if (b) b.onclick = () => { zeroModule(mod); renderHome(); };
}
$("btnZeroAll").onclick = () => { for (const mod of Object.keys(CHILDREN)) zeroModule(mod); renderHome(); };

const cssVar = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
function drawHome() {
  const c = $("homeChart"), dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  if (!w || !h) return;
  if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
  const ctx = c.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  const L = 48, R = 10, T = 8, B = 24, now = Date.now(), span = 300000;
  const series = [["gps", "#2F80ED"], ["lora", "#E07B39"], ["espnow", "#8E5CF6"], ["imu", "#14A39A"], ["imu2", "#D4A017"], ["tape", cssVar("--muted")]]
    .filter(([k]) => k === "tape" || HAS(k));
  let max = 1;
  for (const p of history) for (const [k] of series) if (p[k] != null && isFinite(p[k])) max = Math.max(max, p[k]);
  max = max * 1.15;
  const X = (t) => L + (w - L - R) * (1 - (now - t) / span), Y = (v) => T + (h - T - B) * (1 - v / max);
  ctx.font = "11px -apple-system, Segoe UI, sans-serif"; ctx.fillStyle = cssVar("--muted"); ctx.strokeStyle = cssVar("--line");
  const raw = max / 4, pw = Math.pow(10, Math.floor(Math.log10(raw))), step = [1, 2, 5, 10].map((x) => x * pw).find((x) => x >= raw);
  ctx.textAlign = "right"; ctx.textBaseline = "middle";
  for (let v = 0; v <= max; v += step) {
    ctx.beginPath(); ctx.moveTo(L, Y(v)); ctx.lineTo(w - R, Y(v)); ctx.stroke();
    ctx.fillText(v + " m", L - 6, Y(v));
  }
  ctx.textAlign = "center"; ctx.textBaseline = "top";
  for (let m = 0; m <= 5; m++) ctx.fillText(m === 0 ? "now" : `-${m} min`, X(now - m * 60000), h - B + 6);
  if (!history.length) {
    ctx.textBaseline = "middle"; ctx.fillText(port ? "Collecting data..." : "Connect a board to start", w / 2, h / 2);
    return;
  }
  for (const [k, col] of series) {
    ctx.strokeStyle = col; ctx.lineWidth = k === "tape" ? 1.5 : 2.2; ctx.setLineDash(k === "tape" ? [6, 5] : []);
    ctx.beginPath(); let pen = false;
    for (const p of history) {
      const v = p[k];
      if (v == null || !isFinite(v)) { pen = false; continue; }
      if (!pen) { ctx.moveTo(X(p.t), Y(v)); pen = true; } else ctx.lineTo(X(p.t), Y(v));
    }
    ctx.stroke();
  }
  ctx.setLineDash([]);
}
window.addEventListener("resize", drawHome);
for (const a of document.querySelectorAll("a[data-open]")) a.onclick = () => selectTab(a.dataset.open);

// ---------- Saving CSV files ----------
function csvText(mod) {
  const esc = (v) => v == null ? "" : /[",\n]/.test(String(v)) ? `"${String(v).replace(/"/g, '""')}"` : String(v);
  return [REC[mod].cols, ...REC[mod].rows].map((r) => r.map(esc).join(",")).join("\n") + "\n";
}
function fileStamp() {
  const d = recT0 || new Date(), p = (x) => String(x).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}_${p(d.getHours())}-${p(d.getMinutes())}-${p(d.getSeconds())}`;
}
function download(name, blob) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}
function saveOne(mod) {
  if (!REC[mod].rows.length) return;
  download(`ME74_${REC[mod].file}${labelPart()}_${fileStamp()}.csv`, new Blob([csvText(mod)], { type: "text/csv" }));
}

// Minimal ZIP writer (files stored uncompressed) so "Save all" is one download
const CRC_T = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
function crc32(b) { let c = 0xFFFFFFFF; for (let i = 0; i < b.length; i++) c = CRC_T[(c ^ b[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }
function makeZip(files) {
  const enc = new TextEncoder(), d = new Date();
  const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
  const date = ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  const parts = [], central = [];
  let offset = 0;
  for (const f of files) {
    const name = enc.encode(f.name), data = f.data, crc = crc32(data);
    const lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true); lh.setUint16(4, 20, true); lh.setUint16(6, 0x0800, true);
    lh.setUint16(10, time, true); lh.setUint16(12, date, true); lh.setUint32(14, crc, true);
    lh.setUint32(18, data.length, true); lh.setUint32(22, data.length, true); lh.setUint16(26, name.length, true);
    parts.push(new Uint8Array(lh.buffer), name, data);
    const ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true); ch.setUint16(4, 20, true); ch.setUint16(6, 20, true); ch.setUint16(8, 0x0800, true);
    ch.setUint16(12, time, true); ch.setUint16(14, date, true); ch.setUint32(16, crc, true);
    ch.setUint32(20, data.length, true); ch.setUint32(24, data.length, true); ch.setUint16(28, name.length, true);
    ch.setUint32(42, offset, true);
    central.push(new Uint8Array(ch.buffer), name);
    offset += 30 + name.length + data.length;
  }
  const size = central.reduce((s, a) => s + a.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, files.length, true); end.setUint16(10, files.length, true);
  end.setUint32(12, size, true); end.setUint32(16, offset, true);
  return new Blob([...parts, ...central, new Uint8Array(end.buffer)], { type: "application/zip" });
}
function saveAll() {
  const enc = new TextEncoder(), stampStr = fileStamp();
  const files = Object.keys(REC).filter((m) => REC[m].rows.length)
    .map((m) => ({ name: `ME74_${REC[m].file}${labelPart()}_${stampStr}.csv`, data: enc.encode(csvText(m)) }));
  if (files.length) download(`ME74_all_data${labelPart()}_${stampStr}.zip`, makeZip(files));
}
function totalRows() { return Object.values(REC).reduce((s, r) => s + r.rows.length, 0); }
function updateSaveButtons() {
  const total = totalRows();
  $("btnSaveAll").disabled = $("btnClearData").disabled = total === 0;
  $("recCount").textContent = total ? `${total.toLocaleString()} rows recorded` : "No data recorded yet";
  const r = REC[currentTab];
  saveTabBtn.textContent = `\u2913 Save ${r.label} CSV (${r.rows.length.toLocaleString()} rows)`;
  saveTabBtn.disabled = r.rows.length === 0;
}
setInterval(updateSaveButtons, 1000);
saveTabBtn.onclick = () => saveOne(currentTab);
$("btnSaveAll").onclick = saveAll;
$("btnClearData").onclick = () => {
  if (!confirm("Clear all recorded data? Save it first if you need it.")) return;
  for (const r of Object.values(REC)) r.rows = [];
  history.length = 0; recT0 = null;
  updateSaveButtons(); drawHome();
};
window.addEventListener("beforeunload", (e) => { if (totalRows()) { e.preventDefault(); e.returnValue = ""; } });

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
  REC.console.rows.push([new Date().toISOString(), text]);
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
        if (line.trim()) {
          try { routeLine(line); } catch (err) { console.error(err); }   // one odd line never drops the connection
        }
      }
    }
  } catch (e) {
    if (port) disconnect("Board disconnected");
  } finally {
    try { reader.releaseLock(); } catch (e) {}
  }
}

function routeLine(line) {
  const m = line.match(/^\[(SYS|GPS|IMU2|IMU|LORA|ESPNOW)\] ?(.*)$/);
  if (!m) { consoleLine(line, "other"); return; }
  const tag = m[1], body = m[2];
  if (tag === "SYS" && body.startsWith("STATE ")) { onState(body); return; }
  // The IMU's 50 Hz logging lines would flood the console; show them only in the IMU tab
  if (!((tag === "IMU" || tag === "IMU2") && body.startsWith("LOG,"))) consoleLine(line, tag);
  const mod = TAGS[tag];
  if (!mod || !HAS(mod)) return;   // a module this dashboard doesn't have: console only
  lastSeen[mod] = performance.now();
  recordLine(mod, body);
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
  // The IMU 2 tab is the same dashboard as IMU, so its commands get the "imu2 " prefix
  if (m.mod === "imu2") { for (const c of m.text.split("\n").map((x) => x.trim()).filter(Boolean)) send("imu2 " + c); return; }
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
selectTab("home");
</script>
</body>
</html>
"""

CARD_HTML = {'gps': '      <div class="card" id="card-gps">\n        <h2><span class="swatch" style="background:#2F80ED"></span>GPS <span class="state" id="hs-gps"></span></h2>\n        <div class="big" id="hd-gps">--<small>m</small></div>\n        <div class="method" id="hm-gps">Distance from the GPS origin (press Zero to start from here)</div>\n        <div class="detail" id="hx-gps">--</div>\n        <div class="zero-row"><span id="hz-gps">Not zeroed</span><button id="zb-gps" disabled>Zero</button></div>\n        <div class="hint" id="hh-gps"></div>\n        <div class="foot"><span id="ha-gps">no data</span><a data-open="gps">Open GPS tab</a></div>\n      </div>\n', 'lora': '      <div class="card" id="card-lora">\n        <h2><span class="swatch" style="background:#E07B39"></span>LoRa radio <span class="state" id="hs-lora"></span></h2>\n        <div class="big" id="hd-lora">--<small>m</small></div>\n        <div class="method">Distance from the reference node (the other board), from signal strength</div>\n        <div class="detail" id="hx-lora">--</div>\n        <div class="zero-row"><span id="hz-lora">Not zeroed</span><button id="zb-lora" disabled>Zero</button></div>\n        <div class="hint" id="hh-lora"></div>\n        <div class="foot"><span id="ha-lora">no data</span><a data-open="lora">Open LoRa radio tab</a></div>\n      </div>\n', 'espnow': '      <div class="card" id="card-espnow">\n        <h2><span class="swatch" style="background:#8E5CF6"></span>ESP-NOW <span class="state" id="hs-espnow"></span></h2>\n        <div class="big" id="hd-espnow">--<small>m</small></div>\n        <div class="method">Distance from the reference node (the other board), from signal strength</div>\n        <div class="detail" id="hx-espnow">--</div>\n        <div class="zero-row"><span id="hz-espnow">Not zeroed</span><button id="zb-espnow" disabled>Zero</button></div>\n        <div><label class="cal">RSSI at 1 m <input class="small" id="enRef" type="number" step="1" value="-45"></label>\n             <label class="cal">n <input class="small" id="enN" type="number" step="0.1" min="1" max="6" value="2.5"></label></div>\n        <div class="foot"><span id="ha-espnow">no data</span><a data-open="espnow">Open ESP-NOW tab</a></div>\n      </div>\n', 'imu': '      <div class="card" id="card-imu">\n        <h2><span class="swatch" style="background:#14A39A"></span>IMU <span class="state" id="hs-imu"></span></h2>\n        <div class="big" id="hd-imu">--<small>m</small></div>\n        <div class="method">Distance traveled since zero (acceleration integrated twice; drifts over long runs)</div>\n        <div class="detail" id="hx-imu">--</div>\n        <div class="zero-row"><span id="hz-imu">Not zeroed</span><button id="zb-imu" disabled>Zero</button></div>\n        <div class="hint" id="hh-imu"></div>\n        <div class="foot"><span id="ha-imu">no data</span><a data-open="imu">Open IMU tab</a></div>\n      </div>\n'}
CARD_HTML["imu2"] = (CARD_HTML["imu"].replace('-imu"', '-imu2"').replace('data-open="imu"', 'data-open="imu2"')
                     .replace("#14A39A", "#D4A017").replace(">IMU <", ">IMU 2 (ISM330DHCX) <").replace("Open IMU tab", "Open IMU 2 tab"))
CARD_HTML["imu"] = CARD_HTML["imu"].replace(">IMU <", ">IMU (ICM-20948) <")
LEGEND = {"gps": ("#2F80ED", "GPS"), "lora": ("#E07B39", "LoRa"), "espnow": ("#8E5CF6", "ESP-NOW"),
          "imu": ("#14A39A", "IMU traveled"), "imu2": ("#D4A017", "IMU 2 traveled")}
order = [m for m in ["gps", "lora", "espnow", "imu", "imu2"] if m in SOURCES]
legend = "".join(f'<span><i style="background:{LEGEND[m][0]}"></i>{LEGEND[m][1]}</span>' for m in order if m in LEGEND)
legend += '<span><i style="background:var(--muted)"></i>Tape (actual)</span>'
page = (PAGE.replace("__DATA__", DATA).replace("__TITLE__", TITLE).replace("__FIRMWARE__", FIRMWARE)
        .replace("__NCARDS__", str(len(order))).replace("__CARDS__", "".join(CARD_HTML[m] for m in order))
        .replace("__LEGEND__", legend))
open(OUT, "w", encoding="utf-8").write(page)
print("wrote", OUT, len(page), "bytes")
