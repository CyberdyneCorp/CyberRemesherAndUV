#include "cyber/quadrangulate/topology_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cyber::remesh {
namespace {

bool finite(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// Deterministic, locale-independent float formatting for the debug exports.
// %.9g round-trips a float exactly and never emits a thousands separator.
void appendFloat(std::string& out, float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    out += buf;
}

void appendDouble(std::string& out, double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    out += buf;
}

void appendUint(std::string& out, std::size_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%zu", v);
    out += buf;
}

void appendInt(std::string& out, long long v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lld", v);
    out += buf;
}

void appendVec3(std::string& out, const Vec3& p) {
    out += '[';
    appendFloat(out, p.x);
    out += ',';
    appendFloat(out, p.y);
    out += ',';
    appendFloat(out, p.z);
    out += ']';
}

const char* nodeKindName(LayoutNodeKind k) {
    switch (k) {
        case LayoutNodeKind::Singularity:
            return "singularity";
        case LayoutNodeKind::BoundaryCorner:
            return "boundary-corner";
        case LayoutNodeKind::FeatureCorner:
            return "feature-corner";
        case LayoutNodeKind::TJunction:
            return "t-junction";
        case LayoutNodeKind::GuideAnchor:
            return "guide-anchor";
        case LayoutNodeKind::SymmetryAnchor:
            return "symmetry-anchor";
        case LayoutNodeKind::Regular:
            return "regular";
    }
    return "unknown";
}

const char* arcKindName(LayoutArcKind k) {
    switch (k) {
        case LayoutArcKind::Separatrix:
            return "separatrix";
        case LayoutArcKind::Feature:
            return "feature";
        case LayoutArcKind::Boundary:
            return "boundary";
        case LayoutArcKind::Guide:
            return "guide";
        case LayoutArcKind::Symmetry:
            return "symmetry";
    }
    return "unknown";
}

// Walks a patch's boundary and reports whether it closes. `arcs` must already
// have been checked for valid endpoints. The first arc's orientation is chosen
// by which of its endpoints the second arc touches, preferring `end` so a bigon
// resolves the same way every run.
bool boundaryWalkCloses(const std::vector<LayoutArcId>& flat, const std::vector<LayoutArc>& arcs) {
    if (flat.empty()) {
        return false;
    }
    const LayoutArc& first = arcs[flat[0]];
    if (flat.size() == 1) {
        return first.begin == first.end;
    }
    const LayoutArc& second = arcs[flat[1]];
    const bool endTouches = first.end == second.begin || first.end == second.end;
    const bool beginTouches = first.begin == second.begin || first.begin == second.end;
    if (!endTouches && !beginTouches) {
        return false;
    }
    const LayoutNodeId start = endTouches ? first.begin : first.end;
    LayoutNodeId current = endTouches ? first.end : first.begin;
    for (std::size_t i = 1; i < flat.size(); ++i) {
        const LayoutArc& arc = arcs[flat[i]];
        if (arc.begin == current) {
            current = arc.end;
        } else if (arc.end == current) {
            current = arc.begin;
        } else {
            return false;
        }
    }
    return current == start;
}

}  // namespace

std::size_t LayoutPatch::arcCount() const {
    std::size_t n = 0;
    for (const auto& side : sides) {
        n += side.size();
    }
    return n;
}

std::size_t TopologyLayout::singularityCount() const {
    return static_cast<std::size_t>(
        std::count_if(nodes.begin(), nodes.end(),
                      [](const LayoutNode& n) { return n.kind == LayoutNodeKind::Singularity; }));
}

std::size_t TopologyLayout::nonQuadPatchCount() const {
    return static_cast<std::size_t>(std::count_if(
        patches.begin(), patches.end(), [](const LayoutPatch& p) { return !p.quadLike(); }));
}

LayoutStats TopologyLayout::stats() const {
    LayoutStats s;
    s.nodes = nodes.size();
    s.arcs = arcs.size();
    s.patches = patches.size();
    for (const LayoutNode& n : nodes) {
        if (n.kind == LayoutNodeKind::Singularity) {
            ++s.singularities;
            s.totalIndex += n.singularityIndex;
        } else if (n.kind == LayoutNodeKind::TJunction) {
            ++s.tJunctions;
        }
    }
    for (const LayoutArc& a : arcs) {
        if (a.kind == LayoutArcKind::Boundary) {
            ++s.boundaryArcs;
        } else if (a.kind == LayoutArcKind::Feature) {
            ++s.featureArcs;
        }
        if (a.excluded) {
            ++s.excludedArcs;
        }
    }
    s.nonQuadPatches = nonQuadPatchCount();
    s.nonClosingPatches = nonClosingPatches.size();
    return s;
}

// Each checker returns the violated invariant, or an empty string when clean.
// They run in a fixed order and stop at the first violation, so the reported
// reason is deterministic for a given layout.
namespace {

std::string at(const char* what, std::size_t i) {
    std::string s = what;
    s += " #";
    appendUint(s, i);
    return s;
}

std::string checkNodes(const TopologyLayout& layout, std::size_t faceCount) {
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        const LayoutNode& n = layout.nodes[i];
        if (n.id != i) {
            return at("node id is not its index:", i);
        }
        if (!finite(n.position)) {
            return at("non-finite node position:", i);
        }
        if (!n.face.valid()) {
            return at("node has no source face:", i);
        }
        if (faceCount != 0 && n.face.value >= faceCount) {
            return at("node source face out of range:", i);
        }
        const bool singular = n.kind == LayoutNodeKind::Singularity;
        if (singular && n.singularityIndex == 0) {
            return at("singularity node with zero index:", i);
        }
        if (!singular && n.singularityIndex != 0) {
            return at("non-singularity node with a nonzero index:", i);
        }
    }
    return {};
}

std::string checkArcSamples(const TopologyLayout& layout, const LayoutArc& a, std::size_t i,
                            std::size_t faceCount) {
    for (const LayoutSample& s : a.samples) {
        if (!finite(s.position)) {
            return at("non-finite arc sample:", i);
        }
        if (!s.face.valid() || (faceCount != 0 && s.face.value >= faceCount)) {
            return at("arc sample face out of range:", i);
        }
    }
    if (a.samples.empty()) {
        return {};
    }
    // The polyline is welded to its endpoints, so arcs meeting at a node share
    // that node's exact position.
    if (!(a.samples.front().position == layout.nodes[a.begin].position)) {
        return at("arc does not start at its begin node:", i);
    }
    if (!(a.samples.back().position == layout.nodes[a.end].position)) {
        return at("arc does not end at its end node:", i);
    }
    return {};
}

std::string checkArcs(const TopologyLayout& layout, std::size_t faceCount) {
    const std::size_t nodeCount = layout.nodes.size();
    for (std::size_t i = 0; i < layout.arcs.size(); ++i) {
        const LayoutArc& a = layout.arcs[i];
        if (a.id != i) {
            return at("arc id is not its index:", i);
        }
        if (a.begin >= nodeCount || a.end >= nodeCount) {
            return at("arc endpoint is not a node:", i);
        }
        if (a.begin == a.end && a.samples.size() < 3) {
            return at("closed arc without an interior:", i);
        }
        if (!std::isfinite(a.desiredLength) || a.desiredLength < 0.0) {
            return at("arc length is not a finite non-negative number:", i);
        }
        std::string err = checkArcSamples(layout, a, i, faceCount);
        if (!err.empty()) {
            return err;
        }
    }
    return {};
}

// Flattens one patch's sides into boundary order, rejecting a corrupt side
// list on the way.
std::string flattenPatch(const TopologyLayout& layout, const LayoutPatch& p, std::size_t i,
                         std::vector<LayoutArcId>& flat) {
    if (p.id != i) {
        return at("patch id is not its index:", i);
    }
    if (p.sides.empty()) {
        return at("patch without sides:", i);
    }
    for (const auto& side : p.sides) {
        if (side.empty()) {
            return at("patch has an empty side:", i);
        }
        for (const LayoutArcId a : side) {
            if (a >= layout.arcs.size()) {
                return at("patch references a missing arc:", i);
            }
            flat.push_back(a);
        }
    }
    // An arc may appear at most twice in one patch (a slit the patch wraps on
    // both sides); three or more is a corrupted walk.
    std::vector<LayoutArcId> sorted = flat;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t k = 2; k < sorted.size(); ++k) {
        if (sorted[k] == sorted[k - 2]) {
            return at("patch repeats an arc more than twice:", i);
        }
    }
    return {};
}

