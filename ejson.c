
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

#define MAX_TOK_STRING (4096)

#ifndef ejson_error
#ifdef EJSON_NO_ERROR_MESSAGES
#define ejson_error(p_handler_, p_format, ...) (-1)
#endif
#endif

#ifndef ejson_error
static int ejson_error_fn(struct ejson_error_handler *p_handler, const char *p_format, ...) {
	if (p_handler != NULL) {
		va_list args;
		va_start(args, p_format);
		p_handler->on_parser_error(p_handler->p_context, p_format, args);
		va_end(args);
	}
	return -1;
}
#define ejson_error ejson_error_fn
#endif

struct ast_node;

struct ast_cls {
	const char  *p_name;
	int        (*to_jnode)(struct ast_node *p_node, struct jnode *p_dest);
	void       (*debug_print)(const struct ast_node *p_node, FILE *p_f, unsigned depth);
};

#define DEF_AST_CLS(name_, to_jnode_fn_, debug_print_fn_) \
	static const struct ast_cls name_ = { #name_, to_jnode_fn_, debug_print_fn_ }

struct ev_ast_node;

struct ast_node {
	const struct ast_cls *cls;
	union {
		long long     i; /* AST_CLS_LITERAL_INT, AST_CLS_LITERAL_BOOL */
		double        f; /* AST_CLS_LITERAL_FLOAT */
		struct {
			uint_fast32_t    len;
			uint_fast32_t    hash;
			const char      *p_data;
		} str; /* AST_CLS_LITERAL_STRING */
		struct {
			struct ast_node *p_lhs;
			struct ast_node *p_rhs;
		} binop; /* AST_CLS_STRCAT, AST_CLS_NEG*, AST_CLS_ADD*, AST_CLS_SUB*, AST_CLS_MUL*, AST_CLS_DIV*, AST_CLS_MOD* */
		struct {
			struct ast_node **elements;
			uint_fast32_t     nb_elements;
		} llist; /* AST_CLS_LITERAL_LIST */
		struct {
			int             (*get_element)(struct ev_ast_node *p_dest, const struct ev_ast_node *p_list, unsigned element, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler);
			union {
				struct {
					long long first;
					long long step;
				} range;
				struct {
					struct ev_ast_node *p_function;
					struct ev_ast_node *p_key;
				} map;
				struct {
					struct ast_node **pp_values;
				} literal;
			} d;
			uint_fast32_t     nb_elements;
		} lgen; /* AST_CLS_LIST_GENERATOR */
		struct {
			struct ast_node **elements; /* 2*nb_keys - [key, value] */
			uint_fast32_t     nb_keys;
		} ldict; /* AST_CLS_LITERAL_DICT */
		struct {
			struct ast_node  *p_function;
			struct ast_node  *p_list;
		} lmap;
		struct {
			struct ast_node *node;
			unsigned         nb_args;
		} fn; /* AST_CLS_FUNCTION */
		struct {
			struct ast_node  *fn;
			struct ast_node  *p_args;
		} call;
		struct {
			struct ast_node  *p_args;
		} builtin; /* AST_CLS_RANGE */
		struct {
			struct ast_node *p_function;
			struct ast_node *p_input_list;
		} map;
		struct {
			struct ast_node *p_list;
			struct ast_node *p_index;
		} listval;

	} d;

};

struct dictnode;

struct ev_ast_node {
	struct ast_node      data;

	union {
		struct {
			unsigned         nb_keys;
			struct dictnode *p_root;
		} dict;

	} d;

	/* These may be undefined for certain classes of data. */
	struct ev_ast_node **pp_stack;
	unsigned             stack_size;
};


#define DICTNODE_BITS (2)
#define DICTNODE_NUM  (1u << DICTNODE_BITS)
#define DICTNODE_MASK (DICTNODE_NUM - 1u)

struct dictnode {
	struct ast_node    *data;
	struct dictnode    *p_children[DICTNODE_NUM];
	uint_fast32_t       key;
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
	const struct tok_def *cls;
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
//DEF_AST_CLS(AST_CLS_CALL,            NULL, debug_print_call);
DEF_AST_CLS(AST_CLS_STACKREF,        NULL, debug_print_int_like);
DEF_AST_CLS(AST_CLS_LIST_GENERATOR,  NULL, debug_list_generator);


#if 0
#define AST_CLS_REFERENCE      (23) /* Push the segment to the current offset for all sub nodes */
#define AST_CLS_STACKREF       (24)
#define AST_CLS_FUNCTION       (25)
#define AST_CLS_NAMED_FUNCTION (26)
#define AST_CLS_CALL           (27)
#endif

TOK_DECL(TOK_INT,        -1, 0, NULL); /* 13123 */
TOK_DECL(TOK_FLOAT,      -1, 0, NULL); /* 13123.0 | .123 */
TOK_DECL(TOK_ADD,         1, 0, &AST_CLS_ADD); /* + */
TOK_DECL(TOK_SUB,         1, 0, &AST_CLS_SUB); /* - */
TOK_DECL(TOK_MUL,         2, 0, &AST_CLS_MUL); /* * */
TOK_DECL(TOK_DIV,         2, 0, &AST_CLS_DIV); /* / */
TOK_DECL(TOK_MOD,         2, 0, &AST_CLS_MOD); /* % */
TOK_DECL(TOK_EXP,         3, 1, &AST_CLS_EXP); /* ^ */
TOK_DECL(TOK_STRING,     -1, 0, NULL); /* "afasfasf" */
TOK_DECL(TOK_NULL,       -1, 0, NULL); /* null */
TOK_DECL(TOK_TRUE,       -1, 0, NULL); /* true */
TOK_DECL(TOK_FALSE,      -1, 0, NULL); /* false */

TOK_DECL(TOK_RANGE,      -1, 0, NULL); /* range */
TOK_DECL(TOK_FUNC,       -1, 0, NULL); /* func */
TOK_DECL(TOK_CALL,       -1, 0, NULL); /* call */
TOK_DECL(TOK_DEFINE,     -1, 0, NULL); /* define */
TOK_DECL(TOK_LISTVAL,    -1, 0, NULL); /* listval */
TOK_DECL(TOK_MAP,        -1, 0, NULL); /* map */

TOK_DECL(TOK_IDENTIFIER, -1, 0, NULL); /* afasfasf */
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
TOK_DECL(TOK_EOF,        -1, 0, NULL); /* EOF */

struct tokeniser {
	unsigned      lp; /* line position, character position */
	char         *buf;

	struct token  cur;
};

static void token_print(struct token *p_token) {
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

int tokeniser_next(struct tokeniser *p_tokeniser) {
	char c;
	int in_comment;

	if (p_tokeniser->cur.cls == &TOK_EOF) {
		return -1;
	}

	/* eat whitespace and comments */
	c          = *(p_tokeniser->buf++);
	in_comment = (c == '#');
	while (c == ' ' || c == '\t' || c == '\r' || c == '\n' || in_comment) {
		char nc = *(p_tokeniser->buf++);
		if (c == '\r' || c == '\n') {
			in_comment = 0;
			p_tokeniser->lp++;
			if (c == '\r' && nc == '\n') /* windows style */
				nc = *(p_tokeniser->buf++);
		}
		if (nc == '#') {
			in_comment = 1;
		}
		c = nc;
	}

	/* nothing left */
	if (c == '\0') {
		p_tokeniser->cur.cls = &TOK_EOF;
		return 0;
	}

	if (c == '#') abort();
	assert(c != '#');

	/* quoted string */
	if (c == '\"') {
		p_tokeniser->cur.cls           = &TOK_STRING;
		p_tokeniser->cur.t.strident.len = 0;
		while ((c = *(p_tokeniser->buf++)) != '\"') {
			if (c == '\0')
				return -1; /* EOF */

			if (c == '\n' || c == '\r')
				return -1; /* EOL */

			if (c == '\\') {
				c = *(p_tokeniser->buf++);
				if (c == '\\')
					c = '\\';
				else if (c == '\"')
					c = '\"';
				else
					return -1; /* invalid escape */
			}

			p_tokeniser->cur.t.strident.str[p_tokeniser->cur.t.strident.len++] = c;
		}
		p_tokeniser->cur.t.strident.str[p_tokeniser->cur.t.strident.len] = '\0';
	} else if
	    (   (c == '.' && p_tokeniser->buf[0] >= '0' && p_tokeniser->buf[0] <= '9')
	    ||  (c >= '0' && c <= '9')
	    ) {
		unsigned long long ull = 0;
		p_tokeniser->cur.cls = &TOK_INT;
		while (c >= '0' && c <= '9') {
			ull *= 10;
			ull += c - '0';
			c    = *(p_tokeniser->buf++);
		}
		if (c == '.') {
			double frac = 0.1;
			p_tokeniser->cur.cls    = &TOK_FLOAT;
			p_tokeniser->cur.t.tflt = ull;
			c = *(p_tokeniser->buf++);
			if (c < '0' || c > '9')
				return -1; /* 13413. is not a valid literal */
			while (c >= '0' && c <= '9') {
				p_tokeniser->cur.t.tflt += (c - '0') * frac;
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
				return -1;
			eval = c - '0';
			c = *(p_tokeniser->buf++);
			while (c >= '0' && c <= '9') {
				eval = eval * 10 + (c - '0');
				c = *(p_tokeniser->buf++);
			}
			eval = (eneg) ? -eval : eval;
			if (p_tokeniser->cur.cls == &TOK_FLOAT) {
				p_tokeniser->cur.t.tflt *= pow(10.0, eval);
			} else {
				p_tokeniser->cur.cls = &TOK_FLOAT;
				p_tokeniser->cur.t.tflt = ull * pow(10.0, eval);
			}
		}
		if (p_tokeniser->cur.cls == &TOK_INT) {
			p_tokeniser->cur.t.tint = ull;
		}
		p_tokeniser->buf--;
	} else if
	    (   (c >= 'a' && c <= 'z')
	    ||  (c >= 'A' && c <= 'Z')
	    ) {
		char nc = *(p_tokeniser->buf);
		p_tokeniser->cur.t.strident.str[0] = c;
		p_tokeniser->cur.t.strident.len = 1;
		while
		    (   (nc >= 'a' && nc <= 'z')
		    ||  (nc >= 'A' && nc <= 'Z')
		    ||  (nc >= '0' && nc <= '9')
		    ||  (nc == '_')
		    ) {
			p_tokeniser->cur.t.strident.str[p_tokeniser->cur.t.strident.len++] = nc;
			nc = *(++p_tokeniser->buf);
		}
		p_tokeniser->cur.t.strident.str[p_tokeniser->cur.t.strident.len] = '\0';

		if (!strcmp(p_tokeniser->cur.t.strident.str, "true")) {
			p_tokeniser->cur.cls = &TOK_TRUE;
		} else if (!strcmp(p_tokeniser->cur.t.strident.str, "false")) {
			p_tokeniser->cur.cls = &TOK_FALSE;
		} else if (!strcmp(p_tokeniser->cur.t.strident.str, "null")) {
			p_tokeniser->cur.cls = &TOK_NULL;
		} else if (!strcmp(p_tokeniser->cur.t.strident.str, "range")) {
			p_tokeniser->cur.cls = &TOK_RANGE;
		} else if (!strcmp(p_tokeniser->cur.t.strident.str, "call")) {
			p_tokeniser->cur.cls = &TOK_CALL;
		} else if (!strcmp(p_tokeniser->cur.t.strident.str, "func")) {
			p_tokeniser->cur.cls = &TOK_FUNC;
		} else if (!strcmp(p_tokeniser->cur.t.strident.str, "define")) {
			p_tokeniser->cur.cls = &TOK_DEFINE;
		} else if (!strcmp(p_tokeniser->cur.t.strident.str, "listval")) {
			p_tokeniser->cur.cls = &TOK_LISTVAL;
		} else if (!strcmp(p_tokeniser->cur.t.strident.str, "map")) {
			p_tokeniser->cur.cls = &TOK_MAP;
		} else {
			p_tokeniser->cur.cls = &TOK_IDENTIFIER;
		}

	} else if (c == '=') {
		p_tokeniser->cur.cls = &TOK_EQ;
	} else if (c == '[') {
		p_tokeniser->cur.cls = &TOK_LSQBR;
	} else if (c == ']') {
		p_tokeniser->cur.cls = &TOK_RSQBR;
	} else if (c == '{') {
		p_tokeniser->cur.cls = &TOK_LBRACE;
	} else if (c == '}') {
		p_tokeniser->cur.cls = &TOK_RBRACE;
	} else if (c == '(') {
		p_tokeniser->cur.cls = &TOK_LPAREN;
	} else if (c == ')') {
		p_tokeniser->cur.cls = &TOK_RPAREN;
	} else if (c == ',') {
		p_tokeniser->cur.cls = &TOK_COMMA;
	} else if (c == ':') {
		p_tokeniser->cur.cls = &TOK_COLON;
	} else if (c == ';') {
		p_tokeniser->cur.cls = &TOK_SEMI;
	} else if (c == '%') {
		p_tokeniser->cur.cls = &TOK_MOD;
	} else if (c == '/') {
		p_tokeniser->cur.cls = &TOK_DIV;
	} else if (c == '*') {
		p_tokeniser->cur.cls = &TOK_MUL;
	} else if (c == '^') {
		p_tokeniser->cur.cls = &TOK_EXP;
	} else if (c == '-') {
		p_tokeniser->cur.cls = &TOK_SUB;
	} else if (c == '+') {
		p_tokeniser->cur.cls = &TOK_ADD;
	} else {
		printf("TOKENISATION ERROR %c\n", c);
		return -1;
	}

	return 0;
}

void tokeniser_start(struct tokeniser *p_tokeniser, char *buf) {
	p_tokeniser->buf = buf;
	p_tokeniser->lp = 1;
	p_tokeniser->cur.cls = NULL;
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

struct ast_node *expect_expression(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser);

struct ast_node *parse_primary(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser) {
	struct ast_node *p_temp_nodes[8192];
	struct ast_node *p_ret = NULL;

	if (p_tokeniser->cur.cls == &TOK_IDENTIFIER) {
		const struct istring *k;
		void **node;
		k = istrings_add(&(p_workspace->strings), p_tokeniser->cur.t.strident.str);
		if (k == NULL) {
			fprintf(stderr, "oom\n");
			return NULL;
		}
		node = pdict_get(&(p_workspace->workspace), (uintptr_t)(k->cstr));
		if (node == NULL) {
			fprintf(stderr, "'%s' was not found in the workspace\n", k->cstr);
			return NULL;
		}
		p_ret = *node;

		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_LISTVAL) {
		if (tokeniser_next(p_tokeniser))
			return NULL;
		p_ret             = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls        = &AST_CLS_LISTVAL;
		if ((p_ret->d.listval.p_list = expect_expression(p_workspace, p_tokeniser)) == NULL)
			return NULL;
		if ((p_ret->d.listval.p_index = expect_expression(p_workspace, p_tokeniser)) == NULL)
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_MAP) {
		if (tokeniser_next(p_tokeniser))
			return NULL;
		p_ret             = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls        = &AST_CLS_MAP;
		if ((p_ret->d.map.p_function = expect_expression(p_workspace, p_tokeniser)) == NULL)
			return NULL;
		if ((p_ret->d.map.p_input_list = expect_expression(p_workspace, p_tokeniser)) == NULL)
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_INT) {
		p_ret             = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls        = &AST_CLS_LITERAL_INT;
		p_ret->d.i        = p_tokeniser->cur.t.tint;
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_FLOAT) {
		p_ret             = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls        = &AST_CLS_LITERAL_FLOAT;
		p_ret->d.f        = p_tokeniser->cur.t.tflt;
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_STRING) {
		size_t sl           = strlen(p_tokeniser->cur.t.strident.str);
		char *p_strbuf;
		p_ret               = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_strbuf            = linear_allocator_alloc(&(p_workspace->alloc), sl + 1);
		memcpy(p_strbuf, p_tokeniser->cur.t.strident.str, sl + 1);
		p_ret->cls          = &AST_CLS_LITERAL_STRING;
		p_ret->d.str.hash   = 0;
		p_ret->d.str.len    = sl;
		p_ret->d.str.p_data = p_strbuf;
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_LPAREN) {
		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected another token\n");
			return NULL;
		}
		if ((p_ret = expect_expression(p_workspace, p_tokeniser)) == NULL)
			return NULL;
		if (p_tokeniser->cur.cls != &TOK_RPAREN) {
			fprintf(stderr, "expected close parenthesis\n");
			return NULL;
		}
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_LBRACE) {
		uint_fast32_t    nb_kvs = 0;
		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected another token\n");
			return NULL;
		}
		if (p_tokeniser->cur.cls != &TOK_RBRACE) {
			do {
				p_temp_nodes[2*nb_kvs+0] = expect_expression(p_workspace, p_tokeniser);
				if (p_temp_nodes[2*nb_kvs+0] == NULL) {
					fprintf(stderr, "expected expression\n");
					return NULL;
				}
				if (p_tokeniser->cur.cls != &TOK_COLON) {
					fprintf(stderr, "expected :\n");
					return NULL;
				}
				if (tokeniser_next(p_tokeniser)) {
					fprintf(stderr, "expected another token\n");
					return NULL;
				}
				p_temp_nodes[2*nb_kvs+1] = expect_expression(p_workspace, p_tokeniser);
				if (p_temp_nodes[2*nb_kvs+1] == NULL) {
					fprintf(stderr, "expected expression\n");
					return NULL;
				}
				nb_kvs++;
				if (p_tokeniser->cur.cls == &TOK_RBRACE)
					break;
				if (p_tokeniser->cur.cls != &TOK_COMMA) {
					fprintf(stderr, "expected , or }\n");
					return NULL;
				}
				if (tokeniser_next(p_tokeniser)) {
					fprintf(stderr, "expected another token\n");
					return NULL;
				}
			} while (1);
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
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_LSQBR) {
		uint_fast32_t nb_list = 0;
		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected another token\n");
			return NULL;
		}
		if (p_tokeniser->cur.cls != &TOK_RSQBR) {
			do {
				p_temp_nodes[nb_list] = expect_expression(p_workspace, p_tokeniser);
				if (p_temp_nodes[nb_list] == NULL)
					return NULL;
				nb_list++;
				if (p_tokeniser->cur.cls == &TOK_RSQBR)
					break;
				if (p_tokeniser->cur.cls != &TOK_COMMA) {
					fprintf(stderr, "expected , or ]\n");
					return NULL;
				}
				if (tokeniser_next(p_tokeniser)) {
					fprintf(stderr, "expected another token\n");
					return NULL;
				}
			} while (1);
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
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_SUB) {
		struct ast_node *p_next;
		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected another token after -\n");
			return NULL;
		}
		/* TODO: This sucks we can have unary unary unary unary unary.... don't want. */
		p_next = expect_expression(p_workspace, p_tokeniser);
		if (p_next == NULL)
			return NULL;
		p_ret                = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls           = &AST_CLS_NEG;
		p_ret->d.binop.p_lhs = p_next;
		p_ret->d.binop.p_rhs = NULL;
	} else if (p_tokeniser->cur.cls == &TOK_NULL) {
		p_ret        = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls   = &AST_CLS_LITERAL_NULL;
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_TRUE) {
		p_ret      = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls = &AST_CLS_LITERAL_BOOL;
		p_ret->d.i = 1;
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_FALSE) {
		p_ret      = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls = &AST_CLS_LITERAL_BOOL;
		p_ret->d.i = 0;
		if (tokeniser_next(p_tokeniser))
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_RANGE) {
		uint_fast32_t nb_list = 0;
		p_ret        = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls   = &AST_CLS_RANGE;
		if (tokeniser_next(p_tokeniser))
			return NULL;
		if ((p_ret->d.builtin.p_args = expect_expression(p_workspace, p_tokeniser)) == NULL)
			return NULL;
	} else if (p_tokeniser->cur.cls == &TOK_FUNC) {
		unsigned        nb_args = 0;
		unsigned        i;
		struct ast_node args[32];
		uintptr_t       argnames[32];

		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected another token after func\n");
			return NULL;
		}
		if (p_tokeniser->cur.cls != &TOK_LPAREN) {
			fprintf(stderr, "expected (\n");
			return NULL;
		}
		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected another token after (\n");
			return NULL;
		}
		if (p_tokeniser->cur.cls != &TOK_RPAREN) {
			do {
				const struct istring *k;
				void **node;
				if (p_tokeniser->cur.cls != &TOK_IDENTIFIER) {
					fprintf(stderr, "function parameter names must be identifiers %s\n", p_tokeniser->cur.cls->name);
					return NULL;
				}
				k = istrings_add(&(p_workspace->strings), p_tokeniser->cur.t.strident.str);
				if (k == NULL) {
					fprintf(stderr, "oom\n");
					return NULL;
				}
				if (tokeniser_next(p_tokeniser)) {
					fprintf(stderr, "expected another token\n");
					return NULL;
				}
				if (p_tokeniser->cur.cls != &TOK_COMMA && p_tokeniser->cur.cls != &TOK_RPAREN) {
					fprintf(stderr, "expected , or )\n");
					return NULL;
				}

				argnames[nb_args] = (uintptr_t)(k->cstr);
				node = pdict_get(&(p_workspace->workspace), argnames[nb_args]);
				if (node != NULL) {
					fprintf(stderr, "function parameter names may only appear ones and must alias workspace variables (%s)\n", k->cstr);
					return NULL;
				}
				args[nb_args].cls = &AST_CLS_STACKREF;
				args[nb_args].d.i = p_workspace->stack_depth + nb_args + 1;
				if (pdict_set(&(p_workspace->workspace), argnames[nb_args], &(args[nb_args]))) {
					fprintf(stderr, "oom\n");
					return NULL;
				}
				nb_args++;

				if (p_tokeniser->cur.cls == &TOK_RPAREN)
					break;

				if (tokeniser_next(p_tokeniser)) {
					fprintf(stderr, "expected another token\n");
					return NULL;
				}
			} while (1);
		}
		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected another token after )\n");
			return NULL;
		}

		p_workspace->stack_depth += nb_args;

		p_ret               = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls          = &AST_CLS_FUNCTION;
		p_ret->d.fn.nb_args = nb_args;
		p_ret->d.fn.node    = expect_expression(p_workspace, p_tokeniser);
		p_workspace->stack_depth -= nb_args;

		for (i = 0; i < nb_args; i++) {
			if (pdict_delete(&(p_workspace->workspace), argnames[i])) {
				fprintf(stderr, "ICE\n");
				abort();
			}
		}

		if (p_ret->d.fn.node == NULL) {
			fprintf(stderr, "expected expression for function body\n");
			return NULL;
		}


	} else if (p_tokeniser->cur.cls == &TOK_CALL) {
		unsigned i;
		unsigned nb_args = 0;

		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected identifier\n");
			return NULL;
		}
		p_ret             = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls        = &AST_CLS_CALL;
		p_ret->d.call.fn  = expect_expression(p_workspace, p_tokeniser);
		if (p_ret->d.call.fn == NULL) {
			fprintf(stderr, "expected function expression\n");
			return NULL;
		}

		p_ret->d.call.p_args  = expect_expression(p_workspace, p_tokeniser);
		if (p_ret->d.call.p_args == NULL) {
			fprintf(stderr, "expected argument expression\n");
			return NULL;
		}
	} else {
		token_print(&(p_tokeniser->cur));
		abort();
	}

	assert(p_ret != NULL);
	return p_ret;
}

