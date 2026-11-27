import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("WebAuthn requests")
    struct UIWebAuthnRequestTests {
        /// Steps as the engine raises them (ui-requests.md 5.18).
        static let mechanisms = """
            {"ceremony":"3","relyingParty":"example.com","origin":"https://example.com",\
            "operation":"create","step":"chooseMechanism","detail":"mechanismSelection",\
            "text":{"title":"Choose where to save your passkey","message":"for example.com"},\
            "cancelLabel":"Cancel","busy":false,\
            "mechanisms":[{"id":"0","kind":"securityKey","name":"USB security key"},\
            {"id":"1","kind":"phone","name":"iPhone, iPad or Android device",\
            "description":"Scan a QR code"}]}
            """
        static let accounts = """
            {"ceremony":"3","relyingParty":"example.com","operation":"get",\
            "step":"chooseAccount","detail":"selectAccount","text":{"title":"Choose a passkey"},\
            "cancelLabel":"Cancel",\
            "accounts":[{"id":"0","name":"ada@example.com","displayName":"Ada"},\
            {"id":"1","name":"","displayName":"Grace"}]}
            """
        static let pin = """
            {"ceremony":"3","relyingParty":"example.com","operation":"get","step":"enterPIN",\
            "detail":"clientPinEntry","text":{"title":"Enter your PIN",\
            "error":"Incorrect PIN. 2 attempts left."},"acceptLabel":"Next",\
            "otherMechanismsLabel":"Use a different device","cancelLabel":"Cancel",\
            "pin":{"mode":"enter","minLength":4,"attemptsRemaining":2}}
            """
        static let failure = """
            {"ceremony":"3","relyingParty":"example.com","operation":"get","step":"error",\
            "detail":"timedOut","error":"timedOut","text":{"title":"Something went wrong"},\
            "acceptLabel":"Try again","cancelLabel":"Close"}
            """

        @Test func aStepCarriesWhatChromiumWouldShow() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.mechanisms)
                #expect(request.surface == .webAuthn)
                #expect(request.ceremonyID == "3" && request.relyingParty == "example.com")
                #expect(request.origin == URL(string: "https://example.com"))
                #expect(request.operation == .create && request.step == .chooseMechanism)
                #expect(request.detail == "mechanismSelection" && request.error == nil)
                #expect(request.text.title == "Choose where to save your passkey")
                #expect(request.mechanisms.map(\.kind) == [.securityKey, .phone])
                #expect(request.mechanisms[1].description == "Scan a QR code")
                #expect(request.acceptLabel == nil && request.cancelLabel == "Cancel")
                #expect(request.initiator == .page(origin: try #require(request.origin)))
                try request.choose(request.mechanisms[1])
                try fixture.expectOutcome(.completed, "accept", "{\"mechanism\":\"1\"}")
            }
        }

        @Test func aMechanismMustBeOneTheStepOffered() async throws {
            try await withUIFixture { fixture in
                let other = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN,
                    Self.mechanisms.replacingOccurrences(of: "\"id\":\"1\"", with: "\"id\":\"9\""))
                let stranger = try #require(other.mechanisms.last)
                other.cancel()
                let request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.mechanisms)
                await expectError(.ui, "mechanismNotOffered") { try request.choose(stranger) }
                #expect(!request.isCompleted)
                await expectError(.ui, "nothingToAccept") { try request.accept() }
                await expectError(.ui, "pinNotRequested") { try request.providePIN("1234") }
                request.cancel()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func anAccountIsChosenWithTheConfirmingEvent() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.accounts)
                #expect(request.step == .chooseAccount && request.operation == .get)
                #expect(request.accounts.map(\.displayName) == ["Ada", "Grace"])
                let grace = request.accounts[1]
                #expect(request.presentation.choices.map(\.title) == ["ada@example.com", "Grace"])
                await expectError(.ui, "gestureRequired") {
                    try request.perform(.account(grace))
                }
                try request.chooseAccount(grace, confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{\"account\":\"1\"}")
            }
        }

        @Test func thePINIsCheckedForLengthAndGivenOnce() async throws {
            try await withUIFixture { fixture in
                var request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.pin)
                let entry = try #require(request.pin)
                #expect(entry.mode == .enter && entry.minimumLength == 4)
                #expect(entry.attemptsRemaining == 2)
                #expect(request.errorMessage == "Incorrect PIN. 2 attempts left.")
                await expectError(.ui, "pinTooShort") { try request.providePIN("12") }
                #expect(!request.isCompleted)
                try request.providePIN("1234")
                try fixture.expectOutcome(.completed, "accept", "{\"pin\":\"1234\"}")

                request = try fixture.deliver(WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.pin)
                try request.useOtherMechanism()
                try fixture.expectOutcome(
                    .completed, "accept", "{\"action\":\"otherMechanisms\"}")
            }
        }

        @Test func thePresentationListsTheStepsButtonsInOrder() async throws {
            try await withUIFixture { fixture in
                var request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.pin)
                let presentation = request.presentation
                #expect(presentation.title == "Enter your PIN")
                #expect(presentation.errorMessage == "Incorrect PIN. 2 attempts left.")
                #expect(presentation.pin?.mode == .enter && presentation.choices.isEmpty)
                #expect(
                    presentation.actions == [
                        .accept("Next"), .otherMechanisms("Use a different device"),
                        .cancel("Cancel"),
                    ])
                try request.perform(.accept("Next"), pin: "4321")
                try fixture.expectOutcome(.completed, "accept", "{\"pin\":\"4321\"}")

                request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.failure)
                #expect(request.step == .error && request.error == .timedOut)
                #expect(request.presentation.actions == [.accept("Try again"), .cancel("Close")])
                try request.perform(.accept("Try again"))
                try fixture.expectOutcome(.completed, "accept", "{}")

                request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.mechanisms)
                try request.perform(.mechanism(request.mechanisms[0]))
                try fixture.expectOutcome(.completed, "accept", "{\"mechanism\":\"0\"}")
            }
        }

        @Test func aNewStepWithdrawsTheLastOne() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.mechanisms)
                var withdrawn = false
                request.cancellation.onFire { withdrawn = true }
                try fixture.runtime.stub.withdrawUIRequests()
                fixture.runtime.stub.pump()
                #expect(withdrawn && request.isWithdrawn)
                request.cancel()  // a withdrawn step ignores its answer
                try fixture.expectOutcome(.withdrawn)
                let next = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.accounts)
                #expect(next.ceremonyID == request.ceremonyID)
            }
        }

        @Test func cancelAllRejectsEveryCeremony() async throws {
            try await withUIFixture { fixture in
                try fixture.runtime.ui.webAuthn.setHandler(WebAuthnRequest.cancelAll)
                try fixture.raise(BDK_SURFACE_WEBAUTHN, Self.mechanisms)
                try fixture.expectOutcome(.rejected)
            }
        }

        @Test func aQRCodeIsDrawnForAPhone() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN,
                    "{\"step\":\"connectPhone\",\"qrCode\":\"FIDO:/0123456789\"}")
                #expect(request.step == .connectPhone && request.qrCode == "FIDO:/0123456789")
                let image = try #require(request.qrCodeImage(scale: 4))
                #expect(image.size.width >= 21 * 4)
                request.cancel()
                let none = try fixture.deliver(
                    WebAuthnRequest.self, BDK_SURFACE_WEBAUTHN, Self.mechanisms)
                #expect(none.qrCodeImage() == nil)
                #expect(none.presentation.qrCode == nil)
            }
        }

        @Test func onlyATrueEntitlementCountsAsGranted() {
            #expect(BDKWebAuthnSupport.isEnabled(entitlementValue: true))
            #expect(!BDKWebAuthnSupport.isEnabled(entitlementValue: false))
            #expect(!BDKWebAuthnSupport.isEnabled(entitlementValue: nil))
            #expect(!BDKWebAuthnSupport.isEnabled(entitlementValue: "yes"))
        }

        @Test func supportFollowsChromiumsConditions() {
            let sonoma = OperatingSystemVersion(majorVersion: 14, minorVersion: 0, patchVersion: 0)
            let bare = BDKWebAuthnSupport(
                infoPlist: [:], hasEntitlement: { _ in false }, isHardenedRuntime: true,
                operatingSystem: sonoma, phonesEnabled: false)
            #expect(bare.securityKeys.isAvailable)
            #expect(
                bare.phones.missing == [
                    .configuration("[services.webauthn] phones = true"),
                    .infoPlistKey("NSBluetoothAlwaysUsageDescription"),
                    .entitlement("com.apple.security.device.bluetooth"),
                ])
            #expect(
                bare.systemPasskeys.missing == [
                    .entitlement("com.apple.developer.web-browser.public-key-credential")
                ])
            #expect(
                bare.touchID.missing == [
                    .entitlement("keychain-access-groups <team>.<bundle id>.webauthn")
                ])
            #expect(bare.all.map(\.kind) == [.securityKey, .phone, .iCloudKeychain, .platform])

            let configured = BDKWebAuthnSupport(
                infoPlist: ["NSBluetoothAlwaysUsageDescription": "Sign in with your phone."],
                hasEntitlement: { _ in true }, isHardenedRuntime: true, operatingSystem: sonoma,
                phonesEnabled: true)
            #expect(configured.phones.isAvailable && configured.systemPasskeys.isAvailable)
            // With the switch off, everything else in place still offers no phone.
            let switchedOff = BDKWebAuthnSupport(
                infoPlist: ["NSBluetoothAlwaysUsageDescription": "Sign in with your phone."],
                hasEntitlement: { _ in true }, isHardenedRuntime: true, operatingSystem: sonoma,
                phonesEnabled: false)
            #expect(
                switchedOff.phones.missing == [.configuration("[services.webauthn] phones = true")])

            // Unsigned development builds need no Bluetooth entitlement; old systems no passkeys.
            let old = BDKWebAuthnSupport(
                infoPlist: ["NSBluetoothAlwaysUsageDescription": "x"],
                hasEntitlement: { $0 != "com.apple.developer.web-browser.public-key-credential" },
                isHardenedRuntime: false,
                operatingSystem: OperatingSystemVersion(
                    majorVersion: 13, minorVersion: 4, patchVersion: 0),
                phonesEnabled: true)
            #expect(old.phones.isAvailable)
            #expect(old.systemPasskeys.missing.first == .macOS("13.5"))
            #expect("\(BDKWebAuthnSupport.Requirement.macOS("13.5"))" == "macOS 13.5 or later")
            #expect(BDKWebAuthnSupport.current.securityKeys.isAvailable)
        }

        /// Touch ID: signed by a team, and entitled to that team's group for the bundle.
        @Test func touchIDNeedsTheApplicationsKeychainGroup() {
            let sonoma = OperatingSystemVersion(majorVersion: 14, minorVersion: 0, patchVersion: 0)
            let group = "TEAM.app.example.webauthn"
            let entitled = BDKWebAuthnSupport(
                infoPlist: [:], hasEntitlement: { _ in false }, isHardenedRuntime: true,
                operatingSystem: sonoma, phonesEnabled: false, touchIDGroup: group,
                keychainGroups: [group])
            #expect(entitled.touchID.isAvailable)
            let other = BDKWebAuthnSupport(
                infoPlist: [:], hasEntitlement: { _ in false }, isHardenedRuntime: true,
                operatingSystem: sonoma, phonesEnabled: false, touchIDGroup: group,
                keychainGroups: ["TEAM.other"])
            #expect(other.touchID.missing == [.entitlement("keychain-access-groups \(group)")])
        }

        @Test func phonesFollowTheConfigurationOfTheRunningRuntime() async throws {
            let setting = BDKWebAuthnSupport.Requirement.configuration(
                "[services.webauthn] phones = true")
            #expect(BDKWebAuthnSupport.current.phones.missing.contains(setting))
            try await withStubRuntime { _ in
                #expect(BDKWebAuthnSupport.current.phones.missing.contains(setting))
            }
            let phonesOn = BDKConfiguration(
                resolvedJSON: "{\"services\":{\"webauthn\":{\"phones\":true}}}")
            try await withStubRuntime(phonesOn) { _ in
                #expect(!BDKWebAuthnSupport.current.phones.missing.contains(setting))
            }
        }
    }
}
