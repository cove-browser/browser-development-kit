import { currentBdkPaths } from "../paths.js";
import { fileBackend, githubBackend, type PublishBackend } from "../runtime/backends.js";
import { isMain, repoRootFromHere, takeValue } from "../runtime/cli.js";
import { publishRuntime } from "../runtime/publisher.js";

const USAGE =
  "usage: publish.ts [--backend github|file] [--dest file:///dir] [--repo owner/name] [--config release] [--dry-run]\n" +
  "  github (default): gh release upload to release runtime-<source-key> in --repo (default: origin).\n" +
  "  file: copy the archive into the directory named by --dest.";

/** `chromium:publish`: archive runtimes/<key>, upload, record in chromium/pin.json. Clean tree only. */
async function main(argv: string[]): Promise<number> {
  let backendName = "github";
  let dest: string | undefined;
  let repo: string | undefined;
  let config = "release";
  let dryRun = false;
  try {
    for (let i = 0; i < argv.length; i++) {
      const a = argv[i];
      if (a === "--backend") backendName = takeValue(argv, i++, a);
      else if (a === "--dest") dest = takeValue(argv, i++, a);
      else if (a === "--repo") repo = takeValue(argv, i++, a);
      else if (a === "--config") config = takeValue(argv, i++, a);
      else if (a === "--dry-run") dryRun = true;
      else throw new Error(`unknown argument "${a}"\n${USAGE}`);
    }
    const repoRoot = repoRootFromHere(import.meta.url);
    let backend: PublishBackend;
    if (backendName === "github") {
      backend = githubBackend({ cwd: repoRoot, ...(repo === undefined ? {} : { repo }) });
    } else if (backendName === "file") {
      if (dest === undefined) throw new Error(`--backend file needs --dest file:///dir\n${USAGE}`);
      backend = fileBackend(dest);
    } else {
      throw new Error(`unknown backend "${backendName}" (github or file)`);
    }
    const { record, uploaded } = await publishRuntime({
      repoRoot,
      paths: currentBdkPaths(),
      backend,
      config,
      dryRun,
    });
    console.log(JSON.stringify(record, null, 2));
    console.log(
      uploaded
        ? "chromium:publish: uploaded and recorded in chromium/pin.json; review, commit, then tag."
        : "chromium:publish: dry run; nothing uploaded, pin.json unchanged.",
    );
    return 0;
  } catch (error) {
    console.error(`chromium:publish: ${(error as Error).message}`);
    return 1;
  }
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
