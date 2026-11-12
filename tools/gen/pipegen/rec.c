#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "dm_services.h"
#include "dc.h"
#include "reg_helper.h"
#include "rec.h"
#include <dlfcn.h>

/* The Linux function a register helper was called from, for the step labels. */
static const char *caller(void *ra)
{
	Dl_info i;
	if (dladdr(ra, &i) && i.dli_sname) return i.dli_sname[0] == '_' ? i.dli_sname + 1 : i.dli_sname;
	return "?";
}
#define CALLER() caller(__builtin_return_address(0))

#define MAXEV 8192
struct ev evs[MAXEV]; int nev;
int rec_log_reads = 1;

static struct { uint32_t addr, val; } mem[16384]; static int nmem;
static uint32_t fill;
uint32_t rec_peek(uint32_t a) { for (int i = 0; i < nmem; i++) if (mem[i].addr == a) return mem[i].val; return fill; }
void rec_reset(uint32_t f);
void rec_poke(uint32_t a, uint32_t v)
{
	for (int i = 0; i < nmem; i++) if (mem[i].addr == a) { mem[i].val = v; return; }
	mem[nmem].addr = a; mem[nmem++].val = v;
}
void rec_poke_field(uint32_t a, uint32_t mask, uint32_t v) { rec_poke(a, (rec_peek(a) & ~mask) | (v & mask)); }
static void add(enum ev_kind k, uint32_t a, uint32_t m, uint32_t v, uint32_t arg, const char *who)
{
	if (nev >= MAXEV) { fprintf(stderr, "too many events\n"); abort(); }
	evs[nev++] = (struct ev){ k, a, m, v, arg, strdup(who) };
}
void rec_mark(const char *what, uint32_t arg) { add(EV_MARK, 0, 0, 0, arg, what); }

static const char *set_caller = "?";

/* A loop that polls a status field becomes one wait: the first read of the field by that function records the
 * wait, and every read sees the field settled. */
static struct { const char *func; uint32_t addr, mask, settled, timeout_us; int seen; } polls[16]; static int npolls;
void rec_poll(const char *func, uint32_t addr, uint32_t mask, uint32_t settled, uint32_t timeout_us)
{
	polls[npolls].func = func; polls[npolls].addr = addr; polls[npolls].mask = mask;
	polls[npolls].settled = settled; polls[npolls].timeout_us = timeout_us; polls[npolls++].seen = 0;
}
static void add(enum ev_kind k, uint32_t a, uint32_t m, uint32_t v, uint32_t arg, const char *who);