// Counts how many patches bound each arc, and records the patches whose
// boundary walk does not close. A non-closing patch is a LOCAL failure: it is
// contained (its arcs are marked, and deliberately not counted as incident) so
// the incidence rule still holds on the sound patches around it.
std::string checkPatches(const TopologyLayout& layout, std::vector<std::size_t>& incident,
                         std::vector<char>& touchedByBadPatch,
                         std::vector<LayoutPatchId>& nonClosing) {
    for (std::size_t i = 0; i < layout.patches.size(); ++i) {
        std::vector<LayoutArcId> flat;
        std::string err = flattenPatch(layout, layout.patches[i], i, flat);
        if (!err.empty()) {
            return err;
        }
        if (!boundaryWalkCloses(flat, layout.arcs)) {
            nonClosing.push_back(static_cast<LayoutPatchId>(i));
            for (const LayoutArcId a : flat) {
                touchedByBadPatch[a] = 1;
            }
            continue;
        }
        for (const LayoutArcId a : flat) {
            ++incident[a];
        }
    }
    return {};
}

std::string checkIncidence(const TopologyLayout& layout, const std::vector<std::size_t>& incident,
                           const std::vector<char>& touchedByBadPatch) {
    if (layout.patches.empty()) {
        return {};  // a layout built without patches has no incidence to check
    }
    for (std::size_t i = 0; i < layout.arcs.size(); ++i) {
        const LayoutArc& a = layout.arcs[i];
        // Excluded arcs, and arcs whose only patch was contained above, are not
        // expected to bound anything.
        if (a.excluded || touchedByBadPatch[i]) {
            continue;
        }
        if (incident[i] == 0) {
            return at("arc bounds no patch but is not excluded:", i);
        }
        if (incident[i] > 2) {
            return at("arc bounds more than two patches:", i);
        }
        if (a.kind == LayoutArcKind::Boundary && incident[i] != 1) {
            return at("boundary arc does not bound exactly one patch:", i);
        }
    }
    return {};
}

}  // namespace

