# Two consoles with RetroArch in one process, for gambatte_plus's Cable Link
# (see dualtest.c). "make" builds it for the Miyoo Mini (run it with
# qemu-arm and the core built with platform=miyoomini), "make native" for
# this computer (with the core built with platform=unix).
TC ?= /opt/miyoomini-toolchain/usr/bin/arm-linux
INC = -I../../libgambatte/libretro-common/include

all: dualtest

dualtest: dualtest.c
	$(TC)-gcc -O2 -Wall $(INC) -o $@ $< -ldl -lpthread

native: dualtest-native

dualtest-native: dualtest.c
	gcc -O2 -Wall $(INC) -o $@ $< -ldl -lpthread

clean:
	rm -f dualtest dualtest-native
