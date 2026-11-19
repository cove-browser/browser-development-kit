// swift-tools-version: 6.0
import Foundation
import PackageDescription

// The runtime the application links. With a packaged runtime resolved at .bdk/runtime (`pnpm chromium:runtime`)
// the example links its Chromium framework and runs on it; scripts/bundle.sh copies the framework into the
// bundle. Without one, or with BDK_RUNTIME=stub, it runs on the in-memory stub runtime. The unit tests always
// use the stub, in process.
let repositoryRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("../../..").standardizedFileURL
let runtimeFrameworks = repositoryRoot.appendingPathComponent(
    ".bdk/runtime/Chromium.app/Contents/Frameworks"
).path
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
    name: "MinimalBrowser",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "MinimalBrowser", targets: ["MinimalBrowser"])
    ],
    dependencies: [
        .package(name: "BDK", path: "../../..")
    ],
    targets: [
        // The example logic that needs no runtime: launch options, the status report and address parsing.
        // The end-to-end tests use it too, so it must not link BDK.
        .target(name: "MinimalBrowserCore", path: "Sources/MinimalBrowserCore"),
        // The session store, which holds the runtime's session states.
        .target(
            name: "MinimalBrowserKit",
            dependencies: [.product(name: "BDK", package: "BDK")],
            path: "Sources/MinimalBrowserKit"
        ),
        // The application. Sources/MinimalBrowser/RuntimeSource.swift is the one file that knows which runtime
        // it runs on.
        .executableTarget(
            name: "MinimalBrowser",
            dependencies: [
                "MinimalBrowserKit", "MinimalBrowserCore",
                .product(name: "BDK", package: "BDK"),
            ] + runtimeDependencies,
            path: "Sources/MinimalBrowser",
            swiftSettings: runtimeSwiftSettings,
            linkerSettings: runtimeLinkerSettings
        ),
        .testTarget(
            name: "MinimalBrowserTests",
            dependencies: [
                "MinimalBrowserKit", "MinimalBrowserCore",
                .product(name: "BDK", package: "BDK"),
                .product(name: "BDKTestSupport", package: "BDK"),
            ],
            path: "Tests/MinimalBrowserTests"
        ),
        // Launches the built .app. Run through minimal-browser:test-e2e, which builds the bundle first.
        .testTarget(
            name: "MinimalBrowserE2ETests",
            dependencies: ["MinimalBrowserCore"],
            path: "Tests/MinimalBrowserE2ETests"
        ),
    ],
    cxxLanguageStandard: .cxx2b
)
