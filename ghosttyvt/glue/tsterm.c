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
#define TSTERM_ABI_VERSION 2

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
      // Cells erased with a background color set (as by EL or ED) have
      // that color, but no style: it's in the cell itself.
      GhosttyCellContentTag content = GHOSTTY_CELL_CONTENT_CODEPOINT;
      ghostty_cell_get(raw, GHOSTTY_CELL_DATA_CONTENT_TAG, &content);
      if (content == GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE) {
        GhosttyColorPaletteIndex idx = 0;
        ghostty_cell_get(raw, GHOSTTY_CELL_DATA_COLOR_PALETTE, &idx);
        style.bg_color.tag = GHOSTTY_STYLE_COLOR_PALETTE;
        style.bg_color.value.palette = idx;
      } else if (content == GHOSTTY_CELL_CONTENT_BG_COLOR_RGB) {
        GhosttyColorRgb rgb = {0};
        ghostty_cell_get(raw, GHOSTTY_CELL_DATA_COLOR_RGB, &rgb);
        style.bg_color.tag = GHOSTTY_STYLE_COLOR_RGB;
        style.bg_color.value.rgb = rgb;
      }
      put8(c, wide);
      put16(c, style.flags);
      putcolor(c, style.fg_color);
      putcolor(c, style.bg_color);

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
