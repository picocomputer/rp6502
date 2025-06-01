/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico.h>

#ifdef RP6502_RIA_W

#include "pico/cyw43_arch.h"
#include <string.h>
#include "modem/commands.h"
#include "ser_cdc.h"
#include "tcp_support.h"
#include "support.h"

//
// ATA manually answer an incoming call
//
static char *answerCall(char *atCmd)
{
    tcpClient = serverGetClient(&tcpServer, &tcpClient0);
    ser_set(RI, !ACTIVE); // we've picked up so ringing stops
    ringing = false;
    ringCount = 0;
    if (settings.telnet != NO_TELNET)
    {
        // send incantation to switch from line mode to character mode
        bytesOut += tcpWriteCharModeMagic(tcpClient);
    }
    sendResult(R_RING_IP);
    sleep_ms(1000); // TODO
    connectTime = millis();
    dtrWentInactive = false;
    sendResult(R_CONNECT);
    ser_set(DCD, ACTIVE); // we've got a carrier signal
    amClient = false;
    setStateOnline();
    ser_tx_wait_blocking(ser0); // drain the UART's Tx FIFO
    return atCmd;
}

//
// ATC? query WiFi connection status
// ATC0 disconnect from WiFi network
// ATC1 connect to WiFi network
//
static char *wifiConnection(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%c\r\n", cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP ? '1' : '0');
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '0':
        ++atCmd;
        __attribute__((fallthrough));
    case NUL:
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        ser_set(DSR, !ACTIVE); // modem is not ready
        break;
    case '1':
        ++atCmd;
        if (settings.ssid[0] && settings.wifiPassword[0])
        {
            if (!settings.quiet && settings.extendedCodes)
            {
                printf("CONNECTING TO SSID %s", settings.ssid);
            }
            cyw43_arch_wifi_connect_async(settings.ssid, settings.wifiPassword, CYW43_AUTH_WPA2_AES_PSK);
            for (int i = 0; i < 50; ++i)
            {
                sleep_ms(500);
                if (!settings.quiet && settings.extendedCodes)
                {
                    ser_putc(ser0, '.');
                }
                if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
                {
                    break;
                }
            }
            if (!settings.quiet && settings.extendedCodes)
            {
                crlf();
            }
            if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP)
            {
                sendResult(R_ERROR);
            }
            else
            {
                ser_set(DSR, ACTIVE); // modem is ready
                dns_init();
                if (!settings.quiet && settings.extendedCodes)
                {
                    printf("CONNECTED TO %s IP ADDRESS: %s\r\n",
                           settings.ssid, ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[0])));
                }
                if (!atCmd[0])
                {
                    sendResult(R_OK);
                }
            }
        }
        else
        {
            if (!settings.quiet && settings.extendedCodes)
            {
                printf("Configure SSID and password. Type AT? for help.\r\n");
            }
            sendResult(R_ERROR);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// ATDThost[:port] dial a number
//
static char *dialNumber(char *atCmd)
{
    char *host, *port;
    char tempNumber[MAX_SPEED_DIAL_LEN + 1];
    int portNum;
    // ip_addr_t remoteAddr;

    getHostAndPort(atCmd, &host, &port, &portNum);
    if (!port)
    {
        // check for host names that are 7 identical digits long
        // and dial from the corresponding speed dial slot
        bool speedDial = true;
        if (strlen(host) == MAGIC_SPEED_LEN && isdigit((unsigned char)host[0]))
        {
            for (int i = 0; i < MAGIC_SPEED_LEN; ++i)
            {
                if (host[0] != host[i])
                {
                    speedDial = false;
                    break;
                }
            }
        }
        else
        {
            speedDial = false;
        }
        if (speedDial && settings.speedDial[host[0] - '0'][0])
        {
            strncpy(tempNumber, settings.speedDial[host[0] - '0'], MAX_SPEED_DIAL_LEN);
            tempNumber[MAX_SPEED_DIAL_LEN] = NUL;
            getHostAndPort(tempNumber, &host, &port, &portNum);
        }
        else
        {
            // now check all the speed dial aliases for a match
            for (int i = 0; i < SPEED_DIAL_SLOTS; ++i)
            {
                if (!strcasecmp(host, settings.alias[i]))
                {
                    strncpy(tempNumber, settings.speedDial[i], MAX_SPEED_DIAL_LEN);
                    tempNumber[MAX_SPEED_DIAL_LEN] = NUL;
                    getHostAndPort(tempNumber, &host, &port, &portNum);
                    break;
                }
            }
        }
    }

    sessionTelnetType = settings.telnet;
    switch (host[0])
    {
    case '-':
        ++host;
        sessionTelnetType = NO_TELNET;
        break;
    case '=':
        ++host;
        sessionTelnetType = REAL_TELNET;
        break;
    case '+':
        ++host;
        sessionTelnetType = FAKE_TELNET;
        break;
    }

    if (!settings.quiet && settings.extendedCodes)
    {
        printf("DIALING %s:%u\r\n", host, portNum);
        ser_tx_wait_blocking(ser0);
    }
    sleep_ms(2000); // TODO delay for ZMP to be able to detect CONNECT
    if (!ser_is_readable(ser0))
    {
        tcpClient = tcpConnect(&tcpClient0, host, portNum);
        if (tcpClient)
        {
            connectTime = millis();
            dtrWentInactive = false;
            sendResult(R_CONNECT);
            ser_set(DCD, ACTIVE);
            setStateOnline();
            amClient = true;
        }
        else
        {
            sendResult(R_NO_CARRIER);
            ser_set(DCD, !ACTIVE);
        }
    }
    else
    {
        sendResult(R_NO_CARRIER);
        ser_set(DCD, !ACTIVE);
    }
    atCmd[0] = NUL;
    return atCmd;
}

//
// ATDSn speed dial a number
//
static char *speedDialNumber(char *atCmd)
{
    char number[MAX_SPEED_DIAL_LEN + 1];
    char slot = atCmd[0];

    if (isdigit(slot) && settings.speedDial[slot - '0'][0])
    {
        ++atCmd;
        strncpy(number, settings.speedDial[slot - '0'], MAX_SPEED_DIAL_LEN);
        number[MAX_SPEED_DIAL_LEN] = NUL;
        dialNumber(number);
    }
    else
    {
        sendResult(R_ERROR);
    }
    return atCmd;
}

//
// ATE? query command mode echo status
// ATE0 disable command mode local echo
// ATE1 enable command mode local echo
//
static char *doEcho(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.echo);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '0':
    case '1':
    case NUL:
        settings.echo = atCmd[0] == '1';
        if (atCmd[0])
        {
            ++atCmd;
        }
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// ATGEThttp://host[/path]: fetch a web page
//
// NOTE: http only: no https support
//
static char *httpGet(char *atCmd)
{
    char *host, *path, *port;
    int portNum;
    port = strrchr(atCmd, ':');
    host = strstr(atCmd, "http://");
    if (!port || port == host + 4)
    {
        portNum = HTTP_PORT;
    }
    else
    {
        portNum = atoi(port + 1);
        *port = NUL;
    }
    if (!host)
    {
        sendResult(R_ERROR);
        return atCmd;
    }
    else
    {
        host += 7; // skip over http://
        path = strchr(host, '/');
    }
    if (path)
    {
        *path = NUL;
        ++path;
    }
#if DEBUG
    printf("Getting path /");
    if (path)
    {
        printf("%s", path);
    }
    printf(" from port %u of host %s...\r\n", portNum, host);
#endif
    // Establish connection
    tcpClient = tcpConnect(&tcpClient0, host, portNum);
    if (!tcpClient)
    {
        sendResult(R_NO_CARRIER);
        ser_set(DCD, !ACTIVE);
    }
    else
    {
        connectTime = millis();
        dtrWentInactive = false;
        sendResult(R_CONNECT);
        ser_set(DCD, ACTIVE);
        amClient = true;
        setStateOnline();

        // Send a HTTP request before continuing the connection as usual
        bytesOut += tcpWriteStr(tcpClient, "GET /");
        if (path)
        {
            bytesOut += tcpWriteStr(tcpClient, path);
        }
        bytesOut += tcpWriteStr(tcpClient, " HTTP/1.1\r\nHost: ");
        bytesOut += tcpWriteStr(tcpClient, host);
        bytesOut += tcpWriteStr(tcpClient, "\r\nConnection: close\r\n\r\n");
    }
    atCmd[0] = NUL;
    return atCmd;
}

//
// ATH go offline (if connected to a host)
//
static char *hangup(char *atCmd)
{
    if (tcpIsConnected(tcpClient))
    {
        endCall();
    }
    else
    {
        sendResult(R_OK);
    }
    return atCmd;
}

//
// AT? help: paged output, uses dual columns if defined screen width
// is at least 80 characters, single columns if less.
//
// There must be an even number of help strings defined. If you end up
// with an odd number of strings, add an empty string ("") at the end
// to pad things out.
//
static const char __in_flash("modem") helpStr01[] = "Help..........: AT?";
static const char __in_flash("modem") helpStr02[] = "Repeat command: A/";
static const char __in_flash("modem") helpStr03[] = "Answer call...: ATA";
static const char __in_flash("modem") helpStr04[] = "WiFi connect..: ATCn";
static const char __in_flash("modem") helpStr05[] = "Speed dial....: ATDSn";
static const char __in_flash("modem") helpStr06[] = "Dial host.....: ATDThost[:port]";
static const char __in_flash("modem") helpStr07[] = "Command echo..: ATEn";
static const char __in_flash("modem") helpStr08[] = "HTTP get......: ATGEThttp://host[/page]";
static const char __in_flash("modem") helpStr09[] = "Hang up.......: ATH";
static const char __in_flash("modem") helpStr10[] = "Network info..: ATI";
static const char __in_flash("modem") helpStr11[] = "Handle Telnet.: ATNETn";
static const char __in_flash("modem") helpStr12[] = "Leave cmd mode: ATO";
static const char __in_flash("modem") helpStr13[] = "Quiet mode....: ATQn";
static const char __in_flash("modem") helpStr14[] = "NIST date.time: ATRD/ATRT";
static const char __in_flash("modem") helpStr15[] = "Auto answer...: ATS0=n";
static const char __in_flash("modem") helpStr16[] = "Verbose mode..: ATVn";
static const char __in_flash("modem") helpStr17[] = "Extended codes: ATXn";
static const char __in_flash("modem") helpStr18[] = "Modem reset...: ATZ";
static const char __in_flash("modem") helpStr19[] = "DTR handling..: AT&D";
static const char __in_flash("modem") helpStr20[] = "Fact. defaults: AT&F";
static const char __in_flash("modem") helpStr21[] = "Flow control..: AT&Kn";
static const char __in_flash("modem") helpStr22[] = "Server passwd.: AT&R=server password";
static const char __in_flash("modem") helpStr23[] = "Show settings.: AT&Vn";
static const char __in_flash("modem") helpStr24[] = "Update NVRAM..: AT&W";
static const char __in_flash("modem") helpStr25[] = "Set speed dial: AT&Zn=host[:port],alias";
static const char __in_flash("modem") helpStr26[] = "Auto execute..: AT$AE=AT command";
static const char __in_flash("modem") helpStr27[] = "Are You There?: AT$AYT";
static const char __in_flash("modem") helpStr28[] = "Busy message..: AT$BM=busy message";
static const char __in_flash("modem") helpStr29[] = "mDNS name.....: AT$MDNS=mDNS name";
static const char __in_flash("modem") helpStr30[] = "WiFi password.: AT$PASS=WiFi password";
static const char __in_flash("modem") helpStr31[] = "Serial speed..: AT$SB=n";
static const char __in_flash("modem") helpStr32[] = "Server port...: AT$SP=n";
static const char __in_flash("modem") helpStr33[] = "WiFi SSID.....: AT$SSID=ssid";
static const char __in_flash("modem") helpStr34[] = "Data config...: AT$SU=dps";
static const char __in_flash("modem") helpStr35[] = "Location......: AT$TTL=telnet location";
static const char __in_flash("modem") helpStr36[] = "Terminal size.: AT$TTS=WxH";
static const char __in_flash("modem") helpStr37[] = "Terminal type.: AT$TTY=terminal type";
static const char __in_flash("modem") helpStr38[] = "Startup wait..: AT$W=n";
static const char __in_flash("modem") helpStr39[] = "Query most commands followed by '?'";
static const char __in_flash("modem") helpStr40[] = "e.g. ATQ?, AT&K?, AT$SSID?";

static const char *const __in_flash("modem") helpStrs[] = {
    helpStr01, helpStr02, helpStr03, helpStr04, helpStr05, helpStr06,
    helpStr07, helpStr08, helpStr09, helpStr10, helpStr11, helpStr12,
    helpStr13, helpStr14, helpStr15, helpStr16, helpStr17, helpStr18,
    helpStr19, helpStr20, helpStr21, helpStr22, helpStr23, helpStr24,
    helpStr25, helpStr26, helpStr27, helpStr28, helpStr29, helpStr30,
    helpStr31, helpStr32, helpStr33, helpStr34, helpStr35, helpStr36,
    helpStr37, helpStr38, helpStr39, helpStr40};
#define NUM_HELP_STRS (sizeof(helpStrs) / sizeof(helpStrs[0]))

static char *showHelp(char *atCmd)
{
    char helpLine[80];

    PagedOut("AT Command Summary:", true);
    if (settings.width >= 80)
    {
        // dual columns
        for (unsigned int i = 0; i < NUM_HELP_STRS / 2; ++i)
        {
            snprintf(
                helpLine,
                sizeof helpLine,
                "%-40s%s",
                helpStrs[i],
                helpStrs[i + NUM_HELP_STRS / 2]);
            if (PagedOut(helpLine, false))
            {
                break; // user responded with ^C, quit
            }
        }
    }
    else
    {
        // single column
        for (unsigned int i = 0; i < NUM_HELP_STRS; ++i)
        {
            if (PagedOut(helpStrs[i], false))
            {
                break; // user responded with ^C, quit
            }
        }
    }
    if (!atCmd[0])
    {
        sendResult(R_OK);
    }
    return atCmd;
}

//
// ATI: show network info
//
static char *showNetworkInfo(char *atCmd)
{
    char infoLine[100];
    size_t maxCatChars;
    uint8_t mac[6];

    int wifiStatus = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

    do
    { // a Q&D hack to allow ^C to terminate the output at the
      // end of a page
        if (PagedOut("Pico WiFi modem", true))
            break;
        if (PagedOut("Build......: " __DATE__ " " __TIME__, false))
            break;
        strncpy(infoLine, "WiFi status: ", (sizeof infoLine) - 1);
        infoLine[(sizeof infoLine) - 1] = 0;
        maxCatChars = (sizeof infoLine) - strlen(infoLine);
        switch (wifiStatus)
        {
        case CYW43_LINK_DOWN:
            strncat(infoLine, "LINK IS DOWN", maxCatChars);
            break;
        case CYW43_LINK_JOIN:
            strncat(infoLine, "CONNECTED TO WIFI", maxCatChars);
            break;
        case CYW43_LINK_NOIP:
            strncat(infoLine, "CONNECTED TO WIFI BUT NO IP ADDRESS", maxCatChars);
            break;
        case CYW43_LINK_UP:
            strncat(infoLine, "CONNECTED", maxCatChars);
            break;
        case CYW43_LINK_FAIL:
            strncat(infoLine, "CONNECT FAILED", maxCatChars);
            break;
        case CYW43_LINK_NONET:
            strncat(infoLine, "SSID UNAVAILABLE", maxCatChars);
            break;
        case CYW43_LINK_BADAUTH:
            strncat(infoLine, "BAD AUTHORIZATION", maxCatChars);
            break;
        default:
            snprintf(infoLine, sizeof infoLine, "WiFi status: UNKNOWN (%u)", wifiStatus);
            break;
        }
        infoLine[(sizeof infoLine) - 1] = 0;
        if (PagedOut(infoLine, false))
            break;
        snprintf(infoLine, sizeof infoLine, "SSID.......: %s", settings.ssid);
        if (PagedOut(infoLine, false))
            break;
        if (wifiStatus == CYW43_LINK_JOIN)
        {
            int32_t rssi;
            cyw43_ioctl(&cyw43_state, 254, sizeof rssi, (uint8_t *)&rssi, CYW43_ITF_STA);
            snprintf(infoLine, sizeof infoLine, "RSSI.......: %ld dBm", rssi);
            if (PagedOut(infoLine, false))
                break;
        }
        if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) != ERR_OK)
        {
            if (PagedOut("MAC address: ?", false))
                break;
        }
        else
        {
            snprintf(infoLine, sizeof infoLine,
                     "MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            if (PagedOut(infoLine, false))
                break;
        }
        if (wifiStatus == CYW43_LINK_JOIN)
        {
            snprintf(infoLine, sizeof infoLine, "IP address.: %s", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[0])));
            if (PagedOut(infoLine, false))
                break;
            snprintf(infoLine, sizeof infoLine, "Gateway....: %s", ip4addr_ntoa(netif_ip4_gw(&cyw43_state.netif[0])));
            if (PagedOut(infoLine, false))
                break;
            snprintf(infoLine, sizeof infoLine, "Subnet mask: %s", ip4addr_ntoa(netif_ip4_netmask(&cyw43_state.netif[0])));
            if (PagedOut(infoLine, false))
                break;
        }
        snprintf(infoLine, sizeof infoLine, "mDNS name..: %s.local", settings.mdnsName);
        if (PagedOut(infoLine, false))
            break;
        snprintf(infoLine, sizeof infoLine, "Server port: %u", settings.listenPort);
        if (PagedOut(infoLine, false))
            break;
        snprintf(infoLine, sizeof infoLine, "Bytes in...: %lu", bytesIn);
        if (PagedOut(infoLine, false))
            break;
        snprintf(infoLine, sizeof infoLine, "Bytes out..: %lu", bytesOut);
        if (PagedOut(infoLine, false))
            break;
