// DevTool fixture worker: stores what the panel reports. Protocol: tests/extensions/README.md. Probe: "panelEvents".

const SCHEMA = 1;

let queue = Promise.resolve();
function append(event) {
  queue = queue
    .then(async () => {
      const { panelEvents = [] } = await chrome.storage.session.get("panelEvents");
      panelEvents.push({ t: Date.now(), ...event });
      await chrome.storage.session.set({ panelEvents: panelEvents.slice(-200) });
    })
    .catch(() => {});
}

chrome.runtime.onMessage.addListener((request, _sender, reply) => {
  if (request?.panelEvent) {
    append(request);
    return false;
  }
  if (request?.probe === "panelEvents") {
    queue
      .then(() => chrome.storage.session.get("panelEvents"))
      .then(({ panelEvents = [] }) =>
        reply({ schema: SCHEMA, probe: "panelEvents", ok: true, result: panelEvents }),
      );
    return true;
  }
  reply({ schema: SCHEMA, probe: String(request?.probe), ok: false, error: "unknown probe" });
  return false;
});
