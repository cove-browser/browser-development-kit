import { contents, session, task } from "@bdk/automation";
import { commands } from "@bdk/commands";
import { events } from "@bdk/events";

/** A script as an application would write it: open a page, read it, report, store a note. */
export default async function main(args: { url: string }) {
  const page = await contents.create({ url: args.url });
  await page.waitFor({ load: "finished" });
  const text = await page.text();
  task.progress(0.5, "read page");
  const off = events.on("contents.navigated", (payload) => task.log(JSON.stringify(payload)), {
    contents: page.id,
  });
  const note = await commands.execute("app.notes.create", { body: text.slice(0, 5) });
  off.unsubscribe();
  await page.close();
  return { session: session.name, canNotes: session.can("app.notes.create"), note, text };
}
