# Engine test fixtures: automation scripts and DevTools front ends

Packaged into `BDKEngineConformance.app` by `packages/core/CMakeLists.txt` at the places `bdk build`
uses (`Contents/Resources/BDK/Automation/<name>.js`, `Contents/Resources/BDK/DevTools/{Frontend,Overlay}`),
with their SHA-256 digests passed to `tests/tests_engine_automation.c`.

The scripts are what `bdk build automation` produces: one ES module with no imports. They are written by
hand against the wire protocol (packages/core/docs/conventions.md, "Automation") instead of being bundled
from `@bdk/automation`, so the engine test build needs no Node. `@bdk/automation` itself is tested by
`nx run js-automation:test`.
