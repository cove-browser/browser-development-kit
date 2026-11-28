import AppKit

// Payments (ui-requests.md 5.22, T-UIR-14): a page's PaymentRequest.show(). The request names the
// total, the line items and the payment apps that can pay (service-worker payment handlers); BDK
// has no card form and no wallet of its own, so a request with no app is refused before it is
// raised.
//
// Completions: "grant" with `{"app": id}` and the confirming event pays with that app, which may
// then open its own page as a new-window request of kind `.paymentHandler`; "deny", `.reject`
// and a dropped request cancel it (the page's show() rejects with AbortError).

public final class PaymentRequest: BDKUIRequest {
    public struct Amount: Sendable, Hashable {
        /// An ISO 4217 code, such as "USD".
        public let currency: String
        /// The amount as the page wrote it, such as "1.00": never parsed into a float.
        public let value: String
    }

    public struct Item: Sendable, Hashable {
        public let label: String
        public let amount: Amount
    }

    /// A payment app that can pay: a payment handler installed in the profile, or one the
    /// payment method's manifest offers to install just in time.
    public struct App: Sendable, Hashable, Identifiable {
        public let id: String
        public let name: String
        /// The origin of the payment handler, which the person should see next to its name.
        public let origin: URL?
        public let icon: NSImage?
    }

    /// The origin of the frame that asked, and of the page it is in.
    public let origin: URL
    public let topLevelOrigin: URL
    public let total: Item
    public let displayItems: [Item]
    public let apps: [App]

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        origin = fields.requiredURL("origin")
        topLevelOrigin = fields.url("topLevelOrigin") ?? fields.requiredURL("origin")
        total = Self.item(fields.object("total"))
        displayItems = fields.objects("displayItems").map { Self.item($0) }
        apps = fields.objects("apps").compactMap { app in
            guard let id = app.string("id") else { return nil }
            return App(
                id: id, name: app.string("name") ?? "", origin: app.url("origin"),
                icon: BDKImageDecoding.image(fromDataURL: app.string("icon")))
        }
        super.init(delivery, runtime: runtime)
    }

    /// Pays with `app`. `event` is the click or key press that chose it: without one it throws
    /// `ui.gestureRequired`. Throws `ui.appNotOffered` for an app not on offer.
    public func pay(with app: App, confirmedBy event: NSEvent) throws {
        guard apps.contains(where: { $0.id == app.id }) else {
            throw BDKError(
                domain: .ui, code: "appNotOffered", message: "the request does not offer this app")
        }
        try submit("grant", ["app": .string(app.id)], gesture: event)
    }

    /// Cancels: the page's show() rejects with AbortError.
    public func cancel() {
        try? submit("deny")
    }

    private static func item(_ fields: BDKUIPayload?) -> Item {
        Item(
            label: fields?.string("label") ?? "",
            amount: Amount(
                currency: fields?.string("currency") ?? "", value: fields?.string("value") ?? ""))
    }
}
