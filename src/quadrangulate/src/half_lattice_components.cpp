#include "half_lattice_components.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>

namespace cyber::remesh::halflattice {

namespace {

constexpr double kHalfTolerance = 1e-7;
constexpr std::size_t kNoWitness = static_cast<std::size_t>(-1);

bool addChecked(std::int64_t& target, const std::int64_t value) {
    if ((value > 0 && target > std::numeric_limits<std::int64_t>::max() - value) ||
        (value < 0 && target < std::numeric_limits<std::int64_t>::min() - value)) {
        return false;
    }
    target += value;
    return true;
}

bool multiplyChecked(const std::int64_t left, const std::int64_t right, std::int64_t& product) {
    return !__builtin_mul_overflow(left, right, &product);
}

RejectionReason firstRejection(const RejectionReason current, const RejectionReason candidate) {
    return current == RejectionReason::None ? candidate : current;
}

void xorProvenance(std::vector<std::size_t>& destination, const std::vector<std::size_t>& source) {
    std::vector<std::size_t> merged;
    merged.reserve(destination.size() + source.size());
    std::set_symmetric_difference(destination.begin(), destination.end(), source.begin(), source.end(),
                                  std::back_inserter(merged));
    destination = std::move(merged);
}

}  // namespace

Equation normalize(const SourceRow& source) {
    Equation result;
    result.arc = source.arc;
    result.rejection = source.dependency;
    if (!multiplyChecked(source.targetHalf, 2, result.rhs)) {
        result.rejection = RejectionReason::Overflow;
        return result;
    }

    std::map<std::size_t, std::int64_t> merged;
    for (const auto& [variable, coefficient] : source.terms) {
        const double doubled = coefficient * 2.0;
        if (!std::isfinite(doubled) ||
            std::abs(doubled - std::round(doubled)) > kHalfTolerance ||
            doubled < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            doubled > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            result.rejection = RejectionReason::FractionalCoefficient;
            return result;
        }
        const std::int64_t integer = static_cast<std::int64_t>(std::llround(doubled));
        if (!addChecked(merged[variable], integer)) {
            result.rejection = RejectionReason::Overflow;
            return result;
        }
    }
    for (const auto& [variable, coefficient] : merged) {
        if (coefficient != 0) {
            result.terms.push_back({variable, coefficient});
        }
    }
    if (result.terms.empty()) {
        result.rejection = RejectionReason::EmptyRow;
    }
    return result;
}

std::vector<Component> buildComponents(const std::vector<Equation>& equations) {
    std::map<std::size_t, std::vector<std::size_t>> equationsOfVariable;
    for (std::size_t row = 0; row < equations.size(); ++row) {
        for (const auto& [variable, coefficient] : equations[row].terms) {
            if (coefficient != 0) {
                equationsOfVariable[variable].push_back(row);
            }
        }
    }

    std::vector<char> visited(equations.size(), 0);
    std::vector<Component> components;
    for (std::size_t first = 0; first < equations.size(); ++first) {
        if (visited[first] != 0) {
            continue;
        }
        Component component;
        std::queue<std::size_t> pending;
        pending.push(first);
        visited[first] = 1;
        while (!pending.empty()) {
            const std::size_t row = pending.front();
            pending.pop();
            component.equations.push_back(row);
            component.rejection = firstRejection(component.rejection, equations[row].rejection);
            for (const auto& [variable, coefficient] : equations[row].terms) {
                if (coefficient == 0) {
                    continue;
                }
                component.variables.push_back(variable);
                for (const std::size_t adjacent : equationsOfVariable[variable]) {
                    if (visited[adjacent] == 0) {
                        visited[adjacent] = 1;
                        pending.push(adjacent);
                    }
                }
            }
        }
        std::sort(component.equations.begin(), component.equations.end());
        std::sort(component.variables.begin(), component.variables.end());
        component.variables.erase(std::unique(component.variables.begin(), component.variables.end()),
                                  component.variables.end());
        components.push_back(std::move(component));
    }
    return components;
}

OwnershipAudit auditRejectedOwnership(const std::vector<Equation>& equations) {
    std::vector<Equation> supported;
    std::vector<std::size_t> rejectedVariables;
    for (const Equation& equation : equations) {
        if (equation.rejection == RejectionReason::None) {
            supported.push_back(equation);
            continue;
        }
        for (const auto& [variable, coefficient] : equation.terms) {
            if (coefficient != 0) {
                rejectedVariables.push_back(variable);
            }
        }
    }
    std::sort(rejectedVariables.begin(), rejectedVariables.end());
    rejectedVariables.erase(std::unique(rejectedVariables.begin(), rejectedVariables.end()),
                            rejectedVariables.end());
    OwnershipAudit audit;
    for (const Component& component : buildComponents(supported)) {
        const bool shared = std::any_of(component.variables.begin(), component.variables.end(),
                                        [&rejectedVariables](const std::size_t variable) {
                                            return std::binary_search(rejectedVariables.begin(),
                                                                      rejectedVariables.end(), variable);
                                        });
        if (shared) {
            ++audit.blockedComponents;
        } else {
            ++audit.isolatedComponents;
        }
    }
    audit.sharedRejectedVariables = rejectedVariables.size();
    return audit;
}

ParityResult solveParity(const std::vector<Equation>& equations, const Component& component) {
    ParityResult result;
    std::map<std::size_t, std::size_t> columnOf;
    for (std::size_t column = 0; column < component.variables.size(); ++column) {
        columnOf.emplace(component.variables[column], column);
    }
    const std::size_t columns = component.variables.size();
    std::vector<std::vector<unsigned char>> matrix;
    std::vector<std::vector<std::size_t>> provenance;
    matrix.reserve(component.equations.size());
    provenance.reserve(component.equations.size());
    for (const std::size_t rowIndex : component.equations) {
        std::vector<unsigned char> row(columns + 2, 0);
        const Equation& equation = equations[rowIndex];
        for (const auto& [variable, coefficient] : equation.terms) {
            if ((coefficient & 1) != 0) {
                row[columnOf.at(variable)] ^= 1;
            }
        }
        row[columns] = static_cast<unsigned char>(equation.rhs & 1);
        matrix.push_back(std::move(row));
        provenance.push_back({rowIndex});
    }

    std::size_t pivot = 0;
    for (std::size_t column = 0; column < columns && pivot < matrix.size(); ++column) {
        std::size_t source = pivot;
        while (source < matrix.size() && matrix[source][column] == 0) {
            ++source;
        }
        if (source == matrix.size()) {
            continue;
        }
        std::swap(matrix[pivot], matrix[source]);
        for (std::size_t row = 0; row < matrix.size(); ++row) {
            if (row != pivot && matrix[row][column] != 0) {
                for (std::size_t entry = column; entry <= columns; ++entry) {
                    matrix[row][entry] ^= matrix[pivot][entry];
                }
                xorProvenance(provenance[row], provenance[pivot]);
            }
        }
        ++pivot;
    }
    result.rank = pivot;
    for (std::size_t rowIndex = 0; rowIndex < matrix.size(); ++rowIndex) {
        const auto& row = matrix[rowIndex];
        bool zero = true;
        for (std::size_t column = 0; column < columns; ++column) {
            zero &= row[column] == 0;
        }
        if (zero && row[columns] != 0) {
            result.consistent = false;
            result.witnessEquations = provenance[rowIndex];
            result.witnessEquation = result.witnessEquations.front();
            return result;
        }
    }
    result.witnessEquation = kNoWitness;
    return result;
}

CompletionResult completeBounded(const std::vector<Equation>& equations, const Component& component,
                                 const std::int64_t minimum, const std::int64_t maximum) {
    CompletionResult result;
    if (component.rejection != RejectionReason::None) {
        result.rejection = component.rejection;
        return result;
    }
    if (!solveParity(equations, component).consistent) {
        result.rejection = RejectionReason::ParityConflict;
        return result;
    }
    std::map<std::size_t, std::size_t> columnOf;
    for (std::size_t column = 0; column < component.variables.size(); ++column) {
        columnOf.emplace(component.variables[column], column);
    }
    const std::size_t columns = component.variables.size();
    std::vector<std::vector<long double>> matrix(component.equations.size(),
                                                 std::vector<long double>(columns + 1, 0.0L));
    for (std::size_t row = 0; row < component.equations.size(); ++row) {
        const Equation& equation = equations[component.equations[row]];
        for (const auto& [variable, coefficient] : equation.terms) {
            matrix[row][columnOf.at(variable)] = static_cast<long double>(coefficient);
        }
        matrix[row][columns] = static_cast<long double>(equation.rhs);
    }

    constexpr long double kPivotTolerance = 1e-12L;
    std::vector<std::size_t> pivotColumn;
    std::size_t pivotRow = 0;
    for (std::size_t column = 0; column < columns && pivotRow < matrix.size(); ++column) {
        std::size_t source = pivotRow;
        while (source < matrix.size() && std::abs(matrix[source][column]) <= kPivotTolerance) {
            ++source;
        }
        if (source == matrix.size()) {
            continue;
        }
        std::swap(matrix[pivotRow], matrix[source]);
        const long double divisor = matrix[pivotRow][column];
        for (std::size_t entry = column; entry <= columns; ++entry) {
            matrix[pivotRow][entry] /= divisor;
        }
        for (std::size_t row = 0; row < matrix.size(); ++row) {
            if (row == pivotRow || std::abs(matrix[row][column]) <= kPivotTolerance) {
                continue;
            }
            const long double factor = matrix[row][column];
            for (std::size_t entry = column; entry <= columns; ++entry) {
                matrix[row][entry] -= factor * matrix[pivotRow][entry];
            }
        }
        pivotColumn.push_back(column);
        ++pivotRow;
    }
    for (std::size_t row = pivotRow; row < matrix.size(); ++row) {
        bool zero = true;
        for (std::size_t column = 0; column < columns; ++column) {
            zero &= std::abs(matrix[row][column]) <= kPivotTolerance;
        }
        if (zero && std::abs(matrix[row][columns]) > kPivotTolerance) {
            result.rejection = RejectionReason::TargetResidual;
            return result;
        }
    }

    // A rank-deficient component has a translation/gauge freedom. Fix every
    // free doubled-lattice variable to zero in stable variable order; the
    // resulting RREF solution is deterministic and still undergoes exact
    // integer substitution below.
    std::map<std::size_t, std::int64_t> values;
    for (std::size_t row = 0; row < pivotColumn.size(); ++row) {
        const long double value = matrix[row][columns];
        if (!std::isfinite(static_cast<double>(value)) ||
            std::abs(value - std::round(value)) > kPivotTolerance ||
            value < static_cast<long double>(minimum) || value > static_cast<long double>(maximum)) {
            result.rejection = value < static_cast<long double>(minimum) ||
                                       value > static_cast<long double>(maximum)
                                   ? RejectionReason::BoundViolation
                                   : RejectionReason::TargetResidual;
            return result;
        }
        values.emplace(component.variables[pivotColumn[row]],
                       static_cast<std::int64_t>(std::llround(value)));
    }
    for (const std::size_t variable : component.variables) {
        values.try_emplace(variable, 0);
    }
    for (const std::size_t rowIndex : component.equations) {
        std::int64_t sum = 0;
        for (const auto& [variable, coefficient] : equations[rowIndex].terms) {
            std::int64_t contribution = 0;
            if (!multiplyChecked(coefficient, values.at(variable), contribution) ||
                !addChecked(sum, contribution)) {
                result.rejection = RejectionReason::Overflow;
                return result;
            }
        }
        if (sum != equations[rowIndex].rhs) {
            result.rejection = RejectionReason::TargetResidual;
            return result;
        }
    }
    result.values.assign(values.begin(), values.end());
    return result;
}

}  // namespace cyber::remesh::halflattice
