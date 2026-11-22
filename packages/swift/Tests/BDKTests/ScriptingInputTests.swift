import BDKTestSupport
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    /// Synthesised input and `waitFor` on `page.scripting` (T-AUT-07). The stub has no renderer, so it
    /// answers the input commands with `broker.notImplemented`: each wrapper is checked twice, for the
    /// arguments it builds (`Scripting.Input`) and for the broker accepting them (it reaches the
    /// engine's side, which says "not implemented", rather than refusing the shape). `waitFor` is
    /// answered by the runtime core, so it runs for real.
    @MainActor
    @Suite("Scripting input")
    struct ScriptingInputTests {
        let example = URL(string: "https://example.com/page")!

        // MARK: Input

        @Test func aClickNamesItsTargetFrameAndButton() async throws {
            let selector = Scripting.Input.click(
                .selector("#go"), frame: .main, button: .left, clickCount: 1)
            #expect(selector.command == "contents.click")
            #expect(
                selector.arguments
                    == .object([
                        "selector": .string("#go"), "button": .string("left"),
                        "clickCount": .number(1),
                    ]))
            let point = Scripting.Input.click(
                .point(x: 3.5, y: 8), frame: .frame("f1"), button: .right, clickCount: 2)
            #expect(
                point.arguments
                    == .object([
                        "x": .number(3.5), "y": .number(8), "frame": .string("f1"),
                        "button": .string("right"), "clickCount": .number(2),
                    ]))
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                await expectError(.broker, "notImplemented") {
                    try await page.scripting.click(.selector("#go"))
                }
                await expectError(.broker, "notImplemented") {
                    try await page.scripting.click(
                        .point(x: 1, y: 2), button: .middle, clickCount: 2)
                }
            }
        }

        @Test func aHoverNamesItsTarget() async throws {
            let input = Scripting.Input.hover(.point(x: 1, y: 2), frame: .all)
            #expect(input.command == "contents.hover")
            #expect(input.arguments == .object(["x": .number(1), "y": .number(2)]))
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                await expectError(.broker, "notImplemented") {
                    try await page.scripting.hover(.selector("a"), frame: .frame("f1"))
                }
            }
        }

        @Test func typingSendsTheTextAndOptionalSelector() async throws {
            #expect(
                Scripting.Input.type("hi", selector: nil, frame: .main)
                    == .init(command: "contents.type", arguments: .object(["text": .string("hi")])))
            #expect(
                Scripting.Input.type("hi", selector: "input", frame: .frame("f1")).arguments
                    == .object([
                        "text": .string("hi"), "selector": .string("input"),
                        "frame": .string("f1"),
                    ]))
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                await expectError(.broker, "notImplemented") {
                    try await page.scripting.type("hi", into: "input")
                }
            }
        }

        @Test func pressingAKeySendsItsModifiers() async throws {
            #expect(
                Scripting.Input.press("Enter", modifiers: [])
                    == .init(
                        command: "contents.press", arguments: .object(["key": .string("Enter")])))
            #expect(
                Scripting.Input.press("a", modifiers: ["Meta", "Shift"]).arguments
                    == .object([
                        "key": .string("a"),
                        "modifiers": .array([.string("Meta"), .string("Shift")]),
                    ]))
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                await expectError(.broker, "notImplemented") {
                    try await page.scripting.press("a", modifiers: ["Meta"])
                }
            }
        }

        @Test func scrollingSendsTheDistance() async throws {
            #expect(
                Scripting.Input.scroll(selector: nil, dx: 0, dy: 120, frame: .main)
                    == .init(
                        command: "contents.scroll",
                        arguments: .object(["dx": .number(0), "dy": .number(120)])))
            #expect(
                Scripting.Input.scroll(selector: "#list", dx: -4, dy: 0, frame: .frame("f1"))
                    .arguments
                    == .object([
                        "selector": .string("#list"), "dx": .number(-4), "dy": .number(0),
                        "frame": .string("f1"),
                    ]))
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                await expectError(.broker, "notImplemented") {
                    try await page.scripting.scroll(dy: 200)
                }
            }
        }

        @Test func inputOnAClosedPageIsStale() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                page.destroy()
                await expectError(.contents, "stale") {
                    try await page.scripting.click(.selector("a"))
                }
                await expectError(.contents, "stale") {
                    try await page.scripting.waitFor(.load(.finished))
                }
            }
        }

        // MARK: waitFor

        @Test func waitForSendsItsConditionAndDeadline() {
            #expect(
                Scripting.Input.waitFor(.load(.committed), timeout: .seconds(30))
                    == .init(
                        command: "contents.waitFor",
                        arguments: .object([
                            "load": .string("committed"), "timeoutMs": .number(30_000),
                        ])))
            #expect(
                Scripting.Input.waitFor(.url("https://a/*"), timeout: .milliseconds(250)).arguments
                    == .object(["url": .string("https://a/*"), "timeoutMs": .number(250)]))
            #expect(
                Scripting.Input.waitFor(.url("*"), timeout: .microseconds(1)).arguments
                    == .object(["url": .string("*"), "timeoutMs": .number(1)]))
        }

        @Test func waitForALoadReturnsWhenItFinishes() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: example, title: "Page")
                let page = try profile.webContents.create()
                // Both calls are queued before either runs: the stub's clock moves only when nothing
                // is queued, so a wait left alone would reach its deadline before the load starts.
                let waiter = Task { try await page.scripting.waitFor(.load(.finished)) }
                let load = Task { try await page.navigation.load(example) }
                _ = try await load.value
                try await waiter.value
            }
        }

        @Test func waitForAURLMatchesAGlob() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: example, title: "Page")
                let page = try profile.webContents.create()
                let waiter = Task {
                    try await page.scripting.waitFor(.url("https://example.com/*"))
                }
                let load = Task { try await page.navigation.load(example) }
                _ = try await load.value
                try await waiter.value
                // The URL already matches, so a second wait returns at once.
                try await page.scripting.waitFor(.url("*example.com/page"))
            }
        }

        @Test func waitForTimesOut() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                await expectError(.broker, "timeout") {
                    try await page.scripting.waitFor(
                        .url("https://never.example/*"), timeout: .milliseconds(100))
                }
            }
        }
    }
}
