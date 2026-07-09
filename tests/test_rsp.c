// Host-side tests for src/rsp.c against the mock environment.
// Build via tests/CMakeLists.txt in full, tiny XML, and tiny legacy-no-XML
// profiles, matching the supported firmware/GDB register layouts.

#include <stdio.h>
#include <string.h>

#include "mock_env.h"
#include "rsp.h"

#ifndef TEST_LEGACY
#define TEST_LEGACY 0
#endif
#ifndef TEST_TINY_RISCV
#define TEST_TINY_RISCV 0
#endif

#if TEST_LEGACY
#define TEST_CM_REMOTE_REGS 42u
#define TEST_CM_XPSR_INDEX  41u
#else
#define TEST_CM_REMOTE_REGS 17u
#define TEST_CM_XPSR_INDEX  16u
#endif

static int g_fail;

#define T_ASSERT(cond)                                                    \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

static void t_begin(const char *name)
{
    printf("  %s\n", name);
    mock_reset();
    rsp_init();
}

// ---------------- wire helpers ----------------

static void feed(const char *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        rsp_process_byte((uint8_t) bytes[i]);
    }
}

static void tx_clear(void) { mock_tx_len = 0; }

static uint8_t csum(const char *payload, size_t n)
{
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s = (uint8_t) (s + (uint8_t) payload[i]);
    }
    return s;
}

static char hexc(uint8_t v)
{
    v &= 0xF;
    return (v < 10) ? (char) ('0' + v) : (char) ('a' + v - 10);
}

// Send a framed packet with raw payload bytes (caller pre-escapes binary).
static void send_raw(const char *payload, size_t n)
{
    char buf[2048];
    size_t w = 0;
    buf[w++] = '$';
    memcpy(&buf[w], payload, n);
    w += n;
    uint8_t s = csum(payload, n);
    buf[w++] = '#';
    buf[w++] = hexc(s >> 4);
    buf[w++] = hexc(s);
    feed(buf, w);
}

static void send_str(const char *payload) { send_raw(payload, strlen(payload)); }

// Parse the first reply packet out of mock_tx. Returns the ack character
// seen before it ('+', '-', or 0 for none) and copies the payload out.
static char get_reply(char *out, size_t outsz, size_t *outlen)
{
    char   ack = 0;
    size_t i   = 0;
    while (i < mock_tx_len && mock_tx[i] != '$') {
        if (mock_tx[i] == '+' || mock_tx[i] == '-') {
            ack = mock_tx[i];
        }
        i++;
    }
    if (i >= mock_tx_len) {
        if (outlen) *outlen = 0;
        if (outsz) out[0] = '\0';
        return ack;
    }
    i++; // skip '$'
    size_t n = 0;
    while (i < mock_tx_len && mock_tx[i] != '#' && n + 1 < outsz) {
        out[n++] = mock_tx[i++];
    }
    out[n] = '\0';
    // verify checksum
    T_ASSERT(i + 2 < mock_tx_len && mock_tx[i] == '#');
    if (i + 2 < mock_tx_len) {
        uint8_t want = csum(out, n);
        char    c1 = mock_tx[i + 1], c2 = mock_tx[i + 2];
        char    e1 = hexc(want >> 4), e2 = hexc(want);
        T_ASSERT(c1 == e1 && c2 == e2);
    }
    if (outlen) *outlen = n;
    return ack;
}

// Round-trip helper: send command payload, return reply payload.
static char reply_buf[2048];
static const char *xact(const char *cmd)
{
    tx_clear();
    send_str(cmd);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    return reply_buf;
}

// Escape binary data the way GDB does for X / vFlashWrite payloads.
static size_t escape_bin(const uint8_t *in, size_t n, char *out)
{
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (c == '$' || c == '#' || c == '}' || c == '*') {
            out[w++] = '}';
            out[w++] = (char) (c ^ 0x20u);
        } else {
            out[w++] = (char) c;
        }
    }
    return w;
}

// ---------------- tests ----------------