struct ast_node *expect_expression_1(struct ast_node *p_lhs, struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, int min_precedence) {
	while
	    (   p_tokeniser->cur.cls->precedence > 0 /* lookahead is a binary operator */
		&&  p_tokeniser->cur.cls->precedence >= min_precedence /* whose precedence is >= min_precedence */
		) {
		struct ast_node *p_rhs;
		struct ast_node *p_comb;
		const struct tok_def *p_op = p_tokeniser->cur.cls;

		if (tokeniser_next(p_tokeniser))
			return NULL;

		if ((p_rhs = parse_primary(p_workspace, p_tokeniser)) == NULL)
			return NULL;
	
		while
		    (   p_tokeniser->cur.cls->precedence > 0 /* lookahead is a binary operator */
		    &&  (   p_tokeniser->cur.cls->precedence > p_op->precedence  /* whose precedence is greater than op's */
		        ||  (p_tokeniser->cur.cls->right_associative && p_tokeniser->cur.cls->precedence == p_op->precedence) /* or a right-associative operator whose precedence is equal to op's */
		        )
			) {
			if ((p_rhs = expect_expression_1(p_rhs, p_workspace, p_tokeniser, p_tokeniser->cur.cls->precedence)) == NULL)
				return NULL;
		}

		assert(p_op->bin_op_cls != NULL);

		if ((p_comb = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node))) == NULL)
			return NULL;

		p_comb->cls           = p_op->bin_op_cls;
		p_comb->d.binop.p_lhs = p_lhs;
		p_comb->d.binop.p_rhs = p_rhs;
		p_lhs = p_comb;
	}

	return p_lhs;
}

