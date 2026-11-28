// The history page: what was visited, newest first. Needs `history.read`, granted in bdk.toml.
import { history } from "@bdk/internal/history";

export const title = "History";

export async function load() {
  const items = [];
  for await (const entry of history.search({ limit: 100 })) {
    items.push({
      label: entry.title || entry.url,
      detail: new Date(entry.lastVisit).toLocaleString(),
      href: entry.url,
    });
    if (items.length === 100) break;
  }
  return items;
}
