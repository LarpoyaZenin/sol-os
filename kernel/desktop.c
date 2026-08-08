#include "desktop.h"
#include "framebuffer.h"
#include "font.h"
#include "klog.h"
#include "mm/kheap.h"
#include "arch/x86_64/timer.h"
#include "arch/x86_64/rtc.h"
#include "drivers/virtio/virtio_input.h"
#include "string.h"
#include <stddef.h>
#include <stdint.h>

/* ---- layout constants ---- */

#define TASKBAR_H     48u
#define START_W       96u
#define START_H       34u
#define CLOCK_MARGIN  16u
#define TASK_WIN_W    150u
#define TASK_WIN_H    34u
#define TASK_GAP      8u

#define CURSOR_W      12u
#define CURSOR_H      20u

#define TITLE_H       26u
#define BTN_W         22u
#define BTN_H         20u
#define BTN_MARGIN    4u
#define BTN_GAP       2u

#define MAX_WINDOWS   8u
#define MAX_WIN_LINES 8u

#define MIN_WIN_W     200
#define MIN_WIN_H     120

/* Bottom-right corner grab area for window resizing. */
#define RESIZE_GUTTER 18

/* Start menu geometry. The menu pops up above the Start button, left
 * aligned with it, with one row per desktop app. */
#define MENU_W        200u
#define MENU_ITEM_H   40u
#define MENU_PAD      6u

/* Terminal application. Each terminal window owns a fixed scrollback
 * buffer of TERM_SCROLL_MAX lines (static, never heap-allocated, so
 * the heap watermark stays flat) plus the current input line. */
#define TERM_SCROLL_MAX 80
#define TERM_LINE_MAX   88
#define TERM_ROW_H      10
#define TERM_BG        0x00131720u
#define TERM_TEXT      0x00FFFFFFu
#define TERM_PROMPT    "solos@sol$ "
#define TERM_PROMPT_LEN 11u

#define ICON_W        72u
#define ICON_H        72u
#define ICON_X0       24
#define ICON_Y0       24
#define ICON_ROW_STEP 98
#define ICON_LABEL_H  16u

/* ---- palette ---- */

#define BG_TOP        0x003A5C8Au
#define BG_BOTTOM     0x00121B2Eu
#define TASKBAR_BG    0x001F2A3Cu
#define TASKBAR_EDGE  0x00F5A623u
#define START_BG      0x00F5A623u
#define START_TEXT    0x001B2A4Au
#define CLOCK_TEXT    0x00FFFFFFu
#define TASKBAR_BTN_BG    0x00313E55u
#define TASKBAR_BTN_ACT   0x00F5A623u
#define TASKBAR_BTN_TEXT  0x00FFFFFFu
#define TASKBAR_BTN_TACT  0x001B2A4Au
#define CURSOR_OUTLINE 0x00000000u
#define CURSOR_FILL   0x00FFFFFFu

#define WIN_BORDER    0x000B1119u
#define TITLE_ACTIVE  0x00F5A623u
#define TITLE_ACT_TEXT 0x001B2A4Au
#define TITLE_INACT   0x00505E6Eu
#define TITLE_INACT_TEXT 0x00FFFFFFu
#define BTN_BG_ACT    0x00D98E17u
#define BTN_BG_INACT  0x00424F5Eu
#define BTN_GLYPH     0x00FFFFFFu
#define BODY_BG       0x00F4F6F9u
#define BODY_TEXT     0x001B2A4Au

/* ---- state ---- */

static struct limine_framebuffer *g_fb;
static uint64_t g_w, g_h, g_pitch;
static uint8_t *g_fbaddr;
static uint32_t *g_bb;          /* full-screen backbuffer */
static uint64_t g_bb_bytes;     /* g_w * g_h * 4 */

/* Backbuffer guard: the allocation is bb_bytes + 64, and the trailing
 * words are stamped with canaries. Any out-of-bounds backbuffer write
 * (e.g. a buggy row/extent computation) clobbers a canary and is
 * detected by desktop_gfx_integrity(). */
#define BB_GUARD_BYTES 64u
#define BB_CANARIES    16u
#define BB_CANARY_BASE 0xC0FFEE00u
static uint32_t g_bb_canary[BB_CANARIES];

static int64_t g_cursor_x;
static int64_t g_cursor_y;
static uint8_t g_buttons;
static uint8_t g_buttons_prev;
static uint64_t g_last_second;

struct desktop_window {
    int used;
    int64_t x, y;
    int64_t w, h;
    const char *title;
    const char *lines[MAX_WIN_LINES];
    int nlines;
    uint32_t order;
    int minimized;
    int maximized;
    int64_t rest_x, rest_y;
    int64_t rest_w, rest_h;
    int kind;                      /* 0 = info window, 1 = terminal */
    char term_lines[TERM_SCROLL_MAX][TERM_LINE_MAX];
    int term_len;
    int term_scroll;               /* lines scrolled up from the bottom */
    char term_input[TERM_LINE_MAX];
    int term_input_len;
    int term_cursor_col;
};

static struct desktop_window g_wins[MAX_WINDOWS];
static uint32_t g_order_counter;
static int g_drag;               /* window index being dragged, -1 = none */
static int64_t g_drag_off_x;
static int64_t g_drag_off_y;
static int g_resize;             /* window index being resized, -1 = none */
static int64_t g_resize_x;       /* cursor position where the resize began */
static int64_t g_resize_y;
static int64_t g_resize_w;       /* window size when the resize began */
static int64_t g_resize_h;
static int g_start_menu;         /* 1 when the Start menu is open */
static unsigned g_icon_opens;

/* ---- desktop icons ---- */

struct desktop_icon {
    const char *label;
    char glyph;
    uint32_t color;
    int kind;                      /* 0 = info window, 1 = terminal */
    const char *win_title;
    const char *const *win_lines;
    int nlines;
};

static const char *const ic_terminal_lines[] = {
    "Sol OS terminal",
    "A shell lands in a later phase.",
    "For now this window exists to",
    "prove the desktop can launch.",
};
static const char *const ic_files_lines[] = {
    "Files",
    "Home",
    "  Desktop",
    "  Documents",
    "  Downloads",
    "  Pictures",
    "  Music",
    "  Videos",
};
static const char *const ic_settings_lines[] = {
    "Settings",
    "  Resolution: 1920x1080",
    "  Color depth: 32 bpp",
    "Appearance",
    "  Theme: Sol OS default",
    "System",
    "  Architecture: x86_64",
    "  Sol OS version 0.1",
};
static const char *const ic_about_lines[] = {
    "Sol OS",
    "Version 0.1",
    "Architecture: x86_64",
    "Resolution: 1920x1080",
    "Bootloader: Limine",
    "Welcome to Sol OS.",
};

static const struct desktop_icon g_icons[] = {
    { "Terminal", '>', 0x002E4C73, 1, "Terminal",    ic_terminal_lines, 4 },
    { "Files",    'F', 0x00F5A623, 0, "Files",       ic_files_lines,   8 },
    { "Settings", 'S', 0x003AAFA9, 0, "Settings",    ic_settings_lines, 8 },
    { "About",    'A', 0x006B5B95, 0, "About Sol OS", ic_about_lines,  6 },
};
#define ICON_COUNT (unsigned)(sizeof(g_icons) / sizeof(g_icons[0]))

/* 0 = transparent, 1 = black outline, 2 = white fill. */
static const uint8_t cursor_bmp[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {0,0,0,1,2,2,2,2,2,2,2,1},
    {0,0,0,0,1,2,2,2,2,2,1,0},
    {0,0,0,0,0,1,2,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,1,0,0,0},
};

