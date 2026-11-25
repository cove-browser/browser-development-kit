// The new tab page: the pages visited most recently. Needs `history.read`, granted in bdk.toml.
import { history } from "@bdk/internal/history";

export const title = "New Tab";

export async function load() {
  const items = [];
  for await (const entry of history.search({ limit: 8 })) {
    items.push({ label: entry.title || entry.url, detail: entry.url, href: entry.url });
    if (items.length === 8) break;
  }
  return items;
}
