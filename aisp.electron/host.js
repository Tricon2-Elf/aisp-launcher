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
// Protocol, one line, url last:
//   open width=486 height=343 fps=30 control=127.0.0.1:N video=127.0.0.1:N
//        framed=0|1 scrollx=0 scrolly=0 hide=0 scale=1 mute=0 gain=1
//        url=http://…
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
  const child = spawn(electronBin, childArgs, {
    stdio: "ignore",
    detached: true,
    env: process.env,
  });
  child.unref();
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
        if (line.indexOf("open ") !== 0)
          throw new Error("expected open");
        spawnSession(parseOpen(line.slice(5)));
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
