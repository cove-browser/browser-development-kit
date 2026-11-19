import Foundation

/// What the command line of the application can set. The flags exist so that the end-to-end tests can steer
/// the application; a person launching it normally passes none.
///
/// | Flag | Effect |
/// |---|---|
/// | `--support-dir <path>` | Keep the session in this directory instead of Application Support. |
/// | `--status-file <path>` | Write a JSON report of pages and windows here whenever they change. |
/// | `--navigate <page>=<url>` | After launch, load the URL in page 0 or 1. May repeat. |
/// | `--snapshot-file <path>` | Write a PNG of the window's contents here once the pages are shown. |
/// | `--private` | Show each page in its own private profile, with no session saved or restored. |
/// | `--crash-page <page>` | After launch, crash page 0 or 1's renderer (`chrome://crash`). May repeat. |
/// | `--quit-after-report` | Quit once the first report shows every page visible (and every crashed page recovered). |
public struct AppOptions: Equatable, Sendable {
    public struct Navigation: Equatable, Sendable {
        public var page: Int
        public var url: URL

        public init(page: Int, url: URL) {
            self.page = page
            self.url = url
        }
    }

    public enum Failure: Error, Equatable, CustomStringConvertible {
        case missingValue(String)
        case invalidNavigation(String)
        case invalidPage(String)

        public var description: String {
            switch self {
            case .missingValue(let flag): "\(flag) needs a value"
            case .invalidNavigation(let value):
                "--navigate expects <page>=<url> with a page of 0 or 1, not \(value)"
            case .invalidPage(let value): "--crash-page expects 0 or 1, not \(value)"
            }
        }
    }

    public var supportDirectory: URL?
    public var statusFile: URL?
    public var snapshotFile: URL?
    public var navigations: [Navigation] = []
    public var crashPages: [Int] = []
    public var privatePages = false
    public var quitAfterReport = false

    public init(
        supportDirectory: URL? = nil, statusFile: URL? = nil, snapshotFile: URL? = nil,
        navigations: [Navigation] = [], crashPages: [Int] = [], privatePages: Bool = false,
        quitAfterReport: Bool = false
    ) {
        self.supportDirectory = supportDirectory
        self.statusFile = statusFile
        self.snapshotFile = snapshotFile
        self.navigations = navigations
        self.crashPages = crashPages
        self.privatePages = privatePages
        self.quitAfterReport = quitAfterReport
    }

    /// Reads the flags above from `arguments` (the first is the program name). Anything else is ignored,
    /// because macOS adds arguments of its own.
    public static func parse(_ arguments: [String]) throws -> AppOptions {
        var options = AppOptions()
        var remaining = arguments.dropFirst()[...]
        func value(for flag: String) throws -> String {
            guard let next = remaining.popFirst() else { throw Failure.missingValue(flag) }
            return next
        }
        while let argument = remaining.popFirst() {
            switch argument {
            case "--support-dir":
                options.supportDirectory = URL(fileURLWithPath: try value(for: argument))
            case "--status-file":
                options.statusFile = URL(fileURLWithPath: try value(for: argument))
            case "--snapshot-file":
                options.snapshotFile = URL(fileURLWithPath: try value(for: argument))
            case "--navigate":
                options.navigations.append(try navigation(try value(for: argument)))
            case "--crash-page":
                let text = try value(for: argument)
                guard let page = Int(text), (0...1).contains(page) else {
                    throw Failure.invalidPage(text)
                }
                options.crashPages.append(page)
            case "--private":
                options.privatePages = true
            case "--quit-after-report":
                options.quitAfterReport = true
            default:
                break
            }
        }
        return options
    }

    private static func navigation(_ text: String) throws -> Navigation {
        let parts = text.split(separator: "=", maxSplits: 1).map(String.init)
        guard parts.count == 2, let page = Int(parts[0]), (0...1).contains(page),
            let url = URL(string: parts[1]), url.scheme != nil
        else { throw Failure.invalidNavigation(text) }
        return Navigation(page: page, url: url)
    }
}
