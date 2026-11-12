// Glue between tsterm's Go code and libghostty-vt, compiled together with
// libghostty-vt into one WebAssembly module (see ../build.sh). The Go side is
// github.com/bradfitz/tsterm/pkg/vt/ghosttyvt.
//
// It exists for two reasons. First, libghostty-vt delivers effects, such as
// replies to terminal queries, through C function pointers, which in wasm
// index the module's own function table; a host cannot supply one. The
// callbacks here forward to functions the module imports from the host.
// Second, reading a screen through the library's getters takes several calls
// per cell; tsterm_snapshot reads it inside the module and encodes it into
// one buffer, so the host crosses the wasm boundary once per snapshot.
//
// Copyright (c) 2026 tsterm authors. Released under the same MIT terms as
// libghostty-vt; see ../LICENSE.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghostty/vt.h>

// TSTERM_ABI_VERSION versions the contract with the Go side: the exported
// functions, the imported ones, and the snapshot encoding. Increment it on
// any incompatible change. build.sh copies it into version.go.
#define TSTERM_ABI_VERSION 3

#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
#define EXPORT(name) __attribute__((export_name(#name)))

// write_pty delivers the terminal's replies to queries, which belong on
// the PTY's input. id is the host's identifier for the terminal.
IMPORT(write_pty) void host_write_pty(uint32_t id, const uint8_t* data, size_t len);

// notify delivers a program's request for a desktop notification (OSC 9 or
// OSC 777). The title may be empty.
IMPORT(notify) void host_notify(uint32_t id, const uint8_t* title, size_t title_len, const uint8_t* body, size_t body_len);

EXPORT(tsterm_abi_version) uint32_t tsterm_abi_version(void) { return TSTERM_ABI_VERSION; }

// ctx is the per-terminal state, and the terminal's userdata.
typedef struct {
  GhosttyTerminal term;
  uint32_t id;
  uint64_t bell;
  GhosttyRenderState rs;
  GhosttyRenderStateRowIterator rows;
  GhosttyRenderStateRowCells cells;

  // mark is the first row of the primary screen that tsterm_history has
  // not yet returned, tracked as the terminal scrolls, prunes, and
  // reflows; NULL until tsterm_history first runs.
  GhosttyTrackedGridRef mark;
  // pending_mark, if has_pending_mark, is a mark from a checkpoint to set
  // once the primary screen is active.
  uint32_t pending_mark;
  bool has_pending_mark;
  // fmt formats history for tsterm_history; NULL until it first runs.
  GhosttyFormatter fmt;
  // vtop is the screen row at the top of the viewport, and vrows the
  // viewport's height, while tsterm_history moves it (see row_ref).
  uint32_t vtop;
  uint16_t vrows;

  // out is the buffer tsterm_snapshot and tsterm_checkpoint return.
  uint8_t* out;
  size_t out_cap;
  size_t out_len;
} ctx;

static void on_write_pty(GhosttyTerminal t, void* ud, const uint8_t* data, size_t len) {
  (void)t;
  host_write_pty(((ctx*)ud)->id, data, len);
}

static void on_bell(GhosttyTerminal t, void* ud) {
  (void)t;
  ((ctx*)ud)->bell++;
}

static void on_notification(GhosttyTerminal t, void* ud, const GhosttyTerminalDesktopNotification* n) {
  (void)t;
  host_notify(((ctx*)ud)->id, n->title.ptr, n->title.len, n->body.ptr, n->body.len);
}

// setup configures a new or restored terminal and the state for reading it.
static ctx* setup(GhosttyTerminal term, uint32_t id, uint64_t scrollback_bytes) {
  ctx* c = ghostty_wasm_alloc(sizeof(ctx));
  if (!c) return NULL;
  *c = (ctx){.term = term, .id = id};
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_USERDATA, c);
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void*)on_write_pty);
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_BELL, (const void*)on_bell);
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION, (const void*)on_notification);
  size_t sb = (size_t)scrollback_bytes;
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &sb);
  // A blinking block cursor by default, as in xterm.
  bool blink = true;
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK, &blink);
  // Snapshots taken partway through a sequence keep the unfinished part.
  size_t cont = 64 << 10;
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES, &cont);
  if (ghostty_render_state_new(NULL, &c->rs) != GHOSTTY_SUCCESS ||
      ghostty_render_state_row_iterator_new(NULL, &c->rows) != GHOSTTY_SUCCESS ||
      ghostty_render_state_row_cells_new(NULL, &c->cells) != GHOSTTY_SUCCESS) {
    return NULL;
  }
  return c;
}

