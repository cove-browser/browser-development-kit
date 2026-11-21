// Content script: acknowledges itself to the worker (T-EXT-01 assertion 1) and answers pings (assertion 7).
chrome.runtime.sendMessage({ fromContent: true, url: location.href }, (reply) => {
  const failure = chrome.runtime.lastError?.message;
  document.documentElement.dataset.bdkProbe = reply?.ack
    ? `ack:${reply.tabId}`
    : `no-ack:${failure ?? ""}`;
});

chrome.runtime.onMessage.addListener((message, _sender, reply) => {
  if (message?.ping) {
    reply({
      pong: true,
      url: location.href,
      title: document.title,
      visibility: document.visibilityState,
    });
  }
  return false;
});
