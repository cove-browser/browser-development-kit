import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { mkdir, readdir, readFile, stat, writeFile } from "node:fs/promises";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { cspIssues } from "@bdk/vite/tooling";
import { describe, expect, it } from "vitest";
import { createAssets } from "../../assets.js";
import { tempDir, testContext } from "../../test-support.js";
import { resolveConfig } from "../config/index.js";
import type { Prompter, Question } from "../context.js";
import { ExitCode } from "../exit-codes.js";
import { run } from "../run.js";
import { FRAMEWORKS, LANGUAGES, STARTER_PAGES, STARTERS, STYLINGS } from "./choices.js";

const assets = createAssets({ packaged: false });
const cliPackage = JSON.parse(
  readFileSync(fileURLToPath(new URL("../../../package.json", import.meta.url)), "utf8"),
);
const schema = JSON.parse(assets.readText("bdk.schema.json"));

/** Every file under `dir`, project-relative. */
async function files(dir: string): Promise<string[]> {
  const out: string[] = [];
  for (const entry of await readdir(dir, { recursive: true, withFileTypes: true })) {
    if (entry.isFile()) out.push(relative(dir, join(entry.parentPath, entry.name)));
  }
  return out.sort();
}

/** A prompter that answers from a list and records the questions. */
function scripted(answers: string[]): Prompter & { asked: Question[] } {
  const asked: Question[] = [];
  return {
    asked,
    ask(question) {
      asked.push(question);
      return Promise.resolve(answers.shift() ?? "");
    },
  };
}

const BASE = ["init", "App", "--name", "Cove Browser", "--bundle-id", "com.example.cove"];

/** A packaged bdk, so the generated project's BDK_CLI is set and nothing is noted about it. */
const PACKAGED = "/usr/local/bin/bdk";

async function init(argv: string[], over: Parameters<typeof testContext>[0] = {}) {
  const cwd = await tempDir("bdk-init-");
  const ctx = testContext({ cwd, assets, resolveConfig, ...over });
  const code = await run(argv, ctx);
  return { cwd, dir: join(cwd, "App"), code, ctx };
}

interface Combination {
  pages: "none" | "plain" | "vite";
  framework?: string;
  language?: string;
  styling?: string;
  automation: boolean;
  runtime: "prebuilt" | "source";
}

/** Every combination of the choices that change what is generated. */
function combinations(): Combination[] {
  const out: Combination[] = [];
  for (const automation of [false, true]) {
    for (const runtime of ["prebuilt", "source"] as const) {
      out.push({ pages: "none", automation, runtime });
      out.push({ pages: "plain", automation, runtime });
      for (const framework of FRAMEWORKS) {
        for (const language of LANGUAGES) {
          for (const styling of STYLINGS) {
            out.push({ pages: "vite", framework, language, styling, automation, runtime });
          }
        }
      }
    }
  }
  return out;
}

function flagsFor(c: Combination): string[] {
  const argv = [...BASE, "--pages", c.pages, "--runtime", c.runtime, "--yes"];
  if (c.pages !== "none") argv.push("--starter-pages", STARTER_PAGES.join(","));
  if (c.pages === "vite") {
    argv.push(
      "--framework",
      c.framework ?? "",
      "--language",
      c.language ?? "",
      "--styling",
      c.styling ?? "",
    );
  }
  if (c.automation) argv.push("--automation");
  if (c.pages === "vite" || c.automation) argv.push("--package-manager", "pnpm");
  return argv;
}

