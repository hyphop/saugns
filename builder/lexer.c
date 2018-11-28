/* sgensys: Script lexer module.
 * Copyright (c) 2014, 2017-2019 Joel K. Pettersson
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

#include "lexer.h"
#include "scanner.h"
#include "../math.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * Read helper definitions & functions.
 */

/* Basic character types. */
#define IS_LOWER(c) ((c) >= 'a' && (c) <= 'z')
#define IS_UPPER(c) ((c) >= 'A' && (c) <= 'Z')
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define IS_ALPHA(c) (IS_LOWER(c) || IS_UPPER(c))
#define IS_ALNUM(c) (IS_ALPHA(c) || IS_DIGIT(c))
#define IS_SPACE(c) ((c) == ' ' || (c) == '\t')
#define IS_LNBRK(c) ((c) == '\n' || (c) == '\r')

/* Valid characters in identifiers. */
#define IS_SYMCHAR(c) (IS_ALNUM(c) || (c) == '_')

/* Visible ASCII character. */
#define IS_VISIBLE(c) ((c) >= '!' && (c) <= '~')

/*
 * Lexer implementation.
 */

#define STRBUF_LEN 1024

struct SGS_Lexer {
	SGS_Scanner *sc;
	SGS_SymTab *symtab;
	SGS_ScriptToken token;
	uint8_t *strbuf;
};

/**
 * Create instance for the given file and using the given symbol
 * table.
 *
 * \return instance, or NULL on failure.
 */
SGS_Lexer *SGS_create_Lexer(const char *restrict fname,
		SGS_SymTab *restrict symtab) {
	SGS_Lexer *o;
	if (symtab == NULL) return NULL;
	uint8_t *strbuf = calloc(1, STRBUF_LEN);
	if (strbuf == NULL) return NULL;

	o = calloc(1, sizeof(SGS_Lexer));
	if (o == NULL) return NULL;
	o->sc = SGS_create_Scanner();
	if (!o->sc) goto ERROR;
	o->symtab = symtab;
	o->strbuf = strbuf;
	if (!SGS_Scanner_fopenrb(o->sc, fname)) goto ERROR;
#if SGS_LEXER_QUIET
	o->sc->s_flags |= SGS_SCAN_S_QUIET;
#endif
	return o;

ERROR:
	SGS_destroy_Lexer(o);
	return NULL;
}

/**
 * Destroy instance, also closing the file for which it was made.
 */
void SGS_destroy_Lexer(SGS_Lexer *restrict o) {
	SGS_destroy_Scanner(o->sc);
	if (o->strbuf) free(o->strbuf);
	free(o);
}

static void handle_invalid(SGS_Lexer *o, uint8_t c SGS__maybe_unused) {
	SGS_ScriptToken *t = &o->token;
	t->type = SGS_T_INVALID;
	t->data.b = 0;
}

static void handle_eof(SGS_Lexer *restrict o,
		uint8_t c SGS__maybe_unused) {
	SGS_Scanner *sc = o->sc;
	SGS_ScriptToken *t = &o->token;
	t->type = SGS_T_INVALID;
	t->data.b = SGS_File_STATUS(sc->f);
	//puts("EOF");
}

static void handle_special(SGS_Lexer *restrict o, uint8_t c) {
	SGS_ScriptToken *t = &o->token;
	t->type = SGS_T_SPECIAL;
	t->data.c = c;
	//putchar(c);
}

static void handle_numeric_value(SGS_Lexer *restrict o,
		uint8_t c SGS__maybe_unused) {
	SGS_Scanner *sc = o->sc;
	SGS_ScriptToken *t = &o->token;
	double d;
	SGS_Scanner_ungetc(sc);
	SGS_Scanner_getd(sc, &d, false, NULL);
	t->type = SGS_T_VAL_REAL;
	t->data.f = d;
	//printf("num == %f\n", d);
}

