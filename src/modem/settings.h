/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MODEM_SETTINGS_H_
#define _MODEM_SETTINGS_H_

#include "modem.h"
#include "lwip/dns.h"

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

extern SETTINGS_T settings;
extern TCP_CLIENT_T *tcpClient, tcpClient0, tcpDroppedClient;
extern TCP_SERVER_T tcpServer;
extern uint32_t bytesIn, bytesOut;
extern uint64_t connectTime;
extern char atCmd[MAX_CMD_LEN + 1], lastCmd[MAX_CMD_LEN + 1];
extern unsigned atCmdLen;
extern bool ringing;        // no incoming call
extern uint8_t ringCount;   // current incoming call ring count
extern uint64_t nextRingMs; // time of mext RING result
extern uint8_t escCount;    // Go to AT mode at "+++" sequence, that has to be counted
extern uint64_t guardTime;  // When did we last receive a "+++" sequence
extern char password[MAX_PWD_LEN + 1];
extern uint8_t passwordTries; // # of unsuccessful tries at incoming password
extern uint8_t passwordLen;
extern uint8_t txBuf[TX_BUF_SIZE]; // Transmit Buffer
extern uint8_t sessionTelnetType;
extern volatile bool dtrWentInactive;
extern bool amClient; // true if we've connected TO a remote server
#ifndef NDEBUG
extern volatile uint16_t maxTotLen;
extern volatile uint16_t maxRxBuffLen;
extern uint16_t maxTxBuffLen;
extern err_t lastTcpWriteErr;
#endif

bool readSettings(SETTINGS_T *p);
bool writeSettings(SETTINGS_T *p);
void loadDefaultSettings(SETTINGS_T *p);
void loadNvramSettings(SETTINGS_T *p);

#endif /* _MODEM_SETTINGS_H_ */
