#pragma once

// Diagnostic-only DSP0 output-path observations. None of these functions reads
// a peripheral or advances emulated time. A JIT callback's cycle is the cycle
// at entry to its block; PC and sequence identify the instruction within it.
#include "dsp56kEmu/dsp.h"
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <initializer_list>

namespace dsp56k
{
	struct G1OutputTrace
	{
		std::atomic<const DSP*> target{nullptr};
		std::ofstream writes;
		std::ofstream links;
		uint64_t sequence = 0;
		uint64_t begin = 0, end = 0;
		bool configured = false;
	};

	inline G1OutputTrace& g1OutputTrace()
	{
		static G1OutputTrace trace;
		return trace;
	}

	inline void g1OutputTraceSetTarget(const DSP* dsp)
	{
		g1OutputTrace().target.store(dsp, std::memory_order_release);
	}

	inline bool g1OutputTraceActive(const DSP& dsp)
	{
		auto& trace = g1OutputTrace();
		if(trace.target.load(std::memory_order_acquire) != &dsp) return false;
		if(!trace.configured)
		{
			trace.configured = true;
			const char* path = std::getenv("G1_DSP_OUTPUT_WRITES_FILE");
			if(!path || !*path) return false;
			const char* begin = std::getenv("G1_DSP_OUTPUT_BEGIN");
			const char* end = std::getenv("G1_DSP_OUTPUT_END");
			trace.begin = begin ? std::strtoull(begin, nullptr, 10) : 0;
			trace.end = end ? std::strtoull(end, nullptr, 10) : 0;
			trace.writes.open(path, std::ios::out | std::ios::trunc);
			if(trace.writes)
			{
				trace.writes << "sequence,origin,block_cycle,block_pc,instruction_pc,opcode,area,address,old,new,source_area,source_address,source_value";
				for(char area : {'x', 'y'})
					for(unsigned address = 0x620; address <= 0x62d; ++address)
						trace.writes << ',' << area << std::hex << address << std::dec;
				trace.writes << ",x6c0,x6c1,x6e0,x6e1\n";
			}
			const char* linkPath = std::getenv("G1_DSP_OUTPUT_LINK_FILE");
			if(linkPath && *linkPath)
			{
				trace.links.open(linkPath, std::ios::out | std::ios::trunc);
				if(trace.links)
					trace.links << "cycle,pc,block,x5,x6,link0,link1,y6c0,y6c1,y6e0,y6e1,x6c0,x6c1,x6e0,x6e1,x620,x621,x622,y620,y621,y622\n";
			}
		}
		const auto cycle = dsp.getCycles();
		return trace.writes && cycle >= trace.begin && cycle <= trace.end;
	}

	inline bool g1OutputCell(TWord address)
	{
		return address == 0x6c0 || address == 0x6c1 ||
			address == 0x6e0 || address == 0x6e1;
	}

	inline void g1OutputTraceWrite(const DSP& dsp, const char* origin,
		TWord instructionPc, EMemArea area, TWord address, TWord value,
		EMemArea sourceArea = MemArea_COUNT, TWord sourceAddress = 0,
		TWord sourceValue = 0)
	{
		if(area != MemArea_Y || !g1OutputCell(address) || !g1OutputTraceActive(dsp)) return;
		auto& trace = g1OutputTrace();
		const auto& mem = dsp.memory();
		const auto blockPc = dsp.getPC().toWord();
		trace.writes << ++trace.sequence << ',' << origin << ',' << dsp.getCycles()
			<< ',' << blockPc << ',' << instructionPc << ','
			<< mem.get(MemArea_P, instructionPc) << ',' << static_cast<unsigned>(area)
			<< ',' << address << ',' << mem.get(area, address) << ',' << (value & 0xffffff)
			<< ',' << static_cast<unsigned>(sourceArea) << ',' << sourceAddress
			<< ',' << sourceValue;
		for(EMemArea areaToRead : {MemArea_X, MemArea_Y})
			for(TWord addressToRead = 0x620; addressToRead <= 0x62d; ++addressToRead)
				trace.writes << ',' << mem.get(areaToRead, addressToRead);
		trace.writes << ',' << mem.get(MemArea_X, 0x6c0) << ',' << mem.get(MemArea_X, 0x6c1)
			<< ',' << mem.get(MemArea_X, 0x6e0) << ',' << mem.get(MemArea_X, 0x6e1)
			<< '\n';
	}

