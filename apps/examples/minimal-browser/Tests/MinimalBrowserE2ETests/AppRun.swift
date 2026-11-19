import AppKit
import Foundation
import MinimalBrowserCore

/// One launch of the built application: its own support directory and status file, a quit that goes
/// through `applicationShouldTerminate` and a check of the exit status.
final class AppRun {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    /// The bundle to launch: `MINIMAL_BROWSER_APP`, or the one `scripts/bundle.sh` builds.
    static var bundleURL: URL {
        if let path = ProcessInfo.processInfo.environment["MINIMAL_BROWSER_APP"] {
            return URL(fileURLWithPath: path)
        }
        return URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent(".build/MinimalBrowser.app")
    }

    let supportDirectory: URL
    let statusFile: URL
    private let process = Process()

    /// A directory for the test's session files, removed with `cleanUp()`.
    static func makeSupportDirectory() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("minimal-browser-e2e-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    init(supportDirectory: URL, arguments: [String] = []) throws {
        let executable = Self.bundleURL.appendingPathComponent("Contents/MacOS/MinimalBrowser")
        guard FileManager.default.isExecutableFile(atPath: executable.path) else {
            throw Failure(
                description:
                    "\(executable.path) does not exist; run `pnpm exec nx run minimal-browser:build`"
            )
        }
        self.supportDirectory = supportDirectory
        statusFile = supportDirectory.appendingPathComponent("status-\(UUID().uuidString).json")
        process.executableURL = executable
        process.arguments =
            ["--support-dir", supportDirectory.path, "--status-file", statusFile.path] + arguments
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        try process.run()
    }

    /// The latest status once `condition` holds, polling the status file.
    func waitForStatus(
        timeout: Duration = .seconds(20), _ condition: (AppStatus) -> Bool
    ) async throws -> AppStatus {
        let deadline = ContinuousClock.now + timeout
        var latest: AppStatus?
        while ContinuousClock.now < deadline {
            guard process.isRunning else {
                throw Failure(
                    description:
                        "the application exited early with status \(process.terminationStatus)")
            }
            if let status = try? AppStatus.read(from: statusFile) {
                latest = status
                if condition(status) { return status }
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        throw Failure(
            description:
                "timed out waiting for the application; last status: \(String(describing: latest))")
    }

    /// Asks the application to quit as the Dock or the system would (the quit Apple event), waits for it to
    /// exit and returns the exit status. A hung quit is killed and reported as a failure.
    @discardableResult
    func quitGracefully(timeout: Duration = .seconds(20)) async throws -> Int32 {
        NSRunningApplication(processIdentifier: process.processIdentifier)?.terminate()
        let deadline = ContinuousClock.now + timeout
        while process.isRunning, ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        if process.isRunning {
            process.terminate()
            throw Failure(description: "the application did not quit within \(timeout)")
        }
        return process.terminationStatus
    }

    /// Stops a run a test abandoned.
    func kill() {
        if process.isRunning { process.terminate() }
    }
}
