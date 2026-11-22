// Passwords fixture worker: keeps one demo credential in storage, fills it from the context-menu item, and lets the
// popup ask for it. The credential is a placeholder, not a secret. Protocol: tests/extensions/README.md.
// Probes: "credential" and "fill" ({tabId}).

const SCHEMA = 1;
const DEMO = { username: "demo", password: "bdk-demo-password" };

chrome.runtime.onInstalled.addListener(() => {
  chrome.storage.local.set({ credential: DEMO });
  chrome.contextMenus.create(
    { id: "bdk-fill", title: "Fill login", contexts: ["page", "editable"] },
    () => {
      void chrome.runtime.lastError;
    },
  );
});

async function fill(tabId) {
  const { credential } = await chrome.storage.local.get("credential");
  try {
    return {
      succeeded: true,
      reply: await chrome.tabs.sendMessage(tabId, { fill: credential ?? DEMO }),
    };
  } catch (error) {
    return { succeeded: false, error: String(error?.message ?? error) };
  }
}

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId === "bdk-fill" && tab?.id !== undefined) fill(tab.id);
});

const probes = {
  credential: async () => (await chrome.storage.local.get("credential")).credential ?? null,
  fill: ({ tabId }) => fill(tabId),
};

async function run(request) {
  const probe = request?.probe;
  const fn = probes[probe];
  if (!fn) return { schema: SCHEMA, probe: String(probe), ok: false, error: "unknown probe" };
  try {
    return { schema: SCHEMA, probe, ok: true, result: await fn(request.args ?? {}) };
  } catch (error) {
    return { schema: SCHEMA, probe, ok: false, error: String(error?.message ?? error) };
  }
}

chrome.runtime.onMessage.addListener((request, _sender, reply) => {
  run(request).then(reply);
  return true;
});

chrome.runtime.onMessageExternal.addListener((request, _sender, reply) => {
  run(request).then(reply);
  return true;
});

globalThis.bdkProbe = (probe, args) => run({ probe, args });
