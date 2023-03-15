all: build

ifdef PHH_DEBUG
OPTIMISATION_FLAGS = -g3 -ggdb -no-pie 
else
OPTIMISATION_FLAGS = -O3 -D_FORTIFY_SOURCE=2
endif

build: polite-hwclock-hctosys.o polite-hwclock-hctosys
	gcc -std=c17 $(OPTIMISATION_FLAGS) -o polite-hwclock-hctosys polite-hwclock-hctosys.o 

copy-bin:
	cp polite-hwclock-hctosys /usr/local/bin/


install-systemv: copy-bin
	cp polite-hwclock-hctosys.systemv /etc/init.d/polite-hwclock-hctosys

install-systemd: copy-bin
	cp polite-hwclock-hctosys.service /etc/systemd/system/

uninstall:
	rm -f /usr/local/bin/polite-hwclock-hctosys
	rm -f /etc/init.d/polite-hwclock-hctosys
	rm -f /etc/systemd/system/polite-hwclock-hctosys.service

.c.o:
	gcc -std=c17 -Wall -Werror -Wfatal-errors -fno-strict-aliasing -Wstrict-aliasing $(OPTIMISATION_FLAGS) -c $< -o $@ 

clean: 
	rm -f *.o polite-hwclock-hctosys
