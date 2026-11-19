import Foundation

/// Fan-out for events. Each subscriber gets its own stream and sees every event sent after it
/// subscribed. Thread-safe so that the sequence can be iterated from any task.
final class EventBroadcaster<Element: Sendable>: @unchecked Sendable {
    private let lock = NSLock()
    private var continuations: [UUID: AsyncStream<Element>.Continuation] = [:]
    private var isFinished = false

    func subscribe() -> AsyncStream<Element> {
        let (stream, continuation) = AsyncStream<Element>.makeStream()
        let key = UUID()
        lock.lock()
        defer { lock.unlock() }
        if isFinished {
            continuation.finish()
        } else {
            continuations[key] = continuation
            continuation.onTermination = { [weak self] _ in self?.remove(key) }
        }
        return stream
    }

    func send(_ element: Element) {
        lock.lock()
        let targets = Array(continuations.values)
        lock.unlock()
        for continuation in targets { continuation.yield(element) }
    }

    /// Ends every stream, and every later subscription ends at once.
    func finish() {
        lock.lock()
        isFinished = true
        let targets = Array(continuations.values)
        continuations.removeAll()
        lock.unlock()
        for continuation in targets { continuation.finish() }
    }

    private func remove(_ key: UUID) {
        lock.lock()
        continuations[key] = nil
        lock.unlock()
    }
}

/// The `AsyncSequence` behind every `events` property. Several tasks may iterate it. An iterator
/// receives the events sent after `makeAsyncIterator()`, so create it before the action whose events
/// you want.
public struct BDKEventSequence<Element: Sendable>: AsyncSequence, Sendable {
    private let broadcaster: EventBroadcaster<Element>

    init(_ broadcaster: EventBroadcaster<Element>) {
        self.broadcaster = broadcaster
    }

    public func makeAsyncIterator() -> AsyncStream<Element>.Iterator {
        makeStream().makeAsyncIterator()
    }

    /// The same subscription as a stream, for code that must subscribe now and iterate later.
    func makeStream() -> AsyncStream<Element> {
        broadcaster.subscribe()
    }
}
