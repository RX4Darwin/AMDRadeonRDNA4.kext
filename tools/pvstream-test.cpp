// Host test of src/pvstream.cpp: the three layers' framing and the generated opcode tables (docs/m1-stream.md).
#include "../src/pvstream.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace pvstream;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void put32(std::vector<uint8_t> &v, uint32_t x) { v.insert(v.end(), reinterpret_cast<uint8_t *>(&x), reinterpret_cast<uint8_t *>(&x) + 4); }
static void put16(std::vector<uint8_t> &v, uint16_t x) { v.insert(v.end(), reinterpret_cast<uint8_t *>(&x), reinterpret_cast<uint8_t *>(&x) + 2); }

struct Seen { unsigned n = 0; uint32_t ids[16]; const char *names[16]; uint32_t lens[16]; };
static void sink(void *u, uint32_t id, const char *name, const uint8_t *, uint32_t len, size_t) {
	Seen *s = static_cast<Seen *>(u);
	if (s->n < 16) { s->ids[s->n] = id; s->names[s->n] = name; s->lens[s->n] = len; s->n++; }
}

int main() {
	// tables: sorted, spot checks against what the disassembly showed
	int32_t len = 0;
	CHECK(std::strcmp(streamName(0x132, &len), "Blit.fillBuffer") == 0 && len == 24);
	CHECK(std::strcmp(streamName(0x0c8, &len), "Compute.dispatchThreadgroups") == 0 && len == 48);
	CHECK(std::strcmp(streamName(0x0d0, &len), "Compute.setComputePipelineState") == 0 && len == 4);
	CHECK(std::strcmp(streamName(0x7d, &len), "Render.setVertexBuffers") == 0 && len == -1);
	CHECK(std::strcmp(operationName(0x00d, &len), "Op.newFenceWithAllocator") == 0 && len == 4);
	CHECK(std::strcmp(operationName(0x3e8, &len), "Op.deleteBufferRef") == 0 && len == 4);
	CHECK(streamName(0xffff) == nullptr);
	CHECK(std::strcmp(fifoName(0x3a), "GetDeviceInfo") == 0 && std::strcmp(fifoName(0x37), "ExecIndirect") == 0 && fifoName(0x03) == nullptr);
	CHECK(std::strcmp(segmentTypeName(1), "compute") == 0);

	// FIFO: GetDeviceInfo with one 12-byte record, then DefineChannel, then a header that cannot be one
	std::vector<uint8_t> f;
	put16(f, 0x3a); put16(f, 0); put32(f, 12 + 12); put32(f, 7);          // header: 24 bytes
	put32(f, 0x2d); put32(f, 0x200); put32(f, 0x1234);                    // record: reply buffer 4096 bytes at page 0x1234
	put16(f, 0x30); put16(f, 0); put32(f, 16); put32(f, 0); put32(f, 3);  // DefineChannel 3
	put16(f, 0x38); put16(f, 1); put32(f, 12 + 8 + 16); put32(f, 0);      // DefineHostTask with one barrier
	put32(f, 1); put32(f, 2); put32(f, 5 << 1); put32(f, 0x00ff0000); put32(f, 0x0000000f); put32(f, 0x77);
	FifoCommand c;
	size_t at = 0;
	CHECK(fifoHeader(f.data() + at, f.size() - at, &c) && c.id == 0x3a && c.length == 24 && c.payloadOffset == 12);
	char line[200];
	describeFifo(f.data() + at, c, line, sizeof(line));
	CHECK(std::strstr(line, "GetDeviceInfo") && std::strstr(line, "page 0x1234") && std::strstr(line, "4096 bytes"));
	at += c.length;
	CHECK(fifoHeader(f.data() + at, f.size() - at, &c) && c.id == 0x30);
	describeFifo(f.data() + at, c, line, sizeof(line));
	CHECK(std::strstr(line, "channel 3"));
	at += c.length;
	CHECK(fifoHeader(f.data() + at, f.size() - at, &c) && c.id == 0x38 && c.barriers == 1 && c.payloadOffset == 20);
	describeFifo(f.data() + at, c, line, sizeof(line));
	CHECK(std::strstr(line, "task 5") && std::strstr(line, "root page 0x77"));
	at += c.length;
	CHECK(at == f.size());
	const uint8_t junk[12] = { 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0 };   // length 3: below the header, not a multiple of 4
	CHECK(!fifoHeader(junk, sizeof(junk), &c));
	CHECK(!fifoHeader(f.data(), 8, &c));                                // truncated header

	// DisplaySetupSharedState is described
	std::vector<uint8_t> d;
	put16(d, 0x01); put16(d, 0); put32(d, 20); put32(d, 2); put32(d, 3); put32(d, 0x4567);
	CHECK(fifoHeader(d.data(), d.size(), &c) && c.id == 0x01 && c.signal == 2);
	describeFifo(d.data(), c, line, sizeof(line));
	CHECK(std::strstr(line, "DisplaySetupSharedState") && std::strstr(line, "port 3") && std::strstr(line, "page 0x4567"));

	// The other display commands (docs/m1-stream.md s.1b): ProcessOnline, cursor glyph/state, SubmitTransaction 0x06/0x07, FlushChannelEvent
	{
		auto fifoLine = [&](std::vector<uint8_t> &v, uint16_t id, uint32_t signal) {
			std::vector<uint8_t> hdr;
			put16(hdr, id); put16(hdr, 0); put32(hdr, static_cast<uint32_t>(12 + v.size())); put32(hdr, signal);
			hdr.insert(hdr.end(), v.begin(), v.end());
			FifoCommand k;
			CHECK(fifoHeader(hdr.data(), hdr.size(), &k) && k.id == id && k.signal == signal && k.length == hdr.size());
			describeFifo(hdr.data(), k, line, sizeof(line));
		};
		std::vector<uint8_t> v;
		put32(v, 5); put32(v, 0xabcd);
		fifoLine(v, 0x02, 9);
		CHECK(std::strstr(line, "DisplayProcessOnline") && std::strstr(line, "port 5") && std::strstr(line, "cookie 0xabcd") && std::strstr(line, "signal 0x9"));
		v.clear(); put32(v, 5); put32(v, 1);
		fifoLine(v, 0x05, 1);
		CHECK(std::strstr(line, "DisplayUpdateCursorState") && std::strstr(line, "cursor visible"));
		v.clear(); put32(v, 5); put32(v, 0);
		fifoLine(v, 0x05, 1);
		CHECK(std::strstr(line, "cursor hidden"));
		v.clear(); put32(v, 2); put32(v, 77); put32(v, 3);   // port 2, surface 77, task 3
		fifoLine(v, 0x06, 4);
		CHECK(std::strstr(line, "DisplaySubmitTransaction,") && std::strstr(line, "port 2") && std::strstr(line, "surface 77") && std::strstr(line, "task 3"));
		v.clear(); put32(v, 2); put32(v, 3); put32(v, 77);   // port, task, surface
		put32(v, 0x1000); put32(v, 0); put32(v, 0x2000); put32(v, 0); put32(v, 256); put32(v, 0xfeed);
		fifoLine(v, 0x07, 4);
		CHECK(std::strstr(line, "DisplaySubmitTransaction2") && std::strstr(line, "task 3, surface 77") && std::strstr(line, "gamma 256 entries") &&
		      std::strstr(line, "a 0x1000") && std::strstr(line, "b 0x2000") && std::strstr(line, "sum 0xfeed"));
		v.clear(); put32(v, 2); put32(v, 3); put32(v, 0x7000); put32(v, 0); put32(v, 0x8000); put32(v, 0); put32(v, 256);
		put32(v, 64 | (64u << 16)); put32(v, 4); put32(v, 0x99);
		put32(v, 0);
		v.resize(44);
		{
			uint16_t g[4] = { 64, 48, 3, 5 };
			std::memcpy(v.data() + 32, g, sizeof(g));
		}
		fifoLine(v, 0x04, 6);
		CHECK(std::strstr(line, "DisplayUpdateCursorGlyph") && std::strstr(line, "image 64x48 (3, 5)"));
		v.clear();
		fifoLine(v, 0x1e, 0);
		CHECK(std::strstr(line, "DisplayFlushChannelEvent") && std::strstr(line, "0 payload"));
	}

	// the display shared-state page: the host's fill and the guest's read agree, the guest's words are not touched
	{
		uint8_t page[4096];
		std::memset(page, 0xa5, sizeof(page));
		const uint32_t enabled = 0x12345678, pending = 0x9abcdef0;
		std::memcpy(page + disp::kEnabled, &enabled, 4);
		std::memcpy(page + disp::kPending, &pending, 4);
		const uint8_t origin0 = page[disp::kOriginX], chroma0 = page[0x2c], cursor0 = page[disp::kCursorPos];
		disp::Info info;
		info.id = 0x1234;
		info.txnProtocol = 1;
		info.flags = disp::kFlagCursorVisibleKick | disp::kFlagCursorMoveKick;
		info.modeCount = 2;
		info.modes[1] = { 1280, 720, 30u << 16 };
		disp::fillInfo(page, 3, info);
		disp::InfoRead r;
		disp::readInfo(page, &r);
		CHECK(r.id == 0x1234 && r.port == 3 && r.width == 1920 && r.height == 1080 && r.cursorWidth == 64 && r.cursorHeight == 64);
		CHECK(r.txnProtocol == 1 && r.flags == 3 && r.cookie == 1 && r.count == 2);
		CHECK(std::strcmp(r.name, "RDNA4FB PV") == 0);
		CHECK(r.modes[0].width == 1920 && r.modes[0].height == 1080 && r.modes[0].refresh == (60u << 16));
		CHECK(r.modes[1].width == 1280 && r.modes[1].height == 720 && r.modes[1].refresh == (30u << 16));
		uint32_t e, pd;
		std::memcpy(&e, page + disp::kEnabled, 4);
		std::memcpy(&pd, page + disp::kPending, 4);
		CHECK(e == enabled && pd == pending);                                            // the event words are not the fill's business
		CHECK(page[disp::kOriginX] == origin0 && page[0x2c] == chroma0 && page[disp::kCursorPos] == cursor0 && page[disp::kScale] == 1);
		CHECK(page[disp::kModes + 8] == 0 && page[disp::kModes + 15] == 0);              // the unused half of a mode record is zeroed
		info.modeCount = 99;                                                             // clamped
		disp::fillInfo(page, 0, info);
		disp::readInfo(page, &r);
		CHECK(r.count == disp::kMaxModes && r.port == 0);
	}

	// stream: fillBuffer (24-byte payload) + dispatch (48) + an unknown id, then trailing garbage that must stop the walk
	std::vector<uint8_t> s;
	put32(s, 0x132); put32(s, 8 + 24); for (int i = 0; i < 6; i++) put32(s, i);
	put32(s, 0x0c8); put32(s, 8 + 48); for (int i = 0; i < 12; i++) put32(s, i);
	put32(s, 0x7777); put32(s, 8);
	const size_t good = s.size();
	put32(s, 0x0c8); put32(s, 5);   // size not a multiple of 4
	Seen seen;
	CHECK(walkCommands(s.data(), s.size(), false, sink, &seen) == good);
	CHECK(seen.n == 3 && seen.ids[0] == 0x132 && seen.lens[0] == 24 && seen.ids[1] == 0x0c8 && seen.lens[1] == 48 && seen.names[2] == nullptr && seen.lens[2] == 0);
	CHECK(walkCommands(s.data(), 4, false, sink, &seen) == 0);           // header cut short


	// GetDeviceInfo reply: build, parse with the mirror of Apple's parser, check the fields and the defaults
	{
		uint8_t buf[4096];
		const size_t n = buildDeviceInfoReply(buf, sizeof(buf), 64);
		CHECK(n > 20 && n < 64);
		uint8_t info[kDeviceInfoBytes] = {};
		parseDeviceInfoReference(buf, sizeof(buf) / 8, info);   // Apple walks bytes/8 = 512 pairs, the zero pairs (key 0) must change nothing
		auto u32 = [&](unsigned off) { uint32_t v; std::memcpy(&v, info + off, 4); return v; };
		CHECK(u32(0x00) == 4 && info[0xac] == 1);               // MSAASamples
		CHECK(u32(0x04) == 0 && info[0xad] == 1);               // D24S8Supported defined as 0
		CHECK(u32(0x08) == 1024 && u32(0x0c) == 1024 && u32(0x10) == 1024);
		CHECK(u32(0x14) == 32768 && info[0xb1] == 1);
		CHECK(u32(0x84) == 64 && info[0xcd] == 1);              // GpuCoreCount
		CHECK(info[0xb5] == 0 && u32(0x24) == 0);               // DeserializerVersion left out: not defined
		CHECK(info[0xbd] == 1 && u32(0x44) == 0x20002 && u32(0xd8) == 2 && u32(0xdc) == 2);   // shader version defaulted
		// a requested version above the driver's clamp is cut to 0x20007; one below is kept
		uint8_t hi[16] = {}, lo[16] = {};
		const uint32_t k12 = 0x12, big = 0x30002, small = 0x20004;
		std::memcpy(hi, &k12, 4); std::memcpy(hi + 4, &big, 4);
		std::memcpy(lo, &k12, 4); std::memcpy(lo + 4, &small, 4);
		uint8_t i2[kDeviceInfoBytes] = {}, i3[kDeviceInfoBytes] = {};
		parseDeviceInfoReference(hi, 2, i2);
		parseDeviceInfoReference(lo, 2, i3);
		uint32_t v2, v3;
		std::memcpy(&v2, i2 + 0x44, 4); std::memcpy(&v3, i3 + 0x44, 4);
		CHECK(v2 == 0x20007 && v3 == 0x20004);
		// every key we answer exists in the parser's table, and the table is the one read from the kext
		size_t keys = 0;
		const DeviceInfoKey *kt = deviceInfoKeys(&keys);
		CHECK(keys == 43 && kt[0].key == 1 && kt[0].value == 0 && kt[0].defined == 0xac);
		struct Cnt { unsigned n = 0, unknown = 0; } cnt;
		forEachReplyValue([](void *u, uint8_t, const char *name, uint32_t, const char *) {
			Cnt *c = static_cast<Cnt *>(u);
			c->n++;
			if (name[0] == '?') c->unknown++;
		}, &cnt, 64);
		CHECK(cnt.n == n && cnt.unknown == 0);
	}

	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("pvstream: all checks passed\n");
	return 0;
}
