// The devtools page reports that it ran, with the tab it inspects.
globalThis.ready = null;
chrome.runtime.onMessage.addListener((message) => {
  globalThis.ready = message;
});
