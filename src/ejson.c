#include "ejson/ejson.h"
#include "cop/cop_filemap.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "parse_helpers.h"

/* IDEA: the evaluate ast function should return a pointer to an evaluated ast node object.
 *
 * The AST node object should contain an evaluated ast node structure in the union to hold
 * completely reduced items.
 * 
 * Or maybe this would just be messy? */

#define MAX_TOK_STRING (4096)

struct token {
	const struct tok_def  *cls;
	struct token_pos_info  posinfo;
	union {
		struct {
			size_t len;
			char   str[MAX_TOK_STRING];
		} strident;
		long long tint;
		double    tflt;
	} t;
};


struct token;

struct tok_def {
	const char           *name;
	int                   binary_precedence;
	int                   right_associative;
	const struct ast_cls *bin_op_cls;
	const struct ast_cls *unary_op_cls;
	int                   unary_precedence;
};

#ifndef ejson_error
#ifdef EJSON_NO_ERROR_MESSAGES
#define ejson_error(p_handler_, p_format, ...) (-1)
#define ejson_location_error(p_handler_, p_location, p_format, ...) (-1)
#endif
#endif

#ifndef ejson_error
static int ejson_error_fn(const struct ejson_error_handler *p_handler, const struct token_pos_info *p_location, const char *p_format, ...) {
	if (p_handler != NULL) {
		va_list args;
		va_start(args, p_format);
		p_handler->on_parser_error(p_handler->p_context, p_location, p_format, args);
		va_end(args);
	}
	return -1;
}
#define ejson_error(x_, ...)      ejson_error_fn(x_, NULL, __VA_ARGS__)
#define ejson_location_error(...) ejson_error_fn(__VA_ARGS__)
#endif

#ifndef ejson_error_null
#define ejson_error_null(...) (ejson_error(__VA_ARGS__), NULL)
#endif

#ifndef ejson_location_error_null
#define ejson_location_error_null(...) (ejson_location_error(__VA_ARGS__), NULL)
#endif

struct ast_node;

struct ast_cls {
	const char  *p_name;
	int        (*to_jnode)(struct ast_node *p_node, struct jnode *p_dest);
	void       (*debug_print)(const struct ast_node *p_node, FILE *p_f, unsigned depth);
};

