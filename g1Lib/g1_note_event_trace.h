#pragma once

// Diagnostic-only, bounded note observations. This records existing state; it
// never calls the emulator dispatcher or changes a peripheral or DSP register.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <vector>

namespace g1
{
	class NoteEventTrace
	{
	public:
		bool active() const { return m_active.load(std::memory_order_relaxed); }

		void submit(const void* mc, const void* dsp, uint64_t uc, uint64_t cycle,
			const uint8_t* bytes, int size, int note, bool on)
		{
			std::lock_guard lock(m_mutex);
			if(!m_out.is_open())
			{
				const char* path = std::getenv("G1_VST_NOTE_EVENTS_FILE");
				if(!path || !*path) return;
				m_out.open(path, std::ios::out | std::ios::trunc);
				if(!m_out) return;
				m_out << "event\tid\tuc_cycle\tdsp_cycle\tpc\tnote\tdetail\n";
			}
			if(m_nextId >= 64) return; // At most 64 MIDI messages per run.
			if(m_mc && m_mc != mc) return; // One plugin instance per diagnostic file.
			m_mc = mc;
			m_dsp = dsp;
			const auto id = ++m_nextId;
			m_out << "submit\t" << id << '\t' << uc << '\t' << cycle << "\t\t" << note
				<< "\ton=" << on << ";bytes=";
			for(int i = 0; i < size; ++i)
			{
				if(i) m_out << ',';
				m_out << static_cast<unsigned>(bytes[i]);
				m_pending.push_back({id, bytes[i]});
			}
			m_out << '\n';
			if(note >= 0)
				m_windows.push_back({id, note, cycle, cycle + 120u * 864u * 96u});
			m_active.store(true, std::memory_order_relaxed);
		}

		void sciRead(const void* mc, uint64_t uc, uint32_t pc, uint8_t value)
		{
			if(!active()) return;
			std::lock_guard lock(m_mutex);
			if(mc != m_mc) return;
			if(m_pending.empty()) return;
			const auto byte = m_pending.front();
			m_pending.pop_front();
			m_out << "sci_read\t" << byte.id << '\t' << uc << "\t\t" << pc
				<< "\t\tvalue=" << static_cast<unsigned>(value)
				<< ";submitted=" << static_cast<unsigned>(byte.value) << '\n';
			for(auto& w : m_windows)
				if(w.id == byte.id) { ++w.sciReads; break; }
			if(m_pending.empty() && m_windows.empty())
				m_active.store(false, std::memory_order_relaxed);
		}

		void hostWord(const void* dsp, uint64_t cycle, uint32_t pc, uint32_t word)
		{
			if(!active()) return;
			std::lock_guard lock(m_mutex);
			if(dsp != m_dsp) return;
			for(auto& w : m_windows)
				if(cycle >= w.start && cycle <= w.end)
				{
					++w.hostWords;
					if(word == 31) ++w.hostWord31;
					if(w.hostWords <= 4 || (word == 31 && w.hostWord31 <= 8))
						m_out << "dsp0_host_word\t" << w.id << "\t\t" << cycle << '\t' << pc
						<< '\t' << w.note << "\tword=" << word << '\n';
				}
		}

		void boundary(const void* dsp, uint64_t cycle, uint32_t pc)
		{
			if(!active() || (pc != 0x172 && pc != 0x23e)) return;
			std::lock_guard lock(m_mutex);
			if(dsp != m_dsp) return;
			for(auto& w : m_windows)
				if(cycle >= w.start && cycle <= w.end)
				{
					++w.notePcEntries;
					if(w.notePcEntries <= 8)
						m_out << "dsp0_pc\t" << w.id << "\t\t" << cycle << '\t' << pc
							<< '\t' << w.note << "\tjit_boundary\n";
				}
		}