EXPORT(tsterm_new) ctx* tsterm_new(uint32_t id, uint16_t cols, uint16_t rows, uint64_t scrollback_bytes) {
  GhosttyTerminal term = NULL;
  if (ghostty_terminal_new(NULL, &term, cols, rows) != GHOSTTY_SUCCESS) return NULL;
  return setup(term, id, scrollback_bytes);
}

EXPORT(tsterm_free) void tsterm_free(ctx* c) {
  if (!c) return;
  ghostty_tracked_grid_ref_free(c->mark);
  ghostty_formatter_free(c->fmt);
  ghostty_render_state_row_cells_free(c->cells);
  ghostty_render_state_row_iterator_free(c->rows);
  ghostty_render_state_free(c->rs);
  ghostty_terminal_free(c->term);
  if (c->out) ghostty_wasm_free(c->out, c->out_cap);
  ghostty_wasm_free(c, sizeof(ctx));
}

EXPORT(tsterm_write) void tsterm_write(ctx* c, const uint8_t* data, size_t len) {
  ghostty_terminal_vt_write(c->term, data, len);
}

EXPORT(tsterm_resize) int tsterm_resize(ctx* c, uint16_t cols, uint16_t rows) {
  return ghostty_terminal_resize(c->term, cols, rows, 0, 0);
}

// tsterm_set_colors sets the default foreground and background colors, as
// 0xRRGGBB, which the terminal reports to programs that ask (OSC 10/11).
EXPORT(tsterm_set_colors) void tsterm_set_colors(ctx* c, uint32_t fg, uint32_t bg) {
  GhosttyColorRgb f = {.r = fg >> 16, .g = fg >> 8, .b = fg};
  GhosttyColorRgb b = {.r = bg >> 16, .g = bg >> 8, .b = bg};
  ghostty_terminal_set(c->term, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &f);
  ghostty_terminal_set(c->term, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &b);
}

EXPORT(tsterm_out_len) size_t tsterm_out_len(ctx* c) { return c->out_len; }

// Output buffer.

static bool reserve(ctx* c, size_t n) {
  if (c->out_len + n <= c->out_cap) return true;
  size_t cap = c->out_cap ? c->out_cap * 2 : 64 << 10;
  while (cap < c->out_len + n) cap *= 2;
  uint8_t* p = ghostty_wasm_alloc(cap);
  if (!p) return false;
  for (size_t i = 0; i < c->out_len; i++) p[i] = c->out[i];
  if (c->out) ghostty_wasm_free(c->out, c->out_cap);
  c->out = p;
  c->out_cap = cap;
  return true;
}

static void put8(ctx* c, uint8_t v) {
  if (reserve(c, 1)) c->out[c->out_len++] = v;
}
static void put16(ctx* c, uint16_t v) { put8(c, v); put8(c, v >> 8); }
static void put32(ctx* c, uint32_t v) { put16(c, v); put16(c, v >> 16); }
static void put64(ctx* c, uint64_t v) { put32(c, v); put32(c, v >> 32); }
static void putbytes(ctx* c, const uint8_t* p, size_t n) {
  if (!reserve(c, n)) return;
  for (size_t i = 0; i < n; i++) c->out[c->out_len++] = p[i];
}

static void putcolor(ctx* c, GhosttyStyleColor col) {
  put8(c, col.tag);
  switch (col.tag) {
    case GHOSTTY_STYLE_COLOR_PALETTE:
      put8(c, col.value.palette); put8(c, 0); put8(c, 0);
      break;
    case GHOSTTY_STYLE_COLOR_RGB:
      put8(c, col.value.rgb.r); put8(c, col.value.rgb.g); put8(c, col.value.rgb.b);
      break;
    default:
      put8(c, 0); put8(c, 0); put8(c, 0);
  }
}

static bool mode(ctx* c, uint16_t n) {
  GhosttyTerminalModeConfig m = {.mode = ghostty_mode_new(n, false)};
  return ghostty_terminal_get(c->term, GHOSTTY_TERMINAL_DATA_MODE, &m) == GHOSTTY_SUCCESS && m.value;
}

