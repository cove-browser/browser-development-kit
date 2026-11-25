/**
 * The choices of `bdk init` (sdk/cli.md section 4.2), in the order they are asked, with their flags,
 * defaults and valid values. The prompts, the flags and the replayed command are all built from this.
 */

export const TEMPLATES = ["swift-browser"] as const;
export const PAGES = ["none", "plain", "vite"] as const;
/**
 * The frameworks offered. Each builds its DOM without assigning strings to an HTML sink, which the
 * internal-page policy blocks (`trusted-types` allows no policy). Vue, Svelte, Solid and Lit do assign
 * template strings to `innerHTML`, so they are not offered until BDK has a policy for them.
 */
export const FRAMEWORKS = ["vanilla", "react", "preact"] as const;
export const LANGUAGES = ["ts", "js"] as const;
export const STYLINGS = ["css", "tailwind"] as const;
export const STARTER_PAGES = ["new-tab", "settings", "history", "bookmarks", "downloads"] as const;
export const PACKAGE_MANAGERS = ["pnpm", "npm", "yarn", "bun"] as const;
export const RUNTIMES = ["prebuilt", "source"] as const;

export type Template = (typeof TEMPLATES)[number];
export type PagesKind = (typeof PAGES)[number];
export type Framework = (typeof FRAMEWORKS)[number];
export type Language = (typeof LANGUAGES)[number];
export type Styling = (typeof STYLINGS)[number];
export type StarterPage = (typeof STARTER_PAGES)[number];
export type PackageManager = (typeof PACKAGE_MANAGERS)[number];
export type RuntimeMode = (typeof RUNTIMES)[number];

/** Every answer, as the project is generated from them. */
export interface InitAnswers {
  name: string;
  /** As given (relative to the working directory) or derived from the name. */
  directory: string;
  bundleId: string;
  scheme: string;
  template: Template;
  pages: PagesKind;
  /** The next four only with `pages = vite`. */
  framework?: Framework;
  language?: Language;
  styling?: Styling;
  packageManager?: PackageManager;
  /** Empty with `pages = none`. */
  starterPages: StarterPage[];
  /** Install the page packages now (Vite pages only). */
  install: boolean;
  automation: boolean;
  runtime: RuntimeMode;
  git: boolean;
}

/** What each starter page is called on disk, shows as its title, and is granted in `bdk.toml`. */
export const STARTERS: Readonly<
  Record<StarterPage, { directory: string; title: string; grants: readonly string[] }>
> = {
  "new-tab": { directory: "NewTab", title: "New Tab", grants: ["history.read"] },
  settings: { directory: "Settings", title: "Settings", grants: ["settings.read"] },
  history: { directory: "History", title: "History", grants: ["history.read"] },
  bookmarks: { directory: "Bookmarks", title: "Bookmarks", grants: ["bookmarks.read"] },
  downloads: { directory: "Downloads", title: "Downloads", grants: ["downloads.read"] },
};

/** A reverse-DNS bundle identifier, as Xcode accepts one. */
export const BUNDLE_ID = /^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$/;

/** The application's product and target name: the display name with only letters and digits. */
export function productName(name: string): string {
  const product = name.replace(/[^A-Za-z0-9]/g, "");
  return /^[A-Za-z]/.test(product) ? product : `App${product}`;
}

/** The default directory: the name without spaces. */
export function defaultDirectory(name: string): string {
  return name.replace(/\s+/g, "");
}
