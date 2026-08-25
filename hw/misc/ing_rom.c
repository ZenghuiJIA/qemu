/*
 * Ingchips ROM trap device + patch helpers
 *
 * Two families (ING916, ING20) hardcode Mask-ROM helpers as
 *   #define ROM_Foo ((f_t)0x0000xxxx)
 * This file provides:
 *   - a small MMIO trap at ING_TRAP_BASE (0x4003F000) with 16 slots
 *   - Thumb stubs placed at the hardcoded ROM entry points that
 *     spill r0-r3 to the trap window and reload r0 with the emulated
 *     result before bx lr.  Slots differentiate families.
 *   - behavioural stubs for flash (legal 0x02000000 range), CRC,
 *     status regs, RUID.  All other ROM helpers (cache flush,
 *     continuous-mode, power sequences) are intentional NOPs so
 *     guest can safely BL LR.
 *
 * Flash stubs operate only on the legal XIP window; out-of-range
 * requests return the SDK error value (1) and leave memory untouched.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/ing_rom.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "exec/hwaddr.h"

/* ------------------------------------------------------------------ */
/* CRC-16 tables from tools/icsdw.py (Modbus / CRC-16/ARC style)       */
/* init 0xFFFF, poly 0xA001, used by host burn tools and OTA path.    */
/* We keep the exact same tables so host vs guest agree.               */

static const uint8_t auchCRCHi[256] = {
    0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
    0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
    0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
    0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
    0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
    0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
    0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
    0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
    0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
    0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
    0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
    0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
    0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
    0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
    0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
    0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40
};
static const uint8_t auchCRCLo[256] = {
    0x00,0xC0,0xC1,0x01,0xC3,0x03,0x02,0xC2,0xC6,0x06,0x07,0xC7,0x05,0xC5,0xC4,0x04,
    0xCC,0x0C,0x0D,0xCD,0x0F,0xCF,0xCE,0x0E,0x0A,0xCA,0xCB,0x0B,0xC9,0x09,0x08,0xC8,
    0xD8,0x18,0x19,0xD9,0x1B,0xDB,0xDA,0x1A,0x1E,0xDE,0xDF,0x1F,0xDD,0x1D,0x1C,0xDC,
    0x14,0xD4,0xD5,0x15,0xD7,0x17,0x16,0xD6,0xD2,0x12,0x13,0xD3,0x11,0xD1,0xD0,0x10,
    0xF0,0x30,0x31,0xF1,0x33,0xF3,0xF2,0x32,0x36,0xF6,0xF7,0x37,0xF5,0x35,0x34,0xF4,
    0x3C,0xFC,0xFD,0x3D,0xFF,0x3F,0x3E,0xFE,0xFA,0x3A,0x3B,0xFB,0x39,0xF9,0xF8,0x38,
    0x28,0xE8,0xE9,0x29,0xEB,0x2B,0x2A,0xEA,0xEE,0x2E,0x2F,0xEF,0x2D,0xED,0xEC,0x2C,
    0xE4,0x24,0x25,0xE5,0x27,0xE7,0xE6,0x26,0x22,0xE2,0xE3,0x23,0xE1,0x21,0x20,0xE0,
    0xA0,0x60,0x61,0xA1,0x63,0xA3,0xA2,0x62,0x66,0xA6,0xA7,0x67,0xA5,0x65,0x64,0xA4,
    0x6C,0xAC,0xAD,0x6D,0xAF,0x6F,0x6E,0xAE,0xAA,0x6A,0x6B,0xAB,0x69,0xA9,0xA8,0x68,
    0x78,0xB8,0xB9,0x79,0xBB,0x7B,0x7A,0xBA,0xBE,0x7E,0x7F,0xBF,0x7D,0xBD,0xBC,0x7C,
    0xB4,0x74,0x75,0xB5,0x77,0xB7,0xB6,0x76,0x72,0xB2,0xB3,0x73,0xB1,0x71,0x70,0xB0,
    0x50,0x90,0x91,0x51,0x93,0x53,0x52,0x92,0x96,0x56,0x57,0x97,0x55,0x95,0x94,0x54,
    0x9C,0x5C,0x5D,0x9D,0x5F,0x9F,0x9E,0x5E,0x5A,0x9A,0x9B,0x5B,0x99,0x59,0x58,0x98,
    0x88,0x48,0x49,0x89,0x4B,0x8B,0x8A,0x4A,0x4E,0x8E,0x8F,0x4F,0x8D,0x4D,0x4C,0x8C,
    0x44,0x84,0x85,0x45,0x87,0x47,0x46,0x86,0x82,0x42,0x43,0x83,0x41,0x81,0x80,0x40
};

