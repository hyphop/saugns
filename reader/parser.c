/* saugns: Script file parser.
 * Copyright (c) 2011-2012, 2017-2020 Joel K. Pettersson
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

#include "scanner.h"
#include "parser.h"
#include "../math.h"
#include <string.h>
#include <stdio.h>

/*
 * File-reading code
 */

/* Basic character types. */
#define IS_LOWER(c) ((c) >= 'a' && (c) <= 'z')
#define IS_UPPER(c) ((c) >= 'A' && (c) <= 'Z')
#define IS_ALPHA(c) (IS_LOWER(c) || IS_UPPER(c))

/* Sensible to print, for ASCII only. */
#define IS_VISIBLE(c) ((c) >= '!' && (c) <= '~')

typedef struct ScanLookup {
	SAU_ScriptOptions sopt;
	const char *const*wave_names;
	const char *const*ramp_names;
} ScanLookup;

/*
 * Default script options, used until changed in a script.
 */
static const SAU_ScriptOptions def_sopt = {
	.changed = 0,
	.ampmult = 1.f,
	.A4_freq = 444.f,
	.def_time_ms = 1000,
	.def_freq = 444.f,
	.def_relfreq = 1.f,
	.def_chanmix = 0.f,
};

static bool init_ScanLookup(ScanLookup *restrict o, SAU_SymTab *restrict st) {
	o->sopt = def_sopt;
	o->wave_names = SAU_SymTab_pool_stra(st,
			SAU_Wave_names, SAU_WAVE_TYPES);
	if (!o->wave_names)
		return false;
	o->ramp_names = SAU_SymTab_pool_stra(st,
			SAU_Ramp_names, SAU_RAMP_TYPES);
	if (!o->ramp_names)
		return false;
	return true;
}

/*
 * Handle unknown character, checking for EOF and treating
 * the character as invalid if not an end marker.
 *
 * \return false if EOF reached
 */
static bool handle_unknown_or_eof(SAU_Scanner *restrict o, uint8_t c) {
	if (c == 0)
		return false;
	if (IS_VISIBLE(c)) {
		SAU_Scanner_warning(o, NULL,
				"invalid character '%c'", c);
	} else {
		SAU_Scanner_warning(o, NULL,
				"invalid character (value 0x%02hhX)", c);
	}
	return true;
}

/*
 * Print warning for EOF without closing \p c scope-closing character.
 */
static void warn_eof_without_closing(SAU_Scanner *restrict o, uint8_t c) {
	SAU_Scanner_warning(o, NULL, "end of file without closing '%c'", c);
}

/*
 * Print warning for scope-closing character without scope-opening character.
 */
static void warn_closing_without_opening(SAU_Scanner *restrict o,
		uint8_t close_c, uint8_t open_c) {
	SAU_Scanner_warning(o, NULL, "closing '%c' without opening '%c'",
			close_c, open_c);
}

/*
 * Handle '#'-commands.
 */
static uint8_t scan_filter_hashcommands(SAU_Scanner *restrict o, uint8_t c) {
	SAU_File *f = o->f;
	uint8_t next_c = SAU_File_GETC(f);
	if (next_c == '!') {
		++o->sf.char_num;
		return SAU_Scanner_filter_linecomment(o, next_c);
	}
	if (next_c == 'Q') {
		SAU_File_DECP(f);
		SAU_Scanner_close(o);
		return SAU_SCAN_EOF;
	}
	SAU_File_DECP(f);
	return c;
}

typedef struct NumParser {
	SAU_Scanner *sc;
	SAU_ScanNumConst_f numconst_f;
	SAU_ScanFrame sf_start;
	bool has_infnum;
} NumParser;
enum {
	NUMEXP_SUB = 0,
	NUMEXP_ADT,
	NUMEXP_MLT,
	NUMEXP_POW,
	NUMEXP_NUM,
};
static double scan_num_r(NumParser *restrict o, uint8_t pri, uint32_t level) {
	SAU_Scanner *sc = o->sc;
	double num;
	bool minus = false;
	uint8_t c;
	if (level == 1) SAU_Scanner_setws_level(sc, SAU_SCAN_WS_NONE);
	c = SAU_Scanner_getc(sc);
	if ((level > 0) && (c == '+' || c == '-')) {
		if (c == '-') minus = true;
		c = SAU_Scanner_getc(sc);
	}
	if (c == '(') {
		num = scan_num_r(o, NUMEXP_SUB, level+1);
	} else {
		size_t read_len;
		SAU_Scanner_ungetc(sc);
		SAU_Scanner_getd(sc, &num, false, &read_len, o->numconst_f);
		if (read_len == 0)
			return NAN;
		if (isnan(num))
			return NAN;
	}
	if (minus) num = -num;
	if (level == 0 || pri == NUMEXP_NUM)
		return num; /* defer all */
	for (;;) {
		if (isinf(num)) o->has_infnum = true;
		c = SAU_Scanner_getc(sc);
		switch (c) {
		case '(':
			if (pri >= NUMEXP_MLT) goto DEFER;
			num *= scan_num_r(o, NUMEXP_SUB, level+1);
			break;
		case ')':
			if (pri != NUMEXP_SUB) goto DEFER;
			return num;
		case '^':
			if (pri >= NUMEXP_POW) goto DEFER;
			num = exp(log(num) * scan_num_r(o, NUMEXP_POW, level));
			break;
		case '*':
			if (pri >= NUMEXP_MLT) goto DEFER;
			num *= scan_num_r(o, NUMEXP_MLT, level);
			break;
		case '/':
			if (pri >= NUMEXP_MLT) goto DEFER;
			num /= scan_num_r(o, NUMEXP_MLT, level);
			break;
		case '+':
			if (pri >= NUMEXP_ADT) goto DEFER;
			num += scan_num_r(o, NUMEXP_ADT, level);
			break;
		case '-':
			if (pri >= NUMEXP_ADT) goto DEFER;
			num -= scan_num_r(o, NUMEXP_ADT, level);
			break;
		default:
			if (pri == NUMEXP_SUB) {
				SAU_Scanner_warning(sc, &o->sf_start,
"numerical expression has '(' without closing ')'");
			}
			goto DEFER;
		}
		if (isnan(num)) goto DEFER;
	}
DEFER:
	SAU_Scanner_ungetc(sc);
	return num;
}
static sauNoinline bool scan_num(SAU_Scanner *restrict o,
		SAU_ScanNumConst_f scan_numconst, float *restrict var) {
	NumParser np = {o, scan_numconst, o->sf, false};
	uint8_t ws_level = o->ws_level;
	float num = scan_num_r(&np, NUMEXP_NUM, 0);
	SAU_Scanner_setws_level(o, ws_level); // restore if changed
	if (isnan(num))
		return false;
	if (isinf(num)) np.has_infnum = true;
	if (np.has_infnum) {
		SAU_Scanner_warning(o, &np.sf_start,
				"discarding expression with infinite number");
		return false;
	}
	*var = num;
	return true;
}

