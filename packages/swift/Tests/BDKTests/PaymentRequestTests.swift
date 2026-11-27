import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("Payment requests")
    struct PaymentRequestTests {
        static let payload = """
            {"kind":"paymentRequest","origin":"https://shop.test","topLevelOrigin":"https://shop.test",\
            "total":{"label":"Total","currency":"USD","value":"1.00"},\
            "displayItems":[{"label":"Tea","currency":"USD","value":"1.00"}],\
            "apps":[{"id":"app0","name":"Test Pay","origin":"https://pay.test"}],\
            "requests":{"shipping":false,"payerName":false,"payerEmail":false,"payerPhone":false}}
            """

        @Test func aRequestNamesTheTotalAndTheAppsAndPaysWithAGesture() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    PaymentRequest.self, BDK_SURFACE_PAYMENTS, Self.payload)
                #expect(request.origin == URL(string: "https://shop.test"))
                #expect(request.total.label == "Total")
                #expect(request.total.amount == .init(currency: "USD", value: "1.00"))
                #expect(request.displayItems.map(\.label) == ["Tea"])
                #expect(request.apps.map(\.name) == ["Test Pay"])
                #expect(request.apps.first?.origin == URL(string: "https://pay.test"))
                let other = PaymentRequest.App(id: "app9", name: "Other", origin: nil, icon: nil)
                #expect(throws: BDKError.self) {
                    try request.pay(with: other, confirmedBy: try confirmingEvent())
                }
                try request.pay(with: request.apps[0], confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{\"app\":\"app0\"}")
            }
        }

        @Test func cancelDenies() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    PaymentRequest.self, BDK_SURFACE_PAYMENTS, Self.payload)
                request.cancel()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }
    }
}
