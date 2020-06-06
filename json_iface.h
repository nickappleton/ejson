#ifndef JSON_IFACE
#define JSON_IFACE

#include "linear_allocator.h"

#define JNODE_CLS_NULL    (0)
#define JNODE_CLS_STRING  (1)
#define JNODE_CLS_INTEGER (2)
#define JNODE_CLS_REAL    (3)
#define JNODE_CLS_BOOL    (4)
#define JNODE_CLS_DICT    (5)
#define JNODE_CLS_LIST    (6)

struct jnode;

/* return < 0 to signal termination due to error
 * return > 0 to signal graceful termination
 * return 0 to continue enumerating */
typedef int (jdict_enumerate_fn)(struct jnode *p_dest, const char *p_key, void *p_userctx);

struct jnode {
	int cls;
	union {
		long long       int_bool;
		double          real;
		struct {
			const char *buf;
		} string;
		struct {
			void       *ctx;
			unsigned    nb_elements;

			/* return != 0 for error */
			int       (*get_elemenent)(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, unsigned idx);
		} list;
		struct {
			void       *ctx;
			unsigned    nb_keys;

			/* return < 0 for error, > 0 for callback requested termination or 0 for finished successfully */
			int       (*enumerate)(jdict_enumerate_fn *p_fn, void *ctx, struct linear_allocator *p_alloc, void *p_userctx);

			/* return < 0 for error, > 0 for key not found or 0 for successful */
			int       (*get_by_key)(struct jnode *p_dest, void *ctx, struct linear_allocator *p_alloc, const char *p_key);
		} dict;
	} d;
};

#endif
