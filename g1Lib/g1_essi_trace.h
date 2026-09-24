#pragma once

// Diagnostic-only view of the ESSI clock producer. The caller supplies its
// private clock and slot state; this helper never advances or reads RX data.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>

namespace dsp56k
{
	struct G1EssiTimeline
	{
		struct Block { uint64_t before, after; uint32_t prePc, postPc; };
		std::atomic<uintptr_t> target{0};
		std::mutex mutex;
		std::ofstream events, blocks;
		std::array<Block, 4> recent{};
		uint32_t recentCount = 0, recentNext = 0;
		uint64_t begin = 0, end = 0, lastEntry = 0, lastSample = 0;
		uint64_t lastFineClock = 0, triggerCycle = 0, scheduledAt = 0;
		uint64_t lastBaseClock = 0, clockCalls = 0;
		uint32_t finePeriod = 0, fineReceives = 0, baseServices = 0;
		uint32_t lastDeadline = 0;
		uint32_t baseThreshold = 0, fineThreshold = 0, afterTrigger = 0, maxCatchup = 0;
		uint32_t hostWord195Count = 0;
		uint32_t previousPeriod = 0, previousSource = 0, scheduledDelay = 0;
		bool previousFine = false, configured = false, previousOverdue = false;
		bool opened = false;
	};

	inline G1EssiTimeline& g1EssiTimeline()
	{
		static G1EssiTimeline state;
		return state;
	}

	inline void g1SetEssiTimelineTarget(const uintptr_t dsp)
	{
		g1EssiTimeline().target.store(dsp, std::memory_order_release);
	}

	inline void g1TraceEssiBlock(const uintptr_t dsp, const uint64_t before,
		const uint64_t after, const uint32_t prePc, const uint32_t postPc)
	{
		auto& state = g1EssiTimeline();
		static const bool enabled = std::getenv("G1_DSP_ESSI_TIMELINE_FILE") != nullptr;
		static const char* beginText = std::getenv("G1_DSP_ESSI_TIMELINE_BEGIN");
		static const char* endText = std::getenv("G1_DSP_ESSI_TIMELINE_END");
		static const uint64_t begin = beginText ? std::strtoull(beginText, nullptr, 10) : 0;
		static const uint64_t end = endText ? std::strtoull(endText, nullptr, 10) : 0;
		if(!enabled || !beginText || !endText ||
			state.target.load(std::memory_order_acquire) != dsp || before < begin || before > end)
			return;
		std::lock_guard<std::mutex> lock(state.mutex);
		if(!state.opened) return;
		const G1EssiTimeline::Block block{before, after, prePc, postPc};
		if(state.triggerCycle && after >= state.triggerCycle)
		{
			if(state.afterTrigger == 0)
				for(uint32_t i = 0; i < state.recentCount; ++i)
				{
					const auto& prior = state.recent[(state.recentNext + 4 - state.recentCount + i) % 4];
					state.blocks << state.triggerCycle << ",prior," << prior.before << ','
						<< prior.after << ',' << prior.prePc << ',' << prior.postPc << '\n';
				}
			state.blocks << state.triggerCycle << ",current," << before << ',' << after
				<< ',' << prePc << ',' << postPc << '\n';
			if(++state.afterTrigger == 5)
			{
				state.triggerCycle = 0;
				state.afterTrigger = 0;
			}
		}
		state.recent[state.recentNext] = block;
		state.recentNext = (state.recentNext + 1) % 4;
		if(state.recentCount < 4) ++state.recentCount;
	}

	inline uint32_t g1LagThreshold(const uint64_t lag)
	{
		for(const uint32_t threshold : {192u, 768u, 3072u, 12288u, 49152u, 196608u, 786432u, 3145728u})
			if(lag < threshold) return threshold / 4;
		return 3145728;
	}

	inline void g1TraceEssiHostWord195(const uintptr_t dsp, const uint64_t cycle,
		const uint32_t pc)
	{
		auto& state = g1EssiTimeline();
		if(state.target.load(std::memory_order_acquire) != dsp) return;
		std::lock_guard<std::mutex> lock(state.mutex);
		if(!state.opened || cycle < state.begin || cycle > state.end) return;
		state.events << "host_word_195," << cycle << ',' << pc << ',' << ++state.hostWord195Count;
		for(unsigned i = 0; i < 16; ++i) state.events << ',';
		state.events << '\n';
	}

