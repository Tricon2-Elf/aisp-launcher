#!/usr/bin/env node
// Native (Linux) Electron broker for the in-game hook running under Wine.
//
// Windows never talks to this. There the 32-bit hook CreateProcess's
// aisp.electron\electron.exe and they share named pipes. Wine named pipes are a
// Wine-internal protocol, not a Unix socket a stock Linux Electron can connect to,
// so under Wine the hook listens on 127.0.0.1 TCP and this process starts a normal
// Electron with the same app (aisp.electron/app). Any other off-screen renderer
// that speaks the same TCP protocol (BGRA frames on video, line commands on
// control; with framed=1 headed messages carrying frames and text lines) can replace Electron: point
// AISP_ELECTRON_NATIVE_BIN at it, or swap this file.
//
// Protocol, one line per connection:
//   hub 127.0.0.1:N        start one Electron connected to the hook's hub channel; the hook then
//                          opens every screen through that channel (aisp.electron/app/main.js)
//   open width=486 height=343 fps=30 control=127.0.0.1:N video=127.0.0.1:N
//        framed=0|1 scrollx=0 scrolly=0 hide=0 scale=1 mute=0 gain=1
//        url=http://…      the old form: one Electron for this one screen
// Reply: ok\n  or  err <text>\n
const net = require("net");
const path = require("path");
const { spawn } = require("child_process");

const listen = process.env.AISP_ELECTRON_NATIVE || "127.0.0.1:18764";
const colon = listen.lastIndexOf(":");
const bindHost = colon > 0 ? listen.slice(0, colon) : "127.0.0.1";
const bindPort = Number.parseInt(colon > 0 ? listen.slice(colon + 1) : "18764", 10);
const electronBin = process.env.AISP_ELECTRON_NATIVE_BIN || "electron";
const appDir = process.env.AISP_ELECTRON_APP || path.join(__dirname, "app");

function log(message) {
  process.stderr.write(`electron-host: ${message}\n`);
}

function parseOpen(line) {
  const urlAt = line.indexOf(" url=");
  const head = urlAt >= 0 ? line.slice(0, urlAt) : line;
  const url = urlAt >= 0 ? line.slice(urlAt + 5) : "";
  const args = {};
  const parts = head.split(" ");
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    const eq = part.indexOf("=");
    if (eq > 0)
      args[part.slice(0, eq)] = part.slice(eq + 1);
  }
  args.url = url;
  return args;
}

function spawnSession(args) {
  if (!args.url || !args.control || !args.video)
    throw new Error("open needs url, control, video");
  const childArgs = [
    "--no-sandbox",
    appDir,
    `--width=${args.width || 486}`,
    `--height=${args.height || 343}`,
    `--fps=${args.fps || 30}`,
    `--scrollx=${args.scrollx || 0}`,
    `--scrolly=${args.scrolly || 0}`,
    `--hide-scrollbars=${args.hide || 0}`,
    `--scale=${args.scale || 1}`,
    `--mute=${args.mute || 0}`,
    `--gain=${args.gain || 1}`,
    `--control=${args.control}`,
    `--video=${args.video}`,
    `--url=${args.url}`,
  ];
  if (args.framed)
    childArgs.push(`--framed=${args.framed}`);
  log(`spawn ${electronBin} ${childArgs.join(" ")}`);
  spawnElectron(childArgs);
}

function childEnv() {
  const env = { ...process.env };
  // The Wine launcher can start this file as `ELECTRON_RUN_AS_NODE=1 electron host.js`.
  // The real Chromium children must not inherit that.
  delete env.ELECTRON_RUN_AS_NODE;
  return env;
}

function spawnElectron(childArgs) {
  const child = spawn(electronBin, childArgs, {
    stdio: "ignore",
    detached: true,
    env: childEnv(),
  });
  child.unref();
}

function spawnHub(spec) {
  if (!/^\d+\.\d+\.\d+\.\d+:\d+$/.test(spec))
    throw new Error("hub needs 127.0.0.1:port");
  const childArgs = ["--no-sandbox", appDir, `--hub=${spec}`, "--wine=1"];
  log(`spawn ${electronBin} ${childArgs.join(" ")}`);
  spawnElectron(childArgs);
}

const server = net.createServer((socket) => {
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
      if (!line)
        continue;
      try {
        if (line.indexOf("hub ") === 0)
          spawnHub(line.slice(4).trim());
        else if (line.indexOf("open ") === 0)
          spawnSession(parseOpen(line.slice(5)));
        else
          throw new Error("expected hub or open");
        socket.write("ok\n");
      } catch (error) {
        log(`err ${error.message}`);
        socket.write(`err ${error.message}\n`);
      }
      socket.end();
      return;
    }
  });
});

server.on("error", (error) => {
  log(`listen failed: ${error.message}`);
  process.exit(1);
});
server.listen(bindPort, bindHost, () => log(`listening ${bindHost}:${bindPort} app ${appDir} bin ${electronBin}`));
