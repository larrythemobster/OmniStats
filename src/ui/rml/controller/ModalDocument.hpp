#pragma once

namespace Rml {
    class Context;
    class ElementDocument;
}

class ModalDocument {
  public:
    explicit ModalDocument(const char* resource) : m_resource(resource) {}
    void Load(Rml::Context& context);
    void Unload();
    void Show();
    void Hide();
    void Raise() const;
    bool IsVisible() const noexcept {
        return m_visible;
    }
    Rml::ElementDocument* Document() const noexcept {
        return m_document;
    }

  private:
    const char* m_resource;
    Rml::ElementDocument* m_document = nullptr;
    bool m_visible = false;
};

template <class Model>
struct ModalView : ModalDocument {
    using ModalDocument::ModalDocument;
    Model model;
};
