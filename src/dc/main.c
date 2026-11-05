/* Dreamcast platform layer: KallistiOS video, Maple input and startup for the
 * natively recompiled RISC OS 3.11. */
#include <kos.h>
#include <dc/sq.h>
#include <dc/sound/stream.h>
#include <dc/sd.h>
#include <arch/cache.h>
#ifdef HW_PERF
#include <dc/perfctr.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "../cpu.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

extern const u8 cmos_image[];

#define MAX_LINES 512
#define CURSOR_LINE_BYTES 8

/* Two framebuffers, flipped so a full-screen redraw never tears. line_sum records
 * what each one shows, so a line is only drawn into a buffer that lacks it. */
static u32 line_sum[2][MAX_LINES];
static int back_fb;
static uint16_t *fb_addr[2];
static void clear_buffers(void);
static u32 wrapped[160];
static uint16_t row[640] __attribute__((aligned(32)));
static uint16_t luts[2][256];
static u32 lut_pairs[2][256];
/* 4bpp: a source byte is two pixels, so one lookup gives both, or all four once
 * a low resolution line is doubled */
static u32 lut_nibbles[2][256], lut_quads[2][256][2];
static const uint16_t *lut = luts[0];
static const u32 *lut_pair = lut_pairs[0], *lut_nibble = lut_nibbles[0];
static const u32 (*lut_quad)[2] = lut_quads[0];
static u32 last_pal[2][20];
static int last_w, last_h, last_bpp, frame_count;
static u32 last_start;
static volatile u32 ring_in, ring_out, sound_rate, sound_held;
#ifdef HW_REPLAY
static screen_t replay_screen;
#endif

uint64_t plat_time_us(void)
{
	return timer_us_gettime64();
}

/* The AICA's clock, which the BIOS menu sets. Local time, no zone, and a
 * 32-bit count from 1950, so it runs out in 2086. */
int64_t plat_rtc_secs(void)
{
	return (int64_t)rtc_unix_secs();
}

/* KOS reads the clock back after writing it, so success means it took */
void plat_rtc_set(int64_t secs)
{
	if (rtc_set_unix_secs((time_t)secs) < 0)
		printf("rtc: cannot set the clock\n");
	else
		printf("rtc set to %lld\n", (long long)secs);
}

void plat_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void plat_fatal(const char *fmt, ...)
{
	char text[160];
	va_list ap;
	extern void mach_dump(void);
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	printf("FATAL: %s", text);
	mach_dump();
	bfont_draw_str(vram_s + 640 * 8 + 8, 640, 1, text);
	for (;;)
		thd_sleep(1000);
}

#ifndef PC_ROOT
#define PC_ROOT ""
#endif

/* Files live on the disc. Under dcload the host serves them over /pc instead. */
static file_t open_data(const char *name)
{
	char path[192];
	file_t f;
	snprintf(path, sizeof(path), "/cd/%s", name);
	f = fs_open(path, O_RDONLY);
	if (f == FILEHND_INVALID) {
		snprintf(path, sizeof(path), "/pc%s/%s", PC_ROOT, name);
		f = fs_open(path, O_RDONLY);
	}
	if (f == FILEHND_INVALID)
		plat_fatal("cannot open %s\n", name);
	return f;
}

static file_t rom_file = FILEHND_INVALID;

void plat_rom_read(u32 page, u8 *dest)
{
	if (rom_file == FILEHND_INVALID)
		rom_file = open_data("rom.bin");
	/* The read may bypass the cache, so leave no stale lines behind it */
	dcache_purge_range((uintptr_t)dest, 0x8000);
	fs_seek(rom_file, (off_t)page * 0x8000, SEEK_SET);
	if (fs_read(rom_file, dest, 0x8000) != 0x8000)
		plat_fatal("short read of ROM page %lu\n", (unsigned long)page);
}

static u32 sd_sectors;

/* No card usually means init fails quickly, and the machine runs from the floppy. */
static void sd_open(void)
{
	sd_init_params_t params = {.interface = SD_IF_SCIF, .check_crc = true};
	const char *console = dbgio_dev_get();
	uint64_t bytes;
	/* The probe takes over SCIF, so nothing may log through it meanwhile */
	dbgio_dev_select("null");
	if (sd_init_ex(&params)) {
		if (console)
			dbgio_dev_select(console);
		return;
	}
	bytes = sd_get_size();
	if (bytes == (uint64_t)-1 || bytes < 16 * 1024 * 1024) {
		sd_shutdown();
		if (console)
			dbgio_dev_select(console);
		return;
	}
	/* The serial port is now the SPI bus. Keep dcload's console, which is network */
	if (console && strcmp(console, "scif"))
		dbgio_dev_select(console);
	bytes /= 512;
	sd_sectors = bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)bytes;
	printf("SD card: %lu sectors\n", (unsigned long)sd_sectors);
}

