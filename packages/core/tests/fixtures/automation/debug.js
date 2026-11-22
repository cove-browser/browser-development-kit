// Fills a page's protocol sessions, pauses the page in the debugger, then goes quiet until its
// session is revoked: what the core and the engine must clean up without any further message.
const pending = new Map();
let next = 1;

globalThis.__bdkHost.onmessage = (text) => {
  const frame = JSON.parse(text);
  if (frame.type !== "result" && frame.type !== "error") return;
  const waiter = pending.get(frame.id);
  if (!waiter) return;
  pending.delete(frame.id);
  if (frame.type === "result") waiter.resolve(frame.value);
  else
    waiter.reject(new Error(`${frame.error.domain}.${frame.error.code}: ${frame.error.message}`));
};

function call(command, args = {}, extra = {}) {
  const id = next++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    globalThis.__bdkHost.send(JSON.stringify({ type: "call", id, command, args, ...extra }));
  });
}

export default async function main(args) {
  const page = await call("contents.create", { url: args.url });
  const on = { contents: page.id };
  await call("contents.waitFor", { load: "finished", timeoutMs: 10000 }, on);
  const sessions = [];
  for (let i = 0; i < 8; ++i) sessions.push((await call("debug.attach", {}, on)).session);
  await call("debug.send", { session: sessions[0], method: "Debugger.enable" }, on);
  await call("debug.send", { session: sessions[0], method: "Debugger.pause" }, on);
  console.log("paused");
  await new Promise(() => {});
}
