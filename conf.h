#ifndef __CONF_H__
#define __CONF_H__

typedef enum {
  CONF_TYPE_STRING,
  CONF_TYPE_INT,
  CONF_TYPE_DOUBLE,
} ConfigType;

typedef struct {
  const char *longname;
  char shortname;

  ConfigType type;

  void *var; /* ptr to char* or int or double */

  const char *desc;
  const char *argdesc;
} ConfigEntry;

#define CONF_STRING(name,shortname,var,desc,argdesc) \
  { name, shortname, CONF_TYPE_STRING, &var, desc, argdesc }
#define CONF_INT(name,shortname,var,desc,argdesc) \
  { name, shortname, CONF_TYPE_INT, &var, desc, argdesc }
#define CONF_DOUBLE(name,shortname,var,desc,argdesc) \
  { name, shortname, CONF_TYPE_DOUBLE, &var, desc, argdesc }

int conf_parse(ConfigEntry *entries, int *argcp, char ***argvp);

#endif
