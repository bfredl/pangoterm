#include "pangoterm.h"

#include <wctype.h>

#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#ifdef DEBUG
# define DEBUG_PRINT_INPUT
#endif

struct PangoTerm {
  VTerm *vt;
  VTermScreen *vts;

  GtkIMContext *im_context;

  VTermMouseFunc mousefunc;
  void *mousedata;

  GString *glyphs;
  GArray *glyph_widths;
  GdkRectangle glyph_area;

  struct {
    struct {
      unsigned int bold      : 1;
      unsigned int underline : 2;
      unsigned int italic    : 1;
      unsigned int reverse   : 1;
      unsigned int strike    : 1;
      unsigned int font      : 4;
    } attrs;
    GdkColor fg_col;
    GdkColor bg_col;
    PangoAttrList *pangoattrs;
    PangoLayout *layout;
  } pen;

  int rows;
  int cols;

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

  int has_focus;
  int cursor_blink_interval;
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

  /* These four positions relate to the click/drag highlight state
   * row == -1 for invalid */

  /* Initial mouse position of selection drag */
  VTermPos drag_start;
  /* Current mouse position of selection drag */
  VTermPos drag_pos;
  /* Start and stop bounds of the selection */
  VTermPos highlight_start;
  VTermPos highlight_stop;