#ifndef NDEBUG
        snprintf(infoLine, sizeof infoLine, "Max totLen.: %u", maxTotLen);
        if (PagedOut(infoLine, false))
            break;
        snprintf(infoLine, sizeof infoLine, "Max rxBuff.: %u", maxRxBuffLen);
        if (PagedOut(infoLine, false))
            break;
        snprintf(infoLine, sizeof infoLine, "Max txBuff.: %u", maxTxBuffLen);
        if (PagedOut(infoLine, false))
            break;
#endif
        if (tcpIsConnected(tcpClient))
        {
            snprintf(infoLine, sizeof infoLine, "Call status: CONNECTED TO %s", ip4addr_ntoa(&tcpClient->pcb->remote_ip));
            if (PagedOut(infoLine, false))
                break;
            snprintf(infoLine, sizeof infoLine, "Call length: %s", connectTimeString());
            if (PagedOut(infoLine, false))
                break;
        }
        else
        {
            if (PagedOut("Call status: NOT CONNECTED", false))
                break;
        }
    } while (false);
    if (!atCmd[0])
    {
        sendResult(R_OK);
    }
    return atCmd;
}

//
// ATNET? query Telnet handling status
// ATNET0 turn off Telnet handling
// ATNET1 turn on true Telnet handling
// ATNET2 turn on BBS (fake) Telnet handling
//
static char *doTelnetMode(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.telnet);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case NUL:
    case '0': // no Telnet processing
    case '1': // real Telnet (double IACs, add NUL after CR)
    case '2': // fake (BBS) Telnet (double IACs)
        if (atCmd[0])
        {
            settings.telnet = atCmd[0] - '0';
            ++atCmd;
        }
        else
        {
            settings.telnet = NO_TELNET;
        }
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// ATO go online (if connected to a host)
//
static char *goOnline(char *atCmd)
{
    if (tcpIsConnected(tcpClient))
    {
        setStateOnline();
        dtrWentInactive = false;
        sendResult(R_CONNECT);
    }
    else
    {
        sendResult(R_ERROR);
    }
    return atCmd;
}

