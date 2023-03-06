all: build

ifdef PHH_DEBUG
OPTIMISATION_FLAGS = -g3 -ggdb -no-pie 
else
OPTIMISATION_FLAGS = -O3 -D_FORTIFY_SOURCE=2
endif

LIB_OBJECT_FILES = \
 

TEST_OBJECT_FILES = \

THIRD_PARTY_LIBS=

build: $(LIB_OBJECT_FILES) polite-hwclock-hctosys.o
	gcc -std=c17 $(OPTIMISATION_FLAGS) -o polite-hwclock-hctosys $(LIB_OBJECT_FILES) polite-hwclock-hctosys.o $(THIRD_PARTY_LIBS)

.c.o:
	gcc -std=c17 -Wall -Werror -Wfatal-errors -fno-strict-aliasing -Wstrict-aliasing $(OPTIMISATION_FLAGS) -c $< -o $@ 

clean: 
	rm -f *.o polite-hwclock-hctosys
