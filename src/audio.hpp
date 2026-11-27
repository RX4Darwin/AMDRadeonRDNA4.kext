// Audio over HDMI and DisplayPort (docs/audio-plan.md): the display side Linux programs for a stream with audio.
#ifndef Audio_hpp
#define Audio_hpp

#include <stddef.h>
#include <stdint.h>

namespace Audio {

struct Step {
	uint8_t  seg;      // DMU register segment (base index)
	uint32_t reg;      // dword offset
	uint32_t mask;     // the bits written: reg = (reg & ~mask) | value
	uint32_t value;
};

#include "audio_linux.inc"

// What a monitor's EDID says about its audio, as amdgpu reads it (dm_helpers_parse_edid_caps, amdgpu_dm_fill_audio_info):
// the IDs from the base block, the monitor name, the short audio descriptors (SADs) of its CTA extensions (only from
// CTA revision 3 on) and the first speaker allocation byte (5, FL/FR + FC, without one).
struct Sink {
	uint16_t manufacturer, product;
	char     name[19];          // NUL-terminated, at most 13 characters
	uint8_t  speakers;          // speaker allocation; bit 7 the sink "supports AI" (ACP/ISRC)
	uint8_t  sadCount;
	uint8_t  sads[16][3];       // as in the EDID
};

bool sinkFromEdid(const uint8_t *edid, size_t len, Sink &out);   // false without a valid base block

// The value Linux's dce_aud_az_configure writes into Azalia endpoint register `index` (the endpoint's indirect space)
// for this sink on a DisplayPort SST stream (8b/10b: no audio bandwidth check), for the registers that depend on the sink:
// the speaker allocation (with DP_CONNECTION), ACP SUPPORTS_AI, the audio descriptors 0x28-0x35, SINK_INFO0/1 (IDs, name
// length) and SINK_INFO4-8 (the name). False for any other register (the table's value stands).
bool sinkValue(uint32_t index, const Sink &sink, uint32_t &value);

}  // namespace Audio

#endif
