/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico.h>
#include <string.h>

#ifdef RP6502_RIA_W

#include "modem.h"
#include "modem/ats.h"
#include "ser_cdc.h"
#include "littlefs/lfs.h"
#include "sys/lfs.h"

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

static const char settings_fname[] = "settings.cfg";

// lfs_t lfs_volume;
lfs_file_t lfs_file;

bool readSettings(SETTINGS_T *p)
{
    bool ok = false;
    uint8_t file_buffer[FLASH_PAGE_SIZE];
    struct lfs_file_config file_config = {
        .buffer = file_buffer,
    };
    if (lfs_file_opencfg(&lfs_volume, &lfs_file, settings_fname, LFS_O_RDONLY, &file_config) == LFS_ERR_OK)
    {
        if (lfs_file_read(&lfs_volume, &lfs_file, p, sizeof(SETTINGS_T)) == sizeof(SETTINGS_T))
            ok = true;
        lfs_file_close(&lfs_volume, &lfs_file);
    }
    return ok;
}

bool writeSettings(SETTINGS_T *p)
{
    bool ok = false;
    uint8_t file_buffer[FLASH_PAGE_SIZE];
    struct lfs_file_config file_config = {
        .buffer = file_buffer,
    };
    if (lfs_file_opencfg(&lfs_volume, &lfs_file, settings_fname, LFS_O_RDWR | LFS_O_CREAT, &file_config) == LFS_ERR_OK)
    {
        if (lfs_file_write(&lfs_volume, &lfs_file, p, sizeof(SETTINGS_T)) == sizeof(SETTINGS_T))
        {
            ok = true;
        }
        lfs_file_close(&lfs_volume, &lfs_file);
    }
    return ok;
}

#endif /* RP6502_RIA_W */
