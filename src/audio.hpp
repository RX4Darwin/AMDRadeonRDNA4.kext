// Audio over HDMI and DisplayPort (docs/audio-plan.md): the display side Linux programs for a stream with audio.
#ifndef Audio_hpp
#define Audio_hpp

#include <stdint.h>

namespace Audio {

struct Step {
	uint8_t  seg;      // DMU register segment (base index)
	uint32_t reg;      // dword offset
	uint32_t mask;     // the bits written: reg = (reg & ~mask) | value
	uint32_t value;
};

#include "audio_linux.inc"

}  // namespace Audio

#endif