		void interrupt(const void* dsp, uint64_t cycle, uint32_t pc, uint32_t vector)
		{
			if(!active() || (vector != 0x7e && vector != 0x76)) return;
			std::lock_guard lock(m_mutex);
			if(dsp != m_dsp) return;
			for(auto& w : m_windows)
				if(cycle >= w.start && cycle <= w.end)
				{
					++w.hostInterrupts;
					if(w.hostInterrupts <= 8)
						m_out << "dsp0_interrupt\t" << w.id << "\t\t" << cycle << '\t' << pc
						<< '\t' << w.note << "\tvector=" << vector << '\n';
				}
		}

		void link(const void* dsp, uint64_t cycle, uint64_t block,
			uint32_t left, uint32_t right)
		{
			if(!active()) return;
			std::lock_guard lock(m_mutex);
			if(dsp != m_dsp) return;
			for(auto it = m_windows.begin(); it != m_windows.end();)
			{
				if(cycle > it->end)
				{
					m_out << "window_end\t" << it->id << "\t\t" << cycle << "\t\t"
						<< it->note << "\tsci_reads=" << it->sciReads
						<< ";host_words=" << it->hostWords << ";host_word31=" << it->hostWord31
						<< ";note_pc_entries=" << it->notePcEntries
						<< ";host_interrupts=" << it->hostInterrupts
						<< ";link_blocks=" << it->linkBlocks << ";link_peak=" << it->linkPeak
						<< ";first_nonzero_cycle=" << it->firstNonzero << '\n';
					m_out.flush();
					it = m_windows.erase(it);
					continue;
				}
				if(cycle >= it->start)
				{
					++it->linkBlocks;
					auto magnitude = [](uint32_t raw)
					{
						const auto signedValue = static_cast<int32_t>(raw << 8) >> 8;
						return static_cast<uint32_t>(signedValue < 0 ? -signedValue : signedValue);
					};
					it->linkPeak = std::max({it->linkPeak, magnitude(left), magnitude(right)});
					if(!it->firstNonzero && (left || right))
					{
						it->firstNonzero = cycle;
						m_out << "first_link\t" << it->id << "\t\t" << cycle << "\t\t"
							<< it->note << "\tblock=" << block << ";left=" << left
							<< ";right=" << right << '\n';
					}
				}
				++it;
			}
			if(m_windows.empty() && m_pending.empty())
				m_active.store(false, std::memory_order_relaxed);
		}

		void finish(const void* dsp, uint64_t cycle)
		{
			if(!active()) return;
			std::lock_guard lock(m_mutex);
			if(dsp != m_dsp) return;
			for(auto it = m_windows.begin(); it != m_windows.end();)
			{
				if(cycle <= it->end) { ++it; continue; }
				m_out << "window_end\t" << it->id << "\t\t" << cycle << "\t\t"
					<< it->note << "\tsci_reads=" << it->sciReads
					<< ";host_words=" << it->hostWords << ";host_word31=" << it->hostWord31
					<< ";note_pc_entries=" << it->notePcEntries
					<< ";host_interrupts=" << it->hostInterrupts
					<< ";link_blocks=" << it->linkBlocks << ";link_peak=" << it->linkPeak
					<< ";first_nonzero_cycle=" << it->firstNonzero << '\n';
				it = m_windows.erase(it);
			}
			m_out.flush();
			if(m_windows.empty() && m_pending.empty())
				m_active.store(false, std::memory_order_relaxed);
		}

	private:
		struct PendingByte { uint32_t id; uint8_t value; };
		struct Window
		{
			uint32_t id; int note; uint64_t start, end;
			uint32_t sciReads = 0, hostWords = 0, hostWord31 = 0, notePcEntries = 0;
			uint32_t hostInterrupts = 0;
			uint32_t linkBlocks = 0, linkPeak = 0;
			uint64_t firstNonzero = 0;
		};
		std::mutex m_mutex;
		std::atomic<bool> m_active{false};
		std::ofstream m_out;
		const void* m_mc = nullptr;
		const void* m_dsp = nullptr;
		uint32_t m_nextId = 0;
		std::deque<PendingByte> m_pending;
		std::vector<Window> m_windows;
	};

	inline NoteEventTrace& noteEventTrace()
	{
		static NoteEventTrace trace;
		return trace;
	}
}
