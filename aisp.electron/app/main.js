// Off-screen Electron host for the in-game screens: the electron:<url> video sources and, with
// --framed=1, the primary browser that is the screen page itself (frames, document.title and
// script call replies on --video; eval/call on --control). The crop is the layout viewport, with
// scroll/scale/hide and mute as live extras. BGRA of --width x --height on the --video channel
// (latest-frame; drop if blocked) because Chromium helpers inherit stdout.
// --control takes scroll/scale/hide/mute/gain, and for the primary also paint (0 stops the
// off-screen frames while the hook shows its clear colour; the page keeps running) and eval,
// through which the hook keeps window.aisp in the page up to date (the stream's title,
// duration, position and state; see the hook's page_state.cpp). Both the primary and the
// electron: secondaries run framed: the video channel carries frames, the page's title as it
// changes, a `failed` line for a main frame that did not load, and the primary's call
// replies. There is no PCM tap: mute is
// webContents.setAudioMuted, and gain is applied inside the page -- the volume/rolloff fader
// scales every media element (through the prototype accessor, so the site's own slider still
// reads back what it set) and the AudioContext destination.
// The Chromium process keeps its own WASAPI session at whatever the user set it to; we do
// not touch the Windows mixer. HardwareMediaKeyHandling is off so sites do not
// register SMTC / the Windows now-playing overlay. Offscreen paint follows
// origin/feature/tv-support's aisp.electron app; that branch's hardcoded Twitch TV overlay
// is not used — crop/scroll/scale are the layout knobs.
const { app, BrowserWindow, session } = require("electron");
const net = require("net");
const fs = require("fs");
const path = require("path");

function argValue(name, fallback = "") {
  const prefix = `${name}=`;
  for (let i = 1; i < process.argv.length; i++) {
    const value = process.argv[i];
    if (value.startsWith(prefix)) {
      let text = value.slice(prefix.length);
      if (text.startsWith('"') && text.endsWith('"') && text.length >= 2)
        text = text.slice(1, -1);
      return text;
    }
    if (value === name && i + 1 < process.argv.length)
      return process.argv[i + 1];
  }
  return fallback;
}

function argInt(name, fallback) {
  const text = argValue(name, "");
  if (!text)
    return fallback;
  const n = Number.parseInt(text, 10);
  return Number.isFinite(n) ? n : fallback;
}

function argFloat(name, fallback) {
  const text = argValue(name, "");
  if (!text)
    return fallback;
  const n = Number.parseFloat(text);
  return Number.isFinite(n) ? n : fallback;
}

const logPath = path.join(__dirname, "..", "electron-browser.log");
function log(message) {
  const line = `electron-browser: ${message}\n`;
  try {
    process.stderr.write(line);
  } catch {
    // Wine/NUL stdio: uv_pipe_open already failed before we got here, or write throws
  }
  try {
    fs.appendFileSync(logPath, line);
  } catch {
    // the game directory may be read-only; the named pipes are the real channel
  }
}

// For anything on a timer: the first occurrence is the useful one, the next thousand are noise.
const loggedOnce = new Set();
function logOnce(message) {
  if (loggedOnce.has(message))
    return;
  loggedOnce.add(message);
  log(message);
}

process.on("uncaughtException", (error) => log(`uncaught: ${error && error.stack ? error.stack : error && error.message}`));
process.on("unhandledRejection", (error) => log(`unhandled: ${error && error.stack ? error.stack : error}`));

