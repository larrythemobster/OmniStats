#pragma once

#include <RmlUi/Core/ElementDocument.h>
#include <memory>
#include "ui/rml/views/PlayerNoteView.hpp"

namespace Rml {
    class Context;
}

class PlayerNotePresenter {
  public:
    PlayerNotePresenter() = default;

    bool Initialize(Rml::Context* context);
    void Shutdown();

    void LoadDocument(Rml::Context* context);
    void CloseDocument();

    void Show();
    void Hide();
    bool IsVisible() const noexcept {
        return m_visible;
    }

    PlayerNoteView& Model() noexcept {
        return m_view;
    }
    const PlayerNoteView& Model() const noexcept {
        return m_view;
    }
    Rml::ElementDocument* Document() const noexcept {
        return m_document;
    }

  private:
    PlayerNoteView m_view;
    Rml::ElementDocument* m_document = nullptr;
    bool m_visible = false;
};
