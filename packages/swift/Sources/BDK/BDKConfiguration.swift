import Foundation

/// What the runtime needs to start: the resolved configuration `bdk` produced at build time.
///
/// The configuration carries the resolved JSON and nothing that route resolution would need; `bdk`
/// resolves routes, not the SDK.
public struct BDKConfiguration: Sendable {
    /// The file name of the resolved configuration inside a bundle.
    public static let resolvedFileName = "bdk.resolved.json"

    /// The contents of `bdk.resolved.json`.
    public var resolvedJSON: String
    /// The application bundle; pages and scripts are read from here.
    public var bundleURL: URL?
    public var launch: LaunchOptions
    /// UI-request handlers (swift-api.md section 10). Set the handler of a `custom` surface here.
    /// The object is shared by copies of the configuration; the runtime copies its handlers at start.
    public var ui = BDKUIHandlers()
    /// Application commands and events (swift-api.md section 12).
    public var commands = BDKCommandRegistry()
    /// Application settings (swift-api.md section 12.3).
    public var settings = BDKSettingRegistry()
    /// Provider keys and implementations (swift-api.md section 11.2).
    public var services = BDKServiceRegistry()

    /// Everything the application declares, for code generation: commands, events and settings.
    public var declarations: BDKDeclarations {
        BDKDeclarations(
            commands: commands.commands.map(\.declaration), events: commands.events,
            settings: settings.settings)
    }

    public init(resolvedJSON: String, bundleURL: URL? = nil, launch: LaunchOptions = .init()) {
        self.resolvedJSON = resolvedJSON
        self.bundleURL = bundleURL
        self.launch = launch
    }

    /// Reads `bdk.resolved.json` from the bundle's resources. The runtime verifies its contents at
    /// start; this only reads them.
    public static func load(from bundle: Bundle = .main) throws -> BDKConfiguration {
        guard let url = bundle.url(forResource: "bdk.resolved", withExtension: "json") else {
            throw BDKError(
                domain: .config, code: "resolvedMissing",
                message: "\(resolvedFileName) is not in the bundle \(bundle.bundlePath); "
                    + "the build phase that resolves bdk.toml must copy it into the resources")
        }
        let text: String
        do {
            text = try String(contentsOf: url, encoding: .utf8)
        } catch {
            throw BDKError(
                domain: .config, code: "resolvedMissing",
                message: "\(resolvedFileName) in \(bundle.bundlePath) cannot be read: \(error)")
        }
        return BDKConfiguration(resolvedJSON: text, bundleURL: bundle.bundleURL)
    }
}

public struct LaunchOptions: Sendable {
    /// Replaces the user data directory. Tests only.
    public var userDataDirectory: URL?

    public init(userDataDirectory: URL? = nil) {
        self.userDataDirectory = userDataDirectory
    }
}
