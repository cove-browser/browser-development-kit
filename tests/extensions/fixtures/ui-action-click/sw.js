chrome.action.onClicked.addListener(async (tab) => {
  await chrome.action.setBadgeText({ text: "7", tabId: tab.id });
  await chrome.scripting.executeScript({
    target: { tabId: tab.id },
    func: (id) => {
      document.title = "injected:" + id;
    },
    args: [tab.id],
  });
});
