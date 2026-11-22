import Foundation

/// What the application saves on quit and restores on launch: its own layout and, per page, the
/// runtime's opaque session state. Chromium restores nothing by itself (swift-api.md section 6.8).
public struct SessionFile: Codable, Equatable, Sendable {
    public static let currentVersion = 1

    public var version: Int
    public var layout: Layout
    /// Each page's `BDKSessionState` bytes, by page id. A page with none starts from its URL.
    public var states: [String: Data]

    public init(layout: Layout, states: [Layout.PageID: Data]) {
        version = Self.currentVersion
        self.layout = layout
        self.states = Dictionary(uniqueKeysWithValues: states.map { ($0.key.rawValue, $0.value) })
    }

    public func state(of page: Layout.PageID) -> Data? {
        states[page.rawValue]
    }

    public enum Failure: Error, Equatable {
        case unsupportedVersion(Int)
        case invalidLayout(String)
    }

    public static func fileURL(in directory: URL) -> URL {
        directory.appendingPathComponent("session.json")
    }

    /// The saved session, nil when there is none. A damaged file, a newer version or a layout that
    /// breaks the model's rules throws: the caller starts from the default layout.
    public static func load(from directory: URL) throws -> SessionFile? {
        let url = fileURL(in: directory)
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        let file = try JSONDecoder().decode(SessionFile.self, from: Data(contentsOf: url))
        guard file.version == currentVersion else { throw Failure.unsupportedVersion(file.version) }
        do {
            try file.layout.validate()
        } catch let failure as Layout.Failure {
            throw Failure.invalidLayout("\(failure)")
        }
        return file
    }

    public func save(to directory: URL) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .prettyPrinted]
        try encoder.encode(self).write(to: Self.fileURL(in: directory), options: .atomic)
    }
}

extension Layout {
    /// The layout of a first launch: two spaces with a profile each, a pinned page in each, and
    /// one window showing the first space.
    public static func initial(urls: InitialURLs = .examples) -> Layout {
        var layout = Layout()
        let personal = layout.addSpace(name: "Personal", profile: "Personal")
        let work = layout.addSpace(name: "Work", profile: "Work")
        do {
            let pinned = try layout.addPage(url: urls.personalPinned, to: personal, pinned: true)
            try layout.addPage(url: urls.personal, to: personal)
            try layout.addPage(url: urls.workPinned, to: work, pinned: true)
            try layout.addPage(url: urls.work, to: work)
            let window = try layout.addWindow(showing: personal)
            try layout.show(pinned, in: window)
        } catch {
            preconditionFailure("the initial layout breaks the model: \(error)")
        }
        return layout
    }

    public struct InitialURLs: Sendable {
        public var personalPinned, personal, workPinned, work: String

        public init(personalPinned: String, personal: String, workPinned: String, work: String) {
            self.personalPinned = personalPinned
            self.personal = personal
            self.workPinned = workPinned
            self.work = work
        }

        public static let examples = InitialURLs(
            personalPinned: "https://example.com/", personal: "https://example.org/",
            workPinned: "https://www.iana.org/domains/reserved", work: "https://example.net/")
    }
}
