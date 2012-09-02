#ifndef __PANGOTERM_H__
#define __PANGOTERM_H__

#include "vterm.h"

#include <gtk/gtk.h>

typedef struct PangoTerm PangoTerm;

PangoTerm *pangoterm_new(int rows, int cols);
void pangoterm_free(PangoTerm *pt);

void pangoterm_set_default_colors(PangoTerm *pt, GdkColor *fg_col, GdkColor *bg_col);
void pangoterm_set_font_size(PangoTerm *pt, double size);
void pangoterm_set_fonts(PangoTerm *pt, char *font, char **alt_fonts); // ptr not value

void pangoterm_set_title(PangoTerm *pt, const char *title);

void pangoterm_start(PangoTerm *pt);

void pangoterm_begin_update(PangoTerm *pt);
void pangoterm_push_bytes(PangoTerm *pt, const char *bytes, size_t len);
void pangoterm_end_update(PangoTerm *pt);

typedef size_t PangoTermWriteFn(const char *bytes, size_t len, void *user);
void pangoterm_set_write_fn(PangoTerm *pt, PangoTermWriteFn *fn, void *user);

typedef void PangoTermResizedFn(int rows, int cols, void *user);
void pangoterm_set_resized_fn(PangoTerm *pt, PangoTermResizedFn *fn, void *user);

#endif
