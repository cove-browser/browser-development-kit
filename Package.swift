// swift-tools-version: 6.0
import PackageDescription

// The manifest lives at the repository root because SwiftPM only resolves a package by URL from there. The
// Swift sources stay under packages/swift and the C ABI under packages/core (docs/planning/amendments.md
// section 4).
let package = Package(
    name: "BDK",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "BDK", targets: ["BDK"]),
        .library(name: "BDKTestSupport", targets: ["BDKTestSupport"]),
    ],
    targets: [
        // The public C headers and the generated ABI hash, reached through symlinks into packages/core. The
        // runtime itself arrives as a binary target (T-PKG-04).
        .target(name: "CBDK", path: "packages/swift/Sources/CBDK", publicHeadersPath: "include"),
        // The in-memory runtime, compiled from packages/core/stub. Swift reaches its test hooks through the
        // CBDKStub module declared next to CBDK.
        .target(
            name: "BDKStubRuntime",
            dependencies: ["CBDK"],
            path: "packages/core/stub",
            exclude: ["stub_internal.h", "bdk_stub.h"],
            sources: ["stub_core.cc", "stub_host.cc"],
            publicHeadersPath: "."
        ),
        .target(
            name: "BDK",
            dependencies: ["CBDK"],
            path: "packages/swift/Sources/BDK"
        ),
        .target(
            name: "BDKTestSupport",
            dependencies: ["BDK", "CBDK", "BDKStubRuntime"],
            path: "packages/swift/Sources/BDKTestSupport"
        ),
        .testTarget(
            name: "BDKTests",
            dependencies: ["BDK", "BDKTestSupport", "CBDK", "BDKStubRuntime"],
            path: "packages/swift/Tests/BDKTests"
        ),
    ],
    cxxLanguageStandard: .cxx2b
)
