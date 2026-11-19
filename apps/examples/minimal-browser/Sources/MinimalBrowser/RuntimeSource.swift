// THE ONLY FILE THAT KNOWS THE RUNTIME IS A STUB.
//
// Until the real runtime exists (the engine tasks of M2, blocked on M0), the example runs against the in-memory
// runtime of BDKTestSupport. It renders nothing and runs no JavaScript, and it navigates only to URLs the stub
// is told about, so this file also tells it how the example's sites answer.
//
// To switch to the real runtime, replace the body of `start` with `try BDKRuntime.start(configuration)`,
// set `name` to "chromium" and `notice` to nil, delete the rest of this file, and drop the `BDKTestSupport`
// product from the MinimalBrowser target in Package.swift.

import BDK
import BDKTestSupport
import Foundation

@MainActor
enum RuntimeSource {
    /// Reported in the status file, so a test can tell which runtime it ran against.
    static let name = "stub"

    /// Shown at the top of the window. Nil with the real runtime.
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