  GtkClipboard *primary_clipboard;
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
      thislen += vterm_screen_get_text(pt->vts,
          str ? str    + thislen : NULL,
          str ? strlen - thislen : 0,
          rect);
    }
    else {
      rect.start_row = start.row;
      rect.start_col = start.col;
      rect.end_row   = start.row + 1;
      rect.end_col   = pt->cols;
      thislen += vterm_screen_get_text(pt->vts,
          str ? str    + thislen : NULL,
          str ? strlen - thislen : 0,
          rect);

      thislen += 1;
      if(str)
        str[thislen - 1] = 0x0a;

      for(int row = start.row + 1; row < stop.row; row++) {
        rect.start_row = row;
        rect.start_col = 0;
        rect.end_row   = row + 1;
        rect.end_col   = pt->cols;

        thislen += vterm_screen_get_text(pt->vts,
            str ? str    + thislen : NULL,
            str ? strlen - thislen : 0,
            rect);

        thislen += 1;
        if(str)
          str[thislen - 1] = 0x0a;
      }

      rect.start_row = stop.row;
      rect.start_col = 0;
      rect.end_row   = stop.row + 1;
      rect.end_col   = stop.col + 1;
      thislen += vterm_screen_get_text(pt->vts,
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

#define GDKRECTANGLE_FROM_VTERMRECT(pt, rect)                    \
  {                                                              \
    .x      = rect.start_col * pt->cell_width,                   \
    .y      = rect.start_row * pt->cell_height,                  \
    .width  = (rect.end_col - rect.start_col) * pt->cell_width,  \
    .height = (rect.end_row - rect.start_row) * pt->cell_height, \
  }

#define GDKRECTANGLE_FROM_VTERM_CELLS(pt, pos, width_mult) \
  {                                                        \
    .x      = pos.col * pt->cell_width,                    \
    .y      = pos.row * pt->cell_height,                   \
    .width  = pt->cell_width * width_mult,                 \
    .height = pt->cell_height,                             \
  }

static int is_wordchar(uint32_t c)
{
  return iswalnum(c) || (c == '_');
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
  cairo_destroy(gc);
}

static void flush_glyphs(PangoTerm *pt)
{
  if(!pt->glyphs->len) {
    pt->glyph_area.width = 0;
    pt->glyph_area.height = 0;
    return;
  }
  cairo_t* gc = cairo_create(pt->buffer);
  gdk_cairo_rectangle(gc, &pt->glyph_area);
  cairo_clip(gc);

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

  /* Background fill */
  GdkColor bg = pt->pen.attrs.reverse ? pt->pen.fg_col : pt->pen.bg_col;
  gdk_cairo_set_source_color(gc, &bg);
  cairo_paint(gc);

  /* Draw glyphs */
  GdkColor fg = pt->pen.attrs.reverse ? pt->pen.bg_col : pt->pen.fg_col;
  gdk_cairo_set_source_color(gc, &fg);
  cairo_move_to(gc, pt->glyph_area.x, pt->glyph_area.y);
  pango_cairo_show_layout(gc, layout);
  /* Flush our changes */
  blit_buffer(pt, &pt->glyph_area);

  pt->glyph_area.width = 0;
  pt->glyph_area.height = 0;

  g_string_truncate(pt->glyphs, 0);

  cairo_destroy(gc);
}

static void put_glyph(PangoTerm *pt, const uint32_t chars[], int width, VTermPos pos)
{
  GdkRectangle destarea = GDKRECTANGLE_FROM_VTERM_CELLS(pt, pos, width);

  if(destarea.y != pt->glyph_area.y || destarea.x != pt->glyph_area.x + pt->glyph_area.width)
    flush_glyphs(pt);

  char *chars_str = g_ucs4_to_utf8(chars, -1, NULL, NULL, NULL);

  g_array_set_size(pt->glyph_widths, pt->glyphs->len + 1);
  g_array_index(pt->glyph_widths, int, pt->glyphs->len) = width;

  g_string_append(pt->glyphs, chars_str);

  g_free(chars_str);

  if(pt->glyph_area.width && pt->glyph_area.height)
    gdk_rectangle_union(&destarea, &pt->glyph_area, &pt->glyph_area);
  else
    pt->glyph_area = destarea;
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
    flush_glyphs(pt);
    ADDATTR(pango_attr_weight_new(bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL));
  }

  if(cell->attrs.underline != pt->pen.attrs.underline) {
    int underline = pt->pen.attrs.underline = cell->attrs.underline;
    flush_glyphs(pt);
    ADDATTR(pango_attr_underline_new(underline == 1 ? PANGO_UNDERLINE_SINGLE :
                                     underline == 2 ? PANGO_UNDERLINE_DOUBLE :
                                                      PANGO_UNDERLINE_NONE));
  }

  if(cell->attrs.italic != pt->pen.attrs.italic) {
    int italic = pt->pen.attrs.italic = cell->attrs.italic;
    flush_glyphs(pt);
    ADDATTR(pango_attr_style_new(italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL));
  }

  if(cell->attrs.reverse != pt->pen.attrs.reverse) {
    flush_glyphs(pt);
    pt->pen.attrs.reverse = cell->attrs.reverse;
  }

  if(cell->attrs.strike != pt->pen.attrs.strike) {
    int strike = pt->pen.attrs.strike = cell->attrs.strike;
    flush_glyphs(pt);
    ADDATTR(pango_attr_strikethrough_new(strike));
  }

  if(cell->attrs.font != pt->pen.attrs.font) {
    int font = pt->pen.attrs.font = cell->attrs.font;
    if(font >= pt->n_fonts)
      font = 0;
    flush_glyphs(pt);
    ADDATTR(pango_attr_family_new(pt->fonts[font]));
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
    flush_glyphs(pt);
    pt->pen.fg_col = col;
  }

  col.red   = 257 * cell->bg.red;
  col.green = 257 * cell->bg.green;
  col.blue  = 257 * cell->bg.blue;

  if(cursoroverride)
    col = pt->cursor_col;

  if(col.red   != pt->pen.bg_col.red || col.green != pt->pen.bg_col.green || col.blue  != pt->pen.bg_col.blue) {
    flush_glyphs(pt);
    pt->pen.bg_col = col;
  }
}

static void erase_rect(PangoTerm *pt, VTermRect rect)
{
  flush_glyphs(pt);

  cairo_t *gc = cairo_create(pt->buffer);

  GdkRectangle destarea = GDKRECTANGLE_FROM_VTERMRECT(pt, rect);
  gdk_cairo_rectangle(gc, &destarea);
  cairo_clip(gc);

  GdkColor bg = pt->pen.attrs.reverse ? pt->pen.fg_col : pt->pen.bg_col;
  gdk_cairo_set_source_color(gc, &bg);
  cairo_paint(gc);
  cairo_destroy(gc);

  blit_buffer(pt, &destarea);
}

static void repaint_rect(PangoTerm *pt, VTermRect rect)
{
  for(int row = rect.start_row; row < rect.end_row; row++) {
    for(int col = rect.start_col; col < rect.end_col; ) {
      VTermPos pos = {
        .row = row,
        .col = col,
      };

      VTermScreenCell cell;
      vterm_screen_get_cell(pt->vts, pos, &cell);

      /* Invert the RV attribute if this cell is selected */
      if(pt->highlight_start.row != -1 && pt->highlight_stop.row != -1) {
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
        VTermRect here = {
          .start_row = row,
          .end_row   = row + 1,
          .start_col = col,
          .end_col   = col + 1,
        };
        erase_rect(pt, here);
      }
      else {
        put_glyph(pt, cell.chars, cell.width, pos);
      }

      if(cursor_visible && cursor_here && pt->cursor_shape != VTERM_PROP_CURSORSHAPE_BLOCK) {
        flush_glyphs(pt);

        cairo_t *gc = gdk_cairo_create(pt->termdraw);

        GdkRectangle destarea = GDKRECTANGLE_FROM_VTERM_CELLS(pt, pos, 1);
        gdk_cairo_rectangle(gc, &destarea);
        cairo_clip(gc);

        switch(pt->cursor_shape) {
        case VTERM_PROP_CURSORSHAPE_UNDERLINE:
          gdk_cairo_set_source_color(gc, &pt->cursor_col);
          cairo_rectangle(gc,
              destarea.x,
              destarea.y + (int)(destarea.height * 0.85),
              destarea.width,
              (int)(destarea.height * 0.15));
          cairo_fill(gc);
          break;
        }

        blit_buffer(pt, &destarea);

        cairo_destroy(gc);
      }

      col += cell.width;
    }
  }
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

    flush_glyphs(pt);
  }

  return TRUE;
}

