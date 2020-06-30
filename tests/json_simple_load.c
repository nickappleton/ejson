#include "json_simple_load.h"
#include <string.h>
#include <math.h>
#include "ejson/src/parse_helpers.h"
#include "cop/cop_strdict.h"

struct jlistelem {
	struct jnode      node;
	struct jlistelem *p_next;
};

struct jdictnode {
	struct jnode            data;
	struct cop_strdict_node node;
};

static int eat_remaining_string(const char **pp_buf, struct cop_salloc_iface *p_alloc, const char **pp_str) {
	const char *p_buf = *pp_buf;
	char       *p_str;
	size_t      len;
	for (len = 0; p_buf[len] != '\"'; len++)
		if (p_buf[len] == '\0')
			return -1;
	if ((p_str = cop_salloc(p_alloc, len + 1, 1)) == NULL)
		return -1;
	memcpy(p_str, p_buf, len);
	p_str[len] = 0;
	p_buf += len + 1;
	*pp_str = p_str;
	*pp_buf = p_buf;
	return 0;
}

static int expect_string(const char **pp_buf, const char **pp_str, struct cop_salloc_iface *p_alloc) {
	return expect_char(pp_buf, '\"') || eat_remaining_string(pp_buf, p_alloc, pp_str);
}

static int get_list_element(struct jnode *p_dest, void *p_ctx, struct cop_salloc_iface *p_alloc, unsigned idx) {
	*p_dest = ((struct jnode *)p_ctx)[idx];
	return 0;
}

static struct jdictnode *initdictnode(const char *p_str, struct cop_salloc_iface *p_alloc) {
	struct jdictnode *p_ret;
	struct cop_strh key;
	cop_strh_init_shallow(&key, p_str);
	p_ret = cop_salloc(p_alloc, sizeof(struct jdictnode) + key.len + 1, 0);
	if (p_ret != NULL) {
		memcpy((char *)(p_ret + 1), p_str, key.len + 1);
		key.ptr = (void *)(p_ret + 1);
		cop_strdict_node_init(&(p_ret->node), &key, p_ret);
	}
	return p_ret;
}

struct jenum_ctx {
	struct cop_salloc_iface *p_alloc;
	void                    *p_userctx;
	jdict_enumerate_fn      *p_fn;

};

static int dict_enumerate_sub(void *p_context, struct cop_strdict_node *p_node, int depth) {
	struct jenum_ctx *p_ctx = p_context;
	size_t            save = cop_salloc_save(p_ctx->p_alloc);
	struct jdictnode *p_data = cop_strdict_node_to_data(p_node);
	struct cop_strh   key;
	cop_strdict_node_to_key(p_node, &key);
	if (p_ctx->p_fn(&(p_data->data), (char *)key.ptr, p_ctx->p_userctx))
		return 1;
	cop_salloc_restore(p_ctx->p_alloc, save);
	return 0;
}

static int dict_enumerate(jdict_enumerate_fn *p_fn, void *p_ctx, struct cop_salloc_iface *p_alloc, void *p_userctx) {
	struct jenum_ctx ctx;
	struct cop_strdict_node *p_root = p_ctx;
	ctx.p_userctx = p_userctx;
	ctx.p_alloc = p_alloc;
	ctx.p_fn = p_fn;
	return
		cop_strdict_enumerate
			(&p_root
			,dict_enumerate_sub
			,&ctx
			);
}

/* <0 for error >0 for not found 0 for found. */
static int dict_get_by_key(struct jnode *p_dest, void *ctx, struct cop_salloc_iface *p_alloc, const char *p_key) {
	struct cop_strdict_node *p_root = ctx;
	struct jdictnode        *data;
	if (cop_strdict_get_by_cstr(&p_root, p_key, (void **)&data))
		return 1;
	*p_dest = data->data;
	return 0;
}

static int expect_object(const char **pp_buf, struct jnode *p_root, struct cop_salloc_iface *p_alloc, struct cop_salloc_iface *p_temps) {
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
			size_t            savepos = cop_salloc_save(p_temps);
			unsigned          i;
			while (1) {
				struct jlistelem *elem;
				if (is_eof(pp_buf))
					return -1;
				if ((elem = cop_salloc(p_temps, sizeof(struct jlistelem), 0)) == NULL)
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
			if ((p_list = cop_salloc(p_alloc, sizeof(struct jnode) * nb_elements, 0)) == NULL)
				return -1; /* oom */
			for (i = 0; i < nb_elements; i++) {
				p_list[i] = p_head->node;
				p_head    = p_head->p_next;
			}
			cop_salloc_restore(p_temps, savepos);
		}
		p_root->cls                  = JNODE_CLS_LIST;
		p_root->d.list.nb_elements   = nb_elements;
		p_root->d.list.get_elemenent = get_list_element;
		p_root->d.list.ctx           = p_list;
	} else if (!expect_char(pp_buf, '{')) {
		struct cop_strdict_node *p_dict_root = cop_strdict_init();
		unsigned          nb_keys = 0;
		if (eat_whitespace(pp_buf), expect_char(pp_buf, '}')) {
			while (1) {
				const char        *keystr;
				struct jdictnode  *p_node;
				if  (   (is_eof(pp_buf))
					||  (eat_whitespace(pp_buf), expect_string(pp_buf, &keystr, p_temps))
					||  ((p_node = initdictnode(keystr, p_alloc)) == NULL)
					||  (eat_whitespace(pp_buf), expect_char(pp_buf, ':'))
					||  (eat_whitespace(pp_buf), expect_object(pp_buf, &(p_node->data), p_alloc, p_temps))
					)
					return -1;
				if (cop_strdict_insert(&p_dict_root, &(p_node->node)))
					return -1; /* duplicate key */
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
			if (expect_decimal_digit(pp_buf, &d))
				return -1;
			do {
				fp   += frac * (int)d;
				frac *= 0.1;
			} while (!expect_decimal_digit(pp_buf, &d));
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

int parse_json(struct jnode *p_root, struct cop_salloc_iface *p_alloc, struct cop_salloc_iface *p_temps, const char *p_json) {
	eat_whitespace(&p_json);
	if (expect_object(&p_json, p_root, p_alloc, p_temps))
		return -1;
	eat_whitespace(&p_json);
	return expect_char(&p_json, '\0');
}
