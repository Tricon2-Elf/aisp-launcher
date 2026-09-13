// Off-screen Electron host for the in-game screens: one process per game, one BrowserWindow per
// screen. The hook starts it once with --hub=<pipe or 127.0.0.1:port> and sends an `open` line
// on that channel for every screen (the primary browser that is the screen page itself, and the
// electron:<url> video sources); each open names the screen's own control and video channels,
// which this process connects to. Closing a screen's control channel closes its window; closing
// the hub closes the process. (--url with --control/--video, no hub, still runs one screen and
// exits with it: the old one-process-per-screen form.)
//
// Per screen: the crop is the layout viewport, with scroll/scale/hide and mute as live extras.
// BGRA of width x height on the video channel (latest-frame; drop if blocked) because Chromium
// helpers inherit stdout. The control channel takes scroll/scale/hide/mute/gain, and for the
// primary also paint (0 stops the off-screen frames while the hook shows its clear colour; the
// page keeps running) and eval, through which the hook keeps window.aisp in the page up to date
// (the stream's title, duration, position and state; see the hook's page_state.cpp). Framed
// channels carry frames, the page's title as it changes, a `failed` line for a main frame that
// did not load, and the primary's call replies. There is no PCM tap: mute is
// webContents.setAudioMuted, and gain is applied inside the page -- the volume/rolloff fader
// scales every media element (through the prototype accessor, so the site's own slider still
// reads back what it set) and the AudioContext destination.
// The Chromium process keeps its own WASAPI session at whatever the user set it to; we do
// not touch the Windows mixer. HardwareMediaKeyHandling is off so sites do not
// register SMTC / the Windows now-playing overlay. Offscreen paint follows
// origin/feature/tv-support's aisp.electron app; that branch's hardcoded Twitch TV overlay
// is not used — crop/scroll/scale are the layout knobs.
const { app, BrowserWindow, net: electronNet, session } = require("electron");
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

// The hub: the hook's channel for `open` lines. Without one, the single screen on the command
// line (--url, --control, --video and the layout arguments), and the process ends with it.
const hubName = argValue("--hub", "");
const singleUrl = argValue("--url", "");

// The channel protocol the hook expects; bump both sides together when the format changes. The
// app announces itself as the first framed message on every video channel so a stale copy shows
// up in aisp.screen.log.
const PROTOCOL = 2;
const APP_VERSION = `aisp.electron app 2026-09-09a (protocol ${PROTOCOL})`;
const wine = argInt("--wine", 0) ? 1 : 0;

if (!hubName && !singleUrl) {
  log("missing --hub (or --url)");
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
// A hardware GPU process next to the 32-bit D3D9 client takes the game down when the
// first in-game screen starts. Software blit is enough for the off-screen crop.
app.disableHardwareAcceleration();
app.commandLine.appendSwitch("disable-gpu");
app.commandLine.appendSwitch("disable-gpu-compositing");
if (wine || process.platform !== "win32") {
  // Wine-in-process Chromium dies; native Linux on Xvfb has no usable GPU process.
  // --no-sandbox must also be on the argv (host.js) because the SUID helper check
  // runs before this file.
  app.commandLine.appendSwitch("no-sandbox");
  app.commandLine.appendSwitch("disable-gpu-sandbox");
  app.commandLine.appendSwitch("disable-dev-shm-usage");
}
app.setName("aisp");
app.setAppUserModelId("be.kaetemi.aisp.electron");

function framedMessage(type, payload) {
  const header = Buffer.alloc(8);
  header.writeUInt32LE(type, 0);
  header.writeUInt32LE(payload.length, 4);
  return Buffer.concat([header, payload]);
}

// An `open` line: key=value words, the url last (it may itself hold spaces or = signs).
function parseOpen(line) {
  const urlAt = line.indexOf(" url=");
  const head = urlAt >= 0 ? line.slice(0, urlAt) : line;
  const url = urlAt >= 0 ? line.slice(urlAt + 5) : "";
  const args = {};
  for (const part of head.split(" ")) {
    const eq = part.indexOf("=");
    if (eq > 0)
      args[part.slice(0, eq)] = part.slice(eq + 1);
  }
  args.url = url;
  return args;
}

function numberOr(text, fallback) {
  const n = Number.parseFloat(text);
  return Number.isFinite(n) ? n : fallback;
}

// A line-oriented connection to the hook: Windows \\.\pipe\… or Wine/native 127.0.0.1:port (the
// hook listens, we connect). onLine gets each complete line; the other callbacks are optional.
function connectChannel(name, { onConnect, onLine, onDrain, onClose, onError }) {
  const tcp = /^(\d+\.\d+\.\d+\.\d+):(\d+)$/.exec(name);
  const socket = tcp
    ? net.createConnection({ host: tcp[1], port: Number.parseInt(tcp[2], 10) })
    : net.createConnection({ path: name, allowHalfOpen: true });
  if (onConnect)
    socket.once("connect", onConnect);
  if (onDrain)
    socket.on("drain", onDrain);
  socket.once("error", (error) => (onError ? onError(error) : undefined));
  // A pipe connection is half-open (the hook's end closing only ends our reading), so the
  // other side going away shows as `end`, not `close`; both mean the channel is over.
  let closed = false;
  const finish = () => {
    if (closed)
      return;
    closed = true;
    if (!socket.destroyed)
      socket.destroy();
    if (onClose)
      onClose();
  };
  socket.once("end", finish);
  socket.once("close", finish);
  if (onLine) {
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
          onLine(line);
      }
    });
  }
  return socket;
}

