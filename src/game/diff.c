#include "game/diff.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *const reg_names[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

uint32_t diff_flags(const struct cpu *c)
{
	return c->cf | (c->pf << 2) | (c->zf << 6) | (c->sf << 7) | (c->df << 10) | (c->of << 11);
}

void diff_set_cpu(const uint32_t r[8], uint32_t f)
{
	memcpy(cpu.r, r, sizeof cpu.r);
	cpu.cf = f & 1;
	cpu.pf = (f >> 2) & 1;
	cpu.zf = (f >> 6) & 1;
	cpu.sf = (f >> 7) & 1;
	cpu.df = (f >> 10) & 1;
	cpu.of = (f >> 11) & 1;
}

static void add(struct diff_byte **v, uint32_t *n, uint32_t *cap, uint32_t addr, uint8_t value)
{
	if (*n == *cap) {
		*cap = *cap ? 2 * *cap : 64;
		*v = realloc(*v, *cap * sizeof **v);
		if (!*v)
			abort();
	}
	(*v)[(*n)++] = (struct diff_byte){ addr, value };
}

void diff_record_from_trace(struct diff_record *rec, uint32_t unit, const struct cpu *in,
                            const struct cpu *out, const struct trace_set *t)
{
	rec->unit = unit;
	memcpy(rec->in_r, in->r, sizeof rec->in_r);
	rec->in_flags = diff_flags(in);
	memcpy(rec->out_r, out->r, sizeof rec->out_r);
	rec->out_flags = diff_flags(out);
	rec->n_in = rec->n_out = 0;
	for (size_t i = 0; i < t->n; i++) {
		const struct trace_byte *b = &t->b[i];
		if (b->state & TRACE_READ_FIRST)
			add(&rec->in, &rec->n_in, &rec->cap_in, b->addr, b->pre);
		if ((b->state & TRACE_WRITTEN) && b->addr >= in->r[ESP])
			add(&rec->out, &rec->n_out, &rec->cap_out, b->addr, b->post);
	}
}

static const struct diff_byte *find(const struct diff_byte *v, uint32_t n, uint32_t addr)
{
	uint32_t lo = 0, hi = n;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (v[mid].addr < addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && v[lo].addr == addr ? &v[lo] : NULL;
}

struct why {
	char *s;
	size_t cap, len;
	int n;
};

static void note(struct why *w, const char *fmt, ...)
{
	if (w->n++ >= 6 || w->len + 1 >= w->cap)
		return;
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(w->s + w->len, w->cap - w->len, fmt, ap);
	va_end(ap);
	if (k > 0)
		w->len += (size_t)k < w->cap - w->len ? (size_t)k : w->cap - w->len - 1;
}

int diff_check(const struct game_unit *u, const struct diff_record *rec,
               const struct trace_set *t, char *why, size_t cap)
{
	struct why w = { why, cap, 0, 0 };
	if (cap)
		why[0] = 0;
	for (int r = 0; r < 8; r++)
		if (!(u->scratch_regs & REG_BIT(r)) && cpu.r[r] != rec->out_r[r])
			note(&w, " %s want %08x got %08x;", reg_names[r], rec->out_r[r], cpu.r[r]);
	if (u->flags_out && (diff_flags(&cpu) & 0x8c5) != (rec->out_flags & 0x8c5))
		note(&w, " flags want %03x got %03x;", rec->out_flags & 0x8c5, diff_flags(&cpu) & 0x8c5);
	for (uint32_t i = 0; i < rec->n_out; i++) {
		uint32_t v = rd8(rec->out[i].addr);
		if (v != rec->out[i].value)
			note(&w, " [%08x] want %02x got %02x;", rec->out[i].addr, rec->out[i].value, v);
	}
	uint32_t esp = rec->in_r[ESP];
	for (size_t i = 0; i < t->n; i++) {
		const struct trace_byte *b = &t->b[i];
		if ((b->state & TRACE_WRITTEN) && b->addr >= esp
		    && !find(rec->out, rec->n_out, b->addr) && b->post != b->pre)
			note(&w, " [%08x] written %02x, original kept %02x;", b->addr, b->post, b->pre);
		if ((b->state & TRACE_READ_FIRST) && !find(rec->in, rec->n_in, b->addr))
			note(&w, " [%08x] read, not an input of the original;", b->addr);
	}
	return w.n;
}

/* ---- file format: magic once, then records ---- */

static int put32(FILE *f, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	return fwrite(b, 1, 4, f) == 4;
}

static int get32(FILE *f, uint32_t *v)
{
	uint8_t b[4];
	if (fread(b, 1, 4, f) != 4)
		return 0;
	*v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
	return 1;
}

static int put_bytes(FILE *f, const struct diff_byte *v, uint32_t n)
{
	if (!put32(f, n))
		return 0;
	for (uint32_t i = 0; i < n; i++)
		if (!put32(f, v[i].addr) || fputc(v[i].value, f) == EOF)
			return 0;
	return 1;
}

int diff_write(FILE *f, const struct diff_record *rec)
{
	int ok = put32(f, rec->unit);
	for (int i = 0; i < 8; i++)
		ok = ok && put32(f, rec->in_r[i]);
	ok = ok && put32(f, rec->in_flags);
	for (int i = 0; i < 8; i++)
		ok = ok && put32(f, rec->out_r[i]);
	ok = ok && put32(f, rec->out_flags);
	return ok && put_bytes(f, rec->in, rec->n_in) && put_bytes(f, rec->out, rec->n_out);
}

static int get_bytes(FILE *f, struct diff_byte **v, uint32_t *n, uint32_t *cap)
{
	uint32_t k;
	if (!get32(f, &k) || k > (1u << 24))
		return 0;
	*n = 0;
	for (uint32_t i = 0; i < k; i++) {
		uint32_t a;
		int c;
		if (!get32(f, &a) || (c = fgetc(f)) == EOF)
			return 0;
		add(v, n, cap, a, (uint8_t)c);
	}
	return 1;
}

int diff_read(FILE *f, struct diff_record *rec)
{
	if (!get32(f, &rec->unit))
		return 0;
	int ok = 1;
	for (int i = 0; i < 8; i++)
		ok = ok && get32(f, &rec->in_r[i]);
	ok = ok && get32(f, &rec->in_flags);
	for (int i = 0; i < 8; i++)
		ok = ok && get32(f, &rec->out_r[i]);
	ok = ok && get32(f, &rec->out_flags);
	ok = ok && get_bytes(f, &rec->in, &rec->n_in, &rec->cap_in);
	ok = ok && get_bytes(f, &rec->out, &rec->n_out, &rec->cap_out);
	return ok ? 1 : -1;
}

uint64_t diff_input_hash(const struct diff_record *rec)
{
	uint64_t h = 0xcbf29ce484222325ull;
#define MIX(v) (h = (h ^ (uint64_t)(v)) * 0x100000001b3ull)
	MIX(rec->unit);
	for (int i = 0; i < 8; i++)
		MIX(rec->in_r[i]);
	MIX(rec->in_flags);
	for (uint32_t i = 0; i < rec->n_in; i++) {
		MIX(rec->in[i].addr);
		MIX(rec->in[i].value);
	}
#undef MIX
	return h;
}

void diff_record_free(struct diff_record *rec)
{
	free(rec->in);
	free(rec->out);
	memset(rec, 0, sizeof *rec);
}