const width = Math.max(1, argInt("--width", 486));
const height = Math.max(1, argInt("--height", 343));
const fps = Math.max(1, argInt("--fps", 30));
const frameBytes = width * height * 4;
const url = argValue("--url", "");
const controlName = argValue("--control", "");
const videoName = argValue("--video", "");
// --framed=1: the video channel carries 8-byte-headed messages (type, length): 1 = a BGRA
// frame, 2 = a UTF-8 text line (title …, ret <id> <json>, err <id> <text>). Without it the
// channel is bare frames, as the secondary electron: sources read it.
const framed = argInt("--framed", 0) ? 1 : 0;
// The channel protocol the hook expects; bump both sides together when the format changes. The
// app announces itself as the first framed message so a stale copy shows up in aisp.screen.log.
const PROTOCOL = 2;
const APP_VERSION = `aisp.electron app 2026-09-08a (protocol ${PROTOCOL})`;
const wine = argInt("--wine", 0) ? 1 : 0;

const state = {
  scrollx: argInt("--scrollx", 0),
  scrolly: argInt("--scrolly", 0),
  hideScrollbars: argInt("--hide-scrollbars", 0) ? 1 : 0,
  scale: Math.max(0.1, argFloat("--scale", 1)),
  muted: argInt("--mute", 0) ? 1 : 0,
  gain: Math.min(1, Math.max(0, argFloat("--gain", 1))),
  paint: 1,
};

if (!url) {
  log("missing --url");
  process.exit(2);
}

// Chromium registers System Media Transport Controls / hardware media keys whenever a
// <video> plays. That is the Windows volume-flyout "now playing" overlay. Kill it before
// ready; JS stubs of navigator.mediaSession do not stop the C++ SMTC path.
app.commandLine.appendSwitch(
  "disable-features",
  [
    "HardwareMediaKeyHandling",
    "MediaSessionService",
    "GlobalMediaControls",
    "GlobalMediaControlsUpdatedUI",
    "GlobalMediaControlsPictureInPicture",
    "WebAppSystemMediaControls",
  ].join(",")
);
app.commandLine.appendSwitch("autoplay-policy", "no-user-gesture-required");
app.commandLine.appendSwitch("force-device-scale-factor", "1");
app.commandLine.appendSwitch("disable-logging");
app.commandLine.appendSwitch("log-level", "3");
if (wine || process.platform !== "win32") {
  // Wine-in-process Chromium dies; native Linux on Xvfb has no usable GPU process.
  // Off-screen paint is software either way. --no-sandbox must also be on the argv
  // (host.js) because the SUID helper check runs before this file.
  app.disableHardwareAcceleration();
  app.commandLine.appendSwitch("no-sandbox");
  app.commandLine.appendSwitch("disable-gpu-sandbox");
  app.commandLine.appendSwitch("disable-dev-shm-usage");
}
app.setName("aisp");
app.setAppUserModelId("be.kaetemi.aisp.electron");

let browserWindow;
let videoSocket;
let blocked = false;
let pendingFrame;
let applyTimer;
let gainTimer;
let volumeTimer;

function framedMessage(type, payload) {
  const header = Buffer.alloc(8);
  header.writeUInt32LE(type, 0);
  header.writeUInt32LE(payload.length, 4);
  return Buffer.concat([header, payload]);
}

// Text to the hook, in order with the frames. Never dropped: the socket queues it.
function sendText(text) {
  const sink = videoSocket && !videoSocket.destroyed ? videoSocket : null;
  if (!sink || !framed)
    return;
  sink.write(framedMessage(2, Buffer.from(String(text).replace(/[\r\n]/g, " "), "utf8")));
}

// A page's title is reported once its own script has run (dom-ready), not as the document
// parses: the served <title> is only the source, the page adds its layout to it, and the hook
// acts on the first title it sees after a navigation. Live updates follow from then on.
let titleHold = true;

function sendTitle(title) {
  if (title != null && !titleHold)
    sendText(`title ${title}`);
}

// call <id> <js>: evaluate in the page, answer with the JSON of the value.
// executeJavaScript is held back until the page has finished loading, and the hook's caller
// (the game thread) would sit on it for its whole timeout: while the main frame loads, a call
// is answered at once with `loading`, which the hook reads as "nothing there yet".
let pageLoading = true;

