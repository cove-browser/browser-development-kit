import CBDK
import Foundation

private struct EntriesDTO: Decodable {
    struct Entry: Decodable {
        let url: String
        let title: String
    }

    let current: Int
    let entries: [Entry]
}

extension BDKWebContents {
    /// Navigation of one page. Get it from `BDKWebContents.navigation`.
    @MainActor
    public final class Navigation {
        public struct Entry: Sendable, Equatable {
            public let url: URL?
            public let title: String
        }

        private let contents: BDKWebContents

        init(contents: BDKWebContents) {
            self.contents = contents
        }

        /// Loads a URL and returns when the navigation commits. Throws `navigation.superseded`
        /// when another navigation replaces it, `navigation.aborted` when it is stopped and
        /// `navigation.netError` when it fails. Cancelling the task stops the navigation.
        @discardableResult
        public func load(_ url: URL) async throws -> Commit {
            var iterator = contents.events.makeAsyncIterator()
            let handle = try contents.liveHandle()
            try abiCall(.contents) { error in
                withBDKString(url.absoluteString) { bdk_contents_load(handle, $0, error) }
            }
            // A navigation this one replaced may still have events queued ahead; only the start of this
            // URL's navigation opens the window for its outcome.
            var started = false
            while let event = await iterator.next() {
                switch event {
                case .navigationStarted(let info) where info.url == url:
                    started = true
                case .navigationCommitted(let commit) where started:
                    return commit
                case .navigationFailed(let failure) where started:
                    throw failure.error
                case .closed:
                    throw BDKError.stale(.contents)
                default:
                    break
                }
            }
            if Task.isCancelled {
                try? stop()
                throw BDKError(
                    domain: .broker, code: "cancelled", message: "the task was cancelled")
            }
            throw BDKError.stale(.contents)
        }

        public func reload(bypassingCache: Bool = false) throws {
            let handle = try contents.liveHandle()
            try abiCall(.contents) { bdk_contents_reload(handle, bypassingCache ? 1 : 0, $0) }
        }

        public func stop() throws {
            let handle = try contents.liveHandle()
            try abiCall(.contents) { bdk_contents_stop(handle, $0) }
        }

        /// Fails with `broker.invalidArguments` when there is no such entry.
        public func go(toOffset offset: Int) throws {
            let handle = try contents.liveHandle()
            try abiCall(.contents) { bdk_contents_go_to_offset(handle, Int32(offset), $0) }
        }

        public func go(toIndex index: Int) throws {
            let handle = try contents.liveHandle()
            try abiCall(.contents) { bdk_contents_go_to_index(handle, Int32(index), $0) }
        }

        public func goBack() throws {
            try go(toOffset: -1)
        }

        public func goForward() throws {
            try go(toOffset: 1)
        }

        public var entries: [Entry] {
            get throws { try read().entries }
        }

        /// The index of the current entry, or -1 when there is none.
        public var currentIndex: Int {
            get throws { try read().current }
        }

        public var canGoBack: Bool {
            get throws { try read().current > 0 }
        }

        public var canGoForward: Bool {
            get throws {
                let history = try read()
                return history.current >= 0 && history.current < history.entries.count - 1
            }
        }

        private func read() throws -> (current: Int, entries: [Entry]) {
            let handle = try contents.liveHandle()
            var buffer: bdk_buffer_t = 0
            try abiCall(.contents) { bdk_contents_entries(handle, &buffer, $0) }
            let dto = try decodeResult(
                EntriesDTO.self, from: String(decoding: takeBuffer(buffer), as: UTF8.self))
            return (
                dto.current, dto.entries.map { Entry(url: URL(string: $0.url), title: $0.title) }
            )
        }
    }

    /// Navigation: load, reload, stop, history.
    public var navigation: Navigation {
        Navigation(contents: self)
    }

    /// Shorthand for `navigation.load(_:)`.
    @discardableResult
    public func navigate(to url: URL) async throws -> Commit {
        try await navigation.load(url)
    }
}
