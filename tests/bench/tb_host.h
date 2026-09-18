/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_FPGA_TB_HOST_H_
#define _TESTS_FPGA_TB_HOST_H_

#include "tb_stage.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/* The slot ids are declared in data.json. */
#define TB_HOST_SLOT_ROM 0u
#define TB_HOST_SLOT_FONTS 9u
#define TB_HOST_SLOT_OEMCP 10u

#define TB_HOST_GETFILE_STRUCT 256u

/* The firmware writes Open File's parameter struct through the file window
 * at 0x1000: a 256-byte path, then the flags and size words. */
#define TB_HOST_FILE_WIN 0x1000u
#define TB_HOST_OPEN_NAME 256u

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
    uint8_t open_name_[TB_HOST_OPEN_NAME] = {};

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
        default:
            /* fs_win_put packs the path into each word high byte first. */
            if (addr >= TB_HOST_FILE_WIN
                && addr < TB_HOST_FILE_WIN + TB_HOST_OPEN_NAME)
                for (uint32_t i = 0; i < 4; i++)
                    open_name_[addr - TB_HOST_FILE_WIN + i] =
                        (uint8_t)(data >> (24 - 8 * i));
            break;
        }
    }

    std::string open_name() const
    {
        size_t n = 0;
        while (n < TB_HOST_OPEN_NAME && open_name_[n])
            n++;
        return std::string((const char *)open_name_, n);
    }

    uint32_t read(uint32_t addr) const
    {
        if (addr == 0x10)
            return err_ << 1; /* the busy and drain bits are never set */
        if (addr == 0x14)
            return result_;
        return 0;
    }

    void command(uint32_t op)
    {
        err_ = 0;
        switch (op)
        {
        case 4: /* DT: word 2n is slot n's id and word 2n+1 is its size */
            result_ = (id_ & 1) ? slot_size(id_ >> 1) : (id_ >> 1);
            break;
        case 1: /* READ */
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
        case 5: /* GETFILE */
        {
            /* The whole 256-byte response struct at bridge_ is written on
             * every Get File because the Pocket does the same. Measurements
             * on the device show that a Get File on a bound slot leaves its
             * path in the struct, and a Get File on an unbound slot
             * immediately after leaves an empty name there, so an empty
             * name means an unbound slot. */
            const std::string &s = (id_ == TB_HOST_SLOT_ROM) ? name_
                                                             : empty_;
            for (size_t i = 0; i < TB_HOST_GETFILE_STRUCT; i++)
                tb_stage_write(bridge_ + (uint32_t)i,
                               i < s.size() ? (uint8_t)s[i] : 0);
            break;
        }
        case 3: /* OPEN */
            err_ = (id_ == TB_HOST_SLOT_ROM && open_name() == name_) ? 0 : 3;
            break;
        default: /* WRITE, FLUSH */
            err_ = 5;
            break;
        }
    }

    static const std::string empty_;
};

const std::string tb_host::empty_;

template <class DUT>
static void tb_host_tick(DUT *dut, const std::vector<uint8_t> &rom)
{
    static tb_host host;
    host.bind(rom);
    host.tick(dut);
}

#endif /* _TESTS_FPGA_TB_HOST_H_ */
