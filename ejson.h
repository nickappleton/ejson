#ifndef EJSON_H
#define EJSON_H

#include "json_iface.h"
#include <stdarg.h>
#include "../istrings.h"
#include "../pdict.h"



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
	struct istrings         strings;
	struct pdict            workspace;
	struct linear_allocator alloc;
	unsigned                stack_depth;

};

int evaluation_context_init(struct evaluation_context *p_ctx);
void evaluation_context_free(struct evaluation_context *p_ctx);
int ejson_load(struct jnode *p_node, struct evaluation_context *p_workspace, const char *p_document, struct ejson_error_handler *p_error_handler);

#endif /* EJSON_H */
