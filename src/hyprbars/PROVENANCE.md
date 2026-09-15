# Where this came from, and what we changed

Golem's titlebar for floating windows is a **stripped fork of hyprbars**.

| | |
| --- | --- |
| Upstream | `hyprwm/hyprland-plugins`, `hyprbars/` |
| Tag | **v0.55.0** (the revision nixpkgs pins as `hyprlandPlugins.hyprbars`) |
| Licence | BSD-3-Clause — `LICENSE.hyprbars`, kept verbatim beside the code |
| Vendored | 2026-09-13 |

## Why forked rather than used

The plugin is two halves. One knows Hyprland's internals — reserving space at a
window's top edge, drawing inside the render pass so blur and occlusion behave,
the stencil trick that makes the bar's corners match the window's rounding,
input validation against layer surfaces and seat grabs, and the drag. That half
is worth having and expensive to rediscover.

The other half exists to be *configurable*: seventeen config values, two button
parsers, three window-rule effects, and toggles for blur, alignment, precedence
and hover icons. Golem has one look and one set of buttons, so all of it is
weight — and every styling change would have meant patching a dependency we
don't control, on a release cadence we don't set.

Forking also takes the compositor out of our update path: this builds with
waveview, against the same patched Hyprland, so a version bump is ours to fix
the same day rather than an upstream wait.

**The cost, stated plainly:** upstream fixes no longer arrive on their own. The
tag above is what to diff against when checking for them.

## What we changed

* `CHyprBar` → `CGolemBar`, in `golemBar.{hpp,cpp}`.
* **Floating windows only.** Upstream decorates everything and takes bars away
  with a `hyprbars:no_bar` window rule; here the bar hides itself whenever its
  window is not floating (`syncHidden`), which is the same mechanism with the
  rule replaced by the one condition Golem actually wants.
* **The look is constants, not config** (`golemBar.hpp`): height, colour, text,
  font, padding. The seventeen config values, both button parsers and the three
  window-rule effects are gone.
* **Two fixed buttons** — close, and back into the layout — instead of a
  user-defined list of exec commands.
* **Touch handling removed.** It was ~60 lines serving a device Golem does not
  run on, and it carried the only code path that floated and pinned a window as
  a side effect of dragging it.
* Blur, button alignment, border precedence, hover-only icons, the inactive
  button colour and the double-click action: all dropped.

Everything else — positioning, the render pass, damage, input validation,
dragging, title texture caching — is upstream's, and is where the value is.
