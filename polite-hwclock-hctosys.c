/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * 
 */
/* This code is heavily based on techniques and ideas from hwclock from https://www.kernel.org/pub/linux/utils/util-linux/ */
#define _DEFAULT_SOURCE

#include <linux/rtc.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>

#define PROGRAM_NAME "polite-hwclock-hctosys"

#define LOG(sev__, fmt__, ...) fprintf(stderr, PROGRAM_NAME " %s: " sev__ ": " fmt__ "\n", get_log_time(), __VA_ARGS__)
#define LOG_ERROR(fmt__, ...) LOG("ERROR  ", fmt__ "  error=%s (%d)", __VA_ARGS__, strerror(errno), errno)
#define LOG_ERROR_NARG(fmt__) LOG_ERROR(fmt__ "%s", "")
#define LOG_INFO(fmt__, ...) LOG("INFO   ", fmt__, __VA_ARGS__)
#define LOG_VERBOSE(fmt__, ...) (global_is_verbose ? LOG("VERBOSE", fmt__, __VA_ARGS__) : 0)
#define LOG_VERBOSE_NARG(fmt__) (global_is_verbose ? LOG_VERBOSE(fmt__ "%s", "") : 0)

#define MAX_POLITE_ADJUSTMENT_DELTA_SEC (5 * 60)


int global_rtc_fd = -1;
bool global_is_verbose = false;
char global_log_buf[128];


const char *get_log_time() {
    time_t now;
    time(&now);
    ctime_r(&now, global_log_buf);
    char *newline_pos = strchr(global_log_buf, '\n');
    if (newline_pos) {
        *newline_pos = '\0';
    }

    return global_log_buf;
}


static int open_ro(const char *name) {
    assert(name);
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("Unable to open file %s read-only", name);
    }

    return fd;
}

static void rtc_time_to_tm(const struct rtc_time *rtc, struct tm *tm) {
    assert(rtc);
    assert(tm);

    /* Assume RTC clock is in UTC. */
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = rtc->tm_year;
    tm->tm_mon = rtc->tm_mon;
    tm->tm_mday = rtc->tm_mday;
    tm->tm_hour = rtc->tm_hour;
    tm->tm_min = rtc->tm_min;
    tm->tm_sec = rtc->tm_sec;
    tm->tm_isdst = rtc->tm_isdst;
    tm->tm_wday = rtc->tm_wday;
    tm->tm_yday = rtc->tm_yday;
}

static int tm_to_epoch(struct tm *tm, time_t *epoch) {
    assert(tm);
    assert(epoch);

    time_t tmp_epoch;

    /* Assume hardware clock is in UTC. */
    tmp_epoch = timegm(tm);
    if (-1 == tmp_epoch) {
        LOG_ERROR("timegm(tm) failed.  tm=%04d-%02d-%02d %02d:%02d:%02d  gmtoff=%ld isdst=%d wday=%d yday=%d zone=%s", 
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, (long)tm->tm_gmtoff, 
                tm->tm_isdst, tm->tm_wday, tm->tm_yday, tm->tm_zone);
        return -1;
    }

    *epoch = tmp_epoch;
    return 0;
}


static int open_rtc() {
    if (-1 != global_rtc_fd) {
        return global_rtc_fd;
    }

    global_rtc_fd = open_ro("/dev/rtc0");
    return global_rtc_fd;
}

static void close_rtc() {
    if (-1 != global_rtc_fd) {
        close(global_rtc_fd);
        global_rtc_fd = -1;
    }
}

static int read_rtc(struct rtc_time *time) {
    assert(time);

    int fd = open_rtc();
    if (fd < 0) {
        return -1;
    }

    int rc = ioctl(fd, RTC_RD_TIME, time);
    if (-1 == rc) {
        LOG_ERROR("Unable to read RTC via ioctl(%s)", "RTC_RD_TIME");
        return -1;
    }    

    return 0;
}

