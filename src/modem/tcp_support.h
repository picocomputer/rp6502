//
// lwIP interface functions
//
static volatile bool dnsLookupFinished = false;

static void dnsLookupDone(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    ip_addr_t *resolved = (ip_addr_t *)arg;
    if (ipaddr && ipaddr->addr)
    {
        resolved->addr = ipaddr->addr;
    }
    dnsLookupFinished = true;
}

bool dnsLookup(const char *name, ip_addr_t *resolved)
{

    dnsLookupFinished = false;
    ip4_addr_set_any(resolved);

    switch (dns_gethostbyname(name, resolved, dnsLookupDone, resolved))
    {
    case ERR_OK:
        return true;
        break;
    case ERR_INPROGRESS:
        break;
    default:
        return false;
    }
    while (!dnsLookupFinished)
    {
        tight_loop_contents();
    }
    return !ip4_addr_isany(resolved);
}

uint64_t millis(void)
{
    return time_us_64()/1000;
}

bool tcpIsConnected(TCP_CLIENT_T *client)
{
    if (client && client->pcb && client->pcb->callback_arg)
    {
        return client->connected;
    }
    return false;
}

err_t tcpClientClose(TCP_CLIENT_T *client)
{
    err_t err = ERR_OK;

    cyw43_arch_lwip_begin();
    if (client)
    {
        client->connected = false;
        if (client->pcb)
        {
            tcp_err(client->pcb, NULL);
            tcp_sent(client->pcb, NULL);
            tcp_poll(client->pcb, NULL, 0);
            tcp_recv(client->pcb, NULL);
            tcp_arg(client->pcb, NULL);
            err = tcp_close(client->pcb);
            if (err != ERR_OK)
            {
                tcp_abort(client->pcb);
                err = ERR_ABRT;
            }
            client->pcb = NULL;
        }
    }
    cyw43_arch_lwip_end();
    return err;
}

// NB: the PCB may have already been freed when this function is called
static void tcpClientErr(void *arg, err_t err)
{
    (void)err;
    TCP_CLIENT_T *client = (TCP_CLIENT_T *)arg;

    if (client)
    {
        client->connectFinished = true;
        client->connected = false;
        client->pcb = NULL;
    }
}

static err_t tcpSend(TCP_CLIENT_T *client)
{
    err_t err = ERR_OK;
    uint32_t ints;

    if (client->txBuffLen)
    {
        uint16_t maxLen = tcp_sndbuf(client->pcb);
        if (maxLen > 0 && tcp_sndqueuelen(client->pcb) < TCP_SND_QUEUELEN)
        {
            if (client->txBuffLen < maxLen)
            {
                maxLen = client->txBuffLen;
            }
            uint8_t tmp[maxLen];
            // make copies of the head and length and work
            // with those in case tcp_write fails and we
            // have to re-send the same data later
            uint16_t tmpTxBuffHead = client->txBuffHead;
            uint16_t tmpTxBuffLen = client->txBuffLen;
            for (int i = 0; i < maxLen; ++i)
            {
                tmp[i] = client->txBuff[tmpTxBuffHead++];
                if (tmpTxBuffHead == TCP_CLIENT_TX_BUF_SIZE)
                {
                    tmpTxBuffHead = 0;
                }
                --tmpTxBuffLen;
            }
            err = tcp_write(client->pcb, tmp, maxLen, TCP_WRITE_FLAG_COPY);
            client->waitingForAck = err == ERR_OK;
            tcp_output(client->pcb);
            if (err == ERR_OK)
            {
                client->txBuffHead = tmpTxBuffHead;
                ints = save_and_disable_interrupts();
                client->txBuffLen = tmpTxBuffLen;
                restore_interrupts(ints);
#ifndef NDEBUG
            }
            else
            {
                lastTcpWriteErr = err;
#endif
            }
        }
    }
    return err;
}

static err_t tcpSent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)tpcb;
    (void)len;
    TCP_CLIENT_T *client = (TCP_CLIENT_T *)arg;
    err_t err = ERR_OK;

    if (client->txBuffLen)
    {
        err = tcpSend(client);
    }
    else
    {
        client->waitingForAck = false;
    }
    return err;
}

// in the event that the tcp_write call in tcpSend failed earlier,
// and there weren't any other packets waiting to be ACKed, try
// sending any data in the txBuff again.
static err_t tcpPoll(void *arg, struct tcp_pcb *tpcb)
{
    (void)tpcb;
    TCP_CLIENT_T *client = (TCP_CLIENT_T *)arg;
    err_t err = ERR_OK;
#ifndef NDEBUG
    static bool pollState = false;

    // gpio_put(POLL_STATE_LED, pollState);
    pollState = !pollState;
#endif
    if (!client->waitingForAck && client->txBuffLen)
    {
        err = tcpSend(client);
    }
    return err;
}

