import BDK
import Foundation

/// Keeps the session state of every page between launches: one JSON file in the application's support
/// directory. The state of each page is the runtime's opaque `BDKSessionState`; this file only orders them.
public struct SessionStore: Sendable {
    public struct Snapshot: Codable, Equatable, Sendable {
        public static let currentVersion = 1

        public var version = Snapshot.currentVersion
        public var pages: [BDKSessionState]

        public init(pages: [BDKSessionState]) {
            self.pages = pages
        }
    }

    public enum Failure: Error, Equatable, CustomStringConvertible {
        case unreadable(String)
        case unsupportedVersion(Int)

        public var description: String {
            switch self {
            case .unreadable(let reason): "the session file cannot be read: \(reason)"
            case .unsupportedVersion(let version):
                "the session file has version \(version); this build reads version \(Snapshot.currentVersion)"
            }
        }
    }

    public static let fileName = "session.json"

    public let directory: URL

    public init(directory: URL) {
        self.directory = directory
    }

    public var fileURL: URL {
        directory.appendingPathComponent(Self.fileName)
    }

    /// The default directory: `<Application Support>/<bundle identifier>`.
    public static func defaultDirectory(bundleIdentifier: String) throws -> URL {
        try FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil,
            create: true
        ).appendingPathComponent(bundleIdentifier, isDirectory: true)
    }

    /// Replaces the saved session. The write is atomic, so a crash while saving leaves the previous file.
    public func save(_ snapshot: Snapshot) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(snapshot).write(to: fileURL, options: .atomic)
    }

    /// The saved session, or nil when there is none. A file that cannot be read throws, so that the caller
    /// can log it and start fresh instead of crashing.
    public func load() throws -> Snapshot? {
        guard FileManager.default.fileExists(atPath: fileURL.path) else { return nil }
        let snapshot: Snapshot
        do {
            snapshot = try JSONDecoder().decode(Snapshot.self, from: Data(contentsOf: fileURL))
        } catch {
            throw Failure.unreadable("\(error)")
        }
        guard snapshot.version == Snapshot.currentVersion else {
            throw Failure.unsupportedVersion(snapshot.version)
        }
        return snapshot
    }
}