static sauNoinline bool scan_time_val(SAU_Scanner *restrict o,
		uint32_t *restrict val) {
	SAU_ScanFrame sf = o->sf;
	float val_s;
	if (!scan_num(o, NULL, &val_s))
		return false;
	if (val_s < 0.f) {
		SAU_Scanner_warning(o, &sf, "discarding negative time value");
		return false;
	}
	*val = lrint(val_s * 1000.f);
	return true;
}

static size_t scan_chanmix_const(SAU_Scanner *restrict o,
		double *restrict val) {
	char c = SAU_File_GETC(o->f);
	switch (c) {
	case 'C':
		*val = 0.f;
		return 1;
	case 'L':
		*val = -1.f;
		return 1;
	case 'R':
		*val = 1.f;
		return 1;
	default:
		SAU_File_DECP(o->f);
		return 0;
	}
}

#define OCTAVES 11
static size_t scan_note_const(SAU_Scanner *restrict o,
		double *restrict val) {
	static const float octaves[OCTAVES] = {
		(1.f/16.f),
		(1.f/8.f),
		(1.f/4.f),
		(1.f/2.f),
		1.f, /* no. 4 - standard tuning here */
		2.f,
		4.f,
		8.f,
		16.f,
		32.f,
		64.f
	};
	static const float notes[3][8] = {
		{ /* flat */
			48.f/25.f,
			16.f/15.f,
			6.f/5.f,
			32.f/25.f,
			36.f/25.f,
			8.f/5.f,
			9.f/5.f,
			96.f/25.f
		},
		{ /* normal (9/8 replaced with 10/9 for symmetry) */
			1.f,
			10.f/9.f,
			5.f/4.f,
			4.f/3.f,
			3.f/2.f,
			5.f/3.f,
			15.f/8.f,
			2.f
		},
		{ /* sharp */
			25.f/24.f,
			75.f/64.f,
			125.f/96.f,
			25.f/18.f,
			25.f/16.f,
			225.f/128.f,
			125.f/64.f,
			25.f/12.f
		}
	};
	SAU_File *f = o->f;
	ScanLookup *sl = o->data;
	size_t len = 0, num_len;
	uint8_t c;
	float freq;
	int32_t octave;
	int32_t semitone = 1, note;
	int32_t subnote = -1;
	++len;
	c = SAU_File_GETC(f);
	if (c >= 'a' && c <= 'g') {
		subnote = c - 'c';
		if (subnote < 0) /* a, b */
			subnote += 7;
		++len;
		c = SAU_File_GETC(f);
	}
	if (c < 'A' || c > 'G') {
		SAU_File_UNGETN(f, len);
		return 0;
	}
	note = c - 'C';
	if (note < 0) /* A, B */
		note += 7;
	++len;
	c = SAU_File_GETC(f);
	if (c == 's')
		semitone = 2;
	else if (c == 'f')
		semitone = 0;
	else {
		SAU_File_DECP(f);
		--len;
	}
	SAU_Scanner_geti(o, &octave, false, &num_len);
	len += num_len;
	if (num_len == 0)
		octave = 4;
	else if (octave >= OCTAVES) {
		SAU_Scanner_warning(o, NULL,
"invalid octave specified for note, using 4 (valid range 0-10)");
		octave = 4;
	}
	freq = sl->sopt.A4_freq * (3.f/5.f); /* get C4 */
	freq *= octaves[octave] * notes[semitone][note];
	if (subnote >= 0)
		freq *= 1.f + (notes[semitone][note+1] /
				notes[semitone][note] - 1.f) *
			(notes[1][subnote] - 1.f);
	*val = (double) freq;
	return len;
}

static SAU_SymStr *scan_label(SAU_Scanner *restrict o, char op) {
	SAU_SymStr *symstr;
	SAU_Scanner_get_symstr(o, &symstr);
	if (!symstr) {
		SAU_Scanner_warning(o, NULL,
				"ignoring %c without label name", op);
	}
	return symstr;
}

static bool scan_symafind(SAU_Scanner *restrict o,
		const char *const*restrict stra,
		size_t *restrict found_i, const char *restrict print_type) {
	SAU_ScanFrame sf_begin = o->sf;
	SAU_SymStr *symstr;
	SAU_Scanner_get_symstr(o, &symstr);
	if (!symstr) {
		SAU_Scanner_warning(o, NULL,
				"%s type value missing", print_type);
		return false;
	}
	for (size_t i = 0; stra[i] != NULL; ++i) {
		if (stra[i] == symstr->key) {
			*found_i = i;
			return true;
		}
	}
	SAU_Scanner_warning(o, &sf_begin,
			"invalid %s type value; available are:", print_type);
	SAU_print_names(stra, "\t", stderr);
	return false;
}

