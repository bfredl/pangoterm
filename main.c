/* for putenv() */
#define _XOPEN_SOURCE

/* for ECHOCTL, ECHOKE, cfsetspeed() */
#define _BSD_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>

/* suck up the non-standard openpty/forkpty */
#if defined(__FreeBSD__)
# include <libutil.h>
# include <termios.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
# include <termios.h>
# include <util.h>
#else
# include <pty.h>
#endif

#include <gtk/gtk.h>

#include "pangoterm.h"

#include "conf.h"

CONF_STRING(foreground, 0, "gray90", "Foreground colour", "COL");
CONF_STRING(background, 0, "black",  "Background colour", "COL");

CONF_STRING(font, 0,   "DejaVu Sans Mono", "Font name", "STR");

CONF_STRING(title, 0, "pangoterm", "Title", "STR");

CONF_INT(lines, 0, 25, "Number of lines",   "NUM");
CONF_INT(cols,  0, 80, "Number of columns", "NUM");

CONF_STRING(term, 0, "xterm", "Terminal type", "STR");

static char *alt_fonts[] = {
  "Courier 10 Pitch",
};

static int master;

static size_t write_master(const char *bytes, size_t len, void *user)
{
  return write(master, bytes, len);
}

static void resized(int rows, int cols, void *user)
{
  struct winsize size = { rows, cols, 0, 0 };
  ioctl(master, TIOCSWINSZ, &size);
}

static gboolean master_readable(GIOChannel *source, GIOCondition cond, gpointer user_data)
{
  PangoTerm *pt = user_data;

  pangoterm_begin_update(pt);

  /* Make sure we don't take longer than 20msec doing this */
  guint64 deadline_time = g_get_real_time() + 20*1000;

  while(1) {
    /* Linux kernel's PTY buffer is a fixed 4096 bytes (1 page) so there's
     * never any point read()ing more than that
     */
    char buffer[4096];

    ssize_t bytes = read(master, buffer, sizeof buffer);

    if(bytes == -1 && errno == EAGAIN)
      break;

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

    pangoterm_push_bytes(pt, buffer, bytes);

    if(g_get_real_time() >= deadline_time)
      break;
  }

  pangoterm_end_update(pt);

  return TRUE;
}

int main(int argc, char *argv[])
{
  if(!conf_parse(&argc, &argv))
    exit(1);

  // GLib has consumed the options, but it might leave a -- in place in argv[1]
  if(argc > 1 && strcmp(argv[1], "--") == 0) {
    argv++;
    argc--;
  }

  gtk_init(&argc, &argv);
  setlocale(LC_CTYPE, NULL);

  PangoTerm *pt = pangoterm_new(CONF_lines, CONF_cols);

  GdkColor fg_col;
  gdk_color_parse(CONF_foreground, &fg_col);

  GdkColor bg_col;
  gdk_color_parse(CONF_background, &bg_col);

  pangoterm_set_default_colors(pt, &fg_col, &bg_col);

  pangoterm_set_fonts(pt, CONF_font, alt_fonts);

  pangoterm_set_title(pt, CONF_title);

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
  termios.c_cc[VERASE]   = 0x7f;
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

  struct winsize size = { CONF_lines, CONF_cols, 0, 0 };
  pid_t kid = forkpty(&master, NULL, &termios, &size);
  if(kid == 0) {
    /* Restore the ISIG signals back to defaults */
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGSTOP, SIG_DFL);
    signal(SIGCONT, SIG_DFL);

    gchar *term = g_strdup_printf("TERM=%s", CONF_term);
    putenv(term);
    /* Do not free 'term', it is part of the environment */

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

  fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

  GIOChannel *gio_master = g_io_channel_unix_new(master);
  g_io_add_watch(gio_master, G_IO_IN|G_IO_HUP, master_readable, pt);

  pangoterm_set_write_fn(pt, &write_master, NULL);
  pangoterm_set_resized_fn(pt, &resized, NULL);

  pangoterm_start(pt);

  gtk_main();

  pangoterm_free(pt);

  return 0;
}