struct ast_node *expect_expression(struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser) {
	struct ast_node *p_lhs = parse_primary(p_workspace, p_tokeniser);
	if (p_lhs == NULL)
		return NULL;
	return expect_expression_1(p_lhs, p_workspace, p_tokeniser, 0);
}

struct jnode_data {
	struct ast_node *p_root;
	struct ast_node *p_stack;
	unsigned         nb_stack;


};

static int to_jnode(struct jnode *p_node, struct ev_ast_node *p_ast, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler);

struct list_element_fn_data {
	struct p_error_handler  *p_error_handler;
	struct ast_node        **pp_stack;
	unsigned                 stack_size;
	unsigned                 nb_elements;
	struct ast_node        **pp_elements;
};

struct execution_context {
	struct ev_ast_node          object;
	struct ejson_error_handler *p_error_handler;

};

int evaluate_ast(struct ev_ast_node *p_dest, const struct ev_ast_node *p_src, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler);

static int get_literal_list_element_fn(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, unsigned idx) {
	struct execution_context *ec = ctx;
	struct ev_ast_node tmp = ec->object;
	struct ev_ast_node ret;

	if (idx >= ec->object.data.d.llist.nb_elements)
		return -1;

	tmp.data = *(tmp.data.d.llist.elements[idx]);

	if (evaluate_ast(&ret, &tmp, p_alloc, ec->p_error_handler))
		return -1;

	return to_jnode(p_dest, &ret, p_alloc, NULL);
}

