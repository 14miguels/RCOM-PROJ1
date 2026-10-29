// RCOM 2026/2027 - test tool (not part of the delivered protocol)
//
// Same as src/main.c but with the number of retries and the timeout as
// arguments (main.c fixes them at 3 and 4 s, too slow for automatic tests).
//
//   driver <port> <baud> tx|rx <file> <nTries> <timeoutSeconds>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/application_layer.h"

int main(int argc, char *argv[])
{
    if (argc != 7 || (strcmp(argv[3], "tx") != 0 && strcmp(argv[3], "rx") != 0))
    {
        fprintf(stderr, "usage: %s port baud tx|rx file nTries timeout\n", argv[0]);
        return 2;
    }
    applicationLayer(argv[1], argv[3], atoi(argv[2]), atoi(argv[5]), atoi(argv[6]), argv[4]);
    return 0;
}
