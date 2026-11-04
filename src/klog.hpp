/*
 * klog.hpp
 * AMDRadeonRDNA4
 *
 * The kext's own log. Every line goes to IOLog as before and into a ring
 * buffer that `sysctl debug.rdna4_log` reads: the kernel's message buffer
 * only keeps the last few seconds of a verbose boot, and the lines of the
 * first ones (EDID, pipes, heads) are what a boot is judged by.
 */

#ifndef KLog_hpp
#define KLog_hpp

#include <stddef.h>

/*
 * Ring size: a boot's worth of lines with rdna4-trace=1; the paravirtual
 * host's command buffer dumps need more. rdna4-logd follows the ring into
 * /var/log/rdna4 through debug.rdna4_log_read.
 */
#ifdef AMDRadeonRDNA4_PVGPU
constexpr size_t kRdna4LogRing = 8 * 1024 * 1024;
#else
constexpr size_t kRdna4LogRing = 256 * 1024;
#endif

void rdna4Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Once, from pluginStart: the lock and the sysctl. */
void rdna4LogInit();

#endif /* KLog_hpp */
