#include <pico/stdlib.h>
#include "modem/tcp_support.h"
#include "modem/commands.h"
#include "modem/ser_cdc.h"
#include <string.h>

const char okStr[] = {"OK"};
const char connectStr[] = {"CONNECT"};
const char ringStr[] = {"RING"};
const char noCarrierStr[] = {"NO CARRIER"};
const char errorStr[] = {"ERROR"};
const char noAnswerStr[] = {"NO ANSWER"};
const char *const resultCodes[] = {okStr, connectStr, ringStr, noCarrierStr, errorStr, noAnswerStr, ringStr};

void crlf(void)
{
    ser_puts(ser0, "\r\n");
}

//
// send serial data to the TCP client
//
void sendSerialData()
{
    static uint64_t lastSerialData = 0;
    // in telnet mode, we might have to escape every single char,
    // so don't use more than half the buffer
    size_t maxBufSize = (sessionTelnetType != NO_TELNET) ? TX_BUF_SIZE / 2 : TX_BUF_SIZE;
    size_t len = ser_is_readable(ser0);
    if (len > maxBufSize)
    {
        len = maxBufSize;
    }
    uint8_t *p = txBuf;
    for (size_t i = 0; i < len; ++i)
    {
        *p++ = ser_getc(ser0);
    }

    uint64_t serialInterval = millis() - lastSerialData;
    // if more than 1 second since the last character,
    // start the online escape sequence counter over again
    if (escCount && serialInterval >= GUARD_TIME)
    {
        escCount = 0;
    }
    if (settings.escChar < 128 && (escCount || serialInterval >= GUARD_TIME))
    {
        // check for the online escape sequence
        // +++ with a 1 second pause before and after
        // if escape character is >= 128, it's ignored
        for (size_t i = 0; i < len; ++i)
        {
            if (txBuf[i] == settings.escChar)
            {
                if (++escCount == ESC_COUNT)
                {
                    guardTime = millis() + GUARD_TIME;
                }
                else
                {
                    guardTime = 0;
                }
            }
            else
            {
                escCount = 0;
            }
        }
    }
    else
    {
        escCount = 0;
    }
    lastSerialData = millis();

    // in Telnet mode, escape every IAC (0xff) by inserting another
    // IAC after it into the buffer (this is why we only read up to
    // half of the buffer in Telnet mode)
    //
    // also in Telnet mode, escape every CR (0x0D) by inserting a NUL
    // after it into the buffer
    if (sessionTelnetType != NO_TELNET)
    {
        for (int i = len - 1; i >= 0; --i)
        {
            if (txBuf[i] == IAC)
            {
                memmove(txBuf + i + 1, txBuf + i, len - i);
                ++len;
            }
            else if (txBuf[i] == CR && sessionTelnetType == REAL_TELNET)
            {
                memmove(txBuf + i + 1, txBuf + i, len - i);
                txBuf[i + 1] = NUL;
                ++len;
            }
        }
    }
    bytesOut += tcpWriteBuf(tcpClient, txBuf, len);
}

