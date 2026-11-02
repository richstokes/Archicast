/* Run ARM code that was never recompiled, one instruction at a time.
 *
 * The recompiled ROM stays the operating system. This only takes over when control
 * reaches RAM (or a ROM path the static scan missed), so a game loaded from a real
 * disc can run on top of it. It shares all machine state with recompiled code and
 * keeps nothing on the native stack across a return to dispatch, so SWIs, aborts
 * and interrupts all take their normal routes. Semantics follow tools/armxlat.py.
 *
 * ARM_INTERP_ALL keeps ROM code in here too. That build exists to check this file
 * against the replay checksums, which were all recorded from recompiled code. */
#include "appunit.h"

#define ROM_BASE 0x3800000u
#define EXIT_SWI (1u << 26)
#define EXIT_UNDEF (2u << 26)
#define EXIT_POLL (3u << 26)

#ifdef ARM_INTERP_ALL
#define LEAVES(pc) 0
#else
#define LEAVES(pc) ((pc) >= ROM_BASE)
#endif

const u8 *mach_code_page(u32 pc);

#ifndef __sh__
unsigned long long arm_insns;
u32 arm_hist[0x80000 / 4]; /* RAM below 512KB, by address */
u32 arm_hist_rom[0x200000 / 4]; /* and the 2MB ROM, for the all-interpreted build */
#define COUNT() (arm_insns++, pc < 0x80000 ? (void)arm_hist[pc >> 2]++ : \
	pc - ROM_BASE < 0x200000u ? (void)arm_hist_rom[(pc - ROM_BASE) >> 2]++ : (void)0)
#elif defined(HW_LOG)
u32 arm_insns; /* read by the heartbeat */
#define COUNT() arm_insns++
#else
#define COUNT()
#endif

static const u8 slots[15] = {
	SLOT(0), SLOT(1), SLOT(2), SLOT(3), SLOT(4), SLOT(5), SLOT(6), SLOT(7),
	SLOT(8), SLOT(9), SLOT(10), SLOT(11), SLOT(12), SLOT(13), SLOT(14),
};
#define REG(n) (C->s[slots[n]])

static inline int passes(u32 cond)
{
	switch (cond) {
	case 0: return F_Z;
	case 1: return !F_Z;
	case 2: return F_C;
	case 3: return !F_C;
	case 4: return F_N;
	case 5: return !F_N;
	case 6: return F_V;
	case 7: return !F_V;
	case 8: return F_C && !F_Z;
	case 9: return !F_C || F_Z;
	case 10: return F_N == F_V;
	case 11: return F_N != F_V;
	case 12: return !F_Z && F_N == F_V;
	case 13: return F_Z || F_N != F_V;
	case 14: return 1;
	}
	return 0;
}

/* Rm or Rs: r15 carries the flags and mode */
static inline u32 reg_psr(int n, u32 pc)
{
	return n == 15 ? R15_FULL((pc) & PC_MASK) : REG(n);
}

/* Rn: r15 is the bare PC */
static inline u32 reg_pc(int n, u32 pc)
{
	return n == 15 ? pc & PC_MASK : REG(n);
}

/* The shifted register operand of data processing and of LDR and STR */
static inline u32 shifter_reg(u32 i, u32 pc, u32 *carry)
{
	int kind = (i >> 5) & 3;
	if (i & 0x10) {
		u32 m = reg_psr((int)(i & 15), pc + 12);
		u32 s = reg_psr((int)((i >> 8) & 15), pc + 12) & 255;
		return shift_c(kind, m, s, carry);
	}
	{
		u32 m = reg_psr((int)(i & 15), pc + 8), n = (i >> 7) & 31;
		switch (kind) {
		case 0:
			if (!n)
				return m;
			*carry = (m >> (32 - n)) & 1;
			return m << n;
		case 1:
			if (!n) {
				*carry = m >> 31;
				return 0;
			}
			*carry = (m >> (n - 1)) & 1;
			return m >> n;
		case 2:
			if (!n)
				n = 32;
			*carry = (m >> (n - 1)) & 1;
			return (u32)((s32)m >> (n > 31 ? 31 : n));
		}
		if (!n) {
			u32 v = (C->fc << 31) | (m >> 1);
			*carry = m & 1;
			return v;
		}
		*carry = (m >> (n - 1)) & 1;
		return (m >> n) | (m << (32 - n));
	}
}

