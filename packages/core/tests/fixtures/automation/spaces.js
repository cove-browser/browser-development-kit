// The M8 automation scenario: drives two pages the application already shows, in two windows (an
// Arc-style application's two spaces), one of them signed in to the test site. It opens the
// account page in each and reports what each page says, and the page's own world sees the
// session cookie only where the user signed in.
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

async function account(contents, url) {
  await call("contents.navigate", { url, waitUntil: "finished" }, { contents });
  const text = await call(
    "contents.evaluate",
    { expression: "document.getElementById('who').textContent" },
    { contents },
  );
  return text.value;
}

export default async function main(args) {
  const listed = await call("contents.list", {});
  return {
    listed: listed.items.length,
    signedIn: await account(args.signedIn, args.url),
    signedOut: await account(args.signedOut, args.url),
  };
}