function scrollScript(state) {
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

let nextScreenId = 1;
const screens = new Set();

// One in-game screen: its off-screen window and its two channels to the hook.
class Screen {
  constructor(args, single) {
    this.id = args.id || String(nextScreenId++);
    this.single = !!single; // the command-line screen: the process ends with it
    this.width = Math.max(1, numberOr(args.width, 486) | 0);
    this.height = Math.max(1, numberOr(args.height, 343) | 0);
    this.fps = Math.max(1, numberOr(args.fps, 30) | 0);
    this.frameBytes = this.width * this.height * 4;
    this.url = args.url || "";
    this.controlName = args.control || "";
    this.videoName = args.video || "";
    // run=<url>: a script fetched once and run in the page after every main-frame load (a site's
    // own player button, a layout switch); nothing the page could do on its own from outside.
    this.runUrl = args.run || "";
    this.runScript = undefined;
    // framed=1: the video channel carries 8-byte-headed messages (type, length): 1 = a BGRA
    // frame, 2 = a UTF-8 text line (hello …, title …, failed …, ret <id> <json>, err <id> <text>).
    // Without it the channel is bare frames.
    this.framed = numberOr(args.framed, 0) ? 1 : 0;
    this.state = {
      scrollx: numberOr(args.scrollx, 0) | 0,
      scrolly: numberOr(args.scrolly, 0) | 0,
      hideScrollbars: numberOr(args.hide, 0) ? 1 : 0,
      scale: Math.max(0.1, numberOr(args.scale, 1)),
      muted: numberOr(args.mute, 0) ? 1 : 0,
      gain: Math.min(1, Math.max(0, numberOr(args.gain, 1))),
      paint: 1,
    };
    this.window = undefined;
    this.videoSocket = undefined;
    this.controlSocket = undefined;
    this.blocked = false;
    this.pendingFrame = undefined;
    this.applyTimer = undefined;
    this.gainTimer = undefined;
    this.volumeTimer = undefined;
    // A page's title is reported once its own script has run (dom-ready), not as the document
    // parses: the served <title> is only the source, the page adds its layout to it, and the
    // hook acts on the first title it sees after a navigation. Live updates follow from then on.
    this.titleHold = true;
    // executeJavaScript is held back until the page has finished loading, and the hook's caller
    // (the game thread) would sit on it for its whole timeout: while the main frame loads, a
    // call is answered at once with `loading`, which the hook reads as "nothing there yet".
    this.pageLoading = true;
    this.closed = false;
  }

  log(message) {
    log(`[${this.id}] ${message}`);
  }

  // Text to the hook, in order with the frames. Never dropped: the socket queues it.
  sendText(text) {
    const sink = this.videoSocket && !this.videoSocket.destroyed ? this.videoSocket : null;
    if (!sink || !this.framed)
      return;
    sink.write(framedMessage(2, Buffer.from(String(text).replace(/[\r\n]/g, " "), "utf8")));
  }

  sendTitle(title) {
    if (title != null && !this.titleHold)
      this.sendText(`title ${title}`);
  }

  // call <id> <js>: evaluate in the page, answer with the JSON of the value.
  callScript(id, code) {
    if (!this.window || this.window.isDestroyed()) {
      this.sendText(`err ${id} no window`);
      return;
    }
    if (this.pageLoading) {
      this.sendText(`err ${id} loading`);
      return;
    }
    this.window.webContents.executeJavaScript(code)
      .then((value) => this.sendText(`ret ${id} ${JSON.stringify(value === undefined ? null : value)}`))
      .catch((error) => this.sendText(`err ${id} ${String((error && error.message) || error)}`));
  }

  writeLatest(frame) {
    const sink = this.videoSocket && !this.videoSocket.destroyed ? this.videoSocket : null;
    if (!sink || this.blocked) {
      this.pendingFrame = frame;
      return;
    }
    this.blocked = !sink.write(this.framed ? framedMessage(1, frame) : frame);
  }

  flushPending() {
    if (!this.pendingFrame)
      return;
    const frame = this.pendingFrame;
    this.pendingFrame = undefined;
    this.writeLatest(frame);
  }

  bitmapOf(image) {
    const size = image.getSize();
    if (size.width === this.width && size.height === this.height) {
      const bitmap = image.toBitmap();
      return bitmap.length === this.frameBytes ? bitmap : undefined;
    }
    if (size.width < 1 || size.height < 1)
      return undefined;
    const resized = image.resize({ width: this.width, height: this.height, quality: "nearest" });
    const bitmap = resized.toBitmap();
    return bitmap.length === this.frameBytes ? bitmap : undefined;
  }

  clampedGain() {
    const value = this.state.gain;
    if (!Number.isFinite(value))
      return "1";
    return Math.min(1, Math.max(0, value)).toFixed(4);
  }

  // Subframes matter: an embedded player is its own frame, and executeJavaScript on the
  // webContents only reaches the main one.
  eachFrame(run) {
    if (!this.window || this.window.isDestroyed())
      return;
    const contents = this.window.webContents;
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
  installVolume() {
    this.eachFrame(volumeInstallScript(this.clampedGain()));
  }

  pushGain() {
    if (this.gainTimer)
      return;
    this.gainTimer = setTimeout(() => {
      this.gainTimer = undefined;
      this.eachFrame(volumeSetScript(this.clampedGain()));
    }, 40);
  }

  // Off-screen painting on or off; a page that comes back is repainted whole, its damage from
  // the meantime is gone.
  applyPaint(changed) {
    if (!this.window || this.window.isDestroyed())
      return;
    const contents = this.window.webContents;
    if (this.state.paint) {
      contents.startPainting();
      if (changed)
        contents.invalidate();
    } else {
      contents.stopPainting();
    }
  }

  applyView() {
    if (!this.window || this.window.isDestroyed())
      return;
    const contents = this.window.webContents;
    const scale = this.state.scale > 0 ? this.state.scale : 1;
    contents.setZoomFactor(scale);
    contents.setAudioMuted(!!this.state.muted);
    this.applyPaint(false);
    contents.executeJavaScript(scrollScript(this.state)).catch((error) => this.log(`scroll script failed: ${error.message}`));
    this.installVolume();
  }

  scheduleApply() {
    if (this.applyTimer)
      return;
    this.applyTimer = setTimeout(() => {
      this.applyTimer = undefined;
      this.applyView();
    }, 0);
  }

  applyLine(line) {
    const state = this.state;
    const scroll = /^scroll\s+(-?\d+)\s+(-?\d+)(?:\s+(-?\d+))?/.exec(line);
    if (scroll) {
      state.scrollx = Number.parseInt(scroll[1], 10);
      state.scrolly = Number.parseInt(scroll[2], 10);
      if (scroll[3] !== undefined)
        state.hideScrollbars = Number.parseInt(scroll[3], 10) ? 1 : 0;
      this.scheduleApply();
      return;
    }
    const scale = /^scale\s+([0-9.]+)/.exec(line);
    if (scale) {
      const value = Number.parseFloat(scale[1]);
      if (value > 0) {
        state.scale = value;
        this.scheduleApply();
      }
      return;
    }
    const mute = /^mute\s+(-?\d+)/.exec(line);
    if (mute) {
      state.muted = Number.parseInt(mute[1], 10) ? 1 : 0;
      this.scheduleApply();
      return;
    }
    const paint = /^paint\s+(-?\d+)/.exec(line);
    if (paint) {
      const value = Number.parseInt(paint[1], 10) ? 1 : 0;
      const changed = value !== state.paint;
      state.paint = value;
      if (changed)
        this.log(`paint ${value}`);
      this.applyPaint(changed);
      return;
    }
    if (line.startsWith("eval ")) {
      if (this.window && !this.window.isDestroyed())
        this.window.webContents.executeJavaScript(line.slice(5)).catch((error) => logOnce(`eval: ${error.message}`));
      return;
    }
    const call = /^call (\S+) ([\s\S]*)$/.exec(line);
    if (call) {
      this.callScript(call[1], call[2]);
      return;
    }
    const gain = /^gain\s+([0-9.]+)/.exec(line);
    if (gain) {
      const value = Number.parseFloat(gain[1]);
      if (Number.isFinite(value)) {
        state.gain = Math.min(1, Math.max(0, value));
        this.pushGain();
      }
    }
  }

  open() {
    if (!this.url || !this.controlName || !this.videoName) {
      this.log("open needs url, control and video");
      this.close("bad open");
      return;
    }
    screens.add(this);
    this.controlSocket = connectChannel(this.controlName, {
      onConnect: () => this.log("control pipe connected"),
      onLine: (line) => this.applyLine(line),
      onError: (error) => this.log(`control pipe error: ${error.message}`),
      onClose: () => this.close("control pipe closed"),
    });
    this.videoSocket = connectChannel(this.videoName, {
      onConnect: () => {
        this.log("video pipe connected");
        if (this.framed) {
          this.log(APP_VERSION);
          this.videoSocket.write(framedMessage(2, Buffer.from(`hello ${APP_VERSION}`, "utf8")));
        }
        this.flushPending();
      },
      onDrain: () => {
        this.blocked = false;
        this.flushPending();
      },
      onError: (error) => this.log(`video pipe error: ${error.message}`),
      onClose: () => this.close("video pipe closed"),
    });

    const browserWindow = new BrowserWindow({
      width: this.width,
      height: this.height,
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
    this.window = browserWindow;
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
    contents.setFrameRate(this.fps);
    contents.setAudioMuted(!!this.state.muted);
    contents.on("paint", (_event, _dirty, image) => {
      const bitmap = this.bitmapOf(image);
      if (bitmap)
        this.writeLatest(bitmap);
    });
    contents.on("did-start-navigation", (_event, _navUrl, isInPlace, isMainFrame) => {
      if (isMainFrame && !isInPlace) {
        this.pageLoading = true;
        this.titleHold = true;
      }
    });
    contents.on("did-finish-load", () => {
      this.pageLoading = false;
      this.titleHold = false;
      this.applyView();
      this.sendTitle(browserWindow.getTitle());
      this.log(`loaded ${contents.getURL()}`);
      this.runAfterLoad();
    });
    contents.on("page-title-updated", (_event, title) => this.sendTitle(title));
    contents.on("did-navigate", () => this.scheduleApply());
    contents.on("did-frame-navigate", () => this.installVolume());
    contents.on("dom-ready", () => {
      // The page's inline script has run: its title is complete, and the hook is waiting on it
      // (a slow frame page must not hold the stream back until the load event).
      this.titleHold = false;
      this.sendTitle(browserWindow.getTitle());
      this.installVolume();
    });
    this.volumeTimer = setInterval(() => this.installVolume(), 500);
    contents.on("did-fail-load", (_event, code, description, failedUrl, isMainFrame) => {
      // -3 is ERR_ABORTED: a navigation superseded by the next one, which is loading now.
      if (!isMainFrame || code === -3)
        return;
      this.pageLoading = false;
      this.titleHold = false;
      // The hook decides what a page that did not come means: for the primary, nothing to play.
      this.sendText(`failed ${code} ${description}`);
      this.log(`load failed ${code}: ${description} (${failedUrl})`);
    });
    contents.on("render-process-gone", (_event, details) => this.log(`renderer exited: ${details.reason}`));
    browserWindow.on("closed", () => {
      this.window = undefined;
      this.close("window closed");
    });
    browserWindow.setContentSize(this.width, this.height);
    browserWindow.loadURL(this.url).catch((error) => this.log(`navigate: ${error.message}`));
    this.log(`offscreen ${this.width}x${this.height} @ ${this.fps} fps scale ${this.state.scale} ${this.url}`);
  }

  // The run= script, fetched on first use (the page's own CSP does not apply to a script the
  // host injects), then run in the main frame.
  runAfterLoad() {
    if (!this.runUrl || !this.window || this.window.isDestroyed())
      return;
    const execute = (script) => {
      if (!this.window || this.window.isDestroyed())
        return;
      this.window.webContents.executeJavaScript(script).catch((error) => this.log(`run: ${error.message}`));
    };
    if (this.runScript !== undefined) {
      execute(this.runScript);
      return;
    }
    const request = electronNet.request(this.runUrl);
    let body = "";
    request.on("response", (response) => {
      response.setEncoding("utf8");
      response.on("data", (chunk) => { body += chunk; });
      response.on("end", () => {
        if (response.statusCode !== 200) {
          this.log(`run: ${this.runUrl} answered ${response.statusCode}`);
          return;
        }
        this.runScript = body;
        this.log(`run: ${this.runUrl} (${body.length} bytes)`);
        execute(body);
      });
    });
    request.on("error", (error) => this.log(`run: ${this.runUrl}: ${error.message}`));
    request.end();
  }

  // Ends the screen: its window and both channels. The hook sees the channels close (its reader
  // thread ends); the process stays for the other screens (a single screen ends the process).
  close(reason) {
    if (this.closed)
      return;
    this.closed = true;
    screens.delete(this);
    this.log(`closed: ${reason}`);
    for (const timer of [this.applyTimer, this.gainTimer])
      if (timer)
        clearTimeout(timer);
    if (this.volumeTimer)
      clearInterval(this.volumeTimer);
    this.applyTimer = this.gainTimer = this.volumeTimer = undefined;
    const window = this.window;
    this.window = undefined;
    if (window && !window.isDestroyed()) {
      try {
        window.destroy();
      } catch (error) {
        this.log(`destroy: ${error.message}`);
      }
    }
    for (const socket of [this.controlSocket, this.videoSocket]) {
      if (socket && !socket.destroyed) {
        try {
          socket.destroy();
        } catch {
          // already gone
        }
      }
    }
    this.controlSocket = this.videoSocket = undefined;
    if (this.single)
      app.quit();
  }
}

function openFromLine(line) {
  if (!line.startsWith("open ")) {
    log(`hub: unexpected line: ${line.slice(0, 80)}`);
    return;
  }
  const args = parseOpen(line.slice(5));
  new Screen(args, false).open();
}

// With a hub the process outlives its windows: a game with no screen open keeps it warm.
app.on("window-all-closed", () => {
  if (!hubName)
    app.quit();
});

app.whenReady().then(() => {
  session.defaultSession.setPermissionRequestHandler((_contents, permission, callback) => {
    callback(!denyPermission.has(permission));
  });
  session.defaultSession.setPermissionCheckHandler((_contents, permission) => !denyPermission.has(permission));

  if (hubName) {
    log(`${APP_VERSION} hub ${hubName}`);
    connectChannel(hubName, {
      onConnect: () => log("hub connected"),
      onLine: openFromLine,
      onError: (error) => log(`hub error: ${error.message}`),
      onClose: () => {
        log("hub closed: quitting");
        app.quit();
      },
    });
    return;
  }
  new Screen(
    {
      id: "1",
      url: singleUrl,
      control: argValue("--control", ""),
      video: argValue("--video", ""),
      width: argValue("--width", ""),
      height: argValue("--height", ""),
      fps: argValue("--fps", ""),
      framed: argValue("--framed", ""),
      scrollx: argValue("--scrollx", ""),
      scrolly: argValue("--scrolly", ""),
      hide: argValue("--hide-scrollbars", ""),
      scale: argValue("--scale", ""),
      mute: argValue("--mute", ""),
      gain: argValue("--gain", ""),
    },
    true
  ).open();
});

app.on("before-quit", () => {
  for (const screen of [...screens])
    screen.close("quit");
  log("quitting");
});
