# ClipLite visual system

## Product direction

The interface uses Apple-style clear liquid glass, as requested by the user. The
glass sits on a quiet charcoal workspace, so real clips and editing controls
remain easy to read. Translucency is reserved for the command bar, menus, and
status controls that benefit from depth.

The product currently uses a dark palette for its Windows recording context.
Light mode has not been confirmed as a product preference.

## Color

| Role | Value | Source |
| --- | --- | --- |
| Workspace | `#141518` | `--bg` in `assets/web/app.css` |
| Panel | `#1e2025` | `--panel` |
| Primary text | `#f3f4f6` | `--text` |
| Secondary text | `#b8bcc5` | `--muted` |
| Accent | `#c4dff7` | `--accent` |
| Danger | `#ffa1a7` | `--danger` |
| Success | `#a7dfbd` | `--ok` |

Glass controls use a pale neutral gradient and an inset silver optical rim. The
rim stays at the edge of the surface. Focus uses the accent color, and reduced
transparency and forced-color modes replace the glass treatment with solid
surfaces.

## Type and shape

The local system stack begins with Segoe UI Variable, then Segoe UI and the
platform sans-serif fallback. Body copy is 13px. The command bar uses a 16px
corner radius; small controls use compact 7–10px radii. Spacing follows small
4px increments for control groups and wider 12–24px increments for panels.

## Layout

- A transmitting command bar anchors the Library and Capture workspaces.
- The library keeps the app/game rail narrow and the recordings prominent.
- Capture, playback, settings, and editing use the same graphite surfaces and
  compact control language.
- The editor places its tools in a dedicated panel. At compact sizes, opening
  Tools moves the preview and timeline into their own column.
- Media remains the content plane; borders and light do the work of separating
  nearby controls.

## Motion and input

State changes use the shared `--ease` curve from `assets/web/app.css`. Controls
settle on press and use the accent outline for keyboard focus. The interface
respects reduced-motion, reduced-transparency, and Windows forced-colors
settings.

## App icon

`assets/web/clip-icon.svg` is the authored 64px mark. It pairs a graphite glass
tile with a silver replay arrow and play shape. The Library header renders that
SVG directly; `assets/cliplite.ico` contains the matching 16–256px Windows icon
frames used by the native window, taskbar, tray, and Start Menu shortcut. Keep
the ICO up to date when the SVG changes. CMake tracks the ICO as an input to the
Windows resource object so an icon edit relinks the application.
