#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cyber::remesh::halflattice {

enum class RejectionReason : std::uint8_t {
    None,
    EmptyRow,
    ContinuousDependency,
    ExcludedArc,
    FractionalCoefficient,
    Overflow,
    ParityConflict,
    BoundViolation,
    TargetResidual,
    Underdetermined,
};

struct SourceRow {
    std::size_t arc = 0;
    std::vector<std::pair<std::size_t, double>> terms;
    // The existing Bi-MDF result stores targets in half-cells. This is the
    // corresponding integer h in sum(c_i * x_i) = h / 2.
    std::int64_t targetHalf = 0;
    RejectionReason dependency = RejectionReason::None;
};

struct Equation {
    std::size_t arc = 0;
    std::vector<std::pair<std::size_t, std::int64_t>> terms;
    std::int64_t rhs = 0;
    RejectionReason rejection = RejectionReason::None;
};

// Converts a source equation to sum(a_i * z_i) = rhs where z_i = 2*x_i.
// Coefficients must be representable in half steps; unsupported input is
// classified instead of rounded.
Equation normalize(const SourceRow& source);

struct Component {
    std::vector<std::size_t> equations;
    std::vector<std::size_t> variables;
    RejectionReason rejection = RejectionReason::None;
};

struct OwnershipAudit {
    std::size_t isolatedComponents = 0;
    std::size_t blockedComponents = 0;
    std::size_t sharedRejectedVariables = 0;
};

// Components connect equations only through shared variables. A rejection on
// any member makes the complete component ineligible; it is never separated
// into a clean injectable subset.
std::vector<Component> buildComponents(const std::vector<Equation>& equations);

// Rebuilds components from supported equations only, then proves whether each
// would still share a variable with a rejected equation. Only isolated
// components can be admitted for guarded injection.
OwnershipAudit auditRejectedOwnership(const std::vector<Equation>& equations);

struct ParityResult {
    bool consistent = true;
    std::size_t rank = 0;
    // Equation indices whose XOR yields the contradiction. Empty when
    // consistent. Keeping this provenance makes failures inspectable without
    // relying on a floating pivot order.
    std::vector<std::size_t> witnessEquations;
    // First equation in witnessEquations, or kNoWitness when consistent.
    std::size_t witnessEquation = static_cast<std::size_t>(-1);
};

// Checks sum((a_i mod 2) * z_i) = rhs mod 2 using deterministic elimination.
// The caller supplies one complete component, so a failed result rejects that
// entire component before any integer assignment is considered.
ParityResult solveParity(const std::vector<Equation>& equations,
                         const Component& component);

struct CompletionResult {
    RejectionReason rejection = RejectionReason::None;
    std::vector<std::pair<std::size_t, std::int64_t>> values;
};

struct ProjectionResult {
    RejectionReason rejection = RejectionReason::None;
    std::vector<Equation> equations;
};

// Replaces the targets of one complete component with the exact targets
// implied by supplied doubled-lattice values. Rejected or partially owned
// components remain rejected; no subset is projected.
ProjectionResult projectTargets(const std::vector<Equation>& equations,
                                const Component& component,
                                const std::vector<std::pair<std::size_t, std::int64_t>>& values);

// Completes a closed component only when deterministic integral elimination
// determines every doubled-lattice variable within [minimum, maximum].
CompletionResult completeBounded(const std::vector<Equation>& equations,
                                 const Component& component, std::int64_t minimum,
                                 std::int64_t maximum);

}  // namespace cyber::remesh::halflattice
