import BDKTestSupport
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    /// `page.scripting`, `page.capture` and `page.devTools` (T-AUT-03, T-SVC-09, T-AUT-05). The stub has
    /// no renderer: it answers captures with fixed bytes, the frame list with one frame and the rest
    /// of the scripting commands with `broker.notImplemented`. Real answers are engine scenarios.
    @MainActor
    @Suite("Page automation")
    struct PageAutomationTests {
        // MARK: Scripting

        @Test func theStubHasNoRendererToScript() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                await expectError(.broker, "notImplemented") {
                    _ = try await page.scripting.evaluate("1 + 1")
                }
                await expectError(.broker, "notImplemented") {
                    _ = try await page.scripting.call("f", arguments: [.number(1), .string("a")])
                }
                await expectError(.broker, "notImplemented") {
                    _ = try await page.scripting.query("a", frame: .frame("main"))
                }
                await expectError(.broker, "notImplemented") {
                    _ = try await page.scripting.insertCSS("a{}")
                }
                await expectError(.broker, "notImplemented") {
                    _ = try await page.scripting.snapshot(.text)
                }
            }
        }

        @Test func anIsolatedWorldNeedsAName() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                await expectError(.broker, "invalidArguments") {
                    _ = try await page.scripting.evaluate("1", in: .isolated("page"))
                }
            }
        }

        @Test func framesAreListed() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let frames = try await page.scripting.frames()
                #expect(frames.count == 1)
                #expect(frames[0].id == "main" && frames[0].parent == nil)
            }
        }

        @Test func scriptingAClosedPageIsStale() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                page.destroy()
                await expectError(.contents, "stale") { _ = try await page.scripting.frames() }
            }
        }

        // MARK: Capture

        @Test func aScreenshotIsAPNG() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let png: [UInt8] = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]
                for area in [
                    PageCapture.ScreenshotArea.visible, .fullPage,
                    .rect(CGRect(x: 0, y: 0, width: 10, height: 10)),
                ] {
                    let image = try await page.capture.screenshot(area)
                    #expect(Array(image.prefix(8)) == png)
                }
            }
        }

        @Test func aPDFComesWithProgress() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                var reports: [Double] = []
                let options = PageCapture.PDFOptions(
                    paper: .a4, landscape: true, printBackground: true,
                    margins: .init(all: 0.5), scale: 0.9, pageRanges: "1-2")
                let pdf = try await page.capture.pdf(options) { fraction, _ in
                    reports.append(fraction)
                }
                #expect(String(decoding: pdf.prefix(5), as: UTF8.self) == "%PDF-")
                #expect(reports.last == 1)
                #expect(try await page.capture.pdf().isEmpty == false)
            }
        }

        @Test func capturingAClosedPageIsStale() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                page.destroy()
                await expectError(.contents, "stale") { _ = try await page.capture.screenshot() }
            }
        }

        // MARK: DevTools

        @Test func aDisabledFrontendAndProtocolAreRefused() async throws {
            let configuration = BDKConfiguration(
                resolvedJSON: #"{"devtools":{"frontend":"disabled","protocol":false}}"#)
            try await withStubRuntime(configuration) { runtime in
                let profile = try await runtime.profiles.open("Test")
                let page = try profile.webContents.create()
                #expect(!page.devTools.isOpen)
                await expectError(.devtools, "disabled") { try await page.devTools.present() }
                await expectError(.devtools, "protocolDisabled") {
                    _ = try await page.devTools.attachProtocol()
                }
                #expect(!page.devTools.isOpen)
            }
        }

        @Test func devToolsDoesNotKeepThePageAlive() async throws {
            try await withProfile { _, profile in
                weak var weakPage: BDKWebContents?
                do {
                    let page = try profile.webContents.create()
                    weakPage = page
                    _ = page.devTools
                    page.destroy()
                }
                #expect(weakPage == nil)
            }
        }

        @Test func devToolsOfAClosedPageIsStale() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let devTools = page.devTools
                page.destroy()
                await expectError(.contents, "stale") { try await devTools.present() }
                await expectError(.contents, "stale") { _ = try await devTools.attachProtocol() }
            }
        }
    }
}