static err_t tcpRecv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)tpcb;
    (void)err;
    TCP_CLIENT_T *client = (TCP_CLIENT_T *)arg;

    if (!p)
    {
        return tcpClientClose(client);
    }
    if (p->tot_len > 0 && client)
    {
        for (struct pbuf *q = p; q; q = q->next)
        {
            for (int i = 0; i < q->len; ++i)
            {
                client->rxBuff[client->rxBuffTail++] = ((uint8_t *)q->payload)[i];
                if (client->rxBuffTail == TCP_CLIENT_RX_BUF_SIZE)
                {
                    client->rxBuffTail = 0;
                }
                ++client->rxBuffLen;
            }
        }
#ifndef NDEBUG
        if (client->rxBuffLen > TCP_CLIENT_RX_BUF_SIZE)
        {
            // gpio_put(RXBUFF_OVFL, HIGH);
        }
        if (client->rxBuffLen > maxRxBuffLen)
        {
            maxRxBuffLen = client->rxBuffLen;
        }
#endif
        if (client->rxBuffLen <= TCP_MSS)
        {
            tcp_recved(client->pcb, p->tot_len);
        }
        else
        {
            client->totLen += p->tot_len;
#ifndef NDEBUG
            if (client->totLen > maxTotLen)
            {
                maxTotLen = client->totLen;
            }
#endif
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcpHasConnected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    (void)tpcb;
    TCP_CLIENT_T *client = (TCP_CLIENT_T *)arg;

    client->connectFinished = true;
    client->connected = err == ERR_OK;
    if (err != ERR_OK)
    {
        tcpClientClose(client);
    }
    return ERR_OK;
}

TCP_CLIENT_T *tcpConnect(TCP_CLIENT_T *client, const char *host, int portNum)
{
    if (!dnsLookup(host, &client->remoteAddr))
    {
        return NULL;
    }
    else
    {
        client->pcb = tcp_new_ip_type(IP_GET_TYPE(client->remoteAddr));
        if (!client->pcb)
        {
            return NULL;
        }
    }
    tcp_arg(client->pcb, client);
    tcp_recv(client->pcb, tcpRecv);
    tcp_sent(client->pcb, tcpSent);
    tcp_poll(client->pcb, tcpPoll, 2);
    tcp_err(client->pcb, tcpClientErr);
    tcp_nagle_disable(client->pcb); // disable Nalge algorithm by default

    client->rxBuffLen = 0;
    client->rxBuffHead = 0;
    client->rxBuffTail = 0;
    client->totLen = 0;

    client->txBuffLen = 0;
    client->txBuffHead = 0;
    client->txBuffTail = 0;

    client->connected = false;
    client->connectFinished = false;
    client->waitingForAck = false;

    cyw43_arch_lwip_begin();
    err_t err = tcp_connect(client->pcb, &client->remoteAddr, portNum, tcpHasConnected);
    cyw43_arch_lwip_end();

    if (err != ERR_OK)
    {
        client->pcb = NULL;
        return NULL;
    }

    while (client->pcb && client->pcb->callback_arg && !client->connectFinished && !ser_is_readable(ser0))
    {
        tight_loop_contents();
    }
    if (!client->connected)
    {
        client->pcb = NULL;
        return NULL;
    }
    return client;
}

// NB: the PCB may have already been freed when this function is called
static void tcpServerErr(void *arg, err_t err)
{
    (void)err;
    TCP_SERVER_T *server = (TCP_SERVER_T *)arg;

    if (server)
    {
        server->pcb = NULL;
        server->clientPcb = NULL;
    }
}

static err_t tcpServerAccept(void *arg, struct tcp_pcb *clientPcb, err_t err)
{
    TCP_SERVER_T *server = (TCP_SERVER_T *)arg;

    if (err != ERR_OK || !clientPcb)
    {
        // ###      printf("Failure in accept: %d\n",err); //###
        server->clientPcb = NULL;
        tcp_close(server->pcb);
        return ERR_VAL;
    }
    // ###   if( server->clientPcb ) {
    // ###      printf("Overwriting server->clientPcb\n");   //###
    // ###   }
    server->clientPcb = clientPcb;
    return ERR_OK;
}

bool tcpServerStart(TCP_SERVER_T *server, int portNum)
{
    server->pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!server->pcb)
    {
        return false;
    }

    if (tcp_bind(server->pcb, NULL, portNum) != ERR_OK)
    {
        return false;
    }

    server->clientPcb = NULL;

    struct tcp_pcb *pcb = tcp_listen_with_backlog(server->pcb, 1);
    if (!pcb)
    {
        if (server->pcb)
        {
            tcp_close(server->pcb);
            server->pcb = NULL;
        }
        return false;
    }
    server->pcb = pcb;

    tcp_arg(server->pcb, server);
    tcp_accept(server->pcb, tcpServerAccept);
    tcp_err(server->pcb, tcpServerErr);

    return true;
}

