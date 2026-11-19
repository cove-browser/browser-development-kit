import Foundation

/// A report of what the application shows, written to the file named by `--status-file` so that a test
/// outside the process can read it.
public struct AppStatus: Codable, Equatable, Sendable {
    public struct Entry: Codable, Equatable, Sendable {
        public var url: String
        public var title: String

        public init(url: String, title: String) {
            self.url = url
            self.title = title
        }
    }

    public struct Page: Codable, Equatable, Sendable {
        public var index: Int
        public var url: String
        public var title: String
        public var currentIndex: Int
        public var entries: [Entry]
        /// The page's view is in a visible window, is not hidden and has a size.
        public var isVisible: Bool

        public init(
            index: Int, url: String, title: String, currentIndex: Int, entries: [Entry],
            isVisible: Bool
        ) {
            self.index = index
            self.url = url
            self.title = title
            self.currentIndex = currentIndex
            self.entries = entries
            self.isVisible = isVisible
        }
    }

    public struct Window: Codable, Equatable, Sendable {
        public var title: String
        public var isVisible: Bool
        public var pages: [Page]

        public init(title: String, isVisible: Bool, pages: [Page]) {
            self.title = title
            self.isVisible = isVisible
            self.pages = pages
        }
    }

    public enum Phase: String, Codable, Sendable {
        case launching, running, terminating
    }

    public var phase: Phase
    /// Which runtime serves the pages: "stub" until the real runtime exists.
    public var runtime: String
    /// True when the pages were rebuilt from a saved session.
    public var restored: Bool
    public var windows: [Window]

    public init(phase: Phase, runtime: String, restored: Bool, windows: [Window]) {
        self.phase = phase
        self.runtime = runtime
        self.restored = restored
        self.windows = windows
    }

    /// True when one visible window shows `count` visible pages and no other window exists. With `loaded`,
    /// every page also has at least one navigation entry.
    public func showsPages(_ count: Int, loaded: Bool = false) -> Bool {
        windows.count == 1 && windows[0].isVisible
            && windows[0].pages.filter(\.isVisible).count == count
            && (!loaded || windows[0].pages.allSatisfy { $0.currentIndex >= 0 })
    }

    public static func read(from url: URL) throws -> AppStatus {
        try JSONDecoder().decode(AppStatus.self, from: Data(contentsOf: url))
    }

    public func write(to url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: url, options: .atomic)
    }
}
