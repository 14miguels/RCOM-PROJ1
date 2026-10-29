// RCOM 2026/2027
//
// Link layer protocol implementation
//
// Stop-and-Wait ARQ (sequence numbers modulo 2) with byte stuffing.
// Layout of the file:
//   1. constants, state and statistics
//   2. time / ticker helpers (timeouts)
//   3. frame writing (stuffing, BCC)
//   4. frame reading (the single receive state machine)
//   5. llOpenTx / llOpenRx
//   6. llSend / llReceive
//   7. llCloseTx / llCloseRx

#include "link_layer.h"
#include "serial_port.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

////////////////////////////////////////////////
// CONSTANTS, STATE AND STATISTICS
////////////////////////////////////////////////

#define FLAG 0x7E
#define ESC 0x7D
#define ESC_XOR 0x20 // stuffed byte = original ^ 0x20

// Address field: commands sent by Tx and replies sent by Rx use A_TX;
// commands sent by Rx and replies sent by Tx use A_RX.
#define A_TX 0x03
#define A_RX 0x01

#define C_SET 0x03
#define C_UA 0x07
#define C_DISC 0x0B
#define C_RR0 0xAA
#define C_RR1 0xAB
#define C_REJ0 0x54
#define C_REJ1 0x55
#define C_I0 0x00
#define C_I1 0x80

// Data field of an I frame after destuffing: payload + BCC2.
#define MAX_DATA_FIELD (MAX_PAYLOAD_SIZE + 1)
// Worst case on the wire: FLAG A C BCC1 + every data byte stuffed + FLAG.
#define MAX_FRAME_SIZE (2 * MAX_DATA_FIELD + 5)

#define TICK_USEC 100000 // ticker period (see "TIME" below)
#define NO_DEADLINE (-1LL)

typedef struct
{
    long iSent;        // I frames written to the line (including retransmissions)
    long iRetx;        // I frames retransmitted (timeout or REJ)
    long timeouts;     // timer expirations (SET, I and DISC frames)
    long rrSent, rrRecv;
    long rejSent, rejRecv;
    long iRecv;        // I frames received with valid BCC1 (new + duplicates + bad)
    long iDelivered;   // new valid I frames handed to the application
    long duplicates;   // I frames received again (already delivered)
    long badBcc1;      // frames discarded because of a wrong BCC1
    long badBcc2;      // I frames with corrupted data field (BCC2, escape, size)
    long payloadBytes; // payload bytes sent (acknowledged) / delivered, no overhead
} Stats;

static Stats stats;
static struct timespec startTime;

static int linkOpen = FALSE;  // serial port open and link usable
static int isTx = FALSE;
static int baudRate = 0;
static int timeoutSec = 0;
static int maxRetx = 0;       // retransmissions after the first attempt
static int txSeq = 0;         // Ns of the next I frame to send (Tx)
static int rxExpected = 0;    // Ns of the next new I frame expected (Rx)
static int discSeen = FALSE;  // Rx: a DISC arrived while inside llReceive
static int statsPending = FALSE; // statistics not printed yet

////////////////////////////////////////////////
// TIME
////////////////////////////////////////////////
//
// The serial port blocks in read() until a byte arrives. To implement
// timeouts without busy waiting, a periodic SIGALRM "tick" (every 100 ms)
// interrupts the blocked read() with EINTR; the loops then compare the
// monotonic clock with an absolute deadline. Bytes that arrive do NOT move
// the deadline, so garbage on the line cannot postpone a timeout. Because
// the tick is periodic, a signal that fires just before read() blocks only
// delays the timeout by at most one tick (no lost wake-up).

static void tickHandler(int sig)
{
    (void)sig; // only purpose: make read()/write() return EINTR
}

static struct sigaction oldSigaction;

static int startTicker(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tickHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: read() must fail with EINTR
    if (sigaction(SIGALRM, &sa, &oldSigaction) == -1)
        return -1;

    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = TICK_USEC;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_REAL, &it, NULL) == -1)
    {
        sigaction(SIGALRM, &oldSigaction, NULL);
        return -1;
    }
    return 0;
}

static void stopTicker(void)
{
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);
    sigaction(SIGALRM, &oldSigaction, NULL);
}

