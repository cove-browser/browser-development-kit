// The settings page of the Arc-style example, at arc://settings: plain ES modules, no build step. The SDK is served to every
// bundled page at /@bdk/internal/<module>.js, and the page's grants are in bdk.toml.
import { page } from "/@bdk/internal/page.js";
import { settings } from "/@bdk/internal/settings.js";

const KEY = "privacy.doNotTrack";

const where = document.querySelector("#where");
const list = document.querySelector("#capabilities");
const toggle = document.querySelector("#do-not-track");
const status = document.querySelector("#status");

function show(message, isError = false) {
  status.textContent = message;
  status.classList.toggle("error", isError);
}

where.textContent = `This is the route "${page.route}", at ${page.url(page.route)}.`;

for (const capability of page.capabilities) {
  const item = document.createElement("li");
  const name = document.createElement("code");
  name.textContent = capability;
  item.append(name);
  list.append(item);
}

try {
  toggle.checked = (await settings.get({ key: KEY })) === true;
  toggle.disabled = !page.can("settings.write");
} catch (error) {
  show(`Could not read ${KEY}: ${error.message}`, true);
}

toggle.addEventListener("change", async () => {
  try {
    await settings.set({ key: KEY, value: toggle.checked });
    show(`Saved ${KEY} = ${toggle.checked}.`);
  } catch (error) {
    toggle.checked = !toggle.checked;
    show(`Could not save ${KEY}: ${error.message}`, true);
  }
});

settings.onChanged(
  (change) => {
    toggle.checked = change.newValue === true;
    show(`${KEY} is now ${change.newValue} (changed by ${change.source}).`);
  },
  { key: KEY },
);