//
// ATQ? query quiet mode status
// ATQ0 disable quiet mode (results issued)
// ATQ1 enable quiet mode (no results issued)
//
static char *doQuiet(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.quiet);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '0':
    case '1':
    case NUL:
        settings.quiet = atCmd[0] == '1';
        if (atCmd[0])
        {
            ++atCmd;
        }
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// ATS0?  query auto answer configuration
// ATS0=0 disable auto answer
// ATS0=n enable auto answer after n rings
//
static char *doAutoAnswerConfig(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.autoAnswer);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        if (isdigit((unsigned char)atCmd[0]))
        {
            settings.autoAnswer = atoi(atCmd);
            while (isdigit((unsigned char)atCmd[0]))
            {
                ++atCmd;
            }
            if (!atCmd[0])
            {
                sendResult(R_OK);
            }
        }
        else
        {
            sendResult(R_ERROR);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// ATS2?  query escape character
// ATS2=[128..255] disable escape character
// ATS2=[0..127] set and enable escape character
//
static char *doEscapeCharConfig(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.escChar);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        if (isdigit((unsigned char)atCmd[0]))
        {
            settings.escChar = atoi(atCmd);
            while (isdigit((unsigned char)atCmd[0]))
            {
                ++atCmd;
            }
            if (!atCmd[0])
            {
                sendResult(R_OK);
            }
        }
        else
        {
            sendResult(R_ERROR);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// ATV? query verbose mode status
// ATV0 disable verbose mode (results are shown as numbers)
// ATV1 enable verbose nmode (results are shown as text)
//
static char *doVerbose(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.verbose);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '0':
    case '1':
    case NUL:
        settings.verbose = atCmd[0] == '1';
        if (atCmd[0])
        {
            ++atCmd;
        }
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// ATX? query extended results
// ATX0 disable extended results
// ATX1 enable extended results
//
static char *doExtended(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.extendedCodes);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '0':
    case '1':
    case NUL:
        settings.extendedCodes = atCmd[0] == '1';
        if (atCmd[0])
        {
            ++atCmd;
        }
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// ATZ load settings with saved NVRAM
//
static char *doResetToNvram(char *atCmd)
{
    loadNvramSettings(&settings);
    if (!atCmd[0])
    {
        sendResult(R_OK);
    }
    return atCmd;
}

//
// AT&F reset NVRAM to defaults and load them into current settings
//
static char *doFactoryDefaults(char *atCmd)
{
    loadDefaultSettings(&settings);
    if (!writeSettings(&settings))
        sendResult(R_ERROR);
    else if (!atCmd[0])
        sendResult(R_OK);
    return atCmd;
}

//
// AT&D? DTR handling setting
// AT&D0 ignore DTR
// AT&D1 go offline when DTR transitions to off
// AT&D2 hang up when DTR transitions to off
// AT&D3 reset when DTR transitions to off
//
static char *doDtrHandling(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", (uint8_t)settings.dtrHandling);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case NUL:
    case '0':
    case '1':
    case '2':
    case '3':
        switch (atCmd[0])
        {
        case NUL:
        case '0':
            settings.dtrHandling = DTR_IGNORE;
            break;
        case '1':
            settings.dtrHandling = DTR_GOTO_COMMAND;
            break;
        case '2':
            settings.dtrHandling = DTR_END_CALL;
            break;
        case '3':
            settings.dtrHandling = DTR_RESET;
            break;
        }
        gpio_set_irq_enabled(DTR, GPIO_IRQ_EDGE_RISE, settings.dtrHandling != DTR_IGNORE);
        if (atCmd[0])
        {
            ++atCmd;
        }
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT&R? query incoming password
// AT&R=set incoming password
//
static char *doServerPassword(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%s\r\n", settings.serverPassword);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        strncpy(settings.serverPassword, atCmd, MAX_PWD_LEN);
        settings.serverPassword[MAX_PWD_LEN] = NUL;
        atCmd[0] = NUL;
        sendResult(R_OK);
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT&V  display current settings
// AT&V0 display current settings
// AT&V1 display NVRAM settings
//
static char *displayAllSettings(char *atCmd)
{
    SETTINGS_T temp_storage;
    SETTINGS_T *s = NULL;

    switch (atCmd[0])
    {
    case '0':
        ++atCmd;
        __attribute__((fallthrough));
    case NUL:
        s = &settings;
        break;
    case '1':
        ++atCmd;
        s = &temp_storage;
        readSettings(s);
        break;
    default:
        break;
    }

    if (!s)
        sendResult(R_ERROR);
    else
    {
        printf("Stored Profile:\r\n");
        printf("SSID.......: %s\r\n", s->ssid);
        printf("Pass.......: %s\r\n", s->wifiPassword);
        printf("mDNS name..: %s.local\r\n", s->mdnsName);
        printf("Server port: %u\r\n", s->listenPort);
        printf("Busy Msg...: %s\r\n", s->busyMsg);
        printf("E%u Q%u V%u X%u &D%u NET%u S0=%u S2=%u\r\n",
               s->echo,
               s->quiet,
               s->verbose,
               s->extendedCodes,
               s->dtrHandling,
               s->telnet,
               s->autoAnswer,
               s->escChar);

        printf("Speed dial:\r\n");
        for (int i = 0; i < SPEED_DIAL_SLOTS; i++)
        {
            if (s->speedDial[i][0])
            {
                printf("%u: %s,%s\r\n", i, s->speedDial[i], s->alias[i]);
            }
        }
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
    }

    return atCmd;
}

//
// AT&W: update NVRAM from current settings
//
static char *updateNvram(char *atCmd)
{
    writeSettings(&settings);
    if (!atCmd[0])
    {
        sendResult(R_OK);
    }
    return atCmd;
}

//
// AT&Zn? show contents of speed dial slot n
// AT&Zn=host,alias set speed dial slot n
//
static char *doSpeedDialSlot(char *atCmd)
{
    int slot;

    if (isdigit((unsigned char)atCmd[0]))
    {
        slot = atCmd[0] - '0';
        ++atCmd;
        switch (atCmd[0])
        {
        case '?':
            ++atCmd;
            if (settings.speedDial[slot][0])
            {
                printf("%s,%s\r\n",
                       settings.speedDial[slot], settings.alias[slot]);
                if (!atCmd[0])
                {
                    sendResult(R_OK);
                }
            }
            else
            {
                sendResult(R_ERROR);
            }
            break;
        case '=':
            ++atCmd;
            if (!atCmd[0])
            {
                // erase slot
                settings.speedDial[slot][0] = NUL;
                settings.alias[slot][0] = NUL;
                sendResult(R_OK);
            }
            else
            {
                char *comma = strchr(atCmd, ',');
                if (!comma)
                {
                    sendResult(R_ERROR);
                }
                else
                {
                    *comma++ = NUL;
                    strncpy(settings.speedDial[slot], atCmd, MAX_SPEED_DIAL_LEN);
                    settings.speedDial[slot][MAX_SPEED_DIAL_LEN] = NUL;
                    strncpy(settings.alias[slot], comma, MAX_ALIAS_LEN);
                    settings.alias[slot][MAX_ALIAS_LEN] = NUL;
                    atCmd[0] = NUL;
                    sendResult(R_OK);
                }
            }
            break;
        default:
            sendResult(R_ERROR);
            break;
        }
    }
    else
    {
        sendResult(R_ERROR);
    }
    return atCmd;
}

//
// AT$AE? query auto execute command string
// AT$AE=auto execute command string
//
static char *doAutoExecute(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%s\r\n", settings.autoExecute);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        strncpy(settings.autoExecute, atCmd, MAX_AUTOEXEC_LEN);
        settings.busyMsg[MAX_AUTOEXEC_LEN] = NUL;
        atCmd[0] = NUL;
        sendResult(R_OK);
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT$AYT send "Are you there?" if in a Telnet session
//
static char *doAreYouThere(char *atCmd)
{
    static const uint8_t areYouThere[] = {IAC, AYT};

    if (tcpIsConnected(tcpClient) && settings.telnet != NO_TELNET)
    {
        setStateOnline();
        dtrWentInactive = false;
        bytesOut += tcpWriteBuf(tcpClient, areYouThere, sizeof areYouThere);
    }
    else
    {
        sendResult(R_ERROR);
    }
    return atCmd;
}

//
// AT$BM?  query busy message
// AT$BM=busy message set busy message
//
static char *doBusyMessage(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%s\r\n", settings.busyMsg);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        strncpy(settings.busyMsg, atCmd, MAX_BUSYMSG_LEN);
        settings.busyMsg[MAX_BUSYMSG_LEN] = NUL;
        atCmd[0] = NUL;
        sendResult(R_OK);
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT$MDNS? query mDNS network name
// AT$MDNS=mdnsname set mDNS network name
//
static char *doMdnsName(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%s\r\n", settings.mdnsName);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;

    case '=':
        ++atCmd;
        strncpy(settings.mdnsName, atCmd, MAX_MDNSNAME_LEN);
        settings.mdnsName[MAX_MDNSNAME_LEN] = NUL;
        atCmd[0] = NUL;
        sendResult(R_OK);
        break;

    default:
        sendResult(R_ERROR);
        lastCmd[0] = NUL;
        break;
    }
    return atCmd;
}

//
// AT$PASS? query WiFi password
// AT$PASS=password set WiFi password
//
static char *doWiFiPassword(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%s\r\n", settings.wifiPassword);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        strncpy(settings.wifiPassword, atCmd, MAX_WIFI_PWD_LEN);
        settings.wifiPassword[MAX_WIFI_PWD_LEN] = NUL;
        atCmd[0] = NUL;
        sendResult(R_OK);
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT$SP?  query inbound TCP port #
// AT$SP=n set inbound TCP port #
//         NOTE: n=0 will disable the inbound TCP port
//               and a RING message will never be displayed
//
static char *doServerPort(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.listenPort);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        settings.listenPort = atoi(atCmd);
        while (isdigit((unsigned char)atCmd[0]))
        {
            ++atCmd;
        }
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT$SSID? query WiFi SSID
// AT$SSID=ssid set WiFi SSID
//
static char *doSSID(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%s\r\n", settings.ssid);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        strncpy(settings.ssid, atCmd, MAX_SSID_LEN);
        settings.ssid[MAX_SSID_LEN] = NUL;
        atCmd[0] = NUL;
        sendResult(R_OK);
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT$TTL? query Telnet location
// AT$TTL=location set Telnet location
//
static char *doLocation(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%s\r\n", settings.location);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        strncpy(settings.location, atCmd, MAX_LOCATION_LEN);
        settings.location[MAX_LOCATION_LEN] = NUL;
        atCmd[0] = NUL;
        sendResult(R_OK);
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT$TTS? query Telnet window size
// AT$TTS=WxH set Telnet window size (width x height)
//
static char *doWindowSize(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%ux%u\r\n", settings.width, settings.height);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
    {
        char *width = atCmd + 1;
        char *height = strpbrk(width, "xX");
        if (!width || !height)
        {
            sendResult(R_ERROR);
        }
        else
        {
            ++height; // point to 1st char past X
            settings.width = atoi(width);
            settings.height = atoi(height);
            atCmd = height;
            while (isdigit((unsigned char)atCmd[0]))
            {
                ++atCmd;
            }
            if (!atCmd[0])
            {
                sendResult(R_OK);
            }
        }
    }
    break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT$TTY? query Telnet terminal type
// AT$TTY=terminal set Telnet terminal type
//
static char *doTerminalType(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%s\r\n", settings.terminal);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        strncpy(settings.terminal, atCmd, MAX_TERMINAL_LEN);
        settings.location[MAX_TERMINAL_LEN] = NUL;
        atCmd[0] = NUL;
        sendResult(R_OK);
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

//
// AT$W? query startup wait status
// AT$W=0 disable startup wait
// AT$W=1 enable startup wait (wait for a CR on startup)
//
static char *doStartupWait(char *atCmd)
{
    switch (atCmd[0])
    {
    case '?':
        ++atCmd;
        printf("%u\r\n", settings.startupWait);
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
        break;
    case '=':
        ++atCmd;
        switch (atCmd[0])
        {
        case '0':
        case '1':
            settings.startupWait = atCmd[0] == '1';
            atCmd[0] = NUL;
            sendResult(R_OK);
            break;
        default:
            sendResult(R_ERROR);
            break;
        }
        break;
    default:
        sendResult(R_ERROR);
        break;
    }
    return atCmd;
}

void doAtCmds(char *atCmd)
{
    size_t len;

    trim(atCmd); // get rid of leading and trailing spaces
    if (atCmd[0])
    {
        // is it an AT command?
        if (strncasecmp(atCmd, "AT", 2))
        {
            sendResult(R_ERROR); // nope, time to die
        }
        else
        {
            // save command for possible future A/
            strncpy(lastCmd, atCmd, MAX_CMD_LEN);
            lastCmd[MAX_CMD_LEN] = NUL;
            atCmd += 2; // skip over AT prefix
            len = strlen(atCmd);

            if (!atCmd[0])
            {
                // plain old AT
                sendResult(R_OK);
            }
            else
            {
                trim(atCmd);
                while (atCmd[0])
                {
                    if (!strncasecmp(atCmd, "?", 1))
                    { // help message
                        // help
                        atCmd = showHelp(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "$AYT", 4))
                    {
                        // send Telnet "Are You There?"
                        atCmd = doAreYouThere(atCmd + 4);
                    }
                    else if (!strncasecmp(atCmd, "$SSID", 5))
                    {
                        // query/set WiFi SSID
                        atCmd = doSSID(atCmd + 5);
                    }
                    else if (!strncasecmp(atCmd, "$PASS", 5))
                    {
                        // query/set WiFi password
                        atCmd = doWiFiPassword(atCmd + 5);
                    }
                    else if (!strncasecmp(atCmd, "C", 1))
                    {
                        // connect/disconnect to WiFi
                        atCmd = wifiConnection(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "D", 1) && len > 2 && strchr("TPI", toupper(atCmd[1])))
                    {
                        // dial a number
                        atCmd = dialNumber(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "DS", 2) && len == 3)
                    {
                        // speed dial a number
                        atCmd = speedDialNumber(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "H0", 2))
                    {
                        // hang up call
                        atCmd = hangup(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "H", 1) && !isdigit((unsigned char)atCmd[1]))
                    {
                        // hang up call
                        atCmd = hangup(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "&Z", 2) && isdigit((unsigned char)atCmd[2]))
                    {
                        // speed dial query or set
                        atCmd = doSpeedDialSlot(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "O", 1))
                    {
                        // go online
                        atCmd = goOnline(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "GET", 3))
                    {
                        // get a web page (http only, no https)
                        atCmd = httpGet(atCmd + 3);
                    }
                    else if (settings.listenPort && !strncasecmp(atCmd, "A", 1) && serverHasClient(&tcpServer))
                    {
                        // manually answer incoming connection
                        atCmd = answerCall(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "S0", 2))
                    {
                        // query/set auto answer
                        atCmd = doAutoAnswerConfig(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "S2", 2))
                    {
                        // query/set escape character
                        atCmd = doEscapeCharConfig(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "$SP", 3))
                    {
                        // query set inbound TCP port
                        atCmd = doServerPort(atCmd + 3);
                    }
                    else if (!strncasecmp(atCmd, "$BM", 3))
                    {
                        // query/set busy message
                        atCmd = doBusyMessage(atCmd + 3);
                    }
                    else if (!strncasecmp(atCmd, "&R", 2))
                    {
                        // query/set require password
                        atCmd = doServerPassword(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "I", 1))
                    {
                        // show network information
                        atCmd = showNetworkInfo(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "Z", 1))
                    {
                        // reset to NVRAM
                        atCmd = doResetToNvram(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "&V", 2))
                    {
                        // display current and stored settings
                        atCmd = displayAllSettings(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "&W", 2))
                    {
                        // write settings to EEPROM
                        atCmd = updateNvram(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "&D", 2))
                    {
                        // DTR transition handling
                        atCmd = doDtrHandling(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "&F", 2))
                    {
                        // factory defaults
                        atCmd = doFactoryDefaults(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "E", 1))
                    {
                        // query/set command mode echo
                        atCmd = doEcho(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "Q", 1))
                    {
                        // query/set quiet mode
                        atCmd = doQuiet(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "V", 1))
                    {
                        // query/set verbose mode
                        atCmd = doVerbose(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "X", 1))
                    {
                        // query/set extended result codes
                        atCmd = doExtended(atCmd + 1);
                    }
                    else if (!strncasecmp(atCmd, "$W", 2))
                    {
                        // query/set startup wait
                        atCmd = doStartupWait(atCmd + 2);
                    }
                    else if (!strncasecmp(atCmd, "NET", 3))
                    {
                        // query/set telnet mode
                        atCmd = doTelnetMode(atCmd + 3);
                    }
                    else if (!strncasecmp(atCmd, "$AE", 3))
                    {
                        // do auto execute commands
                        atCmd = doAutoExecute(atCmd + 3);
                    }
                    else if (!strncasecmp(atCmd, "$TTY", 4))
                    {
                        // do telnet terminal type
                        atCmd = doTerminalType(atCmd + 4);
                    }
                    else if (!strncasecmp(atCmd, "$TTL", 4))
                    {
                        // do telnet location
                        atCmd = doLocation(atCmd + 4);
                    }
                    else if (!strncasecmp(atCmd, "$TTS", 4))
                    {
                        // do telnet location
                        atCmd = doWindowSize(atCmd + 4);
                    }
                    else if (!strncasecmp(atCmd, "$MDNS", 5))
                    {
                        // handle mDNS name
                        atCmd = doMdnsName(atCmd + 5);
                    }
                    else
                    {
                        // unrecognized command
                        sendResult(R_ERROR);
                    }
                    trim(atCmd);
                }
            }
        }
    }
}

#endif /* RP6502_RIA_W */