/* The software archive is a read-only hard disc image on the CD. ADFS asks for one
 * sector at a time and the GD-ROM is slow to seek, so reads go through a chunk
 * cache; directory walks and file loads are mostly sequential. */
#define AR_CHUNK 64 /* sectors */
static file_t ar_file = FILEHND_INVALID;
static u32 ar_sectors, ar_base = ~0u, ar_have;
static u8 ar_buf[AR_CHUNK * 512] __attribute__((aligned(32)));

static void archive_open(void)
{
	char path[192];
	int pass;
	for (pass = 0; pass < 2 && ar_file == FILEHND_INVALID; pass++) {
		snprintf(path, sizeof(path), pass ? "/pc%s/archive.hd" : "/cd/archive.hd", PC_ROOT);
		ar_file = fs_open(path, O_RDONLY);
	}
	if (ar_file == FILEHND_INVALID)
		return;
	ar_sectors = (u32)(fs_total(ar_file) / 512);
	printf("archive: %lu sectors\n", (unsigned long)ar_sectors);
}

static int archive_read(u32 lba, u8 *dest)
{
	u32 base = lba & ~(u32)(AR_CHUNK - 1);
	if (base != ar_base) {
		ssize_t got;
		ar_base = ~0u;
		/* The read may bypass the cache, so leave no stale lines behind it */
		dcache_purge_range((uintptr_t)ar_buf, sizeof(ar_buf));
		fs_seek(ar_file, (off_t)base * 512, SEEK_SET);
		got = fs_read(ar_file, ar_buf, sizeof(ar_buf));
		if (got < 512)
			return -1;
		ar_base = base;
		ar_have = (u32)got / 512;
	}
	if (lba - base >= ar_have)
		return -1;
	memcpy(dest, ar_buf + (lba - base) * 512, 512);
	return 0;
}

/* The card is unit 0 when present and the archive follows it; otherwise the
 * archive is the master on its own. */
static int is_card(int unit)
{
	return unit == 0 && sd_sectors;
}

static int is_archive(int unit)
{
	return ar_sectors && unit == (sd_sectors ? 1 : 0);
}

u32 plat_hd_sectors(int unit)
{
	return is_card(unit) ? sd_sectors : is_archive(unit) ? ar_sectors : 0;
}

int plat_hd_writable(int unit)
{
	return is_card(unit);
}

int plat_hd_read(int unit, u32 lba, u8 *dest)
{
	if (is_card(unit))
		return sd_read_blocks(lba, 1, dest);
	return is_archive(unit) ? archive_read(lba, dest) : -1;
}

int plat_hd_write(int unit, u32 lba, const u8 *src)
{
	return is_card(unit) ? sd_write_blocks(lba, 1, src) : -1;
}

/* Either buffer may be on screen, so prompts go to both */
static void show_line(int row, const char *text)
{
	bfont_draw_str(fb_addr[0] + 640 * (120 + row * 28) + 64, 640, 1, text);
	bfont_draw_str(fb_addr[1] + 640 * (120 + row * 28) + 64, 640, 1, text);
}

/* A or Y formats, B or N leaves the card alone for this session. */
static int ask_format(void)
{
	clear_buffers();
	show_line(0, "This SD card is not formatted for RISC OS.");
	show_line(2, "Formatting ERASES EVERYTHING on the card.");
	show_line(4, "A or Y: format as an ADFS hard disc");
	show_line(5, "B or N: start without it");
#ifdef HW_AUTOFORMAT
	return 1;
#endif
	for (;;) {
		maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
		maple_device_t *kbd = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
		cont_state_t *c = dev ? maple_dev_status(dev) : NULL;
		kbd_state_t *k = kbd ? kbd_get_state(kbd) : NULL;
		if ((c && (c->buttons & CONT_A)) || (k && k->key_states[KBD_KEY_Y].is_down))
			return 1;
		if ((c && (c->buttons & CONT_B)) || (k && k->key_states[KBD_KEY_N].is_down))
			return 0;
		thd_sleep(50);
	}
}

/* A card that is declined, unformattable or unreadable is dropped for the session */
int plat_hd_prepare(void)
{
	int ok = 1, state;
	if (!sd_sectors)
		return 1;
	state = hd_is_formatted();
	if (state) {
		if (state < 0)
			sd_sectors = 0; /* unreadable: leave the card alone */
		return state > 0;
	}
	if (!ask_format()) {
		ok = 0;
	} else {
		clear_buffers();
		show_line(0, "Formatting SD card...");
		if (hd_format(sd_sectors)) {
			show_line(2, "Format failed.");
			thd_sleep(3000);
			ok = 0;
		}
	}
	clear_buffers();
	if (!ok)
		sd_sectors = 0;
	return ok;
}

