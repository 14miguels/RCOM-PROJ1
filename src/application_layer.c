// RCOM 2026/2027
//
// Application layer protocol implementation
//
// Packets (all multi-byte numbers are big endian):
//   START / END : C(1) | { T(1) L(1) V(L) } ...      C = 1 / 3
//                 T = 0 file size (mandatory), T = 1 file name
//   DATA        : C(1)=2 | L2(1) | L1(1) | P1 ... PK   K = 256*L2 + L1
// This layer knows nothing about frames, BCC, stuffing or retransmissions.

#include "application_layer.h"
#include "link_layer.h"

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define C_DATA 0x02
#define C_START 0x01
#define C_END 0x03

#define T_FILE_SIZE 0x00
#define T_FILE_NAME 0x01

#define DATA_HEADER_SIZE 3 // C, L2, L1

// File bytes per DATA packet. The default fills the largest payload the link
// layer accepts; it can be overridden at compile time (-DAPP_CHUNK_SIZE=...)
// to study the effect of the frame size on the efficiency.
#ifndef APP_CHUNK_SIZE
#define APP_CHUNK_SIZE (MAX_PAYLOAD_SIZE - DATA_HEADER_SIZE)
#endif
#if APP_CHUNK_SIZE < 1 || APP_CHUNK_SIZE > MAX_PAYLOAD_SIZE - DATA_HEADER_SIZE
#error "APP_CHUNK_SIZE must fit in one link layer payload"
#endif

#define MAX_NAME_LEN 255 // a TLV length is one byte

// Metadata carried by START and END.
typedef struct
{
    unsigned long long size;
    int hasName;
    char name[MAX_NAME_LEN + 1];
} FileInfo;

////////////////////////////////////////////////
// PACKET BUILDING / PARSING
////////////////////////////////////////////////

// Builds a START or END packet in out (at least MAX_PAYLOAD_SIZE bytes).
// Returns the packet length.
static int buildControlPacket(unsigned char *out, unsigned char control, const FileInfo *info)
{
    int idx = 0;
    out[idx++] = control;

    // file size: shortest big-endian representation (1..8 bytes)
    int sizeLen = 1;
    while (sizeLen < 8 && (info->size >> (8 * sizeLen)) != 0)
        sizeLen++;
    out[idx++] = T_FILE_SIZE;
    out[idx++] = (unsigned char)sizeLen;
    for (int i = sizeLen - 1; i >= 0; i--)
        out[idx++] = (unsigned char)(info->size >> (8 * i));

    if (info->hasName)
    {
        int nameLen = (int)strlen(info->name); // text field: strlen is fine here
        out[idx++] = T_FILE_NAME;
        out[idx++] = (unsigned char)nameLen;
        memcpy(out + idx, info->name, nameLen);
        idx += nameLen;
    }
    return idx;
}

// Parses a START or END packet of the expected control type.
// Returns 0 if well formed and containing the mandatory file size, -1 otherwise.
static int parseControlPacket(const unsigned char *p, int len, unsigned char control, FileInfo *info)
{
    int gotSize = FALSE;

    memset(info, 0, sizeof(*info));
    if (len < 1 || p[0] != control)
        return -1;

    int idx = 1;
    while (idx < len)
    {
        if (idx + 2 > len)
            return -1; // truncated TLV header
        unsigned char type = p[idx];
        int vlen = p[idx + 1];
        idx += 2;
        if (idx + vlen > len)
            return -1; // value runs past the end of the packet

        if (type == T_FILE_SIZE)
        {
            if (vlen < 1 || vlen > 8 || gotSize)
                return -1;
            info->size = 0;
            for (int i = 0; i < vlen; i++)
                info->size = (info->size << 8) | p[idx + i];
            gotSize = TRUE;
        }
        else if (type == T_FILE_NAME)
        {
            memcpy(info->name, p + idx, vlen); // vlen <= 255: always fits
            info->name[vlen] = '\0';
            info->hasName = TRUE;
        }
        // unknown types are skipped
        idx += vlen;
    }
    return gotSize ? 0 : -1;
}

