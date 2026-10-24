#include "xserver/cursor_glyphs.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace fbx::xserver {

namespace {
constexpr uint32_t kWhite = 0xFFFFFFFF;
constexpr uint32_t kBlack = 0xFF000000;

// A block-scaled plotter over the shared 20x20 logical grid.
struct Plotter {
    uint32_t* pixels; uint32_t stride; uint32_t size; int sc;
    void put(int lx, int ly, uint32_t color) const noexcept {
        for (int dy = 0; dy < sc; ++dy)
            for (int dx = 0; dx < sc; ++dx) {
                int px = lx * sc + dx, py = ly * sc + dy;
                if (px >= 0 && py >= 0 && px < (int)size && py < (int)size)
                    pixels[py * stride + px] = color;
            }
    }
};

// Fill a silhouette (given by a contains(x,y) predicate over the 20x20 grid)
// with a white interior and a 1px black outline via 4-neighbour erosion: a
// contained pixel is white only if all four neighbours are also contained,
// otherwise it's a boundary pixel and drawn black.
template <typename Fn>
void FillEroded(const Plotter& p, Fn&& contains) {
    for (int ly = 0; ly < 20; ++ly)
        for (int lx = 0; lx < 20; ++lx) {
            if (!contains(lx, ly)) continue;
            bool interior = contains(lx - 1, ly) && contains(lx + 1, ly) &&
                            contains(lx, ly - 1) && contains(lx, ly + 1);
            p.put(lx, ly, interior ? kWhite : kBlack);
        }
}

// Stamp a thin glyph given by an on(x,y) predicate: white for on-pixels, plus a
// 1px black halo on the surrounding (8-neighbour) transparent pixels, so even a
// 1px-wide stroke gets an outline. White is drawn last so it wins over the halo.
// The predicate must return false for out-of-range coordinates (all the ones
// here are guarded by x>=/x<= bounds, so negative/large indices are safe).
template <typename Fn>
void StampHalo(const Plotter& p, Fn&& on) {
    for (int ly = 0; ly < 20; ++ly)
        for (int lx = 0; lx < 20; ++lx) {
            if (on(lx, ly)) continue;
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if ((dx || dy) && on(lx + dx, ly + dy)) { near = true; break; }
            if (near) p.put(lx, ly, kBlack);
        }
    for (int ly = 0; ly < 20; ++ly)
        for (int lx = 0; lx < 20; ++lx)
            if (on(lx, ly)) p.put(lx, ly, kWhite);
}

// Hourglass silhouette: a filled bowtie between y0 (top cap) and y1 (bottom
// cap), centred on cx, pinching to ~1px at the middle and flaring to half-width
// `hw` at the caps.
bool HourglassContains(int x, int y, float cx, int y0, int y1, float hw) noexcept {
    if (y < y0 || y > y1) return false;
    float cy = (y0 + y1) * 0.5f;
    float f  = std::fabs(y - cy) / (cy - y0);   // 0 at centre, 1 at the caps
    float half = 0.5f + f * (hw - 0.5f);
    return std::fabs(x - cx) <= half;
}

// The default pointer arrow, as a filled triangle with vertices (1,1)-(1,12)-
// (9,9): a vertical left edge, a 45° top edge, and a shallow bottom edge —
// pointing up-left. Shared by the drag-and-drop badge cursors so their arrow
// matches the plain cursor; the badge sits clear of it in the lower right.
bool PointerContains(int x, int y) noexcept {
    return x >= 1 && x <= 9 && y >= x &&
           static_cast<float>(y) <= 12.0f - 0.375f * (x - 1);
}

// A "not allowed" ring+slash (⊘) evaluated at an arbitrary centre/radius, so it
// can be drawn full size or as a small badge. Returns 1 for a white core pixel,
// -1 for a black outline pixel, 0 for nothing.
int RingSlashBand(int x, int y, float cx, float cy, float R) noexcept {
    float dx = x - cx, dy = y - cy;
    float r  = std::sqrt(dx * dx + dy * dy);
    float sd = std::fabs(dx - dy) * 0.70710678f;   // distance to the y=x slash
    bool core = (std::fabs(r - R) <= 0.9f) || (sd <= 1.0f && r <= R + 0.8f);
    bool out  = (std::fabs(r - R) <= 1.7f) || (sd <= 1.8f && r <= R + 1.5f);
    return core ? 1 : (out ? -1 : 0);
}

// The default pointer arrow as a filled polygon in its own (unscaled) pixel
// space: tip at (0,0), out to (10,14), a diagonal notch to (4,16), a 4px shaft
// down to (4,27), then back to (0,27). Unlike the block glyphs this is rendered
// anti-aliased at a floating-point scale, so it is described by point-in-polygon
// and distance-to-edge tests rather than a grid predicate.
constexpr int   kANV  = 5;
constexpr float kAVX[] = { 0.f, 10.f,  4.f,  4.f, 0.f };
constexpr float kAVY[] = { 0.f, 14.f, 16.f, 27.f, 27.f };

bool ArrowContains(float px, float py) noexcept {
    bool inside = false;
    for (int i = 0, j = kANV - 1; i < kANV; j = i++) {
        if (((kAVY[i] > py) != (kAVY[j] > py)) &&
            px < (kAVX[j] - kAVX[i]) * (py - kAVY[i]) / (kAVY[j] - kAVY[i]) + kAVX[i])
            inside = !inside;
    }
    return inside;
}

float ArrowBoundaryDist(float px, float py) noexcept {
    float md = 1e9f;
    for (int i = 0, j = kANV - 1; i < kANV; j = i++) {
        float x1 = kAVX[j], y1 = kAVY[j], x2 = kAVX[i], y2 = kAVY[i];
        float dx = x2 - x1, dy = y2 - y1;
        float len2 = dx * dx + dy * dy;
        float t = (len2 < 1e-6f) ? 0.f
                  : std::clamp(((px - x1) * dx + (py - y1) * dy) / len2, 0.f, 1.f);
        float ex = px - (x1 + t * dx), ey = py - (y1 + t * dy);
        md = std::min(md, std::sqrt(ex * ex + ey * ey));
    }
    return md;
}
} // namespace

