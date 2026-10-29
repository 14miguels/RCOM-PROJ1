// RCOM 2026/2027 - Milestone 1 demo (independent of the protocol)
//
// Exchanges a string over the serial port using only the provided serial port
// API. The string contains '\n' in the middle on purpose: in non-canonical
// mode the whole buffer must be sent regardless.
//
//   m1_strings tx <port>      sends the string
//   m1_strings rx <port>      prints what arrives; exit status 0 if it matches

#include <stdio.h>
#include <string.h>

#include "../src/serial_port.h"

static const char MSG[] = "Hello RCOM!\nsecond line\n";

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s tx|rx port\n", argv[0]);
        return 2;
    }
    if (openSerialPort(argv[2], 9600) < 0)
        return 1;

    int n = (int)strlen(MSG), result = 1;
    if (strcmp(argv[1], "tx") == 0)
        result = writeBytesSerialPort((const unsigned char *)MSG, n) == n ? 0 : 1;
    else
    {
        char got[sizeof(MSG)];
        int i = 0;
        unsigned char b;
        while (i < n && readByteSerialPort(&b) == 1)
            got[i++] = (char)b;
        got[i] = '\0';
        printf("received %d bytes: \"%.*s\"\n", i, i, got);
        result = (i == n && strcmp(got, MSG) == 0) ? 0 : 1;
    }
    closeSerialPort();
    return result;
}
