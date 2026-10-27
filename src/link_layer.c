// RCOM 2026/2027
//
// Link layer protocol implementation

#include "link_layer.h"
#include "serial_port.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source
#define BUF_SIZE 256

volatile sig_atomic_t alarmEnabled = FALSE;
volatile sig_atomic_t alarmCount = 0;
int txSequenceNum = 0;
int txTimeout = 0;
int txMaxRet = 0;

// Link layer state
static int expected_Ns = 0;

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

    txTimeout = llParameters.timeout;
    txMaxRet = llParameters.nRetransmissions;

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
        printf("Sending SET - Attemp %d\n", alarmCount+1);
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
                    if (var == 0x03)    // UA is a reply sent by Rx: A = 0x03
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
                    if (var == (0x03 ^ 0x07))   // BCC1 = A ^ C = 0x04
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
    int stop = FALSE;

    while (stop == FALSE)
    {
        // Read one byte from serial port.
        // NOTE: You must check how many bytes were actually read by reading the return value.
        // In this example, we assume that the byte is always read, which may not be true.
        unsigned char var;
        int bytes = readByteSerialPort(&var);

        if(bytes == -1 && errno == EINTR){
            continue;
        }

        if(bytes != 1){

            closeSerialPort();
            printf("Unable to read byte.\n");
            return -1;
        }


        nBytesBuf += bytes;

        printf("var = 0x%02X\n", var);

        switch (state){

            case (initial_state): 
                if (var == 0x7E)
                    state = address_state;
                break;
            case (address_state):
                if (var == 0x03)
                    state = control_state;
                else if (var != 0x7E)
                    state = initial_state;  
                break;
            case (control_state):
                if (var == 0x03)
                    state = bcc1_state;
                else if (var == 0x7E)
                    state = address_state;
                else
                    state = initial_state;
                break;
            case (bcc1_state):
                if (var == (0x03 ^ 0x03))
                    state = final_state;
                else if (var == 0x7E)
                    state = address_state;
                else
                    state = initial_state;
                break;
            case (final_state):
                if (var == 0x7E)
                    stop = TRUE;
                else
                    state = initial_state;
<<<<<<< HEAD
=======
                break;
>>>>>>> origin/Miguel
        }
    }

    printf("Total bytes received: %d\n", nBytesBuf);

    unsigned char arr[5];
    arr[0] = 0x7E;
    arr[1] = 0x03;  // UA is a reply sent by Rx: A = 0x03
    arr[2] = 0x07;
    arr[3] = arr[1] ^ arr[2];
    arr[4] = arr[0];

    int bytes = writeBytesSerialPort(arr, 5);

    if(bytes == 5)
        printf("%d bytes written to serial port\n", bytes);
    else{
        printf("Incomplete sending.\n");
        closeSerialPort();
        return -1;
    }

    return 0;
}

