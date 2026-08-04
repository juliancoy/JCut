#include "editor_document_core.h"

namespace jcut {

EditorClip::EditorClip() = default;
EditorClip::EditorClip(const EditorClip&) = default;
EditorClip::EditorClip(EditorClip&&) noexcept = default;
EditorClip& EditorClip::operator=(const EditorClip&) = default;
EditorClip& EditorClip::operator=(EditorClip&&) noexcept = default;
EditorClip::~EditorClip() = default;

EditorDocumentCore::EditorDocumentCore() = default;
EditorDocumentCore::EditorDocumentCore(const EditorDocumentCore&) = default;
EditorDocumentCore::EditorDocumentCore(EditorDocumentCore&&) noexcept = default;
EditorDocumentCore& EditorDocumentCore::operator=(const EditorDocumentCore&) =
    default;
EditorDocumentCore& EditorDocumentCore::operator=(EditorDocumentCore&&) noexcept =
    default;
EditorDocumentCore::~EditorDocumentCore() = default;

} // namespace jcut
