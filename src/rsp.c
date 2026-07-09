// Minimal GDB RSP over UART transport + command handling

#include "rsp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "probe.h"
#include "target.h"
#include "hal.h"

#if defined(PROBE_ENABLE_FLASH_MSPM0) && (PROBE_ENABLE_FLASH_MSPM0)
#include "flash_mspm0.h"
#define RSP_HAVE_FLASH 1
#else
#define RSP_HAVE_FLASH 0
#endif

#if (defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)) || \
    (defined(PROBE_ENABLE_RISCV_MINIMAL_XML) && (PROBE_ENABLE_RISCV_MINIMAL_XML))
#define RSP_HAVE_TARGET_XML 1
#else
#define RSP_HAVE_TARGET_XML 0
#endif

#if RSP_HAVE_TARGET_XML || RSP_HAVE_FLASH
#define RSP_HAVE_QXFER 1
#else
#define RSP_HAVE_QXFER 0
#endif

#ifndef PROBE_TINY_RAM
#define PROBE_TINY_RAM 0
#endif

#ifndef RSP_MAX_PAYLOAD
#define RSP_MAX_PAYLOAD 512u
#endif

#ifndef RSP_PACKET_SIZE_HEX
#define RSP_PACKET_SIZE_HEX "200"
#endif

#ifndef RSP_IOBUF_SIZE
#if PROBE_TINY_RAM
#define RSP_IOBUF_SIZE (RSP_MAX_PAYLOAD / 2u)
#else
#define RSP_IOBUF_SIZE 256u
#endif
#endif

// Memory payloads and register blocks are handled by disjoint commands, so
// overlay them. This is material on the 1 KB C1104 and keeps the correct
// 168-byte legacy ARM layout compatible with the enforced stack reserve.
#if (!defined(PROBE_ENABLE_QXFER_TARGET_XML) || !(PROBE_ENABLE_QXFER_TARGET_XML)) && \
    defined(PROBE_ENABLE_CORTEXM) && (PROBE_ENABLE_CORTEXM)
#define RSP_MAX_REGS 42u // Legacy ARM: 168-byte r/FPA/FPS/CPSR layout
#elif defined(PROBE_ENABLE_RISCV) && (PROBE_ENABLE_RISCV)
#define RSP_MAX_REGS 33u // RV32: x0-x31 + pc
#else
#define RSP_MAX_REGS 17u // Cortex-M: r0-r15 + xPSR
#endif
typedef union {
    uint32_t regs[RSP_MAX_REGS];
    uint8_t  iobuf[RSP_IOBUF_SIZE];
} rsp_work_t;
static rsp_work_t rsp_work;
#define rsp_regs  (rsp_work.regs)
#define rsp_iobuf (rsp_work.iobuf)

typedef enum {
    RSP_IDLE = 0,
    RSP_IN_PKT,
    RSP_IN_CSUM1,
    RSP_IN_CSUM2,
    RSP_DISCARD,      // oversized packet: consume until '#'
    RSP_DISCARD_CS1,  // consume first checksum char
    RSP_DISCARD_CS2   // consume second checksum char, then NACK
} rsp_state_t;

static rsp_state_t rsp_state = RSP_IDLE;
// Shared receive/retransmit storage. A reply is generated only after its
// request has been parsed, so reusing the packet buffer gives tiny builds
// standards-compliant NACK retransmission without a second 256-byte buffer.
static char        rsp_buf[RSP_MAX_PAYLOAD + 5u];
static uint32_t    rsp_len     = 0;
static uint8_t     rsp_sum     = 0;
static uint8_t     rsp_rx_csum = 0;
static bool        rsp_noack_mode = false;
static bool        rsp_running = false;

static uint32_t rsp_tx_len   = 0;
static bool     rsp_tx_valid = false;

static void rsp_tx_record_begin(void)
{
    rsp_tx_len   = 0;
    rsp_tx_valid = true;
}

static void rsp_tx_byte(uint8_t c)
{
    uart_putc(c);
    if (rsp_tx_len < (uint32_t) sizeof(rsp_buf)) {
        rsp_buf[rsp_tx_len++] = (char) c;
    } else {
        rsp_tx_valid = false; // too long to replay
    }
}

static void rsp_retransmit_last(void)
{
    if (!rsp_tx_valid) {
        return;
    }
    for (uint32_t i = 0; i < rsp_tx_len; i++) {
        uart_putc((uint8_t) rsp_buf[i]);
    }
}

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t) (c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t) (c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (uint8_t) (c - 'A' + 10);
    }
    return 0xFF;
}

