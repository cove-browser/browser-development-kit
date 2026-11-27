import { describe, expect, it } from "vitest";
import { parseCliArgs } from "./args.js";
import { CliError, ExitCode } from "./exit-codes.js";

describe("parseCliArgs", () => {
  it("parses the global flags and positionals", () => {
    const args = parseCliArgs(["build", "config", "--json", "--offline", "--project", "/p", "-h"]);
    expect(args).toMatchObject({
      positionals: ["build", "config"],
      json: true,
      offline: true,
      project: "/p",
      help: true,
      version: false,
    });
  });

  it("reads --watch", () => {
    expect(parseCliArgs(["build", "automation", "--watch"]).watch).toBe(true);
  });

  it("accepts --version and -V", () => {
    expect(parseCliArgs(["--version"]).version).toBe(true);
    expect(parseCliArgs(["-V"]).version).toBe(true);
  });

  it("defaults everything off", () => {
    const args = parseCliArgs([]);
    expect(args).toEqual({
      help: false,
      version: false,
      json: false,
      offline: false,
      watch: false,
      allowSourceBuild: false,
      positionals: [],
    });
  });

  it("reads --out and a forced diagnostics form", () => {
    const args = parseCliArgs(["build", "config", "--out", "x.json", "--diagnostics", "xcode"]);
    expect(args.out).toBe("x.json");
    expect(args.diagnostics).toBe("xcode");
  });

  it.each([[["--nope"]], [["--project"]], [["--diagnostics", "pretty"]], [["--json=1"]]])(
    "rejects %j as a usage error",
    (argv) => {
      try {
        parseCliArgs(argv);
        expect.unreachable();
      } catch (error) {
        expect(error).toBeInstanceOf(CliError);
        expect((error as CliError).exitCode).toBe(ExitCode.Usage);
        expect((error as CliError).diagnostic.code).toBe("cli.usage");
      }
    },
  );
});
