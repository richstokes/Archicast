/* Native machine layer: processor state changes, page tables and the drivers
 * behind the register addresses used by the recompiled kernel. */
#ifdef __sh__
#include <malloc.h>
#endif
#include <setjmp.h>
#ifndef __sh__
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "net.h"

#define RAM_SIZE (4u * 1024 * 1024)
#define PAGE_COUNT 128
#define PAGE_BYTES (1u << PAGE_SHIFT)
#define PRIV_USER 0
#define PRIV_OS 1
#define PRIV_SUPER 2

#define IRQA_VBLANK 0x08
#define IRQA_POR 0x10
#define IRQA_T0 0x20
#define IRQA_T1 0x40
#define IRQB_SOUND 0x02
#define IRQB_KTX 0x40
#define IRQB_KRX 0x80

cpu_t cpu_state;

static u8 *ram;
static u8 *rtab[3][2048], *wtab[3][2048];
static int priv, os_mode;
static jmp_buf run_env;
static int cam_logical[PAGE_COUNT];
static int page_ppl[PAGE_COUNT];
static u32 memc_ctrl, vinit, vstart, vend, cinit, sstart, sendn;
static u32 sound_start, sound_end;
static int sound_fresh;
static int page_size;

static struct {
	u8 ctrl, irqa, irqb, fiq, mska, mskb, mskf;
	u32 latch[4], count[4], read[4];
	u32 carry_ticks;
} ioc;

static struct {
	u32 reg[64];
	int hdstart, hdend, vdstart, vdend, cr;
	int cx, cys, cye;
} vidc;

static uint64_t last_us, next_frame_us, flyback_end_us;
static int flyback;
static uint64_t next_input_us, next_sound_us;

#define POLL_BUDGET 256


static void update_tables(void)
{
	C->rt = rtab[priv];
	C->wt = wtab[priv];
}

static void update_priv(void)
{
	int mode = C->psr & 3;
	priv = mode ? PRIV_SUPER : os_mode ? PRIV_OS : PRIV_USER;
	update_tables();
}

static void switch_bank(int from, int to)
{
	int i;
	if (from == to)
		return;
	if (from == MODE_FIQ) {
		for (i = 8; i < 15; i++)
			cpu_state.fiq[i - 8] = R(i);
		for (i = 8; i < 13; i++)
			R(i) = cpu_state.usr[i - 8];
	} else {
		for (i = 8; i < 13; i++)
			cpu_state.usr[i - 8] = R(i);
		if (from == MODE_USR) {
			cpu_state.usr[5] = R(13);
			cpu_state.usr[6] = R(14);
		} else {
			u32 *bank = from == MODE_IRQ ? cpu_state.irq : cpu_state.svc;
			bank[0] = R(13);
			bank[1] = R(14);
		}
	}
	if (to == MODE_FIQ) {
		for (i = 8; i < 15; i++)
			R(i) = cpu_state.fiq[i - 8];
	} else if (to == MODE_USR) {
		R(13) = cpu_state.usr[5];
		R(14) = cpu_state.usr[6];
	} else {
		u32 *bank = to == MODE_IRQ ? cpu_state.irq : cpu_state.svc;
		R(13) = bank[0];
		R(14) = bank[1];
	}
}

u32 R15_FULL(u32 pc)
{
	return pc | (C->fn & 0x80000000u) | (C->fz ? 0 : 0x40000000u) |
	       (C->fc << 29) | (C->fv << 28) | C->psr;
}

static void set_mode_bits(u32 bits)
{
	int from = C->psr & 3;
	C->psr = bits;
	switch_bank(from, bits & 3);
	update_priv();
}

static void set_flags(u32 v)
{
	C->fn = v;
	C->fz = v & 0x40000000u ? 0 : 1;
	C->fc = (v >> 29) & 1;
	C->fv = (v >> 28) & 1;
}

void psr_write(u32 v)
{
	set_flags(v);
	if (C->psr & 3)
		set_mode_bits(v & (PSR_I | PSR_F | 3));
	C->budget = 0; /* interrupts may just have been enabled */
}

u32 usr_get(int reg)
{
	int mode = C->psr & 3;
	if (mode == MODE_USR || (reg < 13 && mode != MODE_FIQ))
		return R(reg);
	return cpu_state.usr[reg - 8];
}

void usr_set(int reg, u32 value)
{
	int mode = C->psr & 3;
	if (mode == MODE_USR || (reg < 13 && mode != MODE_FIQ))
		R(reg) = value;
	else
		cpu_state.usr[reg - 8] = value;
}

static void take_exception(u32 vector, int mode, u32 link, u32 extra)
{
	u32 saved = R15_FULL(link & PC_MASK);
	set_mode_bits((C->psr & PSR_F) | PSR_I | extra | mode);
	R(14) = saved;
	(void)vector;
}

static struct { u32 pc, num, r0, r1, r2, mode; } swi_ring[64];
static u32 swi_pos;

void swi_insn(u32 pc)
{
	u8 *page = C->rt[PAGE_INDEX(pc)];
#ifdef DEBUG_SWI
	if (page && (*(u32 *)(page + (pc & 0x7FFCu)) & 0xFFFFFF) == DEBUG_SWI)
		plat_fatal("hit debug SWI at %07X\n", pc);
#endif
	if (page) {
		swi_ring[swi_pos & 63].pc = pc;
		swi_ring[swi_pos & 63].num = *(u32 *)(page + (pc & 0x7FFCu)) & 0xFFFFFF;
		swi_ring[swi_pos & 63].r0 = R(0);
		swi_ring[swi_pos & 63].r1 = R(1);
		swi_ring[swi_pos & 63].r2 = R(2);
		swi_ring[swi_pos & 63].mode = C->psr & 3;
		swi_pos++;
	}
	if (page) {
		u32 num = *(u32 *)(page + (pc & 0x7FFCu)) & 0xFDFFFFu; /* with or without X */
		if ((num & ~NET_SWI_MASK) == NET_SWI_BASE) {
			C->cur = pc;
			net_swi(num & NET_SWI_MASK);
			C->fv = 0;
			TAIL(dispatch(pc + 4));
		}
	}
	take_exception(8, MODE_SVC, pc + 4, 0);
	TAIL(dispatch(8));
}

static u32 undef_pc;

void undef_insn(u32 pc)
{
	undef_pc = pc;
	take_exception(4, MODE_SVC, pc + 4, 0);
	TAIL(dispatch(4));
}

static u32 abort_addr, abort_pc, abort_ret;

static void data_abort(u32 addr)
{
	abort_addr = addr;
	abort_pc = C->cur;
	take_exception(0x10, MODE_SVC, C->cur + 8, 0);
	longjmp(run_env, 0x10 + 1);
}

static void update_irq(void)
{
	C->budget = 0;
}

/* --- demand-paged ROM ---
 * ROM reads go through the page tables, so a page that is not cached simply
 * misses into the slow path and loads. Page 0 stays pinned: it holds the reset
 * code and is aliased at address 0. Slots are evicted by clock. */
#define ROM_PAGES 64
#define COLD_SLOTS 8
/* Bundled app files follow the ROM in rom.bin and appear at 0x3C00000 */
#define PACK_INDEX 0x780
#define PACK_END 0x7C0
/* Pages never touched in normal use. Everything else is hot, because the kernel
 * walks module headers and ResourceFS scans its files on every lookup. */
static const u32 cold_pages[2] = {
	(1u << 20) | (1u << 30),
	(1u << (48 - 32)) | (1u << (49 - 32)) | (1u << (50 - 32)) | (1u << (51 - 32)) |
	(1u << (55 - 32)) | (1u << (56 - 32)) | (1u << (57 - 32)) | (1u << (58 - 32)) |
	(1u << (60 - 32)) | (1u << (61 - 32)),
};
#define ROM_IS_COLD(p) ((cold_pages[(p) >> 5] >> ((p) & 31)) & 1)
static u8 *rom_cache;
static short slot_page[COLD_SLOTS];
static int clock_hand, hot_count;
static u32 rom_misses;