static uint16_t ing_crc16(const uint8_t *data, uint32_t len)
{
    uint8_t hi = 0xFF, lo = 0xFF;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t idx = hi ^ data[i];
        hi = lo ^ auchCRCHi[idx];
        lo = auchCRCLo[idx];
    }
    return ((uint16_t)hi << 8) | lo;
}

/* ------------------------------------------------------------------ */
/* Flash helpers (legal window 0x02000000)                              */

static bool flash_legal(IngTrapState *s, uint32_t addr, uint32_t size)
{
    if (!s->flash_mr) {
        return false;
    }
    if (addr < s->flash_base) {
        return false;
    }
    if (size == 0) {
        return addr < s->flash_base + s->flash_size;
    }
    if (addr + size < addr) {
        return false;
    }
    if (addr + size > s->flash_base + s->flash_size) {
        return false;
    }
    return true;
}

static uint8_t *flash_host_ptr(IngTrapState *s, uint32_t gpa)
{
    if (!s->flash_mr || !flash_legal(s, gpa, 1)) {
        return NULL;
    }
    hwaddr off = gpa - s->flash_base;
    void *ptr = memory_region_get_ram_ptr(s->flash_mr);
    return (uint8_t *)ptr + off;
}

/* Read guest memory (buf,len) into host buffer; returns 0 on success */
static int guest_read(uint32_t gpa, uint8_t *dst, uint32_t len)
{
    MemTxResult r = address_space_read(&address_space_memory, gpa,
                                       MEMTXATTRS_UNSPECIFIED, dst, len);
    return r == MEMTX_OK ? 0 : -1;
}

/* Write len bytes from host to flash window (legal check inside) */
static int flash_host_write(IngTrapState *s, uint32_t dst, const uint8_t *src, uint32_t len)
{
    if (!flash_legal(s, dst, len)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ing-trap: flash write out of range 0x%08x len %u (base 0x%08x size 0x%x)\n",
                      dst, len, s->flash_base, s->flash_size);
        return 1;
    }
    uint8_t *hp = flash_host_ptr(s, dst);
    if (!hp) {
        return 1;
    }
    memcpy(hp, src, len);
    return 0;
}

