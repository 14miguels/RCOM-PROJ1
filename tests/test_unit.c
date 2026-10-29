// RCOM 2026/2027 - unit tests
//
// link_layer.c and application_layer.c are #included so that their static
// functions can be tested; the serial port is replaced by serial_mock.c, which
// lets each test script exactly what arrives on the "line" and inspect what
// the code wrote.

#include "../src/application_layer.c"
#include "../src/link_layer.c"
#include "serial_mock.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                   \
    do                                                                \
    {                                                                 \
        checks++;                                                     \
        if (!(cond))                                                  \
        {                                                             \
            failures++;                                               \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);          \
        }                                                             \
    } while (0)

#define RUN(fn)                                \
    do                                         \
    {                                          \
        int before = failures;                 \
        fn();                                  \
        printf("[%s] %s\n", failures == before ? "PASS" : "FAIL", #fn); \
    } while (0)

////////////////////////////////////////////////
// helpers
////////////////////////////////////////////////

static LinkLayer params(int timeout, int retx)
{
    LinkLayer p;
    memset(&p, 0, sizeof(p));
    snprintf(p.serialPort, sizeof(p.serialPort), "/dev/mock");
    p.baudRate = 9600;
    p.timeout = timeout;
    p.nRetransmissions = retx;
    return p;
}

static void resetAll(void)
{
    if (linkOpen)
        linkStop();
    mockReset();
    resync = FALSE;
}

// Answers scripted per write: script[i] lists control bytes (0-terminated)
// that the peer "sends back" after the i-th write; A is scriptA.
static unsigned char script[8][4];
static int scriptPos, scriptA;

static void scriptHook(void)
{
    if (scriptPos >= 8)
        return;
    for (int i = 0; i < 4 && script[scriptPos][i]; i++)
        mockPushSup(scriptA, script[scriptPos][i]);
    scriptPos++;
}

static void setScript(int a, const unsigned char s[][4], int n)
{
    memset(script, 0, sizeof(script));
    for (int i = 0; i < n; i++)
        memcpy(script[i], s[i], 4);
    scriptPos = 0;
    scriptA = a;
    mockOnWrite = scriptHook;
}

static void openTx(int timeout, int retx)
{
    resetAll();
    const unsigned char s[][4] = {{C_UA, 0, 0, 0}};
    setScript(A_TX, s, 1);
    CHECK(llOpenTx(params(timeout, retx)) == 0);
    mockOutLen = 0;
    mockOnWrite = NULL;
}

static void openRx(int timeout, int retx)
{
    resetAll();
    mockPushSup(A_TX, C_SET);
    CHECK(llOpenRx(params(timeout, retx)) == 0);
    mockOutLen = 0;
}

// Pushes a complete I frame as Tx would send it (optionally corrupted).
static void pushInfo(int ns, const unsigned char *data, int n, int corruptBcc2)
{
    unsigned char frame[MAX_FRAME_SIZE];
    int len = buildInfoFrame(frame, ns, data, n);
    if (corruptBcc2)
        frame[len - 3] ^= 0x01; // last byte before the closing FLAG (BCC2 unless stuffed)
    mockPush(frame, len);
}

static int outIs(const unsigned char *expected, int n)
{
    return mockOutLen == n && memcmp(mockOut, expected, n) == 0;
}

static int outIsReply(unsigned char c) // exactly one reply frame from Rx (A = 0x03)
{
    unsigned char f[5] = {FLAG, A_TX, c, A_TX ^ c, FLAG};
    return outIs(f, 5);
}

////////////////////////////////////////////////
// stuffing, BCC and frame construction
////////////////////////////////////////////////

static void test_stuffing(void)
{
    unsigned char out[8];
    CHECK(stuffByte(out, 0, 0x41) == 1 && out[0] == 0x41);
    CHECK(stuffByte(out, 0, 0x7E) == 2 && out[0] == 0x7D && out[1] == 0x5E);
    CHECK(stuffByte(out, 0, 0x7D) == 2 && out[0] == 0x7D && out[1] == 0x5D);
    CHECK(stuffByte(out, 3, 0x7E) == 5);
    CHECK(stuffByte(out, 0, 0x5E) == 1 && out[0] == 0x5E); // 0x5E alone is not special
    CHECK(stuffByte(out, 0, 0x00) == 1 && out[0] == 0x00);
}

static void test_bcc2(void)
{
    unsigned char d[] = {0x01, 0x7E, 0x7D, 0x02};
    CHECK(computeBcc2(d, 0) == 0);
    CHECK(computeBcc2(d, 1) == 0x01);
    CHECK(computeBcc2(d, 4) == 0x00); // 01^7E^7D^02
    unsigned char e[] = {0xFF, 0x0F};
    CHECK(computeBcc2(e, 2) == 0xF0);
}

static void test_build_info_frame(void)
{
    unsigned char f[MAX_FRAME_SIZE];
    unsigned char d[] = {0x01, 0x7E, 0x7D, 0x02};
    // BCC2 over ORIGINAL bytes = 0x00; stuffing only inside the data field
    unsigned char exp0[] = {0x7E, 0x03, 0x00, 0x03, 0x01, 0x7D, 0x5E, 0x7D, 0x5D, 0x02, 0x00, 0x7E};
    int n = buildInfoFrame(f, 0, d, 4);
    CHECK(n == (int)sizeof(exp0) && memcmp(f, exp0, n) == 0);

    // I1: C = 0x80, BCC1 = 0x03 ^ 0x80 = 0x83
    n = buildInfoFrame(f, 1, d, 4);
    CHECK(f[2] == 0x80 && f[3] == 0x83);

    // BCC2 that equals FLAG must itself be stuffed
    unsigned char one[] = {0x7E};
    unsigned char exp1[] = {0x7E, 0x03, 0x00, 0x03, 0x7D, 0x5E, 0x7D, 0x5E, 0x7E};
    n = buildInfoFrame(f, 0, one, 1);
    CHECK(n == (int)sizeof(exp1) && memcmp(f, exp1, n) == 0);

    // empty payload: only BCC2 (= 0)
    unsigned char exp2[] = {0x7E, 0x03, 0x00, 0x03, 0x00, 0x7E};
    n = buildInfoFrame(f, 0, d, 0);
    CHECK(n == (int)sizeof(exp2) && memcmp(f, exp2, n) == 0);

    // worst case never exceeds MAX_FRAME_SIZE
    unsigned char big[MAX_PAYLOAD_SIZE];
    memset(big, 0x7E, sizeof(big));
    n = buildInfoFrame(f, 0, big, MAX_PAYLOAD_SIZE);
    CHECK(n <= MAX_FRAME_SIZE && n >= 2000);
    memset(big, 0x7E, 999);
    big[999] = 0x00; // xor of the data = 0x7E: BCC2 is stuffed too
    n = buildInfoFrame(f, 0, big, MAX_PAYLOAD_SIZE);
    CHECK(n <= MAX_FRAME_SIZE);
}

////////////////////////////////////////////////
// receive state machine
////////////////////////////////////////////////

static void test_receive_supervision(void)
{
    Frame f;
    resetAll();
    mockPushSup(A_TX, C_SET);
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK);
    CHECK(!f.isInfo && f.a == A_TX && f.c == C_SET);

    // garbage before the FLAG, repeated FLAGs, then UA from Rx side
    unsigned char junk[] = {0x12, 0x03, 0x07, 0x7E, 0x7E, 0x7E, 0x01, 0x07, 0x06, 0x7E};
    mockPush(junk, sizeof(junk));
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK);
    CHECK(f.a == A_RX && f.c == C_UA);

    // two frames sharing one FLAG: 7E A C B 7E A C B 7E
    resetAll();
    unsigned char shared[] = {0x7E, 0x03, 0x07, 0x04, 0x7E, 0x01, 0x0B, 0x0A, 0x7E};
    mockPush(shared, sizeof(shared));
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.c == C_UA);
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.c == C_DISC && f.a == A_RX);
}

