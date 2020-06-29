#include "ejson/json_iface_utils.h"
#include <string.h>
#include <math.h>

struct are_different_enum_state {
	int                      ret;
	struct jnode            *p_other;
	struct linear_allocator *p_alloc;

};

static int are_different_jdict_enumerate(struct jnode *p_node, const char *p_key, void *p_userctx) {
	struct are_different_enum_state *p_s = p_userctx;
	struct jnode othernode;
	assert(p_s->p_other->cls == JNODE_CLS_DICT);
	p_s->ret = p_s->p_other->d.dict.get_by_key(&othernode, p_s->p_other->d.dict.ctx, p_s->p_alloc, p_key);
	if (p_s->ret)
		return 1; /* could not get other key for some reason, stop enumeration */
	return are_different(p_node, &othernode, p_s->p_alloc);
}

/* < 0 on error
 * 0 if same
 * 1 if different */
int are_different(struct jnode *p_x1, struct jnode *p_x2, struct linear_allocator *p_alloc) {
	if (p_x1->cls != p_x2->cls)
		return 1;
	if (p_x1->cls == JNODE_CLS_INTEGER || p_x1->cls == JNODE_CLS_BOOL)
		return (p_x1->d.int_bool != p_x2->d.int_bool);
	if (p_x1->cls == JNODE_CLS_REAL)
		return fabs(p_x1->d.real - p_x2->d.real) > 1e-40; /* FIXME : rubbish */
	if (p_x1->cls == JNODE_CLS_STRING)
		return strcmp(p_x1->d.string.buf, p_x2->d.string.buf) != 0;
	if (p_x1->cls == JNODE_CLS_LIST) {
		unsigned i;
		if (p_x1->d.list.nb_elements != p_x2->d.list.nb_elements)
			return 1;
		for (i = 0; i < p_x1->d.list.nb_elements; i++) {
			struct jnode cx1;
			struct jnode cx2;
			int d;
			size_t save = linear_allocator_save(p_alloc);
			if  (   p_x1->d.list.get_elemenent(&cx1, p_x1->d.list.ctx, p_alloc, i)
			    ||  p_x2->d.list.get_elemenent(&cx2, p_x2->d.list.ctx, p_alloc, i)
			    ) {
				linear_allocator_restore(p_alloc, save);
				return -1;
			}
			d = are_different(&cx1, &cx2, p_alloc);
			linear_allocator_restore(p_alloc, save);
			if (d != 0)
				return d;
		}
		return 0;
	}
	if (p_x1->cls == JNODE_CLS_DICT) {
		struct are_different_enum_state state;
		if (p_x1->d.dict.nb_keys != p_x2->d.dict.nb_keys)
			return 1;
		state.p_alloc = p_alloc;
		state.p_other = p_x2;
		state.ret     = 0;
		if (p_x1->d.dict.enumerate(are_different_jdict_enumerate, p_x1->d.dict.ctx, p_alloc, &state) < 0)
			return -1;
		return state.ret;
	}
	assert(p_x1->cls == JNODE_CLS_NULL);
	return 0;
}

struct jnodeenum_state {
	struct linear_allocator *p_alloc;
	unsigned indent;
	int has_printed_something;

};

static int jnode_print_jdict_enumerate(struct jnode *p_dest, const char *p_key, void *p_userctx) {
	struct jnodeenum_state *p_s = p_userctx;
	if (p_s->has_printed_something)
		printf("%*s,", p_s->indent, "");
	else
		p_s->has_printed_something = 1;
	printf("\"%s\": ", p_key);
	return jnode_print(p_dest, p_s->p_alloc, p_s->indent + 1);
}

int jnode_print(struct jnode *p_root, struct linear_allocator *p_alloc, unsigned indent) {
	if (p_root->cls == JNODE_CLS_NULL) {
		printf("null\n");
	} else if (p_root->cls == JNODE_CLS_BOOL) {
		if (p_root->d.int_bool) {
			printf("true\n");
		} else {
			printf("false\n");
		}
	} else if (p_root->cls == JNODE_CLS_INTEGER) {
		printf("%lld\n", p_root->d.int_bool);
	} else if (p_root->cls == JNODE_CLS_REAL) {
		printf("%f\n", p_root->d.real);
	} else if (p_root->cls == JNODE_CLS_STRING) {
		printf("\"%s\"\n", p_root->d.string.buf);
	} else if (p_root->cls == JNODE_CLS_LIST) {
		if (p_root->d.list.nb_elements == 0) {
			printf("[]\n");
		} else {
			unsigned i;
			printf("[");
			for (i = 0; i < p_root->d.list.nb_elements; i++) {
				struct jnode tmp;
				size_t lap = p_alloc->pos;
				if (i)
					printf("%*s,", indent, "");
				if  (   p_root->d.list.get_elemenent(&tmp, p_root->d.list.ctx, p_alloc, i)
				    ||  jnode_print(&tmp, p_alloc, indent + 1)
				    ) {
					p_alloc->pos = lap;
					return -1;
				}
			}
			printf("%*s]\n", indent, "");
		}
	} else if (p_root->cls == JNODE_CLS_DICT) {
		if (p_root->d.dict.nb_keys == 0) {
			printf("{}\n");
		} else {
			printf("{");
			struct jnodeenum_state jes;
			jes.indent = indent;
			jes.p_alloc = p_alloc;
			jes.has_printed_something = 0;
			if (p_root->d.dict.enumerate(jnode_print_jdict_enumerate, p_root->d.dict.ctx, p_alloc, &jes))
				return -1;
			printf("%*s}\n", indent, "");
		}
	}
	return 0;
}

