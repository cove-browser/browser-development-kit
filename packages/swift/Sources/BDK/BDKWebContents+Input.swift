import Foundation

/// Synthesised input and waiting (swift-api.md section 6.5): `contents.click`, `hover`, `type`, `press`,
/// `scroll` and `waitFor`.
extension Scripting {
    /// Where synthesised input lands: the first element a selector matches, or a point in the frame's
    /// viewport in CSS pixels.
    public enum InputTarget: Sendable, Equatable {
        case selector(String)
        case point(x: Double, y: Double)
    }

    /// What `waitFor` waits for. The runtime core answers these two; a selector or a script predicate
    /// is for `query` and `evaluate` to poll.
    public enum WaitCondition: Sendable, Equatable {
        /// The navigation reached `committed` or `finished` (`BDKLoadCondition`).
        case load(BDKLoadCondition)
        /// The page's URL matches a pattern in which `*` matches any run of characters.
        case url(String)
    }

    /// Clicks `target`.
    public func click(
        _ target: InputTarget, frame: FrameSelector = .main, button: BDKMouseButton = .left,
        clickCount: Int = 1
    ) async throws {
        try await send(.click(target, frame: frame, button: button, clickCount: clickCount))
    }

    /// Moves the pointer over `target`.
    public func hover(_ target: InputTarget, frame: FrameSelector = .main) async throws {
        try await send(.hover(target, frame: frame))
    }

    /// Types `text` into the focused element, or into the one `selector` matches.
    public func type(
        _ text: String, into selector: String? = nil, frame: FrameSelector = .main
    ) async throws {
        try await send(.type(text, selector: selector, frame: frame))
    }

    /// Presses a key, named as in `KeyboardEvent.key` ("Enter", "a"), with `modifiers` held.
    public func press(_ key: String, modifiers: [String] = []) async throws {
        try await send(.press(key, modifiers: modifiers))
    }

    /// Scrolls by `dx` and `dy` CSS pixels: the element `selector` matches, or the frame's viewport.
    public func scroll(
        _ selector: String? = nil, dx: Double = 0, dy: Double = 0, frame: FrameSelector = .main
    ) async throws {
        try await send(.scroll(selector: selector, dx: dx, dy: dy, frame: frame))
    }

    /// Returns when `condition` holds. Throws `broker.timeout` after `timeout`.
    public func waitFor(
        _ condition: WaitCondition, timeout: Duration = .seconds(30)
    ) async throws {
        try await send(.waitFor(condition, timeout: timeout))
    }

    private func send(_ input: Input) async throws {
        try await scope.run(input.command, input.arguments)
    }
}

extension Scripting {
    /// One input command: its name and the arguments sent. It is separate from the wrappers so a test can
    /// check what reaches the runtime, which the stub answers with `broker.notImplemented`.
    struct Input: Equatable {
        let command: String
        let arguments: BDKJSValue

        static func click(
            _ target: InputTarget, frame: FrameSelector, button: BDKMouseButton, clickCount: Int
        ) -> Input {
            var arguments = target.arguments(frame: frame)
            arguments["button"] = .string(button.rawValue)
            arguments["clickCount"] = .number(Double(clickCount))
            return Input(command: "contents.click", arguments: .object(arguments))
        }

        static func hover(_ target: InputTarget, frame: FrameSelector) -> Input {
            Input(command: "contents.hover", arguments: .object(target.arguments(frame: frame)))
        }

        static func type(_ text: String, selector: String?, frame: FrameSelector) -> Input {
            var arguments: [String: BDKJSValue] = ["text": .string(text)]
            if let selector { arguments["selector"] = .string(selector) }
            if let id = frame.argument { arguments["frame"] = .string(id) }
            return Input(command: "contents.type", arguments: .object(arguments))
        }

        static func press(_ key: String, modifiers: [String]) -> Input {
            var arguments: [String: BDKJSValue] = ["key": .string(key)]
            if !modifiers.isEmpty { arguments["modifiers"] = .array(modifiers.map { .string($0) }) }
            return Input(command: "contents.press", arguments: .object(arguments))
        }

        static func scroll(
            selector: String?, dx: Double, dy: Double, frame: FrameSelector
        ) -> Input {
            var arguments: [String: BDKJSValue] = ["dx": .number(dx), "dy": .number(dy)]
            if let selector { arguments["selector"] = .string(selector) }
            if let id = frame.argument { arguments["frame"] = .string(id) }
            return Input(command: "contents.scroll", arguments: .object(arguments))
        }

        static func waitFor(_ condition: WaitCondition, timeout: Duration) -> Input {
            var arguments: [String: BDKJSValue] = [
                "timeoutMs": .number(Double(timeout.milliseconds))
            ]
            switch condition {
            case .load(let load): arguments["load"] = .string(load.rawValue)
            case .url(let pattern): arguments["url"] = .string(pattern)
            }
            return Input(command: "contents.waitFor", arguments: .object(arguments))
        }
    }
}

extension Scripting.InputTarget {
    fileprivate func arguments(frame: Scripting.FrameSelector) -> [String: BDKJSValue] {
        var arguments: [String: BDKJSValue] = [:]
        switch self {
        case .selector(let selector):
            arguments["selector"] = .string(selector)
        case .point(let x, let y):
            arguments["x"] = .number(x)
            arguments["y"] = .number(y)
        }
        if let id = frame.argument { arguments["frame"] = .string(id) }
        return arguments
    }
}

extension Duration {
    /// Whole milliseconds, rounded up so a short duration is never a zero deadline.
    fileprivate var milliseconds: Int64 {
        let parts = components
        let fraction = (parts.attoseconds + 999_999_999_999_999) / 1_000_000_000_000_000
        return parts.seconds * 1000 + fraction
    }
}
