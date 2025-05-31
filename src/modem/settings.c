/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico.h>

#ifdef RP6502_RIA_W

#include "modem/settings.h"
#include "sys/lfs.h"
#include <string.h>

#define MAGIC_NUMBER 0x5678
static const char __in_flash("modem") settings_fname[] = "settings.cfg";

SETTINGS_T settings;
TCP_CLIENT_T *tcpClient, tcpClient0, tcpDroppedClient;
TCP_SERVER_T tcpServer;
uint32_t bytesIn = 0, bytesOut = 0;
uint64_t connectTime = 0;
char atCmd[MAX_CMD_LEN + 1], lastCmd[MAX_CMD_LEN + 1];
unsigned atCmdLen = 0;
MdmStates state = CMD_NOT_IN_CALL;
bool ringing = false;    // no incoming call
uint8_t ringCount = 0;   // current incoming call ring count
uint64_t nextRingMs = 0; // time of mext RING result
uint8_t escCount = 0;    // Go to AT mode at "+++" sequence, that has to be counted
uint64_t guardTime = 0;  // When did we last receive a "+++" sequence
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

bool readSettings(SETTINGS_T *p)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    bool ok = false;
    if (lfs_file_opencfg(&lfs_volume, &lfs_file, settings_fname, LFS_O_RDONLY, &lfs_file_config) == LFS_ERR_OK)
    {
        if (lfs_file_read(&lfs_volume, &lfs_file, p, sizeof(SETTINGS_T)) == sizeof(SETTINGS_T))
            ok = true;
        lfs_file_close(&lfs_volume, &lfs_file);
    }
    return ok;
}

bool writeSettings(SETTINGS_T *p)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    bool ok = false;
    if (lfs_file_opencfg(&lfs_volume, &lfs_file, settings_fname, LFS_O_RDWR | LFS_O_CREAT, &lfs_file_config) == LFS_ERR_OK)
    {
        if (lfs_file_write(&lfs_volume, &lfs_file, p, sizeof(SETTINGS_T)) == sizeof(SETTINGS_T))
        {
            ok = true;
        }
        lfs_file_close(&lfs_volume, &lfs_file);
    }
    return ok;
}

void loadDefaultSettings(SETTINGS_T *p)
{
    p->magicNumber = MAGIC_NUMBER;
    p->ssid[0] = NUL;
    p->wifiPassword[0] = NUL;
    p->width = 80;
    p->height = 24;
    p->escChar = ESC_CHAR;
    for (int i = 0; i < SPEED_DIAL_SLOTS; ++i)
    {
        p->alias[i][0] = NUL;
        p->speedDial[i][0] = NUL;
    }
    strcpy(p->mdnsName, "picocomputer");
    p->autoAnswer = 0;
    p->listenPort = 0;
    strcpy(p->busyMsg, "Sorry, the system is currently busy. Please try again later.");
    p->serverPassword[0] = NUL;
    p->echo = true;
    p->telnet = REAL_TELNET;
    p->autoExecute[0] = NUL;
    strcpy(p->terminal, "ansi");
    strcpy(p->location, "Computer Room");
    p->startupWait = false;
    p->extendedCodes = true;
    p->verbose = true;
    p->quiet = false;
    p->dtrHandling = DTR_IGNORE;
    strcpy(p->alias[0], "particles");
    strcpy(p->speedDial[0], "+particlesbbs.dyndns.org:6400");
    strcpy(p->alias[1], "altair");
    strcpy(p->speedDial[1], "altair.virtualaltair.com:4667");
    strcpy(p->alias[2], "heatwave");
    strcpy(p->speedDial[2], "heatwave.ddns.net:9640");
}

void loadNvramSettings(SETTINGS_T *p)
{
    if (!readSettings(p) || p->magicNumber != MAGIC_NUMBER)
        loadDefaultSettings(p);
}

#endif /* RP6502_RIA_W */