struct lrange {
	long long first;
	long long step_size;
	long long numel;
};

int ast_list_generator_get_element(struct ev_ast_node *p_dest, const struct ev_ast_node *p_list, unsigned element, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler) {
	assert(p_list->data.cls == &AST_CLS_LIST_GENERATOR);

	if (element >= p_list->data.d.lgen.nb_elements)
		return ejson_error(p_error_handler, "list index out of range\n");

	p_dest->data.cls   = &AST_CLS_LITERAL_INT;
	p_dest->data.d.i   = p_list->data.d.lgen.d.range.first + p_list->data.d.lgen.d.range.step * (long long)element;
	p_dest->pp_stack   = p_list->pp_stack;
	p_dest->stack_size = p_list->stack_size;
	return 0;
}

static int get_literal_element_fn(struct ev_ast_node *p_dest, const struct ev_ast_node *p_src, unsigned element, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler) {
	struct ev_ast_node tmp;

	assert(p_src->data.cls == &AST_CLS_LIST_GENERATOR);

	if (element >= p_src->data.d.lgen.nb_elements)
		return ejson_error(p_error_handler, "list index out of bounds\n");

	tmp      = *p_src;
	tmp.data = *(p_src->data.d.lgen.d.literal.pp_values[element]);

	if (evaluate_ast(p_dest, &tmp, p_alloc, p_error_handler))
		return ejson_error(p_error_handler, "could not evaluate list element\n");

	return 0;
}