static void test_receive_bad_bcc1_and_resync(void)
{
    Frame f;
    resetAll();
    memset(&stats, 0, sizeof(stats));
    unsigned char bad[] = {0x7E, 0x03, 0x07, 0x00, 0x7E}; // BCC1 wrong
    mockPush(bad, sizeof(bad));
    mockPushSup(A_TX, C_SET); // valid one right after
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK);
    CHECK(f.c == C_SET);
    CHECK(stats.badBcc1 == 1);

    // unknown address / control are ignored too
    resetAll();
    unsigned char unk[] = {0x7E, 0x55, 0x07, 0x52, 0x7E, 0x7E, 0x03, 0x99, 0x9A, 0x7E};
    mockPush(unk, sizeof(unk));
    mockPushSup(A_TX, C_DISC);
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.c == C_DISC);

    // truncated frame followed by a good one
    resetAll();
    unsigned char part[] = {0x7E, 0x03, 0x07};
    mockPush(part, sizeof(part));
    mockPushSup(A_TX, C_SET);
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.c == C_SET);
}

static void test_receive_timeout_not_extended_by_garbage(void)
{
    Frame f;
    resetAll();
    unsigned char junk[200];
    memset(junk, 0x55, sizeof(junk)); // never a FLAG
    mockPush(junk, sizeof(junk));
    long long t0 = nowMs();
    CHECK(receiveFrame(&f, t0 + 200) == RX_TIMEOUT);
    long long dt = nowMs() - t0;
    CHECK(dt >= 190 && dt < 600);
}