LayoutValidation validateTopologyLayout(const TopologyLayout& layout, std::size_t faceCount) {
    LayoutValidation out;
    std::vector<std::size_t> incident(layout.arcs.size(), 0);
    std::vector<char> touchedByBadPatch(layout.arcs.size(), 0);

    out.error = checkNodes(layout, faceCount);
    if (out.error.empty()) {
        out.error = checkArcs(layout, faceCount);
    }
    if (out.error.empty()) {
        out.error = checkPatches(layout, incident, touchedByBadPatch, out.nonClosingPatches);
    }
    if (out.error.empty()) {
        out.error = checkIncidence(layout, incident, touchedByBadPatch);
    }
    if (!out.error.empty()) {
        out.nonClosingPatches.clear();  // a corrupt graph has no meaningful local report
        return out;
    }
    out.ok = true;
    return out;
}

namespace {

void appendJsonString(std::string& out, const std::string& text) {
    out += '"';
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
        }
        out += ch;
    }
    out += '"';
}

void appendStatsJson(std::string& out, const LayoutStats& s) {
    const auto kv = [&out](const char* key, std::size_t v) {
        out += '"';
        out += key;
        out += "\":";
        appendUint(out, v);
        out += ',';
    };
    out += '{';
    kv("nodes", s.nodes);
    kv("arcs", s.arcs);
    kv("patches", s.patches);
    kv("singularities", s.singularities);
    kv("tJunctions", s.tJunctions);
    kv("boundaryArcs", s.boundaryArcs);
    kv("featureArcs", s.featureArcs);
    kv("excludedArcs", s.excludedArcs);
    kv("nonQuadPatches", s.nonQuadPatches);
    kv("nonClosingPatches", s.nonClosingPatches);
    out += "\"totalIndex\":";
    appendInt(out, s.totalIndex);
    out += '}';
}

void appendNodeJson(std::string& out, const LayoutNode& n) {
    out += "{\"id\":";
    appendUint(out, n.id);
    out += ",\"kind\":\"";
    out += nodeKindName(n.kind);
    out += "\",\"index\":";
    appendInt(out, n.singularityIndex);
    out += ",\"face\":";
    appendInt(out, n.face.valid() ? static_cast<long long>(n.face.value) : -1);
    out += ",\"vertex\":";
    appendInt(out, n.vertex.valid() ? static_cast<long long>(n.vertex.value) : -1);
    out += ",\"locked\":";
    out += n.locked ? "true" : "false";
    out += ",\"p\":";
    appendVec3(out, n.position);
    out += '}';
}

