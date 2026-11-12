/*
 * vrambench.hpp
 * AMDRadeonRDNA4
 *
 * CPU write and read speed of VRAM through BAR0, per cache mode, logged
 * with rdna4-vrambench=1 (vrambench.cpp).
 */

#ifndef VramBench_hpp
#define VramBench_hpp

#include <stdint.h>

namespace VramBench
{

/*
 * Measure 8 MiB at `freeOffset` behind the start of BAR0: VRAM nothing
 * else uses (behind the extra heads' surface slots, below the compute
 * pool).
 */
void run(uint64_t fbPhysBase, uint64_t freeOffset);

} /* namespace VramBench */

#endif /* VramBench_hpp */
