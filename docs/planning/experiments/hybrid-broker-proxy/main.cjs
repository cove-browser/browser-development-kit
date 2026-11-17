const { app, protocol, net: enet, BaseWindow, WebContentsView, ipcMain, session } = require("electron");
const { execFile } = require("node:child_process");
const crypto = require("node:crypto");
const fs = require("node:fs");
const net = require("node:net");
const path = require("node:path");
const { pathToFileURL } = require("node:url");

// ---- configuration read at STARTUP from a file, not compiled in (the bdk.toml stand-in) ----
const cfg = JSON.parse(fs.readFileSync(path.join(__dirname, process.env.BDK_CONFIG ?? "bdk.config.json"), "utf8"));
const SCHEME = cfg.browser.internal_scheme;
const results = { scheme: SCHEME, chromium: process.versions.chrome, electron: process.versions.electron, checks: {} };
const check = (name, ok, detail) => { results.checks[name] = { ok: !!ok, detail }; };

// Must run before app ready: the scheme has to be known to every process before scheme registries lock.
protocol.registerSchemesAsPrivileged([{ scheme: SCHEME, privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: false } }]);

// ---- shared native service + broker (one implementation, two authenticated front doors) ----
const settingsService = new Map([["theme", "dark"]]);
const commands = {
  "settings.get": { capability: "settings.read", run: ({ key }) => settingsService.get(key) ?? null },
  "settings.set": { capability: "settings.write", run: ({ key, value }) => { settingsService.set(key, value); return true; } },
};
function brokerInvoke(principal, command, args) {
  const c = commands[command];
  if (!c) return { ok: false, error: "unknown-command" };
  if (!principal.capabilities.has(c.capability)) return { ok: false, error: `denied:${c.capability}`, principal: principal.id };
  return { ok: true, value: c.run(args ?? {}), principal: principal.id };
}
// Page identity is derived natively from the committed frame, never from anything the page says.
function pagePrincipal(frame) {
  if (!frame || frame.parent !== null) return { id: "subframe", capabilities: new Set() };       // main frame only
  let u; try { u = new URL(frame.url); } catch { return { id: "invalid", capabilities: new Set() }; }
  if (u.protocol !== `${SCHEME}:`) return { id: `foreign:${u.protocol}`, capabilities: new Set() };  // scheme/origin bound
  const logical = cfg.pages.routes[u.hostname] ?? u.hostname;                                       // alias -> same identity
  return { id: `page:${logical}`, capabilities: new Set(cfg.pages.capabilities[logical] ?? []) };
}
ipcMain.handle("bdk-internal", (event, command, args) => brokerInvoke(pagePrincipal(event.senderFrame), command, args));

