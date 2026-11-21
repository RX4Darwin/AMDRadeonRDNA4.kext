//
//  translator.hpp
//  AMDRadeonRDNA4
//
//  The Metal translator helper (include/rdna4mtlc.h; userspace root helper, AIR -> GFX12 ELF): its one request slot and its user client.
//  Shared by the paravirtual host (pvgpu.cpp: the guest's functions) and our own accelerator's Metal device (userclient.cpp
//  kRDNA4MethodTranslate: WindowServer's sandbox does not let it start the translator itself; docs/native-metal-spike.md).
//

#ifndef RDNA4Translator_hpp
#define RDNA4Translator_hpp

#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>

// The one request slot between the requesters and the helper. A requester posts a function (id != 0) and waits on &done; the helper's
// kMtlcNext waits on &id, copies the metallib out and sets taken; kMtlcDeliver fills elf and sets done.
struct Translation {
	IOLock *lock;                 // allocated with the host or our accelerator (translatorInit)
	bool helper;                  // a helper is connected
	bool gaveUp;                  // a requester waited for one in vain: none waits again until one connects
	uint32_t helpers;             // helpers connected so far: failures cached under an older one are asked again
	uint32_t seq, id;
	bool taken, done;
	const uint8_t *lib;
	uint32_t libLen;
	char name[64];
	uint8_t *elf;                 // IOMalloc'd by kMtlcDeliver, freed by the requester
	uint32_t elfLen, buffers, written, texturesWritten;
};
extern Translation gTr;

// The slot made (once): by the host and by our own accelerator (rdna4accel.cpp), whichever starts first.
void translatorInit();
// With gTr.lock held: until no request is in the slot (the host's and the runtime clients' requests take turns).
void waitForSlot();
// Whether a helper is connected, waiting up to 20 s for one: WindowServer draws its first frames before launchd has started the helper,
// and a draw left out is not drawn again until its region changes. Waits once until a helper connects; while the helper restarts (it
// exits when lld cannot run again) the same.
bool waitForHelper();
// The helper's user client, handed out by RDNA4ComputeService::newUserClient (n48nkext.cpp): one helper at a time, root only.
IOReturn translatorClient(IOService *service, task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler);
// A runtime client's function translated: `lib` (a metallib or AIR bitcode, the caller's) and `name` in the slot, after any request before
// it; the result IOMalloc'd into *out (the caller frees it), or an error.
IOReturn translateForClient(const uint8_t *lib, uint32_t libLen, const char *name, uint8_t *&out, uint32_t &outLen, uint32_t &buffers);

#endif /* RDNA4Translator_hpp */
