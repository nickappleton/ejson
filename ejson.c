
#include "ejson.h"
#include "cop/cop_filemap.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "linear_allocator.h"
#include "../istrings.h"
#include "../pdict.h"
#include "parse_helpers.h"

#define MAX_TOK_STRING (4096)

#ifndef ejson_error
#ifdef EJSON_NO_ERROR_MESSAGES
#define ejson_error(p_handler_, p_format, ...) (-1)
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

	union {
		long long                   i; /* AST_CLS_LITERAL_INT, AST_CLS_LITERAL_BOOL */
		double                      f; /* AST_CLS_LITERAL_FLOAT */
		struct {
			uint_fast32_t           len;
			uint_fast32_t           hash;
			const char             *p_data;
		} str; /* AST_CLS_LITERAL_STRING */
		struct {
			const struct ast_node  *p_lhs;
			const struct ast_node  *p_rhs;
		} binop; /* AST_CLS_STRCAT, AST_CLS_NEG*, AST_CLS_ADD*, AST_CLS_SUB*, AST_CLS_MUL*, AST_CLS_DIV*, AST_CLS_MOD* */
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
			const struct ast_node  *p_list;
			const struct ast_node  *p_index;
		} listval;

		struct {
			const struct ast_node **pp_stack;
			unsigned                stack_size;
			unsigned                nb_keys;
			struct dictnode        *p_root;
		} rdict;
		struct {
			const struct ast_node **pp_stack;
			unsigned                stack_size;

			const struct ast_node *(*get_element)(const struct ast_node *p_list, unsigned element, struct linear_allocator *p_alloc, const struct ejson_error_handler *p_error_handler);
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
			} d;
			uint_fast32_t     nb_elements;
		} lgen; /* AST_CLS_LIST_GENERATOR */

	} d;

};

struct dictnode;

#define DICTNODE_BITS (2)
#define DICTNODE_NUM  (1u << DICTNODE_BITS)
#define DICTNODE_MASK (DICTNODE_NUM - 1u)

struct dictnode {
	const struct ast_node *data2; /* Unevaluated nodes */
	struct dictnode       *p_children[DICTNODE_NUM];
	uint_fast32_t          key;
};

static size_t hashfnv(const char *p_str, uint_fast32_t *p_hash) {
	uint_fast32_t hash = 2166136261;
	uint_fast32_t c;
	size_t        length = 0;
	while ((c = p_str[length++]) != 0)
		hash = ((hash ^ c) * 16777619) & 0xFFFFFFFFu;
	*p_hash = hash;
	return length;
}

struct token;

struct tok_def {
	const char           *name;
	int                   precedence;
	int                   right_associative;
	const struct ast_cls *bin_op_cls;
};

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
static void debug_print_listval(const struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.listval.p_list->cls->debug_print(p_node->d.listval.p_list, p_f, depth + 1);
	p_node->d.listval.p_index->cls->debug_print(p_node->d.listval.p_index, p_f, depth + 1);
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
DEF_AST_CLS(AST_CLS_ADD,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_SUB,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_MUL,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_DIV,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_MOD,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_EXP,             NULL, debug_print_binop);
DEF_AST_CLS(AST_CLS_RANGE,           NULL, debug_print_builtin);
DEF_AST_CLS(AST_CLS_FUNCTION,        NULL, debug_print_function);
DEF_AST_CLS(AST_CLS_CALL,            NULL, debug_print_call);
DEF_AST_CLS(AST_CLS_LISTVAL,         NULL, debug_print_listval);
DEF_AST_CLS(AST_CLS_MAP,             NULL, debug_print_map);
DEF_AST_CLS(AST_CLS_FORMAT,          NULL, debug_print_builtin);
DEF_AST_CLS(AST_CLS_STACKREF,        NULL, debug_print_int_like);


DEF_AST_CLS(AST_CLS_LIST_GENERATOR,  NULL, debug_list_generator);
DEF_AST_CLS(AST_CLS_READY_DICT,      NULL, debug_ready_dict);

/* Numeric literals */
TOK_DECL(TOK_INT,        -1, 0, NULL); /* 13123 */
TOK_DECL(TOK_FLOAT,      -1, 0, NULL); /* 13123.0 | .123 */

/* Binary operators */
TOK_DECL(TOK_ADD,         1, 0, &AST_CLS_ADD); /* + */
TOK_DECL(TOK_SUB,         1, 0, &AST_CLS_SUB); /* - */
TOK_DECL(TOK_MUL,         2, 0, &AST_CLS_MUL); /* * */
TOK_DECL(TOK_DIV,         2, 0, &AST_CLS_DIV); /* / */
TOK_DECL(TOK_MOD,         2, 0, &AST_CLS_MOD); /* % */
TOK_DECL(TOK_EXP,         3, 1, &AST_CLS_EXP); /* ^ */

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
TOK_DECL(TOK_LISTVAL,    -1, 0, NULL); /* listval */
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
TOK_DECL(TOK_EQ,         -1, 0, NULL); /* = */
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
	char c;
	int in_comment;

	/* eat whitespace and comments */
	c          = *(p_tokeniser->buf++);
	in_comment = (c == '#');
	while (c == ' ' || c == '\t' || c == '\r' || c == '\n' || in_comment) {
		char nc;
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
		p_temp->cls = &TOK_INT;
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
		} else if (!strcmp(p_temp->t.strident.str, "listval")) { p_temp->cls = &TOK_LISTVAL;
		} else if (!strcmp(p_temp->t.strident.str, "map")) { p_temp->cls = &TOK_MAP;
		} else if (!strcmp(p_temp->t.strident.str, "format")) { p_temp->cls = &TOK_FORMAT;
		} else { p_temp->cls = &TOK_IDENTIFIER; }

	} else if (c == '=') { p_temp->cls = &TOK_EQ;
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
	} else {
		return ejson_location_error_null(p_error_handler, &(p_temp->posinfo), "invalid token\n");
	}

	p_temp                 = p_tokeniser->p_next;
	p_tokeniser->p_next    = p_tokeniser->p_current;
	p_tokeniser->p_current = p_temp;

	return p_temp;
}

static int tokeniser_start(struct tokeniser *p_tokeniser, char *buf) {
	p_tokeniser->line_nb      = 1;
	p_tokeniser->p_line_start = buf;
	p_tokeniser->buf          = buf;
	p_tokeniser->p_current    = &(p_tokeniser->curx);
	p_tokeniser->p_next       = &(p_tokeniser->nextx);
	return (tok_read(p_tokeniser, NULL) == NULL) ? 1 : 0;
}

struct evaluation_context {
	struct istrings         strings;
	struct pdict            workspace;
	struct linear_allocator alloc;
	unsigned                stack_depth;

};

int evaluation_context_init(struct evaluation_context *p_ctx) {
	if (istrings_init(&(p_ctx->strings)))
		return -1;
	p_ctx->alloc.pos = 0;
	p_ctx->stack_depth = 0;
	pdict_init(&(p_ctx->workspace));
	return 0;
}