static int flash_host_erase(IngTrapState *s, uint32_t dst, uint32_t size)
{
    if (!flash_legal(s, dst, size)) {
        return 1;
    }
    uint8_t *hp = flash_host_ptr(s, dst);
    if (!hp) {
        return 1;
    }
    memset(hp, 0xFF, size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-slot emulation.  Slots map 1:1 to ROM entry IDs.                   */

static uint32_t emulate_slot(IngTrapState *s, unsigned slot)
{
    uint32_t a0 = s->args[slot][0];
    uint32_t a1 = s->args[slot][1];
    uint32_t a2 = s->args[slot][2];
    uint32_t a3 = s->args[slot][3];

    switch (s->family) {
    case ING_TRAP_FAMILY_916: {
        switch (slot) {
        case 0: { /* crc: 0x1d21  r0=buf r1=len -> r0=crc */
            uint32_t len = a1 & 0xFFFFu;
            if (len == 0) {
                return 0xFFFFu;
            }
            uint8_t *tmp = g_try_malloc(len);
            if (!tmp) {
                return 0xFFFFu;
            }
            if (guest_read(a0, tmp, len) == 0) {
                uint16_t crc = ing_crc16(tmp, len);
                g_free(tmp);
                return (uint32_t)crc;
            }
            g_free(tmp);
            return 0xFFFFu;
        }
        case 1: /* boot_uart_init 0x3aa1 -> NOP */
            qemu_log_mask(LOG_GUEST_ERROR, "ing-trap(916): boot_uart_init NOP\n");
            return 0;
        case 2: { /* prog_flash 0x3b9b r0=dst r1=buf r2=size -> int */
            uint32_t dst = a0, buf = a1, sz = a2;
            if (!flash_legal(s, dst, sz) || (sz & 3u)) {
                return 1;
            }
            /* erase semantics: program_flash erases sectors first */
            /* SDK expects dst aligned to 4096 */
            if (dst & 0xFFFu) {
                return 1;
            }
            uint8_t *tmp = g_try_malloc(sz);
            if (!tmp) {
                return 1;
            }
            if (guest_read(buf, tmp, sz) != 0) {
                g_free(tmp);
                return 1;
            }
            /* erase covering sectors */
            uint32_t cur = dst;
            uint32_t remain = sz;
            uint8_t *p = tmp;
            while (remain) {
                uint32_t sec = cur & ~0xFFFu;
                (void)sec;
                /* erase whole sector if we cross it - simplest: erase 4K */
                uint32_t to_erase = 4096;
                if (cur % 4096 != 0) {
                    /* unaligned start should not happen due to check */
                    g_free(tmp);
                    return 1;
                }
                /* we defer erase to first write of sector; just memcopy */
                int r = flash_host_write(s, cur, p, remain > 4096 ? 4096 : remain);
                if (r) { g_free(tmp); return 1; }
                uint32_t chunk = remain > 4096 ? 4096 : remain;
                (void)to_erase;
                cur += chunk; p += chunk; remain -= chunk;
            }
            g_free(tmp);
            return 0;
        }
        case 3: { /* write_flash 0x3cff r0=dst r1=buf r2=size -> 0=ok 1=need erase 2=verify */
            uint32_t dst = a0, buf = a1, sz = a2;
            if (!flash_legal(s, dst, sz)) {
                return 1;
            }
            uint8_t *tmp = g_try_malloc(sz);
            if (!tmp) {
                return 1;
            }
            if (guest_read(buf, tmp, sz) != 0) {
                g_free(tmp);
                return 1;
            }
            /* check 1->0 only (flash can only clear bits) */
            uint8_t *hp = flash_host_ptr(s, dst);
            for (uint32_t i = 0; i < sz; i++) {
                if ((hp[i] & tmp[i]) != tmp[i]) {
                    g_free(tmp);
                    return 1;
                }
            }
            int r = flash_host_write(s, dst, tmp, sz);
            g_free(tmp);
            if (r) {
                return 1;
            }
            return 0;
        }
        case 4: { /* flash_do_update 0x1d73 r0=n r1=blocks r2=ram_buf */
            uint32_t n = a0;
            uint32_t blocks_gpa = a1;
            if (n == 0 || n > 64 || !flash_legal(s, 0x02000000, s->flash_size)) {
                return 1;
            }
            /* each block: {src,dst,size} 12 bytes */
            for (uint32_t i = 0; i < n; i++) {
                uint32_t entry[3];
                if (guest_read(blocks_gpa + i * 12, (uint8_t *)entry, 12) != 0) {
                    return 1;
                }
                uint32_t src = entry[0], dst = entry[1], sz = entry[2];
                if (!flash_legal(s, dst, sz) || !flash_legal(s, src, sz)) {
                    /* src may be in flash or SRAM? For XIP, src is flash too.
                     * Allow SRAM src: check if not flash-legal, try guest_read
                     * from any memory.
                     */
                    if (!flash_legal(s, src, sz)) {
                        /* still need to copy via guest_read */
                    }
                    if (!flash_legal(s, dst, sz)) {
                        return 1;
                    }
                }
                uint8_t *tmp = g_try_malloc(sz);
                if (!tmp) {
                    return 1;
                }
                if (guest_read(src, tmp, sz) != 0) {
                    g_free(tmp);
                    return 1;
                }
                int r = flash_host_write(s, dst, tmp, sz);
                g_free(tmp);
                if (r) {
                    return 1;
                }
            }
            return 0;
        }
        case 5: /* WaitBusyDown 0xb6d NOP */
        case 6: /* DisableCont 0x7c9 NOP */
        case 7: /* EnableCont 0x80d NOP */
        case 8: /* DCacheFlush 0x651 NOP */
            return 0;
        case 9: { /* ProgPage 0x3cd7 r0=addr r1=data r2=len */
            uint32_t addr = a0, data = a1, len = a2;
            if (len == 0 || len > 256 || !flash_legal(s, addr, len)) {
                return 1;
            }
            uint8_t *tmp = g_try_malloc(len);
            if (!tmp) {
                return 1;
            }
            if (guest_read(data, tmp, len) != 0) {
                g_free(tmp);
                return 1;
            }
            uint8_t *hp = flash_host_ptr(s, addr);
            for (uint32_t i = 0; i < len; i++) {
                if ((hp[i] & tmp[i]) != tmp[i]) { g_free(tmp); return 1; }
            }
            int r = flash_host_write(s, addr, tmp, len);
            g_free(tmp);
            return r ? 1 : 0;
        }
        case 10: /* SetStatus 0xb01 r0=val -> void, cache in trap */
            s->flash_status = (uint16_t)(a0 & 0xFFFFu);
            return 0;
        case 11: /* GetStatus 0x84d -> r0 */
            return (uint32_t)s->flash_status;
        case 12: { /* RUID 0xa41 r0=uid_ptr (4*32) */
            uint32_t uid_gpa = a0;
            uint32_t fake[4] = {
                0x916e0001u, 0x12345678u, 0x9ABCDEF0u, 0x0BADCAFEu,
            };
            MemTxResult r = address_space_write(&address_space_memory, uid_gpa,
                                                MEMTXATTRS_UNSPECIFIED,
                                                fake, 16);
            return r == MEMTX_OK ? 0 : 1;
        }
        case 13: /* PLLinUse 0xe21 NOP */
        case 14: /* PowerOnSeq 0x101d NOP */
        case 15: /* PowerDownSeq 0xf05 NOP */
            return 0;
        default:
            return 0;
        }
    }
    case ING_TRAP_FAMILY_20: {
        switch (slot) {
        case 0: { /* crc 0x1c89 */
            uint32_t len = a1 & 0xFFFFu;
            if (len == 0) {
                return 0xFFFFu;
            }
            uint8_t *tmp = g_try_malloc(len);
            if (!tmp) {
                return 0xFFFFu;
            }
            if (guest_read(a0, tmp, len) == 0) {
                uint16_t crc = ing_crc16(tmp, len);
                g_free(tmp);
                return (uint32_t)crc;
            }
            g_free(tmp);
            return 0xFFFFu;
        }
        case 1: /* boot_uart_init 0x15d9 NOP */
            return 0;
        case 2: { /* erase_flash_sector 0x8977 r0=addr -> 0 */
            uint32_t addr = a0;
            if (!flash_legal(s, addr, 4096)) {
                return 1;
            }
            addr &= ~0xFFFu;
            return flash_host_erase(s, addr, 4096) ? 1 : 0;
        }
        case 3: { /* program_flash 0x25de5 r0=dst r1=buf r2=size */
            uint32_t dst = a0, buf = a1, sz = a2;
            if (!flash_legal(s, dst, sz) || (dst & 0xFFFu)) {
                return 1;
            }
            uint8_t *tmp = g_try_malloc(sz);
            if (!tmp) {
                return 1;
            }
            if (guest_read(buf, tmp, sz) != 0) { g_free(tmp); return 1; }
            /* ING20 program_flash erases sector-by-sector internally */
            uint32_t cur = dst;
            uint8_t *p = tmp;
            uint32_t remain = sz;
            while (remain) {
                flash_host_erase(s, cur & ~0xFFFu, 4096);
                uint32_t chunk = remain > 4096 ? 4096 : remain;
                int r = flash_host_write(s, cur, p, chunk);
                if (r) { g_free(tmp); return 1; }
                cur += chunk; p += chunk; remain -= chunk;
            }
            g_free(tmp);
            return 0;
        }
        case 4: { /* write_flash 0x2bbe9 r0=dst r1=buf r2=size */
            uint32_t dst = a0, buf = a1, sz = a2;
            if (!flash_legal(s, dst, sz)) {
                return 1;
            }
            uint8_t *tmp = g_try_malloc(sz);
            if (!tmp) {
                return 1;
            }
            if (guest_read(buf, tmp, sz) != 0) { g_free(tmp); return 1; }
            uint8_t *hp = flash_host_ptr(s, dst);
            for (uint32_t i = 0; i < sz; i++) {
                if ((hp[i] & tmp[i]) != tmp[i]) { g_free(tmp); return 1; }
            }
            int r = flash_host_write(s, dst, tmp, sz);
            g_free(tmp);
            return r ? 1 : 0;
        }
        case 5: { /* flash_do_update 0x8eb9 */
            uint32_t n = a0, blocks_gpa = a1;
            if (n == 0 || n > 64) {
                return 1;
            }
            for (uint32_t i = 0; i < n; i++) {
                uint32_t entry[3];
                if (guest_read(blocks_gpa + i * 12, (uint8_t *)entry, 12) != 0) {
                    return 1;
                }
                uint32_t src = entry[0], dst = entry[1], sz = entry[2];
                if (!flash_legal(s, dst, sz)) {
                    return 1;
                }
                uint8_t *tmp = g_try_malloc(sz);
                if (!tmp) {
                    return 1;
                }
                if (guest_read(src, tmp, sz) != 0) { g_free(tmp); return 1; }
                int r = flash_host_write(s, dst, tmp, sz);
                g_free(tmp);
                if (r) {
                    return 1;
                }
            }
            return 0;
        }
        case 6: { /* FlashSectorErase 0x7a1 r0=info r1=addr */
            uint32_t info = a0, addr = a1;
            uint32_t sz = 4096;
            if (info == 0x81) {
                sz = 256; /* page */
            } else if (info == 0x20) {
                sz = 4096;
            }
            if (!flash_legal(s, addr, sz)) {
                return 1;
            }
            return flash_host_erase(s, addr & ~(sz - 1), sz) ? 1 : 0;
        }
        case 7: /* Disable 0x72d NOP */
        case 8: /* Enable 0x77d NOP */
            return 0;
        case 9: { /* PageProgram 0x7c9 r0=info r1=addr r2=data r3=len */
            uint32_t addr = a1, data = a2, len = a3;
            (void)a0;
            if (len == 0 || len > 256 || !flash_legal(s, addr, len)) {
                return 1;
            }
            uint8_t *tmp = g_try_malloc(len);
            if (!tmp) {
                return 1;
            }
            if (guest_read(data, tmp, len) != 0) { g_free(tmp); return 1; }
            uint8_t *hp = flash_host_ptr(s, addr);
            for (uint32_t i = 0; i < len; i++) {
                if ((hp[i] & tmp[i]) != tmp[i]) { g_free(tmp); return 1; }
            }
            int r = flash_host_write(s, addr, tmp, len);
            g_free(tmp);
            return r ? 1 : 0;
        }
        default:
            return 0;
        }
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* MMIO trap: 4K window, 16 slots x 16 bytes.                        */

static uint64_t ing_trap_read(void *opaque, hwaddr offset, unsigned size)
{
    IngTrapState *s = ING_TRAP(opaque);
    unsigned slot = (offset / ING_TRAP_SLOT_STRIDE) % ING_TRAP_SLOTS;
    unsigned off = offset % ING_TRAP_SLOT_STRIDE;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "ing-trap: bad read size %u off 0x%" HWADDR_PRIX "\n",
                      size, offset);
        return 0;
    }
    if (off != 0) {
        /* only base offset returns result; others mirror stored arg */
        return s->args[slot][off / 4];
    }
    uint32_t r = emulate_slot(s, slot);
    s->result[slot] = r;
    return r;
}

static void ing_trap_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IngTrapState *s = ING_TRAP(opaque);
    unsigned slot = (offset / ING_TRAP_SLOT_STRIDE) % ING_TRAP_SLOTS;
    unsigned off = offset % ING_TRAP_SLOT_STRIDE;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "ing-trap: bad write size %u off 0x%" HWADDR_PRIX "\n",
                      size, offset);
        return;
    }
    if (off < 16) {
        s->args[slot][off / 4] = (uint32_t)value;
    }
    /* NOP slots still capture args; side-effects happen on the final
     * ldr read, except SetStatus/RUID which could also trigger here.
     * We keep read-triggered model for uniformity.
     */
    if (s->family == ING_TRAP_FAMILY_916 && slot == 10 && off == 0) {
        /* SetStatus could be applied immediately */
        s->flash_status = (uint16_t)(value & 0xFFFFu);
    }
}

