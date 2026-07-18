#pragma once

// UNVERIFIED: desktop shell wiring (application-shell spec, tasks 8.4-8.6,
// "best-effort"). This header describes the window / panel / shortcut-map
// surface a native desktop shell (apps/desktop, owned elsewhere) implements
// against the real library layers (Document, UndoStack, input). It pulls in no
// GUI toolkit: the abstract interfaces are pure declarations and the ShortcutMap
// is real, testable logic, so this header compiles under the strict flags on
// headless CI. The concrete window/menu/event-loop backing needs a real
// desktop toolkit and windowing session and cannot be exercised here.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cyber/app/document.hpp"
#include "cyber/app/input.hpp"
#include "cyber/app/undo.hpp"

namespace cyber::app {

// A keyboard shortcut: a chorded-modifier mask plus a virtual key code.
struct Shortcut {
    std::uint32_t modifiers = 0u;  // bitwise OR of Modifier values
    std::uint32_t keyCode = 0u;

    friend bool operator==(const Shortcut&, const Shortcut&) = default;
};

// Real logic: resolves a (modifier chord, key) event to a named command. The
// shell binds these names to Document/UndoStack actions. Testable off-device.
class ShortcutMap {
public:
    void bind(Shortcut shortcut, std::string command) {
        m_bindings.push_back(Binding{shortcut, std::move(command)});
    }

    // Returns the bound command for an exact chord+key match, or empty. Later
    // bindings win, so a rebind overrides an earlier default.
    [[nodiscard]] std::string resolve(std::uint32_t modifiers, std::uint32_t keyCode) const {
        for (auto it = m_bindings.rbegin(); it != m_bindings.rend(); ++it) {
            if (it->shortcut.modifiers == modifiers && it->shortcut.keyCode == keyCode) {
                return it->command;
            }
        }
        return {};
    }

    [[nodiscard]] std::size_t size() const { return m_bindings.size(); }

private:
    struct Binding {
        Shortcut shortcut;
        std::string command;
    };
    std::vector<Binding> m_bindings;
};

// A dockable panel surface the shell hosts (viewport, parameters, bake, etc.).
class IPanel {
public:
    virtual ~IPanel() = default;
    [[nodiscard]] virtual std::string title() const = 0;
    virtual void onDocumentChanged(const Document& document) = 0;
    virtual void onDraw() = 0;
};

// The top-level application window / event surface a desktop shell provides.
class IShellWindow {
public:
    virtual ~IShellWindow() = default;
    virtual void setTitle(const std::string& title) = 0;
    virtual void addPanel(IPanel& panel) = 0;
    virtual void requestRedraw() = 0;
    // Delivered from the platform event loop into the shared input layer.
    virtual void onPointerEvent(Vec2 position, float pressure, Time time) = 0;
    virtual void onKeyEvent(std::uint32_t modifiers, std::uint32_t keyCode) = 0;
};

// The host binds the shared library state to a concrete window. A desktop shell
// derives a window, feeds platform events into the input recognizers, routes
// resolved shortcuts to commands, and drives autosave on a timer.
struct ShellHost {
    Document* document = nullptr;
    UndoStack* undo = nullptr;
    ShortcutMap shortcuts;
    ModifierState modifiers;
    HoverState hover;
    StrokeCapture stroke;
};

}  // namespace cyber::app