//
// Receive data from the TCP client
//
// We do some limited processing of in band Telnet commands.
// Specifically, we handle the following commanads: BINARY,
// ECHO, SUP_GA (suppress go ahead), TTYPE (terminal type),
// TSPEED (terminal speed), LOC (terminal location) and
// NAWS (terminal columns and rows).
//
// Non-blocking, state-machine version of receiveTcpData()
// Returns -1 if no data available or still processing a Telnet sequence.
//
// TODO this needs to reset
//
int receiveTcpData(void)
{
    enum
    {
        RX_IDLE,
        RX_IAC,
        RX_IAC_CMD,
        RX_SB_DATA,
        RX_SB_WAIT_SE
    };
    static int rx_state = RX_IDLE;
    static char lastc = 0;
    static uint8_t telnet_cmd1 = 0, telnet_cmd2 = 0;
    static uint8_t sb_option = 0;
    static uint8_t txBuf[256];
    static uint16_t txLen = 0;

    int rxByte;

    switch (rx_state)
    {
    case RX_IDLE:
        rxByte = tcpReadByte(tcpClient);
        if (rxByte < 0)
            return -1;
        ++bytesIn;

        if (sessionTelnetType != NO_TELNET && rxByte == IAC)
        {
            rx_state = RX_IAC;
            return -1;
        }
        // Telnet sends <CR> as <CR><NUL>
        if (lastc == CR && (char)rxByte == 0 && sessionTelnetType == REAL_TELNET)
        {
            rxByte = -1;
        }
        lastc = (char)rxByte;
        return rxByte;

    case RX_IAC:
        rxByte = tcpReadByte(tcpClient);
        if (rxByte < 0)
            return -1;
        ++bytesIn;
        if (rxByte == DM)
        { // ignore data marks
            rx_state = RX_IDLE;
            return -1;
        }
        else if (rxByte == BRK)
        { // break?
            ser_set_break(ser0, true);
            sleep_ms(300); ////////////////// TODO
            ser_set_break(ser0, false);
            rx_state = RX_IDLE;
            return -1;
        }
        else if (rxByte == AYT)
        { // are you there?
#ifndef NDEBUG
            char tBuf[160];
            snprintf(tBuf, sizeof tBuf, "\r\nrxLen: %u rxHead: %u rxTail: %u\r\ntxLen: %u, txHead: %u, txTail:%u\r\n",
                     tcpClient->rxBuffLen, tcpClient->rxBuffHead, tcpClient->rxBuffTail,
                     tcpClient->txBuffLen, tcpClient->txBuffHead, tcpClient->txBuffTail);
            bytesOut += tcpWriteStr(tcpClient, tBuf);
            snprintf(tBuf, sizeof tBuf, "maxTotLen: %u\r\nmaxRxBuffLen: %u\r\nmaxTxBuffLen: %u\r\n",
                     maxTotLen, maxRxBuffLen, maxTxBuffLen);
            bytesOut += tcpWriteStr(tcpClient, tBuf);
            if (lastTcpWriteErr != ERR_OK)
            {
                snprintf(tBuf, sizeof tBuf, "lastTcpWriteErr: %d\r\n", lastTcpWriteErr);
                lastTcpWriteErr = ERR_OK;
                bytesOut += tcpWriteStr(tcpClient, tBuf);
            }
            // if (gpio_get_out_level(RXBUFF_OVFL))
            // {
            //     gpio_put(RXBUFF_OVFL, LOW);
            //     bytesOut += tcpWriteStr(tcpClient, "RXBUFF_OVFL\r\n");
            // }
            // if (gpio_get_out_level(TXBUFF_OVFL))
            // {
            //     gpio_put(TXBUFF_OVFL, LOW);
            //     bytesOut += tcpWriteStr(tcpClient, "TXBUFF_OVFL\r\n");
            // }
#else
            bytesOut += tcpWriteStr(tcpClient, "\r\n[Yes]\r\n");
#endif
            rx_state = RX_IDLE;
            return -1;
        }
        else if (rxByte == IAC)
        {
            rx_state = RX_IDLE;
            lastc = IAC;
            return IAC; // Escaped 0xFF
        }
        else
        {
            telnet_cmd1 = rxByte;
            if (telnet_cmd1 == DO || telnet_cmd1 == DONT || telnet_cmd1 == WILL || telnet_cmd1 == WONT || telnet_cmd1 == SB)
            {
                rx_state = RX_IAC_CMD;
                return -1;
            }
            else
            {
                // Single-byte command
                rx_state = RX_IDLE;
                return -1;
            }
        }
        break;

    case RX_IAC_CMD:
        rxByte = tcpReadByte(tcpClient);
        if (rxByte < 0)
            return -1;
        telnet_cmd2 = rxByte;
        txLen = 0;
        // Handle Telnet negotiation (see original code for details)
        switch (telnet_cmd1)
        {
        case DO:
            switch (telnet_cmd2)
            {
            case BINARY:
            case ECHO:
            case SUP_GA:
            case TTYPE:
            case TSPEED:
                if (amClient || (telnet_cmd2 != SUP_GA && telnet_cmd2 != ECHO))
                {

                    // in a server connection, we've already sent out
                    // WILL SUP_GA and WILL ECHO so we shouldn't again
                    // to prevent an endless round robin of WILLs and
                    // DOs SUP_GA/ECHO echoing back and forth
                    txBuf[txLen++] = IAC;
                    txBuf[txLen++] = WILL;
                    txBuf[txLen++] = telnet_cmd2;
                    bytesOut += tcpWriteBuf(tcpClient, txBuf, txLen);
                }
                break;
            case LOC:
            case NAWS:
                txBuf[txLen++] = IAC;
                txBuf[txLen++] = WILL;
                txBuf[txLen++] = telnet_cmd2;
                txBuf[txLen++] = IAC;
                txBuf[txLen++] = SB;
                txBuf[txLen++] = telnet_cmd2;
                switch (telnet_cmd2)
                {
                case NAWS: // window size
                    txBuf[txLen++] = (uint8_t)0;
                    txBuf[txLen++] = settings.width;
                    txBuf[txLen++] = (uint8_t)0;
                    txBuf[txLen++] = settings.height;
                    break;
                case LOC: // terminal location
                    txLen += snprintf((char *)txBuf + txLen, (sizeof txBuf) - txLen, "%s", settings.location);
                    break;
                }
                txBuf[txLen++] = IAC;
                txBuf[txLen++] = SE;
                bytesOut += tcpWriteBuf(tcpClient, txBuf, txLen);
                break;
            default:
                txBuf[txLen++] = IAC;
                txBuf[txLen++] = WONT;
                txBuf[txLen++] = telnet_cmd2;
                bytesOut += tcpWriteBuf(tcpClient, txBuf, txLen);
                break;
            }
            break;
        case WILL:
            // Server wants to do option, allow most
            txBuf[txLen++] = IAC;
            switch (telnet_cmd2)
            {
            case LINEMODE:
            case NAWS:
            case LFLOW:
            case NEW_ENVIRON:
            case XDISPLOC:
                txBuf[txLen++] = DONT;
                break;
            default:
                txBuf[txLen++] = DO;
                break;
            }
            txBuf[txLen++] = telnet_cmd2;
            bytesOut += tcpWriteBuf(tcpClient, txBuf, txLen);
            break;
        case SB:
            sb_option = telnet_cmd2;
            rx_state = RX_SB_DATA;
            return -1;
        }
        rx_state = RX_IDLE;
        return -1;

    case RX_SB_DATA:
        // Wait for SE (end of subnegotiation)
        rxByte = tcpReadByte(tcpClient);
        if (rxByte < 0)
            return -1;
        if (rxByte == IAC)
        {
            rx_state = RX_SB_WAIT_SE;
        }
        // else discard
        return -1;

    case RX_SB_WAIT_SE:
        rxByte = tcpReadByte(tcpClient);
        if (rxByte < 0)
            return -1;
        if (rxByte == SE) // discard rest of cmd
        {
            // Respond to TTYPE or TSPEED
            txLen = 0;
            txBuf[txLen++] = IAC;
            txBuf[txLen++] = SB;
            txBuf[txLen++] = sb_option;
            txBuf[txLen++] = VLSUP;
            switch (sb_option)
            {
            case TTYPE: // terminal type
                txLen += snprintf((char *)txBuf + txLen, (sizeof txBuf) - txLen, "%s", settings.terminal);
                break;
            case TSPEED: // terminal speed
                txLen += snprintf((char *)txBuf + txLen, (sizeof txBuf) - txLen, "%u,%u", DISPLAY_SPEED, DISPLAY_SPEED);
                break;
            }
            txBuf[txLen++] = IAC;
            txBuf[txLen++] = SE;
            bytesOut += tcpWriteBuf(tcpClient, txBuf, txLen);
            rx_state = RX_IDLE;
        }
        else
        {
            rx_state = RX_SB_DATA;
        }
        return -1;
    }
    rx_state = RX_IDLE;
    return -1;
}

