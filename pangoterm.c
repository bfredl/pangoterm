#include "pangoterm.h"

#include <string.h>  // memmove
#include <wctype.h> 
#include <ibus.h>

#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
// #include <gdk/gdkkeysyms.h>
// #include <gdk/gdkx.h>

#include "conf.h"

#undef DEBUG_SHOW_LINECONTINUATION

CONF_STRING(foreground, 0, "gray90", "Foreground colour", "COL");
CONF_STRING(background, 0, "black",  "Background colour", "COL");
CONF_STRING(cursor,     0, "white",  "Cursor colour",     "COL");

CONF_INT(border, 0, 2, "Border width", "PIXELS");

static struct {
  GdkRGBA col;
  gboolean is_set;
} colours[16];

static void apply_colour(int index, ConfigValue v)
{
  if(index < 0 || index > 16)
    return;

  colours[index].col = (GdkRGBA){ 0, 0, 0 };
  gdk_rgba_parse(&colours[index].col, v.s);
  colours[index].is_set = true;
}
CONF_PARAMETRIC_STRING(colour, 0, apply_colour, "Palette colour", "COL");

CONF_INT(cursor_shape, 0, 1, "Cursor shape (1=block 2=underbar 3=vertical bar)", "SHAPE");

CONF_DOUBLE(size, 's', 9.0, "Font size", "NUM");

CONF_INT(cursor_blink_interval, 0, 500, "Cursor blink interval", "MSEC");

CONF_BOOL(bold_highbright, 0, TRUE, "Bold is high-brightness");
CONF_BOOL(altscreen, 0, TRUE, "Alternate screen buffer switching");

CONF_BOOL(altscreen_scroll, 0, FALSE, "Emulate arrows for mouse scrolling in alternate screen buffer");

CONF_INT(scrollback_size, 0, 1000, "Scrollback size", "LINES");

CONF_INT(scrollbar_width, 0, 3, "Scroll bar width", "PIXELS");

CONF_INT(scroll_wheel_delta, 0, 3, "Number of lines to scroll on mouse wheel", "LINES");

CONF_BOOL(unscroll_on_output, 0, TRUE, "Scroll to bottom on output");
CONF_BOOL(unscroll_on_key,    0, TRUE, "Scroll to bottom on keypress");

CONF_BOOL(doubleclick_fullword, 0, FALSE, "Double-click selects fullwords (until whitespace)");

CONF_STRING(geometry, 0, "", "Initial window geometry", "GEOM");

CONF_BOOL(chord_shift_space,     0, TRUE, "Shift-Space chording");
CONF_BOOL(chord_shift_backspace, 0, TRUE, "Shift-Backspace chording");
CONF_BOOL(chord_shift_enter,     0, TRUE, "Shift-Enter chording");

#define VTERM_COLOR_FROM_GDK_COLOR(c) \
  ((VTermColor){ .rgb.type = 0, .rgb.red = (c).red * 255, .rgb.green = (c).green * 255, .rgb.blue = (c).blue * 255 })

#define GDK_COLOR_FROM_VTERM_COLOR(c) \
  ((GdkRGBA){ .red = (c).rgb.red / 255.0, .green = (c).rgb.green / 255.0, .blue = (c).rgb.blue / 255.0, .alpha = 1.0 })

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

  // GtkIMContext *im_context;
  IBusInputContext *ibuscontext;

  // TODO: not needed in GTK4 itself!
  GdkModifierType modifiers;

  int mousemode;

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
    GdkRGBA fg_col;
    GdkRGBA bg_col;
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
  char *font_italic;
  double font_size;

  int cell_width_pango;
  int cell_width;
  int cell_height;

  GdkRGBA fg_col;
  GdkRGBA bg_col;

  int has_focus;
  int cursor_visible;    /* VTERM_PROP_CURSORVISIBLE */
  int cursor_blinkstate; /* during high state of blink */
  int cursor_hidden_for_redraw; /* true to temporarily hide during redraw */
  VTermPos cursorpos;
  GdkRGBA cursor_col;
  int cursor_shape;

#define CURSOR_ENABLED(pt) ((pt)->cursor_visible && !(pt)->cursor_hidden_for_redraw)

  guint cursor_timer_id;

  GtkWidget *termwin;
  GtkWidget *termda;

  cairo_surface_t *buffer;
  GdkSurface *termdraw; // TODO: deleda est
  GdkCairoContext *cairo_context;
  /* area in buffer that needs flushing to termdraw */
  GdkRectangle dirty_area;

  /* These four positions relate to the click/drag highlight state */

  enum { NO_DRAG, DRAG_PENDING, DRAGGING } dragging;
  /* Initial mouse position of selection drag */
  VTermPos drag_start;
  /* Current mouse position of selection drag */
  VTermPos drag_pos;

  /* Start and stop bounds of the selection */
  bool highlight_valid;
  VTermPos highlight_start;
  VTermPos highlight_stop;

  GdkClipboard *selection_primary;
  GdkClipboard *selection_clipboard;

  GString *outbuffer;
  GString *tmpbuffer; /* for handling VTermStringFragment */
  bool did_set_font_size;
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
  if(state & GDK_ALT_MASK)
    mod |= VTERM_MOD_ALT;

  return mod;
}

static void flush_outbuffer(PangoTerm *pt)
{
  if(pt->outbuffer->len) {
    (*pt->writefn)(pt->outbuffer->str, pt->outbuffer->len, pt->writefn_data);
    pt->outbuffer->len = 0;
  }
}

static void term_output(const char *s, size_t len, void *user)
{
  PangoTerm *pt = user;

  g_string_append_len(pt->outbuffer, s, len);
}

