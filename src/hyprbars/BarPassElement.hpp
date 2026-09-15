#pragma once

// The bar's render-pass element.
//
// A stripped fork of hyprbars (BSD-3-Clause, Copyright (c) 2023 Hypr
// Development) — see PROVENANCE.md. Unchanged but for the class name and the
// blur question, which is now always "no": Golem's bar is opaque.
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