//
// return a pointer to a string containing the connect time of the last session
//
char *connectTimeString(void)
{
    uint64_t now = millis();
    int hours, mins, secs;
    static char result[9];

    if (connectTime)
    {
        secs = (now - connectTime) / 1000;
        mins = secs / 60;
        hours = mins / 60;
        secs %= 60;
        mins %= 60;
    }
    else
    {
        hours = mins = secs = 0;
    }
    result[0] = (char)(hours / 10 + '0');
    result[1] = (char)(hours % 10 + '0');
    result[2] = ':';
    result[3] = (char)(mins / 10 + '0');
    result[4] = (char)(mins % 10 + '0');
    result[5] = ':';
    result[6] = (char)(secs / 10 + '0');
    result[7] = (char)(secs % 10 + '0');
    result[8] = NUL;
    return result;
}

//
// print a result code/string to the serial port
//
void sendResult(int resultCode)
{
    if (!settings.quiet)
    {           // quiet mode on?
        crlf(); // no, we're going to display something
        if (!settings.verbose)
        {
            if (resultCode == R_RING_IP)
            {
                resultCode = R_RING;
            }
            printf("%d\r\n", resultCode); // not verbose, just print the code #
        }
        else
        {
            switch (resultCode)
            {               // possible extra info for CONNECT and
                            // NO CARRIER if extended codes are
            case R_CONNECT: // enabled
                ser_puts(ser0, connectStr);
                if (settings.extendedCodes)
                {
                    printf(" %u", DISPLAY_SPEED);
                }
                break;

            case R_NO_CARRIER:
                ser_puts(ser0, noCarrierStr);
                if (settings.extendedCodes)
                {
                    printf(" (%s)", connectTimeString());
                }
                break;

            case R_ERROR:
                ser_puts(ser0, errorStr);
                lastCmd[0] = NUL;
                memset(atCmd, 0, sizeof atCmd);
                break;

            case R_RING_IP:
                ser_puts(ser0, ringStr);
                if (settings.extendedCodes)
                {
                    printf(" %s", ip4addr_ntoa(&tcpClient->pcb->remote_ip));
                }
                break;

            default:
                ser_puts(ser0, resultCodes[resultCode]);
                break;
            }
            crlf();
        }
    }
    else if (resultCode == R_ERROR)
    {
        lastCmd[0] = NUL;
        memset(atCmd, 0, sizeof atCmd);
    }
    if (resultCode == R_NO_CARRIER || resultCode == R_NO_ANSWER)
    {
        sessionTelnetType = settings.telnet;
    }
}

