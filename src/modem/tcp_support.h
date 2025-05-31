#ifndef _TCP_SUPPORT_H_
#define _TCP_SUPPORT_H_

// #include "hardware/sync.h"
#include <stdint.h>
#include <stdbool.h>
#include "modem/ats.h"
#include "lwip/err.h"
#include "lwip/tcp.h"
// #include "tcp_types.h"     // for TCP_CLIENT_T, TCP_SERVER_T (replace with actual header if different)

// static volatile bool dnsLookupFinished = false;

void dnsLookupDone(const char *name, const ip_addr_t *ipaddr, void *arg);
bool dnsLookup(const char *name, ip_addr_t *resolved);
uint64_t millis(void);
bool tcpIsConnected(TCP_CLIENT_T *client);
err_t tcpClientClose(TCP_CLIENT_T *client);
void tcpClientErr(void *arg, err_t err);
err_t tcpSend(TCP_CLIENT_T *client);
err_t tcpSent(void *arg, struct tcp_pcb *tpcb, u16_t len);
err_t tcpPoll(void *arg, struct tcp_pcb *tpcb);
err_t tcpRecv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
err_t tcpHasConnected(void *arg, struct tcp_pcb *tpcb, err_t err);
TCP_CLIENT_T *tcpConnect(TCP_CLIENT_T *client, const char *host, int portNum);
void tcpServerErr(void *arg, err_t err);
err_t tcpServerAccept(void *arg, struct tcp_pcb *clientPcb, err_t err);
bool tcpServerStart(TCP_SERVER_T *server, int portNum);
uint16_t tcpWriteCharModeMagic(TCP_CLIENT_T *client);
uint16_t tcpWriteBuf(TCP_CLIENT_T *client, const uint8_t *buf, uint16_t len);
uint16_t tcpWriteStr(TCP_CLIENT_T *client, const char *str);
uint16_t tcpWriteByte(TCP_CLIENT_T *client, uint8_t c);
uint16_t tcpBytesAvailable(TCP_CLIENT_T *client);
int tcpReadByte(TCP_CLIENT_T *client);
void tcpTxFlush(TCP_CLIENT_T *client);
bool serverHasClient(TCP_SERVER_T *server);
TCP_CLIENT_T *serverGetClient(TCP_SERVER_T *server, TCP_CLIENT_T *client);

#endif /* _TCP_SUPPORT_H_ */