static int read_rtc_as_epoch(time_t *epoch) {
    LOG_VERBOSE_NARG("read_rtc_as_epoch");

    assert(epoch);

    struct rtc_time rtct;
    if (read_rtc(&rtct) != 0) {
        return -1;
    }

    struct tm tm;
    rtc_time_to_tm(&rtct, &tm);
    if (tm_to_epoch(&tm, epoch) != 0) {
        return -1;
    }

    return 0;
}

/* The hardware clock has a granularity of 1 second so we need to wait for the second to tick over before trying to do anything, so 
   we can be as accurate as possible. */
static int wait_for_rtc_tick() {
    LOG_VERBOSE_NARG("wait_for_rtc_tick");

    int fd = open_rtc();
    if (fd < 0) {
        return -1;
    }

    if (ioctl(fd, RTC_UIE_ON, 0) == -1) {
        LOG_ERROR("Unable to turn on clock tick interrupts via ioctl(%s)", "RTC_UIE_ON");
        return -1;
    }

    fd_set rtc_fds;
    FD_ZERO(&rtc_fds);
    FD_SET(fd, &rtc_fds);

    struct timeval tv;    
    const time_t timeout_sec = 10;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    int rc = select(fd + 1, &rtc_fds, NULL, NULL, &tv);
    
    if (0 == rc) {
        LOG_ERROR("Waiting for clock tick interrupt timed out.  timeout=%lld seconds", ((long long)timeout_sec));
    } else if (rc < 0) {
        LOG_ERROR("Waiting for clock tick interrupt failed.  timeout=%lld seconds", ((long long)timeout_sec));
    }

    if (ioctl(fd, RTC_UIE_OFF, 0) == -1) {
        LOG_ERROR("Unable to turn off clock tick interrupts via ioctl(%s)", "RTC_UIE_OFF");
    }
    
    return (rc > 0) ? 0 : -1;
}

static int get_hardware_now(time_t *epoch) {
    LOG_VERBOSE_NARG("get_hardware_now");

    if ((wait_for_rtc_tick() != 0) || (read_rtc_as_epoch(epoch) != 0)) {
        return -1;
    }

    return 0;
}
 
static int get_system_now(time_t *epoch) {
    LOG_VERBOSE_NARG("get_system_now");

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        LOG_ERROR_NARG("gettimeofday failed");
        return -1;
    }

    *epoch = tv.tv_sec;
    return 0;
}
 
static int get_times(time_t *hw, time_t *sys) {
    LOG_VERBOSE_NARG("get_times");

    assert(hw);
    assert(sys);

    /* This funciton is extremely time-sensitive.  We need to get the system time ASAP after getting the hardware time.
       Don't be tempted to get the system time first because we need to wait for the hardware clock to tick. */

    time_t tmp_hw = -1;
    time_t tmp_sys = -1;

    if (get_hardware_now(&tmp_hw) != 0) {
        return -1;
    }

    if (get_system_now(&tmp_sys) != 0) {
        return -1;
    }

    LOG_VERBOSE("get_times: hw=%lld sys=%lld", ((long long)tmp_hw), ((long long)tmp_sys));

    *hw = tmp_hw;
    *sys = tmp_sys;
    return 0;
}

static time_t calculate_delta(size_t hw_time, size_t sys_time) {
    return hw_time - sys_time;
}

static int get_delta(time_t *delta) {
    assert(delta);

    time_t hw_now;
    time_t sys_now;

    if (get_times(&hw_now, &sys_now) != 0) {
        return -1;
    }

    *delta = calculate_delta(hw_now, sys_now);
    return 0;
}


static int get_current_time_adjustment_delta(time_t *current_delta) {
    assert(current_delta);
    /* There's an old bug where adjtime(NULL &old) would not set old.  We're super-unlikely to encounter it because 
       it's back in Linux 2.6 so old but just in case we'll set old to a big value. */
    struct timeval old = { .tv_sec = LONG_MAX, .tv_usec = 0 };
    if (adjtime(NULL, &old) != 0) {
        LOG_ERROR_NARG("Unable to get current adjtime delta");
        return -1;
    }

    return 0;
}


