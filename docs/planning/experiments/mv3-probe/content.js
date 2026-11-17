chrome.runtime.sendMessage({ fromContent: true }, (r) => { document.documentElement.dataset.bdkProbe = r?.ack ? `ack:${r.tabId}` : "no-ack"; });
chrome.runtime.onMessage.addListener((msg, _s, reply) => { if (msg?.ping) reply({ pong: true, title: document.title, visibility: document.visibilityState }); });
