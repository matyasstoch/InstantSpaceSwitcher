# 0001: Make only Cmd+Tab instant

## Context

When Cmd+Tab picks an app whose windows are on another space, macOS slides to that space (~300 ms).
Upstream PR #44 switches instantly from the app activation handler. It works only if the app posts
the switch before macOS starts its own animation, about 20-30 ms after the user releases Cmd. The PR finds the
target on that critical path (window list scan, 10-25 ms) and fires on any activation. Measured on
macOS 26.5: with an accessibility lookup added, 3-4 of 8 Cmd+Tab switches still slid.

## Decision

- Detect Cmd+Tab in the existing event tap (Tab down with Cmd, then Cmd released). Only that switch is instant.
- Remember each app's focused window off the main thread when the app activates or deactivates.
  On Cmd+Tab, only look up that window and switch to its space (`iss_switch_to_window_space`).
- Keep everything else native: the app's hotkeys default to off, swipe override stays off,
  and the menu bar icon is hidden by default.

## Consequences

- Cmd+Tab switches in ~20-50 ms. Other ways of changing spaces keep the macOS animation.
- An app with no remembered window (for example Finder before its first activation) still slides once.
- Uses two private APIs: `CGSCopySpacesForWindows` and `_AXUIElementGetWindow`.
