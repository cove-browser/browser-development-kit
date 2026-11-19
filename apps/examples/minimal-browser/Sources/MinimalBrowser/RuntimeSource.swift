// THE ONLY FILE THAT KNOWS WHICH RUNTIME THE EXAMPLE RUNS ON.
//
// Package.swift builds the example against the real runtime (the packaged Chromium framework that
// `pnpm chromium:runtime` resolves to .bdk/runtime) when it exists, and against the in-memory stub of
// BDKTestSupport otherwise or when BDK_RUNTIME=stub. The stub renders nothing and runs no JavaScript, and it
// navigates only to URLs it is told about, so the stub half of this file also tells it how the example's
// sites answer.

import BDK
import Foundation

#if BDK_CHROMIUM_RUNTIME

    @MainActor
    enum RuntimeSource {
        /// Reported in the status file, so a test can tell which runtime it ran against.
        static let name = "chromium"

        /// Shown at the top of the window. Nil with the real runtime.
        static let notice: String? = nil

        static func start(_ configuration: BDKConfiguration) throws -> BDKRuntime {
            try BDKRuntime.start(configuration)
        }
    }

#else

    import BDKTestSupport

    @MainActor
    enum RuntimeSource {
        static let name = "stub"

        static let notice: String? =
            "Stub runtime: pages are placeholders. Nothing is rendered and no JavaScript runs."

        static func start(_ configuration: BDKConfiguration) throws -> BDKRuntime {
            let runtime = try BDKRuntime.startStub(configuration)
            for (address, title) in scriptedTitles {
                // The stub matches the URL string exactly; a person types the address with or without a slash.
                for spelling in [
                    address, address.hasSuffix("/") ? String(address.dropLast()) : address,
                ] {
                    guard let url = URL(string: spelling) else { continue }
                    try runtime.stub.scriptNavigation(to: url, title: title)
                }
            }
            return runtime
        }

        /// The titles of the sites the example loads (its home pages and the ones the tests navigate to). Any
        /// other URL still commits, titled with the URL itself.
        private static let scriptedTitles: [(String, String)] = [
            ("https://example.com/", "Example Domain"),
            ("https://example.org/", "Example Domain (org)"),
            ("https://example.net/", "Example Domain (net)"),
            ("https://www.iana.org/domains/reserved", "IANA-managed Reserved Domains"),
        ]
    }

#endif
