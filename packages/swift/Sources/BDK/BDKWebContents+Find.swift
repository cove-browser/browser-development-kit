import CBDK
import CoreGraphics
import Foundation

/// Find in page for one page (swift-api.md section 6.7). Get it from `BDKWebContents.find`. An
/// application with its own find field uses `start`, `next`, `previous` and `stop`; `present()` asks
/// for the browser-owned find bar through the `find` UI request instead.
@MainActor
public final class FindSession {
    public struct Options: Sendable, Equatable {
        public var matchCase: Bool

        public init(matchCase: Bool = false) {
            self.matchCase = matchCase
        }
    }

    /// One report of the search. The engine sends several while it is still counting: only the one
    /// with `isFinal` has the full count.
    public struct Result: Sendable, Equatable {
        /// How many matches the page has (so far, unless `isFinal`).
        public let matchCount: Int
        /// The active match, counted from 1; 0 when there is none.
        public let activeIndex: Int
        /// The active match in the page view's coordinates; zero when there is none.
        public let rect: CGRect
        public let isFinal: Bool
    }

    public enum SelectionAction: Sendable {
        /// Ends the search and deselects the active match.
        case clearSelection
        /// Ends the search and leaves the active match selected.
        case keepSelection
    }

    private unowned let contents: BDKWebContents
    private var results: AsyncStream<Result>.Continuation?

    init(contents: BDKWebContents) {
        self.contents = contents
    }

    /// Starts a search, or restarts it with other text, and returns its results: one for the start
    /// and one for each move of the active match, until `stop` or another `start` ends the
    /// sequence. Empty text ends the search and reports no match. The sequence also ends when the
    /// page closes.
    public func start(
        _ text: String, options: Options = Options()
    ) throws -> AsyncStream<Result> {
        let handle = try contents.liveHandle()
        // Subscribe before the call so that the first result cannot be missed.
        let source = contents.broadcaster.subscribe()
        try abiCall(.contents) { error in
            withBDKString(text) { text in
                var abiOptions = bdk_find_options_t()
                abiOptions.struct_size = MemoryLayout<bdk_find_options_t>.size
                abiOptions.match_case = options.matchCase ? 1 : 0
                return bdk_contents_find_start(handle, text, &abiOptions, error)
            }
        }
        results?.finish()
        let (stream, continuation) = AsyncStream<Result>.makeStream()
        results = continuation
        let forwarder = Task {
            for await event in source {
                if case .findResult(let result) = event { continuation.yield(result) }
            }
            continuation.finish()
        }
        continuation.onTermination = { _ in forwarder.cancel() }
        return stream
    }

    /// Moves to the next match, wrapping at the end. Does nothing when no search is in progress.
    public func next() throws {
        let handle = try contents.liveHandle()
        try abiCall(.contents) { bdk_contents_find_next(handle, $0) }
    }

    public func previous() throws {
        let handle = try contents.liveHandle()
        try abiCall(.contents) { bdk_contents_find_previous(handle, $0) }
    }

    /// Ends the search and the sequence `start` returned.
    public func stop(_ action: SelectionAction = .clearSelection) throws {
        let handle = try contents.liveHandle()
        try abiCall(.contents) {
            bdk_contents_find_stop(handle, action == .clearSelection ? 1 : 0, $0)
        }
        results?.finish()
        results = nil
    }

    /// Asks for the find surface: the application's `find` handler, else the default find bar
    /// (ui-requests.md section 5.11). Asking while it is open focuses it.
    public func present() throws {
        let handle = try contents.liveHandle()
        try abiCall(.contents) { bdk_contents_find_present(handle, $0) }
    }
}

extension BDKWebContents {
    /// Find in page.
    public var find: FindSession {
        if let existing = findSession { return existing }
        let session = FindSession(contents: self)
        findSession = session
        return session
    }

    /// Asks for the printing surface, as the print command does: the application's `printing`
    /// handler (a `PrintRequest` with `isUserInitiated` true), else the default presenter
    /// (ui-requests.md section 5.9).
    public func print() throws {
        let handle = try liveHandle()
        try abiCall(.contents) { bdk_contents_print(handle, $0) }
    }
}

extension FindSession.Result {
    init(_ detail: BDKJSValue?) {
        func number(_ value: BDKJSValue?, _ key: String) -> Double {
            if case .object(let object) = value, case .number(let number) = object[key] {
                return number
            }
            return 0
        }
        var rect: BDKJSValue?
        var isFinal = false
        if case .object(let object) = detail {
            rect = object["rect"]
            if case .bool(let flag) = object["final"] { isFinal = flag }
        }
        self.init(
            matchCount: Int(number(detail, "matchCount")),
            activeIndex: Int(number(detail, "activeIndex")),
            rect: CGRect(
                x: number(rect, "x"), y: number(rect, "y"), width: number(rect, "width"),
                height: number(rect, "height")),
            isFinal: isFinal)
    }
}