static int64_t work_h(void) { return (int64_t)g_h - TASKBAR_H; }

static int64_t i64_min(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t i64_max(int64_t a, int64_t b) { return a > b ? a : b; }

/* Forward declarations (defined later in this file). */
static void render_taskbar(void);
static void render_start_menu(void);
static void win_render(struct desktop_window *w);
static int  win_rect_intersects(const struct desktop_window *w,
                                int64_t x, int64_t y, int64_t ww, int64_t hh);
static void redraw_rect(int64_t x, int64_t y, int64_t w, int64_t h);
static void redraw_taskbar(void);
static int  win_count(void);
static int  win_is_topmost_win(const struct desktop_window *w);
static int  win_topmost_at(int64_t px, int64_t py);
static int  win_open(const char *title, int64_t x, int64_t y,
                     int64_t w, int64_t h, int kind,
                     const char *const *lines, int nlines);
static void term_append_line(struct desktop_window *w, const char *s);
static void term_render(struct desktop_window *w, int active);
static void close_start_menu(void);
static void open_start_menu(void);
static int  menu_x(void);
static int  menu_y(void);
static int  menu_h(void);

/* ---- backbuffer primitives (scene, no cursor) ---- */

/* Active clip rectangle for backbuffer writes. scene_region() scopes
 * every primitive it issues (gradient, icons, windows, taskbar, start
 * menu) to the damaged region it is recompositing, so nothing is ever
 * painted into the backbuffer outside the pixels that will be blitted.
 *
 * Without this clip, a scene element that is drawn regardless of the
 * damaged region (the desktop icons are the prime offender) can stamp
 * itself over a window that happens to cover it in the backbuffer but
 * is not part of this redraw. The backbuffer is then wrong and the
 * damage shows up later, whenever cursor_restore() blits the region:
 * that is the stray-pixel / ghosting-trail / random-line artifact. */
static int      g_bb_clip_active;
static int64_t  g_bb_clip_x0, g_bb_clip_y0, g_bb_clip_x1, g_bb_clip_y1;

/* Clips a rect to the backbuffer [0,g_w)x[0,g_h), and additionally to
 * the active clip rect when one is set. Overflow-safe: extents are
 * saturated to the buffer bounds rather than summed. Returns 1 and
 * fills the exclusive bounds when non-empty. */
static int bb_clip(int64_t x, int64_t y, int64_t w, int64_t h,
                   int64_t *x0, int64_t *y0, int64_t *x1, int64_t *y1) {
    if (w <= 0 || h <= 0) return 0;

    /* Left edge: clip negative to 0. */
    int64_t left = x < 0 ? 0 : x;
    if (left >= (int64_t)g_w) return 0;

    /* Right edge computed from the UNCLIPPED x so negative origins
     * shrink the extent correctly, and saturated so x + w can never
     * overflow (when x >= 0 it is only added when the sum < g_w; when
     * x < 0 the sum cannot overflow since x is negative). */
    int64_t right;
    if (x >= 0) {
        right = (w >= (int64_t)g_w - x) ? (int64_t)g_w : x + w;
    } else {
        right = x + w;
        if (right > (int64_t)g_w) right = (int64_t)g_w;
    }
    if (right <= left) return 0;

    int64_t top = y < 0 ? 0 : y;
    if (top >= (int64_t)g_h) return 0;

    int64_t bottom;
    if (y >= 0) {
        bottom = (h >= (int64_t)g_h - y) ? (int64_t)g_h : y + h;
    } else {
        bottom = y + h;
        if (bottom > (int64_t)g_h) bottom = (int64_t)g_h;
    }
    if (bottom <= top) return 0;

    *x0 = left;
    *y0 = top;
    *x1 = right;
    *y1 = bottom;

    if (g_bb_clip_active) {
        if (*x0 < g_bb_clip_x0) *x0 = g_bb_clip_x0;
        if (*y0 < g_bb_clip_y0) *y0 = g_bb_clip_y0;
        if (*x1 > g_bb_clip_x1) *x1 = g_bb_clip_x1;
        if (*y1 > g_bb_clip_y1) *y1 = g_bb_clip_y1;
        if (*x1 <= *x0 || *y1 <= *y0) return 0;
    }
    return 1;
}

static void bb_put_pixel(int64_t x, int64_t y, uint32_t c) {
    if (x < 0 || y < 0 || x >= (int64_t)g_w || y >= (int64_t)g_h) return;
    if (g_bb_clip_active &&
        (x < g_bb_clip_x0 || x >= g_bb_clip_x1 ||
         y < g_bb_clip_y0 || y >= g_bb_clip_y1)) return;
    g_bb[(uint64_t)y * g_w + (uint64_t)x] = c;
}

static void bb_fill_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t c) {
    int64_t x0, y0, x1, y1;
    if (!bb_clip(x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (int64_t yy = y0; yy < y1; yy++) {
        uint32_t *row = g_bb + (uint64_t)yy * g_w;
        for (int64_t xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static void bb_draw_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    bb_fill_rect(x, y, w, 1, c);
    bb_fill_rect(x, y + h - 1, w, 1, c);
    bb_fill_rect(x, y, 1, h, c);
    bb_fill_rect(x + w - 1, y, 1, h, c);
}

static void bb_clear_region(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t c) {
    bb_fill_rect(x, y, w, h, c);
}

static void bb_draw_char(int64_t x, int64_t y, char c, uint32_t fg) {
    const uint8_t *glyph = font8x8_basic[(uint8_t)c];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        if (bits == 0) continue;
        for (int col = 0; col < FONT_W; col++) {
            if ((bits >> col) & 1u) bb_put_pixel(x + col, y + row, fg);
        }
    }
}

static int64_t bb_draw_string(int64_t x, int64_t y, const char *s, uint32_t fg) {
    while (*s) {
        bb_draw_char(x, y, *s, fg);
        x += FONT_W;
        s++;
    }
    return x;
}

/* ---- blits (backbuffer -> framebuffer) ---- */

static void blit_rect(int64_t x, int64_t y, int64_t w, int64_t h) {
    int64_t x0, y0, x1, y1;
    if (!bb_clip(x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (int64_t yy = y0; yy < y1; yy++) {
        uint8_t *dst = g_fbaddr + (uint64_t)yy * g_pitch + (uint64_t)x0 * 4u;
        const uint8_t *src = (const uint8_t *)(g_bb + (uint64_t)yy * g_w) + (uint64_t)x0 * 4u;
        memcpy(dst, src, (size_t)((x1 - x0) * 4));
    }
}

static void blit_full(void) {
    blit_rect(0, 0, (int64_t)g_w, (int64_t)g_h);
}

/* ---- cursor ---- */

static void cursor_restore(void) {
    blit_rect(g_cursor_x, g_cursor_y, CURSOR_W, CURSOR_H);
}

static void cursor_draw(void) {
    for (unsigned row = 0; row < CURSOR_H; row++) {
        for (unsigned col = 0; col < CURSOR_W; col++) {
            uint8_t v = cursor_bmp[row][col];
            if (v == 0) continue;
            fb_put_pixel(g_cursor_x + (int64_t)col,
                         g_cursor_y + (int64_t)row,
                         v == 1 ? CURSOR_OUTLINE : CURSOR_FILL);
        }
    }
}

/* ---- background ---- */

static uint32_t gradient_color(uint64_t y, uint64_t h) {
    uint32_t r0 = (BG_TOP >> 16) & 0xFFu, g0 = (BG_TOP >> 8) & 0xFFu, b0 = BG_TOP & 0xFFu;
    uint32_t r1 = (BG_BOTTOM >> 16) & 0xFFu, g1 = (BG_BOTTOM >> 8) & 0xFFu, b1 = BG_BOTTOM & 0xFFu;
    uint32_t t = (h <= 1) ? 0u : (uint32_t)(y * 255u / (h - 1));
    uint32_t r = r0 + (r1 - r0) * t / 255u;
    uint32_t g = g0 + (g1 - g0) * t / 255u;
    uint32_t b = b0 + (b1 - b0) * t / 255u;
    return 0x00000000u | (r << 16) | (g << 8) | b;
}

/* ---- clock ---- */

static void put_digits(char *buf, size_t *pos, int v, int digits) {
    char tmp[8];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 6) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n < digits) tmp[n++] = '0';
    while (n > 0) buf[(*pos)++] = tmp[--n];
}

static const char *g_months[12] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec",
};

static void build_clock_str(char *buf) {
    struct rtc_datetime dt;
    if (!rtc_read(&dt)) {
        strcpy(buf, "--:--:--");
        return;
    }
    size_t i = 0;
    put_digits(buf, &i, dt.hour, 2);
    buf[i++] = ':';
    put_digits(buf, &i, dt.minute, 2);
    buf[i++] = ':';
    put_digits(buf, &i, dt.second, 2);
    buf[i++] = ' ';
    const char *m = (dt.month >= 1 && dt.month <= 12) ? g_months[dt.month - 1] : "???";
    while (*m) buf[i++] = *m++;
    buf[i++] = ' ';
    put_digits(buf, &i, dt.day, 2);
    buf[i++] = ',';
    buf[i++] = ' ';
    put_digits(buf, &i, dt.year, 4);
    buf[i] = 0;
}

/* ---- window management ---- */

static int win_is_topmost(int idx) {
    uint32_t best = 0;
    int besti = -1;
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        const struct desktop_window *w = &g_wins[i];
        if (!w->used || w->minimized) continue;
        if (besti < 0 || w->order > best) {
            best = w->order;
            besti = (int)i;
        }
    }
    return besti == idx;
}

static int win_topmost_index(void) {
    uint32_t best = 0;
    int besti = -1;
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        const struct desktop_window *w = &g_wins[i];
        if (!w->used || w->minimized) continue;
        if (besti < 0 || w->order > best) {
            best = w->order;
            besti = (int)i;
        }
    }
    return besti;
}

