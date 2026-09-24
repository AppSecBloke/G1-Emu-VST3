# Core fixes needed by the G1, applied only to the build copy. The Gearmulator clone
# remains the source and is never modified.
get_target_property(g1_dsp_source dsp56kEmu SOURCE_DIR)
set(g1_dsp_overlay "${CMAKE_BINARY_DIR}/g1-dsp/dsp56kEmu")
set(g1_dsp_prepare "${CMAKE_BINARY_DIR}/g1-dsp/prepare")
file(GLOB g1_dsp_files CONFIGURE_DEPENDS
	"${g1_dsp_source}/*.cpp" "${g1_dsp_source}/*.h" "${g1_dsp_source}/*.inl")
foreach(source IN LISTS g1_dsp_files)
	get_filename_component(name "${source}" NAME)
	configure_file("${source}" "${g1_dsp_prepare}/${name}" COPYONLY)
endforeach()

function(g1_dsp_replace name before after)
	set(path "${g1_dsp_prepare}/${name}")
	file(READ "${path}" contents)
	string(FIND "${contents}" "${before}" match)
	if(match EQUAL -1)
		message(FATAL_ERROR "Check the G1 fix for ${name}: the external core has changed")
	endif()
	string(REPLACE "${before}" "${after}" contents "${contents}")
	file(WRITE "${path}" "${contents}")
endfunction()

g1_dsp_replace(jitops.h
	"void op_Movem_aa(TWord op)\t\t\t\t{ errNotImplemented(op); }"
	"void op_Movem_aa(TWord op);")
g1_dsp_replace(jitops.h
	"void op_DoForever(TWord op)\t\t\t{ errNotImplemented(op); }"
	"void op_DoForever(TWord op);")
g1_dsp_replace(opcodeanalysis.h
	"case Movem_ea:\n\t\t\t{\n\t\t\t\tconst auto write = getFieldValue<Movem_ea, Field_W>(op);"
	"case Movem_aa:\n\t\t\treturn !getFieldValue<Movem_aa, Field_W>(op);\n\t\tcase Movem_ea:\n\t\t\t{\n\t\t\t\tconst auto write = getFieldValue<Movem_ea, Field_W>(op);")

# CMPM compares magnitudes without changing either operand. An accumulator source
# is a pooled register reference; alu_cmp() takes its absolute value in place.
# Copy that source to a temporary before the comparison (also for parallel moves).
g1_dsp_replace(jitops_alu.cpp
	"const auto r = decode_JJJ_read_56(JJJ, !D);\n\t\talu_cmp(D, r64(r.get()), true);"
	"auto r = decode_JJJ_read_56(JJJ, !D);\n\t\tr.toTemp();\n\t\talu_cmp(D, r64(r.get()), true);")

# DO FOREVER keeps LC. At the loop end, FV prevents decrementing LC or leaving.
g1_dsp_replace(jitblock.cpp
	"\t\t\tm_asm.cmp(lc, asmjit::Imm(1));\n\t\t\tm_asm.jle(enddo);\n\t\t\tm_asm.dec(lc);"
	"\t\t\tconst auto repeatForever = m_asm.newLabel();\n\t\t\tm_asm.bitTest(sr, SRB_FV);\n\t\t\tm_asm.jnz(repeatForever);\n\t\t\tm_asm.cmp(lc, asmjit::Imm(1));\n\t\t\tm_asm.jle(enddo);\n\t\t\tm_asm.dec(lc);\n\t\t\tm_asm.bind(repeatForever);")

# One iteration per block (what the G1 uses) made the mask of that test zero. AArch64 cannot
# encode an immediate of zero for TST: asmjit refused the instruction, the rest of the block
# was never emitted and the DSP ran into it, which is the illegal instruction on Apple
# Silicon. The test is always true with that mask, so the jump is unconditional.
g1_dsp_replace(jitblock.cpp
	"\t\t\tif(m_config.maxDoIterations)\n\t\t\t{\n\t\t\t\tassert(asmjit::Support::isPowerOf2(m_config.maxDoIterations));\n\t\t\t\tm_asm.test_(_regLC, asmjit::Imm(m_config.maxDoIterations-1));\n\t\t\t\tm_asm.jz(skip);\n\t\t\t}"
	"\t\t\tif(m_config.maxDoIterations > 1)\n\t\t\t{\n\t\t\t\tassert(asmjit::Support::isPowerOf2(m_config.maxDoIterations));\n\t\t\t\tm_asm.test_(_regLC, asmjit::Imm(m_config.maxDoIterations-1));\n\t\t\t\tm_asm.jz(skip);\n\t\t\t}\n\t\t\telse if(m_config.maxDoIterations)\n\t\t\t{\n\t\t\t\tm_asm.jmp(skip);\n\t\t\t}")

