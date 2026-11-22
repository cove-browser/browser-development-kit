import { resolve } from "node:path";
import { hasErrors } from "../diagnostics-format.js";
import { ExitCode, usageError } from "../exit-codes.js";
import type { SignRequest } from "../signing/types.js";
import type { Step } from "./step.js";

/**
 * `bdk build sign`: re-sign the application and the runtime nested in it with the application's
 * identity (sdk/cli.md section 5). Everything that touches `codesign` is in the platform module.
 */
export const signStep: Step = {
  name: "sign",
  flags: ["bundle", "identity", "team", "entitlements"],
  // Only in an Xcode build or with an explicit identity (sdk/cli.md section 5).
  applies: ({ ctx, args }) => ctx.platform.isIdeBuild(ctx.env) || args.identity !== undefined,
  async run({ project, ctx, args }) {
    if (ctx.env.BDK_SKIP_SIGNING === "1") {
      return {
        diagnostics: [
          {
            code: "sign.skipped",
            severity: "warning",
            message:
              "BDK_SKIP_SIGNING=1: the application was not signed and will not launch until it is.",
            fix: "Sign it in a later pipeline step, or unset BDK_SKIP_SIGNING.",
          },
        ],
        exitCode: ExitCode.Success,
        data: { skipped: true },
      };
    }
    const bundle = args.bundle ?? ctx.env.CODESIGNING_FOLDER_PATH;
    if (bundle === undefined || bundle === "") {
      throw usageError(
        "`bdk build sign` needs the application bundle.",
        "Pass --bundle <App.app>. Xcode build phases set CODESIGNING_FOLDER_PATH.",
      );
    }
    const identity = args.identity ?? ctx.env.EXPANDED_CODE_SIGN_IDENTITY;
    if (identity === undefined || identity === "") {
      throw usageError(
        "`bdk build sign` needs a signing identity.",
        "Pass --identity <name>, or --identity - for an ad hoc signature.",
      );
    }
    // A real identity gets a secure timestamp, which comes from the signing authority's server.
    if (args.offline && identity !== "-") {
      throw usageError(
        "`bdk build sign --offline` cannot sign with a real identity: its secure timestamp needs the network.",
        "Drop --offline, or sign ad hoc with --identity - (no timestamp, not for distribution).",
      );
    }
    const team = args.team ?? ctx.env.DEVELOPMENT_TEAM;
    const entitlements =
      args.entitlements !== undefined
        ? resolve(ctx.cwd, args.entitlements)
        : ctx.env.CODE_SIGN_ENTITLEMENTS
          ? resolve(ctx.env.SRCROOT ?? project.dir, ctx.env.CODE_SIGN_ENTITLEMENTS)
          : undefined;
    const request: SignRequest = {
      appPath: resolve(ctx.cwd, bundle),
      identity,
      ...(team === undefined || team === "" ? {} : { team }),
      ...(entitlements === undefined ? {} : { entitlementsFile: entitlements }),
    };
    const outcome = await ctx.platform.sign(request);
    return {
      diagnostics: outcome.diagnostics,
      exitCode: hasErrors(outcome.diagnostics) ? ExitCode.ProjectFailure : ExitCode.Success,
      data: { signed: outcome.signed },
    };
  },
};