static void test_receive_info(void)
{
    Frame f;
    unsigned char d[] = {0x01, 0x7E, 0x7D, 0x5E, 0x5D, 0x00, 0xFF};

    resetAll();
    pushInfo(0, d, sizeof(d), 0);
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK);
    CHECK(f.isInfo && f.c == C_I0 && f.dataOk);
    CHECK(f.dataLen == (int)sizeof(d) + 1 && memcmp(f.data, d, sizeof(d)) == 0);

    pushInfo(1, d, sizeof(d), 0);
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.c == C_I1 && f.dataOk);

    // wrong BCC2
    pushInfo(0, d, sizeof(d), 1);
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.isInfo && !f.dataOk);

    // empty payload is valid (only BCC2)
    pushInfo(0, d, 0, 0);
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.dataOk && f.dataLen == 1);

    // header followed directly by FLAG: no BCC2 at all
    unsigned char nodata[] = {0x7E, 0x03, 0x00, 0x03, 0x7E};
    mockPush(nodata, sizeof(nodata));
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.isInfo && !f.dataOk);

    // invalid escape sequence 7D 00
    unsigned char badesc[] = {0x7E, 0x03, 0x00, 0x03, 0x41, 0x7D, 0x00, 0x41, 0x7E};
    mockPush(badesc, sizeof(badesc));
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && !f.dataOk);

    // incomplete escape: ESC immediately before the closing FLAG
    unsigned char escend[] = {0x7E, 0x03, 0x00, 0x03, 0x41, 0x7D, 0x7E};
    mockPush(escend, sizeof(escend));
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && !f.dataOk);

    // the escaped FLAG (7D 5E) is data, not the end of the frame
    unsigned char escflag[] = {0x7E, 0x03, 0x00, 0x03, 0x7D, 0x5E, 0x7D, 0x5E, 0x7E}; // data 7E, bcc 7E
    mockPush(escflag, sizeof(escflag));
    CHECK(receiveFrame(&f, nowMs() + 500) == RX_OK && f.dataOk && f.dataLen == 2 && f.data[0] == 0x7E);
}

static void test_receive_oversized(void)
{
    Frame f;
    resetAll();
    // 3000 data bytes: far more than the buffer; must not overflow (ASan) and must be flagged
    unsigned char hdr[] = {0x7E, 0x03, 0x00, 0x03};
    mockPush(hdr, sizeof(hdr));
    unsigned char fill[3000];
    memset(fill, 0x41, sizeof(fill));
    mockPush(fill, sizeof(fill));
    unsigned char end = 0x7E;
    mockPush(&end, 1);
    CHECK(receiveFrame(&f, nowMs() + 2000) == RX_OK && f.isInfo && !f.dataOk);
    CHECK(f.dataLen == MAX_DATA_FIELD);

    // exactly MAX_PAYLOAD_SIZE bytes is accepted
    unsigned char d[MAX_PAYLOAD_SIZE];
    for (int i = 0; i < MAX_PAYLOAD_SIZE; i++)
        d[i] = (unsigned char)i;
    pushInfo(0, d, MAX_PAYLOAD_SIZE, 0);
    CHECK(receiveFrame(&f, nowMs() + 2000) == RX_OK && f.dataOk && f.dataLen == MAX_PAYLOAD_SIZE + 1);
}

// Build -> stuff -> destuff round trip with payloads rich in special bytes.
static void test_roundtrip_fuzz(void)
{
    srand(12345);
    int bad = 0;
    for (int iter = 0; iter < 3000; iter++)
    {
        unsigned char d[MAX_PAYLOAD_SIZE];
        int n = rand() % (MAX_PAYLOAD_SIZE + 1);
        int density = rand() % 4;
        for (int i = 0; i < n; i++)
        {
            int r = rand() % 4;
            d[i] = (density == 0)   ? (unsigned char)rand()
                   : (r == 0)       ? 0x7E
                   : (r == 1)       ? 0x7D
                   : (r == 2 && density > 1) ? (rand() & 1 ? 0x5E : 0x5D)
                                    : (unsigned char)rand();
        }
        resetAll();
        pushInfo(iter & 1, d, n, 0);
        Frame f;
        if (receiveFrame(&f, nowMs() + 2000) != RX_OK || !f.dataOk || f.dataLen != n + 1 ||
            memcmp(f.data, d, n) != 0)
            bad++;
    }
    CHECK(bad == 0);
}