static bool scan_wavetype(SAU_Scanner *restrict o, size_t *restrict found_id) {
	ScanLookup *sl = o->data;
	return scan_symafind(o, sl->wave_names, found_id, "wave");
}

static bool scan_ramp_state(SAU_Scanner *restrict o,
		SAU_ScanNumConst_f scan_numconst,
		SAU_Ramp *restrict ramp, bool mult) {
	if (!scan_num(o, scan_numconst, &ramp->v0))
		return false;
	if (mult) {
		ramp->flags |= SAU_RAMPP_STATE_RATIO;
	} else {
		ramp->flags &= ~SAU_RAMPP_STATE_RATIO;
	}
	ramp->flags |= SAU_RAMPP_STATE;
	return true;
}

static bool scan_ramp(SAU_Scanner *restrict o,
		SAU_ScanNumConst_f scan_numconst,
		SAU_Ramp *restrict ramp, bool mult) {
	if (!SAU_Scanner_tryc(o, '{')) {
		return scan_ramp_state(o, scan_numconst, ramp, mult);
	}
	ScanLookup *sl = o->data;
	bool goal = false;
	bool time_set = (ramp->flags & SAU_RAMPP_TIME) != 0;
	float vt;
	uint32_t time_ms = sl->sopt.def_time_ms;
	uint8_t type = ramp->type; // has default
	if ((ramp->flags & SAU_RAMPP_GOAL) != 0) {
		// allow partial change
		if (((ramp->flags & SAU_RAMPP_GOAL_RATIO) != 0) == mult) {
			goal = true;
			vt = ramp->vt;
		}
		time_ms = ramp->time_ms;
	}
	for (;;) {
		uint8_t c = SAU_Scanner_getc(o);
		switch (c) {
		case SAU_SCAN_SPACE:
		case SAU_SCAN_LNBRK:
			break;
		case 'c': {
			size_t id;
			if (scan_symafind(o, sl->ramp_names, &id, "ramp")) {
				type = id;
			}
			break; }
		case 't':
			if (scan_time_val(o, &time_ms))
				time_set = true;
			break;
		case 'v':
			if (scan_num(o, scan_numconst, &vt))
				goal = true;
			break;
		case '}':
			goto RETURN;
		default:
			if (!handle_unknown_or_eof(o, c)) {
				warn_eof_without_closing(o, '}');
				goto RETURN;
			}
			break;
		}
	}
RETURN:
	if (!goal) {
		SAU_Scanner_warning(o, NULL,
				"ignoring value ramp with no target value");
		return false;
	}
	ramp->vt = vt;
	ramp->time_ms = time_ms;
	ramp->type = type;
	ramp->flags |= SAU_RAMPP_GOAL;
	if (mult)
		ramp->flags |= SAU_RAMPP_GOAL_RATIO;
	else
		ramp->flags &= ~SAU_RAMPP_GOAL_RATIO;
	if (time_set)
		ramp->flags |= SAU_RAMPP_TIME;
	else
		ramp->flags &= ~SAU_RAMPP_TIME;
	return true;
}

/*
 * Parser
 */

typedef struct SAU_Parser {
	ScanLookup sl;
	SAU_Scanner *sc;
	SAU_SymTab *st;
	SAU_MemPool *mp;
	uint32_t call_level;
	/* node state */
	SAU_ParseEvData *ev, *first_ev;
} SAU_Parser;

/*
 * Finalize parser instance.
 */
static void fini_Parser(SAU_Parser *restrict o) {
	SAU_destroy_Scanner(o->sc);
	SAU_destroy_SymTab(o->st);
	SAU_destroy_MemPool(o->mp);
}

/*
 * Initialize parser instance.
 *
 * The same symbol table and script-set data will be used
 * until the instance is finalized.
 *
 * \return true, or false on allocation failure
 */
static bool init_Parser(SAU_Parser *restrict o) {
	SAU_MemPool *mp = SAU_create_MemPool(0);
	SAU_SymTab *st = SAU_create_SymTab(mp);
	SAU_Scanner *sc = SAU_create_Scanner(st);
	*o = (SAU_Parser){0};
	o->sc = sc;
	o->st = st;
	o->mp = mp;
	if (!sc || !st || !mp) goto ERROR;
	if (!init_ScanLookup(&o->sl, st)) goto ERROR;
	sc->filters['#'] = scan_filter_hashcommands;
	sc->data = &o->sl;
	return true;

ERROR:
	fini_Parser(o);
	return false;
}

/*
 * Scope values.
 */
enum {
	SCOPE_TOP = 0,
	SCOPE_BLOCK,
	SCOPE_BIND,
	SCOPE_NEST,
};

/*
 * Current "location" (what is being parsed/worked on) for parse level.
 */
enum {
	SDPL_IN_NONE = 0, // no target for parameters
	SDPL_IN_DEFAULTS, // adjusting default values
	SDPL_IN_EVENT,    // adjusting operator and/or voice
};

/*
 * Parse level flags.
 */
enum {
	SDPL_BIND_MULTIPLE = 1<<0, // previous node interpreted as set of nodes
	SDPL_NESTED_SCOPE = 1<<1,
	SDPL_ACTIVE_EV = 1<<2,
	SDPL_ACTIVE_OP = 1<<3,
};

/*
 * Things that need to be separate for each nested parse_level() go here.
 *
 *
 */
