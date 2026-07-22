#include "cyber/app/undo.hpp"

#include <algorithm>
#include <utility>

namespace cyber::app {

void UndoStack::setBudget(std::size_t budgetBytes) {
    m_budget = budgetBytes;
    enforceBudget();
}

void UndoStack::push(std::unique_ptr<Command> cmd) {
    if (!cmd) {
        return;
    }
    cmd->apply();
    m_used += cmd->estimatedBytes();
    m_undo.push_back(std::move(cmd));
    m_redo.clear();  // a fresh action invalidates the redo branch
    enforceBudget();
}

bool UndoStack::undo() {
    if (m_undo.empty()) {
        return false;
    }
    std::unique_ptr<Command> cmd = std::move(m_undo.back());
    m_undo.pop_back();
    cmd->revert();
    m_used -= cmd->estimatedBytes();
    m_redo.push_back(std::move(cmd));
    return true;
}

bool UndoStack::redo() {
    if (m_redo.empty()) {
        return false;
    }
    std::unique_ptr<Command> cmd = std::move(m_redo.back());
    m_redo.pop_back();
    cmd->apply();
    m_used += cmd->estimatedBytes();
    m_undo.push_back(std::move(cmd));
    enforceBudget();  // redo re-grows m_used; keep it within the same budget as push()
    return true;
}

void UndoStack::clear() {
    m_undo.clear();
    m_redo.clear();
    m_used = 0;
    m_evicted = 0;
}

void UndoStack::enforceBudget() {
    // Evict oldest undoable commands until within budget, but always keep at
    // least the most recent action undoable (a single command larger than the
    // whole budget stays, documented policy).
    while (m_used > m_budget && m_undo.size() > 1) {
        m_used -= m_undo.front()->estimatedBytes();
        m_undo.erase(m_undo.begin());
        ++m_evicted;
    }
}

JournalMetadata UndoStack::metadata() const {
    JournalMetadata meta;
    meta.undo.reserve(m_undo.size());
    for (const auto& cmd : m_undo) {
        meta.undo.push_back(JournalEntry{cmd->label(), cmd->estimatedBytes()});
    }
    meta.redo.reserve(m_redo.size());
    for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it) {
        meta.redo.push_back(JournalEntry{(*it)->label(), (*it)->estimatedBytes()});
    }
    meta.evicted = m_evicted;
    return meta;
}

namespace {

void writeEntries(ByteWriter& w, const std::vector<JournalEntry>& entries) {
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const JournalEntry& e : entries) {
        w.str(e.label);
        w.u64(static_cast<std::uint64_t>(e.bytes));
    }
}

std::vector<JournalEntry> readEntries(ByteReader& r) {
    const std::uint32_t count = r.u32();
    std::vector<JournalEntry> entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        JournalEntry e;
        e.label = r.str();
        e.bytes = static_cast<std::size_t>(r.u64());
        entries.push_back(std::move(e));
    }
    return entries;
}

}  // namespace

void UndoStack::serializeMetadata(ByteWriter& w) const {
    const JournalMetadata meta = metadata();
    writeEntries(w, meta.undo);
    writeEntries(w, meta.redo);
    w.u64(static_cast<std::uint64_t>(meta.evicted));
}

std::optional<JournalMetadata> UndoStack::loadMetadata(ByteReader& r) {
    JournalMetadata meta;
    meta.undo = readEntries(r);
    meta.redo = readEntries(r);
    meta.evicted = static_cast<std::size_t>(r.u64());
    if (!r.ok()) {
        return std::nullopt;
    }
    return meta;
}

}  // namespace cyber::app