static void rom_map(int page, u8 *mem)
{
	int i, p;
	if (page >= ROM_PAGES) {
		for (p = 0; p < 3; p++)
			rtab[p][PACK_INDEX + page - ROM_PAGES] = mem;
		return;
	}
	/* High ROM is mirrored every 2MB; writes there reach the CAM instead */
	for (i = 0x700 + page; i < 0x800; i += ROM_PAGES) {
		if (i >= PACK_INDEX && i < PACK_END)
			continue;
		for (p = 0; p < 3; p++)
			rtab[p][i] = mem;
	}
}

/* Page for a missing high address, or -1 for the unused end of the pack */
static int rom_page_at(u32 a)
{
	int index = PAGE_INDEX(a);
	if (index < PACK_INDEX || index >= PACK_END)
		return index & (ROM_PAGES - 1);
	return (u32)(index - PACK_INDEX) < rom_ext_pages ? index - PACK_INDEX + ROM_PAGES : -1;
}

static void rom_page_in(int page)
{
	int slot = clock_hand;
	u8 *mem = rom_cache + (size_t)(hot_count + slot) * PAGE_BYTES;
	clock_hand = (clock_hand + 1) % COLD_SLOTS;
	if (slot_page[slot] >= 0)
		rom_map(slot_page[slot], 0);
	plat_rom_read((u32)page, mem);
	slot_page[slot] = (short)page;
	rom_map(page, mem);
	rom_misses++;
}

static void rom_load_hot(void)
{
	int page, i;
	for (page = 0; page < ROM_PAGES; page++)
		hot_count += !ROM_IS_COLD(page);
	/* 32-byte aligned so the disc driver can DMA straight into it */
#ifdef __sh__
	rom_cache = memalign(32, (size_t)(hot_count + COLD_SLOTS) * PAGE_BYTES);
#else
	rom_cache = malloc((size_t)(hot_count + COLD_SLOTS) * PAGE_BYTES);
#endif
	if (!rom_cache)
		plat_fatal("cannot allocate ROM cache\n");
	for (i = 0; i < COLD_SLOTS; i++)
		slot_page[i] = -1;
	for (page = 0, i = 0; page < ROM_PAGES; page++) {
		if (!ROM_IS_COLD(page)) {
			u8 *mem = rom_cache + (size_t)i++ * PAGE_BYTES;
			plat_rom_read((u32)page, mem);
#ifdef ARM_INTERP_ALL
			/* The recompiler's ADFS IDE patch (tools/armxlat.py ROM_PATCHES), so
			 * the all-interpreted build sees the hard discs too */
			if (page == 0xE1470 / PAGE_BYTES) {
				u32 *w = (u32 *)(mem + (0xE1470 & (PAGE_BYTES - 1)));
				if (*w == 0x1A000003u)
					*w = 0xE1A00000u;
			}
#endif
			rom_map(page, mem);
		}
	}
}

u32 mach_rom_misses(void)
{
	return rom_misses;
}

/* --- page tables --- */

static void map_logical(int page_index, int logical, int ppl)
{
	u8 *mem = ram + (size_t)page_index * PAGE_BYTES;
	int state = ppl + 1; /* 1 all, 2 r/o, 3 os r/o, 4 super only */
	static const u8 readable[3][5] = {{0,1,1,0,0},{0,1,1,1,0},{0,1,1,1,1}};
	static const u8 writable[3][5] = {{0,1,0,0,0},{0,1,1,0,0},{0,1,1,1,1}};
	int p;
	for (p = 0; p < 3; p++) {
		rtab[p][logical] = readable[p][state] ? mem : 0;
		wtab[p][logical] = writable[p][state] ? mem : 0;
	}
}

static void unmap_logical(int logical)
{
	int p;
	for (p = 0; p < 3; p++)
		rtab[p][logical] = wtab[p][logical] = 0;
}

static void reset_maps(void)
{
	int i, p;
	memset(rtab, 0, sizeof(rtab));
	memset(wtab, 0, sizeof(wtab));
	for (i = 0; i < PAGE_COUNT; i++)
		cam_logical[i] = -1;
	/* Physical RAM window, visible to supervisor code only */
	for (i = 0x400; i < 0x600; i++) {
		rtab[PRIV_SUPER][i] = ram + (size_t)(i & (PAGE_COUNT - 1)) * PAGE_BYTES;
		wtab[PRIV_SUPER][i] = rtab[PRIV_SUPER][i];
	}
	for (p = 0; p < 3; p++) {
	}
}

static void write_cam(u32 a)
{
	int page, logical, ppl;
	if (page_size != 3)
		plat_fatal("unsupported MEMC page size %d\n", page_size);
	page = ((a >> 3) & 0xF) | ((a & 1) << 4) | ((a & 2) << 5) | ((a & 4) << 3);
	if (a & 0x80)
		page |= 0x80;
	if (a & 0x1000)
		page |= 0x100;
	if (page >= PAGE_COUNT)
		return;
	ppl = (a >> 8) & 3;
	logical = ((a >> 15) & 0xFF) | ((a >> 2) & 0x300);
	if (cam_logical[page] >= 0 &&
	    rtab[PRIV_SUPER][cam_logical[page]] == ram + (size_t)page * PAGE_BYTES)
		unmap_logical(cam_logical[page]);
	cam_logical[page] = logical;
	page_ppl[page] = ppl;
	map_logical(page, logical, ppl);
}

static void write_memc(u32 a)
{
	u32 value = ((a >> 2) & 0x7FFF) << 4;
	switch ((a >> 17) & 7) {
	case 0: vinit = value; break;
	case 1: vstart = value; break;
	case 2: vend = value; break;
	case 3: cinit = value; break;
	case 4:
		sstart = value;
		sound_fresh = 1;
		ioc.irqb &= ~IRQB_SOUND;
		update_irq();
		break;
	case 5: sendn = value; break;
	case 6:
		sound_start = sstart;
		sound_end = sendn;
		sound_fresh = 0;
		ioc.irqb |= IRQB_SOUND;
		update_irq();
		break;
	case 7:
		memc_ctrl = a & 0x3FFC;
		os_mode = !!(a & 0x1000);
		page_size = (a >> 2) & 3;
		update_priv();
		break;
	}
}

/* --- IOC --- */

static int irq_pending(void)
{
	return ((ioc.irqa | 0x80) & ioc.mska) || (ioc.irqb & ioc.mskb);
}

void mach_irqb(u32 bits)
{
	ioc.irqb |= (u8)bits;
	update_irq();
}

void mach_irqb_clear(u32 bits)
{
	ioc.irqb &= (u8)~bits;
}

void mach_fiq(u32 bits)
{
	ioc.fiq |= (u8)bits;
	update_irq();
}

void mach_fiq_clear(u32 bits)
{
	ioc.fiq &= (u8)~bits;
}

static int fiq_pending(void)
{
	return ((ioc.fiq | 0x80) & ioc.mskf) != 0;
}

/* Keyboard controller protocol, spoken by the kernel's keyboard driver. */
enum { KB_RESET, KB_IDLE, KB_DAT1, KB_DAT2, KB_SINGLE };
static struct {
	int state, key_enabled, mouse_enabled;
	u8 data[2];
	uint64_t rx_at, tx_at;
	int mouse_buttons;
	int mouse_dx, mouse_dy;
	int mouse_seen;
	struct { u8 row, col, down; } queue[32];
	int head, tail;
	/* Keys the kernel has seen go down. A reset keyboard reports them again, which
	 * is how the kernel tells Ctrl+Break (a hard reset) from Break */
	u8 held[16];
	int resend; /* next key code to check, or -1 */
} kb;

