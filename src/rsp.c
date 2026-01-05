// Minimal GDB RSP over UART transport + command handling

#include "rsp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "target.h"
#include "hal.h"
#include "target_mem.h"

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

static uint8_t rsp_iobuf[RSP_IOBUF_SIZE];

typedef enum {
    RSP_IDLE = 0,
    RSP_IN_PKT,
    RSP_IN_CSUM1,
    RSP_IN_CSUM2
} rsp_state_t;

static rsp_state_t rsp_state = RSP_IDLE;
static char        rsp_buf[RSP_MAX_PAYLOAD + 1u];
static uint32_t    rsp_len     = 0;
static uint8_t     rsp_sum     = 0;
static uint8_t     rsp_rx_csum = 0;

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
    uart_putc((uint8_t) nibble_hex(v >> 4));
    uart_putc((uint8_t) nibble_hex(v));
}

static void rsp_put_hex_u32_le(uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        uint8_t b = (uint8_t) (v & 0xFF);
        rsp_put_hex_u8(b);
        v >>= 8;
    }
}

static bool parse_u32_hex(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (!s || !*s) {
        return false;
    }
    while (*s) {
        uint8_t n = hex_nibble(*s);
        if (n == 0xFF) {
            break;
        }
        v = (v << 4) | n;
        s++;
    }
    *out = v;
    return true;
}

static bool parse_u32_hex_stop(const char *s, char stop, uint32_t *out, const char **endp)
{
    uint32_t    v = 0;
    const char *p = s;
    if (!p || !*p) {
        return false;
    }
    while (*p && *p != stop) {
        uint8_t n = hex_nibble(*p);
        if (n == 0xFF) {
            return false;
        }
        v = (v << 4) | n;
        p++;
    }
    if (*p != stop) {
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
    uart_putc('$');
}

static void rsp_send_packet_end(uint8_t sum)
{
    uart_putc('#');
    rsp_put_hex_u8(sum);
}

static void rsp_send_packet_str(const char *payload)
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    while (*payload) {
        uint8_t c = (uint8_t) *payload++;
        sum       = (uint8_t) (sum + c);
        uart_putc(c);
    }
    rsp_send_packet_end(sum);
}

static void rsp_send_packet_bytes(const char *payload, uint32_t len)
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t) payload[i];
        sum       = (uint8_t) (sum + c);
        uart_putc(c);
    }
    rsp_send_packet_end(sum);
}

static void rsp_send_packet_prefix_and_bytes(char prefix, const char *payload, uint32_t len)
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    sum = (uint8_t) (sum + (uint8_t) prefix);
    uart_putc((uint8_t) prefix);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t) payload[i];
        sum       = (uint8_t) (sum + c);
        uart_putc(c);
    }
    rsp_send_packet_end(sum);
}

static void rsp_send_ok(void) { rsp_send_packet_str("OK"); }
static void rsp_send_err(void) { rsp_send_packet_str("E01"); }
static void rsp_send_empty(void) { rsp_send_packet_str(""); }

static void rsp_send_sigtrap(void)
{
    // SIGTRAP is 5
    rsp_send_packet_str("S05");
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
        uart_putc(c);
    }

    while (*tag) {
        uint8_t c = (uint8_t) *tag++;
        sum       = (uint8_t) (sum + c);
        uart_putc(c);
    }
    sum = (uint8_t) (sum + (uint8_t) ':');
    uart_putc((uint8_t) ':');

    for (int i = 7; i >= 0; i--) {
        char c = nibble_hex((addr >> (4u * (uint32_t) i)) & 0xFu);
        sum    = (uint8_t) (sum + (uint8_t) c);
        uart_putc((uint8_t) c);
    }

    sum = (uint8_t) (sum + (uint8_t) ';');
    uart_putc((uint8_t) ';');

    rsp_send_packet_end(sum);
}