static void test_qsupported(void)
{
    t_begin("qSupported advertises the right features");
    const char *r = xact("qSupported:multiprocess+;swbreak+");
#if TEST_LEGACY
    T_ASSERT(strstr(r, "PacketSize=151") != NULL);
    T_ASSERT(strstr(r, "qXfer:features:read+") == NULL);
    T_ASSERT(strstr(r, "qXfer:memory-map:read+") == NULL);
#elif TEST_TINY_RISCV
    T_ASSERT(strstr(r, "PacketSize=109") != NULL);
    T_ASSERT(strstr(r, "qXfer:features:read+") != NULL);
    T_ASSERT(strstr(r, "qXfer:memory-map:read+") == NULL);
#elif TEST_TINY
    T_ASSERT(strstr(r, "PacketSize=100") != NULL);
    T_ASSERT(strstr(r, "qXfer:features:read+") != NULL);
    T_ASSERT(strstr(r, "qXfer:memory-map:read+") == NULL);
#else
    T_ASSERT(strstr(r, "PacketSize=200") != NULL);
    T_ASSERT(strstr(r, "qXfer:features:read+") != NULL);
    T_ASSERT(strstr(r, "qXfer:memory-map:read+") != NULL);
#endif
    T_ASSERT(strstr(r, "QStartNoAckMode+") != NULL);
    // swbreak/hwbreak advertise precise stop-reason reporting, not Z-packet
    // support. Breakpoints remain available through normal Z0/Z1 probing.
    T_ASSERT(strstr(r, "swbreak+") == NULL);
    T_ASSERT(strstr(r, "hwbreak+") == NULL);
}

static void test_bad_checksum_nack(void)
{
    t_begin("corrupted packet gets a NACK and no reply");
    tx_clear();
    feed("$qSupported#00", 14); // wrong checksum
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '-');
}

static void test_malformed_checksum_nack(void)
{
    t_begin("non-hex checksum digits are NACKed");
    tx_clear();
    feed("$?#g0", 5);
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '-');

    tx_clear();
    feed("$?#3g", 5);
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '-');
}

static void test_ack_then_reply(void)
{
    t_begin("valid packet is ACKed before the reply");
    tx_clear();
    send_str("?");
    char ack = get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(ack == '+');
    T_ASSERT(strcmp(reply_buf, "S05") == 0);
    T_ASSERT(mock_halt_calls == 1);
}

static void test_question_rejects_missing_target(void)
{
    t_begin("? reports an error when target attach fails");
    mock_arch = MOCK_ARCH_NONE;
    mock_attach_result = MOCK_ARCH_NONE;
    T_ASSERT(strcmp(xact("?"), "E01") == 0);
    T_ASSERT(mock_attach_calls == 1);
}

static void test_g_registers_cm(void)
{
    t_begin("g returns the selected Cortex-M register layout");
    const char *r = xact("g");
    T_ASSERT(strlen(r) == TEST_CM_REMOTE_REGS * 8u);
    // r0 = 0x11110000 little-endian hex
    T_ASSERT(strncmp(r, "00001111", 8) == 0);
    // xPSR (index 16) = 0x11110010
    T_ASSERT(strncmp(r + TEST_CM_XPSR_INDEX * 8u, "10001111", 8) == 0);
    T_ASSERT(mock_halt_calls >= 1);
}

static void test_G_write_registers(void)
{
    t_begin("G writes the full register block");
    char cmd[1 + TEST_CM_REMOTE_REGS * 8u + 1] = "G";
    for (uint32_t i = 0; i < TEST_CM_REMOTE_REGS; i++) {
        // value i as 32-bit little-endian hex: byte0 = i, rest zero
        char *w = cmd + 1 + i * 8;
        w[0] = hexc((uint8_t) i >> 4); w[1] = hexc((uint8_t) i);
        memcpy(w + 2, "000000", 6);
    }
    cmd[1 + TEST_CM_REMOTE_REGS * 8u] = '\0';
    const char *r = xact(cmd);
    T_ASSERT(strcmp(r, "OK") == 0);
    T_ASSERT(mock_regs[0] == 0x00 && mock_regs[5] == 0x05 &&
             mock_regs[16] == TEST_CM_XPSR_INDEX);
}

#if defined(PROBE_ENABLE_RISCV) && PROBE_ENABLE_RISCV
static void test_g_registers_rv(void)
{
    t_begin("g returns 33 RISC-V registers");
    mock_arch = MOCK_ARCH_RV;
    const char *r = xact("g");
    T_ASSERT(strlen(r) == 33u * 8u);

    // The largest tiny response must remain replayable after a NACK.
    tx_clear();
    feed("-", 1);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strlen(reply_buf) == 33u * 8u);
}

