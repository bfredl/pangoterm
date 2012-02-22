/* for putenv() */
#define _XOPEN_SOURCE

/* for ECHOCTL and ECHOKE */
#define _BSD_SOURCE

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

/* suck up the non-standard openpty/forkpty */
#if defined(__FreeBSD__)
# include <libutil.h>
# include <termios.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
# include <termios.h>
# include <util.h>
#else
# include <pty.h>
#endif

#include "vterm.h"

#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#ifdef DEBUG
# define DEBUG_PRINT_INPUT
#endif


typedef struct {
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

  int master;

  int rows;
  int cols;

  int cell_width_pango;
  int cell_width;
  int cell_height;

  int has_focus;
  int cursor_visible;
  int cursor_blinkstate;
  VTermPos cursorpos;
  GdkColor cursor_col;
  int cursor_shape;

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
} PangoTerm;

static char *default_fg = "gray90";
static char *default_bg = "black";

static char *cursor_col_str = "white";
static gint cursor_blink_interval = 500;

static char *default_font = "DejaVu Sans Mono";
static int default_size = 9;

static char *default_title = "pangoterm";

static int lines = 25;
static int cols  = 80;

static char *alt_fonts[] = {
  "Courier 10 Pitch",
};

static GOptionEntry option_entries[] = {
  /* long_name, short_name, flags, arg, arg_data, description, arg_description */
  { "foreground", 0,   0, G_OPTION_ARG_STRING, &default_fg, "Default foreground colour", "COL" },
  { "background", 0,   0, G_OPTION_ARG_STRING, &default_bg, "Default background colour", "COL" },
  { "cursor",     0,   0, G_OPTION_ARG_STRING, &cursor_col_str, "Cursor colour", "COL" },

  { "font",       0,   0, G_OPTION_ARG_STRING, &default_font, "Font name", "FONT" },
  { "size",       's', 0, G_OPTION_ARG_INT,    &default_size, "Font size", "INT" },

  { "title",      0,   0, G_OPTION_ARG_STRING, &default_title, "Title", "STR" },

  { "lines",      0,   0, G_OPTION_ARG_INT,    &lines, "Number of lines", "LINES" },
  { "cols",       0,   0, G_OPTION_ARG_INT,    &cols,  "Number of columns", "COLS" },

  { NULL },
};

/*
 * Utility functions
 */

static VTermKey convert_keyval(guint gdk_keyval, VTermModifier *statep)
{
  if(gdk_keyval >= GDK_F1 && gdk_keyval <= GDK_F35)
    return VTERM_KEY_FUNCTION(gdk_keyval - GDK_F1 + 1);

  switch(gdk_keyval) {
  case GDK_BackSpace:
    return VTERM_KEY_BACKSPACE;
  case GDK_Tab:
    return VTERM_KEY_TAB;
  case GDK_Return:
    return VTERM_KEY_ENTER;
  case GDK_Escape:
    return VTERM_KEY_ESCAPE;

  case GDK_Up:
    return VTERM_KEY_UP;
  case GDK_Down:
    return VTERM_KEY_DOWN;
  case GDK_Left:
    return VTERM_KEY_LEFT;
  case GDK_Right:
    return VTERM_KEY_RIGHT;

  case GDK_Insert:
    return VTERM_KEY_INS;
  case GDK_Delete:
    return VTERM_KEY_DEL;
  case GDK_Home:
    return VTERM_KEY_HOME;
  case GDK_End:
    return VTERM_KEY_END;
  case GDK_Page_Up:
    return VTERM_KEY_PAGEUP;
  case GDK_Page_Down:
    return VTERM_KEY_PAGEDOWN;

  case GDK_KEY_ISO_Left_Tab:
    /* This is Shift-Tab */
    *statep |= VTERM_MOD_SHIFT;
    return VTERM_KEY_TAB;

  default:
    return VTERM_KEY_NONE;
  }
}

