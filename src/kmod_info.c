//
//  kmod_info.c
//  AMDRadeonRDNA4
//
//  Hand-written replacement for the kmod_info glue that Xcode's kext build
//  normally generates from CreateKModInfo.perl.
//
//  Link order matters: <objects> -lkmodc++ kmod_info.o -lkmod
//    * libkmodc++ provides _start/_stop that run C++ static constructors and
//      then jump to _realmain / _antimain.
//    * this file defines _realmain / _antimain and the kmod_info struct.
//
//  The kext is a Lilu plugin: _realmain / _antimain are Lilu's plugin entry
//  points from Lilu/Library/plugin_start.cpp (ADDPR(kern_start) /
//  ADDPR(kern_stop), i.e. AMDRadeonRDNA4_kern_start / AMDRadeonRDNA4_kern_stop) — what
//  Xcode's MODULE_START / MODULE_STOP settings point at for other plugins.
//

#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

// Provided by libkmodc++ (cplus_start.c / cplus_stop.c): runs constructors,
// then calls _realmain, and destructors after _antimain.
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

extern kern_return_t AMDRadeonRDNA4_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t AMDRadeonRDNA4_kern_stop(kmod_info_t *ki, void *data);

// The version is the Makefile's (-DMODULE_VERSION), the same that goes into Info.plist: the two have to agree for the kext to load.
#define VERSION_TEXT2(v) #v
#define VERSION_TEXT(v) VERSION_TEXT2(v)
KMOD_EXPLICIT_DECL(com.hackintosh.AMDRadeonRDNA4, VERSION_TEXT(MODULE_VERSION), _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = AMDRadeonRDNA4_kern_start;
__private_extern__ kmod_stop_func_t  *_antimain = AMDRadeonRDNA4_kern_stop;