void DrawDiagonalResize(uint32_t* pixels, uint32_t stride, uint32_t size,
                        int scale_blocks, bool ne_sw,
                        int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;

    constexpr uint32_t W = 0xFFFFFFFF; // white fill
    constexpr uint32_t B = 0xFF000000; // black outline
    const int sc = scale_blocks;

    // Put one logical pixel (0..19 grid), mirrored in x for the NE↔SW variant,
    // scaled up into an sc×sc block.
    auto put = [&](int lx, int ly, uint32_t color) noexcept {
        int rx = ne_sw ? (19 - lx) : lx;
        for (int dy = 0; dy < sc; ++dy)
            for (int dx = 0; dx < sc; ++dx) {
                int px = rx * sc + dx, py = ly * sc + dy;
                if (px >= 0 && py >= 0 && px < (int)size && py < (int)size)
                    pixels[py * stride + px] = color;
            }
    };

    // NW arrowhead: filled right triangle with the corner at (2,2), legs running
    // to (8,2) and (2,8); black on the three edges, white inside.
    for (int i = 0; i <= 6; ++i)
        for (int j = 0; j <= 6 - i; ++j) {
            bool edge = (i == 0) || (j == 0) || (i + j == 6);
            put(2 + i, 2 + j, edge ? B : W);
        }

    // SE arrowhead: mirror of the above, corner at (17,17).
    for (int i = 0; i <= 6; ++i)
        for (int j = 0; j <= 6 - i; ++j) {
            bool edge = (i == 0) || (j == 0) || (i + j == 6);
            put(17 - i, 17 - j, edge ? B : W);
        }

    // Shaft: white diagonal core with a black border on each side (perpendicular
    // to the 45° line, i.e. offset by (+1,-1) and (-1,+1)).
    for (int t = 6; t <= 13; ++t) {
        put(t + 1, t - 1, B);
        put(t - 1, t + 1, B);
    }
    for (int t = 6; t <= 13; ++t)
        put(t, t, W);

    // Hotspot at the geometric centre of the glyph.
    hot_x = 9 * sc;
    hot_y = 9 * sc;
}