static int win_count(void) {
    int n = 0;
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used) n++;
    }
    return n;
}

static int win_contains(const struct desktop_window *w, int64_t px, int64_t py) {
    return px >= w->x && py >= w->y && px < w->x + w->w && py < w->y + w->h;
}

static char win_button_at(const struct desktop_window *w, int64_t px, int64_t py) {
    if (!win_contains(w, px, py)) return 0;
    if (py < w->y || py >= w->y + TITLE_H) return 0;
    int64_t by = w->y + 3;
    if (py < by || py >= by + BTN_H) return 0;
    int64_t c0 = w->x + w->w - BTN_MARGIN - BTN_W;
    int64_t m0 = c0 - BTN_W - BTN_GAP;
    int64_t n0 = m0 - BTN_W - BTN_GAP;
    if (px >= c0) return 'c';
    if (px >= m0) return 'x';
    if (px >= n0) return 'm';
    return 0;
}

/* ---- desktop icons (drawn on the background) ---- */

static void icon_draw(void) {
    for (unsigned i = 0; i < ICON_COUNT; i++) {
        int64_t ix = ICON_X0;
        int64_t iy = ICON_Y0 + (int64_t)i * ICON_ROW_STEP;
        bb_fill_rect(ix, iy, ICON_W, ICON_H, WIN_BORDER);
        bb_fill_rect(ix + 2, iy + 2, ICON_W - 4, ICON_H - 4, g_icons[i].color);
        bb_draw_char(ix + (ICON_W - FONT_W) / 2, iy + (ICON_H - FONT_H) / 2,
                     g_icons[i].glyph, 0x00FFFFFFu);
        const char *label = g_icons[i].label;
        int64_t lw = (int64_t)strlen(label) * FONT_W;
        bb_draw_string(ix + (ICON_W - lw) / 2, iy + ICON_H + 6, label, 0x00FFFFFFu);
    }
}

static int icon_at(int64_t px, int64_t py) {
    for (unsigned i = 0; i < ICON_COUNT; i++) {
        int64_t ix = ICON_X0;
        int64_t iy = ICON_Y0 + (int64_t)i * ICON_ROW_STEP;
        if (px >= ix && px < ix + (int64_t)ICON_W &&
            py >= iy && py < iy + (int64_t)ICON_H + ICON_LABEL_H) {
            return (int)i;
        }
    }
    return -1;
}

static void icon_open(int idx) {
    const struct desktop_icon *ic = &g_icons[idx];
    int64_t x = 300 + (int64_t)(g_icon_opens % 4) * 36;
    int64_t y = 90 + (int64_t)(g_icon_opens % 4) * 28;
    g_icon_opens++;
    int wi = win_open(ic->win_title, x, y, 460, 220, ic->kind,
                      ic->win_lines, ic->nlines);
    if (wi >= 0) {
        redraw_rect(g_wins[wi].x, g_wins[wi].y, g_wins[wi].w, g_wins[wi].h);
        redraw_taskbar();
    }
}

/* Re-renders the whole scene region [x,y)..[x+w,y+h) into the
 * backbuffer: background + icons, then every window that intersects
 * it in z-order, then the taskbar strip if it intersects. The caller
 * blits whatever it needs. */
static void scene_region(int64_t x, int64_t y, int64_t w, int64_t h) {
    int64_t x0, y0, x1, y1;
    if (!bb_clip(x, y, w, h, &x0, &y0, &x1, &y1)) return;

    /* Scope every backbuffer write in this composite to the damaged
     * region [x0,x1) x [y0,y1). Window rendering paints only the
     * overlapping pixels too: since windows are composited in z-order
     * and any window covering a damaged pixel necessarily intersects
     * the region, the result is still correct occlusion, but no
     * full-scene element (e.g. the icons) can clobber a window in the
     * backbuffer it is not repainting. */
    int clip_was_active = g_bb_clip_active;
    int64_t sx0 = g_bb_clip_x0, sy0 = g_bb_clip_y0;
    int64_t sx1 = g_bb_clip_x1, sy1 = g_bb_clip_y1;
    g_bb_clip_active = 1;
    g_bb_clip_x0 = x0; g_bb_clip_y0 = y0;
    g_bb_clip_x1 = x1; g_bb_clip_y1 = y1;

    for (int64_t yy = y0; yy < y1; yy++) {
        uint32_t c = gradient_color((uint64_t)yy, g_h);
        uint32_t *row = g_bb + (uint64_t)yy * g_w;
        for (int64_t xx = x0; xx < x1; xx++) row[xx] = c;
    }

    icon_draw();

    /* Windows in z-order (ascending order field). */
    unsigned sorted[MAX_WINDOWS];
    int n = 0;
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].used || g_wins[i].minimized) continue;
        int j = n;
        while (j > 0 && g_wins[sorted[j - 1]].order > g_wins[i].order) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = i;
        n++;
    }

    for (int k = 0; k < n; k++) {
        struct desktop_window *win = &g_wins[sorted[k]];
        if (!win_rect_intersects(win, x, y, w, h)) continue;
        win_render(win);
    }

    if (y1 > work_h()) {
        render_taskbar();
    }

    /* The Start menu floats above everything else when open. */
    if (g_start_menu) {
        int64_t mx = menu_x();
        int64_t my = menu_y();
        if (x < mx + (int64_t)MENU_W && mx < x + w &&
            y < my + menu_h() && my < y + h) {
            render_start_menu();
        }
    }

    g_bb_clip_active = clip_was_active;
    g_bb_clip_x0 = sx0; g_bb_clip_y0 = sy0;
    g_bb_clip_x1 = sx1; g_bb_clip_y1 = sy1;
}