static void test_G_write_registers_rv(void)
{
    t_begin("G accepts the complete 265-byte tiny RV32 request");
    mock_arch = MOCK_ARCH_RV;
    char cmd[1 + 33 * 8 + 1] = "G";
    for (uint32_t i = 0; i < 33u; i++) {
        char *w = cmd + 1u + i * 8u;
        w[0] = hexc((uint8_t)i >> 4);
        w[1] = hexc((uint8_t)i);
        memcpy(w + 2, "000000", 6);
    }
    cmd[1 + 33 * 8] = '\0';
    T_ASSERT(strcmp(xact(cmd), "OK") == 0);
    T_ASSERT(mock_regs[32] == 32u);
}
#endif

static void test_memory_rw(void)
{
    t_begin("M then m round-trips memory");
    const char *r = xact("M20000010,4:deadbeef");
    T_ASSERT(strcmp(r, "OK") == 0);
    T_ASSERT(mock_mem[0x10] == 0xde && mock_mem[0x13] == 0xef);
    r = xact("m20000010,4");
    T_ASSERT(strcmp(r, "deadbeef") == 0);
}

static void test_parse_strictness(void)
{
    t_begin("malformed hex is rejected instead of read as address 0");
    T_ASSERT(strcmp(xact("mzz,4"), "E01") == 0);
    T_ASSERT(strcmp(xact("m100000000,4"), "E01") == 0); // 9 digits: would wrap
    T_ASSERT(strcmp(xact("m,4"), "E01") == 0);          // empty address
    T_ASSERT(strcmp(xact("M20000000,1:aabb"), "E01") == 0);
    T_ASSERT(strcmp(xact("P0=78563412junk"), "E01") == 0);
    T_ASSERT(strcmp(xact("mffffffff,2"), "E01") == 0);
}

static void test_truncated_M_oob(void)
{
    t_begin("max-length truncated M packet fails cleanly (OOB regression)");
    // Build a payload of exactly RSP_MAX_PAYLOAD bytes whose claimed length
    // needs one more hex pair than fits: the failing pair starts exactly at
    // rsp_buf[rsp_len] (the NUL terminator), and the pre-fix parser read
    // one byte past it before validating.
#if TEST_LEGACY
    const size_t maxp = 337;
#elif TEST_TINY_RISCV
    const size_t maxp = 265;
#elif TEST_TINY
    const size_t maxp = 256;
#else
    const size_t maxp = 512;
#endif
    char   cmd[600];
    size_t avail   = maxp - 12u;      // hex chars after a 12-char header
    size_t claimed = avail / 2u + 1u; // one pair more than supplied
    size_t hdr     = (size_t) snprintf(cmd, sizeof(cmd), "M2000000,%zx:", claimed);
    T_ASSERT(hdr == 12u);
    size_t n = hdr;
    while (n < maxp) {
        cmd[n++] = 'a';
    }
    tx_clear();
    send_raw(cmd, n);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "E01") == 0);
}

static void test_X_binary_write(void)
{
    t_begin("X binary write with escaped bytes");
    // Probe form first
    T_ASSERT(strcmp(xact("X20000000,0:"), "OK") == 0);

    uint8_t data[6] = {0x7d, 0x23, 0x24, 0x03, 0x2a, 0x00};
    char    payload[64];
    size_t  w = (size_t) snprintf(payload, sizeof(payload), "X20000020,6:");
    w += escape_bin(data, sizeof(data), payload + w);
    tx_clear();
    send_raw(payload, w);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "OK") == 0);
    T_ASSERT(memcmp(&mock_mem[0x20], data, 6) == 0);

    // Length mismatch is an error
    T_ASSERT(strcmp(xact("X20000000,8:ab"), "E01") == 0);

    // A dangling escape byte is malformed, not literal data.
    const char dangling[] = "X20000000,1:}";
    tx_clear();
    send_raw(dangling, sizeof(dangling) - 1u);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "E01") == 0);
}

static void test_ctrlc(void)
{
    t_begin("Ctrl-C: idle-only interrupt, S02 only on successful halt");
    tx_clear();
    feed("\x03", 1);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "S02") == 0);
    T_ASSERT(mock_halt_calls == 1);

    // Failed halt: no reply at all, but retain an outstanding continue so a
    // subsequently observed (ambiguously applied) halt can still be reported.
    tx_clear();
    send_str("c");
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '+');
    mock_fail_halt = true;
    tx_clear();
    feed("\x03", 1);
    T_ASSERT(mock_tx_len == 0);
    mock_fail_halt = false;
    mock_halted = true;
    tx_clear();
    rsp_poll();
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "S05") == 0);

    // 0x03 inside a packet body is data, not an interrupt (X test already
    // wrote one above); explicitly: an unknown packet containing 0x03.
    int halts_before = mock_halt_calls;
    tx_clear();
    char pkt[8] = {'u', 0x03, 'u'};
    send_raw(pkt, 3);
    T_ASSERT(mock_halt_calls == halts_before);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "") == 0); // unknown packet -> empty
}