void DrawNotAllowed(uint32_t* pixels, uint32_t stride, uint32_t size,
                    int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // Ring centred at (9.5, 9.5) with a diagonal slash (NW↔SE) through it.
    constexpr float cx = 9.5f, cy = 9.5f, R = 7.0f;
    for (int ly = 0; ly < 20; ++ly)
        for (int lx = 0; lx < 20; ++lx) {
            float dx = lx - cx, dy = ly - cy;
            float r  = std::sqrt(dx * dx + dy * dy);
            float sd = std::fabs(dx - dy) * 0.70710678f; // dist to the y=x line

            bool ring_core = std::fabs(r - R) <= 1.0f;
            bool ring_out  = std::fabs(r - R) <= 1.9f;
            bool slash_core = (sd <= 1.1f) && (r <= R + 0.9f);
            bool slash_out  = (sd <= 2.0f) && (r <= R + 1.6f);

            if (ring_core || slash_core)      p.put(lx, ly, kWhite);
            else if (ring_out || slash_out)   p.put(lx, ly, kBlack);
        }

    hot_x = 10 * scale_blocks;
    hot_y = 10 * scale_blocks;
}

void DrawGrabbing(uint32_t* pixels, uint32_t stride, uint32_t size,
                  int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // Closed-fist silhouette: a palm block, three knuckle bumps on top, and a
    // thumb nub on the left. Drawn as a filled shape with a 1px black outline
    // (interior = white, boundary = black) so it reads on any background.
    auto contains = [](int x, int y) noexcept -> bool {
        if (x >= 4 && x <= 14 && y >= 9 && y <= 16) return true;   // palm
        if (y >= 7 && y <= 9) {                                    // knuckles
            if (x >= 4  && x <= 6)  return true;
            if (x >= 8  && x <= 10) return true;
            if (x >= 12 && x <= 14) return true;
        }
        if (x >= 2 && x <= 4 && y >= 11 && y <= 14) return true;   // thumb
        return false;
    };

    for (int ly = 0; ly < 20; ++ly)
        for (int lx = 0; lx < 20; ++lx) {
            if (!contains(lx, ly)) continue;
            bool interior = contains(lx - 1, ly) && contains(lx + 1, ly) &&
                            contains(lx, ly - 1) && contains(lx, ly + 1);
            p.put(lx, ly, interior ? kWhite : kBlack);
        }

    hot_x = 9 * scale_blocks;
    hot_y = 12 * scale_blocks;
}

void DrawGrab(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // Open-hand silhouette: a palm block with four fingers extended upward (tips
    // slightly staggered — middle/ring tallest, pinky shortest) and a thumb nub
    // off the lower left. Same fill+erode outline as DrawGrabbing.
    // Fingers are 3px wide so the fill+erode outline leaves a 1px white core;
    // a 2px finger would erode to solid black. Tips are staggered (middle/ring
    // tallest, index/pinky shorter) and separated by 1px gaps.
    auto contains = [](int x, int y) noexcept -> bool {
        if (x >= 3 && x <= 17 && y >= 10 && y <= 17) return true;   // palm
        if (x >= 3  && x <= 5  && y >= 4 && y <= 11) return true;   // index
        if (x >= 7  && x <= 9  && y >= 2 && y <= 11) return true;   // middle
        if (x >= 11 && x <= 13 && y >= 2 && y <= 11) return true;   // ring
        if (x >= 15 && x <= 17 && y >= 4 && y <= 11) return true;   // pinky
        if (x >= 1  && x <= 3  && y >= 12 && y <= 15) return true;  // thumb
        return false;
    };

    for (int ly = 0; ly < 20; ++ly)
        for (int lx = 0; lx < 20; ++lx) {
            if (!contains(lx, ly)) continue;
            bool interior = contains(lx - 1, ly) && contains(lx + 1, ly) &&
                            contains(lx, ly - 1) && contains(lx, ly + 1);
            p.put(lx, ly, interior ? kWhite : kBlack);
        }

    // Match DrawGrabbing's hotspot so the cursor stays put when hover→press
    // switches grab (16) to grabbing (17).
    hot_x = 9 * scale_blocks;
    hot_y = 12 * scale_blocks;
}

