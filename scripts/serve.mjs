#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The dev server every clockwork product needs and each used to write:
// static files with the two cross-origin isolation headers the SAB
// transport needs, and HTTPS on request — AudioWorklet is a secure-context
// feature, so a page reached over plain http on a LAN address boots no
// engine at all. A self-signed certificate is made once and kept.
//
//   node clockwork/scripts/serve.mjs [--root DIR] [--port N] [--host H] [--https [DIR]]
//
//   --root   directory to serve (default: the current directory)
//   --port   default 8000
//   --host   default 127.0.0.1; 0.0.0.0 for the LAN
//   --https  self-signed TLS; the certificate lives in DIR (default
//            ~/.clockwork/cert) and needs `openssl` the first time
//
// A URL for each interface is printed, so the one to open on a phone is on
// the screen.
import http from "node:http";
import https from "node:https";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { execFileSync } from "node:child_process";

const args = process.argv.slice(2);
const opt = (name, fallback) => { const i = args.indexOf(name); return i >= 0 ? (args[i + 1]?.startsWith("--") ? true : args[i + 1] ?? true) : fallback; };
const ROOT = path.resolve(String(opt("--root", process.cwd())));
const PORT = Number(opt("--port", 8000));
const HOST = String(opt("--host", "127.0.0.1"));
const HTTPS = opt("--https", false);
const TYPES = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".mjs": "text/javascript", ".css": "text/css",
  ".json": "application/json", ".wasm": "application/wasm", ".map": "application/json", ".svg": "image/svg+xml",
  ".png": "image/png", ".ico": "image/x-icon", ".flac": "audio/flac", ".wav": "audio/wav", ".mp3": "audio/mpeg",
  ".ogg": "audio/ogg", ".scsyndef": "application/octet-stream", ".taupatch": "application/octet-stream",
};

function certificate(dir) {
  const cert = path.join(dir, "cert.pem"), key = path.join(dir, "key.pem");
  if (!fs.existsSync(cert) || !fs.existsSync(key)) {
    fs.mkdirSync(dir, { recursive: true });
    const ips = lanAddresses().map((ip) => `IP:${ip}`).join(",");
    execFileSync("openssl", ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "365",
      "-subj", "/CN=clockwork-dev", "-addext", `subjectAltName=DNS:localhost,IP:127.0.0.1${ips ? "," + ips : ""}`], { stdio: "ignore" });
    console.log(`made a self-signed certificate in ${dir} — accept it once in each browser`);
  }
  return { cert: fs.readFileSync(cert), key: fs.readFileSync(key) };
}
function lanAddresses() {
  return Object.values(os.networkInterfaces()).flat().filter((a) => a && a.family === "IPv4" && !a.internal).map((a) => a.address);
}

const handler = (req, res) => {
  let urlPath = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (urlPath.endsWith("/")) urlPath += "index.html";
  const file = path.join(ROOT, urlPath);
  if (!file.startsWith(ROOT)) { res.writeHead(403); return res.end(); }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404, { "Content-Type": "text/plain" }); return res.end(`not found: ${urlPath}`); }
    res.writeHead(200, {
      "Content-Type": TYPES[path.extname(file).toLowerCase()] ?? "application/octet-stream",
      "Cross-Origin-Opener-Policy": "same-origin",
      "Cross-Origin-Embedder-Policy": "require-corp",
      "Cross-Origin-Resource-Policy": "cross-origin",
      "Cache-Control": "no-store",
    });
    res.end(data);
  });
};

const server = HTTPS
  ? https.createServer(certificate(HTTPS === true ? path.join(os.homedir(), ".clockwork", "cert") : String(HTTPS)), handler)
  : http.createServer(handler);
server.on("error", (e) => { console.error(e.code === "EADDRINUSE" ? `port ${PORT} is taken (another server?) — pick one with --port` : e.message); process.exit(1); });
server.listen(PORT, HOST, () => {
  const scheme = HTTPS ? "https" : "http";
  const hosts = HOST === "0.0.0.0" ? ["127.0.0.1", ...lanAddresses()] : [HOST];
  console.log(`serving ${ROOT}`);
  for (const h of hosts) console.log(`  ${scheme}://${h}:${PORT}/`);
  if (HOST !== "127.0.0.1" && !HTTPS) console.log("  (plain http off localhost: browsers refuse AudioWorklet there — add --https)");
});
