import Foundation

/// The report `--status-file` writes: what the application shows, and what the runtime says about
/// each page. The end-to-end tests read it; it needs no runtime to decode.
public struct AppStatus: Codable, Equatable, Sendable {
    public enum Phase: String, Codable, Sendable {
        case launching, running, terminating
    }

    public struct Page: Codable, Equatable, Sendable {
        public var id: String
        public var space: String
        public var pinned: Bool
        public var url: String
        public var title: String
        /// The index of the window whose split shows it, nil when it is hidden.
        public var window: Int?
        public var pane: Int?
        /// Its view is in a window, not hidden, with a size.
        public var onScreen: Bool
        /// The runtime's visibility for the page.
        public var runtimeVisible: Bool
        public var entries: Int

        public init(
            id: String, space: String, pinned: Bool, url: String, title: String, window: Int?,
            pane: Int?, onScreen: Bool, runtimeVisible: Bool, entries: Int
        ) {
            self.id = id
            self.space = space
            self.pinned = pinned
            self.url = url
            self.title = title
            self.window = window
            self.pane = pane
            self.onScreen = onScreen
            self.runtimeVisible = runtimeVisible
            self.entries = entries
        }
    }

    public struct Window: Codable, Equatable, Sendable {
        public var space: String
        public var panes: Int
        public var isVisible: Bool

        public init(space: String, panes: Int, isVisible: Bool) {
            self.space = space
            self.panes = panes
            self.isVisible = isVisible
        }
    }

    public var phase: Phase
    public var runtime: String
    public var restored: Bool
    public var actionsDone: Bool
    public var windows: [Window]
    public var pages: [Page]
    /// The bundled settings page's URL once it was opened.
    public var settingsURL: String?
    /// The automation task's result, as JSON text, once it finished.
    public var automationResult: String?
    public var errors: [String]

    public init(
        phase: Phase, runtime: String, restored: Bool, actionsDone: Bool, windows: [Window],
        pages: [Page], settingsURL: String? = nil, automationResult: String? = nil,
        errors: [String] = []
    ) {
        self.phase = phase
        self.runtime = runtime
        self.restored = restored
        self.actionsDone = actionsDone
        self.windows = windows
        self.pages = pages
        self.settingsURL = settingsURL
        self.automationResult = automationResult
        self.errors = errors
    }

    public func write(to url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .prettyPrinted]
        try encoder.encode(self).write(to: url, options: .atomic)
    }

    public static func read(from url: URL) throws -> AppStatus {
        try JSONDecoder().decode(AppStatus.self, from: Data(contentsOf: url))
    }
}
