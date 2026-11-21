// Created on every start; a duplicate id on a restart is not an error here.
chrome.contextMenus.create(
  { id: "ui-item", title: "UI item", contexts: ["page"] },
  () => void chrome.runtime.lastError,
);

chrome.contextMenus.onClicked.addListener((info, tab) => {
  chrome.scripting.executeScript({
    target: { tabId: tab.id },
    func: (id) => {
      document.title = "menu-click:" + id;
    },
    args: [tab.id],
  });
});