static file_t disc_file = FILEHND_INVALID;

int plat_disc_open(void)
{
	char path[192];
	int pass;
	if (sd_sectors)
		return 0; /* the card takes over */
	for (pass = 0; pass < 2 && disc_file == FILEHND_INVALID; pass++) {
		snprintf(path, sizeof(path), pass ? "/pc%s/adfs.adf" : "/cd/adfs.adf", PC_ROOT);
		disc_file = fs_open(path, O_RDONLY);
	}
	return disc_file != FILEHND_INVALID;
}

int plat_disc_read(u8 *sector)
{
	return fs_read(disc_file, sector, 1024) == 1024;
}

void plat_disc_close(void)
{
	fs_close(disc_file);
	disc_file = FILEHND_INVALID;
}

/* Filled from disc; each unit is linked to run at this address. */
u8 overlay_region[OVERLAY_BYTES] __attribute__((aligned(32)));

const ov_header_t *plat_overlay_load(int unit)
{
	char name[16];
	const ov_header_t *h = (const ov_header_t *)overlay_region;
	file_t f;
	ssize_t got;
	snprintf(name, sizeof(name), "ov_%d.bin", unit);
	f = open_data(name);
	/* The read may bypass the cache, so leave no stale lines behind it */
	dcache_purge_range((uintptr_t)overlay_region, OVERLAY_BYTES);
	got = fs_read(f, overlay_region, OVERLAY_BYTES);
	fs_close(f);
	if (got <= 0 || h->magic != OV_MAGIC)
		plat_fatal("bad overlay %s\n", name);
	dcache_flush_range((uintptr_t)overlay_region, (size_t)got);
	icache_flush_range((uintptr_t)overlay_region, (size_t)got);
	return h;
}

static inline uint16_t vidc_565(u32 e)
{
	/* Acorn video black sits at level 3, as Arculator's default black level: (n-3)*255/12 */
	static const u8 level[16] = {0, 0, 0, 0, 21, 42, 63, 85, 106, 127, 148, 170, 191, 212, 233, 255};
	u32 r = level[e & 15], g = level[(e >> 4) & 15], b = level[(e >> 8) & 15];
	return (uint16_t)((r >> 3 << 11) | (g >> 2 << 5) | (b >> 3));
}

static void build_lut(const screen_t *s, const u32 *palette, int set)
{
	int i;
	for (i = 0; i < (1 << s->bpp); i++) {
		u32 e = palette[i & 15];
		if (s->bpp == 8)
			e = (e & 0x737) | ((i & 0x10) >> 1) | ((i & 0x60) << 1) | ((i & 0x80) << 4);
		luts[set][i] = vidc_565(e);
		lut_pairs[set][i] = luts[set][i] * 0x10001u;
	}
	if (s->bpp == 4)
		for (i = 0; i < 256; i++) {
			u32 lo = luts[set][i & 15], hi = luts[set][i >> 4];
			lut_nibbles[set][i] = lo | (hi << 16);
			lut_quads[set][i][0] = lo * 0x10001u;
			lut_quads[set][i][1] = hi * 0x10001u;
		}
}

/* Fills row at the output width. Low resolution modes have fewer pixels on the
 * same scanline, so widen them. */
static void expand_line(const screen_t *s, const u8 *src, int width, int scale)
{
	int x, i;
	if (s->bpp == 8 && scale == 2) {
		u32 *out = (u32 *)row;
		for (x = 0; x < width; x++)
			out[x] = lut_pair[src[x]];
		return;
	}
	if (s->bpp == 4 && scale <= 2) {
		u32 *out = (u32 *)row;
		if (scale == 2)
			for (x = 0; x < width >> 1; x++) {
				const u32 *q = lut_quad[src[x]];
				out[x * 2] = q[0];
				out[x * 2 + 1] = q[1];
			}
		else
			for (x = 0; x < width >> 1; x++)
				out[x] = lut_nibble[src[x]];
		return;
	}
	switch (s->bpp) {
	case 4:
		for (x = 0; x < width; x += 2) {
			u8 v = src[x >> 1];
			row[x] = lut[v & 15];
			row[x + 1] = lut[v >> 4];
		}
		break;
	case 8:
		for (x = 0; x < width; x++)
			row[x] = lut[src[x]];
		break;
	default:
		for (x = 0; x < width; x++) {
			u32 bit = (u32)x * (u32)s->bpp;
			row[x] = lut[(src[bit >> 3] >> (bit & 7)) & ((1u << s->bpp) - 1)];
		}
		break;
	}
	if (scale > 1)
		for (x = width - 1; x >= 0; x--)
			for (i = scale - 1; i >= 0; i--)
				row[x * scale + i] = row[x];
}