static void cursor_start_blinking(PangoTerm *pt)
{
  pt->cursor_timer_id = g_timeout_add(pt->cursor_blink_interval, cursor_blink, pt);

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

  gtk_clipboard_clear(pt->primary_clipboard);
  gtk_clipboard_set_text(pt->primary_clipboard, text, -1);

  free(text);
}

static void cancel_highlight(PangoTerm *pt)
{
  VTermPos old_start = pt->highlight_start,
           old_stop  = pt->highlight_stop;

  if(old_start.row == -1 && old_stop.row == -1)
    return;

  pt->highlight_start.row = -1;
  pt->highlight_stop.row  = -1;

  repaint_flow(pt, old_start, old_stop);
  flush_glyphs(pt);
}

/*
 * VTerm event handlers
 */

static int term_damage(VTermRect rect, void *user_data)
{
  PangoTerm *pt = user_data;

  if(pt->highlight_start.row != -1 && pt->highlight_stop.row != -1) {
    if((pt->highlight_start.row < rect.end_row-1 ||
        (pt->highlight_start.row == rect.end_row-1 && pt->highlight_start.col < rect.end_col-1)) &&
       (pt->highlight_stop.row > rect.start_row ||
        (pt->highlight_stop.row == rect.start_row && pt->highlight_stop.col > rect.start_col))) {
      /* Damage overlaps highlighted region */
      cancel_highlight(pt);
    }
  }

  repaint_rect(pt, rect);

  return 1;
}

