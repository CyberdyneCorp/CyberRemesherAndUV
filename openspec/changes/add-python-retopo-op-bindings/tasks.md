# Tasks: Python bindings for the retopology mesh operations

- [x] 1.1 Declare the eight entry points in `_ffi.py` with correct argtypes/restype
- [x] 1.2 `Mesh` methods with docstrings carrying the header's element-id stability contract
- [x] 1.3 `SnapReport` for `snap_all`'s two out-params; export it from `__init__.py`
- [x] 2.1 `python/cyberremesh/tests/test_retopo_ops.py`, registered with CTest
- [x] 2.2 Verify the tests fail on reverted code, and on targeted wrapper mutations
- [x] 3.1 README: the newly reachable ops, and that the stroke family is still C-ABI-only
