// Shared by the popup, the side panel and the options page. It asks Chromium for the current tab and window from
// its own context (the worker's "current window" is not the same question) and reports the result to the worker,
// where the harness collects it with the "reports" probe (T-EXT-01 assertion 6).
const surface = document.title.replace("bdk probe ", "");

async function describe() {
  const [active] = await chrome.tabs.query({ active: true, currentWindow: true });
  const current = await chrome.windows.getCurrent();
  const fromWorker = await chrome.runtime.sendMessage({ probe: "snapshot" });
  return {
    surface,
    activeCurrent: active ? { id: active.id, windowId: active.windowId, url: active.url } : null,
    currentWindowId: current.id,
    workerActiveCurrent: fromWorker?.result?.activeCurrent ?? null,
    workerCurrentWindowId: fromWorker?.result?.currentWindowId ?? null,
    windowCount: fromWorker?.result?.windows?.length ?? null,
  };
}

describe().then(
  async (report) => {
    document.getElementById("out").textContent = JSON.stringify(report, null, 1);
    await chrome.runtime.sendMessage({ probe: "report", args: report });
    document.documentElement.dataset.ready = "1";
  },
  (error) => {
    document.getElementById("out").textContent = String(error);
    document.documentElement.dataset.ready = "error";
  },
);

// A popup closed by blur or window.close() is what assertion 6 observes from the host side; this records it too.
window.addEventListener("pagehide", () => {
  chrome.runtime.sendMessage({ probe: "report", args: { surface, closed: true } });
});
