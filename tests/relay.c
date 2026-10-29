// RCOM 2026/2027 - test tool (not part of the delivered protocol)
//
// Virtual cable with DETERMINISTIC fault injection, for the integration tests.
// Creates two pseudoterminals and forwards bytes between them, frame by frame:
//
//   relay <link-for-tx> <link-for-rx> [rule ...]
//
// The Tx program opens <link-for-tx>, the Rx program opens <link-for-rx>
// (both are symlinks to the pty slaves). A rule has the form
//
//   <dir>:<type>:<nth>:<action>[:<arg>]
//
//   dir    tx2rx | rx2tx
//   type   SET UA DISC RR REJ I        (frame type, taken from the C field)
//   nth    N      the N-th frame of that dir/type (1-based)
//          N-M    frames N..M        N-   frames N and following      *   all
//   action drop        frame never arrives
//          bcc1        flip the BCC1 (frame must be ignored by the receiver)
//          data        flip a bit in the first data byte (BCC2 must fail)
//          trunc       send only the first half, without the closing FLAG
//          escend      make the last data byte an ESC (incomplete escape)
//          badesc      insert an invalid escape sequence (7D 00) in the data
//          dup         deliver the frame twice
//          garbage     send junk bytes (no 7E) before the frame
//          delay:<ms>  deliver the frame <ms> milliseconds late
//
// Random mode (stress tests): environment variable RELAY_CHAOS="seed:drop:corrupt:dup"
// (probabilities in percent, applied to every frame in both directions; "corrupt"
// XORs a random non-zero value into a random byte of the frame, flags included).
//
// Every injected fault is logged on stderr ("[relay] ...") so tests can check it.
// Stops on SIGTERM/SIGINT.

#define _GNU_SOURCE // glibc: posix_openpt, ptsname, cfmakeraw (harmless on macOS)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FLAG 0x7E
#define MAX_RULES 64
#define MAX_FRAME 8192
#define MAX_PENDING 64

enum { DIR_TX2RX = 0, DIR_RX2TX = 1 };
enum { T_SET, T_UA, T_DISC, T_RR, T_REJ, T_I, T_OTHER, N_TYPES };
static const char *typeNames[] = {"SET", "UA", "DISC", "RR", "REJ", "I"};

typedef struct
{
    int dir, type;
    int from, to; // inclusive; to = -1 means unbounded
    char action[16];
    int arg;
} Rule;

typedef struct
{
    long long due; // ms
    int fd;
    int len;
    unsigned char bytes[MAX_FRAME];
} Pending;

static Rule rules[MAX_RULES];
static int nRules = 0;
static Pending pending[MAX_PENDING];
static int nPending = 0;
static int counters[2][N_TYPES];
static volatile sig_atomic_t stop = 0;
static unsigned long long rng;
static int chaosDrop, chaosCorrupt, chaosDup, chaosOn;

static unsigned rnd(void) // xorshift64*, deterministic for a given seed
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (unsigned)((rng * 2685821657736338717ULL) >> 33);
}

static void onSignal(int s) { (void)s; stop = 1; }

static long long nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int parseRule(const char *s, Rule *r)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    char *dir = strtok(buf, ":"), *type = strtok(NULL, ":"), *nth = strtok(NULL, ":");
    char *action = strtok(NULL, ":"), *arg = strtok(NULL, ":");
    if (!dir || !type || !nth || !action)
        return -1;

    r->dir = strcmp(dir, "tx2rx") == 0 ? DIR_TX2RX : DIR_RX2TX;
    if (strcmp(dir, "tx2rx") != 0 && strcmp(dir, "rx2tx") != 0)
        return -1;
    r->type = -1;
    for (int i = 0; i < 6; i++)
        if (strcmp(type, typeNames[i]) == 0)
            r->type = i;
    if (r->type < 0)
        return -1;

    if (strcmp(nth, "*") == 0)
    {
        r->from = 1;
        r->to = -1;
    }
    else
    {
        char *dash = strchr(nth, '-');
        r->from = atoi(nth);
        r->to = dash == NULL ? r->from : (dash[1] ? atoi(dash + 1) : -1);
    }
    snprintf(r->action, sizeof(r->action), "%s", action);
    r->arg = arg ? atoi(arg) : 0;
    return 0;
}

