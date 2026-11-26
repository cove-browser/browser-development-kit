import AppKit

// Screen capture (ui-requests.md 5.16 and 8.1, T-UIR-13): getDisplayMedia, and
// chrome.desktopCapture.chooseDesktopMedia for an extension's tab. The request stays open while
// the picker is: each change of the sources arrives as an update, and a source keeps its id while
// the request is open.
//
// Completions: "share" with `{"source": id, "audio": Bool}` and the confirming event shares the
// source; "deny", `.reject` and a dropped request refuse it (NotAllowedError). While the page
// shares, `BDKWebContents.captureActivity` contains `.display`: an application that handles this surface
// shows its own indicator.

public final class ScreenCaptureRequest: BDKUIRequest {
    public struct Source: Sendable, Hashable, Identifiable {
        public enum Kind: String, Sendable {
            case screen
            case window
            case page
            case other
        }

        public let id: String
        public let kind: Kind
        public let name: String
        /// A small image of the source, when Chromium has one.
        public let thumbnail: NSImage?
    }

    /// The origin of the page that asked.
    public let origin: URL
    /// The extension that asked for its tab (`chrome.desktopCapture`), or nil for the page itself.
    public let extensionName: String?
    /// Whether the page asked for audio too.
    public let requestsAudio: Bool
    /// The sources on offer now.
    public private(set) var sources: [Source]
    private let changes = EventBroadcaster<[Source]>()

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        origin = fields.requiredURL("origin")
        extensionName = fields.string("extensionName")
        requestsAudio = fields.bool("requestsAudio")
        sources = Self.sources(fields)
        super.init(delivery, runtime: runtime)
        observeUpdates { [weak self] update in self?.receive(BDKUIPayload(update)) }
        cancellation.onFire { [changes] in changes.finish() }
    }

    /// Each new list of sources, as screens, windows and pages come and go while the picker is
    /// open. Ends when the request does.
    public var sourceChanges: BDKEventSequence<[Source]> {
        BDKEventSequence(changes)
    }

    /// Shares `source`, with its audio when the page asked for audio and `audio` is true. `event`
    /// is the click or key press that chose it: without one it throws `ui.gestureRequired`.
    /// Throws `ui.sourceNotOffered` for a source not on offer.
    public func share(_ source: Source, audio: Bool = false, confirmedBy event: NSEvent) throws {
        guard sources.contains(where: { $0.id == source.id }) else {
            throw BDKError(
                domain: .ui, code: "sourceNotOffered",
                message: "the picker does not offer this source")
        }
        try submit(
            "share", ["source": .string(source.id), "audio": .bool(audio && requestsAudio)],
            gesture: event)
    }

    public func cancel() {
        try? submit("deny")
    }

    private func receive(_ update: BDKUIPayload) {
        sources = Self.sources(update)
        changes.send(sources)
    }

    private static func sources(_ fields: BDKUIPayload) -> [Source] {
        fields.objects("sources").compactMap { source in
            guard let id = source.string("id") else { return nil }
            return Source(
                id: id, kind: source.choice("kind", default: .other),
                name: source.string("name") ?? "",
                thumbnail: BDKImageDecoding.image(fromDataURL: source.string("thumbnail")))
        }
    }
}
