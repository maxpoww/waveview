// A stripped fork of hyprbars (BSD-3-Clause, Copyright (c) 2023 Hypr
// Development) — see PROVENANCE.md.

#include "BarPassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include "golemBar.hpp"

using namespace Render::GL;

CBarPassElement::CBarPassElement(const CBarPassElement::SBarData& data_) : data(data_) {
    ;
}

std::vector<UP<IPassElement>> CBarPassElement::draw() {
    data.deco->renderPass(g_pHyprRenderer->m_renderData.pMonitor.lock(), data.a);
    return {};
}

bool CBarPassElement::needsLiveBlur() {
    // Golem's bar is opaque, so there is nothing behind it to blur. (Upstream
    // asked its config here.)
    return false;
}

std::optional<CBox> CBarPassElement::boundingBox() {
    // Upstream's note, still true: expand the box a little or occlusion gets
    // too aggressive and the bar flickers out.
    //
    // Extended by the notch depth: the strip's paint reaches below the seam
    // into the two corner notches (`renderPass`), and a box that stops at the
    // strip lets occlusion cull the fill while the corners are still visible.
    CBox box = data.deco->assignedBoxGlobal();
    box.h += data.deco->notchDepth();
    return box.translate(-g_pHyprRenderer->m_renderData.pMonitor->m_position).expand(10);
}

bool CBarPassElement::needsPrecomputeBlur() {
    return false;
}
