#include "pangoterm.h"

#include <string.h>  // memmove
#include <wctype.h>

#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "conf.h"

CONF_STRING(cursor, 0, "white", "Cursor colour", "COL");
CONF_INT(cursor_shape, 0, 1, "Cursor shape (1=block 2=underbar 3=vertical bar)", "SHAPE");

CONF_DOUBLE(size, 's', 9.0, "Font size", "NUM");

CONF_INT(cursor_blink_interval, 0, 500, "Cursor blink interval", "MSEC");

CONF_BOOL(bold_highbright, 0, TRUE, "Bold is high-brightness");
CONF_BOOL(altscreen, 0, TRUE, "Alternate screen buffer switching");

CONF_INT(scrollback_size, 0, 1000, "Scrollback size", "LINES");

CONF_INT(scrollbar_width, 0, 3, "Scroll bar width", "PIXELS");

CONF_BOOL(unscroll_on_output, 0, TRUE, "Scroll to bottom on output");
CONF_BOOL(unscroll_on_key,    0, TRUE, "Scroll to bottom on keypress");

CONF_BOOL(doubleclick_fullword, 0, FALSE, "Double-click selects fullwords (until whitespace)");

#ifdef DEBUG
# define DEBUG_PRINT_INPUT
#endif

/* To accomodate scrollback scrolling, we'll adopt the convention that VTermPos
 * and VTermRect instances always refer to virtual locations within the
 * VTermScreen buffer (or our scrollback buffer if row is negative), and
 * PhyPos and PhyRect instances refer to physical onscreen positions
 */

typedef struct {
  int prow, pcol;
} PhyPos;

#define PHYSPOS_FROM_VTERMPOS(pt, pos) \
  {                                    \
    .prow = pos.row + pt->scroll_offs, \
    .pcol = pos.col,                   \
  }

#define VTERMPOS_FROM_PHYSPOS(pt, pos) \
  {                                    \
    .row = pos.prow - pt->scroll_offs, \
    .col = pos.pcol,                   \
  }

typedef struct {
  int start_prow, end_prow, start_pcol, end_pcol;
} PhyRect;

#define PHYRECT_FROM_VTERMRECT(pt, rect)            \
  {                                                 \
    .start_prow = rect.start_row + pt->scroll_offs, \
    .end_prow   = rect.end_row   + pt->scroll_offs, \
    .start_pcol = rect.start_col,                   \
    .end_pcol   = rect.end_col,                     \
  }

typedef struct {
  int cols;
  VTermScreenCell cells[];
} PangoTermScrollbackLine;

struct PangoTerm {
  VTerm *vt;
  VTermScreen *vts;

  GtkIMContext *im_context;

  VTermMouseFunc mousefunc;
  void *mousedata;

  GdkRectangle pending_area;
  /* Pending glyphs to flush in flush_pending */
  GString *glyphs;
  GArray *glyph_widths;
  /* Pending area to erase in flush_pending */
  int erase_columns;
  /* Is pending area DWL? */
  int pending_dwl;

  struct {
    struct {
      unsigned int bold      : 1;
      unsigned int underline : 2;
      unsigned int italic    : 1;
      unsigned int reverse   : 1;
      unsigned int strike    : 1;
      unsigned int font      : 4;
      unsigned int dwl       : 1;
      unsigned int dhl       : 2;
    } attrs;
    GdkColor fg_col;
    GdkColor bg_col;
    PangoAttrList *pangoattrs;
    PangoLayout *layout;
  } pen;

  int rows;
  int cols;

  int on_altscreen;
  int scroll_offs;

  int scroll_size;
  int scroll_current;
  PangoTermScrollbackLine **sb_buffer;

  PangoTermWriteFn *writefn;
  void *writefn_data;

  PangoTermResizedFn *resizedfn;
  void *resizedfn_data;

  int n_fonts;
  char **fonts;
  double font_size;

  int cell_width_pango;
  int cell_width;
  int cell_height;

  GdkColor fg_col;

  int has_focus;
  int cursor_visible;    /* VTERM_PROP_CURSORVISIBLE */
  int cursor_blinkstate; /* during high state of blink */
  int cursor_hidden_for_redraw; /* true to temporarily hide during redraw */
  VTermPos cursorpos;
  GdkColor cursor_col;
  int cursor_shape;

#define CURSOR_ENABLED(pt) ((pt)->cursor_visible && !(pt)->cursor_hidden_for_redraw)

  guint cursor_timer_id;

  GtkWidget *termwin;

  cairo_surface_t *buffer;
  GdkDrawable *termdraw;
  /* area in buffer that needs flushing to termdraw */
  GdkRectangle dirty_area;

  /* These four positions relate to the click/drag highlight state */

  enum { NO_DRAG, DRAG_PENDING, DRAGGING } dragging;
  /* Initial mouse position of selection drag */
  VTermPos drag_start;
  /* Current mouse position of selection drag */
  VTermPos drag_pos;

  /* Start and stop bounds of the selection */
  int highlight;
  VTermPos highlight_start;
  VTermPos highlight_stop;

  GtkClipboard *selection_primary;
  GtkClipboard *selection_clipboard;
};

/*
 * Utility functions
 */

static VTermKey convert_keyval(guint gdk_keyval, VTermModifier *statep)
{
  if(gdk_keyval >= GDK_KEY_F1 && gdk_keyval <= GDK_KEY_F35)
    return VTERM_KEY_FUNCTION(gdk_keyval - GDK_KEY_F1 + 1);

  switch(gdk_keyval) {
  case GDK_KEY_BackSpace:
    return VTERM_KEY_BACKSPACE;
  case GDK_KEY_Tab:
  case GDK_KEY_KP_Tab:
    return VTERM_KEY_TAB;
  case GDK_KEY_Return:
    return VTERM_KEY_ENTER;
  case GDK_KEY_Escape:
    return VTERM_KEY_ESCAPE;

  case GDK_KEY_Up:
    return VTERM_KEY_UP;
  case GDK_KEY_Down:
    return VTERM_KEY_DOWN;
  case GDK_KEY_Left:
    return VTERM_KEY_LEFT;
  case GDK_KEY_Right:
    return VTERM_KEY_RIGHT;

  case GDK_KEY_Insert:
    return VTERM_KEY_INS;
  case GDK_KEY_Delete:
    return VTERM_KEY_DEL;
  case GDK_KEY_Home:
    return VTERM_KEY_HOME;
  case GDK_KEY_End:
    return VTERM_KEY_END;
  case GDK_KEY_Page_Up:
    return VTERM_KEY_PAGEUP;
  case GDK_KEY_Page_Down:
    return VTERM_KEY_PAGEDOWN;

  case GDK_KEY_ISO_Left_Tab:
    /* This is Shift-Tab */
    *statep |= VTERM_MOD_SHIFT;
    return VTERM_KEY_TAB;

  case GDK_KEY_KP_Insert:
    return VTERM_KEY_KP_0;
  case GDK_KEY_KP_End:
    return VTERM_KEY_KP_1;
  case GDK_KEY_KP_Down:
    return VTERM_KEY_KP_2;
  case GDK_KEY_KP_Page_Down:
    return VTERM_KEY_KP_3;
  case GDK_KEY_KP_Left:
    return VTERM_KEY_KP_4;
  case GDK_KEY_KP_Begin:
    return VTERM_KEY_KP_5;
  case GDK_KEY_KP_Right:
    return VTERM_KEY_KP_6;
  case GDK_KEY_KP_Home:
    return VTERM_KEY_KP_7;
  case GDK_KEY_KP_Up:
    return VTERM_KEY_KP_8;
  case GDK_KEY_KP_Page_Up:
    return VTERM_KEY_KP_9;
  case GDK_KEY_KP_Delete:
    return VTERM_KEY_KP_PERIOD;
  case GDK_KEY_KP_Enter:
    return VTERM_KEY_KP_ENTER;
  case GDK_KEY_KP_Add:
    return VTERM_KEY_KP_PLUS;
  case GDK_KEY_KP_Subtract:
    return VTERM_KEY_KP_MINUS;
  case GDK_KEY_KP_Multiply:
    return VTERM_KEY_KP_MULT;
  case GDK_KEY_KP_Divide:
    return VTERM_KEY_KP_DIVIDE;
  case GDK_KEY_KP_Equal:
    return VTERM_KEY_KP_EQUAL;

  default:
    return VTERM_KEY_NONE;
  }
}

