/* sgensys: Main module. Command-line interface.
 * Copyright (c) 2011-2013, 2017-2018 Joel K. Pettersson
 * <joelkpettersson@gmail.com>.
 *
 * This file and the software of which it is part is distributed under the
 * terms of the GNU Lesser General Public License, either version 3 or (at
 * your option) any later version, WITHOUT ANY WARRANTY, not even of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * View the file COPYING for details, or if missing, see
 * <https://www.gnu.org/licenses/>.
 */

#include "sgensys.h"
#if SGS_TEST_SCANNER
# include "streamf.h"
# include "scanner.h"
#elif SGS_TEST_LEXER
# include "lexer.h"
#endif
#include "program.h"
#include "parser.h"
#include "generator.h"
#include "audiodev.h"
#include "wavfile.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define BUF_SAMPLES 1024
#define NUM_CHANNELS 2
#define DEFAULT_SRATE 44100

static int16_t audio_buf[BUF_SAMPLES * NUM_CHANNELS];

/*
 * Produce audio for the given program, optionally sending it
 * to a given audio device and/or WAV file.
 *
 * Return true if no error occurred.
 */
static bool produce_audio(SGS_Program *prg,
		SGS_AudioDev *ad, SGS_WAVFile *wf, uint32_t srate) {
	SGS_Generator *gen = SGS_create_Generator(prg, srate);
	size_t len;
	bool error = false;
	bool run;
	do {
		run = SGS_Generator_run(gen, audio_buf, BUF_SAMPLES, &len);
		if (ad && !SGS_AudioDev_write(ad, audio_buf, len)) {
			error = true;
			fputs("error: audio device write failed\n", stderr);
		}
		if (wf && !SGS_WAVFile_write(wf, audio_buf, len)) {
			error = true;
			fputs("error: WAV file write failed\n", stderr);
		}
	} while (run);
	SGS_destroy_Generator(gen);
	return !error;
}

/*
 * Run the given program through the audio generator until completion.
 * The output is sent to either none, one, or both of the audio device
 * or a WAV file.
 *
 * Return true if signal generated and sent to any output(s) without
 * error, false if any error occurred.
 */
static bool run_program(SGS_Program *prg,
		bool use_audiodev, const char *wav_path, uint32_t srate) {
	SGS_AudioDev *ad = NULL;
	uint32_t ad_srate = srate;
	SGS_WAVFile *wf = NULL;
	if (use_audiodev) {
		ad = SGS_open_AudioDev(NUM_CHANNELS, &ad_srate);
		if (!ad) {
			return false;
		}
	}
	if (wav_path) {
		wf = SGS_create_WAVFile(wav_path, NUM_CHANNELS, srate);
		if (!wf) {
			if (ad) SGS_close_AudioDev(ad);
			return false;
		}
	}

	bool status;
	if (ad && wf && (ad_srate != srate)) {
		fputs("warning: generating audio twice, using a different sample rate for each output\n", stderr);
		status = produce_audio(prg, ad, NULL, ad_srate);
		status = status && produce_audio(prg, NULL, wf, srate);
	} else {
		status = produce_audio(prg, ad, wf, ad_srate);
	}

	if (ad) {
		SGS_close_AudioDev(ad);
	}
	if (wf) {
		status = status && (SGS_close_WAVFile(wf) == 0);
	}
	return status;
}

/*
 * Print command line usage instructions.
 */
static void print_usage(bool by_arg) {
	fputs(
"Usage: sgensys [[-a|-m] [-r srate] [-o wavfile]|-p] scriptfile\n"
"\n"
"By default, audio device output is enabled.\n"
"\n"
"  -a \tAudible; always enable audio device output.\n"
"  -m \tMuted; always disable audio device output.\n"
"  -r \tSample rate in Hz (default 44100); if the audio device does not\n"
"     \tsupport the rate requested, a warning will be printed along with\n"
"     \tthe rate used for the audio device instead.\n"
"  -o \tWrite a 16-bit PCM WAV file; by default, this disables audio device\n"
"     \toutput.\n"
"  -p \tStop after parsing the script, upon success or failure; mutually\n"
"     \texclusive with all other options.\n"
"  -h \tPrint this message.\n",
		by_arg ? stdout : stderr);
}

/*
 * Read a positive integer from the given string. Returns the integer,
 * or -1 on error.
 */
static int get_piarg(const char *str) {
	char *endp;
	int i;
	errno = 0;
	i = strtol(str, &endp, 10);
	if (errno || i <= 0 || endp == str || *endp) return -1;
	return i;
}

/*
 * Command line argument flags.
 */
enum {
	ARG_FULL_RUN = 1<<0, /* identifies any non-compile-only flags */
	ARG_ENABLE_AUDIO_DEV = 1<<1,
	ARG_DISABLE_AUDIO_DEV = 1<<2,
	ARG_ONLY_PARSE = 1<<3,
};

