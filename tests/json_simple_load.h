#ifndef JSON_SIMPLE_LOAD_H
#define JSON_SIMPLE_LOAD_H

#include "ejson/ejson_iface.h"

int parse_json(struct jnode *p_root, struct cop_salloc_iface *p_alloc, struct cop_salloc_iface *p_temps, const char *p_json);

#endif /* JSON_SIMPLE_LOAD_H */
