// Asks the worker to fill the login form of the tab the popup was opened for.
document.getElementById("fill").addEventListener("click", async () => {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  const reply = await chrome.runtime.sendMessage({ probe: "fill", args: { tabId: tab.id } });
  document.getElementById("out").textContent = JSON.stringify(reply.result);
  document.documentElement.dataset.result = JSON.stringify(reply.result);
});