/*
 * Parse command line arguments.
 *
 * Returns true if the arguments are valid and include a script to run,
 * otherwise print usage instructions and returns false.
 */
static bool parse_args(int argc, char **argv, uint32_t *flags,
		const char **script_path, const char **wav_path,
		uint32_t *srate) {
	for (;;) {
		const char *arg;
		--argc;
		++argv;
		if (argc < 1) {
			if (!*script_path) goto INVALID;
			break;
		}
		arg = *argv;
		if (*arg != '-') {
			if (*script_path) goto INVALID;
			*script_path = arg;
			continue;
		}
		while (*++arg) {
			if (*arg == 'a') {
				if (*flags & (ARG_DISABLE_AUDIO_DEV |
						ARG_ONLY_PARSE))
					goto INVALID;
				*flags |= ARG_FULL_RUN |
					ARG_ENABLE_AUDIO_DEV;
			} else if (*arg == 'm') {
				if (*flags & (ARG_ENABLE_AUDIO_DEV |
						ARG_ONLY_PARSE))
					goto INVALID;
				*flags |= ARG_FULL_RUN |
					 ARG_DISABLE_AUDIO_DEV;
			} else if (!strcmp(arg, "h")) {
				if (*flags != 0)
					goto INVALID;
				print_usage(true);
				return false;
			} else if (!strcmp(arg, "r")) {
				int i;
				if (*flags & ARG_ONLY_PARSE)
					goto INVALID;
				*flags |= ARG_FULL_RUN;
				--argc;
				++argv;
				if (argc < 1) goto INVALID;
				arg = *argv;
				i = get_piarg(arg);
				if (i < 0) goto INVALID;
				*srate = i;
				break;
			} else if (!strcmp(arg, "o")) {
				if (*flags & ARG_ONLY_PARSE)
					goto INVALID;
				*flags |= ARG_FULL_RUN;
				--argc;
				++argv;
				if (argc < 1) goto INVALID;
				arg = *argv;
				*wav_path = arg;
				break;
			} else if (*arg == 'p') {
				if (*flags & ARG_FULL_RUN)
					goto INVALID;
				*flags |= ARG_ONLY_PARSE;
			} else
				goto INVALID;
		}
	}
	return true;

INVALID:
	print_usage(false);
	return false;
}

/*
 * Process the given script file. Invokes the parser and, if
 * successful, proceeds to build program unless skipped.
 *
 * prg_out must point to a usable pointer unless building skipped.
 *
 * Return true if successful, false on error.
 */
static bool process_script(const char *fname, SGS_Program **prg_out,
		uint32_t options) {
#if SGS_TEST_SCANNER
	SGS_Stream fs;
	SGS_init_Stream(&fs);
	if (!SGS_Stream_fopenrb(&fs, script_path)) {
		return false;
	}
	SGS_Scanner *scanner = SGS_create_Scanner(&fs);
	for (;;) {
		char c = SGS_Scanner_getc(scanner);
		putchar(c);
		if (!c) {
			putchar('\n');
			break;
		}
	}
	SGS_destroy_Scanner(scanner);
	SGS_fini_Stream(&fs);
	return true;
#elif SGS_TEST_LEXER
	SGS_SymTab *symtab = SGS_create_SymTab();
	SGS_Lexer *lexer = SGS_create_Lexer(script_path, symtab);
	if (!lexer) {
		SGS_destroy_SymTab(symtab);
		return false;
	}
	for (;;) {
		SGS_LexerToken token;
		if (!SGS_Lexer_get(lexer, &token)) break;
	}
	SGS_destroy_Lexer(lexer);
	SGS_destroy_SymTab(symtab);
	return true;
#else
	SGS_Parser *parser = SGS_create_Parser();
	SGS_ParseResult *parse = SGS_Parser_process(parser, fname);
	if (!parse) {
		SGS_destroy_Parser(parser);
		return false;
	}
	if (options & ARG_ONLY_PARSE) {
		SGS_destroy_Parser(parser);
		return true;
	}
	SGS_Program *prg = SGS_create_Program(parse);
	SGS_destroy_Parser(parser);
	*prg_out = prg;
	return (prg != NULL);
#endif
}

/**
 * Main function.
 */
int main(int argc, char **argv) {
	const char *script_path = NULL, *wav_path = NULL;
	uint32_t options = 0;
	uint32_t srate = DEFAULT_SRATE;
	bool error = false;

	if (!parse_args(argc, argv, &options, &script_path, &wav_path,
			&srate))
		return false;

	struct SGS_Program *prg = NULL;
	if (!process_script(script_path, &prg, options)) {
		error = true;
	} else if (prg) {
		bool use_audiodev = (wav_path ?
				(options & ARG_ENABLE_AUDIO_DEV) :
				!(options & ARG_DISABLE_AUDIO_DEV));
		error = !run_program(prg, use_audiodev, wav_path, srate);
		SGS_destroy_Program(prg);
	}

	return error;
}
