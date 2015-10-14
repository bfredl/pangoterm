#ifndef __CONF_H__
#define __CONF_H__

#include <stdbool.h>

typedef enum {
  CONF_TYPE_STRING,
  CONF_TYPE_INT,
  CONF_TYPE_DOUBLE,
  CONF_TYPE_BOOL,
} ConfigType;

typedef union {
  char *s;
  int i;
  double d;
} ConfigValue;

typedef struct ConfigEntry ConfigEntry;
struct ConfigEntry {
  ConfigEntry *next;

  const char *longname;
  char shortname;

  ConfigType type;
  bool is_parametric;

  union {
    void *var;    /* ptr to char* or int or double */
    void (*apply)(int key, ConfigValue value);
  };
  int var_set;

  const char *desc;
  const char *argdesc;

  ConfigValue from_file;
  int var_set_from_file;

  ConfigValue dflt;
};

extern ConfigEntry *configs;

#define CONF_STRING(name,shortname_,dflt_,desc_,argdesc_) \
  static char *CONF_##name; \
  static void __attribute__((constructor)) DECLARE_##name(void) { \
    static ConfigEntry config = {                                 \
      .longname = #name,                                          \
      .shortname = shortname_,                                    \
      .type = CONF_TYPE_STRING,                                   \
      .var = &CONF_##name,                                        \
      .var_set = FALSE,                                           \
      .desc = desc_,                                              \
      .argdesc = argdesc_,                                        \
      .dflt.s = dflt_,                                            \
    };                                                            \
    config.next = configs;                                        \
    configs = &config;                                            \
  }

#define CONF_PARAMETRIC_STRING(name,shortname_,func,desc_,argdesc_) \
  static void __attribute__((constructor)) DECLARE_##name(void) { \
    static ConfigEntry config = {                                 \
      .longname = #name,                                          \
      .shortname = shortname_,                                    \
      .type = CONF_TYPE_STRING,                                   \
      .is_parametric = true,                                      \
      .apply = func,                                              \
      .desc = desc_,                                              \
      .argdesc = argdesc_,                                        \
    };                                                            \
    config.next = configs;                                        \
    configs = &config;                                            \
  }

#define CONF_INT(name,shortname_,dflt_,desc_,argdesc_) \
  static int CONF_##name = -1; \
  static void __attribute__((constructor)) DECLARE_##name(void) { \
    static ConfigEntry config = {                                 \
      .longname = #name,                                          \
      .shortname = shortname_,                                    \
      .type = CONF_TYPE_INT,                                      \
      .var = &CONF_##name,                                        \
      .var_set = FALSE,                                           \
      .desc = desc_,                                              \
      .argdesc = argdesc_,                                        \
      .dflt.i = dflt_,                                            \
    };                                                            \
    config.next = configs;                                        \
    configs = &config;                                            \
  }

#define CONF_DOUBLE(name,shortname_,dflt_,desc_,argdesc_) \
  static double CONF_##name = -1.0; \
  static void __attribute__((constructor)) DECLARE_##name(void) { \
    static ConfigEntry config = {                                 \
      .longname = #name,                                          \
      .shortname = shortname_,                                    \
      .type = CONF_TYPE_DOUBLE,                                   \
      .var = &CONF_##name,                                        \
      .var_set = FALSE,                                           \
      .desc = desc_,                                              \
      .argdesc = argdesc_,                                        \
      .dflt.d = dflt_,                                            \
    };                                                            \
    config.next = configs;                                        \
    configs = &config;                                            \
  }

#define CONF_BOOL(name,shortname_,dflt_,desc_) \
  static int CONF_##name = FALSE; \
  static void __attribute__((constructor)) DECLARE_##name(void) { \
    static ConfigEntry config = {                                 \
      .longname = #name,                                          \
      .shortname = shortname_,                                    \
      .type = CONF_TYPE_BOOL,                                     \
      .var = &CONF_##name,                                        \
      .var_set = FALSE,                                           \
      .desc = desc_,                                              \
      .argdesc = NULL,                                            \
      .dflt.i = dflt_,                                            \
    };                                                            \
    config.next = configs;                                        \
    configs = &config;                                            \
  }

int conf_parse(int *argcp, char ***argvp);

#endif
