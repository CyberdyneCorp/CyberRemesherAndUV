#pragma once

#include "cyber/app/document.hpp"
#include "cyber/app/undo.hpp"
#include "cyber/core/partial_retopology.hpp"

namespace cyber::app {

// Runs the core transaction first, then records one reversible replacement of
// Document::editMesh. A rejected request never touches the document or stack.
[[nodiscard]] remesh::PartialRetopologyResult applyPartialRetopology(
    Document& document, UndoStack& undo, const remesh::PartialRetopologyRequest& request);

}  // namespace cyber::app