function callScript(id, code) {
  if (!browserWindow || browserWindow.isDestroyed()) {
    sendText(`err ${id} no window`);
    return;
  }
  if (pageLoading) {
    sendText(`err ${id} loading`);
    return;
  }
  browserWindow.webContents.executeJavaScript(code)
    .then((value) => sendText(`ret ${id} ${JSON.stringify(value === undefined ? null : value)}`))
    .catch((error) => sendText(`err ${id} ${String((error && error.message) || error)}`));
}

function writeLatest(frame) {
  const sink = videoSocket && !videoSocket.destroyed ? videoSocket : null;
  if (!sink) {
    pendingFrame = frame;
    return;
  }
  if (blocked) {
    pendingFrame = frame;
    return;
  }
  blocked = !sink.write(framed ? framedMessage(1, frame) : frame);
}

function bitmapOf(image) {
  const size = image.getSize();
  if (size.width === width && size.height === height) {
    const bitmap = image.toBitmap();
    return bitmap.length === frameBytes ? bitmap : undefined;
  }
  if (size.width < 1 || size.height < 1)
    return undefined;
  const resized = image.resize({ width, height, quality: "nearest" });
  const bitmap = resized.toBitmap();
  return bitmap.length === frameBytes ? bitmap : undefined;
}

function scrollScript() {
  const x = state.scrollx | 0;
  const y = state.scrolly | 0;
  const hide = state.hideScrollbars ? 1 : 0;
  return `(function(){window.__aispX=${x};window.__aispY=${y};window.__aispH=${hide};
window.__aispGoFn=function(){var x=window.__aispX,y=window.__aispY,h=window.__aispH;
var st=document.getElementById('aisp-noscrollbar');
if(h){if(!st){st=document.createElement('style');st.id='aisp-noscrollbar';
st.textContent='html,body{overflow:hidden!important}::-webkit-scrollbar{display:none!important;width:0!important;height:0!important}';
(document.documentElement||document.head).appendChild(st);}
var r=document.scrollingElement||document.documentElement;
if(r){r.scrollLeft=x;r.scrollTop=y;}
if(document.documentElement){document.documentElement.scrollLeft=x;document.documentElement.scrollTop=y;}
if(document.body){document.body.scrollLeft=x;document.body.scrollTop=y;}
window.scrollTo(x,y);}
else if(st){st.remove();try{document.documentElement.style.overflow='';if(document.body)document.body.style.overflow='';}catch(e){}}};
window.__aispGoFn();
if(!window.__aispGo)window.__aispGo=setInterval(window.__aispGoFn,200);
})();`;
}

