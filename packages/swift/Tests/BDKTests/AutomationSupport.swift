import BDKTestSupport
import Foundation
import Testing

@testable import BDK

struct WaitTimedOut: Error, CustomStringConvertible {
    let what: String
    var description: String { "timed out waiting for \(what)" }
}

/// Lets the runtime work until `condition` holds.
@MainActor
func waitUntil(
    _ what: String, timeout: Duration = .seconds(5), _ condition: () -> Bool
) async throws {
    let deadline = ContinuousClock.now + timeout
    while !condition() {
        if ContinuousClock.now > deadline { throw WaitTimedOut(what: what) }
        try await Task.sleep(for: .milliseconds(5))
    }
}

/// A resolved configuration with `[automation]` on and one script, `echo`, listed; `external` also
/// enables the external endpoint. `extra` is more members of the top-level object.
func automationConfiguration(
    enabled: Bool = true, external: Bool = false, extra: String = ""
) -> BDKConfiguration {
    let scripts = """
        {"echo":{"path":"Automation/echo.ts","digest":"sha256:0"}}
        """
    return BDKConfiguration(
        resolvedJSON: """
            {"automation":{"enabled":\(enabled),"scripts":\(scripts),\
            "external":{"enabled":\(external)}}\(extra.isEmpty ? "" : "," + extra)}
            """)
}

extension StubScript {
    /// The frames received so far whose `type` is `type`.
    func frames(ofType type: String) -> [BDKJSValue] {
        frames.filter { $0["type"]?.stringValue == type }
    }
}

extension BDKJSValue {
    /// Reads a nested field of an object value by its path.
    subscript(path: String...) -> BDKJSValue? {
        var current: BDKJSValue? = self
        for key in path { current = current?.objectValue?[key] }
        return current
    }
}
