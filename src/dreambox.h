/* Dreambox, a network drive on dcvmu.com, as the RISC OS filing system Dreambox:.
 * FileSwitch calls a few words of ARM in the DreamApps module (tools/mkapps.py),
 * which call DreamNet_BoxFS until it stops asking to be called again. The
 * network side runs on a thread of its own, so like web.h this stays free of
 * cpu.h: net.c hands over the registers and a way into guest memory. */
#ifndef DREAMBOX_H
#define DREAMBOX_H
#include <stdint.h>

#define BOX_FS_NUMBER 0xDC /* filing system number; nothing Acorn shipped uses it */

enum { BOX_DONE, BOX_AGAIN, BOX_ERROR }; /* what the veneer gets back in r12 */

typedef struct {
	void (*read)(uint32_t addr, void *dest, uint32_t len);
	void (*write)(uint32_t addr, const void *src, uint32_t len);
	void (*string)(uint32_t addr, char *dest, uint32_t size);
} guest_mem_t;

/* r[0] to r[7] in and out, r[12] the FileSwitch entry on the first call. On
 * BOX_ERROR r[0] points at an error block in the workspace from dreambox_init. */
int dreambox_fs(uint32_t r[13], const guest_mem_t *mem);
void dreambox_init(uint32_t workspace);
/* A soft reset keeps the module's workspace but ends every task */
void dreambox_reset(void);

/* The icon bar task, which runs in the DreamApps module. That only issues the
 * SWI this names in r[10], from the table in tools/mkapps.py, and comes back
 * with its registers. r[10] is -1 on *Desktop_Dreambox. */
enum { TASK_INIT, TASK_ICON, TASK_POLL, TASK_MENU, TASK_READ, TASK_REPORT, TASK_CLI, TASK_CLOSE, TASK_EXIT };
void dreambox_task(uint32_t r[11], const guest_mem_t *mem);

/* The console's link token survives power off where the platform can manage it */
int plat_dreambox_load(char *token, int size); /* 1 if there was one */
int plat_dreambox_save(const char *token);     /* "" forgets it; 0 once it will survive */
const char *plat_dreambox_server(void);        /* http:// or https://, host and optional :port */
#endif
