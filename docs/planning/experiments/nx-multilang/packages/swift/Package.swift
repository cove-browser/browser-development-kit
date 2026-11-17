// swift-tools-version:5.9
import PackageDescription
let package = Package(
  name: "BDKDemo",
  targets: [
    .systemLibrary(name: "CBDKCore", path: "Sources/CBDKCore"),
    .executableTarget(name: "BDKDemo", dependencies: ["CBDKCore"], path: "Sources/BDKDemo"),
  ]
)
