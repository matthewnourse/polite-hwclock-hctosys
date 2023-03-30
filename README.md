# polite-hwclock-hctosys
Like `hwclock --hctosys`, but gradually like ntpd if possible.  You can run it as a daemon where it checks for time sync once a second, or as a once-off.  Originally designed to work around WSL2's clock skew.

Blog post: [https://www.nplus1.com.au/wsl2-clock-skew-fix/](https://www.nplus1.com.au/wsl2-clock-skew-fix/)

## Compiling
    cd polite-hwclock-hctosys
    make

You need a `gcc` or compatible that supports C17.  `clang` will probably work but I didn't test it.  Tested with `gcc` and Ubuntu 2022.04.


## Installing
For Systemd:

    sudo make install-systemd
    sudo systemctl enable polite-hwclock-hctosys


For System V init:

    sudo make install-systemv
    sudo update-rc.d polite-hwclock-hctosys defaults  # Debian, Ubuntu and friends
    sudo chkconfig polite-hwclock-hctosys on          # Red Hat, Centos etc

WSL2 does not have a "normal" System V startup process so you need to explicitly tell WSL2 to start `polite-hwclock-hctosys`.  For Windows 11, create/edit `/etc/wsl.conf` with these lines:

    [boot]
    command="/etc/init.d/polite-hwclock-hctosys start"

For Windows 10 you need to add the following to your `~/.bash_profile`:
    
    wsl.exe -u root /etc/init.d/polite-hwclock-hctosys start



## Uninstalling
    sudo make uninstall


## Starting
    sudo /etc/init.d/polite-hwclock-hctosys start     # System V
    sudo service polite-hwclock-hctosys start         # Systemd


## Single-shot, debugging and latest usage information
    polite-hwclock-hctosys once     # Run once and exit
    polite-hwclock-hctosys once -v  # Run once and say a lot about it
    polite-hwclock-hctosys          # Prints usage message.


Please submit bug and feature requests!