////////////////////////////////////////////////
// llOpenTx / llOpenRx
////////////////////////////////////////////////

static void test_open(void)
{
    // Tx: SET is sent, UA accepted
    resetAll();
    const unsigned char s[][4] = {{C_UA, 0, 0, 0}};
    setScript(A_TX, s, 1);
    CHECK(llOpenTx(params(1, 1)) == 0);
    unsigned char set[] = {0x7E, 0x03, 0x03, 0x00, 0x7E};
    CHECK(outIs(set, 5));
    resetAll();

    // Tx: UA with the wrong address is not accepted; SET is retransmitted, then error
    const unsigned char s2[][4] = {{C_UA, 0, 0, 0}, {C_UA, 0, 0, 0}};
    setScript(A_RX, s2, 2);
    long long t0 = nowMs();
    CHECK(llOpenTx(params(1, 1)) == -1);
    CHECK(mockOutLen == 10);               // 1 SET + 1 retransmission
    CHECK(nowMs() - t0 >= 1900);           // two full timeouts
    CHECK(!linkOpen && mockCloseCount > 0);

    // Tx: UA lost once -> SET retransmitted -> OK
    resetAll();
    const unsigned char s3[][4] = {{0, 0, 0, 0}, {C_UA, 0, 0, 0}};
    setScript(A_TX, s3, 2);
    CHECK(llOpenTx(params(1, 2)) == 0);
    CHECK(mockOutLen == 10);

    // Rx: garbage first, then SET -> UA(A=0x03)
    resetAll();
    unsigned char junk[] = {0x00, 0xFF, 0x7E, 0x13, 0x00};
    mockPush(junk, sizeof(junk));
    mockPushSup(A_TX, C_SET);
    CHECK(llOpenRx(params(1, 1)) == 0);
    unsigned char ua[] = {0x7E, 0x03, 0x07, 0x04, 0x7E};
    CHECK(outIs(ua, 5));

    // invalid parameters
    resetAll();
    CHECK(llOpenTx(params(0, 1)) == -1);
    CHECK(llOpenRx(params(1, -1)) == -1);
    LinkLayer p = params(1, 1);
    memset(p.serialPort, 'x', sizeof(p.serialPort)); // not NUL-terminated
    CHECK(llOpenTx(p) == -1);
    p = params(1, 1);
    p.serialPort[0] = '\0';
    CHECK(llOpenRx(p) == -1);
    resetAll();
}

////////////////////////////////////////////////
// llSend (Stop-and-Wait, transmitter)
////////////////////////////////////////////////

static void test_send_basic_and_modulo2(void)
{
    unsigned char d[] = {0x10, 0x7E, 0x20};
    openTx(1, 2);
    const unsigned char s[][4] = {{C_RR1, 0, 0, 0}, {C_RR0, 0, 0, 0}, {C_RR1, 0, 0, 0}};
    setScript(A_TX, s, 3);

    CHECK(llSend(d, 3) == 3);
    CHECK(txSeq == 1);
    unsigned char exp0[] = {0x7E, 0x03, 0x00, 0x03, 0x10, 0x7D, 0x5E, 0x20, 0x4E, 0x7E}; // bcc2 = 10^7E^20 = 4E
    CHECK(outIs(exp0, sizeof(exp0)));

    mockOutLen = 0;
    CHECK(llSend(d, 3) == 3);
    CHECK(txSeq == 0);
    CHECK(mockOut[2] == 0x80 && mockOut[3] == 0x83); // second frame is I1

    mockOutLen = 0;
    CHECK(llSend(d, 3) == 3);
    CHECK(mockOut[2] == 0x00); // back to I0
    CHECK(stats.iSent == 3 && stats.iRetx == 0 && stats.payloadBytes == 9);
    resetAll();
}

static void test_send_rej_retransmits_same_frame(void)
{
    unsigned char d[] = {1, 2, 3, 4};
    openTx(1, 3);
    const unsigned char s[][4] = {{C_REJ0, 0, 0, 0}, {C_REJ0, 0, 0, 0}, {C_RR1, 0, 0, 0}};
    setScript(A_TX, s, 3);
    long long t0 = nowMs();
    CHECK(llSend(d, 4) == 4);
    CHECK(nowMs() - t0 < 900); // REJ => immediate retransmission, no timeout wait
    unsigned char ref[MAX_FRAME_SIZE];
    int fl = buildInfoFrame(ref, 0, d, 4);
    CHECK(mockOutLen == 3 * fl); // the same frame, three times
    CHECK(memcmp(mockOut, mockOut + fl, fl) == 0 && memcmp(mockOut, mockOut + 2 * fl, fl) == 0);
    CHECK(txSeq == 1 && stats.iRetx == 2 && stats.rejRecv == 2);
    resetAll();
}