# Nested DO and ENDDO also save/restore FV, not only LF. One form for both architectures: every
# immediate here encodes as an AArch64 logical immediate, checked by cross-assembling the
# sequences with asmjit's arm64 backend on an x86 host (see NOTES.md, "The DSP JIT on ARM").
g1_dsp_replace(jitops.cpp
	"m_asm.or_(m_dspRegs.getSR(JitDspRegs::ReadWrite), asmjit::Imm(SR_LF));"
	"m_asm.and_(m_dspRegs.getSR(JitDspRegs::ReadWrite), asmjit::Imm(~SR_FV));\n\t\t\tm_asm.or_(m_dspRegs.getSR(JitDspRegs::ReadWrite), asmjit::Imm(SR_LF));")
g1_dsp_replace(jitops.cpp
	"m_asm.and_(r32(r), asmjit::Imm(SR_LF));"
	"m_asm.and_(r32(r), asmjit::Imm(SR_LF | SR_FV));")
g1_dsp_replace(jitops.cpp
	"m_asm.and_(r32(m_dspRegs.getSR(JitDspRegs::ReadWrite)), asmjit::Imm(~SR_LF));"
	"m_asm.and_(r32(m_dspRegs.getSR(JitDspRegs::ReadWrite)), asmjit::Imm(~(SR_LF | SR_FV)));")

# DMA with dual counters on source and destination at once (DAM = 011 011 in the G1's DMA0:
# copies X:$6C0 -> Y:output buffer on every block). Gearmulator does not implement it and in
# Release it reported the block done without copying: audio never passed from one DSP to the next.
# Both sides share DCOH/DCOL and each adds its DOR at the end of every line.
g1_dsp_replace(dma.cpp
	[=[		assert(false && "DMA transfer mode not supported yet");]=]
	[=[		if(agmS <= AddressGenMode::DualCounterDOR3 && agmD <= AddressGenMode::DualCounterDOR3)
		{
			const auto isLineTransfer = getTransferMode() == TransferMode::LineTriggerRequestClearDE;
			const auto dorS = m_dma.getDOR(static_cast<TWord>(agmS));
			const auto dorD = m_dma.getDOR(static_cast<TWord>(agmD));

			do
			{
				memWrite(areaD, m_ddr, memRead(areaS, m_dsr));

				m_dsr = (m_dcol == 0 ? m_dsr + dorS : m_dsr + 1) & 0xffffff;

				if(dualModeIncrement(m_ddr, dorD))
					return true;
			}
			while(!isRequestTrigger() || (isLineTransfer && m_dcol != m_dcolInit));

			return false;
		}

		assert(false && "DMA transfer mode not supported yet");]=])

# Block transfers (triggered by DE) happen immediately. Delayed, DMA0 copied the input to the
# output buffer after the voices had added their sample, and overwrote it.
# On the real DSP they run in parallel and finish much earlier (about 36 cycles).
g1_dsp_replace(dma.cpp
	"constexpr bool g_delayedDmaTransfer = true;"
	"constexpr bool g_delayedDmaTransfer = false;")

# DMA per request in block mode without clearing DE (DTM=100). The G1's DSP 0 brings in the
# left audio input this way: DMA3 moves ESSI1 RX to X:$6C5 (a 1-word block) and its interrupt
# (vector $1E) copies ESSI0 RX to X:$6C4. Gearmulator silently ignored it (it only accepted
# word or line). One request moves the whole block; with DE set, it stays armed.
g1_dsp_replace(dma.cpp
	"const auto isSupportedTransferMode = tm == TransferMode::WordTriggerRequest || tm == TransferMode::WordTriggerRequestClearDE || tm == TransferMode::LineTriggerRequestClearDE;"
	"const auto isSupportedTransferMode = tm == TransferMode::WordTriggerRequest || tm == TransferMode::WordTriggerRequestClearDE || tm == TransferMode::LineTriggerRequestClearDE || tm == TransferMode::BlockTriggerRequest || tm == TransferMode::BlockTriggerRequestClearDE;")
g1_dsp_replace(dma.cpp
	"		if(!bittest(m_dcr, De))\n			return;\n\n		if(execTransfer())\n			finishTransfer();"
	"		if(!bittest(m_dcr, De))\n			return;\n\n		const auto btm = getTransferMode();\n		if(btm == TransferMode::BlockTriggerRequest || btm == TransferMode::BlockTriggerRequestClearDE)\n		{\n			while(!execTransfer()) {}\n			finishTransfer();\n			return;\n		}\n\n		if(execTransfer())\n			finishTransfer();")

