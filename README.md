# Omarchy Screenshot

English | [Simplified Chinese](README.zh.md)

A Qt 6 screenshot and annotation tool for Hyprland and Omarchy, with frozen overlays and cross-monitor selections.

## Features

- Quickly select a window, a monitor, or any region, across screens, then adjust the selection.
- Annotate with shapes, arrows, a pen, text, and numbered markers, hide private details with mosaic, and undo or redo.
- Capture long pages by scrolling, with automatic stitching; continue capturing and annotate the long image.
- Copy or save in one click, or double-click the selection to copy it.
- Recognize text in the selection with PP-OCRv6 Tiny, then copy it in one click.
- Pin screenshots and annotations above other windows, keep multiple pins, drag across monitors, and dismiss each with its top-right × button.
- Customize the annotation color and the light or dark theme; the app remembers your preferences.
- Use it in 42 language or regional variants, with multiple monitors, mixed scaling, and rotated screens.

## Controls

| Action | Result |
| --- | --- |
| Hover, then click | Select a window or monitor |
| H / scrolling capture button, before annotating | Start a scrolling capture; with several windows, choose one first |
| Click a blank area of the capture region while scrolling | Stop and open the long image; Continue capture can extend it later |
| H / Continue capture button on a long image | Continue capturing downward, keeping annotations |
| = / - on a long image | Zoom in / zoom out |
| Drag inside a zoomed long image with the V tool | Scroll through the image; the border and handles still resize the selection |
| Wheel / Ctrl+wheel / middle-button drag on a long image | Scroll / zoom / pan |
| Drag | Select a rectangle; move or resize an existing selection |
| Double-click a blank area inside the selection | Copy the selection and its annotations to the clipboard and close the app; long images export at full resolution regardless of zoom or scroll position |
| O / OCR button, before annotating | Recognize text in the selection, copy it, and show the result in a panel |
| V / T / G / B / W | Selection / text / mosaic redaction / numbered marker / line |
| R / E / A / D | Recall the last rectangle / ellipse / arrow (including line) / pen (including highlighter) style; click a group button to open its icon toolbar |
| Shift+B | Spotlight |
| Shift+R / Shift+D / Shift+E | Rounded rectangle / filled rectangle / filled ellipse |
| Shift+A / Shift+W | Curved arrow / double-headed curved arrow |
| Q / leftmost color button | Choose a preset or drag in the palette to pick a color; saved automatically |
| Sun / moon icons at the top of the palette | Switch to the light / dark toolbar theme; saved automatically |
| Arrow keys / Shift+arrow keys in V mode | Expand / shrink the corresponding edge by 1 px; hold to repeat |
| Click with the text tool | Type text; Shift+Enter inserts a new line, Enter or clicking elsewhere confirms |
| Alt with the text tool | Confirm the text and return to the previous tool |
| Z | Undo the last annotation |
| X | Redo an undone annotation |
| P / pin button | Pin the selection and annotations to the desktop and finish capturing; long images remain complete and fit on screen |
| Drag a pinned image / click its top-right × | Move across monitors / dismiss that pin; double-clicking a pin has no action |
| C | Copy the screenshot and exit |
| S | Save the screenshot to your Pictures directory, or to `$OMARCHY_SCREENSHOT_DIR` if set, and exit |
| Esc / right-click | Exit |

Window captures include the pixels visible in the selection, including overlapping windows.

Normal and scrolling captures exclude the pointer, including software cursors changing shape during a frame. Each capture temporarily suppresses compositor cursor rendering and waits about 550 ms for it to take effect, then restores the previous state on success or failure without changing configuration files.

Pins are temporary and are not automatically saved or copied. Existing pins hide during a new capture and return afterward. The process stays alive while pins remain; without resident mode, closing the last pin exits it.

## OCR