static int win_rect_intersects(const struct desktop_window *win,
                               int64_t x, int64_t y, int64_t w, int64_t h) {
    return win->x < x + w && x < win->x + win->w &&
           win->y < y + h && y < win->y + win->h;
}

/* ---- graphics integrity ---- */

/* Verifies the backbuffer guard canaries and window geometry
 * invariants. Logs loudly on corruption. Returns 1 when all checks
 * pass. Used by the graphics selftest and polled periodically so any
 * out-of-bounds draw shows up in the serial log. */
static int desktop_gfx_integrity(void) {
    int ok = 1;
    for (unsigned i = 0; i < BB_CANARIES; i++) {
        uint32_t want = BB_CANARY_BASE + i;
        if (g_bb_canary[i] != want) {
            klog("[gfx] CANARY %u CORRUPTED: got %x want %x\n",
                 i, g_bb_canary[i], want);
            ok = 0;
        }
    }
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        const struct desktop_window *w = &g_wins[i];
        if (!w->used) continue;
        if (w->x < 0 || w->y < 0 ||
            w->w <= 0 || w->h <= 0 ||
            w->x + w->w > (int64_t)g_w ||
            w->y + w->h > (int64_t)g_h) {
            klog("[gfx] window %u geometry corrupted: %ld,%ld %ldx%ld\n",
                 i, (long)w->x, (long)w->y, (long)w->w, (long)w->h);
            ok = 0;
        }
    }
    return ok;
}

/* Graphics selftest: exercises bb_fill_rect / bb_draw_char /
 * bb_draw_string against edge-case coordinates and confirms, by
 * reading the backbuffer back, that every operation clipped exactly
 * to the framebuffer bounds. Runs inside desktop_init before the
 * scene is drawn (the scene then overwrites the test pixels), so it
 * never disturbs the real desktop. A FAIL here means the graphics
 * stack is not safe to use. */
static int gfx_selftest(void) {
    int pass = 1;

    bb_clear_region(0, 0, (int64_t)g_w, (int64_t)g_h, 0x00000000u);

    /* Half-off-screen rects: clip to the visible part. */
    bb_fill_rect(-10, 5, 40, 30, 0x00111111u);
    if (g_bb[5 * g_w + 0] != 0x00111111u) pass = 0;
    if (g_bb[5 * g_w + 29] != 0x00111111u) pass = 0;
    if (g_bb[5 * g_w + 30] != 0x00000000u) pass = 0;

    bb_fill_rect((int64_t)g_w - 5, 10, 40, 30, 0x00222222u);
    if (g_bb[10 * g_w + (g_w - 5)] != 0x00222222u) pass = 0;
    if (g_bb[10 * g_w + (g_w - 1)] != 0x00222222u) pass = 0;

    bb_fill_rect(10, -10, 40, 30, 0x00333333u);
    if (g_bb[0 * g_w + 20] != 0x00333333u) pass = 0;

    bb_fill_rect(10, (int64_t)g_h - 5, 40, 30, 0x00444444u);
    if (g_bb[(g_h - 5) * g_w + 20] != 0x00444444u) pass = 0;
    if (g_bb[(g_h - 1) * g_w + 20] != 0x00444444u) pass = 0;

    /* Oversized, zero-size and negative-size rects must be inert. */
    bb_fill_rect(-500, -500, 10000, 10000, 0x00555555u);
    if (g_bb[(g_h - 1) * g_w + (g_w - 1)] != 0x00555555u) pass = 0;
    if (g_bb[0 * g_w + 0] != 0x00555555u) pass = 0;

    bb_fill_rect(100, 50, 0, 0, 0x00666666u);
    if (g_bb[50 * g_w + 100] != 0x00555555u) pass = 0;

    bb_fill_rect(100, 50, -20, -20, 0x00666666u);
    if (g_bb[50 * g_w + 100] != 0x00555555u) pass = 0;

    /* Fully off-screen rects must do nothing. */
    bb_fill_rect(-100, -100, 10, 10, 0x00777777u);
    bb_fill_rect((int64_t)g_w, 0, 100, 100, 0x00777777u);
    bb_fill_rect(0, (int64_t)g_h, 100, 100, 0x00777777u);
    if (g_bb[0 * g_w + 0] != 0x00555555u) pass = 0;

    /* Glyphs partially off-screen must not write out of bounds. */
    bb_draw_char(-3, -3, 'A', 0x00888888u);
    bb_draw_string((int64_t)g_w - 10, (int64_t)g_h - 6, "edge", 0x00888888u);

    if (!desktop_gfx_integrity()) pass = 0;

    klog("[gfx] selftest: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

static void redraw_rect(int64_t x, int64_t y, int64_t w, int64_t h) {
    scene_region(x, y, w, h);
    blit_rect(x, y, w, h);
}

static void redraw_union(int64_t x0, int64_t y0, int64_t w0, int64_t h0,
                         int64_t x1, int64_t y1, int64_t w1, int64_t h1) {
    int64_t ux = i64_min(x0, x1);
    int64_t uy = i64_min(y0, y1);
    int64_t ux1 = i64_max(x0 + w0, x1 + w1);
    int64_t uy1 = i64_max(y0 + h0, y1 + h1);
    if (ux1 <= ux || uy1 <= uy) return;
    redraw_rect(ux, uy, ux1 - ux, uy1 - uy);
}

static void redraw_taskbar(void) {
    redraw_rect(0, work_h(), (int64_t)g_w, TASKBAR_H);
}

/* The active (topmost) window's title is drawn orange; every other window's
 * title is drawn inactive gray. A change of topmost therefore alters the
 * appearance of *all* window title bars, even ones far from the dirty rect.
 * Repaint every visible title bar whenever z-order/state changes. */
static void redraw_all_titles(void) {
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        const struct desktop_window *w = &g_wins[i];
        if (!w->used || w->minimized) continue;
        redraw_rect(w->x, w->y, w->w, TITLE_H);
    }
}

/* ---- taskbar ---- */

static int64_t start_x(void) { return 8; }
static int64_t start_y(void) { return (int64_t)g_h - TASKBAR_H + (TASKBAR_H - START_H) / 2; }

static int64_t task_btn_x(unsigned idx) {
    return 112 + (int64_t)idx * (TASK_WIN_W + TASK_GAP);
}

static void render_taskbar(void) {
    int64_t y = (int64_t)g_h - TASKBAR_H;
    bb_fill_rect(0, y, (int64_t)g_w, TASKBAR_H, TASKBAR_BG);
    bb_fill_rect(0, y, (int64_t)g_w, 1, TASKBAR_EDGE);

    bb_fill_rect(start_x(), start_y(), START_W, START_H, START_BG);
    bb_draw_string(start_x() + 12, start_y() + (START_H - FONT_H) / 2, "Start", START_TEXT);

    int slot = 0;
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        const struct desktop_window *w = &g_wins[i];
        if (!w->used) continue;
        int active = win_is_topmost((int)i);
        int64_t bx = task_btn_x(slot);
        int64_t by = start_y();
        bb_fill_rect(bx, by, TASK_WIN_W, TASK_WIN_H,
                     active ? TASKBAR_BTN_ACT : TASKBAR_BTN_BG);
        int64_t tx = bx + 8;
        int64_t ty = by + (TASK_WIN_H - FONT_H) / 2;
        bb_draw_string(tx, ty, w->title,
                       active ? TASKBAR_BTN_TACT : TASKBAR_BTN_TEXT);
        slot++;
    }

    char buf[40];
    build_clock_str(buf);
    size_t tw = strlen(buf) * FONT_W;
    int64_t cx = (int64_t)g_w - CLOCK_MARGIN - (int64_t)tw;
    int64_t cy = y + (TASKBAR_H - FONT_H) / 2;
    bb_draw_string(cx, cy, buf, CLOCK_TEXT);
}

