// swift-tools-version: 6.0
import PackageDescription

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
        // The application. It is the only target that depends on BDKTestSupport, which is the stub runtime;
        // the dependency and Sources/MinimalBrowser/StubRuntime.swift are what to replace with the real
        // runtime once it exists.
        .executableTarget(
            name: "MinimalBrowser",
            dependencies: [
                "MinimalBrowserKit", "MinimalBrowserCore",
                .product(name: "BDK", package: "BDK"),
                .product(name: "BDKTestSupport", package: "BDK"),
            ],
            path: "Sources/MinimalBrowser"
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
