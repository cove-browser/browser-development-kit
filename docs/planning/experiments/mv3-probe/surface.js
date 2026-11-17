// Shared by popup, side panel and options: shows what this surface believes the current context is.
chrome.runtime.sendMessage({ probe: "snapshot" }, (s) => {
  document.getElementById("out").textContent = JSON.stringify({ surface: document.title, activeCurrent: s?.activeCurrent, currentWindowId: s?.currentWindowId, windowCount: s?.windows?.length }, null, 1);
  document.documentElement.dataset.ready = "1";
});
