import Foundation

/// Turns what a person typed in the address field into a URL to load.
public enum AddressInput {
    /// A URL with a scheme is used as it is. Text with no spaces that looks like a host name gets `https://`.
    /// Anything else is not an address: the example has no search.
    public static func url(from text: String) -> URL? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, !trimmed.contains(where: \.isWhitespace) else { return nil }
        if trimmed.contains("://") || trimmed.hasPrefix("about:") {
            return URL(string: trimmed)
        }
        let host = trimmed.split(separator: "/", maxSplits: 1).first.map(String.init) ?? trimmed
        guard host.contains(".") || host.hasPrefix("localhost") else { return nil }
        return URL(string: "https://" + trimmed)
    }
}
