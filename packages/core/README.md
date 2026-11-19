# core

The BDK C ABI: public headers (`include/bdk`), the runtime core shared by every runtime (`runtime`), the in-memory stub backend (`stub`), the runtime-independent
conformance suite (`tests`) and the ABI hash tooling (`cmake/abi-hash.cmake`, `generated`).
The interface definitions (`interface`) and the generated `include/bdk/bdk_ids.h` are described in the conventions.
Read [docs/conventions.md](docs/conventions.md) first.

```sh
pnpm exec nx run core:build      # headers as C11 and C++23, stub, suite
pnpm exec nx run core:test       # ctest: conformance suite + ABI hash check
pnpm exec nx run core:test-engine  # the suite and the engine scenarios against the real runtime (.bdk/runtime)
cmake -P packages/core/cmake/abi-hash.cmake   # print the ABI hash
pnpm exec nx run js-codegen:codegen        # regenerate bdk_ids.h, interface/ids.json and the broker tables from interface/*.json
pnpm exec nx run js-codegen:codegen-check  # fail if they are stale (no Chromium, no CMake)
cmake -DMODE=generate -P packages/core/cmake/abi-hash.cmake   # then record the new ABI hash
```
