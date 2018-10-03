CFLAGS=-std=c99 -W -Wall -O2 -ffast-math
LFLAGS=-s -lm
LFLAGS_LINUX=$(LFLAGS) -lasound
LFLAGS_SNDIO=$(LFLAGS) -lsndio
LFLAGS_OSSAUDIO=$(LFLAGS) -lossaudio
OBJ=common.o \
    ptrlist.o \
    symtab.o \
    parser.o \
    program.o \
    wave.o \
    generator.o \
    audiodev.o \
    wavfile.o \
    sgensys.o

all: sgensys

clean:
	rm -f $(OBJ) sgensys

sgensys: $(OBJ)
	@UNAME="`uname -s`"; \
	if [ $$UNAME = 'Linux' ]; then \
		echo "Linking for Linux (using ALSA and OSS)."; \
		$(CC) $(OBJ) $(LFLAGS_LINUX) -o sgensys; \
	elif [ $$UNAME = 'OpenBSD' ]; then \
		echo "Linking for OpenBSD (using sndio)."; \
		$(CC) $(OBJ) $(LFLAGS_SNDIO) -o sgensys; \
	elif [ $$UNAME = 'NetBSD' ]; then \
		echo "Linking for NetBSD (using OSS)."; \
		$(CC) $(OBJ) $(LFLAGS_OSSAUDIO) -o sgensys; \
	else \
		echo "Linking for generic UNIX (using OSS)."; \
		$(CC) $(OBJ) $(LFLAGS) -o sgensys; \
	fi

audiodev.o: audiodev.c audiodev/*.c audiodev.h common.h
	$(CC) -c $(CFLAGS) audiodev.c

common.o: common.c common.h
	$(CC) -c $(CFLAGS) common.c

generator.o: generator.c generator.h program.h wave.h osc.h math.h common.h
	$(CC) -c $(CFLAGS) generator.c

parser.o: parser.c parser.h symtab.h program.h ptrlist.h wave.h math.h common.h
	$(CC) -c $(CFLAGS) parser.c

program.o: program.c program.h ptrlist.h parser.h common.h
	$(CC) -c $(CFLAGS) program.c

ptrlist.o: ptrlist.c ptrlist.h common.h
	$(CC) -c $(CFLAGS) ptrlist.c

sgensys.o: sgensys.c generator.h program.h audiodev.h wavfile.h common.h
	$(CC) -c $(CFLAGS) sgensys.c

symtab.o: symtab.c symtab.h common.h
	$(CC) -c $(CFLAGS) symtab.c

wave.o: wave.c wave.h math.h common.h
	$(CC) -c $(CFLAGS) wave.c

wavfile.o: wavfile.c wavfile.h common.h
	$(CC) -c $(CFLAGS) wavfile.c
