import Foundation
import Security

/// Which passkey and security-key mechanisms this application can offer, and what it lacks for
/// the others (ui-requests.md 5.18, T-UIR-09). The runtime runs Chromium's own WebAuthn request,
/// so these are Chromium's conditions, read from the running application's `Info.plist` and its
/// code signature. Show it in a settings or diagnostics page, or check it at start; it never
/// changes what a page may ask.
public struct BDKWebAuthnSupport: Sendable, Equatable {
    /// Something the application must have for a mechanism to be offered.
    public enum Requirement: Sendable, Hashable, CustomStringConvertible {
        /// A key the application's `Info.plist` must have.
        case infoPlistKey(String)
        /// An entitlement the application must be signed with.
        case entitlement(String)
        /// The lowest macOS version, as "major.minor".
        case macOS(String)
        /// A `bdk.toml` setting, as it must be written.
        case configuration(String)
        /// This runtime does not offer the mechanism to BDK applications yet (the task names the
        /// work).
        case notInThisRuntime(task: String)

        public var description: String {
            switch self {
            case .infoPlistKey(let key): "the Info.plist key \(key)"
            case .entitlement(let name): "the entitlement \(name)"
            case .macOS(let version): "macOS \(version) or later"
            case .configuration(let setting): "\(setting) in bdk.toml"
            case .notInThisRuntime(let task): "support in a later runtime (\(task))"
            }
        }
    }

    public struct Mechanism: Sendable, Equatable {
        public let kind: WebAuthnRequest.Mechanism.Kind
        /// What the application lacks; empty when the mechanism is offered.
        public let missing: [Requirement]

        public var isAvailable: Bool { missing.isEmpty }
    }

    /// USB, NFC and Bluetooth security keys. They need nothing of the application.
    public let securityKeys: Mechanism
    /// Passkeys on a phone, over a QR code and Bluetooth (hybrid). The phone's own tunnel
    /// service carries the exchange, once the person has chosen the phone. Offered only with
    /// `[services.webauthn] phones = true` (configuration.md 3.8), which is off by default because
    /// an Android phone's tunnel service is a Google host.
    public let phones: Mechanism
    /// The system's passkey provider (iCloud Keychain, or another credential provider). Apple
    /// grants its entitlement to web browsers on request.
    public let systemPasskeys: Mechanism
    /// Passkeys in this Mac's Secure Enclave behind Touch ID, kept by the browser.
    public let touchID: Mechanism

    public var all: [Mechanism] { [securityKeys, phones, systemPasskeys, touchID] }

    /// What the running application can offer, with the configuration of the running runtime
    /// (phones are not offered before it starts).
    @MainActor
    public static var current: BDKWebAuthnSupport {
        BDKWebAuthnSupport(
            infoPlist: Bundle.main.infoDictionary ?? [:],
            hasEntitlement: hasEntitlement,
            isHardenedRuntime: isHardenedRuntime,
            operatingSystem: ProcessInfo.processInfo.operatingSystemVersion,
            phonesEnabled: BDKRuntime.isRunning && BDKRuntime.shared.offersPhonePasskeys,
            touchIDGroup: touchIDGroup, keychainGroups: keychainGroups)
    }

    static let phonesSetting = "[services.webauthn] phones = true"

    static let bluetoothUsage = "NSBluetoothAlwaysUsageDescription"
    static let bluetoothEntitlement = "com.apple.security.device.bluetooth"
    static let browserPasskeysEntitlement = "com.apple.developer.web-browser.public-key-credential"