typedef struct ParseLevel {
	SAU_Parser *o;
	struct ParseLevel *parent;
	uint32_t pl_flags;
	uint8_t location;
	uint8_t scope;
	uint8_t list_type;
	uint8_t last_list_type; /* FIXME: kludge */
	SAU_ParseEvData *event, *last_event;
	SAU_ParseOpRef *op_ref, *parent_op_ref;
	SAU_ParseOpRef *first_op_ref;
	SAU_ParseOpData *last_op;
	SAU_ParseOpList *op_list;
	SAU_SymStr *set_label;
	/* timing/delay */
	SAU_ParseEvData *group_from; /* where to begin for group_events() */
	SAU_ParseEvData *composite; /* grouping of events for a voice and/or operator */
	uint32_t next_wait_ms; /* added for next event */
} ParseLevel;

static bool parse_waittime(ParseLevel *restrict pl) {
	SAU_Parser *o = pl->o;
	SAU_Scanner *sc = o->sc;
	/* FIXME: ADD_WAIT_DURATION */
	if (SAU_Scanner_tryc(sc, 't')) {
		if (!pl->last_op) {
			SAU_Scanner_warning(sc, NULL,
"add wait for last duration before any parts given");
			return false;
		}
		pl->last_event->ev_flags |= SAU_PDEV_ADD_WAIT_DURATION;
	} else {
		uint32_t wait_ms;
		if (!scan_time_val(sc, &wait_ms))
			return false;
		pl->next_wait_ms += wait_ms;
	}
	return true;
}

/*
 * Node- and scope-handling functions
 */

static SAU_ParseOpList *create_op_list(uint8_t list_type,
		SAU_MemPool *restrict memp) {
	SAU_ParseOpList *ol = SAU_MemPool_alloc(memp, sizeof(SAU_ParseOpList));
	if (!ol)
		return NULL;
	ol->type = list_type;
	return ol;
}

static SAU_ParseOpRef *op_list_add(SAU_ParseOpList *restrict ol,
		SAU_ParseOpData *restrict data, uint8_t ref_mode,
		SAU_MemPool *restrict memp) {
	SAU_ParseOpRef *ref = SAU_MemPool_alloc(memp, sizeof(SAU_ParseOpRef));
	if (!ref)
		return NULL;
	ref->data = data;
	if (!ol->refs)
		ol->refs = ref;
	if (!ol->new_refs)
		ol->new_refs = ref;
	else
		ol->last_ref->next = ref;
	ol->last_ref = ref;
	ref->mode = ref_mode;
	ref->list_type = ol->type;
	return ref;
}

static sauNoinline void end_operator(ParseLevel *restrict pl) {
	if (!(pl->pl_flags & SDPL_ACTIVE_OP))
		return;
	pl->pl_flags &= ~SDPL_ACTIVE_OP;
	SAU_Parser *o = pl->o;
	ScanLookup *sl = &o->sl;
	SAU_ParseOpData *op = pl->op_ref->data;
	if (SAU_Ramp_ENABLED(&op->freq))
		op->op_params |= SAU_POPP_FREQ;
	if (SAU_Ramp_ENABLED(&op->freq2))
		op->op_params |= SAU_POPP_FREQ2;
	if (SAU_Ramp_ENABLED(&op->amp)) {
		op->op_params |= SAU_POPP_AMP;
		if (!(op->op_flags & SAU_PDOP_NESTED)) {
			op->amp.v0 *= sl->sopt.ampmult;
			op->amp.vt *= sl->sopt.ampmult;
		}
	}
	if (SAU_Ramp_ENABLED(&op->amp2)) {
		op->op_params |= SAU_POPP_AMP2;
		if (!(op->op_flags & SAU_PDOP_NESTED)) {
			op->amp2.v0 *= sl->sopt.ampmult;
			op->amp2.vt *= sl->sopt.ampmult;
		}
	}
	SAU_ParseOpData *pop = op->prev;
	if (!pop) {
		/*
		 * Reset all operator state for initial event.
		 */
		op->op_params |= SAU_POP_PARAMS;
	} else {
		if (op->wave != pop->wave)
			op->op_params |= SAU_POPP_WAVE;
		/* SAU_TIME set when time set */
		if (op->silence_ms != 0)
			op->op_params |= SAU_POPP_SILENCE;
		/* SAU_PHASE set when phase set */
	}
	pl->op_ref = NULL;
	pl->last_op = op;
}

static sauNoinline void end_event(ParseLevel *restrict pl) {
	if (!(pl->pl_flags & SDPL_ACTIVE_EV))
		return;
	pl->pl_flags &= ~SDPL_ACTIVE_EV;
	SAU_ParseEvData *e = pl->event;
	end_operator(pl);
	if (SAU_Ramp_ENABLED(&e->pan))
		e->vo_params |= SAU_PVOP_PAN;
	SAU_ParseEvData *pve = e->vo_prev;
	if (!pve) {
		/*
		 * Reset all voice state for initial event.
		 */
		e->vo_params |= SAU_PVO_PARAMS & ~SAU_PVOP_GRAPH;
	}
	pl->last_event = e;
	pl->event = NULL;
}

static void begin_event(ParseLevel *restrict pl,
		SAU_ParseEvData *restrict pve,
		bool is_composite) {
	SAU_Parser *o = pl->o;
	ScanLookup *sl = &o->sl;
	end_event(pl);
	SAU_ParseEvData *e = SAU_MemPool_alloc(o->mp, sizeof(SAU_ParseEvData));
	pl->event = e;
	e->wait_ms = pl->next_wait_ms;
	pl->next_wait_ms = 0;
	e->op_list.type = SAU_PDNL_GRAPH;
	SAU_Ramp_reset(&e->pan);
	if (pve != NULL) {
		if (is_composite) {
			if (!pl->composite) {
				pve->composite = e;
				pl->composite = pve;
			} else {
				pve->next = e;
			}
		}
		e->vo_prev = pve;
	} else {
		/*
		 * New voice with initial parameter values.
		 */
		e->pan.v0 = sl->sopt.def_chanmix;
		e->pan.flags |= SAU_RAMPP_STATE;
	}
	if (!pl->group_from)
		pl->group_from = e;
	if (!is_composite) {
		if (!o->first_ev)
			o->first_ev = e;
		else
			o->ev->next = e;
		o->ev = e;
		pl->composite = NULL;
	}
	pl->pl_flags |= SDPL_ACTIVE_EV;
}

