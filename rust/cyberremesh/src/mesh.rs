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
    /// Copies indexed positions and CSR-authored polygons into a new mesh.
    ///
    /// `face_offsets` starts at zero, ends at `indices.len()`, and every face
    /// has at least three corners. This deliberately exposes only geometry:
    /// the C ABI's typed attribute columns are available to hosts that need
    /// them, while this small Rust wrapper does not erase attribute type/domain
    /// information behind an incomplete enum.
    pub fn from_indexed(
        positions: &[[f32; 3]],
        face_offsets: &[usize],
        indices: &[u32],
    ) -> Result<Self> {
        if face_offsets.is_empty()
            || face_offsets[0] != 0
            || *face_offsets.last().expect("checked non-empty") != indices.len()
            || face_offsets
                .windows(2)
                .any(|pair| pair[1] < pair[0] || pair[1] - pair[0] < 3)
        {
            return Err(Error::misuse(
                "cyber_mesh_from_indexed",
                "offsets must span the index buffer with faces of at least three corners",
            ));
        }
        if positions.iter().flatten().any(|value| !value.is_finite()) {
            return Err(Error::misuse(
                "cyber_mesh_from_indexed",
                "positions must be finite",
            ));
        }
        let view = sys::CyberIndexedMesh {
            positions: positions.as_ptr().cast(),
            vertex_count: positions.len(),
            face_offsets: face_offsets.as_ptr(),
            face_count: face_offsets.len() - 1,
            indices: indices.as_ptr(),
            index_count: indices.len(),
            attributes: std::ptr::null(),
            attribute_count: 0,
        };
        let mut raw = std::ptr::null_mut();
        // SAFETY: all buffers remain alive for the call and the ABI copies them.
        let status = unsafe { sys::cyber_mesh_from_indexed(&view, &mut raw) };
        check(status, "cyber_mesh_from_indexed")?;
        if raw.is_null() {
            return Err(Error::misuse(
                "cyber_mesh_from_indexed",
                "the engine reported success and returned no mesh",
            ));
        }
        Ok(Self { raw })
    }

    /// Copies authored polygon CSR connectivity without render triangulation.
    pub fn authored_polygons(&self) -> Result<(Vec<usize>, Vec<u32>)> {
        // SAFETY: a live handle; null output asks for the required count.
        let offset_count =
            unsafe { sys::cyber_mesh_copy_face_offsets(self.raw, std::ptr::null_mut(), 0) };
        // SAFETY: a live handle; null output asks for the required count.
        let index_count =
            unsafe { sys::cyber_mesh_copy_polygon_indices(self.raw, std::ptr::null_mut(), 0) };
        let mut offsets = vec![0; offset_count];
        let mut indices = vec![0; index_count];
        // SAFETY: buffers hold exactly the queried capacities.
        let copied_offsets = unsafe {
            sys::cyber_mesh_copy_face_offsets(self.raw, offsets.as_mut_ptr(), offsets.len())
        };
        // SAFETY: buffers hold exactly the queried capacities.
        let copied_indices = unsafe {
            sys::cyber_mesh_copy_polygon_indices(self.raw, indices.as_mut_ptr(), indices.len())
        };
        if copied_offsets != offsets.len() || copied_indices != indices.len() {
            return Err(Error::misuse(
                "cyber_mesh_copy_polygon_indices",
                "the mesh changed during the two-step copy",
            ));
        }
        Ok((offsets, indices))
    }

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