static VTermModifier convert_modifier(int state)
{
  VTermModifier mod = VTERM_MOD_NONE;
  if(state & GDK_SHIFT_MASK)
    mod |= VTERM_MOD_SHIFT;
  if(state & GDK_CONTROL_MASK)
    mod |= VTERM_MOD_CTRL;
  if(state & GDK_MOD1_MASK)
    mod |= VTERM_MOD_ALT;

    return mod;
}

static void term_flush_output(PangoTerm *pt)
{
  size_t bufflen = vterm_output_get_buffer_current(pt->vt);
  if(bufflen) {
    char buffer[bufflen];
    bufflen = vterm_output_bufferread(pt->vt, buffer, bufflen);
    (*pt->writefn)(buffer, bufflen, pt->writefn_data);
  }
}

static void term_push_string(PangoTerm *pt, gchar *str)
{
  while(str && str[0]) {
    /* 6 bytes is always enough for any UTF-8 character */
    if(vterm_output_get_buffer_remaining(pt->vt) < 6)
      term_flush_output(pt);

    vterm_input_push_char(pt->vt, 0, g_utf8_get_char(str));
    str = g_utf8_next_char(str);
  }

  term_flush_output(pt);
}

static void pos_next(PangoTerm *pt, VTermPos *pos)
{
  pos->col++;
  if(pos->col >= pt->cols) {
    pos->row++;
    pos->col = 0;
  }
}

static void pos_prev(PangoTerm *pt, VTermPos *pos)
{
  pos->col--;
  if(pos->col < 0) {
    pos->row--;
    pos->col = pt->cols - 1;
  }
}

static void fetch_cell(PangoTerm *pt, VTermPos pos, VTermScreenCell *cell)
{
  if(pos.row < 0) {
    if(-pos.row > pt->scroll_current) {
      fprintf(stderr, "ARGH! Attempt to fetch scrollback beyond buffer at line %d\n", -pos.row);
      abort();
    }

    /* pos.row == -1 => sb_buffer[0], -2 => [1], etc... */
    PangoTermScrollbackLine *sb_line = pt->sb_buffer[-pos.row-1];
    if(pos.col < sb_line->cols)
      *cell = sb_line->cells[pos.col];
    else {
      *cell = (VTermScreenCell) { { 0 } };
      cell->width = 1;
      cell->bg = sb_line->cells[sb_line->cols - 1].bg;
    }
  }
  else {
    vterm_screen_get_cell(pt->vts, pos, cell);
  }
}

static int fetch_is_eol(PangoTerm *pt, VTermPos pos)
{
  if(pos.row >= 0)
    return vterm_screen_is_eol(pt->vts, pos);

  PangoTermScrollbackLine *sb_line = pt->sb_buffer[-pos.row-1];
  for(int col = pos.col; col < sb_line->cols; ) {
    if(sb_line->cells[col].chars[0])
      return 0;
    col += sb_line->cells[col].width;
  }
  return 1;
}

static size_t fetch_line_text(PangoTerm *pt, gchar *str, size_t len, VTermRect rect)
{
  size_t ret = 0;
  int end_blank = 0;

  VTermPos pos = {
    .row = rect.start_row,
    .col = rect.start_col,
  };
  while(pos.col < rect.end_col) {
    VTermScreenCell cell;
    fetch_cell(pt, pos, &cell);
    for(int i = 0; cell.chars[i]; i++)
      ret += g_unichar_to_utf8(cell.chars[i], str ? str + ret : NULL);

    end_blank = !cell.chars[0];

    pos.col += cell.width;
  }

  if(end_blank) {
    if(str)
      str[ret] = 0x0a;
    ret++;
  }

  return ret;
}

static gchar *fetch_flow_text(PangoTerm *pt, VTermPos start, VTermPos stop)
{
  size_t strlen = 0;
  char *str = NULL;

  // This logic looks so similar each time it's easier to loop it
  while(1) {
    size_t thislen = 0;

    VTermRect rect;
    if(start.row == stop.row) {
      rect.start_row = start.row;
      rect.start_col = start.col;
      rect.end_row   = start.row + 1;
      rect.end_col   = stop.col + 1;
      thislen += fetch_line_text(pt,
          str ? str    + thislen : NULL,
          str ? strlen - thislen : 0,
          rect);
    }
    else {
      rect.start_row = start.row;
      rect.start_col = start.col;
      rect.end_row   = start.row + 1;
      rect.end_col   = pt->cols;
      thislen += fetch_line_text(pt,
          str ? str    + thislen : NULL,
          str ? strlen - thislen : 0,
          rect);

      for(int row = start.row + 1; row < stop.row; row++) {
        rect.start_row = row;
        rect.start_col = 0;
        rect.end_row   = row + 1;
        rect.end_col   = pt->cols;

        thislen += fetch_line_text(pt,
            str ? str    + thislen : NULL,
            str ? strlen - thislen : 0,
            rect);
      }

      rect.start_row = stop.row;
      rect.start_col = 0;
      rect.end_row   = stop.row + 1;
      rect.end_col   = stop.col + 1;
      thislen += fetch_line_text(pt,
          str ? str    + thislen : NULL,
          str ? strlen - thislen : 0,
          rect);
    }

    if(str)
      break;

    strlen = thislen;
    str = malloc(strlen + 1); // Terminating NUL
  }

  str[strlen] = 0;

  return str;
}

#define GDKRECTANGLE_FROM_PHYRECT(pt, rect)                        \
  {                                                                \
    .x      = rect.start_pcol * pt->cell_width,                    \
    .y      = rect.start_prow * pt->cell_height,                   \
    .width  = (rect.end_pcol - rect.start_pcol) * pt->cell_width,  \
    .height = (rect.end_prow - rect.start_prow) * pt->cell_height, \
  }

#define GDKRECTANGLE_FROM_PHYPOS_CELLS(pt, pos, width_mult) \
  {                                                         \
    .x      = pos.pcol * pt->cell_width,                    \
    .y      = pos.prow * pt->cell_height,                   \
    .width  = pt->cell_width * width_mult,                  \
    .height = pt->cell_height,                              \
  }

static int is_wordchar(uint32_t c)
{
  if(CONF_doubleclick_fullword)
    return c && !iswspace(c);
  else
    return iswalnum(c) || (c == '_');
}

static void lf_to_cr(gchar *str)
{
  for( ; str[0]; str++)
    if(str[0] == '\n')
      str[0] = '\r';
}

/*
 * Repainting operations
 */

static void blit_buffer(PangoTerm *pt, GdkRectangle *area)
{
  cairo_surface_flush(pt->buffer);

  cairo_t* gc = gdk_cairo_create(pt->termdraw);
  gdk_cairo_rectangle(gc, area);
  cairo_clip(gc);
  /* clip rectangle will solve this efficiently */
  cairo_set_source_surface(gc, pt->buffer, 0, 0);
  cairo_paint(gc);

  if(pt->scroll_offs) {
    int whole_height = pt->rows * pt->cell_height;

    /* Map the whole pt->rows + pt->scrollback_current extent onto the entire
     * height of the window, and draw a brighter rectangle to represent the
     * part currently visible
     */
    int pixels_from_bottom = (whole_height * pt->scroll_offs) /
                             (pt->rows + pt->scroll_current);
    int pixels_tall = (whole_height * pt->rows) /
                      (pt->rows + pt->scroll_current);

    cairo_save(gc);

    GdkRectangle rect = {
      .x = pt->cols * pt->cell_width - CONF_scrollbar_width,
      .y = 0,
      .width = CONF_scrollbar_width,
      .height = whole_height,
    };
    gdk_cairo_rectangle(gc, &rect);
    cairo_clip(gc);
    cairo_set_source_rgba(gc,
        pt->fg_col.red   / 65535.0,
        pt->fg_col.green / 65535.0,
        pt->fg_col.blue  / 65535.0,
        0.3);
    cairo_paint(gc);

    rect.height = pixels_tall;
    rect.y = whole_height - pixels_tall - pixels_from_bottom;
    gdk_cairo_rectangle(gc, &rect);
    cairo_clip(gc);
    cairo_set_source_rgba(gc,
        pt->fg_col.red   / 65535.0,
        pt->fg_col.green / 65535.0,
        pt->fg_col.blue  / 65535.0,
        0.7);
    cairo_paint(gc);

    cairo_restore(gc);
  }

  cairo_destroy(gc);
}

