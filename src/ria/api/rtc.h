#ifndef _RTC_H_
#define _RTC_H_

// Kernel events
void rtc_init_(void);
void rtc_print(void);

/*
 * The API implementaiton for RTC support
 */

void rtc_api_read_rtc_time(void);
void rtc_api_write_rtc_time(void);

#endif /* _RTC_H_ */