static int classify(const unsigned char *f, int len)
{
    if (len < 5)
        return T_OTHER;
    switch (f[2])
    {
    case 0x03: return T_SET;
    case 0x07: return T_UA;
    case 0x0B: return T_DISC;
    case 0xAA: case 0xAB: return T_RR;
    case 0x54: case 0x55: return T_REJ;
    case 0x00: case 0x80: return T_I;
    }
    return T_OTHER;
}

// Writes to a master without ever blocking forever (a dead peer = lost bytes,
// like a real cable).
static void writeOut(int fd, const unsigned char *b, int n)
{
    int done = 0;
    while (done < n)
    {
        int w = write(fd, b + done, n - done);
        if (w > 0)
            done += w;
        else if (w < 0 && (errno == EAGAIN || errno == EINTR))
        {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 200) <= 0)
                return;
        }
        else
            return;
    }
}

static void deliverLater(int fd, const unsigned char *b, int n, int ms)
{
    if (nPending >= MAX_PENDING || n > MAX_FRAME)
        return;
    pending[nPending].due = nowMs() + ms;
    pending[nPending].fd = fd;
    pending[nPending].len = n;
    memcpy(pending[nPending].bytes, b, n);
    nPending++;
}

static void flushPending(void)
{
    long long now = nowMs();
    for (int i = 0; i < nPending;)
    {
        if (pending[i].due <= now)
        {
            writeOut(pending[i].fd, pending[i].bytes, pending[i].len);
            pending[i] = pending[--nPending];
        }
        else
            i++;
    }
}

// Handles one complete frame (FLAG ... FLAG) travelling in direction dir.
static void forwardFrame(int dir, int outFd, unsigned char *f, int len)
{
    int type = classify(f, len);
    int nth = ++counters[dir][type];
    const char *dirName = dir == DIR_TX2RX ? "tx2rx" : "rx2tx";
    const char *typeName = type == T_OTHER ? "?" : typeNames[type];
    int drop = 0, dup = 0, delay = 0;

    for (int i = 0; i < nRules; i++)
    {
        Rule *r = &rules[i];
        if (r->dir != dir || r->type != type || nth < r->from || (r->to != -1 && nth > r->to))
            continue;
        fprintf(stderr, "[relay] %s %s#%d: %s\n", dirName, typeName, nth, r->action);
        if (!strcmp(r->action, "drop"))
            drop = 1;
        else if (!strcmp(r->action, "bcc1"))
            f[3] ^= 0x55;
        else if (!strcmp(r->action, "data") && len > 6)
            f[4] ^= 0x01;
        else if (!strcmp(r->action, "trunc"))
            len = len / 2;
        else if (!strcmp(r->action, "escend") && len > 6)
            f[len - 2] = 0x7D;
        else if (!strcmp(r->action, "badesc") && len > 6 && len + 2 < MAX_FRAME)
        {
            memmove(f + 6, f + 4, len - 4);
            f[4] = 0x7D;
            f[5] = 0x00;
            len += 2;
        }
        else if (!strcmp(r->action, "dup"))
            dup = 1;
        else if (!strcmp(r->action, "garbage"))
        {
            unsigned char junk[] = {0xAA, 0x01, 0x55, 0x03, 0x9C};
            writeOut(outFd, junk, sizeof(junk));
        }
        else if (!strcmp(r->action, "delay"))
            delay = r->arg;
    }
    if (chaosOn)
    {
        if ((int)(rnd() % 100) < chaosDrop)
        {
            fprintf(stderr, "[relay] %s %s#%d: chaos drop\n", dirName, typeName, nth);
            return;
        }
        if ((int)(rnd() % 100) < chaosCorrupt)
        {
            f[rnd() % len] ^= (unsigned char)(1 + rnd() % 255);
            fprintf(stderr, "[relay] %s %s#%d: chaos corrupt\n", dirName, typeName, nth);
        }
        if ((int)(rnd() % 100) < chaosDup)
            dup = 1;
    }
    if (drop)
        return;
    if (delay > 0)
        deliverLater(outFd, f, len, delay);
    else
        writeOut(outFd, f, len);
    if (dup)
        writeOut(outFd, f, len);
}