static void overlay_cursor(const screen_t *s, int y, int width, int scale)
{
	int line = y - s->cursor_y, x, i;
	u32 addr;
	if (line < 0 || line >= s->cursor_h)
		return;
	addr = s->cursor_addr + (u32)line * CURSOR_LINE_BYTES;
	for (x = 0; x < 32; x++) {
		u32 v = (s->ram[addr + (u32)(x >> 2)] >> ((x & 3) * 2)) & 3;
		int px = s->cursor_x + x;
		if (v && px >= 0 && px < width)
			for (i = 0; i < scale; i++)
				row[px * scale + i] = vidc_565(s->cursor_palette[v - 1]);
	}
}

/* Stands in for the line's contents; pointer position and shape are part of it. */
static u32 sum_line(const screen_t *s, const u8 *src, u32 bytes, int y)
{
	const u32 *p = (const u32 *)src, *end = (const u32 *)(src + (bytes & ~3u));
	int line = y - s->cursor_y;
	u32 sum = 0x811C9DC5u;
	for (; p < end; p++)
		sum = (sum << 5) + sum + *p; /* a plain rotate cancels out on solid fills */
	if (line >= 0 && line < s->cursor_h) {
		const u32 *c = (const u32 *)(s->ram + s->cursor_addr + (u32)line * CURSOR_LINE_BYTES);
		sum ^= ((u32)s->cursor_x * 2654435761u) ^ c[0] ^ (c[1] << 7);
	}
	return sum;
}

static inline void put_row(int y, int height, int pixels)
{
	int first = y * 480 / height, last = (y + 1) * 480 / height, d;
	for (d = first; d < last; d++)
		sq_cpy(vram_s + d * 640, row, (size_t)pixels * 2);
}

static void clear_buffers(void)
{
	/* vram_s is the buffer after the one flipped to, so this order clears both and
	 * leaves vram_s on back_fb, which plat_frame relies on */
	vid_clear(0, 0, 0);
	vid_flip(back_fb);
	vid_clear(0, 0, 0);
	vid_flip(1 - back_fb);
	memset(line_sum, 0xA5, sizeof(line_sum));
}

#ifdef HW_LOG
static uint64_t present_us;
#endif

/* Redraw only lines that changed. A screen bank flip always means a new picture,
 * otherwise 25Hz is enough. */