// DATA packet: C | L2 | L1 | data. Returns the packet length.
static int buildDataPacket(unsigned char *out, const unsigned char *data, int n)
{
    out[0] = C_DATA;
    out[1] = (unsigned char)(n >> 8);
    out[2] = (unsigned char)(n & 0xFF);
    memcpy(out + DATA_HEADER_SIZE, data, n);
    return DATA_HEADER_SIZE + n;
}

// Validates a DATA packet; on success *data points inside p and *n is K.
static int parseDataPacket(const unsigned char *p, int len, const unsigned char **data, int *n)
{
    if (len < DATA_HEADER_SIZE || p[0] != C_DATA)
        return -1;
    int k = 256 * p[1] + p[2];
    if (k != len - DATA_HEADER_SIZE)
        return -1; // declared size differs from the real size
    *data = p + DATA_HEADER_SIZE;
    *n = k;
    return 0;
}

static int sameInfo(const FileInfo *a, const FileInfo *b)
{
    return a->size == b->size && a->hasName == b->hasName &&
           (!a->hasName || strcmp(a->name, b->name) == 0);
}

////////////////////////////////////////////////
// TRANSMITTER
////////////////////////////////////////////////

// Returns 0 on success, -1 on error. Always closes the link layer.
static int runTx(LinkLayer ll, const char *filename)
{
    struct stat st;
    if (stat(filename, &st) == -1)
    {
        perror(filename);
        return -1;
    }
    if (!S_ISREG(st.st_mode))
    {
        fprintf(stderr, "%s: not a regular file\n", filename);
        return -1;
    }

    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        perror(filename);
        return -1;
    }

    FileInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (unsigned long long)st.st_size;
    char nameCopy[4096];
    snprintf(nameCopy, sizeof(nameCopy), "%s", filename);
    info.hasName = TRUE;
    snprintf(info.name, sizeof(info.name), "%s", basename(nameCopy));

    if (llOpenTx(ll) < 0)
    {
        fclose(fp);
        return -1;
    }

    int ok = FALSE;
    unsigned char packet[MAX_PAYLOAD_SIZE];
    unsigned char chunk[APP_CHUNK_SIZE];
    unsigned long long sent = 0;

    do
    {
        int len = buildControlPacket(packet, C_START, &info);
        if (llSend(packet, len) < 0)
            break;

        int readError = FALSE;
        int sendError = FALSE;
        while (sent < info.size)
        {
            size_t n = fread(chunk, 1, sizeof(chunk), fp);
            if (n == 0)
            {
                // fewer bytes than announced: the file shrank or cannot be read
                fprintf(stderr, "%s: read error or file truncated\n", filename);
                readError = TRUE;
                break;
            }
            if (n > info.size - sent)
                n = (size_t)(info.size - sent); // file grew: send only the announced size
            len = buildDataPacket(packet, chunk, (int)n);
            if (llSend(packet, len) < 0)
            {
                sendError = TRUE;
                break;
            }
            sent += n;
        }
        if (readError || sendError)
            break;

        len = buildControlPacket(packet, C_END, &info);
        if (llSend(packet, len) < 0)
            break;
        ok = TRUE;
    } while (0);

    fclose(fp);
    if (llCloseTx() < 0)
        ok = FALSE;

    if (ok)
        printf("File %s sent (%llu bytes)\n", filename, info.size);
    return ok ? 0 : -1;
}

////////////////////////////////////////////////
// RECEIVER
////////////////////////////////////////////////