uint32_t dm_read_reg_func(const struct dc_context *ctx, uint32_t address, const char *func_name)
{
	uint32_t v;
	for (int i = 0; i < npolls; i++)
		if (polls[i].addr == address && !strcmp(polls[i].func, func_name)) {
			rec_poke_field(address, polls[i].mask, polls[i].settled);
			if (!polls[i].seen++) add(EV_WAIT, address, polls[i].mask, polls[i].settled, polls[i].timeout_us, func_name);
			return rec_peek(address);
		}
	v = rec_peek(address);
	if (rec_log_reads) fprintf(stderr, "READ  %08x = %08x  in %s\n", address, v, func_name);
	return v;
}
void dm_write_reg_func(const struct dc_context *ctx, uint32_t address, uint32_t value, const char *func_name)
{
	if (address == 0) { fprintf(stderr, "DROP  write to absent register in %s\n", func_name); return; }
	rec_poke(address, value);
	add(EV_WRITE, address, 0xffffffff, value, 0, strcmp(func_name, "REG_SET") ? func_name : set_caller);
}
static void gather(uint32_t *mask, uint32_t *value, int n, uint8_t s1, uint32_t m1, uint32_t v1, va_list ap)
{
	*mask = m1; *value = (v1 << s1) & m1;
	for (int i = 1; i < n; i++) {
		uint32_t s = va_arg(ap, uint32_t), m = va_arg(ap, uint32_t), v = va_arg(ap, uint32_t);
		*value = (*value & ~m) | ((v << s) & m); *mask |= m;
	}
}
uint32_t generic_reg_update_ex(const struct dc_context *ctx, uint32_t addr, int n,
		uint8_t shift1, uint32_t mask1, uint32_t field_value1, ...)
{
	uint32_t mask, value, v; va_list ap;
	va_start(ap, field_value1); gather(&mask, &value, n, shift1, mask1, field_value1, ap); va_end(ap);
	if (addr == 0) { fprintf(stderr, "DROP  update of absent register in %s\n", CALLER()); return 0; }
	v = (rec_peek(addr) & ~mask) | value;
	rec_poke(addr, v);
	add(EV_UPDATE, addr, mask, value, 0, CALLER());
	return v;
}
uint32_t generic_reg_set_ex(const struct dc_context *ctx, uint32_t addr, uint32_t reg_val, int n,
		uint8_t shift1, uint32_t mask1, uint32_t field_value1, ...)
{
	uint32_t mask, value; va_list ap;
	va_start(ap, field_value1); gather(&mask, &value, n, shift1, mask1, field_value1, ap); va_end(ap);
	reg_val = (reg_val & ~mask) | value;
	set_caller = CALLER();
	dm_write_reg_func(ctx, addr, reg_val, "REG_SET");
	return reg_val;
}
#define RD() dm_read_reg_func(ctx, addr, CALLER())
#define GET(i) *field_value##i = (reg_val & mask##i) >> shift##i
uint32_t generic_reg_get(const struct dc_context *ctx, uint32_t addr, uint8_t shift1, uint32_t mask1, uint32_t *field_value1)
{ uint32_t reg_val = RD(); GET(1); return reg_val; }
uint32_t generic_reg_get2(const struct dc_context *ctx, uint32_t addr, uint8_t shift1, uint32_t mask1, uint32_t *field_value1,
		uint8_t shift2, uint32_t mask2, uint32_t *field_value2)
{ uint32_t reg_val = RD(); GET(1); GET(2); return reg_val; }
uint32_t generic_reg_get3(const struct dc_context *ctx, uint32_t addr, uint8_t shift1, uint32_t mask1, uint32_t *field_value1,
		uint8_t shift2, uint32_t mask2, uint32_t *field_value2, uint8_t shift3, uint32_t mask3, uint32_t *field_value3)
{ uint32_t reg_val = RD(); GET(1); GET(2); GET(3); return reg_val; }
uint32_t generic_reg_get4(const struct dc_context *ctx, uint32_t addr, uint8_t shift1, uint32_t mask1, uint32_t *field_value1,
		uint8_t shift2, uint32_t mask2, uint32_t *field_value2, uint8_t shift3, uint32_t mask3, uint32_t *field_value3,
		uint8_t shift4, uint32_t mask4, uint32_t *field_value4)
{ uint32_t reg_val = RD(); GET(1); GET(2); GET(3); GET(4); return reg_val; }
uint32_t generic_reg_get5(const struct dc_context *ctx, uint32_t addr, uint8_t shift1, uint32_t mask1, uint32_t *field_value1,
		uint8_t shift2, uint32_t mask2, uint32_t *field_value2, uint8_t shift3, uint32_t mask3, uint32_t *field_value3,
		uint8_t shift4, uint32_t mask4, uint32_t *field_value4, uint8_t shift5, uint32_t mask5, uint32_t *field_value5)
{ uint32_t reg_val = RD(); GET(1); GET(2); GET(3); GET(4); GET(5); return reg_val; }
uint32_t generic_reg_get6(const struct dc_context *ctx, uint32_t addr, uint8_t shift1, uint32_t mask1, uint32_t *field_value1,
		uint8_t shift2, uint32_t mask2, uint32_t *field_value2, uint8_t shift3, uint32_t mask3, uint32_t *field_value3,
		uint8_t shift4, uint32_t mask4, uint32_t *field_value4, uint8_t shift5, uint32_t mask5, uint32_t *field_value5,
		uint8_t shift6, uint32_t mask6, uint32_t *field_value6)
{ uint32_t reg_val = RD(); GET(1); GET(2); GET(3); GET(4); GET(5); GET(6); return reg_val; }
uint32_t generic_reg_get7(const struct dc_context *ctx, uint32_t addr, uint8_t shift1, uint32_t mask1, uint32_t *field_value1,
		uint8_t shift2, uint32_t mask2, uint32_t *field_value2, uint8_t shift3, uint32_t mask3, uint32_t *field_value3,
		uint8_t shift4, uint32_t mask4, uint32_t *field_value4, uint8_t shift5, uint32_t mask5, uint32_t *field_value5,
		uint8_t shift6, uint32_t mask6, uint32_t *field_value6, uint8_t shift7, uint32_t mask7, uint32_t *field_value7)
{ uint32_t reg_val = RD(); GET(1); GET(2); GET(3); GET(4); GET(5); GET(6); GET(7); return reg_val; }
uint32_t generic_reg_get8(const struct dc_context *ctx, uint32_t addr, uint8_t shift1, uint32_t mask1, uint32_t *field_value1,
		uint8_t shift2, uint32_t mask2, uint32_t *field_value2, uint8_t shift3, uint32_t mask3, uint32_t *field_value3,
		uint8_t shift4, uint32_t mask4, uint32_t *field_value4, uint8_t shift5, uint32_t mask5, uint32_t *field_value5,
		uint8_t shift6, uint32_t mask6, uint32_t *field_value6, uint8_t shift7, uint32_t mask7, uint32_t *field_value7,
		uint8_t shift8, uint32_t mask8, uint32_t *field_value8)
{ uint32_t reg_val = RD(); GET(1); GET(2); GET(3); GET(4); GET(5); GET(6); GET(7); GET(8); return reg_val; }

/* A wait always "succeeds" in the model: the field takes the awaited value. */
void generic_reg_wait(const struct dc_context *ctx, uint32_t addr, uint32_t shift, uint32_t mask, uint32_t condition_value,
		unsigned int delay_between_poll_us, unsigned int time_out_num_tries, const char *func_name, int line)
{
	rec_poke(addr, (rec_peek(addr) & ~mask) | ((condition_value << shift) & mask));
	add(EV_WAIT, addr, mask, (condition_value << shift) & mask, delay_between_poll_us * time_out_num_tries, func_name);
}
void dcn41_shim_udelay(unsigned long us) { add(EV_DELAY, 0, 0, 0, (uint32_t)us, CALLER()); }
void dcn41_shim_warn(const char *file, int line) { fprintf(stderr, "WARN  %s:%d\n", strrchr(file, '/') + 1, line); }

void rec_reset(uint32_t f) { nmem = 0; nev = 0; fill = f; npolls = 0; }
