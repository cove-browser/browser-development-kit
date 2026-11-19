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
        // The in-memory runtime: the runtime core (packages/core/runtime) with the stub's backend
        // (packages/core/stub). Swift reaches its test hooks through the CBDKStub module declared next to CBDK.
        .target(
            name: "BDKStubRuntime",
            dependencies: ["CBDK"],
            path: "packages/core",
            sources: ["runtime/core.cc", "runtime/core_host.cc", "stub/stub_backend.cc"],
            publicHeadersPath: "stub",
            cxxSettings: [.headerSearchPath("runtime"), .headerSearchPath("generated")]
        ),
        .target(
            name: "BDK",
            dependencies: ["CBDK"],
            path: "packages/swift/Sources/BDK"
        ),
        // The stub's BDKApplicationBase, a plain NSApplication subclass. The real runtime exports its own.
        .target(
            name: "BDKStubApplication",
            dependencies: ["CBDK"],
            path: "packages/swift/Sources/BDKStubApplication"
        ),
        .target(
            name: "BDKTestSupport",
            dependencies: ["BDK", "CBDK", "BDKStubRuntime", "BDKStubApplication"],
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
