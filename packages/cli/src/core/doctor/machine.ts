import { constants, existsSync } from "node:fs";
import { access } from "node:fs/promises";
import { delimiter, dirname, join } from "node:path";
import { gitRaw } from "@bdk/tooling/dist/git.js";
import { BdkHomeError, bdkPaths, resolveBdkHome } from "@bdk/tooling/dist/paths.js";
import type { HostToolchain } from "../../platform/platform.js";
import { Findings } from "./findings.js";
import type { ReleaseFacts } from "./release.js";
import { compareVersions, REQUIREMENTS } from "./requirements.js";

/** The first executable called `name` on `PATH`, or undefined. */
export async function onPath(env: NodeJS.ProcessEnv, name: string): Promise<string | undefined> {
  for (const dir of (env.PATH ?? "").split(delimiter)) {
    if (dir === "") continue;
    const file = join(dir, name);
    try {
      await access(file, constants.X_OK);
      return file;
    } catch {
      // Not here.
    }
  }
  return undefined;
}

export interface MachineInput {
  env: NodeJS.ProcessEnv;
  toolchain: HostToolchain;
  /** The newest macOS the runtime in the store needs, when one is resolved. */
  runtimeMinOs?: string;
  /** The project builds pages with Vite or automation scripts with Rolldown. */
  needsNode: boolean;
  /** `runtime.mode = "source"`. */
  sourceMode: boolean;
  release: ReleaseFacts;
}

const PACKAGE_MANAGERS = ["pnpm", "npm", "yarn", "bun"] as const;

/**
 * How depot_tools and the managed checkout are made at the pin today. `bdk build runtime
 * --allow-source-build` is to do it (T-CLI-04).
 */
export const SYNC_FIX =
  "Sync the managed checkout to this BDK version's pin with `pnpm chromium:sync` in a BDK checkout of the same version; `bdk build runtime --allow-source-build` will do it once it is built.";

/** Machine: the operating system, the toolchain, Node for JavaScript projects, source-mode tools. */
export async function machineFindings(input: MachineInput): Promise<Findings> {
  const f = new Findings("machine");
  const { toolchain, env } = input;
  if (toolchain.os !== "macos") {
    f.error(
      "doctor.host_unsupported",
      `This machine runs ${toolchain.os}; BDK applications are built on macOS.`,
      "Build on a Mac. Other hosts are planned after v1.",
    );
    return f;
  }
  if (toolchain.arch !== REQUIREMENTS.arch) {
    f.error(
      "doctor.not_apple_silicon",
      `This Mac is ${toolchain.arch}; BDK runtimes are built for Apple Silicon (arm64) only.`,
      "Use a Mac with Apple Silicon.",
    );
  }
  const minOs =
    input.runtimeMinOs !== undefined && compareVersions(input.runtimeMinOs, REQUIREMENTS.macos) > 0
      ? input.runtimeMinOs
      : REQUIREMENTS.macos;
  if (toolchain.osVersion === undefined) {
    f.warning(
      "doctor.macos_unknown",
      "The macOS version could not be read (`sw_vers` failed).",
      `Check that this is macOS ${minOs} or later.`,
    );
  } else if (compareVersions(toolchain.osVersion, minOs) < 0) {
    f.error(
      "doctor.macos_too_old",
      `macOS ${toolchain.osVersion} is older than ${minOs}, the oldest this BDK version runs on.`,
      `Update macOS to ${minOs} or later.`,
    );
  }
  if (toolchain.ide === undefined) {
    f.error(
      "doctor.xcode_missing",
      "Xcode is not installed or not selected (`xcodebuild -version` failed).",
      "Install Xcode from the App Store, then run `sudo xcode-select -s /Applications/Xcode.app`.",
    );
  } else if (compareVersions(toolchain.ide, REQUIREMENTS.xcode) < 0) {
    f.error(
      "doctor.xcode_too_old",
      `Xcode ${toolchain.ide} is older than ${REQUIREMENTS.xcode}, the oldest this BDK version is built with.`,
      `Install Xcode ${REQUIREMENTS.xcode} or later.`,
    );
  }
  if (toolchain.swift === undefined) {
    f.error(
      "doctor.swift_missing",
      "The Swift compiler was not found (`xcrun swift --version` failed).",
      "Install Xcode, or the command line tools with `xcode-select --install`.",
    );
  } else if (compareVersions(toolchain.swift, REQUIREMENTS.swift) < 0) {
    f.error(
      "doctor.swift_too_old",
      `Swift ${toolchain.swift} is older than ${REQUIREMENTS.swift}, which the BDK package needs.`,
      `Install an Xcode whose Swift is ${REQUIREMENTS.swift} or later.`,
    );
  }
  if (input.needsNode) {
    if ((await onPath(env, "node")) === undefined) {
      f.error(
        "doctor.node_missing",
        "The project builds pages with Vite or scripts with Rolldown, and Node is not on PATH to install them.",
        "Install Node (https://nodejs.org) and run your package manager's install in the pages directory.",
      );
    }
    const managers = await Promise.all(PACKAGE_MANAGERS.map((name) => onPath(env, name)));
    if (managers.every((found) => found === undefined)) {
      f.error(
        "doctor.package_manager_missing",
        `The project uses JavaScript packages and none of ${PACKAGE_MANAGERS.join(", ")} is on PATH.`,
        "Install one, for example `corepack enable pnpm`.",
      );
    }
  }
  if (input.sourceMode) await sourceModeFindings(f, input);
  return f;
}

