import AppKit

// FedCM (ui-requests.md 5.22, T-UIR-14): a site's navigator.credentials.get({identity}). Each
// step of Chromium's sign-in is a request of its own, withdrawn when Chromium moves on:
// `.chooseAccount` (the accounts the identity providers offer), `.loading` and `.verifying`
// (nothing to answer), `.failure` (the person is signed in to no account of the provider),
// `.error` (the provider refused), and `.registerIdentityProvider` (a site asks to become an
// identity provider).
//
// Completions: "grant" with `{"account": id}` and the confirming event signs in (it releases an
// identity to the site); "grant" `{}` with the event allows a registration; "accept"
// `{"action": "signInToIdP", "idp": id}` opens the provider's sign-in page as a new-window
// request of kind `.identityProvider`; "accept" `{"action": "moreDetails"}` opens the error's
// page; "deny" dismisses.

public final class IdentityRequest: BDKUIRequest {
    public enum Step: String, Sendable {
        case chooseAccount
        case loading
        case verifying
        case failure
        case error
        case registerIdentityProvider = "registerIdP"
    }

    public enum Context: String, Sendable {
        case signIn = "signin"
        case signUp = "signup"
        case use
        case `continue`
    }

    public struct IdentityProvider: Sendable, Hashable, Identifiable {
        /// The provider's config URL.
        public let id: String
        public let name: String
        public let icon: NSImage?
    }

    public struct Account: Sendable, Hashable, Identifiable {
        public let id: String
        /// The `IdentityProvider.id` it belongs to.
        public let identityProvider: String
        public let name: String
        public let email: String
        public let givenName: String
        public let picture: URL?
        /// The person signed in to this site with it before.
        public let isReturning: Bool
    }

    public let step: Step
    /// The site that asks, and the frame in it when that is another origin.
    public let origin: URL
    public let iframeOrigin: URL?
    public let context: Context
    public let isActiveMode: Bool
    public let identityProviders: [IdentityProvider]
    public let accounts: [Account]
    /// For `.error`: the provider's error code, if it gave one.
    public let errorCode: String?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        step = fields.choice("step", default: .loading)
        let party = fields.object("relyingParty")
        origin = party?.url("origin") ?? fields.url("origin") ?? URL(string: "about:blank")!
        iframeOrigin = party?.url("iframeOrigin")
        context = fields.choice("context", default: .signIn)
        isActiveMode = fields.string("mode") == "active"
        identityProviders = fields.objects("identityProviders").compactMap { idp in
            guard let id = idp.string("id") else { return nil }
            return IdentityProvider(
                id: id, name: idp.string("name") ?? "",
                icon: BDKImageDecoding.image(fromDataURL: idp.string("icon")))
        }
        accounts = fields.objects("accounts").compactMap { account in
            guard let id = account.string("id") else { return nil }
            return Account(
                id: id, identityProvider: account.string("idp") ?? "",
                name: account.string("name") ?? "", email: account.string("email") ?? "",
                givenName: account.string("givenName") ?? "", picture: account.url("picture"),
                isReturning: account.bool("isReturning"))
        }
        errorCode = fields.object("error")?.string("code")
        super.init(delivery, runtime: runtime)
    }

    /// Signs in with `account` at `.chooseAccount`. `event` is the click or key press that chose
    /// it: without one it throws `ui.gestureRequired`. Throws `ui.accountNotOffered` for an
    /// account not on offer.
    public func signIn(as account: Account, confirmedBy event: NSEvent) throws {
        guard accounts.contains(where: { $0.id == account.id }) else {
            throw BDKError(
                domain: .ui, code: "accountNotOffered",
                message: "the request does not offer this account")
        }
        try submit("grant", ["account": .string(account.id)], gesture: event)
    }

    /// Allows a site to register as an identity provider, at `.registerIdentityProvider`.
    public func allowRegistration(confirmedBy event: NSEvent) throws {
        try submit("grant", [:], gesture: event)
    }

    /// Opens `provider`'s sign-in page, as a new-window request of kind `.identityProvider`.
    public func signInToIdentityProvider(_ provider: IdentityProvider) throws {
        try submit("accept", ["action": .string("signInToIdP"), "idp": .string(provider.id)])
    }

    /// Opens the page that explains an `.error`.
    public func showMoreDetails() throws {
        try submit("accept", ["action": .string("moreDetails")])
    }

    /// Dismisses: the site's request rejects.
    public func cancel() {
        try? submit("deny")
    }
}
