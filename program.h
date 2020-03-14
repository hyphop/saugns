/* mgensys: Audio program data.
 * Copyright (c) 2011, 2020 Joel K. Pettersson
 * <joelkpettersson@gmail.com>.
 *
 * This file and the software of which it is part is distributed under the
 * terms of the GNU Lesser General Public License, either version 3 or (at
 * your option) any later version, WITHOUT ANY WARRANTY, not even of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * View the file COPYING for details, or if missing, see
 * <http://www.gnu.org/licenses/>.
 */

#pragma once
#include "wave.h"

/* Node types. */
enum {
	MGS_BASETYPE_SOUND = 0,
	MGS_BASETYPE_SCOPE,
	MGS_BASETYPES,
	MGS_TYPE_OP = MGS_BASETYPES,
	MGS_TYPE_NOISE,
	MGS_TYPE_DUR,
	MGS_TYPE_ARR,
	MGS_TYPE_ENV,
	MGS_TYPES,
};

/* Sound node modulators, all types. */
enum {
	MGS_MOD_NONE = 0,
	MGS_MOD_AM,
	MGS_MOD_FM,
	MGS_MOD_PM,
	MGS_MOD_TYPES
};

/* Operator attributes. */
enum {
	MGS_ATTR_FREQRATIO = 1<<0,
	MGS_ATTR_DYNFREQRATIO = 1<<1
};

/* Operator parameters. */
enum {
	MGS_TIME = 1<<0,
	MGS_WAVE = 1<<1,
	MGS_FREQ = 1<<2,
	MGS_DYNFREQ = 1<<3,
	MGS_PHASE = 1<<4,
	MGS_AMP = 1<<5,
	MGS_DYNAMP = 1<<6,
	MGS_PAN = 1<<7,
	MGS_ATTR = 1<<8,
	MGS_PARAM_MASK = (1<<9) - 1
};

typedef struct MGS_ProgramNode MGS_ProgramNode;
typedef struct MGS_ProgramSoundData MGS_ProgramSoundData;
typedef struct MGS_ProgramOpData MGS_ProgramOpData;
typedef struct MGS_ProgramScopeData MGS_ProgramScopeData;
typedef struct MGS_ProgramDurData MGS_ProgramDurData;
typedef struct MGS_ProgramArrData MGS_ProgramArrData;

/* Time parameter flags. */
enum {
	MGS_TIME_SET = 1<<0,
};

typedef struct MGS_TimePar {
	float v;
	uint32_t flags;
} MGS_TimePar;

struct MGS_ProgramSoundData {
	MGS_TimePar time;
	MGS_ProgramNode *root;
	uint32_t params;
	float amp, dynamp, pan;
	MGS_ProgramArrData *amod;
	MGS_ProgramNode *nested_next;
};

struct MGS_ProgramOpData {
	MGS_ProgramSoundData sound;
	uint8_t attr, wave;
	float freq, dynfreq, phase;
	MGS_ProgramArrData *pmod, *fmod;
};

struct MGS_ProgramScopeData {
	MGS_ProgramNode *first_node;
	MGS_ProgramNode *last_node;
};

struct MGS_ProgramDurData {
	MGS_ProgramScopeData scope;
	MGS_ProgramNode *next;
};

struct MGS_ProgramArrData {
	MGS_ProgramScopeData scope;
	uint32_t count;
	uint8_t mod_type;
};

struct MGS_ProgramNode {
	MGS_ProgramNode *next;
	MGS_ProgramNode *ref_prev;
	float delay;
	uint8_t type;
	uint32_t base_id; // per-base-type id, not increased for references
	uint32_t conv_id; // for use by later processing
	void *data;
};

static inline void *MGS_ProgramNode_get_data(const MGS_ProgramNode *restrict n,
		uint8_t type) {
	if (n->type == type)
		return n->data;
	switch (n->type) {
	case MGS_TYPE_OP:
		return (type == MGS_BASETYPE_SOUND) ? n->data : NULL;
	case MGS_TYPE_DUR:
		return (type == MGS_BASETYPE_SCOPE) ? n->data : NULL;
	default:
		return NULL;
	}
}

struct MGS_MemPool;
struct MGS_SymTab;

typedef struct MGS_LangOpt {
	const char *const*wave_names;
} MGS_LangOpt;

bool MGS_init_LangOpt(MGS_LangOpt *restrict o,
		struct MGS_SymTab *restrict symtab);

struct MGS_Program {
	MGS_ProgramNode *node_list;
	uint32_t node_count;
	uint32_t root_count;
	uint32_t base_counts[MGS_BASETYPES];
	struct MGS_MemPool *mem;
	struct MGS_SymTab *symt;
	const char *name;
	MGS_LangOpt lopt;
};
