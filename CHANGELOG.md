# Changelog

This file records player-facing changes to G-Diffuser. Release candidates are test builds and may still contain known regressions.

## [1.0.2-rc1] - 2026-08-13

This release candidate focuses on controller rebinding, menu and ultrawide stability, and Expansion Kit Course Edit recovery. It is not yet a stable 1.0.2 release.

### Fixed for testing

- Controller rebinding now blocks menu gamepad navigation for the entire mapping operation, so shoulder inputs can be assigned without changing menu tabs ([#16](https://github.com/Zorkats/G-Diffuser/issues/16)).
- Multi-viewport docking is disabled by default to avoid the secondary-window path associated with menu and tooltip crashes. It remains available as an opt-in developer setting that applies after restart ([#7](https://github.com/Zorkats/G-Diffuser/issues/7)).
- Oversized offscreen framebuffers are scaled uniformly to the active renderer's texture limit. Direct3D 11 also creates replacement color and depth resources before replacing the working framebuffer, targeting the reported 32:9 fullscreen crash ([#17](https://github.com/Zorkats/G-Diffuser/issues/17)).
- Expansion Kit custom courses now build their runtime segment data in the buffer expected by Course Edit and race setup ([#12](https://github.com/Zorkats/G-Diffuser/issues/12)).
- Expansion Kit file-list failures now complete with a dismissible result instead of leaving Course Edit stuck waiting for an operation that has already failed.
- A missing MFS root directory can be repaired without formatting the rest of the disk save. Full formatting still requires explicit one-shot authorization from the Workshop menu.
- Disk sidecars now use the application's `saves` directory consistently on Windows and Linux. Linux can copy legacy sidecars from the old working-directory location when no canonical save exists.

### Known issues and test focus

- A newly saved Course Edit track can be written to the `.gdd` sidecar but still fail to appear in **File > Load**. Immediate and post-restart loading remain under investigation, so custom-course persistence is not yet cleared for the stable 1.0.2 release ([#12](https://github.com/Zorkats/G-Diffuser/issues/12)).
- Create Machine needs a complete save, restart, and reload test before its persistence path is considered release-ready ([#10](https://github.com/Zorkats/G-Diffuser/issues/10)).
- The menu, tooltip, and extreme-resolution fixes still need confirmation on the hardware configurations that originally reported them ([#7](https://github.com/Zorkats/G-Diffuser/issues/7), [#17](https://github.com/Zorkats/G-Diffuser/issues/17)).

### Release-candidate test checklist

- Rebind L, R, and Z while controller menu navigation is enabled.
- Open the enhancement menu, inspect tooltips, and enter and leave fullscreen.
- Test 5120x1440 with increased internal resolution on Direct3D 11.
- Save a custom course, find it in **File > Load**, restart, load it again, and enter a race.
- Save a custom machine, restart, and load it again.

[1.0.2-rc1]: https://github.com/Zorkats/G-Diffuser/releases/tag/v1.0.2-rc1
