// Incognito probe worker (shared with the "split" fixture; the manifests differ in "incognito" only).
// Protocol: tests/extensions/README.md. Probes: "context" and "hellos".
//
// Which worker answers, and whether regular and private pages reach the same one, is the observation: a worker
// reports whether it runs in an incognito context, its own start counter (chrome.storage.local is separate per
// context in split mode and shared in spanning mode) and which pages said hello to it.

const SCHEMA = 1;

let queue = Promise.resolve();
function update(key, change) {
  queue = queue
    .then(async () => {
      const stored = (await chrome.storage.local.get(key))[key];
      await chrome.storage.local.set({ [key]: change(stored) });
    })
    .catch(() => {});
  return queue;
}

// The count of worker starts seen by this context's storage.
update("starts", (n) => (n ?? 0) + 1);

const probes = {
  async context() {
    await queue;
    const windows = await chrome.windows.getAll({ populate: true });
    const stored = await chrome.storage.local.get("starts");
    return {
      inIncognitoContext: chrome.extension.inIncognitoContext,
      allowedIncognitoAccess: await chrome.extension.isAllowedIncognitoAccess(),
      manifestIncognito: chrome.runtime.getManifest().incognito,
      starts: stored.starts ?? 0,
      windows: windows.map((window) => ({
        id: window.id,
        incognito: window.incognito,
        tabIds: (window.tabs ?? []).map((tab) => tab.id),
      })),
    };
  },
  async hellos() {
    await queue;
    return (await chrome.storage.local.get("hellos")).hellos ?? [];
  },
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

chrome.runtime.onMessage.addListener((request, sender, reply) => {
  if (request?.fromContent) {
    update("hellos", (list = []) =>
      [
        ...list,
        {
          tabId: sender.tab?.id,
          incognito: sender.tab?.incognito,
          inIncognitoContext: chrome.extension.inIncognitoContext,
          url: request.url,
        },
      ].slice(-100),
    );
    reply({ ack: true });
    return false;
  }
  run(request).then(reply);
  return true;
});

chrome.runtime.onMessageExternal.addListener((request, _sender, reply) => {
  run(request).then(reply);
  return true;
});

globalThis.bdkProbe = (probe, args) => run({ probe, args });
