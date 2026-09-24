#pragma once

// Included only by the diagnostic dsp56300 build overlay. Observe DMA3's
// ESSI1 receive path without reading peripheral data or changing execution.
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <mutex>
#include "g1_essi_trace.h"

namespace dsp56k
{
	inline void g1TraceDma3(const char* stage, IPeripherals& peripherals,
		const TWord index, const TWord dcr, const TWord dsr, const TWord ddr,
		const TWord dco, const TWord dstr, const int injectResult = -1)
	{
		g1CallbackWindowDma(stage, peripherals, index, dcr, dsr, ddr, dco,
			dstr, injectResult);
		if(index != 3 || dsr != 0xffffa8 || ddr != 0x6c5)
			return;
		const char* path = std::getenv("G1_DSP_DMA_TRACE_FILE");
		const char* beginText = std::getenv("G1_DSP_DMA_TRACE_BEGIN");
		const char* endText = std::getenv("G1_DSP_DMA_TRACE_END");
		if(!path || !*path || !beginText || !endText)
			return;
		auto& dsp = peripherals.getDSP();
		const auto cycles = dsp.getCycles();
		const auto begin = std::strtoull(beginText, nullptr, 10);
		const auto end = std::strtoull(endText, nullptr, 10);
		if(cycles < begin || cycles > end)
			return;
		auto& p = static_cast<Peripherals56303&>(peripherals);
		const auto essi0 = static_cast<uint32_t>(p.getEssi0().getSR());
		const auto essi1 = static_cast<uint32_t>(p.getEssi1().getSR());
		static std::mutex mutex;
		std::lock_guard<std::mutex> lock(mutex);
		static std::ofstream out(path, std::ios::out | std::ios::trunc);
		static bool header = false;
		if(!out)
			return;
		if(!header)
		{
			out << "stage,dsp,cycle,pc,pending,essi0_sr,essi1_sr,essi1_rdf,"
				"dma3_dcr,dma3_dsr,dma3_ddr,dma3_dco,dma_dstr,request_source,"
				"interrupt_enable,inject_result\n";
			header = true;
		}
		out << stage << ',' << reinterpret_cast<uintptr_t>(&dsp) << ',' << cycles
			<< ',' << dsp.getPC().toWord() << ','
			<< dsp.hasPendingInterrupts() << ','
			<< essi0 << ',' << essi1 << ',' << ((essi1 >> 7) & 1) << ','
			<< dcr << ',' << dsr << ',' << ddr << ',' << dco << ',' << dstr << ','
			<< ((dcr >> 11) & 31) << ',' << ((dcr >> 22) & 1)
			<< ',' << injectResult << '\n';
	}
}