static void test_send_ignores_invalid_responses(void)
{
    unsigned char d[] = {9, 9};
    openTx(1, 1);
    // First answer carries: RR0 (stale, for I0 it is "RR(Ns)"), REJ1 (other seq) and a
    // supervision with the wrong address, then the real RR1. Only RR1 may confirm.
    mockOnWrite = NULL;
    mockPushSup(A_TX, C_RR0);
    mockPushSup(A_TX, C_REJ1);
    mockPushSup(A_RX, C_RR1);
    unsigned char corrupt[] = {0x7E, 0x03, 0xAB, 0x00, 0x7E}; // RR1 with wrong BCC1
    mockPush(corrupt, sizeof(corrupt));
    CHECK(llSend(d, 2) == -1); // no valid RR1 within the timeouts
    CHECK(txSeq == 0);         // sequence number did NOT advance
    CHECK(stats.iSent == 2 && stats.iRetx == 1);
    resetAll();

    openTx(1, 1);
    mockPushSup(A_TX, C_RR0);
    mockPushSup(A_TX, C_RR1);
    CHECK(llSend(d, 2) == 2 && txSeq == 1); // stale RR0 ignored, RR1 confirms
    resetAll();
}

static void test_send_timeout_then_ok(void)
{
    unsigned char d[] = {5};
    openTx(1, 2);
    const unsigned char s[][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {C_RR1, 0, 0, 0}};
    setScript(A_TX, s, 3);
    long long t0 = nowMs();
    CHECK(llSend(d, 1) == 1);
    long long dt = nowMs() - t0;
    CHECK(dt >= 1900 && dt < 2600); // two timeouts of 1 s
    CHECK(stats.timeouts == 2 && stats.iRetx == 2 && stats.iSent == 3);
    resetAll();
}

static void test_send_gives_up(void)
{
    unsigned char d[] = {5};
    openTx(1, 1);
    CHECK(llSend(d, 1) == -1);
    CHECK(stats.iSent == 2); // 1 + nRetransmissions
    CHECK(!linkOpen);
    CHECK(llSend(d, 1) == -1); // link closed: error, no crash
    resetAll();
}

static void test_send_partial_writes_and_limits(void)
{
    unsigned char d[] = {0x7E, 0x7D, 1, 2, 3, 4, 5};
    openTx(1, 1);
    const unsigned char s[][4] = {{C_RR1, 0, 0, 0}};
    setScript(A_TX, s, 1);
    mockWriteLimit = 3; // the "port" accepts 3 bytes per write
    CHECK(llSend(d, 7) == 7);
    unsigned char frame[MAX_FRAME_SIZE];
    int n = buildInfoFrame(frame, 0, d, 7);
    CHECK(outIs(frame, n));
    mockWriteLimit = 0;

    unsigned char big[MAX_PAYLOAD_SIZE + 1] = {0};
    CHECK(llSend(big, MAX_PAYLOAD_SIZE + 1) == -1);
    CHECK(llSend(NULL, 1) == -1);
    CHECK(llSend(d, -1) == -1);
    CHECK(linkOpen); // bad arguments do not kill the link
    resetAll();
}

////////////////////////////////////////////////
// llReceive (Stop-and-Wait, receiver)
////////////////////////////////////////////////

static void test_receive_in_order(void)
{
    unsigned char a[] = {1, 2, 3}, b[] = {0x7E, 0x7D}, out[MAX_PAYLOAD_SIZE];
    openRx(1, 1);
    pushInfo(0, a, 3, 0);
    CHECK(llReceive(out) == 3 && memcmp(out, a, 3) == 0);
    CHECK(outIsReply(C_RR1)); // after I0 the receiver asks for 1
    mockOutLen = 0;
    pushInfo(1, b, 2, 0);
    CHECK(llReceive(out) == 2 && memcmp(out, b, 2) == 0);
    CHECK(outIsReply(C_RR0));
    CHECK(rxExpected == 0 && stats.iDelivered == 2);
    resetAll();
}

