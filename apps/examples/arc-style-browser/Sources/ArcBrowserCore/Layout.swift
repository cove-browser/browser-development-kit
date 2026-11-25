import Foundation

/// The application's own model of what it shows: spaces of pages, pinned pages, and windows that
/// each show one space with up to three of its pages side by side. BDK knows none of this; it only
/// sees page views entering and leaving windows (docs/planning/amendments.md section 2.1).
public struct Layout: Codable, Equatable, Sendable {
    public struct PageID: Hashable, Codable, Sendable, Comparable, CustomStringConvertible,
        CodingKeyRepresentable
    {
        public let rawValue: String
        public init(_ rawValue: String) { self.rawValue = rawValue }
        public static func make() -> PageID { PageID(UUID().uuidString) }
        public static func < (lhs: PageID, rhs: PageID) -> Bool { lhs.rawValue < rhs.rawValue }
        public var description: String { rawValue }

        // Encoded as a JSON object key, so the session file's page table is readable.
        public var codingKey: any CodingKey { Key(stringValue: rawValue) }
        public init?<T: CodingKey>(codingKey: T) { rawValue = codingKey.stringValue }

        public init(from decoder: any Decoder) throws {
            rawValue = try decoder.singleValueContainer().decode(String.self)
        }

        public func encode(to encoder: any Encoder) throws {
            var container = encoder.singleValueContainer()
            try container.encode(rawValue)
        }

        private struct Key: CodingKey {
            let stringValue: String
            var intValue: Int? { nil }
            init(stringValue: String) { self.stringValue = stringValue }
            init?(intValue: Int) { nil }
        }
    }

    public struct SpaceID: Hashable, Codable, Sendable, CustomStringConvertible {
        public let rawValue: String
        public init(_ rawValue: String) { self.rawValue = rawValue }
        public static func make() -> SpaceID { SpaceID(UUID().uuidString) }
        public var description: String { rawValue }

        public init(from decoder: any Decoder) throws {
            rawValue = try decoder.singleValueContainer().decode(String.self)
        }

        public func encode(to encoder: any Encoder) throws {
            var container = encoder.singleValueContainer()
            try container.encode(rawValue)
        }
    }

    public struct WindowID: Hashable, Codable, Sendable, CustomStringConvertible {
        public let rawValue: String
        public init(_ rawValue: String) { self.rawValue = rawValue }
        public static func make() -> WindowID { WindowID(UUID().uuidString) }
        public var description: String { rawValue }

        public init(from decoder: any Decoder) throws {
            rawValue = try decoder.singleValueContainer().decode(String.self)
        }

        public func encode(to encoder: any Encoder) throws {
            var container = encoder.singleValueContainer()
            try container.encode(rawValue)
        }
    }

    /// A page the application keeps. Its navigation state is the runtime's; this is what the
    /// sidebar shows before the page has loaded, and what a relaunch falls back to.
    public struct Page: Codable, Equatable, Sendable {
        public var url: String
        public var title: String
        /// The name of the BDK profile the page was made in. A page keeps it when it moves to
        /// another space, as its contents do, so a relaunch gives it back the same cookies and
        /// storage. Nil in a session saved before pages recorded it: the space's profile then.
        public var profile: String?

        public init(url: String, title: String = "", profile: String? = nil) {
            self.url = url
            self.title = title
            self.profile = profile
        }
    }

    /// A group of pages with a profile of its own. Pinned pages stay at the top of the sidebar.
    public struct Space: Codable, Equatable, Sendable, Identifiable {
        public let id: SpaceID
        public var name: String
        /// The name of the BDK profile the space's pages use.
        public var profile: String
        public var pinned: [PageID]
        public var pages: [PageID]
        /// What a window shows when it switches to this space.
        public var lastShown: [PageID]

        public init(id: SpaceID = .make(), name: String, profile: String) {
            self.id = id
            self.name = name
            self.profile = profile
            pinned = []
            pages = []
            lastShown = []
        }

        /// Pinned pages first, then the others, in sidebar order.
        public var allPages: [PageID] { pinned + pages }
    }

    /// One window: the space it shows and the panes of its split, left to right.
    public struct Window: Codable, Equatable, Sendable, Identifiable {
        public let id: WindowID
        public var space: SpaceID
        public var panes: [PageID]
        public var focusedPane: Int

        public init(id: WindowID = .make(), space: SpaceID, panes: [PageID] = []) {
            self.id = id
            self.space = space
            self.panes = panes
            focusedPane = 0
        }

        public var focusedPage: PageID? {
            panes.indices.contains(focusedPane) ? panes[focusedPane] : panes.first
        }
    }

    public enum Failure: Error, Equatable, Sendable {
        case unknownPage(PageID)
        case unknownSpace(SpaceID)
        case unknownWindow(WindowID)
        case splitFull(limit: Int)
        case lastWindow
        case invalid(String)
    }

    /// At most this many pages side by side. More than two follow risk R2: each is foreground for
    /// BDK's prompts, and extensions see only the focused one as the active tab.
    public static let maximumPanes = 3

    public internal(set) var spaces: [Space]
    public internal(set) var windows: [Window]
    public internal(set) var pages: [PageID: Page]

    public init(spaces: [Space] = [], windows: [Window] = [], pages: [PageID: Page] = [:]) {
        self.spaces = spaces
        self.windows = windows
        self.pages = pages
    }

    // MARK: Queries

    public func space(_ id: SpaceID) throws -> Space {
        guard let space = spaces.first(where: { $0.id == id }) else {
            throw Failure.unknownSpace(id)
        }
        return space
    }

    public func window(_ id: WindowID) throws -> Window {
        guard let window = windows.first(where: { $0.id == id }) else {
            throw Failure.unknownWindow(id)
        }
        return window
    }

    public func spaceOf(_ page: PageID) -> Space? {
        spaces.first { $0.allPages.contains(page) }
    }

    /// The window that shows the page, if any.
    public func windowShowing(_ page: PageID) -> Window? {
        windows.first { $0.panes.contains(page) }
    }

    /// Every page some window shows.
    public var visiblePages: Set<PageID> {
        Set(windows.flatMap(\.panes))
    }

    /// Pages no window shows: those in hidden spaces, and the others of a shown space.
    public var hiddenPages: Set<PageID> {
        Set(pages.keys).subtracting(visiblePages)
    }
}
