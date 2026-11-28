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
#include <sys/sysctl.h>

#define TLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: trial: " fmt "\n", ##__VA_ARGS__)

namespace
{

/* Lilu's vendor GUID, as compute.cpp's breadcrumbs use it. */
const char *kKey = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial";
const char *kSettingsKey = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-settings";
const char *kGuardKey = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-accel-guard";   // (rdna4accel.cpp's)
const char *kForceSync = "IONVRAM-FORCESYNCNOW-PROPERTY";

bool gLoaded;
bool gArmed;
char gText[Trial::kMaxText];
bool gSettingsOn;
char gSettings[Trial::kMaxText];

/* Variable `key`'s text into `text` (`size` bytes), empty if there is none. */
void readVariable(IORegistryEntry *nvram, const char *key = kKey, char *text = gText, size_t size = sizeof(gText))
{
	OSObject *v = nvram->copyProperty(key);
	size_t n;

	text[0] = 0;

	if (!v)
		return;

	if (auto *s = OSDynamicCast(OSString, v)) {
		strlcpy(text, s->getCStringNoCopy(), size);
	} else if (auto *d = OSDynamicCast(OSData, v)) {
		n = d->getLength();
		if (n > size - 1)
			n = size - 1;

		memcpy(text, d->getBytesNoCopy(), n);
		text[n] = 0;
	}

	v->release();
}

/* Write `text` to the variable and commit it now. */
void writeVariable(IORegistryEntry *nvram, const char *text, const char *name = kKey)
{
	const OSSymbol *key = OSSymbol::withCString(name);
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

// debug.rdna4_settings: the rdna4-settings variable; written (by root, as every sysctl write), it is committed to NVRAM at once (the RDNA4
// app's way to its settings, and the test rig's: no nvram without a password there). Taken at the next boot.
int settingsSysctl SYSCTL_HANDLER_ARGS
{
	IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!nvram)
		return ENOENT;
	char value[Trial::kMaxText];
	readVariable(nvram, kSettingsKey, value, sizeof(value));
	int err = sysctl_handle_string(oidp, value, sizeof(value), req);
	if (!err && req->newptr) {
		for (const char *c = value; *c && !err; c++)   // words of letters, digits, '-', '=', and spaces
			if (!(*c == ' ' || *c == '-' || *c == '=' || (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')))
				err = EINVAL;
		if (!err)
			writeVariable(nvram, value, kSettingsKey);
	}
	nvram->release();
	return err;
}

SYSCTL_PROC(_debug, OID_AUTO, rdna4_settings, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, settingsSysctl, "A",
            "AMDRadeonRDNA4: the persistent settings (\"on rdna4-pvcursor=0 ...\", trial.hpp), taken at the next boot");

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

	sysctl_register_oid(&sysctl__debug_rdna4_settings);   // (load() reaches here once)
	readVariable(nvram, kSettingsKey, gSettings, sizeof(gSettings));
	gSettingsOn = Trial::firstWordIs(gSettings, Trial::kOn);
	if (gSettingsOn) {
		char guard[32] = "";
		readVariable(nvram, kGuardKey, guard, sizeof(guard));
		if (!strcmp(guard, "pending")) {
			char off[Trial::kMaxText + 8];
			const char *rest = gSettings;
			while (*rest == ' ')
				rest++;
			snprintf(off, sizeof(off), "off%s", rest + sizeof(Trial::kOn) - 1);
			writeVariable(nvram, off, kSettingsKey);
			gSettingsOn = false;
			TLOG("settings turned off: the boot before did not last (%s)", gSettings);
		} else {
			TLOG("settings: %s", gSettings);
		}
	}

	nvram->release();
}

bool setting(const char *key, uint32_t &value)
{
	return gSettingsOn && Trial::settingAllowed(key) && Trial::find(gSettings, key, value);
}

} /* namespace */

bool Trial::loaded()
{
	load();

	return gLoaded;
}

/*
 * The driver's defaults (2026-11-21): what the rig's accelerated desktop boots with, so an install needs no boot-args for them; the debug
 * ones (rdna4-trace, the rdna4-gfxclient self-test) stay off. A boot-arg or a test's value wins (=0 turns one off). Also defaulted outside
 * this table: rdna4-client-policy=local (userclient.cpp), rdna4-nativeheld=1 (the Metal bundle), rdna4-accel=4 for rdna4-iop. A VM test
 * build has no card: none.
 */
static bool builtIn(const char *key, uint32_t &value)
{
	static const struct { const char *key; uint32_t value; } kDefaults[] = {
#ifndef AMDRadeonRDNA4_VM_TEST
		{"rdna4-compute", 7}, {"rdna4-gfx", 2}, {"rdna4-vm", 1}, {"rdna4-vmshared", 2}, {"rdna4-accel", 4},
		{"rdna4-dmubload", 1}, {"rdna4-dcninit", 1}, {"rdna4-head2dp", 1},
		{"rdna4-pvgpu-disp", 7}, {"rdna4-pvgpu-class", 0xff0000}, {"rdna4-pvgpu-delay", 5}, {"rdna4-pvfast", 1}, {"rdna4-nativews", 3},
		{"rdna4-accelgl", 1},
		{"rdna4-vrr", 1}, {"rdna4-hdr", 1}, {"rdna4-audio", 1}, {"rdna4-gfxoff", 1},   // (2026-11-28)
#endif
		{nullptr, 0},
	};
	for (const auto &d : kDefaults)
		if (d.key && !strcmp(d.key, key)) {
			value = d.value;
			return true;
		}
	return false;
}

bool Trial::bootArg(const char *key, uint32_t &value)
{
	return PE_parse_boot_argn(key, &value, sizeof(value)) || (gLoaded && setting(key, value)) || builtIn(key, value);
}

bool Trial::option(const char *key, uint32_t &value)
{
	if (PE_parse_boot_argn(key, &value, sizeof(value)))
		return true;

	load();

	return (gArmed && find(gText, key, value)) || setting(key, value) || builtIn(key, value);
}
