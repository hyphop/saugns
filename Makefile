.POSIX:
CC=cc
CFLAGS=-std=c99 -W -Wall -O2
CFLAGS_FAST=$(CFLAGS) -ffast-math
LFLAGS=-s -lm
LFLAGS_LINUX=$(LFLAGS) -lasound
LFLAGS_SNDIO=$(LFLAGS) -lsndio
LFLAGS_OSSAUDIO=$(LFLAGS) -lossaudio
PREFIX=/usr/local
BIN=saugns
MAN1=saugns.1
SHARE=saugns
OBJ=\
	common.o \
	help.o \
	arrtype.o \
	ptrlist.o \
	mempool.o \
	nodelist.o \
	ramp.o \
	wave.o \
	reader/file.o \
	reader/symtab.o \
	reader/scanner.o \
	reader/parser.o \
	reader/parseconv.o \
	builder/voicegraph.o \
	builder/scriptconv.o \
	builder/builder.o \
	interp/osc.o \
	interp/mixer.o \
	interp/generator.o \
	renderer/audiodev.o \
	renderer/wavfile.o \
	renderer/renderer.o \
	saugns.o
TEST1_OBJ=\
	common.o \
	arrtype.o \
	ptrlist.o \
	mempool.o \
	reader/file.o \
	reader/symtab.o \
	reader/scanner.o \
	reader/lexer.o \
	builder/voicegraph.o \
	builder/scriptconv.o \
	test-scan.o

all: $(BIN)
tests: test-scan
clean:
	rm -f $(OBJ) $(BIN)
	rm -f $(TEST1_OBJ) test-scan