describe("bdk init: a generated application for every combination of choices", () => {
  const all = combinations();

  it("covers each framework, language and styling", () => {
    expect(all).toHaveLength(2 * 2 * (2 + FRAMEWORKS.length * LANGUAGES.length * STYLINGS.length));
  });

  it.each(all)("%o", async (c) => {
    const prompter = scripted([]);
    const { dir, code, ctx } = await init(flagsFor(c), { prompter, executable: PACKAGED });
    expect(ctx.err.join("")).toBe("");
    expect(code).toBe(ExitCode.Success);
    // A run with every flag asks nothing, even with a terminal.
    expect(prompter.asked).toEqual([]);
    const written = await files(dir);

    // The Xcode project, its scheme and the sources, named after the product.
    expect(written).toEqual(
      expect.arrayContaining([
        "CoveBrowser.xcodeproj/project.pbxproj",
        "CoveBrowser.xcodeproj/xcshareddata/xcschemes/CoveBrowser.xcscheme",
        "Sources/AppDelegate.swift",
        "Sources/BrowserWindow.swift",
        "Resources/Info.plist",
        "Resources/App.entitlements",
        "Resources/Assets.xcassets/AppIcon.appiconset/Contents.json",
        "Tests/CoveBrowserTests.swift",
        "BuildPhases/bdk-runtime.sh",
        ".gitignore",
        "bdk.toml",
      ]),
    );
    expect(written.some((f) => f.startsWith("Automation/"))).toBe(c.automation);
    // Scripts are bundled with the project's own Rolldown, the version the CLI is built against.
    expect(written.includes("package.json")).toBe(c.automation);
    if (c.automation) {
      const pkg = JSON.parse(await readFile(join(dir, "package.json"), "utf8"));
      expect(pkg.devDependencies.rolldown).toBe(cliPackage.devDependencies.rolldown);
      expect(ctx.execCalls).toContainEqual({ command: "pnpm", args: ["install"], cwd: dir });
    }
    for (const file of written) {
      expect(file).not.toContain("__PRODUCT__");
      expect(await readFile(join(dir, file), "utf8"), file).not.toMatch(/\{\{[A-Za-z]+\}\}/);
    }
    expect((await stat(join(dir, "BuildPhases", "bdk-bundle.sh"))).mode & 0o111).not.toBe(0);
    const pbxproj = await readFile(join(dir, "CoveBrowser.xcodeproj", "project.pbxproj"), "utf8");
    expect(pbxproj).toContain('PRODUCT_BUNDLE_IDENTIFIER = "com.example.cove"');
    expect(pbxproj).toContain("kind = exactVersion;");
    if (process.platform === "darwin") {
      // An old-style property list: plutil parses it as Xcode would.
      execFileSync("plutil", [
        "-lint",
        "-s",
        join(dir, "CoveBrowser.xcodeproj", "project.pbxproj"),
      ]);
      execFileSync("plutil", ["-lint", "-s", join(dir, "Resources", "Info.plist")]);
    }

    // bdk.toml resolves: the pages become bundled routes with the grants each starter needs.
    const resolved = await resolveConfig({ projectDir: dir, schema, bdkVersion: "1.2.3" });
    expect(resolved.diagnostics.filter((d) => d.severity === "error")).toEqual([]);
    const body = resolved.resolved ?? {};
    const routes = (body.routes ?? {}) as Record<string, { mode: string; capabilities?: string[] }>;
    expect((body.runtime as { mode: string }).mode).toBe(c.runtime);
    expect((body.browser as { internal_scheme: string }).internal_scheme).toBe("covebrowser");
    for (const starter of STARTER_PAGES) {
      if (c.pages === "none") {
        expect(routes[starter]?.mode).not.toBe("bundled");
      } else {
        expect(routes[starter]).toMatchObject({
          mode: "bundled",
          capabilities: [...STARTERS[starter].grants],
        });
        const html = await readFile(
          join(dir, "Pages", STARTERS[starter].directory, "index.html"),
          "utf8",
        );
        expect(cspIssues(html, "index.html")).toEqual([]);
      }
    }
    expect((body.automation as { enabled: boolean }).enabled).toBe(c.automation);

    if (c.pages === "vite") {
      const pkg = JSON.parse(await readFile(join(dir, "Pages", "package.json"), "utf8"));
      const versions = { ...pkg.dependencies, ...pkg.devDependencies } as Record<string, string>;
      expect(versions.vite).toBe("8.3.8");
      // Each template pins its versions exactly.
      for (const version of Object.values(versions)) expect(version).toMatch(/^\d+\.\d+\.\d+$/);
      expect(written).toContain(`Pages/vite.config.${c.language}`);
      expect(written).toContain(`Pages/shared/starters/new-tab.${c.language}`);
      if (c.styling === "tailwind") expect(versions["@tailwindcss/vite"]).toBeDefined();
      expect(ctx.execCalls).toContainEqual({
        command: "pnpm",
        args: ["install"],
        cwd: join(dir, "Pages"),
      });
    }
    if (c.pages === "plain") {
      const starter = await readFile(join(dir, "Pages", "History", "starter.js"), "utf8");
      expect(starter).toContain('from "/@bdk/internal/history.js"');
    }
    expect(ctx.execCalls).toContainEqual({ command: "git", args: ["init", "--quiet"], cwd: dir });
  });
});

