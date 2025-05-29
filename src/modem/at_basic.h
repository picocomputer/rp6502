//
// ATA manually answer an incoming call
//
char *answerCall(char *atCmd)
{
    tcpClient = serverGetClient(&tcpServer, &tcpClient0);
    ser_set(RI, !ACTIVE); // we've picked up so ringing stops
    ringing = false;
    ringCount = 0;
    if (settings.telnet != NO_TELNET)
    {
        // send incantation to switch from line mode to character mode
        bytesOut += tcpWriteBuf(tcpClient, toCharModeMagic, sizeof toCharModeMagic);
    }
    sendResult(R_RING_IP);
    sleep_ms(1000);
    connectTime = millis();
    dtrWentInactive = false;
    sendResult(R_CONNECT);
    ser_set(DCD, ACTIVE); // we've got a carrier signal
    amClient = false;
    state = ONLINE;
    ser_tx_wait_blocking(ser0); // drain the UART's Tx FIFO
    return atCmd;
}

//
// ATC? query WiFi connection status
// ATC0 disconnect from WiFi network
// ATC1 connect to WiFi network
//
char *wifiConnection(char *atCmd)
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
// ATDSn speed dial a number
//
char *dialNumber(char *atCmd);

char *speedDialNumber(char *atCmd)
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
// ATDThost[:port] dial a number
//
char *dialNumber(char *atCmd)
{
    char *host, *port, *ptr;
    char tempNumber[MAX_SPEED_DIAL_LEN + 1];
    int portNum;
    ip_addr_t remoteAddr;

    getHostAndPort(atCmd, host, port, portNum);
    if (!port)
    {
        // check for host names that are 7 identical digits long
        // and dial from the corresponding speed dial slot
        bool speedDial = true;
        if (strlen(host) == MAGIC_SPEED_LEN && isdigit(host[0]))
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
            getHostAndPort(tempNumber, host, port, portNum);
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
                    getHostAndPort(tempNumber, host, port, portNum);
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
        printf("DIALLING %s:%u\r\n", host, portNum);
        ser_tx_wait_blocking(ser0);
    }
    sleep_ms(2000); // delay for ZMP to be able to detect CONNECT
    if (!ser_is_readable(ser0))
    {
        tcpClient = tcpConnect(&tcpClient0, host, portNum);
        if (tcpClient)
        {
            connectTime = millis();
            dtrWentInactive = false;
            sendResult(R_CONNECT);
            ser_set(DCD, ACTIVE);
            state = ONLINE;
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
// ATE? query command mode echo status
// ATE0 disable command mode local echo
// ATE1 enable command mode local echo
//
char *doEcho(char *atCmd)
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
char *httpGet(char *atCmd)
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
        state = ONLINE;

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
char *hangup(char *atCmd)
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
const char helpStr01[] = "Help..........: AT?";
const char helpStr02[] = "Repeat command: A/";
const char helpStr03[] = "Answer call...: ATA";
const char helpStr04[] = "WiFi connect..: ATCn";
const char helpStr05[] = "Speed dial....: ATDSn";
const char helpStr06[] = "Dial host.....: ATDThost[:port]";
const char helpStr07[] = "Command echo..: ATEn";
const char helpStr08[] = "HTTP get......: ATGEThttp://host[/page]";
const char helpStr09[] = "Hang up.......: ATH";
const char helpStr10[] = "Network info..: ATI";
const char helpStr11[] = "Handle Telnet.: ATNETn";
const char helpStr12[] = "Leave cmd mode: ATO";
const char helpStr13[] = "Quiet mode....: ATQn";
const char helpStr14[] = "NIST date.time: ATRD/ATRT";
const char helpStr15[] = "Auto answer...: ATS0=n";
const char helpStr16[] = "Verbose mode..: ATVn";
const char helpStr17[] = "Extended codes: ATXn";
const char helpStr18[] = "Modem reset...: ATZ";
const char helpStr19[] = "DTR handling..: AT&D";
const char helpStr20[] = "Fact. defaults: AT&F";
const char helpStr21[] = "Flow control..: AT&Kn";
const char helpStr22[] = "Server passwd.: AT&R=server password";
const char helpStr23[] = "Show settings.: AT&Vn";
const char helpStr24[] = "Update NVRAM..: AT&W";
const char helpStr25[] = "Set speed dial: AT&Zn=host[:port],alias";
const char helpStr26[] = "Auto execute..: AT$AE=AT command";
const char helpStr27[] = "Are You There?: AT$AYT";
const char helpStr28[] = "Busy message..: AT$BM=busy message";
const char helpStr29[] = "mDNS name.....: AT$MDNS=mDNS name";
const char helpStr30[] = "WiFi password.: AT$PASS=WiFi password";
const char helpStr31[] = "Serial speed..: AT$SB=n";
const char helpStr32[] = "Server port...: AT$SP=n";
const char helpStr33[] = "WiFi SSID.....: AT$SSID=ssid";
const char helpStr34[] = "Data config...: AT$SU=dps";
const char helpStr35[] = "Location......: AT$TTL=telnet location";
const char helpStr36[] = "Terminal size.: AT$TTS=WxH";
const char helpStr37[] = "Terminal type.: AT$TTY=terminal type";
const char helpStr38[] = "Startup wait..: AT$W=n";
const char helpStr39[] = "Query most commands followed by '?'";
const char helpStr40[] = "e.g. ATQ?, AT&K?, AT$SSID?";

const char *const helpStrs[] = {
    helpStr01, helpStr02, helpStr03, helpStr04, helpStr05, helpStr06,
    helpStr07, helpStr08, helpStr09, helpStr10, helpStr11, helpStr12,
    helpStr13, helpStr14, helpStr15, helpStr16, helpStr17, helpStr18,
    helpStr19, helpStr20, helpStr21, helpStr22, helpStr23, helpStr24,
    helpStr25, helpStr26, helpStr27, helpStr28, helpStr29, helpStr30,
    helpStr31, helpStr32, helpStr33, helpStr34, helpStr35, helpStr36,
    helpStr37, helpStr38, helpStr39, helpStr40};
#define NUM_HELP_STRS (sizeof(helpStrs) / sizeof(helpStrs[0]))

char *showHelp(char *atCmd)
{
    char helpLine[80];

    PagedOut("AT Command Summary:", true);
    if (settings.width >= 80)
    {
        // dual columns
        for (int i = 0; i < NUM_HELP_STRS / 2; ++i)
        {
            snprintf(
                helpLine,
                sizeof helpLine,
                "%-40s%s",
                helpStrs[i],
                helpStrs[i + NUM_HELP_STRS / 2]);
            if (PagedOut(helpLine))
            {
                break; // user responded with ^C, quit
            }
        }
    }
    else
    {
        // single column
        for (int i = 0; i < NUM_HELP_STRS; ++i)
        {
            if (PagedOut(helpStrs[i]))
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
char *showNetworkInfo(char *atCmd)
{
    char infoLine[80];
    size_t maxCatChars;
    uint8_t mac[6];

    int wifiStatus = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

    do
    { // a Q&D hack to allow ^C to terminate the output at the
      // end of a page
        if (PagedOut("Pico WiFi modem", true))
            break;
        if (PagedOut("Build......: " __DATE__ " " __TIME__))
            break;
        snprintf(infoLine, sizeof infoLine, "Baud.......: %lu", settings.serialSpeed);
        if (PagedOut(infoLine))
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
        if (PagedOut(infoLine))
            break;
        snprintf(infoLine, sizeof infoLine, "SSID.......: %s", settings.ssid);
        if (PagedOut(infoLine))
            break;
        if (wifiStatus == CYW43_LINK_JOIN)
        {
            int32_t rssi;
            cyw43_ioctl(&cyw43_state, 254, sizeof rssi, (uint8_t *)&rssi, CYW43_ITF_STA);
            snprintf(infoLine, sizeof infoLine, "RSSI.......: %d dBm", rssi);
            if (PagedOut(infoLine))
                break;
        }
        if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) != ERR_OK)
        {
            if (PagedOut("MAC address: ?"))
                break;
        }
        else
        {
            snprintf(infoLine, sizeof infoLine,
                     "MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            if (PagedOut(infoLine))
                break;
        }
        if (wifiStatus == CYW43_LINK_JOIN)
        {
            snprintf(infoLine, sizeof infoLine, "IP address.: %s", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[0])));
            if (PagedOut(infoLine))
                break;
            snprintf(infoLine, sizeof infoLine, "Gateway....: %s", ip4addr_ntoa(netif_ip4_gw(&cyw43_state.netif[0])));
            if (PagedOut(infoLine))
                break;
            snprintf(infoLine, sizeof infoLine, "Subnet mask: %s", ip4addr_ntoa(netif_ip4_netmask(&cyw43_state.netif[0])));
            if (PagedOut(infoLine))
                break;
        }
        snprintf(infoLine, sizeof infoLine, "mDNS name..: %s.local", settings.mdnsName);
        if (PagedOut(infoLine))
            break;
        snprintf(infoLine, sizeof infoLine, "Server port: %u", settings.listenPort);
        if (PagedOut(infoLine))
            break;
        snprintf(infoLine, sizeof infoLine, "Bytes in...: %lu", bytesIn);
        if (PagedOut(infoLine))
            break;
        snprintf(infoLine, sizeof infoLine, "Bytes out..: %lu", bytesOut);
        if (PagedOut(infoLine))
            break;
#ifndef NDEBUG
        snprintf(infoLine, sizeof infoLine, "Max totLen.: %u", maxTotLen);
        if (PagedOut(infoLine))
            break;
        snprintf(infoLine, sizeof infoLine, "Max rxBuff.: %u", maxRxBuffLen);
        if (PagedOut(infoLine))
            break;
        snprintf(infoLine, sizeof infoLine, "Max txBuff.: %u", maxTxBuffLen);
        if (PagedOut(infoLine))
            break;
#endif
        snprintf(infoLine, sizeof infoLine, "Heap free..: %lu", getFreeHeap());
        if (PagedOut(infoLine))
            break;
        snprintf(infoLine, sizeof infoLine, "Pgm. size..: %lu", getProgramSize());
        if (PagedOut(infoLine))
            break;
        snprintf(infoLine, sizeof infoLine, "Pgm. free..: %lu", getFreeProgramSpace());
        if (PagedOut(infoLine))
            break;
        if (tcpIsConnected(tcpClient))
        {
            snprintf(infoLine, sizeof infoLine, "Call status: CONNECTED TO %s", ip4addr_ntoa(&tcpClient->pcb->remote_ip));
            if (PagedOut(infoLine))
                break;
            snprintf(infoLine, sizeof infoLine, "Call length: %s", connectTimeString());
            if (PagedOut(infoLine))
                break;
        }
        else
        {
            if (PagedOut("Call status: NOT CONNECTED"))
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
char *doTelnetMode(char *atCmd)
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
char *goOnline(char *atCmd)
{
    if (tcpIsConnected(tcpClient))
    {
        state = ONLINE;
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
char *doQuiet(char *atCmd)
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
// ATRD Displays the UTC date and time from NIST in the format
// ATRT "YY-MM-DD HH:MM:SS"
//
char *doDateTime(char *atCmd)
{
    bool ok = false;
    uint16_t len;

    if (!tcpIsConnected(tcpClient))
    {
        char result[80], *ptr;
        tcpClient = tcpConnect(&tcpClient0, NIST_HOST, NIST_PORT);
        if (tcpClient)
        {
            ser_set(DCD, ACTIVE);
            // read date/time from NIST
            result[0] = tcpReadByte(tcpClient, 1000);
            if (result[0] == '\n')
            { // leading LF
                len = tcpReadBytesUntil(tcpClient, '\n', result, (sizeof result) - 1);
                if (len)
                { // string read?
                    result[len] = NUL;
                    ptr = strtok(result, " ");
                    if (ptr)
                    { // found Julian day?
                        ptr = strtok(NULL, " ");
                        if (ptr)
                        { // found date?
                            printf("%s ", ptr);
                            ptr = strtok(NULL, " ");
                            if (ptr)
                            { // found time?
                                printf("%s\r\n", ptr);
                                ok = true;
                            }
                        }
                    }
                }
            }
            tcpClientClose(tcpClient);
            ser_set(DCD, !ACTIVE);
        }
    }
    if (ok)
    {
        if (!atCmd[0])
        {
            sendResult(R_OK);
        }
    }
    else
    {
        sendResult(R_ERROR);
    }
    return atCmd;
}

//
// ATS0?  query auto answer configuration
// ATS0=0 disable auto answer
// ATS0=n enable auto answer after n rings
//
char *doAutoAnswerConfig(char *atCmd)
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
        if (isdigit(atCmd[0]))
        {
            settings.autoAnswer = atoi(atCmd);
            while (isdigit(atCmd[0]))
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
char *doEscapeCharConfig(char *atCmd)
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
        if (isdigit(atCmd[0]))
        {
            settings.escChar = atoi(atCmd);
            while (isdigit(atCmd[0]))
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
char *doVerbose(char *atCmd)
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
char *doExtended(char *atCmd)
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
// ATZ restart the sketch
//
char *resetToNvram(char *atCmd)
{
    ser_tx_wait_blocking(ser0); // allow for CR/LF to finish
    watchdog_enable(1, false);
    while (true)
    {
        tight_loop_contents();
    }
    return atCmd; // should never actually get here...
}
