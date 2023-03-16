# polite-hwclock-hctosys

Like `hwclock --hctosys`, but gradually like ntpd if possible.  Originally
designed to work around WSL2's clock skew.


WSL2 clock drift pain
---------------------

I've been having a lot of trouble with WSL2 clock drift. I'm running Ubuntu
2022.04 on the latest WSL2 kernel on Windows 11. WSL2's time drifts from
Windows time without limit. I've found it to be hours different, especially
after suspend or hibernate. I originally noticed the problem when connecting
to an Amazon EC2 instance via AWS Session Manager and using a Single-Sign-On
(SSO) system for authentication. SSH would report corrupt packets, then I'd
get this error when attempting to reconnect:

    An error occurred (403) when calling the StartSession operation: Server
    authentication failed: <UnauthorizedRequest><message>Forbidden.
    </message></UnauthorizedRequest>

...even after successfully authenticating with the SSO. This sort of thing
really drains ones life force.


The recommended fixes
---------------------

The widely recommended fixes are to upgrade to the latest WSL2 kernel, restart
WSL2, run `hwclock -s` regularly, or install Systemd and a Network Time
Protocol (NTP) daemon. See for example
[here](https://github.com/microsoft/WSL/issues/4677) and
[here](https://github.com/microsoft/WSL/issues/7255]). However, none of these
particularly appeal to me...

**Upgrading to latest WSL2 kernel?** Already did it. The problem persists.

**Restart WSL2?** I need a bunch of WSL2 tabs and panes open. Closing and
reopening them all is painful and my WSL2 clock drifts several times a day. I
don't hate myself enough to restart WSL2 several times a day.

**hwclock -s ?** `hwclock`'s own `man` page says that `hwclock -s` is risky:

    This function should never be used on a running system. Jumping system time
    will cause problems, such as corrupted filesystem timestamps.

While all the big WSL2 clock skews I've seen have the WSL2 clock running slow,
what really troubles me is the chance of hwclock jumping the time backwards by
several minutes. This cannot be safe.

Also, for big clock drifts (eg right after a hibernate) we need to correct the
clock as soon as possible rather than waiting for cron or a human to wake up
and run `hwclock`.

**Install Systemd and a Network Time Protocol (NTP) daemon?**

* I'm not convinced that adding Systemd to my WSL2 will play nicely with its
  existing init system.
* I am not over-endowed with disk space on this machine instance and I don't
  have the power to add more.
* I want my WSL2 clock to be in sync with my Windows 11 clock not necessarily
  NTP. In theory the Windows 11 clock should be synced with NTP but I don't
  have complete control over this Windows 11 instance.
* Most importantly, NTP in general and `ntpd` in particular operate under the
  assumption that large clock skews are rare. `ntpd` considers clock skews of
  more than 1000 seconds as "terribly wrong" and will exit by default. Even if
  configured to override this check & soldier on, `ntpd` will set the time
  once and then exit [(details)](https://docs.ntpsec.org/latest/ntpd.html).
  Big clock skews are the norm for WSL2 so `ntpd` would quickly stop running.


How does hwclock -s set the time?
---------------------------------

On Linux, `hwclock -s` reads the hardware clock from /dev/rtc0 via an `ioctl`
call. It sets the system clock using `settimeofday`.


How does ntpd do it?
--------------------

`ntpd` adjusts for small (<128ms by default) time differences using the
`adjtime` syscall. `adjtime` asks the kernel to gradually adjust the system
time by a number of microseconds. "Gradual" is the key here- it takes about
2000s to adjust the time for every 1s difference. For larger clock skews,
`ntpd` will use `settimeofday` to jump the time just like `hwclock -s` does.
Again, this behaviour makes me twitchy because WSL2 clock skews are large and
common. I really don't want the clock jumping backwards.


Enter polite-hwclock-hctosys
----------------------------

`polite-hwclock-hctosys` is a small Linux daemon that:

* Reads the Real-Time Clock (RTC) from /dev/rtc0 just like `hwclock -s` (also
  known as `hwclock --hctosys`)
* If the time skew is less than 1 second, do nothing
* If the time skew is between 1 and 5 seconds, use `adjtime` to adjust the
  system time, just like `ntpd`
* If the time skew is more than 5 seconds and the required adjustment is to
  move the clock forwards, use `settimeofday` just like `hwclock -s` and
  `ntpd`.
* If the time skew is more than 5 seconds and the required adjustment is to
  move the clock backwards, write an error message and do nothing. WSL2 really
  does need a reboot.

You can run `polite-hwclock-hctosys` in single-shot mode, as a Systemd daemon,
or as a System V daemon.


Getting polite-hwclock-hctosys
------------------------------

[https://github.com/matthewnourse/polite-hwclock-hctosys](https://github.com/matthewnourse/polite-hwclock-hctosys)


Compiling polite-hwclock-hctosys
--------------------------------

    cd polite-hwclock-hctosys
    make

You need a `gcc` or compatible that supports C17. `clang` will probably work
but I didn't test it. Tested with `gcc` and Ubuntu 2022.04.


Installing polite-hwclock-hctosys
---------------------------------

For System V init:

    sudo make install-systemv
    sudo systemctl enable polite-hwclock-hctosys

For Systemd:

    sudo make install-systemd
    sudo update-rc.d polite-hwclock-hctosys defaults  # Debian, Ubuntu and friends
    sudo chkconfig polite-hwclock-hctosys on          # Red Hat, Centos etc


Uninstalling polite-hwclock-hctosys
-----------------------------------

    sudo make uninstall


Starting polite-hwclock-hctosys
-------------------------------

    sudo /etc/init.d/polite-hwclock-hctosys start     # System V
    sudo service polite-hwclock-hctosys start         # Systemd


Single-shot, debugging and latest usage information
---------------------------------------------------

    polite-hwclock-hctosys once     # Run once and exit
    polite-hwclock-hctosys once -v  # Run once and say a lot about it
    polite-hwclock-hctosys          # Prints usage message.

Please submit bug and feature requests! `polite-hwclock-hctosys` works great
for me but its parameters aren't configurable. Please submit a bug or feature
request via GitHub if you need configurable parameters.
