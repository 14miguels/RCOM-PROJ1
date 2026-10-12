// RCOM 2026/2027
//
// Link layer protocol implementation

#include "link_layer.h"
#include "serial_port.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source
#define BUF_SIZE 256

volatile sig_atomic_t alarmEnabled = FALSE;
volatile sig_atomic_t alarmCount = 0;

void alarmHandler(int signal)
{
    alarmEnabled = FALSE;
    alarmCount++;
}

////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////

int llOpenTx(LinkLayer llParameters)
{
    // ----------------------------------------------------
    // This example code shows how to open the serial port and send a string.
    // TODO: Adapt and extend this code according to the specifications of the project.
    // ----------------------------------------------------

    if (openSerialPort(llParameters.serialPort, llParameters.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }

    printf("Serial port %s opened\n", llParameters.serialPort);
    alarmCount = 0;
    alarmEnabled = FALSE;
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;

    if (sigaction(SIGALRM, &act, NULL) == -1)
    {
        perror("sigaction");
        closeSerialPort();
        return -1;
    }

    // In non-canonical mode, '\n' does not end the writing.
    // Test this condition by placing a '\n' in the middle of the buffer.
    // The whole buffer must be sent even with the '\n'.
    unsigned char arr[5];
    arr[0] = 0x7E;
    arr[1] = 0x03;
    arr[2] = 0x03;
    arr[3] = arr[1] ^ arr[2];
    arr[4] = arr[0];

    int nBytesBuf = 0;
    int uaReceived = FALSE;

    typedef enum {
        initial_state,
        address_state,
        control_state,
        bcc1_state,
        final_state
    }State ;

    State state = initial_state;

    while (alarmCount <= llParameters.nRetransmissions 
        && uaReceived == FALSE)
    {
        // Read one byte from serial port.
        // NOTE: You must check how many bytes were actually read by reading the return value.
        // In this example, we assume that the byte is always read, which may not be true.
        state = initial_state;
        
        int written = writeBytesSerialPort(arr, 5);

        if (written != 5){

            closeSerialPort();
            return -1;
        }

        alarmEnabled = TRUE;
        alarm(llParameters.timeout);

        while(alarmEnabled == TRUE && uaReceived == FALSE)
        {
            unsigned char var;
            int bytes = readByteSerialPort(&var);

            if(bytes != 1)
            {
                if(alarmEnabled == FALSE) break;

                if(bytes < 0 && errno == EINTR) continue;

                alarm(0);
                closeSerialPort();
                return -1;
            }
            nBytesBuf += bytes;

            printf("var = 0x%02X\n", var);

            switch (state)
            {
                case initial_state:
                    if (var == 0x7E)
                        state = address_state;
                    break;

                case address_state:
                    if (var == 0x01)
                        state = control_state;
                    else if (var != 0x7E)
                        state = initial_state;
                    break;

                case control_state:
                    if (var == 0x07)
                        state = bcc1_state;
                    else if (var == 0x7E)
                        state = address_state;
                    else
                        state = initial_state;
                    break;

                case bcc1_state:
                    if (var == 0x06)
                        state = final_state;
                    else if (var == 0x7E)
                        state = address_state;
                    else
                        state = initial_state;
                    break;

                case final_state:
                    if (var == 0x7E)
                    {
                        uaReceived = TRUE;
                        alarm(0);
                        alarmEnabled = FALSE;
                    }
                    else
                        state = initial_state;
                    break;
            }
        }
    }
    alarm(0);

    if (uaReceived == FALSE)
    {
        printf("Failed to establish connection\n");
        closeSerialPort();
        return -1;
    }

    printf("Total bytes received: %d\n", nBytesBuf);

    return 0;
}

int llOpenRx(LinkLayer llParameters)
{
    // ----------------------------------------------------
    // This example code shows how to open the serial port and receive a string.
    // TODO: Adapt and extend this code according to the specifications of the project.
    // ----------------------------------------------------

    if (openSerialPort(llParameters.serialPort, llParameters.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }

    printf("Serial port %s opened\n", llParameters.serialPort);

    // Read from serial port until the 'z' char is received.

    // NOTE: This while() cycle is a simple example showing how to read from the serial port.
    // It must be changed in order to respect the specifications of the protocol indicated in the Lab guide.
    // TODO: Save the received bytes in a buffer array and print it at the end of the program.
    
    int nBytesBuf = 0;

    typedef enum {
        initial_state,
        address_state,
        control_state,
        bcc1_state,
        final_state
    }State ;

    State state = initial_state;

    while (STOP == FALSE)
    {
        // Read one byte from serial port.
        // NOTE: You must check how many bytes were actually read by reading the return value.
        // In this example, we assume that the byte is always read, which may not be true.
        unsigned char var;
        int bytes = readByteSerialPort(&var);
        nBytesBuf += bytes;

        printf("var = 0x%02X\n", var);

        switch (state){

            case (initial_state): 
                state = address_state;
                break;
            case (address_state):
                state = control_state;
                break;
            case (control_state):
                state = bcc1_state;
                break;
            case (bcc1_state):
                state = final_state;
                break;
            case (final_state):
                break;
        }
        

        if (state == final_state)
        {
            printf("Final state. Stop reading from serial port.\n");
            STOP = TRUE;
        }
    }

    printf("Total bytes received: %d\n", nBytesBuf);

    unsigned char arr[5];
    arr[0] = 0x7E;
    arr[1] = 0x01;
    arr[2] = 0x07;
    arr[3] = arr[1] ^ arr[2];
    arr[4] = arr[0];

    int bytes = writeBytesSerialPort(arr, 5);
    printf("%d bytes written to serial port\n", bytes);

    // Wait until all bytes have been written to the serial port
    sleep(1);
    // Close serial port
    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }

    printf("Serial port %s closed\n", llParameters.serialPort);

    return 0;
}

////////////////////////////////////////////////
// LLSEND
////////////////////////////////////////////////
int llSend(const unsigned char *buf, int bufSize)
{
    // TODO: Implement this function

    return 0;
}

////////////////////////////////////////////////
// LLRECEIVE
////////////////////////////////////////////////
int llReceive(unsigned char *packet)
{
    // TODO: Implement this function

    return 0;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llCloseTx()
{
    // TODO: Implement this function

    return 0;
}

int llCloseRx()
{
    // TODO: Implement this function

    return 0;
}
