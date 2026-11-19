import Foundation
import MinimalBrowserCore
import Testing

@Suite("Launch options")
struct AppOptionsTests {
    @Test("no flags leave everything unset")
    func defaults() throws {
        #expect(try AppOptions.parse(["MinimalBrowser"]) == AppOptions())
    }

    @Test("every flag is read, and a repeated --navigate keeps its order")
    func allFlags() throws {
        let options = try AppOptions.parse([
            "MinimalBrowser", "--support-dir", "/tmp/s", "--status-file", "/tmp/s/status.json",
            "--snapshot-file", "/tmp/s/window.png",
            "--navigate", "0=https://example.net/", "--navigate", "1=https://example.com/a?b=c",
            "--quit-after-report",
        ])
        #expect(options.supportDirectory == URL(fileURLWithPath: "/tmp/s"))
        #expect(options.statusFile == URL(fileURLWithPath: "/tmp/s/status.json"))
        #expect(options.snapshotFile == URL(fileURLWithPath: "/tmp/s/window.png"))
        #expect(
            options.navigations == [
                .init(page: 0, url: URL(string: "https://example.net/")!),
                .init(page: 1, url: URL(string: "https://example.com/a?b=c")!),
            ])
        #expect(options.quitAfterReport)
    }

    @Test("arguments macOS adds are ignored")
    func foreignArguments() throws {
        #expect(
            try AppOptions.parse(["MinimalBrowser", "-NSDocumentRevisionsDebugMode", "YES"])
                == AppOptions())
    }

    @Test("a flag without a value, and a bad navigation, are errors")
    func invalid() {
        #expect(throws: AppOptions.Failure.missingValue("--status-file")) {
            try AppOptions.parse(["MinimalBrowser", "--status-file"])
        }
        for bad in ["2=https://example.com/", "x=https://example.com/", "0=nonsense", "0"] {
            #expect(throws: AppOptions.Failure.invalidNavigation(bad)) {
                try AppOptions.parse(["MinimalBrowser", "--navigate", bad])
            }
        }
    }
}

@Suite("Address input")
struct AddressInputTests {
    @Test(
        "text becomes the URL a person meant",
        arguments: [
            ("https://example.com/a", "https://example.com/a"),
            ("  example.org  ", "https://example.org"),
            ("example.net/path?q=1", "https://example.net/path?q=1"),
            ("localhost:8080", "https://localhost:8080"),
            ("about:blank", "about:blank"),
        ])
    func accepted(text: String, expected: String) {
        #expect(AddressInput.url(from: text)?.absoluteString == expected)
    }

    @Test("what is not an address is refused", arguments: ["", "   ", "two words", "word"])
    func refused(text: String) {
        #expect(AddressInput.url(from: text) == nil)
    }
}

@Suite("Status report")
struct AppStatusTests {
    private func page(_ index: Int, visible: Bool = true, entries: Int = 1) -> AppStatus.Page {
        AppStatus.Page(
            index: index, url: "https://example.com/", title: "Example", currentIndex: entries - 1,
            entries: Array(
                repeating: .init(url: "https://example.com/", title: "Example"), count: entries),
            isVisible: visible)
    }

    private func status(_ windows: [AppStatus.Window]) -> AppStatus {
        AppStatus(phase: .running, runtime: "stub", restored: false, windows: windows)
    }

    @Test("two visible pages in one visible window")
    func showsTwo() {
        let window = AppStatus.Window(title: "t", isVisible: true, pages: [page(0), page(1)])
        #expect(status([window]).showsPages(2))
        #expect(!status([window]).showsPages(3))
    }

    @Test("a hidden page, a hidden window or a second window does not count")
    func notShown() {
        let hiddenPage = AppStatus.Window(
            title: "t", isVisible: true, pages: [page(0), page(1, visible: false)])
        let hiddenWindow = AppStatus.Window(
            title: "t", isVisible: false, pages: [page(0), page(1)])
        let visible = AppStatus.Window(title: "t", isVisible: true, pages: [page(0), page(1)])
        #expect(!status([hiddenPage]).showsPages(2))
        #expect(!status([hiddenWindow]).showsPages(2))
        #expect(!status([visible, visible]).showsPages(2))
        #expect(!status([]).showsPages(2))
    }

    @Test("`loaded` waits for a navigation entry in every page")
    func loaded() {
        let window = AppStatus.Window(
            title: "t", isVisible: true, pages: [page(0), page(1, entries: 0)])
        #expect(status([window]).showsPages(2))
        #expect(!status([window]).showsPages(2, loaded: true))
    }

    @Test("a report survives a write and a read")
    func roundTrip() throws {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent(
            "status-\(UUID()).json")
        defer { try? FileManager.default.removeItem(at: file) }
        let original = status([
            .init(title: "t", isVisible: true, pages: [page(0), page(1, entries: 2)])
        ])
        try original.write(to: file)
        #expect(try AppStatus.read(from: file) == original)
    }
}
