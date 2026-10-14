#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/RenderManager.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace {
    void AddLineQuad(Rml::Mesh& mesh, Rml::Vector2f a, Rml::Vector2f b, float thickness, Rml::ColourbPremultiplied colour) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 0.001f) || !std::isfinite(length)) return;

        const float half = std::max(thickness, 1.0f) * 0.5f;
        const Rml::Vector2f normal{-dy / length * half, dx / length * half};
        const int first = static_cast<int>(mesh.vertices.size());

        auto vertex = [&](Rml::Vector2f position) {
            Rml::Vertex v;
            v.position = position;
            v.colour = colour;
            v.tex_coord = {};
            mesh.vertices.push_back(v);
        };

        vertex(a + normal);
        vertex(a - normal);
        vertex(b - normal);
        vertex(b + normal);
        mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }

    // Area under one segment down to `bottom`, fading from `top` colour at the line
    // to transparent at the bottom.
    void AddFillQuad(Rml::Mesh& mesh, Rml::Vector2f a, Rml::Vector2f b, float bottom, Rml::ColourbPremultiplied top) {
        if (!(b.x > a.x)) return;
        const int first = static_cast<int>(mesh.vertices.size());
        auto vertex = [&](Rml::Vector2f position, Rml::ColourbPremultiplied colour) {
            Rml::Vertex v;
            v.position = position;
            v.colour = colour;
            v.tex_coord = {};
            mesh.vertices.push_back(v);
        };
        const Rml::ColourbPremultiplied clear(0, 0, 0, 0);
        vertex(a, top);
        vertex(b, top);
        vertex({b.x, bottom}, clear);
        vertex({a.x, bottom}, clear);
        mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }
} // namespace

RmlMmrGraphLines::RmlMmrGraphLines(const Rml::String& tag) : Rml::Element(tag) {}

void RmlMmrGraphLines::OnRender() {
    if (m_geometry) m_geometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content));
}

void RmlMmrGraphLines::OnResize() {
    Rml::Element::OnResize();
    RebuildGeometry();
}

void RmlMmrGraphLines::OnAttributeChange(const Rml::ElementAttributes& changedAttributes) {
    Rml::Element::OnAttributeChange(changedAttributes);
    if (changedAttributes.find("points") != changedAttributes.end() || changedAttributes.find("fill-to") != changedAttributes.end()) {
        ParsePoints();
        RebuildGeometry();
    }
}

void RmlMmrGraphLines::OnPropertyChange(const Rml::PropertyIdSet& changedProperties) {
    Rml::Element::OnPropertyChange(changedProperties);
    // The graph line color and opacity are theme-driven RCSS properties. Rebuild
    // when computed styling changes so live theme edits affect cached geometry.
    RebuildGeometry();
}

void RmlMmrGraphLines::ParsePoints() {
    m_points.clear();
    const std::string encoded = GetAttribute<Rml::String>("points", "");
    const char* cursor = encoded.c_str();

    while (*cursor) {
        char* end = nullptr;
        const float x = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(x)) break;
        cursor = end;
        if (*cursor != ',') break;
        ++cursor;

        const float y = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(y)) break;
        cursor = end;
        m_points.push_back({std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f)});

        if (*cursor == ';')
            ++cursor;
        else if (*cursor != '\0')
            break;
    }
}

void RmlMmrGraphLines::RebuildGeometry() {
    m_geometry = {};
    if (m_points.size() < 2 || !GetRenderManager()) return;

    const Rml::Vector2f size = GetBox().GetSize();
    if (!(size.x > 0.0f) || !(size.y > 0.0f) || !std::isfinite(size.x) || !std::isfinite(size.y)) return;

    const float dpRatio = GetContext() ? GetContext()->GetDensityIndependentPixelRatio() : 1.0f;
    const float thickness = 3.5f * std::max(dpRatio, 0.5f);
    const auto& computed = GetComputedValues();
    const Rml::ColourbPremultiplied colour = computed.color().ToPremultiplied(computed.opacity() * 0.95f);
    const Rml::ColourbPremultiplied fillColour = computed.color().ToPremultiplied(computed.opacity() * 0.22f);
    const float fillTo = GetAttribute<float>("fill-to", -1.0f);

    Rml::Mesh mesh;
    mesh.vertices.reserve((m_points.size() - 1) * 8);
    mesh.indices.reserve((m_points.size() - 1) * 12);
    const auto at = [&](size_t i) { return Rml::Vector2f{m_points[i].x * size.x, m_points[i].y * size.y}; };
    if (fillTo > 0.0f && std::isfinite(fillTo)) {
        for (size_t i = 1; i < m_points.size(); ++i)
            AddFillQuad(mesh, at(i - 1), at(i), fillTo * size.y, fillColour);
    }
    for (size_t i = 1; i < m_points.size(); ++i)
        AddLineQuad(mesh, at(i - 1), at(i), thickness, colour);
    if (mesh) m_geometry = GetRenderManager()->MakeGeometry(std::move(mesh));
}
