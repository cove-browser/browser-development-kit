import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("UI data requests")
    struct UIDataRequestTests {
        @Test func downloads() async throws {
            try await withUIFixture { fixture in
                let payload = """
                    {"download":{"url":"https://d.test/f.zip","suggestedName":"f.zip",\
                    "mimeType":"application/zip","size":42},"needsLocation":true}
                    """
                var request = try fixture.deliver(
                    DownloadRequest.self, BDK_SURFACE_DOWNLOADS, payload)
                #expect(request.download.suggestedName == "f.zip" && request.download.size == 42)
                #expect(request.needsLocation)
                request.proceed(to: URL(fileURLWithPath: "/tmp/f.zip"))
                try fixture.expectOutcome(
                    .completed, "accept", "{\"destination\":\"/tmp/f.zip\"}")

                request = try fixture.deliver(DownloadRequest.self, BDK_SURFACE_DOWNLOADS, payload)
                request.cancel()
                try fixture.expectOutcome(.completed, "deny", "{}")

                let dangerous = """
                    {"download":{"url":"https://d.test/x.exe"},"danger":"dangerousFileType",\
                    "text":{"title":"Dangerous","message":"This file can harm you"}}
                    """
                var risky = try fixture.deliver(
                    DangerousDownloadRequest.self, BDK_SURFACE_DOWNLOADS, dangerous)
                #expect(risky.danger == .dangerousFileType && risky.text.title == "Dangerous")
                try risky.keep(confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{}")

                risky = try fixture.deliver(
                    DangerousDownloadRequest.self, BDK_SURFACE_DOWNLOADS, dangerous)
                risky.discard()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func passwords() async throws {
            try await withUIFixture { fixture in
                let payload =
                    "{\"origin\":\"https://p.test\",\"username\":\"ada\",\"isUpdate\":true}"
                var save = try fixture.deliver(
                    PasswordSaveRequest.self, BDK_SURFACE_PASSWORDS, payload)
                #expect(save.username == "ada" && save.isUpdate)
                try save.save(confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{}")

                save = try fixture.deliver(PasswordSaveRequest.self, BDK_SURFACE_PASSWORDS, payload)
                save.never()
                try fixture.expectOutcome(.completed, "deny", "{\"never\":true}")

                save = try fixture.deliver(PasswordSaveRequest.self, BDK_SURFACE_PASSWORDS, payload)
                save.dismiss()
                try fixture.expectOutcome(.completed, "deny", "{\"dismissed\":true}")

                let leaked = try fixture.deliver(
                    LeakedPasswordRequest.self, BDK_SURFACE_PASSWORDS,
                    "{\"kind\":\"leaked\",\"origin\":\"https://p.test\",\"isSaved\":true,"
                        + "\"provider\":{\"name\":\"Checker\",\"isRemote\":true}}")
                #expect(
                    leaked.isSaved && leaked.provider?.name == "Checker"
                        && leaked.provider?.isRemote == true)
                leaked.dismiss()
                try fixture.expectOutcome(.completed, "deny", "{\"dismissed\":true}")
            }
        }

        @Test func autofill() async throws {
            try await withUIFixture { fixture in
                var save = try fixture.deliver(
                    AutofillSaveRequest.self, BDK_SURFACE_AUTOFILL,
                    "{\"kind\":\"card\",\"summary\":\"Visa 4242\"}")
                #expect(save.kind == .card && save.summary == "Visa 4242")
                try save.save(confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{}")
                save = try fixture.deliver(AutofillSaveRequest.self, BDK_SURFACE_AUTOFILL)
                save.dismiss()
                try fixture.expectOutcome(.completed, "deny", "{\"dismissed\":true}")

                let payload = """
                    {"fieldRect":{"x":1,"y":2,"width":3,"height":4},"suggestions":[\
                    {"id":"s1","label":"ada","kind":"password"}]}
                    """
                var suggestions = try fixture.deliver(
                    AutofillSuggestionsRequest.self, BDK_SURFACE_AUTOFILL_SUGGESTIONS, payload)
                #expect(suggestions.fieldRect == CGRect(x: 1, y: 2, width: 3, height: 4))
                let first = try #require(suggestions.suggestions.first)
                #expect(first.kind == .password)
                try suggestions.accept(first, confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{\"suggestion\":\"s1\"}")
                suggestions = try fixture.deliver(
                    AutofillSuggestionsRequest.self, BDK_SURFACE_AUTOFILL_SUGGESTIONS, payload)
                suggestions.dismiss()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func notifications() async throws {
            try await withUIFixture { fixture in
                let payload = """
                    {"notification":{"title":"Hi","body":"There","origin":"https://n.test",\
                    "tag":"t","actions":["open"],"requireInteraction":true}}
                    """
                var request = try fixture.deliver(
                    NotificationRequest.self, BDK_SURFACE_NOTIFICATIONS, payload)
                #expect(request.notification.title == "Hi" && request.notification.tag == "t")
                #expect(request.notification.actions == ["open"])
                request.shown()
                #expect(request.isShown && !request.isCompleted)
                request.clicked(action: "open")
                try fixture.expectOutcome(
                    .completed, "accept", "{\"action\":\"open\",\"event\":\"clicked\"}")
                request = try fixture.deliver(
                    NotificationRequest.self, BDK_SURFACE_NOTIFICATIONS, payload)
                request.closed(byUser: true)
                try fixture.expectOutcome(
                    .completed, "deny", "{\"byUser\":true,\"event\":\"closed\"}")
            }
        }

        @Test func interstitials() async throws {
            try await withUIFixture { fixture in
                let overridable = """
                    {"url":"https://bad.test/","error":"dateInvalid","isOverridable":true,\
                    "certificate":{"id":"c","subject":"bad.test"},\
                    "text":{"title":"Not private","message":"Expired"}}
                    """
                var request = try fixture.deliver(
                    CertificateErrorRequest.self, BDK_SURFACE_CERTIFICATE_ERRORS, overridable)
                #expect(request.error == "dateInvalid" && request.certificate.subject == "bad.test")
                #expect(request.text.title == "Not private")
                try request.proceed(confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{}")

                request = try fixture.deliver(
                    CertificateErrorRequest.self, BDK_SURFACE_CERTIFICATE_ERRORS,
                    "{\"url\":\"https://hsts.test/\",\"error\":\"pinningFailure\"}")
                #expect(!request.isOverridable)
                await expectError(.ui, "notOverridable") {
                    try request.proceed(confirmedBy: try confirmingEvent())
                }
                #expect(!request.isCompleted)
                request.goBack()
                try fixture.expectOutcome(.completed, "deny", "{}")

                let warning = try fixture.deliver(
                    ReputationWarningRequest.self, BDK_SURFACE_REPUTATION_WARNINGS,
                    "{\"url\":\"https://phish.test/\",\"verdict\":\"phishing\",\"isOverridable\":true}"
                )
                #expect(warning.verdict == .phishing)
                try warning.proceed(confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{}")
            }
        }

        @Test func downloadsDeliverTheSubclassTheirPayloadNames() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(BDKUIRequest.self, BDK_SURFACE_DOWNLOADS)
                #expect(request is DownloadRequest)
                let dangerous = try fixture.deliver(
                    BDKUIRequest.self, BDK_SURFACE_DOWNLOADS, "{\"danger\":\"uncommon\"}")
                #expect(dangerous is DangerousDownloadRequest)
            }
        }
    }
}
