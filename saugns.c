/* saugns: Main module / Command-line interface.
 * Copyright (c) 2011-2013, 2017-2019 Joel K. Pettersson
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

#include "saugns.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_SRATE 44100

/*
 * Print command line usage instructions.
 */
static void print_usage(bool by_arg) {
	fputs(
"Usage: saugns [-a|-m] [-r <srate>] [-p] [-o <wavfile>] <script>...\n"
"       saugns [-a|-m] [-r <srate>] [-p] [-o <wavfile>] -e <string>...\n"
"       saugns [-c] [-p] <script>...\n"
"       saugns [-c] [-p] -e <string>...\n"
"\n"
"By default, audio device output is enabled.\n"
"\n"
"  -a \tAudible; always enable audio device output.\n"
"  -m \tMuted; always disable audio device output.\n"
"  -r \tSample rate in Hz (default 44100);\n"
"     \tif unsupported for audio device, warns and prints rate used instead.\n"
"  -o \tWrite a 16-bit PCM WAV file, always using the sample rate requested;\n"
"     \tdisables audio device output by default.\n"
"  -e \tEvaluate strings instead of files.\n"
"  -c \tCheck scripts only, reporting any errors or requested info.\n"
"  -p \tPrint info for scripts after loading.\n"
"  -h \tPrint this message.\n"
"  -v \tPrint version.\n",
	(by_arg) ? stdout : stderr);
}

/*
 * Print version.
 */
static void print_version(void) {
	puts(SAU_VERSION_STR);
}

/*
 * Read a positive integer from the given string.
 *
 * \return positive value or -1 if invalid
 */
static int32_t get_piarg(const char *restrict str) {
	char *endp;
	int32_t i;
	errno = 0;
	i = strtol(str, &endp, 10);
	if (errno || i <= 0 || endp == str || *endp)
		return -1;
	return i;
}

/*
 * Command line argument flags.
 */
enum {
	ARG_FULL_RUN = 1<<0, /* identifies any non-compile-only flags */
	ARG_ENABLE_AUDIO_DEV = 1<<1,
	ARG_DISABLE_AUDIO_DEV = 1<<2,
	ARG_ONLY_CHECK = 1<<3,
	ARG_PRINT_INFO = 1<<4,
	ARG_EVAL_STRING = 1<<5,
};

/*
 * Parse command line arguments.
 *
 * Print usage instructions if requested or args invalid.
 *
 * \return true if args valid and script path set
 */
static bool parse_args(int argc, char **restrict argv,
		uint32_t *restrict flags,
		SAU_PtrList *restrict script_args,
		const char **restrict wav_path,
		uint32_t *restrict srate) {
	int i;
	for (;;) {
		const char *arg;
		--argc;
		++argv;
		if (argc < 1) {
			if (!script_args->count) goto INVALID;
			break;
		}
		arg = *argv;
		if (*arg != '-') {
			SAU_PtrList_add(script_args, arg);
			continue;
		}
NEXT_C:
		if (!*++arg) continue;
		switch (*arg) {
		case 'a':
			if ((*flags & (ARG_DISABLE_AUDIO_DEV |
					ARG_ONLY_CHECK)) != 0)
				goto INVALID;
			*flags |= ARG_FULL_RUN |
				ARG_ENABLE_AUDIO_DEV;
			break;
		case 'c':
			if ((*flags & ARG_FULL_RUN) != 0)
				goto INVALID;
			*flags |= ARG_ONLY_CHECK;
			break;
		case 'e':
			*flags |= ARG_EVAL_STRING;
			break;
		case 'h':
			if (*flags != 0) goto INVALID;
			print_usage(true);
			goto CLEAR;
		case 'm':
			if ((*flags & (ARG_ENABLE_AUDIO_DEV |
					ARG_ONLY_CHECK)) != 0)
				goto INVALID;
			*flags |= ARG_FULL_RUN |
				ARG_DISABLE_AUDIO_DEV;
			break;
		case 'o':
			if (arg[1] != '\0') goto INVALID;
			if ((*flags & ARG_ONLY_CHECK) != 0)
				goto INVALID;
			*flags |= ARG_FULL_RUN;
			--argc;
			++argv;
			if (argc < 1) goto INVALID;
			arg = *argv;
			*wav_path = arg;
			continue;
		case 'p':
			*flags |= ARG_PRINT_INFO;
			break;
		case 'r':
			if (arg[1] != '\0') goto INVALID;
			if ((*flags & ARG_ONLY_CHECK) != 0)
				goto INVALID;
			*flags |= ARG_FULL_RUN;
			--argc;
			++argv;
			if (argc < 1) goto INVALID;
			arg = *argv;
			i = get_piarg(arg);
			if (i < 0) goto INVALID;
			*srate = i;
			continue;
		case 'v':
			print_version();
			goto CLEAR;
		default:
			goto INVALID;
		}
		goto NEXT_C;
	}
	return (script_args->count != 0);
INVALID:
	print_usage(false);
CLEAR:
	SAU_PtrList_clear(script_args);
	return false;
}