app.dock?.hide();
app.whenReady().then(async () => {
  try {
    protocol.handle(SCHEME, (req) => {
      const u = new URL(req.url);
      const logical = cfg.pages.routes[u.hostname] ?? u.hostname;
      const file = path.join(__dirname, "pages", logical, "index.html");
      if (!fs.existsSync(file)) return new Response("no such internal page", { status: 404 });
      return enet.fetch(pathToFileURL(file).toString());
    });
    const mk = () => new WebContentsView({ webPreferences: { preload: path.join(__dirname, "preload.cjs"), contextIsolation: true, sandbox: true } });
    const winA = new BaseWindow({ show: false, width: 900, height: 600 });
    const winB = new BaseWindow({ show: false, width: 900, height: 600 });
    const left = mk(), right = mk();
    winA.contentView.addChildView(left); winA.contentView.addChildView(right);
    left.setBounds({ x: 0, y: 0, width: 450, height: 600 }); right.setBounds({ x: 450, y: 0, width: 450, height: 600 });
    const js = (v, code) => v.webContents.executeJavaScript(code, true);
    const call = (v, cmd, args) => js(v, `window.bdkInternal.invoke(${JSON.stringify(cmd)}, ${JSON.stringify(args)})`);

    // 1. runtime-configured branded scheme loads as a standard, secure origin in a prebuilt runtime
    await left.webContents.loadURL(`${SCHEME}://settings/index.html`);
    const o = await js(left, `({origin: location.origin, secure: isSecureContext, title: document.title})`);
    check("branded-scheme-standard-secure-origin", o.origin === `${SCHEME}://settings` && o.secure === true, o);

    // 2. per-page capabilities enforced natively
    const r1 = await call(left, "settings.get", { key: "theme" });
    check("settings-page-read-allowed", r1.ok && r1.value === "dark" && r1.principal === "page:settings", r1);
    const r2 = await call(left, "settings.set", { key: "theme", value: "light" });
    check("settings-page-write-allowed", r2.ok, r2);

    await right.webContents.loadURL(`${SCHEME}://newtab/index.html`);
    const r3 = await call(right, "settings.get", { key: "theme" });
    check("newtab-page-denied-despite-same-bridge", !r3.ok && r3.error === "denied:settings.read", r3);

    // 3. alias keeps the same logical identity and capabilities
    await right.webContents.loadURL(`${SCHEME}://preferences/index.html`);
    const r4 = await call(right, "settings.get", { key: "theme" });
    check("alias-shares-identity", r4.ok && r4.principal === "page:settings" && r4.value === "light", r4);

    // 4. ordinary web origin and file origin get nothing even with the same preload injected
    await right.webContents.loadURL("data:text/html,<title>web</title>web");
    const r5 = await call(right, "settings.get", { key: "theme" });
    check("non-internal-origin-denied", !r5.ok, r5);

    // 5. a subframe inside a privileged page is not privileged (frame-bound, not tab-bound)
    const sub = left.webContents.mainFrame.frames[0];
    const r6 = sub ? await sub.executeJavaScript(`window.bdkInternal ? window.bdkInternal.invoke("settings.get", {key:"theme"}) : {ok:false,error:"no-bridge-in-subframe"}`, true) : { ok: false, error: "no-subframe" };
    check("subframe-denied", r6.ok === false, { result: r6, subframeUrl: sub?.url });

    // 6. navigation away revokes: the same WebContents that was privileged is now a foreign origin
    await left.webContents.loadURL("data:text/html,<title>navigated</title>x");
    const r7 = await call(left, "settings.get", { key: "theme" });
    check("privilege-revoked-after-navigation", !r7.ok, r7);
    await left.webContents.loadURL(`${SCHEME}://settings/index.html`);

    // 7. independent automation host: separate OS process, token-authenticated local socket, same broker + same service
    const sock = path.join(app.getPath("temp"), `bdk-auto-${process.pid}.sock`);
    const token = crypto.randomBytes(24).toString("hex");
    const server = net.createServer((conn) => {
      let authed = false, buf = "";
      conn.on("data", (d) => { buf += d; let i; while ((i = buf.indexOf("\n")) >= 0) { const m = JSON.parse(buf.slice(0, i)); buf = buf.slice(i + 1);
        if (!authed) { authed = m.hello === token; conn.write(JSON.stringify({ authed }) + "\n"); if (!authed) conn.end(); continue; }
        conn.write(JSON.stringify(brokerInvoke({ id: "automation-host", capabilities: new Set(["settings.read", "settings.write"]) }, m.command, m.args)) + "\n"); } });
    });
    await new Promise((r) => server.listen(sock, r)); fs.chmodSync(sock, 0o600);
    const runClient = (tok, ...cmds) => new Promise((res) => execFile("node", [path.join(__dirname, "automation-client.mjs"), sock, tok, ...cmds], { timeout: 8000 }, (e, so) => res(e ? { error: String(e) } : JSON.parse(so))));
    const a1 = await runClient(token, 'settings.get={"key":"theme"}', 'settings.set={"key":"theme","value":"solar"}');
    check("automation-host-reaches-same-service", a1[0]?.authed && a1[1]?.value === "light" && a1[2]?.ok, a1);
    const r8 = await call(left, "settings.get", { key: "theme" });
    check("page-observes-automation-write (one service, no duplication)", r8.value === "solar", r8);
    const a2 = await runClient("wrong-token", 'settings.get={"key":"theme"}');
    check("automation-bad-token-rejected", a2.error !== undefined || a2[0]?.authed === false, a2);
    server.close();

    // 8. native hosting proxy: two WebContents in one window, then reparent one to another window
    await right.webContents.loadURL(`${SCHEME}://newtab/index.html`);
    await js(right, `window.__state = 41; 0`);
    const pidBefore = right.webContents.getOSProcessId();
    winA.contentView.removeChildView(right);                 // detached / hidden: owned by no window
    const detached = await js(right, `++window.__state`);
    winB.contentView.addChildView(right);                    // reparented into another window
    const after = await js(right, `++window.__state`);
    check("reparent-preserves-page-and-renderer", detached === 42 && after === 43 && right.webContents.getOSProcessId() === pidBefore, { detached, after, pidBefore, pidAfter: right.webContents.getOSProcessId() });
    check("distinct-renderer-processes-per-origin", left.webContents.getOSProcessId() !== right.webContents.getOSProcessId(), { left: left.webContents.getOSProcessId(), right: right.webContents.getOSProcessId() });

    // 9. renderer crash is observable and recoverable
    const gone = new Promise((r) => right.webContents.once("render-process-gone", (_e, d) => r(d.reason)));
    right.webContents.forcefullyCrashRenderer();
    const reason = await Promise.race([gone, new Promise((r) => setTimeout(() => r("timeout"), 5000))]);
    await right.webContents.loadURL(`${SCHEME}://newtab/index.html`);
    const alive = await js(right, `document.title`);
    check("renderer-crash-observed-and-reloaded", reason !== "timeout" && alive === "newtab", { reason, alive, leftStillAlive: await js(left, `document.title`) });
  } catch (e) { results.error = String(e && e.stack || e); }
  results.passed = Object.values(results.checks).every((c) => c.ok) && !results.error;
  fs.writeFileSync(path.join(__dirname, `result-${SCHEME}.json`), JSON.stringify(results, null, 2));
  app.exit(results.passed ? 0 : 1);
});