static long long nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleepMs(int ms)
{
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

////////////////////////////////////////////////
// FRAME WRITING
////////////////////////////////////////////////

// write() may write less than requested or be interrupted by the ticker.
static int writeAll(const unsigned char *buf, int n)
{
    int done = 0;
    while (done < n)
    {
        int w = writeBytesSerialPort(buf + done, n - done);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (w == 0)
            sleepMs(1); // avoid spinning if the driver accepts nothing
        done += w;
    }
    return 0;
}

// Supervision / unnumbered frame: FLAG | A | C | BCC1 | FLAG
static int sendSupervision(unsigned char a, unsigned char c)
{
    unsigned char frame[5] = {FLAG, a, c, a ^ c, FLAG};

    if (c == C_RR0 || c == C_RR1)
        stats.rrSent++;
    else if (c == C_REJ0 || c == C_REJ1)
        stats.rejSent++;

    return writeAll(frame, sizeof(frame));
}

// Appends b to out, escaping FLAG and ESC (0x7E -> 0x7D 0x5E, 0x7D -> 0x7D 0x5D).
static int stuffByte(unsigned char *out, int idx, unsigned char b)
{
    if (b == FLAG || b == ESC)
    {
        out[idx++] = ESC;
        out[idx++] = b ^ ESC_XOR;
    }
    else
        out[idx++] = b;
    return idx;
}

static unsigned char computeBcc2(const unsigned char *data, int n)
{
    unsigned char bcc = 0;
    for (int i = 0; i < n; i++)
        bcc ^= data[i];
    return bcc;
}

// I frame: FLAG | A | C(Ns) | BCC1 | stuffed(DATA | BCC2) | FLAG
// BCC2 is computed over the original (unstuffed) payload; the delimiting
// FLAGs and the header are never stuffed. Returns the frame length.
// out must hold MAX_FRAME_SIZE bytes and size <= MAX_PAYLOAD_SIZE.
static int buildInfoFrame(unsigned char *out, int ns, const unsigned char *data, int size)
{
    unsigned char c = ns ? C_I1 : C_I0;
    int idx = 0;

    out[idx++] = FLAG;
    out[idx++] = A_TX;
    out[idx++] = c;
    out[idx++] = A_TX ^ c;
    for (int i = 0; i < size; i++)
        idx = stuffByte(out, idx, data[i]);
    idx = stuffByte(out, idx, computeBcc2(data, size));
    out[idx++] = FLAG;
    return idx;
}

////////////////////////////////////////////////
// FRAME READING (receive state machine)
////////////////////////////////////////////////

typedef struct
{
    unsigned char a, c;
    int isInfo;                         // c is I0 or I1
    unsigned char data[MAX_DATA_FIELD]; // I frames: destuffed payload followed by BCC2
    int dataLen;                        // bytes in data (payload + BCC2)
    int dataOk;                         // I frames: stuffing, size and BCC2 all valid
} Frame;

enum { RX_ERROR = -1, RX_TIMEOUT = 0, RX_OK = 1 };

typedef enum
{
    ST_START,  // waiting for a FLAG
    ST_FLAG,   // got FLAG, waiting for A
    ST_A,      // got A, waiting for C
    ST_C,      // got C, waiting for BCC1
    ST_BCC_OK, // supervision frame header ok, waiting for closing FLAG
    ST_DATA    // I frame header ok, collecting data until closing FLAG
} RxState;

// The closing FLAG of one frame may also be the opening FLAG of the next, so
// after a frame we restart in ST_FLAG instead of ST_START.
static int resync = FALSE;

static int isKnownControl(unsigned char c)
{
    return c == C_SET || c == C_UA || c == C_DISC || c == C_RR0 || c == C_RR1 ||
           c == C_REJ0 || c == C_REJ1 || c == C_I0 || c == C_I1;
}

// Reads one frame (supervision or information) from the serial port.
//   RX_OK      a frame with valid A, C and BCC1 is in *f
//   RX_TIMEOUT absolute deadline (monotonic ms) reached; NO_DEADLINE waits forever
//   RX_ERROR   read error
// Frames with a wrong BCC1 or unknown A/C are silently ignored (counted in
// stats.badBcc1) and the machine resynchronises on the next FLAG.
// For I frames, the data field is destuffed on the fly: an invalid escape
// sequence, an incomplete escape (ESC FLAG), an oversized frame or a wrong
// BCC2 only clear f->dataOk; the caller decides between REJ and RR.
static int receiveFrame(Frame *f, long long deadline)
{
    RxState state = resync ? ST_FLAG : ST_START;
    int escaped = FALSE;
    int bad = FALSE;

    f->dataLen = 0;
    f->dataOk = FALSE;
    f->isInfo = FALSE;

    while (TRUE)
    {
        if (deadline != NO_DEADLINE && nowMs() >= deadline)
            return RX_TIMEOUT;

        unsigned char b;
        int n = readByteSerialPort(&b);
        if (n < 0)
        {
            if (errno == EINTR)
                continue; // ticker: re-check the deadline
            return RX_ERROR;
        }
        if (n == 0)
        {
            sleepMs(1); // no byte: do not spin
            continue;
        }

        switch (state)
        {
        case ST_START:
            if (b == FLAG)
                state = ST_FLAG;
            break;

        case ST_FLAG:
            if (b == FLAG)
                break; // several FLAGs in a row
            if (b == A_TX || b == A_RX)
            {
                f->a = b;
                state = ST_A;
            }
            else
                state = ST_START;
            break;

        case ST_A:
            if (b == FLAG)
                state = ST_FLAG;
            else if (isKnownControl(b))
            {
                f->c = b;
                state = ST_C;
            }
            else
                state = ST_START;
            break;

        case ST_C:
            if (b == (unsigned char)(f->a ^ f->c))
            {
                f->isInfo = (f->c == C_I0 || f->c == C_I1);
                f->dataLen = 0;
                escaped = FALSE;
                bad = FALSE;
                state = f->isInfo ? ST_DATA : ST_BCC_OK;
            }
            else
            {
                stats.badBcc1++;
                state = (b == FLAG) ? ST_FLAG : ST_START;
            }
            break;

        case ST_BCC_OK:
            if (b == FLAG)
            {
                resync = TRUE;
                return RX_OK;
            }
            state = ST_START;
            break;

        case ST_DATA:
            if (b == FLAG)
            {
                // ESC right before the FLAG = incomplete escape sequence
                if (escaped)
                    bad = TRUE;
                // data = payload + BCC2: at least the BCC2 byte must exist
                f->dataOk = !bad && f->dataLen >= 1 &&
                            computeBcc2(f->data, f->dataLen - 1) == f->data[f->dataLen - 1];
                resync = TRUE;
                return RX_OK;
            }
            if (escaped)
            {
                escaped = FALSE;
                if (b == (FLAG ^ ESC_XOR))
                    b = FLAG;
                else if (b == (ESC ^ ESC_XOR))
                    b = ESC;
                else
                {
                    bad = TRUE; // invalid escape sequence
                    break;
                }
            }
            else if (b == ESC)
            {
                escaped = TRUE;
                break;
            }
            if (f->dataLen < MAX_DATA_FIELD)
                f->data[f->dataLen++] = b;
            else
                bad = TRUE; // oversized frame: never write past the buffer
            break;
        }
    }
}

// Waits (until deadline) for a supervision frame with this A and C, ignoring
// every other frame. Returns 1 if found, 0 on timeout, -1 on error.
static int waitSupervision(unsigned char a, unsigned char c, long long deadline)
{
    Frame f;
    int r;
    while ((r = receiveFrame(&f, deadline)) == RX_OK)
        if (!f.isInfo && f.a == a && f.c == c)
            return 1;
    return (r == RX_TIMEOUT) ? 0 : -1;
}

////////////////////////////////////////////////
// LINK MANAGEMENT AND STATISTICS
////////////////////////////////////////////////

static void printStats(void)
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double secs = (end.tv_sec - startTime.tv_sec) + (end.tv_nsec - startTime.tv_nsec) / 1e9;
    double bps = secs > 0 ? stats.payloadBytes * 8.0 / secs : 0.0;

    printf("\n===== Link layer statistics (%s) =====\n", isTx ? "Tx" : "Rx");
    if (isTx)
    {
        printf("I frames sent (total)    : %ld\n", stats.iSent);
        printf("  retransmissions        : %ld\n", stats.iRetx);
        printf("Timeouts                 : %ld\n", stats.timeouts);
        printf("RR received              : %ld\n", stats.rrRecv);
        printf("REJ received             : %ld\n", stats.rejRecv);
        printf("Payload bytes acked      : %ld\n", stats.payloadBytes);
    }
    else
    {
        printf("I frames received (BCC1 ok): %ld\n", stats.iRecv);
        printf("  delivered to application : %ld\n", stats.iDelivered);
        printf("  duplicates (not delivered): %ld\n", stats.duplicates);
        printf("  with data errors (BCC2)  : %ld\n", stats.badBcc2);
        printf("Frames dropped (BCC1)      : %ld\n", stats.badBcc1);
        printf("RR sent                    : %ld\n", stats.rrSent);
        printf("REJ sent                   : %ld\n", stats.rejSent);
        printf("Timeouts                   : %ld\n", stats.timeouts);
        printf("Payload bytes delivered    : %ld\n", stats.payloadBytes);
    }
    printf("Elapsed time             : %.2f s\n", secs);
    printf("Useful throughput R      : %.1f bit/s\n", bps);
    if (baudRate > 0)
        printf("Efficiency S = R/C       : %.3f (C = %d bit/s)\n", bps / baudRate, baudRate);
    printf("======================================\n");
    fflush(stdout);
    statsPending = FALSE;
}

