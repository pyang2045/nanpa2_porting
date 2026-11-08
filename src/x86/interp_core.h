/*
 * The interpreter's executor, included once per mode (no include guard):
 *   interp.c    the backend: real memory, polls, probes, HLE calls
 *   lockstep.c  the shadow of one recompiled block: memory writes go to a
 *               journal, no host calls; it stops at the block's end
 *
 * The includer defines:
 *   IFN(name)              a mode-specific name
 *   SHADOW                 0 or 1
 *   MRD8/16/32(a), MWR8/16/32(a, v)   emulated memory
 *   COUNT(n)               instructions executed (x86_icount)
 *   STEP()                 one instruction decoded and run (speed statistics)
 *   HLE_CALL(pc)           hle_call (interpreter only)
 *   HLE_JUMP(pc)           hle_jump, returns the next pc (interpreter only)
 *   POLL(at)               a taken backward branch at `at` (X86_POLL)
 *   STR_MOVS/STOS/LODS/SCAS/CMPS(sz, rep)   string instructions
 *
 * Semantics follow tools/recomp.py's lifter instruction by instruction (the
 * two are compared in lockstep): the same flag helpers from x86.h, the same
 * order of operand reads and writes, a poll on every taken direct branch to
 * game code at or before the branch, one count per instruction plus one per
 * repeated string iteration.
 */

X86_HOT void IFN(push32)(uint32_t v) { cpu.r[ESP] -= 4; MWR32(cpu.r[ESP], v); }
X86_HOT void IFN(push16)(uint32_t v) { cpu.r[ESP] -= 2; MWR16(cpu.r[ESP], v); }
X86_HOT uint32_t IFN(pop32)(void) { uint32_t v = MRD32(cpu.r[ESP]); cpu.r[ESP] += 4; return v; }
X86_HOT uint32_t IFN(pop16)(void) { uint32_t v = MRD16(cpu.r[ESP]); cpu.r[ESP] += 2; return v; }

X86_HOT uint32_t IFN(mrd)(uint32_t a, int sz)
{
	return sz == 4 ? MRD32(a) : sz == 2 ? MRD16(a) : MRD8(a);
}

X86_HOT void IFN(mwr)(uint32_t a, uint32_t v, int sz)
{
	if (sz == 4) MWR32(a, v); else if (sz == 2) MWR16(a, v); else MWR8(a, v);
}

X86_HOT uint32_t IFN(rm_get)(const struct dec *d, int sz)
{
	return d->modreg ? reg_get(d->rm, sz) : IFN(mrd)(dec_ea(d), sz);
}

X86_HOT void IFN(rm_put)(const struct dec *d, int sz, uint32_t v)
{
	if (d->modreg)
		reg_put(d->rm, sz, v);
	else
		IFN(mwr)(dec_ea(d), v, sz);
}

/* Run from pc. Interpreter: until a return to X86_RET_SENTINEL. Shadow:
 * `limit` instructions, or up to a transfer out of game code (an HLE
 * target or the sentinel). Returns the next pc. `native` is an address
 * whose override is skipped when it is fetched next: the entry of an
 * oracle run (X86_ORACLE_TAG), or a direct jump inside a reimplemented
 * function's original body back to its entry (HLE_GAME_OVERRIDE; the
 * recompile turns those into gotos, and recomp.py refuses direct jumps to
 * the entry from anywhere else). */
