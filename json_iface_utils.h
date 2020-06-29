#ifndef JSON_IFACE_UTILS_H
#define JSON_IFACE_UTILS_H

#include <stdlib.h>
#include "ejson_iface.h"

/* returns non zero on error */
int jnode_print(struct jnode *p_root, struct linear_allocator *p_alloc, unsigned indent);

/* returns < 0 on error.
 * returns > 0 for different.
 * returns 0 for same. */
int are_different(struct jnode *p_x1, struct jnode *p_x2, struct linear_allocator *p_alloc);

#endif /* JSON_IFACE_UTILS_H */
