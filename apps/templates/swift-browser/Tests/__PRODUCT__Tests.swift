import BDK
import BDKTestSupport
import Foundation
import Testing

/// Runs against the in-memory stub runtime, in this process: no Chromium, no bundle.
@Suite("{{name}}", .serialized, .timeLimit(.minutes(1)))
@MainActor
struct BrowserTests {
    @Test("a page loads a URL the stub was told about and takes its title")
    func pageLoads() async throws {
        let runtime = try BDKRuntime.startStub()
        let url = URL(string: "https://example.com/")!
        try runtime.stub.scriptNavigation(to: url, title: "Example Domain")
        let profile = try await runtime.profiles.open("Tests")
        let page = try profile.webContents.create(.init())
        try await page.navigate(to: url)
        #expect(page.url == url)
        #expect(page.title == "Example Domain")
    }
}