static void term_push_string(PangoTerm *pt, gchar *str, gboolean paste)
{
  if(paste)
    vterm_keyboard_start_paste(pt->vt);

  while(str && str[0]) {
    vterm_keyboard_unichar(pt->vt, g_utf8_get_char(str), 0);
    str = g_utf8_next_char(str);
  }

  if(paste)
    vterm_keyboard_end_paste(pt->vt);

  flush_outbuffer(pt);
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
  int skipped_blank = 0;
  int end_blank = 0;

  VTermPos pos = {
    .row = rect.start_row,
    .col = rect.start_col,
  };
  while(pos.col < rect.end_col) {
    VTermScreenCell cell;
    fetch_cell(pt, pos, &cell);
    if(!cell.chars[0])
      skipped_blank++;
    else
      for(; skipped_blank; skipped_blank--) {
        if(str)
          str[ret] = 0x20;
        ret++;
      }

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

static void blit_buffer(PangoTerm *pt, cairo_t *gc, int height, int width)
{
  cairo_surface_flush(pt->buffer);

  int whole_width = 2 * CONF_border + pt->cols * pt->cell_width;
  bool scrollbar = width > (whole_width - CONF_scrollbar_width);

  int whole_height;
  GdkRectangle scrollbar_area;

  if(scrollbar) {
    /* Erase old scrollbar */
    whole_height = pt->rows * pt->cell_height + 2 * CONF_border;
    scrollbar_area = (GdkRectangle){
        .x = whole_width - CONF_scrollbar_width,
        .y = 0,
        .width = CONF_scrollbar_width,
        .height = whole_height,
    };

    cairo_save(gc);

    gdk_cairo_rectangle(gc, &scrollbar_area);
    cairo_clip(gc);
    cairo_set_source_rgb(gc,
        pt->bg_col.red   / 65535.0,
        pt->bg_col.green / 65535.0,
        pt->bg_col.blue  / 65535.0);
    cairo_paint(gc);

    cairo_restore(gc);
  }

  {
    cairo_save(gc);

    /* clip rectangle will solve this efficiently */
    cairo_set_source_surface(gc, pt->buffer, CONF_border, CONF_border);
    cairo_paint(gc);

    cairo_restore(gc);
  }

  if(scrollbar && pt->scroll_offs) {
    /* Map the whole pt->rows + pt->scrollback_current extent onto the entire
     * height of the window, and draw a brighter rectangle to represent the
     * part currently visible
     */
    int pixels_from_bottom = (whole_height * pt->scroll_offs) /
                             (pt->rows + pt->scroll_current);
    int pixels_tall = (whole_height * pt->rows) /
                      (pt->rows + pt->scroll_current);

    cairo_save(gc);

    gdk_cairo_rectangle(gc, &scrollbar_area);
    cairo_clip(gc);
    cairo_set_source_rgba(gc,
        pt->fg_col.red   / 65535.0,
        pt->fg_col.green / 65535.0,
        pt->fg_col.blue  / 65535.0,
        0.3);
    cairo_paint(gc);

    scrollbar_area.height = pixels_tall;
    scrollbar_area.y = whole_height - pixels_tall - pixels_from_bottom;
    gdk_cairo_rectangle(gc, &scrollbar_area);
    cairo_clip(gc);
    cairo_set_source_rgba(gc,
        pt->fg_col.red   / 65535.0,
        pt->fg_col.green / 65535.0,
        pt->fg_col.blue  / 65535.0,
        0.7);
    cairo_paint(gc);

    cairo_restore(gc);
  }

#ifdef DEBUG_SHOW_LINECONTINUATION
  {
    cairo_save(gc);

    cairo_set_source_rgba(gc,
        0.0, 1.0, 0.0,
        0.6);

    VTermState *state = vterm_obtain_state(pt->vt);

    PhyPos ph_pos = { .pcol = pt->cols - 1 };
    for(ph_pos.prow = 1; ph_pos.prow < pt->rows; ph_pos.prow++) {
      VTermPos pos = VTERMPOS_FROM_PHYSPOS(pt, ph_pos);
      if(pos.row < 1)
        continue;

      const VTermLineInfo *lineinfo = vterm_state_get_lineinfo(state, pos.row + 1);
      if(!lineinfo->continuation)
        continue;

      GdkRectangle rect = GDKRECTANGLE_FROM_PHYPOS_CELLS(pt, ph_pos, 1);
      cairo_rectangle(gc,
          rect.x + 1, rect.y + 2, rect.width - 2, rect.height - 4);
      cairo_fill(gc);
    }

    cairo_restore(gc);
  }
#endif

}

static void blit_dirty(PangoTerm *pt)
{
  if(!pt->dirty_area.height || !pt->dirty_area.width)
    return;

  gtk_widget_queue_draw(pt->termda);

  /*
  blit_buffer(pt, &(GdkRectangle){
      .x      = pt->dirty_area.x + CONF_border,
      .y      = pt->dirty_area.y + CONF_border,
      .width  = pt->dirty_area.width,
      .height = pt->dirty_area.height,
  });
  */

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

    GdkRGBA bg = pt->pen.attrs.reverse ? pt->pen.fg_col : pt->pen.bg_col;
    gdk_cairo_set_source_rgba(gc, &bg);
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
    GdkRGBA fg = pt->pen.attrs.reverse ? pt->pen.bg_col : pt->pen.fg_col;

    gdk_cairo_set_source_rgba(gc, &fg);
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

  char *chars_str = g_ucs4_to_utf8(chars, VTERM_MAX_CHARS_PER_CELL, NULL, NULL, NULL);

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
  GdkRGBA col;

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
    switch(underline) {
      case VTERM_UNDERLINE_OFF:
        ADDATTR(pango_attr_underline_new(PANGO_UNDERLINE_NONE));
        break;
      case VTERM_UNDERLINE_DOUBLE:
        ADDATTR(pango_attr_underline_new(PANGO_UNDERLINE_DOUBLE));
        break;
      case VTERM_UNDERLINE_CURLY:
        /* PANGO_UNDERLINE_ERROR is usually rendered with a wavy shape */
        ADDATTR(pango_attr_underline_new(PANGO_UNDERLINE_ERROR));
        break;
      default:
        ADDATTR(pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    }
  }

  if(cell->attrs.font != pt->pen.attrs.font) {
    int font = pt->pen.attrs.font = cell->attrs.font;
    if(font >= pt->n_fonts)
      font = 0;
    flush_pending(pt);
    ADDATTR(pango_attr_family_new(pt->fonts[font]));
  }

  if(cell->attrs.italic != pt->pen.attrs.italic) {
    int italic = pt->pen.attrs.italic = cell->attrs.italic;
    flush_pending(pt);
    if (pt->font_italic != NULL) {
        char *font = italic ? pt->font_italic : pt->fonts[0];
        ADDATTR(pango_attr_family_new(font));
    } else {
        ADDATTR(pango_attr_style_new(italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL));
    }
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

  if(cell->attrs.dwl != pt->pen.attrs.dwl ||
     cell->attrs.dhl != pt->pen.attrs.dhl) {
    pt->pen.attrs.dwl = cell->attrs.dwl;
    pt->pen.attrs.dhl = cell->attrs.dhl;
    flush_pending(pt);
  }

  vterm_screen_convert_color_to_rgb(pt->vts, &cell->fg);
  col = GDK_COLOR_FROM_VTERM_COLOR(cell->fg);

  if(cursoroverride) {
    int grey = ((int)pt->cursor_col.red + pt->cursor_col.green + pt->cursor_col.blue)*2 > 65535*3
        ? 0 : 65535;
    col.red = col.green = col.blue = grey;
  }

  if(col.red   != pt->pen.fg_col.red || col.green != pt->pen.fg_col.green || col.blue  != pt->pen.fg_col.blue) {
    flush_pending(pt);
    pt->pen.fg_col = col;
  }

  vterm_screen_convert_color_to_rgb(pt->vts, &cell->bg);
  col = GDK_COLOR_FROM_VTERM_COLOR(cell->bg);

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
      if(pt->highlight_valid) {
        VTermPos start = pt->highlight_start,
                 stop  = pt->highlight_stop;

        int highlighted = (pos.row > start.row || (pos.row == start.row && pos.col >= start.col)) &&
                          (pos.row < stop.row  || (pos.row == stop.row  && pos.col <= stop.col));

        if(highlighted)
          cell.attrs.reverse = !cell.attrs.reverse;
      }

      int cursor_here = pos.row == pt->cursorpos.row && pos.col == pt->cursorpos.col;
      int cursor_visible = CURSOR_ENABLED(pt) && (pt->cursor_blinkstate || !pt->has_focus);
      int draw_cursor = cursor_visible && cursor_here;

      chpen(&cell, pt, draw_cursor && pt->cursor_shape == VTERM_PROP_CURSORSHAPE_BLOCK);

      if(cell.chars[0] == 0) {
        put_erase(pt, cell.width, pos);
      }
      else {
        put_glyph(pt, cell.chars, cell.width, pos);
      }

      if(draw_cursor) {
        GdkRectangle cursor_area = GDKRECTANGLE_FROM_PHYPOS_CELLS(pt, ph_pos, 1);
        // gtk_im_context_set_cursor_location(pt->im_context, &cursor_area);

        if (pt->cursor_shape != VTERM_PROP_CURSORSHAPE_BLOCK) {
            flush_pending(pt);

            cairo_t *gc = cairo_create(pt->buffer);

            gdk_cairo_rectangle(gc, &cursor_area);
            cairo_clip(gc);

            switch(pt->cursor_shape) {
            case VTERM_PROP_CURSORSHAPE_UNDERLINE:
              gdk_cairo_set_source_rgba(gc, &pt->cursor_col);
              cairo_rectangle(gc,
                  cursor_area.x,
                  cursor_area.y + (int)(cursor_area.height * 0.85),
                  cursor_area.width,
                  (int)(cursor_area.height * 0.15));
              cairo_fill(gc);
              break;
            case VTERM_PROP_CURSORSHAPE_BAR_LEFT:
              gdk_cairo_set_source_rgba(gc, &pt->cursor_col);
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

  // TODO
  // gdk_clipboard_clear(pt->selection_primary);
  // gtk_clipboard_set_text(pt->selection_primary, text, -1);

  free(text);
}

static void cancel_highlight(PangoTerm *pt)
{
  if(!pt->highlight_valid)
    return;

  pt->highlight_valid = FALSE;

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

  if(pt->highlight_valid) {
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
    cells[col] = (VTermScreenCell){
      .chars = {0},
      .width = 1,
      .attrs = {},
      .fg = VTERM_COLOR_FROM_GDK_COLOR(pt->fg_col),
      .bg = VTERM_COLOR_FROM_GDK_COLOR(pt->bg_col),
    };
  }

  free(linebuffer);

  return 1;
}

static int term_moverect(VTermRect dest, VTermRect src, void *user_data)
{
  PangoTerm *pt = user_data;

  flush_pending(pt);
  blit_dirty(pt);

  if(pt->highlight_valid) {
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

  // blit_buffer(pt, &(GdkRectangle){
  //     .x      = destarea.x + CONF_border,
  //     .y      = destarea.y + CONF_border,
  //     .width  = destarea.width,
  //     .height = destarea.height,
  // });

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

  if(vterm_get_prop_type(prop) == VTERM_VALUETYPE_STRING) {
    /* Compose the string fragments into pt->tmpbuffer
     * We assume we won't see multiple fragments of different strings intermingled */
    if(val->string.initial)
      pt->tmpbuffer->len = 0;

    g_string_append_len(pt->tmpbuffer, val->string.str, val->string.len);

    if(!val->string.final)
      return 1;
  }

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
    gtk_window_set_icon_name(GTK_WINDOW(pt->termwin), pt->tmpbuffer->str);
    break;

  case VTERM_PROP_TITLE:
    gtk_window_set_title(GTK_WINDOW(pt->termwin), pt->tmpbuffer->str);
    break;

  case VTERM_PROP_ALTSCREEN:
    pt->on_altscreen = val->boolean;
    break;

  case VTERM_PROP_MOUSE:
    pt->mousemode = val->number;
    break;

  default:
    return 0;
  }

  return 1;
}

static int term_bell(void *user_data)
{
  PangoTerm *pt = user_data;

  gtk_widget_error_bell(GTK_WIDGET(pt->termwin));
  return 1;
}

static VTermScreenCallbacks cb = {
  .damage      = term_damage,
  .moverect    = term_moverect,
  .movecursor  = term_movecursor,
  .settermprop = term_settermprop,
  .bell        = term_bell,
  .sb_pushline = term_sb_pushline,
  .sb_popline  = term_sb_popline,
};

static void altscreen_scroll(PangoTerm *pt, int delta, GtkOrientation orientation)
{
  if (CONF_altscreen_scroll) {
    VTermKey which_arrow;
    if(delta > 0) {
      which_arrow = ((orientation == GTK_ORIENTATION_VERTICAL) ? VTERM_KEY_UP : VTERM_KEY_RIGHT);
    } else if(delta < 0) {
      which_arrow = ((orientation == GTK_ORIENTATION_VERTICAL) ? VTERM_KEY_DOWN : VTERM_KEY_LEFT);
    }
    for(int i=0; i < ((delta <= -1) ? -delta : delta); i++) {
      vterm_keyboard_key(pt->vt, which_arrow, 0);
    }
    flush_outbuffer(pt);
  }
}

static void hscroll_delta(PangoTerm *pt, int delta)
{
  if(pt->on_altscreen) {
    altscreen_scroll(pt, delta, GTK_ORIENTATION_HORIZONTAL);
  }
}

static void vscroll_delta(PangoTerm *pt, int delta)
{
  if(pt->on_altscreen) {
    altscreen_scroll(pt, delta, GTK_ORIENTATION_VERTICAL);
    return;
  }

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

  gtk_widget_queue_draw(pt->termda);
}


// IBUS {{{
static IBusBus *_bus;

static void
_ibus_context_commit_text_cb (IBusInputContext *ibuscontext,
                              IBusText         *text,
                              PangoTerm    *ibusimcontext)
{
    fprintf(stderr, "very text: %s\n", text->text);
}

static void
_ibus_context_forward_key_event_cb (IBusInputContext  *ibuscontext,
                                    guint              keyval,
                                    guint              keycode,
                                    guint              state,
                                    PangoTerm     *ibusimcontext)
{

    fprintf(stderr, "very tangent: %d-%d\n", keyval, state);
}

static void
_create_input_context_done (IBusBus       *bus,
                            GAsyncResult  *res,
                            PangoTerm *pt)
{
    GError *error = NULL;
    IBusInputContext *context = ibus_bus_create_input_context_async_finish (
            _bus, res, &error);
    fprintf(stderr, "DONE context=%p\n", context);

    // if (ibusimcontext->cancellable != NULL) {
    //     g_object_unref (ibusimcontext->cancellable);
    //     ibusimcontext->cancellable = NULL;
    // }

    if (context == NULL) {
        g_warning ("Create input context failed: %s.", error->message);
        g_error_free (error);
    }
    else {
        ibus_input_context_set_client_commit_preedit (context, false);
        pt->ibuscontext = context;

        g_signal_connect (pt->ibuscontext,
                          "commit-text",
                          G_CALLBACK (_ibus_context_commit_text_cb),
                          pt);
        g_signal_connect (pt->ibuscontext,
                          "forward-key-event",
                          G_CALLBACK (_ibus_context_forward_key_event_cb),
                          pt);
        /*
        g_signal_connect (ibusimcontext->ibuscontext,
                          "delete-surrounding-text",
                          G_CALLBACK (_ibus_context_delete_surrounding_text_cb),
                          ibusimcontext);
        g_signal_connect (ibusimcontext->ibuscontext,
                          "update-preedit-text-with-mode",
                          G_CALLBACK (_ibus_context_update_preedit_text_cb),
                          ibusimcontext);
        g_signal_connect (ibusimcontext->ibuscontext,
                          "show-preedit-text",
                          G_CALLBACK (_ibus_context_show_preedit_text_cb),
                          ibusimcontext);
        g_signal_connect (ibusimcontext->ibuscontext,
                          "hide-preedit-text",
                          G_CALLBACK (_ibus_context_hide_preedit_text_cb),
                          ibusimcontext);
        g_signal_connect (ibusimcontext->ibuscontext, "destroy",
                          G_CALLBACK (_ibus_context_destroy_cb),
                          ibusimcontext);
        */

        guint32 caps = IBUS_CAP_FOCUS;
        ibus_input_context_set_capabilities (pt->ibuscontext, caps);

        if (pt->has_focus) {
            /* The time order is _create_input_context() ->
             * ibus_im_context_notify() -> ibus_im_context_focus_in() ->
             * _create_input_context_done()
             * so _set_content_type() is called at the beginning here
             * because ibusimcontext->ibuscontext == NULL before. */
            // _set_content_type (ibusimcontext);

            ibus_input_context_focus_in (pt->ibuscontext);
            // _set_cursor_location_internal (pt);
        }
    }
}

static void create_input_context (PangoTerm *pt)
{
    gchar *prgname = g_strdup (g_get_prgname());
    gchar *client_name;
    g_assert (pt->ibuscontext == NULL);

    // g_return_if_fail (ibusimcontext->cancellable == NULL);

    // ibusimcontext->cancellable = g_cancellable_new ();

    if (!prgname) {
        prgname = g_strdup ("pangoterm");
    }

    client_name = g_strdup_printf ("%s-im", prgname);
    g_free (prgname);

    ibus_bus_create_input_context_async (_bus,
            client_name, -1,
            NULL, // ibusimcontext->cancellable,
            (GAsyncReadyCallback)_create_input_context_done,
            pt);
    g_free (client_name);
}
static void bus_connected_cb (IBusBus          *bus, PangoTerm    *pt)
{
    create_input_context (pt);
}

static void ibus_connect_try (PangoTerm *pt)
{
    if (_bus == NULL) {
        _bus = ibus_bus_new_async_client ();
    }

    g_signal_connect (_bus, "connected", G_CALLBACK (bus_connected_cb), pt);
}

typedef struct {
guint keyval;
                                guint keycode; GdkModifierType state; bool release;

} ProcessKeyEventData;

static void
_process_key_event_done (GObject      *object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
    IBusInputContext *context = (IBusInputContext *)object;

    ProcessKeyEventData *data = (ProcessKeyEventData *)user_data;
    GError *error = NULL;
    gboolean retval;

    retval = ibus_input_context_process_key_event_async_finish (context,
                                                                res,
                                                                &error);

    if (error != NULL) {
        g_warning ("Process Key Event failed: %s.", error->message);
        g_error_free (error);
    }

    fprintf(stderr, "TANGENTEN %d\n", retval);

    if (retval == FALSE) {
        // widget_real_handle_keypress(data);
    }
}

static gboolean ibus_filter_keypress(PangoTerm *pt,  guint keyval,
                                guint keycode, GdkModifierType state, bool release) {

  if (!pt->ibuscontext) {
    return false;
  }

  fprintf(stderr, "FILTRERA\n");
  if (release)
    state |= IBUS_RELEASE_MASK;

  ProcessKeyEventData *data = malloc(sizeof(*data));

  ibus_input_context_process_key_event_async (pt->ibuscontext,
      keyval,
      keycode - 8,
      state,
      -1,
      NULL,
      _process_key_event_done,
      data);

  return true;
}

// IBUS END }}}

/*
 * GTK widget event handlers
 */

static gboolean widget_keypress(GtkEventController *controller,  guint keyval,
                                guint keycode, GdkModifierType state, gpointer user_data)
{
  PangoTerm *pt = user_data;

  /* GtkIMContext will eat a Shift-Space and not tell us about shift.
   * Also don't let IME eat any GDK_KEY_KP_ events
   */
  gboolean ret = (state & GDK_SHIFT_MASK && keyval == ' ') ? FALSE
               : (keyval >= GDK_KEY_KP_Space && keyval <= GDK_KEY_KP_Divide) ? FALSE
               : ibus_filter_keypress(pt, keyval, keycode, state, false);

  if(ret)
    return TRUE;

  fprintf(stderr, "TANGENT %d (%d %d)\n", keyval, keycode, state);

  // We don't need to track the state of modifier bits
  /* if(event->is_modifier)
    return FALSE; */

  if((keyval == GDK_KEY_Insert && state & GDK_SHIFT_MASK) ||
     ((keyval == 'v' || keyval == 'V') &&
      state & GDK_CONTROL_MASK && state & GDK_SHIFT_MASK)) {
    /* Shift-Insert or Ctrl-Shift-V pastes clipboard */
    // TODO
    //gchar *str = gtk_clipboard_wait_for_text(keyval == GDK_KEY_Insert
    //                                         ? pt->selection_primary
    //                                         : pt->selection_clipboard);
    gchar *str = NULL;
    if(!str)
      return TRUE;

    lf_to_cr(str);

    term_push_string(pt, str, TRUE);
    return TRUE;
  }
  if((keyval == 'c' || keyval == 'C') &&
     state & GDK_CONTROL_MASK && state & GDK_SHIFT_MASK) {
    /* Ctrl-Shift-C copies to clipboard */
    if(!pt->highlight_valid)
      return TRUE;

    gchar *text = fetch_flow_text(pt, pt->highlight_start, pt->highlight_stop);
    if(!text)
      return TRUE;

    // TODO
    // gtk_clipboard_clear(pt->selection_clipboard);
    // gtk_clipboard_set_text(pt->selection_clipboard, text, -1);

    free(text);
    return TRUE;
  }
  if(keyval == GDK_KEY_Page_Down && state & GDK_SHIFT_MASK) {
    vscroll_delta(pt, -pt->rows / 2);
    return TRUE;
  }
  if(keyval == GDK_KEY_Page_Up && state & GDK_SHIFT_MASK) {
    vscroll_delta(pt, +pt->rows / 2);
    return TRUE;
  }

  VTermModifier mod = convert_modifier(state);
  VTermKey vterm_keyval = convert_keyval(keyval, &mod);

  /*
   * See also
   *   /usr/include/gtk-2.0/gdk/gdkkeysyms.h
   */

  if(vterm_keyval) {
    /* Shift-Enter and Shift-Backspace are too easy to mistype accidentally
     * Optionally remove shift if it's the only modifier
     */
    if(mod == VTERM_MOD_SHIFT)
      switch(vterm_keyval) {
        case VTERM_KEY_ENTER:
          if(!CONF_chord_shift_enter)
            mod = 0;
          break;

        case VTERM_KEY_BACKSPACE:
          if(!CONF_chord_shift_backspace)
            mod = 0;
          break;

        default:
          break;
      }

    vterm_keyboard_key(pt->vt, vterm_keyval, mod);
  }
  else if(keyval >= 0x10000000) /* Extension key, not printable Unicode */
    return FALSE;
  else if(keyval >= 0x01000000) /* Unicode shifted */
    vterm_keyboard_unichar(pt->vt, keyval - 0x01000000, mod);
  else if(keyval < 0x0f00) {
    /* GDK key code; convert to Unicode */
    guint32 unichar = gdk_keyval_to_unicode(keyval);
    if (unichar == 0)
      return FALSE;

    /* Shift-Space is too easy to mistype so optionally ignore that */
    if(mod == VTERM_MOD_SHIFT && keyval == ' ')
      if(!CONF_chord_shift_space)
        mod = 0;

    vterm_keyboard_unichar(pt->vt, unichar, mod);
  }
  else if(keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9)
    /* event->keyval is a keypad number; just treat it as Unicode */
    vterm_keyboard_unichar(pt->vt, keyval - GDK_KEY_KP_0 + '0', mod);
  else
    return FALSE;

  if(CONF_unscroll_on_key && pt->scroll_offs)
    vscroll_delta(pt, -pt->scroll_offs);

  flush_outbuffer(pt);

  return FALSE;
}

// static gboolean widget_keyrelease(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
// {
//   PangoTerm *pt = user_data;
//   return gtk_im_context_filter_keypress(pt->im_context, event);
// }

static gboolean widget_modifiers(GtkEventController *widget, GdkModifierType object, gpointer user_data)
{
  PangoTerm *pt = user_data;
  pt->modifiers = object;
  fprintf(stderr, "RYCK: %d\n", object);
  return FALSE;
}

static gboolean widget_mousepress(GtkGesture *gesture, gint n_press, gdouble x,
                                    gdouble y, gpointer user_data)
{
  PangoTerm *pt = user_data;

  GdkEvent *event = gtk_gesture_get_last_event(gesture, NULL);
  const GdkEventType type = gdk_event_get_event_type(event);
  GdkModifierType state = gdk_event_get_modifier_state(event);
  guint button = gdk_button_event_get_button(event);

  PhyPos ph_pos = {
    .pcol = (x - CONF_border) / pt->cell_width,
    .prow = (y - CONF_border) / pt->cell_height,
  };

  /* If the mouse is being dragged, we'll get motion events even outside our
   * window */
  int is_inside = (ph_pos.pcol >= 0 && ph_pos.pcol < pt->cols &&
                   ph_pos.prow >= 0 && ph_pos.prow < pt->rows);

  VTermPos pos = VTERMPOS_FROM_PHYSPOS(pt, ph_pos);

  /* Shift modifier bypasses terminal mouse handling */
  if(pt->mousemode && !(state & GDK_SHIFT_MASK) && is_inside) {
    VTermModifier vterm_state = convert_modifier(state);
    int is_press;
    switch(type) {
    case GDK_BUTTON_PRESS:
      is_press = 1;
      break;
    case GDK_BUTTON_RELEASE:
      is_press = 0;
      break;
    default:
      return TRUE;
    }
    vterm_mouse_move(pt->vt, pos.row, pos.col, vterm_state);
    vterm_mouse_button(pt->vt, button, is_press, vterm_state);
    flush_outbuffer(pt);
  }
  else if(button == 2 && type == GDK_BUTTON_PRESS && is_inside) {
    /* Middle-click pastes primary selection */
    // TODO
    // gchar *str = gtk_clipboard_wait_for_text(pt->selection_primary);
    gchar *str = NULL; // RYCK
    if(!str)
      return FALSE;

    lf_to_cr(str);

    term_push_string(pt, str, TRUE);
  }
  else if(button == 1 && type == GDK_BUTTON_PRESS && n_press == 1 && is_inside) {
    cancel_highlight(pt);

    pt->dragging = DRAG_PENDING;
    pt->drag_start = pos;
  }
  else if(button == 1 && type == GDK_BUTTON_RELEASE && pt->dragging != NO_DRAG) {
    /* Always accept a release even when outside */
    pt->dragging = NO_DRAG;

    if(pt->highlight_valid)
      store_clipboard(pt);
  }
  else if(button == 1 && type == GDK_BUTTON_PRESS && n_press == 2 && is_inside) {
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

    pt->highlight_valid = true;
    pt->highlight_start = start_pos;
    pt->highlight_stop  = stop_pos;

    repaint_flow(pt, pt->highlight_start, pt->highlight_stop);
    flush_pending(pt);
    blit_dirty(pt);
    store_clipboard(pt);
  }
  else if(button == 1 && type == GDK_BUTTON_PRESS && n_press == 3 && is_inside) {
    /* Highlight an entire line */
    pt->highlight_valid = true;
    pt->highlight_start.row = pos.row;
    pt->highlight_start.col = 0;
    pt->highlight_stop.row  = pos.row;
    pt->highlight_stop.col  = pt->cols - 1;

    repaint_flow(pt, pt->highlight_start, pt->highlight_stop);
    flush_pending(pt);
    blit_dirty(pt);
    store_clipboard(pt);
  }

  return TRUE;
}

static gboolean widget_mousemove(GtkEventController *controller, gdouble x, gdouble y, gpointer user_data)
{
  PangoTerm *pt = user_data;

  PhyPos ph_pos = {
    .pcol = (x - CONF_border) / pt->cell_width,
    .prow = (y - CONF_border) / pt->cell_height,
  };

  // GdkModifierType state = gtk_event_controller_get_current_event_state(controller);
  GdkModifierType state = pt->modifiers;

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
  if(pt->mousemode > VTERM_PROP_MOUSE_CLICK && !(state & GDK_SHIFT_MASK) && is_inside) {
    if(pos.row < 0 || pos.row >= pt->rows)
      return TRUE;
    VTermModifier vterm_state = convert_modifier(state);
    vterm_mouse_move(pt->vt, pos.row, pos.col, vterm_state);
    flush_outbuffer(pt);
  }
  else if(state & GDK_BUTTON1_MASK) {
    VTermPos old_pos = pt->dragging == DRAGGING ? pt->drag_pos : pt->drag_start;
    if(pos.row == old_pos.row && pos.col == old_pos.col)
      /* Unchanged; stop here */
      return FALSE;

    pt->dragging = DRAGGING;
    pt->drag_pos = pos;

    VTermPos pos_left1 = pt->drag_pos;
    if(pos_left1.col > 0) pos_left1.col--;

    pt->highlight_valid = true;

    VTermPos repaint_start = pt->highlight_start;
    VTermPos repaint_stop  = pt->highlight_stop;

    if(vterm_pos_cmp(pt->drag_start, pt->drag_pos) > 0) {
      pt->highlight_start = pt->drag_pos;
      pt->highlight_stop  = pt->drag_start;
    }
    else {
      pt->highlight_start = pt->drag_start;
      pt->highlight_stop  = pt->drag_pos;

      if(pt->highlight_stop.col > 0)
        pt->highlight_stop.col--; /* exclude partial cell */

      if(fetch_is_eol(pt, pt->highlight_stop))
        pt->highlight_stop.col = pt->cols - 1;
    }

    if(vterm_pos_cmp(pt->highlight_start, repaint_start) < 0)
      repaint_start = pt->highlight_start;
    if(vterm_pos_cmp(pt->highlight_stop, repaint_stop) > 0)
      repaint_stop  = pt->highlight_stop;

    repaint_flow(pt, repaint_start, repaint_stop);

    flush_pending(pt);
    blit_dirty(pt);
  }

  return FALSE;
}

#if 0
static gboolean widget_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  PhyPos ph_pos = {
    .pcol = (event->x - CONF_border) / pt->cell_width,
    .prow = (event->y - CONF_border) / pt->cell_height,
  };

  if(!(~event->state & (GDK_SHIFT_MASK|GDK_CONTROL_MASK))) {
    switch(event->direction) {
      case GDK_SCROLL_UP:
          pangoterm_set_fontsize(pt, pt->font_size+1);
          break;
      case GDK_SCROLL_DOWN:
          pangoterm_set_fontsize(pt, pt->font_size-1);
          break;
      default:
                             return FALSE;
    }
  } else if(pt->mousemode && !(event->state & GDK_SHIFT_MASK)) {
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

    vterm_mouse_move(pt->vt, pos.row, pos.col, state);
    vterm_mouse_button(pt->vt, button, 1, state);
    flush_outbuffer(pt);
  }
  else {
    switch(event->direction) {
      case GDK_SCROLL_UP:    vscroll_delta(pt, +CONF_scroll_wheel_delta); break;
      case GDK_SCROLL_DOWN:  vscroll_delta(pt, -CONF_scroll_wheel_delta); break;
      case GDK_SCROLL_RIGHT: hscroll_delta(pt, +1); break;
      case GDK_SCROLL_LEFT:  hscroll_delta(pt, -1); break;
      default:              return FALSE;
    }
  }

  return FALSE;
}
#endif

static gboolean widget_im_commit(GtkIMContext *context, gchar *str, gpointer user_data)
{
  PangoTerm *pt = user_data;

  printf("COMMIT %s\n", str);

  term_push_string(pt, str, FALSE);

  if(CONF_unscroll_on_key && pt->scroll_offs)
    vscroll_delta(pt, -pt->scroll_offs);

  return FALSE;
}

gboolean
vendored_gdk_cairo_get_clip_rectangle (cairo_t      *cr,
                              GdkRectangle *rect)
{
  double x1, y1, x2, y2;
  gboolean clip_exists;

  cairo_clip_extents (cr, &x1, &y1, &x2, &y2);

  clip_exists = x1 < x2 && y1 < y2;

  if (rect)
    {
      x1 = floor (x1);
      y1 = floor (y1);
      x2 = ceil (x2);
      y2 = ceil (y2);

      rect->x      = CLAMP (x1,      G_MININT, G_MAXINT);
      rect->y      = CLAMP (y1,      G_MININT, G_MAXINT);
      rect->width  = CLAMP (x2 - x1, G_MININT, G_MAXINT);
      rect->height = CLAMP (y2 - y1, G_MININT, G_MAXINT);
    }

  return clip_exists;
}

static void widget_draw(GtkDrawingArea *da, cairo_t *cr, int width, int height, gpointer user_data)
{
  PangoTerm *pt = user_data;

  /* GDK always sends resize events before expose events, so it's possible this
   * expose event is for a region that now doesn't exist.
   */
  int right  = 2 * CONF_border + pt->cols * pt->cell_width;
  int bottom = 2 * CONF_border + pt->rows * pt->cell_height;

  /* Trim to still-valid area, or ignore if there's nothing remaining */
  if(width > right)
    width = right;
  if(height > bottom)
    height = bottom;

  if(height && width)
    blit_buffer(pt, cr, width, height);

  return;
}

static void widget_resize(GtkDrawingArea *da, gint width, gint height, gpointer user_data)
{
  PangoTerm *pt = user_data;

  gint raw_width, raw_height;
  // TODO: gdk_surface_get_scale_factor() (except that we don't use it)
  raw_width = width;
  raw_height = height;
  // gtk_window_get_size(GTK_WINDOW(widget), &raw_width, &raw_height);

  raw_width  -= 2 * CONF_border;
  raw_height -= 2 * CONF_border;

  if (!pt->did_set_font_size) {
    int cols = raw_width  / pt->cell_width;
    int rows = raw_height / pt->cell_height;

    if(cols == pt->cols && rows == pt->rows)
      return;

    // Clamp to a minimum 1x1 size because libvterm doesn't like zero
    if(!cols)
      cols = 1;
    if(!rows)
      rows = 1;

    pt->cols = cols;
    pt->rows = rows;
  }

  if(pt->resizedfn)
    (*pt->resizedfn)(pt->rows, pt->cols, pt->resizedfn_data);

  cairo_surface_t* new_buffer = gdk_surface_create_similar_surface(pt->termdraw,
      CAIRO_CONTENT_COLOR,
      pt->cols * pt->cell_width,
      pt->rows * pt->cell_height);

  cairo_t* gc = cairo_create(new_buffer);
  cairo_set_source_surface(gc, pt->buffer, 0, 0);
  cairo_paint(gc);
  cairo_destroy(gc);

  cairo_surface_destroy(pt->buffer);
  pt->buffer = new_buffer;
  if (pt->did_set_font_size) {
    pt->did_set_font_size = false;

    VTermRect rect = {
      .start_col = 0,
      .end_col   = pt->cols,
      .start_row = 0,
      .end_row   = pt->rows,
    };

    repaint_rect(pt, rect);
  } else {
    vterm_set_size(pt->vt, pt->rows, pt->cols);
    vterm_screen_flush_damage(pt->vts);
  }
}

static void widget_focus_in(GtkWidget *widget, gpointer user_data)
{
  PangoTerm *pt = user_data;
  pt->has_focus = 1;
  fprintf(stderr, "GAIN\n");

  VTermState *state = vterm_obtain_state(pt->vt);
  vterm_state_focus_in(state);

  if(CURSOR_ENABLED(pt)) {
    repaint_cell(pt, pt->cursorpos);

    flush_pending(pt);
    blit_dirty(pt);
  }

  if (pt->ibuscontext) {
    ibus_input_context_focus_in (pt->ibuscontext);
  }
}

static void widget_focus_out(GtkWidget *widget, gpointer user_data)
{
  PangoTerm *pt = user_data;
  pt->has_focus = 0;
  fprintf(stderr, "LOSS\n");

  VTermState *state = vterm_obtain_state(pt->vt);
  vterm_state_focus_out(state);

  if(CURSOR_ENABLED(pt)) {
    repaint_cell(pt, pt->cursorpos);

    flush_pending(pt);
    blit_dirty(pt);
  }
  if (pt->ibuscontext) {
    ibus_input_context_focus_out (pt->ibuscontext);
  }

}

static void widget_quit(GtkWidget* widget, gpointer unused_data)
{
  exit(77);
}

static GdkPixbuf *load_icon(GdkRGBA *background)
{
  /* This technique stolen from
   *   http://git.gnome.org/browse/gtk+/tree/gtk/gtkicontheme.c#n3180
   *
   *   Updated because rsvg no longer supports loading file: URL scheme, only
   *   data:.
   */

  gchar *icon;
  gsize icon_len;
  if(!g_file_get_contents(PANGOTERM_SHAREDIR "/pixmaps/pangoterm.svg", &icon, &icon_len, NULL))
    return NULL;

  gchar *icon_base64 = g_base64_encode((guchar*)icon, icon_len);
  g_free(icon);

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
      "  <xi:include href=\"data:image/svg+xml;base64,%s" "\"/>\n"
      "</svg>",
      (guint)(background->red   / 255),
      (guint)(background->green / 255),
      (guint)(background->blue  / 255),
      icon_base64);
  g_free(icon_base64);

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

  pt->cursor_col = (GdkRGBA){ 1.0, 1.0, 1.0, 1.0 };
  gdk_rgba_parse(&pt->cursor_col, CONF_cursor);

  /* Create VTerm */
  pt->vt = vterm_new(rows, cols);
  vterm_set_utf8(pt->vt, 1);

  /* Set up state */
  VTermState *state = vterm_obtain_state(pt->vt);
  vterm_state_set_bold_highbright(state, CONF_bold_highbright);

  for(int index = 0; index < sizeof(colours)/sizeof(colours[0]); index++) {
    if(!colours[index].is_set)
      continue;

    vterm_state_set_palette_color(state, index,
        &VTERM_COLOR_FROM_GDK_COLOR(colours[index].col));
  }

  /* Set up screen */
  pt->vts = vterm_obtain_screen(pt->vt);
  vterm_screen_enable_altscreen(pt->vts, CONF_altscreen);
  vterm_screen_set_callbacks(pt->vts, &cb, pt);
  vterm_screen_set_damage_merge(pt->vts, VTERM_DAMAGE_SCROLL);

  /* Set up GTK widget */

  pt->termwin = gtk_window_new();
  // Abe would ask: HOW
  // gtk_widget_set_double_buffered(pt->termwin, FALSE);
  // HOW
  // gtk_widget_(pt->termwin, GTK_STATE_NORMAL, &pt->bg_col);

  pt->glyphs = g_string_sized_new(128);
  pt->glyph_widths = g_array_new(FALSE, FALSE, sizeof(int));

  pt->termda = gtk_drawing_area_new();
  gtk_window_set_child (GTK_WINDOW (pt->termwin), pt->termda);

  gtk_widget_realize(pt->termwin);

  pt->termdraw = gtk_native_get_surface(GTK_NATIVE(pt->termwin));
  pt->cairo_context = gdk_surface_create_cairo_context(pt->termdraw);

  // HOW, Abe would need to know
  // gdk_window_set_cursor(pt->termdraw, gdk_cursor_new(GDK_XTERM));

  cursor_start_blinking(pt);
  pt->cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;

  //GdkEventMask mask = gdk_window_get_events(pt->termdraw);
  //gdk_window_set_events(pt->termdraw, mask|GDK_BUTTON_PRESS_MASK|GDK_BUTTON_RELEASE_MASK|GDK_POINTER_MOTION_MASK|GDK_SCROLL_MASK);

  
  GtkEventController *key_ev = gtk_event_controller_key_new();
  gtk_widget_add_controller(pt->termda, key_ev);
  GtkGesture *button_ev = gtk_gesture_click_new();
  gtk_widget_add_controller(pt->termda, GTK_EVENT_CONTROLLER(button_ev));
  GtkEventController *motion_ev = gtk_event_controller_motion_new();
  gtk_widget_add_controller(pt->termda, motion_ev);
  GtkEventController *focus_ev = gtk_event_controller_focus_new();
  gtk_widget_add_controller(pt->termda, focus_ev);
  // GtkEventController *scroll_ev = gtk_event_controller_scroll_new();

  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pt->termda), widget_draw, pt, NULL);

  g_signal_connect(G_OBJECT(key_ev), "key-pressed", G_CALLBACK(widget_keypress), pt);
  g_signal_connect(G_OBJECT(key_ev), "modifiers", G_CALLBACK(widget_modifiers), pt);
  g_signal_connect(G_OBJECT(button_ev), "pressed",   G_CALLBACK(widget_mousepress), pt);
  g_signal_connect(G_OBJECT(button_ev), "released", G_CALLBACK(widget_mousepress), pt);
  g_signal_connect(G_OBJECT(motion_ev), "motion",  G_CALLBACK(widget_mousemove), pt);
  // g_signal_connect(G_OBJECT(pt->termwin), "scroll-event",  G_CALLBACK(widget_scroll), pt);
  g_signal_connect(G_OBJECT(focus_ev), "enter",  G_CALLBACK(widget_focus_in),  pt);
  g_signal_connect(G_OBJECT(focus_ev), "leave", G_CALLBACK(widget_focus_out), pt);
  // TODO: should not be needed
  // g_signal_connect(G_OBJECT(pt->termwin), "destroy", G_CALLBACK(widget_quit), pt);

  gtk_widget_set_focusable(pt->termda, true);

  // NOTE: The IbusIMContext implementation for GTK4 is serverly broken. This is
  // fundamentally a design issue as the GTK4/Wayland-first model does not fit
  // with the X11/XIM flavoured IBus model. At all. This will all be fixed by
  // adopting the Wayland IME protocol through the entire stack, but we are not
  // there yet. Talk to IBus directly instead so we can fake the 
  // pt->im_context = gtk_im_multicontext_new();
  // gtk_im_context_set_client_widget(pt->im_context, pt->termwin);
  // gtk_im_context_set_use_preedit(pt->im_context, false);

  // this is somehow not needed, and NOT inculding it implements shift-space properly???
  // gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(key_ev), pt->im_context);

  // g_signal_connect(G_OBJECT(pt->im_context), "commit", G_CALLBACK(widget_im_commit), pt);

  ibus_connect_try(pt); // async

  g_signal_connect(G_OBJECT(pt->termda), "resize", G_CALLBACK(widget_resize), pt);

  pt->dragging = NO_DRAG;

  // TODO: GRUGG
  // pt->selection_primary   = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
  // pt->selection_clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

  pt->scroll_size = CONF_scrollback_size;
  pt->sb_buffer = g_new0(PangoTermScrollbackLine*, pt->scroll_size);

  pt->outbuffer = g_string_sized_new(256);
  pt->tmpbuffer = g_string_sized_new(256);

  vterm_output_set_callback(pt->vt, term_output, pt);

  return pt;
}

void pangoterm_free(PangoTerm *pt)
{
  g_strfreev(pt->fonts);

  vterm_free(pt->vt);
}

guint32 pangoterm_get_windowid(PangoTerm *pt)
{
  /* TODO: return 0 when not on X11 */
  // GdkWindow *win = gtk_widget_get_window(pt->termwin);
  return 0;  // hahhahaha
}

void pangoterm_set_default_colors(PangoTerm *pt, GdkRGBA *fg_col, GdkRGBA *bg_col)
{
  pt->fg_col = *fg_col;
  pt->bg_col = *bg_col;

  vterm_state_set_default_colors(vterm_obtain_state(pt->vt),
      &VTERM_COLOR_FROM_GDK_COLOR(*fg_col),
      &VTERM_COLOR_FROM_GDK_COLOR(*bg_col));

  /* TODO: Do the equivalent using raw Xlib calls when using X backend,
   * as basically all these calls are deprecated. */
  /*
  GdkColormap* colormap = gdk_colormap_get_system();
  gdk_rgb_find_color(colormap, bg_col);
  gdk_window_set_background(pt->termdraw, bg_col);
  */

  GdkPixbuf *icon = load_icon(bg_col);
  if(icon) {
    // TODO: Abe would wonder, HOW
    // gtk_window_set_icon(GTK_WINDOW(pt->termwin), icon);
    g_object_unref(icon);
  }
}

void pangoterm_set_fonts(PangoTerm *pt, char *font, char *font_italic, char **alt_fonts)
{
  int n_fonts = 1;
  while(alt_fonts[n_fonts-1])
    n_fonts++;

  g_strfreev(pt->fonts);

  pt->n_fonts = n_fonts;

  if (font_italic && strlen(font_italic) != 0) {
      pt->font_italic = g_strdup(font_italic);
  } else {
      pt->font_italic = NULL;
  }


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

void pangoterm_init_font(PangoTerm *pt) {
  // cairo_t *cctx = gdk_cairo_create(pt->termdraw);
  cairo_t *cctx = cairo_create(pt->buffer);
  PangoContext *pctx = pango_cairo_create_context(cctx);

  PangoFontDescription *fontdesc = pango_font_description_from_string(pt->fonts[0]);
  if(pango_font_description_get_size(fontdesc) == 0)
    pango_font_description_set_size(fontdesc, pt->font_size * PANGO_SCALE);

  pango_context_set_font_description(pctx, fontdesc);

  // pango_cairo_context_set_resolution(pctx, gdk_screen_get_resolution(gdk_screen_get_default()));
  // TODO: så jävla BULL
  pango_cairo_context_set_resolution(pctx, 100);

  pt->pen.pangoattrs = pango_attr_list_new();
  pt->pen.layout = pango_layout_new(pctx);
  pango_layout_set_font_description(pt->pen.layout, fontdesc);

  PangoFontMetrics *metrics = pango_context_get_metrics(pctx,
      pango_context_get_font_description(pctx), pango_context_get_language(pctx));

  int width = (pango_font_metrics_get_approximate_char_width(metrics) +
               pango_font_metrics_get_approximate_digit_width(metrics)) / 2;

  int height = pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent(metrics);

  pt->cell_width  = PANGO_PIXELS_CEIL(width);
  pt->cell_width_pango = PANGO_SCALE*pt->cell_width;
  pt->cell_height = PANGO_PIXELS_CEIL(height);
}

void pangoterm_set_fontsize(PangoTerm* pt, double font_size) {
  pt->font_size = font_size;
  pangoterm_init_font(pt);
  pt->did_set_font_size = true;
  gtk_window_set_default_size(GTK_WINDOW(pt->termwin),
      pt->cols * pt->cell_width, pt->rows * pt->cell_height);
}

void pangoterm_start(PangoTerm *pt)
{
  /* Finish the rest of the setup and start */


  pangoterm_init_font(pt);

  GdkRGBA fg_col = { 0.90, 0.90, 0.90, 1.0 };
  gdk_rgba_parse(&fg_col, CONF_foreground);

  GdkRGBA bg_col = { 0, 0, 0, 1.0 };
  gdk_rgba_parse(&bg_col, CONF_background);

  pangoterm_set_default_colors(pt, &fg_col, &bg_col);

  gtk_window_set_default_size(GTK_WINDOW(pt->termwin),
      pt->cols * pt->cell_width  + 2 * CONF_border,
      pt->rows * pt->cell_height + 2 * CONF_border);

  pt->buffer = gdk_surface_create_similar_surface(pt->termdraw,
      CAIRO_CONTENT_COLOR,
      pt->cols * pt->cell_width,
      pt->rows * pt->cell_height);

  // GdkGeometry hints;

  // hints.min_width  = pt->cell_width  + 2 * CONF_border;
  // hints.min_height = pt->cell_height + 2 * CONF_border;
  // hints.width_inc  = pt->cell_width;
  // hints.height_inc = pt->cell_height;

  gtk_window_set_resizable(GTK_WINDOW(pt->termwin), TRUE);
  // TODO: bull
  // gtk_window_set_geometry_hints(GTK_WINDOW(pt->termwin), GTK_WIDGET(pt->termwin), &hints, GDK_HINT_RESIZE_INC | GDK_HINT_MIN_SIZE);

  vterm_screen_reset(pt->vts, 1);

  VTermState *state = vterm_obtain_state(pt->vt);
  vterm_state_set_termprop(state, VTERM_PROP_CURSORSHAPE, &(VTermValue){ .number = CONF_cursor_shape });

  // if(CONF_geometry && CONF_geometry[0])
  //   gtk_window_parse_geometry(GTK_WINDOW(pt->termwin), CONF_geometry);

  gtk_widget_show(pt->termwin);
}

void pangoterm_push_bytes(PangoTerm *pt, const char *bytes, size_t len)
{
  if(CONF_unscroll_on_output && pt->scroll_offs)
    vscroll_delta(pt, -pt->scroll_offs);

  vterm_input_write(pt->vt, bytes, len);
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
  flush_outbuffer(pt);
}