static const MemoryRegionOps ing_trap_ops = {
    .read = ing_trap_read,
    .write = ing_trap_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------ */
/* Thumb stub patching */

static const uint8_t trap_stub_tmpl[30] = {
    /* movw ip, #0xF000 ; movt ip, #0x4003 -> ip=0x4003F000 */
    0x4F, 0xF2, 0x00, 0x0C, 0xC4, 0xF2, 0x03, 0x0C,
    /* str.w r0, [ip, #0] */ 0xCC, 0xF8, 0x00, 0x00,
    /* str.w r1, [ip, #4] */ 0xCC, 0xF8, 0x04, 0x10,
    /* str.w r2, [ip, #8] */ 0xCC, 0xF8, 0x08, 0x20,
    /* str.w r3, [ip, #12]*/ 0xCC, 0xF8, 0x0C, 0x30,
    /* ldr.w r0, [ip, #0] */ 0xDC, 0xF8, 0x00, 0x00,
    /* bx lr */              0x70, 0x47,
};

static void patch_one(MemoryRegion *rom, uint32_t rom_addr, unsigned slot)
{
    void *ram = memory_region_get_ram_ptr(rom);
    hwaddr rom_size = memory_region_size(rom);
    if (rom_addr >= rom_size) {
        qemu_log_mask(LOG_GUEST_ERROR, "ing-rom patch: addr 0x%08x beyond rom 0x%" HWADDR_PRIX "\n",
                      rom_addr, rom_size);
        return;
    }
    uint32_t base_off = slot * ING_TRAP_SLOT_STRIDE;
    /* copy template */
    uint8_t stub[sizeof(trap_stub_tmpl)];
    memcpy(stub, trap_stub_tmpl, sizeof(stub));

    /* patch imm for each slot: second half of 4 str/ldr at byte offsets
     * 10,14,18,22,26 (little endian).  halfword = (Rt<<12)|imm12
     * For r0 base: Rt0, imm=base
     * r1: Rt1, imm=base+4, etc.
     * ldr r0 base: Rt0, imm=base
     */
    struct { unsigned off; unsigned rt; unsigned imm; } patches[] = {
        {10, 0, base_off + 0},
        {14, 1, base_off + 4},
        {18, 2, base_off + 8},
        {22, 3, base_off + 12},
        {26, 0, base_off + 0},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(patches); i++) {
        uint16_t half = (patches[i].rt << 12) | (patches[i].imm & 0xFFFu);
        stub[patches[i].off] = half & 0xFF;
        stub[patches[i].off + 1] = (half >> 8) & 0xFF;
    }
    uint8_t *dst = (uint8_t *)ram + rom_addr;
    /* stale bx lr fill is 0x70 0x47; overwrite */
    memcpy(dst, stub, sizeof(stub));
    /* pad remaining bytes up to next 4K? no - leave bx lr fill beyond stub */
    /* Ensure Thumb LSB is handled by caller: rom_addr is even, BL will set LSB. */
}

/* 916 table */
static const struct { uint32_t addr; unsigned slot; } rom916_map[] = {
    {0x00001d21u, 0}, {0x00003aa1u, 1}, {0x00003b9bu, 2}, {0x00003cFFu, 3},
    {0x00001d73u, 4}, {0x00000b6du, 5}, {0x000007c9u, 6}, {0x0000080du, 7},
    {0x00000651u, 8}, {0x00003cd7u, 9}, {0x00000b01u,10}, {0x0000084du,11},
    {0x00000a41u,12}, {0x00000e21u,13}, {0x0000101du,14}, {0x00000f05u,15},
};

/* 20 table */
static const struct { uint32_t addr; unsigned slot; } rom208_map[] = {
    {0x00001c89u, 0}, {0x000015d9u, 1}, {0x00008977u, 2}, {0x00025de5u, 3},
    {0x00002bbe9u,4}, {0x00008eb9u, 5}, {0x000007a1u, 6}, {0x0000072du, 7},
    {0x0000077du, 8}, {0x000007c9u, 9},
};

void ing_rom_patch_916(MemoryRegion *rom)
{
    for (unsigned i = 0; i < ARRAY_SIZE(rom916_map); i++) {
        patch_one(rom, rom916_map[i].addr & ~1u, rom916_map[i].slot);
    }
}

void ing_rom_patch_208(MemoryRegion *rom)
{
    for (unsigned i = 0; i < ARRAY_SIZE(rom208_map); i++) {
        patch_one(rom, rom208_map[i].addr & ~1u, rom208_map[i].slot);
    }
}

void ing_trap_set_flash(IngTrapState *s, MemoryRegion *flash)
{
    s->flash_mr = flash;
}

/* ------------------------------------------------------------------ */
/* QOM device */

static void ing_trap_init(Object *obj)
{
    IngTrapState *s = ING_TRAP(obj);
    memory_region_init_io(&s->iomem, obj, &ing_trap_ops, s,
                          TYPE_ING_TRAP, ING_TRAP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    s->family = ING_TRAP_FAMILY_916;
    s->flash_base = 0x02000000u;
    s->flash_size = 2 * MiB;
    s->flash_status = 0;
    memset(s->args, 0, sizeof(s->args));
    memset(s->result, 0, sizeof(s->result));
}

static const Property ing_trap_properties[] = {
    DEFINE_PROP_UINT32("family", IngTrapState, family, ING_TRAP_FAMILY_916),
};

static void ing_trap_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, ing_trap_properties);
}

static const TypeInfo ing_trap_info = {
    .name = TYPE_ING_TRAP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IngTrapState),
    .instance_init = ing_trap_init,
    .class_init = ing_trap_class_init,
};

static void ing_trap_register_types(void)
{
    type_register_static(&ing_trap_info);
}

type_init(ing_trap_register_types)
