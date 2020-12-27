// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// track.h: aircraft state tracking prototypes
//
// Copyright (c) 2019 Michael Wolf <michael@mictronics.de>
//
// This code is based on a detached fork of dump1090-fa.
//
// Copyright (c) 2015 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef DUMP1090_UTIL_H
#define DUMP1090_UTIL_H

#define GZBUFFER_BIG (1 * 1024 * 1024)

#include <stdint.h>

struct char_buffer
{
    char *buffer;
    size_t len;
};
struct char_buffer readWholeFile(int fd, char *errorContext);
struct char_buffer readWholeGz(gzFile gzfp, char *errorContext);
int writeGz(gzFile gzfp, void *source, int toWrite, char *errorContext);

void msleep(uint64_t ms);

/* Returns system time in milliseconds */
uint64_t mstime (void);

uint64_t msThreadTime(void);

/* Returns the time elapsed, in nanoseconds, from t1 to t2,
 * where t1 and t2 are 12MHz counters.
 */
int64_t receiveclock_ns_elapsed (uint64_t t1, uint64_t t2);

/* Same, in milliseconds */
int64_t receiveclock_ms_elapsed (uint64_t t1, uint64_t t2);

/* Normalize the value in ts so that ts->nsec lies in
 * [0,999999999]
 */
void normalize_timespec (struct timespec *ts);

struct timespec msToTimespec(uint64_t ms);

/* record current CPU time in start_time */
void start_cpu_timing (struct timespec *start_time);

/* add difference between start_time and the current CPU time to add_to */
void end_cpu_timing (const struct timespec *start_time, struct timespec *add_to);

void start_monotonic_timing(struct timespec *start_time);
void end_monotonic_timing (const struct timespec *start_time, struct timespec *add_to);

// stopwatch, returns elapsed time in milliseconds
void startWatch(struct timespec *start_time);
int64_t stopWatch(const struct timespec *start_time);

// get nanoseconds and some other stuff for use with srand
unsigned int get_seed();

// increment target by increment in ms, if result is in the past, set target to now.
// specialized function for scheduling threads using pthreadcondtimedwait
void incTimedwait(struct timespec *target, uint64_t increment);
void log_with_timestamp(const char *format, ...) __attribute__ ((format(printf, 1, 2)));

#endif