static void render_clock(void) {
    char buf[40];
    build_clock_str(buf);
    size_t tw = strlen(buf) * FONT_W;
    int64_t x = (int64_t)g_w - CLOCK_MARGIN - (int64_t)tw;
    int64_t y = (int64_t)g_h - TASKBAR_H + (TASKBAR_H - FONT_H) / 2;
    int64_t pad = 8;
    bb_clear_region(x - pad, (int64_t)g_h - TASKBAR_H + 4,
                    (int64_t)tw + 2 * pad, TASKBAR_H - 8, TASKBAR_BG);
    bb_draw_string(x, y, buf, CLOCK_TEXT);
    blit_rect(x - pad, (int64_t)g_h - TASKBAR_H + 4,
              (int64_t)tw + 2 * pad, TASKBAR_H - 8);
}

/* ---- Start menu ---- */

static int menu_x(void) { return 8; }

static int menu_h(void) {
    return (int)(MENU_PAD * 2 + ICON_COUNT * MENU_ITEM_H);
}

static int menu_y(void) { return (int)work_h() - menu_h() - 4; }

static void render_start_menu(void) {
    int64_t mx = menu_x();
    int64_t my = menu_y();
    bb_fill_rect(mx, my, MENU_W, menu_h(), TASKBAR_BG);
    bb_draw_rect(mx, my, MENU_W, menu_h(), WIN_BORDER);

    for (unsigned i = 0; i < ICON_COUNT; i++) {
        int64_t iy = my + MENU_PAD + (int64_t)i * MENU_ITEM_H;
        bb_fill_rect(mx + MENU_PAD, iy, MENU_W - 2 * MENU_PAD,
                     MENU_ITEM_H - 6, TASKBAR_BTN_BG);
        int64_t ty = iy + (MENU_ITEM_H - 6 - FONT_H) / 2;
        bb_draw_char(mx + MENU_PAD + 8, ty, g_icons[i].glyph, 0x00FFFFFFu);
        bb_draw_string(mx + MENU_PAD + 30, ty, g_icons[i].label, CLOCK_TEXT);
    }
}

static void close_start_menu(void) {
    if (!g_start_menu) return;
    g_start_menu = 0;
    redraw_rect(menu_x(), menu_y(), MENU_W, menu_h());
}

static void open_start_menu(void) {
    if (g_start_menu) return;
    g_start_menu = 1;
    redraw_rect(menu_x(), menu_y(), MENU_W, menu_h());
}

/* ---- terminal application ---- */

static void sys_reboot(void) {
    /* PS/2 controller reset pulse: asks the machine to reboot. */
    klog("[terminal] reboot requested\n");
    __asm__ volatile ("cli");
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    for (;;) { __asm__ volatile ("hlt"); }
}

static char *ul_to_str(unsigned long v, char *dst) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 22) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) *dst++ = tmp[--n];
    *dst = 0;
    return dst;
}

static void term_append_line(struct desktop_window *w, const char *s) {
    if (w->term_len >= TERM_SCROLL_MAX) {
        memmove(w->term_lines[0], w->term_lines[1],
                (size_t)(TERM_SCROLL_MAX - 1) * TERM_LINE_MAX);
        w->term_len = TERM_SCROLL_MAX - 1;
    }
    char *dst = w->term_lines[w->term_len];
    int n = 0;
    while (s != NULL && *s && n < TERM_LINE_MAX - 1) { dst[n++] = *s++; }
    dst[n] = 0;
    w->term_len++;
    w->term_scroll = 0;   /* new output pulls the view back to the bottom */
}

static void term_print(struct desktop_window *w, const char *s) {
    term_append_line(w, s);
}

static void term_print_time(struct desktop_window *w) {
    struct rtc_datetime dt;
    char buf[24];
    if (!rtc_read(&dt)) {
        term_print(w, "Time: --:--:--");
        return;
    }
    size_t i = 0;
    buf[i++] = 'T'; buf[i++] = 'i'; buf[i++] = 'm'; buf[i++] = 'e';
    buf[i++] = ':'; buf[i++] = ' ';
    put_digits(buf, &i, dt.hour, 2);   buf[i++] = ':';
    put_digits(buf, &i, dt.minute, 2); buf[i++] = ':';
    put_digits(buf, &i, dt.second, 2);
    buf[i] = 0;
    term_print(w, buf);
}

static void term_print_date(struct desktop_window *w) {
    struct rtc_datetime dt;
    char buf[32];
    if (!rtc_read(&dt)) {
        term_print(w, "Date: --/--/----");
        return;
    }
    const char *m = (dt.month >= 1 && dt.month <= 12) ? g_months[dt.month - 1] : "???";
    size_t i = 0;
    buf[i++] = 'D'; buf[i++] = 'a'; buf[i++] = 't'; buf[i++] = 'e';
    buf[i++] = ':'; buf[i++] = ' ';
    while (*m) buf[i++] = *m++;
    buf[i++] = ' ';
    put_digits(buf, &i, dt.day, 2);
    buf[i++] = ',';
    buf[i++] = ' ';
    put_digits(buf, &i, dt.year, 4);
    buf[i] = 0;
    term_print(w, buf);
}

static void term_print_mem(struct desktop_window *w) {
    char buf[96];
    char *p = buf;
    const char *pre = "Heap: ";
    while (*pre) *p++ = *pre++;
    p = ul_to_str(kheap_used_bytes() / 1024, p);
    const char *mid = " KiB used, ";
    while (*mid) *p++ = *mid++;
    p = ul_to_str(kheap_free_bytes() / 1024, p);
    const char *post = " KiB free";
    while (*post) *p++ = *post++;
    *p = 0;
    term_print(w, buf);
}

static void term_exec(struct desktop_window *w) {
    char echo[TERM_LINE_MAX];
    char cmd[TERM_LINE_MAX];

    /* Echo the command line (prompt + typed text) into the scrollback. */
    int n = 0;
    const char *pr = TERM_PROMPT;
    while (*pr && n < TERM_LINE_MAX - 1) echo[n++] = *pr++;
    for (int i = 0; i < w->term_input_len && n < TERM_LINE_MAX - 1; i++) {
        echo[n++] = w->term_input[i];
    }
    echo[n] = 0;
    term_append_line(w, echo);

    int cl = w->term_input_len;
    if (cl > TERM_LINE_MAX - 1) cl = TERM_LINE_MAX - 1;
    for (int i = 0; i < cl; i++) cmd[i] = w->term_input[i];
    cmd[cl] = 0;

    w->term_input[0] = 0;
    w->term_input_len = 0;
    w->term_cursor_col = 0;

    klog("[terminal] exec: '%s'\n", cmd);

    char *args = cmd;
    while (*args == ' ') args++;
    if (*args == 0) return;

    char *tok = args;
    int ti = 0;
    while (args[ti] != 0 && args[ti] != ' ') ti++;
    int tok_len = ti;
    while (args[ti] == ' ') ti++;
    const char *rest = &args[ti];

    if (tok_len == 4 && memcmp(tok, "help", 4) == 0) {
        term_print(w, "Commands: help, clear, about, echo,");
        term_print(w, "time, date, mem, reboot.");
        return;
    }
    if (tok_len == 5 && memcmp(tok, "clear", 5) == 0) {
        w->term_len = 0;
        w->term_scroll = 0;
        return;
    }
    if (tok_len == 5 && memcmp(tok, "about", 5) == 0) {
        term_print(w, "Sol OS");
        term_print(w, "Version 0.1");
        term_print(w, "Architecture: x86_64");
        term_print(w, "Resolution: 1920x1080");
        term_print(w, "Bootloader: Limine");
        return;
    }
    if (tok_len == 4 && memcmp(tok, "echo", 4) == 0) {
        if (rest[0]) term_print(w, rest);
        return;
    }
    if (tok_len == 4 && memcmp(tok, "time", 4) == 0) {
        term_print_time(w);
        return;
    }
    if (tok_len == 4 && memcmp(tok, "date", 4) == 0) {
        term_print_date(w);
        return;
    }
    if (tok_len == 3 && memcmp(tok, "mem", 3) == 0) {
        term_print_mem(w);
        return;
    }
    if (tok_len == 6 && memcmp(tok, "reboot", 6) == 0) {
        term_print(w, "Rebooting...");
        sys_reboot();
        return;
    }
    term_print(w, "Unknown command. Type 'help'.");
}

