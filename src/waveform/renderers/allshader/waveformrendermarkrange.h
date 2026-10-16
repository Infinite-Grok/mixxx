#pragma once

#include <QColor>
#include <QVector2D>

#include "rendergraph/node.h"
#include "util/class.h"
#include "waveform/renderers/waveformmarkrange.h"
#include "waveform/renderers/waveformrendererabstract.h"

class QDomNode;
class SkinContext;

namespace rendergraph {
class GeometryNode;
} // namespace rendergraph

namespace allshader {
class WaveformRenderMarkRange;
} // namespace allshader

class allshader::WaveformRenderMarkRange final : public ::WaveformRendererAbstract,
                                                 public rendergraph::Node {
  public:
    explicit WaveformRenderMarkRange(WaveformWidgetRenderer* waveformWidget);

    void addRange(WaveformMarkRange&& range) {
        m_markRanges.push_back(std::move(range));
    }

    // Pure virtual from WaveformRendererAbstract, not used
    void draw(QPainter* painter, QPaintEvent* event) override final;

    void setup(const QDomNode& node, const SkinContext& skinContext) override;

    void update() override;

  private:
    struct NodeState {
        rendergraph::GeometryNode* pNode;
        QColor color;
        QVector2D lt;
        QVector2D rb;
        bool initialized;
    };

    std::vector<WaveformMarkRange> m_markRanges;
    // One persistent node per entry of m_markRanges (same index); children of this node.
    std::vector<NodeState> m_nodes;

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderMarkRange);
};