// Common part of llOpenTx / llOpenRx.
static int linkStart(LinkLayer p, int tx)
{
    if (p.timeout < 1 || p.nRetransmissions < 0 || p.serialPort[0] == '\0' ||
        memchr(p.serialPort, '\0', sizeof(p.serialPort)) == NULL)
    {
        fprintf(stderr, "link layer: invalid parameters\n");
        return -1;
    }
    if (linkOpen)
    {
        fprintf(stderr, "link layer: already open\n");
        return -1;
    }

    if (openSerialPort(p.serialPort, p.baudRate) < 0)
        return -1;
    if (startTicker() < 0)
    {
        perror("startTicker");
        closeSerialPort();
        return -1;
    }

    memset(&stats, 0, sizeof(stats));
    clock_gettime(CLOCK_MONOTONIC, &startTime);
    isTx = tx;
    baudRate = p.baudRate;
    timeoutSec = p.timeout;
    maxRetx = p.nRetransmissions;
    txSeq = 0;
    rxExpected = 0;
    discSeen = FALSE;
    resync = FALSE;
    statsPending = TRUE;
    linkOpen = TRUE;
    return 0;
}

// Stops the ticker and closes the serial port. Safe to call twice.
static int linkStop(void)
{
    if (!linkOpen)
        return -1;
    linkOpen = FALSE;
    stopTicker();
    return closeSerialPort();
}

