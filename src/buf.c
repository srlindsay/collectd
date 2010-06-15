#include <buf.h>
#include <stdlib.h>
#include <string.h>
#define BUF_SIZE 4096

static buf_t *buf_pool = NULL;
static buf_t *shadow_buf_pool = NULL;

buf_t* buf_new(void) {
	buf_t *b;
	if (buf_pool) {
		b = buf_pool;
		buf_pool = buf_pool->next;
	} else {
		b = malloc(sizeof(buf_t));
		if (!b) { return NULL; }
		b->start = malloc(BUF_SIZE);
		if (!b->start) { free(b); return NULL;}
		b->end = b->start + BUF_SIZE;
	}
	b->shadow = 0;
	b->first = b->last = b->start;
	b->next = NULL;
	return b;
}

buf_t* buf_new_shadow(char *start, char *end) {
	buf_t *b;
	if (shadow_buf_pool) {
		b = shadow_buf_pool;
		shadow_buf_pool = shadow_buf_pool->next;
	} else {
		b = malloc(sizeof(buf_t));
		if (!b) { return NULL; }
		b->shadow = 1;
	}
	b->start = b->first = start;
	b->end = b->last = end;
	b->next = NULL;
	return b;
}

void buf_free(buf_t *b) {
	if (b->shadow) {
		b->next = shadow_buf_pool;
		shadow_buf_pool = b;
	} else {
		b->next = buf_pool;
		buf_pool = b;
	} 
}

void buf_free_chain(buf_t *b) {
	buf_t *next;
	while(b) {
		next = b->next;
		buf_free(b);
		b = next;
	}
}

int buf_chain_len(buf_t *b) {
	int len = 0;
	for ( ; b; b=b->next) {
		len += (b->last - b->first);
	}
	return len;
}

void buf_print_chain(buf_t *b) {
	while(b) {
		int len = b->last - b->first;
		printf ("%.*s", len, b->first);
		b=b->next;
	}
}

void buf_print_chain_slice(chain_slice_t *c) {
	buf_t *b;
	for (b = c->start.b; b != c->end.b; b=b->next) {
		char *start;
		int len;
		if (b == c->start.b) {
			start = c->start.loc;
		} else {
			start = b->start;
		}
		if (b == c->end.b) {
			len = c->end.loc - start;
		} else {
			len = b->end - start;
		}
		printf ("%.*s", len, start);
	}
}

void buf_append_chain(buf_t **head, buf_t *chain) {
	buf_t *b;
	if (*head == NULL) {
		*head = chain;
		return;
	}

	for (b=*head;b->next;b=b->next){}
	b->next = chain;
}

int chain_memcpy(void *dest, chain_slice_t slice, int max_size) {
	int sz_left = max_size;

	buf_t *b = slice.start.b;
	char *c  = slice.start.loc;

	void *d = dest;
	int sz;

	while (1) {
		if (b == slice.end.b) {
			sz = slice.end.loc - c;
		} else {
			sz = b->last - c;
		}
		if (sz > sz_left) {
			/* we've hit the buffer limit */
			memcpy(d, c, sz_left);
			d += sz_left;
			break;
		} else {
			memcpy(d, c, sz);
			d += sz;
			sz_left -= sz;
		}

		if (b == slice.end.b || b == NULL) {
			break;
		}
		b = b->next;
		c = b->first;
	}
	return d - dest;
}

void chain_free_offset (buf_t **start, chain_offset_t end) {
	buf_t *b, *next;

	b = *start;
	while (b != end.b && b != NULL) {
		next = b->next;
		buf_free(b);
		b = next;
	}
	if (b) {
		if (b->last == end.loc) {
			/* end points to the actual end of the chain, free everything */
			buf_free(b);
		} else {
			b->first = end.loc;
		}
	}
	*start = b;
}

chain_offset_t chain_next (buf_t *b, char *c) {
	chain_offset_t next;
	if (c < b->last-1) {
		next.b = b;
		next.loc = c+1;
	} else {
		if (b->next) {
			next.b = b->next;
			next.loc = b->next->first;
		} else {
			next.b = NULL;
			next.loc = NULL;
		}
	}
	return next;
}
