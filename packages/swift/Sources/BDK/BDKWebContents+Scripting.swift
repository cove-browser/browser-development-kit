import Foundation

/// Scripting for one page (swift-api.md section 6.5). Get it from `BDKWebContents.scripting`. Every
/// call is a `contents.*` command with this page as its target, so a closed page throws
/// `contents.stale` and an internal page `contents.internalPage`.
@MainActor
public final class Scripting {
    /// Where a script runs. `.page` is the page's own world, which its scripts can observe and
    /// tamper with; `.isolated(name)` shares the DOM and nothing else. `.isolated("page")` is
    /// refused, because the engine reads that name as `.page`.
    public enum World: Sendable, Equatable {
        case page
        case isolated(String)
    }

    /// Which frame an operation addresses. `.main` and `.all` send no frame, which the engine reads as
    /// the main frame (`.all` is only meaningful for `insertCSS`); `.frame(id)` sends the id from
    /// `frames()`.
    public enum FrameSelector: Sendable, Equatable {
        case main
        case all
        case frame(BDKFrameInfo.ID)
    }

    /// A style sheet `insertCSS` added. It ends with the document, or at `remove`.
    public struct InjectedStyle: Sendable, Equatable {
        public let id: String
    }

    public struct Snapshot: Sendable, Equatable {
        public let kind: BDKSnapshotKind
        /// The tree, as the engine reports it for `kind`.
        public let root: BDKJSValue
    }

    let scope: BDKServiceScope

    init(contents: BDKWebContents) {
        scope = BDKServiceScope(runtime: contents.profile.runtime, profile: nil, contents: contents)
    }

    /// Runs `source` and returns its value, which must be JSON-serialisable.
    public func evaluate(
        _ source: String, in world: World = .isolated("app"), frame: FrameSelector = .main,
        userGesture: Bool = false
    ) async throws -> BDKJSValue {
        struct Arguments: Encodable {
            var expression: String
            var world: String
            var frame: String?
            var userGesture: Bool
        }
        struct Output: Decodable {
            var value: BDKJSValue
        }
        let arguments = Arguments(
            expression: source, world: try world.argument, frame: frame.argument,
            userGesture: userGesture)
        return try await scope.call("contents.evaluate", arguments, as: Output.self).value
    }

    /// Calls `function`, a name or a function expression, with `arguments` encoded as JSON, and
    /// returns its value.
    public func call(
        _ function: String, arguments: [BDKJSValue], in world: World = .isolated("app"),
        frame: FrameSelector = .main
    ) async throws -> BDKJSValue {
        let list = arguments.map(\.jsonString).joined(separator: ",")
        return try await evaluate("(\(function))(\(list))", in: world, frame: frame)
    }

    public func insertCSS(_ css: String, frame: FrameSelector = .all) async throws -> InjectedStyle
    {
        struct Arguments: Encodable {
            var css: String
            var frame: String?
        }
        struct Output: Decodable {
            var id: String
        }
        let output = try await scope.call(
            "contents.insertCSS", Arguments(css: css, frame: frame.argument), as: Output.self)
        return InjectedStyle(id: output.id)
    }

    public func remove(_ style: InjectedStyle) async throws {
        struct Arguments: Encodable {
            var id: String
        }
        try await scope.run("contents.removeCSS", Arguments(id: style.id))
    }

    /// The page's frames: id, parent, URL and origin.
    public func frames() async throws -> [BDKFrameInfo] {
        struct Output: Decodable {
            var frames: [BDKFrameInfo]
        }
        return try await scope.call("contents.frames", BDKVoid(), as: Output.self).frames
    }

    /// The elements `selector` matches, at most `limit` when given.
    public func query(
        _ selector: String, frame: FrameSelector = .main, limit: Int? = nil
    ) async throws -> [BDKElementInfo] {
        struct Arguments: Encodable {
            var selector: String
            var frame: String?
            var limit: Int?
        }
        struct Output: Decodable {
            var elements: [BDKElementInfo]
        }
        let arguments = Arguments(selector: selector, frame: frame.argument, limit: limit)
        return try await scope.call("contents.query", arguments, as: Output.self).elements
    }

    public func snapshot(_ kind: BDKSnapshotKind) async throws -> Snapshot {
        struct Arguments: Encodable {
            var kind: BDKSnapshotKind
        }
        struct Output: Decodable {
            var kind: BDKSnapshotKind
            var root: BDKJSValue
        }
        let output = try await scope.call(
            "contents.snapshot", Arguments(kind: kind), as: Output.self)
        return Snapshot(kind: output.kind, root: output.root)
    }
}

extension Scripting.World {
    /// The `world` argument: "page", or the name of an isolated world.
    fileprivate var argument: String {
        get throws {
            switch self {
            case .page:
                return "page"
            case .isolated("page"), .isolated(""):
                throw BDKError(
                    domain: .broker, code: "invalidArguments",
                    message: "an isolated world needs a name other than \"\" and \"page\"")
            case .isolated(let name):
                return name
            }
        }
    }
}

extension Scripting.FrameSelector {
    var argument: String? {
        if case .frame(let id) = self { id } else { nil }
    }
}

extension BDKFrameInfo {
    public typealias ID = String
}

extension BDKWebContents {
    /// Evaluate scripts, inject CSS, query, snapshot and drive the page.
    public var scripting: Scripting {
        Scripting(contents: self)
    }
}
