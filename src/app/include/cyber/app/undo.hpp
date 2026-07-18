#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cyber/app/serial.hpp"

// Command-journal undo/redo with a memory budget (application-shell spec,
// task 8.2). Every edit is a Command carrying apply/revert and an estimated
// byte cost. The journal keeps the newest history within a byte budget: once
// the undo stack exceeds the budget the oldest (deepest) commands are evicted
// and become permanent, while the most recent action is always kept undoable.
// A lightweight metadata snapshot (labels + costs + cursor) is persisted
// alongside the document autosave for crash-recovery UI; concrete command
// payload replay is the responsibility of concrete command types.
namespace cyber::app {

class Command {
public:
    virtual ~Command() = default;
    virtual void apply() = 0;
    virtual void revert() = 0;
    [[nodiscard]] virtual std::size_t estimatedBytes() const = 0;
    [[nodiscard]] virtual std::string label() const { return {}; }
};

struct JournalEntry {
    std::string label;
    std::size_t bytes = 0;

    friend bool operator==(const JournalEntry&, const JournalEntry&) = default;
};

// Persisted history summary. `undo` is ordered oldest..newest; `redo` is
// ordered from the next command to redo outward.
struct JournalMetadata {
    std::vector<JournalEntry> undo;
    std::vector<JournalEntry> redo;
    std::size_t evicted = 0;

    friend bool operator==(const JournalMetadata&, const JournalMetadata&) = default;
};

class UndoStack {
public:
    explicit UndoStack(std::size_t budgetBytes) : m_budget(budgetBytes) {}

    void setBudget(std::size_t budgetBytes);
    [[nodiscard]] std::size_t budget() const { return m_budget; }
    [[nodiscard]] std::size_t usedBytes() const { return m_used; }

    // Applies `cmd`, pushes it as the newest undoable action, discards any
    // redo history, then enforces the budget.
    void push(std::unique_ptr<Command> cmd);

    [[nodiscard]] bool canUndo() const { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const { return !m_redo.empty(); }
    bool undo();  // reverts the newest action; false if none
    bool redo();  // re-applies the last undone action; false if none

    [[nodiscard]] std::size_t undoDepth() const { return m_undo.size(); }
    [[nodiscard]] std::size_t redoDepth() const { return m_redo.size(); }
    [[nodiscard]] std::size_t evictedCount() const { return m_evicted; }

    void clear();

    // ---- persistence alongside autosave -------------------------------
    [[nodiscard]] JournalMetadata metadata() const;
    void serializeMetadata(ByteWriter& w) const;
    [[nodiscard]] static std::optional<JournalMetadata> loadMetadata(ByteReader& r);

private:
    void enforceBudget();

    std::vector<std::unique_ptr<Command>> m_undo;  // oldest..newest
    std::vector<std::unique_ptr<Command>> m_redo;  // newest-undone..outward
    std::size_t m_budget;
    std::size_t m_used = 0;
    std::size_t m_evicted = 0;
};

}  // namespace cyber::app
