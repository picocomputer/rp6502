#ifndef _RTC_H_
#define _RTC_H_

#include "fatfs/ff.h"

#define FAT_EPOCH_YEAR 1980
#define RTC_OK 0
#define RTC_NOT_SET 20
#define RTC_INVALID_DATETIME 21
#define RTC_NTP_PENDING 22

// Kernel events
void rtc_init_ (void);
DWORD get_fattime (void);
void set_timezone (const char* timezone);

/*
 * The API implementaiton for RTC support
 */

void rtc_api_get_time(void);
void rtc_api_set_time(void);
void rtc_api_get_timezone(void);

#endif /* _RTC_H_ */