/*
 * Add new operator to parent(s), ie. either to the
 * current event node, or to an operator node (ordinary or multiple)
 * in the case of operator linking/nesting.
 */
static SAU_ParseOpRef *list_operator(ParseLevel *restrict pl,
		SAU_ParseOpData *od, uint8_t ref_mode) {
	SAU_Parser *o = pl->o;
	SAU_ParseEvData *e = pl->event;
	SAU_ParseOpList *ol = pl->op_list;
	if (pl->list_type == SAU_PDNL_GRAPH ||
			!(ref_mode & SAU_PDNR_ADD)) {
		ol = &e->op_list;
	}
	SAU_ParseOpRef *ref = op_list_add(ol, od, ref_mode, o->mp);
	pl->op_ref = ref;
	if (!pl->first_op_ref)
		pl->first_op_ref = ref;
	pl->last_list_type = pl->list_type; /* FIXME: kludge */
	return ref;
}

/*
 * Begin a new operator - depending on the context, either for the present
 * event or for a new event begun.
 */
static void begin_operator(ParseLevel *restrict pl,
		SAU_ParseOpRef *restrict prev_op_ref,
		uint8_t ref_mode, bool is_composite) {
	SAU_Parser *o = pl->o;
	ScanLookup *sl = &o->sl;
	if (!pl->event || /* not in event means previous implicitly ended */
			pl->location != SDPL_IN_EVENT ||
			pl->next_wait_ms ||
			is_composite)
		begin_event(pl, (prev_op_ref != NULL ?
					prev_op_ref->data->event : NULL),
				is_composite);
	SAU_ParseEvData *e = pl->event;
	end_operator(pl);
	SAU_ParseOpData *op = SAU_MemPool_alloc(o->mp, sizeof(SAU_ParseOpData));
	if (!is_composite && pl->last_op != NULL)
		pl->last_op->next_bound = op;
	SAU_ParseOpRef *ref = list_operator(pl, op, ref_mode);
	/*
	 * Initialize node.
	 */
	op->time.v_ms = sl->sopt.def_time_ms; /* time is not copied */
	SAU_Ramp_reset(&op->freq);
	SAU_Ramp_reset(&op->freq2);
	SAU_Ramp_reset(&op->amp);
	SAU_Ramp_reset(&op->amp2);
	if (prev_op_ref != NULL) {
		SAU_ParseOpData *pop = prev_op_ref->data;
		op->prev = pop;
		op->op_flags = pop->op_flags &
			(SAU_PDOP_NESTED | SAU_PDOP_MULTIPLE);
		if (is_composite) {
			pop->op_flags |= SAU_PDOP_HAS_COMPOSITE;
		} else {
			op->time.flags |= SAU_TIMEP_SET;
		}
		op->wave = pop->wave;
		op->phase = pop->phase;
		if ((pl->pl_flags & SDPL_BIND_MULTIPLE) != 0) {
			SAU_ParseOpData *mpop = pop;
			uint32_t max_time = 0;
			do {
				if (max_time < mpop->time.v_ms)
					max_time = mpop->time.v_ms;
			} while ((mpop = mpop->next_bound) != NULL);
			op->op_flags |= SAU_PDOP_MULTIPLE;
			op->time.v_ms = max_time;
			pl->pl_flags &= ~SDPL_BIND_MULTIPLE;
		}
	} else {
		/*
		 * New operator with initial parameter values.
		 */
		if (ref->list_type == SAU_PDNL_GRAPH) {
			op->freq.v0 = sl->sopt.def_freq;
		} else {
			op->op_flags |= SAU_PDOP_NESTED;
			op->freq.v0 = sl->sopt.def_relfreq;
			op->freq.flags |= SAU_RAMPP_STATE_RATIO;
		}
		op->freq.flags |= SAU_RAMPP_STATE;
		op->amp.v0 = 1.0f;
		op->amp.flags |= SAU_RAMPP_STATE;
	}
	op->event = e;
	/*
	 * Assign label. If no new label but previous node (for a non-composite)
	 * has one, update label to point to new node, but keep pointer in
	 * previous node.
	 */
	if (pl->set_label != NULL) {
		ref->label_sym = pl->set_label;
		ref->label_sym->data = ref;
		pl->set_label = NULL;
	} else if (!is_composite && prev_op_ref != NULL
			&& prev_op_ref->label_sym != NULL) {
		ref->label_sym = prev_op_ref->label_sym;
		ref->label_sym->data = ref;
	}
	pl->pl_flags |= SDPL_ACTIVE_OP;
}

static void begin_scope(SAU_Parser *restrict o, ParseLevel *restrict pl,
		ParseLevel *restrict parent_pl,
		uint8_t list_type, uint8_t newscope) {
	*pl = (ParseLevel){0};
	pl->o = o;
	pl->scope = newscope;
	pl->list_type = list_type;
	if (!parent_pl) {
		// handle newscope == SCOPE_TOP here
		pl->op_list = create_op_list(list_type, o->mp);
		return;
	}
	pl->parent = parent_pl;
	pl->pl_flags = parent_pl->pl_flags &
		(SDPL_NESTED_SCOPE | SDPL_BIND_MULTIPLE);
	pl->location = parent_pl->location;
	pl->event = parent_pl->event;
	pl->op_ref = parent_pl->op_ref;
	pl->parent_op_ref = parent_pl->parent_op_ref;
	switch (newscope) {
	case SCOPE_BLOCK:
		pl->group_from = parent_pl->group_from;
		pl->op_list = parent_pl->op_list;
		break;
	case SCOPE_BIND:
		pl->group_from = parent_pl->group_from;
		pl->op_list = create_op_list(list_type, o->mp);
		break;
	case SCOPE_NEST:
		pl->pl_flags |= SDPL_NESTED_SCOPE;
		pl->parent_op_ref = parent_pl->op_ref;
		pl->op_list = create_op_list(list_type, o->mp);
		break;
	default:
		break;
	}
}