static void handle_identifier(SGS_Lexer *restrict o,
		uint8_t c SGS__maybe_unused) {
	SGS_Scanner *sc = o->sc;
	SGS_ScriptToken *t = &o->token;
	size_t len;
	SGS_Scanner_ungetc(sc);
	SGS_Scanner_getsyms(sc, o->strbuf, STRBUF_LEN, &len);
	const char *pool_str;
	pool_str = SGS_SymTab_pool_str(o->symtab, o->strbuf, len);
	if (pool_str == NULL) {
		SGS_Scanner_error(sc, NULL,
			"failed to register string '%s'", o->strbuf);
	}
	t->type = SGS_T_ID_STR;
	t->data.id = pool_str;
	//printf("str == %s\n", pool_str);
}

/**
 * Get the next token from the current file.
 *
 * Upon end of file, an SGS_T_INVALID token is set and false is
 * returned. The field data.b is assigned the file reading status.
 * (If true is returned, an SGS_T_INVALID token simply means that
 * invalid input was successfully registered in the current file.)
 *
 * \return true if a token was successfully read from the file
 */
bool SGS_Lexer_get(SGS_Lexer *restrict o, SGS_ScriptToken *restrict t) {
	SGS_Scanner *sc = o->sc;
	uint8_t c;
REGET:
	c = SGS_Scanner_getc_nospace(sc);
	switch (c) {
	case 0x00:
		handle_eof(o, c);
		break;
	case SGS_SCAN_LNBRK:
	case SGS_SCAN_SPACE:
		goto REGET;
	case '!':
	case '"':
	case '#':
	case '$':
	case '%':
	case '&':
	case '\'':
	case '(':
	case ')':
	case '*':
	case '+':
	case ',':
	case '-':
	case '.':
	case '/':
		handle_special(o, c);
		break;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		handle_numeric_value(o, c);
		break;
	case ':':
	case ';':
	case '<':
	case '=':
	case '>':
	case '?':
	case '@':
		handle_special(o, c);
		break;
	case 'A':
	case 'B':
	case 'C':
	case 'D':
	case 'E':
	case 'F':
	case 'G':
	case 'H':
	case 'I':
	case 'J':
	case 'K':
	case 'L':
	case 'M':
	case 'N':
	case 'O':
	case 'P':
	case 'Q':
	case 'R':
	case 'S':
	case 'T':
	case 'U':
	case 'V':
	case 'W':
	case 'X':
	case 'Y':
	case 'Z':
		handle_identifier(o, c);
		break;
	case '[':
	case '\\':
	case ']':
	case '^':
	case '_':
	case '`':
		handle_special(o, c);
		break;
	case 'a':
	case 'b':
	case 'c':
	case 'd':
	case 'e':
	case 'f':
	case 'g':
	case 'h':
	case 'i':
	case 'j':
	case 'k':
	case 'l':
	case 'm':
	case 'n':
	case 'o':
	case 'p':
	case 'q':
	case 'r':
	case 's':
	case 't':
	case 'u':
	case 'v':
	case 'w':
	case 'x':
	case 'y':
	case 'z':
		handle_identifier(o, c);
		break;
	case '{':
	case '|':
	case '}':
	case '~':
		handle_special(o, c);
		break;
	default:
		handle_invalid(o, c);
		break;
	}
	if (t != NULL) {
		*t = o->token;
	}
	return (c != 0);
}

/**
 * Get the next token from the current file. Interprets any visible ASCII
 * character as a special token character.
 *
 * Upon end of file, an SGS_T_INVALID token is set and false is
 * returned. The field data.b is assigned the file reading status.
 * (If true is returned, an SGS_T_INVALID token simply means that
 * invalid input was successfully registered in the current file.)
 *
 * \return true if a token was successfully read from the file
 */
bool SGS_Lexer_get_special(SGS_Lexer *restrict o,
		SGS_ScriptToken *restrict t) {
	SGS_Scanner *sc = o->sc;
	uint8_t c;
	for (;;) {
		c = SGS_Scanner_getc_nospace(sc);
		if (c == 0) {
			handle_eof(o, c);
			break;
		}
		if (IS_VISIBLE(c)) {
			handle_special(o, c);
			break;
		}
	}
	if (t != NULL) {
		*t = o->token;
	}
	return (c != 0);
}