#pragma once

#include <RmlUi/Core/ElementDocument.h>
#include <memory>
#include "ui/rml/views/InsightsView.hpp"

namespace Rml {
    class Context;
}

class InsightsPresenter {
  public:
    InsightsPresenter() = default;

    bool Initialize(Rml::Context* context);
    void Shutdown();

    void LoadDocument(Rml::Context* context);
    void CloseDocument();

    void Show(InsightsView::Tab tab, bool endedSession);
    void Hide();
    bool IsVisible() const noexcept {
        return m_visible;
    }
    bool ShowsEndedSession() const noexcept {
        return m_showsEndedSession;
    }

    InsightsView& Model() noexcept {
        return m_view;
    }
    const InsightsView& Model() const noexcept {
        return m_view;
    }
    Rml::ElementDocument* Document() const noexcept {
        return m_document;
    }

  private:
    InsightsView m_view;
    Rml::ElementDocument* m_document = nullptr;
    bool m_visible = false;
    bool m_showsEndedSession = false;
};