static char nibble_hex(uint8_t n)
{
    n &= 0xF;
    return (n < 10) ? (char) ('0' + n) : (char) ('a' + (n - 10));
}

static void rsp_put_hex_u8(uint8_t v)
{
    rsp_tx_byte((uint8_t) nibble_hex(v >> 4));
    rsp_tx_byte((uint8_t) nibble_hex(v));
}

static bool parse_u32_hex(const char *s, uint32_t *out)
{
    uint32_t v      = 0;
    uint32_t digits = 0;
    if (!s) {
        return false;
    }
    while (*s) {
        uint8_t n = hex_nibble(*s);
        if (n == 0xFF) {
            return false; // trailing garbage is an error, not end-of-number
        }
        if (digits >= 8u) {
            return false; // wider than 32 bits would silently wrap
        }
        v = (v << 4) | n;
        s++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_u32_hex_stop(const char *s, char stop, uint32_t *out, const char **endp)
{
    uint32_t    v      = 0;
    uint32_t    digits = 0;
    const char *p      = s;
    if (!p) {
        return false;
    }
    while (*p && *p != stop) {
        uint8_t n = hex_nibble(*p);
        if (n == 0xFF) {
            return false;
        }
        if (digits >= 8u) {
            return false;
        }
        v = (v << 4) | n;
        p++;
        digits++;
    }
    if (digits == 0 || *p != stop) {
        return false;
    }
    *out = v;
    if (endp) {
        *endp = p + 1;
    }
    return true;
}

static void rsp_send_packet_begin(uint8_t *sum)
{
    *sum = 0;
    rsp_tx_record_begin();
    rsp_tx_byte('$');
}

static void rsp_send_packet_end(uint8_t sum)
{
    rsp_tx_byte('#');
    rsp_put_hex_u8(sum);
}

static void rsp_send_packet_str(const char *payload)
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    while (*payload) {
        uint8_t c = (uint8_t) *payload++;
        sum       = (uint8_t) (sum + c);
        rsp_tx_byte(c);
    }
    rsp_send_packet_end(sum);
}

#if RSP_HAVE_QXFER
static void rsp_send_packet_prefix_and_bytes(char prefix, const char *payload, uint32_t len)
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    sum = (uint8_t) (sum + (uint8_t) prefix);
    rsp_tx_byte((uint8_t) prefix);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t) payload[i];
        sum       = (uint8_t) (sum + c);
        rsp_tx_byte(c);
    }
    rsp_send_packet_end(sum);
}
#endif

// Decode RSP binary escaping (0x7d followed by char^0x20) in place.
// A final escape byte is malformed rather than a literal 0x7d.
static bool rsp_unescape(char *buf, uint32_t len, uint32_t *decoded_len)
{
    uint32_t r = 0, w = 0;
    while (r < len) {
        uint8_t c = (uint8_t) buf[r++];
        if (c == 0x7Du) {
            if (r >= len) {
                return false;
            }
            c = (uint8_t) ((uint8_t) buf[r++] ^ 0x20u);
        }
        buf[w++] = (char) c;
    }
    *decoded_len = w;
    return true;
}

static void rsp_send_ok(void) { rsp_send_packet_str("OK"); }
static void rsp_send_err(void) { rsp_send_packet_str("E01"); }
static void rsp_send_empty(void) { rsp_send_packet_str(""); }

static void rsp_send_sigtrap(void)
{
    // SIGTRAP is 5
    rsp_send_packet_str("S05");
}

static void rsp_send_sigint(void)
{
    // SIGINT is 2 (stop reply for a Ctrl-C interrupt)
    rsp_send_packet_str("S02");
}

