import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("UI prompt requests")
    struct UIPromptRequestTests {
        @Test func permissions() async throws {
            try await withUIFixture { fixture in
                let payload = """
                    {"kinds":["camera","microphone"],"origin":"https://a.test",\
                    "embeddingOrigin":"https://top.test","offersOneTime":true,\
                    "text":{"title":"Allow?","message":"a.test wants the camera"}}
                    """
                var request = try fixture.deliver(
                    PermissionRequest.self, BDK_SURFACE_PERMISSIONS, payload)
                #expect(request.kinds == [.camera, .microphone])
                #expect(request.embeddingOrigin == URL(string: "https://top.test"))
                #expect(request.offersOneTime)
                #expect(
                    request.text == BDKUIText(title: "Allow?", message: "a.test wants the camera"))

                await expectError(.ui, "kindsNotRequested") {
                    try request.grant([.geolocation], confirmedBy: try confirmingEvent())
                }
                #expect(!request.isCompleted)
                try request.grant(
                    [.camera], duration: .thisVisit, confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(
                    .completed, "grant", "{\"duration\":\"thisVisit\",\"kinds\":[\"camera\"]}")

                request = try fixture.deliver(
                    PermissionRequest.self, BDK_SURFACE_PERMISSIONS, "{\"permission\":\"midi\"}")
                #expect(request.kinds == [.midi])
                request.deny(persist: false)
                try fixture.expectOutcome(.completed, "deny", "{\"persist\":false}")

                request = try fixture.deliver(PermissionRequest.self, BDK_SURFACE_PERMISSIONS)
                request.dismiss()
                try fixture.expectOutcome(
                    .completed, "deny", "{\"dismissed\":true,\"persist\":false}")
            }
        }

        @Test func aGrantNeedsTheConfirmingEvent() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(BDKRawUIRequest.self, BDK_SURFACE_SCREEN_CAPTURE)
                await expectError(.ui, "gestureRequired") { try request.complete("grant") }
                #expect(!request.isCompleted)
                try fixture.expectOutcome(.none)
                try request.complete("grant", confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{}")
            }
        }

        @Test func javascriptDialogs() async throws {
            try await withUIFixture { fixture in
                let payload = """
                    {"kind":"prompt","origin":"https://a.test","message":"Name?",\
                    "defaultPromptText":"Ada","offersSuppression":true}
                    """
                var request = try fixture.deliver(
                    JavaScriptDialogRequest.self, BDK_SURFACE_JAVASCRIPT_DIALOGS, payload)
                #expect(request.kind == .prompt && request.message == "Name?")
                #expect(request.defaultPromptText == "Ada" && request.offersSuppression)
                request.accept(text: "Grace")
                try fixture.expectOutcome(
                    .completed, "accept", "{\"suppressFurther\":false,\"text\":\"Grace\"}")

                request = try fixture.deliver(
                    JavaScriptDialogRequest.self, BDK_SURFACE_JAVASCRIPT_DIALOGS,
                    "{\"kind\":\"beforeUnload\",\"isReload\":true}")
                #expect(request.kind == .beforeUnload && request.isReload)
                request.cancel(suppressFurther: true)
                try fixture.expectOutcome(.completed, "deny", "{\"suppressFurther\":true}")
            }
        }

        @Test func httpAuth() async throws {
            try await withUIFixture { fixture in
                let payload = """
                    {"challenger":"https://auth.test","realm":"staff","isProxy":true,\
                    "scheme":"digest","savedCredentialsAvailable":true}
                    """
                var request = try fixture.deliver(
                    HTTPAuthRequest.self, BDK_SURFACE_HTTP_AUTH, payload)
                #expect(request.challenger == URL(string: "https://auth.test"))
                #expect(request.realm == "staff" && request.isProxy && request.scheme == "digest")
                #expect(request.savedCredentialsAvailable)
                request.provide(username: "ada", password: "pw")
                try fixture.expectOutcome(
                    .completed, "accept", "{\"password\":\"pw\",\"username\":\"ada\"}")

                request = try fixture.deliver(HTTPAuthRequest.self, BDK_SURFACE_HTTP_AUTH)
                request.cancel()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func clientCertificates() async throws {
            try await withUIFixture { fixture in
                let payload = """
                    {"host":"mtls.test","certificates":[{"id":"c1","subject":"Ada","issuer":"CA"}]}
                    """
                var request = try fixture.deliver(
                    ClientCertificateRequest.self, BDK_SURFACE_CLIENT_CERTIFICATES, payload)
                let certificate = try #require(request.certificates.first)
                #expect(request.host == "mtls.test" && certificate.subject == "Ada")
                try request.select(certificate)
                try fixture.expectOutcome(.completed, "accept", "{\"certificate\":\"c1\"}")

                request = try fixture.deliver(
                    ClientCertificateRequest.self, BDK_SURFACE_CLIENT_CERTIFICATES, payload)
                let stranger = BDKCertificateSummary(BDKUIPayload(["id": .string("other")]))
                await expectError(.ui, "certificateNotOffered") { try request.select(stranger) }
                request.cancel()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func filePickers() async throws {
            try await withUIFixture { fixture in
                let payload = """
                    {"mode":"open","acceptedTypes":["public.png","text/plain","pdf"],\
                    "suggestedName":"a.png","isFileSystemAccessAPI":true}
                    """
                var request = try fixture.deliver(
                    FilePickerRequest.self, BDK_SURFACE_FILE_PICKERS, payload)
                #expect(request.mode == .open && request.suggestedName == "a.png")
                #expect(request.isFileSystemAccessAPI && request.acceptedTypes.count == 3)
                let first = URL(fileURLWithPath: "/tmp/a.png")
                await expectError(.ui, "tooManyFiles") {
                    try request.choose([first, URL(fileURLWithPath: "/tmp/b.png")])
                }
                try request.choose([first])
                try fixture.expectOutcome(.completed, "accept", "{\"paths\":[\"/tmp/a.png\"]}")

                request = try fixture.deliver(
                    FilePickerRequest.self, BDK_SURFACE_FILE_PICKERS, "{\"mode\":\"openMultiple\"}")
                try request.choose([first, URL(fileURLWithPath: "/tmp/b.png")])
                try fixture.expectOutcome(
                    .completed, "accept", "{\"paths\":[\"/tmp/a.png\",\"/tmp/b.png\"]}")

                request = try fixture.deliver(FilePickerRequest.self, BDK_SURFACE_FILE_PICKERS)
                request.cancel()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }
    }
}
