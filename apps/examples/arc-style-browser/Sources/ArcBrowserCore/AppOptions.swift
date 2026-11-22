import Foundation

/// The launch arguments the tests use; a person passes none.
///
/// `--support-dir <dir>`: where the session goes, and the engine's data with the real runtime.
/// `--status-file <file>`: a JSON report (`AppStatus`) rewritten on every change.
/// `--do <action>` (repeatable): what to do once the window is up, in order (`Action`).
/// `--fresh`: ignore a saved session. `--quit-after-actions`: quit once the actions ran.
/// `--background`: never activate or bring a window to the front (the end-to-end tests run beside
/// whatever the person is doing).
public struct AppOptions: Equatable, Sendable {
    public var supportDirectory: URL?
    public var statusFile: URL?
    public var actions: [Action] = []
    public var fresh = false
    public var quitAfterActions = false
    public var background = false

    /// One thing the application does as a person would, through the same code as its menus.
    public enum Action: Equatable, Sendable {
        /// Opens a new page in the focused window's space and shows it.
        case open(String)
        /// Adds the next page of the focused window's space that no window shows to the split.
        case splitNext
        /// The focused window shows the named space.
        case switchSpace(String)
        /// Moves the focused page to the named space.
        case moveToSpace(String)
        /// Opens a window showing the named space.
        case newWindow(String)
        /// Moves the focused page of the focused window to the window at this index.
        case moveToWindow(Int)
        /// Pins the focused page.
        case pin
        /// Opens the bundled settings page.
        case settings
        /// Runs the bundled automation script over the first page of each space.
        case automation
        /// Focuses the window at this index.
        case focusWindow(Int)
        /// Installs the unpacked extension in this directory in the focused space's profile.
        case installExtension(String)

        public init?(_ text: String) {
            let parts = text.split(separator: "=", maxSplits: 1).map(String.init)
            if parts.count == 1, let action = Self.plain[parts[0]] {
                self = action
            } else if parts.count == 2, let make = Self.withValue[parts[0]],
                let action = make(parts[1])
            {
                self = action
            } else {
                return nil
            }
        }

        private static let plain: [String: Action] = [
            "split-next": .splitNext, "pin": .pin, "settings": .settings, "automation": .automation,
        ]

        private static let withValue: [String: @Sendable (String) -> Action?] = [
            "open": { .open($0) },
            "switch-space": { .switchSpace($0) },
            "move-to-space": { .moveToSpace($0) },
            "new-window": { .newWindow($0) },
            "move-to-window": { Int($0).map(Action.moveToWindow) },
            "focus-window": { Int($0).map(Action.focusWindow) },
            "install-extension": { .installExtension($0) },
        ]
    }

    public enum Failure: Error, Equatable, CustomStringConvertible {
        case missingValue(String)
        case unknownAction(String)
        case unknownArgument(String)

        public var description: String {
            switch self {
            case .missingValue(let flag): "\(flag) needs a value"
            case .unknownAction(let action): "unknown action \(action)"
            case .unknownArgument(let argument): "unknown argument \(argument)"
            }
        }
    }

    public init() {}

    /// Parses `CommandLine.arguments`. Arguments AppKit adds (`-NSDocumentRevisionsDebugMode` and the
    /// like, a `-` and a value) are skipped.
    public static func parse(_ arguments: [String]) throws -> AppOptions {
        var options = AppOptions()
        var rest = arguments.dropFirst()[...]
        func value(_ flag: String) throws -> String {
            guard let next = rest.popFirst() else { throw Failure.missingValue(flag) }
            return next
        }
        while let argument = rest.popFirst() {
            switch argument {
            case "--support-dir":
                options.supportDirectory = URL(fileURLWithPath: try value(argument))
            case "--status-file": options.statusFile = URL(fileURLWithPath: try value(argument))
            case "--do":
                let text = try value(argument)
                guard let action = Action(text) else { throw Failure.unknownAction(text) }
                options.actions.append(action)
            case "--fresh": options.fresh = true
            case "--quit-after-actions": options.quitAfterActions = true
            case "--background": options.background = true
            case let flag where flag.hasPrefix("-") && !flag.hasPrefix("--"): _ = rest.popFirst()
            default: throw Failure.unknownArgument(argument)
            }
        }
        return options
    }
}
