#ifndef _RTC_H_
#define _RTC_H_

#include "fatfs/ff.h"

typedef enum {
    RTC_OK = 0,             /* (0) RTC is set */
    RTC_NOT_SET,            /* (1) RTC is not set*/
    RTC_NTP_PENDING,        /* (2) RTC is waiting for NTP time response*/
    RTC_INVALID_DATETIME    /* (3) datetime_t parameter was invalid*/
} RTC_RESPONSE;

// Kernel events
void rtc_init_(void);
DWORD get_fattime (void);

/*
 * The API implementaiton for RTC support
 */

void rtc_api_get_time(void);
void rtc_api_set_time(void);

#endif /* _RTC_H_ */
