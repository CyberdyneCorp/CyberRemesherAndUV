import CyberRemesher

// Compile-only clean-consumer fixture. Runtime remeshing and cancellation are
// exercised by the simulator test target once the binary package is resolved.
let abi = CyberRuntime.abiVersionComponents
precondition(abi.major > 0)