/* Runs until control leaves. Returns the next PC, with EXIT bits saying how to go on. */
static u32 run(u32 pc)
{
	const u8 *page = 0;
	u32 i, fetched = 0;
	int stale = 0;
	for (;;) {
		if (!page || !(pc & PAGE_OFFSET) || pc >= ROM_BASE) {
			page = C->rt[PAGE_INDEX(pc)];
			if (!page || pc >= ROM_BASE)
				page = mach_code_page(pc);
			if (!page)
				return pc; /* dispatch reports it */
		}
		i = *(const u32 *)(page + (pc & 0x7FFCu));
		COUNT();
decode:
		if (i >> 28 != 14 && !passes(i >> 28)) {
			pc += 4;
			continue;
		}
		switch ((i >> 25) & 7) {
		case 0:
			if ((i & 0x90) == 0x90) {
				if ((i & 0x0FC000F0u) == 0x90) {
					u32 r = REG(i & 15) * REG((i >> 8) & 15);
					if (i & (1u << 21))
						r += REG((i >> 12) & 15);
					REG((i >> 16) & 15) = r;
					if (i & (1u << 20))
						C->fn = C->fz = r;
					pc += 4;
					continue;
				}
				return pc | EXIT_UNDEF;
			}
			/* fall through */
		case 1: {
			u32 op = (i >> 21) & 15, s = i & (1u << 20), a, b, r, carry = C->fc;
			int rn = (i >> 16) & 15, rd = (i >> 12) & 15;
			if (i & (1u << 25)) {
				u32 rot = ((i >> 8) & 15) * 2;
				b = i & 0xFF;
				if (rot) {
					b = (b >> rot) | (b << (32 - rot));
					carry = b >> 31;
				}
				a = reg_pc(rn, pc + 8);
			} else {
				b = shifter_reg(i, pc, &carry);
				a = reg_pc(rn, pc + (i & 0x10 ? 12 : 8));
			}
			switch (op) {
			case 0: case 8: r = a & b; break;
			case 1: case 9: r = a ^ b; break;
			case 12: r = a | b; break;
			case 13: r = b; break;
			case 14: r = a & ~b; break;
			case 15: r = ~b; break;
			case 2: case 10:
				r = a - b;
				if (s && rd != 15) {
					C->fc = a >= b;
					C->fv = ((a ^ b) & (a ^ r)) >> 31;
				}
				break;
			case 3:
				r = b - a;
				if (s && rd != 15) {
					C->fc = b >= a;
					C->fv = ((a ^ b) & (b ^ r)) >> 31;
				}
				break;
			case 4: case 11:
				r = a + b;
				if (s && rd != 15) {
					C->fc = r < a;
					C->fv = ((a ^ r) & (b ^ r)) >> 31;
				}
				break;
			case 5: {
				u32 ci = C->fc;
				r = a + b + ci;
				if (s && rd != 15) {
					C->fc = ci ? r <= a : r < a;
					C->fv = ((a ^ r) & (b ^ r)) >> 31;
				}
				break;
			}
			case 6: {
				u32 ci = C->fc;
				r = a - b - !ci;
				if (s && rd != 15) {
					C->fc = ci ? a >= b : a > b;
					C->fv = ((a ^ b) & (a ^ r)) >> 31;
				}
				break;
			}
			default: {
				u32 ci = C->fc;
				r = b - a - !ci;
				if (s && rd != 15) {
					C->fc = ci ? b >= a : b > a;
					C->fv = ((a ^ b) & (b ^ r)) >> 31;
				}
				break;
			}
			}
			if (s && rd != 15) {
				C->fn = C->fz = r;
				if (op < 2 || (op >= 8 && op != 10 && op != 11))
					C->fc = carry;
			}
			if (op >= 8 && op <= 11) {
				if (rd == 15) { /* TEQP and friends */
					psr_write(r);
					pc += 4;
					goto jump;
				}
				pc += 4;
				continue;
			}
			if (rd == 15) {
				if (s)
					psr_write(r);
				pc = r & PC_MASK;
				goto jump;
			}
			REG(rd) = r;
			pc += 4;
			continue;
		}
		case 2:
		case 3: {
			u32 load = i & (1u << 20), wb = i & (1u << 21), byte = i & (1u << 22);
			u32 up = i & (1u << 23), pre = i & (1u << 24), base, ea, offset, addr, v = 0, unused;
			int rn = (i >> 16) & 15, rd = (i >> 12) & 15;
			if (i & (1u << 25)) {
				if (i & 0x10)
					return pc | EXIT_UNDEF;
				offset = shifter_reg(i, pc, &unused);
			} else {
				offset = i & 0xFFF;
			}
			base = reg_pc(rn, pc + 8);
			ea = up ? base + offset : base - offset;
			addr = pre ? ea : base;
			C->cur = pc;
			if (!pre && wb) { /* post-indexed with W: the user mode forms */
				if (load)
					v = byte ? ld8T(addr, pc) : ld32T(addr, pc);
				else if (byte)
					st8T(addr, reg_psr(rd, pc + 12), pc);
				else
					st32T(addr, reg_psr(rd, pc + 12), pc);
			} else if (load) {
				v = byte ? ld8i(addr) : ld32i(addr);
			} else if (byte) {
				st8i(addr, reg_psr(rd, pc + 12));
			} else {
				/* The pipeline already holds the next instruction, and a store over
				 * it takes effect only on the next pass. Elite's decrypter relies
				 * on that, to catch anyone stepping through it. */
				if ((addr & ~3u) == pc + 4 && (pc + 4) & PAGE_OFFSET) {
					fetched = *(const u32 *)(page + ((pc + 4) & 0x7FFCu));
					stale = 1;
				}
				st32i(addr, reg_psr(rd, pc + 12));
			}
			if ((!pre || wb) && rn != 15)
				REG(rn) = ea;
			if (stale) {
				stale = 0;
				pc += 4;
				i = fetched;
				goto decode;
			}
			if (load) {
				if (rd == 15) {
					pc = v & PC_MASK;
					goto jump;
				}
				REG(rd) = v;
			}
			pc += 4;
			continue;
		}
		case 4: {
			u32 load = i & (1u << 20), wb = i & (1u << 21), sbit = i & (1u << 22);
			u32 up = i & (1u << 23), pre = i & (1u << 24), mask = i & 0xFFFF, base, a, wbv, size;
			int rn = (i >> 16) & 15, r, user_bank = sbit && !(load && (mask & 0x8000));
			size = (u32)__builtin_popcount(mask) * 4;
			if (!size)
				size = 4;
			base = reg_pc(rn, pc + 8);
			if (up) {
				a = pre ? base + 4 : base;
				wbv = base + size;
			} else {
				a = pre ? base - size : base - size + 4;
				wbv = base - size;
			}
			a &= ~3u;
			C->cur = pc;
			if (load) {
				for (r = 0; r < 15; r++) {
					if (!(mask >> r & 1))
						continue;
					if (user_bank && r >= 8)
						usr_set(r, ld32i(a));
					else
						REG(r) = ld32i(a);
					a += 4;
				}
				if (mask & 0x8000) {
					u32 npc = ld32i(a);
					if (wb && rn != 15 && !(mask >> rn & 1))
						REG(rn) = wbv;
					if (sbit)
						psr_write(npc);
					pc = npc & PC_MASK;
					goto jump;
				}
				if (wb && rn != 15 && (!(mask >> rn & 1) || (user_bank && rn >= 8)))
					REG(rn) = wbv;
			} else {
				int first = 1;
				for (r = 0; r < 16; r++) {
					u32 v;
					if (!(mask >> r & 1))
						continue;
					if (r == 15)
						v = R15_FULL((pc + 12) & PC_MASK);
					else if (user_bank && r >= 8)
						v = usr_get(r);
					else if (r == rn && !first && wb)
						v = wbv;
					else
						v = REG(r);
					st32i(a, v);
					a += 4;
					first = 0;
				}
				if (wb && rn != 15)
					REG(rn) = wbv;
			}
			pc += 4;
			continue;
		}
		case 5:
			if (i & (1u << 24))
				REG(14) = R15_FULL((pc + 4) & PC_MASK);
			pc = (pc + 8 + (u32)((s32)(i << 8) >> 6)) & 0x3FFFFFFu;
			goto jump;
		case 7:
			if (i & (1u << 24))
				return pc | EXIT_SWI;
			/* fall through */
		default:
			return pc | EXIT_UNDEF;
		}
jump:
		page = 0;
		if (LEAVES(pc))
			return pc;
		if (--C->budget < 0)
			return pc | EXIT_POLL;
	}
}

void arm_interp(u32 pc)
{
	u32 r = run(pc), next = r & 0x3FFFFFFu;
	switch (r >> 26) {
	case 1: TAIL(swi_insn(next));
	case 2: TAIL(undef_insn(next));
	case 3: TAIL(mach_poll(next));
	}
	TAIL(dispatch(next));
}
