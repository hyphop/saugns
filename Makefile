.POSIX:
CC=cc
CFLAGS=-std=c99 -W -Wall -O2
CFLAGS_FAST=$(CFLAGS) -ffast-math
LFLAGS=-s -lm
LFLAGS_LINUX=$(LFLAGS) -lasound
LFLAGS_SNDIO=$(LFLAGS) -lsndio
LFLAGS_OSSAUDIO=$(LFLAGS) -lossaudio
OBJ=renderer/audiodev.o renderer/wavfile.o renderer/renderer.o \
    interp/generator.o \
    common.o ptrarr.o wave.o \
    mgensys.o parser.o symtab.o

all: mgensys

clean:
	rm -f $(OBJ) mgensys

common.o: common.c common.h
	$(CC) -c $(CFLAGS) common.c

mgensys: $(OBJ)
	@UNAME="`uname -s`"; \
	if [ $$UNAME = 'Linux' ]; then \
		echo "Linking for Linux (using ALSA and OSS)."; \
		$(CC) $(OBJ) $(LFLAGS_LINUX) -o mgensys; \
	elif [ $$UNAME = 'OpenBSD' ]; then \
		echo "Linking for OpenBSD (using sndio)."; \
		$(CC) $(OBJ) $(LFLAGS_SNDIO) -o mgensys; \
	elif [ $$UNAME = 'NetBSD' ]; then \
		echo "Linking for NetBSD (using OSS)."; \
		$(CC) $(OBJ) $(LFLAGS_OSSAUDIO) -o mgensys; \
	else \
		echo "Linking for UNIX with OSS."; \
		$(CC) $(OBJ) $(LFLAGS) -o mgensys; \
	fi

interp/generator.o: common.h interp/generator.c interp/osc.h math.h mgensys.h program.h wave.h
	$(CC) -c $(CFLAGS_FAST) interp/generator.c -o interp/generator.o

renderer/audiodev.o: common.h renderer/audiodev.c renderer/audiodev.h renderer/audiodev/*.c
	$(CC) -c $(CFLAGS) renderer/audiodev.c -o renderer/audiodev.o

renderer/renderer.o: common.h renderer/audiodev.h renderer/renderer.c renderer/wavfile.h math.h mgensys.h ptrarr.h
	$(CC) -c $(CFLAGS_FAST) renderer/renderer.c -o renderer/renderer.o

renderer/wavfile.o: common.h renderer/wavfile.c renderer/wavfile.h
	$(CC) -c $(CFLAGS) renderer/wavfile.c -o renderer/wavfile.o

mgensys.o: common.h mgensys.c mgensys.h ptrarr.h
	$(CC) -c $(CFLAGS) mgensys.c

parser.o: common.h math.h mgensys.h parser.c program.h symtab.h wave.h
	$(CC) -c $(CFLAGS) parser.c

ptrarr.o: common.h ptrarr.c ptrarr.h
	$(CC) -c $(CFLAGS) ptrarr.c

symtab.o: common.h mgensys.h symtab.c symtab.h
	$(CC) -c $(CFLAGS) symtab.c

wave.o: common.h math.h wave.c wave.h
	$(CC) -c $(CFLAGS_FAST) wave.c