describe("bdk init: prompts", () => {
  it("asks for each choice in turn and takes the defaults on Enter", async () => {
    // sdk/cli.md 4.2's order, except that the automation host is asked before the package manager,
    // which automation scripts need too.
    const prompter = scripted([
      "Cove",
      "",
      "com.example.cove",
      "",
      "",
      "vite",
      "react",
      "",
      "",
      "",
      "",
      "pnpm",
      "no",
      "",
      "",
    ]);
    const { cwd, code, ctx } = await init(["init"], { prompter, executable: PACKAGED });
    expect(ctx.err.join("")).toBe("");
    expect(code).toBe(0);
    expect(prompter.asked.map((q) => q.message)).toEqual([
      "Display name",
      "Directory",
      "Bundle identifier",
      "Internal scheme",
      "Template",
      "Internal pages",
      "Framework",
      "Language",
      "Styling",
      "Starter pages",
      "Automation host",
      "Package manager",
      "Install packages now",
      "Runtime",
      "Create a Git repository",
    ]);
    expect(prompter.asked[3]?.default).toBe("cove");
    const dir = join(cwd, "Cove");
    expect(await readFile(join(dir, "Pages", "NewTab", "main.tsx"), "utf8")).toContain(
      "createRoot",
    );
    // "no" to installing: nothing ran but git.
    expect(ctx.execCalls.map((c) => c.command)).toEqual(["git"]);
    expect(ctx.out.join("")).toContain(
      "bdk init Cove --name Cove --bundle-id com.example.cove --scheme cove --template swift-browser --pages vite --framework react --language ts --styling css --starter-pages new-tab --package-manager pnpm --no-install --runtime prebuilt --yes",
    );
  });

  it("skips the questions a flag answers or an earlier answer makes irrelevant", async () => {
    const prompter = scripted(["", "", "", "", ""]);
    const { code } = await init([...BASE, "--pages", "none"], { prompter });
    expect(code).toBe(0);
    expect(prompter.asked.map((q) => q.message)).toEqual([
      "Internal scheme",
      "Template",
      "Automation host",
      "Runtime",
      "Create a Git repository",
    ]);
  });

  it("asks again after an invalid answer", async () => {
    const prompter = scripted(["not an id", "com.example.ok", "", "", "", "", "", ""]);
    const { ctx, code } = await init(["init", "App", "--name", "Ok"], { prompter });
    expect(code).toBe(0);
    expect(ctx.err.join("")).toContain('"not an id" is not a bundle identifier');
  });

  it("asks nothing with --yes or --no-input, or without a terminal, and fails naming a required flag", async () => {
    for (const extra of [["--yes"], ["--no-input"]]) {
      const prompter = scripted([]);
      const { code, ctx } = await init(["init", "App", "--name", "Cove", ...extra], { prompter });
      expect(prompter.asked).toEqual([]);
      expect(code).toBe(ExitCode.Usage);
      expect(ctx.err.join("")).toContain("Pass --bundle-id");
    }
    const { code, ctx } = await init(["init", "App", "--bundle-id", "com.example.cove"]);
    expect(code).toBe(ExitCode.Usage);
    expect(ctx.err.join("")).toContain("Pass --name");
  });
});

