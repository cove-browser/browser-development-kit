// THE ONLY FILE THAT KNOWS WHICH RUNTIME THE APPLICATION RUNS ON.
//
// The application runs on the in-memory stub runtime of BDKTestSupport until `bdk build runtime` and
// `bdk build embed` put the Chromium runtime in the bundle. The stub renders nothing and runs no JavaScript,
// and it navigates only to URLs it is told about, so this file also tells it how the home page answers. When
// the bundle carries the runtime, replace `startStub` with `BDKRuntime.start(configuration)`, set `notice` to
// nil and stop linking BDKTestSupport into the application target.

import BDK
import BDKTestSupport
import Foundation

@MainActor
enum RuntimeSource {
    /// Shown at the top of the window.
    static let notice: String? =
        "Stub runtime: pages are placeholders. Nothing is rendered and no JavaScript runs."

    static func start(_ configuration: BDKConfiguration) throws -> BDKRuntime {
        let runtime = try BDKRuntime.startStub(configuration)
        for spelling in ["https://example.com/", "https://example.com"] {
            if let url = URL(string: spelling) {
                try runtime.stub.scriptNavigation(to: url, title: "Example Domain")
            }
        }
        return runtime
    }
}
