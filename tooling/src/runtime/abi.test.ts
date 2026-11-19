import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { repoRoot } from "../chromium/common.js";
import { abiExports, readAbi } from "./abi.js";

describe("abiExports", () => {
  it("lists every BDK_EXPORT function and each bdk_mac class, ignoring comments", () => {
    const root = mkdtempSync(join(tmpdir(), "bdk-abi-"));
    const include = join(root, "packages", "core", "include");
    mkdirSync(join(include, "bdk"), { recursive: true });
    mkdirSync(join(include, "bdk_mac"), { recursive: true });
    writeFileSync(
      join(include, "bdk", "bdk_a.h"),
      [
        "/* BDK_EXPORT int bdk_in_comment(void); */",
        "BDK_EXPORT BDK_ANY_THREAD uint32_t bdk_one(void);",
        "BDK_EXPORT bdk_status_t bdk_two(bdk_runtime_t,",
        "                                 bdk_string_t name);",
        "typedef void (*bdk_cb)(void*);",
      ].join("\n"),
    );
    writeFileSync(join(include, "bdk_mac", "app.h"), "@interface BDKThing : NSApplication\n@end\n");
    expect(abiExports(root)).toEqual([
      "_OBJC_CLASS_$_BDKThing",
      "_OBJC_METACLASS_$_BDKThing",
      "_bdk_one",
      "_bdk_two",
    ]);
  });

  it("finds the real ABI, including the start and shutdown calls", () => {
    const exports = abiExports(repoRoot());
    expect(exports).toContain("_bdk_runtime_start");
    expect(exports).toContain("_bdk_runtime_shutdown");
    expect(exports).toContain("_OBJC_CLASS_$_BDKApplicationBase");
    expect(readAbi(repoRoot()).version).toBeGreaterThan(0);
  });
});
