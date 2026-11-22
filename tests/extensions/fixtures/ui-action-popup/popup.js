// Reports the active tab, then grows, so that the size the popup asks for
// changes after it opened.
chrome.tabs.query({ active: true, currentWindow: true }).then((tabs) => {
  chrome.runtime.sendMessage({ kind: "popup-tab", tabId: tabs[0].id });
});
setTimeout(() => {
  const box = document.getElementById("box");
  box.style.width = "320px";
  box.style.height = "260px";
}, 400);
