// MSPM0-target flash programming via the target's FLASHCTL, driven over
// SWD MEM-AP word accesses. Register layout and command sequences follow
// TI's hw_flashctl.h / dl_flashctl.c (SDK 2.09); the FLASHCTL IP is common
// across the MSPM0 family.

#include "flash_mspm0.h"

#if defined(PROBE_ENABLE_CORTEXM) && (PROBE_ENABLE_CORTEXM) && \
    defined(PROBE_ENABLE_FLASH_MSPM0) && (PROBE_ENABLE_FLASH_MSPM0)

#include <stddef.h>

#include "hal.h"
#include "target_mem.h"

#define FLASHCTL_BASE       0x400CD000u
#define FLASHCTL_CMDEXEC    (FLASHCTL_BASE + 0x1100u)
#define FLASHCTL_CMDTYPE    (FLASHCTL_BASE + 0x1104u)
#define FLASHCTL_CMDCTL     (FLASHCTL_BASE + 0x1108u)
#define FLASHCTL_CMDADDR    (FLASHCTL_BASE + 0x1120u)
#define FLASHCTL_CMDBYTEN   (FLASHCTL_BASE + 0x1124u)
#define FLASHCTL_CMDDATA0   (FLASHCTL_BASE + 0x1130u)
#define FLASHCTL_CMDDATA1   (FLASHCTL_BASE + 0x1134u)
#define FLASHCTL_CMDDATA2   (FLASHCTL_BASE + 0x1138u)
#define FLASHCTL_CMDDATA3   (FLASHCTL_BASE + 0x113Cu)
#define FLASHCTL_CMDWEPROTA (FLASHCTL_BASE + 0x11D0u)
#define FLASHCTL_CMDWEPROTB (FLASHCTL_BASE + 0x11D4u)
#define FLASHCTL_CMDWEPROTC (FLASHCTL_BASE + 0x11D8u)
#define FLASHCTL_STATCMD    (FLASHCTL_BASE + 0x13D0u)
#define FLASHCTL_GBLINFO0   (FLASHCTL_BASE + 0x13F0u)
#define FLASHCTL_GBLINFO1   (FLASHCTL_BASE + 0x13F4u)
#define FLASHCTL_BANK0INFO0 (FLASHCTL_BASE + 0x1400u)
#define FLASHCTL_BANKINFO_STRIDE 0x10u

#define CMDTYPE_PROGRAM      0x1u
#define CMDTYPE_ERASE        0x2u
#define CMDTYPE_CLEARSTATUS  0x5u
#define CMDTYPE_SIZE_ONEWORD (0x0u << 4)
#define CMDTYPE_SIZE_TWOWORD (0x1u << 4)
#define CMDTYPE_SIZE_SECTOR  (0x4u << 4)

#define STATCMD_CMDDONE (1u << 0)
#define STATCMD_CMDPASS (1u << 1)
#define STATCMD_CMDINPROGRESS (1u << 2)

// Program a complete physical flash word and have FLASHCTL generate/program
// its ECC. These match TI driverlib's 64/128-bit generated-ECC configurations.
#define CMDBYTEN_WORD64_WITH_ECC 0x1FFu
#define CMDBYTEN_WORD128_WITH_ECC 0x1FFFFu

// Reject a program that would require a 0 -> 1 transition.  Erased flash is
// a precondition, but enabling the controller check makes violations fail
// rather than risking corruption.
#define CMDCTL_DATAVEREN (1u << 21)

// Sector erase is spec'd in the low milliseconds; leave generous margin
// since each poll costs a full SWD transaction anyway.
#define FLASH_CMD_TIMEOUT_US 100000u

static bool fl_wait_done(uint32_t *statcmd_out)
{
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < FLASH_CMD_TIMEOUT_US) {
        uint32_t st = 0;
        if (!target_mem_read_word(FLASHCTL_STATCMD, &st)) {
            return false;
        }
        if (st & STATCMD_CMDDONE) {
            if (statcmd_out) {
                *statcmd_out = st;
            }
            return true;
        }
    }
    return false;
}

static bool fl_wait_idle(void)
{
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < FLASH_CMD_TIMEOUT_US) {
        uint32_t st = 0;
        if (!target_mem_read_word(FLASHCTL_STATCMD, &st)) {
            return false;
        }
        if ((st & STATCMD_CMDINPROGRESS) == 0u) {
            return true;
        }
    }
    return false;
}

static bool fl_clear_status(void)
{
    if (!target_mem_write_word(FLASHCTL_CMDTYPE, CMDTYPE_CLEARSTATUS)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDEXEC, 1u)) return false;

    // CLEARSTATUS clears STATCMD, including CMDDONE.  TI driverlib therefore
    // waits for CMDINPROGRESS to deassert; waiting for CMDDONE can never
    // complete on conforming hardware.
    return fl_wait_idle();
}

