#include "json_simple_load.h"
#include <string.h>
#include <math.h>

struct jdictnode {
	uint_fast32_t    key;
	struct jnode     data;
	struct jdictnode *p_children[4];
};

struct jlistelem {
	struct jnode      node;
	struct jlistelem *p_next;
};

static void eat_whitespace(const char **pp_buf) {
	const char *p_buf = *pp_buf;
	while (*p_buf == '\t' || *p_buf == '\r' || *p_buf == '\n' || *p_buf == ' ')
		p_buf++;
	*pp_buf = p_buf;
}

static int expect_char(const char **pp_buf, char c) {
	const char *p_buf = *pp_buf;
	if (*p_buf != c)
		return 1;
	*pp_buf = p_buf + 1;
	return 0;
}

static int expect_digit(const char **pp_buf, unsigned *digit) {
	const char *p_buf = *pp_buf;
	if (*p_buf < '0' || *p_buf > '9')
		return 1;
	*digit  = (*p_buf) - '0';
	*pp_buf = p_buf + 1;
	return 0;
}

static int expect_num(const char **pp_buf, unsigned long long *p_num) {
	unsigned long long num;
	unsigned d;
	if (expect_digit(pp_buf, &d))
		return -1;
	num = d;
	while (!expect_digit(pp_buf, &d))
		num = num * 10 + d;
	*p_num = num;
	return 0;
}

static int expect_consecutive(const char **pp_buf, const char *p_expect) {
	const char *p_buf = *pp_buf;
	while (*p_expect != '\0' && *p_buf == *p_expect) {
		p_buf++;
		p_expect++;
	}
	if (*p_expect != '\0')
		return -1;
	*pp_buf = p_buf;
	return 0;
}

static int is_eof(const char **pp_buf) {
	const char *p_buf = *pp_buf;
	return *p_buf == '\0';
}

static int eat_remaining_string(const char **pp_buf, struct linear_allocator *p_alloc, const char **pp_str) {
	const char *p_buf = *pp_buf;
	char       *p_str;
	size_t      len;
	for (len = 0; p_buf[len] != '\"'; len++)
		if (p_buf[len] == '\0')
			return -1;
	if ((p_str = linear_allocator_alloc_align(p_alloc, 1, len + 1)) == NULL)
		return -1;
	memcpy(p_str, p_buf, len);
	p_str[len] = 0;
	p_buf += len + 1;
	*pp_str = p_str;
	*pp_buf = p_buf;
	return 0;
}

static int expect_string(const char **pp_buf, const char **pp_str, struct linear_allocator *p_alloc) {
	return expect_char(pp_buf, '\"') || eat_remaining_string(pp_buf, p_alloc, pp_str);
}

static int get_list_element(struct jnode *p_dest, void *p_ctx, struct linear_allocator *p_alloc, unsigned idx) {
	*p_dest = ((struct jnode *)p_ctx)[idx];
	return 0;
}

static size_t hashfnv(const char *p_str, uint_fast32_t *p_hash) {
	uint_fast32_t hash = 2166136261;
	uint_fast32_t c;
	size_t        length = 0;
	while ((c = p_str[length++]) != 0)
		hash = ((hash ^ c) * 16777619) & 0xFFFFFFFFu;
	*p_hash = hash;
	return length;
}

static struct jdictnode *initdictnode(const char *p_str, struct linear_allocator *p_alloc) {
	struct jdictnode *p_ret;
	uint_fast32_t     hash;
	size_t            length = hashfnv(p_str, &hash);
	p_ret = linear_allocator_alloc(p_alloc, sizeof(struct jdictnode) + length + 1);
	if (p_ret != NULL) {
		p_ret->key = hash;
		memset(p_ret->p_children, 0, sizeof(p_ret->p_children));
		memcpy(p_ret + 1, p_str, length + 1);
	}
	return p_ret;
}

static int dict_enumerate(jdict_enumerate_fn *p_fn, void *ctx, struct linear_allocator *p_alloc, void *p_userctx) {
	/* note there are no failure modes for this enumerate function hence the return values of 1. */
	if (ctx != NULL) {
		size_t            save = linear_allocator_save(p_alloc);
		struct jdictnode *node = ctx;
		unsigned          i;
		if (p_fn(&(node->data), (const char *)(node + 1), p_userctx))
			return 1;
		for (i = 0; i < 4; i++)
			if (dict_enumerate(p_fn, node->p_children[i], p_alloc, p_userctx))
				return 1;
		/* do this as a checking courtesy - hopefully this will corrupt some memory! */
		linear_allocator_restore(p_alloc, save);
	}
	return 0;
}

/* <0 for error >0 for not found 0 for found. */
static int dict_get_by_key(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, const char *p_key) {
	uint_fast32_t hash, ukey;
	struct jdictnode *p_ins = ctx;
	(void)hashfnv(p_key, &hash);
	ukey = hash;
	while (p_ins != NULL) {
		if (p_ins->key == hash && !strcmp((const char *)(p_ins + 1), p_key)) {
			*p_dest = p_ins->data;
			return 0;
		}
		p_ins    = p_ins->p_children[ukey & 0x3];
		ukey   >>= 2;
	}
	return 1;
}

