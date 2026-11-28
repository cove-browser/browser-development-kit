// The bookmarks page: the bookmarks at the top of each root folder. Needs `bookmarks.read`, granted in
// bdk.toml.
import { bookmarks } from "@bdk/internal/bookmarks";

export const title = "Bookmarks";

export async function load() {
  const items = [];
  for (const root of await bookmarks.roots()) {
    for (const node of await bookmarks.children({ id: root.id })) {
      items.push({
        label: node.title || node.url || "Untitled",
        detail: node.isFolder ? `Folder in ${root.title}` : (node.url ?? ""),
        ...(node.url ? { href: node.url } : {}),
      });
    }
  }
  return items;
}