void DrawZoom(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, bool zoom_in, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // Layered bottom-to-top: diagonal handle, then the lens ring over its
    // junction, then the +/- symbol on top of the (transparent) lens interior.
    constexpr float cx = 8.0f, cy = 8.0f, R = 5.5f;

    // Handle: white shaft with black borders, from the lens SE edge to the corner.
    for (int t = 12; t <= 17; ++t) { p.put(t + 1, t - 1, kBlack); p.put(t - 1, t + 1, kBlack); }
    for (int t = 12; t <= 17; ++t) p.put(t, t, kWhite);

    // Lens ring.
    for (int ly = 0; ly < 20; ++ly)
        for (int lx = 0; lx < 20; ++lx) {
            float dx = lx - cx, dy = ly - cy;
            float r  = std::sqrt(dx * dx + dy * dy);
            if (std::fabs(r - R) <= 0.9f)      p.put(lx, ly, kWhite);
            else if (std::fabs(r - R) <= 1.7f) p.put(lx, ly, kBlack);
        }

    // Symbol outline first, then white cores so the bars cross cleanly.
    for (int i = 5; i <= 11; ++i) { p.put(i, 7, kBlack); p.put(i, 9, kBlack); }
    p.put(4, 8, kBlack); p.put(12, 8, kBlack);
    if (zoom_in) {
        for (int i = 5; i <= 11; ++i) { p.put(7, i, kBlack); p.put(9, i, kBlack); }
        p.put(8, 4, kBlack); p.put(8, 12, kBlack);
    }
    for (int i = 5; i <= 11; ++i) p.put(i, 8, kWhite);
    if (zoom_in) for (int i = 5; i <= 11; ++i) p.put(8, i, kWhite);

    // Hotspot at the lens centre (the point that gets zoomed).
    hot_x = 8 * scale_blocks;
    hot_y = 8 * scale_blocks;
}

void DrawWait(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // A centred hourglass (the classic "busy, please wait" glyph).
    FillEroded(p, [](int x, int y) { return HourglassContains(x, y, 9.5f, 3, 16, 6.0f); });

    hot_x = 10 * scale_blocks;
    hot_y = 10 * scale_blocks;
}

void DrawProgress(uint32_t* pixels, uint32_t stride, uint32_t size,
                  int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // "Working, but still interactive": the pointer arrow with a small busy
    // hourglass badge at the lower right. The arrow is the filled triangle with
    // vertices (1,1)-(1,12)-(9,9), pointing up-left like the default cursor.
    FillEroded(p, [](int x, int y) -> bool {
        bool arrow = x >= 1 && x <= 9 && y >= x &&
                     static_cast<float>(y) <= 12.0f - 0.375f * (x - 1);
        bool badge = HourglassContains(x, y, 15.0f, 9, 17, 3.5f);
        return arrow || badge;
    });

    // Hotspot at the arrow tip — clicking still works during progress.
    hot_x = 1 * scale_blocks;
    hot_y = 1 * scale_blocks;
}