static void present_frame(const screen_t *s)
{
#ifdef HW_REPLAY
	replay_screen = *s;
#endif
#ifdef HW_LOG
	/* Hardware runs have no video capture: one line every 5s reports progress.
	 * dcload printf runs with interrupts off, so never log more often. */
	static uint64_t next_log;
	static u32 log_frames, log_polls, log_flips;
	static int dumped;
	uint64_t now = timer_us_gettime64();
	log_frames++;
	log_flips += s->start != last_start;
	if (now >= next_log) {
		u32 polls = mach_polls();
		printf("hb t=%llu frames=%lu polls=%lu flips=%lu present=%lums mode=%dx%dx%d sum=%08lX sound=%lu/%lu held=%lu\n",
			(unsigned long long)(now / 1000000), (unsigned long)log_frames,
			(unsigned long)(polls - log_polls), (unsigned long)log_flips,
			(unsigned long)(present_us / 1000), s->width, s->height, s->bpp,
			(unsigned long)mach_screen_sum(s), (unsigned long)ring_in,
			(unsigned long)ring_out, (unsigned long)sound_held);
		{
			extern u32 mach_prof[66];
			u32 app = 0;
			int i;
			for (i = 2; i < 66; i++)
				app += mach_prof[i];
			printf("prof rom=%lums ram=%lums app=%lums:", (unsigned long)(mach_prof[0] / 1000),
				(unsigned long)(mach_prof[1] / 1000), (unsigned long)(app / 1000));
			for (i = 2; i < 66; i++)
				if (mach_prof[i] > app / 20)
					printf(" %X=%lu", 0x8000 + (i - 2) * 256, (unsigned long)(mach_prof[i] / 1000));
			printf("\n");
			memset(mach_prof, 0, sizeof(mach_prof));
		}
		{
			extern u32 mach_splits;
			extern int mach_split_min, mach_split_max;
			u32 mach_peek(u32 addr);
			/* the kernel's speed calibration, which a mode change divides by */
			printf("splits=%lu lines %d-%d calib=%08lX\n", (unsigned long)mach_splits, mach_split_min,
				mach_split_max, (unsigned long)mach_peek(0x110));
			mach_splits = 0;
			mach_split_min = 9999;
			mach_split_max = -1;
		}
#ifdef ARM_INTERP
		{
			extern u32 arm_insns;
			static u32 last_insns;
			printf("arm insns=%lu in 5s\n", (unsigned long)(arm_insns - last_insns));
#ifdef HW_PERF
			{
				extern u32 interp_cycles, interp_events;
				u32 n = arm_insns - last_insns;
				printf("perf interp=%lums cyc/insn=%lu event%d=%lu per insn x100=%lu\n",
					(unsigned long)(interp_cycles / 200000), (unsigned long)(n ? interp_cycles / n : 0),
					HW_PERF, (unsigned long)interp_events,
					(unsigned long)(n ? (uint64_t)interp_events * 100 / n : 0));
				interp_cycles = interp_events = 0;
			}
#endif
			last_insns = arm_insns;
		}
#endif
		log_flips = 0;
		present_us = 0;
		{
			extern u32 mach_pc_ring[256];
			extern void mach_dump(void);
			printf("hb pcs %07lX %07lX %07lX %07lX\n", (unsigned long)mach_pc_ring[0],
				(unsigned long)mach_pc_ring[85], (unsigned long)mach_pc_ring[170],
				(unsigned long)mach_pc_ring[255]);
			if (s->bpp == 1 && now > 30000000 && !dumped) {
				dumped = 1;
				mach_dump();
			}
		}
		log_polls = polls;
		next_log = now + 5000000;
	}
#endif
	u32 addr = s->start, line_bytes = (u32)(s->width * s->bpp / 8), n;
	int width = s->width > 640 ? 640 : s->width;
	int scale, height = s->height;
	u32 *sums = line_sum[back_fb];
	int y, drawn = 0, split = s->split_line >= 0 ? s->split_line : height;
	const u32 *below = s->split_line >= 0 ? s->palette2 : s->palette;
	if (++frame_count & 1 && s->start == last_start)
		return;
	last_start = s->start;
	if (width <= 0 || height <= 0 || line_bytes == 0)
		return;
	scale = 640 / width;
	n = (u32)(width * s->bpp / 8);
	if (width == last_w && height == last_h && s->bpp == last_bpp) {
		/* Games cycle colours all the time, so a new palette only redraws its own lines */
		int part;
		for (part = 0; part < 2; part++) {
			const u32 *pal = part ? below : s->palette;
			if (!memcmp(last_pal[part], pal, sizeof(last_pal[part])))
				continue;
			memcpy(last_pal[part], pal, sizeof(last_pal[part]));
			build_lut(s, pal, part);
			for (y = part ? split : 0; y < (part ? height : split) && y < MAX_LINES; y++)
				line_sum[0][y] = line_sum[1][y] = 0xA5A5A5A5u;
		}
	} else {
		memcpy(last_pal[0], s->palette, sizeof(last_pal[0]));
		memcpy(last_pal[1], below, sizeof(last_pal[1]));
		last_w = width;
		last_h = height;
		last_bpp = s->bpp;
		build_lut(s, s->palette, 0);
		build_lut(s, below, 1);
		clear_buffers();
		sums = line_sum[back_fb];
	}
	for (y = 0; y < height && y < MAX_LINES; y++) {
		const u8 *src = s->ram + addr;
		/* A hardware scrolled screen can wrap part way through a line */
		if (addr < s->end && addr + n > s->end) {
			u32 part = s->end - addr;
			memcpy(wrapped, src, part);
			memcpy((u8 *)wrapped + part, s->ram + s->base, n - part);
			src = (const u8 *)wrapped;
		}
		int part = y >= split;
		u32 sum = sum_line(s, src, n, y) ^ (u32)part;
		if (sum != sums[y]) {
			sums[y] = sum;
			lut = luts[part];
			lut_pair = lut_pairs[part];
			lut_nibble = lut_nibbles[part];
			lut_quad = lut_quads[part];
			expand_line(s, src, width, scale);
			overlay_cursor(s, y, width, scale);
			put_row(y, height, width * scale);
			drawn++;
		}
		addr += line_bytes;
		if (addr >= s->end)
			addr = s->base + (addr - s->end);
	}
	if (drawn) {
		vid_flip(back_fb);
		/* Flycast only redraws after a write to the buffer it last scanned out, and a
		 * flip alone is not one, so rewrite a pixel of both. A console never notices. */
		for (y = 0; y < 2; y++) {
			volatile uint16_t *pixel = &fb_addr[y][240 * 640];
			*pixel = *pixel;
		}
		back_fb = 1 - back_fb;
	}
#ifdef HW_LOG
	present_us += timer_us_gettime64() - now;
#endif
}

