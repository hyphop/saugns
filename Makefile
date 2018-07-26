CFLAGS=-W -Wall -Werror=implicit-function-declaration -O2 -ffast-math
LFLAGS=-s -lm
LFLAGS_LINUX=$(LFLAGS) -lasound
LFLAGS_OSSAUDIO=$(LFLAGS) -lossaudio
OBJ=plist.o \
    symtab.o \
    parser.o \
    program.o \
    interpreter.o \
    wave.o \
    generator.o \
    renderer.o \
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
	elif [ $$UNAME = 'OpenBSD' ] || [ $$UNAME = 'NetBSD' ]; then \
		echo "Linking for OpenBSD or NetBSD (using OSS)."; \
		$(CC) $(OBJ) $(LFLAGS_OSSAUDIO) -o sgensys; \
	else \
		echo "Linking for generic UNIX (using OSS)."; \
		$(CC) $(OBJ) $(LFLAGS) -o sgensys; \
	fi

audiodev.o: audiodev.c audiodev_*.c audiodev.h sgensys.h
	$(CC) -c $(CFLAGS) audiodev.c

program.o: program.c program.h parser.h plist.h wave.h math.h sgensys.h
	$(CC) -c $(CFLAGS) program.c

generator.o: generator.c generator.h osc.h wave.h math.h program.h sgensys.h
	$(CC) -c $(CFLAGS) generator.c

interpreter.o: interpreter.c interpreter.h result.h program.h plist.h sgensys.h
	$(CC) -c $(CFLAGS) interpreter.c

parser.o: parser.c parser.h symtab.h program.h plist.h wave.h math.h sgensys.h

plist.o: plist.c plist.h sgensys.h
	$(CC) -c $(CFLAGS) plist.c

renderer.o: renderer.c renderer.h interpreter.h result.h program.h plist.h osc.h math.h sgensys.h
	$(CC) -c $(CFLAGS) renderer.c

sgensys.o: sgensys.c generator.h renderer.h interpreter.h result.h parser.h program.h wave.h plist.h audiodev.h wavfile.h sgensys.h
	$(CC) -c $(CFLAGS) sgensys.c

symtab.o: symtab.c symtab.h sgensys.h
	$(CC) -c $(CFLAGS) symtab.c

wave.o: wave.c wave.h math.h sgensys.h
	$(CC) -c $(CFLAGS) wave.c

wavfile.o: wavfile.c wavfile.h sgensys.h
	$(CC) -c $(CFLAGS) wavfile.c
