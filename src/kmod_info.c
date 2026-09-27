//
//  kmod_info.c
//  RDNA4FB
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
//  ADDPR(kern_stop), i.e. RDNA4FB_kern_start / RDNA4FB_kern_stop) — what
//  Xcode's MODULE_START / MODULE_STOP settings point at for other plugins.
//

#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

// Provided by libkmodc++ (cplus_start.c / cplus_stop.c): runs constructors,
// then calls _realmain, and destructors after _antimain.
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

extern kern_return_t RDNA4FB_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t RDNA4FB_kern_stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(com.hackintosh.RDNA4FB, "0.0.1", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = RDNA4FB_kern_start;
__private_extern__ kmod_stop_func_t  *_antimain = RDNA4FB_kern_stop;