/* The machine stands still meanwhile, so it needs to know how long this took */
u32 plat_frame(const screen_t *s)
{
	uint64_t start = timer_us_gettime64();
	present_frame(s);
	return (u32)(timer_us_gettime64() - start);
}

/* The machine layer fills a ring and a thread feeds it to the AICA, so sound keeps
 * flowing while the machine blocks on disc. The thread runs inside KOS, where r14
 * is not the machine state, so it must stay away from anything using C. */
#define SOUND_BYTES 4096 /* per channel, on the AICA; refilled by half */
#define RING_FRAMES 2048
#define SOUND_SLACK 256 /* frames in hand before starting, to absorb buffer timing */
static u32 ring[RING_FRAMES];
static u32 sound_out[SOUND_BYTES / 4] __attribute__((aligned(32)));
static snd_stream_hnd_t sound_stream = SND_STREAM_INVALID;

void plat_sound(const s16 *stereo, int frames, int rate)
{
	const u32 *in = (const u32 *)stereo;
	int i;
	sound_rate = (u32)rate;
	for (i = 0; i < frames && ring_in - ring_out < RING_FRAMES; i++) {
		ring[ring_in & (RING_FRAMES - 1)] = in[i];
		ring_in++;
	}
}

/* Running dry holds the last frame, which is silent, instead of clicking */
static void *sound_fill(snd_stream_hnd_t stream, int bytes, int *got)
{
	static u32 last;
	static int primed;
	u32 n = (u32)bytes / 4, i, have = ring_in - ring_out;
	(void)stream;
	if (have >= n + SOUND_SLACK)
		primed = 1;
	for (i = 0; i < n; i++) {
		if (primed && ring_out != ring_in) {
			last = ring[ring_out & (RING_FRAMES - 1)];
			ring_out++;
		} else {
			primed = 0;
			sound_held++;
		}
		sound_out[i] = last;
	}
	*got = bytes;
	return sound_out;
}

static void *sound_thread(void *arg)
{
	u32 rate = 0;
	(void)arg;
	for (;;) {
		if (sound_rate != rate) {
			if (rate)
				snd_stream_stop(sound_stream);
			rate = sound_rate;
			snd_stream_start(sound_stream, rate, 1);
		}
		if (rate)
			snd_stream_poll(sound_stream);
		thd_sleep(10);
	}
	return NULL;
}

static void sound_start(void)
{
	kthread_attr_t attr = {.stack_size = 16384, .prio = PRIO_DEFAULT - 1, .label = "sound"};
	if (snd_stream_init_ex(2, SOUND_BYTES) < 0)
		return;
	sound_stream = snd_stream_alloc(sound_fill, SOUND_BYTES);
	if (sound_stream != SND_STREAM_INVALID)
		thd_create_ex(&attr, sound_thread, NULL);
}

/* USB HID code to Archimedes keyboard row and column; 0xFF is unmapped. */
#define K(r, c) (((r) << 4) | (c))
static const u8 hid_map[0x64] = {
	[0x04] = K(3,12), [0x05] = K(5,2), [0x06] = K(5,0), [0x07] = K(3,14), [0x08] = K(2,9),
	[0x09] = K(3,15), [0x0A] = K(4,0), [0x0B] = K(4,1), [0x0C] = K(2,14), [0x0D] = K(4,2),
	[0x0E] = K(4,3), [0x0F] = K(4,4), [0x10] = K(5,4), [0x11] = K(5,3), [0x12] = K(2,15),
	[0x13] = K(3,0), [0x14] = K(2,7), [0x15] = K(2,10), [0x16] = K(3,13), [0x17] = K(2,11),
	[0x18] = K(2,13), [0x19] = K(5,1), [0x1A] = K(2,8), [0x1B] = K(4,15), [0x1C] = K(2,12),
	[0x1D] = K(4,14), [0x1E] = K(1,1), [0x1F] = K(1,2), [0x20] = K(1,3), [0x21] = K(1,4),
	[0x22] = K(1,5), [0x23] = K(1,6), [0x24] = K(1,7), [0x25] = K(1,8), [0x26] = K(1,9),
	[0x27] = K(1,10), [0x28] = K(4,7), [0x29] = K(0,0), [0x2A] = K(1,14), [0x2B] = K(2,6),
	[0x2C] = K(5,15), [0x2D] = K(1,11), [0x2E] = K(1,12), [0x2F] = K(3,1), [0x30] = K(3,2),
	[0x31] = K(3,3), [0x33] = K(4,5), [0x34] = K(4,6), [0x35] = K(1,0), [0x36] = K(5,5),
	[0x37] = K(5,6), [0x38] = K(5,7), [0x39] = K(5,13), [0x3A] = K(0,1), [0x3B] = K(0,2),
	[0x3C] = K(0,3), [0x3D] = K(0,4), [0x3E] = K(0,5), [0x3F] = K(0,6), [0x40] = K(0,7),
	[0x41] = K(0,8), [0x42] = K(0,9), [0x43] = K(0,10), [0x44] = K(0,11), [0x45] = K(0,12),
	[0x47] = K(0,14), [0x48] = K(0,15), [0x49] = K(1,15), [0x4A] = K(2,0), [0x4B] = K(2,1),
	[0x4C] = K(3,4), [0x4D] = K(3,5), [0x4E] = K(3,6), [0x4F] = K(6,4), [0x50] = K(6,2),
	[0x51] = K(6,3), [0x52] = K(5,9), [0x53] = K(2,2), [0x54] = K(2,3), [0x55] = K(2,4),
	[0x56] = K(2,5), [0x57] = K(4,11), [0x58] = K(6,7), [0x59] = K(5,10), [0x5A] = K(5,11),
	[0x5B] = K(5,12), [0x5C] = K(4,8), [0x5D] = K(4,9), [0x5E] = K(4,10), [0x5F] = K(3,7),
	[0x60] = K(3,8), [0x61] = K(3,9), [0x62] = K(6,5), [0x63] = K(6,6),
};

