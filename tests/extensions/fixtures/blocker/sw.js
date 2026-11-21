// Blocker fixture worker. Blocking is done by the static rules; the worker only reports what is loaded.
// Protocol: tests/extensions/README.md. Probe: "rules". The harness checks blocking from the page: a request whose
// URL contains "bdk-blocked-ad" must fail and any other must succeed.

const SCHEMA = 1;

async function rules() {
  return {
    enabledRulesets: await chrome.declarativeNetRequest.getEnabledRulesets(),
    availableStaticRules: await chrome.declarativeNetRequest.getAvailableStaticRuleCount(),
  };
}

async function run(request) {
  if (request?.probe !== "rules") {
    return { schema: SCHEMA, probe: String(request?.probe), ok: false, error: "unknown probe" };
  }
  try {
    return { schema: SCHEMA, probe: "rules", ok: true, result: await rules() };
  } catch (error) {
    return { schema: SCHEMA, probe: "rules", ok: false, error: String(error?.message ?? error) };
  }
}

chrome.runtime.onMessageExternal.addListener((request, _sender, reply) => {
  run(request).then(reply);
  return true;
});

globalThis.bdkProbe = (probe, args) => run({ probe, args });