static void blit_dirty(PangoTerm *pt)
{
  if(!pt->dirty_area.height || !pt->dirty_area.width)
    return;

  blit_buffer(pt, &pt->dirty_area);

  pt->dirty_area.width  = 0;
  pt->dirty_area.height = 0;
}

static void flush_pending(PangoTerm *pt)
{
  if(!pt->pending_area.width)
    return;

  cairo_t* gc = cairo_create(pt->buffer);
  GdkRectangle pending_area = pt->pending_area;
  int glyphs_x = pending_area.x;
  int glyphs_y = pending_area.y;

  if(pt->pen.attrs.dwl)
    cairo_scale(gc, 2.0, 1.0);

  if(pt->pen.attrs.dhl) {
    cairo_scale(gc, 1.0, 2.0);
    pending_area.y /= 2;
    pending_area.height /= 2;
    glyphs_y = pending_area.y;

    if(pt->pen.attrs.dhl == 2)
      glyphs_y -= pending_area.height;
  }

  /* Background fill */
  {
    cairo_save(gc);

    gdk_cairo_rectangle(gc, &pending_area);
    cairo_clip(gc);

    GdkColor bg = pt->pen.attrs.reverse ? pt->pen.fg_col : pt->pen.bg_col;
    gdk_cairo_set_source_color(gc, &bg);
    cairo_paint(gc);

    cairo_restore(gc);
  }

  if(pt->glyphs->len) {
    PangoLayout *layout = pt->pen.layout;

    pango_layout_set_text(layout, pt->glyphs->str, pt->glyphs->len);

    if(pt->pen.pangoattrs)
      pango_layout_set_attributes(layout, pt->pen.pangoattrs);

    // Now adjust all the widths
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    do {
      PangoLayoutRun *run = pango_layout_iter_get_run(iter);
      if(!run)
        continue;

      PangoGlyphString *glyph_str = run->glyphs;
      int i;
      for(i = 0; i < glyph_str->num_glyphs; i++) {
        PangoGlyphInfo *glyph = &glyph_str->glyphs[i];
        int str_index = run->item->offset + glyph_str->log_clusters[i];
        int char_width = g_array_index(pt->glyph_widths, int, str_index);
        if(glyph->geometry.width && glyph->geometry.width != char_width * pt->cell_width_pango) {
          /* Adjust its x_offset to match the width change, to ensure it still
           * remains centered in the cell */
          glyph->geometry.x_offset -= (glyph->geometry.width - char_width * pt->cell_width_pango) / 2;
          glyph->geometry.width = char_width * pt->cell_width_pango;
        }
      }
    } while(pango_layout_iter_next_run(iter));

    pango_layout_iter_free(iter);

    /* Draw glyphs */
    GdkColor fg = pt->pen.attrs.reverse ? pt->pen.bg_col : pt->pen.fg_col;
    gdk_cairo_set_source_color(gc, &fg);
    cairo_move_to(gc, glyphs_x, glyphs_y);
    pango_cairo_show_layout(gc, layout);

    g_string_truncate(pt->glyphs, 0);
  }

  if(pt->pen.attrs.dwl)
    pt->pending_area.x *= 2, pt->pending_area.width *= 2;

  if(pt->dirty_area.width && pt->pending_area.height)
    gdk_rectangle_union(&pt->pending_area, &pt->dirty_area, &pt->dirty_area);
  else
    pt->dirty_area = pt->pending_area;

  pt->pending_area.width = 0;
  pt->pending_area.height = 0;
  pt->erase_columns = 0;

  cairo_destroy(gc);
}

static void put_glyph(PangoTerm *pt, const uint32_t chars[], int width, VTermPos pos)
{
  PhyPos ph_pos = PHYSPOS_FROM_VTERMPOS(pt, pos);
  if(ph_pos.prow < 0 || ph_pos.prow >= pt->rows)
    return;

  GdkRectangle destarea = GDKRECTANGLE_FROM_PHYPOS_CELLS(pt, ph_pos, width);

  if(pt->erase_columns)
    flush_pending(pt);
  if(destarea.y != pt->pending_area.y || destarea.x != pt->pending_area.x + pt->pending_area.width)
    flush_pending(pt);

  char *chars_str = g_ucs4_to_utf8(chars, -1, NULL, NULL, NULL);

  g_array_set_size(pt->glyph_widths, pt->glyphs->len + 1);
  g_array_index(pt->glyph_widths, int, pt->glyphs->len) = width;

  g_string_append(pt->glyphs, chars_str);

  g_free(chars_str);

  if(pt->pending_area.width && pt->pending_area.height)
    gdk_rectangle_union(&destarea, &pt->pending_area, &pt->pending_area);
  else
    pt->pending_area = destarea;
}

static void put_erase(PangoTerm *pt, int width, VTermPos pos)
{
  PhyPos ph_pos = PHYSPOS_FROM_VTERMPOS(pt, pos);
  if(ph_pos.prow < 0 || ph_pos.prow >= pt->rows)
    return;

  GdkRectangle destarea = GDKRECTANGLE_FROM_PHYPOS_CELLS(pt, ph_pos, width);

  if(!pt->erase_columns)
    flush_pending(pt);
  if(destarea.y != pt->pending_area.y || destarea.x != pt->pending_area.x + pt->pending_area.width)
    flush_pending(pt);

  if(pt->pending_area.width && pt->pending_area.height)
    gdk_rectangle_union(&destarea, &pt->pending_area, &pt->pending_area);
  else
    pt->pending_area = destarea;

  pt->erase_columns += width;
}

static void chpen(VTermScreenCell *cell, void *user_data, int cursoroverride)
{
  PangoTerm *pt = user_data;
  GdkColor col;

#define ADDATTR(a) \
  do { \
    PangoAttribute *newattr = (a); \
    newattr->start_index = 0; \
    newattr->end_index = -1; \
    pango_attr_list_change(pt->pen.pangoattrs, newattr); \
  } while(0)

  if(cell->attrs.bold != pt->pen.attrs.bold) {
    int bold = pt->pen.attrs.bold = cell->attrs.bold;
    flush_pending(pt);
    ADDATTR(pango_attr_weight_new(bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL));
  }

  if(cell->attrs.underline != pt->pen.attrs.underline) {
    int underline = pt->pen.attrs.underline = cell->attrs.underline;
    flush_pending(pt);
    ADDATTR(pango_attr_underline_new(underline == 1 ? PANGO_UNDERLINE_SINGLE :
                                     underline == 2 ? PANGO_UNDERLINE_DOUBLE :
                                                      PANGO_UNDERLINE_NONE));
  }

  if(cell->attrs.italic != pt->pen.attrs.italic) {
    int italic = pt->pen.attrs.italic = cell->attrs.italic;
    flush_pending(pt);
    ADDATTR(pango_attr_style_new(italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL));
  }

  if(cell->attrs.reverse != pt->pen.attrs.reverse) {
    flush_pending(pt);
    pt->pen.attrs.reverse = cell->attrs.reverse;
  }

  if(cell->attrs.strike != pt->pen.attrs.strike) {
    int strike = pt->pen.attrs.strike = cell->attrs.strike;
    flush_pending(pt);
    ADDATTR(pango_attr_strikethrough_new(strike));
  }

  if(cell->attrs.font != pt->pen.attrs.font) {
    int font = pt->pen.attrs.font = cell->attrs.font;
    if(font >= pt->n_fonts)
      font = 0;
    flush_pending(pt);
    ADDATTR(pango_attr_family_new(pt->fonts[font]));
  }

  if(cell->attrs.dwl != pt->pen.attrs.dwl ||
     cell->attrs.dhl != pt->pen.attrs.dhl) {
    pt->pen.attrs.dwl = cell->attrs.dwl;
    pt->pen.attrs.dhl = cell->attrs.dhl;
    flush_pending(pt);
  }

  // Upscale 8->16bit
  col.red   = 257 * cell->fg.red;
  col.green = 257 * cell->fg.green;
  col.blue  = 257 * cell->fg.blue;

  if(cursoroverride) {
    int grey = ((int)pt->cursor_col.red + pt->cursor_col.green + pt->cursor_col.blue)*2 > 65535*3
        ? 0 : 65535;
    col.red = col.green = col.blue = grey;
  }

  if(col.red   != pt->pen.fg_col.red || col.green != pt->pen.fg_col.green || col.blue  != pt->pen.fg_col.blue) {
    flush_pending(pt);
    pt->pen.fg_col = col;
  }

  col.red   = 257 * cell->bg.red;
  col.green = 257 * cell->bg.green;
  col.blue  = 257 * cell->bg.blue;

  if(cursoroverride)
    col = pt->cursor_col;

  if(col.red   != pt->pen.bg_col.red || col.green != pt->pen.bg_col.green || col.blue  != pt->pen.bg_col.blue) {
    flush_pending(pt);
    pt->pen.bg_col = col;
  }
}