static const struct { u32 mask; int row, col; } modifiers[] = {
	{0x01, 3, 11}, {0x02, 4, 12}, {0x04, 5, 14},
	{0x10, 6, 1}, {0x20, 5, 8}, {0x40, 6, 0},
};

static u8 key_down[0x64];
static u32 mod_down;

static void poll_keyboard(void)
{
	maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
	kbd_state_t *k = dev ? kbd_get_state(dev) : NULL;
	u32 mods, changed;
	int i;
	if (!k)
		return;
	for (i = 4; i < 0x64; i++) {
		int down = k->key_states[i].is_down != 0;
		if (down != key_down[i] && hid_map[i]) {
			key_down[i] = (u8)down;
			mach_key(hid_map[i] >> 4, hid_map[i] & 15, down);
		}
	}
	mods = k->last_modifiers.raw;
	changed = mods ^ mod_down;
	for (i = 0; i < 6; i++)
		if (changed & modifiers[i].mask)
			mach_key(modifiers[i].row, modifiers[i].col, !!(mods & modifiers[i].mask));
	mod_down = mods;
}

#ifdef HW_REPLAY
/* Scripted input for unattended hardware tests, generated from a .replay file */
typedef struct { u32 ms; char verb; int a, b; } replay_t;
#include "replay.h"
static u32 replay_pos;
static int replay_dx, replay_dy, replay_buttons;

/* Flycast only: serial at this rate would take minutes over dcload. The displayed
 * buffer as hex, for tools/fbdump.py, so the output can be checked without a capture. */
static void dump_screen(int id)
{
#ifdef HW_DUMP
	const uint16_t *fb = fb_addr[1 - back_fb];
	static char text[2600];
	int x, y;
	static const char hex[] = "0123456789abcdef";
	if (id != 7)
		return;
	printf("fbdump %d\n", id);
	for (y = 0; y < 480; y++) {
		for (x = 0; x < 640; x++) {
			uint16_t v = fb[y * 640 + x];
			text[x * 4] = hex[v >> 12];
			text[x * 4 + 1] = hex[(v >> 8) & 15];
			text[x * 4 + 2] = hex[(v >> 4) & 15];
			text[x * 4 + 3] = hex[v & 15];
		}
		text[2560] = '\n';
		text[2561] = 0;
		fputs(text, stdout);
	}
#else
	(void)id;
#endif
}

static void replay_step(int *dx, int *dy, int *buttons)
{
	static u32 last_key_ms;
	u32 now = (u32)(timer_us_gettime64() / 1000);
	while (replay_pos < sizeof(replay_events) / sizeof(replay_events[0]) &&
	       now >= replay_events[replay_pos].ms) {
		const replay_t *e = &replay_events[replay_pos];
		/* After a stall (dcload printf runs with interrupts off) a key's press and
		 * release would both land in one poll, and RISC OS would never see it */
		if (e->verb == 'k' && now - last_key_ms < 40)
			break;
		replay_pos++;
		if (e->verb == 'k')
			last_key_ms = now;
		if (e->verb == 'm') {
			replay_dx += e->a;
			replay_dy += e->b;
		} else if (e->verb == 'b') {
			replay_buttons = e->a;
		} else if (e->verb == 'k') {
			mach_key(e->a >> 8, e->a & 15, e->b);
		} else {
			char path[192];
			file_t f;
			snprintf(path, sizeof(path), "/pc%s/../screen%d.bin", PC_ROOT, e->a);
			f = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
			if (f != FILEHND_INVALID) {
				u32 bytes = (u32)(replay_screen.width * replay_screen.height * replay_screen.bpp / 8);
				u32 head = replay_screen.end - replay_screen.start;
				if (head > bytes)
					head = bytes;
				fs_write(f, replay_screen.ram + replay_screen.start, head);
				fs_write(f, replay_screen.ram + replay_screen.base, bytes - head);
				fs_close(f);
			}
			printf("replay checkpoint %d sum=%08lX rtc=%lld\n", e->a,
				(unsigned long)mach_screen_sum(&replay_screen), (long long)rtc_unix_secs());
			dump_screen(e->a);
		}
	}
	*dx = replay_dx > 16 ? 16 : replay_dx < -16 ? -16 : replay_dx;
	*dy = replay_dy > 16 ? 16 : replay_dy < -16 ? -16 : replay_dy;
	replay_dx -= *dx;
	replay_dy -= *dy;
	*buttons = replay_buttons;
}
#endif

