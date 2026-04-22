// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "bconn-ui",
    platforms: [.macOS(.v13)],
    targets: [
        // C++ 코어 래퍼
        .target(
            name: "BconnCore",
            path: "Sources/BconnCore",
            publicHeadersPath: "include",
            cxxSettings: [
                .unsafeFlags(["-std=c++17", "-O3"])
            ],
            linkerSettings: [
                .linkedFramework("ApplicationServices"),
                .linkedFramework("Cocoa"),
                .linkedLibrary("pthread"),
            ]
        ),
        // Swift UI 앱
        .executableTarget(
            name: "BconnApp",
            dependencies: ["BconnCore"],
            path: "Sources/BconnApp",
            swiftSettings: [
                .interoperabilityMode(.Cxx)
            ],
            linkerSettings: [
                .linkedFramework("ServiceManagement"),
            ]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
