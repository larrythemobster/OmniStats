#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Types.h>
#include <vector>

// Renders the MMR graph polyline and dashed baseline at the resolved element size.
class RmlMmrGraphLines final : public Rml::Element {
  public:
    explicit RmlMmrGraphLines(const Rml::String& tag);

  protected:
    void OnRender() override;
    void OnResize() override;
    void OnAttributeChange(const Rml::ElementAttributes& changedAttributes) override;
    void OnPropertyChange(const Rml::PropertyIdSet& changedProperties) override;

  private:
    void ParsePoints();
    void RebuildGeometry();

    std::vector<Rml::Vector2f> m_points;
    Rml::Geometry m_geometry;
};