	struct G1DispatchSnapshot
	{
		uint64_t cycle = 0, instructions = 0, targetClock = 0, baseLag = 0,
			fineLag = 0, clockCalls = 0, serviced = 0;
		uint32_t pc = 0, mode = 0, deadline = 0, scheduledDelay = 0,
			lastVector = 0;
		bool pending = false, external = false, periphSelected = false, periphDue = false;
	};

	inline bool g1DispatchTraceActive(const uint64_t cycle)
	{
		static const char* path = std::getenv("G1_DSP_DISPATCH_TRACE_FILE");
		static const char* beginText = std::getenv("G1_DSP_DISPATCH_TRACE_BEGIN");
		static const char* endText = std::getenv("G1_DSP_DISPATCH_TRACE_END");
		if(!path || !*path || !beginText || !endText) return false;
		static const uint64_t begin = std::strtoull(beginText, nullptr, 10);
		static const uint64_t end = std::strtoull(endText, nullptr, 10);
		return cycle >= begin && cycle <= end;
	}

	inline G1DispatchSnapshot g1CaptureDispatch(DSP& dsp, const uint64_t serviced,
		const uint32_t lastVector)
	{
		G1DispatchSnapshot s;
		s.cycle = dsp.getCycles();
		s.instructions = dsp.getInstructionCounter();
		s.pc = dsp.getPC().toWord();
		s.mode = static_cast<uint32_t>(dsp.getProcessingMode());
		s.pending = dsp.hasPendingInterrupts();
		s.external = dsp.hasPendingExternalInterrupts();
		s.periphSelected = dsp.getInterruptFunc() == dsp.getExecPeripheralsFunc();
		s.periphDue = dsp.getPeriph(0)->isDue(s.instructions, s.cycle);
		s.targetClock = dsp.getPeriph(0)->getTargetClock();
		s.serviced = serviced;
		s.lastVector = lastVector;
		auto& state = g1EssiTimeline();
		std::lock_guard<std::mutex> lock(state.mutex);
		s.baseLag = state.lastBaseClock && s.cycle >= state.lastBaseClock ?
			s.cycle - state.lastBaseClock : 0;
		s.fineLag = state.lastFineClock && s.cycle >= state.lastFineClock ?
			s.cycle - state.lastFineClock : 0;
		s.deadline = state.lastDeadline;
		s.scheduledDelay = state.scheduledDelay;
		s.clockCalls = state.clockCalls;
		return s;
	}

	inline void g1TraceIrqdEvent(const char* event, DSP& dsp,
		const uint64_t previousDeadline, const uint64_t nextDeadline,
		const uint64_t previousCount, const uint64_t nextCount,
		const bool pendingBefore, const bool pendingAfter)
	{
		static const char* path = std::getenv("G1_DSP_IRQD_TRACE_FILE");
		if(!path || !*path || !g1DispatchTraceActive(dsp.getCycles())) return;
		static std::ofstream out(path, std::ios::out | std::ios::trunc);
		static bool header = false;
		if(!out) return;
		if(!header)
		{
			out << "event,cycle,pc,mode,previous_deadline,next_deadline,previous_count,next_count,pending_before,pending_after,external_pending,peripheral_due,peripheral_target_clock\n";
			header = true;
		}
		out << event << ',' << dsp.getCycles() << ',' << dsp.getPC().toWord() << ','
			<< static_cast<uint32_t>(dsp.getProcessingMode()) << ','
			<< previousDeadline << ',' << nextDeadline << ','
			<< previousCount << ',' << nextCount << ','
			<< pendingBefore << ',' << pendingAfter << ','
			<< dsp.hasPendingExternalInterrupts() << ','
			<< dsp.getPeriph(0)->isDue(dsp.getInstructionCounter(), dsp.getCycles()) << ','
			<< dsp.getPeriph(0)->getTargetClock() << '\n';
	}

