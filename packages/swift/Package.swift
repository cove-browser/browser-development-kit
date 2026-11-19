// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "BDK",
    platforms: [.macOS(.v14)],
    products: [.library(name: "BDK", targets: ["BDK"])],
    targets: [
        .target(name: "BDK"),
        .testTarget(name: "BDKTests", dependencies: ["BDK"]),
    ]
)
