const { app, BrowserWindow } = require("electron");
const fs = require("node:fs");
const net = require("node:net");
const path = require("node:path");

const targetUrl = "https://www.twitch.tv/michimochievee";
const viewport = { width: 1024, height: 1024 };
const television = { x: 9, y: 15, width: 486, height: 343 };
const pipeArgument = process.argv.find((value) => value.startsWith("--aisp-pipe="));
const pipeName = pipeArgument?.slice("--aisp-pipe=".length);
const logPath = path.join(path.dirname(path.dirname(process.execPath)), "aisp.electron-helper.log");

function log(message) {
  fs.appendFileSync(logPath, `${new Date().toISOString()} ${message}\r\n`);
}

process.on("uncaughtException", (error) => log(`Uncaught exception: ${error.stack ?? error.message}`));
process.on("unhandledRejection", (error) => log(`Unhandled rejection: ${error?.stack ?? error}`));
process.on("exit", (code) => log(`Main process exiting: ${code}`));

if (!pipeName) {
  log("Missing --aisp-pipe argument");
  app.exit(2);
}

app.commandLine.appendSwitch("autoplay-policy", "no-user-gesture-required");
app.commandLine.appendSwitch("force-device-scale-factor", "1");
app.commandLine.appendSwitch("no-sandbox");

let socket;
let blocked = false;
let pendingFrame;
let browserWindow;

function writeLatest(frame) {
  if (!socket || socket.destroyed || blocked) {
    pendingFrame = frame;
    return;
  }

  blocked = !socket.write(frame);
}

function connectPipe() {
  socket = net.createConnection({ path: pipeName, allowHalfOpen: true });
  socket.once("connect", () => {
    log("Frame pipe connected");
    if (pendingFrame) {
      const frame = pendingFrame;
      pendingFrame = undefined;
      writeLatest(frame);
    }
  });
  socket.on("drain", () => {
    blocked = false;
    if (pendingFrame) {
      const frame = pendingFrame;
      pendingFrame = undefined;
      writeLatest(frame);
    }
  });
  socket.once("error", (error) => {
    log(`Frame pipe error: ${error.message}`);
  });
  socket.once("close", () => {
    log("Frame pipe closed");
    app.quit();
  });
}

function makePacket(bitmap) {
  const headerSize = 20;
  const packet = Buffer.allocUnsafe(headerSize + bitmap.length);
  packet.writeUInt32LE(0x50534941, 0);
  packet.writeUInt32LE(television.width, 4);
  packet.writeUInt32LE(television.height, 8);
  packet.writeUInt32LE(television.width * 4, 12);
  packet.writeUInt32LE(bitmap.length, 16);
  bitmap.copy(packet, headerSize);
  return packet;
}

const videoModeScript = `(() => {
  if (window.__aispVideoMode) return true;
  window.__aispVideoMode = setInterval(() => {
    const videos = [...document.querySelectorAll("video")];
    const video = videos.sort((left, right) => right.clientWidth * right.clientHeight - left.clientWidth * left.clientHeight)[0];
    if (!video) return;

    let overlay = document.getElementById("aisp-video-overlay");
    if (!overlay) {
      overlay = document.createElement("div");
      overlay.id = "aisp-video-overlay";
      document.body.appendChild(overlay);
    }

    const important = (element, property, value) => element.style.setProperty(property, value, "important");
    important(document.documentElement, "overflow", "hidden");
    important(document.documentElement, "background", "#000");
    important(document.body, "overflow", "hidden");
    important(document.body, "background", "#000");
    important(overlay, "position", "fixed");
    important(overlay, "left", "9px");
    important(overlay, "top", "15px");
    important(overlay, "width", "486px");
    important(overlay, "height", "343px");
    important(overlay, "overflow", "hidden");
    important(overlay, "background", "#000");
    important(overlay, "z-index", "2147483647");

    if (video.parentElement !== overlay) overlay.appendChild(video);
    important(video, "position", "absolute");
    important(video, "inset", "0");
    important(video, "width", "100%");
    important(video, "height", "100%");
    important(video, "max-width", "none");
    important(video, "max-height", "none");
    important(video, "object-fit", "contain");
    important(video, "background", "#000");
    video.muted = false;
    video.volume = 1;
    const playback = video.play();
    if (playback) playback.catch(() => {});
  }, 250);
  return true;
})()`;

app.whenReady().then(() => {
  connectPipe();

  browserWindow = new BrowserWindow({
    width: viewport.width,
    height: viewport.height,
    useContentSize: true,
    show: false,
    paintWhenInitiallyHidden: true,
    webPreferences: {
      backgroundThrottling: false,
      offscreen: { useSharedTexture: false },
    },
  });

  const chromeVersion = process.versions.chrome;
  browserWindow.webContents.setUserAgent(`Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${chromeVersion} Safari/537.36`);
  browserWindow.webContents.setFrameRate(30);
  browserWindow.webContents.on("paint", (_event, _dirtyRect, image) => {
    const size = image.getSize();
    if (size.width < television.x + television.width || size.height < television.y + television.height) return;
    writeLatest(makePacket(image.crop(television).toBitmap()));
  });
  browserWindow.webContents.on("did-finish-load", () => {
    browserWindow.webContents.executeJavaScript(videoModeScript).catch((error) => log(`Video script failed: ${error.message}`));
    log("Twitch page loaded");
  });
  browserWindow.webContents.on("did-fail-load", (_event, code, description, url) => log(`Load failed ${code}: ${description} (${url})`));
  browserWindow.webContents.on("render-process-gone", (_event, details) => log(`Renderer exited: ${details.reason}`));
  browserWindow.once("closed", () => log("Offscreen window closed"));

  browserWindow.loadURL(targetUrl).then(() => log("Electron offscreen renderer started")).catch((error) => log(`Initial navigation changed: ${error.message}`));
});
app.on("before-quit", () => log("Electron app quitting"));
