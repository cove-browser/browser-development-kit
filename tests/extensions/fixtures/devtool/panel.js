// Evaluates the inspected page's title and reports the result to the worker, where the harness collects it.
chrome.devtools.inspectedWindow.eval("document.title", (result, exception) => {
  const report = {
    panelEvent: "evaluated",
    tabId: chrome.devtools.inspectedWindow.tabId,
    result: result ?? null,
    exception: exception ? (exception.value ?? exception.description ?? String(exception)) : null,
  };
  document.getElementById("out").textContent = JSON.stringify(report);
  document.documentElement.dataset.result = JSON.stringify(report);
  chrome.runtime.sendMessage(report);
});