static void kb_arm_rx(void)
{
	kb.rx_at = plat_time_us() + 1000;
}

static void kb_single(u8 v)
{
	kb.data[0] = v;
	kb.state = KB_SINGLE;
	kb_arm_rx();
}

static void kb_double(u8 a, u8 b)
{
	kb.data[0] = a;
	kb.data[1] = b;
	kb.state = KB_DAT1;
	kb_arm_rx();
}

static void kb_write(u8 v)
{
	kb.tx_at = plat_time_us() + 1000;
	switch (kb.state) {
	case KB_RESET:
		if (v == 0xFF) {
			kb_single(0xFF);
			kb.key_enabled = kb.mouse_enabled = 0;
			kb.state = KB_RESET;
			kb.resend = 0;
		} else if (v == 0xFE) {
			kb_single(0xFE);
			kb.key_enabled = kb.mouse_enabled = 0;
			kb.state = KB_RESET;
		} else if (v == 0xFD) {
			kb_single(0xFD);
			kb.state = KB_IDLE;
		}
		break;
	case KB_IDLE:
		if (v == 0x20) {
			kb_single(0x81);
		} else if ((v & 0xFC) == 0x30) {
			kb.mouse_enabled |= !!(v & 2);
			kb.key_enabled |= !!(v & 1);
		} else if (v == 0xFF) {
			kb_single(0xFF);
			kb.state = KB_RESET;
			kb.resend = 0;
		}
		break;
	case KB_DAT1:
		if (v == 0x3F) {
			kb.state = KB_DAT2;
			kb_arm_rx();
		} else if (v == 0xFF) {
			kb_single(0xFF);
			kb.state = KB_RESET;
			kb.resend = 0;
		}
		break;
	default:
		if ((v & 0xFC) == 0x30) {
			kb.mouse_enabled |= !!(v & 2);
			kb.key_enabled |= !!(v & 1);
			kb.state = KB_IDLE;
		} else if (v == 0xFF) {
			kb_single(0xFF);
			kb.state = KB_RESET;
			kb.resend = 0;
		}
		break;
	}
}

static u8 kb_read(void)
{
	switch (kb.state) {
	case KB_SINGLE:
		kb.state = KB_IDLE;
		return kb.data[0];
	case KB_DAT2:
		return kb.data[1];
	default:
		return kb.data[0];
	}
}

void mach_key(int row, int col, int down)
{
	int next = (kb.tail + 1) % 32;
	if (next == kb.head)
		return;
	kb.queue[kb.tail].row = (u8)row;
	kb.queue[kb.tail].col = (u8)col;
	kb.queue[kb.tail].down = (u8)down;
	kb.tail = next;
}

static const u8 *app_pad, *pad_map;

/* Key codes for the controller while an app that wants them runs, or null */
const u8 *mach_pad_map(void)
{
	return pad_map;
}

void mach_mouse(int dx, int dy, int buttons)
{
	int changed = buttons ^ kb.mouse_buttons, b;
	kb.mouse_dx += dx;
	kb.mouse_dy += dy;
	/* Select, Menu, Adjust are keys at row 7 */
	for (b = 0; b < 3; b++)
		if (changed & (1 << b))
			mach_key(7, b, !!(buttons & (1 << b)));
	kb.mouse_buttons = buttons;
}

static void kb_poll(void)
{
	if (kb.state != KB_IDLE || !kb.key_enabled)
		return;
	while (kb.resend >= 0 && kb.resend < 128) {
		int code = kb.resend++;
		if (kb.held[code >> 3] & (1 << (code & 7))) {
			kb_double((u8)(0xC0 | code >> 4), (u8)(0xC0 | (code & 15)));
			return;
		}
	}
	kb.resend = -1;
	if (kb.head != kb.tail) {
		int code = kb.queue[kb.head].row << 4 | kb.queue[kb.head].col;
		u8 base = kb.queue[kb.head].down ? 0xC0 : 0xD0;
		if (kb.queue[kb.head].down)
			kb.held[code >> 3] |= (u8)(1 << (code & 7));
		else
			kb.held[code >> 3] &= (u8)~(1 << (code & 7));
		kb_double(base | kb.queue[kb.head].row, base | kb.queue[kb.head].col);
		kb.head = (kb.head + 1) % 32;
		return;
	}
	if (kb.mouse_enabled && (kb.mouse_dx || kb.mouse_dy)) {
		int dx = kb.mouse_dx, dy = kb.mouse_dy;
		if (dx > 63) dx = 63;
		if (dx < -63) dx = -63;
		if (dy > 63) dy = 63;
		if (dy < -63) dy = -63;
		kb.mouse_dx -= dx;
		kb.mouse_dy -= dy;
		kb_double(dx & 0x7F, -dy & 0x7F);
	}
}

/* CMOS and RTC on the I2C lines. The chip is a PCF8583: registers 0x00-0x0F
 * are its clock and the rest the battery-backed RAM RISC OS keeps its
 * configuration in. The kernel reads the clock once per reset (register 5 for
 * the two year bits, then 1-6 in one go) and counts on from there off timer 0,
 * so the registers are worked out from the platform's clock as each read
 * starts: at boot that read comes ~18s in, and Ctrl+Break reads them again.
 * The full year is the kernel's own, in CMOS bytes &80-&81 (physical
 * 0xC0-0xC1: year in century, then century, in binary) and set at init.
 * Setting the time (OS_Word 15, from !Alarm or BASIC's TIME$) moves the clock
 * here and on the platform, so it survives power off. */
#ifdef I2C_TRACE
#define I2CLOG(...) plat_log(__VA_ARGS__)
#else
#define I2CLOG(...) ((void)0)
#endif
enum { I2C_IDLE, I2C_RECEIVE, I2C_TRANSMIT, I2C_ACK, I2C_TRANSACK };
enum { CM_IDLE, CM_ADDR, CM_DATA, CM_SEND, CM_NONE };
enum { RTC_REGS = 1, RTC_YEAR = 2 }; /* what a write burst touched */
static struct {
	u8 ram[256];
	int state, addr, rw, is_tx;
	int i2c, last, pos, clock, line;
	u8 byte;
	int64_t rtc_base; /* Unix seconds at rtc_base_us */
	uint64_t rtc_base_us;
	int written;
} cm;

static u8 bcd(int n)
{
	return (u8)(((n / 10) << 4) | (n % 10));
}

static int unbcd(u8 v)
{
	return (v >> 4) * 10 + (v & 15);
}

/* Days since 1970-01-01 to and from a civil date (Howard Hinnant's algorithms) */
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
	int64_t era;
	unsigned doe, yoe, doy, mp;
	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = (int)(yoe + era * 400 + (*m <= 2));
}

static int64_t days_from_civil(int y, int m, int d)
{
	int64_t era;
	unsigned yoe, doy, doe;
	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (unsigned)(m > 2 ? m - 3 : m + 9) + 2) / 5 + (unsigned)d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

/* Fill the clock registers in for now, and return the year they can't hold */
static int rtc_refresh(void)
{
	uint64_t us = last_us - cm.rtc_base_us;
	int64_t secs = cm.rtc_base + (int64_t)(us / 1000000), days = secs / 86400;
	int rem, y, m, d;
	if (secs < 0 && secs % 86400)
		days--;
	rem = (int)(secs - days * 86400);
	civil_from_days(days, &y, &m, &d);
	cm.ram[1] = bcd((int)(us / 10000 % 100));
	cm.ram[2] = bcd(rem % 60);
	cm.ram[3] = bcd(rem / 60 % 60);
	cm.ram[4] = bcd(rem / 3600); /* bit 7 clear: 24 hour clock */
	cm.ram[5] = (u8)(((y & 3) << 6) | bcd(d));
	cm.ram[6] = (u8)(((days % 7 + 11) % 7 << 5) | bcd(m)); /* weekday 0 is Sunday */
	return y;
}

