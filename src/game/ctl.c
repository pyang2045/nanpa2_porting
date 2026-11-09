#include "game/ctl.h"

#include <setjmp.h>

#include "game/state.h"
#include "hle/hle.h"

int ctl_pump(void)
{
	int r = win32_pump();
	/* the native pump returns from PeekMessageA/DispatchMessageA, which leave
	 * ECX and EDX changed (hle.c, override_call 0x401bf0) */
	cpu.r[ECX] = 0;
	cpu.r[EDX] = 0;
	return r;
}

static jmp_buf *catch_top;

_Noreturn void ctl_quit(void)
{
	COST(1);
	if (!catch_top)
		x86_trap(0x40638d, "WM_QUIT with nothing to unwind to");
	longjmp(*catch_top, 1);
}

void ctl_pump_or_quit(void)
{
	if (!ctl_pump())
		ctl_quit();
}

int ctl_catch(void (*fn)(void *), void *arg)
{
	jmp_buf here, *outer = catch_top;
	if (setjmp(here)) {
		catch_top = outer;
		return 0;
	}
	catch_top = &here;
	fn(arg);
	catch_top = outer;
	return 1;
}

uint32_t hle_invoke(uint32_t addr, int nargs, const uint32_t *args)
{
	uint32_t esp = cpu.r[ESP];
	for (int i = nargs - 1; i >= 0; i--)
		push32(args[i]);
	push32(X86_RET_SENTINEL);
	hle_call(addr);
	cpu.r[ESP] = esp;
	return cpu.r[EAX];
}

uint32_t ctl_piece(void (*fn)(void *), void *arg, uint32_t next_pc)
{
	if (ctl_catch(fn, arg))
		return next_pc;
	/* WM_QUIT: ctl_quit charged the `mov esp, [top]`; set it and leave for the
	 * native tail (pop ebp, ret to WinMain, which the dispatcher counts) */
	cpu.r[ESP] = RT(top_esp);
	return 0x406393;
}
