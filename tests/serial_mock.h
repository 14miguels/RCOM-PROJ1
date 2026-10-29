// RCOM 2026/2027 - test tool
// In-memory replacement for serial_port.c used by the unit tests.
#ifndef SERIAL_MOCK_H
#define SERIAL_MOCK_H

#define MOCK_BUF 65536

extern unsigned char mockOut[MOCK_BUF]; // everything the code under test wrote
extern int mockOutLen;
extern int mockWriteLimit;              // >0: write() accepts at most this many bytes per call
extern int mockOpenCount, mockCloseCount;
// Called after every write; may call mockPush() to simulate the peer's answer.
extern void (*mockOnWrite)(void);

void mockReset(void);
void mockPush(const unsigned char *bytes, int n); // bytes the "line" delivers to the code
void mockPushSup(unsigned char a, unsigned char c); // supervision frame with valid BCC1

#endif