static void end_scope(ParseLevel *restrict pl) {
	SAU_Parser *o = pl->o;
	end_operator(pl);
	if (pl->set_label != NULL) {
		SAU_Scanner_warning(o->sc, NULL,
				"ignoring label assignment without operator");
	}
	switch (pl->scope) {
	case SCOPE_TOP: {
		/*
		 * At end of top scope (ie. at end of script),
		 * end last event and adjust timing.
		 */
		SAU_ParseEvData *group_to;
		end_event(pl);
		group_to = (pl->composite) ? pl->composite : pl->last_event;
		if (group_to)
			group_to->groupfrom = pl->group_from;
		break; }
	case SCOPE_BLOCK:
		if (!pl->parent->group_from)
			pl->parent->group_from = pl->group_from;
		if (pl->pl_flags & SDPL_ACTIVE_EV) {
			end_event(pl->parent);
			pl->parent->pl_flags |= SDPL_ACTIVE_EV;
			pl->parent->event = pl->event;
		}
		if (pl->last_event != NULL)
			pl->parent->last_event = pl->last_event;
		break;
	case SCOPE_BIND:
		if (!pl->parent->group_from)
			pl->parent->group_from = pl->group_from;
		/*
		 * Begin multiple-operator node in parent scope
		 * for the operator nodes in this scope,
		 * provided any are present.
		 */
		if (pl->first_op_ref != NULL) {
			pl->parent->pl_flags |= SDPL_BIND_MULTIPLE;
			uint8_t list_type = pl->parent->list_type;
			pl->parent->list_type = pl->parent->last_list_type;
			begin_operator(pl->parent, pl->first_op_ref,
					SAU_PDNR_UPDATE, false);
			pl->parent->list_type = list_type;
		}
		break;
	case SCOPE_NEST: {
		if (!pl->parent_op_ref)
			break;
		SAU_ParseOpData *parent_op = pl->parent_op_ref->data;
		if (!parent_op->nest_lists)
			parent_op->nest_lists = pl->op_list;
		else
			parent_op->last_nest_list->next = pl->op_list;
		parent_op->last_nest_list = pl->op_list;
		break; }
	default:
		break;
	}
}

/*
 * Main parser functions
 */

static bool parse_settings(ParseLevel *restrict pl) {
	SAU_Parser *o = pl->o;
	ScanLookup *sl = &o->sl;
	SAU_Scanner *sc = o->sc;
	pl->location = SDPL_IN_DEFAULTS;
	for (;;) {
		uint8_t c = SAU_Scanner_getc(sc);
		switch (c) {
		case SAU_SCAN_SPACE:
			break;
		case 'a':
			if (scan_num(sc, NULL, &sl->sopt.ampmult))
				sl->sopt.changed |= SAU_SOPT_AMPMULT;
			break;
		case 'c':
			if (scan_num(sc, scan_chanmix_const,
						&sl->sopt.def_chanmix))
				sl->sopt.changed |= SAU_SOPT_DEF_CHANMIX;
			break;
		case 'f':
			if (scan_num(sc, scan_note_const, &sl->sopt.def_freq))
				sl->sopt.changed |= SAU_SOPT_DEF_FREQ;
			break;
		case 'n': {
			float freq;
			if (scan_num(sc, NULL, &freq)) {
				if (freq < 1.f) {
					SAU_Scanner_warning(sc, NULL,
"ignoring tuning frequency (Hz) below 1.0");
					break;
				}
				sl->sopt.A4_freq = freq;
				sl->sopt.changed |= SAU_SOPT_A4_FREQ;
			}
			break; }
		case 'r':
			if (scan_num(sc, NULL, &sl->sopt.def_relfreq))
				sl->sopt.changed |= SAU_SOPT_DEF_RELFREQ;
			break;
		case 't':
			if (scan_time_val(sc, &sl->sopt.def_time_ms))
				sl->sopt.changed |= SAU_SOPT_DEF_TIME;
			break;
		default:
			goto UNKNOWN;
		}
	}
	return false;
UNKNOWN:
	SAU_Scanner_ungetc(sc);
	return true; /* let parse_level() take care of it */
}

static bool parse_level(SAU_Parser *restrict o,
		ParseLevel *restrict parent_pl,
		uint8_t list_type, uint8_t newscope);

static bool parse_ev_amp(ParseLevel *restrict pl) {
	SAU_Parser *o = pl->o;
	SAU_Scanner *sc = o->sc;
	SAU_ParseOpData *op = pl->op_ref->data;
	scan_ramp(sc, NULL, &op->amp, false);
	if (SAU_Scanner_tryc(sc, ',')) {
		scan_ramp(sc, NULL, &op->amp2, false);
	}
	if (SAU_Scanner_tryc(sc, '~') && SAU_Scanner_tryc(sc, '[')) {
		op->op_params |= SAU_POPP_ADJCS;
		parse_level(o, pl, SAU_PDNL_AMODS, SCOPE_NEST);
	}
	return false;
}

static bool parse_ev_chanmix(ParseLevel *restrict pl) {
	SAU_Parser *o = pl->o;
	SAU_Scanner *sc = o->sc;
	SAU_ParseEvData *e = pl->event;
	SAU_ParseOpData *op = pl->op_ref->data;
	if (op->op_flags & SAU_PDOP_NESTED)
		return true; // reject
	scan_ramp(sc, scan_chanmix_const, &e->pan, false);
	return false;
}