static void cmos_set_year(int year)
{
	u8 lo = (u8)(year % 100), hi = (u8)(year / 100);
	cm.ram[0x3F] = (u8)(cm.ram[0x3F] + lo - cm.ram[0xC0] + hi - cm.ram[0xC1]);
	cm.ram[0xC0] = lo;
	cm.ram[0xC1] = hi;
}

/* RISC OS wrote the time, the year or both: the clock starts again from them.
 * The platform's clock is only moved by a time, as the kernel also rewrites
 * the year on its own, when the chip's two bits have moved past it. */
static void rtc_written(void)
{
	int64_t secs;
	if (!(cm.written & RTC_REGS))
		rtc_refresh();
	secs = days_from_civil(cm.ram[0xC1] * 100 + cm.ram[0xC0], unbcd(cm.ram[6] & 0x1F), unbcd(cm.ram[5] & 0x3F)) * 86400
		+ unbcd(cm.ram[4] & 0x3F) * 3600 + unbcd(cm.ram[3] & 0x7F) * 60 + unbcd(cm.ram[2] & 0x7F);
	cm.rtc_base = secs;
	cm.rtc_base_us = last_us;
	I2CLOG("i2c   clock set to %lld%s\n", (long long)secs, cm.written & RTC_REGS ? "" : " (year only)");
	if (cm.written & RTC_REGS)
		plat_rtc_set(secs);
	cm.written = 0;
}

static void cmos_write_byte(u8 v)
{
	switch (cm.state) {
	case CM_IDLE:
		cm.rw = v & 1;
		if ((v & 0xFE) == 0xA0) {
			I2CLOG("i2c %.3f start %s at %02X\n", last_us / 1e6, cm.rw ? "read" : "write", cm.addr & 0xFF);
			if (cm.rw) {
				cm.state = CM_SEND;
				cm.is_tx = 1;
				if ((cm.addr & 0xFF) < 8)
					rtc_refresh();
				cm.byte = cm.ram[cm.addr++ & 0xFF];
			} else {
				cm.state = CM_ADDR;
				cm.is_tx = 0;
			}
		} else {
			cm.state = CM_NONE;
			cm.byte = 0xFF;
		}
		break;
	case CM_ADDR:
		cm.addr = v;
		cm.state = cm.rw ? CM_SEND : CM_DATA;
		break;
	case CM_DATA: {
		int a = cm.addr++ & 0xFF;
		I2CLOG("i2c   wr [%02X]=%02X (was %02X)\n", a, v, cm.ram[a]);
		if (a >= 1 && a <= 6) {
			if (!(cm.written & RTC_REGS))
				rtc_refresh(); /* so a date alone keeps the time of day, and the other way round */
			cm.written |= RTC_REGS;
		} else if (a == 0xC0 || a == 0xC1) {
			cm.written |= RTC_YEAR;
		}
		cm.ram[a] = v;
		break;
	}
	}
}

static void i2c_change(int nclock, int ndata)
{
	switch (cm.i2c) {
	case I2C_IDLE:
		if (cm.clock && nclock && cm.last && !ndata) {
			cm.i2c = I2C_RECEIVE;
			cm.pos = 0;
		}
		break;
	case I2C_RECEIVE:
		if (!cm.clock && nclock) {
			cm.byte = (u8)((cm.byte << 1) | !!ndata);
			if (++cm.pos == 8) {
				cmos_write_byte(cm.byte);
				cm.i2c = I2C_ACK;
			}
		} else if (cm.clock && nclock && ndata && !cm.last) {
			cm.i2c = I2C_IDLE;
			cm.state = CM_IDLE;
			cm.is_tx = 0;
			if (cm.written)
				rtc_written();
		} else if (cm.clock && nclock && !ndata && cm.last) {
			cm.pos = 0;
			cm.state = CM_IDLE;
		}
		break;
	case I2C_ACK:
		if (!cm.clock && nclock) {
			ndata = 0;
			cm.pos = 0;
			cm.i2c = cm.is_tx ? I2C_TRANSMIT : I2C_RECEIVE;
		}
		break;
	case I2C_TRANSACK:
		if (!cm.clock && nclock) {
			if (ndata) {
				cm.i2c = I2C_IDLE;
				cm.state = CM_IDLE;
				cm.is_tx = 0;
			} else {
				cm.i2c = I2C_TRANSMIT;
				cm.byte = cm.ram[cm.addr++ & 0xFF];
				cm.pos = 0;
			}
		}
		break;
	case I2C_TRANSMIT:
		if (!cm.clock && nclock) {
			cm.line = ndata = cm.byte & 0x80;
			cm.byte = (u8)(cm.byte << 1);
			if (++cm.pos == 8)
				cm.i2c = I2C_TRANSACK;
			cm.clock = nclock;
			return;
		}
		break;
	}
	if (!cm.clock && nclock)
		cm.line = ndata;
	cm.last = ndata;
	cm.clock = nclock;
}

static void timer_load(int n)
{
	ioc.count[n] = ioc.latch[n] ? ioc.latch[n] : 0x10000;
}

static void ioc_write(u32 a, u32 v)
{
	v &= 0xFF;
	switch (a & 0x7C) {
	case 0x00:
		i2c_change(v & 2, v & 1);
		ioc.ctrl = v & 0xFC;
		break;
	case 0x04:
		ioc.irqb &= ~IRQB_KTX;
		kb_write((u8)v);
		break;
	case 0x14: ioc.irqa &= ~v; break;
	case 0x18: ioc.mska = (u8)v; break;
	case 0x28: ioc.mskb = (u8)v; break;
	case 0x38: ioc.mskf = (u8)v; break;
	case 0x40: case 0x50: case 0x60: case 0x70: {
		int n = (a >> 4) & 3;
		ioc.latch[n] = (ioc.latch[n] & 0xFF00) | v;
		break;
	}
	case 0x44: case 0x54: case 0x64: case 0x74: {
		int n = (a >> 4) & 3;
		ioc.latch[n] = (ioc.latch[n] & 0xFF) | (v << 8);
		break;
	}
	case 0x48: case 0x58: case 0x68: case 0x78:
		timer_load((a >> 4) & 3);
		break;
	case 0x4C: case 0x5C: case 0x6C: case 0x7C: {
		int n = (a >> 4) & 3;
		ioc.read[n] = ioc.count[n] & 0xFFFF;
		break;
	}
	}
	update_irq();
}

static u32 ioc_read(u32 a)
{
	u32 v;
	switch (a & 0x7C) {
	case 0x00:
		return (cm.clock ? 2 : 0) | (cm.line ? 1 : 0) | (flyback ? 0x80 : 0) |
		       (fdc_ready() ? 4 : 0) | 0x40;
	case 0x04:
		ioc.irqb &= ~IRQB_KRX;
		v = kb_read();
		update_irq();
		return v;
	case 0x10: return ioc.irqa | 0x80;
	case 0x14: return (ioc.irqa | 0x80) & ioc.mska;
	case 0x18: return ioc.mska;
	case 0x20: return ioc.irqb;
	case 0x24: return ioc.irqb & ioc.mskb;
	case 0x28: return ioc.mskb;
	case 0x30: return ioc.fiq;
	case 0x34: return ioc.fiq & ioc.mskf;
	case 0x38: return ioc.mskf;
	case 0x40: case 0x50: case 0x60: case 0x70:
		return ioc.read[(a >> 4) & 3] & 0xFF;
	case 0x44: case 0x54: case 0x64: case 0x74:
		return ioc.read[(a >> 4) & 3] >> 8;
	}
	return 0;
}

