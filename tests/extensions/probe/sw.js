// Service worker: records tab and window events and answers probe requests (protocol: tests/extensions/README.md).
//
// MV3 workers are stopped when idle, so nothing here lives only in memory: events, content acknowledgements,
// surface reports and the context-menu state are kept in chrome.storage.session, and every listener is registered
// synchronously at the top level so the browser can restart the worker for an event.

const SCHEMA = 1;
const LIMIT = 500;

let queue = Promise.resolve();

/** Appends to a list kept in session storage. Writes are serialised so concurrent events do not overwrite each other. */
function append(key, value) {
  queue = queue
    .then(async () => {
      const stored = (await chrome.storage.session.get(key))[key] ?? [];
      stored.push(value);
      await chrome.storage.session.set({ [key]: stored.slice(-LIMIT) });
    })
    .catch(() => {});
  return queue;
}

const log = (type, detail) => append("events", { t: Date.now(), type, detail });

chrome.tabs.onCreated.addListener((tab) =>
  log("tabs.onCreated", { tabId: tab.id, windowId: tab.windowId }),
);
chrome.tabs.onActivated.addListener((info) => log("tabs.onActivated", info));
chrome.tabs.onAttached.addListener((tabId, info) => log("tabs.onAttached", { tabId, ...info }));
chrome.tabs.onDetached.addListener((tabId, info) => log("tabs.onDetached", { tabId, ...info }));
chrome.tabs.onMoved.addListener((tabId, info) => log("tabs.onMoved", { tabId, ...info }));
chrome.tabs.onRemoved.addListener((tabId, info) => log("tabs.onRemoved", { tabId, ...info }));
chrome.windows.onCreated.addListener((window) =>
  log("windows.onCreated", { windowId: window.id, type: window.type }),
);
chrome.windows.onRemoved.addListener((windowId) => log("windows.onRemoved", { windowId }));
chrome.windows.onFocusChanged.addListener((windowId) =>
  log("windows.onFocusChanged", { windowId }),
);
// With a default_popup the action never fires onClicked; the popup opening is what a click does. This is kept for a
// build that clears the popup with action.setPopup.
chrome.action.onClicked.addListener((tab) => log("action.onClicked", { tabId: tab.id }));

// A context-menu click is a user gesture, so it is the one place sidePanel.open can be expected to succeed.
chrome.contextMenus.onClicked.addListener((info, tab) => {
  log("contextMenus.onClicked", { menuItemId: info.menuItemId, tabId: tab?.id });
  if (tab?.id !== undefined) {
    chrome.sidePanel.open({ tabId: tab.id }).then(
      () => log("sidePanel.open.result", { succeeded: true }),
      (error) =>
        log("sidePanel.open.result", { succeeded: false, error: String(error?.message ?? error) }),
    );
  }
});

// Menu items are created on install, not on every worker start (a second create with the same id is an error).
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create(
    { id: "bdk-probe", title: "BDK probe item", contexts: ["page"] },
    () => {
      const failure = chrome.runtime.lastError?.message;
      append("menu", { id: "bdk-probe", created: failure === undefined, error: failure });
    },
  );
});

const brief = (tab) => ({
  id: tab.id,
  windowId: tab.windowId,
  index: tab.index,
  active: tab.active,
  url: tab.url,
});

const message = (error) => String(error?.message ?? error);