void DrawCopy(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    FillEroded(p, PointerContains);
    // Badge: a bold '+' (copy) in the lower right, centred on (14,14).
    FillEroded(p, [](int x, int y) {
        return (std::abs(x - 14) <= 1 && std::abs(y - 14) <= 3) ||
               (std::abs(y - 14) <= 1 && std::abs(x - 14) <= 3);
    });

    hot_x = 1 * scale_blocks;
    hot_y = 1 * scale_blocks;
}

void DrawAlias(uint32_t* pixels, uint32_t stride, uint32_t size,
               int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    FillEroded(p, PointerContains);
    // Badge: a right-pointing arrow (alias/link "refers to") in the lower right.
    FillEroded(p, [](int x, int y) {
        bool shaft = std::abs(y - 14) <= 1 && x >= 10 && x <= 15;
        bool head  = x >= 14 && x <= 17 && std::abs(y - 14) <= (17 - x);
        return shaft || head;
    });

    hot_x = 1 * scale_blocks;
    hot_y = 1 * scale_blocks;
}

void DrawNoDrop(uint32_t* pixels, uint32_t stride, uint32_t size,
                int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    FillEroded(p, PointerContains);
    // Badge: a small "not allowed" ring+slash (⊘) in the lower right.
    for (int ly = 0; ly < 20; ++ly)
        for (int lx = 0; lx < 20; ++lx) {
            int b = RingSlashBand(lx, ly, 14.0f, 14.0f, 3.2f);
            if (b > 0)      p.put(lx, ly, kWhite);
            else if (b < 0) p.put(lx, ly, kBlack);
        }

    hot_x = 1 * scale_blocks;
    hot_y = 1 * scale_blocks;
}

void DrawMove(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // A centred four-way "fleur": a thick cross with an arrowhead on each end.
    FillEroded(p, [](int x, int y) {
        bool vert  = std::abs(x - 9) <= 1 && y >= 4  && y <= 14;
        bool horz  = std::abs(y - 9) <= 1 && x >= 4  && x <= 14;
        bool up    = y >= 1  && y <= 4  && std::abs(x - 9) <= (y - 1);
        bool down  = y >= 14 && y <= 17 && std::abs(x - 9) <= (17 - y);
        bool left  = x >= 1  && x <= 4  && std::abs(y - 9) <= (x - 1);
        bool right = x >= 14 && x <= 17 && std::abs(y - 9) <= (17 - x);
        return vert || horz || up || down || left || right;
    });

    hot_x = 9 * scale_blocks;
    hot_y = 9 * scale_blocks;
}

void DrawCell(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // A thick '+' spanning the grid — the spreadsheet cell-selection cursor.
    // Thicker than the 1px crosshair (8) so the two read differently.
    FillEroded(p, [](int x, int y) {
        return (std::abs(x - 9) <= 1 && y >= 1 && y <= 17) ||
               (std::abs(y - 9) <= 1 && x >= 1 && x <= 17);
    });

    hot_x = 9 * scale_blocks;
    hot_y = 9 * scale_blocks;
}

void DrawAllScroll(uint32_t* pixels, uint32_t stride, uint32_t size,
                   int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // Four arrowheads around a central hub, with a gap between them (unlike the
    // solid-cross fleur of move) — the omnidirectional-scroll cursor.
    FillEroded(p, [](int x, int y) -> bool {
        bool up    = y >= 2  && y <= 5  && std::abs(x - 9) <= (y - 2);
        bool down  = y >= 13 && y <= 16 && std::abs(x - 9) <= (16 - y);
        bool left  = x >= 2  && x <= 5  && std::abs(y - 9) <= (x - 2);
        bool right = x >= 13 && x <= 16 && std::abs(y - 9) <= (16 - x);
        bool hub   = std::abs(x - 9) <= 1 && std::abs(y - 9) <= 1;
        return up || down || left || right || hub;
    });

    hot_x = 9 * scale_blocks;
    hot_y = 9 * scale_blocks;
}