static int ast_list_generator_map(struct ev_ast_node *p_dest, const struct ev_ast_node *p_src, unsigned element, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler) {
	struct ev_ast_node argument, tmp;
	struct ev_ast_node **pp_tmp;
	struct ev_ast_node *p_tmp;

	const struct ev_ast_node *p_function;
	const struct ev_ast_node *p_list;

	assert(p_src->data.cls == &AST_CLS_LIST_GENERATOR);
	p_function = p_src->data.d.lgen.d.map.p_function;
	p_list     = p_src->data.d.lgen.d.map.p_key;
	assert(p_function->data.cls == &AST_CLS_FUNCTION);
	assert(p_list->data.cls == &AST_CLS_LIST_GENERATOR);

	if (element >= p_list->data.d.lgen.nb_elements)
		return ejson_error(p_error_handler, "list index out of range\n");

	if (p_list->data.d.lgen.get_element(&argument, p_list, element, p_alloc, p_error_handler))
		return ejson_error(p_error_handler, "could not get list item\n");

	if ((pp_tmp = linear_allocator_alloc(p_alloc, sizeof(struct ev_ast_node *) * (p_function->stack_size + 1))) == NULL)
		return ejson_error(p_error_handler, "oom\n");
	if (p_function->stack_size)
		memcpy(pp_tmp, p_function->pp_stack, p_function->stack_size * sizeof(struct ev_ast_node *));
	if ((p_tmp = linear_allocator_alloc(p_alloc, sizeof(struct ev_ast_node))) == NULL)
		return ejson_error(p_error_handler, "oom\n");
	*p_tmp = argument;
	pp_tmp[p_function->stack_size] = p_tmp;

	tmp.pp_stack   = pp_tmp;
	tmp.stack_size = p_function->stack_size + 1;
	tmp.data       = *(p_function->data.d.fn.node);

	if (evaluate_ast(p_dest, &tmp, p_alloc, p_error_handler))
		return ejson_error(p_error_handler, "could not evaluate function\n");

	return 0;
}

/* Evaluate to the point where we have a node that can be used as a JSON
 * element.
 * 
 * 
 * call -> push values onto stack and execute function
 * 
 * list generator -> return list generator node
 * 
 * 
 * 
 *  */
