/* Network printing. RISC OS's own PDriverDP renders pages in strips and hands them
 * to a dumper; ours is a few words of ARM in the DreamApps module (tools/mkapps.py)
 * that land here through the DreamNet SWIs. Strips become URF raster, which a
 * worker thread streams to the printer as an IPP Print-Job, so like web.h this
 * stays free of cpu.h. */
#ifndef PRINT_H
#define PRINT_H
#include <stdint.h>

#define PRINT_DPI 300

enum { PRINT_IDLE, PRINT_BUSY, PRINT_DONE, PRINT_FAILED };
enum { PAPER_LETTER, PAPER_A4, PAPER_COUNT };
enum { PRINT_OK, PRINT_WAIT, PRINT_ERROR }; /* what the dumper's SWI returns in r0 */

/* Both structures are shared with BASIC, so the layouts are part of the SWI API */
typedef struct {
	uint32_t paper;      /* PAPER_* */
	char host[60];       /* address, optionally :port and /path */
} print_config_t;

typedef struct {
	uint32_t state;      /* PRINT_* */
	uint32_t pages;      /* finished in this job */
	uint32_t bytes;      /* sent to the printer in this job */
	char text[48];       /* one line of progress for the user */
} print_status_t;

void print_get_config(print_config_t *config);
void print_set_config(const print_config_t *config);
void print_status(print_status_t *status);

int print_job_start(void);
int print_page_start(int top_rows, int left_pixels);
int print_row_ready(void);                   /* room for one more row? */
void print_row(const uint8_t *bits, int width); /* 1 bit per pixel, bit 0 leftmost */
int print_page_end(void);
int print_job_end(int abort);

/* Settings survive power off where the platform can manage it */
int plat_print_load(print_config_t *config);    /* 1 if there was one */
int plat_print_save(const print_config_t *config); /* 0 once it will survive */
#endif
