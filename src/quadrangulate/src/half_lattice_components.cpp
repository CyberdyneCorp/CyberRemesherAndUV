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
    result.rhs = source.targetHalf;
    result.rejection = source.dependency;

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
    std::map<std::size_t, std::int64_t> values;
    bool progressed = true;
    while (progressed && values.size() < component.variables.size()) {
        progressed = false;
        for (const std::size_t rowIndex : component.equations) {
            const Equation& equation = equations[rowIndex];
            std::int64_t rhs = equation.rhs;
            std::size_t unknown = static_cast<std::size_t>(-1);
            std::int64_t coefficient = 0;
            for (const auto& [variable, term] : equation.terms) {
                const auto known = values.find(variable);
                if (known == values.end()) {
                    if (unknown != static_cast<std::size_t>(-1)) {
                        unknown = static_cast<std::size_t>(-1);
                        break;
                    }
                    unknown = variable;
                    coefficient = term;
                } else {
                    std::int64_t contribution = 0;
                    if (!multiplyChecked(term, known->second, contribution) ||
                        !addChecked(rhs, -contribution)) {
                        result.rejection = RejectionReason::Overflow;
                        return result;
                    }
                }
            }
            if (unknown == static_cast<std::size_t>(-1) || coefficient == 0 ||
                rhs % coefficient != 0) {
                continue;
            }
            const std::int64_t value = rhs / coefficient;
            if (value < minimum || value > maximum) {
                result.rejection = RejectionReason::BoundViolation;
                return result;
            }
            values.emplace(unknown, value);
            progressed = true;
        }
    }
    if (values.size() != component.variables.size()) {
        result.rejection = RejectionReason::Underdetermined;
        return result;
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
