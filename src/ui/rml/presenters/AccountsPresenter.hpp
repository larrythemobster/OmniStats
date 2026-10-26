#pragma once

#include <RmlUi/Core/ElementDocument.h>
#include <memory>
#include "ui/rml/views/AccountsView.hpp"

namespace Rml {
    class Context;
}

class AccountsPresenter {
  public:
    AccountsPresenter() = default;

    bool Initialize(Rml::Context* context);
    void Shutdown();

    void LoadDocument(Rml::Context* context);
    void CloseDocument();

    void Show();
    void Hide();
    bool IsVisible() const noexcept {
        return m_visible;
    }

    AccountsView& Model() noexcept {
        return m_view;
    }
    const AccountsView& Model() const noexcept {
        return m_view;
    }
    Rml::ElementDocument* Document() const noexcept {
        return m_document;
    }

  private:
    AccountsView m_view;
    Rml::ElementDocument* m_document = nullptr;
    bool m_visible = false;
};
