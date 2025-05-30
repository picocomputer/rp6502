//
// AT$AE? query auto execute command string
// AT$AE=auto execute command string
//
char *doAutoExecute(char *atCmd)
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
char *doAreYouThere(char *atCmd)
{
    static const uint8_t areYouThere[] = {IAC, AYT};

    if (tcpIsConnected(tcpClient) && settings.telnet != NO_TELNET)
    {
        state = ONLINE;
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
char *doBusyMessage(char *atCmd)
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
char *doMdnsName(char *atCmd)
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
char *doWiFiPassword(char *atCmd)
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
char *doServerPort(char *atCmd)
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
        while (isdigit(atCmd[0]))
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
char *doSSID(char *atCmd)
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
char *doLocation(char *atCmd)
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
char *doWindowSize(char *atCmd)
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
            while (isdigit(atCmd[0]))
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
char *doTerminalType(char *atCmd)
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
char *doStartupWait(char *atCmd)
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
