// An automation script, bundled into the application by `bdk build automation` and run in its automation
// host when the application, or a session the application allows, starts it. It reaches the browser only
// through `@bdk/automation`.
import { contents, task } from "@bdk/automation";

export default async function main() {
  const pages = await contents.list();
  const titles: string[] = [];
  for (const page of pages) {
    titles.push((await page.state()).title);
    task.progress(titles.length / pages.length);
  }
  return { pages: titles };
}
