import CBDK

extension BDKWebContents {
    /// What a page captures, and whether another page captures it (`BDK_CAPTURE_*`). While
    /// `.display` is set the application shows that the page is sharing, unless it left the
    /// `screenCapture` surface to BDK, whose sharing bar does.
    public struct CaptureActivity: OptionSet, Sendable, Hashable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }
        public static let camera = CaptureActivity(rawValue: BDK_CAPTURE_CAMERA)
        public static let microphone = CaptureActivity(rawValue: BDK_CAPTURE_MICROPHONE)
        /// A screen, a window or a page.
        public static let display = CaptureActivity(rawValue: BDK_CAPTURE_DISPLAY)
        /// This page is being captured.
        public static let captured = CaptureActivity(rawValue: BDK_CAPTURE_CAPTURED)
    }

    /// What the page captures now, or the last value read once the page is gone.
    public var captureActivity: CaptureActivity { current.captureActivity }
}
