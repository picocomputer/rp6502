/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host a machine test does not otherwise have. rp6502 routes its 0x8
 * window out to rp6502_host_*, and a platform puts a file bridge there —
 * pocket_file on the Pocket. A bench that boots the firmware has to
 * answer the same registers, because the ROM is a slot: the loader asks
 * the data table how long the image is before parsing it out of the
 * store, and a bench that answers nothing leaves it spinning on a
 * status that never clears.
 *
 * Only what the loader uses is modelled — the data table, a slot read
 * for exec's pull, and the name behind Get File. Open, write and flush
 * are the drive's, and tests/host/pocket has a fuller host for those.
 *
 * Commands complete in the cycle they are issued. Nothing here is trying
 * to be a timing model; pocket_file's own bench is where the deadlines
 * and the drain live.
 */

#ifndef _TESTS_FPGA_TB_HOST_H_
#define _TESTS_FPGA_TB_HOST_H_

#include "tb_stage.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/* Slot ids, which data.json declares and msc.h mirrors. */
#define TB_HOST_SLOT_ROM 0u
#define TB_HOST_SLOT_FONTS 9u
#define TB_HOST_SLOT_OEMCP 10u

/* get_dataslot_file_t: one 256-byte path, per Analogue's reference. */
#define TB_HOST_GETFILE_STRUCT 256u

class tb_host
{
public:
    void bind(const std::vector<uint8_t> &rom)
    {
        if (rom.data() == data_ && rom.size() == size_)
            return;
        data_ = rom.data();
        size_ = rom.size();
        tb_stage_clear();
    }

    /* One call per clk_sys edge, before eval. */
    template <class DUT> void tick(DUT *dut)
    {
        if (dut->wiring_host_stb && dut->wiring_host_we)
            write(dut->wiring_host_addr & 0x0FFFFFFFu, dut->wiring_host_wdata);
        dut->host_rdata = read(dut->wiring_host_addr & 0x0FFFFFFFu);
    }

private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    std::string name_ = "/Assets/rp6502/common/test.rp6502";
    uint32_t id_ = 0, off_ = 0, len_ = 0, bridge_ = 0, result_ = 0;
    uint32_t err_ = 0;

    uint32_t slot_size(uint32_t slot) const
    {
        if (slot == TB_HOST_SLOT_ROM)
            return (uint32_t)size_;
        if (slot == TB_HOST_SLOT_FONTS)
            return (uint32_t)tb_stage_fonts().size();
        if (slot == TB_HOST_SLOT_OEMCP)
            return (uint32_t)tb_stage_oemcp().size();
        return 0;
    }

    const uint8_t *slot_data(uint32_t slot, uint32_t *size) const
    {
        if (slot == TB_HOST_SLOT_ROM)
        {
            *size = (uint32_t)size_;
            return data_;
        }
        if (slot == TB_HOST_SLOT_FONTS)
        {
            *size = (uint32_t)tb_stage_fonts().size();
            return tb_stage_fonts().data();
        }
        if (slot == TB_HOST_SLOT_OEMCP)
        {
            *size = (uint32_t)tb_stage_oemcp().size();
            return tb_stage_oemcp().data();
        }
        *size = 0;
        return nullptr;
    }

    void write(uint32_t addr, uint32_t data)
    {
        switch (addr)
        {
        case 0x00: id_ = data; break;
        case 0x04: off_ = data; break;
        case 0x08: len_ = data; break;
        case 0x0C: bridge_ = data; break;
        case 0x10: command(data & 7u); break;
        default: break;
        }
    }

    uint32_t read(uint32_t addr) const
    {
        if (addr == 0x10)
            return err_ << 1; /* never busy, never draining */
        if (addr == 0x14)
            return result_;
        return 0;
    }

    void command(uint32_t op)
    {
        err_ = 0;
        switch (op)
        {
        case 4: /* DT: pairs of id and size, laid out by id */
            result_ = (id_ & 1) ? slot_size(id_ >> 1) : (id_ >> 1);
            break;
        case 1: /* READ into the slot's window */
        {
            uint32_t size;
            const uint8_t *p = slot_data(id_, &size);
            if (!p || off_ > size)
            {
                err_ = 5;
                break;
            }
            uint32_t n = size - off_;
            if (n > len_)
                n = len_;
            for (uint32_t i = 0; i < n; i++)
                tb_stage_write(bridge_ + i, p[off_ + i]);
            break;
        }
        case 5: /* GETFILE: the response struct, where BRIDGE points */
        {
            /* The whole 256 bytes, every time, blanked past the name --
             * which is what the device does. Measured: a Get File on a
             * bound slot leaves its path in the window, and a Get File
             * on an unbound slot immediately after leaves the window
             * empty. The host clears it rather than declining to write,
             * so the window's own contents say whether a slot is bound
             * and a reader needs nothing else to tell.
             *
             * Writing only the name plus its terminator, as this did,
             * modelled a host that leaves the rest alone -- under which
             * an unbound slot would answer with whatever the last ask
             * left there, and a bench could never catch a reader that
             * trusted it. */
            const std::string &s = (id_ == TB_HOST_SLOT_ROM) ? name_
                                                             : empty_;
            for (size_t i = 0; i < TB_HOST_GETFILE_STRUCT; i++)
                tb_stage_write(bridge_ + (uint32_t)i,
                               i < s.size() ? (uint8_t)s[i] : 0);
            break;
        }
        default: /* open, write, flush: the drive's, not the loader's */
            err_ = 5;
            break;
        }
    }

    static const std::string empty_;
};

const std::string tb_host::empty_;

/* One call per clk_sys edge, beside the staging read every clock loop
 * already does. The state is static because the loops are lambdas and a
 * host that was rebuilt each cycle would forget the window it just
 * filled. */
template <class DUT>
static void tb_host_tick(DUT *dut, const std::vector<uint8_t> &rom)
{
    static tb_host host;
    host.bind(rom);
    host.tick(dut);
}

#endif /* _TESTS_FPGA_TB_HOST_H_ */
