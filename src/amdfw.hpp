//
//  amdfw.hpp
//  RDNA4FB
//
//  Freestanding parser for AMD's firmware containers (the linux-firmware
//  amdgpu/*.bin files): the common header every blob starts with, the
//  payload a LOAD_IP_FW hands the PSP, and the PSP secure-OS package
//  (header v2), which bundles the bootloader-stage components (KDB, SPL,
//  SYS/SOC/INTF/DBG/RAS/IPKEYMGR drivers, the sOS itself and the TOC).
//
//  No IOKit, no allocation: views into the caller's buffer, bounds-checked.
//  Layouts follow amdgpu_ucode.h (hardware/firmware ABI facts).
//

#ifndef AmdFw_hpp
#define AmdFw_hpp

#include <stddef.h>
#include <stdint.h>

namespace AmdFw {

struct Blob {
	const uint8_t *data;
	uint32_t       size;
	bool valid() const { return data && size; }
};

// common_firmware_header, the first 32 bytes of every blob.
struct Common {
	uint32_t sizeBytes;
	uint32_t headerSizeBytes;
	uint16_t headerMajor, headerMinor;
	uint16_t ipMajor, ipMinor;
	uint32_t ucodeVersion;
	uint32_t ucodeSizeBytes;
	uint32_t ucodeArrayOffsetBytes;
	uint32_t crc32;
};

bool parseCommon(const uint8_t *data, size_t size, Common &out);

// The ucode a LOAD_IP_FW carries for single-image blobs (SMU, ...): the
// bytes at ucode_array_offset, ucode_size long.
bool payload(const uint8_t *data, size_t size, Blob &out);

// psp_fw_type: the component kinds inside a PSP package.
enum PspPart : uint32_t {
	PspSos = 1, PspSysDrv, PspKdb, PspToc, PspSpl, PspRl, PspSocDrv, PspIntfDrv,
	PspDbgDrv, PspRasDrv, PspIpKeyMgrDrv, PspSpdmDrv,
	PspPartCount,
};

const char *pspPartName(uint32_t part);

struct PspPackage {
	Blob     part[PspPartCount];
	uint32_t version[PspPartCount];
	uint32_t count;           // descriptors in the package
};

// psp_firmware_header_v2_0 / v2_1 (a v2_1 package's auxiliary images, used
// only by some boards, are skipped as amdgpu does by default).
bool parsePsp(const uint8_t *data, size_t size, PspPackage &out);

} // namespace AmdFw

#endif /* AmdFw_hpp */