const probes = {
  // Assertion 1: which tabs' content scripts have acknowledged.
  acks: async () => (await chrome.storage.session.get("acks")).acks ?? [],

  // Assertions 2 and 10: the active tab of the current window, every tab, and every window of every type.
  async snapshot() {
    const [activeCurrent, all, current, windows] = await Promise.all([
      chrome.tabs.query({ active: true, currentWindow: true }),
      chrome.tabs.query({}),
      chrome.windows.getCurrent(),
      chrome.windows.getAll({
        populate: true,
        windowTypes: ["normal", "popup", "panel", "app", "devtools"],
      }),
    ]);
    return {
      activeCurrent: activeCurrent.map(brief),
      all: all.map(brief),
      currentWindowId: current.id,
      windows: windows.map((window) => ({
        id: window.id,
        focused: window.focused,
        type: window.type,
        incognito: window.incognito,
        state: window.state,
        tabIds: (window.tabs ?? []).map((tab) => tab.id),
      })),
    };
  },

  // Assertions 3, 4 and 8: tab and window events, and the click on the context-menu item.
  events: async () => (await chrome.storage.session.get("events")).events ?? [],
  async clearEvents() {
    await queue;
    await chrome.storage.session.remove("events");
    return true;
  },

  // Assertion 6: what each surface (popup, side panel, options page) believed the current tab was.
  reports: async () => (await chrome.storage.session.get("reports")).reports ?? [],
  async clearReports() {
    await queue;
    await chrome.storage.session.remove("reports");
    return true;
  },
  report: async (report) => {
    await append("reports", { t: Date.now(), ...report });
    return true;
  },

  // Assertion 5: succeeds only where a host permission or an activeTab grant exists for the tab.
  async inject({ tabId }) {
    try {
      const [injection] = await chrome.scripting.executeScript({
        target: { tabId },
        func: () => document.title,
      });
      return { succeeded: true, title: injection.result };
    } catch (error) {
      return { succeeded: false, error: message(error) };
    }
  },

  // Assertion 7: reach the content script of one tab, or of every tab.
  async pingContent({ tabId }) {
    try {
      return { succeeded: true, reply: await chrome.tabs.sendMessage(tabId, { ping: true }) };
    } catch (error) {
      return { succeeded: false, error: message(error) };
    }
  },
  async pingAll() {
    const tabs = await chrome.tabs.query({});
    return Promise.all(
      tabs.map(async (tab) => ({
        tabId: tab.id,
        ...(await probes.pingContent({ tabId: tab.id })),
      })),
    );
  },

  // Assertion 4: move a tab to another window.
  async move({ tabId, windowId, index }) {
    return brief(await chrome.tabs.move(tabId, { windowId, index: index ?? -1 }));
  },

  // Assertions 6 and 9, and the options page. Outside a user gesture, sidePanel.open is expected to fail with an
  // explicit error; that is a valid record. The gesture path is the context-menu handler above.
  async openPopup() {
    try {
      await chrome.action.openPopup();
      return { succeeded: true };
    } catch (error) {
      return { succeeded: false, error: message(error) };
    }
  },
  async openSidePanel({ tabId }) {
    try {
      await chrome.sidePanel.open({ tabId });
      return { succeeded: true };
    } catch (error) {
      return { succeeded: false, error: message(error) };
    }
  },
  async openOptions() {
    await chrome.runtime.openOptionsPage();
    return { succeeded: true };
  },

  // Assertion 8: whether the context-menu item was created.
  menu: async () => (await chrome.storage.session.get("menu")).menu ?? [],

  // Which of the namespaces this extension asked for are in its chrome object.
  apiPresence: async () =>
    Object.fromEntries(
      [
        "tabs",
        "windows",
        "scripting",
        "sidePanel",
        "action",
        "contextMenus",
        "storage",
        "webstorePrivate",
        "identity",
        "tabGroups",
        "sessions",
      ].map((name) => [name, typeof chrome[name] !== "undefined"]),
    ),
};

async function run(request) {
  const probe = request?.probe;
  const fn = probes[probe];
  if (!fn) return { schema: SCHEMA, probe: String(probe), ok: false, error: "unknown probe" };
  try {
    return { schema: SCHEMA, probe, ok: true, result: await fn(request.args ?? {}) };
  } catch (error) {
    return { schema: SCHEMA, probe, ok: false, error: message(error) };
  }
}

function onMessage(request, sender, reply) {
  if (request?.fromContent) {
    append("acks", { tabId: sender.tab?.id, frameId: sender.frameId, url: request.url });
    log("content.hello", { tabId: sender.tab?.id, frameId: sender.frameId });
    reply({ ack: true, tabId: sender.tab?.id });
    return false;
  }
  run(request).then(reply);
  return true;
}

chrome.runtime.onMessage.addListener(onMessage);
// A page on 127.0.0.1 or localhost (see externally_connectable) can drive the same probes.
chrome.runtime.onMessageExternal.addListener((request, _sender, reply) => {
  run(request).then(reply);
  return true;
});

// Callable from a DevTools or CDP evaluate in the worker.
globalThis.bdkProbe = (probe, args) => run({ probe, args });
