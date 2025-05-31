
#include <string.h>
#include <time.h>
#include <malloc.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"

#include "modem.h"
// #include "types.h"
// #include "globals.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"

#include "tusb.h"
// #include "usb_cdc.h"

#include "modem/ser_cdc.h"
#include "modem/atc.h"
#include "modem/modem.h"
#include "modem/support.h"
#include "modem/tcp_support.h"
// #include "eeprom.h"
// #include "modem/lfs.h"
// #include "tcp_support.h"
// #include "support.h"

#include "modem/ats.h"


// =============================================================
void setup(void)
{
    // bool ok = true;

    // tud_init(TUD_OPT_RHPORT);
    stdio_init_all();
    // cdc_init();
    // do
    // {
    //     tud_task();
    // } while (!tud_ready());

    // gpio_init(DTR);
    // gpio_set_dir(DTR, INPUT);

    // gpio_init(RI);
    // gpio_set_dir(RI, OUTPUT);
    // gpio_put(RI, !ACTIVE);           // not ringing

    // gpio_init(DCD);
    // gpio_set_dir(DCD, OUTPUT);
    // gpio_put(DCD, !ACTIVE);          // not connected

    // gpio_init(DSR);
    // gpio_set_dir(DSR, OUTPUT);
    // gpio_put(DSR, !ACTIVE);          // modem is not ready
    // #ifndef NDEBUG
    //     gpio_init(POLL_STATE_LED);
    //     gpio_set_dir(POLL_STATE_LED, OUTPUT);
    //     gpio_put(POLL_STATE_LED, LOW);

    //     gpio_init(RXBUFF_OVFL);
    //     gpio_set_dir(RXBUFF_OVFL, OUTPUT);
    //     gpio_put(RXBUFF_OVFL, LOW);

    //     gpio_init(TXBUFF_OVFL);
    //     gpio_set_dir(TXBUFF_OVFL, OUTPUT);
    //     gpio_put(TXBUFF_OVFL, LOW);
    // #endif

    // initEEPROM();
    // initLFS();
    readSettings(&settings);

    if (settings.magicNumber != MAGIC_NUMBER)
    {
        // no valid data in EEPROM/NVRAM, populate with defaults
        factoryDefaults(NULL);
    }
    sessionTelnetType = settings.telnet;

    // ser_set_baudrate(ser0, settings.serialSpeed);
    // ser_set_format(ser0, settings.dataBits, settings.stopBits, (ser_parity_t)settings.parity);
    // ser_set_translate_crlf(ser0, false);
    // setHardwareFlow(settings.rtsCts);

    // enable interrupt when DTR goes inactive if we're not ignoring it
    gpio_set_irq_enabled_with_callback(DTR, GPIO_IRQ_EDGE_RISE, settings.dtrHandling != DTR_IGNORE, dtrIrq);

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

// Invoked when device is mounted
void tud_mount_cb(void)
{
    // blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    // blink_interval_ms = BLINK_NOT_MOUNTED;
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

// int main(void)
// {
//     setup();
//     while (true)
//     {
//         loop();
//     }
//     return 0;
// }
