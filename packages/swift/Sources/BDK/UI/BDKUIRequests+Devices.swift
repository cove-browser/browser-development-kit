import AppKit

// Device choosers (ui-requests.md 5.16 and 8.1, T-UIR-13): WebUSB's, WebHID's, Web Serial's and
// Web Bluetooth's. The request stays open while the chooser is: each change of the device list
// arrives as an update, and a device keeps its id while the request is open.
//
// Completions: "select" with `{"device": id}` and the confirming event connects the page to that
// device; "deny", `.reject` and a dropped request cancel it, so the page's request rejects.

public final class DeviceChooserRequest: BDKUIRequest {
    public enum Kind: String, Sendable {
        case usb
        case hid
        case serial
        case bluetooth
    }

    public struct Device: Sendable, Hashable, Identifiable {
        public let id: String
        public let name: String
        /// Bluetooth only: whether the system has paired the device, and whether it is connected.
        public let isPaired: Bool
        public let isConnected: Bool
    }

    public let kind: Kind
    /// The origin of the frame that asked.
    public let origin: URL
    /// The devices on offer now.
    public private(set) var devices: [Device]
    /// Bluetooth: whether the adapter is on. Always true for the other kinds.
    public private(set) var isAdapterEnabled: Bool
    /// Whether the chooser is still looking for devices.
    public private(set) var isScanning: Bool
    private let changes = EventBroadcaster<[Device]>()

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        kind = fields.choice("kind", default: .usb)
        origin = fields.requiredURL("origin")
        devices = Self.devices(fields)
        isAdapterEnabled = fields.bool("adapterEnabled", default: true)
        isScanning = fields.bool("scanning")
        super.init(delivery, runtime: runtime)
        observeUpdates { [weak self] update in self?.receive(BDKUIPayload(update)) }
        cancellation.onFire { [changes] in changes.finish() }
    }

    /// Each new list of devices, as devices appear and disappear while the chooser is open. Ends
    /// when the request does.
    public var deviceChanges: BDKEventSequence<[Device]> {
        BDKEventSequence(changes)
    }

    /// Connects the page to `device`. `event` is the click or key press that chose it: without one
    /// it throws `ui.gestureRequired`. Throws `ui.deviceNotOffered` for a device not on offer.
    public func select(_ device: Device, confirmedBy event: NSEvent) throws {
        guard devices.contains(where: { $0.id == device.id }) else {
            throw BDKError(
                domain: .ui, code: "deviceNotOffered",
                message: "the chooser does not offer this device")
        }
        try submit("select", ["device": .string(device.id)], gesture: event)
    }

    public func cancel() {
        try? submit("deny")
    }

    private func receive(_ update: BDKUIPayload) {
        devices = Self.devices(update)
        isAdapterEnabled = update.bool("adapterEnabled", default: isAdapterEnabled)
        isScanning = update.bool("scanning", default: isScanning)
        changes.send(devices)
    }

    private static func devices(_ fields: BDKUIPayload) -> [Device] {
        fields.objects("devices").compactMap { device in
            guard let id = device.string("id") else { return nil }
            return Device(
                id: id, name: device.string("name") ?? "", isPaired: device.bool("paired"),
                isConnected: device.bool("connected"))
        }
    }
}
