import type { InitFlags } from "../args.js";
import { deriveScheme, isReservedScheme } from "../config/checks.js";
import type { Context, Question } from "../context.js";
import { onPath } from "../doctor/machine.js";
import { usageError } from "../exit-codes.js";
import {
  BUNDLE_ID,
  defaultDirectory,
  FRAMEWORKS,
  type Framework,
  type InitAnswers,
  LANGUAGES,
  type Language,
  PACKAGE_MANAGERS,
  PAGES,
  type PackageManager,
  type PagesKind,
  RUNTIMES,
  type RuntimeMode,
  STARTER_PAGES,
  STYLINGS,
  type StarterPage,
  type Styling,
  TEMPLATES,
  type Template,
} from "./choices.js";

/** `Display name [Cove] (a, b): ` as the terminal shows it. */
export function formatQuestion(question: Question): string {
  const choices = question.choices === undefined ? "" : ` (${question.choices.join(", ")})`;
  const fallback = question.default === undefined ? "" : ` [${question.default}]`;
  return `${question.message}${choices}${fallback}: `;
}

/** An answer is either a value or the reason it is not one. */
type Check<T> = (text: string) => { value: T } | { problem: string };

const SCHEME = /^[a-z][a-z0-9+.-]*$/;

function oneOf<T extends string>(values: readonly T[]): Check<T> {
  return (text) =>
    (values as readonly string[]).includes(text)
      ? { value: text as T }
      : { problem: `"${text}" is not one of ${values.join(", ")}.` };
}

const yesNo: Check<boolean> = (text) => {
  if (/^(y|yes)$/i.test(text)) return { value: true };
  if (/^(n|no)$/i.test(text)) return { value: false };
  return { problem: `Answer yes or no, not "${text}".` };
};

const nonEmpty: Check<string> = (text) =>
  text.trim() === "" ? { problem: "It cannot be empty." } : { value: text.trim() };

const bundleId: Check<string> = (text) =>
  BUNDLE_ID.test(text)
    ? { value: text }
    : { problem: `"${text}" is not a bundle identifier such as com.example.browser.` };

const scheme: Check<string> = (text) => {
  if (!SCHEME.test(text)) {
    return { problem: `"${text}" is not a URL scheme: lower case, starting with a letter.` };
  }
  if (isReservedScheme(text)) return { problem: `"${text}" is a reserved scheme.` };
  return { value: text };
};

const starterPages: Check<StarterPage[]> = (text) => {
  const names = text
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean);
  const unknown = names.filter((n) => !(STARTER_PAGES as readonly string[]).includes(n));
  if (names.length === 0) return { problem: "Choose at least one page." };
  if (unknown.length > 0) {
    return { problem: `${unknown.join(", ")} not among ${STARTER_PAGES.join(", ")}.` };
  }
  return { value: [...new Set(names)] as StarterPage[] };
};

/** The package manager that ran `bdk` (npm sets `npm_config_user_agent`), else the first one installed. */
async function defaultPackageManager(env: NodeJS.ProcessEnv): Promise<PackageManager> {
  const agent = env.npm_config_user_agent?.split("/")[0];
  if (agent !== undefined && (PACKAGE_MANAGERS as readonly string[]).includes(agent)) {
    return agent as PackageManager;
  }
  for (const manager of PACKAGE_MANAGERS) {
    if ((await onPath(env, manager)) !== undefined) return manager;
  }
  return "npm";
}

/**
 * Collects every answer: a flag answers its question; otherwise a person at a terminal is asked; otherwise
 * (`--yes`, `--no-input`, no terminal) the default is taken, and a choice with no default fails naming its
 * flag. Invalid flag values and flags whose question does not apply are usage errors.
 */
