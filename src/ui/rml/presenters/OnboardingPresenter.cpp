#include "ui/rml/presenters/OnboardingPresenter.hpp"
#include <RmlUi/Core/Context.h>

bool OnboardingPresenter::Initialize(Rml::Context* context) {
    if (!context) return false;
    return m_view.Create(context);
}

void OnboardingPresenter::Shutdown() {
    CloseDocument();
    m_view.Reset();
}

void OnboardingPresenter::LoadDocument(Rml::Context* context) {
    if (!context) return;
    m_document = context->LoadDocument("res://onboarding.rml");
    if (m_visible && m_document) {
        m_document->Show();
    }
}

void OnboardingPresenter::CloseDocument() {
    if (m_document) {
        m_document->Close();
        m_document = nullptr;
    }
}

void OnboardingPresenter::Show() {
    if (m_document && !m_visible) {
        m_document->Show();
        m_visible = true;
    }
}

void OnboardingPresenter::Hide() {
    if (m_document && m_visible) {
        m_document->Hide();
        m_visible = false;
    }
}
