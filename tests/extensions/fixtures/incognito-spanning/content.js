// Tells the worker which context this page is in, so the harness can see which worker served it.
chrome.runtime.sendMessage({ fromContent: true, url: location.href }, () => {
  void chrome.runtime.lastError;
});
