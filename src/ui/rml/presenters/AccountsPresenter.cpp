#include "ui/rml/presenters/AccountsPresenter.hpp"
#include <RmlUi/Core/Context.h>

bool AccountsPresenter::Initialize(Rml::Context* context) {
    if (!context) return false;
    return m_view.Create(context);
}

void AccountsPresenter::Shutdown() {
    CloseDocument();
    m_view.Reset();
}

void AccountsPresenter::LoadDocument(Rml::Context* context) {
    if (!context) return;
    m_document = context->LoadDocument("res://accounts.rml");
    if (m_visible && m_document) {
        m_document->Show();
    }
}

void AccountsPresenter::CloseDocument() {
    if (m_document) {
        m_document->Close();
        m_document = nullptr;
    }
}

void AccountsPresenter::Show() {
    if (m_document && !m_visible) {
        m_document->Show();
        m_visible = true;
    }
}

void AccountsPresenter::Hide() {
    if (m_document && m_visible) {
        m_document->Hide();
        m_visible = false;
    }
}