static bool parse_ev_freq(ParseLevel *restrict pl, bool rel_freq) {
	SAU_Parser *o = pl->o;
	SAU_Scanner *sc = o->sc;
	SAU_ParseOpData *op = pl->op_ref->data;
	if (rel_freq && !(op->op_flags & SAU_PDOP_NESTED))
		return true; // reject
	SAU_ScanNumConst_f numconst_f = rel_freq ? NULL : scan_note_const;
	scan_ramp(sc, numconst_f, &op->freq, rel_freq);
	if (SAU_Scanner_tryc(sc, ',')) {
		scan_ramp(sc, numconst_f, &op->freq2, rel_freq);
	}
	if (SAU_Scanner_tryc(sc, '~') && SAU_Scanner_tryc(sc, '[')) {
		op->op_params |= SAU_POPP_ADJCS;
		parse_level(o, pl, SAU_PDNL_FMODS, SCOPE_NEST);
	}
	return false;
}

static bool parse_ev_phase(ParseLevel *restrict pl) {
	SAU_Parser *o = pl->o;
	SAU_Scanner *sc = o->sc;
	SAU_ParseOpData *op = pl->op_ref->data;
	if (scan_num(sc, NULL, &op->phase)) {
		op->phase = fmod(op->phase, 1.f);
		if (op->phase < 0.f)
			op->phase += 1.f;
		op->op_params |= SAU_POPP_PHASE;
	}
	if (SAU_Scanner_tryc(sc, '+') && SAU_Scanner_tryc(sc, '[')) {
		op->op_params |= SAU_POPP_ADJCS;
		parse_level(o, pl, SAU_PDNL_PMODS, SCOPE_NEST);
	}
	return false;
}

static bool parse_step(ParseLevel *restrict pl) {
	SAU_Parser *o = pl->o;
	SAU_Scanner *sc = o->sc;
	if (!pl->op_ref) {
		SAU_error("parser", "parse_step() called with NULL op_ref");
		return false;
	}
	SAU_ParseOpData *op = pl->op_ref->data;
	pl->location = SDPL_IN_EVENT;
	for (;;) {
		uint8_t c = SAU_Scanner_getc(sc);
		switch (c) {
		case SAU_SCAN_SPACE:
			break;
		case '\\':
			if (parse_waittime(pl)) {
				begin_operator(pl, pl->op_ref,
						SAU_PDNR_UPDATE, false);
			}
			break;
		case 'a':
			if (parse_ev_amp(pl)) goto UNKNOWN;
			break;
		case 'c':
			if (parse_ev_chanmix(pl)) goto UNKNOWN;
			break;
		case 'f':
			if (parse_ev_freq(pl, false)) goto UNKNOWN;
			break;
		case 'p':
			if (parse_ev_phase(pl)) goto UNKNOWN;
			break;
		case 'r':
			if (parse_ev_freq(pl, true)) goto UNKNOWN;
			break;
		case 's':
			scan_time_val(sc, &op->silence_ms);
			break;
		case 't':
			if (SAU_Scanner_tryc(sc, '*')) {
				/* later fitted or kept to default */
				op->time.v_ms = o->sl.sopt.def_time_ms;
				op->time.flags = 0;
			} else if (SAU_Scanner_tryc(sc, 'i')) {
				if (!(op->op_flags & SAU_PDOP_NESTED)) {
					SAU_Scanner_warning(sc, NULL,
"ignoring 'ti' (infinite time) for non-nested operator");
					break;
				}
				op->time.flags |= SAU_TIMEP_SET
					| SAU_TIMEP_LINKED;
			} else {
				if (!scan_time_val(sc, &op->time.v_ms))
					break;
				op->time.flags = SAU_TIMEP_SET;
			}
			op->op_params |= SAU_POPP_TIME;
			break;
		case 'w': {
			size_t wave;
			if (!scan_wavetype(sc, &wave))
				break;
			op->wave = wave;
			break; }
		default:
			goto UNKNOWN;
		}
	}
	return false;
UNKNOWN:
	SAU_Scanner_ungetc(sc);
	return true; /* let parse_level() take care of it */
}

