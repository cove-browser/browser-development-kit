// The popup tells the worker which tab it sees; the worker writes it into that
// tab's title, which the test reads (activeTab was granted by the click).
// A page of https://bdk.test asks for the popup, as chrome.action.openPopup
// callers do (the test cannot click).
chrome.runtime.onMessageExternal.addListener((message, sender, reply) => {
  chrome.action.openPopup().then(
    () => reply({ ok: true }),
    (error) => reply({ ok: false, error: String(error) }),
  );
  return true;
});

chrome.runtime.onMessage.addListener((message) => {
  if (message.kind !== "popup-tab") {
    return;
  }
  chrome.scripting.executeScript({
    target: { tabId: message.tabId },
    func: (id) => {
      document.title = "popup-tab:" + id;
    },
    args: [message.tabId],
  });
});
