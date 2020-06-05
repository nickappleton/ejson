#ifndef EJSON_H
#define EJSON_H

#include "json_iface.h"
#include <stdarg.h>

struct ejson_error_handler {
	void  *p_context;
	void (*on_parser_error)(void *p_context, const char *p_format, va_list args);
};

//int parse_ejson(struct jnode *p_root, struct linear_allocator *p_alloc, struct linear_allocator *p_temps, const char *p_json);

#endif /* EJSON_H */