static int polite_set_time() {
    LOG_VERBOSE_NARG("polite_set_time");

    time_t delta;
    if (get_delta(&delta) != 0) {
        return -1;
    }

    if (0 == delta) {
        LOG_VERBOSE_NARG("0 == delta");
    } else {
        struct timeval old = { 0, 0 };
        struct timeval tv;
        tv.tv_sec = delta;
        tv.tv_usec = 0;        
        if (adjtime(&tv, &old) != 0) {
            LOG_ERROR("Unable to adjust time politely.  delta=%lld sec", ((long long)delta));
            return -1;
        }

        LOG_INFO("Time is adjusting politely.  delta=%lld sec  old=%lld sec", ((long long)delta), ((long long)old.tv_sec));        
    }

    return 0;
}

static int impolite_set_time() {
    LOG_VERBOSE_NARG("impolite_set_time");

    time_t hw_now;
    time_t sys_now;

    if (get_times(&hw_now, &sys_now) != 0) {
        return -1;
    }
   
    time_t delta = calculate_delta(hw_now, sys_now);
    if (0 == delta) {
        LOG_VERBOSE_NARG("0 == delta");
    } else {
        struct timeval tv;
        tv.tv_sec = hw_now;
        tv.tv_usec = 0;
        if (settimeofday(&tv, NULL) != 0) {
            LOG_ERROR("Unable to set time impolitely.  delta=%lld sec", ((long long)delta));
            return -1;
        }

        LOG_INFO("Adjusted time impolitely.  delta=%lld sec", ((long long)delta));
    }

    return 0;
}

static bool has_same_sign(time_t one, time_t two) {
    if ((0 == one) && (0 == two)) {
        return true;
    }

    if ((one > 0) && (two > 0)) {
       return true;
    }
   
    if  ((one < 0) && (two < 0)) {
        return true;
    }

    return false;
}

static int set_time() {
    LOG_VERBOSE_NARG("set_time");
    time_t delta;
    if (get_delta(&delta) != 0) {
        return -1;
    }

    if (0 == delta) {
        LOG_VERBOSE_NARG("No work to do, there is no delta");
        return 0;
    }

    time_t current_adjtime_delta = LONG_MAX;
    if (get_current_time_adjustment_delta(&current_adjtime_delta) != 0) {
        return -1;
    }

    if (has_same_sign(delta, current_adjtime_delta)) {
        LOG_VERBOSE("delta=%lld current_adjtime_delta=%lld, they have the same sign, will leave the situation as-is for now", ((long long)delta), ((long long)current_adjtime_delta));
        return 0;
    }

    LOG_VERBOSE("delta=%lld max_polite_delta=%d", ((long long)delta), MAX_POLITE_ADJUSTMENT_DELTA_SEC);

    if ((delta < 0) && (llabs(delta) > MAX_POLITE_ADJUSTMENT_DELTA_SEC)) {
        LOG_ERROR("delta=%lld and is outside the polite adjustment limit.  I will not jolt the clock backwards, reboot recommended.", ((long long)delta));
        return -1;
    }

    int result = (llabs(delta) <= MAX_POLITE_ADJUSTMENT_DELTA_SEC) ? polite_set_time() : impolite_set_time();
    if ((0 == result) && global_is_verbose) {
        LOG_VERBOSE_NARG("set_time: success.  Will re-get times for the log");
        time_t hw_now;
        time_t sys_now;
        get_times(&hw_now, &sys_now);
    }

    return result;
}
    

void print_usage(const char *argv[]) {
    fprintf(stderr, 
            "%s [-v]\nLike hwclock -s, but gradually like ntpd if the time delta <= %d seconds.\n"
                "Will refuse to jolt the clock backwards.\n"
                "Assumes that hardware clock is in UTC.\n", 
            argv[0], MAX_POLITE_ADJUSTMENT_DELTA_SEC);
}


int main(int argc, const char *argv[]) {
    if ((argc != 1) && (argc != 2)) {
        print_usage(argv);
        return -1;
    }

    if (2 == argc) {
        if (strcmp(argv[1], "-v") == 0) {
            global_is_verbose = true;
        } else {
            print_usage(argv);
            return -1;
        }
    }

    int ret = set_time();

    close_rtc();

    return ret;
}