////////////////////////////////////////////////
// LLSEND
////////////////////////////////////////////////
int llSend(const unsigned char *buf, int bufSize)
{
    // frame is a VLA on the stack: reject sizes that would make it invalid or huge
    if (buf == NULL || bufSize < 0 || bufSize > MAX_PAYLOAD_SIZE)
        return -1;

    unsigned char frame [2* bufSize + 7];
    frame[0] = 0x7E;
    frame[1] = 0x03;
    if (txSequenceNum == 0)
        frame[2] = 0x00;
    else
        frame[2] = 0x80;
    
    frame[3] = frame[1] ^ frame[2];
    
    unsigned char bcc2 = 0;
    int curByte = 0;
    while(curByte < bufSize){

        bcc2 = bcc2 ^ buf[curByte];
        curByte++;
    }

    int frameIndex = 4;
    curByte = 0;
    while(curByte < bufSize){
        
        if(buf[curByte] != 0x7E && buf[curByte] != 0x7D)
            frame[frameIndex] = buf[curByte];
        
        else{

            frame[frameIndex] = 0x7D;
            frameIndex++;
            frame[frameIndex] = buf[curByte] ^ 0x20;
        }
        frameIndex++;
        curByte++;
    }

    if(bcc2 != 0x7E && bcc2 != 0x7D)
        frame[frameIndex] = bcc2;
    
    else{
        frame[frameIndex] = 0x7D;
        frameIndex++;
        frame[frameIndex] = bcc2 ^ 0x20;
    }
    frameIndex++;
    frame[frameIndex] = 0x7E;
    frameIndex++;

    typedef enum{
        initial_state,
        address_state,
        control_state,
        bcc1_state,
        final_state
    }RRState;
    
    RRState state = initial_state; 
    int rrReceived = FALSE;
    int rejReceived = FALSE;
    struct sigaction act = {0};
    
    act.sa_handler = &alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    alarmCount = 0;
    alarmEnabled = FALSE;
    int responseType = 0;
    int attempts = 0;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        closeSerialPort();
        return -1;
    }


    // attempts counts every transmission (timeout or REJ), so both respect the limit.
    // 1 original transmission + txMaxRet retransmissions, same convention as llOpenTx.
    while(attempts <= txMaxRet && rrReceived == FALSE){
        attempts++;
        responseType = 0;
        rejReceived = FALSE;
        state = initial_state;
        int nbytes = writeBytesSerialPort(frame, frameIndex);
        
        if(nbytes != frameIndex){
            printf("Error sending: wrote %d of %d bytes\n", nbytes, frameIndex);
            closeSerialPort();
            return -1;
        }

        alarmEnabled = TRUE;
        alarm(txTimeout);
    
        while(rrReceived == FALSE && alarmEnabled == TRUE && rejReceived == FALSE){
            
            unsigned char var;
            int bytes = readByteSerialPort(&var);
    
            if (bytes != 1) {
                if (alarmEnabled == FALSE)
                    break;

                if (bytes < 0 && errno == EINTR)
                    continue;

                alarm(0);
                printf("Error while reading the serial port.\n");
                closeSerialPort();
                return -1;
            }
    
            switch (state)
            {
            case initial_state:
                if (var == 0x7E)
                    state = address_state;
                    else {
                        state = initial_state;
                        
                    }
                break;
    
            case address_state:
            
                // Rx replies (RR/REJ) carry A = 0x03
                if (var == 0x03)
                    state = control_state;
                else if (var != 0x7E)   // a 0x7E here is a new FLAG: stay in address_state
                    state = initial_state;

                break;

            case control_state:
                // After I(Ns): accept RR(Ns^1), react to REJ(Ns); anything else is stale
                if(txSequenceNum == 0){
                    if(var == 0xAB){            // RR1
                        state = bcc1_state;
                        responseType = 1;
                    }

                    else if(var == 0x54){       // REJ0
                            state = bcc1_state;
                            responseType = 2;
                        }
                    else if(var == 0x7E)
                        state = address_state;
                    else {
                        state = initial_state;
                    }
                }
                else {
                    if(var == 0xAA){            // RR0
                        state = bcc1_state;
                        responseType = 1;
                    }

                    else if(var == 0x55){       // REJ1
                            state = bcc1_state;
                            responseType = 2;
                        }
                    else if(var == 0x7E)
                        state = address_state;
                    else{
                        state = initial_state;
                    }
                }
                break;

            case bcc1_state:
                // BCC1 = A ^ C = 0x03 ^ C
                if(txSequenceNum == 0){
                    if((responseType == 1 && var == 0xA8 )||    // 0x03 ^ 0xAB
                       (responseType == 2 && var == 0x57))      // 0x03 ^ 0x54
                        state = final_state;
                    else if(var == 0x7E)
                        state = address_state;
                    else
                        state = initial_state;

                    }
                    else {
                        if ((responseType == 1 && var == 0xA9)||    // 0x03 ^ 0xAA
                            (responseType == 2 && var == 0x56))     // 0x03 ^ 0x55
                            state = final_state;
                        else if(var == 0x7E)
                            state = address_state;
                        else
                            state = initial_state;

                }
                break;
            case final_state:
                if (var == 0x7E){

                    if(responseType == 1){
                        rrReceived = TRUE;
                        if(txSequenceNum == 0) txSequenceNum = 1;
                        else txSequenceNum = 0;
                        alarm(0);
                    }

                    else if (responseType == 2){
                        rejReceived = TRUE;
                        alarm(0);
                    }
                }
                else
                    state = initial_state;

                break;
            }
        }

    }

    alarm(0);

    // Success is decided by the RR, not by alarmCount (an alarm may fire
    // right before alarm(0) cancels it, even though the RR already arrived)
    if(rrReceived == FALSE){
        printf("Number of tries exceeded.\n");
        closeSerialPort();
        return -1;
    }

    return bufSize;
}

