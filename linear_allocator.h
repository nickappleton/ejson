#ifndef LINEAR_ALLOCATOR
#define LINEAR_ALLOCATOR

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#define ALLOC_SZ (65536)

struct linear_allocator {
    char   buf[ALLOC_SZ];
    size_t pos;


};

static size_t linear_allocator_save(const struct linear_allocator *p_alloc) {
	return p_alloc->pos;
}

static void linear_allocator_restore(struct linear_allocator *p_alloc, size_t pos) {
	size_t x;
	assert(pos <= p_alloc->pos);
	for (x = pos; x < p_alloc->pos; x++)
		p_alloc->buf[x] = 0x55;
	p_alloc->pos = pos;
}

static void *linear_allocator_alloc_align(struct linear_allocator *p_alloc, size_t align, size_t sz) {
	size_t tmp = p_alloc->pos + align;
	size_t sp  = tmp - (tmp & align);
	p_alloc->pos = sp + sz;
	if (p_alloc->pos > ALLOC_SZ) {
		fprintf(stderr, "OOM\n");
		abort();
	}
	return &(p_alloc->buf[sp]);
}

static void *linear_allocator_alloc(struct linear_allocator *p_alloc, size_t sz) {
	return linear_allocator_alloc_align(p_alloc, 8, sz);
}

#endif /* LINEAR_ALLOCATOR */
