// API surface probe. Protocol: tests/extensions/README.md. Probes: "apiPresence" and "commands".
//
// "apiPresence" reports, for every namespace the compatibility matrix names, whether it exists in this worker's
// chrome object and, for two that can be called without side effects, whether a call succeeds. A namespace the
// product removes must be absent here (feature detection), not present and throwing.

const SCHEMA = 1;

const NAMESPACES = [
  "action",
  "alarms",
  "bookmarks",
  "browsingData",
  "commands",
  "contentSettings",
  "contextMenus",
  "cookies",
  "debugger",
  "declarativeNetRequest",
  "desktopCapture",
  "downloads",
  "enterprise",
  "fontSettings",
  "history",
  "i18n",
  "identity",
  "loginState",
  "management",
  "notifications",
  "offscreen",
  "omnibox",
  "permissions",
  "platformKeys",
  "privacy",
  "proxy",
  "runtime",
  "scripting",
  "search",
  "sessions",
  "sidePanel",
  "storage",
  "tabCapture",
  "tabGroups",
  "tabs",
  "topSites",
  "tts",
  "ttsEngine",
  "webNavigation",
  "webRequest",
  "webstore",
  "webstorePrivate",
  "windows",
];

// Members of a namespace whose absence the matrix records separately from the namespace.
const MEMBERS = [
  ["identity", "getAuthToken"],
  ["identity", "getProfileUserInfo"],
  ["identity", "launchWebAuthFlow"],
  ["downloads", "setShelfEnabled"],
  ["downloads", "setUiOptions"],
  ["tabs", "group"],
  ["tabs", "discard"],
  ["tabs", "duplicate"],
  ["tabs", "highlight"],
  ["action", "openPopup"],
  ["sidePanel", "open"],
  ["sidePanel", "getLayout"],
  ["permissions", "request"],
];

const message = (error) => String(error?.message ?? error);

const probes = {
  async apiPresence() {
    const namespaces = Object.fromEntries(
      NAMESPACES.map((name) => [name, typeof chrome[name] !== "undefined"]),
    );
    const members = Object.fromEntries(
      MEMBERS.map(([name, member]) => [
        `${name}.${member}`,
        typeof chrome[name]?.[member] === "function",
      ]),
    );
    return { namespaces, members };
  },

  // sessions.getRecentlyClosed returns an empty list in BDK (it keeps no session history).
  async recentlyClosed() {
    try {
      return { succeeded: true, sessions: await chrome.sessions.getRecentlyClosed() };
    } catch (error) {
      return { succeeded: false, error: message(error) };
    }
  },

  // The commands this extension declares and the bindings the product reports for them.
  async commands() {
    return chrome.commands.getAll();
  },
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

chrome.runtime.onMessage.addListener((request, _sender, reply) => {
  run(request).then(reply);
  return true;
});

chrome.runtime.onMessageExternal.addListener((request, _sender, reply) => {
  run(request).then(reply);
  return true;
});

chrome.omnibox?.onInputChanged.addListener((_text, suggest) => suggest([]));

globalThis.bdkProbe = (probe, args) => run({ probe, args });
