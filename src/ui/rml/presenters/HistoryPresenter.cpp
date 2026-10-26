#include "ui/rml/presenters/HistoryPresenter.hpp"
#include <RmlUi/Core/Context.h>

bool HistoryPresenter::Initialize(Rml::Context* context) {
    if (!context) return false;
    return m_view.Create(context);
}

void HistoryPresenter::Shutdown() {
    CloseDocument();
    m_view.Reset();
}

void HistoryPresenter::LoadDocument(Rml::Context* context) {
    if (!context) return;
    m_document = context->LoadDocument("res://history.rml");
    if (m_visible && m_document) {
        m_document->Show();
    }
}

void HistoryPresenter::CloseDocument() {
    if (m_document) {
        m_document->Close();
        m_document = nullptr;
    }
}

void HistoryPresenter::Show() {
    if (m_document && !m_visible) {
        m_document->Show();
        m_visible = true;
    }
}

void HistoryPresenter::Hide() {
    if (m_document && m_visible) {
        m_document->Hide();
        m_visible = false;
    }
}