static void rsp_send_trap_watchpoint(target_watch_t wt, uint32_t addr)
{
    const char *tag = "watch";
    if (wt == TARGET_WATCH_READ) {
        tag = "rwatch";
    } else if (wt == TARGET_WATCH_ACCESS) {
        tag = "awatch";
    }

    uint8_t sum;
    rsp_send_packet_begin(&sum);

    const char *p = "T05";
    while (*p) {
        uint8_t c = (uint8_t) *p++;
        sum       = (uint8_t) (sum + c);
        rsp_tx_byte(c);
    }

    while (*tag) {
        uint8_t c = (uint8_t) *tag++;
        sum       = (uint8_t) (sum + c);
        rsp_tx_byte(c);
    }
    sum = (uint8_t) (sum + (uint8_t) ':');
    rsp_tx_byte((uint8_t) ':');

    for (int i = 7; i >= 0; i--) {
        char c = nibble_hex((addr >> (4u * (uint32_t) i)) & 0xFu);
        sum    = (uint8_t) (sum + (uint8_t) c);
        rsp_tx_byte((uint8_t) c);
    }

    sum = (uint8_t) (sum + (uint8_t) ';');
    rsp_tx_byte((uint8_t) ';');

    rsp_send_packet_end(sum);
}

static bool rsp_parse_hex_byte(const char *p, uint8_t *out)
{
    // Validate p[0] before touching p[1]: if p[0] is the terminating NUL,
    // p[1] is out of bounds.
    uint8_t hi = hex_nibble(p[0]);
    if (hi == 0xFF) {
        return false;
    }
    uint8_t lo = hex_nibble(p[1]);
    if (lo == 0xFF) {
        return false;
    }
    *out = (uint8_t) ((hi << 4) | lo);
    return true;
}

static bool rsp_hex_to_bytes(const char *hex, uint8_t *out, uint32_t outlen)
{
    for (uint32_t i = 0; i < outlen; i++) {
        uint8_t b;
        if (!rsp_parse_hex_byte(hex + 2u * i, &b)) {
            return false;
        }
        out[i] = b;
    }
    return hex[2u * outlen] == '\0';
}

static void rsp_send_bytes_as_hex(const uint8_t *data, uint32_t len)
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b  = data[i];
        char    h1 = nibble_hex(b >> 4);
        char    h2 = nibble_hex(b);
        sum        = (uint8_t) (sum + (uint8_t) h1);
        rsp_tx_byte((uint8_t) h1);
        sum = (uint8_t) (sum + (uint8_t) h2);
        rsp_tx_byte((uint8_t) h2);
    }
    rsp_send_packet_end(sum);
}

static void rsp_send_regs_hex(const uint32_t *regs, uint32_t count)
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = regs[i];
        for (int j = 0; j < 4; j++) {
            uint8_t b  = (uint8_t) (v & 0xFF);
            char    h1 = nibble_hex(b >> 4);
            char    h2 = nibble_hex(b);
            sum        = (uint8_t) (sum + (uint8_t) h1);
            rsp_tx_byte((uint8_t) h1);
            sum = (uint8_t) (sum + (uint8_t) h2);
            rsp_tx_byte((uint8_t) h2);
            v >>= 8;
        }
    }
    rsp_send_packet_end(sum);
}

static bool rsp_parse_regs_hex(const char *hex, uint32_t *regs, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = 0;
        for (uint32_t j = 0; j < 4; j++) {
            uint8_t b;
            if (!rsp_parse_hex_byte(hex + (i * 8u + j * 2u), &b)) {
                return false;
            }
            v |= ((uint32_t) b << (8u * j));
        }
        regs[i] = v;
    }
    return hex[8u * count] == '\0';
}

#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
#define QS_XML ";qXfer:features:read+"
#else
#define QS_XML ""
#endif
#if RSP_HAVE_FLASH
#define QS_MAP ";qXfer:memory-map:read+"
#else
#define QS_MAP ""
#endif

static void handle_qSupported(void)
{
    // qSupported marks the start of a GDB session: acks are back on until
    // the new session requests otherwise, and if no target was found at
    // boot (e.g. it was unpowered), re-run detection now.  Always probing at
    // this session boundary also recovers from a target power-cycle or swap;
    // target_attached() alone only reflects cached software state.
    rsp_noack_mode = false;
    rsp_running = false;
#if RSP_HAVE_FLASH
    flash_mspm0_abort();
#endif
    (void) probe_attach();

#if defined(PROBE_ENABLE_RISCV_MINIMAL_XML) && (PROBE_ENABLE_RISCV_MINIMAL_XML)
    const char *xml = NULL;
    uint32_t xml_len = 0u;
    if (target_xml_get(&xml, &xml_len) && xml && xml_len != 0u) {
        rsp_send_packet_str("PacketSize=" RSP_PACKET_SIZE_HEX
                            ";QStartNoAckMode+"
                            ";qXfer:features:read+" QS_MAP);
        return;
    }
#endif
    rsp_send_packet_str("PacketSize=" RSP_PACKET_SIZE_HEX
                        ";QStartNoAckMode+" QS_XML QS_MAP);
}