static uint32_t IFN(run)(uint32_t pc, uint32_t limit, uint32_t native)
{
	(void)limit;
#if SHADOW
	uint32_t steps = 0;
#endif
	for (;;) {
		if (pc == X86_RET_SENTINEL)
			return pc;
#if !SHADOW
		/* the oracle of an opcode handler ends at its exit (game.c) */
		if (x86_stop_esp && x86_stop_at(pc))
			return pc;
#endif
		uint32_t off = pc - CODE_START;
		if ((off >= CODE_LEN || x86_interp_override[off]) && pc != native) {
#if SHADOW
			return pc;
#else
			if (off < CODE_LEN && x86_interp_override[off] == OVR_JUMP) {
				/* an opcode handler: no return address, the C
				 * handler gives the next pc; inside an oracle it is
				 * the tagged entry of the original, run here */
				pc = HLE_JUMP(pc);
				if (x86_is_oracle_pc(pc)) {
					pc &= ~X86_ORACLE_TAG;
					native = pc;
				}
				continue;
			}
			/* as the recompiled dispatch: hle_call pops the return
			 * address and the caller continues there */
			uint32_t ra = rd32(cpu.r[ESP]);
			HLE_CALL(pc);
			pc = ra;
			continue;
#endif
		}
		native = 0;
#if SHADOW
		if (steps++ == limit)
			return pc;
#endif
		const struct dec *d = &dec_cache[off];
		if (!d->len)
			d = x86_decode(pc);
#if !SHADOW
		if (d->probe)
			x86_probe(pc);
#endif
		uint32_t nxt = pc + d->len;
		uint32_t op = d->op;
		int sz = d->osz, bits = sz * 8;
		uint32_t a, t;
		COUNT(1);
		STEP();

#define JUMP(target) do { \
	uint32_t t_ = (target); \
	int game_ = t_ - CODE_START < CODE_LEN && x86_interp_override[t_ - CODE_START] >= OVR_GAME; \
	if (t_ <= pc && (game_ || !x86_is_hle(t_))) \
		POLL(pc); \
	if (game_ && x86_interp_override[t_ - CODE_START] == OVR_GAME) \
		native = t_; \
	pc = t_; \
	goto next; \
} while (0)

		if (op < 0x40) { /* add or adc sbb and sub xor cmp */
			int k = (int)(op >> 3) & 7, f = (int)op & 7;
			if (f >= 6)
				goto bad;
			if (!(f & 1)) {
				sz = 1;
				bits = 8;
			}
			switch (f >> 1) {
			case 0:
				t = alu(k, IFN(rm_get)(d, sz), reg_get(d->reg, sz), bits);
				if (k != 7)
					IFN(rm_put)(d, sz, t);
				break;
			case 1:
				t = alu(k, reg_get(d->reg, sz), IFN(rm_get)(d, sz), bits);
				if (k != 7)
					reg_put(d->reg, sz, t);
				break;
			default:
				t = alu(k, reg_get(EAX, sz), d->imm, bits);
				if (k != 7)
					reg_put(EAX, sz, t);
				break;
			}
			pc = nxt;
			continue;
		}

		switch (op) {
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
			reg_put(d->reg, sz, f_inc(reg_get(d->reg, sz), bits));
			break;
		case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
			reg_put(d->reg, sz, f_dec(reg_get(d->reg, sz), bits));
			break;
		case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
			if (sz == 2)
				IFN(push16)(cpu.r[d->reg] & 0xffffu);
			else
				IFN(push32)(cpu.r[d->reg]);
			break;
		case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
			if (sz == 2) {
				t = IFN(pop16)();
				set16(d->reg, t);
			} else {
				t = IFN(pop32)();
				cpu.r[d->reg] = t;
			}
			break;
		case 0x60: {
			uint32_t sp = cpu.r[ESP];
			IFN(push32)(cpu.r[EAX]); IFN(push32)(cpu.r[ECX]); IFN(push32)(cpu.r[EDX]);
			IFN(push32)(cpu.r[EBX]); IFN(push32)(sp); IFN(push32)(cpu.r[EBP]);
			IFN(push32)(cpu.r[ESI]); IFN(push32)(cpu.r[EDI]);
			break;
		}
		case 0x61:
			cpu.r[EDI] = IFN(pop32)(); cpu.r[ESI] = IFN(pop32)(); cpu.r[EBP] = IFN(pop32)();
			cpu.r[ESP] += 4;
			cpu.r[EBX] = IFN(pop32)(); cpu.r[EDX] = IFN(pop32)(); cpu.r[ECX] = IFN(pop32)();
			cpu.r[EAX] = IFN(pop32)();
			break;
		case 0x68: case 0x6a: /* the lifter pushes 32 bits for any immediate */
			IFN(push32)(d->imm);
			break;
		case 0x69: case 0x6b:
			reg_put(d->reg, sz, f_imul2(IFN(rm_get)(d, sz), d->imm, bits));
			break;
		case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
			if (cond(op & 15))
				JUMP(d->imm);
			break;
		case 0x80: case 0x81: case 0x83: {
			int k = d->reg;
			if (op == 0x80) {
				sz = 1;
				bits = 8;
			}
			t = alu(k, IFN(rm_get)(d, sz), d->imm, bits);
			if (k != 7)
				IFN(rm_put)(d, sz, t);
			break;
		}
		case 0x84: case 0x85:
			if (op == 0x84) {
				sz = 1;
				bits = 8;
			}
			f_logic(IFN(rm_get)(d, sz) & reg_get(d->reg, sz), bits);
			break;
		case 0x86: case 0x87:
			if (op == 0x86)
				sz = 1;
			t = IFN(rm_get)(d, sz);
			IFN(rm_put)(d, sz, reg_get(d->reg, sz));
			reg_put(d->reg, sz, t);
			break;
		case 0x88: IFN(rm_put)(d, 1, reg_get(d->reg, 1)); break;
		case 0x89: IFN(rm_put)(d, sz, reg_get(d->reg, sz)); break;
		case 0x8a: reg_put(d->reg, 1, IFN(rm_get)(d, 1)); break;
		case 0x8b: reg_put(d->reg, sz, IFN(rm_get)(d, sz)); break;
		case 0x8d:
			if (d->modreg)
				goto bad;
			reg_put(d->reg, sz, dec_ea(d));
			break;
		case 0x8f: /* pop r/m: the address uses ESP after the pop */
			if (d->reg != 0)
				goto bad;
			t = sz == 2 ? IFN(pop16)() : IFN(pop32)();
			IFN(rm_put)(d, sz, t);
			break;
		case 0x90:
			break;
		case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
			t = reg_get(EAX, sz);
			reg_put(EAX, sz, reg_get(d->reg, sz));
			reg_put(d->reg, sz, t);
			break;
		case 0x98:
			if (sz == 2)
				set16(EAX, (uint32_t)(int16_t)(int8_t)cpu.r[EAX]);
			else
				cpu.r[EAX] = (uint32_t)(int32_t)(int16_t)cpu.r[EAX];
			break;
		case 0x99:
			if (sz == 2)
				set16(EDX, (cpu.r[EAX] & 0x8000u) ? 0xffffu : 0);
			else
				cpu.r[EDX] = (cpu.r[EAX] & 0x80000000u) ? 0xffffffffu : 0;
			break;
		case 0x9c: IFN(push32)(get_eflags()); break;
		case 0x9d: set_eflags(IFN(pop32)()); break;
		case 0x9e: set_eflags((get_eflags() & ~0xffu) | ((cpu.r[EAX] >> 8) & 0xffu)); break;
		case 0x9f: set8h(EAX, get_eflags() & 0xffu); break;
		case 0xa0: set8l(EAX, MRD8(d->imm)); break;
		case 0xa1: reg_put(EAX, sz, IFN(mrd)(d->imm, sz)); break;
		case 0xa2: MWR8(d->imm, cpu.r[EAX]); break;
		case 0xa3: IFN(mwr)(d->imm, cpu.r[EAX], sz); break;
		case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab:
		case 0xac: case 0xad: case 0xae: case 0xaf: {
			int ssz = (op & 1) ? sz : 1;
			int kind = (int)(op & 0xfe);
			int rep = REP_NONE;
			if (d->rep)
				rep = kind == 0xa6 || kind == 0xae ? (d->rep == 0xf3 ? REP_E : REP_NE)
								   : (d->rep == 0xf3 ? REP_PLAIN : REP_NE);
			uint32_t c0 = cpu.r[ECX];
			switch (kind) {
			case 0xa4: STR_MOVS(ssz, rep); break;
			case 0xa6: STR_CMPS(ssz, rep); break;
			case 0xaa: STR_STOS(ssz, rep); break;
			case 0xac: STR_LODS(ssz, rep); break;
			default: STR_SCAS(ssz, rep); break;
			}
			if (rep != REP_NONE)
				COUNT(c0 - cpu.r[ECX]);
			break;
		}
		case 0xa8: f_logic(cpu.r[EAX] & d->imm, 8); break;
		case 0xa9: f_logic(cpu.r[EAX] & d->imm, bits); break;
		case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
			reg_put(d->reg, 1, d->imm);
			break;
		case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
			reg_put(d->reg, sz, d->imm);
			break;
		case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
			if (!(op & 1)) {
				sz = 1;
				bits = 8;
			}
			uint32_t n = op <= 0xc1 ? d->imm : op <= 0xd1 ? 1u : cpu.r[ECX] & 0xffu;
			a = IFN(rm_get)(d, sz);
			switch (d->reg) {
			case 0: t = f_rol(a, n, bits); break;
			case 1: t = f_ror(a, n, bits); break;
			case 2: t = f_rcl(a, n, bits); break;
			case 3: t = f_rcr(a, n, bits); break;
			case 5: t = f_shr(a, n, bits); break;
			case 7: t = f_sar(a, n, bits); break;
			default: t = f_shl(a, n, bits); break;
			}
			IFN(rm_put)(d, sz, t);
			break;
		}
		case 0xc2:
			pc = IFN(pop32)();
			cpu.r[ESP] += d->imm;
			continue;
		case 0xc3:
			pc = IFN(pop32)();
			continue;
		case 0xc6: case 0xc7:
			if (d->reg != 0)
				goto bad;
			IFN(rm_put)(d, op == 0xc6 ? 1 : sz, d->imm);
			break;
		case 0xc9:
			cpu.r[ESP] = cpu.r[EBP];
			cpu.r[EBP] = IFN(pop32)();
			break;
		case 0xcc:
			x86_trap(pc, "int3");
		case 0xe0: case 0xe1: case 0xe2:
			if (--cpu.r[ECX] != 0 && (op == 0xe2 || (op == 0xe1) == (cpu.zf != 0)))
				JUMP(d->imm);
			break;
		case 0xe3:
			if (cpu.r[ECX] == 0)
				JUMP(d->imm);
			break;
		case 0xe8: /* to game code: jump there; else the HLE check above */
			IFN(push32)(nxt);
			pc = d->imm;
			continue;
		case 0xe9: case 0xeb:
			JUMP(d->imm);
		case 0xf5: cpu.cf ^= 1; break;
		case 0xf8: cpu.cf = 0; break;
		case 0xf9: cpu.cf = 1; break;
		case 0xfc: cpu.df = 0; break;
		case 0xfd: cpu.df = 1; break;
		case 0xf6: case 0xf7:
			if (op == 0xf6) {
				sz = 1;
				bits = 8;
			}
			switch (d->reg) {
			case 0: case 1: f_logic(IFN(rm_get)(d, sz) & d->imm, bits); break;
			case 2: IFN(rm_put)(d, sz, ~IFN(rm_get)(d, sz)); break;
			case 3: IFN(rm_put)(d, sz, f_neg(IFN(rm_get)(d, sz), bits)); break;
			case 4:
				a = IFN(rm_get)(d, sz);
				if (sz == 1) mul8(a); else if (sz == 2) mul16(a); else mul32(a);
				break;
			case 5:
				a = IFN(rm_get)(d, sz);
				if (sz == 1) imul8(a); else if (sz == 2) imul16(a); else imul32(a);
				break;
			case 6:
				a = IFN(rm_get)(d, sz);
				if (sz == 1) div8(a); else if (sz == 2) div16(a); else div32(a);
				break;
			default:
				a = IFN(rm_get)(d, sz);
				if (sz == 1) idiv8(a); else if (sz == 2) idiv16(a); else idiv32(a);
				break;
			}
			break;
		case 0xfe:
			if (d->reg == 0)
				IFN(rm_put)(d, 1, f_inc(IFN(rm_get)(d, 1), 8));
			else if (d->reg == 1)
				IFN(rm_put)(d, 1, f_dec(IFN(rm_get)(d, 1), 8));
			else
				goto bad;
			break;
		case 0xff:
			switch (d->reg) {
			case 0: IFN(rm_put)(d, sz, f_inc(IFN(rm_get)(d, sz), bits)); break;
			case 1: IFN(rm_put)(d, sz, f_dec(IFN(rm_get)(d, sz), bits)); break;
			case 2: /* the target is read before the push, as on x86 (the
				 * lifter reads it after; the game has no call [esp+n]) */
				t = IFN(rm_get)(d, 4);
				IFN(push32)(nxt);
				pc = t;
				continue;
			case 4:
				pc = IFN(rm_get)(d, 4);
				continue;
			case 6:
				if (sz == 2)
					IFN(push16)(IFN(rm_get)(d, 2));
				else
					IFN(push32)(IFN(rm_get)(d, 4));
				break;
			default:
				goto bad;
			}
			break;

		/* 0f xx */
		case 0x180: case 0x181: case 0x182: case 0x183: case 0x184: case 0x185: case 0x186: case 0x187:
		case 0x188: case 0x189: case 0x18a: case 0x18b: case 0x18c: case 0x18d: case 0x18e: case 0x18f:
			if (cond(op & 15))
				JUMP(d->imm);
			break;
		case 0x190: case 0x191: case 0x192: case 0x193: case 0x194: case 0x195: case 0x196: case 0x197:
		case 0x198: case 0x199: case 0x19a: case 0x19b: case 0x19c: case 0x19d: case 0x19e: case 0x19f:
			IFN(rm_put)(d, 1, cond(op & 15) ? 1u : 0u);
			break;
		case 0x1a3: /* bt r/m, r */
			if (d->modreg)
				cpu.cf = (reg_get(d->rm, sz) >> (reg_get(d->reg, sz) & (uint32_t)(bits - 1))) & 1;
			else {
				int32_t bi = (int32_t)reg_get(d->reg, sz);
				cpu.cf = (MRD32(dec_ea(d) + (uint32_t)((bi >> 3) & ~3)) >> (bi & 31)) & 1;
			}
			break;
		case 0x1ab: case 0x1b3: case 0x1bb: case 0x1ba: {
			uint32_t bit, k;
			if (op == 0x1ba) {
				k = d->reg;
				bit = d->imm & (uint32_t)(bits - 1);
				if (k == 4) {
					cpu.cf = (IFN(rm_get)(d, sz) >> bit) & 1;
					break;
				}
				if (k < 4)
					goto bad;
			} else {
				k = op == 0x1ab ? 5 : op == 0x1b3 ? 6 : 7;
				bit = reg_get(d->reg, sz) & (uint32_t)(bits - 1);
			}
			if (!d->modreg) /* the lifter has bts/btr/btc on registers only */
				goto bad;
			a = reg_get(d->rm, sz);
			cpu.cf = (a >> bit) & 1;
			reg_put(d->rm, sz, k == 5 ? a | (1u << bit) : k == 6 ? a & ~(1u << bit) : a ^ (1u << bit));
			break;
		}
		case 0x1a4: case 0x1a5:
			IFN(rm_put)(d, sz, f_shld(IFN(rm_get)(d, sz), reg_get(d->reg, sz),
						  op == 0x1a4 ? d->imm : cpu.r[ECX] & 0xffu, bits));
			break;
		case 0x1ac: case 0x1ad:
			IFN(rm_put)(d, sz, f_shrd(IFN(rm_get)(d, sz), reg_get(d->reg, sz),
						  op == 0x1ac ? d->imm : cpu.r[ECX] & 0xffu, bits));
			break;
		case 0x1af:
			reg_put(d->reg, sz, f_imul2(reg_get(d->reg, sz), IFN(rm_get)(d, sz), bits));
			break;
		case 0x1b6: reg_put(d->reg, sz, IFN(rm_get)(d, 1)); break;
		case 0x1b7: reg_put(d->reg, sz, IFN(rm_get)(d, 2)); break;
		case 0x1be: reg_put(d->reg, sz, (uint32_t)(int32_t)(int8_t)IFN(rm_get)(d, 1)); break;
		case 0x1bf: reg_put(d->reg, sz, (uint32_t)(int32_t)(int16_t)IFN(rm_get)(d, 2)); break;
		case 0x1c8: case 0x1c9: case 0x1ca: case 0x1cb: case 0x1cc: case 0x1cd: case 0x1ce: case 0x1cf:
			cpu.r[d->reg] = bswap32(cpu.r[d->reg]);
			break;
		default:
			goto bad;
		}
		pc = nxt;
		continue;
next:
		continue;
bad:
		x86_unimplemented(pc);
#undef JUMP
	}
}