// The fader lives in the page, not in the Windows mixer. HTMLMediaElement.prototype.volume is
// wrapped so the page keeps reading back the volume it asked for (players re-read their own
// slider and would fight a value we wrote behind their back) while the element actually plays
// at page volume x host gain. AudioContext.destination hands out a GainNode in front of the
// real destination for sites that mix in WebAudio. Idempotent: re-running only refreshes.
function volumeInstallScript(gain) {
  return `(function(){window.__aispGain=${gain};
if(window.__aispVolFn){window.__aispVolFn();return;}
var proto=window.HTMLMediaElement&&HTMLMediaElement.prototype;
var d=proto&&Object.getOwnPropertyDescriptor(proto,'volume');
if(d&&d.get&&d.set){try{Object.defineProperty(proto,'volume',{configurable:true,enumerable:d.enumerable,
get:function(){var v=this.__aispPage;return v===undefined?d.get.call(this):v;},
set:function(v){v=Number(v);if(!(v>=0))v=0;if(v>1)v=1;this.__aispPage=v;
try{d.set.call(this,v*window.__aispGain);}catch(e){}}});}catch(e){d=null;}}
window.__aispVolFn=function(){var g=window.__aispGain;
var list=document.querySelectorAll('video,audio');
for(var i=0;i<list.length;i++){var el=list[i];var p=el.__aispPage;
if(p===undefined){p=d?d.get.call(el):el.volume;el.__aispPage=p;}
try{if(d)d.set.call(el,p*g);else el.volume=p*g;}catch(e){}}
var ns=window.__aispNodes;if(ns)for(var j=0;j<ns.length;j++){try{ns[j].gain.value=g;}catch(e){}}};
var Ctx=window.AudioContext||window.webkitAudioContext;
if(Ctx&&Ctx.prototype&&!Ctx.prototype.__aispPatched){
var base=Object.getPrototypeOf(Ctx.prototype);
var dd=Object.getOwnPropertyDescriptor(Ctx.prototype,'destination')||(base&&Object.getOwnPropertyDescriptor(base,'destination'));
if(dd&&dd.get){window.__aispNodes=window.__aispNodes||[];
try{Object.defineProperty(Ctx.prototype,'destination',{configurable:true,get:function(){
var real=dd.get.call(this);
if(!this.__aispNode){try{var n=this.createGain();n.gain.value=window.__aispGain;n.connect(real);
try{n.maxChannelCount=real.maxChannelCount;}catch(e){}
this.__aispNode=n;window.__aispNodes.push(n);}catch(e){return real;}}
return this.__aispNode;}});Ctx.prototype.__aispPatched=true;}catch(e){}}}
window.__aispVolFn();
if(!window.__aispVol)window.__aispVol=setInterval(window.__aispVolFn,250);
})();`;
}

function volumeSetScript(gain) {
  return `window.__aispGain=${gain};window.__aispVolFn&&window.__aispVolFn();`;
}

function clampedGain() {
  const value = state.gain;
  if (!Number.isFinite(value))
    return "1";
  return Math.min(1, Math.max(0, value)).toFixed(4);
}

// Subframes matter: an embedded player is its own frame, and executeJavaScript on the
// webContents only reaches the main one.
function eachFrame(run) {
  if (!browserWindow || browserWindow.isDestroyed())
    return;
  const contents = browserWindow.webContents;
  let frames;
  try {
    frames = contents.mainFrame.framesInSubtree;
  } catch {
    frames = null;
  }
  if (!frames || !frames.length) {
    // No frame tree to walk (it went away, or this Electron does not expose one): the main
    // document is still worth fading.
    contents.executeJavaScript(run).catch((error) => logOnce(`fader: ${error.message}`));
    return;
  }
  for (const frame of frames) {
    try {
      if (!frame.detached)
        frame.executeJavaScript(run, true).catch((error) => logOnce(`fader: ${error.message}`));
    } catch {
      // the frame went away between the walk and the call
    }
  }
}

// The install runs on a timer as well as on load: a navigation throws the page state away,
// and iframes appear late.
function installVolume() {
  eachFrame(volumeInstallScript(clampedGain()));
}

function pushGain() {
  if (gainTimer)
    return;
  gainTimer = setTimeout(() => {
    gainTimer = undefined;
    eachFrame(volumeSetScript(clampedGain()));
  }, 40);
}

// Off-screen painting on or off; a page that comes back is repainted whole, its damage from
// the meantime is gone.
function applyPaint(changed) {
  if (!browserWindow || browserWindow.isDestroyed())
    return;
  const contents = browserWindow.webContents;
  if (state.paint) {
    contents.startPainting();
    if (changed)
      contents.invalidate();
  } else {
    contents.stopPainting();
  }
}

function applyView() {
  if (!browserWindow || browserWindow.isDestroyed())
    return;
  const contents = browserWindow.webContents;
  const scale = state.scale > 0 ? state.scale : 1;
  contents.setZoomFactor(scale);
  contents.setAudioMuted(!!state.muted);
  applyPaint(false);
  contents.executeJavaScript(scrollScript()).catch((error) => log(`scroll script failed: ${error.message}`));
  installVolume();
}