describe("bdk init: usage and safety", () => {
  it.each([
    [["--pages", "web"], '--pages: "web" is not one of none, plain, vite'],
    [
      ["--pages", "vite", "--framework", "vue"],
      '--framework: "vue" is not one of vanilla, react, preact',
    ],
    [["--pages", "plain", "--framework", "react"], "--framework applies only with --pages vite"],
    [
      ["--pages", "none", "--starter-pages", "history"],
      "--starter-pages applies only with --pages plain or vite",
    ],
    [["--pages", "plain", "--starter-pages", "history,nope"], "nope not among"],
    [
      ["--package-manager", "npm"],
      "--package-manager applies only with --pages vite or --automation",
    ],
    [["--scheme", "http"], '"http" is a reserved scheme'],
    [["--scheme", "Cove"], "is not a URL scheme"],
    [["--watch"], "--watch applies to `bdk build` only"],
  ])("%j", async (extra, message) => {
    const { code, ctx } = await init([...BASE, "--yes", ...extra]);
    expect(code).toBe(ExitCode.Usage);
    expect(ctx.err.join("")).toContain(message);
  });

  it("refuses a directory that is not empty, and with --force adds files without overwriting any", async () => {
    const cwd = await tempDir("bdk-init-");
    await mkdir(join(cwd, "App"));
    await writeFile(join(cwd, "App", "bdk.toml"), "# mine\n");
    const ctx = testContext({ cwd, assets, resolveConfig });
    expect(await run([...BASE, "--yes"], ctx)).toBe(ExitCode.ProjectFailure);
    expect(ctx.err.join("")).toContain("[init.directory_not_empty]");
    const forced = testContext({ cwd, assets, resolveConfig });
    expect(await run([...BASE, "--yes", "--force", "--json"], forced)).toBe(0);
    expect(await readFile(join(cwd, "App", "bdk.toml"), "utf8")).toBe("# mine\n");
    const document = JSON.parse(forced.out.join(""));
    expect(document.result.kept).toEqual(["bdk.toml"]);
    expect(document.diagnostics.map((d: { code: string }) => d.code)).toContain("init.file_kept");
  });

  it("skips git with --no-git and installing with --no-install or --offline", async () => {
    const vite = [...BASE, "--yes", "--pages", "vite", "--package-manager", "npm"];
    expect((await init([...vite, "--no-git", "--no-install"])).ctx.execCalls).toEqual([]);
    const offline = await init([...vite, "--no-git", "--offline"]);
    expect(offline.ctx.execCalls).toEqual([]);
    expect(offline.ctx.err.join("")).toContain("[init.install_skipped]");
  });

  it("reports a failed install or git as a warning and still exits 0", async () => {
    const { code, ctx } = await init(
      [...BASE, "--yes", "--pages", "vite", "--package-manager", "npm"],
      {
        exec: () => Promise.resolve({ code: 1, stdout: "", stderr: "no network" }),
      },
    );
    expect(code).toBe(0);
    expect(ctx.err.join("")).toContain("[init.install_failed]");
    expect(ctx.err.join("")).toContain("[init.git_failed]");
  });

  it("refers to a local BDK checkout with --bdk-package <path>, relative to the project", async () => {
    const cwd = await tempDir("bdk-init-");
    const ctx = testContext({ cwd, assets, resolveConfig });
    expect(await run([...BASE, "--yes", "--bdk-package", "../bdk"], ctx)).toBe(0);
    const pbxproj = await readFile(
      join(cwd, "App", "CoveBrowser.xcodeproj", "project.pbxproj"),
      "utf8",
    );
    expect(pbxproj).toContain("isa = XCLocalSwiftPackageReference;");
    expect(pbxproj).toContain('relativePath = "../../bdk";');
  });

  it("writes the packaged executable into BDK_CLI, and notes when there is none", async () => {
    const packaged = await init([...BASE, "--yes"], { executable: "/opt/bdk/bin/bdk" });
    const pbxproj = await readFile(
      join(packaged.dir, "CoveBrowser.xcodeproj", "project.pbxproj"),
      "utf8",
    );
    expect(pbxproj).toContain('BDK_CLI = "/opt/bdk/bin/bdk";');
    expect(packaged.ctx.err.join("")).not.toContain("init.cli_unset");
    const unpackaged = await init([...BASE, "--yes"]);
    expect(unpackaged.ctx.err.join("")).toContain("[init.cli_unset]");
  });

  it("escapes a display name in Swift, property lists and TOML", async () => {
    const { dir, code } = await init([
      "init",
      "App",
      "--name",
      'Q "Browser" & <Co>',
      "--bundle-id",
      "com.example.q",
      "--scheme",
      "q",
      "--yes",
    ]);
    expect(code).toBe(0);
    expect(await readFile(join(dir, "Sources", "AppDelegate.swift"), "utf8")).toContain(
      '"Q \\"Browser\\" & <Co> cannot start"',
    );
    expect(await readFile(join(dir, "Resources", "Info.plist"), "utf8")).toContain(
      "<string>Q &quot;Browser&quot; &amp; &lt;Co&gt;</string>",
    );
    const { resolved = {} } = await resolveConfig({ projectDir: dir, schema });
    expect((resolved.browser as { name: string }).name).toBe('Q "Browser" & <Co>');
    expect(await files(dir)).toContain("QBrowserCo.xcodeproj/project.pbxproj");
  });
});
