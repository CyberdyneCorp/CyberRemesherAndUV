//! An owned mesh handle.

use crate::error::{check, Error, Result};
use cyberremesh_sys as sys;
use std::ffi::CString;
use std::path::Path;

/// A mesh owned by the engine, freed on drop.
///
/// # Element ids move
///
/// Most retopology operations reassign element ids and subdivision reassigns
/// ALL of them, with nothing in the call to announce it. Anything you key on a
/// vertex or face id is stale the moment such a call returns. Either drop the
/// handle per operation, or compare [`Mesh::topology_generation`] before
/// trusting id-keyed state.
pub struct Mesh {
    raw: *mut sys::CyberMesh,
}

// SAFETY: a handle is an owned pointer into the engine's heap with no
// thread-affinity in its contract -- the header documents the process-global
// state (the worker cap, the import ceiling) separately, and the error slot is
// thread-local by design. Nothing here is shared, so moving one between threads
// is sound. Sync is deliberately NOT implemented: two threads mutating one mesh
// is not something the engine promises.
unsafe impl Send for Mesh {}

impl Mesh {
    /// Loads a mesh, dispatching on the file extension.
    pub fn load(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref();
        let c_path = CString::new(path.to_string_lossy().as_bytes())
            .map_err(|_| Error::misuse("cyber_mesh_load", "the path contains a NUL byte"))?;
        let mut raw: *mut sys::CyberMesh = std::ptr::null_mut();
        // SAFETY: a valid NUL-terminated path and a valid out-pointer. On
        // failure the engine leaves `raw` null, which the check below relies on.
        let status = unsafe { sys::cyber_mesh_load(c_path.as_ptr(), &mut raw) };
        check(status, "cyber_mesh_load")?;
        if raw.is_null() {
            return Err(Error::misuse(
                "cyber_mesh_load",
                "the engine reported success and returned no mesh",
            ));
        }
        Ok(Self { raw })
    }

    /// Writes the mesh, dispatching on the file extension.
    pub fn save(&self, path: impl AsRef<Path>) -> Result<()> {
        let path = path.as_ref();
        let c_path = CString::new(path.to_string_lossy().as_bytes())
            .map_err(|_| Error::misuse("cyber_mesh_save", "the path contains a NUL byte"))?;
        // SAFETY: a live handle and a valid NUL-terminated path.
        let status = unsafe { sys::cyber_mesh_save(self.raw, c_path.as_ptr()) };
        check(status, "cyber_mesh_save")
    }

    /// Vertices in the mesh.
    pub fn vertex_count(&self) -> usize {
        // SAFETY: a live handle; a counting query that cannot fail.
        unsafe { sys::cyber_mesh_vertex_count(self.raw) }
    }

    /// Faces in the mesh.
    pub fn face_count(&self) -> usize {
        // SAFETY: a live handle; a counting query that cannot fail.
        unsafe { sys::cyber_mesh_face_count(self.raw) }
    }

    /// A counter that changes whenever this mesh's element ids MAY have moved.
    ///
    /// Equal values prove the ids you hold still mean what they meant.
    /// Differing values only mean you must not assume — it errs in the safe
    /// direction, so a failed operation bumps it conservatively.
    ///
    /// A clone carries its source's value, because a clone's ids are the
    /// source's ids.
    pub fn topology_generation(&self) -> u64 {
        // SAFETY: a live handle; a read that cannot fail and tolerates null.
        unsafe { sys::cyber_mesh_topology_generation(self.raw) }
    }

    /// The raw handle, for calls this crate does not wrap yet.
    ///
    /// # Safety
    ///
    /// The pointer is owned by this `Mesh` and freed on drop. Do not free it,
    /// and do not keep it past the borrow.
    pub unsafe fn as_raw(&self) -> *mut sys::CyberMesh {
        self.raw
    }
}

impl Drop for Mesh {
    fn drop(&mut self) {
        // SAFETY: `raw` was produced by an engine call that reported success
        // and is freed exactly once, here. The constructor refuses a null, so
        // this is never a null free.
        unsafe { sys::cyber_mesh_free(self.raw) };
    }
}

impl std::fmt::Debug for Mesh {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Mesh")
            .field("vertices", &self.vertex_count())
            .field("faces", &self.face_count())
            .field("topology_generation", &self.topology_generation())
            .finish()
    }
}