static void test_breakpoints(void)
{
    t_begin("Z0/z0 insert and remove");
    T_ASSERT(strcmp(xact("Z0,20000100,2"), "OK") == 0);
    T_ASSERT(mock_bp_count == 1 && mock_bp[0] == 0x20000100u);
    T_ASSERT(strcmp(xact("z0,20000100,2"), "OK") == 0);
    T_ASSERT(mock_bp_count == 0);
}

static void test_p_P_mapping(void)
{
    t_begin("p/P per-architecture register numbering");
    // XML Cortex-M layout: xPSR is consecutive remote register 16. Legacy
    // ARM uses that number for FPA0 and exposes xPSR through CPSR slot 25.
    mock_regs[16] = 0xcafe0001u;
#if TEST_LEGACY
    const char *r = xact("p10");
    T_ASSERT(strcmp(r, "") == 0);
#else
    const char *r = xact("p10"); // 0x10 = 16
    T_ASSERT(strcmp(r, "0100feca") == 0);
#endif
    // Retain the historical CPSR alias for no-XML/older clients.
    T_ASSERT(strcmp(xact("p19"), "0100feca") == 0);
    // Unmapped register: empty reply
    T_ASSERT(strcmp(xact("p1a"), "") == 0);
    // P write to r5
    T_ASSERT(strcmp(xact("P5=78563412"), "OK") == 0);
    T_ASSERT(mock_regs[5] == 0x12345678u);

#if defined(PROBE_ENABLE_RISCV) && PROBE_ENABLE_RISCV
    mock_arch = MOCK_ARCH_RV;
    mock_regs[20] = 0x0000beefu;
    r = xact("p14"); // 0x14 = 20 -> x20, NOT remapped via the CM alias
    T_ASSERT(strcmp(r, "efbe0000") == 0);
    // PC is regno 32 (0x20)
    mock_regs[32] = 0x40u;
    r = xact("p20");
    T_ASSERT(strcmp(r, "40000000") == 0);
#endif
}

static void test_step_and_continue(void)
{
    t_begin("s replies S05; c goes silent until halt");
    const char *r = xact("s");
    T_ASSERT(strcmp(r, "S05") == 0);
    T_ASSERT(mock_step_calls == 1);

    tx_clear();
    send_str("c");
    // ack only, no reply while running
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '+');
    T_ASSERT(mock_continue_calls == 1);

    // Target halts on a watchpoint: rsp_poll emits T05watch
    mock_halted    = true;
    mock_watch_hit = true;
    mock_watch_addr = 0x20000042u;
    tx_clear();
    rsp_poll();
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strncmp(reply_buf, "T05watch:20000042;", 18) == 0);
}

static void test_question_consumes_running_stop(void)
{
    t_begin("? reports a synchronous stop only once");
    tx_clear();
    send_str("c");
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '+');
    T_ASSERT(!mock_halted);

    T_ASSERT(strcmp(xact("?"), "S05") == 0);
    T_ASSERT(mock_halted);

    tx_clear();
    rsp_poll();
    T_ASSERT(mock_tx_len == 0);
}

static void test_ambiguous_continue_remains_tracked(void)
{
    t_begin("ambiguous continue remains tracked until a stop is observed");
    mock_fail_continue = true;
    mock_continue_failure_runs = true;

    tx_clear();
    send_str("c");
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '+');
    T_ASSERT(!mock_halted);

    mock_fail_continue = false;
    mock_halted = true;
    tx_clear();
    rsp_poll();
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "S05") == 0);

    t_begin("confirmed-halted continue failure returns E01");
    mock_fail_continue = true;
    mock_continue_failure_runs = false;
    T_ASSERT(strcmp(xact("c"), "E01") == 0);
}

static void test_ambiguous_detach_state(void)
{
    t_begin("detach accepts confirmed running after ambiguous resume");
    mock_fail_continue = true;
    mock_continue_failure_runs = true;
    T_ASSERT(strcmp(xact("D"), "OK") == 0);
    T_ASSERT(mock_disconnect_calls == 1);

    t_begin("detach retains unknown run state for later polling");
    mock_fail_continue = true;
    mock_continue_failure_runs = true;
    mock_fail_halt_status = true;
    T_ASSERT(strcmp(xact("D"), "E01") == 0);
    T_ASSERT(mock_disconnect_calls == 0);

    mock_fail_halt_status = false;
    mock_halted = true;
    tx_clear();
    rsp_poll();
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "S05") == 0);
}

