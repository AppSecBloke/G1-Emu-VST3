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
		const int rxCounter, const int rxDivider, const int rxCount,
		const uint32_t finePeriod = 0, const uint64_t fineLastClock = 0,
		const bool hasFine = false, const uint32_t nextDeadline = 0)
	{
		// The runner starts a new process for each case. Resolve its trace settings
		// once: this hook is reached on every peripheral clock poll, including Saw.
		static const char* path = std::getenv("G1_DSP_ESSI_TRACE_FILE");
		static const char* beginText = std::getenv("G1_DSP_DMA_TRACE_BEGIN");
		static const char* endText = std::getenv("G1_DSP_DMA_TRACE_END");
		if(!path || !*path || !beginText || !endText)
			return;
		auto& dsp = peripherals.getDSP();
		const auto cycles = dsp.getCycles();
		static const auto begin = std::strtoull(beginText, nullptr, 10);
		static const auto end = std::strtoull(endText, nullptr, 10);
		if(cycles < begin || cycles > end)
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
				"clock_source,entry,esxi,is_essi1,rx_counter,rx_divider,rx_count,"
				"fine_period,fine_last_clock,fine_lag,has_fine,next_deadline,essi1_sr\n";
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
			<< rxDivider << ',' << rxCount << ',' << finePeriod << ',' << fineLastClock
			<< ',' << (finePeriod ? clockCount - fineLastClock : 0) << ','
			<< hasFine << ',' << nextDeadline << ',' << sr << '\n';
	}
}