static bool parse_u32_le_hex_bytes(const char *hex, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (!rsp_parse_hex_byte(hex + i * 2, &b)) {
            return false;
        }
        v |= ((uint32_t) b << (8u * (uint32_t) i));
    }
    if (hex[8] != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static void rsp_send_u32_le(uint32_t v)
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    for (int i = 0; i < 4; i++) {
        uint8_t b  = (uint8_t) (v & 0xFFu);
        char    h1 = nibble_hex(b >> 4);
        char    h2 = nibble_hex(b);
        sum        = (uint8_t) (sum + (uint8_t) h1);
        rsp_tx_byte((uint8_t) h1);
        sum = (uint8_t) (sum + (uint8_t) h2);
        rsp_tx_byte((uint8_t) h2);
        v >>= 8;
    }
    rsp_send_packet_end(sum);
}

static void handle_breakpoint(const char *p)
{
    // Ztype,addr,kind or ztype,addr,kind
    bool is_set = (p[0] == 'Z');
    uint32_t type = 0, addr = 0, kind = 0;
    const char *q = NULL;
    if (!parse_u32_hex_stop(p + 1, ',', &type, &q)) {
        rsp_send_err();
        return;
    }
    const char *r = NULL;
    if (!parse_u32_hex_stop(q, ',', &addr, &r)) {
        rsp_send_err();
        return;
    }
    if (!parse_u32_hex(r, &kind)) {
        rsp_send_err();
        return;
    }

    if (type == 0 || type == 1) {
        bool ok = is_set ? target_breakpoint_insert(addr) : target_breakpoint_remove(addr);
        if (ok) {
            rsp_send_ok();
        } else {
            rsp_send_err();
        }
        return;
    }

    if (type == 2 || type == 3 || type == 4) {
        target_watch_t wt = TARGET_WATCH_ACCESS;
        if (type == 2) {
            wt = TARGET_WATCH_WRITE;
        } else if (type == 3) {
            wt = TARGET_WATCH_READ;
        } else {
            wt = TARGET_WATCH_ACCESS;
        }

        if (!target_watchpoints_supported()) {
            rsp_send_empty();
            return;
        }

        bool ok = is_set ? target_watchpoint_insert(wt, addr, kind) : target_watchpoint_remove(wt, addr, kind);
        if (ok) {
            rsp_send_ok();
        } else {
            rsp_send_err();
        }
        return;
    }

    rsp_send_empty();
}

#if RSP_HAVE_QXFER
// Serve one OFFSET,LENGTH chunk of a qXfer document ('m' = more, 'l' = last).
static void rsp_send_xfer_chunk(const char *doc, uint32_t doc_len, const char *args)
{
    uint32_t    off = 0, len = 0;
    const char *r = NULL;
    if (!parse_u32_hex_stop(args, ',', &off, &r) || !parse_u32_hex(r, &len)) {
        rsp_send_err();
        return;
    }

    if (off >= doc_len) {
        rsp_send_packet_str("l");
        return;
    }

    uint32_t remaining = doc_len - off;
    if (len > remaining) {
        len = remaining;
    }
    if (len > (RSP_MAX_PAYLOAD - 1u)) {
        len = (RSP_MAX_PAYLOAD - 1u);
    }

    char more = ((off + len) < doc_len) ? 'm' : 'l';
    rsp_send_packet_prefix_and_bytes(more, doc + off, len);
}
#endif

#if RSP_HAVE_TARGET_XML
static void handle_qXfer_features_read(const char *p)
{
    // qXfer:features:read:target.xml:OFFSET,LENGTH
    const char *annex = p + (sizeof("qXfer:features:read:") - 1u);
    const char *q     = strchr(annex, ':');
    if (!q) {
        rsp_send_err();
        return;
    }

    size_t annex_len = (size_t) (q - annex);
    if (annex_len != (sizeof("target.xml") - 1u) ||
        strncmp(annex, "target.xml", annex_len) != 0) {
        rsp_send_empty();
        return;
    }

    const char *xml     = NULL;
    uint32_t    xml_len = 0;
    if (!target_xml_get(&xml, &xml_len) || !xml) {
        rsp_send_empty();
        return;
    }

    rsp_send_xfer_chunk(xml, xml_len, q + 1);
}
#endif

