#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Types.h>
#include <vector>

// Custom RmlUi element used only for the MMR graph polyline. The normal DOM
// handles labels, points, controls, and the dashed baseline. Rendering the
// connecting segments here lets us use the element's resolved pixel size, so
// the line remains geometrically correct at every card width/aspect ratio.
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