static bool rsp_parse_hex_byte(const char *p, uint8_t *out)
{
    uint8_t hi = hex_nibble(p[0]);
    uint8_t lo = hex_nibble(p[1]);
    if (hi == 0xFF || lo == 0xFF) {
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
    return true;
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
        uart_putc((uint8_t) h1);
        sum = (uint8_t) (sum + (uint8_t) h2);
        uart_putc((uint8_t) h2);
    }
    rsp_send_packet_end(sum);
}

static void rsp_send_regs_hex(const uint32_t regs[17])
{
    uint8_t sum;
    rsp_send_packet_begin(&sum);
    for (int i = 0; i < 17; i++) {
        uint32_t v = regs[i];
        for (int j = 0; j < 4; j++) {
            uint8_t b  = (uint8_t) (v & 0xFF);
            char    h1 = nibble_hex(b >> 4);
            char    h2 = nibble_hex(b);
            sum        = (uint8_t) (sum + (uint8_t) h1);
            uart_putc((uint8_t) h1);
            sum = (uint8_t) (sum + (uint8_t) h2);
            uart_putc((uint8_t) h2);
            v >>= 8;
        }
    }
    rsp_send_packet_end(sum);
}

static bool rsp_parse_regs_hex(const char *hex, uint32_t regs[17])
{
    for (int i = 0; i < 17; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++) {
            uint8_t b;
            if (!rsp_parse_hex_byte(hex + (i * 8 + j * 2), &b)) {
                return false;
            }
            v |= ((uint32_t) b << (8u * j));
        }
        regs[i] = v;
    }
    return true;
}

static void handle_qSupported(void)
{
#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
    rsp_send_packet_str("PacketSize=" RSP_PACKET_SIZE_HEX ";swbreak+;hwbreak+;qXfer:features:read+");
#else
    rsp_send_packet_str("PacketSize=" RSP_PACKET_SIZE_HEX ";swbreak+;hwbreak+");
#endif
}

static bool rsp_running = false;

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
        uart_putc((uint8_t) h1);
        sum = (uint8_t) (sum + (uint8_t) h2);
        uart_putc((uint8_t) h2);
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

#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
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
    if (annex_len != 9u || strncmp(annex, "target.xml", annex_len) != 0) {
        rsp_send_empty();
        return;
    }

    uint32_t    off = 0, len = 0;
    const char *r = NULL;
    if (!parse_u32_hex_stop(q + 1, ',', &off, &r)) {
        rsp_send_err();
        return;
    }
    if (!parse_u32_hex(r, &len)) {
        rsp_send_err();
        return;
    }

    const char *xml     = NULL;
    uint32_t    xml_len = 0;
    if (!target_xml_get(&xml, &xml_len) || !xml) {
        rsp_send_empty();
        return;
    }

    if (off >= xml_len) {
        rsp_send_packet_str("l");
        return;
    }

    uint32_t remaining = xml_len - off;
    if (len > remaining) {
        len = remaining;
    }
    if (len > (RSP_MAX_PAYLOAD - 1u)) {
        len = (RSP_MAX_PAYLOAD - 1u);
    }

    char more = ((off + len) < xml_len) ? 'm' : 'l';
    rsp_send_packet_prefix_and_bytes(more, xml + off, len);
}
#endif