static void term_feed(struct desktop_window *w, unsigned char c) {
    if (c >= 0x20 && c <= 0x7E) {
        if (w->term_input_len < TERM_LINE_MAX - 1) {
            if (w->term_cursor_col < w->term_input_len) {
                memmove(&w->term_input[w->term_cursor_col + 1],
                        &w->term_input[w->term_cursor_col],
                        (size_t)(w->term_input_len - w->term_cursor_col));
            }
            w->term_input[w->term_cursor_col] = (char)c;
            w->term_input_len++;
            w->term_cursor_col++;
        }
        return;
    }
    if (c == 8) {   /* backspace */
        if (w->term_cursor_col > 0) {
            if (w->term_cursor_col < w->term_input_len) {
                memmove(&w->term_input[w->term_cursor_col - 1],
                        &w->term_input[w->term_cursor_col],
                        (size_t)(w->term_input_len - w->term_cursor_col));
            }
            w->term_cursor_col--;
            w->term_input_len--;
            w->term_input[w->term_input_len] = 0;
        }
        return;
    }
    if (c == 10) {   /* enter */
        term_exec(w);
        return;
    }
    if (c == 0x01 || c == 0x05) {   /* page up / up: scroll back */
        w->term_scroll++;
        if (w->term_scroll > w->term_len) w->term_scroll = w->term_len;
        return;
    }
    if (c == 0x02 || c == 0x06) {   /* page down / down: scroll forward */
        if (w->term_scroll > 0) w->term_scroll--;
        return;
    }
    if (c == 0x03) {   /* cursor left */
        if (w->term_cursor_col > 0) w->term_cursor_col--;
        return;
    }
    if (c == 0x04) {   /* cursor right */
        if (w->term_cursor_col < w->term_input_len) w->term_cursor_col++;
        return;
    }
}

static void term_render(struct desktop_window *w, int active) {
    int64_t bx = w->x + 1;
    int64_t by = w->y + 1 + TITLE_H;
    int64_t bw = w->w - 2;
    int64_t bh = w->h - 2 - TITLE_H;
    bb_fill_rect(bx, by, bw, bh, TERM_BG);

    int64_t rows = (bh - 2) / TERM_ROW_H;
    if (rows < 2) rows = 2;
    int64_t cols = (bw - 8) / FONT_W;
    if (cols < 8) cols = 8;
    if (cols > TERM_LINE_MAX - 1) cols = TERM_LINE_MAX - 1;

    int64_t tx = bx + 4;
    int64_t ty = by + 2;

    int64_t sb = rows - 1;
    int64_t start = w->term_len - sb - w->term_scroll;
    if (start < 0) start = 0;

    for (int64_t i = 0; i < sb; i++) {
        int64_t ly = ty + i * TERM_ROW_H;
        const char *s = w->term_lines[start + i];
        int64_t x = tx;
        for (int k = 0; k < cols && s[k]; k++) {
            bb_draw_char(x, ly, s[k], TERM_TEXT);
            x += FONT_W;
        }
    }

    /* input row with prompt, typed text, and the (blinking) cursor */
    int64_t iy = ty + sb * TERM_ROW_H;
    int64_t x = tx;
    int ncells = 0;
    const char *pr = TERM_PROMPT;
    while (*pr && ncells < cols) {
        bb_draw_char(x, iy, *pr, TERM_TEXT);
        x += FONT_W;
        pr++;
        ncells++;
    }
    int cursor_cell = ncells;
    for (int k = 0; k < w->term_input_len && ncells < cols; k++) {
        bb_draw_char(x, iy, w->term_input[k], TERM_TEXT);
        x += FONT_W;
        ncells++;
    }
    if (active && (g_last_second & 1u)) {
        int64_t cx = tx + ((int64_t)cursor_cell + w->term_cursor_col) * FONT_W;
        bb_fill_rect(cx, iy + FONT_H + 1, FONT_W, 2, TERM_TEXT);
    }
}

/* ---- windows (drawing) ---- */

static void win_render(struct desktop_window *w) {
    int64_t x = w->x, y = w->y;
    int64_t bw = w->w, bh = w->h;
    int active = win_is_topmost_win(w);
    uint32_t title_bg = active ? TITLE_ACTIVE : TITLE_INACT;
    uint32_t title_tx = active ? TITLE_ACT_TEXT : TITLE_INACT_TEXT;
    uint32_t btn_bg = active ? BTN_BG_ACT : BTN_BG_INACT;

    bb_draw_rect(x, y, bw, bh, WIN_BORDER);
    bb_fill_rect(x + 1, y + 1, bw - 2, TITLE_H, title_bg);

    bb_draw_string(x + 7, y + (TITLE_H - FONT_H) / 2, w->title, title_tx);

    int64_t c0 = x + bw - BTN_MARGIN - BTN_W;
    int64_t m0 = c0 - BTN_W - BTN_GAP;
    int64_t n0 = m0 - BTN_W - BTN_GAP;
    int64_t by = y + 3;

    bb_fill_rect(c0, by, BTN_W, BTN_H, btn_bg);
    bb_fill_rect(m0, by, BTN_W, BTN_H, btn_bg);
    bb_fill_rect(n0, by, BTN_W, BTN_H, btn_bg);

    /* minimize: horizontal bar */
    bb_fill_rect(n0 + (BTN_W - 12) / 2, by + BTN_H / 2 - 1, 12, 2, BTN_GLYPH);
    /* maximize: hollow box */
    int64_t gx = m0 + (BTN_W - 12) / 2, gy = by + (BTN_H - 10) / 2;
    bb_fill_rect(gx, gy, 12, 1, BTN_GLYPH);
    bb_fill_rect(gx, gy + 9, 12, 1, BTN_GLYPH);
    bb_fill_rect(gx, gy + 1, 1, 8, BTN_GLYPH);
    bb_fill_rect(gx + 11, gy + 1, 1, 8, BTN_GLYPH);
    /* close: x glyph */
    bb_draw_char(c0 + (BTN_W - FONT_W) / 2, by + (BTN_H - FONT_H) / 2, 'x', BTN_GLYPH);

    if (w->kind == 1) {
        term_render(w, active);
        return;
    }

    bb_fill_rect(x + 1, y + 1 + TITLE_H, bw - 2, bh - 2 - TITLE_H, BODY_BG);

    int64_t lx = x + 8;
    int64_t ly = y + 1 + TITLE_H + 6;
    for (int li = 0; li < w->nlines && li < (int)MAX_WIN_LINES; li++) {
        bb_draw_string(lx, ly, w->lines[li], BODY_TEXT);
        ly += FONT_H + 4;
    }

    /* resize grip in the bottom-right corner of a normal (non-maximized)
     * window; maximized windows cannot be resized */
    if (!w->maximized) {
        int64_t gr = x + bw - 12;
        int64_t gb = y + bh - 12;
        bb_fill_rect(gr, gb + 8, 10, 2, 0x00C0C8D4u);
        bb_fill_rect(gr + 4, gb + 4, 10, 2, 0x00C0C8D4u);
        bb_fill_rect(gr + 8, gb, 10, 2, 0x00C0C8D4u);
    }
}

