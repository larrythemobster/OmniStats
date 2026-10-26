#include "ui/rml/presenters/InsightsPresenter.hpp"
#include <RmlUi/Core/Context.h>

bool InsightsPresenter::Initialize(Rml::Context* context) {
    if (!context) return false;
    return m_view.Create(context);
}

void InsightsPresenter::Shutdown() {
    CloseDocument();
    m_view.Reset();
}

void InsightsPresenter::LoadDocument(Rml::Context* context) {
    if (!context) return;
    m_document = context->LoadDocument("res://insights.rml");
    if (m_visible && m_document) {
        m_document->Show();
    }
}

void InsightsPresenter::CloseDocument() {
    if (m_document) {
        m_document->Close();
        m_document = nullptr;
    }
}

void InsightsPresenter::Show(InsightsView::Tab tab, bool endedSession) {
    m_showsEndedSession = endedSession;
    m_view.ClearArchivedSelection();
    m_view.SetTab(tab);
    if (m_document && !m_visible) {
        m_document->Show();
        m_visible = true;
    }
}

void InsightsPresenter::Hide() {
    if (m_document && m_visible) {
        m_document->Hide();
        m_visible = false;
    }
}
