#pragma once

// The bar's render-pass element.
//
// A stripped fork of hyprbars (BSD-3-Clause, Copyright (c) 2023 Hypr
// Development) — see PROVENANCE.md. Unchanged but for the class name and the
// blur question, answered per frame: the strip is as translucent as its
// window and blurs its backdrop the same way (since 2026-09-26).
//
// Drawing through a pass element rather than straight from `draw()` is what
// gives the bar correct occlusion and damage behaviour inside Hyprland's
// render pass.

#include <hyprland/src/render/pass/PassElement.hpp>

class CGolemBar;

class CBarPassElement : public IPassElement {
  public:
    struct SBarData {
        CGolemBar* deco = nullptr;
        float      a    = 1.F;
        /// The strip is translucent like its window and wants the window's
        /// blur behind it: one of these is set (never both), decided the way
        /// the compositor decides it for the content (`CGolemBar::draw`).
        bool       liveBlur       = false;
        bool       precomputeBlur = false;
    };

    CBarPassElement(const SBarData& data_);
    virtual ~CBarPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override;
    virtual bool                          needsLiveBlur() override;
    virtual bool                          needsPrecomputeBlur() override;
    virtual std::optional<CBox>           boundingBox() override;

    virtual const char*                   passName() override {
        return "CBarPassElement";
    }

    virtual ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    SBarData data;
};
