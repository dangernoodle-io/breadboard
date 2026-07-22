# display

Display driver backends for `bb_display` (the portable framebuffer/font
primitive) — one component per panel/controller, each registering with
`bb_display` via the backend-dispatch convention rather than being a
standalone consumer-facing API. Components land here as they migrate out of
the flat `components/` tree; `bb_display` itself stays flat (it's the
framework primitive the backends depend on, not a member of this group).

<!-- BEGIN bbtool:group-index -->
| Component | Purpose |
|-----------|---------|
| [bb_display_ssd1306](./bb_display_ssd1306/) | — |
<!-- END bbtool:group-index -->
