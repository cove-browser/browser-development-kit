import { isAbsolute, relative, resolve } from "node:path";
import type { Assets } from "../../assets.js";
import { type InitAnswers, productName, STARTERS, type StarterPage } from "./choices.js";

/** Embedded templates (`apps/templates/` in the repository). */
export const SWIFT_TEMPLATE_PREFIX = "templates/swift-browser/";
export const PAGES_TEMPLATE_PREFIX = "templates/pages/";

/** The BDK Swift package, resolved by URL at the CLI's version unless `--bdk-package` says otherwise. */
export const BDK_PACKAGE_URL = "https://github.com/cove-browser/browser-development-kit.git";

/** Where the generated project gets BDK's Swift package from. */
export type PackageSource =
  | { kind: "remote"; url: string; version: string }
  | { kind: "local"; path: string };

/** `--bdk-package`: a URL, or a path to a checkout of BDK (made relative to the project). */
export function packageSource(
  given: string | undefined,
  version: string,
  cwd: string,
  projectDir: string,
): PackageSource {
  if (given === undefined) return { kind: "remote", url: BDK_PACKAGE_URL, version };
  if (/^[a-z][a-z0-9+.-]*:\/\//i.test(given) || given.startsWith("git@")) {
    return { kind: "remote", url: given, version };
  }
  const absolute = isAbsolute(given) ? given : resolve(cwd, given);
  return { kind: "local", path: relative(projectDir, absolute) || "." };
}

export interface GenerateInput {
  answers: InitAnswers;
  assets: Assets;
  version: string;
  package: PackageSource;
  /** The `bdk` executable the build phases run; empty when it is not known. */
  bdkCli: string;
}

type Escape = (value: string) => string;

const xml: Escape = (v) =>
  v.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
const swiftString: Escape = (v) => v.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
const pbxString: Escape = (v) => v.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
const plain: Escape = (v) => v;

/** How a file's substituted values are escaped, by the file's kind. */
function escaperFor(path: string): Escape {
  if (/\.(plist|entitlements|xcscheme)$/.test(path)) return xml;
  if (path.endsWith(".swift")) return swiftString;
  if (path.endsWith(".pbxproj")) return pbxString;
  if (path.endsWith(".html")) return xml;
  return plain;
}

/** Replaces every `{{key}}` in `text`; an unknown key is left as it is. */
export function substitute(
  text: string,
  values: Record<string, string>,
  escapeValue: Escape = plain,
): string {
  return text.replace(/\{\{([A-Za-z]+)\}\}/g, (whole, key: string) =>
    key in values ? escapeValue(values[key] as string) : whole,
  );
}

