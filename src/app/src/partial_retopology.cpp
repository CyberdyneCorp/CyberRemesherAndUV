#include "cyber/app/partial_retopology.hpp"

#include <memory>
#include <utility>

namespace cyber::app {
namespace {

class ReplaceEditMeshCommand final : public Command {
public:
    ReplaceEditMeshCommand(Document& document, Mesh before, Mesh after)
        : m_document(document), m_before(std::move(before)), m_after(std::move(after)) {}

    void apply() override { replace(m_after); }
    void revert() override { replace(m_before); }
    [[nodiscard]] std::size_t estimatedBytes() const override {
        return m_before.ownedBufferBytes() + m_after.ownedBufferBytes();
    }
    [[nodiscard]] std::string label() const override { return "partial retopology"; }

private:
    void replace(const Mesh& mesh) {
        m_document.editMesh = mesh;
        m_document.markDirty();
    }

    Document& m_document;
    Mesh m_before;
    Mesh m_after;
};

}  // namespace

remesh::PartialRetopologyResult applyPartialRetopology(
    Document& document, UndoStack& undo, const remesh::PartialRetopologyRequest& request) {
    remesh::PartialRetopologyResult result =
        remesh::partialRetopologize(document.editMesh, request);
    if (result.status == remesh::PartialRetopologyStatus::Applied) {
        undo.push(
            std::make_unique<ReplaceEditMeshCommand>(document, document.editMesh, result.mesh));
    }
    return result;
}

}  // namespace cyber::app
