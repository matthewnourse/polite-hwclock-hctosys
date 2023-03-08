/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * 
 */
/* This code is heavily based on techniques and ideas from hwclock: https://www.kernel.org/pub/linux/utils/util-linux/ */
#define _DEFAULT_SOURCE

#include <linux/rtc.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <syslog.h>
#include <stdarg.h>


#define PROGRAM_NAME "polite-hwclock-hctosys"

#define LOG_WRITE_SYSTEM_V(sev__, fmt__, ...) syslog(severity_to_system_v_severity(sev__), fmt__, __VA_ARGS__)
#define LOG_WRITE_SYSTEMD(sev__, fmt__, ...) fprintf(stderr, "%s" PROGRAM_NAME ": " fmt__ "\n", severity_to_systemd_severity(sev__), __VA_ARGS__)
#define LOG_WRITE_STANDALONE(sev__, fmt__, ...) fprintf(stderr, "%s %s" fmt__ "\n", get_log_time(), severity_to_human_readable_severity(sev__), __VA_ARGS__)
#define LOG_WRITE_RUN_MODE_SPECIFIC(sev__, fmt__, ...) ((RUN_MODE_SYSTEM_V == global_run_mode) ? LOG_WRITE_SYSTEM_V(sev__, fmt__, __VA_ARGS__) : ((RUN_MODE_SYSTEMD == global_run_mode) ? LOG_WRITE_SYSTEMD(sev__, fmt__, __VA_ARGS__) : LOG_WRITE_STANDALONE(sev__, fmt__, __VA_ARGS__)))

#define LOG_WRITE(sev__, fmt__, ...) LOG_WRITE_RUN_MODE_SPECIFIC(sev__, fmt__, __VA_ARGS__)
#define LOG_WRITE_ERROR(fmt__, ...) LOG_WRITE(LOG_SEVERITY_ERROR, fmt__ "  error=%s (%d)", __VA_ARGS__, strerror(errno), errno)
#define LOG_WRITE_ERROR_NARG(fmt__) LOG_WRITE_ERROR(fmt__ "%s", "")
#define LOG_WRITE_INFO(fmt__, ...) LOG_WRITE(LOG_SEVERITY_INFO, fmt__, __VA_ARGS__)
#define LOG_WRITE_VERBOSE(fmt__, ...) (global_is_verbose ? LOG_WRITE(LOG_SEVERITY_DEBUG, fmt__, __VA_ARGS__) : 0)
#define LOG_WRITE_VERBOSE_NARG(fmt__) (global_is_verbose ? LOG_WRITE_VERBOSE(fmt__ "%s", "") : 0)

#define USEC_FMT PRId64

#define MAX_POLITE_ADJUSTMENT_DELTA_SEC 30
#define LOOP_POLL_SEC 5



typedef enum {
    RUN_MODE_SYSTEM_V,
    RUN_MODE_SYSTEMD,
    RUN_MODE_ONCE
} run_mode_t;

typedef enum {
    /* Values chosen to match the systemd and system V severity values. */
    LOG_SEVERITY_ERROR = 3,
    LOG_SEVERITY_INFO = 6,
    LOG_SEVERITY_DEBUG = 7
} log_severity_t;


int global_rtc_fd = -1;
bool global_is_verbose = false;
char global_log_buf[128] = { '\0' };
run_mode_t global_run_mode = RUN_MODE_ONCE;

static int64_t sec_to_usec(int64_t sec) {
    return sec * 1000 * 1000;
}

static int64_t usec_to_sec(int64_t usec) {
    return (usec / 1000) / 1000;
}


static const char *get_log_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    localtime_r(&tv.tv_sec, &tm);

    snprintf(global_log_buf, sizeof(global_log_buf) - 1, "%04d-%02d-%02d %02d:%02d:%02d.%06ld", 
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tv.tv_usec);

    return global_log_buf;
}

static int severity_to_system_v_severity(log_severity_t sev) {
    switch (sev) {
        case LOG_SEVERITY_ERROR:
            return LOG_ERR;

        case LOG_SEVERITY_INFO:
            return LOG_INFO;

        case LOG_SEVERITY_DEBUG:
            return LOG_DEBUG;
    }

    assert(0);
    return LOG_DEBUG;
}

static const char *severity_to_systemd_severity(log_severity_t sev) {
    /* We could include sd-daemon.h for the SD_* severity definitions but we want to be compilable without Systemd. */
    switch (sev) {
        case LOG_SEVERITY_ERROR:
            return "<3>";

        case LOG_SEVERITY_INFO:
            return "<6>";

        case LOG_SEVERITY_DEBUG:
            return "<7>";
    }

    assert(0);
    return "<7>";
}

