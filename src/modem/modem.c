#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "modem/ser_cdc.h"
#include "modem/commands.h"
#include "modem/support.h"
#include "modem/tcp_support.h"

typedef enum MdmState
{
    CMD_NOT_IN_CALL,
    CMD_IN_CALL,
    ONLINE,
    PASSWORD
} MdmStates;

MdmStates state = CMD_NOT_IN_CALL;

//
// terminate an active call
//
void endCall(void)
{
    state = CMD_NOT_IN_CALL;
    tcpClientClose(tcpClient);
    tcpClient = NULL;
    sendResult(R_NO_CARRIER);
    ser_set(DCD, !ACTIVE);
    connectTime = 0;
    escCount = 0;
}

void setStateOnline(void)
{
    state = ONLINE;
}

//
// Check for an incoming TCP session. There are 3 scenarios:
//
// 1. We're already in a call, or auto answer is disabled and the
//    ring count exceeds the limit: tell the caller we're busy.
// 2. We're not in a call and auto answer is disabled, or the #
//    of rings is less than the auto answer count: either start
//    or continue ringing.
// 3. We're no in a call, auto answer is enabled and the # of rings
//    is at least the auto answer count: answer the call.
//
static void checkForIncomingCall()
{
    if (settings.listenPort && serverHasClient(&tcpServer))
    {
        if (state != CMD_NOT_IN_CALL || (!settings.autoAnswer && ringCount > MAGIC_ANSWER_RINGS))
        {
            ser_set(RI, !ACTIVE);
            TCP_CLIENT_T *droppedClient = serverGetClient(&tcpServer, &tcpDroppedClient);
            if (settings.busyMsg[0])
            {
                tcpWriteStr(droppedClient, settings.busyMsg);
                tcpWriteStr(droppedClient, "\r\nCurrent call length: ");
                tcpWriteStr(droppedClient, connectTimeString());
            }
            else
            {
                tcpWriteStr(droppedClient, "BUSY");
            }
            tcpWriteStr(droppedClient, "\r\n\r\n");
            tcpTxFlush(droppedClient);
            tcpClientClose(droppedClient);
            ringCount = 0;
            ringing = false;
        }
        else if (!settings.autoAnswer || ringCount < settings.autoAnswer)
        {
            if (!ringing)
            {
                ringing = true; // start ringing
                ringCount = 1;
                ser_set(RI, ACTIVE);
                if (!settings.autoAnswer || ringCount < settings.autoAnswer)
                {
                    sendResult(R_RING); // only show RING if we're not just
                } // about to answer
                nextRingMs = millis() + RING_INTERVAL;
            }
            else if (millis() > nextRingMs)
            {
                if (ser_get(RI) == ACTIVE)
                {
                    ser_set(RI, !ACTIVE);
                }
                else
                {
                    ++ringCount;
                    ser_set(RI, ACTIVE);
                    if (!settings.autoAnswer || ringCount < settings.autoAnswer)
                    {
                        sendResult(R_RING);
                    }
                }
                nextRingMs = millis() + RING_INTERVAL;
            }
        }
        else if (settings.autoAnswer && ringCount >= settings.autoAnswer)
        {
            ser_set(RI, !ACTIVE);
            tcpClient = serverGetClient(&tcpServer, &tcpClient0);
            if (settings.telnet != NO_TELNET)
            {
                // send incantation to switch from line mode to character mode
                bytesOut += tcpWriteCharModeMagic(tcpClient);
            }
            sendResult(R_RING_IP);
            ser_set(DCD, ACTIVE);
            if (settings.serverPassword[0])
            {
                tcpWriteStr(tcpClient, "\r\r\nPassword: ");
                state = PASSWORD;
                passwordTries = 0;
                passwordLen = 0;
                password[0] = NUL;
            }
            else
            {
                sleep_ms(1000); ////////////////// TODO
                state = ONLINE;
                amClient = false;
                dtrWentInactive = false;
                sendResult(R_CONNECT);
            }
            connectTime = millis();
        }
    }
    else if (ringing)
    {
        ser_set(RI, !ACTIVE);
        ringing = false;
        ringCount = 0;
    }
}

//
// Password is set for incoming connections.
// Allow 3 tries or 60 seconds before hanging up.
//
static void inPasswordMode()
{
    if (tcpBytesAvailable(tcpClient))
    {
        int c = receiveTcpData();
        switch (c)
        {
        case -1: // telnet control sequence: no data returned
            break;

        case LF:
        case CR:
            tcpWriteStr(tcpClient, "\r\n");
            if (strcmp(settings.serverPassword, password))
            {
                ++passwordTries;
                password[0] = NUL;
                passwordLen = 0;
                tcpWriteStr(tcpClient, "\r\nPassword: ");
            }
            else
            {
                state = ONLINE;
                amClient = false;
                ;
                dtrWentInactive = false;
                sendResult(R_CONNECT);
                tcpWriteStr(tcpClient, "Welcome\r\n");
            }
            break;

        case BS:
        case DEL:
            if (passwordLen)
            {
                password[--passwordLen] = NUL;
                tcpWriteStr(tcpClient, "\b \b");
            }
            break;

        default:
            if (isprint((unsigned char)c) && passwordLen < MAX_PWD_LEN)
            {
                tcpWriteByte(tcpClient, '*');
                password[passwordLen++] = (char)c;
                password[passwordLen] = 0;
            }
            break;
        }
    }
    if (millis() - connectTime > PASSWORD_TIME || passwordTries >= PASSWORD_TRIES)
    {
        tcpWriteStr(tcpClient, "Good-bye\r\n");
        endCall();
    }
    else if (!tcpIsConnected(tcpClient))
    {              // no client?
        endCall(); // then hang up
    }
}