static void repaint_phyrect(PangoTerm *pt, PhyRect ph_rect)
{
  PhyPos ph_pos;

  for(ph_pos.prow = ph_rect.start_prow; ph_pos.prow < ph_rect.end_prow; ph_pos.prow++) {
    for(ph_pos.pcol = ph_rect.start_pcol; ph_pos.pcol < ph_rect.end_pcol; ) {
      VTermPos pos = VTERMPOS_FROM_PHYSPOS(pt, ph_pos);

      VTermScreenCell cell;
      fetch_cell(pt, pos, &cell);

      if(cell.attrs.dwl != pt->pending_dwl)
        flush_pending(pt);
      pt->pending_dwl = cell.attrs.dwl;

      /* Invert the RV attribute if this cell is selected */
      if(pt->highlight) {
        VTermPos start = pt->highlight_start,
                 stop  = pt->highlight_stop;

        int highlighted = (pos.row > start.row || (pos.row == start.row && pos.col >= start.col)) &&
                          (pos.row < stop.row  || (pos.row == stop.row  && pos.col <= stop.col));

        if(highlighted)
          cell.attrs.reverse = !cell.attrs.reverse;
      }

      int cursor_here = pos.row == pt->cursorpos.row && pos.col == pt->cursorpos.col;
      int cursor_visible = CURSOR_ENABLED(pt) && (pt->cursor_blinkstate || !pt->has_focus);

      chpen(&cell, pt, cursor_visible && cursor_here && pt->cursor_shape == VTERM_PROP_CURSORSHAPE_BLOCK);

      if(cell.chars[0] == 0) {
        put_erase(pt, cell.width, pos);
      }
      else {
        put_glyph(pt, cell.chars, cell.width, pos);
      }

      if(cursor_visible && cursor_here && pt->cursor_shape != VTERM_PROP_CURSORSHAPE_BLOCK) {
        flush_pending(pt);

        cairo_t *gc = cairo_create(pt->buffer);

        GdkRectangle cursor_area = GDKRECTANGLE_FROM_PHYPOS_CELLS(pt, ph_pos, 1);
        gdk_cairo_rectangle(gc, &cursor_area);
        cairo_clip(gc);

        switch(pt->cursor_shape) {
        case VTERM_PROP_CURSORSHAPE_UNDERLINE:
          gdk_cairo_set_source_color(gc, &pt->cursor_col);
          cairo_rectangle(gc,
              cursor_area.x,
              cursor_area.y + (int)(cursor_area.height * 0.85),
              cursor_area.width,
              (int)(cursor_area.height * 0.15));
          cairo_fill(gc);
          break;
        case VTERM_PROP_CURSORSHAPE_BAR_LEFT:
          gdk_cairo_set_source_color(gc, &pt->cursor_col);
          cairo_rectangle(gc,
              cursor_area.x,
              cursor_area.y,
              (cursor_area.width * 0.15),
              cursor_area.height);
          cairo_fill(gc);
          break;
        }

        cairo_destroy(gc);
      }

      ph_pos.pcol += cell.width;
    }
  }
}

static void repaint_rect(PangoTerm *pt, VTermRect rect)
{
  PhyRect ph_rect = PHYRECT_FROM_VTERMRECT(pt, rect);
  repaint_phyrect(pt, ph_rect);
}

static void repaint_cell(PangoTerm *pt, VTermPos pos)
{
  VTermRect rect = {
    .start_col = pos.col,
    .end_col   = pos.col + 1,
    .start_row = pos.row,
    .end_row   = pos.row + 1,
  };

  repaint_rect(pt, rect);
}

static void repaint_flow(PangoTerm *pt, VTermPos start, VTermPos stop)
{
  VTermRect rect;

  if(start.row == stop.row) {
    rect.start_col = start.col;
    rect.start_row = start.row;
    rect.end_col   = stop.col + 1;
    rect.end_row   = start.row + 1;
    repaint_rect(pt, rect);
  }
  else {
    rect.start_col = start.col;
    rect.start_row = start.row;
    rect.end_col   = pt->cols;
    rect.end_row   = start.row + 1;
    repaint_rect(pt, rect);

    if(start.row + 1 < stop.row) {
      rect.start_col = 0;
      rect.start_row = start.row + 1;
      rect.end_col   = pt->cols;
      rect.end_row   = stop.row;
      repaint_rect(pt, rect);
    }

    rect.start_col = 0;
    rect.start_row = stop.row;
    rect.end_col   = stop.col + 1;
    rect.end_row   = stop.row + 1;
    repaint_rect(pt, rect);
  }
}

static gboolean cursor_blink(void *user_data)
{
  PangoTerm *pt = user_data;

  pt->cursor_blinkstate = !pt->cursor_blinkstate;

  if(CURSOR_ENABLED(pt)) {
    repaint_cell(pt, pt->cursorpos);

    flush_pending(pt);
    blit_dirty(pt);
  }

  return TRUE;
}

static void cursor_start_blinking(PangoTerm *pt)
{
  if(!CONF_cursor_blink_interval)
    return;

  pt->cursor_timer_id = g_timeout_add(CONF_cursor_blink_interval, cursor_blink, pt);

  /* Should start blinking in visible state */
  pt->cursor_blinkstate = 1;

  if(CURSOR_ENABLED(pt))
    repaint_cell(pt, pt->cursorpos);
}

static void cursor_stop_blinking(PangoTerm *pt)
{
  g_source_remove(pt->cursor_timer_id);
  pt->cursor_timer_id = 0;

  /* Should always be in visible state */
  pt->cursor_blinkstate = 1;

  if(CURSOR_ENABLED(pt))
    repaint_cell(pt, pt->cursorpos);
}

static void store_clipboard(PangoTerm *pt)
{
  VTermPos start = pt->highlight_start,
           stop  = pt->highlight_stop;

  gchar *text = fetch_flow_text(pt, start, stop);

  gtk_clipboard_clear(pt->selection_primary);
  gtk_clipboard_set_text(pt->selection_primary, text, -1);

  free(text);
}

static void cancel_highlight(PangoTerm *pt)
{
  if(!pt->highlight)
    return;

  pt->highlight = 0;

  repaint_flow(pt, pt->highlight_start, pt->highlight_stop);
  flush_pending(pt);
  blit_dirty(pt);
}

/*
 * VTerm event handlers
 */

