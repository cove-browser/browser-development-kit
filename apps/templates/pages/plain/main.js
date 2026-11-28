// A page with no build step: plain ES modules. The SDK is served to every bundled page at
// /@bdk/internal/<module>.js. The content security policy forbids inline script and HTML strings, so the
// page builds its DOM with createElement and textContent.
import { load } from "./starter.js";

const status = document.querySelector("#status");
const list = document.querySelector("#items");

try {
  const items = await load();
  status.textContent = items.length === 0 ? "Nothing here yet." : "";
  for (const item of items) {
    const row = document.createElement("li");
    const label = item.href ? document.createElement("a") : document.createElement("span");
    if (item.href) label.href = item.href;
    label.textContent = item.label;
    const detail = document.createElement("small");
    detail.textContent = item.detail;
    row.append(label, detail);
    list.append(row);
  }
} catch (error) {
  status.textContent = `Could not load: ${error.message}`;
  status.classList.add("error");
}