static void term_flush_output(PangoTerm *pt)
{
  size_t bufflen = vterm_output_get_buffer_current(pt->vt);
  if(bufflen) {
    char buffer[bufflen];
    bufflen = vterm_output_bufferread(pt->vt, buffer, bufflen);
    write(pt->master, buffer, bufflen);
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

static int cmp_positions(VTermPos a, VTermPos b) 
{
  if(a.row == b.row)
    return a.col - b.col;
  else
    return a.row - b.row;
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
    flush_glyphs(pt);
    if(font == 0 || font > sizeof(alt_fonts)/sizeof(alt_fonts[0]))
      ADDATTR(pango_attr_family_new(default_font));
    else
      ADDATTR(pango_attr_family_new(alt_fonts[font - 1]));
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
      int cursor_visible = pt->cursor_visible && (pt->cursor_blinkstate || !pt->has_focus);

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

  if(pt->cursor_visible) {
    repaint_cell(pt, pt->cursorpos);

    flush_glyphs(pt);
  }

  return TRUE;
}

static void cursor_start_blinking(PangoTerm *pt)
{
  pt->cursor_timer_id = g_timeout_add(cursor_blink_interval, cursor_blink, pt);

  /* Should start blinking in visible state */
  pt->cursor_blinkstate = 1;

  repaint_cell(pt, pt->cursorpos);
}

static void cursor_stop_blinking(PangoTerm *pt)
{
  g_source_remove(pt->cursor_timer_id);
  pt->cursor_timer_id = 0;

  /* Should always be in visible state */
  pt->cursor_blinkstate = 1;

  repaint_cell(pt, pt->cursorpos);
}

/*
 * VTerm event handlers
 */

int term_damage(VTermRect rect, void *user_data)
{
  PangoTerm *pt = user_data;

  repaint_rect(pt, rect);

  return 1;
}

int term_moverect(VTermRect dest, VTermRect src, void *user_data)
{
  PangoTerm *pt = user_data;

  flush_glyphs(pt);

  int cursor_in_area = pt->cursor_visible && pt->cursor_blinkstate &&
     (pt->cursorpos.col >= src.start_col && pt->cursorpos.col < src.end_col) &&
     (pt->cursorpos.row >= src.start_row && pt->cursorpos.row < src.end_row);

  if(cursor_in_area) {
    /* Hide cursor before reading source area */
    pt->cursor_visible = 0;
    repaint_cell(pt, pt->cursorpos);
    flush_glyphs(pt);
    pt->cursor_visible = 1;
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

  if(cursor_in_area) {
    /* Show cursor after writing dest area */
    repaint_cell(pt, pt->cursorpos);
    flush_glyphs(pt);
  }

  return 1;
}

int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user_data)
{
  PangoTerm *pt = user_data;

  pt->cursorpos = pos;
  pt->cursor_blinkstate = 1;

  if(pt->cursor_visible) {
    repaint_cell(pt, oldpos);
    repaint_cell(pt, pos);
  }

  return 1;
}

int term_settermprop(VTermProp prop, VTermValue *val, void *user_data)
{
  PangoTerm *pt = user_data;

  switch(prop) {
  case VTERM_PROP_CURSORVISIBLE:
    pt->cursor_visible = val->boolean;
    repaint_cell(pt, pt->cursorpos);
    break;

  case VTERM_PROP_CURSORBLINK:
    if(val->boolean && !pt->cursor_timer_id)
      cursor_start_blinking(pt);
    else if(!val->boolean && pt->cursor_timer_id)
      cursor_stop_blinking(pt);
    break;

  case VTERM_PROP_CURSORSHAPE:
    pt->cursor_shape = val->number;
    repaint_cell(pt, pt->cursorpos);
    break;

  case VTERM_PROP_ICONNAME:
    gdk_window_set_icon_name(GDK_WINDOW(pt->termwin->window), val->string);
    break;

  case VTERM_PROP_TITLE:
    gtk_window_set_title(GTK_WINDOW(pt->termwin), val->string);
    break;

  default:
    return 0;
  }

  return 1;
}

int term_setmousefunc(VTermMouseFunc func, void *data, void *user_data)
{
  PangoTerm *pt = user_data;

  pt->mousefunc = func;
  pt->mousedata = data;

  return 1;
}

int term_bell(void *user_data)
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

void widget_get_clipboard(GtkClipboard *clipboard, GtkSelectionData *data, guint info, gpointer user_data)
{
  PangoTerm *pt = user_data;

  gchar *text = fetch_flow_text(pt, pt->highlight_start, pt->highlight_stop);

  gtk_selection_data_set_text(data, text, -1);

  free(text);
}

void widget_clear_clipboard(GtkClipboard *clipboard, gpointer user_data)
{
  PangoTerm *pt = user_data;

  VTermPos old_start = pt->highlight_start,
           old_stop  = pt->highlight_stop;

  pt->highlight_start.row = -1;
  pt->highlight_stop.row  = -1;

  repaint_flow(pt, old_start, old_stop);
}

gboolean widget_keypress(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
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

  VTermModifier state = VTERM_MOD_NONE;
  if(event->state & GDK_SHIFT_MASK)
    state |= VTERM_MOD_SHIFT;
  if(event->state & GDK_CONTROL_MASK)
    state |= VTERM_MOD_CTRL;
  if(event->state & GDK_MOD1_MASK)
    state |= VTERM_MOD_ALT;

  VTermKey keyval = convert_keyval(event->keyval, &state);

  if(keyval)
    vterm_input_push_key(pt->vt, state, keyval);
  else if(event->keyval >= 0x01000000)
    vterm_input_push_char(pt->vt, state, event->keyval - 0x01000000);
  else if(event->keyval < 0x0f00)
    /* event->keyval already contains a Unicode codepoint so that's easy */
    vterm_input_push_char(pt->vt, state, event->keyval);
  else
    return FALSE;

  term_flush_output(pt);

  return FALSE;
}

gboolean widget_mousepress(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  int col = event->x / pt->cell_width;
  int row = event->y / pt->cell_height;

  /* If the mouse is being dragged, we'll get motion events even outside our
   * window */
  if(col < 0 || col >= cols || row < 0 || row >= lines)
    return FALSE;

  /* Shift modifier bypasses terminal mouse handling */
  if(pt->mousefunc && !(event->state & GDK_SHIFT_MASK)) {
    (*pt->mousefunc)(col, row, event->button, event->type == GDK_BUTTON_PRESS, pt->mousedata);
    term_flush_output(pt);
  }
  else if(event->button == 2 && event->type == GDK_BUTTON_PRESS) {
    /* Middle-click paste */
    gchar *str = gtk_clipboard_wait_for_text(pt->primary_clipboard);

    term_push_string(pt, str);
  }
  else if(event->button == 1 && event->type == GDK_BUTTON_PRESS) {
    if(pt->highlight_start.row != -1 && pt->highlight_stop.row != -1) {
      gtk_clipboard_clear(pt->primary_clipboard);
    }

    pt->drag_start.row = row;
    pt->drag_start.col = col;
    pt->drag_pos.row = -1;
  }
  else if(event->button == 1 && event->type == GDK_BUTTON_RELEASE && pt->drag_pos.row != -1) {
    pt->drag_start.row = -1;
    pt->drag_pos.row   = -1;

    GtkTargetEntry info = {
      .target = "UTF8_STRING",
      .flags  = 0,
      .info   = 0,
    };

    gtk_clipboard_set_with_data(pt->primary_clipboard,
        &info, 1, widget_get_clipboard, widget_clear_clipboard, pt);
  }

  return FALSE;
}

gboolean widget_mousemove(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  int col = event->x / pt->cell_width;
  int row = event->y / pt->cell_height;

  /* If the mouse is being dragged, we'll get motion events even outside our
   * window */
  if(col < 0 || col >= cols || row < 0 || row >= lines)
    return FALSE;

  /* Shift modifier bypasses terminal mouse handling */
  if(pt->mousefunc && !(event->state & GDK_SHIFT_MASK)) {
    (*pt->mousefunc)(col, row, 0, 0, pt->mousedata);
    term_flush_output(pt);
  }
  else if(event->state & GDK_BUTTON1_MASK) {
    VTermPos old_end = pt->drag_pos;
    if(old_end.row == -1)
      old_end = pt->drag_start;

    pt->drag_pos.row = row;
    pt->drag_pos.col = col;

    if(cmp_positions(pt->drag_start, pt->drag_pos) > 0) {
      pt->highlight_start = pt->drag_pos;
      pt->highlight_stop  = pt->drag_start;
    }
    else {
      pt->highlight_start = pt->drag_start;
      pt->highlight_stop  = pt->drag_pos;
    }

    if(cmp_positions(old_end, pt->drag_pos) > 0)
      repaint_flow(pt, pt->drag_pos, old_end);
    else
      repaint_flow(pt, old_end, pt->drag_pos);
  }

  return FALSE;
}

gboolean widget_im_commit(GtkIMContext *context, gchar *str, gpointer user_data)
{
  PangoTerm *pt = user_data;

  term_push_string(pt, str);

  return FALSE;
}

gboolean widget_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  PangoTerm *pt = user_data;

  blit_buffer(pt, &event->area);

  return TRUE;
}

void widget_resize(GtkContainer* widget, gpointer user_data)
{
  PangoTerm *pt = user_data;

  gint raw_width, raw_height;
  gtk_window_get_size(GTK_WINDOW(widget), &raw_width, &raw_height);

  cols = raw_width   / pt->cell_width;
  lines = raw_height / pt->cell_height;

  pt->cols = cols;
  pt->rows = lines;

  struct winsize size = { lines, cols, 0, 0 };
  ioctl(pt->master, TIOCSWINSZ, &size);

  cairo_surface_t* new_buffer = gdk_window_create_similar_surface(pt->termdraw,
      CAIRO_CONTENT_COLOR,
      cols * pt->cell_width,
      lines * pt->cell_height);

  cairo_t* gc = cairo_create(new_buffer);
  cairo_set_source_surface(gc, pt->buffer, 0, 0);
  cairo_paint(gc);
  cairo_destroy(gc);

  cairo_surface_destroy(pt->buffer);
  pt->buffer = new_buffer;

  vterm_set_size(pt->vt, lines, cols);

  return;
}

void widget_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  PangoTerm *pt = user_data;
  pt->has_focus = 1;

  if(pt->cursor_visible) {
    repaint_cell(pt, pt->cursorpos);

    flush_glyphs(pt);
  }
}