////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////

// Tx: send SET, wait for UA; on timeout send SET again (up to maxRetx times).
int llOpenTx(LinkLayer llParameters)
{
    if (linkStart(llParameters, TRUE) < 0)
        return -1;

    for (int attempt = 0; attempt <= maxRetx; attempt++)
    {
        if (sendSupervision(A_TX, C_SET) < 0)
        {
            linkStop();
            return -1;
        }

        int r = waitSupervision(A_TX, C_UA, nowMs() + timeoutSec * 1000LL);
        if (r == 1)
            return 0;
        if (r < 0)
            break;
        stats.timeouts++;
        fprintf(stderr, "llOpenTx: no UA (attempt %d/%d)\n", attempt + 1, maxRetx + 1);
    }

    fprintf(stderr, "llOpenTx: could not establish the connection\n");
    linkStop();
    return -1;
}

// Rx: wait (forever) for SET, answer UA. If the UA is lost, Tx sends SET
// again and llReceive() answers it with another UA.
int llOpenRx(LinkLayer llParameters)
{
    if (linkStart(llParameters, FALSE) < 0)
        return -1;

    if (waitSupervision(A_TX, C_SET, NO_DEADLINE) != 1 ||
        sendSupervision(A_TX, C_UA) < 0)
    {
        linkStop();
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &startTime); // do not count the wait for Tx
    return 0;
}

////////////////////////////////////////////////
// LLSEND (Stop-and-Wait, transmitter)
////////////////////////////////////////////////

