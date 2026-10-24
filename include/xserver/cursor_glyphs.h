#pragma once
#include <cstdint>

namespace fbx::xserver {

// Rasterize a diagonal double-headed resize cursor into an ARGB8888 buffer.
// The glyph is a white shaft + arrowheads with a black outline, drawn in a 20x20
// logical grid scaled up by `scale_blocks` (nearest-neighbour) to keep edges
// crisp. `ne_sw == false` draws the NW↔SE diagonal (↖↘, for nw/se/nwse_resize);
// `ne_sw == true` mirrors it to the NE↔SW diagonal (↗↙, for ne/sw/nesw_resize).
//
// The caller must have already cleared `pixels`. hot_x/hot_y receive the cursor
// hotspot in physical pixels (the centre of the glyph).
void DrawDiagonalResize(uint32_t* pixels, uint32_t stride, uint32_t size,
                        int scale_blocks, bool ne_sw,
                        int32_t& hot_x, int32_t& hot_y) noexcept;

// Rasterize a "not allowed" cursor (⊘: a ring with a diagonal slash) into an
// ARGB8888 buffer. White fill, black outline; same 20x20 grid + block scaling as
// DrawDiagonalResize. The caller must have already cleared `pixels`.
void DrawNotAllowed(uint32_t* pixels, uint32_t stride, uint32_t size,
                    int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// Rasterize a "grabbing" cursor (a closed fist) into an ARGB8888 buffer. White
// fill, black outline; same 20x20 grid + block scaling. The caller must have
// already cleared `pixels`.
void DrawGrabbing(uint32_t* pixels, uint32_t stride, uint32_t size,
                  int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// Rasterize a "grab" cursor (an open hand, shown on hover before a drag) into an
// ARGB8888 buffer. White fill, black outline; same 20x20 grid + block scaling.
// Shares its hotspot with DrawGrabbing so the cursor doesn't jump on press. The
// caller must have already cleared `pixels`.
void DrawGrab(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// Rasterize a "zoom" cursor (a magnifying glass with a diagonal handle) into an
// ARGB8888 buffer. `zoom_in == true` draws a '+' inside the lens; `false` draws
// a '-' (zoom_out). White fill, black outline; same 20x20 grid + block scaling.
// The caller must have already cleared `pixels`.
void DrawZoom(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, bool zoom_in, int32_t& hot_x, int32_t& hot_y) noexcept;

// Rasterize a "wait" cursor (a centred hourglass) into an ARGB8888 buffer. White
// fill, black outline; same 20x20 grid + block scaling. The caller must have
// already cleared `pixels`.
void DrawWait(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// Rasterize a "progress" cursor (the pointer arrow with a small busy-hourglass
// badge — working but still interactive) into an ARGB8888 buffer. White fill,
// black outline; same 20x20 grid + block scaling. Hotspot is the arrow tip. The
// caller must have already cleared `pixels`.
void DrawProgress(uint32_t* pixels, uint32_t stride, uint32_t size,
                  int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// Drag-and-drop cursors: the default pointer arrow plus a small badge in the
// lower right — '+' for copy, a right-arrow for alias (link), a ⊘ ring+slash for
// no_drop. Hotspot is the arrow tip. White fill, black outline; same 20x20 grid
// + block scaling. The caller must have already cleared `pixels`.
void DrawCopy(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;
void DrawAlias(uint32_t* pixels, uint32_t stride, uint32_t size,
               int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;
void DrawNoDrop(uint32_t* pixels, uint32_t stride, uint32_t size,
                int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The "move" cursor: a centred four-way arrow (fleur). Hotspot is the centre.
// White fill, black outline; same 20x20 grid + block scaling. The caller must
// have already cleared `pixels`.
void DrawMove(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The "cell" cursor: a thick '+' (spreadsheet cell selector), heavier than the
// 1px crosshair. Hotspot is the centre. White fill, black outline; same 20x20
// grid + block scaling. The caller must have already cleared `pixels`.
void DrawCell(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The "all_scroll" cursor: four arrowheads around a central hub, separated by a
// gap (unlike the solid-cross move fleur). Hotspot is the centre. White fill,
// black outline; same 20x20 grid + block scaling. The caller must have already
// cleared `pixels`.
void DrawAllScroll(uint32_t* pixels, uint32_t stride, uint32_t size,
                   int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The "help" cursor: the default pointer arrow with a small '?' badge in the
// lower right. Hotspot is the arrow tip. White fill, black outline; same 20x20
// grid + block scaling. The caller must have already cleared `pixels`.
void DrawHelp(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The "context_menu" cursor: the default pointer arrow with a small menu (three
// list rows) badge in the lower right. Hotspot is the arrow tip. White fill,
// black outline; same 20x20 grid + block scaling. The caller must have already
// cleared `pixels`.
void DrawContextMenu(uint32_t* pixels, uint32_t stride, uint32_t size,
                     int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The "text" cursor: a vertical I-beam with top/bottom serifs. Hotspot is the
// centre of the beam. White fill, black outline; same 20x20 grid + block
// scaling. The caller must have already cleared `pixels`.
void DrawText(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The "vertical_text" cursor: the text I-beam rotated 90° (a horizontal beam
// with left/right serifs), for vertical writing modes. Hotspot is the centre of
// the beam. White fill, black outline; same 20x20 grid + block scaling. The
// caller must have already cleared `pixels`.
void DrawVerticalText(uint32_t* pixels, uint32_t stride, uint32_t size,
                      int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The "crosshair" cursor: a thin white '+' broken at the centre. Hotspot is the
// centre. White fill, black outline; same 20x20 grid + block scaling. The caller
// must have already cleared `pixels`.
void DrawCrosshair(uint32_t* pixels, uint32_t stride, uint32_t size,
                   int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// Horizontal/vertical double-headed resize arrows (←→ / ↑↓), shared by the
// e/w/ew/col and n/s/ns/row resize shapes respectively. Hotspot is the centre.
// White fill, black outline; same 20x20 grid + block scaling. The caller must
// have already cleared `pixels`.
void DrawHResize(uint32_t* pixels, uint32_t stride, uint32_t size,
                 int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;
void DrawVResize(uint32_t* pixels, uint32_t stride, uint32_t size,
                 int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept;

// The default pointer arrow — anti-aliased with a soft drop shadow, rendered at
// a floating-point `scale` (unlike the nearest-neighbour block glyphs). Writes
// premultiplied-looking ARGB (grey = white fill over black shadow/outline).
// Hotspot (the arrow tip) is round(scale). The caller must have already cleared
// `pixels`.
void DrawArrow(uint32_t* pixels, uint32_t stride, uint32_t size,
               float scale, int32_t& hot_x, int32_t& hot_y) noexcept;

} // namespace fbx::xserver