// The file is written to "<filename>.part" and renamed to <filename> only
// after END confirmed the metadata and the size, so an interrupted or
// corrupted transfer never leaves (or replaces) a file that looks complete.
// Returns 0 on success, -1 on error. Always closes the link layer.
static int runRx(LinkLayer ll, const char *filename)
{
    char tmpName[4096 + 8];
    if (snprintf(tmpName, sizeof(tmpName), "%s.part", filename) >= (int)sizeof(tmpName))
    {
        fprintf(stderr, "file name too long\n");
        return -1;
    }

    if (llOpenRx(ll) < 0)
        return -1;

    unsigned char packet[MAX_PAYLOAD_SIZE];
    FileInfo startInfo, endInfo;
    FILE *fp = NULL;
    unsigned long long received = 0;
    int ok = FALSE;

    do
    {
        // START
        int len = llReceive(packet);
        if (len < 0)
            break;
        if (parseControlPacket(packet, len, C_START, &startInfo) < 0)
        {
            fprintf(stderr, "expected a valid START packet\n");
            break;
        }
        fp = fopen(tmpName, "wb");
        if (fp == NULL)
        {
            perror(tmpName);
            break;
        }

        // DATA ... END
        int finished = FALSE;
        int error = FALSE;
        while (!finished && !error)
        {
            len = llReceive(packet);
            if (len < 0)
            {
                error = TRUE;
            }
            else if (len >= 1 && packet[0] == C_DATA)
            {
                const unsigned char *data;
                int n;
                if (parseDataPacket(packet, len, &data, &n) < 0)
                {
                    fprintf(stderr, "malformed DATA packet\n");
                    error = TRUE;
                }
                else if ((unsigned long long)n > startInfo.size - received)
                {
                    fprintf(stderr, "more data than announced in START\n");
                    error = TRUE;
                }
                else if (n > 0 && fwrite(data, 1, n, fp) != (size_t)n)
                {
                    perror("fwrite");
                    error = TRUE;
                }
                else
                    received += n;
            }
            else if (parseControlPacket(packet, len, C_END, &endInfo) == 0)
            {
                finished = TRUE;
            }
            else
            {
                fprintf(stderr, "unexpected packet (C = 0x%02X)\n", len > 0 ? packet[0] : 0);
                error = TRUE;
            }
        }
        if (error)
            break;

        if (!sameInfo(&startInfo, &endInfo))
        {
            fprintf(stderr, "END metadata differs from START\n");
            break;
        }
        if (received != startInfo.size)
        {
            fprintf(stderr, "received %llu bytes, expected %llu\n", received, startInfo.size);
            break;
        }
        // fclose() flushes: a full disk shows up here
        int closeRes = fclose(fp);
        fp = NULL;
        if (closeRes != 0)
        {
            perror("fclose");
            break;
        }
        if (rename(tmpName, filename) == -1)
        {
            perror("rename");
            break;
        }
        ok = TRUE;
    } while (0);

    if (fp != NULL)
        fclose(fp);
    if (!ok)
        remove(tmpName);

    if (llCloseRx() < 0)
        ok = FALSE; // closing failed; the file (if any) was already complete

    if (ok)
        printf("File %s received (%llu bytes)\n", filename, received);
    return ok ? 0 : -1;
}

////////////////////////////////////////////////
// ENTRY POINT
////////////////////////////////////////////////

void applicationLayer(const char *serialPort, const char *role, int baudRate,
                      int nTries, int timeout, const char *filename)
{
    LinkLayer ll;
    memset(&ll, 0, sizeof(ll));
    ll.baudRate = baudRate;
    ll.nRetransmissions = nTries;
    ll.timeout = timeout;
    if (snprintf(ll.serialPort, sizeof(ll.serialPort), "%s", serialPort) >= (int)sizeof(ll.serialPort))
    {
        fprintf(stderr, "serial port name too long\n");
        return;
    }

    if (strcmp(role, "tx") == 0)
    {
        if (runTx(ll, filename) < 0)
            fprintf(stderr, "Transmission of %s FAILED\n", filename);
    }
    else if (strcmp(role, "rx") == 0)
    {
        if (runRx(ll, filename) < 0)
            fprintf(stderr, "Reception of %s FAILED\n", filename);
    }
    else
        fprintf(stderr, "Invalid role: %s. Must be 'tx' or 'rx'.\n", role);
}