static const char *severity_to_human_readable_severity(log_severity_t sev) {
    switch (sev) {
        case LOG_SEVERITY_ERROR:
            return "ERROR: ";

        case LOG_SEVERITY_INFO:
            return "INFO:  ";

        case LOG_SEVERITY_DEBUG:
            return "DEBUG: ";
    }

    assert(0);
    return "DEBUG: ";
}

static int open_ro(const char *name) {
    assert(name);
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        LOG_WRITE_ERROR("Unable to open file %s read-only", name);
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

static int tm_to_epoch_usec(struct tm *tm, int64_t *epoch_usec) {
    assert(tm);
    assert(epoch_usec);

    time_t epoch_sec;

    /* Assume hardware clock is in UTC. */
    epoch_sec = timegm(tm);
    if (-1 == epoch_sec) {
        LOG_WRITE_ERROR("timegm(tm) failed.  tm=%04d-%02d-%02d %02d:%02d:%02d  gmtoff=%ld isdst=%d wday=%d yday=%d zone=%s", 
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, (long)tm->tm_gmtoff, 
                tm->tm_isdst, tm->tm_wday, tm->tm_yday, tm->tm_zone);
        return -1;
    }

    *epoch_usec = sec_to_usec(epoch_sec);
    return 0;
}

static int64_t tv_to_epoch_usec(const struct timeval *tv) {
    assert(tv);
    int64_t epoch_usec = sec_to_usec(tv->tv_sec);
    epoch_usec += tv->tv_usec;
    return epoch_usec;
}

static void epoch_usec_to_tv(const int64_t epoch_usec, struct timeval *tv) {
    assert(tv);
    tv->tv_sec = usec_to_sec(epoch_usec);
    tv->tv_usec = epoch_usec - sec_to_usec(tv->tv_sec);
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
        LOG_WRITE_ERROR("Unable to read RTC via ioctl(%s)", "RTC_RD_TIME");
        return -1;
    }    

    return 0;
}

static int read_rtc_as_epoch_usec(int64_t *epoch_usec) {
    LOG_WRITE_VERBOSE_NARG("read_rtc_as_epoch");

    assert(epoch_usec);

    struct rtc_time rtct;
    if (read_rtc(&rtct) != 0) {
        return -1;
    }

    struct tm tm;
    rtc_time_to_tm(&rtct, &tm);
    if (tm_to_epoch_usec(&tm, epoch_usec) != 0) {
        return -1;
    }

    return 0;
}

static int enable_rtc_tick_interrupt(int fd) {
    if (ioctl(fd, RTC_UIE_ON, 0) == -1) {
        LOG_WRITE_ERROR("Unable to turn on clock tick interrupts via ioctl(%s)", "RTC_UIE_ON");
        return -1;
    }

    return 0;
}

static void disable_rtc_tick_interrupt(int fd) {
    if (ioctl(fd, RTC_UIE_OFF, 0) == -1) {
        LOG_WRITE_ERROR("Unable to turn off clock tick interrupts via ioctl(%s)", "RTC_UIE_OFF");
    }
}

static int select_on_rtc(int fd) {
    fd_set rtc_fds;
    FD_ZERO(&rtc_fds);
    FD_SET(fd, &rtc_fds);

    struct timeval tv;    
    const time_t timeout_sec = 10;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    int rc = select(fd + 1, &rtc_fds, NULL, NULL, &tv);
    
    if (0 == rc) {
        LOG_WRITE_ERROR("Waiting for clock tick interrupt timed out.  timeout=%lld seconds", ((long long)timeout_sec));
        return -1;
    } 
    
    if (rc < 0) {
        LOG_WRITE_ERROR("Waiting for clock tick interrupt failed.  timeout=%lld seconds", ((long long)timeout_sec));
        return -1;
    } 

    return 0;
}

static int read_interrupt_info_from_rtc(int fd) {
    unsigned long interrupt_info;
    if (read(fd, &interrupt_info, sizeof(interrupt_info)) != sizeof(interrupt_info)) {
        LOG_WRITE_ERROR_NARG("read() on RTC failed");
        return -1;
    } 

    /* Least significant byte contains the interrupt that fired. */
    uint8_t interrupt = (uint8_t)interrupt_info;
    LOG_WRITE_VERBOSE("read() on RTC returned interrupt bitmask=0x%02x", (unsigned int)interrupt);
    return 0;
}
    

/* The hardware clock has a granularity of 1 second so we need to wait for the second to tick over before trying to do 
   anything, so we can be as accurate as possible. */
static int wait_for_rtc_tick() {
    LOG_WRITE_VERBOSE_NARG("wait_for_rtc_tick");

    int fd = open_rtc();
    if (fd < 0) {
        return -1;
    }

    if (enable_rtc_tick_interrupt(fd) != 0) {
        return -1;
    }

    /* We need to read() from the RTC fd after select()ing to reset it so the select() will wait next time. */
    bool success = ((select_on_rtc(fd) == 0) && (read_interrupt_info_from_rtc(fd) == 0));
    disable_rtc_tick_interrupt(fd);
   
    return success ? 0 : -1;
}

