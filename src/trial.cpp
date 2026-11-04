/*
 * trial.cpp
 * AMDRadeonRDNA4
 *
 * See trial.hpp. Only IORegistryEntry's own virtuals are called on the NVRAM
 * entry, as in compute.cpp's breadcrumbs: IODTNVRAM's moved between releases.
 */

#include "trial.hpp"
#include "klog.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

#define TLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: trial: " fmt "\n", ##__VA_ARGS__)

namespace
{

/* Lilu's vendor GUID, as compute.cpp's breadcrumbs use it. */
const char *kKey = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial";
const char *kForceSync = "IONVRAM-FORCESYNCNOW-PROPERTY";

bool gLoaded;
bool gArmed;
char gText[Trial::kMaxText];

/* The variable's text into gText, empty if there is none. */
void readVariable(IORegistryEntry *nvram)
{
	OSObject *v = nvram->copyProperty(kKey);
	size_t n;

	gText[0] = 0;

	if (!v)
		return;

	if (auto *s = OSDynamicCast(OSString, v)) {
		strlcpy(gText, s->getCStringNoCopy(), sizeof(gText));
	} else if (auto *d = OSDynamicCast(OSData, v)) {
		n = d->getLength();
		if (n > sizeof(gText) - 1)
			n = sizeof(gText) - 1;

		memcpy(gText, d->getBytesNoCopy(), n);
		gText[n] = 0;
	}

	v->release();
}

/* Write `text` to the variable and commit it now. */
void writeVariable(IORegistryEntry *nvram, const char *text)
{
	const OSSymbol *key = OSSymbol::withCString(kKey);
	OSString *value = OSString::withCString(text);
	OSDictionary *sync = OSDictionary::withCapacity(1);
	OSString *yes = OSString::withCString("1");

	if (key && value)
		nvram->setProperty(key, value);

	if (sync && yes && sync->setObject(kForceSync, yes))
		nvram->setProperties(sync);

	OSSafeReleaseNULL(yes);
	OSSafeReleaseNULL(sync);
	OSSafeReleaseNULL(value);
	OSSafeReleaseNULL(key);
}

/*
 * Consumed before any option acts: a boot that does not come back runs
 * without them the next time.
 */
void markUsed(IORegistryEntry *nvram)
{
	char used[Trial::kMaxText + 8];

	snprintf(used, sizeof(used), "used%s",
		 gText + sizeof(Trial::kArmed) - 1);
	writeVariable(nvram, used);
}

void load()
{
	IORegistryEntry *nvram;

	if (gLoaded)
		return;

	nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!nvram) {
		TLOG("NVRAM not there yet: no test options this time");
		return;
	}

	gLoaded = true;
	readVariable(nvram);
	gArmed = Trial::armed(gText);

	if (gArmed) {
		markUsed(nvram);
		TLOG("armed for this boot: %s (now marked used)", gText);
	} else {
		TLOG("none armed (%s)", gText[0] ? gText : "no variable");
	}

	nvram->release();
}

} /* namespace */

bool Trial::option(const char *key, uint32_t &value)
{
	if (PE_parse_boot_argn(key, &value, sizeof(value)))
		return true;

	load();

	return gArmed && find(gText, key, value);
}
