// The downloads page: every download and its state. Needs `downloads.read`, granted in bdk.toml.
import { downloads } from "@bdk/internal/downloads";

export const title = "Downloads";

export async function load() {
  const items = [];
  for await (const item of downloads.list({ limit: 100 })) {
    const name = item.path?.split("/").pop() || item.url;
    const size = item.totalBytes > 0 ? ` of ${Math.round(item.totalBytes / 1024)} KB` : "";
    items.push({ label: name, detail: `${item.state}${size}`, href: item.url });
    if (items.length === 100) break;
  }
  return items;
}
