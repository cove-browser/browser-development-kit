chrome.devtools.panels.create("BDK Panel", "", "panel.html", () => {
  chrome.runtime.sendMessage({ panel: "created", tabId: chrome.devtools.inspectedWindow.tabId });
});