////////////////////////////////////////////////
// LLRECEIVE
////////////////////////////////////////////////
int llReceive(unsigned char *packet)
{
    typedef enum {
        initial_state,
        address_state,
        control_state,
        bcc1_state,
        data_state
    } State;

    State state = initial_state;
    unsigned char control_byte = 0;

    // Buffer para guardar os bytes recebidos da trama (com stuffing)
    unsigned char raw_data[2050];
    int raw_idx = 0;
    while (TRUE)
    {
        unsigned char var;
        int bytes = readByteSerialPort(&var);
        if (bytes <= 0)
        {
            if (bytes < 0 && errno == EINTR)
                continue;
            return -1;
        }
        switch (state)
        {
            case initial_state:
                if (var == 0x7E)
                    state = address_state;
                break;

            case address_state:
                if (var == 0x7E)
                    state = address_state;
                else if (var == 0x03)
                    state = control_state;
                else
                    state = initial_state;
                break;
                
            case control_state:
                if (var == 0x7E)
                    state = address_state;
                else if (var == 0x00 || var == 0x80)
                {
                    control_byte = var;
                    state = bcc1_state;
                }
                else
                    state = initial_state;
                break;

            case bcc1_state:
                if (var == 0x7E)
                    state = address_state;
                else if (var == (0x03 ^ control_byte)) // BCC1 = A ^ C
                {
                    raw_idx = 0;
                    state = data_state;
                }
                else
                {
                    state = initial_state;
                }
                break;

            case data_state:
                if (var == 0x7E)
                {
                    int ns = (control_byte == 0x00) ? 0 : 1;
                    // 1. Destuffing dos dados + BCC2
                    unsigned char destuffed[1050];
                    int destuffed_len = 0;
                    int destuff_error = FALSE;
                    for (int i = 0; i < raw_idx; i++)
                    {
                        if (raw_data[i] == 0x7D)
                        {
                            i++;
                            if (i >= raw_idx) {
                                destuff_error = TRUE;
                                break;
                            }
                            if (raw_data[i] == 0x5E)
                                destuffed[destuffed_len++] = 0x7E;
                            else if (raw_data[i] == 0x5D)
                                destuffed[destuffed_len++] = 0x7D;
                            else {
                                destuff_error = TRUE;
                                break;
                            }
                        }
                        else
                        {
                            destuffed[destuffed_len++] = raw_data[i];
                        }
                    }
                    // Se não tiver pelo menos o byte de BCC2
                    if (destuff_error || destuffed_len < 1)
                    {
                        // Envia REJ se for a trama esperada, ou RR se for repetida
                        unsigned char rej_rr = (ns == expected_Ns) 
                            ? ((expected_Ns == 0) ? 0x54 : 0x55) 
                            : ((expected_Ns == 0) ? 0xAA : 0xAB);
                        unsigned char reply[5] = {0x7E, 0x03, rej_rr, 0x03 ^ rej_rr, 0x7E};
                        writeBytesSerialPort(reply, 5);
                        state = initial_state;
                        break;
                    }
                    // 2. Verificar BCC2 (o último byte é o BCC2 recebido)
                    int payload_len = destuffed_len - 1;
                    unsigned char bcc2_rcv = destuffed[payload_len];
                    unsigned char bcc2_calc = 0;
                    for (int i = 0; i < payload_len; i++)
                        bcc2_calc ^= destuffed[i];
                    int bcc2_ok = (bcc2_calc == bcc2_rcv);
                    // 3. Regras Stop-and-Wait
                    if (ns == expected_Ns)
                    {
                        if (bcc2_ok)
                        {
                            // Trama válida e nova: entrega à aplicação
                            memcpy(packet, destuffed, payload_len);
                            // Envia RR para pedir a próxima trama:
                            // Se esperava 0, pede 1 (0xAB). Se esperava 1, pede 0 (0xAA).
                            unsigned char next_rr = (expected_Ns == 0) ? 0xAB : 0xAA;
                            unsigned char reply[5] = {0x7E, 0x03, next_rr, 0x03 ^ next_rr, 0x7E};
                            writeBytesSerialPort(reply, 5);
                            // Alterna número de sequência esperado
                            expected_Ns = 1 - expected_Ns;
                            return payload_len;
                        }
                        else
                        {
                            // Erro nos dados: envia REJ
                            // REJ0 = 0x54 | REJ1 = 0x55
                            unsigned char rej = (expected_Ns == 0) ? 0x54 : 0x55;
                            unsigned char reply[5] = {0x7E, 0x03, rej, 0x03 ^ rej, 0x7E};
                            writeBytesSerialPort(reply, 5);
                            state = initial_state; // Aguarda retransmissão no loop
                        }
                    }
                    else
                    {
                        // Trama duplicada: confirma com RR da trama que ainda espera
                        // RR0 = 0xAA | RR1 = 0xAB
                        unsigned char cur_rr = (expected_Ns == 0) ? 0xAA : 0xAB;
                        unsigned char reply[5] = {0x7E, 0x03, cur_rr, 0x03 ^ cur_rr, 0x7E};
                        writeBytesSerialPort(reply, 5);
                        state = initial_state; // Continua à espera da nova trama
                    }
                }
                else
                {
                    if (raw_idx < (int)sizeof(raw_data))
                        raw_data[raw_idx++] = var;
                    else
                        state = initial_state;
                }
                break;
        }
    }
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