static void test_receive_duplicate_not_delivered(void)
{
    unsigned char a[] = {1, 2, 3}, b[] = {4, 5}, out[MAX_PAYLOAD_SIZE];
    openRx(1, 1);
    pushInfo(0, a, 3, 0);
    CHECK(llReceive(out) == 3);
    mockOutLen = 0;

    // Tx never saw RR1 and resends I0 (twice), then goes on with I1
    pushInfo(0, a, 3, 0);
    pushInfo(0, a, 3, 0);
    pushInfo(1, b, 2, 0);
    int n = llReceive(out);
    CHECK(n == 2 && memcmp(out, b, 2) == 0); // the duplicates were NOT returned
    CHECK(stats.duplicates == 2 && stats.iDelivered == 2);
    // replies: RR1, RR1 (duplicates: RR of the frame still expected), then RR0
    unsigned char exp[15];
    unsigned char rr[3] = {C_RR1, C_RR1, C_RR0};
    for (int i = 0; i < 3; i++)
    {
        unsigned char f[5] = {FLAG, A_TX, rr[i], A_TX ^ rr[i], FLAG};
        memcpy(exp + 5 * i, f, 5);
    }
    CHECK(outIs(exp, 15));
    resetAll();
}

static void test_receive_bad_bcc2(void)
{
    unsigned char a[] = {1, 2, 3}, out[MAX_PAYLOAD_SIZE];
    openRx(1, 1);

    // new frame, BCC2 wrong -> REJ(expected), not delivered, state kept
    pushInfo(0, a, 3, 1);
    pushInfo(0, a, 3, 0); // retransmission is then accepted
    CHECK(llReceive(out) == 3 && memcmp(out, a, 3) == 0);
    unsigned char exp[10] = {FLAG, A_TX, C_REJ0, A_TX ^ C_REJ0, FLAG, FLAG, A_TX, C_RR1, A_TX ^ C_RR1, FLAG};
    CHECK(outIs(exp, 10));
    CHECK(stats.badBcc2 == 1 && stats.iDelivered == 1);

    // duplicate (I0 again) with wrong BCC2 -> RR(expected=1), nothing delivered
    mockOutLen = 0;
    unsigned char b[] = {7};
    pushInfo(0, a, 3, 1);
    pushInfo(1, b, 1, 0);
    CHECK(llReceive(out) == 1 && out[0] == 7);
    unsigned char exp2[10] = {FLAG, A_TX, C_RR1, A_TX ^ C_RR1, FLAG, FLAG, A_TX, C_RR0, A_TX ^ C_RR0, FLAG};
    CHECK(outIs(exp2, 10));
    resetAll();
}

static void test_receive_frame_errors_get_no_reply(void)
{
    unsigned char a[] = {1, 2, 3}, out[MAX_PAYLOAD_SIZE];
    openRx(1, 1);
    unsigned char frame[MAX_FRAME_SIZE];
    int n = buildInfoFrame(frame, 0, a, 3);
    frame[3] ^= 0x10; // corrupt BCC1
    mockPush(frame, n);
    pushInfo(0, a, 3, 0);
    CHECK(llReceive(out) == 3);
    CHECK(outIsReply(C_RR1)); // only ONE reply: the BCC1-corrupted frame was ignored silently
    CHECK(stats.badBcc1 == 1);
    resetAll();

    // escape errors in a new frame => REJ
    openRx(1, 1);
    unsigned char badesc[] = {0x7E, 0x03, 0x00, 0x03, 0x41, 0x7D, 0x00, 0x41, 0x7E};
    mockPush(badesc, sizeof(badesc));
    pushInfo(0, a, 3, 0);
    CHECK(llReceive(out) == 3);
    unsigned char exp[10] = {FLAG, A_TX, C_REJ0, A_TX ^ C_REJ0, FLAG, FLAG, A_TX, C_RR1, A_TX ^ C_RR1, FLAG};
    CHECK(outIs(exp, 10));
    resetAll();
}

static void test_receive_duplicate_set_and_disc(void)
{
    unsigned char a[] = {1}, out[MAX_PAYLOAD_SIZE];
    openRx(1, 1);
    mockPushSup(A_TX, C_SET); // our first UA was lost: Tx sends SET again
    pushInfo(0, a, 1, 0);
    CHECK(llReceive(out) == 1);
    unsigned char exp[10] = {FLAG, A_TX, C_UA, A_TX ^ C_UA, FLAG, FLAG, A_TX, C_RR1, A_TX ^ C_RR1, FLAG};
    CHECK(outIs(exp, 10)); // UA again, no state reset, then the normal RR1
    CHECK(rxExpected == 1);

    // DISC inside llReceive: error return; llCloseRx answers it
    mockOutLen = 0;
    mockPushSup(A_TX, C_DISC);
    mockPushSup(A_RX, C_UA);
    CHECK(llReceive(out) == -1 && linkOpen && discSeen);
    CHECK(llCloseRx() == 0);
    unsigned char disc[] = {FLAG, A_RX, C_DISC, A_RX ^ C_DISC, FLAG};
    CHECK(outIs(disc, 5));
    CHECK(!linkOpen);
    resetAll();
}

