import { contents, session, task } from "@bdk/automation";

/**
 * The M8 automation scenario: reads every page the application gave this session, one in each
 * space. With `account`, each page first opens that URL, so a site the person signed in to in one
 * space shows it there and not in the other (each space has a profile of its own).
 */
export default async function main(args: { account?: string } | null) {
  const pages = await contents.list();
  const report: { url: string; title: string; profile: string; signedIn: boolean }[] = [];
  for (const page of pages) {
    if (args?.account) await page.navigate(args.account, { waitUntil: "finished" });
    const state = await page.state();
    const signedIn = await page.evaluate<boolean>("document.cookie.includes('session=')");
    report.push({ url: state.url, title: state.title, profile: state.profile, signedIn });
    task.progress(report.length / pages.length, state.title);
  }
  return { session: session.name, pages: report };
}