void widget_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  PangoTerm *pt = user_data;
  pt->has_focus = 0;

  if(pt->cursor_visible) {
    repaint_cell(pt, pt->cursorpos);

    flush_glyphs(pt);
  }
}

void widget_quit(GtkContainer* widget, gpointer unused_data)
{
  gtk_main_quit();
}

/*
 * Misc.
 */

gboolean master_readable(GIOChannel *source, GIOCondition cond, gpointer user_data)
{
  PangoTerm *pt = user_data;

  char buffer[8192];

  ssize_t bytes = read(pt->master, buffer, sizeof buffer);

  if(bytes == 0 || (bytes == -1 && errno == EIO)) {
    gtk_main_quit();
    return FALSE;
  }
  if(bytes < 0) {
    fprintf(stderr, "read(master) failed - %s\n", strerror(errno));
    exit(1);
  }

#ifdef DEBUG_PRINT_INPUT
  printf("Read %zd bytes from master:\n", bytes);
  int i;
  for(i = 0; i < bytes; i++) {
    printf(i % 16 == 0 ? " |  %02x" : " %02x", buffer[i]);
    if(i % 16 == 15)
      printf("\n");
  }
  if(i % 16)
    printf("\n");
#endif

  /* Hide cursor during damage flush */

  int was_cursor_visible = pt->cursor_visible;
  pt->cursor_visible = 0;
  repaint_cell(pt, pt->cursorpos);

  vterm_push_bytes(pt->vt, buffer, bytes);

  vterm_screen_flush_damage(pt->vts);

  flush_glyphs(pt);

  if(was_cursor_visible) {
    pt->cursor_visible = 1;
    repaint_cell(pt, pt->cursorpos);
  }

  flush_glyphs(pt);

  return TRUE;
}

