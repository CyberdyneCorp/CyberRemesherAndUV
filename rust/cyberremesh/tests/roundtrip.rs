//! The crate against a real engine: load, inspect, save.
//!
//! Deliberately a procedural fixture rather than a corpus model — examples/
//! models are git-ignored and fetched on demand, so a test needing one is a
//! test that does not run in CI.

use cyberremesh::{abi, check_abi, solver, version, Mesh, Solver};

const PLANE_OBJ: &str = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";

#[test]
fn bulk_indexed_mesh_preserves_authored_quad() {
    let mesh = Mesh::from_indexed(
        &[
            [0.0, 0.0, 0.0],
            [1.0, 0.0, 0.0],
            [1.0, 1.0, 0.0],
            [0.0, 1.0, 0.0],
        ],
        &[0, 4],
        &[0, 1, 2, 3],
    )
    .expect("the bulk mesh should import");
    assert_eq!(
        mesh.authored_polygons().expect("polygon export"),
        (vec![0, 4], vec![0, 1, 2, 3])
    );
}

fn write_plane(name: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    std::fs::write(&path, PLANE_OBJ).expect("the fixture should be writable");
    path
}

#[test]
fn the_abi_is_checked_before_anything_else() {
    // What a host should do at startup, and the only test here that would fail
    // for a reason other than a bug in this crate: a mismatched library.
    check_abi().expect("the linked engine cannot serve this crate's ABI");
    let (major, minor) = abi();
    assert_eq!(major, cyberremesh::ABI_MAJOR);
    assert!(minor >= cyberremesh::ABI_MINOR);
}

#[test]
fn a_mesh_loads_reports_and_saves() {
    let path = write_plane("cyberremesh_rs_plane.obj");
    let mesh = Mesh::load(&path).expect("the plane should load");
    assert_eq!(mesh.vertex_count(), 4);
    assert_eq!(mesh.face_count(), 1);

    let out = std::env::temp_dir().join("cyberremesh_rs_plane_out.obj");
    mesh.save(&out).expect("the mesh should save");
    let reloaded = Mesh::load(&out).expect("what we wrote should load");
    assert_eq!(reloaded.vertex_count(), 4);

    let _ = std::fs::remove_file(&path);
    let _ = std::fs::remove_file(&out);
}

#[test]
fn a_missing_file_is_an_error_and_not_a_panic() {
    // The point of the safe layer: a refusal crosses as a Result carrying the
    // engine's own status name and message, not a code the caller looks up.
    let err = Mesh::load("/nonexistent/cyberremesh/plane.obj").expect_err("should refuse");
    assert_eq!(err.operation, "cyber_mesh_load");
    assert!(
        !err.status.is_empty(),
        "the engine's status name is missing"
    );
}

#[test]
fn a_path_with_a_nul_byte_is_refused_before_the_engine_is_called() {
    // Refused by this crate rather than the engine, and reported as such --
    // "the library said no" and "we never asked it" are different facts.
    let err = Mesh::load("plane\0.obj").expect_err("should refuse");
    assert_eq!(err.status, "refused before the call");
}

#[test]
fn the_topology_generation_tracks_the_id_contract() {
    let path = write_plane("cyberremesh_rs_gen.obj");
    let mesh = Mesh::load(&path).expect("the plane should load");
    // Two reads with no edit between them must agree, or the counter reports
    // something other than topology and a host drops valid annotations every
    // frame.
    assert_eq!(mesh.topology_generation(), mesh.topology_generation());
    let _ = std::fs::remove_file(&path);
}

#[test]
fn the_solver_is_reported_and_this_build_has_the_vendored_field() {
    // The build script configures CYBER_WITH_QUADCOVER=ON, so a build from this
    // repository should carry it. If this fails, the build silently fell back
    // to the portable quadrangulator -- which is exactly what the entry point
    // exists to make visible, so a failure here is the feature working.
    match solver() {
        Solver::NativeAndGeogram => {}
        other => panic!(
            "built without the in-process QuadCover solver ({other:?}); this \
             engine reports {} and would produce different quads",
            version()
        ),
    }
}