function scheduleApply() {
  if (applyTimer)
    return;
  applyTimer = setTimeout(() => {
    applyTimer = undefined;
    applyView();
  }, 0);
}

function applyLine(line) {
  const scroll = /^scroll\s+(-?\d+)\s+(-?\d+)(?:\s+(-?\d+))?/.exec(line);
  if (scroll) {
    state.scrollx = Number.parseInt(scroll[1], 10);
    state.scrolly = Number.parseInt(scroll[2], 10);
    if (scroll[3] !== undefined)
      state.hideScrollbars = Number.parseInt(scroll[3], 10) ? 1 : 0;
    scheduleApply();
    return;
  }
  const scale = /^scale\s+([0-9.]+)/.exec(line);
  if (scale) {
    const value = Number.parseFloat(scale[1]);
    if (value > 0) {
      state.scale = value;
      scheduleApply();
    }
    return;
  }
  const mute = /^mute\s+(-?\d+)/.exec(line);
  if (mute) {
    state.muted = Number.parseInt(mute[1], 10) ? 1 : 0;
    scheduleApply();
    return;
  }
  const paint = /^paint\s+(-?\d+)/.exec(line);
  if (paint) {
    const value = Number.parseInt(paint[1], 10) ? 1 : 0;
    const changed = value !== state.paint;
    state.paint = value;
    if (changed)
      log(`paint ${value}`);
    applyPaint(changed);
    return;
  }
  if (line.startsWith("eval ")) {
    if (browserWindow && !browserWindow.isDestroyed())
      browserWindow.webContents.executeJavaScript(line.slice(5)).catch((error) => logOnce(`eval: ${error.message}`));
    return;
  }
  const call = /^call (\S+) ([\s\S]*)$/.exec(line);
  if (call) {
    callScript(call[1], call[2]);
    return;
  }
  const gain = /^gain\s+([0-9.]+)/.exec(line);
  if (gain) {
    const value = Number.parseFloat(gain[1]);
    if (Number.isFinite(value)) {
      state.gain = Math.min(1, Math.max(0, value));
      pushGain();
    }
  }
}

function connectPipe(name, label, onData) {
  if (!name)
    return null;
  // Windows: \\.\pipe\…  Wine/native: 127.0.0.1:port (the hook listens, we connect).
  const tcp = /^(\d+\.\d+\.\d+\.\d+):(\d+)$/.exec(name);
  const socket = tcp
    ? net.createConnection({ host: tcp[1], port: Number.parseInt(tcp[2], 10) })
    : net.createConnection({ path: name, allowHalfOpen: true });
  socket.once("connect", () => {
    log(`${label} pipe connected`);
    if (label === "video" && framed) {
      log(APP_VERSION);
      socket.write(framedMessage(2, Buffer.from(`hello ${APP_VERSION}`, "utf8")));
    }
    if (label === "video" && pendingFrame) {
      const frame = pendingFrame;
      pendingFrame = undefined;
      writeLatest(frame);
    }
  });
  socket.on("drain", () => {
    if (label !== "video")
      return;
    blocked = false;
    if (pendingFrame) {
      const frame = pendingFrame;
      pendingFrame = undefined;
      writeLatest(frame);
    }
  });
  socket.once("error", (error) => log(`${label} pipe error: ${error.message}`));
  socket.once("close", () => {
    log(`${label} pipe closed`);
    if (label === "control" || label === "video")
      app.quit();
  });
  if (onData) {
    let leftover = "";
    socket.setEncoding("utf8");
    socket.on("data", (chunk) => {
      leftover += chunk;
      for (;;) {
        const newline = leftover.indexOf("\n");
        if (newline < 0)
          break;
        let line = leftover.slice(0, newline);
        leftover = leftover.slice(newline + 1);
        if (line.endsWith("\r"))
          line = line.slice(0, -1);
        if (line)
          onData(line);
      }
    });
  }
  return socket;
}

