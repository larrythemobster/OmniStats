#include "ui/rml/controller/ModalDocument.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <utility>

void ModalDocument::Load(Rml::Context& context) {
    m_document = context.LoadDocument(m_resource);
    if (m_visible && m_document) m_document->Show();
}

void ModalDocument::Unload() {
    if (auto* document = std::exchange(m_document, nullptr)) document->Close();
}

void ModalDocument::Show() {
    if (!m_document || m_visible) return;
    m_document->Show();
    m_visible = true;
}

void ModalDocument::Hide() {
    if (m_document) m_document->Hide();
    m_visible = false;
}

void ModalDocument::Raise() const {
    if (m_visible && m_document) m_document->PullToFront();
}