static void test_noack_mode(void)
{
    t_begin("QStartNoAckMode stops acks");
    T_ASSERT(strcmp(xact("QStartNoAckMode"), "OK") == 0);
    tx_clear();
    send_str("?");
    char ack = get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(ack == 0); // no '+' in no-ack mode
    T_ASSERT(strcmp(reply_buf, "S05") == 0);
    // qSupported re-enables acks (new session)
    xact("qSupported");
    tx_clear();
    send_str("?");
    ack = get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(ack == '+');
}

static void test_retransmit(void)
{
    t_begin("'-' triggers retransmission of the last reply");
    tx_clear();
    send_str("?");
    char first[64];
    get_reply(first, sizeof(first), NULL);
    T_ASSERT(strcmp(first, "S05") == 0);

    tx_clear();
    feed("-", 1);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "S05") == 0);

    // A new connection must not inherit retransmit state from the old one.
    rsp_init();
    tx_clear();
    feed("-", 1);
    T_ASSERT(mock_tx_len == 0);
}

static void test_oversized_packet(void)
{
    t_begin("oversized packet is drained and NACKed");
#if TEST_LEGACY
    const size_t maxp = 337;
#elif TEST_TINY_RISCV
    const size_t maxp = 265;
#elif TEST_TINY
    const size_t maxp = 256;
#else
    const size_t maxp = 512;
#endif
    tx_clear();
    rsp_process_byte('$');
    for (size_t i = 0; i < maxp + 50; i++) {
        rsp_process_byte('a');
    }
    rsp_process_byte('#');
    rsp_process_byte('0');
    rsp_process_byte('0');
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '-');
    // Parser recovered: a normal packet still works
    T_ASSERT(strcmp(xact("?"), "S05") == 0);
}

static void test_reattach_on_qsupported(void)
{
    t_begin("qSupported retries detection when no target attached");
    mock_arch          = MOCK_ARCH_NONE;
    mock_attach_result = MOCK_ARCH_CM;
    xact("qSupported");
    T_ASSERT(mock_attach_calls == 1);
    T_ASSERT(mock_arch == MOCK_ARCH_CM);
    // A new qSupported is a new session boundary and revalidates even a
    // cached attachment so a target power-cycle/replacement is recoverable.
    xact("qSupported");
    T_ASSERT(mock_attach_calls == 2);

    // A session boundary consumes any outstanding continue state. Production
    // probe_attach() halts; model the resulting halt and ensure it is not
    // reported again as an unsolicited stop.
    tx_clear();
    send_str("c");
    T_ASSERT(mock_tx_len == 1 && mock_tx[0] == '+');
    xact("qSupported");
    mock_halted = true;
    tx_clear();
    rsp_poll();
    T_ASSERT(mock_tx_len == 0);
}

#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && PROBE_ENABLE_QXFER_TARGET_XML
static void test_target_xml(void)
{
    t_begin("qXfer:features:read serves target.xml (annex regression)");
    const char *r = xact("qXfer:features:read:target.xml:0,fff");
    T_ASSERT(r[0] == 'l');
    T_ASSERT(strstr(r, "armv6-m") != NULL);
    // 9-char annex must NOT match
    r = xact("qXfer:features:read:target.xm:0,fff");
    T_ASSERT(strcmp(r, "") == 0);
}
#endif

#if defined(PROBE_ENABLE_RISCV_MINIMAL_XML) && PROBE_ENABLE_RISCV_MINIMAL_XML
static void test_runtime_riscv_minimal_xml(void)
{
    t_begin("legacy Cortex / minimal RV32 XML switch follows runtime target");
    T_ASSERT(strstr(xact("qSupported"), "qXfer:features:read+") == NULL);

    mock_arch = MOCK_ARCH_RV;
    mock_attach_result = MOCK_ARCH_RV;
    T_ASSERT(strstr(xact("qSupported"), "qXfer:features:read+") != NULL);
    const char *r = xact("qXfer:features:read:target.xml:0,fff");
    T_ASSERT(r[0] == 'l');
    T_ASSERT(strstr(r, "riscv:rv32") != NULL);
}
#endif