typedef struct
{
    int dir;
    int inFd, outFd;
    unsigned char buf[MAX_FRAME];
    int len;
} Stream;

// Feeds bytes read from one side; splits them into frames at FLAGs.
static void feed(Stream *s, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (s->len == 0 && b[i] != FLAG)
        {
            writeOut(s->outFd, &b[i], 1); // junk outside any frame: pass through
            continue;
        }
        if (s->len < MAX_FRAME)
            s->buf[s->len++] = b[i];
        if (b[i] == FLAG && s->len > 1)
        {
            if (s->len == 2 && s->buf[0] == FLAG)
            {
                s->len = 1; // FLAG FLAG: keep a single opening FLAG
                continue;
            }
            forwardFrame(s->dir, s->outFd, s->buf, s->len);
            s->len = 0;
        }
    }
}

static int openPty(const char *linkPath, int *slaveFd)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0)
    {
        perror("posix_openpt");
        return -1;
    }
    const char *name = ptsname(m);
    if (name == NULL)
        return -1;
    // Keep one slave open ourselves so the master never sees a hang-up.
    *slaveFd = open(name, O_RDWR | O_NOCTTY);
    if (*slaveFd < 0)
        return -1;
    struct termios t;
    tcgetattr(*slaveFd, &t);
    cfmakeraw(&t);
    tcsetattr(*slaveFd, TCSANOW, &t);
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    unlink(linkPath);
    if (symlink(name, linkPath) < 0)
    {
        perror(linkPath);
        return -1;
    }
    return m;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <link-tx> <link-rx> [dir:type:nth:action[:arg] ...]\n", argv[0]);
        return 2;
    }
    for (int i = 3; i < argc && nRules < MAX_RULES; i++)
        if (parseRule(argv[i], &rules[nRules++]) < 0)
        {
            fprintf(stderr, "bad rule: %s\n", argv[i]);
            return 2;
        }

    const char *chaos = getenv("RELAY_CHAOS");
    if (chaos)
    {
        unsigned long long seed;
        if (sscanf(chaos, "%llu:%d:%d:%d", &seed, &chaosDrop, &chaosCorrupt, &chaosDup) != 4)
        {
            fprintf(stderr, "bad RELAY_CHAOS (seed:drop:corrupt:dup)\n");
            return 2;
        }
        rng = seed * 2654435761ULL + 88172645463325252ULL;
        chaosOn = 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int slaveA, slaveB;
    int mA = openPty(argv[1], &slaveA);
    int mB = openPty(argv[2], &slaveB);
    if (mA < 0 || mB < 0)
        return 1;

    Stream fromTx = {DIR_TX2RX, mA, mB, {0}, 0};
    Stream fromRx = {DIR_RX2TX, mB, mA, {0}, 0};
    fprintf(stderr, "[relay] ready: tx=%s rx=%s rules=%d\n", argv[1], argv[2], nRules);

    while (!stop)
    {
        struct pollfd p[2] = {{mA, POLLIN, 0}, {mB, POLLIN, 0}};
        poll(p, 2, nPending ? 20 : 200);
        flushPending();
        Stream *ss[2] = {&fromTx, &fromRx};
        for (int i = 0; i < 2; i++)
            if (p[i].revents & POLLIN)
            {
                unsigned char b[512];
                int n = read(ss[i]->inFd, b, sizeof(b));
                if (n > 0)
                    feed(ss[i], b, n);
            }
    }
    unlink(argv[1]);
    unlink(argv[2]);
    return 0;
}