int main(int argc, char *argv[])
{
  GError *args_error = NULL;
  GOptionContext *args_context;

  args_context = g_option_context_new("commandline...");
  g_option_context_add_main_entries(args_context, option_entries, NULL);
  g_option_context_add_group(args_context, gtk_get_option_group(TRUE));
  if(!g_option_context_parse(args_context, &argc, &argv, &args_error)) {
    fprintf(stderr, "Option parsing failed: %s\n", args_error->message);
    exit (1);
  }

  gtk_init(&argc, &argv);

  struct winsize size = { lines, cols, 0, 0 };

  PangoTerm *pt = g_new0(PangoTerm, 1);

  pt->cols = cols;
  pt->rows = lines;

  pt->vt = vterm_new(size.ws_row, size.ws_col);
  vterm_parser_set_utf8(pt->vt, 1);

  pt->termwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(pt->termwin), default_title);
  gtk_widget_set_double_buffered(pt->termwin, FALSE);

  pt->glyphs = g_string_sized_new(128);
  pt->glyph_widths = g_array_new(FALSE, FALSE, sizeof(int));

  gtk_widget_realize(pt->termwin);

  pt->termdraw = pt->termwin->window;

  gdk_window_set_cursor(GDK_WINDOW(pt->termdraw), gdk_cursor_new(GDK_XTERM));

  GdkColor gdk_col;
  gdk_color_parse(default_fg, &gdk_col);

  VTermColor col_fg;
  col_fg.red   = gdk_col.red   / 257;
  col_fg.green = gdk_col.green / 257;
  col_fg.blue  = gdk_col.blue  / 257;

  gdk_color_parse(default_bg, &gdk_col);

  VTermColor col_bg;
  col_bg.red   = gdk_col.red   / 257;
  col_bg.green = gdk_col.green / 257;
  col_bg.blue  = gdk_col.blue  / 257;

  GdkColormap* colormap = gdk_colormap_get_system();
  gdk_rgb_find_color(colormap, &gdk_col);
  gdk_window_set_background(pt->termdraw, &gdk_col);
  vterm_state_set_default_colors(vterm_obtain_state(pt->vt), &col_fg, &col_bg);
  vterm_state_set_bold_highbright(vterm_obtain_state(pt->vt), 1);

  pt->vts = vterm_obtain_screen(pt->vt);
  vterm_screen_enable_altscreen(pt->vts, 1);
  vterm_screen_set_callbacks(pt->vts, &cb, pt);
  vterm_screen_set_damage_merge(pt->vts, VTERM_DAMAGE_ROW);

  pt->cursor_timer_id = g_timeout_add(cursor_blink_interval, cursor_blink, pt);
  pt->cursor_blinkstate = 1;
  pt->cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;

  GdkEventMask mask = gdk_window_get_events(pt->termwin->window);
  gdk_window_set_events(pt->termwin->window, mask|GDK_BUTTON_PRESS_MASK|GDK_BUTTON_RELEASE_MASK|GDK_POINTER_MOTION_MASK);

  g_signal_connect(G_OBJECT(pt->termwin), "expose-event", GTK_SIGNAL_FUNC(widget_expose), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "key-press-event", GTK_SIGNAL_FUNC(widget_keypress), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "button-press-event",   GTK_SIGNAL_FUNC(widget_mousepress), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "button-release-event", GTK_SIGNAL_FUNC(widget_mousepress), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "motion-notify-event",  GTK_SIGNAL_FUNC(widget_mousemove), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "focus-in-event",  GTK_SIGNAL_FUNC(widget_focus_in),  pt);
  g_signal_connect(G_OBJECT(pt->termwin), "focus-out-event", GTK_SIGNAL_FUNC(widget_focus_out), pt);
  g_signal_connect(G_OBJECT(pt->termwin), "destroy", GTK_SIGNAL_FUNC(widget_quit), pt);

  pt->im_context = gtk_im_context_simple_new();

  g_signal_connect(G_OBJECT(pt->im_context), "commit", GTK_SIGNAL_FUNC(widget_im_commit), pt);
  cairo_t *cctx = gdk_cairo_create(pt->termdraw);
  PangoContext *pctx = pango_cairo_create_context(cctx);

  PangoFontDescription *fontdesc = pango_font_description_new();
  pango_font_description_set_family(fontdesc, default_font);
  pango_font_description_set_size(fontdesc, default_size * PANGO_SCALE);

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
      size.ws_col * pt->cell_width, size.ws_row * pt->cell_height);

  pt->buffer = gdk_window_create_similar_surface(pt->termdraw,
      CAIRO_CONTENT_COLOR,
      size.ws_col * pt->cell_width,
      size.ws_row * pt->cell_height);
  gdk_color_parse(cursor_col_str, &pt->cursor_col);

  GdkGeometry hints;

  hints.min_width  = pt->cell_width;
  hints.min_height = pt->cell_height;
  hints.width_inc  = pt->cell_width;
  hints.height_inc = pt->cell_height;

  gtk_window_set_resizable(GTK_WINDOW(pt->termwin), TRUE);
  gtk_window_set_geometry_hints(GTK_WINDOW(pt->termwin), GTK_WIDGET(pt->termwin), &hints, GDK_HINT_RESIZE_INC | GDK_HINT_MIN_SIZE);
  g_signal_connect(G_OBJECT(pt->termwin), "check-resize", GTK_SIGNAL_FUNC(widget_resize), pt);

  pt->drag_start.row      = -1;
  pt->drag_pos.row        = -1;
  pt->highlight_start.row = -1;
  pt->highlight_stop.row  = -1;

  pt->primary_clipboard = gtk_clipboard_get(GDK_SELECTION_PRIMARY);

  /* None of the docs about termios explain how to construct a new one of
   * these, so this is largely a guess */
  struct termios termios = {
    .c_iflag = ICRNL|IXON|IUTF8,
    .c_oflag = OPOST|ONLCR|NL0|CR0|TAB0|BS0|VT0|FF0,
    .c_cflag = CS8|CREAD,
    .c_lflag = ISIG|ICANON|IEXTEN|ECHO|ECHOE|ECHOK,
    /* c_cc later */
  };

