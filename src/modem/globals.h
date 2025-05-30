#ifndef _GLOBALS_H
#define _GLOBALS_H
#include "modem.h"
// globals
const char okStr[] = {"OK"};
const char connectStr[] = {"CONNECT"};
const char ringStr[] = {"RING"};
const char noCarrierStr[] = {"NO CARRIER"};
const char errorStr[] = {"ERROR"};
const char noAnswerStr[] = {"NO ANSWER"};
const char *const resultCodes[] = {okStr, connectStr, ringStr, noCarrierStr, errorStr, noAnswerStr, ringStr};

SETTINGS_T settings;
TCP_CLIENT_T *tcpClient, tcpClient0, tcpDroppedClient;
TCP_SERVER_T tcpServer;
// incantation to switch from line mode to character mode
const uint8_t toCharModeMagic[] = {IAC, WILL, SUP_GA, IAC, WILL, ECHO, IAC, WONT, LINEMODE};
uint32_t bytesIn = 0, bytesOut = 0;
unsigned long connectTime = 0;
char atCmd[MAX_CMD_LEN + 1], lastCmd[MAX_CMD_LEN + 1];
unsigned atCmdLen = 0;
enum
{
    CMD_NOT_IN_CALL,
    CMD_IN_CALL,
    ONLINE,
    PASSWORD
} state = CMD_NOT_IN_CALL;
bool ringing = false;    // no incoming call
uint8_t ringCount = 0;   // current incoming call ring count
uint32_t nextRingMs = 0; // time of mext RING result
uint8_t escCount = 0;    // Go to AT mode at "+++" sequence, that has to be counted
uint32_t guardTime = 0;  // When did we last receive a "+++" sequence
char password[MAX_PWD_LEN + 1];
uint8_t passwordTries = 0; // # of unsuccessful tries at incoming password
uint8_t passwordLen = 0;
uint8_t txBuf[TX_BUF_SIZE]; // Transmit Buffer
uint8_t sessionTelnetType;
volatile bool dtrWentInactive = false;
bool amClient = false; // true if we've connected TO a remote server
#ifndef NDEBUG
volatile uint16_t maxTotLen = 0;
volatile uint16_t maxRxBuffLen = 0;
uint16_t maxTxBuffLen = 0;
err_t lastTcpWriteErr = ERR_OK;
#endif

#endif