#if defined(PROBE_ENABLE_FLASH_MSPM0) && PROBE_ENABLE_FLASH_MSPM0
static void test_memory_map(void)
{
    t_begin("qXfer:memory-map:read serves detected flash geometry");
    const char *r = xact("qXfer:memory-map:read::0,fff");
    T_ASSERT(r[0] == 'l');
    T_ASSERT(strstr(r, "type=\"flash\"") != NULL);
    T_ASSERT(strstr(r, "length=\"0x8000\"") != NULL);
    T_ASSERT(strstr(r, "blocksize\">0x400<") != NULL);

    // No MSPM0 FLASHCTL on the target: feature degrades to unsupported
    mock_flash_geometry_ok = false;
    r = xact("qXfer:memory-map:read::0,fff");
    T_ASSERT(strcmp(r, "") == 0);
}

static void test_vflash(void)
{
    t_begin("vFlashErase/Write/Done program the mock flash");
    T_ASSERT(strcmp(xact("vFlashErase:0,800"), "OK") == 0);
    T_ASSERT(mock_flash_erase_calls == 1);
    T_ASSERT(mock_flash_erase_addr == 0 && mock_flash_erase_len == 0x800u);
    T_ASSERT(mock_halt_calls >= 1);

    uint8_t data[5] = {0x01, 0x7d, 0x23, 0xff, 0x2a};
    char    payload[64];
    size_t  w = (size_t) snprintf(payload, sizeof(payload), "vFlashWrite:400:");
    w += escape_bin(data, sizeof(data), payload + w);
    tx_clear();
    send_raw(payload, w);
    get_reply(reply_buf, sizeof(reply_buf), NULL);
    T_ASSERT(strcmp(reply_buf, "OK") == 0);
    T_ASSERT(memcmp(&mock_flash[0x400], data, 5) == 0);

    T_ASSERT(strcmp(xact("vFlashDone"), "OK") == 0);
    T_ASSERT(mock_flash_done_calls == 1);

    int aborts = mock_flash_abort_calls;
    T_ASSERT(strcmp(xact("vFlashWrite:zz:bad"), "E01") == 0);
    T_ASSERT(mock_flash_abort_calls == aborts + 1);
}
#endif

static void test_misc(void)
{
    t_begin("qAttached / detach / unknown packets");
    T_ASSERT(strcmp(xact("qAttached"), "1") == 0);
    T_ASSERT(strcmp(xact("D"), "OK") == 0);
    T_ASSERT(mock_continue_calls == 1);
    T_ASSERT(mock_debug_clear_calls == 1);
    T_ASSERT(mock_disconnect_calls == 1);
    T_ASSERT(strcmp(xact("vMustReplyEmpty"), "") == 0);
}

int main(void)
{
#if TEST_LEGACY
    printf("rsp tests (legacy no-XML profile)\n");
#elif TEST_TINY_RISCV
    printf("rsp tests (tiny RISC-V profile)\n");
#elif TEST_TINY
    printf("rsp tests (tiny profile)\n");
#else
    printf("rsp tests (full profile)\n");
#endif

    test_qsupported();
    test_bad_checksum_nack();
    test_malformed_checksum_nack();
    test_ack_then_reply();
    test_question_rejects_missing_target();
    test_g_registers_cm();
    test_G_write_registers();
#if defined(PROBE_ENABLE_RISCV) && PROBE_ENABLE_RISCV
    test_g_registers_rv();
    test_G_write_registers_rv();
#endif
    test_memory_rw();
    test_parse_strictness();
    test_truncated_M_oob();
    test_X_binary_write();
    test_ctrlc();
    test_breakpoints();
    test_p_P_mapping();
    test_step_and_continue();
    test_question_consumes_running_stop();
    test_ambiguous_continue_remains_tracked();
    test_ambiguous_detach_state();
    test_noack_mode();
    test_retransmit();
    test_oversized_packet();
    test_reattach_on_qsupported();
#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && PROBE_ENABLE_QXFER_TARGET_XML
    test_target_xml();
#endif
#if defined(PROBE_ENABLE_RISCV_MINIMAL_XML) && PROBE_ENABLE_RISCV_MINIMAL_XML
    test_runtime_riscv_minimal_xml();
#endif
#if defined(PROBE_ENABLE_FLASH_MSPM0) && PROBE_ENABLE_FLASH_MSPM0
    test_memory_map();
    test_vflash();
#endif
    test_misc();

    if (g_fail) {
        printf("%d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