static void test_receive_idle_timeout(void)
{
    unsigned char out[MAX_PAYLOAD_SIZE];
    openRx(1, 1); // idle limit = (1 + 1) * 1 s
    long long t0 = nowMs();
    CHECK(llReceive(out) == -1);
    long long dt = nowMs() - t0;
    CHECK(dt >= 1900 && dt < 2700);
    CHECK(!linkOpen);
    resetAll();
}

////////////////////////////////////////////////
// llClose
////////////////////////////////////////////////

static void test_close_tx(void)
{
    openTx(1, 2);
    const unsigned char s[][4] = {{0, 0, 0, 0}, {C_DISC, 0, 0, 0}};
    setScript(A_RX, s, 2); // first DISC unanswered, second answered with DISC (A = 0x01)
    CHECK(llCloseTx() == 0);
    unsigned char exp[15] = {FLAG, A_TX, C_DISC, A_TX ^ C_DISC, FLAG,
                             FLAG, A_TX, C_DISC, A_TX ^ C_DISC, FLAG,
                             FLAG, A_RX, C_UA, A_RX ^ C_UA, FLAG};
    CHECK(outIs(exp, 15)); // DISC, DISC (retransmission), UA with A = 0x01
    CHECK(!linkOpen && mockCloseCount > 0);

    // peer never answers: error after 1 + nRetransmissions DISC frames
    openTx(1, 1);
    CHECK(llCloseTx() == -1);
    CHECK(mockOutLen == 10);
    resetAll();
}

static void test_close_rx(void)
{
    // lost RR for the last frame: a duplicate I arrives while waiting for DISC
    unsigned char a[] = {1}, out[MAX_PAYLOAD_SIZE];
    openRx(1, 2);
    pushInfo(0, a, 1, 0);
    CHECK(llReceive(out) == 1);
    mockOutLen = 0;
    pushInfo(0, a, 1, 0); // retransmitted END
    mockPushSup(A_TX, C_DISC);
    mockPushSup(A_RX, C_UA);
    CHECK(llCloseRx() == 0);
    unsigned char exp[10] = {FLAG, A_TX, C_RR1, A_TX ^ C_RR1, FLAG, FLAG, A_RX, C_DISC, A_RX ^ C_DISC, FLAG};
    CHECK(outIs(exp, 10)); // RR(expected) for the duplicate, then DISC
    resetAll();

    // final UA lost: DISC retransmitted, then closes anyway (documented limitation)
    openRx(1, 1);
    mockPushSup(A_TX, C_DISC);
    CHECK(llCloseRx() == 0);
    CHECK(mockOutLen == 10 && !linkOpen);
    resetAll();
}

////////////////////////////////////////////////
// application layer packets
////////////////////////////////////////////////