static int term_damage(VTermRect rect, void *user_data)
{
  PangoTerm *pt = user_data;

  if(pt->highlight) {
    if((pt->highlight_start.row < rect.end_row - 1 ||
        (pt->highlight_start.row == rect.end_row - 1 && pt->highlight_start.col < rect.end_col - 1)) &&
       (pt->highlight_stop.row > rect.start_row ||
        (pt->highlight_stop.row == rect.start_row && pt->highlight_stop.col > rect.start_col))) {
      /* Damage overlaps highlighted region */
      cancel_highlight(pt);
    }
  }

  repaint_rect(pt, rect);

  return 1;
}

static int term_sb_pushline(int cols, const VTermScreenCell *cells, void *user_data)
{
  PangoTerm *pt = user_data;

  PangoTermScrollbackLine *linebuffer = NULL;
  if(pt->scroll_current == pt->scroll_size) {
    /* Recycle old row if it's the right size */
    if(pt->sb_buffer[pt->scroll_current-1]->cols == cols)
      linebuffer = pt->sb_buffer[pt->scroll_current-1];
    else
      free(pt->sb_buffer[pt->scroll_current-1]);

    memmove(pt->sb_buffer + 1, pt->sb_buffer, sizeof(pt->sb_buffer[0]) * (pt->scroll_current - 1));
  }
  else if(pt->scroll_current > 0) {
    memmove(pt->sb_buffer + 1, pt->sb_buffer, sizeof(pt->sb_buffer[0]) * pt->scroll_current);
  }

  if(!linebuffer) {
    linebuffer = g_malloc0(sizeof(PangoTermScrollbackLine) + cols * sizeof(linebuffer->cells[0]));
    linebuffer->cols = cols;
  }

  pt->sb_buffer[0] = linebuffer;

  if(pt->scroll_current < pt->scroll_size)
    pt->scroll_current++;

  memcpy(linebuffer->cells, cells, sizeof(cells[0]) * cols);

  return 1;
}

static int term_sb_popline(int cols, VTermScreenCell *cells, void *user_data)
{
  PangoTerm *pt = user_data;

  if(!pt->scroll_current)
    return 0;

  PangoTermScrollbackLine *linebuffer = pt->sb_buffer[0];
  pt->scroll_current--;
  memmove(pt->sb_buffer, pt->sb_buffer + 1, sizeof(pt->sb_buffer[0]) * (pt->scroll_current));

  int cols_to_copy = cols;
  if(cols_to_copy > linebuffer->cols)
    cols_to_copy = linebuffer->cols;

  memcpy(cells, linebuffer->cells, sizeof(cells[0]) * cols_to_copy);

  for(int col = cols_to_copy; col < cols; col++) {
    cells[col].chars[0] = 0;
    cells[col].width = 1;
  }

  free(linebuffer);

  return 1;
}

static int term_moverect(VTermRect dest, VTermRect src, void *user_data)
{
  PangoTerm *pt = user_data;

  flush_pending(pt);
  blit_dirty(pt);

  if(pt->highlight) {
    int start_inside = vterm_rect_contains(src, pt->highlight_start);
    int stop_inside  = vterm_rect_contains(src, pt->highlight_stop);

    if(start_inside && stop_inside &&
        (pt->highlight_start.row == pt->highlight_stop.row ||
         (src.start_col == 0 && src.end_col == pt->cols))) {
      int delta_row = dest.start_row - src.start_row;
      int delta_col = dest.start_col - src.start_col;

      pt->highlight_start.row += delta_row;
      pt->highlight_start.col += delta_col;
      pt->highlight_stop.row  += delta_row;
      pt->highlight_stop.col  += delta_col;
    }
    else if(start_inside || stop_inside) {
      cancel_highlight(pt);
    }
  }

  PhyRect ph_dest = PHYRECT_FROM_VTERMRECT(pt, dest);

  if(ph_dest.end_prow < 0 || ph_dest.start_prow >= pt->rows)
    return 1;
  if(ph_dest.start_prow < 0)
    ph_dest.start_prow = 0;
  if(ph_dest.end_prow >= pt->rows)
    ph_dest.end_prow = pt->rows;

  GdkRectangle destarea = GDKRECTANGLE_FROM_PHYRECT(pt, ph_dest);

  cairo_surface_flush(pt->buffer);
  cairo_t* gc = cairo_create(pt->buffer);
  gdk_cairo_rectangle(gc, &destarea);
  cairo_clip(gc);
  cairo_set_source_surface(gc,
      pt->buffer,
      (dest.start_col - src.start_col) * pt->cell_width,
      (dest.start_row - src.start_row) * pt->cell_height);
  cairo_paint(gc);

  cairo_destroy(gc);
  blit_buffer(pt, &destarea);

  return 1;
}

static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user_data)
{
  PangoTerm *pt = user_data;

  pt->cursorpos = pos;
  pt->cursor_blinkstate = 1;

  return 1;
}

static int term_settermprop(VTermProp prop, VTermValue *val, void *user_data)
{
  PangoTerm *pt = user_data;

  switch(prop) {
  case VTERM_PROP_CURSORVISIBLE:
    pt->cursor_visible = val->boolean;
    break;

  case VTERM_PROP_CURSORBLINK:
    if(val->boolean && !pt->cursor_timer_id)
      cursor_start_blinking(pt);
    else if(!val->boolean && pt->cursor_timer_id)
      cursor_stop_blinking(pt);
    break;

  case VTERM_PROP_CURSORSHAPE:
    pt->cursor_shape = val->number;
    break;

  case VTERM_PROP_ICONNAME:
    gdk_window_set_icon_name(GDK_WINDOW(pt->termwin->window), val->string);
    break;

  case VTERM_PROP_TITLE:
    gtk_window_set_title(GTK_WINDOW(pt->termwin), val->string);
    break;

  case VTERM_PROP_ALTSCREEN:
    pt->on_altscreen = val->boolean;
    return 1;

  default:
    return 0;
  }

  return 1;
}

static int term_setmousefunc(VTermMouseFunc func, void *data, void *user_data)
{
  PangoTerm *pt = user_data;

  pt->mousefunc = func;
  pt->mousedata = data;

  return 1;
}

static int term_bell(void *user_data)
{
  PangoTerm *pt = user_data;

  gtk_widget_error_bell(GTK_WIDGET(pt->termwin));
  return 1;
}

static VTermScreenCallbacks cb = {
  .damage       = term_damage,
  .moverect     = term_moverect,
  .movecursor   = term_movecursor,
  .settermprop  = term_settermprop,
  .setmousefunc = term_setmousefunc,
  .bell         = term_bell,
  .sb_pushline  = term_sb_pushline,
  .sb_popline   = term_sb_popline,
};

static void scroll_delta(PangoTerm *pt, int delta)
{
  if(pt->on_altscreen)
    return;

  if(delta > 0) {
    if(pt->scroll_offs + delta > pt->scroll_current)
      delta = pt->scroll_current - pt->scroll_offs;
  }
  else if(delta < 0) {
    if(delta < -pt->scroll_offs)
      delta = -pt->scroll_offs;
  }

  if(!delta)
    return;

  pt->scroll_offs += delta;

  pt->cursor_hidden_for_redraw = 1;
  repaint_cell(pt, pt->cursorpos);

  PhyRect ph_repaint = {
      .start_pcol = 0,
      .end_pcol   = pt->cols,
      .start_prow = 0,
      .end_prow   = pt->rows,
  };

  if(abs(delta) < pt->rows) {
    PhyRect ph_dest = {
      .start_pcol = 0,
      .end_pcol   = pt->cols,
      .start_prow = 0,
      .end_prow   = pt->rows,
    };

    if(delta > 0) {
      ph_dest.start_prow  = delta;
      ph_repaint.end_prow = delta;
    }
    else {
      ph_dest.end_prow      = pt->rows + delta;
      ph_repaint.start_prow = pt->rows + delta;
    }

    GdkRectangle destarea = GDKRECTANGLE_FROM_PHYRECT(pt, ph_dest);

    cairo_surface_flush(pt->buffer);
    cairo_t *gc = cairo_create(pt->buffer);
    gdk_cairo_rectangle(gc, &destarea);
    cairo_clip(gc);
    cairo_set_source_surface(gc, pt->buffer, 0, delta * pt->cell_height);
    cairo_paint(gc);

    cairo_destroy(gc);
  }

  repaint_phyrect(pt, ph_repaint);

  pt->cursor_hidden_for_redraw = 0;
  repaint_cell(pt, pt->cursorpos);

  flush_pending(pt);

  GdkRectangle whole_screen = {
    .x = 0,
    .y = 0,
    .width  = pt->cols * pt->cell_width,
    .height = pt->rows * pt->cell_height,
  };
  blit_buffer(pt, &whole_screen);
}