void DrawHelp(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    FillEroded(p, PointerContains);
    // Badge: a '?' in the lower right (a 1px stroke haloed to an outline).
    StampHalo(p, [](int x, int y) -> bool {
        return (y == 9  && x >= 12 && x <= 14) ||   // top curve
               (y == 10 && (x == 11 || x == 15)) || // curve shoulders
               (y == 11 && x == 15) ||              // right side
               (y == 12 && x == 14) ||              // sweep in
               (y == 13 && x == 13) ||              // stem
               (y == 14 && x == 13) ||
               (y == 16 && x == 13);                // dot (gap at y15)
    });

    hot_x = 1 * scale_blocks;
    hot_y = 1 * scale_blocks;
}

void DrawContextMenu(uint32_t* pixels, uint32_t stride, uint32_t size,
                     int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    FillEroded(p, PointerContains);
    // Badge: a small menu (three list rows) in the lower right.
    StampHalo(p, [](int x, int y) {
        return (y == 11 || y == 13 || y == 15) && x >= 11 && x <= 16;
    });

    hot_x = 1 * scale_blocks;
    hot_y = 1 * scale_blocks;
}

void DrawText(uint32_t* pixels, uint32_t stride, uint32_t size,
              int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // Vertical I-beam: top/bottom serifs (thick black caps with a white lip) and
    // a white stem down the middle, bordered by black rails.
    for (int x = 0; x <= 6; ++x) { p.put(x, 0, kBlack); p.put(x, 1, kBlack); p.put(x, 18, kBlack); p.put(x, 19, kBlack); }
    for (int y = 0; y <= 19; ++y) { p.put(0, y, kBlack); p.put(6, y, kBlack); }
    for (int y = 2; y <= 17; ++y) { p.put(1, y, kBlack); p.put(5, y, kBlack); }
    for (int x = 1; x <= 5; ++x) { p.put(x, 1, kWhite); p.put(x, 18, kWhite); }
    for (int y = 2; y <= 17; ++y) p.put(3, y, kWhite);

    hot_x = 3 * scale_blocks;
    hot_y = 9 * scale_blocks;
}

void DrawVerticalText(uint32_t* pixels, uint32_t stride, uint32_t size,
                      int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // The text I-beam transposed 90° into a horizontal beam (left/right serifs).
    for (int x = 0; x <= 6; ++x) { p.put(0, x, kBlack); p.put(1, x, kBlack); p.put(18, x, kBlack); p.put(19, x, kBlack); }
    for (int y = 0; y <= 19; ++y) { p.put(y, 0, kBlack); p.put(y, 6, kBlack); }
    for (int y = 2; y <= 17; ++y) { p.put(y, 1, kBlack); p.put(y, 5, kBlack); }
    for (int x = 1; x <= 5; ++x) { p.put(1, x, kWhite); p.put(18, x, kWhite); }
    for (int y = 2; y <= 17; ++y) p.put(y, 3, kWhite);

    hot_x = 9 * scale_blocks;
    hot_y = 3 * scale_blocks;
}

void DrawCrosshair(uint32_t* pixels, uint32_t stride, uint32_t size,
                   int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // A '+' of thin white lines (black-bordered), broken at the very centre so
    // the target point stays visible.
    for (int i = 0; i <= 18; ++i) {
        if (i >= 8 && i <= 10) continue;
        p.put(i, 8, kBlack); p.put(i, 10, kBlack); p.put(8, i, kBlack); p.put(10, i, kBlack);
        p.put(i, 9, kWhite); p.put(9, i, kWhite);
    }

    hot_x = 9 * scale_blocks;
    hot_y = 9 * scale_blocks;
}