static void test_app_control_packets(void)
{
    unsigned long long sizes[] = {0, 1, 255, 256, 65535, 65536, 10968, 1ULL << 32, 1ULL << 40, ~0ULL};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        FileInfo in, outInfo;
        unsigned char p[MAX_PAYLOAD_SIZE];
        memset(&in, 0, sizeof(in));
        in.size = sizes[i];
        in.hasName = (i % 2 == 0);
        snprintf(in.name, sizeof(in.name), "penguin.gif");
        int n = buildControlPacket(p, C_START, &in);
        CHECK(parseControlPacket(p, n, C_START, &outInfo) == 0);
        CHECK(sameInfo(&in, &outInfo));
        CHECK(parseControlPacket(p, n, C_END, &outInfo) == -1); // wrong control field
        // every strict prefix must be rejected or at least never crash
        for (int cut = 0; cut < n; cut++)
        {
            int r = parseControlPacket(p, cut, C_START, &outInfo);
            if (cut < 3)
                CHECK(r == -1);
        }
    }

    // 255-char name
    FileInfo in, outInfo;
    unsigned char p[MAX_PAYLOAD_SIZE];
    memset(&in, 0, sizeof(in));
    in.size = 7;
    in.hasName = TRUE;
    memset(in.name, 'n', MAX_NAME_LEN);
    int n = buildControlPacket(p, C_END, &in);
    CHECK(n <= MAX_PAYLOAD_SIZE);
    CHECK(parseControlPacket(p, n, C_END, &outInfo) == 0 && strlen(outInfo.name) == MAX_NAME_LEN);

    // malformed: no size, size TLV with L=0 / L=9, value past the end, duplicate size
    unsigned char noSize[] = {C_START, T_FILE_NAME, 1, 'a'};
    CHECK(parseControlPacket(noSize, sizeof(noSize), C_START, &outInfo) == -1);
    unsigned char l0[] = {C_START, T_FILE_SIZE, 0};
    CHECK(parseControlPacket(l0, sizeof(l0), C_START, &outInfo) == -1);
    unsigned char l9[] = {C_START, T_FILE_SIZE, 9, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK(parseControlPacket(l9, sizeof(l9), C_START, &outInfo) == -1);
    unsigned char over[] = {C_START, T_FILE_SIZE, 4, 1, 2};
    CHECK(parseControlPacket(over, sizeof(over), C_START, &outInfo) == -1);
    unsigned char dup[] = {C_START, T_FILE_SIZE, 1, 5, T_FILE_SIZE, 1, 6};
    CHECK(parseControlPacket(dup, sizeof(dup), C_START, &outInfo) == -1);
    CHECK(parseControlPacket(p, 0, C_START, &outInfo) == -1);
    unsigned char unknown[] = {C_START, 0x7F, 2, 1, 1, T_FILE_SIZE, 1, 9}; // unknown TLV is skipped
    CHECK(parseControlPacket(unknown, sizeof(unknown), C_START, &outInfo) == 0 && outInfo.size == 9);

    // different metadata is detected
    FileInfo a, b;
    memset(&a, 0, sizeof(a));
    b = a;
    a.size = 1;
    b.size = 2;
    CHECK(!sameInfo(&a, &b));
    b.size = 1;
    b.hasName = TRUE;
    CHECK(!sameInfo(&a, &b));
}

static void test_app_data_packets(void)
{
    unsigned char data[APP_CHUNK_SIZE], p[MAX_PAYLOAD_SIZE];
    for (int i = 0; i < APP_CHUNK_SIZE; i++)
        data[i] = (unsigned char)(i * 7);
    int n = buildDataPacket(p, data, APP_CHUNK_SIZE);
    CHECK(n == APP_CHUNK_SIZE + 3 && n <= MAX_PAYLOAD_SIZE);
    CHECK(p[0] == C_DATA && 256 * p[1] + p[2] == APP_CHUNK_SIZE);

    const unsigned char *d;
    int k;
    CHECK(parseDataPacket(p, n, &d, &k) == 0 && k == APP_CHUNK_SIZE && memcmp(d, data, k) == 0);
    CHECK(parseDataPacket(p, n - 1, &d, &k) == -1); // shorter than declared
    CHECK(parseDataPacket(p, n + 1, &d, &k) == -1); // longer than declared
    CHECK(parseDataPacket(p, 2, &d, &k) == -1);
    p[0] = C_END;
    CHECK(parseDataPacket(p, n, &d, &k) == -1);

    // 0x00 bytes and L2/L1 boundaries (K = 256 * L2 + L1)
    unsigned char zeros[300] = {0};
    n = buildDataPacket(p, zeros, 300);
    CHECK(p[1] == 1 && p[2] == 44 && parseDataPacket(p, n, &d, &k) == 0 && k == 300);
    n = buildDataPacket(p, zeros, 0);
    CHECK(parseDataPacket(p, n, &d, &k) == 0 && k == 0);
}

int main(void)
{
    RUN(test_stuffing);
    RUN(test_bcc2);
    RUN(test_build_info_frame);
    RUN(test_receive_supervision);
    RUN(test_receive_bad_bcc1_and_resync);
    RUN(test_receive_timeout_not_extended_by_garbage);
    RUN(test_receive_info);
    RUN(test_receive_oversized);
    RUN(test_roundtrip_fuzz);
    RUN(test_open);
    RUN(test_send_basic_and_modulo2);
    RUN(test_send_rej_retransmits_same_frame);
    RUN(test_send_ignores_invalid_responses);
    RUN(test_send_timeout_then_ok);
    RUN(test_send_gives_up);
    RUN(test_send_partial_writes_and_limits);
    RUN(test_receive_in_order);
    RUN(test_receive_duplicate_not_delivered);
    RUN(test_receive_bad_bcc2);
    RUN(test_receive_frame_errors_get_no_reply);
    RUN(test_receive_duplicate_set_and_disc);
    RUN(test_receive_idle_timeout);
    RUN(test_close_tx);
    RUN(test_close_rx);
    RUN(test_app_control_packets);
    RUN(test_app_data_packets);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
