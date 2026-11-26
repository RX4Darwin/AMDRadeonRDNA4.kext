#pragma once

#include <stdint.h>

// Owned by one RtClient and updated under rtLock. Time is monotonic.
// A summary counts all repeats since the previous report, including its triggering refusal.
struct HostRefusals {
	struct Window {
		uint64_t nextReportNs { 0 };
		uint64_t pending { 0 };

		bool record(uint64_t nowNs, uint64_t &repeats) {
			if (nextReportNs)
				++pending;
			if (nowNs < nextReportNs)
				return false;
			repeats = pending;
			pending = 0;
			nextReportNs = nowNs + 1000000000ull;
			return true;
		}
	};

	Window bufferSize, clientBytes, totalBytes, tableSlots, clientSlots, dmaMapping;
};