static int win_is_topmost_win(const struct desktop_window *w) {
    return win_is_topmost((int)(w - g_wins));
}

/* ---- window ops ---- */

static int win_open(const char *title, int64_t x, int64_t y,
                    int64_t w, int64_t h, int kind,
                    const char *const *lines, int nlines) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > (int64_t)g_w) x = (int64_t)g_w - w;
    if (y + h > work_h()) y = work_h() - h;
    if (w < MIN_WIN_W) w = MIN_WIN_W;
    if (h < MIN_WIN_H) h = MIN_WIN_H;

    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        struct desktop_window *n = &g_wins[i];
        if (n->used) continue;
        n->used = 1;
        n->x = x;
        n->y = y;
        n->w = w;
        n->h = h;
        n->title = title;
        n->kind = kind;
        n->order = ++g_order_counter;
        n->minimized = 0;
        n->maximized = 0;
        if (kind == 1) {
            n->term_len = 0;
            n->term_scroll = 0;
            n->term_input[0] = 0;
            n->term_input_len = 0;
            n->term_cursor_col = 0;
            term_append_line(n, "Sol OS terminal 0.1");
            term_append_line(n, "Type 'help' for commands.");
        } else {
            n->nlines = nlines > (int)MAX_WIN_LINES ? (int)MAX_WIN_LINES : nlines;
            for (int li = 0; li < n->nlines; li++) n->lines[li] = lines[li];
        }
        redraw_all_titles();
        return (int)i;
    }
    klog("[desktop] win_open: no free slot\n");
    return -1;
}

static void win_raise(int idx) {
    struct desktop_window *w = &g_wins[idx];
    if (!w->used) return;
    int prev = win_topmost_index();
    if (prev == idx) return;   /* already topmost */
    w->order = ++g_order_counter;
    redraw_union(w->x, w->y, w->w, w->h,
                 g_wins[prev].x, g_wins[prev].y, g_wins[prev].w, g_wins[prev].h);
    redraw_all_titles();
    redraw_taskbar();
}

static void win_close(int idx) {
    struct desktop_window *w = &g_wins[idx];
    if (!w->used) return;
    int64_t x = w->x, y = w->y, bw = w->w, bh = w->h;
    w->used = 0;
    if (g_drag == idx) g_drag = -1;
    if (g_resize == idx) g_resize = -1;
    redraw_rect(x, y, bw, bh);
    redraw_all_titles();
    redraw_taskbar();
}

static void win_minimize(int idx) {
    struct desktop_window *w = &g_wins[idx];
    if (!w->used) return;
    if (w->minimized) return;
    int64_t x = w->x, y = w->y, bw = w->w, bh = w->h;
    w->minimized = 1;
    if (g_drag == idx) g_drag = -1;
    if (g_resize == idx) g_resize = -1;
    redraw_rect(x, y, bw, bh);
    redraw_all_titles();
    redraw_taskbar();
}

static void win_restore_from_taskbar(int idx) {
    struct desktop_window *w = &g_wins[idx];
    if (!w->used) return;
    w->minimized = 0;
    w->order = ++g_order_counter;
    redraw_rect(w->x, w->y, w->w, w->h);
    redraw_all_titles();
    redraw_taskbar();
}

static void win_toggle_max(int idx) {
    struct desktop_window *w = &g_wins[idx];
    if (!w->used) return;
    int64_t ox = w->x, oy = w->y, ow = w->w, oh = w->h;
    if (!w->maximized) {
        w->rest_x = w->x;
        w->rest_y = w->y;
        w->rest_w = w->w;
        w->rest_h = w->h;
        w->x = 0;
        w->y = 0;
        w->w = (int64_t)g_w;
        w->h = work_h();
        w->maximized = 1;
    } else {
        w->x = w->rest_x;
        w->y = w->rest_y;
        w->w = w->rest_w;
        w->h = w->rest_h;
        w->maximized = 0;
    }
    redraw_union(ox, oy, ow, oh, w->x, w->y, w->w, w->h);
    redraw_taskbar();
}

/* ---- click dispatch ---- */

static int taskbar_win_at(int64_t px, int64_t py) {
    if (py < start_y() || py >= start_y() + TASK_WIN_H) return -1;
    int slot = 0;
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].used) continue;
        int64_t bx = task_btn_x(slot);
        if (px >= bx && px < bx + TASK_WIN_W) return (int)i;
        slot++;
    }
    return -1;
}

static void handle_left_press(int64_t px, int64_t py) {
    if (py >= work_h()) {
        /* click in the taskbar strip */
        if (px >= start_x() && px < start_x() + START_W &&
            py >= start_y() && py < start_y() + START_H) {
            klog("[desktop] start button\n");
            if (g_start_menu) close_start_menu();
            else open_start_menu();
            return;
        }
        if (g_start_menu) close_start_menu();
        int wi = taskbar_win_at(px, py);
        if (wi >= 0) {
            klog("[desktop] taskbar '%s'\n", g_wins[wi].title);
            if (g_wins[wi].minimized) {
                win_restore_from_taskbar(wi);
            } else {
                win_raise(wi);
            }
        }
        return;
    }

    /* a click anywhere outside the open Start menu closes it */
    if (g_start_menu &&
        px >= menu_x() && px < menu_x() + MENU_W &&
        py >= menu_y() && py < menu_y() + menu_h()) {
        int row = (int)((py - menu_y() - MENU_PAD) / MENU_ITEM_H);
        if (row >= 0 && row < (int)ICON_COUNT) {
            close_start_menu();
            icon_open(row);
        }
        return;
    }
    if (g_start_menu) close_start_menu();

    int wi = win_topmost_at(px, py);
    if (wi < 0) {
        int ic = icon_at(px, py);
        if (ic >= 0) {
            klog("[desktop] open '%s'\n", g_icons[ic].label);
            icon_open(ic);
        }
        return;
    }
    struct desktop_window *w = &g_wins[wi];

    char btn = win_button_at(w, px, py);
    if (btn == 'c') {
        klog("[desktop] close '%s'\n", w->title);
        win_close(wi);
        return;
    }
    if (btn == 'x') {
        klog("[desktop] max '%s'\n", w->title);
        win_toggle_max(wi);
        return;
    }
    if (btn == 'm') {
        klog("[desktop] min '%s'\n", w->title);
        win_minimize(wi);
        return;
    }

    /* bottom-right resize grip of a normal-sized window */
    if (!w->maximized &&
        px >= w->x + w->w - RESIZE_GUTTER && px < w->x + w->w &&
        py >= w->y + w->h - RESIZE_GUTTER && py < w->y + w->h) {
        win_raise(wi);
        g_resize = wi;
        g_drag_off_x = px - w->x;
        g_drag_off_y = py - w->y;
        g_resize_x = px;
        g_resize_y = py;
        g_resize_w = w->w;
        g_resize_h = w->h;
        klog("[desktop] resize '%s'\n", w->title);
        return;
    }

    if (py < w->y + TITLE_H) {
        win_raise(wi);
        g_drag = wi;
        g_drag_off_x = px - w->x;
        g_drag_off_y = py - w->y;
        klog("[desktop] drag '%s'\n", w->title);
    } else {
        win_raise(wi);
    }
}