install: $(BIN)
	@if [ -d "$(DESTDIR)$(PREFIX)/man" ]; then \
		MANDIR="man"; \
	else \
		MANDIR="share/man"; \
	fi; \
	echo "Installing man page under $(DESTDIR)$(PREFIX)/$$MANDIR."; \
	if [ -d "/usr/share/man" ]; then \
		for f in "/usr/share/man/man"?/*.gz; do \
			[ -e "$$f" ] && USE_GZIP=yes || USE_GZIP=; break; \
		done; \
	else \
		for f in "$(DESTDIR)$(PREFIX)/$$MANDIR/man"?/*.gz; do \
			[ -e "$$f" ] && USE_GZIP=yes || USE_GZIP=; break; \
		done; \
	fi; \
	mkdir -p $(DESTDIR)$(PREFIX)/$$MANDIR/man1; \
	if [ $$USE_GZIP ]; then \
		echo "(Gzip man pages? 'Yes', quick look says.)"; \
		gzip < man/$(MAN1) > $(DESTDIR)$(PREFIX)/$$MANDIR/man1/$(MAN1).gz; \
	else \
		echo "(Gzip man pages? 'No', quick look says.)"; \
		cp -f man/$(MAN1) $(DESTDIR)$(PREFIX)/$$MANDIR/man1; \
	fi; \
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(PREFIX)/share/doc/$(SHARE)
	mkdir -p $(DESTDIR)$(PREFIX)/share/examples/$(SHARE)
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	cp -RP doc/* $(DESTDIR)$(PREFIX)/share/doc/$(SHARE)
	cp -RP examples/* $(DESTDIR)$(PREFIX)/share/examples/$(SHARE)
uninstall:
	@if [ -d "$(DESTDIR)$(PREFIX)/man" ]; then \
		MANDIR="man"; \
	else \
		MANDIR="share/man"; \
	fi; \
	echo "Uninstalling man page under $(DESTDIR)$(PREFIX)/$$MANDIR."; \
	rm -f $(DESTDIR)$(PREFIX)/$$MANDIR/man1/$(MAN1).gz \
		$(DESTDIR)$(PREFIX)/$$MANDIR/man1/$(MAN1)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -Rf $(DESTDIR)$(PREFIX)/share/doc/$(SHARE)
	rm -Rf $(DESTDIR)$(PREFIX)/share/examples/$(SHARE)

$(BIN): $(OBJ)
	@UNAME="`uname -s`"; \
	if [ $$UNAME = 'Linux' ]; then \
		echo "Linking for Linux (using ALSA and OSS)."; \
		$(CC) $(OBJ) $(LFLAGS_LINUX) -o $(BIN); \
	elif [ $$UNAME = 'OpenBSD' ]; then \
		echo "Linking for OpenBSD (using sndio)."; \
		$(CC) $(OBJ) $(LFLAGS_SNDIO) -o $(BIN); \
	elif [ $$UNAME = 'NetBSD' ]; then \
		echo "Linking for NetBSD (using OSS)."; \
		$(CC) $(OBJ) $(LFLAGS_OSSAUDIO) -o $(BIN); \
	else \
		echo "Linking for UNIX with OSS."; \
		$(CC) $(OBJ) $(LFLAGS) -o $(BIN); \
	fi

test-scan: $(TEST1_OBJ)
	$(CC) $(TEST1_OBJ) $(LFLAGS) -o test-scan

arrtype.o: arrtype.c arrtype.h common.h mempool.h
	$(CC) -c $(CFLAGS) arrtype.c

builder/builder.o: arrtype.h builder/builder.c common.h math.h nodelist.h program.h ramp.h saugns.h script.h time.h wave.h
	$(CC) -c $(CFLAGS) builder/builder.c -o builder/builder.o

builder/scriptconv.o: arrtype.h builder/scriptconv.c builder/scriptconv.h common.h math.h mempool.h nodelist.h program.h ptrlist.h ramp.h script.h time.h wave.h
	$(CC) -c $(CFLAGS) builder/scriptconv.c -o builder/scriptconv.o

builder/voicegraph.o: arrtype.h builder/scriptconv.h builder/voicegraph.c common.h math.h mempool.h nodelist.h program.h ramp.h script.h time.h wave.h
	$(CC) -c $(CFLAGS) builder/voicegraph.c -o builder/voicegraph.o

common.o: common.c common.h
	$(CC) -c $(CFLAGS) common.c

help.o: common.h help.c help.h ramp.h wave.h
	$(CC) -c $(CFLAGS) help.c

interp/generator.o: common.h interp/generator.c interp/generator.h interp/mixer.h interp/osc.h math.h program.h ramp.h time.h wave.h
	$(CC) -c $(CFLAGS_FAST) interp/generator.c -o interp/generator.o

interp/mixer.o: common.h interp/mixer.c interp/mixer.h math.h ramp.h
	$(CC) -c $(CFLAGS_FAST) interp/mixer.c -o interp/mixer.o

interp/osc.o: common.h interp/osc.c interp/osc.h math.h wave.h
	$(CC) -c $(CFLAGS_FAST) interp/osc.c -o interp/osc.o

mempool.o: common.h mempool.c mempool.h
	$(CC) -c $(CFLAGS) mempool.c

nodelist.o: common.h mempool.h nodelist.c nodelist.h
	$(CC) -c $(CFLAGS) nodelist.c

ptrlist.o: common.h mempool.h ptrlist.c ptrlist.h
	$(CC) -c $(CFLAGS) ptrlist.c

ramp.o: common.h math.h ramp.c ramp.h time.h
	$(CC) -c $(CFLAGS_FAST) ramp.c

reader/file.o: common.h reader/file.c reader/file.h
	$(CC) -c $(CFLAGS) reader/file.c -o reader/file.o

reader/lexer.o: common.h math.h mempool.h reader/file.h reader/lexer.c reader/lexer.h reader/scanner.h reader/symtab.h
	$(CC) -c $(CFLAGS) reader/lexer.c -o reader/lexer.o

reader/parseconv.o: common.h math.h mempool.h nodelist.h program.h ramp.h reader/parseconv.c reader/parser.h script.h time.h wave.h
	$(CC) -c $(CFLAGS) reader/parseconv.c -o reader/parseconv.o

reader/parser.o: common.h help.h math.h mempool.h nodelist.h program.h ramp.h reader/file.h reader/parser.c reader/parser.h reader/scanner.h reader/symtab.h script.h time.h wave.h
	$(CC) -c $(CFLAGS) reader/parser.c -o reader/parser.o

reader/scanner.o: common.h math.h mempool.h reader/file.h reader/scanner.c reader/scanner.h reader/symtab.h
	$(CC) -c $(CFLAGS) reader/scanner.c -o reader/scanner.o

reader/symtab.o: common.h mempool.h reader/symtab.c reader/symtab.h
	$(CC) -c $(CFLAGS) reader/symtab.c -o reader/symtab.o

renderer/audiodev.o: common.h renderer/audiodev.c renderer/audiodev.h renderer/audiodev/*.c
	$(CC) -c $(CFLAGS) renderer/audiodev.c -o renderer/audiodev.o

renderer/renderer.o: common.h interp/generator.h renderer/audiodev.h renderer/renderer.c renderer/wavfile.h math.h program.h ptrlist.h ramp.h saugns.h time.h wave.h
	$(CC) -c $(CFLAGS_FAST) renderer/renderer.c -o renderer/renderer.o

renderer/wavfile.o: common.h renderer/wavfile.c renderer/wavfile.h
	$(CC) -c $(CFLAGS) renderer/wavfile.c -o renderer/wavfile.o

saugns.o: common.h help.h math.h ptrlist.h program.h ramp.h saugns.c saugns.h time.h wave.h
	$(CC) -c $(CFLAGS) saugns.c

test-scan.o: common.h reader/file.h reader/lexer.h reader/scanner.h reader/symtab.h math.h mempool.h program.h ptrlist.h ramp.h saugns.h test-scan.c time.h wave.h
	$(CC) -c $(CFLAGS) test-scan.c

wave.o: common.h math.h wave.c wave.h
	$(CC) -c $(CFLAGS_FAST) wave.c
