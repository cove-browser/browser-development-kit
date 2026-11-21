import CBDK
import Foundation

/// A fixed capability set and scope for an agent, a script or an external tool. Make one with
/// `runtime.automation.makeSession(_:)`. The runtime keeps it until `revoke()`.
@MainActor
public final class BDKAutomationSession {
    public let name: String
    public let capabilities: [String]
    /// True once `revoke()` ran or the runtime shut down.
    public private(set) var isRevoked = false

    private unowned let automation: BDKAutomation
    private(set) var handle: bdk_session_t
    private var running: [ObjectIdentifier: BDKAutomationTask] = [:]

    init(
        automation: BDKAutomation, handle: bdk_session_t, name: String, capabilities: [String]
    ) {
        self.automation = automation
        self.handle = handle
        self.name = name
        self.capabilities = capabilities
    }

    /// Runs a bundled script (`Automation/<script>.ts`, listed in the resolved configuration) as
    /// this session, with `arguments` passed to its default export. Throws `automation.disabled`
    /// unless `[automation] enabled` is true, `automation.scriptNotFound` for a name that is not
    /// listed and `automation.sessionRevoked` after `revoke()`.
    public func run(_ script: String, arguments: BDKJSValue = .null) throws -> BDKAutomationTask {
        let handle = try liveHandle()
        var task: bdk_task_t = 0
        try withBDKStrings([script, arguments.jsonString]) { lent in
            try abiCall(.session) { bdk_session_run(handle, lent[0], lent[1], &task, $0) }
        }
        let started = BDKAutomationTask(session: self, handle: task)
        running[ObjectIdentifier(started)] = started
        return started
    }

    /// Widens the scope by one page: for an `.explicit` scope (and a `.created` one, which takes
    /// pages too).
    public func grant(contents: BDKWebContents) throws {
        let handle = try liveHandle()
        let page = try contents.liveHandle()
        try abiCall(.session) { bdk_session_grant_contents(handle, page, $0) }
    }

    /// Ends the session: its tasks finish with `automation.sessionRevoked`, its calls and
    /// subscriptions stop, its external connections close and its user scripts are removed. Does
    /// nothing when it was revoked already.
    public func revoke() {
        guard !isRevoked, handle != 0 else { return }
        var error: bdk_error_t = 0
        _ = bdk_session_revoke(handle, &error)
        if error != 0 { bdk_error_release(error) }
        EventLoop.kick()
        automation.forget(self, handle: handle)
        isRevoked = true
        handle = 0
    }

    func liveHandle() throws -> bdk_session_t {
        guard !isRevoked, handle != 0 else {
            throw BDKError(
                domain: .automation, code: "sessionRevoked",
                message: "the session '\(name)' was revoked")
        }
        return handle
    }

    func taskFinished(_ task: BDKAutomationTask) {
        running[ObjectIdentifier(task)] = nil
    }

    /// The runtime shut down: the session and its tasks are gone.
    func invalidate() {
        isRevoked = true
        handle = 0
        let tasks = Array(running.values)
        running.removeAll()
        for task in tasks { task.abandon() }
    }
}
