// Memory access via MEM-AP (AHB-AP typically), with selectable APSEL.

#include "target_mem.h"

#include "adiv5.h"

#define AP_CSW 0x00u // addr[3:2]=0
#define AP_TAR 0x04u // addr[3:2]=1
#define AP_DRW 0x0Cu // addr[3:2]=3

// AHB-AP CSW value: 32-bit, auto-increment, debug access
// CSW: [2:0]=SIZE, [5:4]=AddrInc, other bits implementation-specific.
// A common safe value: 0x23000000 (DBGSWENABLE etc.) tolerated by many MEM-APs.
#define CSW_SIZE_32          (2u)      // 32-bit
#define CSW_ADDRINC_SINGLE   (1u << 4) // increment by one item
#define CSW_DEFAULT          (0x23000000u)

static uint8_t g_memap_ap_sel = 0u;

void target_mem_set_ap(uint8_t ap_sel) { g_memap_ap_sel = ap_sel; }
uint8_t target_mem_get_ap(void) { return g_memap_ap_sel; }

static bool memap_set_csw_ap(uint8_t ap_sel, uint32_t csw)
{
    return adiv5_ap_write(ap_sel, AP_CSW, csw);
}

static bool memap_set_tar_ap(uint8_t ap_sel, uint32_t addr)
{
    return adiv5_ap_write(ap_sel, AP_TAR, addr);
}

static bool memap_read_drw_ap(uint8_t ap_sel, uint32_t *out)
{
    return adiv5_ap_read(ap_sel, AP_DRW, out);
}

static bool memap_write_drw_ap(uint8_t ap_sel, uint32_t v)
{
    return adiv5_ap_write(ap_sel, AP_DRW, v);
}

bool target_mem_read_word_ap(uint8_t ap_sel, uint32_t addr, uint32_t *out)
{
    if (!memap_set_csw_ap(ap_sel, CSW_DEFAULT | CSW_ADDRINC_SINGLE | CSW_SIZE_32)) {
        return false;
    }
    if (!memap_set_tar_ap(ap_sel, addr)) {
        return false;
    }
    return memap_read_drw_ap(ap_sel, out);
}

bool target_mem_write_word_ap(uint8_t ap_sel, uint32_t addr, uint32_t v)
{
    if (!memap_set_csw_ap(ap_sel, CSW_DEFAULT | CSW_ADDRINC_SINGLE | CSW_SIZE_32)) {
        return false;
    }
    if (!memap_set_tar_ap(ap_sel, addr)) {
        return false;
    }
    return memap_write_drw_ap(ap_sel, v);
}

bool target_mem_read_word(uint32_t addr, uint32_t *out)
{
    return target_mem_read_word_ap(g_memap_ap_sel, addr, out);
}

bool target_mem_write_word(uint32_t addr, uint32_t v)
{
    return target_mem_write_word_ap(g_memap_ap_sel, addr, v);
}

bool target_mem_read_bytes(uint32_t addr, uint8_t *buf, uint32_t len)
{
    while (len) {
        uint32_t aligned = addr & ~3u;
        uint32_t w       = 0;
        if (!target_mem_read_word(aligned, &w)) {
            return false;
        }
        for (uint32_t i = (addr & 3u); i < 4u && len; i++) {
            *buf++ = (uint8_t) ((w >> (8u * i)) & 0xFFu);
            addr++;
            len--;
        }
    }
    return true;
}

bool target_mem_write_bytes(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len) {
        uint32_t offset = addr & 3u;

        // Fast path: word-aligned write of 4+ bytes
        // Skip RMW since we're writing the entire word.
        // This avoids reading from volatile/side-effect registers.
        if (offset == 0 && len >= 4) {
            uint32_t w = (uint32_t) buf[0] |
                         ((uint32_t) buf[1] << 8) |
                         ((uint32_t) buf[2] << 16) |
                         ((uint32_t) buf[3] << 24);
            if (!target_mem_write_word(addr, w)) {
                return false;
            }
            buf += 4;
            addr += 4;
            len -= 4;
            continue;
        }

        // Slow path: unaligned or partial word - need RMW
        uint32_t aligned = addr & ~3u;
        uint32_t w       = 0;
        if (!target_mem_read_word(aligned, &w)) {
            return false;
        }

        for (uint32_t i = offset; i < 4u && len; i++) {
            uint32_t mask = 0xFFu << (8u * i);
            w             = (w & ~mask) | ((uint32_t) (*buf++) << (8u * i));
            addr++;
            len--;
        }
        if (!target_mem_write_word(aligned, w)) {
            return false;
        }
    }
    return true;
}
