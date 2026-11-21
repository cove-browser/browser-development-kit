import CoreGraphics
import Foundation

/// Screenshots, PDFs and saved copies of one page (swift-api.md section 6.7). Get it from
/// `BDKWebContents.capture`. Every call is a `contents.*` command with this page as its target.
@MainActor
public struct PageCapture {
    public enum ScreenshotArea: Sendable, Equatable {
        /// What the page shows now.
        case visible
        /// The whole document.
        case fullPage
        /// A part of the document, in CSS pixels.
        case rect(CGRect)
    }

    /// A paper size in inches.
    public struct Paper: Sendable, Equatable {
        public var width: Double
        public var height: Double

        public init(width: Double, height: Double) {
            self.width = width
            self.height = height
        }

        public static let letter = Paper(width: 8.5, height: 11)
        // The paper is called A4 (swift-api.md 6.7).
        // swiftlint:disable:next identifier_name
        public static let a4 = Paper(width: 8.27, height: 11.69)
    }

    /// Page margins in inches.
    public struct Margins: Sendable, Equatable {
        public var top: Double
        public var bottom: Double
        public var left: Double
        public var right: Double

        public init(top: Double, bottom: Double, left: Double, right: Double) {
            self.top = top
            self.bottom = bottom
            self.left = left
            self.right = right
        }

        public init(all margin: Double) {
            self.init(top: margin, bottom: margin, left: margin, right: margin)
        }
    }

    /// What `pdf` prints. A nil field takes the engine's default.
    public struct PDFOptions: Sendable, Equatable {
        public var paper: Paper?
        public var landscape: Bool
        public var printBackground: Bool
        public var margins: Margins?
        public var scale: Double?
        /// For example "1-3, 5".
        public var pageRanges: String?

        public init(
            paper: Paper? = nil, landscape: Bool = false, printBackground: Bool = false,
            margins: Margins? = nil, scale: Double? = nil, pageRanges: String? = nil
        ) {
            self.paper = paper
            self.landscape = landscape
            self.printBackground = printBackground
            self.margins = margins
            self.scale = scale
            self.pageRanges = pageRanges
        }
    }

    private let scope: BDKServiceScope

    init(contents: BDKWebContents) {
        scope = BDKServiceScope(runtime: contents.profile.runtime, profile: nil, contents: contents)
    }

    /// A PNG of the page.
    public func screenshot(_ area: ScreenshotArea = .visible) async throws -> Data {
        struct Arguments: Encodable {
            var area: BDKScreenshotArea
            var rect: BDKRect?
            var format = BDKImageFormat.png
        }
        var arguments = Arguments(area: .visible)
        switch area {
        case .visible:
            break
        case .fullPage:
            arguments.area = .fullPage
        case .rect(let rect):
            arguments.area = .rect
            arguments.rect = BDKRect(
                x: rect.origin.x, y: rect.origin.y, width: rect.width, height: rect.height)
        }
        let result = try await scope.callBinary(
            "contents.screenshot", arguments, as: BDKCapture.self)
        return try Self.bytes(result, kind: "image")
    }

    /// The page as a PDF. `onProgress` runs on the main actor while the engine paginates.
    public func pdf(
        _ options: PDFOptions = PDFOptions(),
        onProgress: (@MainActor (Double, String) -> Void)? = nil
    ) async throws -> Data {
        struct Arguments: Encodable {
            var landscape: Bool
            var printBackground: Bool
            var scale: Double?
            var paperWidth: Double?
            var paperHeight: Double?
            var marginTop: Double?
            var marginBottom: Double?
            var marginLeft: Double?
            var marginRight: Double?
            var pageRanges: String?
        }
        let arguments = Arguments(
            landscape: options.landscape, printBackground: options.printBackground,
            scale: options.scale, paperWidth: options.paper?.width,
            paperHeight: options.paper?.height, marginTop: options.margins?.top,
            marginBottom: options.margins?.bottom, marginLeft: options.margins?.left,
            marginRight: options.margins?.right, pageRanges: options.pageRanges)
        let result = try await scope.callBinary(
            "contents.pdf", arguments, as: BDKCapture.self, onProgress: onProgress)
        return try Self.bytes(result, kind: "PDF")
    }

    /// Saves the page to `url`.
    public func save(to url: URL, as format: BDKSaveFormat) async throws {
        struct Arguments: Encodable {
            var path: String
            var format: BDKSaveFormat
        }
        try await scope.run("contents.save", Arguments(path: url.path, format: format))
    }

    private static func bytes(_ result: (value: BDKCapture, binary: Data?), kind: String) throws
        -> Data
    {
        guard let binary = result.binary else {
            throw BDKError(
                domain: .runtime, code: "malformedResult",
                message: "the runtime returned no \(kind) bytes")
        }
        return binary
    }
}

extension BDKWebContents {
    /// Screenshots, PDFs and saved copies.
    public var capture: PageCapture {
        PageCapture(contents: self)
    }
}