// return and reset DTR change indicator
static bool checkDtrIrq(void)
{
    bool ret = dtrWentInactive;
    dtrWentInactive = false;
    return ret;
}

//
// We're in local command mode. Assemble characters from the
// serial port into a buffer for processing.
//
static void inAtCommandMode()
{
    char c;

    // get AT command
    if (ser_is_readable(ser0))
    {
        c = ser_getc(ser0);

        if (c == LF || c == CR)
        { // command finished?
            if (settings.echo)
            {
                crlf();
            }
            doAtCmds(atCmd); // yes, then process it
            atCmd[0] = NUL;
            atCmdLen = 0;
        }
        else if ((c == BS || c == DEL) && atCmdLen > 0)
        {
            atCmd[--atCmdLen] = NUL; // remove last character
            if (settings.echo)
            {
                printf("\b \b");
            }
        }
        else if (c == '/' && atCmdLen == 1 && toupper(atCmd[0]) == 'A' && lastCmd[0] != NUL)
        {
            if (settings.echo)
            {
                printf("/\r\n");
            }
            strncpy(atCmd, lastCmd, sizeof atCmd);
            atCmd[MAX_CMD_LEN] = NUL;
            doAtCmds(atCmd); // repeat last command
            atCmd[0] = NUL;
            atCmdLen = 0;
        }
        else if (c >= ' ' && c <= '~')
        { // printable char?
            if (atCmdLen < MAX_CMD_LEN)
            {
                atCmd[atCmdLen++] = c; // add to command string
                atCmd[atCmdLen] = NUL;
            }
            if (settings.echo)
            {
                ser_putc(ser0, c);
            }
        }
    }
}

void setup(void)
{
    loadNvramSettings(&settings);
    sessionTelnetType = settings.telnet;

    // enable interrupt when DTR goes inactive if we're not ignoring it
    // gpio_set_irq_enabled_with_callback(DTR, GPIO_IRQ_EDGE_RISE, settings.dtrHandling != DTR_IGNORE, dtrIrq);

    if (settings.startupWait)
    {
        while (true)
        { // wait for a CR
            if (ser_is_readable(ser0))
            {
                if (ser_getc(ser0) == CR)
                {
                    break;
                }
            }
        }
    }

    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();
    // disable Wifi power management
    // if this is not done, after 5-10 minutes the Pico W does not
    // respond to incoming packets and can only be awoken by the
    // arrival of serial data
    cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf);
    if (settings.ssid[0])
    {
        for (int i = 0; i < 4; ++i)
        {
            cyw43_arch_wifi_connect_timeout_ms(settings.ssid, settings.wifiPassword, CYW43_AUTH_WPA2_AES_PSK, 10000);
            if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
            {
                break;
            }
        }
    }

    if (settings.listenPort)
    {
        tcpServerStart(&tcpServer, settings.listenPort);
    }

    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP || !settings.ssid[0])
    {
        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
        {
            ser_set(DSR, ACTIVE); // modem is finally ready or SSID not configured
            dns_init();
        }
        if (settings.autoExecute[0])
        {
            strncpy(atCmd, settings.autoExecute, MAX_CMD_LEN);
            atCmd[MAX_CMD_LEN] = NUL;
            if (settings.echo)
            {
                printf("%s\r\n", atCmd);
            }
            doAtCmds(atCmd); // auto execute command
        }
        else
        {
            sendResult(R_OK);
        }
    }
    else
    {
        sendResult(R_ERROR); // SSID configured, but not connected
    }
}

// =============================================================
void modem_run(void)
{

    // tud_task();
    // cdc_task();

    checkForIncomingCall();

    if (settings.dtrHandling == DTR_RESET && checkDtrIrq())
    {
        loadNvramSettings(&settings);
    }

    switch (state)
    {

    case CMD_NOT_IN_CALL:
        inAtCommandMode();
        break;

    case CMD_IN_CALL:
        inAtCommandMode();
        if (state == CMD_IN_CALL && !tcpIsConnected(tcpClient))
        {
            endCall(); // hang up if not in a call
        }
        break;

    case PASSWORD:
        inPasswordMode();
        break;

    case ONLINE:
        if (ser_is_readable(ser0))
        { // data from RS-232 to Wifi
            sendSerialData();
        }

        while (tcpBytesAvailable(tcpClient) && !ser_is_readable(ser0))
        {
            // data from WiFi to RS-232
            int c = receiveTcpData();
            if (c != -1)
            {
                ser_putc_raw(ser0, (char)c);
            }
        }

        if (escCount == ESC_COUNT && millis() > guardTime)
        {
            state = CMD_IN_CALL; // +++ detected, back to command mode
            sendResult(R_OK);
            escCount = 0;
        }

        if (settings.dtrHandling != DTR_IGNORE && checkDtrIrq())
        {
            switch (settings.dtrHandling)
            {

            case DTR_GOTO_COMMAND:
                state = CMD_IN_CALL;
                sendResult(R_OK);
                escCount = 0;
                break;

            case DTR_END_CALL:
                endCall();
                break;

            case DTR_RESET:
                loadNvramSettings(&settings);
                break;

            case DTR_IGNORE:
                break;
            }
        }

        if (!tcpIsConnected(tcpClient))
        {              // no client?
            endCall(); // then hang up
        }
        break;
    }
}
