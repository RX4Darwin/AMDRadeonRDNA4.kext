/*
 * pvjob.hpp
 * AMDRadeonRDNA4
 *
 * The paravirtual host's compute dispatches, handed to the compiler service
 * in user space (include/rdna4pvjob.h). One job at a time: the host's work
 * loop runs a client's command buffer and waits in run() until the service
 * has compiled and run the dispatch and ended it with done(), or until the
 * timeout. Without a service that asked for work lately, run() fails at once
 * and the dispatch is skipped, as before there was one.
 */

#ifndef PvJob_hpp
#define PvJob_hpp

#include <stdint.h>

#include "rdna4pvjob.h"

#ifdef KERNEL
#include <IOKit/IOReturn.h>
#include <mach/task.h>

namespace PvJob
{

/* The host: hand `job` (id filled here) to the service and wait for it. */
bool run(RDNA4PvJob &job, uint32_t timeoutMs);

/* The service: the next job, waiting up to `timeoutMs` for one. */
IOReturn next(uint32_t timeoutMs, RDNA4PvJob &out);

/* The service: `len` bytes between the client's memory at its GPU address
 * `va` and `user` in the service's task. */
IOReturn access(uint32_t task, uint64_t va, task_t userTask, uint64_t user, uint64_t len, bool write);

/* The service: the job is over; status 0 = it ran. */
IOReturn done(uint32_t id, uint32_t status);

} /* namespace PvJob */
#endif

#endif /* PvJob_hpp */