#ifdef ECHOCTL
  termios.c_lflag |= ECHOCTL;
#endif
#ifdef ECHOKE
  termios.c_lflag |= ECHOKE;
#endif

  cfsetspeed(&termios, 38400);

  termios.c_cc[VINTR]    = 0x1f & 'C';
  termios.c_cc[VQUIT]    = 0x1f & '\\';
  termios.c_cc[VERASE]   = 0x1f & 'H';
  termios.c_cc[VKILL]    = 0x1f & 'U';
  termios.c_cc[VEOF]     = 0x1f & 'D';
  termios.c_cc[VEOL]     = _POSIX_VDISABLE;
  termios.c_cc[VEOL2]    = _POSIX_VDISABLE;
  termios.c_cc[VSTART]   = 0x1f & 'Q';
  termios.c_cc[VSTOP]    = 0x1f & 'S';
  termios.c_cc[VSUSP]    = 0x1f & 'Z';
  termios.c_cc[VREPRINT] = 0x1f & 'R';
  termios.c_cc[VWERASE]  = 0x1f & 'W';
  termios.c_cc[VLNEXT]   = 0x1f & 'V';
  termios.c_cc[VMIN]     = 1;
  termios.c_cc[VTIME]    = 0;

  pid_t kid = forkpty(&pt->master, NULL, &termios, &size);
  if(kid == 0) {
    /* Restore the ISIG signals back to defaults */
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGSTOP, SIG_DFL);
    signal(SIGCONT, SIG_DFL);

    putenv("TERM=xterm");
    if(argc > 1) {
      execvp(argv[1], argv + 1);
      fprintf(stderr, "Cannot exec(%s) - %s\n", argv[1], strerror(errno));
    }
    else {
      char *shell = getenv("SHELL");
      char *args[2] = { shell, NULL };
      execvp(shell, args);
      fprintf(stderr, "Cannot exec(%s) - %s\n", shell, strerror(errno));
    }
    _exit(1);
  }

  GIOChannel *gio_master = g_io_channel_unix_new(pt->master);
  g_io_add_watch(gio_master, G_IO_IN|G_IO_HUP, master_readable, pt);

  gtk_widget_show_all(pt->termwin);

  vterm_screen_reset(pt->vts);

  gtk_main();

  vterm_free(pt->vt);

  return 0;
}