// Execute one prepared command: unlock write/erase protection (it re-arms
// after every command execution), pulse CMDEXEC, and require DONE+PASS.
static bool fl_exec(void)
{
    if (!target_mem_write_word(FLASHCTL_CMDWEPROTA, 0u)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDWEPROTB, 0u)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDWEPROTC, 0u)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDEXEC, 1u)) return false;

    uint32_t st = 0;
    if (!fl_wait_done(&st)) {
        return false;
    }
    return (st & STATCMD_CMDPASS) != 0;
}

bool flash_mspm0_geometry(uint32_t *main_size_bytes, uint32_t *sector_size_bytes)
{
    uint32_t gbl = 0;
    if (!target_mem_read_word(FLASHCTL_GBLINFO0, &gbl)) {
        return false;
    }

    uint32_t sector    = gbl & 0xFFFFu;          // GBLINFO0.SECTORSIZE (bytes)
    uint32_t num_banks = (gbl >> 16) & 0x7u;    // GBLINFO0.NUMBANKS

    // Plausibility check: MSPM0 sectors are 1 KB and main flash is
    // 8..512 sectors per bank.  Keep accepting power-of-two sector sizes in
    // the documented field range so later compatible FLASHCTL parts work.
    if (sector < 256u || sector > 4096u || (sector & (sector - 1u)) != 0u ||
        num_banks < 1u || num_banks > 5u) {
        return false;
    }

    uint32_t total_sectors = 0;
    for (uint32_t bank = 0; bank < num_banks; bank++) {
        uint32_t info = 0;
        if (!target_mem_read_word(FLASHCTL_BANK0INFO0 +
                                      bank * FLASHCTL_BANKINFO_STRIDE,
                                  &info)) {
            return false;
        }

        uint32_t sectors = info & 0xFFFu; // BANKnINFO0.MAINSIZE
        if (sectors < 8u || sectors > 512u ||
            total_sectors > UINT32_MAX - sectors) {
            return false;
        }
        total_sectors += sectors;
    }

    if (total_sectors > UINT32_MAX / sector) {
        return false;
    }

    if (sector_size_bytes) {
        *sector_size_bytes = sector;
    }
    if (main_size_bytes) {
        *main_size_bytes = sector * total_sectors;
    }
    return true;
}

typedef struct {
    uint32_t word_base;
    uint32_t next_word;
    uint32_t next_addr;
    uint8_t  data[16];
    uint16_t byte_mask;
    uint8_t  word_size;
    bool     pending;
    bool     started;
    bool     sealed;
} flash_write_state_t;

static flash_write_state_t g_write;

void flash_mspm0_abort(void)
{
    g_write.word_base = 0u;
    g_write.next_word = 0u;
    g_write.next_addr = 0u;
    g_write.byte_mask = 0u;
    g_write.word_size = 0u;
    g_write.pending   = false;
    g_write.started   = false;
    g_write.sealed    = false;
}

static bool fl_get_word_size(uint8_t *word_size)
{
    uint32_t info = 0;
    if (!target_mem_read_word(FLASHCTL_GBLINFO1, &info)) {
        return false;
    }

    uint32_t data_width = info & 0xFFu; // GBLINFO1.DATAWIDTH (bits)
    if (data_width != 64u && data_width != 128u) {
        return false;
    }
    *word_size = (uint8_t) (data_width / 8u);
    return true;
}

static void fl_stage_word(uint32_t word_base, uint8_t word_size)
{
    g_write.word_base = word_base;
    g_write.byte_mask = 0u;
    g_write.word_size = word_size;
    g_write.pending   = true;
    for (uint32_t i = 0; i < word_size; i++) {
        g_write.data[i] = 0xFFu;
    }
}

static bool fl_program_staged_word(void)
{
    if (!g_write.pending) {
        return true;
    }

    uint32_t d0 = (uint32_t) g_write.data[0] |
                  ((uint32_t) g_write.data[1] << 8) |
                  ((uint32_t) g_write.data[2] << 16) |
                  ((uint32_t) g_write.data[3] << 24);
    uint32_t d1 = (uint32_t) g_write.data[4] |
                  ((uint32_t) g_write.data[5] << 8) |
                  ((uint32_t) g_write.data[6] << 16) |
                  ((uint32_t) g_write.data[7] << 24);
    uint32_t d2 = (uint32_t) g_write.data[8] |
                  ((uint32_t) g_write.data[9] << 8) |
                  ((uint32_t) g_write.data[10] << 16) |
                  ((uint32_t) g_write.data[11] << 24);
    uint32_t d3 = (uint32_t) g_write.data[12] |
                  ((uint32_t) g_write.data[13] << 8) |
                  ((uint32_t) g_write.data[14] << 16) |
                  ((uint32_t) g_write.data[15] << 24);
    uint32_t cmd_size = g_write.word_size == 16u ?
                            CMDTYPE_SIZE_TWOWORD : CMDTYPE_SIZE_ONEWORD;
    uint32_t byten = g_write.word_size == 16u ?
                         CMDBYTEN_WORD128_WITH_ECC :
                         CMDBYTEN_WORD64_WITH_ECC;

    if (!fl_clear_status()) return false;
    if (!target_mem_write_word(FLASHCTL_CMDTYPE,
                               CMDTYPE_PROGRAM | cmd_size)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDCTL, CMDCTL_DATAVEREN)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDADDR, g_write.word_base)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDBYTEN, byten)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDDATA0, d0)) return false;
    if (!target_mem_write_word(FLASHCTL_CMDDATA1, d1)) return false;
    if (g_write.word_size == 16u) {
        if (!target_mem_write_word(FLASHCTL_CMDDATA2, d2)) return false;
        if (!target_mem_write_word(FLASHCTL_CMDDATA3, d3)) return false;
    }
    if (!fl_exec()) return false;

    g_write.next_word = g_write.word_base + g_write.word_size;
    g_write.pending   = false;
    return true;
}

