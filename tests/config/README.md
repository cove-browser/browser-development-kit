# tests/config

Fixtures for `bdk.toml` validation and for the resolver that produces `bdk.resolved.json`. They are a
contract: the format is plain files, so a second implementation (a Swift parser, an editor plugin) can run
the same cases. The CLI's tests are `packages/cli/src/core/config/fixtures.test.ts`.

The rules being tested are in [configuration.md](../../docs/planning/sdk/configuration.md) sections 2, 3, 4,
7 and 9. The schema is `packages/cli/assets/bdk.schema.json`.

## Layout

| Path | Contents | Expectation |
|---|---|---|
| `valid/*.toml` | A single `bdk.toml` each | Zero diagnostics from validation (TOML syntax, schema and the checks that need no files) |
| `invalid/<name>.toml` and `invalid/<name>.expected.json` | A `bdk.toml` and its diagnostics | Validation reports exactly the diagnostics in the sidecar, in order |
| `resolve/<case>/` | A project directory: `bdk.toml`, optional `Pages/`, `Automation/` and other directories | See below |

`taplo` skips `invalid/` because some files there are deliberately malformed. Everything else is formatted
by `taplo format`, so the line numbers in sidecars stay valid; keep them formatted.

## Sidecar format

```json
{
  "diagnostics": [
    {
      "code": "pages.reserved",
      "severity": "error",
      "message": "Route \"print\" uses a reserved name. Chromium needs it internally.",
      "file": "bdk.toml",
      "line": 5
    }
  ]
}
```

- `code`, `severity` and `message` are exact. `file` is relative to the project directory with forward
  slashes (`bdk.toml`, `Pages/NewTab`, `Info.plist`). `line` is 1-based and is absent for findings about a
  directory or an external file.
- The order is the order of the tool: by file, line, column, code, then message.
- `fix` text and columns are not part of the contract.

## Resolve cases

A case directory holds the project and its expectation:

| File | Meaning |
|---|---|
| `expected.resolved.json` | The exact bytes of `bdk.resolved.json`: two-space JSON, a final newline, keys in the documented order. Biome ignores these files so the bytes stay as written. |
| `expected.diagnostics.json` | The diagnostics, in the sidecar format. Without `expected.resolved.json` the case must fail and write no resolved file; with it, these are the warnings. |
| `bundle-facts.json` | Optional. `{ "infoPlist": {...}, "entitlements": {...} }`, the already-parsed facts the build passes in |

A case with neither expectation file is a mistake. Digests in `expected.resolved.json` come from the files in
the case, so changing a page changes them: regenerate with the tool and review the diff.

## Adding a case

1. Write the `.toml` (or project directory).
2. Run the resolver or validator and read its output. Check that every message and line is what the
   documentation says it should be before saving it as the expectation.
3. Run `pnpm exec nx run cli:test` and `cli:lint`.

Keys that configuration.md marks Added or Conditional are not accepted yet, so `invalid/unaccepted-additions.toml`
expects the ordinary unknown-key error for each. When one is accepted, move it to `valid/` and add it to the schema.
