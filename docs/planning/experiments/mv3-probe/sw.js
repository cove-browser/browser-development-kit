// Service worker: records tab/window events and answers probe requests from the harness, popup or side panel.
const events = [];
const log = (type, detail) => events.push({ t: Date.now(), type, detail });
chrome.tabs.onActivated.addListener((i) => log("tabs.onActivated", i));
chrome.tabs.onAttached.addListener((id, i) => log("tabs.onAttached", { id, ...i }));
chrome.tabs.onDetached.addListener((id, i) => log("tabs.onDetached", { id, ...i }));
chrome.tabs.onMoved.addListener((id, i) => log("tabs.onMoved", { id, ...i }));
chrome.tabs.onRemoved.addListener((id, i) => log("tabs.onRemoved", { id, ...i }));
chrome.windows.onFocusChanged.addListener((id) => log("windows.onFocusChanged", { id }));
chrome.action.onClicked.addListener((tab) => log("action.onClicked", { tabId: tab.id }));
chrome.contextMenus.onClicked.addListener((info, tab) => log("contextMenus.onClicked", { menuItemId: info.menuItemId, tabId: tab?.id }));
chrome.runtime.onInstalled.addListener(() => chrome.contextMenus.create({ id: "bdk-probe", title: "BDK probe item", contexts: ["page"] }));

const brief = (t) => ({ id: t.id, windowId: t.windowId, index: t.index, active: t.active, url: t.url });
const probes = {
  async snapshot() {
    const [activeCurrent, all, win, wins] = await Promise.all([
      chrome.tabs.query({ active: true, currentWindow: true }), chrome.tabs.query({}),
      chrome.windows.getCurrent(), chrome.windows.getAll({ populate: true }),
    ]);
    return { activeCurrent: activeCurrent.map(brief), all: all.map(brief), currentWindowId: win.id,
      windows: wins.map((w) => ({ id: w.id, focused: w.focused, type: w.type, tabIds: w.tabs.map((t) => t.id) })) };
  },
  events: async () => events.slice(),
  clearEvents: async () => { events.length = 0; return true; },
  // Succeeds only where host permission or an activeTab grant exists for the tab.
  async inject({ tabId }) {
    try { const [r] = await chrome.scripting.executeScript({ target: { tabId }, func: () => document.title }); return { ok: true, title: r.result }; }
    catch (e) { return { ok: false, error: String(e.message ?? e) }; }
  },
  async pingContent({ tabId }) {
    try { return { ok: true, reply: await chrome.tabs.sendMessage(tabId, { ping: true }) }; }
    catch (e) { return { ok: false, error: String(e.message ?? e) }; }
  },
  async move({ tabId, windowId, index }) { return brief(await chrome.tabs.move(tabId, { windowId, index: index ?? -1 })); },
  async openPopup() { try { await chrome.action.openPopup(); return { ok: true }; } catch (e) { return { ok: false, error: String(e.message ?? e) }; } },
  async openSidePanel({ tabId }) { try { await chrome.sidePanel.open({ tabId }); return { ok: true }; } catch (e) { return { ok: false, error: String(e.message ?? e) }; } },
  async openOptions() { await chrome.runtime.openOptionsPage(); return { ok: true }; },
  async apiPresence() { return Object.fromEntries(["tabs", "windows", "scripting", "sidePanel", "action", "contextMenus", "storage", "webstorePrivate", "identity"].map((k) => [k, typeof chrome[k] !== "undefined"])); },
};
chrome.runtime.onMessage.addListener((msg, sender, reply) => {
  if (msg?.fromContent) { log("content.hello", { tabId: sender.tab?.id, frameId: sender.frameId }); reply({ ack: true, tabId: sender.tab?.id }); return false; }
  const fn = probes[msg?.probe];
  if (!fn) { reply({ error: "unknown probe" }); return false; }
  fn(msg.args ?? {}).then(reply, (e) => reply({ error: String(e.message ?? e) }));
  return true;
});
globalThis.bdkProbe = (name, args) => probes[name](args ?? {}); // callable from a DevTools/CDP evaluate in the worker