#if RSP_HAVE_FLASH
// GDB memory map for an MSPM0 target: main flash geometry is read from the
// target's FLASHCTL, so `load` uses vFlash* for flash regions. Regions
// outside the map default to RAM behavior, but list the SRAM/peripheral
// spaces anyway for GDB configurations with inaccessible-by-default set.
static char     rsp_mmap_xml[320];
static uint32_t rsp_mmap_len = 0;

static void mmap_append(const char *s)
{
    while (*s && rsp_mmap_len < (uint32_t) sizeof(rsp_mmap_xml) - 1u) {
        rsp_mmap_xml[rsp_mmap_len++] = *s++;
    }
}

static void mmap_append_hex(uint32_t v)
{
    bool started = false;
    for (int i = 7; i >= 0; i--) {
        uint8_t nib = (uint8_t) ((v >> (4u * (uint32_t) i)) & 0xFu);
        if (nib != 0 || started || i == 0) {
            started = true;
            if (rsp_mmap_len < (uint32_t) sizeof(rsp_mmap_xml) - 1u) {
                rsp_mmap_xml[rsp_mmap_len++] = nibble_hex(nib);
            }
        }
    }
}

static bool rsp_mmap_build(void)
{
    uint32_t fsize = 0, ssize = 0;
    if (!flash_mspm0_geometry(&fsize, &ssize)) {
        return false; // not an MSPM0 FLASHCTL: no map, GDB proceeds without
    }

    rsp_mmap_len = 0;
    mmap_append("<memory-map>"
                "<memory type=\"flash\" start=\"0x0\" length=\"0x");
    mmap_append_hex(fsize);
    mmap_append("\"><property name=\"blocksize\">0x");
    mmap_append_hex(ssize);
    mmap_append("</property></memory>"
                "<memory type=\"ram\" start=\"0x20000000\" length=\"0x8000000\"/>"
                "<memory type=\"ram\" start=\"0x40000000\" length=\"0xc0000000\"/>"
                "</memory-map>");
    return true;
}
#endif

