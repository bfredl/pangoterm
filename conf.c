#include "conf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <glib.h>
#include <gtk/gtk.h>

ConfigEntry *configs = NULL;

static int conf_from_file(const char *path)
{
  int fd = open(path, O_RDONLY);
  if(!fd) {
    fprintf(stderr, "Cannot open configuration file %s: %s\n", path, strerror(errno));
    return 0;
  }

  GScanner *scanner = g_scanner_new(NULL);
  scanner->input_name = g_strdup(path);
  g_scanner_input_file(scanner, fd);

  GTokenType t;
  while((t = g_scanner_get_next_token(scanner)) != G_TOKEN_EOF) {
    if(t == G_TOKEN_IDENTIFIER) {
      char *name = scanner->value.v_identifier;

      ConfigEntry *cfg = NULL;
      for(ConfigEntry *p = configs; p; p = p->next) {
        if(strcmp(name, p->longname) != 0)
          continue;
        cfg = p;
        break;
      }

      if(!cfg) {
        g_scanner_error(scanner, "\"%s\" is not a recognised setting name", name);
        goto abort;
      }

      if(g_scanner_get_next_token(scanner) != G_TOKEN_EQUAL_SIGN) {
        g_scanner_error(scanner, "Expected '='");
        goto abort;
      }

      t = g_scanner_get_next_token(scanner);
      switch(cfg->type) {
        case CONF_TYPE_STRING:
          if(t != G_TOKEN_STRING) {
            g_scanner_error(scanner, "Expected \"%s\" to take a string value", cfg->longname);
            goto abort;
          }
          if(!cfg->var_set) {
            *(char**)cfg->var = g_strdup(scanner->value.v_string);
            cfg->var_set = TRUE;
          }
          break;
        case CONF_TYPE_INT:
          if(t != G_TOKEN_INT) {
            g_scanner_error(scanner, "Expected \"%s\" to take an integer value", cfg->longname);
            goto abort;
          }
          if(!cfg->var_set) {
            *(int*)cfg->var = scanner->value.v_int;
            cfg->var_set = TRUE;
          }
          break;
        case CONF_TYPE_DOUBLE:
          if(t == G_TOKEN_INT) {
            t = G_TOKEN_FLOAT;
            scanner->value.v_float = scanner->value.v_int;
          }
          if(t != G_TOKEN_FLOAT) {
            g_scanner_error(scanner, "Expected \"%s\" to take a float value", cfg->longname);
            goto abort;
          }
          if(!cfg->var_set) {
            *(double*)cfg->var = scanner->value.v_float;
            cfg->var_set = TRUE;
          }
          break;
      }
    }
    else {
      g_scanner_error(scanner, "Expected a setting name");
      goto abort;
    }
  }

  g_scanner_destroy(scanner);
  close(fd);

  return 1;

abort:
  g_scanner_destroy(scanner);
  close(fd);

  return 0;
}

int conf_parse(int *argcp, char ***argvp)
{
  int n_entries = 0;
  for(ConfigEntry *p = configs; p; p = p->next)
    n_entries++;

  GOptionEntry *option_entries = malloc(sizeof(GOptionEntry) * (n_entries + 2));

  char *config_file = NULL;
  option_entries[0].long_name  = "config-file";
  option_entries[0].short_name = 0;
  option_entries[0].flags      = 0;
  option_entries[0].arg        = G_OPTION_ARG_FILENAME;
  option_entries[0].arg_data   = &config_file;
  option_entries[0].description     = "Path to config file";
  option_entries[0].arg_description = "PATH";

  int i = 1;
  for(ConfigEntry *cfg = configs; cfg; cfg = cfg->next, i++) {
    option_entries[i].long_name  = cfg->longname;
    option_entries[i].short_name = cfg->shortname;
    option_entries[i].flags      = 0;
    switch(cfg->type) {
      case CONF_TYPE_STRING:
        option_entries[i].arg = G_OPTION_ARG_STRING;
        break;
      case CONF_TYPE_INT:
        option_entries[i].arg = G_OPTION_ARG_INT;
        break;
      case CONF_TYPE_DOUBLE:
        option_entries[i].arg = G_OPTION_ARG_DOUBLE;
        break;
    }
    option_entries[i].arg_data        = cfg->var;
    option_entries[i].description     = cfg->desc;
    option_entries[i].arg_description = cfg->argdesc;
  }

  option_entries[i].long_name = NULL;

  GError *args_error = NULL;
  GOptionContext *args_context;

  args_context = g_option_context_new("commandline...");
  g_option_context_add_main_entries(args_context, option_entries, NULL);
  free(option_entries);

  g_option_context_add_group(args_context, gtk_get_option_group(TRUE));
  if(!g_option_context_parse(args_context, argcp, argvp, &args_error)) {
    fprintf(stderr, "Option parsing failed: %s\n", args_error->message);
    return 0;
  }

  /* g_option doesn't give us any way to tell if variables were set or not;
   * this is the best we can do
   */
  for(ConfigEntry *cfg = configs; cfg; cfg = cfg->next) {
    switch(cfg->type) {
      case CONF_TYPE_STRING:
        cfg->var_set = *(void**)cfg->var != NULL;
        break;
      case CONF_TYPE_INT:
        cfg->var_set = *(int*)cfg->var != -1;
        break;
      case CONF_TYPE_DOUBLE:
        cfg->var_set = *(double*)cfg->var != -1.0;
        break;
    }
  }

  if(config_file) {
    if(!conf_from_file(config_file))
      return 0;
  }
  else {
    config_file = g_strdup_printf("%s/.config/pangoterm.cfg", getenv("HOME"));
    struct stat st;
    if(stat(config_file, &st) == 0)
      if(!conf_from_file(config_file))
        return 0;
    g_free(config_file);
  }

  for(ConfigEntry *cfg = configs; cfg; cfg = cfg->next) {
    switch(cfg->type) {
      case CONF_TYPE_STRING:
        if(!cfg->var_set)
          *(char**)cfg->var = cfg->dflt.s;
        break;
      case CONF_TYPE_INT:
        if(!cfg->var_set)
          *(int*)cfg->var = cfg->dflt.i;
        break;
      case CONF_TYPE_DOUBLE:
        if(!cfg->var_set)
          *(double*)cfg->var = cfg->dflt.d;
        break;
    }
  }

  return 1;
}