#pragma once

#include <RmlUi/Core/ElementDocument.h>
#include <memory>
#include "ui/rml/views/OnboardingView.hpp"

namespace Rml {
    class Context;
}

class OnboardingPresenter {
  public:
    OnboardingPresenter() = default;

    bool Initialize(Rml::Context* context);
    void Shutdown();

    void LoadDocument(Rml::Context* context);
    void CloseDocument();

    void Show();
    void Hide();
    bool IsVisible() const noexcept {
        return m_visible;
    }

    OnboardingView& Model() noexcept {
        return m_view;
    }
    const OnboardingView& Model() const noexcept {
        return m_view;
    }
    Rml::ElementDocument* Document() const noexcept {
        return m_document;
    }

  private:
    OnboardingView m_view;
    Rml::ElementDocument* m_document = nullptr;
    bool m_visible = false;
};
