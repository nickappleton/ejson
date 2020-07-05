#include "ejson/ejson.h"
#include "cop/cop_filemap.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "parse_helpers.h"


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
	int                   precedence;
	int                   right_associative;
	const struct ast_cls *bin_op_cls;
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
			unsigned                stack_depth_at_function_def;
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
			const struct ast_node  **pp_stack;
			unsigned                 stack_size;
			unsigned                 nb_keys;
			struct cop_strdict_node *p_root;
		} rdict;
		struct {
			const struct ast_node **pp_stack;
			unsigned                stack_size;

			const struct ast_node *(*get_element)(const struct ast_node *p_list, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler);
			union {
				struct {
					long long first;
					long long step;
				} range;
				struct {
					const struct ast_node *p_function;
					const struct ast_node *p_key;
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

#define TOK_DECL(name_, precedence_, right_associative_, binop_ast_cls_) \
	static const struct tok_def name_ = {#name_, precedence_, right_associative_, binop_ast_cls_}

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


DEF_AST_CLS(AST_CLS_LIST_GENERATOR,  NULL, debug_list_generator);
DEF_AST_CLS(AST_CLS_READY_DICT,      NULL, debug_ready_dict);

/* Numeric literals */
TOK_DECL(TOK_INT,        -1, 0, NULL); /* 13123 */
TOK_DECL(TOK_FLOAT,      -1, 0, NULL); /* 13123.0 | .123 */

/* Binary operators */
TOK_DECL(TOK_LOGOR,       1, 0, &AST_CLS_LOGOR);  /* or */
TOK_DECL(TOK_LOGAND,      2, 0, &AST_CLS_LOGAND); /* and */
TOK_DECL(TOK_EQ,          3, 0, &AST_CLS_EQ);     /* == */
TOK_DECL(TOK_NEQ,         3, 0, &AST_CLS_NEQ);    /* != */
TOK_DECL(TOK_GT,          4, 0, &AST_CLS_GT);     /* > */
TOK_DECL(TOK_GEQ,         4, 0, &AST_CLS_GEQ);    /* >= */
TOK_DECL(TOK_LT,          4, 0, &AST_CLS_LT);     /* < */
TOK_DECL(TOK_LEQ,         4, 0, &AST_CLS_LEQ);    /* <= */
TOK_DECL(TOK_BITOR,       5, 0, &AST_CLS_BITOR);  /* | */
TOK_DECL(TOK_BITAND,      6, 0, &AST_CLS_BITAND); /* & */
TOK_DECL(TOK_ADD,         7, 0, &AST_CLS_ADD);    /* + */
TOK_DECL(TOK_SUB,         7, 0, &AST_CLS_SUB);    /* - */
TOK_DECL(TOK_MUL,         8, 0, &AST_CLS_MUL);    /* * */
TOK_DECL(TOK_DIV,         8, 0, &AST_CLS_DIV);    /* / */
TOK_DECL(TOK_MOD,         8, 0, &AST_CLS_MOD);    /* % */
TOK_DECL(TOK_EXP,         9, 1, &AST_CLS_EXP);    /* ^ */


/* String */
TOK_DECL(TOK_STRING,     -1, 0, NULL); /* "afasfasf" */

/* Keywords */
TOK_DECL(TOK_NULL,       -1, 0, NULL); /* null */
TOK_DECL(TOK_TRUE,       -1, 0, NULL); /* true */
TOK_DECL(TOK_FALSE,      -1, 0, NULL); /* false */
TOK_DECL(TOK_RANGE,      -1, 0, NULL); /* range */
TOK_DECL(TOK_FUNC,       -1, 0, NULL); /* func */
TOK_DECL(TOK_CALL,       -1, 0, NULL); /* call */
TOK_DECL(TOK_DEFINE,     -1, 0, NULL); /* define */
TOK_DECL(TOK_ACCESS,     -1, 0, NULL); /* access */
TOK_DECL(TOK_MAP,        -1, 0, NULL); /* map */
TOK_DECL(TOK_FORMAT,     -1, 0, NULL); /* format */
TOK_DECL(TOK_IDENTIFIER, -1, 0, NULL); /* afasfasf - anything not a keyword */

/* Symbols */
TOK_DECL(TOK_COMMA,      -1, 0, NULL); /* , */
TOK_DECL(TOK_LBRACE,     -1, 0, NULL); /* { */
TOK_DECL(TOK_RBRACE,     -1, 0, NULL); /* } */
TOK_DECL(TOK_LPAREN,     -1, 0, NULL); /* ( */
TOK_DECL(TOK_RPAREN,     -1, 0, NULL); /* ) */
TOK_DECL(TOK_LSQBR,      -1, 0, NULL); /* [ */
TOK_DECL(TOK_RSQBR,      -1, 0, NULL); /* ] */
TOK_DECL(TOK_ASSIGN,     -1, 0, NULL); /* = */
TOK_DECL(TOK_COLON,      -1, 0, NULL); /* : */
TOK_DECL(TOK_SEMI,       -1, 0, NULL); /* ; */

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
	p_tokeniser->line_nb      = 1;
	p_tokeniser->p_line_start = buf;
	p_tokeniser->buf          = buf;
	p_tokeniser->p_current    = &(p_tokeniser->curx);
	p_tokeniser->p_next       = &(p_tokeniser->nextx);
	return (tok_read(p_tokeniser, NULL) == NULL) ? 1 : 0;
}


void evaluation_context_init(struct evaluation_context *p_ctx, struct cop_salloc_iface *p_alloc) {
	p_ctx->p_alloc     = p_alloc;
	p_ctx->stack_depth = 0;
	p_ctx->p_workspace = cop_strdict_init();
}

const struct ast_node *expect_expression(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, const struct ejson_error_handler *p_error_handler);

const struct ast_node *parse_primary(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, const struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_temp_nodes[128];
	struct ast_node *p_ret = NULL;
	const struct token *p_token;

	if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
		return NULL;

	if (p_token->cls == &TOK_LPAREN) {
		const struct ast_node *p_subexpr;
		if ((p_subexpr = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
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
		return node;
	}

	if ((p_ret = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node), 0)) == NULL)
		return ejson_error_null(p_error_handler, "out of memory\n");
	p_ret->doc_pos = p_token->posinfo;

	if (p_token->cls == &TOK_ACCESS) {
		p_ret->cls        = &AST_CLS_ACCESS;
		if ((p_ret->d.access.p_data = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.access.p_key = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_MAP) {
		p_ret->cls        = &AST_CLS_MAP;
		if ((p_ret->d.map.p_function = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.map.p_input_list = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
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
				if ((p_temp_nodes[2*nb_kvs+0] = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if (p_token->cls != &TOK_COLON)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a :\n");
				if ((p_temp_nodes[2*nb_kvs+1] = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
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
				if ((p_temp_nodes[nb_list] = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
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
	} else if (p_token->cls == &TOK_SUB) {
		const struct ast_node *p_next;
		/* TODO: This sucks we can have unary unary unary unary unary.... don't want. */
		if ((p_next = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		p_ret->cls           = &AST_CLS_NEG;
		p_ret->d.binop.p_lhs = p_next;
		p_ret->d.binop.p_rhs = NULL;
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
		if ((p_ret->d.builtin.p_args = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_FORMAT) {
		p_ret->cls   = &AST_CLS_FORMAT;
		if ((p_ret->d.builtin.p_args = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
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
					return ejson_location_error_null(p_error_handler, &identpos, "function parameter names may only appear once and must alias workspace variables\n");
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
		p_ret->d.fn.stack_depth_at_function_def = p_workspace->stack_depth;

		p_workspace->stack_depth += nb_args;
		if ((p_ret->d.fn.node = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
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
		if ((p_ret->d.call.fn = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.call.p_args  = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
	} else {
		token_print(p_token);
		abort();
	}

	assert(p_ret != NULL);
	return p_ret;
}

const struct ast_node *expect_expression_1(const struct ast_node *p_lhs, struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, int min_precedence, const struct ejson_error_handler *p_error_handler) {
	const struct token *p_token;

	while
	    (   (p_token = tok_peek(p_tokeniser)) != NULL
	    &&  p_token->cls->precedence > 0 /* lookahead is a binary operator */
	    &&  p_token->cls->precedence >= min_precedence /* whose precedence is >= min_precedence */
	    ) {
		const struct ast_node *p_rhs;
		struct ast_node *p_comb;
		const struct tok_def *p_op = p_token->cls;
		struct token_pos_info loc_info = p_token->posinfo;

		p_token = tok_read(p_tokeniser, p_error_handler); /* Skip over the operator */
		assert(p_token != NULL);

		if ((p_rhs = parse_primary(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
	
		while
		    (   (p_token = tok_peek(p_tokeniser)) != NULL
		    &&   p_token->cls->precedence > 0 /* lookahead is a binary operator */
		    &&  (   p_token->cls->precedence > p_op->precedence  /* whose precedence is greater than op's */
		        ||  (p_token->cls->right_associative && p_token->cls->precedence == p_op->precedence) /* or a right-associative operator whose precedence is equal to op's */
		        )
		    ) {
			if ((p_rhs = expect_expression_1(p_rhs, p_workspace, p_tokeniser, p_token->cls->precedence, p_error_handler)) == NULL)
				return NULL; /* No error message is needed here. The recursion guarantees that the previous error will trigger at the same token. */
		}

		assert(p_op->bin_op_cls != NULL);

		if ((p_comb = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");

		p_comb->cls           = p_op->bin_op_cls;
		p_comb->doc_pos       = loc_info;
		p_comb->d.binop.p_lhs = p_lhs;
		p_comb->d.binop.p_rhs = p_rhs;
		p_lhs = p_comb;
	}


	return p_lhs;
}

const struct ast_node *expect_expression(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, const struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_lhs = parse_primary(p_workspace, p_tokeniser, p_error_handler);
	if (p_lhs == NULL)
		return NULL;
	return expect_expression_1(p_lhs, p_workspace, p_tokeniser, 0, p_error_handler);
}

struct jnode_data {
	struct ast_node *p_root;
	struct ast_node *p_stack;
	unsigned         nb_stack;


};

static int to_jnode(struct jnode *p_node, const struct ast_node *p_src, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler);

struct list_element_fn_data {
	struct p_error_handler  *p_error_handler;
	struct ast_node        **pp_stack;
	unsigned                 stack_size;
	unsigned                 nb_elements;
	struct ast_node        **pp_elements;

};

struct execution_context {
	const struct ast_node            *p_object;
	const struct ejson_error_handler *p_error_handler;

};

const struct ast_node *evaluate_ast(const struct ast_node *p_src, const struct ast_node **pp_stackx, unsigned stack_sizex, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler);

struct lrange {
	long long first;
	long long step_size;
	long long numel;

};

const struct ast_node *ast_list_generator_get_element(const struct ast_node *p_list, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	struct ast_node *p_dest;
	assert(p_list->cls == &AST_CLS_LIST_GENERATOR);
	if (element >= p_list->d.lgen.nb_elements)
		return (ejson_error(p_error_handler, "list index out of range\n"), NULL);
	if ((p_dest = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
		return ejson_error_null(p_error_handler, "out of memory\n");
	p_dest->doc_pos = p_list->doc_pos;
	p_dest->cls     = &AST_CLS_LITERAL_INT;
	p_dest->d.i     = p_list->d.lgen.d.range.first + p_list->d.lgen.d.range.step * (long long)element;
	return p_dest;
}

const struct ast_node *get_literal_element_fn(const struct ast_node *p_src, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	assert(p_src->cls == &AST_CLS_LIST_GENERATOR);
	if (element >= p_src->d.lgen.nb_elements)
		return (ejson_error(p_error_handler, "list index out of bounds\n"), NULL);
	if ((p_src = evaluate_ast(p_src->d.lgen.d.literal.pp_values[element], p_src->d.lgen.pp_stack, p_src->d.lgen.stack_size, p_alloc, p_error_handler)) == NULL)
		return NULL;
	return p_src;
}

const struct ast_node *get_list_cat(const struct ast_node *p_src, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_first;
	const struct ast_node *p_second;
	assert(p_src->cls == &AST_CLS_LIST_GENERATOR);
	p_first  = p_src->d.lgen.d.cat.p_first;
	p_second = p_src->d.lgen.d.cat.p_second;
	assert(p_first->cls == &AST_CLS_LIST_GENERATOR);
	assert(p_second->cls == &AST_CLS_LIST_GENERATOR);
	if (element < p_first->d.lgen.nb_elements)
		return p_first->d.lgen.get_element(p_first, element, p_alloc, p_error_handler);
	element -= p_first->d.lgen.nb_elements;
	return p_second->d.lgen.get_element(p_second, element, p_alloc, p_error_handler);
}

const struct ast_node *ast_list_generator_map(const struct ast_node *p_src, unsigned element, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_argument;
	const struct ast_node **pp_tmp;
	const struct ast_node *p_function;
	const struct ast_node *p_list;

	assert(p_src->cls == &AST_CLS_LIST_GENERATOR);
	p_function = p_src->d.lgen.d.map.p_function;
	p_list     = p_src->d.lgen.d.map.p_key;
	assert(p_function->cls == &AST_CLS_FUNCTION);
	assert(p_list->cls == &AST_CLS_LIST_GENERATOR);

	if (element >= p_list->d.lgen.nb_elements)
		return (ejson_error(p_error_handler, "list index out of range\n"), NULL);

	/* Evaluate argument and shove on stack */
	if ((p_argument = p_list->d.lgen.get_element(p_list, element, p_alloc, p_error_handler)) == NULL)
		return NULL;

	assert(p_src->d.lgen.stack_size == p_function->d.fn.stack_depth_at_function_def || !p_function->d.fn.stack_depth_at_function_def);

	if ((pp_tmp = cop_salloc(p_alloc, sizeof(struct ast_node *) * (p_function->d.fn.stack_depth_at_function_def + 1), 0)) == NULL)
		return ejson_error_null(p_error_handler, "out of memory\n");
	if (p_function->d.fn.stack_depth_at_function_def)
		memcpy(pp_tmp, p_src->d.lgen.pp_stack, p_function->d.fn.stack_depth_at_function_def * sizeof(struct ast_node *));
	pp_tmp[p_function->d.fn.stack_depth_at_function_def] = p_argument;

	return evaluate_ast(p_function->d.fn.node, pp_tmp, p_function->d.fn.stack_depth_at_function_def + 1, p_alloc, p_error_handler);
}

const struct ast_node *evaluate_ast(const struct ast_node *p_src, const struct ast_node **pp_stackx, unsigned stack_sizex, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	/* Move through stack references. */
	assert(p_src->cls != NULL);
	assert(p_src->cls->p_name != NULL);

	while (p_src->cls == &AST_CLS_STACKREF) {
		assert(pp_stackx != NULL);
		assert(p_src->d.i > 0 && p_src->d.i <= stack_sizex);
		p_src = pp_stackx[p_src->d.i - 1];
		assert(p_src != NULL);
	}
	
	assert(p_src->cls != NULL);
	assert(p_src->cls->p_name != NULL);

	/* Shortcuts for fully simplified objects. */
	if  (   p_src->cls == &AST_CLS_LITERAL_INT
	    ||  p_src->cls == &AST_CLS_LITERAL_BOOL
	    ||  p_src->cls == &AST_CLS_LITERAL_STRING
	    ||  p_src->cls == &AST_CLS_LITERAL_FLOAT
	    ||  p_src->cls == &AST_CLS_LITERAL_NULL
	    ||  p_src->cls == &AST_CLS_LIST_GENERATOR
	    ||  p_src->cls == &AST_CLS_READY_DICT
	    ||  p_src->cls == &AST_CLS_FUNCTION
	    ) {
		return p_src;
	}

	/* Convert lists into list generators */
	if  (p_src->cls == &AST_CLS_LITERAL_LIST) {
		struct ast_node *p_ret;
		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls                        = &AST_CLS_LIST_GENERATOR;
		p_ret->doc_pos                    = p_src->doc_pos;
		p_ret->d.lgen.pp_stack            = pp_stackx;
		p_ret->d.lgen.stack_size          = stack_sizex;
		p_ret->d.lgen.nb_elements         = p_src->d.llist.nb_elements;
		p_ret->d.lgen.d.literal.pp_values = p_src->d.llist.elements;
		p_ret->d.lgen.get_element         = get_literal_element_fn;
		return p_ret;
	}

	if (p_src->cls == &AST_CLS_FORMAT) {
		char strbuf[8192];
		unsigned i;
		unsigned argidx;
		const struct ast_node *p_args;
		const struct ast_node *p_fmtstr;
		struct ast_node *p_ret;
		const char *cp;
		char *ob;
		char c;

		if ((p_args = evaluate_ast(p_src->d.builtin.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if (p_args->cls != &AST_CLS_LIST_GENERATOR || p_args->d.lgen.nb_elements < 1)
			return (ejson_error(p_error_handler, "format expects a list argument with at least a format string\n"), NULL);
		if ((p_fmtstr = p_args->d.lgen.get_element(p_args, 0, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if (p_fmtstr->cls != &AST_CLS_LITERAL_STRING)
			return (ejson_error(p_error_handler, "first argument of format must be a string\n"), NULL);
		
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
						return (ejson_error(p_error_handler, "unsupported format flag '%c'\n", c), NULL);
					}
					c = *cp++;
				}

				if (c == '%') {
					strbuf[i] = c;
					i++;
				} else if (c == 'd') {
					const struct ast_node *p_argval;
					if (argidx >= p_args->d.lgen.nb_elements)
						return (ejson_error(p_error_handler, "not enough arguments given to format\n"), NULL);
					if ((p_argval = p_args->d.lgen.get_element(p_args, argidx++, p_alloc, p_error_handler)) == NULL)
						return NULL;
					if (p_argval->cls != &AST_CLS_LITERAL_INT)
						return (ejson_error(p_error_handler, "%%d expects an integer argument\n"), NULL);
					fmtspec[specpos++] = 'l';
					fmtspec[specpos++] = 'l';
					fmtspec[specpos++] = 'd';
					fmtspec[specpos++] = '\0';
					i += sprintf(&(strbuf[i]), fmtspec, p_argval->d.i);
				} else if (c == 's') {
					const struct ast_node *p_argval;
					if (argidx >= p_args->d.lgen.nb_elements)
						return (ejson_error(p_error_handler, "not enough arguments given to format\n"), NULL);
					if ((p_argval = p_args->d.lgen.get_element(p_args, argidx++, p_alloc, p_error_handler)) == NULL)
						return NULL;
					if (p_argval->cls != &AST_CLS_LITERAL_STRING)
						return (ejson_error(p_error_handler, "%%s expects a string argument (%s)\n", p_argval->cls->p_name), NULL);
					memcpy(&(strbuf[i]), p_argval->d.str.p_data, p_argval->d.str.len);
					i += p_argval->d.str.len;
				} else {
					return (ejson_error(p_error_handler, "invalid escape sequence (%%%c)\n", c), NULL);
				}
			} else {
				strbuf[i] = c;
				i++;
			}
		}
		strbuf[i] = '\0';

		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node) + i + 1, 0)) == NULL)
			return NULL;
		ob = (char *)(p_ret + 1);
		memcpy(ob, strbuf, i+1);

		p_ret->cls          = &AST_CLS_LITERAL_STRING;
		p_ret->doc_pos      = p_src->doc_pos;
		p_ret->d.str.p_data = ob;
		p_ret->d.str.len    = i;
		return p_ret;
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
			
			if ((p_key = evaluate_ast(p_src->d.ldict.elements[2*i+0], pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
				return NULL;
			if (p_key->cls != &AST_CLS_LITERAL_STRING)
				return ejson_location_error_null(p_error_handler, &(p_src->doc_pos), "a key expression in the dictionary did not evaluate to a string\n");

			/* fixme: there is no need to keep another copy of the string. ideally, we would have a separate stack as we don't need to keep hold of the above node and all memory that was needed to figure it out */
			cop_strh_init_shallow(&key, p_key->d.str.p_data);
			if ((p_dn = cop_salloc(p_alloc, sizeof(struct dictnode) + key.len + 1, 0)) == NULL)
				return ejson_error_null(p_error_handler, "out of memory\n");
			memcpy(p_dn + 1, key.ptr, key.len + 1);
			key.ptr = (unsigned char *)(p_dn + 1);
			cop_strdict_node_init(&(p_dn->node), &key, p_dn);
			p_dn->data = p_src->d.ldict.elements[2*i+1];

			if (cop_strdict_insert(&p_root, &(p_dn->node)))
				return ejson_error_null(p_error_handler, "attempted to add a key to a dictionary that already existed (%s)\n", p_key->d.str.p_data);
		}

		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls                = &AST_CLS_READY_DICT;
		p_ret->doc_pos            = p_src->doc_pos;
		p_ret->d.rdict.pp_stack   = pp_stackx;
		p_ret->d.rdict.stack_size = stack_sizex;
		p_ret->d.rdict.nb_keys    = p_src->d.ldict.nb_keys;
		p_ret->d.rdict.p_root     = p_root;
		return p_ret;
	}

	if (p_src->cls == &AST_CLS_ACCESS) {
		const struct ast_node *p_list;
		if ((p_list = evaluate_ast(p_src->d.access.p_data, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if (p_list->cls == &AST_CLS_LIST_GENERATOR) {
			const struct ast_node *p_idx;
			long long idx;
			size_t save = cop_salloc_save(p_alloc);
			if ((p_idx = evaluate_ast(p_src->d.access.p_key, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
				return NULL;
			if (p_idx->cls != &AST_CLS_LITERAL_INT)
				return ejson_location_error_null(p_error_handler, &(p_src->d.access.p_key->doc_pos), "the key expression for a list access did not evaluate to an integer\n");
			idx = p_idx->d.i;
			cop_salloc_restore(p_alloc, save);
			return p_list->d.lgen.get_element(p_list, idx, p_alloc, p_error_handler);
		}
		
		if (p_list->cls == &AST_CLS_READY_DICT) {
			const struct ast_node *p_key;
			struct dictnode *p_node;
			if ((p_key = evaluate_ast(p_src->d.access.p_key, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
				return NULL;
			if (p_key->cls != &AST_CLS_LITERAL_STRING)
				return ejson_location_error_null(p_error_handler, &(p_src->d.access.p_key->doc_pos), "the key expression for dict access did not evaluate to a string\n");
			if (cop_strdict_get_by_cstr(p_list->d.rdict.p_root, p_key->d.str.p_data, (void **)&p_node))
				return ejson_location_error_null(p_error_handler, &(p_src->d.access.p_key->doc_pos), "key '%s' not in dict\n", p_key->d.str.p_data);
			return evaluate_ast(p_node->data, pp_stackx, stack_sizex, p_alloc, p_error_handler);
		}

		return ejson_error_null(p_error_handler, "the list expression for access did not evaluate to a list\n");
	}

	/* Function call */
	if (p_src->cls == &AST_CLS_CALL) {
		const struct ast_node *p_function;
		const struct ast_node *p_args;
		const struct ast_node **pp_stack2 = pp_stackx;

		if ((p_function = evaluate_ast(p_src->d.call.fn, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if (p_function->cls != &AST_CLS_FUNCTION)
			return ejson_error_null(p_error_handler, "the function expression for call did not evaluate to a function\n");
		if ((p_args = evaluate_ast(p_src->d.call.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if (p_args->cls != &AST_CLS_LIST_GENERATOR)
			return ejson_error_null(p_error_handler, "the argument expression for call did not evaluate to a list\n");
		if (p_args->d.lgen.nb_elements != p_function->d.fn.nb_args)
			return ejson_location_error_null(p_error_handler, &(p_src->doc_pos), "the number of arguments supplied to function was incorrect (expected %u but got %u)\n", p_function->d.fn.nb_args, p_args->d.lgen.nb_elements);
		if (p_args->d.lgen.nb_elements) {
			unsigned i;
			assert(stack_sizex == p_function->d.fn.stack_depth_at_function_def || !p_function->d.fn.stack_depth_at_function_def);
			if ((pp_stack2 = cop_salloc(p_alloc, sizeof(struct ast_node *) * (p_function->d.fn.stack_depth_at_function_def + p_args->d.lgen.nb_elements), 0)) == NULL)
				return ejson_error_null(p_error_handler, "out of memory\n");
			if (p_function->d.fn.stack_depth_at_function_def)
				memcpy(pp_stack2, pp_stackx, p_function->d.fn.stack_depth_at_function_def * sizeof(struct ast_node *));
			for (i = 0; i < p_args->d.lgen.nb_elements; i++) {
				if ((pp_stack2[p_function->d.fn.stack_depth_at_function_def + i] = p_args->d.lgen.get_element(p_args, i, p_alloc, p_error_handler)) == NULL)
					return ejson_error_null(p_error_handler, "failed to evaluate list element %d because\n", i);
			}
		}

		return evaluate_ast(p_function->d.fn.node, pp_stack2, p_function->d.fn.stack_depth_at_function_def + p_args->d.lgen.nb_elements, p_alloc, p_error_handler);
	}

	/* Unary negation */
	if (p_src->cls == &AST_CLS_NEG) {
		const struct ast_node *p_result;
		struct ast_node       *p_tmp2;
		size_t                 save;

		if ((p_tmp2 = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		save = cop_salloc_save(p_alloc);
		if ((p_result = evaluate_ast(p_src->d.binop.p_lhs, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;

		if (p_result->cls == &AST_CLS_LITERAL_INT) {
			p_tmp2->cls = &AST_CLS_LITERAL_INT;
			p_tmp2->d.i = -p_result->d.i;
			cop_salloc_restore(p_alloc, save);
			return p_tmp2;
		}

		if (p_result->cls == &AST_CLS_LITERAL_FLOAT) {
			p_tmp2->cls = &AST_CLS_LITERAL_FLOAT;
			p_tmp2->d.f = -p_result->d.f;
			cop_salloc_restore(p_alloc, save);
			return p_tmp2;
		}

		return ejson_location_error_null(p_error_handler, &(p_src->doc_pos), "the expression for the unary negation operator did not evaluate to a numeric type\n");
	}

	/* Convert range into an accessor object */
	if (p_src->cls == &AST_CLS_RANGE) {
		struct ast_node       *p_result;
		size_t                 save;
		struct lrange          lrange;
		const struct ast_node *p_args;

		if ((p_result = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		save = cop_salloc_save(p_alloc);
		if ((p_args = evaluate_ast(p_src->d.builtin.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if (p_args->cls != &AST_CLS_LIST_GENERATOR)
			return ejson_location_error_null(p_error_handler, &(p_src->doc_pos), "range expects a list argument\n");
		if (p_args->d.lgen.nb_elements < 1 || p_args->d.lgen.nb_elements > 3)
			return ejson_location_error_null(p_error_handler, &(p_src->doc_pos), "range expects between 1 and 3 arguments\n");

		if (p_args->d.lgen.nb_elements == 1) {
			const struct ast_node *p_first;

			if ((p_first = p_args->d.lgen.get_element(p_args, 0, p_alloc, p_error_handler)) == NULL)
				return NULL;
			if (p_first->cls != &AST_CLS_LITERAL_INT)
				return (ejson_error(p_error_handler, "single argument range expects an integer number of items\n"), NULL);

			lrange.first     = 0;
			lrange.step_size = 1;
			lrange.numel     = p_first->d.i;
		} else if (p_args->d.lgen.nb_elements == 2) {
			const struct ast_node *p_first, *p_last;

			if  (   (p_first = p_args->d.lgen.get_element(p_args, 0, p_alloc, p_error_handler)) == NULL
			    ||  (p_last = p_args->d.lgen.get_element(p_args, 1, p_alloc, p_error_handler)) == NULL
			    )
				return NULL;
			if (p_first->cls != &AST_CLS_LITERAL_INT || p_last->cls != &AST_CLS_LITERAL_INT)
				return (ejson_error(p_error_handler, "dual argument range expects an integer first and last index\n"), NULL);

			lrange.first     = p_first->d.i;
			lrange.step_size = (p_first->d.i > p_last->d.i) ? -1 : 1;
			lrange.numel     = ((p_first->d.i > p_last->d.i) ? (p_first->d.i - p_last->d.i) : (p_last->d.i - p_first->d.i)) + 1;
		} else {
			const struct ast_node *p_first, *p_step, *p_last;

			if  (   (p_first = p_args->d.lgen.get_element(p_args, 0, p_alloc, p_error_handler)) == NULL
			    ||  (p_step = p_args->d.lgen.get_element(p_args, 1, p_alloc, p_error_handler)) == NULL
			    ||  (p_last = p_args->d.lgen.get_element(p_args, 2, p_alloc, p_error_handler)) == NULL
			    )
				return NULL;
			if  (   p_first->cls != &AST_CLS_LITERAL_INT || p_step->cls != &AST_CLS_LITERAL_INT ||  p_last->cls != &AST_CLS_LITERAL_INT
			    ||  p_step->d.i == 0
			    ||  (p_step->d.i > 0 && (p_first->d.i > p_last->d.i))
			    ||  (p_step->d.i < 0 && (p_first->d.i < p_last->d.i))
			    )
				return (ejson_error(p_error_handler, "triple argument range expects an integer first, step and last values. step must be non-zero and have the correct sign for the range.\n"), NULL);

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
		return p_result;
	}

	if (p_src->cls == &AST_CLS_MAP) {
		const struct ast_node *p_function, *p_list;
		struct ast_node *p_tmp;

		if ((p_function = evaluate_ast(p_src->d.map.p_function, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if (p_function->cls != &AST_CLS_FUNCTION || p_function->d.fn.nb_args != 1)
			return ejson_location_error_null(p_error_handler, &(p_src->doc_pos), "map expects a function argument that takes one argument\n");
		if ((p_list = evaluate_ast(p_src->d.map.p_input_list, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if (p_list->cls != &AST_CLS_LIST_GENERATOR)
			return ejson_location_error_null(p_error_handler, &(p_src->doc_pos), "map expected a list argument following the function\n");

		p_tmp = cop_salloc(p_alloc, sizeof(struct ast_node), 0);
		p_tmp->cls                     = &AST_CLS_LIST_GENERATOR;
		p_tmp->doc_pos                 = p_src->doc_pos;
		p_tmp->d.lgen.pp_stack         = pp_stackx;
		p_tmp->d.lgen.stack_size       = stack_sizex;
		p_tmp->d.lgen.d.map.p_function = p_function;
		p_tmp->d.lgen.d.map.p_key      = p_list;
		p_tmp->d.lgen.get_element      = ast_list_generator_map;
		p_tmp->d.lgen.nb_elements      = p_list->d.lgen.nb_elements;
		return p_tmp;
	}

	/* Ops */
	if  (p_src->cls == &AST_CLS_ADD || p_src->cls == &AST_CLS_SUB || p_src->cls == &AST_CLS_MUL || p_src->cls == &AST_CLS_MOD
	    || p_src->cls == &AST_CLS_BITOR || p_src->cls == &AST_CLS_BITAND
	    || p_src->cls == &AST_CLS_LOGAND || p_src->cls == &AST_CLS_LOGOR
	    || p_src->cls == &AST_CLS_EQ || p_src->cls == &AST_CLS_NEQ || p_src->cls == &AST_CLS_GT || p_src->cls == &AST_CLS_GEQ || p_src->cls == &AST_CLS_LEQ || p_src->cls == &AST_CLS_LT
	    ) {
		const struct ast_node *p_lhs;
		const struct ast_node *p_rhs;
		struct ast_node *p_ret;
		size_t save;
		
		if ((p_ret = cop_salloc(p_alloc, sizeof(struct ast_node), 0)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->doc_pos = p_src->doc_pos;

		save = cop_salloc_save(p_alloc);

		if ((p_lhs = evaluate_ast(p_src->d.binop.p_lhs, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;
		if ((p_rhs = evaluate_ast(p_src->d.binop.p_rhs, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return NULL;

		if (p_src->cls == &AST_CLS_LOGAND || p_src->cls == &AST_CLS_LOGOR) {
			if (p_lhs->cls != &AST_CLS_LITERAL_BOOL)
				return ejson_location_error_null(p_error_handler, &p_src->doc_pos, "lhs of logical operator was not boolean\n");
			if (p_rhs->cls != &AST_CLS_LITERAL_BOOL)
				return ejson_location_error_null(p_error_handler, &p_src->doc_pos, "rhs of logical operator was not boolean\n");
			p_ret->cls = &AST_CLS_LITERAL_BOOL;
			p_ret->d.i = (p_src->cls == &AST_CLS_LOGAND) ? (p_lhs->d.i && p_rhs->d.i) : (p_lhs->d.i || p_rhs->d.i);
			return p_ret;
		}

		if (p_src->cls == &AST_CLS_BITAND || p_src->cls == &AST_CLS_BITOR) {
			if (p_lhs->cls != &AST_CLS_LITERAL_INT)
				return ejson_location_error_null(p_error_handler, &p_src->doc_pos, "lhs of bitwise operator was not integer\n");
			if (p_rhs->cls != &AST_CLS_LITERAL_INT)
				return ejson_location_error_null(p_error_handler, &p_src->doc_pos, "rhs of bitwise operator was not integer\n");
			p_ret->cls = &AST_CLS_LITERAL_INT;
			p_ret->d.i = (p_src->cls == &AST_CLS_BITAND) ? (p_lhs->d.i & p_rhs->d.i) : (p_lhs->d.i | p_rhs->d.i);
			return p_ret;
		}

		if ((p_src->cls == &AST_CLS_EQ || p_src->cls == &AST_CLS_NEQ) && (p_lhs->cls == &AST_CLS_LITERAL_BOOL || p_rhs->cls == &AST_CLS_LITERAL_BOOL)) {
			if (p_lhs->cls != &AST_CLS_LITERAL_BOOL)
				return ejson_location_error_null(p_error_handler, &p_src->doc_pos, "lhs must be boolean if rhs is\n");
			if (p_rhs->cls != &AST_CLS_LITERAL_BOOL)
				return ejson_location_error_null(p_error_handler, &p_src->doc_pos, "rhs must be boolean if lhs is\n");
			p_ret->cls = &AST_CLS_LITERAL_BOOL;
			p_ret->d.i = (p_src->cls == &AST_CLS_EQ) ? (!p_lhs->d.i == !p_rhs->d.i) : (p_lhs->d.i != p_rhs->d.i);
			return p_ret;
		}

		if (p_src->cls == &AST_CLS_ADD && (p_lhs->cls == &AST_CLS_LIST_GENERATOR || p_rhs->cls == &AST_CLS_LIST_GENERATOR)) {
			if (p_lhs->cls != p_rhs->cls)
				return ejson_location_error_null(p_error_handler, &p_src->doc_pos, "expected lhs and rhs to both be lists\n");
			p_ret->cls                   = &AST_CLS_LIST_GENERATOR;
			p_ret->d.lgen.nb_elements    = p_lhs->d.lgen.nb_elements + p_rhs->d.lgen.nb_elements;
			p_ret->d.lgen.pp_stack       = pp_stackx;
			p_ret->d.lgen.stack_size     = stack_sizex;
			p_ret->d.lgen.get_element    = get_list_cat;
			p_ret->d.lgen.d.cat.p_first  = p_lhs;
			p_ret->d.lgen.d.cat.p_second = p_rhs;
			return p_ret;
		}

		/* Promote types */
		if (p_lhs->cls == &AST_CLS_LITERAL_FLOAT || p_rhs->cls == &AST_CLS_LITERAL_FLOAT) {
			double lhs, rhs;

			if (p_lhs->cls == &AST_CLS_LITERAL_FLOAT)
				lhs = p_lhs->d.f;
			else if (p_lhs->cls == &AST_CLS_LITERAL_INT)
				lhs = (double)(p_lhs->d.i);
			else
				return NULL;

			if (p_rhs->cls == &AST_CLS_LITERAL_FLOAT)
				rhs = p_rhs->d.f;
			else if (p_rhs->cls == &AST_CLS_LITERAL_INT)
				rhs = (double)(p_rhs->d.i);
			else
				return NULL;

			cop_salloc_restore(p_alloc, save);

			if (p_src->cls == &AST_CLS_ADD) {
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

			return p_ret;
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

			return p_ret;
		}

		return ejson_location_error_null(p_error_handler, &(p_ret->doc_pos), "the types given for binary operator %s were invalid\n", p_src->cls->p_name);
	}

	fprintf(stderr, "what?\n");
	p_src->cls->debug_print(p_src, stderr, 10);
	fprintf(stderr, "endwhat?\n");
	abort();

	return NULL;
}


static int jnode_list_get_element(struct jnode *p_dest, void *ctx, struct cop_salloc_iface *p_alloc, unsigned idx) {
	struct execution_context *ec   = ctx;
	const struct ast_node *p_tmp;

	if ((p_tmp = ec->p_object->d.lgen.get_element(ec->p_object, idx, p_alloc, ec->p_error_handler)) == NULL)
		return -1;

	return to_jnode(p_dest, p_tmp, p_alloc, ec->p_error_handler);
}

struct enum_args {
	const struct ejson_error_handler  *p_handler;
	const struct ast_node            **pp_stack;
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
	const struct ast_node *p_eval;
	struct jnode           tmp;
	int                    ret;
	p_dn = cop_strdict_node_to_data(p_node);
	cop_strdict_node_to_key(p_node, &key);
	save = cop_salloc_save(p_eargs->p_alloc);
	if ((p_eval = evaluate_ast(p_dn->data, p_eargs->pp_stack, p_eargs->stack_size, p_eargs->p_alloc, p_eargs->p_handler)) == NULL)
		return -1;
	if (to_jnode(&tmp, p_eval, p_eargs->p_alloc, p_eargs->p_handler))
		return -1;
	ret = p_eargs->p_fn(&tmp, (char *)key.ptr, p_eargs->p_user_context);
	cop_salloc_restore(p_eargs->p_alloc, save);
	return ret;
}

static int enumerate_dict_keys(jdict_enumerate_fn *p_fn, void *p_ctx, struct cop_salloc_iface *p_alloc, void *p_userctx) {
	struct execution_context *ec = p_ctx;
	struct enum_args eargs;
	eargs.p_handler      = ec->p_error_handler;
	eargs.pp_stack       = ec->p_object->d.rdict.pp_stack;
	eargs.stack_size     = ec->p_object->d.rdict.stack_size;
	eargs.p_alloc        = p_alloc;
	eargs.p_user_context = p_userctx;
	eargs.p_fn           = p_fn;
	return cop_strdict_enumerate(ec->p_object->d.rdict.p_root, enumerate_dict_keys2, &eargs);
}

static int jnode_get_dict_element(struct jnode *p_dest, void *p_ctx, struct cop_salloc_iface *p_alloc, const char *p_key) {
	struct execution_context *ec = p_ctx;
	struct dictnode          *dn;
	const struct ast_node    *prval;
	if (cop_strdict_get_by_cstr(ec->p_object->d.rdict.p_root, p_key, (void **)&dn))
		return 1; /* Not found */
	prval = evaluate_ast(dn->data, ec->p_object->d.rdict.pp_stack, ec->p_object->d.rdict.stack_size, p_alloc, ec->p_error_handler);
	if (prval == NULL)
		return -1; /* Error */
	return to_jnode(p_dest, prval, p_alloc, ec->p_error_handler);
}

static int to_jnode(struct jnode *p_node, const struct ast_node *p_ast, struct cop_salloc_iface *p_alloc, const struct ejson_error_handler *p_error_handler) {
	struct execution_context *p_ec;

	if (p_ast->cls == &AST_CLS_LITERAL_INT) {
		p_node->cls        = JNODE_CLS_INTEGER;
		p_node->d.int_bool = p_ast->d.i;
		return 0;
	}
	
	if (p_ast->cls == &AST_CLS_LITERAL_FLOAT) {
		p_node->cls    = JNODE_CLS_REAL;
		p_node->d.real = p_ast->d.f;
		return 0;
	}

	if (p_ast->cls == &AST_CLS_LITERAL_STRING) {
		p_node->cls          = JNODE_CLS_STRING;
		p_node->d.string.buf = p_ast->d.str.p_data;
		return 0;
	}

	if (p_ast->cls == &AST_CLS_LITERAL_NULL) {
		p_node->cls = JNODE_CLS_NULL;
		return 0;
	}

	if (p_ast->cls == &AST_CLS_LITERAL_BOOL) {
		p_node->cls        = JNODE_CLS_BOOL;
		p_node->d.int_bool = p_ast->d.i;
		return 0;
	}

	if ((p_ec = cop_salloc(p_alloc, sizeof(struct execution_context), 0)) == NULL)
		return ejson_error(p_error_handler, "out of memory\n");

	p_ec->p_error_handler       = p_error_handler;
	p_ec->p_object              = p_ast;

	if (p_ast->cls == &AST_CLS_READY_DICT) {
		p_node->cls               = JNODE_CLS_DICT;
		p_node->d.dict.nb_keys    = p_ast->d.rdict.nb_keys;
		p_node->d.dict.ctx        = p_ec;
		p_node->d.dict.get_by_key = jnode_get_dict_element;
		p_node->d.dict.enumerate  = enumerate_dict_keys;
		return 0;
	}

	if (p_ast->cls == &AST_CLS_LIST_GENERATOR) {
		p_node->cls                  = JNODE_CLS_LIST;
		p_node->d.list.ctx           = p_ec;
		p_node->d.list.nb_elements   = p_ast->d.lgen.nb_elements;
		p_node->d.list.get_elemenent = jnode_list_get_element;
		return 0;
	}

	return ejson_location_error(p_error_handler, &(p_ast->doc_pos), "the given root node class (%s) cannot be represented using JSON\n", p_ast->cls->p_name);
}

int parse_document(struct jnode *p_node, struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_obj;
	const struct ast_node *p_root;
	const struct token *p_token;

	while ((p_token = tok_peek(p_tokeniser)) != NULL && p_token->cls == &TOK_DEFINE) {
		struct cop_strh ident;
		struct cop_strdict_node *p_wsnode;
		p_token = tok_read(p_tokeniser, p_error_handler); assert(p_token != NULL);
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return 1;
		if (p_token->cls != &TOK_IDENTIFIER)
			return ejson_location_error(p_error_handler, &(p_token->posinfo), "expected an identifier\n");
		cop_strh_init_shallow(&ident, p_token->t.strident.str);
		if ((p_wsnode = cop_salloc(p_workspace->p_alloc, sizeof(struct ast_node) + ident.len + 1, 0)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		memcpy((char *)(p_wsnode + 1), p_token->t.strident.str, ident.len + 1);
		ident.ptr = (unsigned char *)(p_wsnode + 1);
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return 1;
		if (p_token->cls != &TOK_ASSIGN)
			return ejson_location_error(p_error_handler, &(p_token->posinfo), "expected '='\n");
		if ((p_obj = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return ejson_error(p_error_handler, "expected an expression\n");
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return 1;
		if (p_token->cls != &TOK_SEMI)
			return ejson_location_error(p_error_handler, &(p_token->posinfo), "expected ';'\n");
		cop_strdict_node_init(p_wsnode, &ident, (void *)p_obj);
		if (cop_strdict_insert(&(p_workspace->p_workspace), p_wsnode))
			return ejson_error(p_error_handler, "cannot redefine variable '%s'\n", ident.ptr);
	}
	if ((p_obj = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
		return 1;
	if ((p_token = tok_peek(p_tokeniser)) != NULL)
		return ejson_location_error(p_error_handler, &(p_token->posinfo), "expected no more tokens at end of document\n", p_token->cls->name);
#if 0
	p_obj->cls->debug_print(p_obj, stdout, 0);
#endif
	if ((p_root = evaluate_ast(p_obj, NULL, 0, p_workspace->p_alloc, p_error_handler)) == NULL)
		return 1;
	if (to_jnode(p_node, p_root, p_workspace->p_alloc, p_error_handler))
		return 1;
	return 0;
}

int ejson_load(struct jnode *p_node, struct evaluation_context *p_workspace, const char *p_document, struct ejson_error_handler *p_error_handler) {
	struct tokeniser t;

	if (tokeniser_start(&t, p_document))
		return ejson_error(p_error_handler, "could not initialise tokeniser\n");

	return parse_document(p_node, p_workspace, &t, p_error_handler);

}

#if EJSON_TEST

#endif

