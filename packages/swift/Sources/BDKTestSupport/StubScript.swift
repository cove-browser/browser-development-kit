import BDK
import CBDK
import CBDKStub
import Foundation

/// A script the test plays in the stub's automation host (`BDKStub.defineScript`).
@MainActor
public final class StubScript {
    /// Every frame BDK sent to the script, in order, parsed.
    public private(set) var frames: [BDKJSValue] = []
    /// The stub's identifier of the most recently started task.
    public private(set) var task: UInt64 = 0

    /// The newest frame BDK sent, or nil before the first.
    public var last: BDKJSValue? { frames.last }

    func receive(task: UInt64, frame: String) {
        self.task = task
        frames.append(BDKJSValue(jsonString: frame) ?? .null)
    }

    /// Sends a frame from the script, in the wire protocol, as JSON text. It is delivered on the
    /// next pump, which the awaiting test performs. Throws when the task already ended.
    public func send(_ frameJSON: String) throws {
        let status = withBDKString(frameJSON) { bdk_stub_script_send(task, $0) }
        EventLoop.scheduler?()
        guard status == BDK_OK else {
            throw BDKError(
                domain: .runtime, code: "stubRejected",
                message: "the stub rejected the frame with status \(status.rawValue)")
        }
    }

    /// Sends a frame from the script.
    public func send(_ frame: BDKJSValue) throws {
        try send(frame.jsonString)
    }
}
