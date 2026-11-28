// The settings page: what this page is granted and the setting it reads. Needs `settings.read`, granted in
// bdk.toml. Add `settings.write` there to change settings with `settings.set`.
import { page } from "@bdk/internal/page";
import { settings } from "@bdk/internal/settings";

export const title = "Settings";

export async function load() {
  const doNotTrack = await settings.get({ key: "privacy.doNotTrack" });
  return [
    { label: "Send a Do Not Track request", detail: doNotTrack === true ? "On" : "Off" },
    ...page.capabilities.map((capability) => ({
      label: capability,
      detail: "Granted to this page",
    })),
  ];
}
