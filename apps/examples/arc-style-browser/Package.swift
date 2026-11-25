// swift-tools-version: 6.0
import Foundation
import PackageDescription

// The runtime the application links. With a packaged runtime resolved at .bdk/runtime (`pnpm chromium:runtime`),
// or the Frameworks directory of a local engine build in BDK_RUNTIME_FRAMEWORKS (for instance
// ~/.bdk/chromium/managed/src/out/release/Chromium.app/Contents/Frameworks), the example links its Chromium
// framework and runs on it; scripts/bundle.sh copies the framework into the bundle. Without one, or with
// BDK_RUNTIME=stub, it runs on the in-memory stub runtime. The unit tests always use the stub, in process.
let repositoryRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("../../..").standardizedFileURL
let runtimeFrameworks =
    Context.environment["BDK_RUNTIME_FRAMEWORKS"]
    ?? repositoryRoot.appendingPathComponent(".bdk/runtime/Chromium.app/Contents/Frameworks").path
let usesChromium =
    Context.environment["BDK_RUNTIME"] != "stub"
    && FileManager.default.fileExists(atPath: runtimeFrameworks + "/Chromium Framework.framework")

let runtimeDependencies: [Target.Dependency] =
    usesChromium ? [] : [.product(name: "BDKTestSupport", package: "BDK")]
let runtimeSwiftSettings: [SwiftSetting] = usesChromium ? [.define("BDK_CHROMIUM_RUNTIME")] : []
let runtimeLinkerSettings: [LinkerSetting] =
    usesChromium
    ? [.unsafeFlags(["-F", runtimeFrameworks, "-framework", "Chromium Framework"])] : []

// The example is its own package so that it builds and tests from the command line with SwiftPM. It depends
// on the repository's root package by relative path, as an application would depend on BDK by URL.
let package = Package(
    name: "ArcStyleBrowser",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "ArcStyleBrowser", targets: ["ArcBrowser"])
    ],
    dependencies: [
        .package(name: "BDK", path: "../../..")
    ],
    targets: [
        // The layout model (spaces, pinned pages, splits, windows), the session file, launch options and
        // the status report. No BDK: the end-to-end tests use it too.
        .target(name: "ArcBrowserCore", path: "Sources/ArcBrowserCore"),
        // The application over the SDK: pages, windows, extension placement, automation.
        .target(
            name: "ArcBrowserKit",
            dependencies: ["ArcBrowserCore", .product(name: "BDK", package: "BDK")],
            path: "Sources/ArcBrowserKit"
        ),
        // The executable. Sources/ArcBrowser/RuntimeSource.swift is the one file that knows which runtime it
        // runs on.
        .executableTarget(
            name: "ArcBrowser",
            dependencies: [
                "ArcBrowserCore", "ArcBrowserKit", .product(name: "BDK", package: "BDK"),
            ] + runtimeDependencies,
            path: "Sources/ArcBrowser",
            swiftSettings: runtimeSwiftSettings,
            linkerSettings: runtimeLinkerSettings
        ),
        .testTarget(
            name: "ArcBrowserTests",
            dependencies: [
                "ArcBrowserCore", "ArcBrowserKit",
                .product(name: "BDK", package: "BDK"),
                .product(name: "BDKTestSupport", package: "BDK"),
            ],
            path: "Tests/ArcBrowserTests"
        ),
        // Launches the built .app. Run through arc-style-browser:test-e2e, which builds the bundle first.
        .testTarget(
            name: "ArcBrowserE2ETests",
            dependencies: ["ArcBrowserCore"],
            path: "Tests/ArcBrowserE2ETests"
        ),
    ],
    cxxLanguageStandard: .cxx2b
)
