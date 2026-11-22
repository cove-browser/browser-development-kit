import BDK
import CBDK
import CBDKStub
import Foundation

// MARK: - Providers

extension BDKStub {
    /// The page finished loading with these text segments, in `language` (nil: not declared). The
    /// stub detects, offers or translates as the engine would after a load.
    public func setPageText(
        _ texts: [String], language: String? = nil, on page: BDKWebContents
    ) throws {
        let json = String(decoding: try JSONEncoder().encode(texts), as: UTF8.self)
        let status = withBDKStrings([language ?? "", json]) { lent in
            bdk_stub_page_text_set(page.handle, lent[0], lent[1])
        }
        try check(status)
    }

    /// The page's text segments now: translated while a translation is on.
    public func pageText(of page: BDKWebContents) throws -> [String] {
        var buffer: bdk_buffer_t = 0
        try check(bdk_stub_page_text(page.handle, &buffer))
        return try JSONDecoder().decode([String].self, from: Self.take(buffer))
    }

    /// Loads a block list for the default reputation provider, as a downloaded list whose
    /// signature was checked would be. Throws `provider.invalidList` for a list the core refuses.
    public func loadReputationList(_ listJSON: String) throws {
        let handle = try runtime.liveHandle()
        var error: bdk_error_t = 0
        let status = withBDKString(listJSON) { list in
            bdk_stub_reputation_list_load(handle, list, &error)
        }
        defer {
            if error != 0 { bdk_error_release(error) }
        }
        guard status != BDK_OK else { return }
        func string(_ value: bdk_string_t) -> String {
            guard let data = value.data else { return "" }
            return String(
                decoding: UnsafeRawBufferPointer(start: data, count: value.length), as: UTF8.self)
        }
        throw BDKError(
            domain: BDKError.Domain(rawValue: string(bdk_error_domain(error))),
            code: string(bdk_error_code(error)), message: string(bdk_error_message(error)))
    }

    /// The page's script calls `watchPosition` (its permission is granted).
    public func startGeolocationWatch(on page: BDKWebContents, highAccuracy: Bool = false) throws {
        try check(bdk_stub_geolocation_watch(page.handle, highAccuracy ? 1 : 0))
    }

    /// The last position or error the page received, as JSON; nil when none.
    public func lastGeolocation(of page: BDKWebContents) throws -> String? {
        var buffer: bdk_buffer_t = 0
        try check(bdk_stub_geolocation_last(page.handle, &buffer))
        let json = String(decoding: Self.take(buffer), as: UTF8.self)
        return json.isEmpty ? nil : json
    }

    /// The page clears its watch.
    public func stopGeolocationWatch(on page: BDKWebContents) throws {
        try check(bdk_stub_geolocation_stop(page.handle))
    }
}