static void rsp_handle_command(void)
{
    rsp_buf[rsp_len] = '\0';
    const char *p    = rsp_buf;

    if (p[0] == '?' && p[1] == '\0') {
        bool halted = false;
        if (!target_attached() || !target_is_halted(&halted)) {
            if (!probe_attach() || !target_is_halted(&halted)) {
                rsp_send_err();
                return;
            }
        }
        if (!halted && !target_halt()) {
            rsp_send_err();
            return;
        }
        // '?' is a synchronous stop query.  Once a stop is confirmed there
        // is no longer an outstanding continue whose completion rsp_poll()
        // should report a second time.
        rsp_running = false;
        rsp_send_sigtrap();
        return;
    }

    if (p[0] == 'g' && p[1] == '\0') {
        uint32_t count = target_gdb_reg_count();
        if (count == 0u || count > RSP_MAX_REGS) {
            rsp_send_err();
            return;
        }
        if (!target_halt()) {
            rsp_send_err();
            return;
        }
        rsp_running = false;
        if (!target_read_gdb_regs(rsp_regs, count)) {
            rsp_send_err();
            return;
        }
        rsp_send_regs_hex(rsp_regs, count);
        return;
    }

    if (p[0] == 'G') {
        uint32_t count = target_gdb_reg_count();
        if (count == 0u || count > RSP_MAX_REGS) {
            rsp_send_err();
            return;
        }
        if (!target_halt()) {
            rsp_send_err();
            return;
        }
        rsp_running = false;
        if (!rsp_parse_regs_hex(p + 1, rsp_regs, count)) {
            rsp_send_err();
            return;
        }
        if (!target_write_gdb_regs(rsp_regs, count)) {
            rsp_send_err();
            return;
        }
        rsp_send_ok();
        return;
    }

    if (p[0] == 'm') {
        uint32_t    addr = 0, len = 0;
        const char *q = NULL;
        if (!parse_u32_hex_stop(p + 1, ',', &addr, &q)) {
            rsp_send_err();
            return;
        }
        if (!parse_u32_hex(q, &len)) {
            rsp_send_err();
            return;
        }

        if (len > (uint32_t) sizeof(rsp_iobuf)) {
            rsp_send_err();
            return;
        }
        if (!target_mem_read_bytes(addr, rsp_iobuf, len)) {
            rsp_send_err();
            return;
        }
        rsp_send_bytes_as_hex(rsp_iobuf, len);
        return;
    }

    if (p[0] == 'M') {
        uint32_t    addr = 0, len = 0;
        const char *q = NULL;
        if (!parse_u32_hex_stop(p + 1, ',', &addr, &q)) {
            rsp_send_err();
            return;
        }
        const char *r = NULL;
        if (!parse_u32_hex_stop(q, ':', &len, &r)) {
            rsp_send_err();
            return;
        }

        if (len > (uint32_t) sizeof(rsp_iobuf)) {
            rsp_send_err();
            return;
        }
        if (!rsp_hex_to_bytes(r, rsp_iobuf, len)) {
            rsp_send_err();
            return;
        }
        if (!target_mem_write_bytes(addr, rsp_iobuf, len)) {
            rsp_send_err();
            return;
        }
        rsp_send_ok();
        return;
    }

    if (p[0] == 'X') {
        // Xaddr,len:binary-data (0x7d-escaped). GDB probes support with a
        // zero-length write; replying OK enables binary downloads.
        uint32_t    addr = 0, len = 0;
        const char *q = NULL, *r = NULL;
        if (!parse_u32_hex_stop(p + 1, ',', &addr, &q) ||
            !parse_u32_hex_stop(q, ':', &len, &r)) {
            rsp_send_err();
            return;
        }

        char    *data = rsp_buf + (r - p);
        uint32_t raw  = rsp_len - (uint32_t) (r - p);
        uint32_t n = 0u;
        if (!rsp_unescape(data, raw, &n)) {
            rsp_send_err();
            return;
        }

        if (n != len) {
            rsp_send_err();
            return;
        }
        if (len == 0u) {
            rsp_send_ok();
            return;
        }
        if (!target_mem_write_bytes(addr, (const uint8_t *) data, len)) {
            rsp_send_err();
            return;
        }
        rsp_send_ok();
        return;
    }

#if 0
    /*
     * Reference (pre tiny-RAM experiment):
     * - m: used static uint8_t mbuf[1024]
     * - M: used static uint8_t wbuf[512]
     * Kept as a note while exploring 1KB-RAM parts.
     */
    static uint8_t mbuf[1024];
    static uint8_t wbuf[512];
#endif

    if (p[0] == 'c') {
        // Optional address form: cADDR
        if (p[1] != '\0') {
            uint32_t addr = 0;
            if (!parse_u32_hex(p + 1, &addr)) {
                rsp_send_err();
                return;
            }
            if (!target_write_reg(target_pc_regnum(), addr)) {
                rsp_send_err();
                return;
            }
        }

        if (!target_continue()) {
            // Resume transports are posted/acknowledged asynchronously. A
            // false result can therefore mean "request applied, confirmation
            // failed". Only report E01 when the target is confirmed halted;
            // otherwise keep tracking the possibly running target.
            bool halted = false;
            if (target_is_halted(&halted) && halted) {
                rsp_send_err();
                return;
            }
        }
        rsp_running = true;
        return;
    }

    if (p[0] == 's') {
        // Optional address form: sADDR
        if (p[1] != '\0') {
            uint32_t addr = 0;
            if (!parse_u32_hex(p + 1, &addr)) {
                rsp_send_err();
                return;
            }
            if (!target_write_reg(target_pc_regnum(), addr)) {
                rsp_send_err();
                return;
            }
        }

        if (!target_step()) {
            rsp_send_err();
            return;
        }
        rsp_running = false;
        rsp_send_sigtrap();
        return;
    }

    if (p[0] == 'p') {
        uint32_t regno = 0;
        if (!parse_u32_hex(p + 1, &regno)) {
            rsp_send_err();
            return;
        }

        uint32_t core_reg = 0;
        if (!target_map_gdb_regnum(regno, &core_reg)) {
            rsp_send_empty(); // register not exposed by this architecture
            return;
        }

        if (!target_halt()) {
            rsp_send_err();
            return;
        }
        rsp_running = false;

        uint32_t val = 0;
        if (!target_read_reg(core_reg, &val)) {
            rsp_send_err();
            return;
        }
        rsp_send_u32_le(val);
        return;
    }

    if (p[0] == 'P') {
        // Pn=val (val is encoded as bytes, like in 'g')
        uint32_t regno = 0;
        const char *q = NULL;
        if (!parse_u32_hex_stop(p + 1, '=', &regno, &q)) {
            rsp_send_err();
            return;
        }
        uint32_t val = 0;
        if (!parse_u32_le_hex_bytes(q, &val)) {
            rsp_send_err();
            return;
        }

        uint32_t core_reg = 0;
        if (!target_map_gdb_regnum(regno, &core_reg)) {
            rsp_send_empty();
            return;
        }

        if (!target_halt()) {
            rsp_send_err();
            return;
        }
        rsp_running = false;

        if (!target_write_reg(core_reg, val)) {
            rsp_send_err();
            return;
        }
        rsp_send_ok();
        return;
    }

#if RSP_HAVE_TARGET_XML
    if (strncmp(p, "qXfer:features:read:", (sizeof("qXfer:features:read:") - 1u)) == 0) {
        handle_qXfer_features_read(p);
        return;
    }
#endif

#if RSP_HAVE_FLASH
    if (strncmp(p, "qXfer:memory-map:read::", (sizeof("qXfer:memory-map:read::") - 1u)) == 0) {
        if (!rsp_mmap_build()) {
            rsp_send_empty();
            return;
        }
        rsp_send_xfer_chunk(rsp_mmap_xml, rsp_mmap_len,
                            p + (sizeof("qXfer:memory-map:read::") - 1u));
        return;
    }

    if (strncmp(p, "vFlashErase:", 12) == 0) {
        uint32_t    addr = 0, len = 0;
        const char *q = NULL;
        if (!parse_u32_hex_stop(p + 12, ',', &addr, &q) || !parse_u32_hex(q, &len)) {
            flash_mspm0_abort();
            rsp_send_err();
            return;
        }
        if (!target_halt()) {
            flash_mspm0_abort();
            rsp_send_err();
            return;
        }
        rsp_running = false;
        if (!flash_mspm0_erase_range(addr, len)) {
            flash_mspm0_abort();
            rsp_send_err();
            return;
        }
        rsp_send_ok();
        return;
    }

    if (strncmp(p, "vFlashWrite:", 12) == 0) {
        // vFlashWrite:addr:binary-data (0x7d-escaped)
        uint32_t    addr = 0;
        const char *r = NULL;
        if (!parse_u32_hex_stop(p + 12, ':', &addr, &r)) {
            flash_mspm0_abort();
            rsp_send_err();
            return;
        }
        char    *data = rsp_buf + (r - p);
        uint32_t raw  = rsp_len - (uint32_t) (r - p);
        uint32_t n = 0u;
        if (!rsp_unescape(data, raw, &n)) {
            flash_mspm0_abort();
            rsp_send_err();
            return;
        }
        if (!target_halt()) {
            flash_mspm0_abort();
            rsp_send_err();
            return;
        }
        rsp_running = false;
        if (!flash_mspm0_write(addr, (const uint8_t *) data, n)) {
            flash_mspm0_abort();
            rsp_send_err();
            return;
        }
        rsp_send_ok();
        return;
    }

    if (strcmp(p, "vFlashDone") == 0) {
        if (!flash_mspm0_done()) {
            flash_mspm0_abort();
            rsp_send_err();
            return;
        }
        rsp_send_ok();
        return;
    }
#endif

    if (strncmp(p, "qSupported", 10) == 0 &&
        (p[10] == '\0' || p[10] == ':')) {
        handle_qSupported();
        return;
    }

    if (strcmp(p, "QStartNoAckMode") == 0) {
        rsp_send_ok();
        rsp_noack_mode = true; // takes effect for subsequent packets
        return;
    }

    if (strncmp(p, "qAttached", 9) == 0 &&
        (p[9] == '\0' || p[9] == ':')) {
        rsp_send_packet_str("1");
        return;
    }

    if (p[0] == 'Z' || p[0] == 'z') {
        handle_breakpoint(p);
        return;
    }

    if (p[0] == 'D' || p[0] == 'k') {
        rsp_running = false;
#if RSP_HAVE_FLASH
        flash_mspm0_abort();
#endif
        if (!target_debug_resources_clear()) {
            rsp_send_err();
            return;
        }
        if (!target_continue()) {
            bool halted = false;
            if (!target_is_halted(&halted)) {
                // Detach did not complete and run state is unknown. Retain the
                // attachment and polling so a later observed stop is reported.
                rsp_running = true;
                rsp_send_err();
                return;
            }
            if (halted) {
                rsp_send_err();
                return;
            }
            // Confirmed running is the requested detach outcome even if the
            // driver's final resume cleanup/acknowledgment failed.
        }
        target_disconnect();
        rsp_send_ok();
        return;
    }

    rsp_send_empty();
}

