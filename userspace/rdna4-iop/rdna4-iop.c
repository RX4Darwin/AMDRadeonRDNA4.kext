/*
 *  rdna4-iop.c
 *  AMDRadeonRDNA4
 *
 *  Switches CoreDisplay's IOPresentment for the boot, by what the kext
 *  decided (debug.rdna4_pvheads, src/pvscanout.hpp). With the heads as the
 *  paravirtual displays WindowServer must present through their display
 *  pipes, which only IOPresentment drives; in any other boot IOPresentment
 *  finds no display pipe for our framebuffers and leaves no display online
 *  (docs/m0-pvgpu.md). The switch is the preference useIOP of
 *  com.apple.CoreDisplay (any user, current host), which WindowServer reads
 *  when it starts: when this boot needs the other value, it is written and
 *  WindowServer restarted.
 *
 *    rdna4-iop [-n]       -n: say what would change, change nothing
 *
 *  Needs root; runs once at boot as a LaunchDaemon (userspace/rdna4-iop/install.sh).
 */

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define WAIT_SECONDS 90

static const CFStringRef kDomain = CFSTR("com.apple.CoreDisplay");
static const CFStringRef kKey = CFSTR("useIOP");

/* The kext's decision: 1 or 0, or 0 when it does not tell within the wait (no such kext). */
static int pvHeads(void)
{
	for (int i = 0; i < WAIT_SECONDS; i++) {
		int v = -1;
		size_t len = sizeof(v);

		if (!sysctlbyname("debug.rdna4_pvheads", &v, &len, NULL, 0) && v >= 0)
			return v;

		sleep(1);
	}

	return 0;
}

/* useIOP now: 1 true, 0 false, -1 not set. */
static int current(void)
{
	CFTypeRef v = CFPreferencesCopyValue(kKey, kDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
	int r = v == kCFBooleanTrue ? 1 : v == kCFBooleanFalse ? 0 : -1;

	if (v)
		CFRelease(v);

	return r;
}

/* The user logged in on the console, or 0 at the login window (the console then belongs to root). */
static uid_t consoleUser(void)
{
	struct stat st;

	return stat("/dev/console", &st) ? 0 : st.st_uid;
}

int main(int argc, char **argv)
{
	const int dryRun = argc > 1 && !strcmp(argv[1], "-n");
	const int heads = pvHeads();
	const int now = current();

	/* the paravirtual displays need it on; every other boot the default (unset) */
	if ((heads && now == 1) || (!heads && now == -1)) {
		printf("rdna4-iop: useIOP %s, as this boot needs\n", heads ? "on" : "unset");
		return 0;
	}

	printf("rdna4-iop: the heads are%s the paravirtual displays: useIOP %s -> %s%s\n", heads ? "" : " not",
	       now == 1 ? "on" : now == 0 ? "off" : "unset", heads ? "on" : "unset", dryRun ? " (not changed: -n)" : "");
	if (dryRun)
		return 0;

	CFPreferencesSetValue(kKey, heads ? kCFBooleanTrue : NULL, kDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
	if (!CFPreferencesSynchronize(kDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost)) {
		fprintf(stderr, "rdna4-iop: the preference could not be written\n");
		return 1;
	}

	/* WindowServer read the other value when it started. Restarting it ends a logged-in user's session: then only
	 * the next start (a logout, a reboot) takes the new value. */
	if (consoleUser()) {
		fprintf(stderr, "rdna4-iop: NOT restarting WindowServer: user %u is logged in; useIOP takes effect at the next "
		        "logout or reboot\n", (unsigned)consoleUser());
		return 1;
	}

	return system("/usr/bin/killall -9 WindowServer") ? 1 : 0;
}
