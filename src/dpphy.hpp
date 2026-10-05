//
//  dpphy.hpp
//  RDNA4FB
//
//  What DisplayPort link training (dptrain.hpp) asks of the hardware, for
//  DCN 4.01: the link encoder registers behind a training pattern, and the
//  DMUB commands that turn the transmitter on and off and set the lanes'
//  drive. As amdgpu's link encoder has them (dcn10_link_encoder.c:
//  enc1_configure_encoder, dcn10_link_encoder_dp_set_phy_pattern,
//  link_encoder_disable, and the bp_transmitter_control it fills for
//  transmitter_control_v1_7).
//
//  tools/atomdump.cpp holds the register steps against what Linux writes
//  while it trains (tools/dp_train_linux.inc, kTrainRegs_*).
//
//  Freestanding; shared by the kext and the host test.
//

#ifndef DpPhy_hpp
#define DpPhy_hpp

#include <stddef.h>
#include <stdint.h>

#include "dmub.hpp"
#include "dptrain.hpp"
#include "modeset.hpp"

namespace DpPhy {

constexpr size_t kMaxSteps = 6;

// Each writes its steps (DMU segment 2 updates and writes) to `out`, which
// has room for kMaxSteps, and returns how many. `link` is the link encoder
// (0 = UNIPHY A).

// Before the transmitter is enabled: the lane count, and the scrambler
// reset every 512th symbol.
size_t configureSteps(uint8_t link, uint8_t lanes, ModeSet::Step *out);
// A training pattern on the main link, or (Video) what the stream encoder
// sends, with the link marked trained.
size_t patternSteps(uint8_t link, DpTrain::Pattern p, ModeSet::Step *out);
// After the transmitter is disabled.
size_t offSteps(uint8_t link, ModeSet::Step *out);

// The transmitter of `link`, whose connector's hot-plug pin is `hpd`
// (counting from 1). The symbol clock goes in units of 10 kHz.
void buildEnable(Dmub::Cmd cmd, uint8_t link, uint8_t hpd, const DpTrain::Link &l);
// The lanes' voltage swing and pre-emphasis (levels 0..3). The command has
// no lane number: amdgpu sends it once per lane, each time with that lane's
// levels, which training keeps the same for all.
void buildDrive(Dmub::Cmd cmd, uint8_t link, uint8_t hpd, uint8_t connectorObjId, const DpTrain::Link &l,
                uint8_t swing, uint8_t preEmphasis);
void buildDisable(Dmub::Cmd cmd, uint8_t link, uint8_t hpd, uint8_t connectorObjId);

constexpr uint8_t kActionSetDrive = 11;   // TRANSMITTER_CONTROL_SET_VOLTAGE_AND_PREEMPASIS

} // namespace DpPhy

#endif /* DpPhy_hpp */