bool flash_mspm0_erase_range(uint32_t addr, uint32_t len)
{
    // An erase starts a new programming transaction.  Never carry a partial
    // word or ordering state across it.
    flash_mspm0_abort();

    if (len == 0) {
        return true;
    }

    uint32_t sector = 0, main_size = 0;
    if (!flash_mspm0_geometry(&main_size, &sector)) {
        return false;
    }
    if (addr >= main_size || len > main_size - addr) {
        return false; // outside main flash (non-main regions are off limits)
    }

    uint32_t first = addr & ~(sector - 1u);
    uint32_t last  = (addr + len - 1u) & ~(sector - 1u);

    for (uint32_t s = first;; s += sector) {
        if (!fl_clear_status()) return false;
        if (!target_mem_write_word(FLASHCTL_CMDTYPE, CMDTYPE_ERASE | CMDTYPE_SIZE_SECTOR)) return false;
        if (!target_mem_write_word(FLASHCTL_CMDCTL, 0u)) return false;
        if (!target_mem_write_word(FLASHCTL_CMDADDR, s)) return false;
        if (!fl_exec()) return false;
        if (s == last) break;
    }
    return true;
}

bool flash_mspm0_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0u) {
        return true;
    }
    if (g_write.sealed) {
        return false;
    }
    if (!data) {
        flash_mspm0_abort();
        return false;
    }

    uint32_t main_size = 0;
    uint8_t  word_size = 0u;
    if (!flash_mspm0_geometry(&main_size, NULL) ||
        !fl_get_word_size(&word_size) ||
        addr >= main_size || len > main_size - addr) {
        flash_mspm0_abort();
        return false;
    }
    if (g_write.started && g_write.word_size != word_size) {
        flash_mspm0_abort();
        return false;
    }
    if (g_write.started && addr < g_write.next_addr) {
        flash_mspm0_abort();
        return false;
    }

    uint32_t request_end = addr + len;

    // vFlashWrite packet boundaries need not follow flash-word boundaries.
    // Coalesce one physical (64- or 128-bit) word across calls, fill
    // unspecified erased bytes with 0xff, and execute exactly one program
    // command per word. Once a word has been committed, revisiting it is
    // rejected: repeated program pulses are device-limited and can corrupt
    // the containing word line.
    while (len > 0) {
        uint32_t word_base = addr & ~((uint32_t) word_size - 1u);
        uint32_t off       = addr - word_base;
        uint32_t n         = (uint32_t) word_size - off;
        if (n > len) {
            n = len;
        }

        if (!g_write.pending) {
            if (g_write.started && word_base < g_write.next_word) {
                flash_mspm0_abort();
                return false;
            }
            fl_stage_word(word_base, word_size);
            g_write.started = true;
        } else if (word_base != g_write.word_base) {
            if (word_base < g_write.word_base || !fl_program_staged_word() ||
                word_base < g_write.next_word) {
                flash_mspm0_abort();
                return false;
            }
            fl_stage_word(word_base, word_size);
        }

        for (uint32_t i = 0; i < n; i++) {
            uint16_t bit = (uint16_t) (1u << (off + i));
            if ((g_write.byte_mask & bit) != 0u) {
                flash_mspm0_abort();
                return false;
            }
            g_write.data[off + i] = data[i];
            g_write.byte_mask |= bit;
        }

        addr += n;
        data += n;
        len -= n;

        uint16_t full_mask = word_size == 16u ? UINT16_MAX : 0x00FFu;
        if (g_write.byte_mask == full_mask && !fl_program_staged_word()) {
            flash_mspm0_abort();
            return false;
        }
    }
    g_write.next_addr = request_end;
    return true;
}

bool flash_mspm0_done(void)
{
    if (g_write.sealed) {
        return true;
    }
    if (!fl_program_staged_word()) {
        flash_mspm0_abort();
        return false;
    }
    g_write.sealed = true;
    return true;
}

#endif // PROBE_ENABLE_CORTEXM && PROBE_ENABLE_FLASH_MSPM0