	inline void g1TraceDispatchStep(const G1DispatchSnapshot& before,
		const G1DispatchSnapshot& after, const uint32_t cause)
	{
		static const char* path = std::getenv("G1_DSP_DISPATCH_TRACE_FILE");
		if(!path || !*path) return;
		static std::ofstream out(path, std::ios::out | std::ios::trunc);
		static bool header = false;
		if(!out) return;
		if(!header)
		{
			out << "pre_cycle,post_cycle,pre_pc,post_pc,cause,pre_instructions,post_instructions,selection,pre_mode,post_mode,pre_pending,post_pending,pre_external,post_external,pre_due,post_due,pre_target_clock,post_target_clock,pre_deadline,post_deadline,pre_scheduled_delay,post_scheduled_delay,pre_base_lag,post_base_lag,pre_fine_lag,post_fine_lag,clock_calls,interrupts_serviced,last_vector\n";
			header = true;
		}
		const char* selection = before.periphSelected ?
			(before.periphDue ? "peripheral" : "peripheral_not_due") :
			before.mode == DSP::DefaultPreventInterrupt ? "interrupt_suppressed" :
			before.mode == DSP::LongInterrupt ? "long_interrupt_noop" :
			before.pending ? "interrupt" : "other";
		out << before.cycle << ',' << after.cycle << ',' << before.pc << ',' << after.pc
			<< ',' << cause << ',' << before.instructions << ',' << after.instructions
			<< ',' << selection << ',' << before.mode << ',' << after.mode << ','
			<< before.pending << ',' << after.pending << ',' << before.external << ','
			<< after.external << ',' << before.periphDue << ',' << after.periphDue
			<< ',' << before.targetClock << ',' << after.targetClock << ','
			<< before.deadline << ',' << after.deadline << ','
			<< before.scheduledDelay << ',' << after.scheduledDelay << ','
			<< before.baseLag << ',' << after.baseLag << ','
			<< before.fineLag << ',' << after.fineLag << ','
			<< after.clockCalls - before.clockCalls << ','
			<< after.serviced - before.serviced << ',' << after.lastVector << '\n';
	}

