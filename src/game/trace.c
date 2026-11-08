#include "game/trace.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "x86/x86.h"

/* Other threads read emulated memory too (the status writer reads the
 * frame counter, src/hle/status.c); only the thread that started the trace
 * is traced. */
#ifdef _WIN32
typedef DWORD thread_id;
static thread_id self(void) { return GetCurrentThreadId(); }
static int same(thread_id a, thread_id b) { return a == b; }
#else
typedef pthread_t thread_id;
static thread_id self(void) { return pthread_self(); }
static int same(thread_id a, thread_id b) { return pthread_equal(a, b); }
#endif
static thread_id tracer;

int x86_trace_on;

#define PAGE_BITS 12
#define PAGE_SIZE (1u << PAGE_BITS)
#define NPAGES ((MEM_END - MEM_BASE) >> PAGE_BITS)

struct tpage {
	uint8_t state[PAGE_SIZE];
	uint8_t pre[PAGE_SIZE];
	uint8_t dirty; /* listed in dirty[] */
};

static struct tpage **pages;   /* NPAGES entries, allocated on first touch */
static uint32_t *dirty;        /* pages touched by the current trace */
static size_t ndirty, dirty_cap;

void x86_trace_access(uint32_t a, uint32_t n, int write)
{
	if (!same(self(), tracer))
		return;
	if (!pages && !(pages = calloc(NPAGES, sizeof *pages)))
		abort();
	for (uint32_t i = 0; i < n; i++) {
		uint32_t addr = a + i;
		if (addr < MEM_BASE || addr >= MEM_END)
			continue; /* x86_ptr faults next */
		uint32_t pi = (addr - MEM_BASE) >> PAGE_BITS, off = addr & (PAGE_SIZE - 1);
		struct tpage *p = pages[pi];
		if (!p && !(p = pages[pi] = calloc(1, sizeof *p)))
			abort();
		uint8_t s = p->state[off];
		if (!s) {
			if (!p->dirty) {
				p->dirty = 1;
				if (ndirty == dirty_cap) {
					dirty_cap = dirty_cap ? 2 * dirty_cap : 64;
					dirty = realloc(dirty, dirty_cap * sizeof *dirty);
					if (!dirty)
						abort();
				}
				dirty[ndirty++] = pi;
			}
			p->pre[off] = x86_mem[addr - MEM_BASE];
			s = TRACE_TOUCHED | (write ? 0 : TRACE_READ_FIRST);
		}
		if (write)
			s |= TRACE_WRITTEN;
		p->state[off] = s;
	}
}

void trace_begin(void)
{
	for (size_t k = 0; k < ndirty; k++) {
		memset(pages[dirty[k]]->state, 0, PAGE_SIZE);
		pages[dirty[k]]->dirty = 0;
	}
	ndirty = 0;
	tracer = self();
	x86_trace_on = 1;
}

void trace_end(void)
{
	x86_trace_on = 0;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static void push(struct trace_set *s, struct trace_byte b)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? 2 * s->cap : 256;
		s->b = realloc(s->b, s->cap * sizeof *s->b);
		if (!s->b)
			abort();
	}
	s->b[s->n++] = b;
}

void trace_collect(struct trace_set *out)
{
	qsort(dirty, ndirty, sizeof *dirty, cmp_u32);
	for (size_t k = 0; k < ndirty; k++) {
		struct tpage *p = pages[dirty[k]];
		uint32_t base = MEM_BASE + (dirty[k] << PAGE_BITS);
		for (uint32_t off = 0; off < PAGE_SIZE; off++)
			if (p->state[off]) {
				struct trace_byte b = { base + off, p->state[off], p->pre[off],
					x86_mem[base + off - MEM_BASE] };
				push(out, b);
			}
	}
}

const struct trace_byte *trace_find(const struct trace_set *s, uint32_t addr)
{
	size_t lo = 0, hi = s->n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (s->b[mid].addr < addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < s->n && s->b[lo].addr == addr ? &s->b[lo] : NULL;
}

void trace_set_clear(struct trace_set *s)
{
	s->n = 0;
}

void trace_set_free(struct trace_set *s)
{
	free(s->b);
	s->b = NULL;
	s->n = s->cap = 0;
}