// Sends one I frame carrying buf and waits for its acknowledgement.
//   I(Ns) -> expects RR(Ns^1). The sequence number only toggles after that RR.
//   REJ(Ns)        -> retransmit immediately.
//   timeout        -> retransmit.
//   anything else (RR(Ns) stale, corrupted, other REJ) is ignored while the
//   same timer keeps running.
// Total transmissions = 1 + nRetransmissions (REJ and timeouts both count).
// Returns bufSize on success, -1 on error (the link is then closed).
int llSend(const unsigned char *buf, int bufSize)
{
    if (!linkOpen || !isTx || buf == NULL || bufSize < 0 || bufSize > MAX_PAYLOAD_SIZE)
        return -1;

    unsigned char frame[MAX_FRAME_SIZE];
    int frameLen = buildInfoFrame(frame, txSeq, buf, bufSize);
    unsigned char rrNext = txSeq ? C_RR0 : C_RR1; // RR(Ns+1): "send me the next one"
    unsigned char rejThis = txSeq ? C_REJ1 : C_REJ0;

    for (int attempt = 0; attempt <= maxRetx; attempt++)
    {
        if (attempt > 0)
            stats.iRetx++;
        if (writeAll(frame, frameLen) < 0)
        {
            perror("llSend: write");
            linkStop();
            return -1;
        }
        stats.iSent++;

        long long deadline = nowMs() + timeoutSec * 1000LL;
        int retransmit = FALSE;
        while (!retransmit)
        {
            Frame f;
            int r = receiveFrame(&f, deadline);
            if (r == RX_ERROR)
            {
                perror("llSend: read");
                linkStop();
                return -1;
            }
            if (r == RX_TIMEOUT)
            {
                stats.timeouts++;
                break;
            }
            if (f.isInfo || f.a != A_TX)
                continue; // not a reply from Rx

            if (f.c == rrNext)
            {
                stats.rrRecv++;
                stats.payloadBytes += bufSize;
                txSeq ^= 1; // only now is the frame confirmed
                return bufSize;
            }
            if (f.c == rejThis)
            {
                stats.rejRecv++;
                retransmit = TRUE;
            }
        }
    }

    fprintf(stderr, "llSend: no acknowledgement after %d transmissions\n", maxRetx + 1);
    linkStop();
    return -1;
}

////////////////////////////////////////////////
// LLRECEIVE (Stop-and-Wait, receiver)
////////////////////////////////////////////////

enum { RXA_NONE, RXA_DELIVER, RXA_DISC };

// Reaction of Rx to one frame (shared by llReceive and llCloseRx).
//   new I(Ns==expected), BCC2 ok  -> RR(expected+1), toggle expected, deliver
//   new I(Ns==expected), BCC2 bad -> REJ(expected), discard
//   duplicate I (Ns!=expected)    -> RR(expected), discard (even if BCC2 bad)
//   SET                           -> UA again (our first UA may have been lost)
//   DISC                          -> RXA_DISC
// payload == NULL means "do not accept new frames" (used while closing).
static int rxProcess(const Frame *f, unsigned char *payload, int *len)
{
    if (f->a != A_TX)
        return RXA_NONE;

    if (!f->isInfo)
    {
        if (f->c == C_SET)
            sendSupervision(A_TX, C_UA);
        else if (f->c == C_DISC)
            return RXA_DISC;
        return RXA_NONE;
    }

    stats.iRecv++;
    int ns = (f->c == C_I1);
    unsigned char rrExpected = rxExpected ? C_RR1 : C_RR0;

    if (ns != rxExpected)
    {
        stats.duplicates++;
        sendSupervision(A_TX, rrExpected);
        return RXA_NONE;
    }
    if (payload == NULL)
        return RXA_NONE;
    if (!f->dataOk)
    {
        stats.badBcc2++;
        sendSupervision(A_TX, rxExpected ? C_REJ1 : C_REJ0);
        return RXA_NONE;
    }

    int n = f->dataLen - 1; // drop BCC2
    memcpy(payload, f->data, n);
    rxExpected ^= 1;
    sendSupervision(A_TX, rxExpected ? C_RR1 : C_RR0); // RR(next expected)
    stats.iDelivered++;
    stats.payloadBytes += n;
    *len = n;
    return RXA_DELIVER;
}