static int term_moverect(VTermRect dest, VTermRect src, void *user_data)
{
  PangoTerm *pt = user_data;

  flush_glyphs(pt);

  if(pt->highlight_start.row != -1 && pt->highlight_stop.row != -1) {
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

  GdkRectangle destarea = GDKRECTANGLE_FROM_VTERMRECT(pt, dest);

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
    /* recognised but don't need to do anything here */
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
};

/*
 * GTK widget event handlers
 */

static gboolean widget_keypress(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  PangoTerm *pt = user_data;
  /* GtkIMContext will eat a Shift-Space and not tell us about shift.
   */
  gboolean ret = (event->state & GDK_SHIFT_MASK && event->keyval == ' ') ? FALSE
      : gtk_im_context_filter_keypress(pt->im_context, event);

  if(ret)
    return TRUE;

  // We don't need to track the state of modifier bits
  if(event->is_modifier)
    return FALSE;

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
  else
    return FALSE;

  term_flush_output(pt);

  return FALSE;
}

static gboolean widget_mousepress(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  int col = event->x / pt->cell_width;
  int row = event->y / pt->cell_height;

  /* If the mouse is being dragged, we'll get motion events even outside our
   * window */
  int is_inside = (col >= 0 && col < pt->cols &&
                   row >= 0 && row < pt->rows);

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
    (*pt->mousefunc)(col, row, event->button, is_press, state, pt->mousedata);
    term_flush_output(pt);
  }
  else if(event->button == 2 && event->type == GDK_BUTTON_PRESS && is_inside) {
    /* Middle-click paste */
    gchar *str = gtk_clipboard_wait_for_text(pt->primary_clipboard);

    term_push_string(pt, str);
  }
  else if(event->button == 1 && event->type == GDK_BUTTON_PRESS && is_inside) {
    cancel_highlight(pt);

    pt->drag_start.row = row;
    pt->drag_start.col = col;
    pt->drag_pos.row = -1;
  }
  else if(event->button == 1 && event->type == GDK_BUTTON_RELEASE && pt->drag_pos.row != -1) {
    /* Always accept a release even when outside */
    pt->drag_start.row = -1;
    pt->drag_pos.row   = -1;

    store_clipboard(pt);
  }
  else if(event->button == 1 && event->type == GDK_2BUTTON_PRESS && is_inside) {
    /* Highlight a word. start with the position, and extend it both sides
     * over word characters
     */
    int start_col = col;
    while(start_col > 0) {
      VTermPos pos = { .row = row, .col = start_col - 1 };
      VTermScreenCell cell;

      vterm_screen_get_cell(pt->vts, pos, &cell);
      if(!is_wordchar(cell.chars[0]))
        break;

      start_col--;
    }

    int stop_col = col;
    while(stop_col < pt->cols) {
      VTermPos pos = { .row = row, .col = stop_col + 1 };
      VTermScreenCell cell;

      vterm_screen_get_cell(pt->vts, pos, &cell);
      if(!is_wordchar(cell.chars[0]))
        break;

      stop_col++;
    }

    pt->highlight_start.row = row;
    pt->highlight_start.col = start_col;
    pt->highlight_stop.row  = row;
    pt->highlight_stop.col  = stop_col;

    repaint_flow(pt, pt->highlight_start, pt->highlight_stop);
    flush_glyphs(pt);
    store_clipboard(pt);
  }
  else if(event->button == 1 && event->type == GDK_3BUTTON_PRESS && is_inside) {
    /* Highlight an entire line */
    pt->highlight_start.row = row;
    pt->highlight_start.col = 0;
    pt->highlight_stop.row  = row;
    pt->highlight_stop.col  = pt->cols - 1;

    repaint_flow(pt, pt->highlight_start, pt->highlight_stop);
    flush_glyphs(pt);
    store_clipboard(pt);
  }

  return FALSE;
}

