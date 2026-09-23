#pragma once

// Diagnostic-only view of the ESSI clock producer. The caller supplies its
// private clock and slot state; this helper never advances or reads RX data.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>

namespace dsp56k
{
	inline void g1TraceEssiClock(const char* stage, IPeripherals& peripherals,
		const uint64_t clockCount, const uint64_t lastClock, const uint32_t period,
		const uint32_t clockSource, const int entry, const uintptr_t esxi,
		const int rxCounter, const int rxDivider, const int rxCount)
	{
		const char* path = std::getenv("G1_DSP_ESSI_TRACE_FILE");
		const char* beginText = std::getenv("G1_DSP_DMA_TRACE_BEGIN");
		const char* endText = std::getenv("G1_DSP_DMA_TRACE_END");
		if(!path || !*path || !beginText || !endText)
			return;
		auto& dsp = peripherals.getDSP();
		const auto cycles = dsp.getCycles();
		if(cycles < std::strtoull(beginText, nullptr, 10) ||
			cycles > std::strtoull(endText, nullptr, 10))
			return;
		static std::mutex mutex;
		std::lock_guard<std::mutex> lock(mutex);
		static std::ofstream out(path, std::ios::out | std::ios::trunc);
		static bool header = false;
		static uint64_t sequence = 0;
		if(!out)
			return;
		if(!header)
		{
			out << "sequence,stage,dsp,cycle,pc,clock_count,last_clock,clock_lag,period,"
				"clock_source,entry,esxi,is_essi1,rx_counter,rx_divider,rx_count,essi1_sr\n";
			header = true;
		}
		auto& essi1 = static_cast<Peripherals56303&>(peripherals).getEssi1();
		const auto sr = static_cast<uint32_t>(essi1.getSR());
		out << ++sequence << ',' << stage << ',' << reinterpret_cast<uintptr_t>(&dsp)
			<< ',' << cycles << ',' << dsp.getPC().toWord() << ',' << clockCount << ','
			<< lastClock << ',' << (clockCount - lastClock) << ',' << period << ','
			<< clockSource << ',' << entry << ',' << esxi << ','
			<< (esxi == reinterpret_cast<uintptr_t>(static_cast<Esxi*>(&essi1)))
			<< ',' << rxCounter << ','
			<< rxDivider << ',' << rxCount << ',' << sr << '\n';
	}
}