    /// The rules, for facts read elsewhere: Chromium offers a phone when the application can use
    /// Bluetooth (the usage string, and under the hardened runtime the Bluetooth entitlement),
    /// and the system's passkeys on macOS 13.5 or later to a process with the browser passkey
    /// entitlement. Touch ID needs the application signed by a team and entitled to the keychain
    /// access group `touchIDGroup`, "<team>.<bundle id>.webauthn" (nil when it is not signed by a
    /// team), which the engine gives Chromium (T-UIR-11).
    init(
        infoPlist: [String: Any], hasEntitlement: (String) -> Bool, isHardenedRuntime: Bool,
        operatingSystem: OperatingSystemVersion, phonesEnabled: Bool,
        touchIDGroup: String? = nil, keychainGroups: [String] = []
    ) {
        securityKeys = Mechanism(kind: .securityKey, missing: [])
        var phone: [Requirement] = phonesEnabled ? [] : [.configuration(Self.phonesSetting)]
        if (infoPlist[Self.bluetoothUsage] as? String)?.isEmpty ?? true {
            phone.append(.infoPlistKey(Self.bluetoothUsage))
        }
        if isHardenedRuntime && !hasEntitlement(Self.bluetoothEntitlement) {
            phone.append(.entitlement(Self.bluetoothEntitlement))
        }
        phones = Mechanism(kind: .phone, missing: phone)
        var system: [Requirement] = []
        let passkeySystem = OperatingSystemVersion(
            majorVersion: 13, minorVersion: 5, patchVersion: 0)
        if (operatingSystem.majorVersion, operatingSystem.minorVersion)
            < (passkeySystem.majorVersion, passkeySystem.minorVersion)
        {
            system.append(.macOS("13.5"))
        }
        if !hasEntitlement(Self.browserPasskeysEntitlement) {
            system.append(.entitlement(Self.browserPasskeysEntitlement))
        }
        systemPasskeys = Mechanism(kind: .iCloudKeychain, missing: system)
        let group = touchIDGroup ?? "<team>.<bundle id>.webauthn"
        touchID = Mechanism(
            kind: .platform,
            missing: touchIDGroup.map(keychainGroups.contains) == true
                ? [] : [.entitlement("\(Self.keychainGroupsEntitlement) \(group)")])
    }

    static let keychainGroupsEntitlement = "keychain-access-groups"

    /// "<team>.<bundle id>.webauthn" of the running application, or nil when it is not signed by
    /// a team.
    private static var touchIDGroup: String? {
        guard let team = signingInformation?[kSecCodeInfoTeamIdentifier as String] as? String,
            !team.isEmpty, let bundle = Bundle.main.bundleIdentifier
        else { return nil }
        return "\(team).\(bundle).webauthn"
    }

    private static var keychainGroups: [String] {
        guard let task = SecTaskCreateFromSelf(nil) else { return [] }
        return SecTaskCopyValueForEntitlement(task, keychainGroupsEntitlement as CFString, nil)
            as? [String] ?? []
    }

    private static func hasEntitlement(_ name: String) -> Bool {
        guard let task = SecTaskCreateFromSelf(nil) else { return false }
        return isEnabled(
            entitlementValue: SecTaskCopyValueForEntitlement(task, name as CFString, nil))
    }

    /// A Boolean entitlement is granted only when its value is `true`; a signed `false` is
    /// still a value, and it disables the capability.
    static func isEnabled(entitlementValue value: Any?) -> Bool {
        (value as? Bool) == true
    }

    private static var signingInformation: [String: Any]? {
        var code: SecCode?
        guard SecCodeCopySelf([], &code) == errSecSuccess, let code else { return nil }
        var staticCode: SecStaticCode?
        guard SecCodeCopyStaticCode(code, [], &staticCode) == errSecSuccess, let staticCode else {
            return nil
        }
        var information: CFDictionary?
        guard
            SecCodeCopySigningInformation(
                staticCode, SecCSFlags(rawValue: kSecCSSigningInformation), &information)
                == errSecSuccess
        else { return nil }
        return information as? [String: Any]
    }

    private static var isHardenedRuntime: Bool {
        guard let flags = signingInformation?[kSecCodeInfoFlags as String] as? UInt32 else {
            return false
        }
        return SecCodeSignatureFlags(rawValue: flags).contains(.runtime)
    }
}