#define DEF_AST_CLS(name_, to_jnode_fn_, debug_print_fn_) \
	static const struct ast_cls name_ = { #name_, to_jnode_fn_, debug_print_fn_ }

#define EJSON_TYPE_NULL     (0)
#define EJSON_TYPE_STRING   (1)
#define EJSON_TYPE_INTEGER  (2)
#define EJSON_TYPE_REAL     (3)
#define EJSON_TYPE_BOOL     (4)
#define EJSON_TYPE_DICT     (5)
#define EJSON_TYPE_LIST     (6)
#define EJSON_TYPE_FUNCTION (7)

struct ev_ast_node {
	const struct ast_node      *p_node;
	const struct ev_ast_node  **pp_stack;
	unsigned                    stack_size;

};

struct ast_node {
	const struct ast_cls  *cls;
	struct token_pos_info  doc_pos;

	union {
		long long                   i; /* AST_CLS_LITERAL_INT, AST_CLS_LITERAL_BOOL */
		double                      f; /* AST_CLS_LITERAL_FLOAT */
		struct {
			uint_fast32_t           len;
			const char             *p_data;
		} str; /* AST_CLS_LITERAL_STRING */
		struct {
			const struct ast_node  *p_test;
			const struct ast_node  *p_true;
			const struct ast_node  *p_false;
		} ifexpr; /* AST_CLS_NEG*, AST_CLS_ADD*, AST_CLS_SUB*, AST_CLS_MUL*, AST_CLS_DIV*, AST_CLS_MOD* */
		struct {
			const struct ast_node  *p_lhs;
			const struct ast_node  *p_rhs;
		} binop; /* AST_CLS_NEG*, AST_CLS_ADD*, AST_CLS_SUB*, AST_CLS_MUL*, AST_CLS_DIV*, AST_CLS_MOD* */
		struct {
			const struct ast_node **elements;
			uint_fast32_t           nb_elements;
		} llist; /* AST_CLS_LITERAL_LIST */
		struct {
			const struct ast_node **elements; /* 2*nb_keys - [key, value] */
			uint_fast32_t           nb_keys;
		} ldict; /* AST_CLS_LITERAL_DICT */
		struct {
			const struct ast_node  *p_function;
			const struct ast_node  *p_list;
		} lmap;
		struct {
			const struct ast_node  *node;
			unsigned                nb_args;
		} fn; /* AST_CLS_FUNCTION */
		struct {
			const struct ast_node  *fn;
			const struct ast_node  *p_args;
		} call;
		struct {
			const struct ast_node  *p_args;
		} builtin; /* AST_CLS_RANGE */
		struct {
			const struct ast_node  *p_function;
			const struct ast_node  *p_input_list;
		} map;
		struct {
			const struct ast_node  *p_data;
			const struct ast_node  *p_key;
		} access;

		struct {
			unsigned                 nb_keys;
			struct cop_strdict_node *p_root;
		} rdict;
		struct {
			int (*get_element)(struct ev_ast_node *p_node, const struct ev_ast_node *p_list, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler);
			union {
				struct {
					long long first;
					long long step;
				} range;
				struct {
					const struct ev_ast_node *p_function;
					const struct ev_ast_node *p_key;
				} map;
				struct {
					const struct ast_node **pp_values;
				} literal;
				struct {
					const struct ast_node *p_first;
					const struct ast_node *p_second;
				} cat;
			} d;
			uint_fast32_t     nb_elements;
		} lgen; /* AST_CLS_LIST_GENERATOR */

	} d;

};

struct dictnode {
	struct cop_strdict_node  node;
	const struct ast_node   *data; /* Unevaluated nodes */
};

#define TOK_DECL(name_, precedence_, right_associative_, binop_ast_cls_, unary_precedence_, unop_ast_cls_) \
	static const struct tok_def name_ = {#name_, precedence_, right_associative_, binop_ast_cls_, unop_ast_cls_, unary_precedence_}

static void debug_print_null(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
}
static void debug_print_int_like(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s(%lld)\n", depth, "", p_node->cls->p_name, p_node->d.i);
}
static void debug_print_real(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s(%f)\n", depth, "", p_node->cls->p_name, p_node->d.f);
}
static void debug_print_string(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s('%s')\n", depth, "", p_node->cls->p_name, p_node->d.str.p_data);
}
static void debug_print_bool(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s(%s)\n", depth, "", p_node->cls->p_name, (p_node->d.i) ? "true" : "false");
}
static void debug_print_list(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	unsigned i;
	fprintf(p_f, "%*s%s(%u)\n", depth, "", p_node->cls->p_name, p_node->d.llist.nb_elements);
	for (i = 0; i < p_node->d.llist.nb_elements; i++)
		p_node->d.llist.elements[i]->cls->debug_print(p_node->d.llist.elements[i], p_f, depth + 1);
}
static void debug_print_dict(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	unsigned i;
	fprintf(p_f, "%*s%s(%u)\n", depth, "", p_node->cls->p_name, p_node->d.ldict.nb_keys);
	for (i = 0; i < p_node->d.ldict.nb_keys; i++) {
		p_node->d.ldict.elements[2*i+0]->cls->debug_print(p_node->d.ldict.elements[2*i+0], p_f, depth + 1);
		p_node->d.ldict.elements[2*i+1]->cls->debug_print(p_node->d.ldict.elements[2*i+1], p_f, depth + 2);
	}
}
static void debug_print_binop(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.binop.p_lhs->cls->debug_print(p_node->d.binop.p_lhs, p_f, depth + 1);
	p_node->d.binop.p_rhs->cls->debug_print(p_node->d.binop.p_rhs, p_f, depth + 1);
}
static void debug_print_unop(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.binop.p_lhs->cls->debug_print(p_node->d.binop.p_lhs, p_f, depth + 1);
}
static void debug_print_builtin(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.builtin.p_args->cls->debug_print(p_node->d.builtin.p_args, p_f, depth + 1);
}
static void debug_print_function(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s(nb_args=%u)\n", depth, "", p_node->cls->p_name, p_node->d.fn.nb_args);
	p_node->d.fn.node->cls->debug_print(p_node->d.fn.node, p_f, depth + 1);
}
static void debug_print_call(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.call.fn->cls->debug_print(p_node->d.call.fn, p_f, depth + 1);
	p_node->d.call.p_args->cls->debug_print(p_node->d.call.p_args, p_f, depth + 1);
}
static void debug_print_access(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.access.p_data->cls->debug_print(p_node->d.access.p_data, p_f, depth + 1);
	p_node->d.access.p_key->cls->debug_print(p_node->d.access.p_key, p_f, depth + 1);
}
static void debug_print_map(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.map.p_function->cls->debug_print(p_node->d.map.p_function, p_f, depth + 1);
	p_node->d.map.p_input_list->cls->debug_print(p_node->d.map.p_input_list, p_f, depth + 1);
}
static void debug_list_generator(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	/* TODO */
}
static void debug_ready_dict(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	/* TODO */
}
static void debug_print_ifexpr(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.ifexpr.p_test->cls->debug_print(p_node->d.ifexpr.p_test, p_f, depth + 1);
	p_node->d.ifexpr.p_true->cls->debug_print(p_node->d.ifexpr.p_true, p_f, depth + 1);
	p_node->d.ifexpr.p_false->cls->debug_print(p_node->d.ifexpr.p_false, p_f, depth + 1);
}


DEF_AST_CLS(AST_CLS_LITERAL_NULL,    NULL, debug_print_null);
DEF_AST_CLS(AST_CLS_LITERAL_INT,     NULL, debug_print_int_like);
DEF_AST_CLS(AST_CLS_LITERAL_FLOAT,   NULL, debug_print_real);
DEF_AST_CLS(AST_CLS_LITERAL_STRING,  NULL, debug_print_string);
DEF_AST_CLS(AST_CLS_LITERAL_BOOL,    NULL, debug_print_bool);
DEF_AST_CLS(AST_CLS_LITERAL_LIST,    NULL, debug_print_list);
DEF_AST_CLS(AST_CLS_LITERAL_DICT,    NULL, debug_print_dict);
DEF_AST_CLS(AST_CLS_NEG,             NULL, debug_print_unop);
DEF_AST_CLS(AST_CLS_BITAND,          NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_BITOR,           NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_LOGNOT,          NULL, debug_print_unop);
DEF_AST_CLS(AST_CLS_LOGAND,          NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_LOGOR,           NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_ADD,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_SUB,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_MUL,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_DIV,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_MOD,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_EXP,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_EQ,              NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_NEQ,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_LT,              NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_LEQ,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_GEQ,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_GT,              NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_RANGE,           NULL, debug_print_builtin);
DEF_AST_CLS(AST_CLS_FUNCTION,        NULL, debug_print_function);
DEF_AST_CLS(AST_CLS_CALL,            NULL, debug_print_call);
DEF_AST_CLS(AST_CLS_ACCESS,          NULL, debug_print_access);
DEF_AST_CLS(AST_CLS_MAP,             NULL, debug_print_map);
DEF_AST_CLS(AST_CLS_FORMAT,          NULL, debug_print_builtin);
DEF_AST_CLS(AST_CLS_STACKREF,        NULL, debug_print_int_like);
DEF_AST_CLS(AST_CLS_IF,              NULL, debug_print_ifexpr);


DEF_AST_CLS(AST_CLS_LIST_GENERATOR,  NULL, debug_list_generator);
DEF_AST_CLS(AST_CLS_READY_DICT,      NULL, debug_ready_dict);

/* Numeric literals */
TOK_DECL(TOK_INT,        -1, 0, NULL, -1, NULL); /* 13123 */
TOK_DECL(TOK_FLOAT,      -1, 0, NULL, -1, NULL); /* 13123.0 | .123 */

/* Binary and unary operators */
TOK_DECL(TOK_LOGOR,       1, 0, &AST_CLS_LOGOR,  -1, NULL);            /* or */
TOK_DECL(TOK_LOGAND,      2, 0, &AST_CLS_LOGAND, -1, NULL);            /* and */
TOK_DECL(TOK_LOGNOT,     -1, 0, NULL,             3, &AST_CLS_LOGNOT); /* not */
TOK_DECL(TOK_EQ,          4, 0, &AST_CLS_EQ,     -1, NULL);            /* == */
TOK_DECL(TOK_NEQ,         4, 0, &AST_CLS_NEQ,    -1, NULL);            /* != */
TOK_DECL(TOK_GT,          5, 0, &AST_CLS_GT,     -1, NULL);            /* > */
TOK_DECL(TOK_GEQ,         5, 0, &AST_CLS_GEQ,    -1, NULL);            /* >= */
TOK_DECL(TOK_LT,          5, 0, &AST_CLS_LT,     -1, NULL);            /* < */
TOK_DECL(TOK_LEQ,         5, 0, &AST_CLS_LEQ,    -1, NULL);            /* <= */
TOK_DECL(TOK_BITOR,       6, 0, &AST_CLS_BITOR,  -1, NULL);            /* | */
TOK_DECL(TOK_BITAND,      7, 0, &AST_CLS_BITAND, -1, NULL);            /* & */
TOK_DECL(TOK_ADD,         8, 0, &AST_CLS_ADD,    -1, NULL);            /* + */
TOK_DECL(TOK_SUB,         8, 0, &AST_CLS_SUB,    10, &AST_CLS_NEG);    /* - */
TOK_DECL(TOK_MUL,         9, 0, &AST_CLS_MUL,    -1, NULL);            /* * */
TOK_DECL(TOK_DIV,         9, 0, &AST_CLS_DIV,    -1, NULL);            /* / */
TOK_DECL(TOK_MOD,         9, 0, &AST_CLS_MOD,    -1, NULL);            /* % */
TOK_DECL(TOK_EXP,        11, 1, &AST_CLS_EXP,    -1, NULL);            /* ^ */

/* String */
TOK_DECL(TOK_STRING,     -1, 0, NULL, -1, NULL); /* "afasfasf" */

/* Keywords */
TOK_DECL(TOK_NULL,       -1, 0, NULL, -1, NULL); /* null */
TOK_DECL(TOK_TRUE,       -1, 0, NULL, -1, NULL); /* true */
TOK_DECL(TOK_FALSE,      -1, 0, NULL, -1, NULL); /* false */
TOK_DECL(TOK_RANGE,      -1, 0, NULL, -1, NULL); /* range */
TOK_DECL(TOK_FUNC,       -1, 0, NULL, -1, NULL); /* func */
TOK_DECL(TOK_CALL,       -1, 0, NULL, -1, NULL); /* call */
TOK_DECL(TOK_DEFINE,     -1, 0, NULL, -1, NULL); /* define */
TOK_DECL(TOK_ACCESS,     -1, 0, NULL, -1, NULL); /* access */
TOK_DECL(TOK_MAP,        -1, 0, NULL, -1, NULL); /* map */
TOK_DECL(TOK_FORMAT,     -1, 0, NULL, -1, NULL); /* format */
TOK_DECL(TOK_IDENTIFIER, -1, 0, NULL, -1, NULL); /* afasfasf - anything not a keyword */
TOK_DECL(TOK_IF,         -1, 0, NULL, -1, NULL); /* if */

/* Symbols */
TOK_DECL(TOK_COMMA,      -1, 0, NULL, -1, NULL); /* , */
TOK_DECL(TOK_LBRACE,     -1, 0, NULL, -1, NULL); /* { */
TOK_DECL(TOK_RBRACE,     -1, 0, NULL, -1, NULL); /* } */
TOK_DECL(TOK_LPAREN,     -1, 0, NULL, -1, NULL); /* ( */
TOK_DECL(TOK_RPAREN,     -1, 0, NULL, -1, NULL); /* ) */
TOK_DECL(TOK_LSQBR,      -1, 0, NULL, -1, NULL); /* [ */
TOK_DECL(TOK_RSQBR,      -1, 0, NULL, -1, NULL); /* ] */
TOK_DECL(TOK_ASSIGN,     -1, 0, NULL, -1, NULL); /* = */
TOK_DECL(TOK_COLON,      -1, 0, NULL, -1, NULL); /* : */
TOK_DECL(TOK_SEMI,       -1, 0, NULL, -1, NULL); /* ; */

struct tokeniser {
	uint_fast32_t line_nb;
	const char   *p_line_start;

	const char   *buf;

	struct token  *p_current;
	struct token  *p_next;

	struct token  curx;
	struct token  nextx;
};

static void token_print(const struct token *p_token) {
	if (p_token->cls == &TOK_IDENTIFIER || p_token->cls == &TOK_STRING) {
		printf("%s:'%s'\n", p_token->cls->name, p_token->t.strident.str);
	} else if (p_token->cls == &TOK_FLOAT) {
		printf("%s:%f\n", p_token->cls->name, p_token->t.tflt);
	} else if (p_token->cls == &TOK_INT) {
		printf("%s:%lld\n", p_token->cls->name, p_token->t.tint);
	} else {
		printf("%s\n", p_token->cls->name);
	}
}

/* init: load something into next */
/* peek: return p_next */
/* next: load next thing into p_current, swap p_current and p_next, return p_current */

const struct token *tok_peek(const struct tokeniser *p_tokeniser) {
	return p_tokeniser->p_next;
}

void tok_get_nearest_location(const struct tokeniser *p_tokeniser, struct token_pos_info *p_tpi) {
	*p_tpi = (p_tokeniser->p_next != NULL) ? p_tokeniser->p_next->posinfo : p_tokeniser->p_current->posinfo;
}

const struct token *tok_read(struct tokeniser *p_tokeniser, const struct ejson_error_handler *p_error_handler) {
	struct token *p_temp;
	char c, nc;
	int in_comment;

	/* eat whitespace and comments */
	c          = *(p_tokeniser->buf++);
	in_comment = (c == '#');
	while (c == ' ' || c == '\t' || c == '\r' || c == '\n' || in_comment) {
		nc = *(p_tokeniser->buf++);
		if (c == '\r' || c == '\n') {
			in_comment = 0;
			p_tokeniser->p_line_start = p_tokeniser->buf - 1;
			p_tokeniser->line_nb++;
			if (c == '\r' && nc == '\n') { /* windows style */
				nc                        = *(p_tokeniser->buf++);
				p_tokeniser->p_line_start = p_tokeniser->buf - 1;
			}
		}
		if (nc == '#') {
			in_comment = 1;
		}
		c = nc;
	}

	/* nothing left */
	if (c == '\0') {
		p_tokeniser->buf--;
		if (p_tokeniser->p_next != NULL) {
			p_tokeniser->p_current = p_tokeniser->p_next;
			p_tokeniser->p_next = NULL;
			return p_tokeniser->p_current;
		}
		return ejson_location_error_null(p_error_handler, &(p_tokeniser->p_current->posinfo), "expected another token\n");
	}

	nc = *(p_tokeniser->buf);

	assert(c != '#');

	p_temp                   = p_tokeniser->p_current;
	p_temp->posinfo.p_line   = p_tokeniser->p_line_start;
	p_temp->posinfo.char_pos = p_tokeniser->buf - p_tokeniser->p_line_start;
	p_temp->posinfo.line_nb  = p_tokeniser->line_nb;

	/* quoted string */
	if (c == '\"') {
		p_temp->cls            = &TOK_STRING;
		p_temp->t.strident.len = 0;
		while ((c = *(p_tokeniser->buf++)) != '\"') {
			if (c == '\0')
				return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "unterminated string\n");

			if (c == '\n' || c == '\r')
				return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "newline encountered in string\n");

			if (c == '\\') {
				c = *(p_tokeniser->buf++);
				if (c == '\\')
					c = '\\';
				else if (c == '\"')
					c = '\"';
				else if (c == '/')
					c = '/';
				else if (c == 'b')
					c = '\b';
				else if (c == 'f')
					c = '\f';
				else if (c == 'n')
					c = '\n';
				else if (c == 'r')
					c = '\r';
				else if (c == 't')
					c = '\t';
				else if (c == 'u') {
					unsigned h;
					if  (   expect_hex_digit(&(p_tokeniser->buf), &h)
					    ||  expect_hex_digit_accumulate(&(p_tokeniser->buf), &h)
					    ||  expect_hex_digit_accumulate(&(p_tokeniser->buf), &h)
					    ||  expect_hex_digit_accumulate(&(p_tokeniser->buf), &h)
					    )
						return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "invalid json codepoint escape sequence\n");
					/* Convert to UTF-8 now. */
					return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "do not support json codepoint escape sequences\n");
				} else
					return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "invalid json codepoint escape sequence\n");
			}

			p_temp->t.strident.str[p_temp->t.strident.len++] = c;
		}
		p_temp->t.strident.str[p_temp->t.strident.len] = '\0';
	} else if
	    (   (c == '.' && p_tokeniser->buf[0] >= '0' && p_tokeniser->buf[0] <= '9')
	    ||  (c >= '0' && c <= '9')
	    ) {
		unsigned long long ull = 0;
		int ishex;
		p_temp->cls = &TOK_INT;
		ishex = c == '0' && p_tokeniser->buf[0] == 'x';
		if (ishex) {
			unsigned uu;
			++p_tokeniser->buf;
			if (expect_hex_digit(&(p_tokeniser->buf), &uu))
				return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "invalid extended json numeric\n");
			do {
				ull *= 16;
				ull += uu;
			} while (!expect_hex_digit(&(p_tokeniser->buf), &uu));
			p_temp->t.tint = ull;
		} else {
			while (c >= '0' && c <= '9') {
				ull *= 10;
				ull += c - '0';
				c    = *(p_tokeniser->buf++);
			}
			if (c == '.') {
				double frac = 0.1;
				p_temp->cls    = &TOK_FLOAT;
				p_temp->t.tflt = ull;
				c = *(p_tokeniser->buf++);
				if (c < '0' || c > '9')
					return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "invalid json numeric\n");
				while (c >= '0' && c <= '9') {
					p_temp->t.tflt += (c - '0') * frac;
					frac *= 0.1;
					c = *(p_tokeniser->buf++);
				}
			}
			if (c == 'e' || c == 'E') {
				int eneg = 0;
				int eval;
				c = *(p_tokeniser->buf++);
				if (c == '-') {
					eneg = 1;
					c = *(p_tokeniser->buf++);
				} else if (c == '+') {
					c = *(p_tokeniser->buf++);
				}
				if (c < '0' || c > '9')
					return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "invalid json numeric\n");
				eval = c - '0';
				c = *(p_tokeniser->buf++);
				while (c >= '0' && c <= '9') {
					eval = eval * 10 + (c - '0');
					c = *(p_tokeniser->buf++);
				}
				eval = (eneg) ? -eval : eval;
				if (p_temp->cls == &TOK_FLOAT) {
					p_temp->t.tflt *= pow(10.0, eval);
				} else {
					p_temp->cls = &TOK_FLOAT;
					p_temp->t.tflt = ull * pow(10.0, eval);
				}
			}
			if (p_temp->cls == &TOK_INT) {
				p_temp->t.tint = ull;
			}
			p_tokeniser->buf--;
		}
	} else if
	    (   (c >= 'a' && c <= 'z')
	    ||  (c >= 'A' && c <= 'Z')
	    ) {
		char nc = *(p_tokeniser->buf);
		p_temp->t.strident.str[0] = c;
		p_temp->t.strident.len = 1;
		while
		    (   (nc >= 'a' && nc <= 'z')
		    ||  (nc >= 'A' && nc <= 'Z')
		    ||  (nc >= '0' && nc <= '9')
		    ||  (nc == '_')
		    ) {
			p_temp->t.strident.str[p_temp->t.strident.len++] = nc;
			nc = *(++p_tokeniser->buf);
		}
		p_temp->t.strident.str[p_temp->t.strident.len] = '\0';

		if (!strcmp(p_temp->t.strident.str, "true")) { p_temp->cls = &TOK_TRUE;
		} else if (!strcmp(p_temp->t.strident.str, "false")) { p_temp->cls = &TOK_FALSE;
		} else if (!strcmp(p_temp->t.strident.str, "null")) { p_temp->cls = &TOK_NULL;
		} else if (!strcmp(p_temp->t.strident.str, "range")) { p_temp->cls = &TOK_RANGE;
		} else if (!strcmp(p_temp->t.strident.str, "call")) { p_temp->cls = &TOK_CALL;
		} else if (!strcmp(p_temp->t.strident.str, "func")) { p_temp->cls = &TOK_FUNC;
		} else if (!strcmp(p_temp->t.strident.str, "define")) { p_temp->cls = &TOK_DEFINE;
		} else if (!strcmp(p_temp->t.strident.str, "access")) { p_temp->cls = &TOK_ACCESS;
		} else if (!strcmp(p_temp->t.strident.str, "map")) { p_temp->cls = &TOK_MAP;
		} else if (!strcmp(p_temp->t.strident.str, "format")) { p_temp->cls = &TOK_FORMAT;
		} else if (!strcmp(p_temp->t.strident.str, "and")) { p_temp->cls = &TOK_LOGAND;
		} else if (!strcmp(p_temp->t.strident.str, "or")) { p_temp->cls = &TOK_LOGOR;
		} else if (!strcmp(p_temp->t.strident.str, "not")) { p_temp->cls = &TOK_LOGNOT;
		} else if (!strcmp(p_temp->t.strident.str, "if")) { p_temp->cls = &TOK_IF;
		} else { p_temp->cls = &TOK_IDENTIFIER; }

	} else if (c == '!' && nc == '=') {
		p_tokeniser->buf++;
		p_temp->cls = &TOK_NEQ;
	} else if (c == '=') {
		if (nc == '=') {
			p_temp->cls = &TOK_EQ;
			p_tokeniser->buf++;
		} else {
			p_temp->cls = &TOK_ASSIGN;
		}
	} else if (c == '>') {
		if (nc == '=') {
			p_temp->cls = &TOK_GEQ;
			p_tokeniser->buf++;
		} else {
			p_temp->cls = &TOK_GT;
		}
	} else if (c == '<') {
		if (nc == '=') {
			p_temp->cls = &TOK_LEQ;
			p_tokeniser->buf++;
		} else {
			p_temp->cls = &TOK_LT;
		}
	} else if (c == '[') { p_temp->cls = &TOK_LSQBR;
	} else if (c == ']') { p_temp->cls = &TOK_RSQBR;
	} else if (c == '{') { p_temp->cls = &TOK_LBRACE;
	} else if (c == '}') { p_temp->cls = &TOK_RBRACE;
	} else if (c == '(') { p_temp->cls = &TOK_LPAREN;
	} else if (c == ')') { p_temp->cls = &TOK_RPAREN;
	} else if (c == ',') { p_temp->cls = &TOK_COMMA;
	} else if (c == ':') { p_temp->cls = &TOK_COLON;
	} else if (c == ';') { p_temp->cls = &TOK_SEMI;
	} else if (c == '%') { p_temp->cls = &TOK_MOD;
	} else if (c == '/') { p_temp->cls = &TOK_DIV;
	} else if (c == '*') { p_temp->cls = &TOK_MUL;
	} else if (c == '^') { p_temp->cls = &TOK_EXP;
	} else if (c == '-') { p_temp->cls = &TOK_SUB;
	} else if (c == '+') { p_temp->cls = &TOK_ADD;
	} else if (c == '|') { p_temp->cls = &TOK_BITOR;
	} else if (c == '&') { p_temp->cls = &TOK_BITAND;
	} else {
		return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "invalid token\n");
	}

	p_temp                 = p_tokeniser->p_next;
	p_tokeniser->p_next    = p_tokeniser->p_current;
	p_tokeniser->p_current = p_temp;

	return p_temp;
}

