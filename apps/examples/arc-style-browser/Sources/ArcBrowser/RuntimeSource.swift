// THE ONLY FILE THAT KNOWS WHICH RUNTIME THE EXAMPLE RUNS ON.
//
// Package.swift builds the example against the real runtime (the packaged Chromium framework that
// `pnpm chromium:runtime` resolves to .bdk/runtime) when it exists, and against the in-memory stub of
// BDKTestSupport otherwise or when BDK_RUNTIME=stub. The stub renders nothing and runs no JavaScript, so
// pages show their URL and title, and the bundled automation script does not run.

import BDK
import Foundation

#if BDK_CHROMIUM_RUNTIME

    @MainActor
    enum RuntimeSource {
        /// Reported in the status file, so a test can tell which runtime it ran against.
        static let name = "chromium"
        static let notice = ""

        static func start(_ configuration: BDKConfiguration) throws -> BDKRuntime {
            try BDKRuntime.start(configuration)
        }
    }

#else

    import BDKTestSupport

    @MainActor
    enum RuntimeSource {
        static let name = "stub"
        /// The stub renders nothing, so each window says so: a page shows its URL and title only.
        static let notice = "Stub runtime: pages do not render (see README, Run it)"

        static func start(_ configuration: BDKConfiguration) throws -> BDKRuntime {
            try BDKRuntime.startStub(configuration)
        }
    }

#endif
