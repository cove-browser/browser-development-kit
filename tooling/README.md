# tooling

TypeScript and shell helpers called by Nx targets.

## Convention for running tooling TypeScript

Run source directly with tsx through pnpm; there is no build step for tooling commands:

```sh
pnpm exec tsx tooling/src/<file>.ts
```

Nx targets (for example `chromium:sync`) call `pnpm exec tsx tooling/src/chromium/<command>.ts` from the
workspace root. Import siblings with a `.js` extension (`./paths.js`), as NodeNext requires. The `build`
target only type-checks and emits `dist/` for consumers; nothing at runtime depends on it.

## Shared modules

- `src/paths.ts`: `BDK_HOME` resolution and the subpaths under it. Rejects paths with spaces.
- `src/toolchain.ts`: version checks used by `setup`.
- `toolchain-id.sh`: the `nativeToolchain` Nx input.

## `pnpm run setup`

pnpm has a built-in `setup` command (it edits shell profiles), and built-ins win over scripts, so
`pnpm setup` never reaches the script. Use `pnpm run setup` (or `pnpm exec nx run tooling:setup`).