app.whenReady().then(() => {
  connectPipe(controlName, "control", applyLine);
  videoSocket = connectPipe(videoName, "video");

  const denyPermission = new Set([
    "notifications",
    "clipboard-read",
    "openExternal",
    "pointerLock",
    "idle-detection",
    "geolocation",
    "display-capture",
    "media",
  ]);
  session.defaultSession.setPermissionRequestHandler((_contents, permission, callback) => {
    callback(!denyPermission.has(permission));
  });
  session.defaultSession.setPermissionCheckHandler((_contents, permission) => !denyPermission.has(permission));

  browserWindow = new BrowserWindow({
    width,
    height,
    useContentSize: true,
    show: false,
    frame: false,
    skipTaskbar: true,
    focusable: false,
    autoHideMenuBar: true,
    title: "aisp",
    paintWhenInitiallyHidden: true,
    backgroundColor: "#000000",
    webPreferences: {
      backgroundThrottling: false,
      // Electron 22 (Wine ia32) wants a boolean; 31+ accepts { useSharedTexture }.
      offscreen: Number.parseInt(String(process.versions.electron).split(".")[0], 10) >= 31
        ? { useSharedTexture: false }
        : true,
    },
  });
  browserWindow.setMenuBarVisibility(false);
  browserWindow.setSkipTaskbar(true);

  const chromeVersion = process.versions.chrome;
  const contents = browserWindow.webContents;
  contents.setUserAgent(
    `Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${chromeVersion} Safari/537.36`
  );
  if (typeof contents.setWindowOpenHandler === "function")
    contents.setWindowOpenHandler(() => ({ action: "deny" }));
  else
    contents.on("new-window", (event) => event.preventDefault());
  contents.setFrameRate(fps);
  contents.setAudioMuted(!!state.muted);
  contents.on("paint", (_event, _dirty, image) => {
    const bitmap = bitmapOf(image);
    if (bitmap)
      writeLatest(bitmap);
  });
  contents.on("did-start-navigation", (_event, _navUrl, isInPlace, isMainFrame) => {
    if (isMainFrame && !isInPlace) {
      pageLoading = true;
      titleHold = true;
    }
  });
  contents.on("did-finish-load", () => {
    pageLoading = false;
    titleHold = false;
    applyView();
    sendTitle(browserWindow.getTitle());
    log(`loaded ${contents.getURL()}`);
  });
  contents.on("page-title-updated", (_event, title) => sendTitle(title));
  contents.on("did-navigate", () => scheduleApply());
  contents.on("did-frame-navigate", () => installVolume());
  contents.on("dom-ready", () => {
    // The page's inline script has run: its title is complete, and the hook is waiting on it
    // (a slow frame page must not hold the stream back until the load event).
    titleHold = false;
    sendTitle(browserWindow.getTitle());
    installVolume();
  });
  volumeTimer = setInterval(installVolume, 500);
  contents.on("did-fail-load", (_event, code, description, failedUrl, isMainFrame) => {
    // -3 is ERR_ABORTED: a navigation superseded by the next one, which is loading now.
    if (!isMainFrame || code === -3)
      return;
    pageLoading = false;
    titleHold = false;
    // The hook decides what a page that did not come means: for the primary, nothing to play.
    sendText(`failed ${code} ${description}`);
    log(`load failed ${code}: ${description} (${failedUrl})`);
  });
  contents.on("render-process-gone", (_event, details) =>
    log(`renderer exited: ${details.reason}`)
  );
  browserWindow.setContentSize(width, height);
  browserWindow.loadURL(url).catch((error) => log(`navigate: ${error.message}`));
  log(`offscreen ${width}x${height} @ ${fps} fps scale ${state.scale}`);
});

app.on("before-quit", () => {
  if (volumeTimer)
    clearInterval(volumeTimer);
  volumeTimer = undefined;
  log("quitting");
});
