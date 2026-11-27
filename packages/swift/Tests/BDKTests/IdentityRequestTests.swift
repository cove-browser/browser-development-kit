import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("Identity requests")
    struct IdentityRequestTests {
        static let chooseAccount = """
            {"step":"chooseAccount","relyingParty":{"origin":"https://shop.test"},\
            "context":"signin","mode":"active",\
            "identityProviders":[{"id":"https://idp.test/config.json","name":"idp.test"}],\
            "accounts":[{"id":"1","idp":"https://idp.test/config.json","name":"Ada Lovelace",\
            "email":"ada@idp.test","givenName":"Ada","isReturning":true}]}
            """

        @Test func anAccountSignsInWithAGesture() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    IdentityRequest.self, BDK_SURFACE_IDENTITY, Self.chooseAccount)
                #expect(request.step == .chooseAccount)
                #expect(request.origin == URL(string: "https://shop.test"))
                #expect(request.context == .signIn && request.isActiveMode)
                #expect(request.identityProviders.map(\.name) == ["idp.test"])
                #expect(request.accounts.first?.email == "ada@idp.test")
                #expect(request.accounts.first?.isReturning == true)
                let stranger = IdentityRequest.Account(
                    id: "9", identityProvider: "", name: "", email: "", givenName: "",
                    picture: nil, isReturning: false)
                #expect(throws: BDKError.self) {
                    try request.signIn(as: stranger, confirmedBy: try confirmingEvent())
                }
                try request.signIn(as: request.accounts[0], confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{\"account\":\"1\"}")
            }
        }

        @Test func theProvidersSignInPageAndCancel() async throws {
            try await withUIFixture { fixture in
                let failure = try fixture.deliver(
                    IdentityRequest.self, BDK_SURFACE_IDENTITY,
                    "{\"step\":\"failure\",\"relyingParty\":{\"origin\":\"https://shop.test\"},"
                        + "\"identityProviders\":[{\"id\":\"cfg\",\"name\":\"idp.test\"}]}")
                #expect(failure.step == .failure)
                try failure.signInToIdentityProvider(failure.identityProviders[0])
                try fixture.expectOutcome(
                    .completed, "accept", "{\"action\":\"signInToIdP\",\"idp\":\"cfg\"}")

                let register = try fixture.deliver(
                    IdentityRequest.self, BDK_SURFACE_IDENTITY,
                    "{\"step\":\"registerIdP\",\"relyingParty\":{\"origin\":\"https://idp.test\"}}")
                #expect(register.step == .registerIdentityProvider)
                register.cancel()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }
    }
}