/* Games change the palette part way down the frame, most often to give a status
 * panel its own colours. The presenter gets the palette from above the first such
 * change and the line it happened on. */
static u32 pal_top[20], frame_start_us;
static int split_line = -1;
#ifdef ARM_INTERP
u32 vidc_pal_writes, vidc_when[8];
#endif

static int display_line(void)
{
	/* The frame interrupt comes at the end of the display, then the lines of flyback */
	int since = (int)((u32)last_us - frame_start_us);
	int blank = 312 - (vidc.vdend - vidc.vdstart);
	return since / 64 - blank;
}

static void vidc_write(u32 v)
{
	u32 reg = v >> 26, byte = v >> 24, field = (v & 0xFFFFFF) >> 14;
	if (reg <= 16 && split_line < 0 && vidc.reg[reg] != (v & 0xFFFFFF)) {
		int line = display_line();
		if (line > 0 && line < vidc.vdend - vidc.vdstart) {
			memcpy(pal_top, vidc.reg, sizeof(pal_top));
			split_line = line;
		}
	}
#ifdef ARM_INTERP
	if (reg < 16) {
		vidc_pal_writes++;
		vidc_when[((u32)last_us - frame_start_us) / 2501 & 7]++;
	}
#endif
	vidc.reg[reg] = v & 0xFFFFFF;
	switch (byte) {
	case 0x8C: vidc.hdstart = (int)field; break;
	case 0x90: vidc.hdend = (int)field; break;
	case 0xAC: vidc.vdstart = (int)field + 1; break;
	case 0xB0: vidc.vdend = (int)field + 1; break;
	case 0x98: vidc.cx = (int)((v & 0xFFE000) >> 13) + 6; break;
	case 0xB8: vidc.cys = (int)field; break;
	case 0xBC: vidc.cye = (int)field; break;
	case 0xE0: vidc.cr = (int)(v & 0xC1FF); break;
	}
}

/* --- bus --- */

static u32 io_read(u32 a, int word)
{
	if (priv != PRIV_SUPER)
		data_abort(a);
	if (ide_owns(a))
		return ide_read(a, word);
	switch ((a >> 16) & 7) {
	case 0:
		if (a & (1 << 21))
			return ioc_read(a);
		break;
	case 1:
		return fdc_read(a);
	case 5:
		if ((a & 0xFFFC) == 0x50)
			return 0;
		break;
	}
	return word ? 0xFFFFFFFFu : 0xFF;
}

static void io_write(u32 a, u32 v, int word)
{
	if (priv != PRIV_SUPER)
		data_abort(a);
	if (ide_owns(a)) {
		ide_write(a, v, word);
		return;
	}
	switch ((a >> 16) & 7) {
	case 0:
		if (a & (1 << 21))
			ioc_write(a, v);
		break;
	case 1:
		fdc_write(a, v & 0xFF);
		break;
	case 5:
		if ((a & 0xFFFC) == 0x18)
			fdc_latch_b(v & 0xFF);
		else if ((a & 0xFFFC) == 0x40)
			fdc_latch_a(v & 0xFF);
		break;
	}
}

static u32 ld32_slow(u32 addr)
{
	u32 a = addr & 0x3FFFFFFu, top = a >> 16;
	u8 *page;
	u32 v;
	if (addr >> 26)
		data_abort(addr);
	if (top >= 0x380 && !C->rt[PAGE_INDEX(a)]) {
		if (rom_page_at(a) < 0)
			return 0;
		rom_page_in(rom_page_at(a));
	}
	page = C->rt[PAGE_INDEX(a)];
	if (page) {
		u32 s = (a & 3) * 8;
		v = *(u32 *)(page + (a & 0x7FFCu));
		return s ? (v >> s) | (v << (32 - s)) : v;
	}
	if (top >= 0x300 && top < 0x340)
		return io_read(a, 1);
	if (top >= 0x340 && top < 0x380)
		return priv == PRIV_SUPER ? 0xFFFFFFFFu : (data_abort(a), 0);
	if (top >= 0x3F0)
		return 0xFFFFFFFFu;
	data_abort(a);
	return 0;
}

static u32 ld8_slow(u32 addr)
{
	u32 a = addr & 0x3FFFFFFu, top = a >> 16;
	if (addr >> 26)
		data_abort(addr);
	if (top >= 0x380) {
		if (rom_page_at(a) < 0)
			return 0;
		rom_page_in(rom_page_at(a));
		return ld8(addr);
	}
	if (top >= 0x300 && top < 0x340)
		return io_read(a, 0) & 0xFF;
	if (top >= 0x340 && top < 0x380)
		return 0xFF;
	if (top >= 0x3F0)
		return 0xFF;
	data_abort(a);
	return 0;
}

static void st32_slow(u32 addr, u32 v)
{
	abort_ret = (u32)(uintptr_t)__builtin_return_address(0);
	u32 a = addr & 0x3FFFFFFu, top = a >> 16;
	if (addr >> 26)
		data_abort(addr);
	if (top >= 0x300 && priv != PRIV_SUPER)
		data_abort(a);
	if (top >= 0x300 && top < 0x340) {
		io_write(a, v >> 16, 1);
	} else if (top >= 0x340 && top < 0x360) {
		vidc_write(v);
	} else if (top >= 0x360 && top < 0x380) {
		write_memc(a);
	} else if (top >= 0x380) {
		write_cam(a);
	} else {
		data_abort(a);
	}
}

static void st8_slow(u32 addr, u32 v)
{
	abort_ret = (u32)(uintptr_t)__builtin_return_address(0);
	u32 a = addr & 0x3FFFFFFu, top = a >> 16;
	if (addr >> 26)
		data_abort(addr);
	if (top >= 0x300 && top < 0x340) {
		io_write(a, v & 0xFF, 0);
		return;
	}
	st32_slow(addr, v * 0x01010101u);
}

u32 ld32(u32 a)
{
	u8 *page = C->rt[PAGE_INDEX(a)];
	if (LIKELY(page && !(a & 0xFC000003u)))
		return *(u32 *)(page + (a & PAGE_OFFSET));
	abort_ret = (u32)(uintptr_t)__builtin_return_address(0);
	return ld32_slow(a);
}

u32 ld8(u32 a)
{
	u8 *page = C->rt[PAGE_INDEX(a)];
	if (LIKELY(page && !(a & 0xFC000000u)))
		return page[a & PAGE_OFFSET];
	abort_ret = (u32)(uintptr_t)__builtin_return_address(0);
	return ld8_slow(a);
}

void st32(u32 a, u32 v)
{
	u8 *page = C->wt[PAGE_INDEX(a)];
	if (LIKELY(page && !(a & 0xFC000000u)))
		*(u32 *)(page + (a & 0x7FFCu)) = v;
	else
		st32_slow(a, v);
}

void st8(u32 a, u32 v)
{
	u8 *page = C->wt[PAGE_INDEX(a)];
	if (LIKELY(page && !(a & 0xFC000000u)))
		page[a & PAGE_OFFSET] = (u8)v;
	else
		st8_slow(a, v);
}

static const u8 slot_of[15] = {
	SLOT(0), SLOT(1), SLOT(2), SLOT(3), SLOT(4), SLOT(5), SLOT(6), SLOT(7),
	SLOT(8), SLOT(9), SLOT(10), SLOT(11), SLOT(12), SLOT(13), SLOT(14),
};

u32 ldm_regs(u32 a, u32 mask)
{
	int r;
	for (r = 0; mask; r++, mask >>= 1)
		if (mask & 1) {
			C->s[slot_of[r]] = ld32(a);
			a += 4;
		}
	return a;
}