static int expect_object(const char **pp_buf, struct jnode *p_root, struct linear_allocator *p_alloc, struct linear_allocator *p_temps) {
	if (!expect_consecutive(pp_buf, "true")) {
		p_root->cls = JNODE_CLS_BOOL;
		p_root->d.int_bool = 1;
	} else if (!expect_consecutive(pp_buf, "false")) {
		p_root->cls = JNODE_CLS_BOOL;
		p_root->d.int_bool = 0;
	} else if (!expect_consecutive(pp_buf, "null")) {
		p_root->cls = JNODE_CLS_NULL;
	} else if (!expect_char(pp_buf, '\"')) {
		p_root->cls = JNODE_CLS_STRING;
		if (eat_remaining_string(pp_buf, p_alloc, &(p_root->d.string.buf)))
			return -1;
	} else if (!expect_char(pp_buf, '[')) {
		unsigned          nb_elements = 0;
		struct jnode     *p_list      = NULL;
		if (eat_whitespace(pp_buf), expect_char(pp_buf, ']')) {
			struct jlistelem *p_head  = NULL;
			struct jlistelem *p_last  = NULL;
			size_t            savepos = linear_allocator_save(p_temps);
			unsigned          i;
			while (1) {
				struct jlistelem *elem;
				if (is_eof(pp_buf))
					return -1;
				if ((elem = linear_allocator_alloc(p_temps, sizeof(struct jlistelem))) == NULL)
					return -1;
				if (expect_object(pp_buf, &(elem->node), p_alloc, p_temps))
					return -1;
				if (p_last == NULL) {
					p_last         = elem;
					p_head         = elem;
				} else {
					p_last->p_next = elem;
					p_last         = elem;
				}
				nb_elements++;
				eat_whitespace(pp_buf);
				if (!expect_char(pp_buf, ',')) {
					eat_whitespace(pp_buf);
					continue;
				}
				if (expect_char(pp_buf, ']'))
					return -1;
				break;
			}
			if ((p_list = linear_allocator_alloc(p_alloc, sizeof(struct jnode) * nb_elements)) == NULL)
				return -1; /* oom */
			for (i = 0; i < nb_elements; i++) {
				p_list[i] = p_head->node;
				p_head    = p_head->p_next;
			}
			linear_allocator_restore(p_temps, savepos);
		}
		p_root->cls                  = JNODE_CLS_LIST;
		p_root->d.list.nb_elements   = nb_elements;
		p_root->d.list.get_elemenent = get_list_element;
		p_root->d.list.ctx           = p_list;
	} else if (!expect_char(pp_buf, '{')) {
		struct jdictnode *p_dict_root = NULL;
		unsigned          nb_keys = 0;
		if (eat_whitespace(pp_buf), expect_char(pp_buf, '}')) {
			while (1) {
				const char        *keystr;
				struct jdictnode  *p_key;
				struct jdictnode **pp_ref = &p_dict_root;
				struct jdictnode  *p_ins  = *pp_ref;
				uint_fast32_t      ukey;
				if  (   (is_eof(pp_buf))
					||  (eat_whitespace(pp_buf), expect_string(pp_buf, &keystr, p_temps))
					||  ((p_key = initdictnode(keystr, p_alloc)) == NULL)
					||  (eat_whitespace(pp_buf), expect_char(pp_buf, ':'))
					||  (eat_whitespace(pp_buf), expect_object(pp_buf, &(p_key->data), p_alloc, p_temps))
					)
					return -1;
				ukey = p_key->key;
				while (p_ins != NULL) {
					if (p_ins->key == p_key->key && !strcmp((const char *)(p_ins + 1), (const char *)(p_key + 1)))
						return -1; /* duplicate key */
					pp_ref   = &(p_ins->p_children[ukey & 0x3]);
					ukey   >>= 2;
					p_ins    = *pp_ref;
				}
				*pp_ref = p_key;
				nb_keys++;
				eat_whitespace(pp_buf);
				if (!expect_char(pp_buf, ',')) {
					eat_whitespace(pp_buf);
					continue;
				}
				if (expect_char(pp_buf, '}'))
					return -1;
				break;
			}
		}
		p_root->cls               = JNODE_CLS_DICT;
		p_root->d.dict.nb_keys    = nb_keys;
		p_root->d.dict.ctx        = p_dict_root;
		p_root->d.dict.enumerate  = dict_enumerate;
		p_root->d.dict.get_by_key = dict_get_by_key;
	} else {
		int neg = !expect_char(pp_buf, '-');
		unsigned d;
		unsigned long long ipart;
		if (expect_num(pp_buf, &ipart))
			return -1;
		p_root->cls = JNODE_CLS_INTEGER;
		if (!expect_char(pp_buf, '.')) {
			double frac = 0.1;
			double fp = 0.0;
			if (expect_digit(pp_buf, &d))
				return -1;
			do {
				fp   += frac * (int)d;
				frac *= 0.1;
			} while (!expect_digit(pp_buf, &d));
			p_root->cls = JNODE_CLS_REAL;
			p_root->d.real = fp + ipart;
		}
		if ((!expect_char(pp_buf, 'e')) || (!expect_char(pp_buf, 'E'))) {
			int pos_exp = (!expect_char(pp_buf, '+')) || expect_char(pp_buf, '-');
			unsigned long long num;
			int expval;
			if (expect_num(pp_buf, &num))
				return -1;
			expval = (pos_exp) ? (int)num : -(int)num;
			if (p_root->cls != JNODE_CLS_REAL)
				p_root->d.real = ipart;
			p_root->d.real *= pow(10.0, expval);
			p_root->cls = JNODE_CLS_REAL;
		}
		if (p_root->cls == JNODE_CLS_INTEGER) {
			p_root->d.int_bool = (long long)ipart;
			if (neg)
				p_root->d.int_bool = -p_root->d.int_bool;
		} else {
			if (neg)
				p_root->d.real = -p_root->d.real;
		}
	}
	return 0;
}

int parse_json(struct jnode *p_root, struct linear_allocator *p_alloc, struct linear_allocator *p_temps, const char *p_json) {
	eat_whitespace(&p_json);
	if (expect_object(&p_json, p_root, p_alloc, p_temps))
		return -1;
	eat_whitespace(&p_json);
	return expect_char(&p_json, '\0');
}
