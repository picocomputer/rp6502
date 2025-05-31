#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "modem/ser_cdc.h"
#include "modem/commands.h"
#include "modem/support.h"
#include "modem/tcp_support.h"

void setup(void)
{
    readSettings(&settings);

    if (settings.magicNumber != MAGIC_NUMBER)
    {
        // no valid data in EEPROM/NVRAM, populate with defaults
        factoryDefaults(NULL);
    }
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
        resetToNvram(NULL);
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
                resetToNvram(NULL);
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
