// The T-AUT-02 acceptance script: runs with no page open, drives two pages it creates, evaluates in
// an isolated world and in the page's own world, receives events and calls an application command.
const pending = new Map();
const listeners = new Map();
let next = 1;

globalThis.__bdkHost.onmessage = (text) => {
  const frame = JSON.parse(text);
  if (frame.type === "result" || frame.type === "error") {
    const waiter = pending.get(frame.id);
    if (!waiter) return;
    pending.delete(frame.id);
    if (frame.type === "result") waiter.resolve(frame.value);
    else
      waiter.reject(new Error(`${frame.error.domain}.${frame.error.code}: ${frame.error.message}`));
  } else if (frame.type === "event") {
    listeners.get(frame.id)?.(frame.payload);
  }
};

function call(command, args = {}, extra = {}) {
  const id = next++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    globalThis.__bdkHost.send(JSON.stringify({ type: "call", id, command, args, ...extra }));
  });
}

function on(event, listener) {
  const id = next++;
  listeners.set(id, listener);
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    globalThis.__bdkHost.send(JSON.stringify({ type: "subscribe", id, event }));
  });
}

export default async function main(args) {
  const navigated = [];
  await on("contents.navigated", (payload) => navigated.push(payload.url));
  const a = await call("contents.create", { url: args.a });
  const b = await call("contents.create", { url: args.b });
  await call("contents.waitFor", { load: "finished", timeoutMs: 10000 }, { contents: a.id });
  await call("contents.waitFor", { load: "finished", timeoutMs: 10000 }, { contents: b.id });
  const isolated = await call(
    "contents.evaluate",
    { expression: "typeof window.pageVar" },
    { contents: a.id },
  );
  const page = await call(
    "contents.evaluate",
    { expression: "typeof window.pageVar", world: "page" },
    { contents: a.id },
  );
  const title = await call(
    "contents.evaluate",
    { expression: "document.title" },
    { contents: b.id },
  );
  const note = await call("app.notes.create", { title: title.value });
  await new Promise((resolve) => setTimeout(resolve, 10));
  console.log("done");
  return {
    pages: [a.id, b.id],
    navigations: navigated.length,
    isolated: isolated.value,
    page: page.value,
    title: title.value,
    note,
  };
}