/*
 * GTK widget event handlers
 */

static gboolean widget_keypress(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  PangoTerm *pt = user_data;
  /* GtkIMContext will eat a Shift-Space and not tell us about shift.
   * Also don't let IME eat any GDK_KEY_KP_ events
   */
  gboolean ret = (event->state & GDK_SHIFT_MASK && event->keyval == ' ') ? FALSE
               : (event->keyval >= GDK_KEY_KP_Space && GDK_KEY_KP_Divide) ? FALSE
               : gtk_im_context_filter_keypress(pt->im_context, event);

  if(ret)
    return TRUE;

  // We don't need to track the state of modifier bits
  if(event->is_modifier)
    return FALSE;

  if((event->keyval == GDK_KEY_Insert && event->state & GDK_SHIFT_MASK) ||
     ((event->keyval == 'v' || event->keyval == 'V') &&
      event->state & GDK_CONTROL_MASK && event->state & GDK_SHIFT_MASK)) {
    /* Shift-Insert or Ctrl-Shift-V pastes clipboard */
    gchar *str = gtk_clipboard_wait_for_text(pt->selection_clipboard);
    lf_to_cr(str);

    term_push_string(pt, str);
    return TRUE;
  }
  if((event->keyval == 'c' || event->keyval == 'C') &&
     event->state & GDK_CONTROL_MASK && event->state & GDK_SHIFT_MASK) {
    /* Ctrl-Shift-C copies to clipboard */
    if(!pt->highlight)
      return TRUE;

    gchar *text = fetch_flow_text(pt, pt->highlight_start, pt->highlight_stop);

    gtk_clipboard_clear(pt->selection_clipboard);
    gtk_clipboard_set_text(pt->selection_clipboard, text, -1);

    free(text);
    return TRUE;
  }
  if(event->keyval == GDK_KEY_Page_Down && event->state & GDK_SHIFT_MASK) {
    scroll_delta(pt, -pt->rows / 2);
    return TRUE;
  }
  if(event->keyval == GDK_KEY_Page_Up && event->state & GDK_SHIFT_MASK) {
    scroll_delta(pt, +pt->rows / 2);
    return TRUE;
  }

  VTermModifier state = convert_modifier(event->state);
  VTermKey keyval = convert_keyval(event->keyval, &state);

  /*
   * See also
   *   /usr/include/gtk-2.0/gdk/gdkkeysyms.h
   */

  if(keyval)
    vterm_input_push_key(pt->vt, state, keyval);
  else if(event->keyval >= 0x10000000) /* Extension key, not printable Unicode */
    return FALSE;
  else if(event->keyval >= 0x01000000) /* Unicode shifted */
    vterm_input_push_char(pt->vt, state, event->keyval - 0x01000000);
  else if(event->keyval < 0x0f00)
    /* event->keyval already contains a Unicode codepoint so that's easy */
    vterm_input_push_char(pt->vt, state, event->keyval);
  else if(event->keyval >= GDK_KEY_KP_0 && event->keyval <= GDK_KEY_KP_9)
    /* event->keyval is a keypad number; just treat it as Unicode */
    vterm_input_push_char(pt->vt, state, event->keyval - GDK_KEY_KP_0 + '0');
  else
    return FALSE;

  if(CONF_unscroll_on_key && pt->scroll_offs)
    scroll_delta(pt, -pt->scroll_offs);

  term_flush_output(pt);

  return FALSE;
}

static gboolean widget_mousepress(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  PhyPos ph_pos = {
    .pcol = event->x / pt->cell_width,
    .prow = event->y / pt->cell_height,
  };

  /* If the mouse is being dragged, we'll get motion events even outside our
   * window */
  int is_inside = (ph_pos.pcol >= 0 && ph_pos.pcol < pt->cols &&
                   ph_pos.prow >= 0 && ph_pos.prow < pt->rows);

  VTermPos pos = VTERMPOS_FROM_PHYSPOS(pt, ph_pos);

  /* Shift modifier bypasses terminal mouse handling */
  if(pt->mousefunc && !(event->state & GDK_SHIFT_MASK) && is_inside) {
    VTermModifier state = convert_modifier(event->state);
    int is_press;
    switch(event->type) {
    case GDK_BUTTON_PRESS:
      is_press = 1;
      break;
    case GDK_BUTTON_RELEASE:
      is_press = 0;
      break;
    default:
      return TRUE;
    }
    (*pt->mousefunc)(pos.col, pos.row, event->button, is_press, state, pt->mousedata);
    term_flush_output(pt);
  }
  else if(event->button == 2 && event->type == GDK_BUTTON_PRESS && is_inside) {
    /* Middle-click pastes primary selection */
    gchar *str = gtk_clipboard_wait_for_text(pt->selection_primary);
    if(!str)
      return FALSE;

    lf_to_cr(str);

    term_push_string(pt, str);
  }
  else if(event->button == 1 && event->type == GDK_BUTTON_PRESS && is_inside) {
    cancel_highlight(pt);

    pt->dragging = DRAG_PENDING;
    pt->drag_start = pos;
  }
  else if(event->button == 1 && event->type == GDK_BUTTON_RELEASE && pt->dragging != NO_DRAG) {
    /* Always accept a release even when outside */
    pt->dragging = NO_DRAG;

    store_clipboard(pt);
  }
  else if(event->button == 1 && event->type == GDK_2BUTTON_PRESS && is_inside) {
    /* Highlight a word. start with the position, and extend it both sides
     * over word characters
     */
    VTermPos start_pos = pos;
    while(start_pos.col > 0 || start_pos.row > 0) {
      VTermPos cellpos = start_pos;
      VTermScreenCell cell;

      pos_prev(pt, &cellpos);
      fetch_cell(pt, cellpos, &cell);
      if(!is_wordchar(cell.chars[0]))
        break;

      start_pos = cellpos;
    }

    VTermPos stop_pos = pos;
    while(stop_pos.col < pt->cols - 1 || stop_pos.row < pt->rows - 1) {
      VTermPos cellpos = stop_pos;
      VTermScreenCell cell;

      pos_next(pt, &cellpos);
      fetch_cell(pt, cellpos, &cell);
      if(!is_wordchar(cell.chars[0]))
        break;

      stop_pos = cellpos;
    }

    pt->highlight = 1;
    pt->highlight_start = start_pos;
    pt->highlight_stop  = stop_pos;

    repaint_flow(pt, pt->highlight_start, pt->highlight_stop);
    flush_pending(pt);
    blit_dirty(pt);
    store_clipboard(pt);
  }
  else if(event->button == 1 && event->type == GDK_3BUTTON_PRESS && is_inside) {
    /* Highlight an entire line */
    pt->highlight = 1;
    pt->highlight_start.row = pos.row;
    pt->highlight_start.col = 0;
    pt->highlight_stop.row  = pos.row;
    pt->highlight_stop.col  = pt->cols - 1;

    repaint_flow(pt, pt->highlight_start, pt->highlight_stop);
    flush_pending(pt);
    blit_dirty(pt);
    store_clipboard(pt);
  }

  return FALSE;
}

