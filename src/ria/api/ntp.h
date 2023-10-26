#ifndef _NTP_H_
#define _NTP_H_

#include "fatfs/ff.h"

// Kernel events
void ntp_init(void);
DWORD get_fattime(void);

/*
 * The API implementaiton for time support
 */

void ntp_api_get_res(void);
void ntp_api_get_time(void);
void ntp_api_set_time(void);

#endif /* _NTP_H_ */
