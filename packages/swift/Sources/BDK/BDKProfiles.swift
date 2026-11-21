import CBDK
import Foundation
import os

private struct ProfileInfoDTO: Decodable {
    let id: String
    let name: String
    let kind: String
    let parent: String?

    var info: BDKProfile.Info {
        BDKProfile.Info(
            id: BDKProfile.ID(rawValue: id), name: name,
            kind: kind == "persistent" ? .persistent : .ephemeral,
            parent: parent.map(BDKProfile.ID.init(rawValue:)))
    }
}

private struct ProfileIdentifierDTO: Decodable {
    let id: String
}

/// A profile on disk, as `BDKProfiles.list()` reports it.
public struct ProfileDescriptor: Sendable, Identifiable, Equatable {
    public let id: BDKProfile.ID
    public let name: String
}

/// Profile lifecycle: list, create, open, delete.
@MainActor
public final class BDKProfiles {
    private unowned let runtime: BDKRuntime
    private var registry: [UInt64: WeakBox<BDKProfile>] = [:]

    init(runtime: BDKRuntime) {
        self.runtime = runtime
    }

    /// The profiles that are open now.
    public var loaded: [BDKProfile] {
        registry.keys.sorted().compactMap { registry[$0]?.value }
    }

    /// Every persistent profile. Ephemeral profiles are never listed.
    public func list() async throws -> [ProfileDescriptor] {
        let handle = try runtime.liveHandle()
        let json = try await abiAsync(.runtime) { context, result, release in
            bdk_profiles_list(handle, result, context, release)
        }
        return try decodeResult([ProfileInfoDTO].self, from: json).map {
            ProfileDescriptor(id: BDKProfile.ID(rawValue: $0.id), name: $0.name)
        }
    }

    /// Creates a profile. Fails with `profile.nameTaken` when the name exists.
    public func create(name: String) async throws -> ProfileDescriptor {
        let handle = try runtime.liveHandle()
        let json = try await abiAsync(.runtime) { context, result, release in
            withBDKString(name) { bdk_profile_create(handle, $0, result, context, release) }
        }
        let created = try decodeResult(ProfileIdentifierDTO.self, from: json)
        return ProfileDescriptor(id: BDKProfile.ID(rawValue: created.id), name: name)
    }

    /// Opens a profile by identifier. It never creates. A profile has at most one open object:
    /// opening it again fails with `profile.inUse`, and an unknown identifier fails with
    /// `profile.notFound`.
    public func open(id: BDKProfile.ID) async throws -> BDKProfile {
        let handle = try runtime.liveHandle()
        var profileHandle: bdk_profile_t = 0
        let json = try await abiAsync(.runtime) { context, result, release in
            withBDKString(id.rawValue) {
                bdk_profile_open(handle, $0, &profileHandle, result, context, release)
            }
        }
        return adopt(profileHandle, info: try decodeResult(ProfileInfoDTO.self, from: json).info)
    }

    /// Opens the profile with this name, creating it first when `createIfMissing` is true.
    public func open(_ name: String, createIfMissing: Bool = true) async throws -> BDKProfile {
        let existing = try await list().first { $0.name == name }
        if let existing { return try await open(id: existing.id) }
        guard createIfMissing else {
            throw BDKError(
                domain: .profile, code: "notFound", message: "no profile is named '\(name)'")
        }
        return try await open(id: try await create(name: name).id)
    }

    /// Opens an ephemeral profile: no disk state, discarded on close.
    public func openPrivate() throws -> BDKProfile {
        let handle = try runtime.liveHandle()
        var profileHandle: bdk_profile_t = 0
        try abiCall(.runtime) { bdk_profile_open_private(handle, &profileHandle, $0) }
        return adopt(profileHandle, info: try BDKProfile.readInfo(profileHandle))
    }

    /// Deletes a closed profile. Fails with `profile.inUse` while it is open and with
    /// `profile.notFound` for an unknown identifier.
    public func delete(_ id: BDKProfile.ID) async throws {
        let handle = try runtime.liveHandle()
        _ = try await abiAsync(.runtime) { context, result, release in
            withBDKString(id.rawValue) { bdk_profile_delete(handle, $0, result, context, release) }
        }
    }

    // MARK: Internals