void evaluation_context_free(struct evaluation_context *p_ctx) {
	pdict_free(&(p_ctx->workspace));
	istrings_free(&(p_ctx->strings));
}

const struct ast_node *expect_expression(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, const struct ejson_error_handler *p_error_handler);

const struct ast_node *parse_primary(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, const struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_temp_nodes[8192];
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
		const struct istring   *k;
		const struct ast_node **node;
		if ((k = istrings_add(&(p_workspace->strings), p_token->t.strident.str)) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		if ((node = (const struct ast_node **)pdict_get(&(p_workspace->workspace), (uintptr_t)(k->cstr))) == NULL)
			return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "'%s' was not found in the workspace\n", k->cstr);
		return *node;
	}

	if (p_token->cls == &TOK_LISTVAL) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls        = &AST_CLS_LISTVAL;
		if ((p_ret->d.listval.p_list = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.listval.p_index = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_MAP) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls        = &AST_CLS_MAP;
		if ((p_ret->d.map.p_function = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		if ((p_ret->d.map.p_input_list = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_INT) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls        = &AST_CLS_LITERAL_INT;
		p_ret->d.i        = p_token->t.tint;
	} else if (p_token->cls == &TOK_FLOAT) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls        = &AST_CLS_LITERAL_FLOAT;
		p_ret->d.f        = p_token->t.tflt;
	} else if (p_token->cls == &TOK_STRING) {
		size_t sl = strlen(p_token->t.strident.str);
		char *p_strbuf;
		p_ret               = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_strbuf            = linear_allocator_alloc(&(p_workspace->alloc), sl + 1);
		memcpy(p_strbuf, p_token->t.strident.str, sl + 1);
		p_ret->cls          = &AST_CLS_LITERAL_STRING;
		p_ret->d.str.hash   = 0;
		p_ret->d.str.len    = sl;
		p_ret->d.str.p_data = p_strbuf;
	} else if (p_token->cls == &TOK_LBRACE) {
		uint_fast32_t    nb_kvs = 0;
		const struct token *p_next;
		if ((p_next = tok_peek(p_tokeniser)) == NULL) {
			fprintf(stderr, "expected another token\n");
			return NULL;
		}
		if (p_next->cls != &TOK_RBRACE) {
			do {
				p_temp_nodes[2*nb_kvs+0] = expect_expression(p_workspace, p_tokeniser, p_error_handler);
				if (p_temp_nodes[2*nb_kvs+0] == NULL) {
					fprintf(stderr, "expected expression\n");
					return NULL;
				}
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if (p_token->cls != &TOK_COLON)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a :\n");

				p_temp_nodes[2*nb_kvs+1] = expect_expression(p_workspace, p_tokeniser, p_error_handler);
				if (p_temp_nodes[2*nb_kvs+1] == NULL) {
					fprintf(stderr, "expected expression\n");
					return NULL;
				}
				nb_kvs++;
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if (p_token->cls != &TOK_RBRACE && p_token->cls != &TOK_COMMA)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a , or }\n");
			} while (p_token->cls != &TOK_RBRACE);
		} else {
			if (tok_read(p_tokeniser, p_error_handler) == NULL) { /* empty dict - skip over } */
				return NULL;
			}
		}
		p_ret                  = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls             = &AST_CLS_LITERAL_DICT;
		p_ret->d.ldict.nb_keys = nb_kvs;
		if (nb_kvs) {
			p_ret->d.ldict.elements = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node *) * nb_kvs * 2);
			memcpy(p_ret->d.ldict.elements, p_temp_nodes, sizeof(struct ast_node *) * nb_kvs * 2);
		} else {
			p_ret->d.ldict.elements = NULL;
		}
	} else if (p_token->cls == &TOK_LSQBR) {
		uint_fast32_t nb_list = 0;
		const struct token *p_next;
		if ((p_next = tok_peek(p_tokeniser)) == NULL)
			return ejson_error_null(p_error_handler, "a list expression must be terminated\n");
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
		p_ret             = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls        = &AST_CLS_LITERAL_LIST;
		p_ret->d.llist.nb_elements = nb_list;
		if (nb_list) {
			p_ret->d.llist.elements = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node *) * nb_list);
			memcpy(p_ret->d.llist.elements, p_temp_nodes, sizeof(struct ast_node *) * nb_list);
		} else {
			p_ret->d.llist.elements = NULL;
		}
	} else if (p_token->cls == &TOK_SUB) {
		const struct ast_node *p_next;
		/* TODO: This sucks we can have unary unary unary unary unary.... don't want. */
		p_next = expect_expression(p_workspace, p_tokeniser, p_error_handler);
		if (p_next == NULL)
			return NULL;
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls           = &AST_CLS_NEG;
		p_ret->d.binop.p_lhs = p_next;
		p_ret->d.binop.p_rhs = NULL;
	} else if (p_token->cls == &TOK_NULL) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls   = &AST_CLS_LITERAL_NULL;
	} else if (p_token->cls == &TOK_TRUE) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls = &AST_CLS_LITERAL_BOOL;
		p_ret->d.i = 1;
	} else if (p_token->cls == &TOK_FALSE) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls = &AST_CLS_LITERAL_BOOL;
		p_ret->d.i = 0;
	} else if (p_token->cls == &TOK_RANGE) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls   = &AST_CLS_RANGE;
		if ((p_ret->d.builtin.p_args = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_FORMAT) {
		if ((p_ret = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");
		p_ret->cls   = &AST_CLS_FORMAT;
		if ((p_ret->d.builtin.p_args = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;
	} else if (p_token->cls == &TOK_FUNC) {
		unsigned        nb_args = 0;
		unsigned        i;
		struct ast_node args[32];
		uintptr_t       argnames[32];
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return NULL;
		if (p_token->cls != &TOK_LPAREN)
			return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected an open parenthesis\n");
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
			return NULL;

		if (p_token->cls != &TOK_RPAREN) {
			do {
				const struct istring *k;
				struct token_pos_info identpos;
				void **node;
				identpos = p_token->posinfo;
				if (p_token->cls != &TOK_IDENTIFIER)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a parameter name literal but got a %s token\n", p_token->cls->name);
				if ((k = istrings_add(&(p_workspace->strings), p_token->t.strident.str)) == NULL)
					return ejson_error_null(p_error_handler, "out of memory\n");
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
				if (p_token->cls != &TOK_COMMA && p_token->cls != &TOK_RPAREN)
					return ejson_location_error_null(p_error_handler, &(p_token->posinfo), "expected a comma or close parenthesis\n");
				argnames[nb_args] = (uintptr_t)(k->cstr);
				node = pdict_get(&(p_workspace->workspace), argnames[nb_args]);
				if (node != NULL)
					return ejson_location_error_null(p_error_handler, &identpos, "function parameter names may only appear once and must alias workspace variables\n");
				args[nb_args].cls = &AST_CLS_STACKREF;
				args[nb_args].d.i = p_workspace->stack_depth + nb_args + 1;
				if (pdict_set(&(p_workspace->workspace), argnames[nb_args], &(args[nb_args])))
					return ejson_error_null(p_error_handler, "out of memory\n");
				nb_args++;
				if (p_token->cls == &TOK_RPAREN)
					break;
				if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL)
					return NULL;
			} while (1);
		}

		p_workspace->stack_depth += nb_args;

		p_ret               = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls          = &AST_CLS_FUNCTION;
		p_ret->d.fn.nb_args = nb_args;
		if ((p_ret->d.fn.node = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return NULL;

		p_workspace->stack_depth -= nb_args;

		for (i = 0; i < nb_args; i++) {
			if (pdict_delete(&(p_workspace->workspace), argnames[i])) {
				fprintf(stderr, "ICE\n");
				abort();
			}
		}

	} else if (p_token->cls == &TOK_CALL) {
		p_ret             = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls        = &AST_CLS_CALL;
		p_ret->d.call.fn  = expect_expression(p_workspace, p_tokeniser, p_error_handler);
		if (p_ret->d.call.fn == NULL)
			return NULL;
		p_ret->d.call.p_args  = expect_expression(p_workspace, p_tokeniser, p_error_handler);
		if (p_ret->d.call.p_args == NULL)
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

		if ((p_comb = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return ejson_error_null(p_error_handler, "out of memory\n");

		p_comb->cls           = p_op->bin_op_cls;
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

static int to_jnode(struct jnode *p_node, const struct ast_node *p_src, struct linear_allocator *p_alloc, const struct ejson_error_handler *p_error_handler);

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

const struct ast_node *evaluate_ast(const struct ast_node *p_src, const struct ast_node **pp_stackx, unsigned stack_sizex, struct linear_allocator *p_alloc, const struct ejson_error_handler *p_error_handler);

struct lrange {
	long long first;
	long long step_size;
	long long numel;

};

const struct ast_node *ast_list_generator_get_element(const struct ast_node *p_list, unsigned element, struct linear_allocator *p_alloc, const struct ejson_error_handler *p_error_handler) {
	struct ast_node *p_dest;
	assert(p_list->cls == &AST_CLS_LIST_GENERATOR);
	if (element >= p_list->d.lgen.nb_elements)
		return (ejson_error(p_error_handler, "list index out of range\n"), NULL);
	if ((p_dest = linear_allocator_alloc(p_alloc, sizeof(struct ast_node))) == NULL)
		return ejson_error_null(p_error_handler, "out of memory\n");
	p_dest->cls = &AST_CLS_LITERAL_INT;
	p_dest->d.i = p_list->d.lgen.d.range.first + p_list->d.lgen.d.range.step * (long long)element;
	return p_dest;
}

const struct ast_node *get_literal_element_fn(const struct ast_node *p_src, unsigned element, struct linear_allocator *p_alloc, const struct ejson_error_handler *p_error_handler) {
	assert(p_src->cls == &AST_CLS_LIST_GENERATOR);
	if (element >= p_src->d.lgen.nb_elements)
		return (ejson_error(p_error_handler, "list index out of bounds\n"), NULL);
	if ((p_src = evaluate_ast(p_src->d.lgen.d.literal.pp_values[element], p_src->d.lgen.pp_stack, p_src->d.lgen.stack_size, p_alloc, p_error_handler)) == NULL)
		return (ejson_error(p_error_handler, "list index out of bounds\n"), NULL);
	return p_src;
}

const struct ast_node *ast_list_generator_map(const struct ast_node *p_src, unsigned element, struct linear_allocator *p_alloc, const struct ejson_error_handler *p_error_handler) {
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
		return (ejson_error(p_error_handler, "could not get list item\n"), NULL);
	if ((pp_tmp = linear_allocator_alloc(p_alloc, sizeof(struct ast_node *) * (p_src->d.lgen.stack_size + 1))) == NULL)
		return ejson_error_null(p_error_handler, "out of memory\n");
	if (p_src->d.lgen.stack_size)
		memcpy(pp_tmp, p_src->d.lgen.pp_stack, p_src->d.lgen.stack_size * sizeof(struct ast_node *));
	pp_tmp[p_src->d.lgen.stack_size] = p_argument;

	return evaluate_ast(p_function->d.fn.node, pp_tmp, p_src->d.lgen.stack_size + 1, p_alloc, p_error_handler);
}

const struct ast_node *evaluate_ast(const struct ast_node *p_src, const struct ast_node **pp_stackx, unsigned stack_sizex, struct linear_allocator *p_alloc, const struct ejson_error_handler *p_error_handler) {
	/* Move through stack references. */
	while (p_src->cls == &AST_CLS_STACKREF) {
		assert(pp_stackx != NULL);
		assert(p_src->d.i > 0 && p_src->d.i <= stack_sizex);
		p_src = pp_stackx[stack_sizex - p_src->d.i];
		assert(p_src != NULL);
	}
	
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
		struct ast_node        *p_ret   = linear_allocator_alloc(p_alloc, sizeof(struct ast_node));
		p_ret->cls                        = &AST_CLS_LIST_GENERATOR;
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

		if ((p_args = evaluate_ast(p_src->d.builtin.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL || p_args->cls != &AST_CLS_LIST_GENERATOR || p_args->d.lgen.nb_elements < 1)
			return (ejson_error(p_error_handler, "format expects a list argument with at least a format string\n"), NULL);

		if ((p_fmtstr = p_args->d.lgen.get_element(p_args, 0, p_alloc, p_error_handler)) == NULL || p_fmtstr->cls != &AST_CLS_LITERAL_STRING)
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
					if ((p_argval = p_args->d.lgen.get_element(p_args, argidx++, p_alloc, p_error_handler)) == NULL || p_argval->cls != &AST_CLS_LITERAL_INT)
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
					if ((p_argval = p_args->d.lgen.get_element(p_args, argidx++, p_alloc, p_error_handler)) == NULL || p_argval->cls != &AST_CLS_LITERAL_STRING)
						return (ejson_error(p_error_handler, "%%s expects a string argument\n"), NULL);
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

		p_ret = linear_allocator_alloc(p_alloc, sizeof(struct ast_node) + i + 1);
		ob = (char *)(p_ret + 1);
		memcpy(ob, strbuf, i+1);

		p_ret->cls          = &AST_CLS_LITERAL_STRING;
		p_ret->d.str.p_data = ob;
		p_ret->d.str.len    = hashfnv(ob, &(p_ret->d.str.hash));
		return p_ret;
	}

	if  (p_src->cls == &AST_CLS_LITERAL_DICT) {
		unsigned i;

		struct dictnode *p_root = NULL;

		/* Build the dictionary */
		for (i = 0; i < p_src->d.ldict.nb_keys; i++) {
			struct dictnode **pp_ipos = &p_root;
			struct dictnode  *p_iter = *pp_ipos;
			uint_fast32_t hash, ukey;
			size_t len;

			const struct ast_node *p_key;
			
			if ((p_key = evaluate_ast(p_src->d.ldict.elements[2*i+0], pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
				return (ejson_error(p_error_handler, "failed to evaluate the key expression for the dictionary because\n"), NULL);
			if (p_key->cls != &AST_CLS_LITERAL_STRING)
				return (ejson_error(p_error_handler, "a key expression in a dictionary did not evaluate to a string\n"), NULL);

			len  = hashfnv(p_key->d.str.p_data, &hash);
			ukey = hash;
			while (p_iter != NULL) {
				if (p_iter->key == hash && !strcmp((const char *)(p_iter + 1), p_key->d.str.p_data))
					return ejson_error_null(p_error_handler, "attempted to add a key to a dictionary that already existed (%s)\n", p_key->d.str.p_data);
				pp_ipos = &(p_iter->p_children[ukey & DICTNODE_MASK]);
				p_iter = *pp_ipos;
				ukey >>= DICTNODE_BITS;
			}

			if ((p_iter = linear_allocator_alloc(p_alloc, sizeof(struct dictnode) + len + 1)) == NULL)
				return ejson_error_null(p_error_handler, "out of memory\n");
			p_iter->data2 = p_src->d.ldict.elements[2*i+1];
			p_iter->key  = hash;
			memset(p_iter->p_children, 0, sizeof(p_iter->p_children));
			memcpy((char *)(p_iter + 1), p_key->d.str.p_data, len + 1);

			*pp_ipos = p_iter;
		}

		struct ast_node *p_ret = linear_allocator_alloc(p_alloc, sizeof(struct ast_node));
		p_ret->cls                = &AST_CLS_READY_DICT;
		p_ret->d.rdict.pp_stack   = pp_stackx;
		p_ret->d.rdict.stack_size = stack_sizex;
		p_ret->d.rdict.nb_keys    = p_src->d.ldict.nb_keys;
		p_ret->d.rdict.p_root     = p_root;
		return p_ret;
	}

	if (p_src->cls == &AST_CLS_LISTVAL) {
		const struct ast_node *p_list;
		const struct ast_node *p_idx;
		const struct ast_node *p_ret;
		long long idx;
		size_t save = linear_allocator_save(p_alloc);
		if ((p_idx = evaluate_ast(p_src->d.listval.p_index, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return ejson_error_null(p_error_handler, "failed to evaluate the index expression for listval because\n");
		if (p_idx->cls != &AST_CLS_LITERAL_INT)
			return ejson_error_null(p_error_handler, "the index expression for listval did not evaluate to an integer\n");
		idx = p_idx->d.i;
		linear_allocator_restore(p_alloc, save);
		if ((p_list = evaluate_ast(p_src->d.listval.p_list, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return ejson_error_null(p_error_handler, "failed to evaluate the list expression for listval because\n");
		if (p_list->cls != &AST_CLS_LIST_GENERATOR)
			return ejson_error_null(p_error_handler, "the list expression for listval did not evaluate to a list\n");
		if ((p_ret = p_list->d.lgen.get_element(p_list, idx, p_alloc, p_error_handler)) == NULL)
			return ejson_error_null(p_error_handler, "failed to evaluate list element %d because\n", idx);
		return p_ret;
	}

	/* Function call */
	if (p_src->cls == &AST_CLS_CALL) {
		const struct ast_node *p_function;
		const struct ast_node *p_args;
		const struct ast_node **pp_stack2 = pp_stackx;

		if ((p_function = evaluate_ast(p_src->d.call.fn, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return ejson_error_null(p_error_handler, "failed to evaluate the function expression for call because\n");
		if (p_function->cls != &AST_CLS_FUNCTION)
			return ejson_error_null(p_error_handler, "the function expression for call did not evaluate to a function\n");
		if ((p_args = evaluate_ast(p_src->d.call.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return ejson_error_null(p_error_handler, "failed to evaluate the argument expression for call because\n");
		if (p_args->cls != &AST_CLS_LIST_GENERATOR)
			return ejson_error_null(p_error_handler, "the argument expression for call did not evaluate to a list\n");
		if (p_args->d.lgen.nb_elements != p_function->d.fn.nb_args)
			return ejson_error_null(p_error_handler, "the number of arguments supplied to function was incorrect (expected %u but got %u)\n", p_function->d.fn.nb_args, p_args->d.lgen.nb_elements);
		if (p_args->d.lgen.nb_elements) {
			unsigned i;
			if ((pp_stack2 = linear_allocator_alloc(p_alloc, sizeof(struct ast_node *) * (stack_sizex + p_args->d.lgen.nb_elements))) == NULL)
				return ejson_error_null(p_error_handler, "out of memory\n");
			if (stack_sizex)
				memcpy(pp_stack2, pp_stackx, stack_sizex * sizeof(struct ast_node *));
			for (i = 0; i < p_args->d.lgen.nb_elements; i++) {
				if ((pp_stack2[stack_sizex + p_args->d.lgen.nb_elements - 1 - i] = p_args->d.lgen.get_element(p_args, i, p_alloc, p_error_handler)) == NULL)
					return ejson_error_null(p_error_handler, "failed to evaluate list element %d because\n", i);
			}
		}

		return evaluate_ast(p_function->d.fn.node, pp_stack2, stack_sizex + p_args->d.lgen.nb_elements, p_alloc, p_error_handler);
	}

	/* Unary negation */
	if (p_src->cls == &AST_CLS_NEG) {
		struct ast_node *p_tmp2 = linear_allocator_alloc(p_alloc, sizeof(struct ast_node));
		size_t save                = linear_allocator_save(p_alloc);
		const struct ast_node *p_result;

		if ((p_result = evaluate_ast(p_src->d.binop.p_lhs, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return ejson_error_null(p_error_handler, "failed to evaluate the expression for unary negation because\n");

		if (p_result->cls == &AST_CLS_LITERAL_INT) {
			p_tmp2->cls = &AST_CLS_LITERAL_INT;
			p_tmp2->d.i = -p_result->d.i;
			linear_allocator_restore(p_alloc, save);
			return p_tmp2;
		}

		if (p_result->cls == &AST_CLS_LITERAL_FLOAT) {
			p_tmp2->cls = &AST_CLS_LITERAL_FLOAT;
			p_tmp2->d.f = -p_result->d.f;
			linear_allocator_restore(p_alloc, save);
			return p_tmp2;
		}

		return ejson_error_null(p_error_handler, "the expression for the unary negation operator did not evaluate to a numeric type\n");
	}

	/* Convert range into an accessor object */
	if (p_src->cls == &AST_CLS_RANGE) {
		struct ast_node       *p_result = linear_allocator_alloc(p_alloc, sizeof(struct ast_node));
		size_t                    save     = linear_allocator_save(p_alloc);
		struct lrange             lrange;
		const struct ast_node *p_args;

		if ((p_args = evaluate_ast(p_src->d.builtin.p_args, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL || p_args->cls != &AST_CLS_LIST_GENERATOR)
			return (ejson_error(p_error_handler, "range expects a list argument\n"), NULL);

		if (p_args->d.lgen.nb_elements < 1 || p_args->d.lgen.nb_elements > 3)
			return (ejson_error(p_error_handler, "range expects between 1 and 3 arguments\n"), NULL);

		if (p_args->d.lgen.nb_elements == 1) {
			const struct ast_node *p_first;

			if ((p_first = p_args->d.lgen.get_element(p_args, 0, p_alloc, p_error_handler)) == NULL || p_first->cls != &AST_CLS_LITERAL_INT)
				return (ejson_error(p_error_handler, "single argument range expects an integer number of items\n"), NULL);

			lrange.first     = 0;
			lrange.step_size = 1;
			lrange.numel     = p_first->d.i;
		} else if (p_args->d.lgen.nb_elements == 2) {
			const struct ast_node *p_first, *p_last;

			if  (   (p_first = p_args->d.lgen.get_element(p_args, 0, p_alloc, p_error_handler)) == NULL || p_first->cls != &AST_CLS_LITERAL_INT
			    ||  (p_last = p_args->d.lgen.get_element(p_args, 1, p_alloc, p_error_handler)) == NULL || p_last->cls != &AST_CLS_LITERAL_INT
			    )
				return (ejson_error(p_error_handler, "dual argument range expects an integer first and last index\n"), NULL);

			lrange.first     = p_first->d.i;
			lrange.step_size = (p_first->d.i > p_last->d.i) ? -1 : 1;
			lrange.numel     = ((p_first->d.i > p_last->d.i) ? (p_first->d.i - p_last->d.i) : (p_last->d.i - p_first->d.i)) + 1;
		} else {
			const struct ast_node *p_first, *p_step, *p_last;

			if  (   (p_first = p_args->d.lgen.get_element(p_args, 0, p_alloc, p_error_handler)) == NULL || p_first->cls != &AST_CLS_LITERAL_INT
			    ||  (p_step = p_args->d.lgen.get_element(p_args, 1, p_alloc, p_error_handler)) == NULL || p_step->cls != &AST_CLS_LITERAL_INT
			    ||  (p_last = p_args->d.lgen.get_element(p_args, 2, p_alloc, p_error_handler)) == NULL || p_last->cls != &AST_CLS_LITERAL_INT
			    ||  p_step->d.i == 0
			    ||  (p_step->d.i > 0 && (p_first->d.i > p_last->d.i))
			    ||  (p_step->d.i < 0 && (p_first->d.i < p_last->d.i))
			    )
				return (ejson_error(p_error_handler, "triple argument range expects an integer first, step and last values. step must be non-zero and have the correct sign for the range.\n"), NULL);

			lrange.first     = p_first->d.i;
			lrange.step_size = p_step->d.i;
			lrange.numel     = (p_last->d.i - p_first->d.i) / p_step->d.i + 1;
		}

		linear_allocator_restore(p_alloc, save);
		
		p_result->cls             = &AST_CLS_LIST_GENERATOR;
		p_result->d.lgen.get_element   = ast_list_generator_get_element;
		p_result->d.lgen.nb_elements   = lrange.numel;
		p_result->d.lgen.d.range.first = lrange.first;
		p_result->d.lgen.d.range.step  = lrange.step_size;
		return p_result;
	}

	if (p_src->cls == &AST_CLS_MAP) {
		const struct ast_node *p_function, *p_list;
		struct ast_node *p_tmp;

		if ((p_function = evaluate_ast(p_src->d.map.p_function, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL || p_function->cls != &AST_CLS_FUNCTION || p_function->d.fn.nb_args != 1)
			return (ejson_error(p_error_handler, "map expects a function argument that takes one argument\n"), NULL);

		if ((p_list = evaluate_ast(p_src->d.map.p_input_list, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL || p_list->cls != &AST_CLS_LIST_GENERATOR)
			return (ejson_error(p_error_handler, "map expected a list argument following the function\n"), NULL);

		p_tmp = linear_allocator_alloc(p_alloc, sizeof(struct ast_node));
		p_tmp->cls                     = &AST_CLS_LIST_GENERATOR;
		p_tmp->d.lgen.pp_stack         = pp_stackx;
		p_tmp->d.lgen.stack_size       = stack_sizex;
		p_tmp->d.lgen.d.map.p_function = p_function;
		p_tmp->d.lgen.d.map.p_key      = p_list;
		p_tmp->d.lgen.get_element      = ast_list_generator_map;
		p_tmp->d.lgen.nb_elements      = p_list->d.lgen.nb_elements;
		return p_tmp;
	}

	/* Ops */
	if (p_src->cls == &AST_CLS_ADD || p_src->cls == &AST_CLS_SUB || p_src->cls == &AST_CLS_MUL || p_src->cls == &AST_CLS_MOD) {
		const struct ast_node *p_lhs;
		const struct ast_node *p_rhs;
		struct ast_node *p_ret;
		size_t save;
		
		p_ret       = linear_allocator_alloc(p_alloc, sizeof(struct ast_node));
		
		save        = linear_allocator_save(p_alloc);

		if ((p_lhs = evaluate_ast(p_src->d.binop.p_lhs, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return (ejson_error(p_error_handler, "could not evaluate binop lhs\n"), NULL);
		if ((p_rhs = evaluate_ast(p_src->d.binop.p_rhs, pp_stackx, stack_sizex, p_alloc, p_error_handler)) == NULL)
			return (ejson_error(p_error_handler, "could not evaluate binop rhs\n"), NULL);

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

			linear_allocator_restore(p_alloc, save);

			p_ret->cls = &AST_CLS_LITERAL_FLOAT;

			if (p_src->cls == &AST_CLS_ADD) {
				p_ret->d.f = lhs + rhs;
			} else if (p_src->cls == &AST_CLS_SUB) {
				p_ret->d.f = lhs - rhs;
			} else if (p_src->cls == &AST_CLS_MUL) {
				p_ret->d.f = lhs * rhs;
			} else if (p_src->cls == &AST_CLS_MOD) {
				p_ret->d.f = fmod(lhs, rhs);
			} else {
				abort();
			}

			return p_ret;
		}
		
		if (p_lhs->cls == &AST_CLS_LITERAL_INT && p_rhs->cls == &AST_CLS_LITERAL_INT) {
			long long lhs, rhs;

			lhs = p_lhs->d.i;
			rhs = p_rhs->d.i;

			linear_allocator_restore(p_alloc, save);

			p_ret->cls = &AST_CLS_LITERAL_INT;

			if (p_src->cls == &AST_CLS_ADD) {
				p_ret->d.i = lhs + rhs;
			} else if (p_src->cls == &AST_CLS_SUB) {
				p_ret->d.i = lhs - rhs;
			} else if (p_src->cls == &AST_CLS_MUL) {
				p_ret->d.i = lhs * rhs;
			} else if (p_src->cls == &AST_CLS_MOD) {
				lhs = lhs % rhs;
				p_ret->d.i = lhs + ((lhs < 0) ? rhs : 0);
			} else {
				abort();
			}

			return p_ret;
		}

		ejson_error(p_error_handler, "the types given for binary operator %s were invalid\n", p_src->cls->p_name);
		return NULL;
	}

	fprintf(stderr, "what?\n");
	p_src->cls->debug_print(p_src, stderr, 10);
	fprintf(stderr, "endwhat?\n");
	abort();

	return NULL;
}


static int jnode_list_get_element(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, unsigned idx) {
	struct execution_context *ec   = ctx;
	const struct ast_node *p_tmp;

	if ((p_tmp = ec->p_object->d.lgen.get_element(ec->p_object, idx, p_alloc, ec->p_error_handler)) == NULL)
		return -1;

	return to_jnode(p_dest, p_tmp, p_alloc, ec->p_error_handler);
}

static
int
enumerate_dict_keys2
	(jdict_enumerate_fn               *p_fn
	,const struct ejson_error_handler *p_error_handler
	,const struct dictnode            *p_node
	,const struct ast_node           **pp_stack
	,unsigned                          stack_size
	,struct linear_allocator          *p_alloc
	,void                             *p_userctx
	) {
	unsigned               i;
	struct jnode           tmp;
	const struct ast_node *p_eval;
	size_t                 save;
	int                    ret;

	for (i = 0; i < DICTNODE_NUM; i++) {
		if (p_node->p_children[i] != NULL) {
			int r;
			if ((r = enumerate_dict_keys2(p_fn, p_error_handler, p_node->p_children[i], pp_stack, stack_size, p_alloc, p_userctx)) != 0) {
				return r;
			}
		}
	}

	/* This is very important. The lifetime of the objects provided in the
	 * enumeration is only for the life of the callback. The lifetime of the
	 * string, however, persists. */
	save = linear_allocator_save(p_alloc);

	if ((p_eval = evaluate_ast(p_node->data2, pp_stack, stack_size, p_alloc, p_error_handler)) == NULL)
		return ejson_error(p_error_handler, "could not get dictionary value\n");

	if (to_jnode(&tmp, p_eval, p_alloc, p_error_handler))
		return ejson_error(p_error_handler, "could not convert dictionary data\n");

	ret = p_fn(&tmp, (const char *)(p_node + 1), p_userctx);

	linear_allocator_restore(p_alloc, save);

	return ret;
}

static int enumerate_dict_keys(jdict_enumerate_fn *p_fn, void *p_ctx, struct linear_allocator *p_alloc, void *p_userctx) {
	struct execution_context *ec = p_ctx;
	if (ec->p_object->d.rdict.p_root != NULL)
		return enumerate_dict_keys2(p_fn, ec->p_error_handler, ec->p_object->d.rdict.p_root, ec->p_object->d.rdict.pp_stack, ec->p_object->d.rdict.stack_size, p_alloc, p_userctx);
	return 0;
}

static int to_jnode(struct jnode *p_node, const struct ast_node *p_ast, struct linear_allocator *p_alloc, const struct ejson_error_handler *p_error_handler) {
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

	if (p_ast->cls == &AST_CLS_READY_DICT) {
		struct execution_context *ec = linear_allocator_alloc(p_alloc, sizeof(struct execution_context));
		ec->p_error_handler = p_error_handler;
		ec->p_object        = p_ast;

		p_node->cls               = JNODE_CLS_DICT;
		p_node->d.dict.nb_keys    = p_ast->d.rdict.nb_keys;
		p_node->d.dict.ctx        = ec;
		p_node->d.dict.get_by_key = NULL; /* TODO */
		p_node->d.dict.enumerate  = enumerate_dict_keys;
		return 0;
	}

	if (p_ast->cls == &AST_CLS_LIST_GENERATOR) {
		struct execution_context *ec = linear_allocator_alloc(p_alloc, sizeof(struct execution_context));
		ec->p_error_handler          = p_error_handler;
		ec->p_object                 = p_ast;
		p_node->cls                  = JNODE_CLS_LIST;
		p_node->d.list.ctx           = ec;
		p_node->d.list.nb_elements   = p_ast->d.lgen.nb_elements;
		p_node->d.list.get_elemenent = jnode_list_get_element;
		return 0;
	}

	return ejson_error(p_error_handler, "the given root node class (%s) cannot be represented using JSON\n", p_ast->cls->p_name);
}

int parse_document(struct jnode *p_node, struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, struct ejson_error_handler *p_error_handler) {
	const struct ast_node *p_obj;
	const struct ast_node *p_root;
	const struct token *p_token;

	while ((p_token = tok_peek(p_tokeniser)) != NULL && p_token->cls == &TOK_DEFINE) {
		const struct istring *p_key;
		void **pp_obj;
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL) /* skip over define */
			return ejson_error(p_error_handler, "no tokens to read\n");

		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL || p_token->cls != &TOK_IDENTIFIER)
			return ejson_error(p_error_handler, "expected an identifier token after '@'\n");
		if ((p_key = istrings_add(&(p_workspace->strings), p_token->t.strident.str)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		if ((pp_obj = pdict_get(&(p_workspace->workspace), (uintptr_t)(p_key->cstr))) != NULL)
			return ejson_error(p_error_handler, "cannot redefine variable '%s'\n", p_token->t.strident.str);
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL || p_token->cls != &TOK_EQ)
			return ejson_error(p_error_handler, "expected '='\n");
		if ((p_obj = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
			return ejson_error(p_error_handler, "expected an expression\n");
		if ((p_token = tok_read(p_tokeniser, p_error_handler)) == NULL || p_token->cls != &TOK_SEMI)
			return ejson_error(p_error_handler, "expected ';'\n");
		if (pdict_set(&(p_workspace->workspace), (uintptr_t)(p_key->cstr), (void *)p_obj))
			return ejson_error(p_error_handler, "out of memory\n");
	}
	if ((p_obj = expect_expression(p_workspace, p_tokeniser, p_error_handler)) == NULL)
		return ejson_error(p_error_handler, "failed to parse root document expression because\n");
	if ((p_token = tok_peek(p_tokeniser)) != NULL)
		return ejson_error(p_error_handler, "expected no more tokens - got '%s'\n", p_token->cls->name);
	if ((p_root = evaluate_ast(p_obj, NULL, 0, &(p_workspace->alloc), p_error_handler)) == NULL)
		return ejson_error(p_error_handler, "failed to evaluate root document expression because\n");
	if (to_jnode(p_node, p_root, &(p_workspace->alloc), p_error_handler))
		return ejson_error(p_error_handler, "failed to convert the evaluated root document to expression to a json node because\n");
	return 0;
}


#if EJSON_TEST||1

#include "json_simple_load.h"
#include "json_iface_utils.h"
#include <stdarg.h>

static int unexpected_fail(const char *p_fmt, ...) {
	va_list args;
	fprintf(stderr, "unexpected error: ");
	va_start(args, p_fmt);
	vfprintf(stderr, p_fmt, args);
	va_end(args);
	abort();
	return -1;
}

static void on_parser_error(void *p_context, const struct token_pos_info *p_location, const char *p_format, va_list args) {
	if (p_location != NULL) {
		const char *p_line = p_location->p_line;
		fprintf(stderr, "  on line %d character %d: ", p_location->line_nb, p_location->char_pos);
		vfprintf(stderr, p_format, args);
		printf("    '");
		while (*p_line != '\0' && *p_line != '\n' && *p_line != '\r')
			printf("%c", *p_line++);
		printf("'\n");
		printf("    %*s^\n", p_location->char_pos, "");
	} else {
		fprintf(stderr, "  ");
		vfprintf(stderr, p_format, args);
	}
}

int run_test(const char *p_ejson, const char *p_ref, const char *p_name) {
	struct jnode dut;
	struct tokeniser t;
	struct evaluation_context ws;
	struct ejson_error_handler err;
	int d;
	
	err.p_context = NULL;
	err.on_parser_error = on_parser_error;

	if (tokeniser_start(&t, (char *)p_ejson))
		return unexpected_fail("could not init tokeniser\n");

	if (evaluation_context_init(&ws))
		return unexpected_fail("could not init workspace\n");

	if (parse_document(&dut, &ws, &t, &err)) {
		if (p_ref != NULL) {
			fprintf(stderr, "FAILED: test '%s' failed due to above messages.\n", p_name);
			return -1;
		} else {
			printf("PASSED: xtest '%s'\n", p_name);
		}
	} else if (p_ref == NULL) {
		printf("FAILED: xtest '%s' generated a node.\n", p_name);
		return -1;
	}

	if (p_ref != NULL) {
		struct jnode ref;
		struct linear_allocator a1;
		struct linear_allocator a2;
		struct linear_allocator a3;

		a1.pos = 0;
		a2.pos = 0;
		a3.pos = 0;

		if (parse_json(&ref, &a1, &a2, p_ref))
			return unexpected_fail("could not parse reference JSON:\n  %s\n", p_ref);

		if ((d = are_different(&dut, &ref, &a3)) < 0) {
			return unexpected_fail("are_different failed to execute\n");
		}
		
		if (d) {
			fprintf(stderr, "test '%s' failed:\n", p_name);
			fprintf(stderr, "  Reference:\n    ");
			jnode_print(&ref, &a2, 4);
			fprintf(stderr, "  DUT:\n    ");
			jnode_print(&dut, &a2, 4);
			return -1;
		}

		printf("PASSED: test '%s'.\n", p_name);
	}

	return 0;
}

int main(int argc, char *argv[]) {
	run_test
		("\"hello world\""
		,"\"hello world\""
		,"string objects"
		);
	run_test
		("null"
		,"null"
		,"positive null object"
		);
	run_test
		("true"
		,"true"
		,"positive true boolean object"
		);
	run_test
		("false"
		,"false"
		,"positive false boolean object"
		);
	run_test
		("5"
		,"5"
		,"positive int objects"
		);
	run_test
		("5.0"
		,"5.0"
		,"positive real objects"
		);
	run_test
		("-5"
		,"-5"
		,"negative int objects"
		);
	run_test
		("-5.0"
		,"-5.0"
		,"negative real objects"
		);
	run_test
		("[]"
		,"[]"
		,"empty list"
		);
	run_test
		("{}"
		,"{}"
		,"empty dictionary"
		);
	run_test
		("{\"hello1\": null}"
		,"{\"hello1\": null}"
		,"dictionary with a single null key"
		);
	run_test
		("{\"hello1\": {\"uhh\": null, \"thing\": 100}}"
		,"{\"hello1\": {\"uhh\": null, \"thing\": 100}}"
		,"dictionary nesting"
		);
	run_test
		("5+5+5"
		,"15"
		,"int additive expression 1"
		);
	run_test
		("5+5-5"
		,"5"
		,"int additive expression 2"
		);
	run_test
		("5-5-5"
		,"-5"
		,"int additive expression 3"
		);
	run_test
		("5+5.0-5"
		,"5.0"
		,"promotion additive expression 1"
		);
	run_test
		("5-5.0-5"
		,"-5.0"
		,"promotion additive expression 2"
		);
	run_test
		("1.0+5.0"
		,"6.0"
		,"float additive expression 1"
		);
	run_test
		("[1,-2,3.4,-4.5,5.6e2,-7.8e-2]"
		,"[1,-2,3.4,-4.5,5.6e2,-7.8e-2]"
		,"numeric objects in a list"
		);
	run_test
		("range[5]"
		,"[0, 1, 2, 3, 4]"
		,"range generator simple"
		);
	run_test
		("range[6,11]"
		,"[6, 7, 8, 9, 10, 11]"
		,"range generator from-to"
		);
	run_test
		("range[6,-11]"
		,"[6,5,4,3,2,1,0,-1,-2,-3,-4,-5,-6,-7,-8,-9,-10,-11]"
		,"range generator from-to reverse"
		);
	run_test
		("range[6,2,10]"
		,"[6,8,10]"
		,"range generator from-step-to"
		);
	run_test
		("range[6,-3,-9]"
		,"[6,3,0,-3,-6,-9]"
		,"range generator from-step-to 2"
		);
	run_test
		("call func() 1 []"
		,"1"
		,"calling a function that takes no arguments and returns 1"
		);
	run_test
		("call func(x) x [55]"
		,"55"
		,"calling a function that takes one argument and returns its value"
		);
	run_test
		("call func(x) [x] [55]"
		,"[55]"
		,"calling a function that takes one argument and returns its value "
		 "in a list"
		);
	run_test
		("call func(x, y, z) x * y + z [3, 5, 7]"
		,"22"
		,"calling a function that multiplies the first two arguments and "
		 "adds the third (test order of arguments on stack)"
		);
	run_test
		("call func(x, y) call func(z) x - y * z [3] [5, 7]"
		,"-32"
		,"calling a function that contains another function (nested stack "
		 "access test)"
		);
	run_test
		("call func(x) call func(y) call func(z) x - y * z [3] [5] [7]"
		,"-32"
		,"triple nested function call"
		);
	run_test
		("call func(y) call y [] [func() 111]"
		,"111"
		,"calling a function that calls the given function passed as an "
		 "argument"
		);
	run_test
		("define x = 11; define y = 7; x * y"
		,"77"
		,"use a workspace variable"
		);
	run_test
		("define x = func(z) z*z; define y = 7; call x [y]"
		,"49"
		,"use a workspace variable as a function"
		);
	run_test
		("listval [1,2,3] 1"
		,"2"
		,"extraction of a value from a literal list"
		);
	run_test
		("listval range[10] 4"
		,"4"
		,"extracting an element of a generated list"
		);
	run_test
		("call func(x) [1, x, 2, 3] [50]"
		,"[1, 50, 2, 3]"
		,"a function that returns a 4 element list with the second element "
		 "equal to the argument"
		);
	run_test
		("call func(x, y, z) x * y + z call func(x) [3, 5, x] [7]"
		,"22"
		,"calling a function where the arguments are the list produced by "
		 "calling another function"
		);
	run_test
		("call func(x, y, z) x * y + z range[4, 6]"
		,"26"
		,"calling a function where the arguments are the list produced by "
		 "calling range"
		);
	run_test
		("listval call func(x) [1, x, 2, 3] [50] 1"
		,"50"
		,"extracting an element of the list returned by a function"
		);
	run_test
		("map func(x) [1, x, x*x] [1,2,3]"
		,"[[1,1,1],[1,2,4],[1,3,9]]"
		,"map operation basics"
		);
	run_test
		("map func(x) listval [\"a\",\"b\",\"c\",\"d\",\"e\"] x%5 range[-2,1,8]"
		,"[\"d\",\"e\",\"a\",\"b\",\"c\",\"d\",\"e\",\"a\",\"b\",\"c\",\"d\"]"
		,"map over a range basics"
		);
	run_test
		("map func(x) range[x] range[0,5]"
		,"[[],[0],[0,1],[0,1,2],[0,1,2,3],[0,1,2,3,4]]"
		,"use map to generate a list of incrementing ranges over a range"
		);
	run_test
		("range call func() [1,2,9] []"
		,"[1,3,5,7,9]"
		,"call range with arguments given by the result of a function call"
		);
	run_test
		("format[\"hello\"]"
		,"\"hello\""
		,"test format with no arguments"
		);
	run_test
		("format[\"hello %%\"]"
		,"\"hello %\""
		,"format escapeing %% properly"
		);
	run_test
		("format[\"hello %d %d\", 1, 2000]"
		,"\"hello 1 2000\""
		,"test format with two integer arguments"
		);
	run_test
		("format[\"%d-%s.wav\", 36, \"c\"]"
		,"\"36-c.wav\""
		,"test format with an integer and string argument"
		);
	run_test
		("map func(x) format[\"%03d-%s.wav\", x, listval [\"c\", \"d\", \"e\"] x%3] range[36,40]"
		,"[\"036-c.wav\", \"037-d.wav\", \"038-e.wav\", \"039-c.wav\", \"040-d.wav\"]"
		,"test using format to generate mapped strings"
		);
	run_test
		("call listval [func(x) x+1, func(x) x+2, func(x) x+3] 1 [10]"
		,"12"
		,"test calling a function that is in a list of functions"
		);

	/* Expected fail tests */
	run_test
		("1+\"a\""
		,NULL
		,"arguments to operators must be integers of floats"
		);
	run_test
		("-\"a\""
		,NULL
		,"arguments to unary negate must be an integer or a float"
		);
	run_test
		("{1: null}"
		,NULL
		,"dictionary keys must evaluate to strings"
		);
	run_test
		("func(x) x"
		,NULL
		,"the evaluation of the root node cannot be a function"
		);
	run_test
		("call func() 1 [1, 2]"
		,NULL
		,"call a function with incorrect number of arguments (0)"
		);
	run_test
		("call func(x) x [1, 2]"
		,NULL
		,"call a function with incorrect number of arguments (1)"
		);
	run_test
		("9 + ("
		,NULL
		,"expect expression after ("
		);
	run_test
		("9 + 8 * k"
		,NULL
		,"failure to parse rhs due to identifier not existing"
		);
	run_test
		("(1,"
		,NULL
		,"failure because expect )"
		);
	run_test
		("["
		,NULL
		,"failure because need more tokens"
		);
	run_test
		("[1,ggg"
		,NULL
		,"failure because need more tokens"
		);
	run_test
		("func"
		,NULL
		,"failure because need more tokens"
		);
	run_test
		("func sadsa 1"
		,NULL
		,"func expects open parenthesis"
		);
	run_test
		("func (sadsa 1"
		,NULL
		,"func expects comma or close parenthesis"
		);
	run_test
		("func (sadsa, 1"
		,NULL
		,"func arguments must be literals"
		);
	run_test
		("func (sadsa 1)"
		,NULL
		,"func expects a function body"
		);
	run_test
		("func (sadsa 1) ("
		,NULL
		,"func cannot parse function body"
		);
	run_test
		("func (sadsa, sadsa) 1"
		,NULL
		,"func arguments must not alias each other"
		);
	run_test
		("define sadsa = 1; func (sadsa) 1"
		,NULL
		,"func arguments must not alias workspace variables"
		);
	run_test
		("listval"
		,NULL
		,"listval no tokens for first expression"
		);
	run_test
		("listval [1"
		,NULL
		,"listval could not parse first expression"
		);
	run_test
		("listval 1"
		,NULL
		,"listval no tokens for second expression"
		);
	run_test
		("listval 1 [1"
		,NULL
		,"listval could not parse second expression"
		);
	run_test
		("map"
		,NULL
		,"map no tokens for first expression"
		);
	run_test
		("map [1"
		,NULL
		,"map could not parse first expression"
		);
	run_test
		("map 1"
		,NULL
		,"map no tokens for second expression"
		);
	run_test
		("map 1 [1"
		,NULL
		,"map could not parse second expression"
		);


#if 0
	/* This is more an experiment than a test. */
	run_test
		("define names = [\"c\", \"c#\", \"d\", \"d#\", \"e\", \"f\", \"f#\", \"g\", \"g#\", \"a\", \"a#\", \"b\"];"
		 "define filename_gen = func(p, y) format[\"%s/%03d-%s.wav\", p, y, listval names y % 12];"
		 "map "
		 "  func(x) "
		 "    {\"attack-samples\": [call filename_gen [\"Bourdon 8/A0\", x]]"
		 "    }"
		 "  range[36,96]"
		,"0"
		,"func cannot parse function body"
		);
#endif

	/* FIXME: something is broken i think with the test comparison function. */
	run_test
		("define notes=[\"a\",\"b\",\"c\"];\n"
		 "map func(x)\n"
		 "  {\"name\": listval notes x % 3, \"id\": x} range[0,5]\n"
		,"[{\"id\":0,\"name\":\"a\"}"
		 ",{\"id\":1,\"name\":\"b\"}"
		 ",{\"id\":2,\"name\":\"c\"}"
		 ",{\"id\":3,\"name\":\"a\"}"
		 ",{\"id\":4,\"name\":\"b\"}"
		 ",{\"id\":2,\"name\":\"c\"}"
		 "]"
		,"use map to generate a list of dicts"
		);

	//run_test("{}", "{}", "empty dict");

	return EXIT_SUCCESS;
}
#endif