export async function collectAnswers(
  ctx: Context,
  flags: InitFlags,
  positional: string | undefined,
): Promise<InitAnswers> {
  const prompter = flags.yes || flags["no-input"] ? undefined : ctx.prompter;

  async function choose<T>(
    flag: string,
    given: string | undefined,
    check: Check<T>,
    question: Question,
    fallback: string | undefined,
  ): Promise<T> {
    if (given !== undefined) {
      const result = check(given);
      if ("problem" in result) throw usageError(`--${flag}: ${result.problem}`);
      return result.value;
    }
    if (prompter !== undefined) {
      for (;;) {
        const answer = await prompter.ask(
          fallback === undefined ? question : { ...question, default: fallback },
        );
        const result = check(answer === "" && fallback !== undefined ? fallback : answer);
        if ("value" in result) return result.value;
        ctx.stderr(`${result.problem}\n`);
      }
    }
    if (fallback === undefined) {
      throw usageError(
        `${question.message} has no default, and nothing can be asked.`,
        `Pass --${flag}.`,
      );
    }
    const result = check(fallback);
    if ("problem" in result)
      throw usageError(`${question.message}: ${result.problem}`, `Pass --${flag}.`);
    return result.value;
  }

  /** A switch flag answers yes (`--automation`) or no (`--no-git`); otherwise it is asked. */
  async function confirm(
    flag: string,
    given: true | undefined,
    means: boolean,
    message: string,
    fallback: boolean,
  ) {
    if (given) return means;
    return choose(flag, undefined, yesNo, { message }, fallback ? "yes" : "no");
  }

  const name = await choose("name", flags.name, nonEmpty, { message: "Display name" }, undefined);
  const directory = await choose(
    "directory",
    positional,
    nonEmpty,
    { message: "Directory" },
    defaultDirectory(name),
  );
  const bundle = await choose(
    "bundle-id",
    flags["bundle-id"],
    bundleId,
    { message: "Bundle identifier" },
    undefined,
  );
  const derived = deriveScheme(name);
  const internalScheme = await choose(
    "scheme",
    flags.scheme,
    scheme,
    { message: "Internal scheme" },
    SCHEME.test(derived) && !isReservedScheme(derived) ? derived : undefined,
  );
  const template = await choose<Template>(
    "template",
    flags.template,
    oneOf(TEMPLATES),
    { message: "Template", choices: TEMPLATES },
    "swift-browser",
  );
  const pages = await choose<PagesKind>(
    "pages",
    flags.pages,
    oneOf(PAGES),
    { message: "Internal pages", choices: PAGES },
    "none",
  );

  const viteOnly = ["framework", "language", "styling"] as const;
  if (pages !== "vite") {
    const misplaced = viteOnly.filter((flag) => flags[flag] !== undefined);
    if (misplaced.length > 0) {
      throw usageError(
        `--${misplaced[0]} applies only with --pages vite, and the pages are "${pages}".`,
        "Leave it out, or choose --pages vite.",
      );
    }
  }
  if (pages === "none" && flags["starter-pages"] !== undefined) {
    throw usageError(
      '--starter-pages applies only with --pages plain or vite, and the pages are "none".',
      "Leave it out, or choose --pages plain or --pages vite.",
    );
  }

  const answers: InitAnswers = {
    name,
    directory,
    bundleId: bundle,
    scheme: internalScheme,
    template,
    pages,
    starterPages: [],
    install: false,
    automation: false,
    runtime: "prebuilt",
    git: true,
  };
  if (pages === "vite") {
    answers.framework = await choose<Framework>(
      "framework",
      flags.framework,
      oneOf(FRAMEWORKS),
      { message: "Framework", choices: FRAMEWORKS },
      "vanilla",
    );
    answers.language = await choose<Language>(
      "language",
      flags.language,
      oneOf(LANGUAGES),
      { message: "Language", choices: LANGUAGES },
      "ts",
    );
    answers.styling = await choose<Styling>(
      "styling",
      flags.styling,
      oneOf(STYLINGS),
      { message: "Styling", choices: STYLINGS },
      "css",
    );
  }
  if (pages !== "none") {
    answers.starterPages = await choose(
      "starter-pages",
      flags["starter-pages"],
      starterPages,
      { message: "Starter pages", choices: STARTER_PAGES },
      "new-tab",
    );
  }
  answers.automation = await confirm(
    "automation",
    flags.automation,
    true,
    "Automation host",
    false,
  );
  // Vite pages and automation scripts are built from packages: Vite, and Rolldown, whose native
  // binding the packaged bdk cannot carry.
  if (pages === "vite" || answers.automation) {
    answers.packageManager = await choose<PackageManager>(
      "package-manager",
      flags["package-manager"],
      oneOf(PACKAGE_MANAGERS),
      { message: "Package manager", choices: PACKAGE_MANAGERS },
      await defaultPackageManager(ctx.env),
    );
    answers.install = await confirm(
      "no-install",
      flags["no-install"],
      false,
      "Install packages now",
      true,
    );
  } else {
    const misplaced = (["package-manager", "no-install"] as const).find(
      (f) => flags[f] !== undefined,
    );
    if (misplaced !== undefined) {
      throw usageError(
        `--${misplaced} applies only with --pages vite or --automation.`,
        "Leave it out, or choose Vite pages or the automation host.",
      );
    }
  }
  answers.runtime = await choose<RuntimeMode>(
    "runtime",
    flags.runtime,
    oneOf(RUNTIMES),
    { message: "Runtime", choices: RUNTIMES },
    "prebuilt",
  );
  answers.git = await confirm("no-git", flags["no-git"], false, "Create a Git repository", true);
  return answers;
}

/** Quotes a word for a POSIX shell when it needs it. */
function shellWord(word: string): string {
  return /^[A-Za-z0-9_@%+=:,./-]+$/.test(word) ? word : `'${word.replace(/'/g, "'\\''")}'`;
}

/** The command that makes the same project with no questions (sdk/cli.md 4.2, "Replay"). */
export function replayCommand(answers: InitAnswers, extra: { bdkPackage?: string } = {}): string {
  const words = [
    "bdk",
    "init",
    answers.directory,
    "--name",
    answers.name,
    "--bundle-id",
    answers.bundleId,
    "--scheme",
    answers.scheme,
    "--template",
    answers.template,
    "--pages",
    answers.pages,
  ];
  if (answers.pages === "vite") {
    words.push("--framework", answers.framework ?? "vanilla");
    words.push("--language", answers.language ?? "ts");
    words.push("--styling", answers.styling ?? "css");
  }
  if (answers.pages !== "none") words.push("--starter-pages", answers.starterPages.join(","));
  if (answers.automation) words.push("--automation");
  if (answers.packageManager !== undefined) {
    words.push("--package-manager", answers.packageManager);
    if (!answers.install) words.push("--no-install");
  }
  words.push("--runtime", answers.runtime);
  if (!answers.git) words.push("--no-git");
  if (extra.bdkPackage !== undefined) words.push("--bdk-package", extra.bdkPackage);
  words.push("--yes");
  return words.map(shellWord).join(" ");
}