// Mode bits in a snapshot.
enum {
  MODE_ALT_SCREEN = 1 << 0,
  MODE_APP_CURSOR = 1 << 1,
  MODE_BRACKETED_PASTE = 1 << 2,
  MODE_FOCUS_EVENTS = 1 << 3,
  MODE_MOUSE_SGR = 1 << 4,
  MODE_CURSOR_VISIBLE = 1 << 5,
  // Bits 8-11 hold the mouse tracking mode: 0 off, 1 X10 (9), 2 normal
  // (1000), 3 button (1002), 4 any (1003).
  MODE_MOUSE_SHIFT = 8,
};

// put_cell_style encodes a cell's width and style, the start of a cell's
// encoding (see tsterm_snapshot).
static void put_cell_style(ctx* c, GhosttyCell raw, GhosttyCellWide wide, GhosttyStyle* style) {
  // Cells erased with a background color set (as by EL or ED) have that
  // color, but no style: it's in the cell itself.
  GhosttyCellContentTag content = GHOSTTY_CELL_CONTENT_CODEPOINT;
  ghostty_cell_get(raw, GHOSTTY_CELL_DATA_CONTENT_TAG, &content);
  if (content == GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE) {
    GhosttyColorPaletteIndex idx = 0;
    ghostty_cell_get(raw, GHOSTTY_CELL_DATA_COLOR_PALETTE, &idx);
    style->bg_color.tag = GHOSTTY_STYLE_COLOR_PALETTE;
    style->bg_color.value.palette = idx;
  } else if (content == GHOSTTY_CELL_CONTENT_BG_COLOR_RGB) {
    GhosttyColorRgb rgb = {0};
    ghostty_cell_get(raw, GHOSTTY_CELL_DATA_COLOR_RGB, &rgb);
    style->bg_color.tag = GHOSTTY_STYLE_COLOR_RGB;
    style->bg_color.value.rgb = rgb;
  }
  put8(c, wide);
  put16(c, style->flags);
  putcolor(c, style->fg_color);
  putcolor(c, style->bg_color);
}

