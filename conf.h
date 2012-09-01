#ifndef __CONF_H__
#define __CONF_H__

typedef enum {
  CONF_TYPE_STRING,
  CONF_TYPE_INT,
  CONF_TYPE_DOUBLE,
} ConfigType;

typedef struct ConfigEntry ConfigEntry;
struct ConfigEntry {
  ConfigEntry *next;

  const char *longname;
  char shortname;

  ConfigType type;

  void *var; /* ptr to char* or int or double */

  const char *desc;
  const char *argdesc;
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
      .desc = desc_,                                              \
      .argdesc = argdesc_,                                        \
    };                                                            \
    CONF_##name = dflt_;                                          \
    config.next = configs;                                        \
    configs = &config;                                            \
  }

#define CONF_INT(name,shortname_,dflt_,desc_,argdesc_) \
  static int CONF_##name; \
  static void __attribute__((constructor)) DECLARE_##name(void) { \
    static ConfigEntry config = {                                 \
      .longname = #name,                                          \
      .shortname = shortname_,                                    \
      .type = CONF_TYPE_INT,                                      \
      .var = &CONF_##name,                                        \
      .desc = desc_,                                              \
      .argdesc = argdesc_,                                        \
    };                                                            \
    CONF_##name = dflt_;                                          \
    config.next = configs;                                        \
    configs = &config;                                            \
  }

#define CONF_DOUBLE(name,shortname_,dflt_,desc_,argdesc_) \
  static double CONF_##name; \
  static void __attribute__((constructor)) DECLARE_##name(void) { \
    static ConfigEntry config = {                                 \
      .longname = #name,                                          \
      .shortname = shortname_,                                    \
      .type = CONF_TYPE_DOUBLE,                                   \
      .var = &CONF_##name,                                        \
      .desc = desc_,                                              \
      .argdesc = argdesc_,                                        \
    };                                                            \
    CONF_##name = dflt_;                                          \
    config.next = configs;                                        \
    configs = &config;                                            \
  }

int conf_parse(int *argcp, char ***argvp);

#endif
