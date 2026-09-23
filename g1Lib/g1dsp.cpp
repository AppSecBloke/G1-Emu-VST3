#include "g1dsp.h"

#include "mc68k/hdi08.h"
#include "dsp56kEmu/jit.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace g1
{
	namespace
	{
		// The DSP56303 has 4K of internal P and 2K+2K of X/Y. Plenty is reserved in case some
		// G1 program uses more; the boot ROM lives at $FF0000.
		constexpr dsp56k::TWord g_pMemSize = 0x10000;
		constexpr dsp56k::TWord g_xyMemSize = 0x10000;
		constexpr dsp56k::TWord g_externalMemAddr = 0x8000;
		constexpr dsp56k::TWord g_bootRom = 0xff0000;

		// The G1 works at 96 kHz with the DSPs at 82.944 MHz: 864 cycles per sample.
		constexpr uint32_t g_samplerate = 96000;
		constexpr uint32_t g_dspClock = 82944000;
		constexpr uint32_t g_cyclesPerFrame = g_dspClock / g_samplerate;	// 864

		// The ESSIs run on the clock derived from their CRA (the core's "fine link" mode):
		// 2*(PM+1)*24 cycles per word, 96 on the links between DSPs (PM=1: 9 words per sample,
		// exactly what DMA4 sends each block) and 144 on DSP 3 towards the codec (PM=2). The base
		// clock is only an upper bound: it has to be slower than both.
		constexpr uint32_t g_essiBaseCyclesPerWord = g_cyclesPerFrame / 2;

		// In asynchronous mode the receiver uses the transmitter's clock (external SC0), not its
		// own CRA. Every DSP transmitting down the chain has PM=1: 96 cycles per word. DSP 3 has
		// PM=2 because its CRA is set for the codec: its receiver would run at 144 and could only
		// take 6 of the 9 words of each sample.
		constexpr uint32_t g_linkCyclesPerWord = 96;

		// Link delay between DSPs, in blocks. The threads sync every ~4,000 cycles (about 5
		// blocks): the receiver takes the block from g_linkLatency blocks ago, which has surely
		// arrived. On the hardware it is less than a block; here it is ~83 us per DSP.
		constexpr uint64_t g_linkLatency = 8;
		// Each ESSI's receive DMA writes into a 9-word ring: X:$6C0 (ESSI0) and X:$6C9 (ESSI1).
		// Word i of the block always goes to base+i.
		constexpr dsp56k::TWord g_linkBase[2] = {0x6c0, 0x6c9};

		// Word period derived from a CRA (DSP56303UM, fig. 7-3), as in the core.
		uint32_t essiWordCycles(const dsp56k::TWord _cra)
		{
			static constexpr uint32_t bits[8] = {8, 12, 16, 24, 32, 32, 24, 24};
			const uint32_t pm = (_cra & 0xff) + 1;
			const uint32_t prescale = (_cra & (1u << 11)) ? 1 : 8;
			return 2 * pm * prescale * bits[(_cra >> 19) & 7];
		}

		// Each DSP's IRQD pin gets the sample clock; its vector ($16) jumps to the block routine
		// ($175 or later), which computes one sample of the patch.
		constexpr dsp56k::TWord g_irqdVector = 0x16;

		// Maximum number of cycles a DSP is allowed to run in one CPU wait.
		constexpr uint64_t g_waitClamp = 200000;
		// ...and in a status (ISR) poll, much less: a few words of margin.
		constexpr uint64_t g_isrWaitClamp = 2000;
	}

	Dsp::Dsp(mc68k::Hdi08& _hdiUc, const uint32_t _index)
		: m_hdiUc(_hdiUc)
		, m_index(_index)
		, m_memory(m_validator, g_pMemSize, g_xyMemSize, g_externalMemAddr)
		, m_dsp(m_memory, &m_periph, &m_periphNop)
	{
		auto config = m_dsp.getJit().getConfig();
		config.aguSupportBitreverse = true;
		config.linkJitBlocks = false;
		config.dynamicPeripheralAddressing = false;
		config.dynamicFastInterrupts = true;
		config.maxInstructionsPerBlock = 32;
#ifdef G1_DSP_TRACE
		// Reference run only. The failing diagnostic always uses the normal 32.
		if(_index == 0 && std::getenv("G1_DSP_WATCH_BLOCKSIZE") &&
			std::string(std::getenv("G1_DSP_WATCH_BLOCKSIZE")) == "1")
			config.maxInstructionsPerBlock = 1;
#endif
		config.maxDoIterations = 1;
		m_dsp.getJit().setConfig(config);

		// Program memory full of RTS: a jump into garbage does not compile odd things.
		for(dsp56k::TWord i = 0; i < m_memory.sizeP(); ++i)
		{
			m_memory.set(dsp56k::MemArea_P, i, 0x00000c);
			m_dsp.getJit().notifyProgramMemWrite(i);
		}

		auto& clock = m_periph.getEssiClock();
		clock.setClockSource(dsp56k::EsxiClock::ClockSource::Cycles);
		clock.setSamplerate(g_samplerate * 2);
		clock.setCyclesPerSample(g_essiBaseCyclesPerWord);

		// Must be enabled before the program writes CRA.
		for(auto* essi : {&m_periph.getEssi0(), &m_periph.getEssi1()})
			essi->setFineLinkMode(true);

		// Link by position: when the receiver asks for a frame, the emulator looks at which ring
		// word its DMA will write next and gives it that channel from the previous DSP's block. So
		// channel i always ends up at base+i, like on the hardware, regardless of the phase between
		// the ESSIs or of when the receiver was re-enabled (the hardware gets that from its common clock).
		for(uint32_t e = 0; e < 2; ++e)
		{
			auto& essi = e == 0 ? m_periph.getEssi0() : m_periph.getEssi1();
			essi.setReadRxCallback([this, e](uint64_t&, dsp56k::Audio::RxFrame& _frame)
			{
				if(!m_hasUpstream)
				{
					// The first DSP has no DSP upstream: it receives the codec, the back-panel audio
					// inputs (R on ESSI0 -> X:$6C4, L on ESSI1 -> X:$6C5; from there they travel down
					// the link to the other DSPs, in channels 4 and 5).
					_frame.resize(2);
					_frame[0][0] = _frame[1][0] = static_cast<dsp56k::TWord>(m_input[e]) & 0xffffff;
					return;
				}
				readLink(e, _frame);
			});
		}

		// ESSI slot masks (TSMA/TSMB/RSMA/RSMB) at their reset value: all slots enabled. The
		// emulator leaves them at 0, and the G1's voice DSPs never write them (only DSP 3 sets
		// them): with 0 they neither transmitted nor requested data from the DMA.
		for(const dsp56k::TWord reg : {0xffffb4u, 0xffffb3u, 0xffffb2u, 0xffffb1u, 0xffffa4u, 0xffffa3u, 0xffffa2u, 0xffffa1u})
			m_periph.write(reg, 0xffffff);

		// An ESSI waiting for input audio must not block. On the hardware the codec always
		// delivers frames: here the input starts with empty ones.
		m_periph.getEssi0().writeEmptyAudioIn(64);
		m_periph.getEssi1().writeEmptyAudioIn(64);

		hdi08().setRXRateLimit(0);
		// No arbitration: the G1's host commands are fast interrupts (a single movep in the vector,
		// no JSR/RTI). Gearmulator's arbitration waits for the RTI on the stack to consider the
		// command done, and with these it stayed "busy" forever.
		hdi08().setHostCommandArbitration(false);
		m_dsp.setInterruptServicedCallback([this](const dsp56k::TWord _vba)
		{
			++m_servicedVectors[_vba];
			m_lastVector = _vba;
		});

		// HF0/HF1 flags from the CPU's ICR to the DSP's HSR.
		m_hdiUc.setIcrWriteCallback([this](const uint8_t _icr)
		{
			hdi08().setHostFlags((_icr & mc68k::Hdi08::Hf0) ? 1 : 0, (_icr & mc68k::Hdi08::Hf1) ? 1 : 0);
		});
		m_hdiUc.setWriteIrqCallback([this](const uint8_t _vector) { hostCommand(_vector); });
		m_hdiUc.setReadIsrCallback([this](const uint8_t _isr) { return readIsr(_isr); });
		m_hdiUc.setRxEmptyCallback([this](const bool _needMoreData)
		{
			if(_needMoreData && m_booted && !hdi08().hasTX())
				runUntil(m_dsp.getCycles() + g_waitClamp, RunCause::RxEmpty);
			transferToHost();
		});
		m_hdiUc.setForceTxde(false);
		m_hdiUc.setInitHdi08Callback([this]
		{
			m_hdiUc.icr(m_hdiUc.icr() & 0x7f);
			m_hdiUc.isr(m_hdiUc.isr() | mc68k::Hdi08::IsrBits::Txde | mc68k::Hdi08::IsrBits::Trdy);
		});

		// G1_INTERP=mask: those DSPs (bit n = DSP n) run on the core's interpreter instead of the
		// JIT. Much slower; useful to tell whether a fault is the JIT's.
		m_noLaFix = std::getenv("G1_NO_LA_FIX") != nullptr;	// only to compare against the bug
		if(const char* in = std::getenv("G1_INTERP"))
			m_interpreter = ((std::strtoul(in, nullptr, 0) >> _index) & 1) != 0;

		armBoot();
	}

	// Back to the boot ROM state: the next words are length, address and program.
	void Dsp::armBoot()
	{
		m_booted = false;

		// What the CPU had already sent and the previous program did not read waits in the
		// port: on the hardware the boot ROM reads it, so it is handed over in order.
		std::vector<dsp56k::TWord> pending;
		auto& rx = const_cast<std::remove_const_t<std::remove_reference_t<decltype(hdi08().rxData())>>&>(hdi08().rxData());
		while(!rx.empty())
			pending.push_back(rx.pop_front());
		hdi08().clearRX();

		m_boot = std::make_unique<dsp56k::DspBoot>(m_dsp);
		m_hdiUc.setWriteTxCallback([this](const uint32_t _word)
		{
			if(!m_booted && m_boot->hdiWriteTX(_word))
				onBootFinished();
			else if(m_booted)
				hostWord(_word);
#ifdef G1_DSP_TRACE
			watchExternal("boot_word");
#endif
		});
		for(const auto w : pending)
		{
			if(m_booted)
				hostWord(w);
			else if(m_boot->hdiWriteTX(w))
				onBootFinished();
#ifdef G1_DSP_TRACE
			watchExternal("pending_boot_word");
#endif
		}
	}

	void Dsp::onBootFinished()
	{
		m_booted = true;
		++m_bootCount;
		m_hdiUc.setWriteTxCallback([this](const uint32_t _word) { hostWord(_word); });
	}

#ifdef G1_DSP_TRACE
	Dsp::WatchSnapshot Dsp::watchCapture()
	{
		WatchSnapshot s;
		s.pc = m_dsp.getPC().toWord();
		s.opcode = s.pc < m_memory.sizeP() ? m_memory.get(dsp56k::MemArea_P, s.pc) : 0;
		s.cycles = m_dsp.getCycles();
		s.a = m_dsp.regs().a.var; s.b = m_dsp.regs().b.var;
		s.x0 = m_dsp.x0().toWord(); s.x1 = m_dsp.x1().toWord();
		s.y0 = m_dsp.y0().toWord(); s.y1 = m_dsp.y1().toWord();
		s.r3 = m_dsp.regs().r[3].var; s.r4 = m_dsp.regs().r[4].var;
		s.sr = m_dsp.getSR().toWord();
		for(uint32_t i = 0; i < 3; ++i)
			s.words[i] = m_memory.get(dsp56k::MemArea_X, 0x1d + i);
		return s;
	}

	void Dsp::watchEmit(const char* _kind, const WatchSnapshot& _before, const WatchSnapshot& _after)
	{
		m_startupWatch << _kind << ',' << m_watchStage << ',' << m_watchCall
			<< ',' << _before.pc << ',' << _after.pc << ',' << _before.opcode
			<< ',' << _before.cycles << ',' << _after.cycles;
		for(const auto w : _before.words) m_startupWatch << ',' << w;
		for(const auto w : _after.words) m_startupWatch << ',' << w;
		m_startupWatch << ',' << _before.a << ',' << _after.a
			<< ',' << _before.b << ',' << _after.b
			<< ',' << _before.x0 << ',' << _after.x0
			<< ',' << _before.x1 << ',' << _after.x1
			<< ',' << _before.y0 << ',' << _after.y0
			<< ',' << _before.y1 << ',' << _after.y1
			<< ',' << _before.r3 << ',' << _after.r3
			<< ',' << _before.r4 << ',' << _after.r4
			<< ',' << _before.sr << ',' << _after.sr << '\n';
	}

	bool Dsp::armStartupWatch(const char* _path)
	{
		if(m_index != 0 || !_path || !_path[0] || m_startupWatch.is_open())
			return false;
		m_startupWatch.open(_path, std::ios::out | std::ios::trunc);
		if(!m_startupWatch)
			return false;
		m_startupWatch << "kind,stage,call,pre_pc,post_pc,pre_opcode,pre_cycles,post_cycles,"
			"pre_x1d,pre_x1e,pre_x1f,post_x1d,post_x1e,post_x1f,"
			"pre_a,post_a,pre_b,post_b,pre_x0,post_x0,pre_x1,post_x1,"
			"pre_y0,post_y0,pre_y1,post_y1,pre_r3,post_r3,pre_r4,post_r4,pre_sr,post_sr\n";
		m_watchCall = 0;
		m_watchContextRemaining = 2048;
		m_watchStage = "before_upload";
		const auto s = watchCapture();
		m_watchLast = s.words;
		watchEmit("checkpoint", s, s);
		return true;
	}

	void Dsp::watchExternal(const char* _source)
	{
		if(!m_startupWatch.is_open())
			return;
		std::array<dsp56k::TWord, 3> current{};
		for(uint32_t i = 0; i < current.size(); ++i)
			current[i] = m_memory.get(dsp56k::MemArea_X, 0x1d + i);
		if(current != m_watchLast)
		{
			const auto after = watchCapture();
			auto before = after;
			before.words = m_watchLast;
			watchEmit(_source, before, after);
			m_watchLast = after.words;
		}
	}

	void Dsp::startupCheckpoint(const char* _stage)
	{
		if(!m_startupWatch.is_open())
			return;
		watchExternal("between_calls");
		m_watchStage = _stage;
		const auto s = watchCapture();
		watchEmit("checkpoint", s, s);
		m_startupWatch.flush();
	}

	bool Dsp::armSettleTrace(const char* _path)
	{
		if(m_index != 0 || !_path || !_path[0] || !m_startupWatch.is_open() || m_settleTrace.is_open())
			return false;
		m_settleTrace.open(_path, std::ios::out | std::ios::trunc);
		if(!m_settleTrace)
			return false;
		m_settleBlocksTrace.open(std::string(_path) + ".blocks.csv", std::ios::out | std::ios::trunc);
		if(!m_settleBlocksTrace)
			return false;
		if(const char* focus = std::getenv("G1_DSP_SETTLE_FOCUS_MS"))
			m_settleFocusMs = static_cast<uint32_t>(std::strtoul(focus, nullptr, 10));
		m_settleBlocksTrace << "ms,block,cause,pre_pc,post_pc,pre_opcode,pre_cycles,post_cycles,"
			"pre_sr,post_sr,pre_a,post_a,pre_b,post_b,pre_r3,post_r3,pre_r4,post_r4,"
			"pre_x1d,post_x1d,pre_x1e,post_x1e,pre_x1f,post_x1f,irqd_count,rx_depth\n";
		m_settleTrace << "uc_cycles,dsp_cycles,pc,opcode,sr,la,irqd_count,next_irqd,host_words,host_commands,"
			"rx_depth,pending_interrupts,x1d,x1e,x1f,blocks,top_pc,top_pc_blocks,max_block_pc,max_block_cycles,"
			"catchup_calls,catchup_cycles,hostword_calls,hostword_cycles,hostcommand_calls,hostcommand_cycles,"
			"readisr_calls,readisr_cycles,rxempty_calls,rxempty_cycles\n";
		return true;
	}

	void Dsp::settleSample(const uint64_t _ucCycles)
	{
		if(!m_settleTrace.is_open())
			return;
		uint32_t topPc = 0;
		uint64_t topCount = 0;
		for(const auto& [pc, count] : m_settlePcs)
			if(count > topCount) { topPc = pc; topCount = count; }
		const auto pc = m_dsp.getPC().toWord();
		m_settleTrace << _ucCycles << ',' << m_dsp.getCycles() << ',' << pc << ','
			<< (pc < m_memory.sizeP() ? m_memory.get(dsp56k::MemArea_P, pc) : 0) << ','
			<< m_dsp.getSR().toWord() << ',' << m_dsp.regs().la.var << ',' << m_irqdCount << ','
			<< m_nextIrqd << ',' << m_hostWords << ',' << m_hostCommands << ','
			<< hdi08().rxData().size() << ',' << m_dsp.hasPendingInterrupts();
		for(uint32_t i = 0x1d; i <= 0x1f; ++i)
			m_settleTrace << ',' << m_memory.get(dsp56k::MemArea_X, i);
		m_settleTrace << ',' << m_settleBlocks << ',' << topPc << ',' << topCount << ','
			<< m_settleMaxBlockPc << ',' << m_settleMaxBlockCycles;
		for(size_t i = 0; i < m_settleCalls.size(); ++i)
			m_settleTrace << ',' << m_settleCalls[i] << ',' << m_settleCycles[i];
		m_settleTrace << '\n';
		m_settleTrace.flush();
		m_settleCalls.fill(0);
		m_settleCycles.fill(0);
		m_settlePcs.clear();
		m_settleBlocks = m_settleMaxBlockCycles = 0;
		m_settleMaxBlockPc = 0;
		++m_settleSampleIndex;
	}

	void Dsp::watchExec()
	{
		watchExternal("between_calls");
		const auto before = watchCapture();
		m_dsp.exec();
		const auto after = watchCapture();
		++m_watchCall;
		if(m_settleBlocksTrace.is_open() &&
			m_settleSampleIndex == static_cast<int32_t>(m_settleFocusMs) &&
			m_settleLoggedBlocks < 20000)
		{
			m_settleBlocksTrace << m_settleFocusMs << ',' << m_settleLoggedBlocks++ << ','
				<< m_settleActiveCause << ',' << before.pc << ',' << after.pc << ',' << before.opcode
				<< ',' << before.cycles << ',' << after.cycles << ',' << before.sr << ',' << after.sr
				<< ',' << before.a << ',' << after.a << ',' << before.b << ',' << after.b
				<< ',' << before.r3 << ',' << after.r3 << ',' << before.r4 << ',' << after.r4;
			for(uint32_t i = 0; i < 3; ++i)
				m_settleBlocksTrace << ',' << before.words[i] << ',' << after.words[i];
			m_settleBlocksTrace << ',' << m_irqdCount << ',' << hdi08().rxData().size() << '\n';
		}
		const bool changed = before.words != after.words;
		const auto pc = before.pc;
		const bool nearby = m_watchStage == "note_on" &&
			((pc >= 0x200 && pc <= 0x250) || (pc >= 0x3b7 && pc <= 0x43d) ||
			 (pc >= 0x650 && pc <= 0x680));
		if(changed || (nearby && m_watchContextRemaining))
		{
			watchEmit(changed ? "changed_in_block" : "note_context", before, after);
			if(!changed && nearby) --m_watchContextRemaining;
		}
		m_watchLast = after.words;
	}

	bool Dsp::armDiagnosticTrace(const char* _path, const uint32_t _entryPc, const uint32_t _steps)
	{
		if(m_index != 0 || !_path || !_path[0] || !_steps)
			return false;
		m_trace.open(_path, std::ios::out | std::ios::trunc);
		m_traceWrites.open(std::string(_path) + ".writes.csv", std::ios::out | std::ios::trunc);
		if(!m_trace || !m_traceWrites)
			return false;
		m_trace << "step,phase,pc,opcode,cycles,a,b,x0,x1,y0,y1,r2,r3,r4,r5,sr,x5,x6,xr2,xr3,yr4,yr5,ybuf0,ybuf1\n";
		m_traceWrites << "step,area,address,before,after\n";
		m_traceEntry = _entryPc;
		m_traceRemaining = _steps;
		m_traceStep = 0;
		m_traceStarted = false;
		return true;
	}

	void Dsp::traceExec()
	{
		if(!m_traceStarted)
		{
			if(m_traceEntry && m_dsp.getPC().toWord() != m_traceEntry)
			{
				m_dsp.exec();
				return;
			}
			m_traceStarted = true;
		}
		struct Snapshot
		{
			uint32_t pc = 0, opcode = 0, sr = 0;
			uint32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
			uint32_t r2 = 0, r3 = 0, r4 = 0, r5 = 0;
			int64_t a = 0, b = 0;
			uint64_t cycles = 0;
			std::array<dsp56k::TWord, 0x800> x{}, y{};
		};
		auto snapshot = [this]()
		{
			Snapshot s;
			s.pc = m_dsp.getPC().toWord();
			s.opcode = m_memory.get(dsp56k::MemArea_P, s.pc);
			s.cycles = m_dsp.getCycles();
			s.a = m_dsp.regs().a.var;
			s.b = m_dsp.regs().b.var;
			s.sr = m_dsp.getSR().toWord();
			s.x0 = m_dsp.x0().toWord(); s.x1 = m_dsp.x1().toWord();
			s.y0 = m_dsp.y0().toWord(); s.y1 = m_dsp.y1().toWord();
			s.r2 = m_dsp.regs().r[2].var; s.r3 = m_dsp.regs().r[3].var;
			s.r4 = m_dsp.regs().r[4].var; s.r5 = m_dsp.regs().r[5].var;
			for(uint32_t i = 0; i < s.x.size(); ++i)
			{
				s.x[i] = m_memory.get(dsp56k::MemArea_X, i);
				s.y[i] = m_memory.get(dsp56k::MemArea_Y, i);
			}
			return s;
		};
		const auto before = snapshot();
		m_dsp.exec();
		const auto after = snapshot();
		auto emit = [this](const Snapshot& s, const char* phase)
		{
			const auto at = [](const auto& mem, uint32_t address)
			{
				return address < mem.size() ? static_cast<int64_t>(mem[address]) : int64_t{-1};
			};
			m_trace << m_traceStep << ',' << phase << ',' << s.pc << ',' << s.opcode
				<< ',' << s.cycles << ',' << s.a << ',' << s.b << ',' << s.x0 << ',' << s.x1
				<< ',' << s.y0 << ',' << s.y1 << ',' << s.r2 << ',' << s.r3 << ',' << s.r4
				<< ',' << s.r5 << ',' << s.sr << ',' << s.x[5] << ',' << s.x[6]
				<< ',' << at(s.x, s.r2) << ',' << at(s.x, s.r3)
				<< ',' << at(s.y, s.r4) << ',' << at(s.y, s.r5)
				<< ',' << at(s.y, s.x[5]) << ',' << at(s.y, s.x[6]) << '\n';
		};
		emit(before, "pre");
		emit(after, "post");
		for(uint32_t i = 0; i < before.x.size(); ++i)
		{
			if(before.x[i] != after.x[i])
				m_traceWrites << m_traceStep << ",X," << i << ',' << before.x[i] << ',' << after.x[i] << '\n';
			if(before.y[i] != after.y[i])
				m_traceWrites << m_traceStep << ",Y," << i << ',' << before.y[i] << ',' << after.y[i] << '\n';
		}
		++m_traceStep;
		if(--m_traceRemaining == 0)
		{
			m_trace.flush();
			m_traceWrites.flush();
		}
	}
#endif

	void Dsp::runUntil(const uint64_t _cycles, const RunCause _cause)
	{
#ifdef G1_DSP_TRACE
		const auto cause = static_cast<size_t>(_cause);
		if(m_settleTrace.is_open())
		{
			++m_settleCalls[cause];
			m_settleActiveCause = static_cast<uint32_t>(_cause);
		}
#endif
		while(m_booted && m_dsp.getCycles() < _cycles)
		{
			// `jmp $FF0000`: the program returns to the boot ROM.
			if(m_dsp.getPC().toWord() >= g_bootRom)
			{
				armBoot();
				return;
			}
			if(!m_pcWatch.empty())
			{
				auto it = m_pcWatch.find(m_dsp.getPC().toWord());
				if(it != m_pcWatch.end()) ++it->second;
			}
			const auto before = m_dsp.getCycles();
#ifdef G1_DSP_TRACE
			const auto blockPc = m_settleTrace.is_open() ? m_dsp.getPC().toWord() : 0;
#endif
			if(before >= m_nextIrqd)
			{
				// Fixed grid (not "now + period"): IRQD does not drift against the ESSI clock, which
				// also counts exact cycles. If it fell far behind (reload stop, boot), it re-locks
				// without a burst of interrupts. In multiples of 864 cycles: the four DSPs share the
				// grid, like on the hardware (they stay in lock-step), so the block number is common.
				m_nextIrqd = (before - m_nextIrqd > g_cyclesPerFrame * 4) ? (before / g_cyclesPerFrame + 1) * g_cyclesPerFrame : m_nextIrqd + g_cyclesPerFrame;
				if(irqdEnabled())
				{
					if(m_inputProvider)
						m_inputProvider(m_input[1], m_input[0]);	// L comes in on ESSI1 and R on ESSI0
					if(m_blockCallback)
						tapBlock();
					if(m_next)
						tapLink(before / g_cyclesPerFrame);
					m_dsp.injectInterrupt(g_irqdVector);	// like a peripheral: does not block
					++m_irqdCount;
				}
			}
			if(m_interpreter)
				m_dsp.execInterpreter();
			else
#ifdef G1_DSP_TRACE
			if(m_index == 0 && m_startupWatch.is_open())
				watchExec();
			else if(m_index == 0 && m_traceRemaining)
				traceExec();
			else
#endif
				m_dsp.exec();
			if(static_cast<dsp56k::TWord>(m_dsp.regs().la.var) != m_lastLa && !m_noLaFix)
				onLaChanged();
			const auto now = m_dsp.getCycles();
#ifdef G1_DSP_TRACE
			if(m_settleTrace.is_open())
			{
				const auto delta = now - before;
				m_settleCycles[cause] += delta;
				++m_settleBlocks;
				++m_settlePcs[blockPc];
				if(delta > m_settleMaxBlockCycles)
				{
					m_settleMaxBlockCycles = delta;
					m_settleMaxBlockPc = blockPc;
				}
			}
#endif
			if(now == before)	// DSP stopped (WAIT/STOP or halted): do not insist
			{
				++m_stalls;
				return;
			}
			if((now & 0x3ff) < now - before)	// every ~1000 cycles (less than a frame)
				drainAudio();
		}
	}

	// IRQD only counts if the program enables it in the IPRC (X:$FFFFFF, IDL level in bits
	// 9-10; 0 = disabled) and the SR mask lets it through. The emulator only checks the SR:
	// during the reload stop (IPRC=$FF0800) the G1 disables it on purpose.
	bool Dsp::irqdEnabled()
	{
		const auto iprc = m_periph.read(0xffffff, dsp56k::Instruction::Invalid);
		return ((iprc >> 9) & 3) != 0 && !m_dsp.isInterruptMasked(g_irqdVector);
	}

	// What this DSP sent through its two ESSIs in the previous block: 9 words per ESSI from
	// X:$5 and X:$6 (the same buffers as tapBlock), for the next DSP.
	void Dsp::tapLink(const uint64_t _block)
	{
		auto& mem = m_dsp.memory();
		const auto p0 = mem.get(dsp56k::MemArea_X, 5);
		const auto p1 = mem.get(dsp56k::MemArea_X, 6);
		if(p0 < 0x600 || p0 > 0x7f7 || p1 < 0x600 || p1 > 0x7f7 || _block == 0)
			return;
		LinkBlock b;
		b.index = _block - 1;
		for(dsp56k::TWord i = 0; i < 9; ++i)
		{
			b.words[i] = mem.get(dsp56k::MemArea_Y, p0 + i);
			b.words[9 + i] = mem.get(dsp56k::MemArea_Y, p1 + i);
		}
		for(size_t i = 0; i < b.words.size(); ++i)
		{
			const auto v = static_cast<int32_t>(b.words[i] << 8) >> 8;
			m_linkPeak[i] = std::max<uint32_t>(m_linkPeak[i], static_cast<uint32_t>(v < 0 ? -v : v));
		}
		m_linkOut.push_back(b);
	}

	// The previous block's output, before the next one starts. The block routine alternates its
	// buffers ($6C0/$6E0) and leaves in X:$5 and X:$6 the ones it just sent through DMA4 (ESSI0)
	// and DMA5 (ESSI1): two words each. Reading them here gives exactly one sample per block,
	// regardless of how the ESSI splits it into frames.
	void Dsp::tapBlock()
	{
		auto& mem = m_dsp.memory();
		const auto p0 = mem.get(dsp56k::MemArea_X, 5);
		const auto p1 = mem.get(dsp56k::MemArea_X, 6);
		if(p0 < 0x600 || p0 > 0x7fe || p1 < 0x600 || p1 > 0x7fe)
			return;	// the sound program is not running yet
		BlockFrame f;
		// Each ESSI carries its pair reversed: first the even output (2 and 4), then the odd one
		// (1 and 3). Checked with a 4Output and a different signal on each output.
		f[0] = mem.get(dsp56k::MemArea_Y, p0 + 1);	// output 1
		f[1] = mem.get(dsp56k::MemArea_Y, p0);		// output 2
		f[2] = mem.get(dsp56k::MemArea_Y, p1 + 1);	// output 3
		f[3] = mem.get(dsp56k::MemArea_Y, p1);		// output 4
		m_blocks.push_back(f);
	}

	void Dsp::readLink(const uint32_t _essi, dsp56k::Audio::RxFrame& _frame)
	{
		_frame.resize(2);
		const auto ddr = m_periph.getDMA().getDDR(2 + _essi);
		const bool inRing = ddr >= g_linkBase[_essi] && ddr < g_linkBase[_essi] + 9;
		// The frame is requested at slot 0, but slot 1 comes 96 cycles later and may already fall
		// into the next block (9 words per block, 2 per frame): each word is taken from the block
		// in which it will be received.
		for(uint32_t s = 0; s < 2; ++s)
		{
			const auto want = (m_dsp.getCycles() + s * g_linkCyclesPerWord) / g_cyclesPerFrame;
			const auto block = want > g_linkLatency ? want - g_linkLatency : 0;
			// Blocks no longer needed are dropped. If the one needed is missing (the previous DSP
			// is stopped reloading, without IRQD), silence: repeating an old one would buzz.
			while(m_linkIn.size() > 1 && m_linkIn[1].index <= block)
				m_linkIn.pop_front();
			const LinkBlock* src = (!m_linkIn.empty() && m_linkIn.front().index == block) ? &m_linkIn.front() : nullptr;
			const auto pos = (ddr - g_linkBase[_essi] + s) % 9;
			_frame[s][0] = (src && inRing) ? src->words[_essi * 9 + pos] : 0;
		}
	}

	// The OS extends the main loop without rewriting the DO: when it loads a patch with
	// control-rate modules (envelopes, clocks, master oscillators...) it puts their code at the
	// end of the loop (from $174) and changes the LA register with a host command (vector $7C:
	// movep ...,la). On the DSP the loop end is compared with LA on every pass; Gearmulator's
	// JIT, instead, records the end when it compiles the DO and cuts its blocks there. Without
	// this, only the first instruction of the control code ran and all control rate stood still.
	void Dsp::onLaChanged()
	{
		const dsp56k::TWord oldLa = m_lastLa;
		const dsp56k::TWord newLa = static_cast<dsp56k::TWord>(m_dsp.regs().la.var);
		m_lastLa = newLa;
		auto& jit = m_dsp.getJit();
		constexpr dsp56k::TWord none = 0xffffffff;
		dsp56k::TWord begin = none;
		for(const auto& [b, end] : jit.getLoops())
			if(end == oldLa + 1)
				begin = b;
		if(begin == none)
			return;	// that loop is not compiled: the JIT will record it correctly when it compiles it
		jit.removeLoop(begin);
		jit.addLoop(begin, newLa + 1);
		// Blocks that ended at the old end or run through the new one are recompiled.
		for(const dsp56k::TWord pc : std::array<dsp56k::TWord, 4>{oldLa, oldLa + 1, newLa, newLa + 1})
			jit.destroy(pc);
		++m_laChanges;
	}

	void Dsp::catchUp(const uint64_t _cycles)
	{
		runUntil(_cycles);
		drainAudio();
		transferToHost();
	}

	// Collects what left the ESSIs (for flushAudio) and measures the peaks.
	void Dsp::drainAudio()
	{
		uint32_t e = 0;
		for(auto* essi : {&m_periph.getEssi0(), &m_periph.getEssi1()})
		{
			// The receiver of a DSP with another one upstream runs at its transmitter's rate.
			if(m_hasUpstream)
			{
				const dsp56k::TWord cra = essi->getCRA();
				if(cra != m_craSeen[e])
				{
					m_craSeen[e] = cra;
					if(essiWordCycles(cra) != g_linkCyclesPerWord && essiWordCycles(cra) < g_essiBaseCyclesPerWord)
						m_periph.getEssiClock().setEsaiFinePeriod(essi, g_linkCyclesPerWord);
				}
			}
			auto& out = essi->getAudioOutputs();
			auto& meter = m_meter[e];
			auto& slotCount = m_slotCount[e];
			while(!out.empty())
			{
				out.pop_front([&](const auto& _frame)
				{
					slotCount = _frame.size();
					StagedFrame f{static_cast<uint32_t>(std::min<size_t>(_frame.size(), 4)), {}};
					for(uint32_t s = 0; s < f.slots; ++s)
						f.v[s] = _frame[s][0];
					m_staged[e].push_back(f);
					for(uint32_t s = 0; s < std::min<uint32_t>(_frame.size(), MeterSlots); ++s)
						for(uint32_t l = 0; l < MeterLines; ++l)
						{
							auto v = static_cast<int32_t>(_frame[s][l] << 8) >> 8;	// signed 24-bit
							const auto a = static_cast<uint32_t>(v < 0 ? -v : v);
							if(a > meter[s][l])
								meter[s][l] = a;
						}
				});
				++m_audioFrames;
			}
			++e;
		}
	}

	void Dsp::flushAudio()
	{
		// Both ESSIs interleaved, frame by frame, as they left the DSP.
		const auto n = std::max(m_staged[0].size(), m_staged[1].size());
		for(size_t k = 0; k < n; ++k)
		{
			for(uint32_t e = 0; e < 2; ++e)
			{
				if(k >= m_staged[e].size())
					continue;
				const auto& f = m_staged[e][k];
				if(m_audioCallback && f.slots >= 2)
					m_audioCallback(e, static_cast<int32_t>(f.v[0] << 8) >> 8, static_cast<int32_t>(f.v[1] << 8) >> 8);
			}
		}
		m_staged[0].clear();
		m_staged[1].clear();

		if(m_next)
		{
			for(const auto& b : m_linkOut)
				m_next->m_linkIn.push_back(b);
			m_chainedFrames += m_linkOut.size();
			m_linkOut.clear();
			// Cap in case the next DSP does not consume (stopped): about 100 ms.
			while(m_next->m_linkIn.size() > 10000)
				m_next->m_linkIn.pop_front();
		}

		if(m_blockCallback)
			for(const auto& f : m_blocks)
			{
				auto s24 = [](const dsp56k::TWord _v) { return static_cast<int32_t>(_v << 8) >> 8; };
				m_blockCallback(s24(f[0]), s24(f[1]), s24(f[2]), s24(f[3]));
			}
		m_blocks.clear();
	}

	void Dsp::hostWord(const uint32_t _word)
	{
		// HRX holds a single word: if the previous one is still there, let the DSP run.
		const auto stop = m_dsp.getCycles() + g_waitClamp;
		while(hdi08().hasRXData() && m_booted && m_dsp.getCycles() < stop)
			runUntil(m_dsp.getCycles() + 64, RunCause::HostWord);
		// If meanwhile the program went back to the boot ROM, the word belongs to it.
		if(!m_booted)
		{
			if(m_boot->hdiWriteTX(_word))
				onBootFinished();
#ifdef G1_DSP_TRACE
			watchExternal("host_boot_word");
#endif
			return;
		}
		hdi08().writeRX(&_word, 1);
		++m_hostWords;
#ifdef G1_DSP_TRACE
		watchExternal("host_word");
#endif
	}

	void Dsp::hostCommand(const uint8_t _vector)
	{
		if(!m_booted)
			return;
		// The real DSP services each host command as soon as it arrives. Here it is allowed to run
		// until it has dispatched what is pending: otherwise the external interrupt queue
		// (32 entries) fills up and injectExternalInterrupt waits forever.
		const auto stop = m_dsp.getCycles() + g_waitClamp;
		while(m_dsp.hasPendingInterrupts() && m_booted && m_dsp.getCycles() < stop)
			runUntil(m_dsp.getCycles() + 16, RunCause::HostCommand);
		if(m_dsp.hasPendingInterrupts())
			return;	// the DSP does not service them (stopped): better to lose the command than hang
		hdi08().writeHostCommand(_vector);
		++m_hostCommands;
		transferToHost();
	}

	uint8_t Dsp::readIsr(uint8_t _isr)
	{
		// On the hardware the DSP takes the incoming word at once; here it may lag. If the CPU
		// polls with a word pending, the DSP runs until it takes it: the OS routine drops the word
		// after 10 polls. Short wait: if the DSP is in a loop that does not read the port (for
		// example stopped with HF2 up waiting for the CPU to lower HF0), the poll returns the status
		// as it is.
		if(m_booted && hdi08().hasRXData())
		{
			const auto stop = m_dsp.getCycles() + g_isrWaitClamp;
			while(hdi08().hasRXData() && m_booted && m_dsp.getCycles() < stop)
				runUntil(m_dsp.getCycles() + 64, RunCause::ReadIsr);
		}
		transferToHost();
		_isr = static_cast<uint8_t>((_isr & ~mc68k::Hdi08::Rxdf) | (m_hdiUc.canReceiveData() ? 0 : mc68k::Hdi08::Rxdf));

		// HF2/HF3 from the DSP to the CPU's ISR.
		const auto hf23 = hdi08().readControlRegister() & 0x18;
		_isr = static_cast<uint8_t>((_isr & ~0x18) | hf23);

		// TXDE: room for another word. During boot, the ROM always accepts.
		_isr &= static_cast<uint8_t>(~(mc68k::Hdi08::Txde | mc68k::Hdi08::Trdy));
		const auto depth = m_booted ? hdi08().rxData().size() : 0;
		if(depth == 0)
			_isr |= mc68k::Hdi08::Txde | mc68k::Hdi08::Trdy;
		else if(depth == 1)
			_isr |= mc68k::Hdi08::Txde;
		return _isr;
	}

	bool Dsp::transferToHost()
	{
		if(m_hdiUc.canReceiveData() && hdi08().hasTX())
		{
			m_hdiUc.writeRx(hdi08().readTX());
			++m_wordsToHost;
			return true;
		}
		return false;
	}
}