    private func adopt(_ handle: bdk_profile_t, info: BDKProfile.Info) -> BDKProfile {
        let profile = BDKProfile(runtime: runtime, handle: handle, info: info)
        registry[handle] = WeakBox(profile)
        return profile
    }

    /// The open profile with this handle, if Swift holds it.
    func loaded(handle: bdk_profile_t) -> BDKProfile? {
        handle == 0 ? nil : registry[handle]?.value
    }

    func forget(_ handle: bdk_profile_t) {
        registry[handle] = nil
    }
}

/// An open profile: a storage context. Pages are created in one.
@MainActor
public final class BDKProfile: Identifiable {
    public struct ID: Hashable, Sendable, RawRepresentable, CustomStringConvertible {
        public let rawValue: String

        public init(rawValue: String) {
            self.rawValue = rawValue
        }

        public var description: String {
            rawValue
        }
    }

    public enum Kind: Sendable {
        case persistent
        /// No disk state; discarded on close.
        case ephemeral
    }

    public struct Info: Sendable, Equatable {
        public let id: ID
        public let name: String
        public let kind: Kind
        public let parent: ID?
    }

    public let id: ID
    public private(set) var name: String
    public let kind: Kind
    public let parent: ID?
    public var isPrivate: Bool {
        kind == .ephemeral
    }

    /// False once the profile was closed or its runtime shut down.
    public var isOpen: Bool {
        handle != 0
    }

    let runtime: BDKRuntime
    private(set) var handle: bdk_profile_t

    init(runtime: BDKRuntime, handle: bdk_profile_t, info: Info) {
        self.runtime = runtime
        self.handle = handle
        self.id = info.id
        self.name = info.name
        self.kind = info.kind
        self.parent = info.parent
    }

    isolated deinit {
        guard handle != 0 else { return }
        Logger(subsystem: "BDK", category: "lifecycle")
            .warning("profile \(self.id.rawValue) was released without close(); closing it")
        var error: bdk_error_t = 0
        _ = bdk_profile_close(handle, &error)
        if error != 0 { bdk_error_release(error) }
        EventLoop.kick()
    }

    /// The pages of this profile.
    public var webContents: WebContentsFactory {
        WebContentsFactory(profile: self)
    }

    /// Reads the profile's current description from the runtime.
    public func info() throws -> Info {
        try Self.readInfo(try liveHandle())
    }

    static func readInfo(_ handle: bdk_profile_t) throws -> Info {
        var buffer: bdk_buffer_t = 0
        try abiCall(.profile) { bdk_profile_info(handle, &buffer, $0) }
        return try decodeResult(
            ProfileInfoDTO.self, from: String(decoding: takeBuffer(buffer), as: UTF8.self)
        ).info
    }

    /// Fails with `profile.nameTaken`, and with `profile.isPrivate` for an ephemeral profile.
    public func rename(to newName: String) throws {
        let handle = try liveHandle()
        try abiCall(.profile) { error in
            withBDKString(newName) { bdk_profile_rename(handle, $0, error) }
        }
        name = newName
    }

    /// Closes the profile and every page in it, and releases it. After this the profile and its
    /// pages are stale. A closed persistent profile can be opened again.
    public func close() throws {
        let handle = try liveHandle()
        do {
            try abiCall(.profile) { bdk_profile_close(handle, $0) }
        } catch let failure as BDKError where failure.isStale {
            invalidate()
            throw failure
        }
        invalidate()
    }

    func liveHandle() throws -> bdk_profile_t {
        guard handle != 0, runtime.state == .running else { throw BDKError.stale(.profile) }
        return handle
    }

    /// Forgets the native handle after the runtime released it.
    func invalidate() {
        guard handle != 0 else { return }
        for contents in runtime.webContents where contents.profile === self {
            contents.invalidate()
        }
        runtime.profiles.forget(handle)
        handle = 0
    }
}

/// Creates and lists the pages of one profile.
@MainActor
public struct WebContentsFactory {
    let profile: BDKProfile

    /// The live pages of the profile.
    public var all: [BDKWebContents] {
        profile.runtime.webContents.filter { $0.profile === profile }
    }

    /// Creates a page. It needs the registrations to be frozen, which `BDKRuntime.start` does.
    public func create(_ options: BDKWebContents.Options = .init()) throws -> BDKWebContents {
        try BDKWebContents.create(in: profile, options: options)
    }
}