void DrawHResize(uint32_t* pixels, uint32_t stride, uint32_t size,
                 int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // Horizontal double-headed arrow (←→): a white shaft on row 5 with black
    // rails on rows 4/6, and a triangular arrowhead at each end (span shrinks
    // from 4 to 0 as it approaches the tip).
    for (int x = 0; x <= 18; ++x) { p.put(x, 4, kBlack); p.put(x, 6, kBlack); }
    for (int x = 0; x <= 18; ++x) p.put(x, 5, kWhite);
    for (int d = 0; d <= 4; ++d) {
        int span = 4 - d;
        for (int dy = -span; dy <= span; ++dy) {
            p.put(d,      5 + dy, kWhite); p.put(d - 1,  5 + dy, kBlack);
            p.put(18 - d, 5 + dy, kWhite); p.put(19 - d, 5 + dy, kBlack);
        }
    }

    hot_x = 9 * scale_blocks;
    hot_y = 5 * scale_blocks;
}

void DrawVResize(uint32_t* pixels, uint32_t stride, uint32_t size,
                 int scale_blocks, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale_blocks < 1) return;
    const Plotter p{pixels, stride, size, scale_blocks};

    // Vertical double-headed arrow (↑↓): the H-resize arrow transposed.
    for (int y = 0; y <= 18; ++y) { p.put(4, y, kBlack); p.put(6, y, kBlack); }
    for (int y = 0; y <= 18; ++y) p.put(5, y, kWhite);
    for (int d = 0; d <= 4; ++d) {
        int span = 4 - d;
        for (int dx = -span; dx <= span; ++dx) {
            p.put(5 + dx, d,      kWhite); p.put(5 + dx, d - 1,  kBlack);
            p.put(5 + dx, 18 - d, kWhite); p.put(5 + dx, 19 - d, kBlack);
        }
    }

    hot_x = 5 * scale_blocks;
    hot_y = 9 * scale_blocks;
}

void DrawArrow(uint32_t* pixels, uint32_t stride, uint32_t size,
               float scale, int32_t& hot_x, int32_t& hot_y) noexcept {
    if (!pixels || scale <= 0.f) return;

    // Anti-aliased pointer arrow with a soft drop shadow, rendered at a
    // floating-point scale. Each output pixel is mapped back into arrow space,
    // 4x4 supersampled for the fill, and composited shadow → outline → fill.
    hot_x = static_cast<int>(std::round(scale));
    hot_y = hot_x;
    for (uint32_t py = 0; py < size; ++py) {
        for (uint32_t px = 0; px < size; ++px) {
            float ax = (static_cast<float>(px) - hot_x + 0.5f) / scale;
            float ay = (static_cast<float>(py) - hot_y + 0.5f) / scale;

            int n = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    if (ArrowContains(ax + (sx + 0.5f) * 0.25f / scale,
                                      ay + (sy + 0.5f) * 0.25f / scale))
                        ++n;
            float fill = n / 16.f;

            bool  is_in   = ArrowContains(ax, ay);
            float dist    = ArrowBoundaryDist(ax, ay) * scale;
            float outline = (!is_in && dist < 1.5f) ? (1.f - dist / 1.5f) : 0.f;

            float sax = ax - 2.5f / scale, say = ay - 3.5f / scale;
            float s_d = (ArrowContains(sax, say) ? 0.f
                         : ArrowBoundaryDist(sax, say)) * scale;
            float shadow = std::exp(-s_d / 3.f) * 0.38f;

            float alpha_dark = 1.f - (1.f - shadow) * (1.f - outline);
            float fill_over  = fill * (1.f - alpha_dark);
            float A = alpha_dark + fill_over;
            if (A < 0.004f) continue;
            float R = fill_over / A;

            auto u8 = [](float v) -> uint8_t {
                return static_cast<uint8_t>(std::clamp(v * 255.f, 0.f, 255.f));
            };
            pixels[py * stride + px] =
                (static_cast<uint32_t>(u8(A)) << 24) |
                (static_cast<uint32_t>(u8(R)) << 16) |
                (static_cast<uint32_t>(u8(R)) <<  8) |
                 static_cast<uint32_t>(u8(R));
        }
    }
}

} // namespace fbx::xserver
