#include "pcsx2vu_bridge.h"

#include <atomic>
#include <chrono>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "Common.h"
#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "Vif.h"
#include "GS/MultiISA.h"
#include "common/emitter/x86emitter.h"
#include "pcsx2vu_shim.h"
#include "pcsx2gs_host.h"

#include <cstdio>
#include <utility>
#include <xmmintrin.h>

namespace pcsx2vu
{
    namespace
    {
        Kick s_kick;
        bool s_reserved = false;
        u8 *s_code = nullptr;
        u8 *s_data = nullptr;
        u64 s_generation = 0;

        void kick(const u8 *packet, u32 size, void *)
        {
            if (s_kick)
                s_kick(packet, size);
        }

        // microVU compiles VU1 memory's address into its code, so a new
        // memory means a fresh cache.
        void bind(u8 *code, u8 *data)
        {
            VU1.Mem = data;
            VU1.Micro = code;
            VU1.idx = 1;
            // VF0 reads (0, 0, 0, 1), as PCSX2's VU reset leaves it.
            VU1.VF[0].f.x = 0.0f;
            VU1.VF[0].f.y = 0.0f;
            VU1.VF[0].f.z = 0.0f;
            VU1.VF[0].f.w = 1.0f;
            VU1.VI[0].UL = 0;
            if (!s_reserved)
            {
                // As PCSX2's VMManager sets it before any recompiler runs:
                // microVU picks AVX forms from g_cpu, the emitter from this.
                x86Emitter::use_avx = g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX;
                CpuMicroVU1.Reserve();
                s_reserved = true;
            }
            CpuMicroVU1.Reset();
            s_code = code;
            s_data = data;
        }
    }

    void configure(const Settings &settings, Kick kick_)
    {
        s_kick = std::move(kick_);
        pcsx2vu_shim::setKick(&kick, nullptr);
        EmuConfig.Cpu.VU1FPCR.SetRoundMode(static_cast<FPRoundMode>(settings.roundMode & 3));
    }

    uint8_t *memory(bool code)
    {
        u8 *near = pcsx2NearCodeData();
        return near ? near + (code ? 0x4000 : 0) : nullptr;
    }

    namespace
    {
        // Timed in TSC ticks (a thousand programs a frame: the clock call
        // itself was costing more than many of them), turned into time
        // with a rate measured against the steady clock as the game runs.
        std::atomic<uint32_t> s_runs{0};
        std::atomic<uint64_t> s_runTicks{0};
        std::atomic<uint64_t> s_longestTicks{0};
        std::atomic<uint32_t> s_slowRuns{0};
        std::atomic<uint32_t> s_clears{0};
        uint64_t s_calibTicks = 0;
        std::chrono::steady_clock::time_point s_calibTime;
        double s_nsPerTick = 0.0;
    }

    Timing takeTiming()
    {
        const uint64_t ticks = __rdtsc();
        const auto now = std::chrono::steady_clock::now();
        if (s_calibTicks == 0)
        {
            s_calibTicks = ticks;
            s_calibTime = now;
        }
        else if (s_nsPerTick == 0.0 && now - s_calibTime >= std::chrono::milliseconds(500) && ticks > s_calibTicks)
        {
            s_nsPerTick = std::chrono::duration<double, std::nano>(now - s_calibTime).count() /
                          static_cast<double>(ticks - s_calibTicks);
        }
        Timing t;
        t.runs = s_runs.exchange(0);
        t.totalNs = static_cast<uint64_t>(static_cast<double>(s_runTicks.exchange(0)) * s_nsPerTick);
        t.longestNs = static_cast<uint64_t>(static_cast<double>(s_longestTicks.exchange(0)) * s_nsPerTick);
        t.slowRuns = s_slowRuns.exchange(0);
        t.clears = s_clears.exchange(0);
        return t;
    }

    static Result runProgram(const Run &r);

    Result run(const Run &r)
    {
        const uint64_t t0 = __rdtsc();
        const Result result = runProgram(r);
        const uint64_t ticks = __rdtsc() - t0;
        // The EE's thread runs VU1 and takes the timing: plain increments.
        s_runs.store(s_runs.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        s_runTicks.store(s_runTicks.load(std::memory_order_relaxed) + ticks, std::memory_order_relaxed);
        if (ticks > s_longestTicks.load(std::memory_order_relaxed))
            s_longestTicks.store(ticks, std::memory_order_relaxed);
        if (s_nsPerTick > 0.0 && static_cast<double>(ticks) * s_nsPerTick > 1e6)
            s_slowRuns.store(s_slowRuns.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        return result;
    }

    static Result runProgram(const Run &r)
    {
        Result result;
        if (!r.code || !r.data)
            return result;
        if (r.code != s_code || r.data != s_data)
        {
            bind(r.code, r.data);
            s_generation = r.codeGeneration;
        }
        else if (r.codeGeneration != s_generation)
        {
            // As PCSX2 does after VIF MPG: the next start looks the program up again.
            CpuMicroVU1.Clear(0, 0x4000);
            s_generation = r.codeGeneration;
            s_clears.store(s_clears.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        }

        vif1Regs.top = r.top & 0x3ff;
        vif1Regs.itop = r.itop & 0x3ff;
        VU0.VI[REG_FBRST].UL = r.fbrst;
        // vu1ExecMicro, without MTVU.
        VU0.VI[REG_VPU_STAT].UL = (VU0.VI[REG_VPU_STAT].UL & ~0xFF00u) | 0x0100u;
        if (!r.resume)
            VU1.VI[REG_TPC].UL = (r.startPC >> 3) & 0x7FF;
        CpuMicroVU1.SetStartPC(VU1.VI[REG_TPC].UL << 3);

        // microVU switches MXCSR to VU1's rounding while it runs and back to
        // the EE FPU's on exit; the host's own setting is kept around it.
        const unsigned int mxcsr = _mm_getcsr();
        const u64 startCycle = VU1.cycle;
        // vu1Finish's loop: a program runs until it ends, in slices.
        for (int slice = 0; (VU0.VI[REG_VPU_STAT].UL & 0x100) && slice < 8; ++slice)
            CpuMicroVU1.Execute(vu1RunCycles);
        _mm_setcsr(mxcsr);
        if (VU0.VI[REG_VPU_STAT].UL & 0x100)
        {
            std::fprintf(stderr, "[pcsx2 vu] force stopping VU1, ran for too long (pc %04x)\n",
                         VU1.VI[REG_TPC].UL << 3);
            VU0.VI[REG_VPU_STAT].UL &= ~0x100u;
        }
        pcsx2vu_shim::takeInterrupts();
        result.vpuStat = VU0.VI[REG_VPU_STAT].UL & 0x0600u;
        result.cycles = VU1.cycle - startCycle;
        return result;
    }
}