static gboolean widget_mousemove(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  PhyPos ph_pos = {
    .pcol = event->x / pt->cell_width,
    .prow = event->y / pt->cell_height,
  };

  /* If the mouse is being dragged, we'll get motion events even outside our
   * window */
  int is_inside = (ph_pos.pcol >= 0 && ph_pos.pcol < pt->cols &&
                   ph_pos.prow >= 0 && ph_pos.prow < pt->rows);

  if(ph_pos.pcol < 0)         ph_pos.pcol = 0;
  if(ph_pos.pcol > pt->cols)  ph_pos.pcol = pt->cols; /* allow off-by-1 */
  if(ph_pos.prow < 0)         ph_pos.prow = 0;
  if(ph_pos.prow >= pt->rows) ph_pos.prow = pt->rows - 1;

  VTermPos pos = VTERMPOS_FROM_PHYSPOS(pt, ph_pos);

  /* Shift modifier bypasses terminal mouse handling */
  if(pt->mousefunc && !(event->state & GDK_SHIFT_MASK) && is_inside) {
    if(pos.row < 0 || pos.row >= pt->rows)
      return TRUE;
    VTermModifier state = convert_modifier(event->state);
    (*pt->mousefunc)(pos.col, pos.row, 0, 0, state, pt->mousedata);
    term_flush_output(pt);
  }
  else if(event->state & GDK_BUTTON1_MASK) {
    VTermPos old_end = pt->dragging == DRAGGING ? pt->drag_pos : pt->drag_start;

    if(pos.row == old_end.row && pos.col == old_end.col)
      /* Unchanged; stop here */
      return FALSE;

    pt->dragging = DRAGGING;
    pt->drag_pos = pos;

    VTermPos pos_left1 = pt->drag_pos;
    if(pos_left1.col > 0) pos_left1.col--;

    if(fetch_is_eol(pt, pos_left1))
      pt->drag_pos.col = pt->cols;

    pt->highlight = 1;
    if(vterm_pos_cmp(pt->drag_start, pt->drag_pos) > 0) {
      pt->highlight_start = pt->drag_pos;
      pt->highlight_stop  = pt->drag_start;
    }
    else {
      pt->highlight_start = pt->drag_start;
      pt->highlight_stop  = pt->drag_pos;
      pt->highlight_stop.col--; /* exclude partial cell */
    }

    if(vterm_pos_cmp(old_end, pt->drag_pos) > 0)
      repaint_flow(pt, pt->drag_pos, old_end);
    else
      repaint_flow(pt, old_end, pt->drag_pos);

    flush_pending(pt);
    blit_dirty(pt);
  }

  return FALSE;
}

static gboolean widget_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  PhyPos ph_pos = {
    .pcol = event->x / pt->cell_width,
    .prow = event->y / pt->cell_height,
  };

  if(pt->mousefunc && !(event->state & GDK_SHIFT_MASK)) {
    VTermPos pos = VTERMPOS_FROM_PHYSPOS(pt, ph_pos);
    if(pos.row < 0 || pos.row >= pt->rows)
      return TRUE;

    /* Translate scroll direction back into a button number */

    int button;
    switch(event->direction) {
      case GDK_SCROLL_UP:    button = 4; break;
      case GDK_SCROLL_DOWN:  button = 5; break;
      default:
                             return FALSE;
    }
    VTermModifier state = convert_modifier(event->state);

    (*pt->mousefunc)(pos.col, pos.row, button, 1, state, pt->mousedata);
    term_flush_output(pt);
  }
  else {
    switch(event->direction) {
      case GDK_SCROLL_UP:   scroll_delta(pt, +3); break;
      case GDK_SCROLL_DOWN: scroll_delta(pt, -3); break;
      default:              return FALSE;
    }
  }

  return FALSE;
}

static gboolean widget_im_commit(GtkIMContext *context, gchar *str, gpointer user_data)
{
  PangoTerm *pt = user_data;

  term_push_string(pt, str);

  if(CONF_unscroll_on_key && pt->scroll_offs)
    scroll_delta(pt, -pt->scroll_offs);

  return FALSE;
}

static gboolean widget_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  /* GDK always sends resize events before expose events, so it's possible this
   * expose event is for a region that now doesn't exist.
   */
  int right  = pt->cols * pt->cell_width;
  int bottom = pt->rows * pt->cell_height;

  /* Trim to still-valid area, or ignore if there's nothing remaining */
  if(event->area.x + event->area.width > right)
    event->area.width = right - event->area.x;
  if(event->area.y + event->area.height > bottom)
    event->area.height = bottom - event->area.y;

  if(event->area.height && event->area.width)
    blit_buffer(pt, &event->area);

  return TRUE;
}

static void widget_resize(GtkContainer* widget, gpointer user_data)
{
  PangoTerm *pt = user_data;

  gint raw_width, raw_height;
  gtk_window_get_size(GTK_WINDOW(widget), &raw_width, &raw_height);

  int cols = raw_width  / pt->cell_width;
  int rows = raw_height / pt->cell_height;

  if(cols == pt->cols && rows == pt->rows)
    return;

  pt->cols = cols;
  pt->rows = rows;

  if(pt->resizedfn)
    (*pt->resizedfn)(rows, cols, pt->resizedfn_data);

  cairo_surface_t* new_buffer = gdk_window_create_similar_surface(pt->termdraw,
      CAIRO_CONTENT_COLOR,
      cols * pt->cell_width,
      rows * pt->cell_height);

  cairo_t* gc = cairo_create(new_buffer);
  cairo_set_source_surface(gc, pt->buffer, 0, 0);
  cairo_paint(gc);
  cairo_destroy(gc);

  cairo_surface_destroy(pt->buffer);
  pt->buffer = new_buffer;

  vterm_set_size(pt->vt, rows, cols);
  vterm_screen_flush_damage(pt->vts);

  return;
}

static void widget_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  PangoTerm *pt = user_data;
  pt->has_focus = 1;

  if(CURSOR_ENABLED(pt)) {
    repaint_cell(pt, pt->cursorpos);

    flush_pending(pt);
    blit_dirty(pt);
  }
}

static void widget_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  PangoTerm *pt = user_data;
  pt->has_focus = 0;

  if(CURSOR_ENABLED(pt)) {
    repaint_cell(pt, pt->cursorpos);

    flush_pending(pt);
    blit_dirty(pt);
  }
}

static void widget_quit(GtkContainer* widget, gpointer unused_data)
{
  gtk_main_quit();
}

static GdkPixbuf *load_icon(GdkColor *background)
{
  /* This technique stolen from 
   *   http://git.gnome.org/browse/gtk+/tree/gtk/gtkicontheme.c#n3180
   */

  gchar *str = g_strdup_printf(
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
      "<svg version=\"1.1\"\n"
      "     xmlns=\"http://www.w3.org/2000/svg\"\n"
      "     xmlns:xi=\"http://www.w3.org/2001/XInclude\"\n"
      "     width=\"64\"\n"
      "     height=\"64\">\n"
      "  <style type=\"text/css\">\n"
      "    #screen {\n"
      "      fill: #%02x%02x%02x !important;\n"
      "    }\n"
      "  </style>\n"
      "  <xi:include href=\"%s/pixmaps/pangoterm.svg" "\"/>\n"
      "</svg>",
      background->red   / 255,
      background->green / 255,
      background->blue  / 255,
      PANGOTERM_SHAREDIR);

  GInputStream *stream = g_memory_input_stream_new_from_data(str, -1, g_free);

  GdkPixbuf *ret = gdk_pixbuf_new_from_stream(stream, NULL, NULL);

  g_object_unref(stream);

  return ret;
}

