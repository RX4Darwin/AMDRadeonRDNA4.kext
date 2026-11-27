// Audio over HDMI and DisplayPort (docs/audio-plan.md): a monitor's audio from its EDID, as Linux puts it into the Azalia
// endpoint (dce_aud_az_configure; amdgpu_dm_helpers.c for what is read from the EDID).
#include "audio.hpp"

namespace Audio {

namespace {

// Azalia endpoint registers (the endpoint's indirect space, ixAZALIA_F0_CODEC_PIN_CONTROL_*).
constexpr uint32_t kChannelSpeaker = 0x25, kAcpData = 0x27, kDescriptor0 = 0x28, kSinkInfo0 = 0x3a, kSinkInfo1 = 0x3b,
                   kSinkInfo4 = 0x3e, kSinkInfo8 = 0x42;
constexpr uint32_t kFormats = 14;               // AUDIO_FORMAT_CODE_COUNT - 1: codes 1-14, descriptors 0x28-0x35
constexpr uint8_t  kLpcm = 1, kOneBit = 9, kDst = 13;
constexpr uint32_t kDpConnection = 1u << 17;    // CHANNEL_SPEAKER.DP_CONNECTION
constexpr uint32_t kMaxName = 18;               // MAX_HW_AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS
constexpr uint8_t  kDefaultSpeakers = 5;        // DEFAULT_SPEAKER_LOCATION

}  // namespace

bool sinkFromEdid(const uint8_t *edid, size_t len, Sink &out) {
	out = Sink {};
	static const uint8_t kHeader[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	if (!edid || len < 128)
		return false;
	for (int i = 0; i < 8; i++)
		if (edid[i] != kHeader[i])
			return false;
	out.manufacturer = static_cast<uint16_t>(edid[8] | edid[9] << 8);
	out.product = static_cast<uint16_t>(edid[10] | edid[11] << 8);
	for (size_t d = 54; d + 18 <= 126; d += 18)   // the monitor name descriptor (drm_edid_get_monitor_name: to the newline)
		if (!edid[d] && !edid[d + 1] && edid[d + 3] == 0xfc) {
			size_t n = 0;
			while (n < 13 && edid[d + 5 + n] != '\n')
				out.name[n] = static_cast<char>(edid[d + 5 + n]), n++;
			break;
		}
	bool speakers = false;
	for (size_t off = 128; off + 128 <= len; off += 128) {
		const uint8_t *b = edid + off;
		if (b[0] != 0x02 || b[1] < 3 || b[2] < 4)   // a CTA extension of revision 3 or later with data blocks
			continue;
		const size_t end = b[2] < 127 ? b[2] : 127;
		for (size_t pos = 4; pos < end;) {
			const uint8_t tag = b[pos] >> 5, n = b[pos] & 0x1f;
			if (pos + 1 + n > end)
				break;
			if (tag == 1)   // audio data block: SADs of 3 bytes
				for (size_t i = 0; i + 3 <= n && out.sadCount < 16; i += 3, out.sadCount++)
					for (int k = 0; k < 3; k++)
						out.sads[out.sadCount][k] = b[pos + 1 + i + k];
			else if (tag == 4 && n >= 1 && !speakers)   // speaker allocation data block
				out.speakers = b[pos + 1], speakers = true;
			pos += 1u + n;
		}
	}
	if (!speakers)
		out.speakers = kDefaultSpeakers;
	return true;
}

bool sinkValue(uint32_t index, const Sink &s, uint32_t &value) {
	if (index == kChannelSpeaker) {
		value = (s.speakers & 0x7fu) | kDpConnection;   // SPEAKER_ALLOCATION; LFE, HDMI 0, DP 1, EXTRA bit 0 cleared
		return true;
	}
	if (index == kAcpData) {
		value = (s.speakers >> 7 & 1u) << 6;   // SUPPORTS_AI
		return true;
	}
	if (index >= kDescriptor0 && index < kDescriptor0 + kFormats) {
		const uint8_t code = static_cast<uint8_t>(index - kDescriptor0 + 1);
		if (code == kOneBit || code == kDst)
			return false;   // (not programmed)
		int best = -1;
		for (int i = 0; i < s.sadCount; i++) {   // the format's SAD with the most channels (is_audio_format_supported)
			if ((s.sads[i][0] >> 3 & 0xf) != code)
				continue;
			if (best < 0 || (s.sads[i][0] & 7) > (s.sads[best][0] & 7))
				best = i;
		}
		value = 0;
		if (best >= 0) {
			const uint8_t *sad = s.sads[best];
			const uint32_t channels = sad[0] & 7u, rates = sad[1] & 0x7fu, byte2 = sad[2];
			// MAX_CHANNELS (channels - 1), SUPPORTED_FREQUENCIES, DESCRIPTOR_BYTE_2; LPCM also SUPPORTED_FREQUENCIES_STEREO
			value = channels | rates << 8 | byte2 << 16 | (code == kLpcm ? rates << 24 : 0);
		}
		return true;
	}
	if (index == kSinkInfo0) {
		value = s.manufacturer | static_cast<uint32_t>(s.product) << 16;
		return true;
	}
	uint32_t len = 0;   // the name's length counting its NUL, at most 18 (dce_aud_az_configure's loop)
	while (len < kMaxName && s.name[len])
		len++;
	if (index == kSinkInfo1) {
		value = len < kMaxName ? len + 1 : kMaxName;
		return true;
	}
	if (index >= kSinkInfo4 && index <= kSinkInfo8) {
		const uint32_t first = (index - kSinkInfo4) * 4, count = index == kSinkInfo8 ? 2 : 4;
		value = 0;
		for (uint32_t i = 0; i < count; i++)
			value |= static_cast<uint32_t>(static_cast<uint8_t>(first + i < sizeof(s.name) ? s.name[first + i] : 0)) << (8 * i);
		return true;
	}
	return false;
}

}  // namespace Audio