/** Source mode: BDK_HOME, depot_tools, the managed checkout at the pin, ccache. */
async function sourceModeFindings(f: Findings, input: MachineInput): Promise<void> {
  const { env } = input;
  let home: string;
  try {
    home = resolveBdkHome({ env });
  } catch (error) {
    if (!(error instanceof BdkHomeError)) throw error;
    f.error(
      "doctor.bdk_home_invalid",
      error.message,
      "Set BDK_HOME to an absolute path with no spaces.",
    );
    return;
  }
  // The nearest directory that exists must be writable, so the checkout and store can be made.
  let existing = home;
  while (!existsSync(existing) && dirname(existing) !== existing) existing = dirname(existing);
  try {
    await access(existing, constants.W_OK);
  } catch {
    f.error(
      "doctor.bdk_home_not_writable",
      `BDK_HOME (${home}) cannot be written: ${existing} is not writable.`,
      "Set BDK_HOME to a directory you own, or fix its permissions.",
    );
    return;
  }
  const paths = bdkPaths(home);
  if (!existsSync(join(home, "depot_tools", "gclient"))) {
    f.error(
      "doctor.depot_tools_missing",
      `depot_tools is not at ${join(home, "depot_tools")}; a source build needs it.`,
      SYNC_FIX,
    );
  }
  const src = join(paths.chromiumManaged, "src");
  if (!existsSync(join(src, ".git"))) {
    f.warning(
      "doctor.checkout_missing",
      `There is no managed Chromium checkout at ${src}; \`bdk patch\` needs one.`,
      `${SYNC_FIX} A first checkout is of the order of 100 GB.`,
    );
  } else {
    const pin = input.release.chromium.commit;
    const atPin = await gitRaw(src, ["merge-base", "--is-ancestor", pin, "HEAD"]);
    if (atPin.code !== 0) {
      f.warning(
        "doctor.checkout_not_at_pin",
        `The managed checkout is not at Chromium ${input.release.chromium.tag} (${pin.slice(0, 12)}), the pin of this BDK version.`,
        SYNC_FIX,
      );
    }
  }
  if ((await onPath(env, "ccache")) === undefined) {
    f.warning(
      "doctor.ccache_missing",
      "ccache is not on PATH; the source build's GN presets use it.",
      "Install it with `brew install ccache`.",
    );
  }
}