void stm_regs(u32 a, u32 mask)
{
	int r;
	for (r = 0; mask; r++, mask >>= 1)
		if (mask & 1) {
			st32(a, C->s[slot_of[r]]);
			a += 4;
		}
}

static int enter_user(u32 pc)
{
	int saved = priv;
	C->cur = pc;
	if (C->psr & 3) {
		priv = os_mode ? PRIV_OS : PRIV_USER;
		update_tables();
	}
	return saved;
}

static void leave_user(int saved)
{
	priv = saved;
	update_tables();
}

u32 ld32T(u32 a, u32 pc) { int s = enter_user(pc); u32 v = ld32(a); leave_user(s); return v; }
u32 ld8T(u32 a, u32 pc) { int s = enter_user(pc); u32 v = ld8(a); leave_user(s); return v; }
void st32T(u32 a, u32 v, u32 pc) { int s = enter_user(pc); st32(a, v); leave_user(s); }
void st8T(u32 a, u32 v, u32 pc) { int s = enter_user(pc); st8(a, v); leave_user(s); }

/* --- control flow --- */

static int ov_unit = -1;
static const ov_header_t *ov_header;
static u32 ov_loads;
static u8 app_running[8];
/* Games tied to the display would run far too fast on this CPU, so while one has
 * been seen in the last frame it gets its own vertical sync period. Timers and the
 * OS clock are unaffected. */
static u32 app_frame_us;

static block_fn table_find(const block_t *table, u32 mask, u32 key);
#ifdef ARM_INTERP
void arm_interp(u32 pc);
#endif

u32 mach_overlay_loads(void)
{
	return ov_loads;
}

static void overlay_use(int unit, u32 pc)
{
	if (ov_unit == unit)
		return;
	ov_unit = unit;
	plat_log("overlay %d for pc=%07X\n", unit, pc);
	ov_header = plat_overlay_load(unit);
	ov_loads++;
}

static block_fn overlay_find(u32 key)
{
	u32 i;
	for (i = 0; i < ov_range_count; i++) {
		if (key < ov_ranges[i].start || key >= ov_ranges[i].end)
			continue;
		overlay_use((int)ov_ranges[i].unit, key);
		return table_find(ov_header->table, ov_header->mask, key);
	}
	return 0;
}

/* A task that may be the app starts it by jumping to the entry point. From then on
 * its unit can come back at any address, as a ROM overlay may have replaced it. */
static block_fn app_find(u32 pc, u32 word)
{
	u32 i;
	for (i = 0; i < ov_app_count; i++) {
		const ov_app_t *a = &ov_apps[i];
		u32 mask, slot;
		if (pc < a->start || pc >= a->end)
			continue;
		if (pc == a->entry && word == a->entry_word)
			app_running[i] = 1;
		if (!app_running[i])
			continue;
		overlay_use((int)a->unit, pc);
		mask = ov_header->mask;
		for (slot = ((pc >> 2) * 2654435761u) >> 8 & mask;; slot = (slot + 1) & mask) {
			const block_t *b = &ov_header->table[slot];
			if (!b->fn)
				break;
			if (b->key == pc) {
				if (ov_header->guard[slot] == word) {
					app_frame_us = a->frame_us;
					app_pad = a->pad[0] == PAD_NONE ? 0 : a->pad;
					return b->fn;
				}
				break;
			}
		}
		app_running[i] = 0;
	}
	return 0;
}

static u32 fetch_word(u32 pc);

static void ram_branch(u32 pc)
{
	u32 word = fetch_word(pc), target;
	s32 offset = (s32)(word << 8) >> 6;
	if (word & (1u << 24))
		R(14) = R15_FULL((pc + 4) & PC_MASK);
	target = (pc + 8 + (u32)offset) & 0x3FFFFFFu;
	TAIL(dispatch(target));
}

/* Veneers built on the stack load their SWI arguments with LDMIA r12!,{list} */
static void ram_ldm(u32 pc)
{
	C->cur = pc;
	R(12) = ldm_regs(R(12) & ~3u, fetch_word(pc) & 0x3FFu);
	TAIL(dispatch((pc + 4) & PC_MASK));
}

u32 mach_peek(u32 addr);

static void prefetch_abort(u32 pc)
{
	plat_log("no native code for pc=%07X word=%08X\n", pc, mach_peek(pc));
	if (pc < 0x20)
		plat_fatal("no native code for exception vector %02X\n", pc);
	take_exception(0x0C, MODE_SVC, pc + 4, 0);
	TAIL(dispatch(0x0C));
}

static block_fn table_find(const block_t *table, u32 mask, u32 key)
{
	u32 slot = ((key >> 2) * 2654435761u) >> 8 & mask;
	for (;; slot = (slot + 1) & mask) {
		const block_t *b = &table[slot];
		if (!b->fn)
			return 0;
		if (b->key == key)
			return b->fn;
	}
}

static u32 fetch_word(u32 pc)
{
	u8 *page = rtab[PRIV_SUPER][PAGE_INDEX(pc)];
	return page ? *(u32 *)(page + (pc & 0x7FFCu)) : 0;
}

u32 mach_miss_pc;

/* Where the interpreter fetches from. ROM pages may be cold. */
const u8 *mach_code_page(u32 pc)
{
	u8 *page = rtab[PRIV_SUPER][PAGE_INDEX(pc)];
	if (!page && pc >= 0x3800000u && rom_page_at(pc) >= 0) {
		rom_page_in(rom_page_at(pc));
		page = rtab[PRIV_SUPER][PAGE_INDEX(pc)];
	}
	return page;
}

u32 mach_peek(u32 addr)
{
	return fetch_word(addr & ~3u);
}

/* ROM code is reached by address. Anything else was precompiled per instruction
 * word, so a copied or generated stub runs natively wherever it lands. */
static block_fn find_block(u32 pc)
{
	block_fn fn;
#ifdef ARM_INTERP_ALL
	return arm_interp;
#endif
	if (pc >= 0x3800000u) {
		u32 key = 0x3800000u | (pc & 0x1FFFFFu);
		fn = table_find(block_table, block_table_mask, key);
		if (!fn)
			fn = overlay_find(key);
	} else {
		u32 word = fetch_word(pc);
		fn = app_find(pc, word);
		if (fn)
			return fn;
		if (!rtab[PRIV_SUPER][PAGE_INDEX(pc)]) {
			mach_miss_pc = pc;
			plat_fatal("jump to unmapped pc=%07X r14=%08X mode=%d\n", pc, R(14), C->psr & 3);
		}
		/* BASIC's SYS builds SWI instructions; the number is read by the handler */
#ifdef ARM_INTERP
		return arm_interp; /* anything in RAM, recompiled apps aside */
#endif
		if ((word >> 24) == 0xEF)
			return swi_insn;
		if ((word >> 25) == 0x75)
			return ram_branch; /* SharedCLibrary builds tables of branches */
		if ((word & 0xFFFFFC00u) == 0xE8BC0000u)
			return ram_ldm;
		fn = table_find(ram_table, ram_table_mask, word);
	}
#ifdef ARM_INTERP
	if (!fn) /* a ROM path the static scan missed */
		return arm_interp;
#endif
#ifndef STOP_ON_MISS
	if (!fn) /* Report it to RISC OS as an abort, which kills only the task */
		return prefetch_abort;
#endif
	if (!fn) {
		mach_miss_pc = pc;
		plat_fatal("no native code for pc=%07X word=%08X r14=%08X mode=%d\n", pc,
			fetch_word(pc), R(14), C->psr & 3);
	}
	return fn;
}

static u32 jump_ring[32], jump_pos;

void dispatch(u32 pc)
{
	jump_ring[jump_pos++ & 31] = pc;
	if (--C->budget < 0)
		TAIL(mach_poll(pc));
	TAIL(find_block(pc)(pc));
}