# DMA from a fixed address to a fixed address (DAM 100 100): a peripheral register to a memory
# cell. The G1's DSP 0 uses it for the audio inputs (ESSI1 RX -> X:$6C5). It had no branch:
# it fell into the final assert and, in Release, reported the block done without copying.
g1_dsp_replace(dma.cpp
	"		assert(false && \"DMA transfer mode not supported yet\");\n		return true;\n	}"
	"		if(agmS == AddressGenMode::SingleCounterAnoUpdate && agmD == AddressGenMode::SingleCounterAnoUpdate)\n		{\n			memWrite(areaD, m_ddr, memRead(areaS, m_dsr));\n			if(isRequestTrigger() && m_dco)\n			{\n				--m_dco;\n				return false;\n			}\n			m_dco = m_dcomInit;\n			return true;\n		}\n\n		assert(false && \"DMA transfer mode not supported yet\");\n		return true;\n	}")

# Observe ESSI clock catch-up, DMA3 requests and vector-$1E injection only in diagnostic builds.
if(G1_DSP_TRACE)
	# Capture JIT-resident registers before and after only the two relevant
	# straight-line producer ranges. The generated guard skips the probe outside
	# the post-note cycle window; no DSP register or simulated cycle is changed.
	g1_dsp_replace(jitblock.h
		"void markPeripheralAccess();"
		"void markPeripheralAccess();\n\t\tvoid g1TraceVoiceOp(TWord pc, bool after);")
	g1_dsp_replace(jitblock.cpp
		"#include \"opcodecycles.h\""
		"#include \"opcodecycles.h\"\n#include \"g1_output_trace.h\"")
	g1_dsp_replace(jitblock.cpp
		"void JitBlock::markPeripheralAccess()"
		[=[void callG1VoiceTrace(DSP* dsp, TWord pc, TWord after)
	{
		g1VoiceTraceEmit(*dsp, pc, after != 0);
		g1FlowTraceEmit(*dsp, pc, after != 0);
		g1CounterTraceEmit(*dsp, pc, after != 0);
	}

	void JitBlock::g1TraceVoiceOp(TWord pc, bool after)
	{
		const bool voice = g1VoiceTracePc(pc) && std::getenv("G1_DSP_VOICE_FILE");
		const bool flow = g1FlowTracePc(pc) && std::getenv("G1_DSP_FLOW_FILE");
		const bool counter = g1CounterTracePc(pc) && std::getenv("G1_DSP_COUNTER_FILE");
		if((!voice && !flow && !counter) ||
			g1OutputTrace().target.load(std::memory_order_acquire) != &m_dsp) return;

		const SkipLabel skip(m_asm);
		{
			const RegScratch pointer(*this), cycle(*this), bound(*this);
			m_asm.mov(r64(cycle), m_mem.makePtr(pointer, &m_dsp.getCycles(), sizeof(uint64_t)));
			m_asm.mov(r64(bound), asmjit::Imm(counter ? 211250000 : voice ? 236970000 : 236976000));
			m_asm.cmp(r64(bound), r64(cycle));
			m_asm.jg(skip.get());
			m_asm.mov(r64(bound), asmjit::Imm(counter ? 236977800 : voice ? 237070000 : 236979500));
			m_asm.cmp(r64(cycle), r64(bound));
			m_asm.jg(skip.get());
		}

		auto& snapshot = g1VoiceTrace().regs;
		{
			const auto value = m_dspRegs.getALU(0);
			m_mem.mov(snapshot.a, value.get());
		}
		{
			const auto value = m_dspRegs.getALU(1);
			m_mem.mov(snapshot.b, value.get());
		}
		{
			const RegGP value(*this);
			m_dspRegPool.getXY0(r32(value), 0);
			m_mem.mov(snapshot.x0, value.get());
			m_dspRegPool.getXY1(r32(value), 0);
			m_mem.mov(snapshot.x1, value.get());
			m_dspRegPool.getXY0(r32(value), 1);
			m_mem.mov(snapshot.y0, value.get());
			m_dspRegPool.getXY1(r32(value), 1);
			m_mem.mov(snapshot.y1, value.get());
		}
		{
			const auto value = m_dspRegs.getR(1);
			m_mem.mov(snapshot.r1, value.get());
		}
		{
			const auto value = m_dspRegs.getR(2);
			m_mem.mov(snapshot.r2, value.get());
		}
		{
			const auto value = m_dspRegs.getR(3);
			m_mem.mov(snapshot.r3, value.get());
		}
		{
			const auto value = m_dspRegs.getR(4);
			m_mem.mov(snapshot.r4, value.get());
		}
		{
			const auto value = m_dspRegs.getR(5);
			m_mem.mov(snapshot.r5, value.get());
		}
		{
			DspValue value(*this);
			m_dspRegs.getN(value, 1);
			m_mem.mov(snapshot.n1, value.get());
		}
		{
			const RegGP value(*this);
			m_asm.mov(r32(value), r32(m_dspRegs.getSR(JitDspRegs::ReadWrite)));
			m_mem.mov(snapshot.sr, value.get());
		}
		const FuncArg arg0(*this, 0), arg1(*this, 1), arg2(*this, 2);
		m_mem.makeDspPtr(arg0);
		m_asm.mov(r32(arg1), asmjit::Imm(pc));
		m_asm.mov(r32(arg2), asmjit::Imm(after ? 1 : 0));
		m_stack.call(asmjit::func_as_ptr(&callG1VoiceTrace));
	}

	void JitBlock::markPeripheralAccess()]=])
	g1_dsp_replace(jitblock.cpp
		"ops.emit(opPC, opA, opB);"
		"g1TraceVoiceOp(opPC, false);\n\t\t\tops.emit(opPC, opA, opB);\n\t\t\tg1TraceVoiceOp(opPC, true);")
	# Observe direct JIT Y writes at their original store site. The observer reads
	# the previous value and returns; the existing native store still executes.
	g1_dsp_replace(jitmem.h
		"Jitmem(JitBlock& _block) : m_block(_block) {}"
		"Jitmem(JitBlock& _block) : m_block(_block) {}\n\t\tvoid g1SetTracePc(TWord pc) { m_g1TracePc = pc; }")
	g1_dsp_replace(jitmem.h
		"JitBlock& m_block;"
		"void g1TraceYWrite(const JitRegGP& offset, const DspValue& src) const;\n\t\tTWord m_g1TracePc = 0;\n\t\tJitBlock& m_block;")
	g1_dsp_replace(jitops.cpp
		"m_pcCurrentOp = _pc;\n\t\tm_opWordA = _op;"
		"m_pcCurrentOp = _pc;\n\t\tm_block.mem().g1SetTracePc(_pc);\n\t\tm_opWordA = _op;")
	g1_dsp_replace(jitmem.cpp
		"#include \"jitregtracker.h\""
		"#include \"jitregtracker.h\"\n#include \"g1_output_trace.h\"")
	g1_dsp_replace(jitmem.cpp
		"void callDSPMemWrite(DSP* const _dsp, const EMemArea _area, const TWord _offset, const TWord _value)"
		[=[void callG1JitYWrite(DSP* dsp, TWord pc, TWord address, TWord value)
	{
		g1OutputTraceWrite(*dsp, "jit", pc, MemArea_Y, address, value);
	}

	void Jitmem::g1TraceYWrite(const JitRegGP& offset, const DspValue& src) const
	{
		if(!std::getenv("G1_DSP_OUTPUT_WRITES_FILE") ||
			g1OutputTrace().target.load(std::memory_order_acquire) != &m_block.dsp()) return;
		const FuncArg r0(m_block, 0), r1(m_block, 1), r2(m_block, 2), r3(m_block, 3);
		if(src.isImm24())
		{
			m_block.asm_().mov(r32(r2), r32(offset));
			m_block.asm_().mov(r32(r3), asmjit::Imm(src.imm24()));
		}
		else if(m_block.stack().isUsedFuncArg(offset) && m_block.stack().isUsedFuncArg(src.get()))
		{
			const RegScratch temp(m_block);
			m_block.asm_().mov(r32(temp), r32(src.get()));
			m_block.asm_().mov(r32(r2), r32(offset));
			m_block.asm_().mov(r32(r3), r32(temp));
		}
		else if(m_block.stack().isUsedFuncArg(src.get()))
		{
			m_block.asm_().mov(r32(r3), r32(src.get()));
			m_block.asm_().mov(r32(r2), r32(offset));
		}
		else
		{
			m_block.asm_().mov(r32(r2), r32(offset));
			m_block.asm_().mov(r32(r3), r32(src.get()));
		}
		makeDspPtr(r0);
		m_block.asm_().mov(r32(r1), asmjit::Imm(m_g1TracePc));
		m_block.stack().call(asmjit::func_as_ptr(&callG1JitYWrite));
	}

	void callDSPMemWrite(DSP* const _dsp, const EMemArea _area, const TWord _offset, const TWord _value)]=])
	g1_dsp_replace(jitmem.cpp
		"DspValue tempXY(m_block);\n\t\t\tauto p = getMemAreaPtr(tempXY, _area, _offset, std::move(_ref));"
		"if(_area == MemArea_Y) g1TraceYWrite(_offset, _src);\n\t\t\tDspValue tempXY(m_block);\n\t\t\tauto p = getMemAreaPtr(tempXY, _area, _offset, std::move(_ref));")
	g1_dsp_replace(jitmem.cpp
		"DspValue tempXY(m_block);\n\t\tauto px = getMemAreaPtr(tempXY, MemArea_X, _offset, noRef());"
		"g1TraceYWrite(_offset, _srcY);\n\t\tDspValue tempXY(m_block);\n\t\tauto px = getMemAreaPtr(tempXY, MemArea_X, _offset, noRef());")
	g1_dsp_replace(jitmem.cpp
		"auto p = getMemAreaPtr(_area, _offset, std::move(_ref), false);\n\t\twriteDspMemory(p, _src);"
		"if(_area == MemArea_Y)\n\t\t{\n\t\t\tconst RegGP address(m_block);\n\t\t\tm_block.asm_().mov(r32(address), asmjit::Imm(_offset));\n\t\t\tg1TraceYWrite(address.get(), _src);\n\t\t}\n\t\tauto p = getMemAreaPtr(_area, _offset, std::move(_ref), false);\n\t\twriteDspMemory(p, _src);")
	g1_dsp_replace(jitmem.cpp
		"p = getMemAreaPtr(MemArea_Y, _offset, std::move(p), false);\n\t\twriteDspMemory(p, _srcY);\n\t\treturn p;"
		"const RegGP address(m_block);\n\t\tm_block.asm_().mov(r32(address), asmjit::Imm(_offset));\n\t\tg1TraceYWrite(address.get(), _srcY);\n\t\tp = getMemAreaPtr(MemArea_Y, _offset, std::move(p), false);\n\t\twriteDspMemory(p, _srcY);\n\t\treturn p;")
	g1_dsp_replace(dma.cpp
		"auto& dsp = m_peripherals.getDSP();\n\t\tif (isPeripheralAddr(_area, _addr))\n\t\t\tdsp.getPeriph(_area)->write(_addr | 0xff0000, _value);"
		"auto& dsp = m_peripherals.getDSP();\n\t\tif(_area == MemArea_Y && g1OutputCell(_addr))\n\t\t\tg1OutputTraceWrite(dsp, \"dma\", dsp.getPC().toWord(), _area, _addr, _value,\n\t\t\t\tgetSourceSpace(), m_dsr, _value);\n\t\tif (isPeripheralAddr(_area, _addr))\n\t\t\tdsp.getPeriph(_area)->write(_addr | 0xff0000, _value);")
	# Separate the real peripheral callback from the JIT block that follows it.
	# These calls and the queue accessor exist only in the diagnostic build copy.
	g1_dsp_replace(dsp.h
		"void\texecInterrupts\t\t\t\t\t();"
		"void\tg1TraceCallbackWindowState(const char* event, TWord vector = 0, int result = -1);\n\t\tvoid\tg1MaybeArmFineDrain();\n\t\tvoid\tg1FineDrainInterruptCleared(TWord vector);\n\t\tvoid\tg1MaybeEndFineDrain();\n\t\tvoid\texecInterrupts\t\t\t\t\t();")
	g1_dsp_replace(dsp.h
		"bool\t\t\t\t\t\t\tm_invalidPCReported = false;"
		"bool\t\t\t\t\t\t\tm_invalidPCReported = false;\n\t\tbool\t\t\t\t\t\t\tm_g1FineDrainActive = false;\n\t\tuint32_t\t\t\t\t\t\tm_g1FineDrainRemaining = 0;")
	g1_dsp_replace(dsp.h
		"const auto delayA = static_cast<Ta*>(perif[0])->exec();"
		"g1TraceCallbackWindowState(\"callback_entry\");\n\t\t\tconst auto delayA = static_cast<Ta*>(perif[0])->exec();")
	g1_dsp_replace(dsp.h
		"processExternalInterrupts();\n\t\t}"
		"processExternalInterrupts();\n\t\t\tg1TraceCallbackWindowState(\"callback_exit\");\n\t\t\tg1MaybeArmFineDrain();\n\t\t}")
	g1_dsp_replace(esaiclock.h
		"auto getLastClock() const { return m_lastClock; }"
		"auto getLastClock() const { return m_lastClock; }\n\t\tuint64_t g1TraceFineClock(const Esxi* esxi) const\n\t\t{\n\t\t\tfor(const auto& entry : m_esais)\n\t\t\t\tif(entry.esai == esxi) return entry.fineLastClock;\n\t\t\treturn 0;\n\t\t}")
	g1_dsp_replace(dsp.cpp
		"#include \"opcodecycles.h\""
		"#include \"opcodecycles.h\"\n#include \"peripherals.h\"\n#include \"g1_essi_trace.h\"")
	g1_dsp_replace(dsp.cpp
		"void DSP::execInterrupts()"
		[=[void DSP::g1TraceCallbackWindowState(const char* event, TWord vector, int result)
	{
		g1CallbackWindowSnapshot(event, *this, m_pendingInterrupts, m_pendingExternalInterrupts, vector, result);
	}

	// The callback has returned and no JIT block is executing. Rebuild the cache
	// only for this exact DSP0 runtime occurrence; earlier compiled blocks ran at 32.
	void DSP::g1MaybeArmFineDrain()
	{
		static const char* enabled = std::getenv("G1_DSP_FINE_DRAIN_FILE");
		if(!enabled || !*enabled || m_g1FineDrainActive || m_cycles != 211258189 ||
			getPC().toWord() != 0x16e || m_instructions != 89708861 ||
			g1EssiTimeline().target.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(this) ||
			m_processingMode != Default || m_pendingInterrupts.size() != 7)
			return;
		for(size_t i = 0; i < 7; ++i)
			if(m_pendingInterrupts[i] != 0x1e) return;
		m_g1FineDrainActive = true;
		m_g1FineDrainRemaining = 7;
		auto config = m_jit.getConfig();
		config.maxInstructionsPerBlock = 1;
		m_jit.setConfig(config);
		m_jit.destroyAllBlocks();
		g1FineDrainEvent("arm", *this, m_g1FineDrainRemaining, 1);
	}

	void DSP::g1FineDrainInterruptCleared(TWord vector)
	{
		if(!m_g1FineDrainActive || vector != 0x1e || !m_g1FineDrainRemaining) return;
		--m_g1FineDrainRemaining;
		g1FineDrainEvent("clear", *this, m_g1FineDrainRemaining, 1);
	}

	// The last fast handler has completed and the ordinary suppression transition
	// has returned to the dispatcher. Restore cached 32-instruction blocks here.
	void DSP::g1MaybeEndFineDrain()
	{
		if(!m_g1FineDrainActive || m_g1FineDrainRemaining ||
			m_processingMode != Default) return;
		auto config = m_jit.getConfig();
		config.maxInstructionsPerBlock = 32;
		m_jit.setConfig(config);
		m_jit.destroyAllBlocks();
		m_g1FineDrainActive = false;
		g1FineDrainEvent("restore", *this, 0, 32);
	}

	void DSP::execInterrupts()]=])
	g1_dsp_replace(dsp.cpp
		"m_pendingInterrupts.push_back({_interruptVectorAddress});"
		"g1TraceCallbackWindowState(\"interrupt_enqueue_pre\", _interruptVectorAddress);\n\t\tm_pendingInterrupts.push_back({_interruptVectorAddress});\n\t\tg1TraceCallbackWindowState(\"interrupt_enqueue_post\", _interruptVectorAddress);")
	g1_dsp_replace(dsp.cpp
		"// it is important that the processing mode is switched first before popping the vector to prevent a possible race condition in hasPendingInterrupt()"
		"g1TraceCallbackWindowState(\"interrupt_accept_pre\", vba);\n\t\t// it is important that the processing mode is switched first before popping the vector to prevent a possible race condition in hasPendingInterrupt()")
	g1_dsp_replace(dsp.cpp
		"if(isInterruptMasked(vba))\n\t\t{"
		"if(isInterruptMasked(vba))\n\t\t{\n\t\t\tg1TraceCallbackWindowState(\"interrupt_masked\", vba);")
	g1_dsp_replace(dsp.cpp
		"m_processingMode = FastInterrupt;\n\t\t\tm_pendingInterrupts.pop_front();"
		"m_processingMode = FastInterrupt;\n\t\t\tm_pendingInterrupts.pop_front();\n\t\t\tg1TraceCallbackWindowState(\"interrupt_clear\", vba);\n\t\t\tg1FineDrainInterruptCleared(vba);")
	g1_dsp_replace(dsp.cpp
		"void DSP::execDefaultPreventInterrupt()\n\t{\n\t\tm_processingMode = Default;"
		"void DSP::execDefaultPreventInterrupt()\n\t{\n\t\tm_processingMode = Default;\n\t\tg1MaybeEndFineDrain();")
	g1_dsp_replace(dsp.cpp
		"m_processingMode = Default;\n\t\t\t\tm_pendingInterrupts.pop_front();"
		"m_processingMode = Default;\n\t\t\t\tm_pendingInterrupts.pop_front();\n\t\t\t\tg1TraceCallbackWindowState(\"interrupt_custom_clear\", interrupt);")
	g1_dsp_replace(dsp.cpp
		"\t\texecInterrupt(vba);\n\t}"
		"\t\texecInterrupt(vba);\n\t\tg1TraceCallbackWindowState(\"interrupt_accept_post\", vba);\n\t}")
	g1_dsp_replace(dsp.cpp
		"\t\texecInterrupt(_interruptVectorAddress);\n\n\t\twhile(m_processingMode != Default)"
		"\t\tg1TraceCallbackWindowState(\"interrupt_immediate_accept_pre\", _interruptVectorAddress);\n\t\texecInterrupt(_interruptVectorAddress);\n\t\tg1TraceCallbackWindowState(\"interrupt_immediate_accept_post\", _interruptVectorAddress);\n\n\t\twhile(m_processingMode != Default)")
	g1_dsp_replace(dsp.cpp
		"while(!m_pendingExternalInterrupts.empty())\n\t\t\tinjectInterrupt(m_pendingExternalInterrupts.pop_front());"
		"while(!m_pendingExternalInterrupts.empty())\n\t\t{\n\t\t\tconst auto vector = m_pendingExternalInterrupts.front();\n\t\t\tg1TraceCallbackWindowState(\"external_clear_pre\", vector);\n\t\t\tm_pendingExternalInterrupts.pop_front();\n\t\t\tg1TraceCallbackWindowState(\"external_clear_post\", vector);\n\t\t\tinjectInterrupt(vector);\n\t\t}")
	g1_dsp_replace(dma.cpp
		"#include \"interrupts.h\""
		"#include \"interrupts.h\"\n#include \"g1_dma_trace.h\"\n#include \"g1_output_trace.h\"")
	g1_dsp_replace(dma.cpp
		"void DmaChannel::triggerByRequest()\n\t{\n\t\tif(!bittest(m_dcr, De))"
		"void DmaChannel::triggerByRequest()\n\t{\n\t\tg1TraceDma3(\"request_enter\", m_peripherals, m_index, m_dcr, m_dsr, m_ddr, m_dco, m_dma.getDSTR());\n\t\tif(!bittest(m_dcr, De))")
	g1_dsp_replace(dma.cpp
		"\t\tif(bitvalue(m_dcr, Die))\n\t\t\tm_peripherals.getDSP().injectInterrupt(Vba_DMAchannel0 + (m_index<<1));"
		"\t\tif(bitvalue(m_dcr, Die))\n\t\t{\n\t\t\tg1TraceDma3(\"enqueue_pre\", m_peripherals, m_index, m_dcr, m_dsr, m_ddr, m_dco, m_dma.getDSTR());\n\t\t\tconst auto injected = m_peripherals.getDSP().injectInterrupt(Vba_DMAchannel0 + (m_index<<1));\n\t\t\tg1TraceDma3(\"enqueue_post\", m_peripherals, m_index, m_dcr, m_dsr, m_ddr, m_dco, m_dma.getDSTR(), injected ? 1 : 0);\n\t\t}")
	g1_dsp_replace(esaiclock.cpp
		"#include \"peripherals.h\""
		"#include \"peripherals.h\"\n#include \"g1_essi_trace.h\"")
	g1_dsp_replace(esaiclock.cpp
		"const auto diff = ic - m_lastClock;"
		"const auto diff = ic - m_lastClock;\n\tg1TraceEssiClock(\"clock_enter\", m_periph, ic, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, 0, -1, -1, -1, 0, 0, m_hasFineEsais, m_nextCycleDeadline);")
	g1_dsp_replace(esaiclock.cpp
		"m_nextCycleDeadline = _delay;"
		"m_nextCycleDeadline = _delay;\n\t\t\tg1TraceEssiClock(\"clock_schedule\", m_periph, *m_dspInstructionCounter, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, 0, -1, -1, -1, 0, 0, m_hasFineEsais, m_nextCycleDeadline);")
	g1_dsp_replace(esaiclock.cpp
		"m_nextCycleDeadline = 0;\n\t\t\treturn 0;"
		"m_nextCycleDeadline = 0;\n\t\t\tg1TraceEssiClock(\"clock_immediate\", m_periph, *m_dspInstructionCounter, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, 0, -1, -1, -1, 0, 0, m_hasFineEsais, m_nextCycleDeadline);\n\t\t\treturn 0;")
	g1_dsp_replace(esaiclock.cpp
		"e.fineLastClock += e.finePeriod;"
		"g1TraceEssiClock(\"fine_tick_pre\", m_periph, ic, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, reinterpret_cast<uintptr_t>(e.esai), e.rx.counter, e.rx.divider, -1, e.finePeriod, e.fineLastClock, m_hasFineEsais, m_nextCycleDeadline);\n\t\t\t\te.fineLastClock += e.finePeriod;\n\t\t\t\tg1TraceEssiClock(\"fine_tick_post\", m_periph, ic, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, reinterpret_cast<uintptr_t>(e.esai), e.rx.counter, e.rx.divider, -1, e.finePeriod, e.fineLastClock, m_hasFineEsais, m_nextCycleDeadline);")
	g1_dsp_replace(esaiclock.cpp
		"e.esai->execRX();"
		"g1TraceEssiClock(\"fine_rx_pre\", m_periph, ic, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, reinterpret_cast<uintptr_t>(e.esai), e.rx.counter, e.rx.divider, -1, e.finePeriod, e.fineLastClock, m_hasFineEsais, m_nextCycleDeadline);\n\t\t\t\t\t\te.esai->execRX();\n\t\t\t\t\t\tg1TraceEssiClock(\"fine_rx_post\", m_periph, ic, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, reinterpret_cast<uintptr_t>(e.esai), e.rx.counter, e.rx.divider, -1, e.finePeriod, e.fineLastClock, m_hasFineEsais, m_nextCycleDeadline);")
	g1_dsp_replace(esaiclock.cpp
		"m_lastClock += m_cyclesPerSample;"
		"m_lastClock += m_cyclesPerSample;\n\tg1TraceEssiClock(\"clock_advance\", m_periph, *m_dspInstructionCounter, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, 0, -1, -1, -1);")
	g1_dsp_replace(esaiclock.cpp
		"if (e.esai->hasEnabledReceivers() && advanceClock(e.rx))"
		"g1TraceEssiClock(\"rx_pre\", m_periph, *m_dspInstructionCounter, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, reinterpret_cast<uintptr_t>(e.esai), e.rx.counter, e.rx.divider, rxCount);\n\t\t\tif (e.esai->hasEnabledReceivers() && advanceClock(e.rx))")
	g1_dsp_replace(esaiclock.cpp
		"processRx[rxCount++] = e.esai;"
		"processRx[rxCount++] = e.esai;\n\t\tg1TraceEssiClock(\"rx_post\", m_periph, *m_dspInstructionCounter, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, reinterpret_cast<uintptr_t>(e.esai), e.rx.counter, e.rx.divider, rxCount);")
	g1_dsp_replace(esaiclock.cpp
		"for(size_t i=0; i<rxCount; ++i) processRx[i]->execRX();"
		"g1TraceEssiClock(\"rx_dispatch\", m_periph, *m_dspInstructionCounter, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, 0, -1, -1, rxCount);\n\tfor(size_t i=0; i<rxCount; ++i) processRx[i]->execRX();")
	g1_dsp_replace(esaiclock.cpp
		"for(size_t i=0; i<rxCount; ++i) processRx[i]->execRX();"
		"for(size_t i=0; i<rxCount; ++i)\n\t{\n\t\tg1TraceEssiClock(\"base_rx_pre\", m_periph, *m_dspInstructionCounter, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, reinterpret_cast<uintptr_t>(processRx[i]), -1, -1, rxCount);\n\t\tprocessRx[i]->execRX();\n\t\tg1TraceEssiClock(\"base_rx_post\", m_periph, *m_dspInstructionCounter, m_lastClock, m_cyclesPerSample, static_cast<uint32_t>(m_clockSource), -1, reinterpret_cast<uintptr_t>(processRx[i]), -1, -1, rxCount);\n\t}")
