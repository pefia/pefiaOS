#ifndef PEFIA_RTC_H
#define PEFIA_RTC_H

void rtc_time(int *h, int *m, int *s);

/* Write the wall clock (24h). Matches the CMOS BCD/binary mode in use. */
void rtc_set(int h, int m, int s);

#endif