static gboolean widget_mousemove(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  int col = event->x / pt->cell_width;
  int row = event->y / pt->cell_height;

  /* If the mouse is being dragged, we'll get motion events even outside our
   * window */
  int is_inside = (col >= 0 && col < pt->cols &&
                   row >= 0 && row < pt->rows);

  /* Shift modifier bypasses terminal mouse handling */
  if(pt->mousefunc && !(event->state & GDK_SHIFT_MASK) && is_inside) {
    VTermModifier state = convert_modifier(event->state);
    (*pt->mousefunc)(col, row, 0, 0, state, pt->mousedata);
    term_flush_output(pt);
  }
  else if(event->state & GDK_BUTTON1_MASK) {
    VTermPos old_end = pt->drag_pos;
    if(old_end.row == -1)
      old_end = pt->drag_start;

    if(row == old_end.row && col == old_end.col)
      /* Unchanged; stop here */
      return FALSE;

    if(col < 0)         col = 0;
    if(col > pt->cols)  col = pt->cols; /* allow off-by-1 */
    if(row < 0)         row = 0;
    if(row >= pt->rows) row = pt->rows - 1;

    pt->drag_pos.row = row;
    pt->drag_pos.col = col;

    VTermPos pos_left1 = pt->drag_pos;
    if(pos_left1.col > 0) pos_left1.col--;

    if(vterm_screen_is_eol(pt->vts, pos_left1))
      pt->drag_pos.col = pt->cols;

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

    flush_glyphs(pt);
  }

  return FALSE;
}

static gboolean widget_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  int col = event->x / pt->cell_width;
  int row = event->y / pt->cell_height;

  /* Translate scroll direction back into a button number */
  int button;
  switch(event->direction) {
    case GDK_SCROLL_UP:    button = 4; break;
    case GDK_SCROLL_DOWN:  button = 5; break;
    default:
      return FALSE;
  }

  VTermModifier state = convert_modifier(event->state);

  if(pt->mousefunc && !(event->state & GDK_SHIFT_MASK)) {
    (*pt->mousefunc)(col, row, button, 1, state, pt->mousedata);
    term_flush_output(pt);
  }

  return FALSE;
}

static gboolean widget_im_commit(GtkIMContext *context, gchar *str, gpointer user_data)
{
  PangoTerm *pt = user_data;

  term_push_string(pt, str);

  return FALSE;
}

static gboolean widget_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

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

    flush_glyphs(pt);
  }
}

static void widget_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  PangoTerm *pt = user_data;
  pt->has_focus = 0;

  if(CURSOR_ENABLED(pt)) {
    repaint_cell(pt, pt->cursorpos);

    flush_glyphs(pt);
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
      "  <xi:include href=\"%s/pangoterm.svg" "\"/>\n"
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
  pt->font_size = 9.0;

  pt->cursor_blink_interval = 500;

  /* Create VTerm */
  pt->vt = vterm_new(rows, cols);

  /* Set up parser */
  vterm_parser_set_utf8(pt->vt, 1);

  /* Set up state */
  vterm_state_set_bold_highbright(vterm_obtain_state(pt->vt), 1);

  /* Set up screen */
  pt->vts = vterm_obtain_screen(pt->vt);
  vterm_screen_enable_altscreen(pt->vts, 1);
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

  pt->cursor_timer_id = g_timeout_add(pt->cursor_blink_interval, cursor_blink, pt);
  pt->cursor_blinkstate = 1;
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

  pt->drag_start.row      = -1;
  pt->drag_pos.row        = -1;
  pt->highlight_start.row = -1;
  pt->highlight_stop.row  = -1;

  pt->primary_clipboard = gtk_clipboard_get(GDK_SELECTION_PRIMARY);

  return pt;
}

void pangoterm_free(PangoTerm *pt)
{
  g_strfreev(pt->fonts);

  vterm_free(pt->vt);
}

void pangoterm_set_default_colors(PangoTerm *pt, GdkColor *fg_col, GdkColor *bg_col)
{
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

void pangoterm_set_cursor_color(PangoTerm *pt, GdkColor *cursor_col)
{
  pt->cursor_col = *cursor_col;
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

  gtk_widget_show_all(pt->termwin);
}

void pangoterm_push_bytes(PangoTerm *pt, const char *bytes, size_t len)
{
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

  flush_glyphs(pt);
  term_flush_output(pt);
}
