#ifndef JSON_SIMPLE_LOAD_H
#define JSON_SIMPLE_LOAD_H

#include "json_iface.h"

int parse_json(struct jnode *p_root, struct linear_allocator *p_alloc, struct linear_allocator *p_temps, const char *p_json);

#endif /* JSON_SIMPLE_LOAD_H */