Press `O` or click the OCR button to recognize text in the current selection. Recognition runs in the background, reads the captured pixels without annotations, copies the result to the clipboard, and shows it in a panel where you can select or copy it again. Press Esc to close the panel, then Esc again to leave the capture.

OCR uses PP-OCRv6 Tiny through the bundled RapidOCRLite C++ engine. The models support Simplified Chinese, Traditional Chinese, English, and Latin-script languages. Japanese, Korean, Cyrillic, Arabic, Hebrew, Thai, and other scripts need a different recognition model.

Mosaic and spotlight annotations do not hide text from OCR, because OCR reads the original captured pixels. Annotate after running OCR if the text is sensitive.

Long-image OCR is not supported yet. OCR is only available on normal selections.

## Interface languages

Supports 42 language or regional variants and follows the system interface language by default. Keyboard layouts do not affect the interface language. To override it:

```sh
omarchy-screenshot --language zh_CN
OMARCHY_SCREENSHOT_LANGUAGE=ja omarchy-screenshot
```

Priority: `--language` > `OMARCHY_SCREENSHOT_LANGUAGE` > system preferences (`LANGUAGE`, `LC_ALL`, `LC_MESSAGES`, `LANG`). Selection takes effect at startup, accepts tags such as `pt-BR` and `zh-Hant`, and falls back to English when no translation matches. Shortcuts stay the same. See [translations/](translations/) for available languages; `noto-fonts` and `noto-fonts-cjk` are recommended for font coverage.

Qt Linguist `.ts` translations are embedded during the build. After changing interface text, run `cmake --build build --target update_translations` and complete the translations, preserving `%1` placeholders, application names, and shortcuts.

## Build and run

Requires a Hyprland Wayland session; supports `x86_64` and `aarch64`. Install from the AUR on Omarchy:

```sh
omarchy pkg aur add omarchy-screenshot
```

Set shortcuts in `~/.config/hypr/bindings.lua` for immediate capture with `Ctrl+Alt+A` and a five-second countdown with `Ctrl+Alt+Shift+A`:

```lua
hl.unbind("CTRL + ALT + A")
o.bind("CTRL + ALT + A", "Omarchy Screenshot", "omarchy-screenshot")
hl.unbind("CTRL + ALT + SHIFT + A")
o.bind("CTRL + ALT + SHIFT + A", "Screenshot after 5 seconds", "omarchy-screenshot --delay 5")
```

The delayed shortcut shows a film-style circular countdown from 5 to 1 in the center of the active screen. It passes keyboard and mouse input through, so you can switch windows or open menus. The countdown fades out and closes before capturing, keeping it out of the screenshot. You can also run `omarchy-screenshot --delay 5`; `--delay` accepts 0–60 seconds, with 0 (the default) capturing immediately.

Building from source requires `cmake`, `gcc`, `pkgconf`, `qt6-base`, `qt6-declarative`, `qt6-wayland`, `qt6-tools`, `layer-shell-qt`, `wayland`, `wayland-protocols`, `grim`, `wl-clipboard`, and `hyprland`. Tests also require `python` by default; disable them with `-DBUILD_TESTING=OFF`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/omarchy-screenshot
```

The configure step fetches RapidOCRLite and downloads the PP-OCRv6 Tiny models into the build directory. For an offline build, extract the RapidOCRLite source and point the build at it and at a directory containing the three model files:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_RAPIDOCRLITE=/path/to/RapidOCRLite \
  -DOMARCHY_SCREENSHOT_OCR_MODEL_DIR=/path/to/models
```

