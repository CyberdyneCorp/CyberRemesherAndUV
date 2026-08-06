// UNVERIFIED: requires the Swift toolchain and the `cyber_capi` library; not
// buildable in headless Linux CI. Written against the `cyber_document_*` block
// of capi/include/cyber_capi.h, not compiled by anything in this repo.
//
// RAII wrapper over the opaque `CyberDocument` handle — the editing unit that
// OUTLIVES the session: the high-poly Target, the low-poly EditMesh, and the
// named soft-selection slots, serialized into one versioned byte container.
// The slots on a `Mesh` handle are session state and vanish with the handle;
// this is what carries them across a save/load.
//
// THE SEAM IS BY VALUE: `setEditMesh` copies the mesh's geometry AND its named
// slots in, `editMesh()` hands back a fresh `Mesh` carrying them. Nothing stays
// in sync afterwards — mutate the mesh and set it again. The LIVE (unsaved)
// weight field is not a slot and is not persisted: name it with
// `saveSelection(_:)` first and `loadSelection(_:)` after the round trip.

import CCyberRemesher

/// A persisted CyberRemesher document.
public final class Document {
    /// The opaque engine handle. Non-optional for a live instance.
    let handle: OpaquePointer

    /// Wraps an already-owned engine handle. Takes ownership.
    init(owning handle: OpaquePointer) {
        self.handle = handle
    }

    /// An empty document (no meshes, no slots).
    public convenience init() throws {
        guard let handle = cyber_document_create() else { throw CyberError.outOfMemory }
        self.init(owning: handle)
    }

    deinit {
        cyber_document_free(handle)
    }

    // MARK: - Meshes

    /// Copies `mesh` in as the high-poly Target (`nil` clears it).
    public func setTarget(_ mesh: Mesh?) throws {
        try CyberError.check(cyber_document_set_target(handle, mesh?.handle))
    }

    /// Copies `mesh` in as the EditMesh, its named selection slots included.
    public func setEditMesh(_ mesh: Mesh?) throws {
        try CyberError.check(cyber_document_set_edit_mesh(handle, mesh?.handle))
    }

    /// A fresh `Mesh` owning a copy of the stored Target.
    public func target() throws -> Mesh {
        guard let mesh = cyber_document_target(handle) else { throw CyberError.outOfMemory }
        return Mesh(owning: mesh)
    }

    /// A fresh `Mesh` with the stored EditMesh and its named selection slots.
    public func editMesh() throws -> Mesh {
        guard let mesh = cyber_document_edit_mesh(handle) else { throw CyberError.outOfMemory }
        return Mesh(owning: mesh)
    }

    // MARK: - Slots

    /// Names of the persisted selection slots, in name order.
    public func selectionSlots() -> [String] {
        let count = Int(cyber_document_slot_count(handle))
        return (0..<count).compactMap { index in
            guard let raw = cyber_document_slot_name(handle, index) else { return nil }
            return String(cString: raw)
        }
    }

    /// The weights of one slot, indexed by EditMesh vertex id. Empty when the
    /// slot does not exist.
    public func selectionWeights(_ name: String) -> [Float] {
        let count = Int(cyber_document_slot_weights(handle, name, nil, 0))
        guard count > 0 else { return [] }
        var weights = [Float](repeating: 0, count: count)
        _ = weights.withUnsafeMutableBufferPointer { buffer in
            cyber_document_slot_weights(handle, name, buffer.baseAddress, buffer.count)
        }
        return weights
    }

    // MARK: - Serialization

    /// The document as one versioned byte container.
    public func save() -> [UInt8] {
        let count = Int(cyber_document_save(handle, nil, 0))
        guard count > 0 else { return [] }
        var bytes = [UInt8](repeating: 0, count: count)
        _ = bytes.withUnsafeMutableBufferPointer { buffer in
            cyber_document_save(handle, buffer.baseAddress, buffer.count)
        }
        return bytes
    }

    /// Parses a byte container produced by `save()`.
    public static func load(_ bytes: [UInt8]) throws -> Document {
        let loaded: OpaquePointer? = bytes.withUnsafeBufferPointer { buffer in
            cyber_document_load(buffer.baseAddress, buffer.count)
        }
        guard let loaded else { throw CyberError.invalidArgument("not a CyberRemesher document") }
        return Document(owning: loaded)
    }

    /// Writes the byte container to `path`.
    public func saveFile(_ path: String) throws {
        try CyberError.check(cyber_document_save_file(handle, path))
    }

    /// Reads a document written by `saveFile(_:)`.
    public static func loadFile(_ path: String) throws -> Document {
        guard let loaded = cyber_document_load_file(path) else {
            throw CyberError.invalidArgument("cannot read document '\(path)'")
        }
        return Document(owning: loaded)
    }
}