static int get_hardware_now(int64_t *epoch_usec) {
    LOG_WRITE_VERBOSE_NARG("get_hardware_now");

    assert(epoch_usec);

    if ((wait_for_rtc_tick() != 0) || (read_rtc_as_epoch_usec(epoch_usec) != 0)) {
        return -1;
    }

    return 0;
}
 
static int get_system_now(int64_t *epoch_usec) {
    LOG_WRITE_VERBOSE_NARG("get_system_now");

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        LOG_WRITE_ERROR_NARG("gettimeofday failed");
        return -1;
    }

    *epoch_usec = tv_to_epoch_usec(&tv);
    return 0;
}
 
static int get_times(int64_t *hw, int64_t *sys) {
    LOG_WRITE_VERBOSE_NARG("get_times");

    assert(hw);
    assert(sys);

    /* This funciton is extremely time-sensitive.  We need to get the system time ASAP after getting the hardware time.
       Don't be tempted to get the system time first because we need to wait for the hardware clock to tick. */

    int64_t tmp_hw = -1;
    int64_t tmp_sys = -1;

    if (get_hardware_now(&tmp_hw) != 0) {
        return -1;
    }

    if (get_system_now(&tmp_sys) != 0) {
        return -1;
    }

    LOG_WRITE_VERBOSE("get_times: hw=%" USEC_FMT " sys=%" USEC_FMT, tmp_hw, tmp_sys);

    *hw = tmp_hw;
    *sys = tmp_sys;
    return 0;
}

static int64_t calculate_delta(int64_t hw_time, int64_t sys_time) {
    return hw_time - sys_time;
}

static int get_delta(int64_t *delta) {
    assert(delta);

    int64_t hw_now;
    int64_t sys_now;

    if (get_times(&hw_now, &sys_now) != 0) {
        return -1;
    }

    *delta = calculate_delta(hw_now, sys_now);
    return 0;
}


static int get_current_time_adjustment_delta(int64_t *current_delta) {
    LOG_WRITE_VERBOSE_NARG("get_current_time_adjustment_delta");

    assert(current_delta);
    /* There's an old bug where adjtime(NULL &old) would not set old.  We're super-unlikely to encounter it because 
       it's back in Linux 2.6 so old but just in case we'll set old to a big value. */
    struct timeval old = { .tv_sec = LONG_MAX, .tv_usec = 0 };
    if (adjtime(NULL, &old) != 0) {
        LOG_WRITE_ERROR_NARG("Unable to get current adjtime delta");
        return -1;
    }

    *current_delta = tv_to_epoch_usec(&old);
    LOG_WRITE_VERBOSE("current_adjtime_delta=%" USEC_FMT, *current_delta);
    return 0;
}


static int polite_set_time(int64_t delta) {
    LOG_WRITE_VERBOSE("polite_set_time, delta=%" USEC_FMT, delta);

    if (0 == delta) {
        LOG_WRITE_VERBOSE_NARG("0 == delta");
    } else {
        struct timeval old = { 0, 0 };
        struct timeval tv;
        epoch_usec_to_tv(delta, &tv);
        if (adjtime(&tv, &old) != 0) {
            LOG_WRITE_ERROR("Unable to adjust time politely.  delta=%" USEC_FMT " usec", delta);
            return -1;
        }

        LOG_WRITE_INFO("Time is adjusting politely.  delta=%" USEC_FMT " usec  old=%" USEC_FMT " usec", 
                delta, tv_to_epoch_usec(&old));
    }

    return 0;
}

static int impolite_set_time() {
    LOG_WRITE_VERBOSE_NARG("impolite_set_time");

    int64_t hw_now;
    int64_t sys_now;

    if (get_times(&hw_now, &sys_now) != 0) {
        return -1;
    }
   
    int64_t delta = calculate_delta(hw_now, sys_now);
    if (0 == delta) {
        LOG_WRITE_VERBOSE_NARG("0 == delta");
        return 0;
    }
    
    if (delta < 0) {
        LOG_WRITE_ERROR("delta=%" USEC_FMT " usec, will not step the clock backwards.  Reboot recommended.", delta);
        return -1;
    } 

    struct timeval tv;
    epoch_usec_to_tv(hw_now, &tv);
    if (settimeofday(&tv, NULL) != 0) {
        LOG_WRITE_ERROR("Unable to set time impolitely.  delta=%" USEC_FMT " usec", delta);
        return -1;
    }

    LOG_WRITE_INFO("Adjusted time impolitely.  delta=%" USEC_FMT " usec", delta);
    return 0;
}

