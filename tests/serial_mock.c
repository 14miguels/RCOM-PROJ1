// RCOM 2026/2027 - test tool
// In-memory replacement for serial_port.c used by the unit tests.
// When no input is available, read() behaves like the real port interrupted by
// the ticker: it waits 10 ms and fails with EINTR.

#include "serial_mock.h"
#include "../src/serial_port.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static unsigned char in[MOCK_BUF];
static int inLen, inPos;

unsigned char mockOut[MOCK_BUF];
int mockOutLen;
int mockWriteLimit;
int mockOpenCount, mockCloseCount;
void (*mockOnWrite)(void);

void mockReset(void)
{
    inLen = inPos = mockOutLen = 0;
    mockWriteLimit = 0;
    mockOnWrite = NULL;
}

void mockPush(const unsigned char *bytes, int n)
{
    if (inLen + n <= MOCK_BUF)
    {
        memcpy(in + inLen, bytes, n);
        inLen += n;
    }
}

void mockPushSup(unsigned char a, unsigned char c)
{
    unsigned char f[5] = {0x7E, a, c, a ^ c, 0x7E};
    mockPush(f, 5);
}

int openSerialPort(const char *serialPort, int baudRate)
{
    (void)serialPort; (void)baudRate;
    mockOpenCount++;
    return 3;
}

int closeSerialPort(void)
{
    mockCloseCount++;
    return 0;
}

int readByteSerialPort(unsigned char *byte)
{
    if (inPos < inLen)
    {
        *byte = in[inPos++];
        return 1;
    }
    struct timespec ts = {0, 10 * 1000000L};
    nanosleep(&ts, NULL);
    errno = EINTR;
    return -1;
}

int writeBytesSerialPort(const unsigned char *bytes, int nBytes)
{
    int n = nBytes;
    if (mockWriteLimit > 0 && n > mockWriteLimit)
        n = mockWriteLimit;
    if (mockOutLen + n <= MOCK_BUF)
    {
        memcpy(mockOut + mockOutLen, bytes, n);
        mockOutLen += n;
    }
    if (mockOnWrite)
        mockOnWrite();
    return n;
}
