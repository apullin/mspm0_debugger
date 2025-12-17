// Memory access via AHB-AP (AP#0 assumed)

#include "target_mem.h"

#include "adiv5.h"

#define APSEL_AHB 0u

#define AP_CSW 0x00u // addr[3:2]=0
#define AP_TAR 0x04u // addr[3:2]=1
#define AP_DRW 0x0Cu // addr[3:2]=3

// AHB-AP CSW value: 32-bit, auto-increment, debug access
// CSW: [2:0]=SIZE, [5:4]=AddrInc, other bits implementation-specific.
// A common safe value: 0x23000000 (DBGSWENABLE etc.) tolerated by many MEM-APs.
#define CSW_SIZE_32          (2u)      // 32-bit
#define CSW_ADDRINC_SINGLE   (1u << 4) // increment by one item
#define CSW_DEFAULT          (0x23000000u)

static bool memap_set_csw(uint32_t csw)
{
    return adiv5_ap_write(APSEL_AHB, AP_CSW, csw);
}

static bool memap_set_tar(uint32_t addr)
{
    return adiv5_ap_write(APSEL_AHB, AP_TAR, addr);
}

static bool memap_read_drw(uint32_t *out)
{
    return adiv5_ap_read(APSEL_AHB, AP_DRW, out);
}

static bool memap_write_drw(uint32_t v)
{
    return adiv5_ap_write(APSEL_AHB, AP_DRW, v);
}

bool target_mem_read_word(uint32_t addr, uint32_t *out)
{
    if (!memap_set_csw(CSW_DEFAULT | CSW_ADDRINC_SINGLE | CSW_SIZE_32)) {
        return false;
    }
    if (!memap_set_tar(addr)) {
        return false;
    }
    return memap_read_drw(out);
}

bool target_mem_write_word(uint32_t addr, uint32_t v)
{
    if (!memap_set_csw(CSW_DEFAULT | CSW_ADDRINC_SINGLE | CSW_SIZE_32)) {
        return false;
    }
    if (!memap_set_tar(addr)) {
        return false;
    }
    return memap_write_drw(v);
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
        uint32_t aligned = addr & ~3u;
        uint32_t w       = 0;
        if (!target_mem_read_word(aligned, &w)) {
            return false;
        }

        for (uint32_t i = (addr & 3u); i < 4u && len; i++) {
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