static bool has_same_sign(int64_t one, int64_t two) {
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

static int64_t max_polite_adjustment_delta_usec() {
    return sec_to_usec(MAX_POLITE_ADJUSTMENT_DELTA_SEC);
}

static int set_time() {
    LOG_WRITE_VERBOSE_NARG("set_time");
    int64_t delta;
    if (get_delta(&delta) != 0) {
        return -1;
    }

    if (0 == delta) {
        LOG_WRITE_VERBOSE_NARG("No work to do, there is no delta");
        return 0;
    }

    int64_t current_adjtime_delta = INT64_MAX;
    if (get_current_time_adjustment_delta(&current_adjtime_delta) != 0) {
        return -1;
    }

    if (has_same_sign(delta, current_adjtime_delta)) {
        LOG_WRITE_VERBOSE("delta=%" USEC_FMT " current_adjtime_delta=%" USEC_FMT
                        ", they have the same sign, will leave the situation as-is for now", 
                    delta, current_adjtime_delta);
        return 0;
    }

    LOG_WRITE_VERBOSE("delta=%" USEC_FMT " usec max_polite_delta=%" USEC_FMT " usec", 
            delta, max_polite_adjustment_delta_usec());

    int result = (llabs(delta) <= max_polite_adjustment_delta_usec()) ? polite_set_time(delta) : impolite_set_time();
    if ((0 == result) && global_is_verbose) {
        LOG_WRITE_VERBOSE_NARG("set_time: success.  Will re-get times for the log");
        int64_t hw_now;
        int64_t sys_now;
        get_times(&hw_now, &sys_now);
    }

    return result;
}

static void ignore_irrelevant_signals() {
    /* Ignore signals that don't really matter to us. */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
}



static int run_forever() {
    int result;
    while (0 == (result = set_time())) {
        LOG_WRITE_VERBOSE("Sleeping for %d seconds", LOOP_POLL_SEC);
        sleep(LOOP_POLL_SEC);
    }

    return result;
}

static int run_system_v() {
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        /* parent */
        exit(EXIT_SUCCESS);
    }

    /* child */
    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    ignore_irrelevant_signals();

    /* Fork off for the second time*/
    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        /* parent (ie middle parent not grandparent) */
        exit(EXIT_SUCCESS);
    }

    /* child (ie grandchild) */
    /* We don't need to create any files. */
    umask(0);

    /* Change the working directory to the root directory to avoid holding a lock on the original working directory. */
    if (chdir("/") != 0) {
        LOG_WRITE_ERROR_NARG("chdir() failed");
        exit(EXIT_FAILURE);
    }

    /* Close all open file descriptors */
    int fd;
    for (fd = sysconf(_SC_OPEN_MAX); fd >= 0; fd--) {
        close(fd);
    }

    openlog (PROGRAM_NAME, LOG_PID, LOG_DAEMON);

    return run_forever();
}

static int run_systemd() {
    ignore_irrelevant_signals();
    return run_forever();
}

static int run() {
    switch (global_run_mode) {
        case RUN_MODE_SYSTEM_V:
            return run_system_v();
            
        case RUN_MODE_SYSTEMD:
            return run_systemd();

        case RUN_MODE_ONCE:
            return set_time();
    }

    assert(false);
    return -1;
}
    

void print_usage(const char *argv[]) {
    fprintf(stderr, 
            "%s <systemv|systemd|once> [-v]\n\nLike hwclock -s, but gradually like ntpd if the time delta <= %d seconds.\n"        
                "Will refuse to jolt the clock backwards.\n"
                "Assumes that hardware clock is in UTC.\n"
                "\n"
                "systemv: run as a System V daemon.  Useful on WSL2 which doesn't tend to have Systemd.\n"
                "systemd: run as a Systemd daemon (ie log to stderr & don't detach).\n"
                "once:    just check & adjust the time once.\n"
                "-v:      verbose output.\n", 
            argv[0], MAX_POLITE_ADJUSTMENT_DELTA_SEC);
}


int main(int argc, const char *argv[]) {
    if ((argc != 2) && (argc != 3)) {
        print_usage(argv);
        return -1;
    }

    const char *mode = argv[1];
    if (strcmp(mode, "systemv") == 0) {
        global_run_mode = RUN_MODE_SYSTEM_V;
    } else if (strcmp(mode, "systemd") == 0) {
        global_run_mode = RUN_MODE_SYSTEMD;
    } else if (strcmp(mode, "once") == 0) {
        global_run_mode = RUN_MODE_ONCE;
    } else {
        fprintf(stderr, "Invalid mode: %s\n", mode);
        print_usage(argv);
        return -1;
    }

    if (3 == argc) {        
        if (strcmp(argv[2], "-v") == 0) {
            global_is_verbose = true;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[2]);
            print_usage(argv);
            return -1;
        }
    }

    int ret = run();

    close_rtc();

    return ret;
}