// tsterm_snapshot encodes the visible screen into the output buffer and
// returns it; its length is tsterm_out_len. The encoding, all little
// endian, is decoded by the Go side:
//
//	u16 rows, u16 cols, u16 cursor x, u16 cursor y,
//	u8 cursor visible, u8 cursor blinking, u8 cursor style (GhosttyCursorStyle),
//	u32 modes (see above), u64 bell count, u32 title length, title bytes,
//	then rows*cols cells: u8 wide (GhosttyCellWide), u16 style flags,
//	fg color, bg color (each u8 tag, then u8 x3: palette index or R, G, B),
//	u8 grapheme length, grapheme UTF-8 bytes, u16 link length, link bytes.
EXPORT(tsterm_snapshot) uint8_t* tsterm_snapshot(ctx* c) {
  c->out_len = 0;
  if (ghostty_render_state_update(c->rs, c->term) != GHOSTTY_SUCCESS) return NULL;
  uint16_t rows = 0, cols = 0;
  ghostty_render_state_get(c->rs, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows);
  ghostty_render_state_get(c->rs, GHOSTTY_RENDER_STATE_DATA_COLS, &cols);
  GhosttyRenderStateCursor cur = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
  ghostty_render_state_get(c->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cur);
  put16(c, rows);
  put16(c, cols);
  put16(c, cur.viewport_has_value ? cur.viewport_x : 0);
  put16(c, cur.viewport_has_value ? cur.viewport_y : 0);
  put8(c, cur.visible);
  put8(c, cur.blinking);
  put8(c, cur.visual_style);

  uint32_t modes = 0;
  GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
  ghostty_terminal_get(c->term, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
  if (screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE) modes |= MODE_ALT_SCREEN;
  if (mode(c, 1)) modes |= MODE_APP_CURSOR;
  if (mode(c, 2004)) modes |= MODE_BRACKETED_PASTE;
  if (mode(c, 1004)) modes |= MODE_FOCUS_EVENTS;
  if (mode(c, 1006)) modes |= MODE_MOUSE_SGR;
  if (mode(c, 25)) modes |= MODE_CURSOR_VISIBLE;
  uint32_t mouse = mode(c, 1003) ? 4 : mode(c, 1002) ? 3 : mode(c, 1000) ? 2 : mode(c, 9) ? 1 : 0;
  modes |= mouse << MODE_MOUSE_SHIFT;
  put32(c, modes);
  put64(c, c->bell);

  GhosttyString title = {0};
  ghostty_terminal_get(c->term, GHOSTTY_TERMINAL_DATA_TITLE, &title);
  put32(c, title.len);
  putbytes(c, title.ptr, title.len);

  uint8_t buf[256];
  ghostty_render_state_get(c->rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &c->rows);
  uint16_t y = 0;
  while (ghostty_render_state_row_iterator_next(c->rows)) {
    ghostty_render_state_row_get(c->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &c->cells);
    uint16_t x = 0;
    while (ghostty_render_state_row_cells_next(c->cells)) {
      GhosttyCell raw = 0;
      GhosttyStyle style;
      ghostty_render_state_row_cells_get(c->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
      ghostty_render_state_row_cells_get(c->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
      GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
      ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
      put_cell_style(c, raw, wide, &style);

      GhosttyBuffer g = {.ptr = buf, .cap = sizeof(buf)};
      if (ghostty_render_state_row_cells_get(c->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &g) != GHOSTTY_SUCCESS) {
        g.len = 0; // a grapheme over 256 bytes: drop it
      }
      put8(c, g.len);
      putbytes(c, buf, g.len);

      bool has_link = false;
      ghostty_cell_get(raw, GHOSTTY_CELL_DATA_HAS_HYPERLINK, &has_link);
      size_t link_len = 0;
      if (has_link) {
        GhosttyPoint pt = {.tag = GHOSTTY_POINT_TAG_ACTIVE, .value = {.coordinate = {.x = x, .y = y}}};
        GhosttyGridRef ref = {0};
        if (ghostty_terminal_grid_ref(c->term, pt, &ref) == GHOSTTY_SUCCESS &&
            ghostty_grid_ref_hyperlink_uri(&ref, buf, sizeof(buf), &link_len) != GHOSTTY_SUCCESS) {
          link_len = 0; // longer than 256 bytes: drop it
        }
      }
      put16(c, link_len);
      putbytes(c, buf, link_len);
      x++;
    }
    y++;
  }
  return c->out;
}

// History.
//
// tsterm_history returns the primary screen's lines that have scrolled into
// its history since it last ran: whole logical lines, a run of rows joined
// where they soft-wrapped, and only once their last row is in history, since
// until then the program can still change them. The host stores them, so
// that history outlives libghostty's bounded scrollback.

// MAX_LINE_ROWS is the most rows tsterm_history joins into one line. A
// longer run, such as a huge single line of output, is returned in parts.
#define MAX_LINE_ROWS 1000

static void scroll_viewport(ctx* c, GhosttyTerminalScrollViewportTag tag, intptr_t delta) {
  GhosttyTerminalScrollViewport b = {.tag = tag, .value = {.delta = delta}};
  ghostty_terminal_scroll_viewport(c->term, b);
}

// row_ref sets ref to the first cell of screen row y, a history row.
//
// Looking up a screen row walks libghostty's pages from the top of
// history, which is slow when history is long, but looking up a viewport
// row walks only from the viewport's top. So row_ref scrolls the viewport
// to the rows it reads, and tsterm_history scrolls it back to the bottom,
// where tsterm_snapshot expects it, before returning. Callers look up each
// row once and reach its other cells by setting ref->x: a page is as wide
// as the screen, and GhosttyGridRef's layout is frozen.
static bool row_ref(ctx* c, uint32_t y, GhosttyGridRef* ref) {
  if (y < c->vtop || y >= c->vtop + c->vrows) {
    scroll_viewport(c, GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA, (intptr_t)y - (intptr_t)c->vtop);
    c->vtop = y;
  }
  GhosttyPoint pt = {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = {.x = 0, .y = y - c->vtop}}};
  *ref = (GhosttyGridRef){0};
  return ghostty_terminal_grid_ref(c->term, pt, ref) == GHOSTTY_SUCCESS;
}

static bool ref_wraps(const GhosttyGridRef* ref) {
  GhosttyRow row = 0;
  bool wrap = false;
  return ghostty_grid_ref_row(ref, &row) == GHOSTTY_SUCCESS &&
         ghostty_row_get(row, GHOSTTY_ROW_DATA_WRAP, &wrap) == GHOSTTY_SUCCESS && wrap;
}

static bool row_wraps(ctx* c, uint32_t y) {
  GhosttyGridRef ref;
  return row_ref(c, y, &ref) && ref_wraps(&ref);
}

// set_mark moves the mark to the start of screen row y.
static void set_mark(ctx* c, uint32_t y) {
  GhosttyPoint pt = {.tag = GHOSTTY_POINT_TAG_SCREEN, .value = {.coordinate = {.x = 0, .y = y}}};
  if (y >= c->vtop && y < c->vtop + c->vrows) {
    // Cheaper; see row_ref.
    pt = (GhosttyPoint){.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = {.x = 0, .y = y - c->vtop}}};
  }
  if (c->mark) {
    ghostty_tracked_grid_ref_set(c->mark, c->term, pt);
  } else {
    ghostty_terminal_grid_ref_track(c->term, pt, &c->mark);
  }
}

static bool primary(ctx* c) {
  GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
  ghostty_terminal_get(c->term, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
  return screen == GHOSTTY_TERMINAL_SCREEN_PRIMARY;
}

// format_rows appends rows y0 through y1 of the screen, whole, to the
// output, as VT text: each logical line's text, with SGR sequences for its
// styles and OSC 8 for its hyperlinks, the lines separated by CRLF, and
// without blank cells at their ends.
static bool format_rows(ctx* c, uint32_t y0, uint32_t y1, uint16_t cols) {
  if (!c->fmt) {
    if (ghostty_formatter_terminal_new(NULL, &c->fmt, c->term) != GHOSTTY_SUCCESS) return false;
    GhosttyFormatterFormat emit = GHOSTTY_FORMATTER_FORMAT_VT;
    GhosttyFormatterExtras extras = GHOSTTY_FORMATTER_EXTRAS_NONE;
    // Trimming would also drop the blank lines at the end, which the host
    // needs to count lines.
    bool unwrap = true, trim = false;
    ghostty_formatter_set(c->fmt, GHOSTTY_FORMATTER_OPT_EMIT, &emit);
    ghostty_formatter_set(c->fmt, GHOSTTY_FORMATTER_OPT_EXTRAS, &extras);
    ghostty_formatter_set(c->fmt, GHOSTTY_FORMATTER_OPT_UNWRAP, &unwrap);
    ghostty_formatter_set(c->fmt, GHOSTTY_FORMATTER_OPT_TRIM, &trim);
  }
  GhosttySelection sel = {0};
  if (!row_ref(c, y0, &sel.start) || !row_ref(c, y1, &sel.end)) return false;
  sel.end.x += cols - 1;
  if (ghostty_formatter_set(c->fmt, GHOSTTY_FORMATTER_OPT_SELECTION, &sel) != GHOSTTY_SUCCESS) return false;
  // Format into the output buffer's free space, and if that's too small,
  // grow it to the size needed and format again.
  if (!reserve(c, 4096)) return false;
  size_t n = 0;
  GhosttyResult r = ghostty_formatter_format_buf(c->fmt, c->out + c->out_len, c->out_cap - c->out_len, &n);
  if (r == GHOSTTY_OUT_OF_SPACE) {
    if (!reserve(c, n)) return false;
    r = ghostty_formatter_format_buf(c->fmt, c->out + c->out_len, n, &n);
  }
  if (r != GHOSTTY_SUCCESS) return false;
  c->out_len += n;
  return true;
}

// tsterm_history formats the lines that have newly scrolled into history,
// and returns them: u8 lost, 1 if lines may have been dropped from
// libghostty's scrollback before they could be returned; u8 continued, 1 if
// the last line is part of a longer one that continues; then the lines, as
// format_rows formats them, or nothing if there are none.
EXPORT(tsterm_history) uint8_t* tsterm_history(ctx* c) {
  c->out_len = 0;
  put8(c, 0);
  put8(c, 0);
  // The alternate screen has no history; the primary screen's waits.
  if (!primary(c)) return c->out;
  size_t sb = 0;
  uint16_t cols = 0;
  ghostty_terminal_get(c->term, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &sb);
  ghostty_terminal_get(c->term, GHOSTTY_TERMINAL_DATA_COLS, &cols);
  ghostty_terminal_get(c->term, GHOSTTY_TERMINAL_DATA_ROWS, &c->vrows);
  c->vtop = (uint32_t)sb; // the viewport is at the bottom
  if (c->has_pending_mark) {
    c->has_pending_mark = false;
    set_mark(c, c->pending_mark);
  }
  uint32_t y = (uint32_t)sb; // a new terminal starts with its first row
  if (c->mark) {
    GhosttyPointCoordinate p = {0};
    if (ghostty_tracked_grid_ref_point(c->mark, GHOSTTY_POINT_TAG_SCREEN, &p) == GHOSTTY_SUCCESS) {
      y = p.y;
    } else {
      // The marked row is gone: pruned before we got to it, or erased
      // along with the scrollback (as by ED 3). Start over at the top.
      y = 0;
      c->out[0] = sb > 0;
    }
  }
  // The lines from y up to the one that ends in the active area are
  // complete. Find where that one starts, looking back at most
  // MAX_LINE_ROWS rows.
  uint32_t lo = sb > y + MAX_LINE_ROWS ? (uint32_t)sb - MAX_LINE_ROWS : y;
  uint32_t end = (uint32_t)sb;
  while (end > lo && row_wraps(c, end - 1)) end--;
  if (end == lo && lo == y && sb - y >= MAX_LINE_ROWS) {
    // A line has wrapped over MAX_LINE_ROWS rows: return that many.
    end = y + MAX_LINE_ROWS;
  }
  // If row end - 1 wraps, the last line is part of a longer one, cut at
  // MAX_LINE_ROWS from the bottom or the top.
  if (end > y && format_rows(c, y, end - 1, cols)) {
    c->out[1] = row_wraps(c, end - 1);
    y = end;
    set_mark(c, y);
  } else if (!c->mark) {
    set_mark(c, y);
  }
  if (c->vtop != sb) scroll_viewport(c, GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM, 0);
  return c->out;
}

// tsterm_history_mark returns the screen row of the mark, the first row
// tsterm_history has not yet returned, so that a checkpoint can record it.
EXPORT(tsterm_history_mark) uint32_t tsterm_history_mark(ctx* c) {
  if (c->has_pending_mark) return c->pending_mark;
  GhosttyPointCoordinate p = {0};
  if (c->mark && ghostty_tracked_grid_ref_point(c->mark, GHOSTTY_POINT_TAG_SCREEN, &p) == GHOSTTY_SUCCESS) {
    return p.y;
  }
  size_t sb = 0;
  ghostty_terminal_get(c->term, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &sb);
  return (uint32_t)sb;
}

// tsterm_set_history_mark sets the mark to screen row y, as recorded in a
// checkpoint, on the primary screen.
EXPORT(tsterm_set_history_mark) void tsterm_set_history_mark(ctx* c, uint32_t y) {
  if (!primary(c)) {
    // A tracked reference attaches to the active screen, so wait for the
    // primary one. Its rows don't move meanwhile.
    c->pending_mark = y;
    c->has_pending_mark = true;
    return;
  }
  set_mark(c, y);
}

// tsterm_checkpoint encodes the terminal's complete state, as a libghostty
// snapshot, into the output buffer and returns it.
EXPORT(tsterm_checkpoint) uint8_t* tsterm_checkpoint(ctx* c) {
  c->out_len = 0;
  uint8_t* p = NULL;
  size_t n = 0;
  if (ghostty_snapshot_encode_alloc(c->term, NULL, &p, &n) != GHOSTTY_SUCCESS) return NULL;
  putbytes(c, p, n);
  ghostty_free(NULL, p, n);
  return c->out;
}

// tsterm_restore creates a terminal from a tsterm_checkpoint encoding.
EXPORT(tsterm_restore) ctx* tsterm_restore(uint32_t id, const uint8_t* data, size_t len, uint64_t scrollback_bytes) {
  GhosttySnapshotDecoder dec = NULL;
  if (ghostty_snapshot_decoder_new_buf(NULL, &dec, data, len) != GHOSTTY_SUCCESS) return NULL;
  GhosttyTerminal term = NULL;
  GhosttyResult r = ghostty_snapshot_decoder_decode(dec, &term);
  ghostty_snapshot_decoder_free(dec);
  if (r != GHOSTTY_SUCCESS) return NULL;
  return setup(term, id, scrollback_bytes);
}