// trim leading and trailing blanks from a string
void trim(char *str)
{
    char *trimmed = str;
    // find first non blank character
    while (*trimmed && isspace((unsigned char)*trimmed))
    {
        ++trimmed;
    }
    if (*trimmed)
    {
        // trim off any trailing blanks
        for (int i = strlen(trimmed) - 1; i >= 0; --i)
        {
            if (isspace((unsigned char)trimmed[i]))
            {
                trimmed[i] = NUL;
            }
            else
            {
                break;
            }
        }
    }
    // shift string only if we had leading blanks
    if (str != trimmed)
    {
        int i, len = strlen(trimmed);
        for (i = 0; i < len; ++i)
        {
            str[i] = trimmed[i];
        }
        str[i] = NUL;
    }
}

//
// Parse a string in the form "hostname[:port]" and return
//
// 1. A pointer to the hostname
// 2. A pointer to the optional port
// 3. The numeric value of the port (if not specified, 23)
//
void getHostAndPort(char *number, char **host, char **port, int *portNum)
{
    char *ptr;

    *port = strrchr(number, ':');
    if (!port)
    {
        *portNum = TELNET_PORT;
    }
    else
    {
        *port++ = NUL;
        *portNum = atoi(*port);
    }
    *host = number;
    while (**host && isspace((unsigned char)**host))
    {
        ++(*host);
    }
    ptr = *host;
    while (*ptr && !isspace((unsigned char)*ptr))
    {
        ++ptr;
    }
    *ptr = NUL;
}

//
// Paged text output: using the terminal rows defined in
// settings.height, these routines pause the output when
// a screen full of text has been shown.
//
// Call with PagedOut("text", true); to initialise the
// line counter.
//

bool PagedOut(const char *str, bool reset) // reset = false
{
    static uint8_t numLines = 0;

    char c = ' ';

    if (reset)
    {
        numLines = 0;
    }
    if (numLines >= settings.height - 1)
    {
        printf("[More]");
        while (!ser_is_readable(ser0))
            ;
        c = ser_getc(ser0);
        printf("\r      \r");
        numLines = 0;
    }
    if (c != CTLC)
    {
        printf("%s\r\n", str);
        ++numLines;
    }
    return c == CTLC;
}