static u32 pad_keys;

/* An app can turn the controller into keys, for games with no use for a pointer */
static void pad_app(cont_state_t *c, const u8 *map)
{
	static const u32 masks[PAD_BUTTONS] = {CONT_DPAD_LEFT, CONT_DPAD_RIGHT, CONT_DPAD_UP,
		CONT_DPAD_DOWN, CONT_A, CONT_B, CONT_X, CONT_START};
	static u8 held[PAD_BUTTONS];
	int i;
	for (i = 0; i < PAD_BUTTONS; i++) {
		u8 code = map ? map[i] : held[i] ? held[i] - 1 : PAD_NONE;
		int down = map && c && (c->buttons & masks[i]);
		if (code == PAD_NONE || !!held[i] == down)
			continue;
		mach_key(code >> 4, code & 15, down);
		held[i] = down ? code + 1 : 0;
	}
}

void plat_poll_input(void)
{
	int dx = 0, dy = 0, buttons = 0;
	maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_MOUSE);
	const u8 *map = mach_pad_map();
	poll_keyboard();
	if (dev) {
		mouse_state_t *m = maple_dev_status(dev);
		if (m) {
			dx = m->dx;
			dy = m->dy;
			m->dx = m->dy = 0;
			buttons = ((m->buttons & MOUSE_LEFTBUTTON) ? 1 : 0) |
			          ((m->buttons & MOUSE_RIGHTBUTTON) ? 2 : 0) |
			          ((m->buttons & MOUSE_SIDEBUTTON) ? 4 : 0);
		}
	}
	dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
	if (dev) {
		cont_state_t *c = maple_dev_status(dev);
		if (c) {
			dx += c->joyx / 24;
			dy += c->joyy / 24;
			if (!map) {
				if (c->buttons & CONT_DPAD_LEFT) dx -= 4;
				if (c->buttons & CONT_DPAD_RIGHT) dx += 4;
				if (c->buttons & CONT_DPAD_UP) dy -= 4;
				if (c->buttons & CONT_DPAD_DOWN) dy += 4;
				if (c->buttons & CONT_A) buttons |= 1;
				if (c->buttons & CONT_B) buttons |= 2;
				if (c->buttons & CONT_X) buttons |= 4;
			}
		}
	}
	pad_app(dev ? maple_dev_status(dev) : 0, map);
	if (dev) {
		cont_state_t *c = maple_dev_status(dev);
		u32 now = c ? c->buttons & (CONT_START | CONT_Y) : 0;
		if (map)
			now &= ~CONT_START;
		if ((now ^ pad_keys) & CONT_START)
			mach_key(0, 0, !!(now & CONT_START)); /* Escape */
		if ((now ^ pad_keys) & CONT_Y)
			mach_key(4, 7, !!(now & CONT_Y)); /* Return */
		pad_keys = now;
	}
#ifdef HW_REPLAY
	replay_step(&dx, &dy, &buttons);
#endif
	mach_mouse(dx, dy, buttons);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	vid_set_mode(DM_640x480 | DM_MULTIBUFFER, PM_RGB565);
	vid_mode->fb_count = 2; /* KOS fills video RAM with them otherwise */
	vid_flip(1);
	fb_addr[0] = vram_s;
	vid_flip(0);
	fb_addr[1] = vram_s;
	back_fb = 1;
	clear_buffers();
#ifdef HW_PERF
	perf_cntr_start(PRFC1, HW_PERF, PMCR_COUNT_CPU_CYCLES);
#endif
	sound_start();
#ifndef HW_NO_SD /* a card replaces the floppy, and these tests need the floppy */
	sd_open();
#endif
	archive_open();
	mach_init(cmos_image);
	mach_run(0);
	return 0;
}