static int win_topmost_at(int64_t px, int64_t py) {
    int best = -1;
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        const struct desktop_window *w = &g_wins[i];
        if (!w->used || w->minimized) continue;
        if (!win_contains(w, px, py)) continue;
        if (best < 0 || w->order > g_wins[best].order) best = (int)i;
    }
    return best;
}

/* ---- public API ---- */

void desktop_init(struct limine_framebuffer *fb) {
    if (fb == NULL) {
        klog("[desktop] FATAL: no framebuffer\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }
    if (fb->bpp != 32) {
        klog("[desktop] FATAL: %u bpp unsupported (need 32)\n",
             (unsigned)fb->bpp);
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }
    g_fb = fb;
    g_w = fb->width;
    g_h = fb->height;
    g_pitch = fb->pitch;
    g_fbaddr = (uint8_t *)fb->address;
    g_drag = -1;
    g_resize = -1;

    if (g_w == 0 || g_h == 0) {
        klog("[desktop] FATAL: zero-size framebuffer\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }
    if (g_w > 0x100000u || g_h > 0x100000u ||
        g_w * g_h > 0x40000000u / 4u) {
        klog("[desktop] FATAL: framebuffer too large\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }
    g_bb_bytes = g_w * g_h * 4;

    /* Backbuffer with a trailing guard region for OOB detection. */
    g_bb = kmalloc(g_bb_bytes + BB_GUARD_BYTES);
    if (g_bb == NULL) {
        klog("[desktop] FATAL: no heap for backbuffer (%lu bytes)\n",
             (unsigned long)g_bb_bytes);
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }
    for (unsigned i = 0; i < BB_CANARIES; i++) {
        g_bb_canary[i] = BB_CANARY_BASE + i;
        g_bb[g_bb_bytes / 4u + i] = g_bb_canary[i];
    }

    gfx_selftest();

    static const char *about_lines[] = {
        "Welcome to Sol OS!",
        "Desktop: mouse via VirtIO.",
        "Drag the title bar to move windows.",
        "Bottom-right corner resizes windows.",
        "Start menu: icons and the terminal.",
        "Terminal: 'help' lists commands.",
    };
    win_open("About Sol OS", 120, 90, 460, 240, 0, about_lines, 6);

    scene_region(0, 0, (int64_t)g_w, (int64_t)g_h);
    blit_full();
    g_last_second = timer_get_ticks() / 100;

    g_cursor_x = (int64_t)g_w / 2 - CURSOR_W / 2;
    g_cursor_y = (int64_t)g_h / 2 - CURSOR_H / 2;
    cursor_draw();

    klog("[desktop] %lu x %lu desktop up, %u window(s), heap free %lu KiB\n",
         (unsigned long)g_w, (unsigned long)g_h,
         (unsigned)win_count(), (unsigned long)(kheap_free_bytes() / 1024));
}

void desktop_poll(void) {
    cursor_restore();

    virtio_input_poll();

    int32_t dx, dy;
    uint8_t btns;
    virtio_mouse_get_delta(&dx, &dy, &btns);
    g_cursor_x += dx;
    g_cursor_y += dy;
    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_y < 0) g_cursor_y = 0;
    if (g_cursor_x > (int64_t)g_w - CURSOR_W) g_cursor_x = (int64_t)g_w - CURSOR_W;
    if (g_cursor_y > (int64_t)g_h - CURSOR_H) g_cursor_y = (int64_t)g_h - CURSOR_H;

    g_buttons_prev = g_buttons;
    g_buttons = btns;

    uint8_t pressed = g_buttons & ~g_buttons_prev;
    uint8_t released = g_buttons_prev & ~g_buttons;

    if (g_drag >= 0 && g_wins[g_drag].used && (dx != 0 || dy != 0)) {
        struct desktop_window *w = &g_wins[g_drag];
        int64_t ox = w->x, oy = w->y, ow = w->w, oh = w->h;
        if (w->maximized) {
            /* Dragging a maximized window restores it under the cursor. */
            w->x = w->rest_x;
            w->y = w->rest_y;
            w->w = w->rest_w;
            w->h = w->rest_h;
            w->maximized = 0;
        }
        int64_t nx = g_cursor_x - g_drag_off_x;
        int64_t ny = g_cursor_y - g_drag_off_y;
        if (nx + w->w > (int64_t)g_w) nx = (int64_t)g_w - w->w;
        if (ny + w->h > work_h()) ny = work_h() - w->h;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        w->x = nx;
        w->y = ny;
        redraw_union(ox, oy, ow, oh, nx, ny, w->w, w->h);
        redraw_taskbar();
    }

    if (g_resize >= 0 && g_wins[g_resize].used) {
        struct desktop_window *w = &g_wins[g_resize];
        if (w->maximized) {
            g_resize = -1;
        } else {
            int64_t ox = w->x, oy = w->y, ow = w->w, oh = w->h;
            int64_t nw = g_resize_w + (g_cursor_x - g_resize_x);
            int64_t nh = g_resize_h + (g_cursor_y - g_resize_y);
            if (nw < MIN_WIN_W) nw = MIN_WIN_W;
            if (nh < MIN_WIN_H) nh = MIN_WIN_H;
            if (w->x + nw > (int64_t)g_w) nw = (int64_t)g_w - w->x;
            if (w->y + nh > work_h()) nh = work_h() - w->y;
            if (nw != ow || nh != oh) {
                w->w = nw;
                w->h = nh;
                redraw_union(ox, oy, ow, oh, w->x, w->y, nw, nh);
                redraw_taskbar();
            }
        }
    }

    if (pressed & 0x01u) {
        handle_left_press(g_cursor_x, g_cursor_y);
    }
    if (pressed & 0x02u) {
        int wi = win_topmost_at(g_cursor_x, g_cursor_y);
        if (wi >= 0) {
            klog("[desktop] right click on '%s'\n", g_wins[wi].title);
            win_raise(wi);
        } else {
            klog("[desktop] right click at %ld,%ld\n",
                 (long)g_cursor_x, (long)g_cursor_y);
        }
    }
    if (released & 0x01u) {
        g_drag = -1;
        g_resize = -1;
    }

    /* Keyboard input: forward to the focused terminal window. */
    char kch;
    while (virtio_keyboard_read_char(&kch)) {
        int top = win_topmost_index();
        if (top < 0) continue;
        struct desktop_window *w = &g_wins[top];
        if (!w->used || w->kind != 1) continue;
        term_feed(w, (unsigned char)kch);
        redraw_rect(w->x, w->y, w->w, w->h);
    }

    uint64_t sec = timer_get_ticks() / 100;
    if (sec != g_last_second) {
        g_last_second = sec;
        render_clock();
        /* blink the cursor of the focused terminal */
        int top = win_topmost_index();
        if (top >= 0 && g_wins[top].used && g_wins[top].kind == 1) {
            redraw_rect(g_wins[top].x, g_wins[top].y,
                        g_wins[top].w, g_wins[top].h);
        }
        if (!desktop_gfx_integrity()) {
            klog("[gfx] INTEGRITY FAILURE at %lu s\n", (unsigned long)sec);
        }
    }

    cursor_draw();
}