endif()

foreach(source IN LISTS g1_dsp_files)
	get_filename_component(name "${source}" NAME)
	configure_file("${g1_dsp_prepare}/${name}" "${g1_dsp_overlay}/${name}" COPYONLY)
endforeach()

get_target_property(g1_dsp_sources dsp56kEmu SOURCES)
set(g1_dsp_build_sources)
foreach(source IN LISTS g1_dsp_sources)
	get_filename_component(name "${source}" NAME)
	if(EXISTS "${g1_dsp_overlay}/${name}")
		list(APPEND g1_dsp_build_sources "${g1_dsp_overlay}/${name}")
	else()
		list(APPEND g1_dsp_build_sources "${source}")
	endif()
endforeach()
set_property(TARGET dsp56kEmu PROPERTY SOURCES "${g1_dsp_build_sources}")
list(FIND g1_dsp_build_sources "${g1_dsp_overlay}/jitops_alu.cpp" g1_cmpm_source_index)
if(g1_cmpm_source_index EQUAL -1)
	message(FATAL_ERROR "CMPM correction is not in the dsp56kEmu source list")
endif()
if(G1_DSP_TRACE)
	foreach(name IN ITEMS dsp.cpp dma.cpp esaiclock.cpp jitmem.cpp jitops.cpp jitblock.cpp)
		list(FIND g1_dsp_build_sources "${g1_dsp_overlay}/${name}" g1_trace_source_index)
		if(g1_trace_source_index EQUAL -1)
			message(FATAL_ERROR "${name} diagnostic overlay is not in the dsp56kEmu source list")
		endif()
	endforeach()
endif()
message(STATUS "G1 DSP CMPM correction: ${g1_dsp_overlay}/jitops_alu.cpp")
target_include_directories(dsp56kEmu BEFORE PUBLIC "${CMAKE_BINARY_DIR}/g1-dsp")
target_include_directories(dsp56kEmu PRIVATE "${g1_dsp_source}")
if(G1_DSP_TRACE)
	target_include_directories(dsp56kEmu PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../g1Lib")
endif()
target_sources(dsp56kEmu PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../g1Lib/dsp56300.cpp")