enum {
	HANDLE_DEFER = 1<<0,
	DEFERRED_STEP = 1<<1,
	DEFERRED_SETTINGS = 1<<2,
};
static bool parse_level(SAU_Parser *restrict o,
		ParseLevel *restrict parent_pl,
		uint8_t list_type, uint8_t newscope) {
	ParseLevel pl;
	SAU_SymStr *label;
	uint8_t flags = 0;
	bool endscope = false;
	begin_scope(o, &pl, parent_pl, list_type, newscope);
	++o->call_level;
	SAU_Scanner *sc = o->sc;
	for (;;) {
		uint8_t c = SAU_Scanner_getc(sc);
		switch (c) {
		case SAU_SCAN_SPACE:
			break;
		case SAU_SCAN_LNBRK:
			if (pl.scope == SCOPE_TOP) {
				/*
				 * On top level of script,
				 * each line has a new "subscope".
				 */
				if (o->call_level > 1)
					goto RETURN;
				flags = 0;
				pl.location = SDPL_IN_NONE;
				pl.first_op_ref = NULL;
			}
			break;
		case '\'':
			/*
			 * Label assignment (set to what follows).
			 */
			if (pl.set_label != NULL) {
				SAU_Scanner_warning(sc, NULL,
"ignoring label assignment to label assignment");
				break;
			}
			label = scan_label(sc, c);
			if (label != NULL)
				pl.set_label = label;
			break;
		case ';':
			if (pl.location == SDPL_IN_DEFAULTS || !pl.event)
				goto INVALID;
			begin_operator(&pl, pl.op_ref, SAU_PDNR_UPDATE, true);
			flags = parse_step(&pl) ?
				(HANDLE_DEFER | DEFERRED_STEP) :
				0;
			break;
		case '@':
			if (SAU_Scanner_tryc(sc, '[')) {
				end_operator(&pl);
				if (parse_level(o, &pl, list_type, SCOPE_BIND))
					goto RETURN;
				/*
				 * Multiple-operator node now open.
				 */
				flags = parse_step(&pl) ?
					(HANDLE_DEFER | DEFERRED_STEP) :
					0;
				break;
			}
			/*
			 * Label reference (get and use value).
			 */
			if (pl.set_label != NULL) {
				SAU_Scanner_warning(sc, NULL,
"ignoring label assignment to label reference");
				pl.set_label = NULL;
			}
			pl.location = SDPL_IN_NONE;
			label = scan_label(sc, c);
			if (label != NULL) {
				SAU_ParseOpRef *ref = label->data;
				if (!ref)
					SAU_Scanner_warning(sc, NULL,
"ignoring reference to undefined label");
				else {
					begin_operator(&pl, ref,
							SAU_PDNR_UPDATE, false);
					flags = parse_step(&pl) ?
						(HANDLE_DEFER | DEFERRED_STEP) :
						0;
				}
			}
			break;
		case 'O': {
			size_t wave;
			if (!scan_wavetype(sc, &wave))
				break;
			begin_operator(&pl, NULL, SAU_PDNR_ADD, false);
			pl.op_ref->data->wave = wave;
			flags = parse_step(&pl) ?
				(HANDLE_DEFER | DEFERRED_STEP) :
				0;
			break; }
		case 'S':
			flags = parse_settings(&pl) ?
				(HANDLE_DEFER | DEFERRED_SETTINGS) :
				0;
			break;
		case '[':
			if (parse_level(o, &pl, list_type, SCOPE_BLOCK))
				goto RETURN;
			break;
		case '\\':
			if (pl.location == SDPL_IN_DEFAULTS ||
					((pl.pl_flags & SDPL_NESTED_SCOPE) != 0
					&& pl.event != NULL))
				goto INVALID;
			parse_waittime(&pl);
			break;
		case ']':
			if (pl.scope == SCOPE_NEST) {
				end_operator(&pl);
			}
			if (pl.scope > SCOPE_TOP) {
				endscope = true;
				goto RETURN;
			}
			warn_closing_without_opening(sc, ']', '[');
			break;
		case '|':
			if (pl.location == SDPL_IN_DEFAULTS ||
					((pl.pl_flags & SDPL_NESTED_SCOPE) != 0
					&& pl.event != NULL))
				goto INVALID;
			if (!pl.event) {
				SAU_Scanner_warning(sc, NULL,
"end of sequence before any parts given");
				break;
			}
			if (pl.group_from != NULL) {
				SAU_ParseEvData *group_to = (pl.composite) ?
					pl.composite :
					pl.event;
				group_to->groupfrom = pl.group_from;
				pl.group_from = NULL;
			}
			end_event(&pl);
			flags &= ~DEFERRED_STEP;
			pl.location = SDPL_IN_NONE;
			break;
		case '}':
			warn_closing_without_opening(sc, '}', '{');
			break;
		default:
		INVALID:
			if (!handle_unknown_or_eof(sc, c)) goto FINISH;
			break;
		}
		/* Return to sub-parsing routines. */
		if (flags != 0 && !(flags & HANDLE_DEFER)) {
			uint8_t test = flags;
			flags = 0;
			if ((test & DEFERRED_STEP) != 0) {
				if (parse_step(&pl))
					flags = HANDLE_DEFER | DEFERRED_STEP;
			} else if ((test & DEFERRED_SETTINGS) != 0)
				if (parse_settings(&pl))
					flags = HANDLE_DEFER | DEFERRED_SETTINGS;
		}
		flags &= ~HANDLE_DEFER;
	}
FINISH:
	if (newscope > SCOPE_TOP)
		warn_eof_without_closing(sc, ']');
RETURN:
	end_scope(&pl);
	--o->call_level;
	/* Should return from calling scope if/when parent scope is ended. */
	return (endscope && pl.scope != newscope);
}

/*
 * Process file.
 *
 * \return name of script, or NULL on error preventing parse
 */
static const char *parse_file(SAU_Parser *restrict o,
		const char *restrict script, bool is_path) {
	SAU_Scanner *sc = o->sc;
	const char *name;
	if (!SAU_Scanner_open(sc, script, is_path))
		return NULL;
	parse_level(o, NULL, SAU_PDNL_GRAPH, SCOPE_TOP);
	name = sc->f->path;
	SAU_Scanner_close(sc);
	return name;
}

/**
 * Parse a file and return script data.
 *
 * \return instance or NULL on error preventing parse
 */
SAU_Parse* SAU_create_Parse(const char *restrict script_arg, bool is_path) {
	if (!script_arg)
		return NULL;
	SAU_Parser pr;
	if (!init_Parser(&pr))
		return NULL;
	SAU_Parse *o = NULL;
	const char *name = parse_file(&pr, script_arg, is_path);
	if (!name) goto DONE;

	o = SAU_MemPool_alloc(pr.mp, sizeof(SAU_Parse));
	o->events = pr.first_ev;
	o->name = name;
	o->sopt = pr.sl.sopt;
	o->symtab = pr.st;
	o->mem = pr.mp;
	pr.st = NULL; // keep for result
	pr.mp = NULL; // keep for result
DONE:
	fini_Parser(&pr);
	return o;
}

/**
 * Destroy instance.
 */
void SAU_destroy_Parse(SAU_Parse *restrict o) {
	if (!o)
		return;
	SAU_destroy_SymTab(o->symtab);
	SAU_destroy_MemPool(o->mem);
}