Install locally, configuring the service path for resident mode:

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DSYSTEMD_USER_UNIT_DIR=$HOME/.local/share/systemd/user
cmake --build build -j
cmake --install build --component omarchy-screenshot
```

### Omarchy bar plugin

The repository also supplies a Quickshell bar widget with plugin ID `io.github.manateelazycat.screenshot`. Install the native application first; the standard plugin installer copies the widget but does not compile or install the Qt application. The five-second countdown requires native version 0.1.12 or later.

```sh
omarchy pkg aur add omarchy-screenshot
omarchy plugin add https://github.com/manateelazycat/omarchy-screenshot --enable
```

Left-click the camera button to capture immediately, or right-click for the five-second circular countdown. The widget launches `omarchy-screenshot` from `PATH`. For a local build, configure its bar entry with `"command": "/home/yourname/.local/bin/omarchy-screenshot"`. Installing the widget does not change Hyprland shortcuts; configure those explicitly using the example above.

Remove the widget with `omarchy plugin remove io.github.manateelazycat.screenshot`. To also remove an AUR installation, run `omarchy pkg drop omarchy-screenshot`. For a source installation under `~/.local`, stop its optional socket and daemon with `systemctl --user disable --now omarchy-screenshot.socket` and `systemctl --user stop omarchy-screenshot.service`, then remove `~/.local/bin/omarchy-screenshot` and any installed `omarchy-screenshot.service` and `omarchy-screenshot.socket` user units. Remove shortcut entries you added from `~/.config/hypr/bindings.lua`.

### Tests

`ctest --test-dir build --output-on-failure` checks translations, scroll stitching, cursor suppression and restoration, the daemon, OCR, and image sampling at different scales without a desktop session. Inside Hyprland, `./build/cursor-live-test` checks monitor captures and scrolling frames while the pointer moves across outputs and changes between three shapes; it briefly covers each screen with a solid fixture. Run these additional checks with `./build/omarchy-screenshot` and the listed options:

| Option | Checks |
| --- | --- |
| `--self-test` | Cross-monitor selection, undo/redo, and markers; requires at least two monitors |
| `--ui-self-test --language he` | Toolbar, themes, annotations, export, and translated layouts; uses temporary settings |
| `--scroll-stitch-test` | Scroll stitching |
| `--scroll-ui-self-test` | Long-image interface |
| `--pin-ui-self-test` | Pin export, real cross-monitor dragging, multiple pins, capture exclusion, and close button; requires two monitors |

### Resident mode

Off by default. Reusing Qt and GPU initialization reduces startup overhead; each capture still waits for compositor cursor suppression. The daemon exits after 10 idle minutes and uses about 100 MB of RAM plus 320 MB of GPU memory while running.

```sh
systemctl --user enable --now omarchy-screenshot.socket   # Enable
systemctl --user disable --now omarchy-screenshot.socket  # Disable and stop the daemon
```

Without systemd, add `omarchy-screenshot --daemon` to your session's autostart. Keep the same shortcut; only launches without options use the daemon. `--language`, `--delay`, and self-tests run independently.

Run `systemctl --user edit omarchy-screenshot.service` to change the idle timeout (seconds; `0` means no timeout), keeping the actual installation path:

```ini
[Service]
ExecStart=
ExecStart=/usr/bin/omarchy-screenshot --daemon --idle-timeout 1800
```

Omarchy's overlay fade-in can add up to 400 ms. For immediate display, add this rule to `~/.config/hypr/hyprland.lua`:

```lua
hl.layer_rule({ match = { namespace = "^omarchy-screenshot$" }, no_anim = true, animation = "none" })
```

### AUR publishing

[PKGBUILD](PKGBUILD) defines the package. Pushing a `vMAJOR.MINOR.PATCH` tag triggers [GitHub Actions](.github/workflows/publish-aur.yml) to update the version and checksum and commit to the AUR. Before the first release, configure the `AUR_USERNAME`, `AUR_EMAIL`, and `AUR_SSH_PRIVATE_KEY` secrets and add the public key to your AUR account.

## License

Copyright (C) 2026 Andy Stewart. Source code: [GPL-3.0-only](LICENSE). Bundled [virtual pointer protocol](protocols/wlr-virtual-pointer-unstable-v1.xml): MIT. RapidOCRLite and its OCR components: Apache-2.0 and BSD-3-Clause.