void rsp_init(void)
{
    rsp_state  = RSP_IDLE;
    rsp_len    = 0;
    rsp_sum    = 0;
    rsp_rx_csum = 0;
    rsp_tx_len = 0;
    rsp_tx_valid = false;
    rsp_running = false;
    rsp_noack_mode = false;
#if RSP_HAVE_FLASH
    flash_mspm0_abort();
#endif
}

void rsp_process_byte(uint8_t c)
{
    switch (rsp_state) {
    case RSP_IDLE:
        if (c == '$') {
            rsp_state = RSP_IN_PKT;
            rsp_len   = 0;
            rsp_sum   = 0;
            rsp_tx_valid = false;
        } else if (c == 0x03) {
            // Ctrl-C interrupt. Only recognized between packets: GDB sends
            // it as a lone byte, and 0x03 inside a packet body (e.g. binary
            // X data) is payload, not an interrupt.
            if (target_halt()) {
                rsp_running = false;
                rsp_send_sigint();
            }
            // On halt failure send nothing: a timeout at the GDB end is
            // more truthful than claiming a stop that didn't happen.
        } else if (c == '-' && !rsp_noack_mode) {
            // GDB rejected our last packet (checksum error on its side):
            // the spec requires retransmission.
            rsp_retransmit_last();
        }
        // '+' acks and line noise are ignored.
        break;

    case RSP_IN_PKT:
        if (c == '#') {
            rsp_state = RSP_IN_CSUM1;
        } else {
            if (rsp_len < RSP_MAX_PAYLOAD) {
                rsp_buf[rsp_len++] = (char) c;
                rsp_sum            = (uint8_t) (rsp_sum + c);
            } else {
                // Oversized packet: swallow the rest, then NACK it so the
                // sender fails fast instead of waiting for a timeout.
                rsp_state = RSP_DISCARD;
                rsp_len   = 0;
            }
        }
        break;

    case RSP_DISCARD:
        if (c == '#') {
            rsp_state = RSP_DISCARD_CS1;
        }
        break;

    case RSP_DISCARD_CS1:
        rsp_state = RSP_DISCARD_CS2;
        break;

    case RSP_DISCARD_CS2:
        if (!rsp_noack_mode) {
            uart_putc('-');
        }
        rsp_state = RSP_IDLE;
        break;

    case RSP_IN_CSUM1: {
        uint8_t hi = hex_nibble((char) c);
        if (hi == 0xFF) {
            if (!rsp_noack_mode) {
                uart_putc('-');
            }
            rsp_state = RSP_IDLE;
            rsp_len   = 0u;
            break;
        }
        rsp_rx_csum = (uint8_t) (hi << 4);
        rsp_state   = RSP_IN_CSUM2;
        break;
    }

    case RSP_IN_CSUM2: {
        uint8_t lo = hex_nibble((char) c);
        if (lo == 0xFF) {
            if (!rsp_noack_mode) {
                uart_putc('-');
            }
            rsp_state = RSP_IDLE;
            rsp_len   = 0u;
            break;
        }
        rsp_rx_csum |= lo;

        if (rsp_rx_csum == rsp_sum) {
            if (!rsp_noack_mode) {
                uart_putc('+');
            }
            rsp_handle_command();
        } else if (!rsp_noack_mode) {
            uart_putc('-');
        }
        // In no-ack mode a corrupted packet is silently dropped; the
        // transport is assumed reliable, so this should not happen.

        rsp_state = RSP_IDLE;
        rsp_len   = 0;
        break;
    }

    default:
        rsp_state = RSP_IDLE;
        break;
    }
}

void rsp_poll(void)
{
    if (!rsp_running) {
        return;
    }
    bool halted = false;
    if (!target_is_halted(&halted)) {
        return;
    }
    if (halted) {
        rsp_running = false;
        target_watch_t wt = TARGET_WATCH_ACCESS;
        uint32_t        wa = 0;
        if (target_watchpoint_hit(&wt, &wa)) {
            rsp_send_trap_watchpoint(wt, wa);
        } else {
            rsp_send_sigtrap();
        }
    }
}