#ifdef HW_LOG
/* Frames with a palette split and the range of lines it fell on, for the heartbeat */
u32 mach_splits;
int mach_split_min = 9999, mach_split_max = -1;
#endif

/* Returns the microseconds the platform took to show it */
static u32 present(void)
{
	screen_t s;
	int bpp = 1 << ((vidc.cr >> 2) & 3);
	s.width = (vidc.hdend - vidc.hdstart) * 2;
	s.height = vidc.vdend - vidc.vdstart;
	s.bpp = bpp;
	s.ram = ram;
	s.ram_size = RAM_SIZE;
	s.start = vinit;
	s.base = vstart;
	s.end = vend + 16;
	s.palette = split_line >= 0 ? pal_top : vidc.reg;
	s.palette2 = vidc.reg;
	s.split_line = split_line;
#ifdef HW_LOG
	if (split_line >= 0) {
		mach_splits++;
		if (split_line < mach_split_min)
			mach_split_min = split_line;
		if (split_line > mach_split_max)
			mach_split_max = split_line;
	}
#endif
	s.cursor_palette[0] = vidc.reg[0x11];
	s.cursor_palette[1] = vidc.reg[0x12];
	s.cursor_palette[2] = vidc.reg[0x13];
	s.cursor_addr = cinit;
	{
		/* Offsets from the VIDC timing formula, which vary with colour depth */
		static const u8 k[4] = {19, 11, 7, 5};
		s.cursor_x = vidc.cx - (vidc.hdstart * 2 + k[(vidc.cr >> 2) & 3]);
	}
	s.cursor_y = vidc.cys - (vidc.vdstart - 1);
	s.cursor_h = vidc.cye - vidc.cys;
	return plat_frame(&s);
}

/* --- sound ---
 * VIDC plays one byte per period through eight stereo positions in turn. A buffer
 * is complete by the time the DMA reaches it, so it goes to the platform whole. */
#define SOUND_CHUNK 128
static s16 sound_linear[256];

static void sound_init(void)
{
	int v;
	for (v = 0; v < 256; v++) {
		int chord = v >> 5, point = (v >> 1) & 15;
		int level = (1 << (chord + 4)) - (1 << 4) + (point << chord);
		sound_linear[v] = (s16)(v & 1 ? -level : level);
	}
}

/* Several channels share the sample rate, so fold them into one output frame.
 * A channel in eight is 1/8 of the amplitude, as it is on the real DAC. */
static void sound_output(const u8 *src, u32 bytes, u32 period)
{
	static const u8 left[8] = {0, 6, 5, 4, 3, 2, 1, 0};
	static const u8 right[8] = {0, 0, 1, 2, 3, 4, 5, 6};
	s16 out[SOUND_CHUNK * 2];
	u8 image[8];
	u32 group = 1, shift = 0, frames, f, i, n = 0, pos = 0;
	while (1000000 / (period * group) > 30000 && group < 8) {
		group *= 2;
		shift++;
	}
	for (i = 0; i < 8; i++)
		image[i] = vidc.reg[0x18 + ((i + 1) & 7)] & 7;
	frames = bytes / group;
	for (f = 0; f < frames; f++) {
		s32 l = 0, r = 0;
		for (i = 0; i < group; i++, pos++) {
			s32 v = sound_linear[src[pos]];
			u32 im = image[pos & 7];
			l += v * left[im];
			r += v * right[im];
		}
		out[n * 2] = (s16)(l >> shift);
		out[n * 2 + 1] = (s16)(r >> shift);
		if (++n == SOUND_CHUNK) {
			plat_sound(out, (int)n, (int)(1000000 / (period * group)));
			n = 0;
		}
	}
	if (n)
		plat_sound(out, (int)n, (int)(1000000 / (period * group)));
}

/* The DMA loops on its buffer unless the OS gave it a new one since the last swap */
static void sound_swap(uint64_t now)
{
	u32 period = (vidc.reg[0x30] & 0xFF) + 2, bytes = 208;
	if (sound_fresh) {
		sound_start = sstart;
		sound_end = sendn;
		sound_fresh = 0;
	}
	if (sound_end > sound_start) {
		bytes = sound_end - sound_start + 16;
		sound_output(ram + sound_start, bytes, period);
	}
	/* Add to the old deadline, or poll jitter would slow the sample rate down */
	next_sound_us += (uint64_t)bytes * period;
	if (next_sound_us < now)
		next_sound_us = now + (uint64_t)bytes * period;
	ioc.irqb |= IRQB_SOUND;
}

static void step_timer(int n, u32 ticks)
{
	u32 period = ioc.latch[n] ? ioc.latch[n] + 1 : 0x10001;
	if (ticks >= period)
		ticks %= period;
	if (ioc.count[n] >= ticks) {
		ioc.count[n] -= ticks;
		if (ticks == 0 || ioc.count[n])
			return;
	} else {
		ioc.count[n] = period - (ticks - ioc.count[n]);
	}
	if (n == 0)
		ioc.irqa |= IRQA_T0;
	else if (n == 1)
		ioc.irqa |= IRQA_T1;
}

static void machine_tick(void)
{
	uint64_t now = plat_time_us();
	u32 delta = (u32)(now - last_us), took;
	int n;
	last_us = now;
	for (n = 0; n < 4; n++)
		step_timer(n, delta * 2);
	if (now >= next_frame_us) {
		next_frame_us = now + (app_frame_us ? app_frame_us : 20000);
		app_frame_us = 0;
		pad_map = app_pad;
		app_pad = 0;
		flyback_end_us = now + 2000;
		flyback = 1;
		ioc.irqa |= IRQA_VBLANK;
		took = present();
		split_line = -1;
		/* The machine stands still while the platform presents, which takes
		 * milliseconds on a console. Its timers skip that time and the frame starts
		 * once presenting is done. Otherwise the raster interrupt a game sets from
		 * the frame interrupt fires early, and its palette split lands on the wrong
		 * line, or gets no line at all. The next frame waits, if it must, until the
		 * display has had time to run. Timer 0 keeps counting, as it is the OS
		 * clock, and stopping it would make the time of day drift. On the harness
		 * presenting takes no time. */
		if (took) {
			int lines = vidc.vdend - vidc.vdstart;
			uint64_t display_us = (uint64_t)(lines > 0 && lines < 256 ? lines : 256) * 64;
			step_timer(0, took * 2);
			now += took;
			last_us = now;
			if (next_frame_us < now + display_us)
				next_frame_us = now + display_us;
			flyback_end_us = now + 2000;
		}
		frame_start_us = (u32)now;
	} else if (flyback && now >= flyback_end_us) {
		flyback = 0;
	}
	if ((memc_ctrl & 0x800) && now >= next_sound_us)
		sound_swap(now);
	fdc_tick(now);
	ide_tick(now);
	if (kb.rx_at && now >= kb.rx_at) {
		kb.rx_at = 0;
		ioc.irqb |= IRQB_KRX;
	}
	if (kb.tx_at && now >= kb.tx_at) {
		kb.tx_at = 0;
		ioc.irqb |= IRQB_KTX;
	}
	if (now >= next_input_us) {
		next_input_us = now + 10000;
		plat_poll_input();
		kb_poll();
	}
}

u32 mach_pc_ring[256];
u32 mach_pc_pos;

#ifdef HW_LOG
/* Time between polls, charged to wherever the poll lands: ROM, other RAM, or a
 * 256-byte bucket of the app. Good enough to rank routines on a real console. */
u32 mach_prof[2 + 64];
static uint64_t prof_last;

