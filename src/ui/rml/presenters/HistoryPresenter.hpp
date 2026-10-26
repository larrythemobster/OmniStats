#pragma once

#include <RmlUi/Core/ElementDocument.h>
#include <memory>
#include <string>
#include "ui/rml/views/HistoryView.hpp"

namespace Rml {
    class Context;
}
class DatabaseManager;

class HistoryPresenter {
  public:
    HistoryPresenter() = default;

    bool Initialize(Rml::Context* context);
    void Shutdown();

    void LoadDocument(Rml::Context* context);
    void CloseDocument();

    void Show();
    void Hide();
    bool IsVisible() const noexcept {
        return m_visible;
    }

    HistoryView& Model() noexcept {
        return m_view;
    }
    const HistoryView& Model() const noexcept {
        return m_view;
    }
    Rml::ElementDocument* Document() const noexcept {
        return m_document;
    }

  private:
    HistoryView m_view;
    Rml::ElementDocument* m_document = nullptr;
    bool m_visible = false;
};
