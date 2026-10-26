#include "ui/rml/presenters/PlayerNotePresenter.hpp"
#include <RmlUi/Core/Context.h>

bool PlayerNotePresenter::Initialize(Rml::Context* context) {
    if (!context) return false;
    return m_view.Create(context);
}

void PlayerNotePresenter::Shutdown() {
    CloseDocument();
    m_view.Reset();
}

void PlayerNotePresenter::LoadDocument(Rml::Context* context) {
    if (!context) return;
    m_document = context->LoadDocument("res://player_note.rml");
    if (m_visible && m_document) {
        m_document->Show();
    }
}

void PlayerNotePresenter::CloseDocument() {
    if (m_document) {
        m_document->Close();
        m_document = nullptr;
    }
}

void PlayerNotePresenter::Show() {
    if (m_document && !m_visible) {
        m_document->Show();
        m_visible = true;
    }
}

void PlayerNotePresenter::Hide() {
    if (m_document && m_visible) {
        m_document->Hide();
        m_visible = false;
    }
}
