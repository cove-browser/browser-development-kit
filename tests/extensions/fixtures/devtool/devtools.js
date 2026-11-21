// Runs in the DevTools page for each inspected page and adds one panel.
chrome.devtools.panels.create("BDK Tool", "", "panel.html", (panel) => {
  panel.onShown.addListener(() => {
    chrome.runtime.sendMessage({
      panelEvent: "shown",
      tabId: chrome.devtools.inspectedWindow.tabId,
    });
  });
});
