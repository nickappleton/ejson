#ifndef EJSON_H
#define EJSON_H

#include "ejson_iface.h"
#include <stdarg.h>
#include "cop/cop_strdict.h"
#include "cop/cop_alloc.h"

struct token_pos_info {
	const char           *p_line;
	uint_fast32_t         line_nb;
	uint_fast32_t         char_pos;
};

struct ejson_error_handler {
	void  *p_context;
	void (*on_parser_error)(void *p_context, const struct token_pos_info *p_location, const char *p_format, va_list args);
};


struct evaluation_context {
	struct cop_strdict_node *p_workspace;
	struct cop_salloc_iface *p_alloc;
	unsigned                 stack_depth;

};

void evaluation_context_init(struct evaluation_context *p_ctx, struct cop_salloc_iface *p_alloc);
int ejson_load(struct jnode *p_node, struct evaluation_context *p_workspace, const char *p_document, struct ejson_error_handler *p_error_handler);

#endif /* EJSON_H */
