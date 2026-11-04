/*
 * pvsurface.cpp
 * AMDRadeonRDNA4
 *
 * See pvsurface.hpp.
 */

#include "pvsurface.hpp"
#include "klog.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>

#define SLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: pvgpu: surface: " fmt "\n", ##__VA_ARGS__)

namespace
{

const char *kPathSurface = "/System/Library/Extensions/IOSurface.kext/Contents/MacOS/IOSurface";

KernelPatcher::KextInfo kextSurface {
	"com.apple.iokit.IOSurface", &kPathSurface, 1, { true }, {},
	KernelPatcher::KextInfo::Unloaded,
};

/* IOSurfaceRoot, as find_surface reads it (Sequoia 24H232). */
constexpr size_t kRootTable = 0xd0;      /* IOSurface *[]  */
constexpr size_t kRootCount = 0xd8;      /* u32 entries     */
constexpr size_t kRootLock = 0x138;      /* IOLock *        */
constexpr size_t kSurfaceId = 0x10;      /* u32, in IOSurface */

using TryRetain = bool (*)(const void *);
using GetDescriptor = IOMemoryDescriptor *(*)(const void *);
using GetBytesPerRow = size_t (*)(const void *);

TryRetain tryRetain;
GetDescriptor getDescriptor;
GetBytesPerRow getBytesPerRow;
IOService *gRoot;
uint32_t gFailures;

void solve(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size,
	   const char *symbol, mach_vm_address_t &out)
{
	out = patcher.solveSymbol(index, symbol, address, size);
	if (!out)
		SLOG("%s not found (error %d)", symbol, patcher.getError());

	patcher.clearError();
}

void processSurface(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
	mach_vm_address_t retain = 0, descriptor = 0, bytesPerRow = 0;

	if (index != kextSurface.loadIndex)
		return;

	solve(patcher, index, address, size, "__ZNK9IOSurface9tryRetainEv", retain);
	solve(patcher, index, address, size, "__ZNK9IOSurface19getMemoryDescriptorEv", descriptor);
	solve(patcher, index, address, size, "__ZNK9IOSurface14getBytesPerRowEv", bytesPerRow);

	if (!retain || !descriptor || !bytesPerRow)
		return;

	tryRetain = reinterpret_cast<TryRetain>(retain);
	getDescriptor = reinterpret_cast<GetDescriptor>(descriptor);
	getBytesPerRow = reinterpret_cast<GetBytesPerRow>(bytesPerRow);
	SLOG("IOSurface methods resolved");
}

/* The IOSurfaceRoot service, once. */
IOService *root()
{
	OSDictionary *match;

	if (gRoot)
		return gRoot;

	match = IOService::serviceMatching("IOSurfaceRoot");
	if (!match)
		return nullptr;

	gRoot = IOService::copyMatchingService(match);
	match->release();

	return gRoot;
}

bool isSurface(const OSObject *o)
{
	const OSMetaClass *meta = o->getMetaClass();

	return meta && !strcmp(meta->getClassName(), "IOSurface");
}

/* Surface `id`, retained, or nullptr. */
OSObject *lookup(uint32_t id)
{
	auto *base = reinterpret_cast<uint8_t *>(root());
	OSObject *s = nullptr;
	IOLock *lock;

	if (!base || !tryRetain || getKernelVersion() != KernelVersion::Sequoia)
		return nullptr;

	lock = *reinterpret_cast<IOLock **>(base + kRootLock);
	if (!lock)
		return nullptr;

	IOLockLock(lock);

	if (id && id < *reinterpret_cast<uint32_t *>(base + kRootCount)) {
		auto *candidate = reinterpret_cast<OSObject **>(*reinterpret_cast<uint8_t **>(base + kRootTable))[id];

		if (candidate && isSurface(candidate) &&
		    *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(candidate) + kSurfaceId) == id &&
		    tryRetain(candidate))
			s = candidate;
	}

	IOLockUnlock(lock);

	return s;
}

bool fillRows(IOMemoryDescriptor *md, size_t bytesPerRow, uint32_t width, uint32_t height, uint32_t pixel)
{
	const size_t rowBytes = 4ull * width;
	auto *row = static_cast<uint32_t *>(IOMalloc(rowBytes));
	bool ok = row != nullptr;

	for (uint32_t x = 0; ok && x < width; x++)
		row[x] = pixel;

	for (uint32_t y = 0; ok && y < height; y++)
		ok = md->writeBytes(y * bytesPerRow, row, rowBytes) == rowBytes;

	if (row)
		IOFree(row, rowBytes);

	return ok;
}

} /* namespace */

void PvSurface::start()
{
	lilu.onKextLoadForce(&kextSurface, 1, processSurface, nullptr);
}

bool PvSurface::fill(uint32_t id, uint32_t width, uint32_t height, uint32_t pixel)
{
	OSObject *s = lookup(id);
	IOMemoryDescriptor *md;
	size_t bytesPerRow;
	bool ok = false;

	if (!s) {
		if (gFailures++ < 8)
			SLOG("surface %u not found", id);
		return false;
	}

	md = getDescriptor(s);
	bytesPerRow = getBytesPerRow(s);

	if (md && bytesPerRow >= 4ull * width && md->getLength() >= bytesPerRow * height &&
	    md->prepare() == kIOReturnSuccess) {
		ok = fillRows(md, bytesPerRow, width, height, pixel);
		md->complete();
	}

	s->release();

	return ok;
}