static int tokeniser_start(struct tokeniser *p_tokeniser, const char *buf) {
	p_tokeniser->line_nb                  = 1;
	p_tokeniser->p_line_start             = buf;
	p_tokeniser->buf                      = buf;
	p_tokeniser->p_current                = &(p_tokeniser->curx);
	p_tokeniser->p_next                   = &(p_tokeniser->nextx);
	p_tokeniser->p_next->posinfo.p_line   = buf;
	p_tokeniser->p_next->posinfo.char_pos = 0;
	p_tokeniser->p_next->posinfo.line_nb  = 1;
	return (tok_read(p_tokeniser, NULL) == NULL) ? 1 : 0;
}


void evaluation_context_init(struct evaluation_context *p_ctx, struct cop_salloc_iface *p_alloc) {
	p_ctx->p_alloc     = p_alloc;
	p_ctx->stack_depth = 0;
	p_ctx->p_workspace = cop_strdict_init();
}

const struct ast_node *expect_expression(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, unsigned min_prec, const struct ejson_error_handler *p_error_handler);

const struct ast_node *parse_primary(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, const struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_temp_nodes[128];
	struct ast_node *p_ret = NULL;
	const struct token *p_token;

	if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
		return NULL;

	if (p_token->cls == &TOK_LPAREN) {
		const struct ast_node *p_subexpr;
		if ((p_subexpr = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		if (p_token->cls != &TOK_RPAREN)
			return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected close parenthesis\n");
		return p_subexpr;
	}

	if (p_token->cls == &TOK_IDENTIFIER) {
		const struct ast_node *node;
		if (cop_strdict_get_by_cstr(p_workspace->p_workspace, p_token->t.strident.str, (void **)&node))
			return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "'%s' was not found in the workspace\n", p_token->t.strident.str);
		assert(node != NULL);

		/* if the node is not a stack reference, it is definitely a define'ed workspace expression. */
		if (node->cls != &AST_CLS_STACKREF)
			return node;

		/* otherwise, the node is absolutely a reference to a function argument which needs to be adjusted based on the current stack position. */
		if ((p_ret = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");

		p_ret->doc_pos = p_token->posinfo;
		p_ret->cls     = &AST_CLS_STACKREF;
		p_ret->d.i     = 1 + p_workspace->stack_depth - node->d.i;
		return p_ret;
	}


	if ((p_ret = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node), 0)) == NULL)
		return ejson_error_null(p_error_handler, "out of memory\n");
	p_ret->doc_pos = p_token->posinfo;

	if (p_token->cls->unary_op_cls != NULL) {
		p_ret->cls           = p_token->cls->unary_op_cls;
		if ((p_ret->d.binop.p_lhs = expect_expression(p_workspace, p_tokeniser, p_token->cls->unary_precedence, p_error_handler)) == NULL)
			return NULL;
		p_ret->d.binop.p_rhs = NULL;
		return p_ret;
	}

	if (p_token->cls == &TOK_ACCESS) {
		p_ret->cls        = &AST_CLS_ACCESS;
		if ((p_ret->d.access.p_data = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.access.p_key = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_MAP) {
		p_ret->cls        = &AST_CLS_MAP;
		if ((p_ret->d.map.p_function = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.map.p_input_list = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_IF) {
		p_ret->cls = &AST_CLS_IF;
		if ((p_ret->d.ifexpr.p_test = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.ifexpr.p_true = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.ifexpr.p_false = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_INT) {
		p_ret->cls        = &AST_CLS_LITERAL_INT;
		p_ret->d.i        = p_token->t.tint;
	} else if (p_token->cls == &TOK_FLOAT) {
		p_ret->cls        = &AST_CLS_LITERAL_FLOAT;
		p_ret->d.f        = p_token->t.tflt;
	} else if (p_token->cls == &TOK_STRING) {
		size_t sl = strlen(p_token->t.strident.str);
		char *p_strbuf;
		if ((p_strbuf = cop_salloc(p_workspace->p_alloc, sl + 1, 1)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		memcpy(p_strbuf, p_token->t.strident.str, sl + 1);
		p_ret->cls          = &AST_CLS_LITERAL_STRING;
		p_ret->d.str.len    = sl;
		p_ret->d.str.p_data = p_strbuf;
	} else if (p_token->cls == &TOK_LBRACE) {
		uint_fast32_t    nb_kvs = 0;
		const struct token *p_next;
		if ((p_next = tok_peek(p_tokeniser)) == NULL) {
			struct token_pos_info tpi;
			tok_get_nearest_location(p_tokeniser, &tpi);
			return ejson_location_error_null(p_error_handler, &tpi, "a dict expression must be terminated\n");
		}
		if (p_next->cls != &TOK_RBRACE) {
			do {
				if ((p_temp_nodes[2*nb_kvs+0] = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
					return NULL;
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if (p_token->cls != &TOK_COLON)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a :\n");
				if ((p_temp_nodes[2*nb_kvs+1] = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
					return NULL;
				nb_kvs++;
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if (p_token->cls != &TOK_RBRACE && p_token->cls != &TOK_COMMA)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a , or }\n");
			} while (p_token->cls != &TOK_RBRACE);
		} else {
			p_token = tok_read(p_tokeniser, p_error_handler);
			assert(p_token != NULL && "peeked a valid token but could not skip over it");
		}
		p_ret->cls             = &AST_CLS_LITERAL_DICT;
		p_ret->d.ldict.nb_keys = nb_kvs;
		if (nb_kvs) {
			if ((p_ret->d.ldict.elements = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node *) * nb_kvs * 2, 0 )) == NULL)
				return ejson_error_null(p_error_handler, "out of memory\n");
			memcpy(p_ret->d.ldict.elements, p_temp_nodes, sizeof(struct ast_node *) * nb_kvs * 2);
		} else {
			p_ret->d.ldict.elements = NULL;
		}
	} else if (p_token->cls == &TOK_LSQBR) {
		uint_fast32_t nb_list = 0;
		const struct token *p_next;
		if ((p_next = tok_peek(p_tokeniser)) == NULL) {
			struct token_pos_info tpi;
			tok_get_nearest_location(p_tokeniser, &tpi);
			return ejson_location_error_null(p_error_handler, &tpi, "a list expression must be terminated\n");
		}
		if (p_next->cls != &TOK_RSQBR) {
			do {
				if ((p_temp_nodes[nb_list] = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
					return NULL;
				nb_list++;
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if (p_token->cls != &TOK_RSQBR && p_token->cls != &TOK_COMMA)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected either a , or ]\n");
			} while (p_token->cls != &TOK_RSQBR);
		} else {
			p_token = tok_read(p_tokeniser, p_error_handler);
			assert(p_token != NULL && "peeked a valid token but could not skip over it");
		}
		p_ret->cls        = &AST_CLS_LITERAL_LIST;
		p_ret->d.llist.nb_elements = nb_list;
		if (nb_list) {
			if ((p_ret->d.llist.elements = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node *) * nb_list, 0)) == NULL)
				return ejson_error_null(p_error_handler, "out of memory\n");
			memcpy(p_ret->d.llist.elements, p_temp_nodes, sizeof(struct ast_node *) * nb_list);
		} else {
			p_ret->d.llist.elements = NULL;
		}
	} else if (p_token->cls == &TOK_NULL) {
		p_ret->cls   = &AST_CLS_LITERAL_NULL;
	} else if (p_token->cls == &TOK_TRUE) {
		p_ret->cls = &AST_CLS_LITERAL_BOOL;
		p_ret->d.i = 1;
	} else if (p_token->cls == &TOK_FALSE) {
		p_ret->cls = &AST_CLS_LITERAL_BOOL;
		p_ret->d.i = 0;
	} else if (p_token->cls == &TOK_RANGE) {
		p_ret->cls   = &AST_CLS_RANGE;
		if ((p_ret->d.builtin.p_args = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_FORMAT) {
		p_ret->cls   = &AST_CLS_FORMAT;
		if ((p_ret->d.builtin.p_args = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_FUNC) {
		unsigned        nb_args = 0;
		unsigned        i;
		struct cop_strh argnames[64];
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		if (p_token->cls != &TOK_LSQBR)
			return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a [\n");
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return NULL;

		if (p_token->cls != &TOK_RSQBR) {
			do {
				struct token_pos_info identpos;
				struct ast_node *p_arg;
				struct cop_strdict_node *p_wsnode;
				identpos = p_token->posinfo;
				if (p_token->cls != &TOK_IDENTIFIER)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a parameter name literal but got a %s token\n", p_token->cls->name);
				cop_strh_init_shallow(&(argnames[nb_args]), p_token->t.strident.str);
				if ((p_arg = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node), 0)) == NULL)
					return ejson_error_null(p_error_handler, "out of memory\n");
				/* todo, this memory will be used forever. could go on stack with aalloc(). */
				if ((p_wsnode = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node) + argnames[nb_args].len + 1, 0)) == NULL)
					return ejson_error_null(p_error_handler, "out of memory\n");
				memcpy((char *)(p_wsnode + 1), p_token->t.strident.str, argnames[nb_args].len + 1);
				argnames[nb_args].ptr = (unsigned char *)(p_wsnode + 1);
				cop_strdict_node_init(p_wsnode, &(argnames[nb_args]), p_arg);
				if (cop_strdict_insert(&(p_workspace->p_workspace), p_wsnode))
					return ejson_location_error_null(p_error_handler, &identpos, "function parameter names may only appear once and must not alias workspace variables\n");
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if (p_token->cls != &TOK_COMMA && p_token->cls != &TOK_RSQBR)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a , or ]\n");
				p_arg->cls = &AST_CLS_STACKREF;
				p_arg->d.i = p_workspace->stack_depth + nb_args + 1;
				nb_args++;
				if (p_token->cls == &TOK_RSQBR)
					break;
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
			} while (1);
		}

		p_ret->cls                              = &AST_CLS_FUNCTION;
		p_ret->d.fn.nb_args                     = nb_args;

		p_workspace->stack_depth += nb_args;
		if ((p_ret->d.fn.node = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
		p_workspace->stack_depth -= nb_args;

		for (i = 0; i < nb_args; i++) {
			if (cop_strdict_delete(&(p_workspace->p_workspace), &(argnames[i])) == NULL) {
				fprintf(stderr, "ICE\n");
				abort();
			}
		}
	} else if (p_token->cls == &TOK_CALL) {
		p_ret->cls        = &AST_CLS_CALL;
		if ((p_ret->d.call.fn = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.call.p_args  = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return NULL;
	} else {
		token_print(p_token);
		abort();
	}

	assert(p_ret != NULL);
	return p_ret;
}

const struct ast_node *expect_expression(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, unsigned min_prec, const struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_lhs;
	const struct token *p_token;

	p_lhs = parse_primary(p_workspace, p_tokeniser, p_error_handler);
	if (p_lhs == NULL)
		return NULL;

	while
		(   (p_token = tok_peek(p_tokeniser)) != NULL
		&&  p_token->cls->bin_op_cls != NULL
		&&  p_token->cls->binary_precedence >= min_prec
		) {
		const struct ast_node *p_rhs;
		struct ast_node       *p_comb;
		struct token_pos_info  loc_info = p_token->posinfo;
		const struct tok_def  *p_cls    = p_token->cls;
		unsigned               q        = (p_cls->right_associative) ? p_token->cls->binary_precedence : (p_token->cls->binary_precedence + 1);

		if (tok_read(p_tokeniser, p_error_handler) == NULL)
			abort();

		if ((p_rhs = expect_expression(p_workspace, p_tokeniser, q, p_error_handler)) == NULL)
			return NULL;

		if ((p_comb = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");

		p_comb->cls           = p_cls->bin_op_cls;
		p_comb->doc_pos       = loc_info;
		p_comb->d.binop.p_lhs = p_lhs;
		p_comb->d.binop.p_rhs = p_rhs;
		p_lhs = p_comb;
	}

	return p_lhs;
}

struct jnode_data {
	struct ast_node *p_root;
	struct ast_node *p_stack;
	unsigned         nb_stack;

};

static int to_jnode(struct jnode *p_node, const struct ev_ast_node *p_src, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler);

struct list_element_fn_data {
	struct p_error_handler  *p_error_handler;
	struct ast_node        **pp_stack;
	unsigned                 stack_size;
	unsigned                 nb_elements;
	struct ast_node        **pp_elements;

};

struct execution_context {
	struct ev_ast_node                object;
	const struct ejson_error_handler *p_error_handler;

};

static int evaluate_ast(struct ev_ast_node *p_dest, const struct ast_node *p_src, const struct ev_ast_node **pp_stackx, unsigned stack_sizex, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler);

struct lrange {
	long long first;
	long long step_size;
	long long numel;

};

static int ast_list_generator_get_element(struct ev_ast_node *p_ret, const struct ev_ast_node *p_list, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	struct ast_node *p_dest;
	assert(p_list->p_node->cls == &AST_CLS_LIST_GENERATOR);
	if (element >= p_list->p_node->d.lgen.nb_elements)
		return ejson_error(p_error_handler, "list index out of range\n");
	if ((p_dest = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
		return ejson_error(p_error_handler, "out of memory\n");
	p_dest->doc_pos   = p_list->p_node->doc_pos;
	p_dest->cls       = &AST_CLS_LITERAL_INT;
	p_dest->d.i       = p_list->p_node->d.lgen.d.range.first + p_list->p_node->d.lgen.d.range.step * (long long)element;
	p_ret->p_node     = p_dest;
	p_ret->pp_stack   = NULL;
	p_ret->stack_size = 0;
	return 0;
}

static int get_literal_element_fn(struct ev_ast_node *p_dest, const struct ev_ast_node *p_src, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	assert(p_src->p_node->cls == &AST_CLS_LIST_GENERATOR);
	if (element >= p_src->p_node->d.lgen.nb_elements)
		return ejson_error(p_error_handler, "list index out of bounds\n");
	return evaluate_ast(p_dest, p_src->p_node->d.lgen.d.literal.pp_values[element], p_src->pp_stack, p_src->stack_size, p_alloc, p_error_handler);
}

static int get_list_cat(struct ev_ast_node *p_dest, const struct ev_ast_node *p_src, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	struct ev_ast_node f;
	struct ev_ast_node s;
	assert(p_src->p_node->cls == &AST_CLS_LIST_GENERATOR);
	f.p_node = p_src->p_node->d.lgen.d.cat.p_first;
	s.p_node = p_src->p_node->d.lgen.d.cat.p_second;
	assert(f.p_node->cls == &AST_CLS_LIST_GENERATOR);
	assert(s.p_node->cls == &AST_CLS_LIST_GENERATOR);
	if (element < f.p_node->d.lgen.nb_elements)
		return f.p_node->d.lgen.get_element(p_dest, &f, element, p_alloc, p_error_handler);
	element -= f.p_node->d.lgen.nb_elements;
	return s.p_node->d.lgen.get_element(p_dest, &s, element, p_alloc, p_error_handler);
}

static int ast_list_generator_map(struct ev_ast_node *p_dest, const struct ev_ast_node *p_src, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	struct ev_ast_node *p_argument;
	const struct ev_ast_node **pp_tmp;
	const struct ev_ast_node *p_function;
	const struct ev_ast_node *p_list;

	assert(p_src->p_node->cls == &AST_CLS_LIST_GENERATOR);
	p_function  = p_src->p_node->d.lgen.d.map.p_function;
	p_list      = p_src->p_node->d.lgen.d.map.p_key;
	assert(p_function->p_node->cls == &AST_CLS_FUNCTION);
	assert(p_list->p_node->cls == &AST_CLS_LIST_GENERATOR);

	if (element >= p_list->p_node->d.lgen.nb_elements)
		return ejson_error(p_error_handler, "list index out of range\n");
	if  (   (pp_tmp     = cop_salloc(p_alloc, sizeof(struct ev_ast_node *) * (p_function->stack_size + 1), 0)) == NULL
	    ||  (p_argument = cop_salloc(p_alloc, sizeof(struct ev_ast_node), 0)) == NULL
	    )
		return ejson_error(p_error_handler, "out of memory\n");

	/* Evaluate argument, shove on stack, execute function. */
	if (p_list->p_node->d.lgen.get_element(p_argument, p_list, element, p_alloc, p_error_handler))
		return -1;

	if (p_function->stack_size)
		memcpy(pp_tmp, p_function->pp_stack, p_function->stack_size * sizeof(struct ev_ast_node *));
	pp_tmp[p_function->stack_size] = p_argument;

	return evaluate_ast(p_dest, p_function->p_node->d.fn.node, pp_tmp, p_function->stack_size + 1, p_alloc, p_error_handler);
}

static int evaluate_ast(struct ev_ast_node *p_dest, const struct ast_node *p_src, const struct ev_ast_node **pp_stackx, unsigned stack_sizex, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	/* Move through stack references. */
	assert(p_src != NULL);
	assert(p_src->cls != NULL);
	assert(p_src->cls->p_name != NULL);

	if (p_src->cls == &AST_CLS_STACKREF) {
		assert(pp_stackx != NULL);
		assert(p_src->d.i > 0 && p_src->d.i <= stack_sizex);
		*p_dest = pp_stackx[stack_sizex - p_src->d.i][0];
		return 0;
	}	

	/* Shortcuts for fully simplified objects. */
	if  (   p_src->cls == &AST_CLS_LITERAL_INT
	    ||  p_src->cls == &AST_CLS_LITERAL_BOOL
	    ||  p_src->cls == &AST_CLS_LITERAL_STRING
	    ||  p_src->cls == &AST_CLS_LITERAL_FLOAT
	    ||  p_src->cls == &AST_CLS_LITERAL_NULL
	    ||  p_src->cls == &AST_CLS_LIST_GENERATOR
	    ||  p_src->cls == &AST_CLS_READY_DICT
	    ) {
		p_dest->stack_size = stack_sizex;
		p_dest->pp_stack   = pp_stackx;
		p_dest->p_node     = p_src;
		return 0;
	}

	if (p_src->cls == &AST_CLS_FUNCTION) {
		struct ast_node *p_ret;
		//FIXME??? WHY CAN THIS NOT BE ASSERTED?  assert(p_src->d.fn.pp_stack == NULL);
		if (stack_sizex == 0) {
			p_dest->stack_size = stack_sizex;
			p_dest->pp_stack   = pp_stackx;
			p_dest->p_node     = p_src;
			return 0;
		}
		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		p_ret->cls          = p_src->cls;
		p_ret->doc_pos      = p_src->doc_pos;
		p_ret->d.fn.nb_args = p_src->d.fn.nb_args;
		p_ret->d.fn.node    = p_src->d.fn.node;
		p_dest->stack_size  = stack_sizex;
		p_dest->pp_stack    = pp_stackx;
		p_dest->p_node      = p_ret;
		return 0;
	}

	if  (p_src->cls == &AST_CLS_IF) {
		struct ev_ast_node test;
		if (evaluate_ast(&test, p_src->d.ifexpr.p_test, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		if (test.p_node->cls != &AST_CLS_LITERAL_BOOL)
			return ejson_location_error(p_error_handler, &(p_src->d.ifexpr.p_test->doc_pos), "first argument to if must be a boolean\n");
		if (test.p_node->d.i)
			return evaluate_ast(p_dest, p_src->d.ifexpr.p_true, pp_stackx, stack_sizex, p_alloc, p_error_handler);
		return evaluate_ast(p_dest, p_src->d.ifexpr.p_false, pp_stackx, stack_sizex, p_alloc, p_error_handler);
	}

	/* Convert lists into list generators - fixme: this should happen while building the ast. */
	if  (p_src->cls == &AST_CLS_LITERAL_LIST) {
		struct ast_node *p_ret;
		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		p_ret->cls                        = &AST_CLS_LIST_GENERATOR;
		p_ret->doc_pos                    = p_src->doc_pos;
		p_ret->d.lgen.nb_elements         = p_src->d.llist.nb_elements;
		p_ret->d.lgen.d.literal.pp_values = p_src->d.llist.elements;
		p_ret->d.lgen.get_element         = get_literal_element_fn;
		p_dest->stack_size                = stack_sizex;
		p_dest->pp_stack                  = pp_stackx;
		p_dest->p_node                    = p_ret;
		return 0;
	}

	if (p_src->cls == &AST_CLS_FORMAT) {
		char strbuf[8192];
		unsigned i;
		unsigned argidx;
		const struct ast_node *p_fmtstr;
		struct ast_node *p_ret;
		const char *cp;
		char *ob;
		char c;
		struct ev_ast_node n;
		struct ev_ast_node args;

		if (evaluate_ast(&args, p_src->d.builtin.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		if (args.p_node->cls != &AST_CLS_LIST_GENERATOR || args.p_node->d.lgen.nb_elements < 1)
			return ejson_error(p_error_handler, "format expects a list argument with at least a format string\n");
		if (args.p_node->d.lgen.get_element(&n, &args, 0, p_alloc, p_error_handler))
			return -1;
		p_fmtstr = n.p_node;
		if (p_fmtstr->cls != &AST_CLS_LITERAL_STRING)
			return ejson_error(p_error_handler, "first argument of format must be a string\n");
		
		cp = p_fmtstr->d.str.p_data;
		i = 0;
		argidx = 1;
		while ((c = *cp++) != '\0') {
			if (c == '%') {
				char fmtspec[32];
				unsigned specpos = 1;
				fmtspec[0] = '%';

				c = *cp++;
				while (c != '\0' && c != 's' && c != 'd' && c != '%') {
					if (c > '0' && c <= '9') {
						do {
							fmtspec[specpos++] = c;
							c = *cp++;
						} while (c >= '0' && c <= '9');
						break;
					}
					if (c == '+' || c == '-' || c == '0') {
						fmtspec[specpos++] = c;
					} else {
						return ejson_error(p_error_handler, "unsupported format flag '%c'\n", c);
					}
					c = *cp++;
				}

				if (c == '%') {
					strbuf[i] = c;
					i++;
				} else if (c == 'd') {
					const struct ast_node *p_argval;
					if (argidx >= args.p_node->d.lgen.nb_elements)
						return ejson_error(p_error_handler, "not enough arguments given to format\n");
					if (args.p_node->d.lgen.get_element(&n, &args, argidx++, p_alloc, p_error_handler))
						return -1;
					p_argval = n.p_node;
					if (p_argval->cls != &AST_CLS_LITERAL_INT)
						return ejson_error(p_error_handler, "%%d expects an integer argument\n");
					fmtspec[specpos++] = 'l';
					fmtspec[specpos++] = 'l';
					fmtspec[specpos++] = 'd';
					fmtspec[specpos++] = '\0';
					i += sprintf(&(strbuf[i]), fmtspec, p_argval->d.i);
				} else if (c == 's') {
					const struct ast_node *p_argval;
					if (argidx >= args.p_node->d.lgen.nb_elements)
						return ejson_error(p_error_handler, "not enough arguments given to format\n");
					if (args.p_node->d.lgen.get_element(&n, &args, argidx++, p_alloc, p_error_handler))
						return -1;
					p_argval = n.p_node;
					if (p_argval->cls != &AST_CLS_LITERAL_STRING)
						return ejson_error(p_error_handler, "%%s expects a string argument (%s)\n", p_argval->cls->p_name);
					memcpy(&(strbuf[i]), p_argval->d.str.p_data, p_argval->d.str.len);
					i += p_argval->d.str.len;
				} else {
					return ejson_error(p_error_handler, "invalid escape sequence (%%%c)\n", c);
				}
			} else {
				strbuf[i] = c;
				i++;
			}
		}
		strbuf[i] = '\0';

		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node) + i + 1, 0)) == NULL)
			return -1;
		ob = (char *)(p_ret + 1);
		memcpy(ob, strbuf, i+1);

		p_ret->cls          = &AST_CLS_LITERAL_STRING;
		p_ret->doc_pos      = p_src->doc_pos;
		p_ret->d.str.p_data = ob;
		p_ret->d.str.len    = i;

		p_dest->p_node = p_ret;
		return 0;
	}

	if  (p_src->cls == &AST_CLS_LITERAL_DICT) {
		struct ast_node *p_ret;
		unsigned i;
		struct cop_strdict_node *p_root = cop_strdict_init();

		/* Build the dictionary */
		for (i = 0; i < p_src->d.ldict.nb_keys; i++) {
			const struct ast_node *p_key;
			struct cop_strh key;
			struct dictnode *p_dn;
			struct ev_ast_node p;

			if (evaluate_ast(&p, p_src->d.ldict.elements[2*i+0], pp_stackx, stack_sizex, p_alloc, p_error_handler))
				return -1;
			p_key = p.p_node;
			if (p_key->cls != &AST_CLS_LITERAL_STRING)
				return ejson_location_error(p_error_handler, &(p_src->doc_pos), "a key expression in the dictionary did not evaluate to a string\n");

			/* fixme: there is no need to keep another copy of the string. ideally, we would have a separate stack as we don't need to keep hold of the above node and all memory that was needed to figure it out */
			cop_strh_init_shallow(&key, p_key->d.str.p_data);
			if ((p_dn = cop_salloc(p_alloc, sizeof(struct dictnode) + key.len + 1, 0)) == NULL)
				return ejson_error(p_error_handler, "out of memory\n");
			memcpy(p_dn + 1, key.ptr, key.len + 1);
			key.ptr = (unsigned char *)(p_dn + 1);
			cop_strdict_node_init(&(p_dn->node), &key, p_dn);
			p_dn->data = p_src->d.ldict.elements[2*i+1];

			if (cop_strdict_insert(&p_root, &(p_dn->node)))
				return ejson_location_error(p_error_handler, &(p_src->d.ldict.elements[2*i+0]->doc_pos), "attempted to add a key to a dictionary that already existed (%s)\n", p_key->d.str.p_data);
		}

		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		p_ret->cls                = &AST_CLS_READY_DICT;
		p_ret->doc_pos            = p_src->doc_pos;
		p_ret->d.rdict.nb_keys    = p_src->d.ldict.nb_keys;
		p_ret->d.rdict.p_root     = p_root;
		p_dest->stack_size        = stack_sizex;
		p_dest->pp_stack          = pp_stackx;
		p_dest->p_node            = p_ret;
		return 0;
	}

	if (p_src->cls == &AST_CLS_ACCESS) {
		struct ev_ast_node obj;
		struct ev_ast_node p;

		if (evaluate_ast(&obj, p_src->d.access.p_data, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;

		if (obj.p_node->cls == &AST_CLS_LIST_GENERATOR) {
			const struct ast_node *p_idx;
			long long idx;
			size_t save = cop_salloc_save(p_alloc);

			if (evaluate_ast(&p, p_src->d.access.p_key, pp_stackx, stack_sizex, p_alloc, p_error_handler))
				return -1;
			p_idx = p.p_node;

			if (p_idx->cls != &AST_CLS_LITERAL_INT)
				return ejson_location_error(p_error_handler, &(p_src->d.access.p_key->doc_pos), "the key expression for a list access did not evaluate to an integer\n");
			idx = p_idx->d.i;
			cop_salloc_restore(p_alloc, save);

			return obj.p_node->d.lgen.get_element(p_dest, &obj, idx, p_alloc, p_error_handler);
		}
		
		if (obj.p_node->cls == &AST_CLS_READY_DICT) {
			const struct ast_node *p_key;
			struct dictnode *p_node;

			if (evaluate_ast(&p, p_src->d.access.p_key, pp_stackx, stack_sizex, p_alloc, p_error_handler))
				return -1;
			p_key = p.p_node;
			if (p_key->cls != &AST_CLS_LITERAL_STRING)
				return ejson_location_error(p_error_handler, &(p_src->d.access.p_key->doc_pos), "the key expression for dict access did not evaluate to a string\n");
			if (cop_strdict_get_by_cstr(obj.p_node->d.rdict.p_root, p_key->d.str.p_data, (void **)&p_node))
				return ejson_location_error(p_error_handler, &(p_src->d.access.p_key->doc_pos), "key '%s' not in dict\n", p_key->d.str.p_data);
			return evaluate_ast(p_dest, p_node->data, pp_stackx, stack_sizex, p_alloc, p_error_handler);
		}

		return ejson_location_error(p_error_handler, &(p_src->d.access.p_data->doc_pos), "the list expression for access did not evaluate to a list or a dictionary\n");
	}

	/* Function call */
	if (p_src->cls == &AST_CLS_CALL) {
		const struct ev_ast_node **pp_stack2;
		struct ev_ast_node args;
		struct ev_ast_node function;

		if (evaluate_ast(&function, p_src->d.call.fn, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		if (function.p_node->cls != &AST_CLS_FUNCTION)
			return ejson_error(p_error_handler, "the function expression for call did not evaluate to a function\n");

		if (evaluate_ast(&args, p_src->d.call.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		if (args.p_node->cls != &AST_CLS_LIST_GENERATOR)
			return ejson_error(p_error_handler, "the argument expression for call did not evaluate to a list\n");

		if (args.p_node->d.lgen.nb_elements != function.p_node->d.fn.nb_args)
			return ejson_location_error(p_error_handler, &(p_src->doc_pos), "the number of arguments supplied to function was incorrect (expected %u but got %u)\n", function.p_node->d.fn.nb_args, args.p_node->d.lgen.nb_elements);

		pp_stack2 = function.pp_stack;
		if (args.p_node->d.lgen.nb_elements) {
			struct ev_ast_node *p_nargs;
			unsigned i;
			if  (   (pp_stack2 = cop_salloc(p_alloc, sizeof(struct ev_ast_node *) * (function.stack_size + args.p_node->d.lgen.nb_elements), 0)) == NULL
			    ||  (p_nargs = cop_salloc(p_alloc, sizeof(struct ev_ast_node) * args.p_node->d.lgen.nb_elements, 0)) == NULL
			    )
				return ejson_error(p_error_handler, "out of memory\n");
			if (function.stack_size)
				memcpy(pp_stack2, function.pp_stack, function.stack_size * sizeof(struct ev_ast_node *));
			for (i = 0; i < args.p_node->d.lgen.nb_elements; i++) {
				if (args.p_node->d.lgen.get_element(&(p_nargs[i]), &args, i, p_alloc, p_error_handler))
					return -1;
				pp_stack2[function.stack_size + i] = &(p_nargs[i]);
			}
		}
		return evaluate_ast(p_dest, function.p_node->d.fn.node, pp_stack2, function.stack_size + args.p_node->d.lgen.nb_elements, p_alloc, p_error_handler);
	}

	/* Unary negation */
	if (p_src->cls == &AST_CLS_NEG || p_src->cls == &AST_CLS_LOGNOT) {
		const struct ast_node *p_result;
		struct ast_node       *p_tmp2;
		size_t                 save;
		struct ev_ast_node     p;

		if ((p_tmp2 = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		save = cop_salloc_save(p_alloc);


		if (evaluate_ast(&p, p_src->d.binop.p_lhs, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		p_result = p.p_node;

		if (p_src->cls == &AST_CLS_NEG) {
			if (p_result->cls == &AST_CLS_LITERAL_INT) {
				p_tmp2->cls = &AST_CLS_LITERAL_INT;
				p_tmp2->d.i = -p_result->d.i;
				cop_salloc_restore(p_alloc, save);
				p_dest->p_node = p_tmp2;
				return 0;
			}

			if (p_result->cls == &AST_CLS_LITERAL_FLOAT) {
				p_tmp2->cls = &AST_CLS_LITERAL_FLOAT;
				p_tmp2->d.f = -p_result->d.f;
				cop_salloc_restore(p_alloc, save);
				p_dest->p_node = p_tmp2;
				return 0;
			}

			return ejson_location_error(p_error_handler, &(p_src->doc_pos), "the expression for the unary negation operator did not evaluate to a numeric type\n");
		}

		if (p_result->cls == &AST_CLS_LITERAL_BOOL) {
			p_tmp2->cls = &AST_CLS_LITERAL_BOOL;
			p_tmp2->d.i = !p_result->d.i;
			cop_salloc_restore(p_alloc, save);
			p_dest->p_node = p_tmp2;
			return 0;
		}

		return ejson_location_error(p_error_handler, &(p_src->doc_pos), "the expression for the unary not operator did not evaluate to a boolean type\n");
	}

	/* Convert range into an accessor object */
	if (p_src->cls == &AST_CLS_RANGE) {
		struct ast_node       *p_result;
		size_t                 save;
		struct lrange          lrange;
		struct ev_ast_node     p;
		struct ev_ast_node     args;

		if ((p_result = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		save = cop_salloc_save(p_alloc);
		if (evaluate_ast(&args, p_src->d.builtin.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		if (args.p_node->cls != &AST_CLS_LIST_GENERATOR)
			return ejson_location_error(p_error_handler, &(p_src->doc_pos), "range expects a list argument\n");
		if (args.p_node->d.lgen.nb_elements < 1 || args.p_node->d.lgen.nb_elements > 3)
			return ejson_location_error(p_error_handler, &(p_src->doc_pos), "range expects between 1 and 3 arguments\n");

		if (args.p_node->d.lgen.nb_elements == 1) {
			const struct ast_node *p_first;

			if (args.p_node->d.lgen.get_element(&p, &args, 0, p_alloc, p_error_handler))
				return -1;
			p_first = p.p_node;
			if (p_first->cls != &AST_CLS_LITERAL_INT)
				return ejson_error(p_error_handler, "single argument range expects an integer number of items\n");

			lrange.first     = 0;
			lrange.step_size = 1;
			lrange.numel     = p_first->d.i;
		} else if (args.p_node->d.lgen.nb_elements == 2) {
			const struct ast_node *p_first, *p_last;
			struct ev_ast_node     p1, p2;

			if  (    args.p_node->d.lgen.get_element(&p1, &args, 0, p_alloc, p_error_handler)
			    ||   args.p_node->d.lgen.get_element(&p2, &args, 1, p_alloc, p_error_handler)
			    )
				return -1;
			p_first = p1.p_node;
			p_last  = p2.p_node;
			if (p_first->cls != &AST_CLS_LITERAL_INT || p_last->cls != &AST_CLS_LITERAL_INT)
				return ejson_error(p_error_handler, "dual argument range expects an integer first and last index\n");

			lrange.first     = p_first->d.i;
			lrange.step_size = (p_first->d.i > p_last->d.i) ? -1 : 1;
			lrange.numel     = ((p_first->d.i > p_last->d.i) ? (p_first->d.i - p_last->d.i) : (p_last->d.i - p_first->d.i)) + 1;
		} else {
			const struct ast_node *p_first, *p_step, *p_last;
			struct ev_ast_node     p1, p2, p3;

			if  (   args.p_node->d.lgen.get_element(&p1, &args, 0, p_alloc, p_error_handler)
			    ||  args.p_node->d.lgen.get_element(&p2, &args, 1, p_alloc, p_error_handler)
			    ||  args.p_node->d.lgen.get_element(&p3, &args, 2, p_alloc, p_error_handler)
			    )
				return -1;
			p_first = p1.p_node;
			p_step = p2.p_node;
			p_last = p3.p_node;			
			if  (   p_first->cls != &AST_CLS_LITERAL_INT || p_step->cls != &AST_CLS_LITERAL_INT ||  p_last->cls != &AST_CLS_LITERAL_INT
			    ||  p_step->d.i == 0
			    ||  (p_step->d.i > 0 && (p_first->d.i > p_last->d.i))
			    ||  (p_step->d.i < 0 && (p_first->d.i < p_last->d.i))
			    )
				return ejson_error(p_error_handler, "triple argument range expects an integer first, step and last values. step must be non-zero and have the correct sign for the range.\n");

			lrange.first     = p_first->d.i;
			lrange.step_size = p_step->d.i;
			lrange.numel     = (p_last->d.i - p_first->d.i) / p_step->d.i + 1;
		}

		cop_salloc_restore(p_alloc, save);
		
		p_result->cls                  = &AST_CLS_LIST_GENERATOR;
		p_result->doc_pos              = p_src->doc_pos;
		p_result->d.lgen.get_element   = ast_list_generator_get_element;
		p_result->d.lgen.nb_elements   = lrange.numel;
		p_result->d.lgen.d.range.first = lrange.first;
		p_result->d.lgen.d.range.step  = lrange.step_size;
		p_dest->p_node                 = p_result;
		p_dest->pp_stack               = NULL;
		p_dest->stack_size             = 0;
		return 0;
	}

	if (p_src->cls == &AST_CLS_MAP) {
		struct ast_node *p_tmp;
		struct ev_ast_node *p_function;
		struct ev_ast_node *p_list;
		
		if  (   (p_function = cop_salloc(p_alloc, sizeof(struct ev_ast_node), 0)) == NULL
		    ||  (p_list = cop_salloc(p_alloc, sizeof(struct ev_ast_node), 0)) == NULL
			||  (p_tmp = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL
		    )
			return ejson_error(p_error_handler, "out of memory\n");

		if (evaluate_ast(p_function, p_src->d.map.p_function, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		if (p_function->p_node->cls != &AST_CLS_FUNCTION || p_function->p_node->d.fn.nb_args != 1)
			return ejson_location_error(p_error_handler, &(p_src->doc_pos), "map expects a function argument that takes one argument\n");

		if (evaluate_ast(p_list, p_src->d.map.p_input_list, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		if (p_list->p_node->cls != &AST_CLS_LIST_GENERATOR)
			return ejson_location_error(p_error_handler, &(p_src->doc_pos), "map expected a list argument following the function\n");

		p_tmp->cls                     = &AST_CLS_LIST_GENERATOR;
		p_tmp->doc_pos                 = p_src->doc_pos;
		p_tmp->d.lgen.d.map.p_function = p_function;
		p_tmp->d.lgen.d.map.p_key      = p_list;
		p_tmp->d.lgen.get_element      = ast_list_generator_map;
		p_tmp->d.lgen.nb_elements      = p_list->p_node->d.lgen.nb_elements;
		p_dest->p_node                 = p_tmp;
		p_dest->pp_stack               = NULL;
		p_dest->stack_size             = 0;
		return 0;
	}

	/* Ops */
	if  (p_src->cls == &AST_CLS_ADD || p_src->cls == &AST_CLS_SUB || p_src->cls == &AST_CLS_MUL || p_src->cls == &AST_CLS_MOD
	    || p_src->cls == &AST_CLS_BITOR || p_src->cls == &AST_CLS_BITAND
	    || p_src->cls == &AST_CLS_LOGAND || p_src->cls == &AST_CLS_LOGOR
	    || p_src->cls == &AST_CLS_EQ || p_src->cls == &AST_CLS_NEQ || p_src->cls == &AST_CLS_GT || p_src->cls == &AST_CLS_GEQ || p_src->cls == &AST_CLS_LEQ || p_src->cls == &AST_CLS_LT
	    || p_src->cls == &AST_CLS_EXP
	    ) {
		const struct ast_node *p_lhs;
		const struct ast_node *p_rhs;
		struct ast_node *p_ret;
		size_t save;
		struct ev_ast_node lhs;
		struct ev_ast_node rhs;

		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		p_ret->doc_pos = p_src->doc_pos;

		save = cop_salloc_save(p_alloc);

		if (evaluate_ast(&lhs, p_src->d.binop.p_lhs, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;
		if (evaluate_ast(&rhs, p_src->d.binop.p_rhs, pp_stackx, stack_sizex, p_alloc, p_error_handler))
			return -1;

		p_lhs = lhs.p_node;
		p_rhs = rhs.p_node;

		if (p_src->cls == &AST_CLS_LOGAND || p_src->cls == &AST_CLS_LOGOR) {
			if (p_lhs->cls != &AST_CLS_LITERAL_BOOL)
				return ejson_location_error(p_error_handler, &p_src->doc_pos, "lhs of logical operator was not boolean\n");
			if (p_rhs->cls != &AST_CLS_LITERAL_BOOL)
				return ejson_location_error(p_error_handler, &p_src->doc_pos, "rhs of logical operator was not boolean\n");
			p_ret->cls = &AST_CLS_LITERAL_BOOL;
			p_ret->d.i = (p_src->cls == &AST_CLS_LOGAND) ? (p_lhs->d.i && p_rhs->d.i) : (p_lhs->d.i || p_rhs->d.i);
			p_dest->p_node = p_ret;
			return 0;
		}

		if (p_src->cls == &AST_CLS_BITAND || p_src->cls == &AST_CLS_BITOR) {
			if (p_lhs->cls != &AST_CLS_LITERAL_INT)
				return ejson_location_error(p_error_handler, &p_src->doc_pos, "lhs of bitwise operator was not integer\n");
			if (p_rhs->cls != &AST_CLS_LITERAL_INT)
				return ejson_location_error(p_error_handler, &p_src->doc_pos, "rhs of bitwise operator was not integer\n");
			p_ret->cls = &AST_CLS_LITERAL_INT;
			p_ret->d.i = (p_src->cls == &AST_CLS_BITAND) ? (p_lhs->d.i & p_rhs->d.i) : (p_lhs->d.i | p_rhs->d.i);
			p_dest->p_node = p_ret;
			return 0;
		}

		if ((p_src->cls == &AST_CLS_EQ || p_src->cls == &AST_CLS_NEQ) && (p_lhs->cls == &AST_CLS_LITERAL_BOOL || p_rhs->cls == &AST_CLS_LITERAL_BOOL)) {
			if (p_lhs->cls != &AST_CLS_LITERAL_BOOL)
				return ejson_location_error(p_error_handler, &p_src->doc_pos, "lhs must be boolean if rhs is\n");
			if (p_rhs->cls != &AST_CLS_LITERAL_BOOL)
				return ejson_location_error(p_error_handler, &p_src->doc_pos, "rhs must be boolean if lhs is\n");
			p_ret->cls = &AST_CLS_LITERAL_BOOL;
			p_ret->d.i = (p_src->cls == &AST_CLS_EQ) ? (!p_lhs->d.i == !p_rhs->d.i) : (p_lhs->d.i != p_rhs->d.i);
			p_dest->p_node = p_ret;
			return 0;
		}

		if (p_src->cls == &AST_CLS_ADD && (p_lhs->cls == &AST_CLS_LIST_GENERATOR || p_rhs->cls == &AST_CLS_LIST_GENERATOR)) {
			if (p_lhs->cls != p_rhs->cls)
				return ejson_location_error(p_error_handler, &p_src->doc_pos, "expected lhs and rhs to both be lists\n");
			p_ret->cls                   = &AST_CLS_LIST_GENERATOR;
			p_ret->d.lgen.nb_elements    = p_lhs->d.lgen.nb_elements + p_rhs->d.lgen.nb_elements;
			p_ret->d.lgen.get_element    = get_list_cat;
			p_ret->d.lgen.d.cat.p_first  = p_lhs;
			p_ret->d.lgen.d.cat.p_second = p_rhs;
			p_dest->p_node               = p_ret;
			p_dest->pp_stack             = pp_stackx;
			p_dest->stack_size           = stack_sizex;
			return 0;
		}

		/* Promote types */
		if (p_lhs->cls == &AST_CLS_LITERAL_FLOAT || p_rhs->cls == &AST_CLS_LITERAL_FLOAT || p_src->cls == &AST_CLS_EXP) {
			double lhs, rhs;

			if (p_lhs->cls == &AST_CLS_LITERAL_FLOAT)
				lhs = p_lhs->d.f;
			else if (p_lhs->cls == &AST_CLS_LITERAL_INT)
				lhs = (double)(p_lhs->d.i);
			else
				return -1;

			if (p_rhs->cls == &AST_CLS_LITERAL_FLOAT)
				rhs = p_rhs->d.f;
			else if (p_rhs->cls == &AST_CLS_LITERAL_INT)
				rhs = (double)(p_rhs->d.i);
			else
				return -1;

			cop_salloc_restore(p_alloc, save);

			if (p_src->cls == &AST_CLS_EXP) {
				p_ret->cls = &AST_CLS_LITERAL_FLOAT;
				p_ret->d.f = pow(lhs, rhs);
			} else if (p_src->cls == &AST_CLS_ADD) {
				p_ret->cls = &AST_CLS_LITERAL_FLOAT;
				p_ret->d.f = lhs + rhs;
			} else if (p_src->cls == &AST_CLS_SUB) {
				p_ret->cls = &AST_CLS_LITERAL_FLOAT;
				p_ret->d.f = lhs - rhs;
			} else if (p_src->cls == &AST_CLS_MUL) {
				p_ret->cls = &AST_CLS_LITERAL_FLOAT;
				p_ret->d.f = lhs * rhs;
			} else if (p_src->cls == &AST_CLS_MOD) {
				p_ret->cls = &AST_CLS_LITERAL_FLOAT;
				p_ret->d.f = fmod(lhs, rhs);
			} else if (p_src->cls == &AST_CLS_EQ) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs == rhs;
			} else if (p_src->cls == &AST_CLS_NEQ) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs != rhs;
			} else if (p_src->cls == &AST_CLS_LT) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs < rhs;
			} else if (p_src->cls == &AST_CLS_LEQ) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs <= rhs;
			} else if (p_src->cls == &AST_CLS_GEQ) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs >= rhs;
			} else if (p_src->cls == &AST_CLS_GT) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs > rhs;
			} else {
				abort();
			}

			p_dest->p_node = p_ret;
			return 0;
		}
		
		if (p_lhs->cls == &AST_CLS_LITERAL_INT && p_rhs->cls == &AST_CLS_LITERAL_INT) {
			long long lhs, rhs;

			lhs = p_lhs->d.i;
			rhs = p_rhs->d.i;

			cop_salloc_restore(p_alloc, save);

			if (p_src->cls == &AST_CLS_ADD) {
				p_ret->cls = &AST_CLS_LITERAL_INT;
				p_ret->d.i = lhs + rhs;
			} else if (p_src->cls == &AST_CLS_SUB) {
				p_ret->cls = &AST_CLS_LITERAL_INT;
				p_ret->d.i = lhs - rhs;
			} else if (p_src->cls == &AST_CLS_MUL) {
				p_ret->cls = &AST_CLS_LITERAL_INT;
				p_ret->d.i = lhs * rhs;
			} else if (p_src->cls == &AST_CLS_MOD) {
				p_ret->cls = &AST_CLS_LITERAL_INT;
				lhs = lhs % rhs;
				p_ret->d.i = lhs + ((lhs < 0) ? rhs : 0);
			} else if (p_src->cls == &AST_CLS_EQ) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs == rhs;
			} else if (p_src->cls == &AST_CLS_NEQ) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs != rhs;
			} else if (p_src->cls == &AST_CLS_LT) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs < rhs;
			} else if (p_src->cls == &AST_CLS_LEQ) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs <= rhs;
			} else if (p_src->cls == &AST_CLS_GEQ) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs >= rhs;
			} else if (p_src->cls == &AST_CLS_GT) {
				p_ret->cls = &AST_CLS_LITERAL_BOOL;
				p_ret->d.i = lhs > rhs;
			} else {
				abort();
			}

			p_dest->p_node = p_ret;
			return 0;
		}

		return ejson_location_error(p_error_handler, &(p_ret->doc_pos), "the types given for binary operator %s were invalid (%s, %s)\n", p_src->cls->p_name, p_lhs->cls->p_name, p_rhs->cls->p_name);
	}

	fprintf(stderr, "what?\n");
	p_src->cls->debug_print(p_src, stderr, 10);
	fprintf(stderr, "endwhat?\n");
	abort();

	return -1;
}


static int jnode_list_get_element(struct jnode *p_dest, void *ctx, struct cop_salloc_iface *p_alloc, unsigned idx) {
	struct execution_context *ec   = ctx;
	struct ev_ast_node p;

	if (ec->object.p_node->d.lgen.get_element(&p, &(ec->object), idx, p_alloc, ec->p_error_handler))
		return -1;

	return to_jnode(p_dest, &p, p_alloc, ec->p_error_handler);
}

struct enum_args {
	const struct ejson_error_handler  *p_handler;
	const struct ev_ast_node         **pp_stack;
	unsigned                           stack_size;
	struct cop_salloc_iface           *p_alloc;
	void                              *p_user_context;
	jdict_enumerate_fn                *p_fn;

};

static int enumerate_dict_keys2(void *p_context, struct cop_strdict_node *p_node, int depth) {
	struct enum_args      *p_eargs = p_context;
	struct dictnode       *p_dn;
	size_t                 save;
	struct cop_strh        key;
	struct jnode           tmp;
	int                    ret;
	struct ev_ast_node     p;
	p_dn = cop_strdict_node_to_data(p_node);
	cop_strdict_node_to_key(p_node, &key);
	save = cop_salloc_save(p_eargs->p_alloc);
	if (evaluate_ast(&p, p_dn->data, p_eargs->pp_stack, p_eargs->stack_size, p_eargs->p_alloc, p_eargs->p_handler))
		return -1;
	if (to_jnode(&tmp, &p, p_eargs->p_alloc, p_eargs->p_handler))
		return -1;
	ret = p_eargs->p_fn(&tmp, (char *)key.ptr, p_eargs->p_user_context);
	cop_salloc_restore(p_eargs->p_alloc, save);
	return ret;
}

static int enumerate_dict_keys(jdict_enumerate_fn *p_fn, void *p_ctx, struct cop_salloc_iface *p_alloc, void *p_userctx) {
	struct execution_context *ec = p_ctx;
	struct enum_args eargs;
	eargs.p_handler      = ec->p_error_handler;
	eargs.pp_stack       = ec->object.pp_stack;
	eargs.stack_size     = ec->object.stack_size;
	eargs.p_alloc        = p_alloc;
	eargs.p_user_context = p_userctx;
	eargs.p_fn           = p_fn;
	return cop_strdict_enumerate(ec->object.p_node->d.rdict.p_root, enumerate_dict_keys2, &eargs);
}

static int jnode_get_dict_element(struct jnode *p_dest, void *p_ctx, struct cop_salloc_iface *p_alloc, const char *p_key) {
	struct execution_context *ec = p_ctx;
	struct dictnode          *dn;
	struct ev_ast_node        p;
	if (cop_strdict_get_by_cstr(ec->object.p_node->d.rdict.p_root, p_key, (void **)&dn))
		return 1; /* Not found */
	if (evaluate_ast(&p, dn->data, ec->object.pp_stack, ec->object.stack_size, p_alloc, ec->p_error_handler))
		return -1;
	return to_jnode(p_dest, &p, p_alloc, ec->p_error_handler);
}

static int to_jnode(struct jnode *p_node, const struct ev_ast_node *p_ast, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	struct execution_context *p_ec;

	if (p_ast->p_node->cls == &AST_CLS_LITERAL_INT) {
		p_node->cls        = JNODE_CLS_INTEGER;
		p_node->d.int_bool = p_ast->p_node->d.i;
		return 0;
	}
	
	if (p_ast->p_node->cls == &AST_CLS_LITERAL_FLOAT) {
		p_node->cls    = JNODE_CLS_REAL;
		p_node->d.real = p_ast->p_node->d.f;
		return 0;
	}

	if (p_ast->p_node->cls == &AST_CLS_LITERAL_STRING) {
		p_node->cls          = JNODE_CLS_STRING;
		p_node->d.string.buf = p_ast->p_node->d.str.p_data;
		return 0;
	}

	if (p_ast->p_node->cls == &AST_CLS_LITERAL_NULL) {
		p_node->cls = JNODE_CLS_NULL;
		return 0;
	}

	if (p_ast->p_node->cls == &AST_CLS_LITERAL_BOOL) {
		p_node->cls        = JNODE_CLS_BOOL;
		p_node->d.int_bool = p_ast->p_node->d.i;
		return 0;
	}

	if ((p_ec = cop_salloc(p_alloc, sizeof(struct execution_context), 0)) == NULL)
		return ejson_error(p_error_handler, "out of memory\n");

	p_ec->p_error_handler       = p_error_handler;
	p_ec->object                = *p_ast;

	if (p_ast->p_node->cls == &AST_CLS_READY_DICT) {
		p_node->cls               = JNODE_CLS_DICT;
		p_node->d.dict.nb_keys    = p_ast->p_node->d.rdict.nb_keys;
		p_node->d.dict.ctx        = p_ec;
		p_node->d.dict.get_by_key = jnode_get_dict_element;
		p_node->d.dict.enumerate  = enumerate_dict_keys;
		return 0;
	}

	if (p_ast->p_node->cls == &AST_CLS_LIST_GENERATOR) {
		p_node->cls                  = JNODE_CLS_LIST;
		p_node->d.list.ctx           = p_ec;
		p_node->d.list.nb_elements   = p_ast->p_node->d.lgen.nb_elements;
		p_node->d.list.get_elemenent = jnode_list_get_element;
		return 0;
	}

	return ejson_location_error(p_error_handler, &(p_ast->p_node->doc_pos), "the given root node class (%s) cannot be represented using JSON\n", p_ast->p_node->cls->p_name);
}

int parse_document(struct jnode *p_node, struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_obj;
	const struct token *p_token;
	struct ev_ast_node p;
	while ((p_token = tok_peek(p_tokeniser)) != NULL && p_token->cls == &TOK_DEFINE) {
		struct cop_strh ident;
		struct cop_strdict_node *p_wsnode;
		p_token = tok_read(p_tokeniser, p_error_handler); assert(p_token != NULL);
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return 1;
		if (p_token->cls != &TOK_IDENTIFIER)
			return ejson_location_error(p_error_handler, &(p_token->posinfo), "expected an identifier, got a %s\n", p_token->cls->name);
		cop_strh_init_shallow(&ident, p_token->t.strident.str);
		if ((p_wsnode = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node) + ident.len + 1, 0)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		memcpy((char *)(p_wsnode + 1), p_token->t.strident.str, ident.len + 1);
		ident.ptr = (unsigned char *)(p_wsnode + 1);
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return 1;
		if (p_token->cls != &TOK_ASSIGN)
			return ejson_location_error(p_error_handler, &(p_token->posinfo), "expected '='\n");
		if ((p_obj = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
			return ejson_error(p_error_handler, "expected an expression\n");
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return 1;
		if (p_token->cls != &TOK_SEMI)
			return ejson_location_error(p_error_handler, &(p_token->posinfo), "expected ';'\n");
		cop_strdict_node_init(p_wsnode, &ident, (void *)p_obj);
		if (cop_strdict_insert(&(p_workspace->p_workspace), p_wsnode))
			return ejson_error(p_error_handler, "cannot redefine variable '%s'\n", ident.ptr);
#if 0
		printf("-%s-\n", ident.ptr);
		p_obj->cls->debug_print(p_obj, stdout, 0);
#endif
	}
	if ((p_obj = expect_expression(p_workspace, p_tokeniser, 0, p_error_handler)) == NULL)
		return 1;
	if ((p_token = tok_peek(p_tokeniser)) != NULL)
		return ejson_location_error(p_error_handler, &(p_token->posinfo), "expected no more tokens at end of document\n", p_token->cls->name);
#if 0
	printf("---\n");
	p_obj->cls->debug_print(p_obj, stdout, 0);
#endif
	if (evaluate_ast(&p, p_obj, NULL, 0, p_workspace->p_alloc, p_error_handler))
		return 1;
	return to_jnode(p_node, &p, p_workspace->p_alloc, p_error_handler);
}

int ejson_load(struct jnode *p_node, struct evaluation_context *p_workspace, const char *p_document, struct ejson_error_handler *p_error_handler) {
	struct tokeniser t;

	if (tokeniser_start(&t, p_document))
		return ejson_error(p_error_handler, "could not initialise tokeniser\n");

	return parse_document(p_node, p_workspace, &t, p_error_handler);

}

#if EJSON_TEST

#endif