/** A TOML basic string. */
function tomlString(value: string): string {
  return `"${value.replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}

/** `bdk.toml`: every answer that belongs in it, written out so the developer sees it (sdk/cli.md 4.4). */
export function bdkToml(answers: InitAnswers): string {
  const lines = [
    "# Written by `bdk init`. The reference is the bdk.toml page of the BDK documentation.",
    "[browser]",
    `name = ${tomlString(answers.name)}`,
    "# Written explicitly, so renaming the browser does not change its internal URLs.",
    `internal_scheme = ${tomlString(answers.scheme)}`,
    "",
    "[runtime]",
    `mode = ${tomlString(answers.runtime)}`,
  ];
  for (const starter of answers.starterPages) {
    lines.push("", `[pages.capabilities.${starter}]`);
    lines.push(`allow = [${STARTERS[starter].grants.map(tomlString).join(", ")}]`);
  }
  if (answers.automation) lines.push("", "[automation]", "enabled = true");
  return `${lines.join("\n")}\n`;
}

/** The project-relative files of one embedded template directory, by their template-relative names. */
function templateFiles(assets: Assets, prefix: string): Map<string, string> {
  const files = new Map<string, string>();
  for (const name of assets.list(prefix))
    files.set(name.slice(prefix.length), assets.readText(name));
  return files;
}

function pagesFile(assets: Assets, name: string): string {
  return assets.readText(`${PAGES_TEMPLATE_PREFIX}${name}`);
}

/** `@bdk/internal/x` as a page with no build step imports it: from where the runtime serves the SDK. */
function servedImports(source: string): string {
  return source.replace(/from "@bdk\/internal\/([a-z-]+)"/g, 'from "/@bdk/internal/$1.js"');
}

function plainPages(
  assets: Assets,
  starters: readonly StarterPage[],
  out: Map<string, string>,
): void {
  for (const starter of starters) {
    const { directory, title } = STARTERS[starter];
    const dir = `Pages/${directory}`;
    out.set(`${dir}/index.html`, substitute(pagesFile(assets, "plain/index.html"), { title }, xml));
    out.set(`${dir}/main.js`, pagesFile(assets, "plain/main.js"));
    out.set(`${dir}/style.css`, pagesFile(assets, "plain/style.css"));
    out.set(`${dir}/starter.js`, servedImports(pagesFile(assets, `starters/${starter}.js`)));
  }
}

interface Versions {
  common: Record<string, string>;
  language: Record<string, Record<string, string>>;
  styling: Record<string, Record<string, string>>;
  framework: Record<
    string,
    {
      dependencies: Record<string, string>;
      devDependencies: Record<string, string>;
      devDependenciesTs: Record<string, string>;
    }
  >;
}

function sorted(record: Record<string, string>): Record<string, string> {
  return Object.fromEntries(Object.entries(record).sort(([a], [b]) => (a < b ? -1 : 1)));
}

/** `Pages/vite.config.*`: the framework's plugin, Tailwind's when chosen, and BDK's preset last. */
function viteConfig(answers: InitAnswers): string {
  const imports = ['import { bdk } from "@bdk/vite";'];
  const plugins: string[] = [];
  if (answers.framework === "react") {
    imports.push('import react from "@vitejs/plugin-react";');
    plugins.push("react()");
  } else if (answers.framework === "preact") {
    imports.push('import preact from "@preact/preset-vite";');
    plugins.push("preact()");
  }
  if (answers.styling === "tailwind") {
    imports.push('import tailwindcss from "@tailwindcss/vite";');
    plugins.push("tailwindcss()");
  }
  plugins.push("bdk()");
  imports.push('import { defineConfig } from "vite";');
  return `// One Vite project for every page. BDK's preset builds each page directory on its own, as its own
// origin, and fails the build on output the internal-page content security policy would block.
${imports.sort().join("\n")}

export default defineConfig({
  plugins: [${plugins.join(", ")}],
});
`;
}

/** `Pages/tsconfig.json` for TypeScript pages; `@bdk/*` resolve through the link `bdk build codegen` makes. */
function tsconfig(answers: InitAnswers): string {
  const jsx =
    answers.framework === "react"
      ? { jsx: "react-jsx" }
      : answers.framework === "preact"
        ? { jsx: "react-jsx", jsxImportSource: "preact" }
        : {};
  return `${JSON.stringify(
    {
      compilerOptions: {
        target: "ES2022",
        module: "ESNext",
        moduleResolution: "bundler",
        lib: ["ES2022", "DOM", "DOM.Iterable"],
        strict: true,
        noEmit: true,
        skipLibCheck: true,
        types: ["vite/client"],
        ...jsx,
      },
      include: ["**/*.ts", "**/*.tsx"],
      exclude: ["node_modules"],
    },
    null,
    2,
  )}\n`;
}

function vitePages(input: GenerateInput, out: Map<string, string>): void {
  const { answers, assets } = input;
  const framework = answers.framework ?? "vanilla";
  const language = answers.language ?? "ts";
  const styling = answers.styling ?? "css";
  const versions = JSON.parse(pagesFile(assets, "versions.json")) as Versions;
  const pins = versions.framework[framework];
  if (pins === undefined) throw new Error(`versions.json has no framework ${framework}.`);
  const devDependencies = {
    ...versions.common,
    ...versions.language[language],
    ...versions.styling[styling],
    ...pins.devDependencies,
    ...(language === "ts" ? pins.devDependenciesTs : {}),
  };
  const packageJson = {
    name: `${productName(answers.name).toLowerCase()}-pages`,
    private: true,
    type: "module",
    ...(language === "ts" ? { scripts: { typecheck: "tsc --noEmit" } } : {}),
    ...(Object.keys(pins.dependencies).length === 0
      ? {}
      : { dependencies: sorted(pins.dependencies) }),
    devDependencies: sorted(devDependencies),
  };
  out.set("Pages/package.json", `${JSON.stringify(packageJson, null, 2)}\n`);
  out.set(`Pages/vite.config.${language}`, viteConfig(answers));
  if (language === "ts") out.set("Pages/tsconfig.json", tsconfig(answers));
  out.set("Pages/shared/style.css", pagesFile(assets, `vite/styling/${styling}/style.css`));
  const ext = language;
  for (const starter of answers.starterPages) {
    out.set(`Pages/shared/starters/${starter}.${ext}`, pagesFile(assets, `starters/${starter}.js`));
  }
  const frameworkDir = `${PAGES_TEMPLATE_PREFIX}vite/frameworks/${framework}/${language}/`;
  let entry: string | undefined;
  const pageFiles: [string, string][] = [];
  for (const [name, text] of templateFiles(assets, frameworkDir)) {
    if (name.startsWith("shared/")) out.set(`Pages/${name}`, text);
    else if (name.startsWith("page/")) {
      pageFiles.push([name.slice("page/".length), text]);
      if (name.startsWith("page/main.")) entry = name.slice("page/".length);
    }
  }
  if (entry === undefined)
    throw new Error(`The ${framework} ${language} template has no page/main file.`);
  for (const starter of answers.starterPages) {
    const { directory, title } = STARTERS[starter];
    out.set(
      `Pages/${directory}/index.html`,
      substitute(pagesFile(assets, "vite/index.html"), { title, entry }, xml),
    );
    for (const [name, text] of pageFiles) {
      out.set(`Pages/${directory}/${name}`, substitute(text, { starter }));
    }
  }
}

/** The package reference of the Xcode project, as the pbxproj template's placeholders take it. */
function packageValues(source: PackageSource): Record<string, string> {
  if (source.kind === "local") {
    return {
      packageSection: "XCLocalSwiftPackageReference",
      packageComment: `XCLocalSwiftPackageReference "${source.path}"`,
      packageFields: `\t\t\trelativePath = "${pbxString(source.path)}";`,
    };
  }
  return {
    packageSection: "XCRemoteSwiftPackageReference",
    packageComment: `XCRemoteSwiftPackageReference "browser-development-kit"`,
    packageFields: [
      `\t\t\trepositoryURL = "${pbxString(source.url)}";`,
      "\t\t\trequirement = {",
      "\t\t\t\tkind = exactVersion;",
      `\t\t\t\tversion = ${source.version};`,
      "\t\t\t};",
    ].join("\n"),
  };
}

/**
 * Every file of the new project, by project-relative path. Pure: it reads the embedded templates and
 * returns text, so every combination of choices can be generated in a test.
 */
export function generateProject(input: GenerateInput): Map<string, string> {
  const { answers, assets } = input;
  const product = productName(answers.name);
  const values: Record<string, string> = {
    name: answers.name,
    product,
    bundleId: answers.bundleId,
    scheme: answers.scheme,
    bdkCli: input.bdkCli,
    packageName: product.toLowerCase(),
  };
  const out = new Map<string, string>();
  for (const [name, text] of templateFiles(assets, SWIFT_TEMPLATE_PREFIX)) {
    // The scripts, and the package.json that brings Rolldown to bundle them, only with --automation.
    if ((name.startsWith("Automation/") || name === "package.json") && !answers.automation)
      continue;
    const path = (name === "gitignore" ? ".gitignore" : name).replaceAll("__PRODUCT__", product);
    if (path.endsWith(".pbxproj")) {
      // The package fields are pbxproj text already; the rest are values.
      const withPackage = substitute(text, packageValues(input.package));
      out.set(path, substitute(withPackage, values, pbxString));
    } else {
      out.set(path, substitute(text, values, escaperFor(path)));
    }
  }
  out.set("bdk.toml", bdkToml(answers));
  if (answers.pages === "plain") plainPages(assets, answers.starterPages, out);
  if (answers.pages === "vite") vitePages(input, out);
  return out;
}