static void prof_step(u32 pc)
{
	uint64_t now = plat_time_us();
	u32 d = (u32)(now - prof_last), i = pc - ov_apps[0].start;
	prof_last = now;
	if (pc >= 0x3800000u)
		mach_prof[0] += d;
	else if (ov_app_count && pc >= ov_apps[0].start && i >> 8 < 64)
		mach_prof[2 + (i >> 8)] += d;
	else
		mach_prof[1] += d;
}
#endif

void mach_poll(u32 pc)
{
#ifdef HW_LOG
	prof_step(pc);
#endif
	mach_pc_ring[mach_pc_pos++ & 255] = pc;
	C->budget = POLL_BUDGET;
	machine_tick();
	if (fiq_pending() && !(C->psr & PSR_F)) {
		take_exception(0x1C, MODE_FIQ, pc + 4, PSR_F);
		pc = 0x1C;
	} else if (irq_pending() && !(C->psr & PSR_I)) {
		take_exception(0x18, MODE_IRQ, pc + 4, 0);
		pc = 0x18;
	}
	TAIL(find_block(pc)(pc));
}

void mach_dump(void)
{
	u32 i;
#if defined(ARM_INTERP) && !defined(__sh__)
	extern unsigned long long arm_insns;
	plat_log("interpreted %llu ARM instructions\n", arm_insns);
	if (getenv("DREAM_PROF")) {
		extern u32 arm_hist[];
		plat_log("palette writes %u by eighth:", vidc_pal_writes);
		for (i = 0; i < 8; i++)
			plat_log(" %u", vidc_when[i]);
		plat_log("\n");
		if (getenv("DREAM_RAM")) {
			u32 a;
			FILE *f = fopen(getenv("DREAM_RAM"), "wb");
			for (a = 0; a < 0x90000; a += 4) {
				u32 w = fetch_word(a);
				fwrite(&w, 4, 1, f);
			}
			fclose(f);
		}
		for (i = 0; i < 0x80000 / 4; i++)
			if (arm_hist[i])
				plat_log("prof %05X %u\n", i * 4, arm_hist[i]);
		{
			extern u32 arm_hist_rom[]; /* the 2MB ROM at 0x3800000, see interp.c */
			for (i = 0; i < 0x200000 / 4; i++)
				if (arm_hist_rom[i])
					plat_log("prof %07X %u\n", 0x3800000u + i * 4, arm_hist_rom[i]);
		}
		{
			extern u32 arm_hist_rma[];
			for (i = 0; i < 0x200000 / 4; i++)
				if (arm_hist_rma[i])
					plat_log("prof %07X %u\n", 0x1800000u + i * 4, arm_hist_rma[i]);
		}
	}
#endif
	plat_log("abort addr=%08X pc=%07X ret=%08X C=%p state=%p\n", abort_addr, abort_pc, abort_ret,
		(void *)C, (void *)&cpu_state);
	for (i = 0; i < 15; i++)
		plat_log("r%u=%08X ", i, R(i));
	plat_log("\n");
	plat_log("jumps:");
	for (i = jump_pos > 32 ? jump_pos - 32 : 0; i < jump_pos; i++)
		plat_log(" %07X", jump_ring[i & 31]);
	plat_log("\n");
	for (i = swi_pos > 64 ? swi_pos - 64 : 0; i < swi_pos; i++)
		plat_log("swi %07X m%u %06X r0=%08X r1=%08X r2=%08X\n", swi_ring[i & 63].pc,
			swi_ring[i & 63].mode, swi_ring[i & 63].num, swi_ring[i & 63].r0,
			swi_ring[i & 63].r1, swi_ring[i & 63].r2);
	plat_log("IOC irqa=%02X mska=%02X irqb=%02X mskb=%02X fiq=%02X mskf=%02X psr=%08X kb=%d ken=%d men=%d mdx=%d mdy=%d\n",
		ioc.irqa, ioc.mska, ioc.irqb, ioc.mskb, ioc.fiq, ioc.mskf, C->psr, kb.state, kb.key_enabled, kb.mouse_enabled, kb.mouse_dx, kb.mouse_dy);
}

/* Checksum of the visible screen memory, comparable between platforms. */
u32 mach_screen_sum(const screen_t *s)
{
	u32 size = (u32)(s->width * s->bpp / 8) * (u32)s->height, sum = 2166136261u, i;
	for (i = 0; i < size && s->start + i < RAM_SIZE; i++)
		sum = (sum ^ s->ram[s->start + i]) * 16777619u;
	return sum;
}

const u8 *mach_cmos(void)
{
	return cm.ram;
}

u32 mach_polls(void)
{
	return mach_pc_pos;
}

void mach_stop(void)
{
	longjmp(run_env, 1);
}

void mach_init(const u8 *cmos_image)
{
	int i;
#ifdef CPU_BASE_REG
	C = &cpu_state;
#endif
	ram = malloc(RAM_SIZE);
	if (!ram)
		plat_fatal("cannot allocate RAM\n");
	memset(ram, 0, RAM_SIZE);
	memcpy(cm.ram, cmos_image, 256);
	plat_hd_prepare(); /* may drop the card, which moves the archive up to master */
	for (i = 0; i < HD_UNITS; i++) {
		u32 sectors = plat_hd_sectors(i);
		if (!sectors)
			break; /* a slave with no master is never configured */
		ide_init(i, sectors, plat_hd_writable(i) ? "Dreamcast SD" : "Archicast archive");
	}
	if (i) {
		/* IDEDiscs lives in the top bits of logical CMOS byte 0x87. Keep the
		 * checksum at 0x3F right, or the kernel resets CMOS at boot. */
		u8 old = cm.ram[0xC7];
		cm.ram[0xC7] = (u8)((old & 0x3F) | (u8)(i << 6));
		cm.ram[0x3F] = (u8)(cm.ram[0x3F] + cm.ram[0xC7] - old);
	}
	if (i && plat_hd_writable(0)) {
		/* The card replaces the floppy, so it must be the default drive (logical
		 * byte 0x0B, low bits), or ADFS:$ and the scrap directory find no disc. */
		u8 old = cm.ram[0x4B];
		cm.ram[0x4B] = (u8)((old & 0xF8) | 4);
		cm.ram[0x3F] = (u8)(cm.ram[0x3F] + cm.ram[0x4B] - old);
	}
	cm.clock = cm.line = 1;
	reset_maps();
	rom_load_hot();
	fdc_init();
	sound_init();
#ifndef HW_NO_NET /* KOS taking the adapter from dcload can hang at boot */
	plat_net_init();
#endif
	for (i = 0; i < 3; i++)
		rtab[i][0] = rom_cache; /* ROM aliased at 0 until the kernel maps page zero */
	memset(&cpu_state, 0, sizeof(cpu_state));
	cpu_state.psr = PSR_I | PSR_F | MODE_SVC;
	cpu_state.fz = 1;
	ioc.irqa = IRQA_POR;
	ioc.irqb = IRQB_SOUND;
	ioc.ctrl = 0xFF;
	kb_single(0xFF);
	kb.state = KB_RESET;
	priv = PRIV_SUPER;
	update_tables();
	last_us = plat_time_us();
	next_frame_us = last_us + 20000;
	cm.rtc_base = plat_rtc_secs();
	cm.rtc_base_us = last_us;
	cmos_set_year(rtc_refresh());
	plat_log("clock %d-%02d-%02d %02d:%02d:%02d\n", cm.ram[0xC1] * 100 + cm.ram[0xC0], unbcd(cm.ram[6] & 0x1F),
		unbcd(cm.ram[5] & 0x3F), unbcd(cm.ram[4]), unbcd(cm.ram[3]), unbcd(cm.ram[2]));
}

void mach_run(u32 start_pc)
{
	int code = setjmp(run_env);
	if (code == 1)
		return;
	dispatch(code ? (u32)code - 1 : start_pc);
}
