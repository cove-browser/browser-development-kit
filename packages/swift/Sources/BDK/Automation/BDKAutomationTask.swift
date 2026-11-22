import CBDK
import Foundation

/// One run of a bundled script. Its `events` report progress, logs and the commands it called; its
/// `value` is what the script's default export returned.
@MainActor
public final class BDKAutomationTask {
    public enum Level: String, Sendable {
        case debug
        case info
        case warn
        case error
    }

    public enum Event: Sendable, Equatable {
        /// `fraction` is 0 to 1.
        case progress(Double, String)
        case log(Level, String)
        /// The script called `command`; `error` is set when the call failed or was refused. For
        /// auditing: every call is also an `automation.commandInvoked` event.
        case commandInvoked(String, error: BDKError?)
    }

    /// Progress, log and command events, in order. It ends when the task finishes. Each iterator
    /// sees every event from when it was created.
    public var events: BDKEventSequence<Event> {
        BDKEventSequence(broadcaster)
    }

    /// What the script returned, once it finished. Throws what ended it: `broker.cancelled`
    /// after `cancel()`, `automation.sessionRevoked`, `automation.hostGone` when the automation
    /// host died, `automation.scriptFailed` when the script threw (the message is the script's).
    public var value: BDKJSValue {
        get async throws {
            if let outcome { return try outcome.get() }
            return try await withCheckedThrowingContinuation { waiters.append($0) }
        }
    }

    private unowned let session: BDKAutomationSession
    private var handle: bdk_task_t
    private let broadcaster = EventBroadcaster<Event>()
    private var outcome: Result<BDKJSValue, BDKError>?
    private var waiters: [CheckedContinuation<BDKJSValue, any Error>] = []

    init(session: BDKAutomationSession, handle: bdk_task_t) {
        self.session = session
        self.handle = handle
        let box = Unmanaged.passRetained(TaskBox(self))
        let subscription = bdk_task_subscribe(
            handle, taskEventCallback, box.toOpaque(), taskReleaseCallback)
        EventLoop.kick()
        if subscription == 0 {
            box.release()
            abandon()
        }
    }

    isolated deinit {
        guard handle != 0 else { return }
        // Releasing a task that is still running cancels it.
        _ = bdk_release(handle)
        EventLoop.kick()
    }

    /// Ends the task: it finishes with `broker.cancelled`. Does nothing to a finished task.
    public func cancel() {
        guard handle != 0, outcome == nil else { return }
        var error: bdk_error_t = 0
        _ = bdk_task_cancel(handle, &error)
        if error != 0 { bdk_error_release(error) }
        EventLoop.kick()
    }

    // MARK: Events from the runtime

    fileprivate func receive(kind: UInt32, detailJSON: String) {
        guard case .object(let detail) = BDKJSValue(jsonString: detailJSON) else { return }
        switch kind {
        case BDK_TASK_EVENT_PROGRESS:
            broadcaster.send(
                .progress(
                    detail["fraction"]?.doubleValue ?? 0, detail["message"]?.stringValue ?? ""))
        case BDK_TASK_EVENT_LOG:
            let level = Level(rawValue: detail["level"]?.stringValue ?? "") ?? .info
            broadcaster.send(.log(level, detail["message"]?.stringValue ?? ""))
        case BDK_TASK_EVENT_COMMAND:
            let command = detail["command"]?.stringValue ?? ""
            var failure: BDKError?
            if case .object(let error) = detail["error"] {
                failure = BDKError(
                    domain: .init(rawValue: error["domain"]?.stringValue ?? ""),
                    code: error["code"]?.stringValue ?? "",
                    message: "\(command) did not succeed")
            }
            broadcaster.send(.commandInvoked(command, error: failure))
        case BDK_TASK_EVENT_FINISHED:
            if case .object(let error) = detail["error"] {
                finish(
                    .failure(
                        BDKError(
                            domain: .init(rawValue: error["domain"]?.stringValue ?? ""),
                            code: error["code"]?.stringValue ?? "",
                            message: error["message"]?.stringValue ?? "the task failed")))
            } else {
                finish(.success(detail["result"] ?? .null))
            }
        default:
            break
        }
    }

    private func finish(_ result: Result<BDKJSValue, BDKError>) {
        guard outcome == nil else { return }
        outcome = result
        let waiting = waiters
        waiters.removeAll()
        for waiter in waiting { waiter.resume(with: result.mapError { $0 as any Error }) }
        broadcaster.finish()
        session.taskFinished(self)
    }

    /// The runtime shut down before the task finished.
    func abandon() {
        handle = 0
        finish(.failure(.stale(.task)))
    }
}

/// The subscription's context: a weak link, so that a task nobody holds can be released.
@MainActor
private final class TaskBox {
    weak var task: BDKAutomationTask?

    init(_ task: BDKAutomationTask) {
        self.task = task
    }
}

private let taskEventCallback: bdk_task_event_cb = { context, _, kind, detail in
    guard let context else { return }
    let box = Unmanaged<TaskBox>.fromOpaque(context).takeUnretainedValue()
    let text = swiftString(detail)
    MainActor.assumeIsolated { box.task?.receive(kind: kind, detailJSON: text) }
}

private let taskReleaseCallback: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<TaskBox>.fromOpaque(context).release()
}
