#ifndef _MODEM_SUPPORT_H_
#define _MODEM_SUPPORT_H_

#include <stdbool.h>
#include <stdint.h>

void crlf(void);
bool checkDtrIrq(void);
void inAtCommandMode();
void sendSerialData();
int receiveTcpData(void);
char *connectTimeString(void);
void sendResult(int resultCode);
void endCall();
void checkForIncomingCall();
void trim(char *str);
void getHostAndPort(char *number, char **host, char **port, int *portNum);
void displayCurrentSettings(void);
void displayStoredSettings(void);
void inPasswordMode();
bool PagedOut(const char *str, bool reset);

#endif /* _MODEM_SUPPORT_H_ */