	inline void g1TraceEssiTimeline(const char* stage, IPeripherals& peripherals,
		const uint64_t clockCount, const uint64_t lastClock, const uint32_t period,
		const uint32_t source, const uintptr_t esxi, const uint32_t finePeriod,
		const uint64_t fineLastClock, const bool hasFine, const uint32_t deadline)
	{
		static const char* path = std::getenv("G1_DSP_ESSI_TIMELINE_FILE");
		if(!path || !*path) return;
		auto& dsp = peripherals.getDSP();
		auto& state = g1EssiTimeline();
		if(state.target.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(&dsp)) return;
		static const char* beginText = std::getenv("G1_DSP_ESSI_TIMELINE_BEGIN");
		static const char* endText = std::getenv("G1_DSP_ESSI_TIMELINE_END");
		if(!beginText || !endText) return;
		static const uint64_t begin = std::strtoull(beginText, nullptr, 10);
		static const uint64_t end = std::strtoull(endText, nullptr, 10);
		const auto cycle = dsp.getCycles();
		if(cycle < begin || cycle > end) return;
		std::lock_guard<std::mutex> lock(state.mutex);
		if(!state.opened)
		{
			state.begin = begin; state.end = end;
			state.events.open(path, std::ios::out | std::ios::trunc);
			state.blocks.open(std::string(path) + ".blocks.csv", std::ios::out | std::ios::trunc);
			state.events << "event,cycle,pc,previous_entry,entry_interval,scheduled_at,scheduled_delay,base_lag,fine_lag,last_base,last_fine,period,fine_period,source,deadline,has_fine,essi1_rx_enabled,essi1_sr,previous_fine_receives,previous_base_services\n";
			state.blocks << "trigger_cycle,position,pre_cycle,post_cycle,pre_pc,post_pc\n";
			state.opened = true;
		}
		if(!state.events || !state.blocks) return;
		if(std::strcmp(stage, "clock_schedule") == 0)
		{
			state.scheduledAt = cycle;
			state.scheduledDelay = deadline;
			state.lastDeadline = deadline;
			return;
		}
		if(std::strcmp(stage, "clock_immediate") == 0)
		{
			state.lastDeadline = 0;
			return;
		}
		if(std::strcmp(stage, "fine_rx_post") == 0 &&
			esxi == reinterpret_cast<uintptr_t>(&static_cast<Peripherals56303&>(peripherals).getEssi1()))
		{
			++state.fineReceives;
			return;
		}
		if(std::strcmp(stage, "clock_advance") == 0)
		{
			++state.baseServices;
			state.lastBaseClock = lastClock;
			return;
		}
		if(std::strcmp(stage, "fine_tick_post") == 0 &&
			esxi == reinterpret_cast<uintptr_t>(&static_cast<Peripherals56303&>(peripherals).getEssi1()))
		{
			state.lastFineClock = fineLastClock;
			return;
		}
		if(std::strcmp(stage, "fine_tick_pre") == 0 &&
			esxi == reinterpret_cast<uintptr_t>(&static_cast<Peripherals56303&>(peripherals).getEssi1()))
		{
			const auto level = g1LagThreshold(clockCount - fineLastClock);
			if(level > state.fineThreshold)
			{
				state.events << "fine_threshold," << cycle << ',' << dsp.getPC().toWord()
					<< ',' << state.lastEntry << ',' << cycle - state.lastEntry
					<< ',' << state.scheduledAt << ',' << state.scheduledDelay
					<< ',' << clockCount - lastClock << ','
					<< clockCount - fineLastClock << ',' << lastClock << ',' << fineLastClock
					<< ',' << period << ',' << finePeriod << ',' << source << ','
					<< deadline << ',' << hasFine << ','
					<< static_cast<Peripherals56303&>(peripherals).getEssi1().hasEnabledReceivers() << ','
					<< static_cast<Peripherals56303&>(peripherals).getEssi1().getSR()
					<< ',' << state.fineReceives << ',' << state.baseServices << '\n';
				state.fineThreshold = level;
				if(!state.triggerCycle) state.triggerCycle = cycle;
			}
			state.lastFineClock = fineLastClock;
			state.finePeriod = finePeriod;
			return;
		}
		if(std::strcmp(stage, "clock_enter") != 0) return;
		++state.clockCalls;
		state.lastBaseClock = lastClock;
		state.lastDeadline = deadline;
		const uint64_t baseLag = clockCount - lastClock;
		const uint64_t fineLag = state.lastFineClock && clockCount >= state.lastFineClock ?
			clockCount - state.lastFineClock : 0;
		const auto baseLevel = g1LagThreshold(baseLag);
		const auto fineLevel = g1LagThreshold(fineLag);
		const bool configChange = state.configured &&
			(period != state.previousPeriod || source != state.previousSource || hasFine != state.previousFine);
		const bool threshold = baseLevel > state.baseThreshold || fineLevel > state.fineThreshold;
		const bool sample = !state.lastSample || cycle - state.lastSample >= 8192;
		const bool overdueNow = state.configured && deadline > 0 &&
			cycle - state.lastEntry > deadline + 4096;
		const bool overdue = overdueNow && !state.previousOverdue;
		const bool newCatchupHigh = state.fineReceives > 16 && state.fineReceives > state.maxCatchup;
		if(sample || threshold || configChange || overdue || newCatchupHigh)
		{
			const char* reason = threshold ? "threshold" : configChange ? "configuration" :
				overdue ? "late_entry" : newCatchupHigh ? "catchup" : "sample";
			state.events << reason << ',' << cycle << ',' << dsp.getPC().toWord() << ','
				<< state.lastEntry << ',' << cycle - state.lastEntry << ','
				<< state.scheduledAt << ',' << state.scheduledDelay << ','
				<< baseLag << ',' << fineLag << ',' << lastClock
				<< ',' << state.lastFineClock << ',' << period << ',' << state.finePeriod
				<< ',' << source << ',' << deadline << ',' << hasFine << ','
				<< static_cast<Peripherals56303&>(peripherals).getEssi1().hasEnabledReceivers() << ','
				<< static_cast<Peripherals56303&>(peripherals).getEssi1().getSR() << ','
				<< state.fineReceives << ',' << state.baseServices << '\n';
			state.lastSample = cycle;
			if((threshold || configChange || overdue) && !state.triggerCycle)
			{
				state.triggerCycle = cycle;
				state.afterTrigger = 0;
			}
		}
		if(baseLevel > state.baseThreshold) state.baseThreshold = baseLevel;
		if(fineLevel > state.fineThreshold) state.fineThreshold = fineLevel;
		if(state.fineReceives > state.maxCatchup) state.maxCatchup = state.fineReceives;
		state.previousPeriod = period;
		state.previousSource = source;
		state.previousFine = hasFine;
		state.previousOverdue = overdueNow;
		state.configured = true;
		state.lastEntry = cycle;
		state.fineReceives = 0;
		state.baseServices = 0;
	}

	inline void g1TraceEssiClock(const char* stage, IPeripherals& peripherals,
		const uint64_t clockCount, const uint64_t lastClock, const uint32_t period,
		const uint32_t clockSource, const int entry, const uintptr_t esxi,
		const int rxCounter, const int rxDivider, const int rxCount,
		const uint32_t finePeriod = 0, const uint64_t fineLastClock = 0,
		const bool hasFine = false, const uint32_t nextDeadline = 0)
	{
		g1TraceEssiTimeline(stage, peripherals, clockCount, lastClock, period,
			clockSource, esxi, finePeriod, fineLastClock, hasFine, nextDeadline);
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
