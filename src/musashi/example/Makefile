CC =     gcc
WARNINGS = -Wall -pedantic
CFLAGS = $(WARNINGS) -c -Iobj -I. -I..
LFLAGS = $(WARNINGS)

all: obj sim

clean:
	rm -rf obj
	rm -f sim

obj:
	mkdir obj

sim: obj/sim.o obj/m68kcpu.o obj/m68kops.o obj/m68kopac.o obj/m68kopdm.o obj/m68kopnz.o
	$(CC) $(LFLAGS) obj/sim.o obj/m68kcpu.o obj/m68kops.o obj/m68kopac.o obj/m68kopdm.o obj/m68kopnz.o -o sim

obj/sim.o: sim.c sim.h ../m68k.h ../m68kconf.h
	$(CC) $(CFLAGS) sim.c -o obj/sim.o

obj/m68kcpu.o: sim.h obj/m68kops.h sim.h ../m68k.h ../m68kconf.h
	$(CC) $(CFLAGS) ../m68kcpu.c -o obj/m68kcpu.o

obj/m68kops.o: obj/m68kmake obj/m68kops.h obj/m68kops.c sim.h ../m68k.h ../m68kconf.h
	$(CC) $(CFLAGS) obj/m68kops.c -o obj/m68kops.o

obj/m68kopac.o: obj/m68kmake obj/m68kops.h obj/m68kopac.c sim.h ../m68k.h ../m68kconf.h
	$(CC) $(CFLAGS) obj/m68kopac.c -o obj/m68kopac.o

obj/m68kopdm.o: obj/m68kmake obj/m68kops.h obj/m68kopdm.c sim.h ../m68k.h ../m68kconf.h
	$(CC) $(CFLAGS) obj/m68kopdm.c -o obj/m68kopdm.o

obj/m68kopnz.o: obj/m68kmake obj/m68kops.h obj/m68kopnz.c sim.h ../m68k.h ../m68kconf.h
	$(CC) $(CFLAGS) obj/m68kopnz.c -o obj/m68kopnz.o

obj/m68kops.h: obj/m68kmake
	obj/m68kmake obj ../m68k_in.c

obj/m68kmake: ../m68kmake.c ../m68k_in.c
	$(CC) $(WARNINGS) ../m68kmake.c -o obj/m68kmake
