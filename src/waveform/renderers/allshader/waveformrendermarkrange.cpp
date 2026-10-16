#include "waveform/renderers/allshader/waveformrendermarkrange.h"

#include "rendergraph/geometry.h"
#include "rendergraph/geometrynode.h"
#include "rendergraph/material/unicolormaterial.h"
#include "rendergraph/vertexupdaters/vertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "waveform/renderers/markimagestats.h"
#include "waveform/renderers/waveformwidgetrenderer.h"

using namespace rendergraph;

namespace allshader {

WaveformRenderMarkRange::WaveformRenderMarkRange(WaveformWidgetRenderer* waveformWidget)
        : ::WaveformRendererAbstract(waveformWidget) {
}

void WaveformRenderMarkRange::setup(const QDomNode& node, const SkinContext& skinContext) {
    m_markRanges.clear();
    // The ranges are rebuilt below, so the persistent nodes are dropped (re-created by update()).
    for (const NodeState& state : m_nodes) {
        auto pDetached = detachChildNode(state.pNode);
        noteRangeNode("detached");
    }
    m_nodes.clear();

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        if (child.nodeName() == "MarkRange") {
            addRange(WaveformMarkRange(
                    m_waveformRenderer->getGroup(),
                    child,
                    skinContext,
                    *m_waveformRenderer->getWaveformSignalColors()));
        }
        child = child.nextSibling();
    }
}

void WaveformRenderMarkRange::draw(QPainter* painter, QPaintEvent* event) {
    Q_UNUSED(painter);
    Q_UNUSED(event);
    DEBUG_ASSERT(false);
}

void WaveformRenderMarkRange::update() {
    // I6e: one persistent node per configured range (index into m_markRanges). Nodes are never compacted or
    // detached because a range became inactive/hidden/off-screen: such a range gets a zero-area rectangle.
    // Colour and geometry are only rewritten (and marked dirty) when they change.
    while (m_nodes.size() < m_markRanges.size()) {
        auto pNode = std::make_unique<GeometryNode>();
        GeometryNode* pRaw = pNode.get();
        pRaw->initForRectangles<UniColorMaterial>(1);
        appendChildNode(std::move(pNode));
        m_nodes.push_back({pRaw, {}, {}, {}, false});
        noteRangeNode("created");
    }

    const float breadth = static_cast<float>(m_waveformRenderer->getBreadth());
    for (size_t i = 0; i < m_markRanges.size(); ++i) {
        const auto& markRange = m_markRanges[i];
        NodeState& state = m_nodes[i];

        bool show = markRange.active() && markRange.visible();
        QVector2D lt;
        QVector2D rb;
        QColor color = state.color;
        if (show) {
            // Active mark ranges by definition have starts/ends that are not disabled.
            double startPosition = std::floor(
                    m_waveformRenderer->transformSamplePositionInRendererWorld(
                            markRange.start()));
            double endPosition = std::floor(
                    m_waveformRenderer->transformSamplePositionInRendererWorld(
                            markRange.end()));
            // range not in the current display
            if (startPosition > m_waveformRenderer->getLength() || endPosition < 0) {
                show = false;
            } else {
                lt = {static_cast<float>(startPosition), 0.f};
                rb = {static_cast<float>(endPosition) + 1.f, breadth};
                color = markRange.enabled() ? markRange.m_activeColor : markRange.m_disabledColor;
                color.setAlphaF(0.3f);
            }
        }
        if (!show) {
            lt = {0.f, 0.f};
            rb = {0.f, 0.f};
        }

        bool changed = false;
        if (!state.initialized || lt != state.lt || rb != state.rb) {
            VertexUpdater vertexUpdater{state.pNode->geometry().vertexDataAs<Geometry::Point2D>()};
            vertexUpdater.addRectangle(lt, rb);
            state.pNode->markDirtyGeometry();
            state.lt = lt;
            state.rb = rb;
            changed = true;
        }
        // The colour of a hidden range is irrelevant (zero area); keep the last one to avoid material writes.
        if (!state.initialized || (show && color != state.color)) {
            state.pNode->material().setUniform(1, color);
            state.pNode->markDirtyMaterial();
            state.color = color;
            changed = true;
        }
        state.initialized = true;
        if (changed) {
            noteRangeNode("updated");
        }
    }
}

void WaveformRenderMarkRange::noteRangeNode(const char* what) const {
    MarkImageStats::Scope statsScope(m_waveformRenderer->getGroup() + QStringLiteral("/range:") +
            QLatin1String(what));
    MarkImageStats::noteCause(MarkImageStats::Other);
}

} // namespace allshader
