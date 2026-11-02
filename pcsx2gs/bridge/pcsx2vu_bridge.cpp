#include "pcsx2vu_bridge.h"

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

    Result run(const Run &r)
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
