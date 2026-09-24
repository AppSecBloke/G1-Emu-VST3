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
	g1_dsp_replace(dma.cpp
		"#include \"interrupts.h\""
		"#include \"interrupts.h\"\n#include \"g1_dma_trace.h\"")
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
	list(FIND g1_dsp_build_sources "${g1_dsp_overlay}/esaiclock.cpp" g1_essi_trace_source_index)
	if(g1_essi_trace_source_index EQUAL -1)
		message(FATAL_ERROR "ESSI diagnostic overlay is not in the dsp56kEmu source list")
	endif()
endif()
message(STATUS "G1 DSP CMPM correction: ${g1_dsp_overlay}/jitops_alu.cpp")
target_include_directories(dsp56kEmu BEFORE PUBLIC "${CMAKE_BINARY_DIR}/g1-dsp")
target_include_directories(dsp56kEmu PRIVATE "${g1_dsp_source}")
if(G1_DSP_TRACE)
	target_include_directories(dsp56kEmu PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../g1Lib")
endif()
target_sources(dsp56kEmu PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../g1Lib/dsp56300.cpp")
