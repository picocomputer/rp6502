#include "fattime.h"
#include "hardware/rtc.h"
#include "pico/util/datetime.h"


DWORD get_fattime (void)
{
	DWORD res;
	datetime_t datetime;

	rtc_get_datetime(&datetime);
	
	res =  (((DWORD)datetime.year - 1980) << 25)
			| ((DWORD)datetime.month << 21)
			| ((DWORD)datetime.mday << 16)
			| (WORD)(datetime.hour << 11)
			| (WORD)(datetime.min << 5)
			| (WORD)(datetime.sec >> 1);

	return res;
}