	inline void g1OutputTraceLink(const DSP& dsp, uint64_t block,
		TWord p0, TWord p1, TWord link0, TWord link1)
	{
		if(!g1OutputTraceActive(dsp)) return;
		auto& trace = g1OutputTrace();
		if(!trace.links) return;
		const auto& mem = dsp.memory();
		trace.links << dsp.getCycles() << ',' << dsp.getPC().toWord() << ','
			<< block << ',' << p0 << ',' << p1 << ',' << link0 << ',' << link1;
		for(TWord address : {0x6c0u, 0x6c1u, 0x6e0u, 0x6e1u})
			trace.links << ',' << mem.get(MemArea_Y, address);
		for(TWord address : {0x6c0u, 0x6c1u, 0x6e0u, 0x6e1u,
			0x620u, 0x621u, 0x622u})
			trace.links << ',' << mem.get(MemArea_X, address);
		for(TWord address : {0x620u, 0x621u, 0x622u})
			trace.links << ',' << mem.get(MemArea_Y, address);
		trace.links << '\n';
	}

	struct G1VoiceRegisters
	{
		uint64_t a = 0, b = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
		uint64_t r1 = 0, r3 = 0, r4 = 0, n1 = 0;
	};

	struct G1VoiceTrace
	{
		G1VoiceRegisters regs;
		std::ofstream out;
		std::array<uint32_t, 0x10000> pcOrdinals{};
		uint64_t sequence = 0;
		TWord beforeX = 0;
		bool configured = false;
	};

	inline G1VoiceTrace& g1VoiceTrace()
	{
		static G1VoiceTrace trace;
		return trace;
	}

	inline bool g1VoiceTracePc(TWord pc)
	{
		return (pc >= 0x23e && pc <= 0x244) || pc == 0x652 ||
			(pc >= 0x663 && pc <= 0x672);
	}

	inline void g1VoiceTraceEmit(const DSP& dsp, TWord pc, bool after)
	{
		if(g1OutputTrace().target.load(std::memory_order_acquire) != &dsp ||
			!g1VoiceTracePc(pc)) return;
		auto& trace = g1VoiceTrace();
		if(!trace.configured)
		{
			trace.configured = true;
			const char* path = std::getenv("G1_DSP_VOICE_FILE");
			if(path && *path)
			{
				trace.out.open(path, std::ios::out | std::ios::trunc);
				if(trace.out)
					trace.out << "sequence,phase,pc,opcode,pc_ordinal,block_entry_cycle,block_pc,output_write_sequence,a,b,x0,x1,y0,y1,r1,r3,r4,n1,x_at_r3,y_at_r4,y_at_r1,y_at_r1_plus_n1,x1d,x1e,x1f,x24,x25,x26,x_write_address,x_write_old,x_write_new\n";
			}
		}
		if(!trace.out || dsp.getCycles() < 236970000 || dsp.getCycles() > 237070000)
			return;
		const auto& mem = dsp.memory();
		const auto& regs = trace.regs;
		TWord writeAddress = 0;
		if(pc == 0x652) writeAddress = 0x24;
		if(pc == 0x244) writeAddress = 0x25;
		if(pc == 0x669) writeAddress = 0x26;
		if(!after)
		{
			++trace.pcOrdinals[pc];
			if(writeAddress) trace.beforeX = mem.get(MemArea_X, writeAddress);
		}
		const auto read = [&](EMemArea area, uint64_t address) -> int64_t
		{
			return address < mem.size(area) ? mem.get(area, static_cast<TWord>(address)) : -1;
		};
		trace.out << ++trace.sequence << ',' << (after ? "post" : "pre") << ','
			<< pc << ',' << mem.get(MemArea_P, pc) << ',' << trace.pcOrdinals[pc]
			<< ',' << dsp.getCycles() << ',' << dsp.getPC().toWord()
			<< ',' << g1OutputTrace().sequence
			<< ',' << regs.a << ',' << regs.b << ',' << regs.x0 << ',' << regs.x1
			<< ',' << regs.y0 << ',' << regs.y1 << ',' << regs.r1 << ',' << regs.r3
			<< ',' << regs.r4 << ',' << regs.n1
			<< ',' << read(MemArea_X, regs.r3) << ',' << read(MemArea_Y, regs.r4)
			<< ',' << read(MemArea_Y, regs.r1)
			<< ',' << read(MemArea_Y, (regs.r1 + regs.n1) & 0xffffff)
			<< ',' << mem.get(MemArea_X, 0x1d) << ',' << mem.get(MemArea_X, 0x1e)
			<< ',' << mem.get(MemArea_X, 0x1f)
			<< ',' << mem.get(MemArea_X, 0x24) << ',' << mem.get(MemArea_X, 0x25)
			<< ',' << mem.get(MemArea_X, 0x26)
			<< ',' << (after ? writeAddress : 0)
			<< ',' << (after && writeAddress ? trace.beforeX : 0)
			<< ',' << (after && writeAddress ? mem.get(MemArea_X, writeAddress) : 0)
			<< '\n';
	}
}
