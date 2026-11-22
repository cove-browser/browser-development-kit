// Driven by the engine tests from a page with a user gesture; the answer is the page title.
async function activeTab() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  return tab;
}
window.run = async (what) => {
  try {
    const win = await chrome.windows.getCurrent();
    const tab = await activeTab();
    let r = null;
    if (what === "openWindow") {
      await chrome.sidePanel.open({ windowId: win.id });
    } else if (what === "setTabPanel") {
      await chrome.sidePanel.setOptions({ tabId: tab.id, path: "tab.html", enabled: true });
    } else if (what === "openTab") {
      await chrome.sidePanel.open({ tabId: tab.id });
    } else if (what === "layout") {
      r = (await chrome.sidePanel.getLayout()).side;
    } else if (what === "close") {
      await chrome.sidePanel.close({ windowId: win.id });
    }
    document.title = "ok:" + JSON.stringify(r);
  } catch (e) {
    document.title = "err:" + e.message;
  }
};