/*
 * Discard the programs in the list, ignoring NULL entries,
 * and clearing the list.
 */
static void discard_programs(SAU_PtrList *restrict prg_objs) {
	SAU_Program **prgs = (SAU_Program**) SAU_PtrList_ITEMS(prg_objs);
	for (size_t i = 0; i < prg_objs->count; ++i) {
		SAU_discard_Program(prgs[i]);
	}
	SAU_PtrList_clear(prg_objs);
}

/*
 * Process the listed scripts.
 *
 * \return true if at least one script succesfully built
 */
static bool build(const SAU_PtrList *restrict script_args,
		SAU_PtrList *restrict prg_objs,
		uint32_t options) {
	bool are_paths = !(options & ARG_EVAL_STRING);
	SAU_build(script_args, are_paths, prg_objs);
	const SAU_Program **prgs =
		(const SAU_Program**) SAU_PtrList_ITEMS(prg_objs);
	size_t built = 0;
	for (size_t i = 0; i < prg_objs->count; ++i) {
		const SAU_Program *prg = prgs[i];
		if (prg != NULL) {
			++built;
			if ((options & ARG_PRINT_INFO) != 0)
				SAU_Program_print_info(prg);
		}
	}
	if (!built)
		return false;
	if ((options & ARG_ONLY_CHECK) != 0) {
		discard_programs(prg_objs);
	}
	return true;
}

/*
 * Produce results from the list of programs, ignoring NULL entries.
 *
 * \return true unless error occurred
 */
static bool render(const SAU_PtrList *restrict prg_objs,
		uint32_t srate, uint32_t options,
		const char *restrict wav_path) {
	bool use_audiodev = (wav_path != NULL) ?
		((options & ARG_ENABLE_AUDIO_DEV) != 0) :
		((options & ARG_DISABLE_AUDIO_DEV) == 0);
	return SAU_render(prg_objs, srate, use_audiodev, wav_path);
}

/**
 * Main function.
 */
int main(int argc, char **restrict argv) {
	SAU_PtrList script_args = (SAU_PtrList){0};
	SAU_PtrList prg_objs = (SAU_PtrList){0};
	const char *wav_path = NULL;
	uint32_t options = 0;
	uint32_t srate = DEFAULT_SRATE;
	if (!parse_args(argc, argv, &options, &script_args, &wav_path,
			&srate))
		return 0;
	bool error = !build(&script_args, &prg_objs, options);
	SAU_PtrList_clear(&script_args);
	if (error)
		return 1;
	if (prg_objs.count > 0) {
		error = !render(&prg_objs, srate, options, wav_path);
		discard_programs(&prg_objs);
		if (error)
			return 1;
	}
	return 0;
}