uint16_t tcpWriteBuf(TCP_CLIENT_T *client, const uint8_t *buf, uint16_t len)
{
    uint32_t ints;

    if (client && client->pcb && client->pcb->callback_arg)
    {

        if (client->txBuffLen + len > TCP_CLIENT_TX_BUF_SIZE && client->connected)
        {
#ifndef NDEBUG
            // gpio_put(TXBUFF_OVFL, HIGH);
#endif
            while (client->txBuffLen + len > TCP_CLIENT_TX_BUF_SIZE && client->connected)
            {
                tight_loop_contents();
            }
#ifndef NDEBUG
            // gpio_put(TXBUFF_OVFL, LOW);
#endif
        }
        // lock out the lwIP thread now so that it can't end up calling
        // tcpSend until we're done with it... really don't want two
        // threads messing with txBuff at the same time.
        cyw43_arch_lwip_begin();
        for (uint16_t i = 0; i < len; ++i)
        {
            client->txBuff[client->txBuffTail++] = buf[i];
            if (client->txBuffTail == TCP_CLIENT_TX_BUF_SIZE)
            {
                client->txBuffTail = 0;
            }
            ints = save_and_disable_interrupts();
            ++client->txBuffLen;
            restore_interrupts(ints);
        }
#ifndef NDEBUG
        if (client->txBuffLen > maxTxBuffLen)
        {
            maxTxBuffLen = client->txBuffLen;
        }
#endif
        if (client->txBuffLen && client->pcb && client->pcb->callback_arg && !client->waitingForAck)
        {
            tcpSend(client);
        }
        cyw43_arch_lwip_end();
        return len;
    }
    return 0;
}

uint16_t tcpWriteStr(TCP_CLIENT_T *client, const char *str)
{
    return tcpWriteBuf(client, (uint8_t *)str, strlen(str));
}

uint16_t tcpWriteByte(TCP_CLIENT_T *client, uint8_t c)
{
    return tcpWriteBuf(client, (uint8_t *)&c, 1);
}

uint16_t tcpBytesAvailable(TCP_CLIENT_T *client)
{
    if (client)
    {
        return client->rxBuffLen;
    }
    return 0;
}

int tcpReadByte(TCP_CLIENT_T *client)
{
    if (client && client->rxBuffLen)
    {
        int c = client->rxBuff[client->rxBuffHead++];
        if (client->rxBuffHead == TCP_CLIENT_RX_BUF_SIZE)
        {
            client->rxBuffHead = 0;
        }
        uint32_t ints = save_and_disable_interrupts();
        --client->rxBuffLen;
        restore_interrupts(ints);
        if (!client->rxBuffLen && client->totLen && client->pcb)
        {
            cyw43_arch_lwip_begin();
            tcp_recved(client->pcb, client->totLen);
            client->totLen = 0;
            cyw43_arch_lwip_end();
        }
        return c;
    }
    return -1;
}

void tcpTxFlush(TCP_CLIENT_T *client)
{
    if (client)
    {
        while (client->pcb && client->connected && client->txBuffLen)
        {
            tight_loop_contents();
        }
    }
}

bool serverHasClient(TCP_SERVER_T *server)
{
    return server->clientPcb != NULL;
}

TCP_CLIENT_T *serverGetClient(TCP_SERVER_T *server, TCP_CLIENT_T *client)
{
    client->pcb = server->clientPcb;
    server->clientPcb = NULL;

    client->rxBuffLen = 0;
    client->rxBuffHead = 0;
    client->rxBuffTail = 0;
    client->totLen = 0;

    client->txBuffLen = 0;
    client->txBuffHead = 0;
    client->txBuffTail = 0;

    client->waitingForAck = false;

    tcp_arg(client->pcb, client);
    tcp_err(client->pcb, tcpClientErr);
    tcp_sent(client->pcb, tcpSent);
    tcp_poll(client->pcb, tcpPoll, 2);
    tcp_recv(client->pcb, tcpRecv);
    tcp_nagle_disable(client->pcb); // disable Nalge algorithm by default

    client->connected = true;
    client->connectFinished = true;

    return client;
}