static void rsp_handle_command(void)
{
    rsp_buf[rsp_len] = '\0';
    const char *p    = rsp_buf;

    if (p[0] == '?' && p[1] == '\0') {
        rsp_send_sigtrap();
        return;
    }

    if (p[0] == 'g' && p[1] == '\0') {
        uint32_t regs[17];
        if (!target_halt()) {
            rsp_send_err();
            return;
        }
        if (!target_read_gdb_regs(regs, 17)) {
            rsp_send_err();
            return;
        }
        rsp_send_regs_hex(regs);
        return;
    }

    if (p[0] == 'G') {
        uint32_t regs[17];
        if (!target_halt()) {
            rsp_send_err();
            return;
        }
        if (!rsp_parse_regs_hex(p + 1, regs)) {
            rsp_send_err();
            return;
        }
        if (!target_write_gdb_regs(regs, 17)) {
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
            if (!target_write_reg(15, addr)) {
                rsp_send_err();
                return;
            }
        }

        if (!target_continue()) {
            rsp_send_err();
            return;
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
            if (!target_write_reg(15, addr)) {
                rsp_send_err();
                return;
            }
        }

        if (!target_step()) {
            rsp_send_err();
            return;
        }
        rsp_send_sigtrap();
        return;
    }

    if (p[0] == 'p') {
        uint32_t regno = 0;
        if (!parse_u32_hex(p + 1, &regno)) {
            rsp_send_err();
            return;
        }

        if (!target_halt()) {
            rsp_send_err();
            return;
        }

        uint32_t val = 0;
        uint32_t core_reg = regno;
        if (regno == 25) {
            core_reg = 16; // CPSR -> xPSR alias for M-profile
        }
        if (core_reg <= 16) {
            if (!target_read_reg(core_reg, &val)) {
                rsp_send_err();
                return;
            }
            rsp_send_u32_le(val);
            return;
        }

        rsp_send_empty();
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

        if (!target_halt()) {
            rsp_send_err();
            return;
        }

        uint32_t core_reg = regno;
        if (regno == 25) {
            core_reg = 16;
        }
        if (core_reg <= 16) {
            if (!target_write_reg(core_reg, val)) {
                rsp_send_err();
                return;
            }
            rsp_send_ok();
            return;
        }

        rsp_send_empty();
        return;
    }

#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
    if (strncmp(p, "qXfer:features:read:", (sizeof("qXfer:features:read:") - 1u)) == 0) {
        handle_qXfer_features_read(p);
        return;
    }
#endif

    if (strncmp(p, "qSupported", 10) == 0) {
        handle_qSupported();
        return;
    }

    if (strncmp(p, "qAttached", 9) == 0) {
        rsp_send_packet_str("1");
        return;
    }

    if (p[0] == 'Z' || p[0] == 'z') {
        handle_breakpoint(p);
        return;
    }

    if (p[0] == 'D' || p[0] == 'k') {
        rsp_running = false;
        (void) target_continue();
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
    rsp_running = false;
}

void rsp_process_byte(uint8_t c)
{
    // Ctrl-C (0x03) is out-of-band interrupt
    if (c == 0x03) {
        rsp_running = false;
        (void) target_halt();
        rsp_send_sigtrap();
        rsp_state = RSP_IDLE;
        rsp_len   = 0;
        return;
    }

    switch (rsp_state) {
    case RSP_IDLE:
        if (c == '$') {
            rsp_state = RSP_IN_PKT;
            rsp_len   = 0;
            rsp_sum   = 0;
        }
        break;

    case RSP_IN_PKT:
        if (c == '#') {
            rsp_state = RSP_IN_CSUM1;
        } else {
            if (rsp_len < RSP_MAX_PAYLOAD) {
                rsp_buf[rsp_len++] = (char) c;
                rsp_sum            = (uint8_t) (rsp_sum + c);
            } else {
                rsp_state = RSP_IDLE;
                rsp_len   = 0;
            }
        }
        break;

    case RSP_IN_CSUM1: {
        uint8_t hi = hex_nibble((char) c);
        if (hi == 0xFF) {
            rsp_state = RSP_IDLE;
            break;
        }
        rsp_rx_csum = (uint8_t) (hi << 4);
        rsp_state   = RSP_IN_CSUM2;
        break;
    }

    case RSP_IN_CSUM2: {
        uint8_t lo = hex_nibble((char) c);
        if (lo == 0xFF) {
            rsp_state = RSP_IDLE;
            break;
        }
        rsp_rx_csum |= lo;

        if (rsp_rx_csum == rsp_sum) {
            uart_putc('+');
            rsp_handle_command();
        } else {
            uart_putc('-');
        }

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