int evaluate_ast(struct ev_ast_node *p_result, const struct ev_ast_node *p_src, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler) {
	/* Move through stack references. */
	while (p_src->data.cls == &AST_CLS_STACKREF) {
		assert(p_src->pp_stack != NULL);
		assert(p_src->data.d.i > 0 && p_src->data.d.i <= p_src->stack_size);
		p_src = p_src->pp_stack[p_src->stack_size - p_src->data.d.i];
		assert(p_src != NULL);
	}
	
	/* Shortcuts for fully simplified objects. */
	if  (   p_src->data.cls == &AST_CLS_LITERAL_INT
	    ||  p_src->data.cls == &AST_CLS_LITERAL_BOOL
	    ||  p_src->data.cls == &AST_CLS_LITERAL_STRING
	    ||  p_src->data.cls == &AST_CLS_LITERAL_FLOAT
	    ||  p_src->data.cls == &AST_CLS_LITERAL_NULL
	    ) {
		*p_result = *p_src;
#ifndef NDEBUG
		/* Set these as a debug courtesy so we get a crash if anything tries
		 * to access it. */
		p_result->pp_stack   = NULL;
		p_result->stack_size = 0;
#endif
		return 0;
	}

	if  (   p_src->data.cls == &AST_CLS_LIST_GENERATOR
	    ||  p_src->data.cls == &AST_CLS_FUNCTION
	    ) {
		*p_result = *p_src;
		return 0;
	}

	/* Convert lists into list generators */
	if  (p_src->data.cls == &AST_CLS_LITERAL_LIST) {
		*p_result                                   = *p_src;
		p_result->data.cls                          = &AST_CLS_LIST_GENERATOR;
		p_result->data.d.lgen.nb_elements           = p_src->data.d.llist.nb_elements;
		p_result->data.d.lgen.d.literal.pp_values   = p_src->data.d.llist.elements;
		p_result->data.d.lgen.get_element           = get_literal_element_fn;
		return 0;
	}

	if  (p_src->data.cls == &AST_CLS_LITERAL_DICT) {
		unsigned i;

		struct dictnode *p_root = NULL;

		/* Build the dictionary */
		for (i = 0; i < p_src->data.d.ldict.nb_keys; i++) {
			struct dictnode **pp_ipos = &p_root;
			struct dictnode  *p_iter = *pp_ipos;
			uint_fast32_t hash, ukey;
			size_t len;

			struct ev_ast_node key;
			struct ev_ast_node tmp = *p_src;
			tmp.data = *(p_src->data.d.ldict.elements[2*i+0]);
			if (evaluate_ast(&key, &tmp, p_alloc, p_error_handler) || key.data.cls != &AST_CLS_LITERAL_STRING)
				return ejson_error(p_error_handler, "dictionary keys must evaluate to strings\n");
			len  = hashfnv(key.data.d.str.p_data, &hash);
			ukey = hash;

			while (p_iter != NULL) {
				if (p_iter->key == hash && !strcmp((const char *)(p_iter + 1), key.data.d.str.p_data))
					return -1;
				pp_ipos = &(p_iter->p_children[ukey & DICTNODE_MASK]);
				p_iter = *pp_ipos;
				ukey >>= DICTNODE_BITS;
			}

			p_iter       = linear_allocator_alloc(p_alloc, sizeof(struct dictnode) + len + 1);
			p_iter->data = p_src->data.d.ldict.elements[2*i+1];
			p_iter->key  = hash;
			memset(p_iter->p_children, 0, sizeof(p_iter->p_children));
			memcpy((char *)(p_iter + 1), key.data.d.str.p_data, len + 1);
			*pp_ipos = p_iter;
		}

		p_result->data           = p_src->data;
		p_result->d.dict.nb_keys = p_src->data.d.ldict.nb_keys;
		p_result->d.dict.p_root  = p_root;
		p_result->pp_stack       = p_src->pp_stack;
		p_result->stack_size     = p_src->stack_size;
		return 0;
	}

	if (p_src->data.cls == &AST_CLS_LISTVAL) {
		struct ev_ast_node list;
		struct ev_ast_node idx;
		size_t save;

		struct ev_ast_node tmp;

		tmp.data       = *(p_src->data.d.listval.p_index);
		tmp.pp_stack   = p_src->pp_stack;
		tmp.stack_size = p_src->stack_size;
		
		save = linear_allocator_save(p_alloc);
		if (evaluate_ast(&idx, &tmp, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "couldn't evaluate listval index expression\n");
		if (idx.data.cls != &AST_CLS_LITERAL_INT)
			return ejson_error(p_error_handler, "listval index expression was not an integer\n");
		linear_allocator_restore(p_alloc, save);

		tmp.data = *(p_src->data.d.listval.p_list);
		assert(tmp.pp_stack   == p_src->pp_stack);
		assert(tmp.stack_size == p_src->stack_size);

		if (evaluate_ast(&list, &tmp, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "couldn't evaluate listval list expression\n");

		if (list.data.cls != &AST_CLS_LIST_GENERATOR)
			return ejson_error(p_error_handler, "listval list expression was not a list generator\n");

		if (list.data.d.lgen.get_element(p_result, &list, idx.data.d.i, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "could not get element\n");

		return 0;
	}

	/* Function call */
	if (p_src->data.cls == &AST_CLS_CALL) {
		struct ev_ast_node function;
		struct ev_ast_node args;
		struct ev_ast_node tmp;

		tmp = *p_src;
		tmp.data = *(p_src->data.d.call.fn);
		if (evaluate_ast(&function, &tmp, p_alloc, p_error_handler) || function.data.cls != &AST_CLS_FUNCTION)
			return ejson_error(p_error_handler, "call expects a function\n");

		tmp.data = *(p_src->data.d.call.p_args);
		if (evaluate_ast(&args, &tmp, p_alloc, p_error_handler) || args.data.cls != &AST_CLS_LIST_GENERATOR)
			return ejson_error(p_error_handler, "call expects the arguments to evaluate to a list\n");

		if (args.data.d.lgen.nb_elements != function.data.d.fn.nb_args)
			return ejson_error(p_error_handler, "function supplied with incorrect number of arguments (call=%u,fn=%u)\n", args.data.d.lgen.nb_elements, function.data.d.fn.nb_args);

		if (args.data.d.lgen.nb_elements) {
			unsigned i;
			struct ev_ast_node **pp_stack2;
			tmp = args;

			if ((pp_stack2 = linear_allocator_alloc(p_alloc, sizeof(struct ev_ast_node *) * (function.stack_size + args.data.d.lgen.nb_elements))) == NULL)
				return ejson_error(p_error_handler, "oom\n");

			if (function.stack_size)
				memcpy(pp_stack2, function.pp_stack, function.stack_size * sizeof(struct ev_ast_node *));

			for (i = 0; i < args.data.d.lgen.nb_elements; i++) {
				if ((pp_stack2[function.stack_size + args.data.d.lgen.nb_elements - 1 - i] = linear_allocator_alloc(p_alloc, sizeof(struct ev_ast_node))) == NULL)
					return ejson_error(p_error_handler, "oom\n");

				if (args.data.d.lgen.get_element(pp_stack2[function.stack_size + args.data.d.lgen.nb_elements - 1 - i], &args, i, p_alloc, p_error_handler))
					return ejson_error(p_error_handler, "failed to eval argument\n");
			}

			function.pp_stack    = pp_stack2;
			function.stack_size += args.data.d.lgen.nb_elements;
		}

		function.data = *(function.data.d.fn.node);

		if (evaluate_ast(p_result, &function, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "failed to execute function\n");

		return 0;
	}

	/* Unary negation */
	if (p_src->data.cls == &AST_CLS_NEG) {
		struct ev_ast_node tmp = *p_src;
		tmp.data = *(tmp.data.d.binop.p_lhs);

		if (evaluate_ast(p_result, &tmp, p_alloc, p_error_handler))
			return -1;

		if (p_result->data.cls == &AST_CLS_LITERAL_INT) {
			p_result->data.d.i = -p_result->data.d.i;
			return 0;
		}

		if (p_result->data.cls == &AST_CLS_LITERAL_FLOAT) {
			p_result->data.d.f = -p_result->data.d.f;
			return 0;
		}

		return ejson_error(p_error_handler, "unary negation can only be applied to numeric types\n");
	}

	/* Convert range into an accessor object */
	if (p_src->data.cls == &AST_CLS_RANGE) {
		struct lrange    lrange;
		size_t save = linear_allocator_save(p_alloc);

		struct ev_ast_node args;
		struct ev_ast_node tmp = *p_src;
		tmp.data = *(p_src->data.d.builtin.p_args);

		if (evaluate_ast(&args, &tmp, p_alloc, p_error_handler) || args.data.cls != &AST_CLS_LIST_GENERATOR)
			return ejson_error(p_error_handler, "range expects a list argument\n");

		if (args.data.d.lgen.nb_elements < 1 || args.data.d.lgen.nb_elements > 3)
			return ejson_error(p_error_handler, "range expects between 1 and 3 arguments\n");

		if (args.data.d.lgen.nb_elements == 1) {
			struct ev_ast_node numel;

			if (args.data.d.lgen.get_element(&numel, &args, 0, p_alloc, p_error_handler) || numel.data.cls != &AST_CLS_LITERAL_INT)
				return ejson_error(p_error_handler, "single argument range expects an integer number of items\n");

			lrange.first     = 0;
			lrange.step_size = 1;
			lrange.numel     = numel.data.d.i;
		} else if (args.data.d.lgen.nb_elements == 2) {
			struct ev_ast_node first, last;

			if  (   args.data.d.lgen.get_element(&first, &args, 0, p_alloc, p_error_handler) || first.data.cls != &AST_CLS_LITERAL_INT
			    ||  args.data.d.lgen.get_element(&last, &args, 1, p_alloc, p_error_handler) || last.data.cls != &AST_CLS_LITERAL_INT
			    )
				return ejson_error(p_error_handler, "dual argument range expects an integer first and last index\n");

			lrange.first     = first.data.d.i;
			lrange.step_size = (first.data.d.i > last.data.d.i) ? -1 : 1;
			lrange.numel     = ((first.data.d.i > last.data.d.i) ? (first.data.d.i - last.data.d.i) : (last.data.d.i - first.data.d.i)) + 1;
		} else {
			struct ev_ast_node first, step, last;

			if  (   args.data.d.lgen.get_element(&first, &args, 0, p_alloc, p_error_handler) || first.data.cls != &AST_CLS_LITERAL_INT
			    ||  args.data.d.lgen.get_element(&step, &args, 1, p_alloc, p_error_handler) || step.data.cls != &AST_CLS_LITERAL_INT
			    ||  args.data.d.lgen.get_element(&last, &args, 2, p_alloc, p_error_handler) || last.data.cls != &AST_CLS_LITERAL_INT
			    ||  step.data.d.i == 0
			    ||  (step.data.d.i > 0 && (first.data.d.i > last.data.d.i))
			    ||  (step.data.d.i < 0 && (first.data.d.i < last.data.d.i))
			    )
				return ejson_error(p_error_handler, "triple argument range expects an integer first, step and last values. step must be non-zero and have the correct sign for the range.\n");

			lrange.first     = first.data.d.i;
			lrange.step_size = step.data.d.i;
			lrange.numel     = (last.data.d.i - first.data.d.i) / step.data.d.i + 1;
		}

		linear_allocator_restore(p_alloc, save);
		
		p_result->pp_stack                  = p_src->pp_stack;
		p_result->stack_size                = p_src->stack_size;
		p_result->data.cls                  = &AST_CLS_LIST_GENERATOR;
		p_result->data.d.lgen.get_element   = ast_list_generator_get_element;
		p_result->data.d.lgen.nb_elements   = lrange.numel;
		p_result->data.d.lgen.d.range.first = lrange.first;
		p_result->data.d.lgen.d.range.step  = lrange.step_size;

		return 0;
	}

	if (p_src->data.cls == &AST_CLS_MAP) {
		struct ev_ast_node function, list, tmp = *p_src;

		tmp.data = *(p_src->data.d.map.p_function);
		if (evaluate_ast(&function, &tmp, p_alloc, p_error_handler) || function.data.cls != &AST_CLS_FUNCTION || function.data.d.fn.nb_args != 1)
			return ejson_error(p_error_handler, "map expects a function argument that takes one argument\n");

		tmp.data = *(p_src->data.d.map.p_input_list);
		if (evaluate_ast(&list, &tmp, p_alloc, p_error_handler) || list.data.cls != &AST_CLS_LIST_GENERATOR)
			return ejson_error(p_error_handler, "map expected a list argument following the function\n");

		p_result->pp_stack                        = p_src->pp_stack;
		p_result->stack_size                      = p_src->stack_size;
		p_result->data.cls                        = &AST_CLS_LIST_GENERATOR;
		p_result->data.d.lgen.d.map.p_function    = linear_allocator_alloc(p_alloc, sizeof(struct ev_ast_node));
		p_result->data.d.lgen.d.map.p_key         = linear_allocator_alloc(p_alloc, sizeof(struct ev_ast_node));
		p_result->data.d.lgen.d.map.p_function[0] = function;
		p_result->data.d.lgen.d.map.p_key[0]      = list;
		p_result->data.d.lgen.get_element         = ast_list_generator_map;
		p_result->data.d.lgen.nb_elements         = list.data.d.lgen.nb_elements;

		return 0;
	}


	/* Ops */
	if (p_src->data.cls == &AST_CLS_ADD || p_src->data.cls == &AST_CLS_SUB || p_src->data.cls == &AST_CLS_MUL || p_src->data.cls == &AST_CLS_MOD) {
		struct ev_ast_node lhs;
		struct ev_ast_node rhs;

		struct ev_ast_node tmp = *p_src;

		tmp.data = *(p_src->data.d.binop.p_lhs);
		if (evaluate_ast(&lhs, &tmp, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "could not evaluate binop lhs\n");

		tmp.data = *(p_src->data.d.binop.p_rhs);
		if (evaluate_ast(&rhs, &tmp, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "could not evaluate binop rhs\n");

		/* Deal with string concatenation */
		if (p_src->data.cls == &AST_CLS_ADD && (lhs.data.cls == &AST_CLS_LITERAL_STRING || rhs.data.cls == &AST_CLS_LITERAL_STRING)) {
			if (lhs.data.cls != &AST_CLS_LITERAL_STRING || rhs.data.cls != &AST_CLS_LITERAL_STRING)
				return ejson_error(p_error_handler, "using + for concatenation requires lhs and rhs to be strings.\n");
			abort(); /* implement me */
			return -1;
		}

		/* Promote types */
		if (lhs.data.cls == &AST_CLS_LITERAL_FLOAT && rhs.data.cls == &AST_CLS_LITERAL_INT) {
			rhs.data.d.f = rhs.data.d.i;
			rhs.data.cls = &AST_CLS_LITERAL_FLOAT;
		} else if (lhs.data.cls == &AST_CLS_LITERAL_INT && rhs.data.cls == &AST_CLS_LITERAL_FLOAT) {
			lhs.data.d.f = lhs.data.d.i;
			lhs.data.cls = &AST_CLS_LITERAL_FLOAT;
		}

		if (lhs.data.cls == &AST_CLS_LITERAL_FLOAT && rhs.data.cls == &AST_CLS_LITERAL_FLOAT) {
			if (p_src->data.cls == &AST_CLS_ADD) {
				lhs.data.d.f = lhs.data.d.f + rhs.data.d.f;
			} else if (p_src->data.cls == &AST_CLS_SUB) {
				lhs.data.d.f = lhs.data.d.f - rhs.data.d.f;
			} else if (p_src->data.cls == &AST_CLS_MUL) {
				lhs.data.d.f = lhs.data.d.f * rhs.data.d.f;
			} else if (p_src->data.cls == &AST_CLS_MOD) {
				lhs.data.d.f = fmod(lhs.data.d.f, rhs.data.d.f);
			} else {
				abort();
			}
			*p_result = lhs;
			return 0;
		}

		if (lhs.data.cls == &AST_CLS_LITERAL_INT && rhs.data.cls == &AST_CLS_LITERAL_INT) {
			if (p_src->data.cls == &AST_CLS_ADD) {
				lhs.data.d.i = lhs.data.d.i + rhs.data.d.i;
			} else if (p_src->data.cls == &AST_CLS_SUB) {
				lhs.data.d.i = lhs.data.d.i - rhs.data.d.i;
			} else if (p_src->data.cls == &AST_CLS_MUL) {
				lhs.data.d.i = lhs.data.d.i * rhs.data.d.i;
			} else if (p_src->data.cls == &AST_CLS_MOD) {
				lhs.data.d.i = lhs.data.d.i % rhs.data.d.i;
				lhs.data.d.i += (lhs.data.d.i < 0) ? rhs.data.d.i : 0;
			} else {
				abort();
			}
			*p_result = lhs;
			return 0;
		}


		abort();
		return -1;
	}

	fprintf(stderr, "what?\n");
	p_src->data.cls->debug_print(&(p_src->data), stderr, 10);
	fprintf(stderr, "endwhat?\n");
	abort();

	return -1;
}


static int jnode_list_get_element(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, unsigned idx) {
	struct execution_context *ec   = ctx;
	struct ev_ast_node tmp;

	if (ec->object.data.d.lgen.get_element(&tmp, &(ec->object), idx, p_alloc, ec->p_error_handler))
		return -1;

	return to_jnode(p_dest, &tmp, p_alloc, ec->p_error_handler);
}

static
int
enumerate_dict_keys2
	(jdict_enumerate_fn         *p_fn
	,struct ejson_error_handler *p_error_handler
	,const struct dictnode      *p_node
	,struct linear_allocator    *p_alloc
	,void                       *p_userctx
	,struct ev_ast_node        **pp_stack
	,unsigned                    stack_size
	) {
	unsigned i;
	struct jnode tmp;
	struct ev_ast_node tmp_dst, tmp_src;
	for (i = 0; i < DICTNODE_NUM; i++) {
		if (p_node->p_children[i] != NULL) {
			int r;
			if ((r = enumerate_dict_keys2(p_fn, p_error_handler, p_node->p_children[i], p_alloc, p_userctx, pp_stack, stack_size)) != 0) {
				return r;
			}
		}
	}
	tmp_src.stack_size = stack_size;
	tmp_src.pp_stack   = pp_stack;
	tmp_src.data       = *(p_node->data);
	if (evaluate_ast(&tmp_dst, &tmp_src, p_alloc, p_error_handler))
		return ejson_error(p_error_handler, "could not evaluate dictionary data\n");

	if (to_jnode(&tmp, &tmp_dst, p_alloc, p_error_handler))
		return ejson_error(p_error_handler, "could not convert dictionary data\n");

	return p_fn(&tmp, (const char *)(p_node + 1), p_userctx);
}

static int enumerate_dict_keys(jdict_enumerate_fn *p_fn, void *p_ctx, struct linear_allocator *p_alloc, void *p_userctx) {
	struct execution_context *ec = p_ctx;
	if (ec->object.d.dict.p_root != NULL)
		return enumerate_dict_keys2(p_fn, ec->p_error_handler, ec->object.d.dict.p_root, p_alloc, p_userctx, ec->object.pp_stack, ec->object.stack_size);
	return 0;
}


static int to_jnode(struct jnode *p_node, struct ev_ast_node *p_src, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler) {
	struct ev_ast_node ast;

	assert(p_src != NULL);

	/* Fully simplify - perhaps we should assert the thing has already been simplified? */
	if (evaluate_ast(&ast, p_src, p_alloc, p_error_handler))
		return -1;

	if (ast.data.cls == &AST_CLS_LITERAL_INT) {
		p_node->cls        = JNODE_CLS_INTEGER;
		p_node->d.int_bool = ast.data.d.i;
		return 0;
	}
	
	if (ast.data.cls == &AST_CLS_LITERAL_FLOAT) {
		p_node->cls    = JNODE_CLS_REAL;
		p_node->d.real = ast.data.d.f;
		return 0;
	}

	if (ast.data.cls == &AST_CLS_LITERAL_STRING) {
		p_node->cls          = JNODE_CLS_STRING;
		p_node->d.string.buf = ast.data.d.str.p_data;
		return 0;
	}

	if (ast.data.cls == &AST_CLS_LITERAL_NULL) {
		p_node->cls = JNODE_CLS_NULL;
		return 0;
	}

	if (ast.data.cls == &AST_CLS_LITERAL_BOOL) {
		p_node->cls        = JNODE_CLS_BOOL;
		p_node->d.int_bool = ast.data.d.i;
		return 0;
	}

	if (ast.data.cls == &AST_CLS_LITERAL_DICT) {
		struct execution_context *ec = linear_allocator_alloc(p_alloc, sizeof(struct execution_context));
		ec->p_error_handler = p_error_handler;
		ec->object          = ast;

		p_node->cls               = JNODE_CLS_DICT;
		p_node->d.dict.nb_keys    = ast.d.dict.nb_keys;
		p_node->d.dict.ctx        = ec;
		p_node->d.dict.get_by_key = NULL; /* TODO */
		p_node->d.dict.enumerate  = enumerate_dict_keys;
		return 0;
	}

	if (ast.data.cls == &AST_CLS_LITERAL_LIST) {
		struct execution_context *ec = linear_allocator_alloc(p_alloc, sizeof(struct execution_context));
		ec->p_error_handler          = p_error_handler;
		ec->object                   = ast;
		p_node->cls                  = JNODE_CLS_LIST;
		p_node->d.list.ctx           = ec;
		p_node->d.list.nb_elements   = ast.data.d.llist.nb_elements;
		p_node->d.list.get_elemenent = get_literal_list_element_fn;
		return 0;
	}

	if (ast.data.cls == &AST_CLS_LIST_GENERATOR) {
		struct execution_context *ec = linear_allocator_alloc(p_alloc, sizeof(struct execution_context));
		ec->p_error_handler          = p_error_handler;
		ec->object                   = ast;
		p_node->cls                  = JNODE_CLS_LIST;
		p_node->d.list.ctx           = ec;
		p_node->d.list.nb_elements   = ast.data.d.lgen.nb_elements;
		p_node->d.list.get_elemenent = jnode_list_get_element;
		return 0;
	}

	fprintf(stderr, "cannot convert the following type to json ---\n");
	ast.data.cls->debug_print(&(ast.data), stderr, 10);
	fprintf(stderr, "---\n");
	abort();

	return -1;
}

int parse_document(struct jnode *p_node, struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, struct ejson_error_handler *p_error_handler) {
	struct ast_node *p_obj;
	struct ev_ast_node tmp;
	while (p_tokeniser->cur.cls == &TOK_DEFINE) {
		const struct istring *p_key;
		void **pp_obj;
		if (tokeniser_next(p_tokeniser) || p_tokeniser->cur.cls != &TOK_IDENTIFIER)
			return ejson_error(p_error_handler, "expected an identifier token after '@'\n");
		if ((p_key = istrings_add(&(p_workspace->strings), p_tokeniser->cur.t.strident.str)) == NULL)
			return ejson_error(p_error_handler, "out of memory\n");
		if ((pp_obj = pdict_get(&(p_workspace->workspace), (uintptr_t)(p_key->cstr))) != NULL)
			return ejson_error(p_error_handler, "cannot redefine variable '%s'\n", p_tokeniser->cur.t.strident.str);
		if (tokeniser_next(p_tokeniser) || p_tokeniser->cur.cls != &TOK_EQ)
			return ejson_error(p_error_handler, "expected '='\n");
		if (tokeniser_next(p_tokeniser) || (p_obj = expect_expression(p_workspace, p_tokeniser)) == NULL)
			return ejson_error(p_error_handler, "expected an expression\n");
		if (p_tokeniser->cur.cls != &TOK_SEMI || tokeniser_next(p_tokeniser))
			return ejson_error(p_error_handler, "expected ';'\n");
		if (pdict_set(&(p_workspace->workspace), (uintptr_t)(p_key->cstr), p_obj))
			return ejson_error(p_error_handler, "out of memory\n");
	}
	if ((p_obj = expect_expression(p_workspace, p_tokeniser)) == NULL)
		return ejson_error(p_error_handler, "expected main document expression\n");
	//p_obj->cls->debug_print(p_obj, stdout, 20);
	tmp.data = *p_obj;
	tmp.pp_stack = NULL;
	tmp.stack_size = 0;
	if (to_jnode(p_node, &tmp, &(p_workspace->alloc), p_error_handler))
		return ejson_error(p_error_handler, "could not convert AST to root document node\n");
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

static void on_parser_error(void *p_context, const char *p_format, va_list args) {
	fprintf(stderr, "  ");
	vfprintf(stderr, p_format, args);
}

int run_test(const char *p_ejson, const char *p_ref, const char *p_name) {
	struct linear_allocator a1;
	struct linear_allocator a2;
	struct linear_allocator a3;
	struct jnode ref;
	struct jnode dut;
	struct tokeniser t;
	struct evaluation_context ws;
	struct ejson_error_handler err;
	int d;
	
	err.p_context = NULL;
	err.on_parser_error = on_parser_error;

	a1.pos = 0;
	a2.pos = 0;
	a3.pos = 0;

	if (parse_json(&ref, &a1, &a2, p_ref))
		return unexpected_fail("could not parse reference JSON:\n  %s\n", p_ref);

	tokeniser_start(&t, (char *)p_ejson);

	if (tokeniser_next(&t))
		return unexpected_fail("expected a token in the reference\n");

	if (evaluation_context_init(&ws))
		return unexpected_fail("could not init workspace\n");

	if (parse_document(&dut, &ws, &t, &err))
		return (fprintf(stderr, "could not parse dut:\n  %s\n", p_ejson), -1);

	if ((d = are_different(&dut, &ref, &a3)) < 0) {
		return unexpected_fail("are_different failed to execute\n");
	}
	
	if (d) {
		fprintf(stderr, "test '%s' failed.\n", p_name);
		fprintf(stderr, "  Reference:\n    ");
		jnode_print(&ref, &a2, 4);
		fprintf(stderr, "  DUT:\n    ");
		jnode_print(&dut, &a2, 4);
		return -1;
	}

	printf("test '%s' passed.\n", p_name);
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
		("[1,-2,3.4,-4.5,5.6e2,-7.8e-2]"
		,"[1,-2,3.4,-4.5,5.6e2,-7.8e-2]"
		,"numeric objects in a list"
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


	/* FIXME: something is broken i think with the test comparison function. */
	run_test
		("define notes=[\"a\",\"b\",\"c\"];"
		 "map func(x) {\"name\": listval notes x % 3, \"id\": x} range[0,5]"
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

