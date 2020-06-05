
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
	void       (*debug_print)(struct ast_node *p_node, FILE *p_f, unsigned depth);
};

#define DEF_AST_CLS(name_, to_jnode_fn_, debug_print_fn_) \
	static const struct ast_cls name_ = { #name_, to_jnode_fn_, debug_print_fn_ }

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
			int             (*get_element)(struct ast_node *p_dest, struct ast_node *p_ast, struct ast_node **pp_stack, unsigned stack_size, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler);
			union {
				struct {
					long long first;
					long long step;
				} range;
			} d;
			uint_fast32_t     nb_elements;
		} lgen; /* AST_CLS_LIST_GENERATOR */
		struct {
			struct ast_node **elements; /* 2*nb_keys - [key, value] */
			uint_fast32_t     nb_keys;
		} ldict; /* AST_CLS_LITERAL_DICT */
		struct {
			struct ast_node *node;
			unsigned         nb_args;
		} fn; /* AST_CLS_FUNCTION */
		struct {
			struct ast_node  *fn;
			struct ast_node **pp_args;
			unsigned          nb_args;
		} call;
		struct {
			unsigned          nb_args;
			struct ast_node **pp_args;
		} builtin;
		struct {
			struct ast_node *p_function;
			struct ast_node *p_input_list;
		} map;


	} d;

};

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


static void debug_print_null(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
}
static void debug_print_int_like(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s(%lld)\n", depth, "", p_node->cls->p_name, p_node->d.i);
}
static void debug_print_real(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s(%f)\n", depth, "", p_node->cls->p_name, p_node->d.f);
}
static void debug_print_string(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s('%s')\n", depth, "", p_node->cls->p_name, p_node->d.str.p_data);
}
static void debug_print_bool(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s(%s)\n", depth, "", p_node->cls->p_name, (p_node->d.i) ? "true" : "false");
}
static void debug_print_list(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	unsigned i;
	fprintf(p_f, "%*s%s(%u)\n", depth, "", p_node->cls->p_name, p_node->d.llist.nb_elements);
	for (i = 0; i < p_node->d.llist.nb_elements; i++)
		p_node->d.llist.elements[i]->cls->debug_print(p_node->d.llist.elements[i], p_f, depth + 1);
}
static void debug_print_dict(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	unsigned i;
	fprintf(p_f, "%*s%s(%u)\n", depth, "", p_node->cls->p_name, p_node->d.ldict.nb_keys);
	for (i = 0; i < p_node->d.ldict.nb_keys; i++) {
		p_node->d.ldict.elements[2*i+0]->cls->debug_print(p_node->d.ldict.elements[2*i+0], p_f, depth + 1);
		p_node->d.ldict.elements[2*i+1]->cls->debug_print(p_node->d.ldict.elements[2*i+1], p_f, depth + 2);
	}
}
static void debug_print_binop(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.binop.p_lhs->cls->debug_print(p_node->d.binop.p_lhs, p_f, depth + 1);
	p_node->d.binop.p_rhs->cls->debug_print(p_node->d.binop.p_rhs, p_f, depth + 1);
}
static void debug_print_unop(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.binop.p_lhs->cls->debug_print(p_node->d.binop.p_lhs, p_f, depth + 1);
}
static void debug_print_builtin(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	unsigned i;
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	for (i = 0; i < p_node->d.builtin.nb_args; i++)
		p_node->d.builtin.pp_args[i]->cls->debug_print(p_node->d.builtin.pp_args[i], p_f, depth + 1);
}
static void debug_print_function(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	fprintf(p_f, "%*s%s(nb_args=%u)\n", depth, "", p_node->cls->p_name, p_node->d.fn.nb_args);
	p_node->d.fn.node->cls->debug_print(p_node->d.fn.node, p_f, depth + 1);
}
static void debug_print_call(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	unsigned i;
	fprintf(p_f, "%*s%s\n", depth, "", p_node->cls->p_name);
	p_node->d.call.fn->cls->debug_print(p_node->d.call.fn, p_f, depth + 1);
	for (i = 0; i < p_node->d.call.nb_args; i++)
		p_node->d.call.pp_args[i]->cls->debug_print(p_node->d.call.pp_args[i], p_f, depth + 1);
}

static void debug_print_map(struct ast_node *p_node, FILE *p_f, unsigned depth) {
	abort();
}

