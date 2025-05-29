#ifndef _TYPES_H
#define _TYPES_H
#include "pico/stdlib.h"
#include "lwip/dns.h"
#include "wifi_modem.h"

typedef enum ResultCodes
{
    R_OK,
    R_CONNECT,
    R_RING,
    R_NO_CARRIER,
    R_ERROR,
    R_NO_ANSWER,
    R_RING_IP
} ResultCodes;
typedef enum DtrStates
{
    DTR_IGNORE,
    DTR_GOTO_COMMAND,
    DTR_END_CALL,
    DTR_RESET
} DtrStates;

typedef struct Settings
{
    uint16_t magicNumber;
    char ssid[MAX_SSID_LEN + 1];
    char wifiPassword[MAX_WIFI_PWD_LEN + 1];
    uint32_t serialSpeed;
    uint8_t dataBits;
    uart_parity_t parity;
    uint8_t stopBits;
    bool rtsCts;
    uint8_t width, height;
    char escChar;
    char alias[SPEED_DIAL_SLOTS][MAX_ALIAS_LEN + 1];
    char speedDial[SPEED_DIAL_SLOTS][MAX_SPEED_DIAL_LEN + 1];
    char mdnsName[MAX_MDNSNAME_LEN + 1];
    uint8_t autoAnswer;
    uint16_t listenPort;
    char busyMsg[MAX_BUSYMSG_LEN + 1];
    char serverPassword[MAX_PWD_LEN + 1];
    bool echo;
    uint8_t telnet;
    char autoExecute[MAX_AUTOEXEC_LEN + 1];
    char terminal[MAX_TERMINAL_LEN + 1];
    char location[MAX_LOCATION_LEN + 1];
    bool startupWait;
    bool extendedCodes;
    bool verbose;
    bool quiet;
    DtrStates dtrHandling;
} SETTINGS_T;

typedef struct TCP_CLIENT_T_
{
    struct tcp_pcb *pcb;
    ip_addr_t remoteAddr;
    volatile bool connected;
    volatile bool connectFinished;
    volatile bool waitingForAck;
    volatile uint8_t rxBuff[TCP_CLIENT_RX_BUF_SIZE];
    volatile uint16_t rxBuffLen;
    volatile uint16_t rxBuffHead;
    volatile uint16_t rxBuffTail;
    volatile uint16_t totLen;
    volatile uint8_t txBuff[TCP_CLIENT_TX_BUF_SIZE];
    volatile uint16_t txBuffLen;
    volatile uint16_t txBuffHead;
    volatile uint16_t txBuffTail;
} TCP_CLIENT_T;

typedef struct TCP_SERVER_T_
{
    struct tcp_pcb *pcb;
    struct tcp_pcb *clientPcb;
} TCP_SERVER_T;
#endif