void appendArcJson(std::string& out, const LayoutArc& a) {
    out += "{\"id\":";
    appendUint(out, a.id);
    out += ",\"kind\":\"";
    out += arcKindName(a.kind);
    out += "\",\"begin\":";
    appendUint(out, a.begin);
    out += ",\"end\":";
    appendUint(out, a.end);
    out += ",\"desiredLength\":";
    appendDouble(out, a.desiredLength);
    out += ",\"quantizedLength\":";
    appendInt(out, a.quantizedLength);
    out += ",\"samples\":";
    appendUint(out, a.samples.size());
    out += ",\"excluded\":";
    out += a.excluded ? "true" : "false";
    out += ",\"locked\":";
    out += a.locked ? "true" : "false";
    out += '}';
}

void appendPatchJson(std::string& out, const LayoutPatch& p) {
    out += "{\"id\":";
    appendUint(out, p.id);
    out += ",\"uCount\":";
    appendInt(out, p.uCount);
    out += ",\"vCount\":";
    appendInt(out, p.vCount);
    out += ",\"sides\":[";
    for (std::size_t k = 0; k < p.sides.size(); ++k) {
        out += k == 0 ? "[" : ",[";
        for (std::size_t j = 0; j < p.sides[k].size(); ++j) {
            if (j != 0) {
                out += ',';
            }
            appendUint(out, p.sides[k][j]);
        }
        out += ']';
    }
    out += "]}";
}

// One JSON array of `items`, each rendered by `emit`, with the file's
// indentation.
template <typename T, typename Emit>
void appendArray(std::string& out, const char* name, const std::vector<T>& items, Emit emit) {
    out += ",\n  \"";
    out += name;
    out += "\": [";
    for (std::size_t i = 0; i < items.size(); ++i) {
        out += i == 0 ? "\n    " : ",\n    ";
        emit(out, items[i]);
    }
    out += "\n  ]";
}

}  // namespace

std::string layoutToJson(const TopologyLayout& layout) {
    std::string out;
    out.reserve(layout.arcs.size() * 96 + layout.nodes.size() * 96 + 512);
    out += "{\n  \"stats\": ";
    appendStatsJson(out, layout.stats());
    out += ",\n  \"valid\": ";
    out += layout.valid ? "true" : "false";
    out += ",\n  \"invalidReason\": ";
    appendJsonString(out, layout.invalidReason);
    out += ",\n  \"nonClosingPatchIds\": [";
    for (std::size_t i = 0; i < layout.nonClosingPatches.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        appendUint(out, layout.nonClosingPatches[i]);
    }
    out += ']';
    appendArray(out, "nodes", layout.nodes, appendNodeJson);
    appendArray(out, "arcs", layout.arcs, appendArcJson);
    appendArray(out, "patches", layout.patches, appendPatchJson);
    out += "\n}\n";
    return out;
}

std::string layoutToObj(const TopologyLayout& layout) {
    std::string out;
    out.reserve(layout.arcs.size() * 96 + 256);
    out += "# cyber topology layout\n";
    out += "# nodes ";
    appendUint(out, layout.nodes.size());
    out += " arcs ";
    appendUint(out, layout.arcs.size());
    out += " patches ";
    appendUint(out, layout.patches.size());
    out += '\n';

    const auto vertexLine = [&out](const Vec3& p) {
        out += "v ";
        appendFloat(out, p.x);
        out += ' ';
        appendFloat(out, p.y);
        out += ' ';
        appendFloat(out, p.z);
        out += '\n';
    };
    // Node vertices first, so a node's OBJ index is its id + 1.
    for (const LayoutNode& n : layout.nodes) {
        vertexLine(n.position);
    }
    std::size_t next = layout.nodes.size() + 1;
    std::string lines;
    for (const LayoutArc& a : layout.arcs) {
        if (a.samples.size() < 2) {
            // No traced geometry: fall back to the straight node-to-node chord.
            lines += "l ";
            appendUint(lines, static_cast<std::size_t>(a.begin) + 1);
            lines += ' ';
            appendUint(lines, static_cast<std::size_t>(a.end) + 1);
            lines += '\n';
            continue;
        }
        // Interior samples get their own vertices; the endpoints reuse the
        // node vertices so arcs stay welded at the nodes in a viewer.
        const std::size_t interior = a.samples.size() - 2;
        lines += "l ";
        appendUint(lines, static_cast<std::size_t>(a.begin) + 1);
        for (std::size_t k = 0; k < interior; ++k) {
            vertexLine(a.samples[k + 1].position);
            lines += ' ';
            appendUint(lines, next++);
        }
        lines += ' ';
        appendUint(lines, static_cast<std::size_t>(a.end) + 1);
        lines += '\n';
    }
    out += lines;
    return out;
}

}  // namespace cyber::remesh