// Receives the payload of the next new I frame into packet (which must hold
// MAX_PAYLOAD_SIZE bytes). Duplicates are acknowledged but never returned.
// Returns the payload size, or -1 on error: read failure, silence for longer
// than (nRetransmissions + 1) * timeout (a live Tx would have retransmitted
// by then), or a DISC from Tx.
int llReceive(unsigned char *packet)
{
    if (!linkOpen || isTx || packet == NULL)
        return -1;

    long long idleMs = (maxRetx + 1) * timeoutSec * 1000LL;
    long long deadline = nowMs() + idleMs;

    while (TRUE)
    {
        Frame f;
        int r = receiveFrame(&f, deadline);
        if (r == RX_ERROR)
        {
            perror("llReceive: read");
            linkStop();
            return -1;
        }
        if (r == RX_TIMEOUT)
        {
            stats.timeouts++;
            fprintf(stderr, "llReceive: nothing received for %lld s, giving up\n", idleMs / 1000);
            linkStop();
            return -1;
        }
        deadline = nowMs() + idleMs; // a valid frame arrived: Tx is alive

        int len = 0;
        int action = rxProcess(&f, packet, &len);
        if (action == RXA_DELIVER)
            return len;
        if (action == RXA_DISC)
        {
            discSeen = TRUE; // llCloseRx() will answer it
            return -1;
        }
    }
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
//
//   Tx                Rx
//   DISC  --------->
//         <---------  DISC
//   UA    --------->
//
// Limitation of the 3-way handshake: nobody acknowledges the final UA. If it
// is lost, Rx cannot know; it retransmits DISC and, if Tx has already closed,
// gives up after maxRetx retransmissions and closes anyway.

int llCloseTx()
{
    if (!linkOpen || !isTx)
    {
        if (statsPending && isTx)
            printStats(); // link already closed by a fatal error: still report
        return -1;
    }

    int ok = FALSE;
    for (int attempt = 0; attempt <= maxRetx && !ok; attempt++)
    {
        if (sendSupervision(A_TX, C_DISC) < 0)
            break;
        int r = waitSupervision(A_RX, C_DISC, nowMs() + timeoutSec * 1000LL);
        if (r < 0)
            break;
        if (r == 0)
        {
            stats.timeouts++;
            fprintf(stderr, "llCloseTx: no DISC (attempt %d/%d)\n", attempt + 1, maxRetx + 1);
        }
        ok = (r == 1);
    }

    if (ok && sendSupervision(A_RX, C_UA) < 0)
        ok = FALSE;

    printStats();
    linkStop();
    return ok ? 0 : -1;
}

int llCloseRx()
{
    if (!linkOpen || isTx)
    {
        if (statsPending && !isTx)
            printStats(); // link already closed by a fatal error: still report
        return -1;
    }

    // 1. Wait for DISC (it may already have been seen inside llReceive).
    //    While waiting, keep answering duplicates of the last I frame (e.g. the
    //    RR for END was lost) and repeated SETs, but accept nothing new.
    if (!discSeen)
    {
        long long idleMs = (maxRetx + 1) * timeoutSec * 1000LL;
        long long deadline = nowMs() + idleMs;
        while (!discSeen)
        {
            Frame f;
            int r = receiveFrame(&f, deadline);
            if (r != RX_OK)
            {
                if (r == RX_TIMEOUT)
                    fprintf(stderr, "llCloseRx: no DISC received\n");
                printStats();
                linkStop();
                return -1;
            }
            discSeen = (rxProcess(&f, NULL, NULL) == RXA_DISC);
        }
    }

    // 2. Answer with our DISC and wait for the final UA (retransmitting DISC).
    int gotUa = FALSE;
    for (int attempt = 0; attempt <= maxRetx && !gotUa; attempt++)
    {
        if (sendSupervision(A_RX, C_DISC) < 0)
            break;
        int r = waitSupervision(A_RX, C_UA, nowMs() + timeoutSec * 1000LL);
        if (r < 0)
            break;
        if (r == 0)
        {
            stats.timeouts++;
            fprintf(stderr, "llCloseRx: no UA (attempt %d/%d)\n", attempt + 1, maxRetx + 1);
        }
        gotUa = (r == 1);
    }
    if (!gotUa)
        fprintf(stderr, "llCloseRx: final UA not received (lost, or Tx already closed); closing anyway\n");

    printStats();
    // The data transfer was already completed and acknowledged, so a missing
    // final UA does not make the closing fail.
    return linkStop() < 0 ? -1 : 0;
}
