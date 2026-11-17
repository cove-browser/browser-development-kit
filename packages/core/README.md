# core

The BDK C ABI: public headers (`include/bdk`), the in-memory stub runtime (`stub`), the runtime-independent
conformance suite (`tests`) and the ABI hash tooling (`cmake/abi-hash.cmake`, `generated`).
Read [docs/conventions.md](docs/conventions.md) first.

```sh
pnpm exec nx run core:build      # headers as C11 and C++17, stub, suite
pnpm exec nx run core:test       # ctest: conformance suite + ABI hash check
cmake -P packages/core/cmake/abi-hash.cmake   # print the ABI hash
```