static void debug_list_generator(struct ast_node *p_node, FILE *p_f, unsigned depth) {
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
		p_ret->cls   = &AST_CLS_LITERAL_NULL;
		if (tokeniser_next(p_tokeniser))
			return NULL;
		if (p_tokeniser->cur.cls != &TOK_LPAREN) {
			fprintf(stderr, "expected lparen\n");
			return NULL;
		}
		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected another token\n");
			return NULL;
		}
		if (p_tokeniser->cur.cls != &TOK_RPAREN) {
			do {
				p_temp_nodes[nb_list] = expect_expression(p_workspace, p_tokeniser);
				if (p_temp_nodes[nb_list] == NULL)
					return NULL;
				nb_list++;
				if (p_tokeniser->cur.cls == &TOK_RPAREN)
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
		if (nb_list < 1 || nb_list > 3) {
			fprintf(stderr, "range expects 1-3 arguments\n");
			return NULL;
		}
		p_ret                    = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node));
		p_ret->cls               = &AST_CLS_RANGE;
		p_ret->d.builtin.nb_args = nb_list;
		if (nb_list) {
			p_ret->d.builtin.pp_args = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node *) * nb_list);
			memcpy(p_ret->d.builtin.pp_args, p_temp_nodes, sizeof(struct ast_node *) * nb_list);
		} else {
			p_ret->d.builtin.pp_args = NULL;
		}
		if (tokeniser_next(p_tokeniser))
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
		if (p_tokeniser->cur.cls != &TOK_LSQBR) {
			fprintf(stderr, "expected [\n");
			return NULL;
		}
		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected identifier\n");
			return NULL;
		}

		if (p_tokeniser->cur.cls != &TOK_RSQBR) {
			do {
				p_temp_nodes[nb_args] = expect_expression(p_workspace, p_tokeniser);
				if (p_temp_nodes[nb_args] == NULL)
					return NULL;
				nb_args++;
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

		if (tokeniser_next(p_tokeniser)) {
			fprintf(stderr, "expected identifier\n");
			return NULL;
		}

		if (nb_args) {
			if ((p_ret->d.call.pp_args = linear_allocator_alloc(&(p_workspace->alloc), sizeof(struct ast_node *) * nb_args)) == NULL)
				return NULL;
			for (i = 0; i < nb_args; i++) {
				p_ret->d.call.pp_args[i] = p_temp_nodes[i];
			}
		}

		p_ret->d.call.nb_args = nb_args;
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

struct jnode_stack {
	unsigned          nb_stack;
	struct ast_node **pp_nodes;
};

static int to_jnode(struct jnode *p_node, struct ast_node *p_ast, struct jnode_stack *p_stack, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler);

static int get_literal_list_element_fn(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, unsigned idx) {
	return to_jnode(p_dest, ((struct ast_node **)ctx)[idx], NULL, p_alloc, NULL);
}

struct lrange {
	long long first;
	long long step_size;
	long long numel;
};

static int get_range_list_element_fn(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, unsigned idx) {
	struct lrange *lrange = ctx;
	if (idx < 0 || (long)idx > lrange->numel)
		return -1;
	p_dest->cls = JNODE_CLS_INTEGER;
	p_dest->d.int_bool = lrange->first + lrange->step_size * (long)idx;
	return 0;
}

static struct ast_node *dereference(struct ast_node *p_ast, struct jnode_stack *p_stack) {
	while (p_ast->cls == &AST_CLS_STACKREF) {
		assert(p_stack != NULL);
		assert(p_ast->d.i < p_stack->nb_stack);
		p_ast = p_stack->pp_nodes[p_ast->d.i];
		assert(p_ast != NULL);
	}
	return p_ast;
}

int evaluate_ast(struct ast_node *p_dest, struct ast_node *p_ast, struct ast_node **pp_stack, unsigned stack_size, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler);

int ast_list_generator_get_element(struct ast_node *p_dest, struct ast_node *p_ast, struct ast_node **pp_stack, unsigned stack_size, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler) {
	struct ast_node idx;
	size_t save;
	assert(stack_size);
	save = linear_allocator_save(p_alloc);
	if (evaluate_ast(&idx, pp_stack[stack_size - 1], pp_stack, stack_size - 1, p_alloc, p_error_handler))
		return -1;
	if (idx.cls != &AST_CLS_LITERAL_INT)
		return ejson_error(p_error_handler, "list indexes should be integers\n");
	if (idx.d.i < 0 || (unsigned)idx.d.i >= p_ast->d.lgen.nb_elements)
		return ejson_error(p_error_handler, "list index out of range\n");
	linear_allocator_restore(p_alloc, save);
	p_dest->cls = &AST_CLS_LITERAL_INT;
	p_dest->d.i = p_ast->d.lgen.d.range.first + p_ast->d.lgen.d.range.step * idx.d.i;
	return 0;
}

int evaluate_ast(struct ast_node *p_dest, struct ast_node *p_ast, struct ast_node **pp_stack, unsigned stack_size, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler) {
	/* Move through stack references. */
	while (p_ast->cls == &AST_CLS_STACKREF && stack_size) {
		assert(pp_stack != NULL);
		assert(p_ast->d.i > 0 && p_ast->d.i <= stack_size);
		p_ast = pp_stack[stack_size - p_ast->d.i];
		assert(p_ast != NULL);
	}
	
	/* Shortcuts for fully simplified objects. */
	if  (   p_ast->cls == &AST_CLS_LITERAL_INT
	    ||  p_ast->cls == &AST_CLS_LITERAL_LIST
	    ||  p_ast->cls == &AST_CLS_LITERAL_BOOL
	    ||  p_ast->cls == &AST_CLS_LITERAL_STRING
	    ||  p_ast->cls == &AST_CLS_LITERAL_FLOAT
	    ||  p_ast->cls == &AST_CLS_LITERAL_NULL
	    ||  p_ast->cls == &AST_CLS_LITERAL_DICT
	    ||  p_ast->cls == &AST_CLS_FUNCTION
	    ) {
		*p_dest = *p_ast;
		return 0;
	}

	/* Function call */
	if (p_ast->cls == &AST_CLS_CALL) {
		struct ast_node function;

		if (evaluate_ast(&function, p_ast->d.call.fn, pp_stack, stack_size, p_alloc, p_error_handler) || function.cls != &AST_CLS_FUNCTION)
			return ejson_error(p_error_handler, "call expects a function\n");

		if (p_ast->d.call.nb_args != function.d.fn.nb_args)
			return ejson_error(p_error_handler, "function supplied with incorrect number of arguments (call=%u,fn=%u)\n", p_ast->d.call.nb_args, function.d.fn.nb_args);

		if (p_ast->d.call.nb_args) {
			unsigned i;
			struct ast_node **pp_stack2;

			if ((pp_stack2 = linear_allocator_alloc(p_alloc, sizeof(struct ast_node *) * (stack_size + p_ast->d.call.nb_args))) == NULL)
				return ejson_error(p_error_handler, "oom\n");

			if (stack_size)
				memcpy(pp_stack2, pp_stack, stack_size * sizeof(struct ast_node *));

			for (i = 0; i < p_ast->d.call.nb_args; i++) {
				if ((pp_stack2[stack_size + p_ast->d.call.nb_args - 1 - i] = linear_allocator_alloc(p_alloc, sizeof(struct ast_node))) == NULL)
					return ejson_error(p_error_handler, "oom\n");
				if (evaluate_ast(pp_stack2[stack_size + p_ast->d.call.nb_args - 1 - i], p_ast->d.call.pp_args[i], pp_stack, stack_size, p_alloc, p_error_handler))
					return ejson_error(p_error_handler, "failed to eval argument\n");
			}

			pp_stack    = pp_stack2;
			stack_size += p_ast->d.call.nb_args;
		}

		if (evaluate_ast(p_dest, function.d.fn.node, pp_stack, stack_size, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "failed to execute function\n");

		return 0;
	}

	/* Unary negation */
	if (p_ast->cls == &AST_CLS_NEG) {
		if (evaluate_ast(p_dest, p_ast->d.binop.p_lhs, pp_stack, stack_size, p_alloc, p_error_handler))
			return -1;

		if (p_dest->cls == &AST_CLS_LITERAL_INT) {
			p_dest->d.i = -p_dest->d.i;
			return 0;
		}

		if (p_dest->cls == &AST_CLS_LITERAL_FLOAT) {
			p_dest->d.f = -p_dest->d.f;
			return 0;
		}

		return -1;
	}

	if (p_ast->cls == &AST_CLS_LIST_GENERATOR) {
		if (!stack_size) {
			*p_dest = *p_ast;
			return 0;
		}
		return p_ast->d.lgen.get_element(p_dest, p_ast, pp_stack, stack_size, p_alloc, p_error_handler);
	}

	/* Convert range into an accessor object */
	if (p_ast->cls == &AST_CLS_RANGE) {
		struct lrange    lrange;
		size_t save = linear_allocator_save(p_alloc);

		if (p_ast->d.builtin.nb_args == 1) {
			struct ast_node numel;
			if (evaluate_ast(&numel, p_ast->d.builtin.pp_args[0], pp_stack, stack_size, p_alloc, p_error_handler) || numel.cls != &AST_CLS_LITERAL_INT)
				return -1;
			lrange.first     = 0;
			lrange.step_size = 1;
			lrange.numel     = numel.d.i;
		} else if (p_ast->d.builtin.nb_args == 2) {
			struct ast_node first, last;
			if  (   evaluate_ast(&first, p_ast->d.builtin.pp_args[0], pp_stack, stack_size, p_alloc, p_error_handler) || first.cls != &AST_CLS_LITERAL_INT
			    ||  evaluate_ast(&last, p_ast->d.builtin.pp_args[1], pp_stack, stack_size, p_alloc, p_error_handler) || last.cls != &AST_CLS_LITERAL_INT
			    )
				return -1;
			lrange.first     = first.d.i;
			lrange.step_size = (first.d.i > last.d.i) ? -1 : 1;
			lrange.numel     = ((first.d.i > last.d.i) ? (first.d.i - last.d.i) : (last.d.i - first.d.i)) + 1;
		} else {
			struct ast_node first, step, last;
			assert(p_ast->d.builtin.nb_args == 3);
			if  (   evaluate_ast(&first, p_ast->d.builtin.pp_args[0], pp_stack, stack_size, p_alloc, p_error_handler) || first.cls != &AST_CLS_LITERAL_INT
			    ||  evaluate_ast(&step, p_ast->d.builtin.pp_args[1], pp_stack, stack_size, p_alloc, p_error_handler) || step.cls != &AST_CLS_LITERAL_INT
			    ||  evaluate_ast(&last, p_ast->d.builtin.pp_args[2], pp_stack, stack_size, p_alloc, p_error_handler) || last.cls != &AST_CLS_LITERAL_INT
			    ||  step.d.i == 0
			    ||  (step.d.i > 0 && (first.d.i > last.d.i))
			    ||  (step.d.i < 0 && (first.d.i < last.d.i))
			    )
				return -1;
			lrange.first     = first.d.i;
			lrange.step_size = step.d.i;
			lrange.numel     = (last.d.i - first.d.i) / step.d.i + 1;
		}

		linear_allocator_restore(p_alloc, save);

		struct ast_node *p_tmp = p_dest;
		if (stack_size) {
			p_tmp = linear_allocator_alloc(p_alloc, sizeof(*p_tmp));
		}
		p_tmp->cls                  = &AST_CLS_LIST_GENERATOR;
		p_tmp->d.lgen.get_element   = ast_list_generator_get_element;
		p_tmp->d.lgen.nb_elements   = lrange.numel;
		p_tmp->d.lgen.d.range.first = lrange.first;
		p_tmp->d.lgen.d.range.step  = lrange.step_size;
		if (stack_size) {
			return p_tmp->d.lgen.get_element(p_dest, p_tmp, pp_stack, stack_size, p_alloc, p_error_handler);
		}

		return 0;
	}

	/* Ops */
	if (p_ast->cls == &AST_CLS_ADD || p_ast->cls == &AST_CLS_SUB || p_ast->cls == &AST_CLS_MUL) {
		struct ast_node lhs;
		struct ast_node rhs;
		struct ast_node *p_ret;

		if (evaluate_ast(&lhs, p_ast->d.binop.p_lhs, pp_stack, stack_size, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "could not evaluate binop lhs\n");

		if (evaluate_ast(&rhs, p_ast->d.binop.p_rhs, pp_stack, stack_size, p_alloc, p_error_handler))
			return ejson_error(p_error_handler, "could not evaluate binop rhs\n");

		/* Deal with string concatenation */
		if (p_ast->cls == &AST_CLS_ADD && (lhs.cls == &AST_CLS_LITERAL_STRING || rhs.cls == &AST_CLS_LITERAL_STRING)) {
			if (lhs.cls != &AST_CLS_LITERAL_STRING || rhs.cls != &AST_CLS_LITERAL_STRING)
				return ejson_error(p_error_handler, "using + for concatenation requires lhs and rhs to be strings.\n");
			abort(); /* implement me */
			return -1;
		}

		/* Promote types */
		if (lhs.cls == &AST_CLS_LITERAL_FLOAT && rhs.cls == &AST_CLS_LITERAL_INT) {
			rhs.d.f = rhs.d.i;
			rhs.cls = &AST_CLS_LITERAL_FLOAT;
		} else if (lhs.cls == &AST_CLS_LITERAL_INT && rhs.cls == &AST_CLS_LITERAL_FLOAT) {
			lhs.d.f = lhs.d.i;
			lhs.cls = &AST_CLS_LITERAL_FLOAT;
		}

		if (lhs.cls == &AST_CLS_LITERAL_FLOAT && rhs.cls == &AST_CLS_LITERAL_FLOAT) {
			if (p_ast->cls == &AST_CLS_ADD) {
				lhs.d.f = lhs.d.f + rhs.d.f;
			} else if (p_ast->cls == &AST_CLS_SUB) {
				lhs.d.f = lhs.d.f - rhs.d.f;
			} else if (p_ast->cls == &AST_CLS_MUL) {
				lhs.d.f = lhs.d.f * rhs.d.f;
			} else {
				abort();
			}
			*p_dest = lhs;
			return 0;
		}

		if (lhs.cls == &AST_CLS_LITERAL_INT && rhs.cls == &AST_CLS_LITERAL_INT) {
			if (p_ast->cls == &AST_CLS_ADD) {
				lhs.d.i = lhs.d.i + rhs.d.i;
			} else if (p_ast->cls == &AST_CLS_SUB) {
				lhs.d.i = lhs.d.i - rhs.d.i;
			} else if (p_ast->cls == &AST_CLS_MUL) {
				lhs.d.i = lhs.d.i * rhs.d.i;
			} else {
				abort();
			}
			*p_dest = lhs;
			return 0;
		}


		abort();
		return -1;
	}


	fprintf(stderr, "what?");
	printf("---\n");
	p_ast->cls->debug_print(p_ast, stdout, 10);
	printf("---\n");
	fprintf(stderr, "endwhat?");
	abort();

	return -1;
}

struct execution_context {
	struct ast_node            *p_object;
	struct ast_node           **pp_stack;
	unsigned                    stack_size;
	struct ejson_error_handler *p_error_handler;

};

static int to_jnode(struct jnode *p_node, struct ast_node *p_ast, struct jnode_stack *p_stack, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler);

static int jnode_list_get_element(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, unsigned idx) {
	struct execution_context *ec   = ctx;
	struct ast_node *obj           = linear_allocator_alloc(p_alloc, sizeof(struct ast_node));
	struct ast_node **pp_stack2    = linear_allocator_alloc(p_alloc, sizeof(struct ast_node *) * (ec->stack_size + 1));
	if (ec->stack_size)
		memcpy(pp_stack2, ec->pp_stack, ec->stack_size * sizeof(struct ast_node *));
	pp_stack2[ec->stack_size]      = linear_allocator_alloc(p_alloc, sizeof(struct ast_node));
	pp_stack2[ec->stack_size]->cls = &AST_CLS_LITERAL_INT;
	pp_stack2[ec->stack_size]->d.i = idx;

	if (evaluate_ast(obj, ec->p_object, pp_stack2, ec->stack_size + 1, p_alloc, ec->p_error_handler))
		return -1;

	//obj->cls->debug_print(obj, stdout, 5);

	return to_jnode(p_dest, obj, NULL, p_alloc, ec->p_error_handler);
}

static int to_jnode(struct jnode *p_node, struct ast_node *p_ast, struct jnode_stack *p_stack, struct linear_allocator *p_alloc, struct ejson_error_handler *p_error_handler) {
	struct ast_node ast;

	assert(p_ast != NULL);

	if (evaluate_ast(&ast, p_ast, NULL, 0, p_alloc, p_error_handler))
		return -1;

	if (ast.cls == &AST_CLS_LITERAL_INT) {
		p_node->cls        = JNODE_CLS_INTEGER;
		p_node->d.int_bool = ast.d.i;
		return 0;
	}
	
	if (ast.cls == &AST_CLS_LITERAL_FLOAT) {
		p_node->cls    = JNODE_CLS_REAL;
		p_node->d.real = ast.d.f;
		return 0;
	}

	if (ast.cls == &AST_CLS_LITERAL_STRING) {
		p_node->cls          = JNODE_CLS_STRING;
		p_node->d.string.buf = ast.d.str.p_data;
		return 0;
	}

	if (ast.cls == &AST_CLS_LITERAL_NULL) {
		p_node->cls = JNODE_CLS_NULL;
		return 0;
	}

	if (ast.cls == &AST_CLS_LITERAL_BOOL) {
		p_node->cls        = JNODE_CLS_BOOL;
		p_node->d.int_bool = ast.d.i;
		return 0;
	}

	if (ast.cls == &AST_CLS_LITERAL_LIST) {
		p_node->cls                  = JNODE_CLS_LIST;
		p_node->d.list.ctx           = ast.d.llist.elements;
		p_node->d.list.nb_elements   = ast.d.llist.nb_elements;
		p_node->d.list.get_elemenent = get_literal_list_element_fn;
		return 0;
	}

	if (ast.cls == &AST_CLS_LIST_GENERATOR) {
		struct execution_context *ec = linear_allocator_alloc(p_alloc, sizeof(struct execution_context));
		ec->p_error_handler          = p_error_handler;
		ec->p_object                 = p_ast;
		ec->pp_stack                 = NULL;
		ec->stack_size               = 0;
		p_node->cls                  = JNODE_CLS_LIST;
		p_node->d.list.ctx           = ec;
		p_node->d.list.nb_elements   = ast.d.lgen.nb_elements;
		p_node->d.list.get_elemenent = jnode_list_get_element;
		return 0;
	}

	printf("---\n");
	ast.cls->debug_print(&ast, stdout, 10);
	printf("---\n");
	abort();

	return -1;
}

int parse_document(struct jnode *p_node, struct evaluation_context *p_workspace, struct tokeniser *p_tokeniser, struct ejson_error_handler *p_error_handler) {
	struct ast_node *p_obj;
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
	if (to_jnode(p_node, p_obj, NULL, &(p_workspace->alloc), p_error_handler))
		return ejson_error(p_error_handler, "could not convert AST to root document node\n");
	return 0;
}


#if EJSON_TEST

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
		return unexpected_fail("could not parse reference JSON\n");

	tokeniser_start(&t, (char *)p_ejson);

	if (tokeniser_next(&t))
		return unexpected_fail("expected a token in the reference\n");

	if (evaluation_context_init(&ws))
		return unexpected_fail("could not init workspace\n");

	if (parse_document(&dut, &ws, &t, &err))
		return (fprintf(stderr, "could not parse dut\n"), -1);

	if ((d = are_different(&ref, &dut, &a3)) < 0) {
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
		("range(5)"
		,"[0, 1, 2, 3, 4]"
		,"range generator simple"
		);
	run_test
		("range(6,11)"
		,"[6, 7, 8, 9, 10, 11]"
		,"range generator from-to"
		);
	run_test
		("range(6,-11)"
		,"[6,5,4,3,2,1,0,-1,-2,-3,-4,-5,-6,-7,-8,-9,-10,-11]"
		,"range generator from-to reverse"
		);
	run_test
		("range(6,2,10)"
		,"[6,8,10]"
		,"range generator from-step-to"
		);
	run_test
		("range(6,-3,-9)"
		,"[6,3,0,-3,-6,-9]"
		,"range generator from-step-to 2"
		);
	run_test
		("call (func() 1) []"
		,"1"
		,"function call with no argument"
		);
	run_test
		("call (func(x) x) [55]"
		,"55"
		,"function call that returns the argument"
		);
	run_test
		("call (func(x, y, z) x * y + z) [3, 5, 7]"
		,"22"
		,"function mac-like function"
		);
	run_test
		("call (func(x, y) call (func(z) x - y * z) [3]) [5, 7]"
		,"-32"
		,"test that accesses arguments from outer function call"
		);
	run_test
		("call (func(y) call y []) [(func() 111)]"
		,"111"
		,"pass function as argument to function"
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

	run_test("[]", "[]", "empty list");
	//run_test("{}", "{}", "empty dict");

	return EXIT_SUCCESS;
}
#endif

