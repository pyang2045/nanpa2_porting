#include "game/state_view.h"

#include <string.h>

#include "game/state.h"
#include "game/vm.h"
#include "hle/replay.h"

void state_save_export(uint8_t out[STATE_SAVE_SIZE])
{
	memcpy(out, &G.save, STATE_SAVE_SIZE);
	/* the segment bases as the original's addresses (G addresses: gmem.h) */
	static const uint32_t segment_base[4] = { VM_SCRIPT, VM_SEGMENT1, VM_SEGMENT2, VM_SEGMENT2 };
	memcpy(out + offsetof(struct save_image, n.segment), segment_base, sizeof segment_base);
}

void state_save_import(const uint8_t *in, size_t n)
{
	if (n > STATE_SAVE_SIZE)
		n = STATE_SAVE_SIZE;
	uint32_t segment[4];
	memcpy(segment, G.save.n.segment, sizeof segment);
	memcpy(&G.save, in, n);
	memcpy(G.save.n.segment, segment, sizeof segment);
}

/* The fields of G that hold platform values: object handles, the window, the font handles, the
 * timer id, the locked pixel pointers and the DirectDraw lock descriptor. Their values are
 * whatever the platform layer hands out (in R3 and before, addresses of the emulated heap), so
 * the data hash leaves them out. The pitches, the animation and script addresses and every
 * other word of G are state and stay in. */
#define MASK(member, n) { offsetof(struct game_state, member), (n) }
static const struct { size_t off, len; } platform_fields[] = {
	MASK(rt.surface, sizeof G.rt.surface),
	MASK(sc.scratch_surface, sizeof G.sc.scratch_surface),
	MASK(sc.hwnd, sizeof G.sc.hwnd),
	MASK(sc.hinstance, sizeof G.sc.hinstance),
	MASK(sc.font, sizeof G.sc.font),
	MASK(sc.surface_desc, sizeof G.sc.surface_desc),
	MASK(sc.lock_ptr, sizeof G.sc.lock_ptr),
	MASK(sc.surface_desc_rest, sizeof G.sc.surface_desc_rest),
	MASK(sc.ddraw, sizeof G.sc.ddraw),
	MASK(sc.palette_obj, sizeof G.sc.palette_obj),
	MASK(sc.timer_id, sizeof G.sc.timer_id),
	MASK(map.v.locked_screen, 3 * sizeof(uint32_t)), /* locked_screen, _tiles, _sprites */
};

uint64_t state_data_hash(void)
{
	static uint8_t copy[sizeof G];
	memcpy(copy, &G, sizeof G);
	for (size_t i = 0; i < sizeof platform_fields / sizeof platform_fields[0]; i++)
		memset(copy + platform_fields[i].off, 0, platform_fields[i].len);
	return replay_hash(copy, sizeof copy, 0);
}

uint32_t state_frame(void)
{
	return rd32(0x446292);
}
