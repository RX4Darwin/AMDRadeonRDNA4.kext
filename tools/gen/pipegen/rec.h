/* Recorder: Linux DC's register helpers, implemented to record instead of touching hardware. */
#ifndef REC_H
#define REC_H
#include <stdint.h>
enum ev_kind { EV_WRITE, EV_UPDATE, EV_WAIT, EV_DELAY, EV_MARK };
struct ev { enum ev_kind kind; uint32_t addr, mask, value, arg; const char *label; };
extern struct ev evs[]; extern int nev;
void rec_mark(const char *what, uint32_t arg);
void rec_poke(uint32_t addr, uint32_t value);     /* model state the GOP is assumed to leave */
uint32_t rec_peek(uint32_t addr);
void rec_poke_field(uint32_t addr, uint32_t mask, uint32_t value);   /* settle one field, leave the rest unknown */
void rec_reset(uint32_t fill);              /* forget everything; unknown registers read as fill */
void rec_poll(const char *func, uint32_t addr, uint32_t mask, uint32_t settled, uint32_t timeout_us);
extern int rec_log_reads;
#endif