PangoTerm *pangoterm_new(int rows, int cols)
{
  PangoTerm *pt = g_new0(PangoTerm, 1);

  pt->rows = rows;
  pt->cols = cols;

  pt->writefn = NULL;
  pt->resizedfn = NULL;

  pt->n_fonts = 1;
  pt->fonts = malloc(sizeof(char *) * 2);
  pt->fonts[0] = g_strdup("Monospace");
  pt->fonts[1] = NULL;
  pt->font_size = CONF_size;

  gdk_color_parse(CONF_cursor, &pt->cursor_col);

  /* Create VTerm */
  pt->vt = vterm_new(rows, cols);

  /* Set up parser */
  vterm_parser_set_utf8(pt->vt, 1);

  /* Set up state */
  VTermState *state = vterm_obtain_state(pt->vt);
  vterm_state_set_bold_highbright(state, CONF_bold_highbright);

  /* Set up screen */
  pt->vts = vterm_obtain_screen(pt->vt);
  vterm_screen_enable_altscreen(pt->vts, CONF_altscreen);
  vterm_screen_set_callbacks(pt->vts, &cb, pt);
  vterm_screen_set_damage_merge(pt->vts, VTERM_DAMAGE_SCROLL);

  /* Set up GTK widget */

  pt->termwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_double_buffered(pt->termwin, FALSE);

  pt->glyphs = g_string_sized_new(128);
  pt->glyph_widths = g_array_new(FALSE, FALSE, sizeof(int));

  gtk_widget_realize(pt->termwin);

  pt->termdraw = pt->termwin->window;

  gdk_window_set_cursor(GDK_WINDOW(pt->termdraw), gdk_cursor_new(GDK_XTERM));

  cursor_start_blinking(pt);
  pt->cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;

  GdkEventMask mask = gdk_window_get_events(pt->termwin->window);
  gdk_window_set_events(pt->termwin->window, mask|GDK_BUTTON_PRESS_MASK|GDK_BUTTON_RELEASE_MASK|GDK_POINTER_MOTION_MASK);

  g_signal_connect(G_OBJECT(pt->termwin), "expose-event", GTK_SIGNAL_FUNC(widget_expose), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "key-press-event", GTK_SIGNAL_FUNC(widget_keypress), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "button-press-event",   GTK_SIGNAL_FUNC(widget_mousepress), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "button-release-event", GTK_SIGNAL_FUNC(widget_mousepress), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "motion-notify-event",  GTK_SIGNAL_FUNC(widget_mousemove), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "scroll-event",  GTK_SIGNAL_FUNC(widget_scroll), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "focus-in-event",  GTK_SIGNAL_FUNC(widget_focus_in),  pt);
  g_signal_connect(G_OBJECT(pt->termwin), "focus-out-event", GTK_SIGNAL_FUNC(widget_focus_out), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "destroy", GTK_SIGNAL_FUNC(widget_quit), pt);

  pt->im_context = gtk_im_context_simple_new();

  g_signal_connect(G_OBJECT(pt->im_context), "commit", GTK_SIGNAL_FUNC(widget_im_commit), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "check-resize", GTK_SIGNAL_FUNC(widget_resize), pt);

  pt->dragging = NO_DRAG;

  pt->selection_primary   = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
  pt->selection_clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

  pt->scroll_size = CONF_scrollback_size;
  pt->sb_buffer = g_new0(PangoTermScrollbackLine*, pt->scroll_size);

  return pt;
}

void pangoterm_free(PangoTerm *pt)
{
  g_strfreev(pt->fonts);

  vterm_free(pt->vt);
}

void pangoterm_set_default_colors(PangoTerm *pt, GdkColor *fg_col, GdkColor *bg_col)
{
  pt->fg_col = *fg_col;

  VTermColor fg;
  fg.red   = fg_col->red   / 257;
  fg.green = fg_col->green / 257;
  fg.blue  = fg_col->blue  / 257;

  VTermColor bg;
  bg.red   = bg_col->red   / 257;
  bg.green = bg_col->green / 257;
  bg.blue  = bg_col->blue  / 257;

  vterm_state_set_default_colors(vterm_obtain_state(pt->vt), &fg, &bg);

  GdkColormap* colormap = gdk_colormap_get_system();
  gdk_rgb_find_color(colormap, bg_col);
  gdk_window_set_background(pt->termdraw, bg_col);

  GdkPixbuf *icon = load_icon(bg_col);
  gtk_window_set_icon(GTK_WINDOW(pt->termwin), icon);
  g_object_unref(icon);
}

void pangoterm_set_fonts(PangoTerm *pt, char *font, char **alt_fonts)
{
  int n_fonts = 1;
  while(alt_fonts[n_fonts-1])
    n_fonts++;

  g_strfreev(pt->fonts);

  pt->n_fonts = n_fonts;

  pt->fonts = malloc(sizeof(char*) * (n_fonts + 1));
  pt->fonts[0] = g_strdup(font);

  for(int i = 1; i < n_fonts; i++)
    pt->fonts[i] = g_strdup(alt_fonts[i-1]);

  pt->fonts[n_fonts] = NULL;
}

void pangoterm_set_font_size(PangoTerm *pt, double size)
{
  pt->font_size = size;
}

void pangoterm_set_title(PangoTerm *pt, const char *title)
{
  gtk_window_set_title(GTK_WINDOW(pt->termwin), title);
}

void pangoterm_set_write_fn(PangoTerm *pt, PangoTermWriteFn *fn, void *user)
{
  pt->writefn = fn;
  pt->writefn_data = user;
}

void pangoterm_set_resized_fn(PangoTerm *pt, PangoTermResizedFn *fn, void *user)
{
  pt->resizedfn = fn;
  pt->resizedfn_data = user;
}

void pangoterm_start(PangoTerm *pt)
{
  /* Finish the rest of the setup and start */

  cairo_t *cctx = gdk_cairo_create(pt->termdraw);
  PangoContext *pctx = pango_cairo_create_context(cctx);

  PangoFontDescription *fontdesc = pango_font_description_from_string(pt->fonts[0]);
  if(pango_font_description_get_size(fontdesc) == 0)
    pango_font_description_set_size(fontdesc, pt->font_size * PANGO_SCALE);

  pango_context_set_font_description(pctx, fontdesc);

  pt->pen.pangoattrs = pango_attr_list_new();
  pt->pen.layout = pango_layout_new(pctx);
  pango_layout_set_font_description(pt->pen.layout, fontdesc);

  PangoFontMetrics *metrics = pango_context_get_metrics(pctx,
      pango_context_get_font_description(pctx), pango_context_get_language(pctx));

  int width = (pango_font_metrics_get_approximate_char_width(metrics) +
               pango_font_metrics_get_approximate_digit_width(metrics)) / 2;

  int height = pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent(metrics);

  pt->cell_width_pango = width;
  pt->cell_width  = PANGO_PIXELS_CEIL(width);
  pt->cell_height = PANGO_PIXELS_CEIL(height);

  gtk_window_resize(GTK_WINDOW(pt->termwin),
      pt->cols * pt->cell_width, pt->rows * pt->cell_height);

  pt->buffer = gdk_window_create_similar_surface(pt->termdraw,
      CAIRO_CONTENT_COLOR,
      pt->cols * pt->cell_width,
      pt->rows * pt->cell_height);

  GdkGeometry hints;

  hints.min_width  = pt->cell_width;
  hints.min_height = pt->cell_height;
  hints.width_inc  = pt->cell_width;
  hints.height_inc = pt->cell_height;

  gtk_window_set_resizable(GTK_WINDOW(pt->termwin), TRUE);
  gtk_window_set_geometry_hints(GTK_WINDOW(pt->termwin), GTK_WIDGET(pt->termwin), &hints, GDK_HINT_RESIZE_INC | GDK_HINT_MIN_SIZE);

  vterm_screen_reset(pt->vts, 1);

  VTermState *state = vterm_obtain_state(pt->vt);
  vterm_state_set_termprop(state, VTERM_PROP_CURSORSHAPE, &(VTermValue){ .number = CONF_cursor_shape });

  gtk_widget_show_all(pt->termwin);
}

void pangoterm_push_bytes(PangoTerm *pt, const char *bytes, size_t len)
{
  if(CONF_unscroll_on_output && pt->scroll_offs)
    scroll_delta(pt, -pt->scroll_offs);

  vterm_push_bytes(pt->vt, bytes, len);
}

void pangoterm_begin_update(PangoTerm *pt)
{
  /* Hide cursor during damage flush */

  pt->cursor_hidden_for_redraw = 1;
  repaint_cell(pt, pt->cursorpos);
}

void pangoterm_end_update(PangoTerm *pt)
{
  vterm_screen_flush_damage(pt->vts);

  pt->cursor_hidden_for_redraw = 0;
  repaint_cell(pt, pt->cursorpos);

  flush_pending(pt);
  blit_dirty(pt);
  term_flush_output(pt);
}
