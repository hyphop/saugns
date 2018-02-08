/* sgensys: Program builder module.
 * Copyright (c) 2011-2012, 2017-2018 Joel K. Pettersson
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

#include "program.h"
#include "parser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void print_linked(const char *header, const char *footer,
		uint32_t count, const int32_t *nodes) {
  uint32_t i;
  if (!count) return;
  printf("%s%d", header, nodes[0]);
  for (i = 0; ++i < count; )
    printf(", %d", nodes[i]);
  printf("%s", footer);
}

static void build_graph(struct SGSProgramEvent *root,
		const SGSEventNode *voice_in) {
  SGSOperatorNode **nl;
  struct SGSProgramGraph *graph, **graph_out;
  uint32_t i;
  uint32_t size;
  if (!voice_in->voice_params & SGS_GRAPH)
    return;
  size = voice_in->graph.count;
  graph_out = (struct SGSProgramGraph**)&root->voice->graph;
  if (!size) {
    *graph_out = 0;
    return;
  }
  nl = SGS_NODELIST_GET(&voice_in->graph);
  graph = malloc(sizeof(struct SGSProgramGraph) + sizeof(int32_t) * (size - 1));
  graph->opc = size;
  for (i = 0; i < size; ++i)
    graph->ops[i] = nl[i]->operator_id;
  *graph_out = graph;
}

static void build_adjcs(struct SGSProgramEvent *root,
		const SGSOperatorNode *operator_in) {
  SGSOperatorNode **nl;
  struct SGSProgramGraphAdjcs *adjcs, **adjcs_out;
  int32_t *data;
  uint32_t i;
  uint32_t size;
  if (!operator_in || !(operator_in->operator_params & SGS_ADJCS))
    return;
  size = operator_in->fmods.count +
         operator_in->pmods.count +
         operator_in->amods.count;
  adjcs_out = (struct SGSProgramGraphAdjcs**)&root->operator->adjcs;
  if (!size) {
    *adjcs_out = 0;
    return;
  }
  adjcs = malloc(sizeof(struct SGSProgramGraphAdjcs)
		+ sizeof(int32_t) * (size - 1));
  adjcs->fmodc = operator_in->fmods.count;
  adjcs->pmodc = operator_in->pmods.count;
  adjcs->amodc = operator_in->amods.count;
  data = adjcs->adjcs;
  nl = SGS_NODELIST_GET(&operator_in->fmods);
  for (i = 0; i < adjcs->fmodc; ++i)
    *data++ = nl[i]->operator_id;
  nl = SGS_NODELIST_GET(&operator_in->pmods);
  for (i = 0; i < adjcs->pmodc; ++i)
    *data++ = nl[i]->operator_id;
  nl = SGS_NODELIST_GET(&operator_in->amods);
  for (i = 0; i < adjcs->amodc; ++i)
    *data++ = nl[i]->operator_id;
  *adjcs_out = adjcs;
}

/*
 * Program (event, voice, operator) allocation
 */

typedef struct VoiceAllocData {
  SGSEventNode *last;
  uint32_t duration_ms;
} VoiceAllocData;

typedef struct VoiceAlloc {
  VoiceAllocData *data;
  uint32_t voicec;
  uint32_t alloc;
} VoiceAlloc;

static void voice_alloc_init(VoiceAlloc *va) {
  va->data = calloc(1, sizeof(VoiceAllocData));
  va->voicec = 0;
  va->alloc = 1;
}

static void voice_alloc_fini(VoiceAlloc *va, struct SGSProgram *prg) {
  prg->voicec = va->voicec;
  free(va->data);
}

/*
 * Returns the longest operator duration among top-level operators for
 * the graph of the voice event.
 */
static uint32_t voice_duration(SGSEventNode *ve) {
  SGSOperatorNode **nl = SGS_NODELIST_GET(&ve->operators);
  uint32_t i;
  uint32_t duration_ms = 0;
  /* FIXME: node list type? */
  for (i = 0; i < ve->operators.count; ++i) {
    SGSOperatorNode *op = nl[i];
    if (op->time_ms > (int32_t)duration_ms)
      duration_ms = op->time_ms;
  }
  return duration_ms;
}

/*
 * Incremental voice allocation - allocate voice for event,
 * returning voice id.
 */
static size_t voice_alloc_inc(VoiceAlloc *va, SGSEventNode *e) {
  uint32_t voice;
  for (voice = 0; voice < va->voicec; ++voice) {
    if ((int32_t)va->data[voice].duration_ms < e->wait_ms)
      va->data[voice].duration_ms = 0;
    else
      va->data[voice].duration_ms -= e->wait_ms;
  }
  if (e->voice_prev) {
    SGSEventNode *prev = e->voice_prev;
    voice = prev->voice_id;
  } else {
    for (voice = 0; voice < va->voicec; ++voice)
      if (!(va->data[voice].last->en_flags & EN_VOICE_LATER_USED) &&
          va->data[voice].duration_ms == 0) break;
    /*
     * If no unused voice found, allocate new one.
     */
    if (voice == va->voicec) {
      ++va->voicec;
      if (va->voicec > va->alloc) {
        uint32_t i = va->alloc;
        va->alloc <<= 1;
        va->data = realloc(va->data, va->alloc * sizeof(VoiceAllocData));
        while (i < va->alloc) {
          va->data[i].last = 0;
          va->data[i].duration_ms = 0;
          ++i;
        }
      }
    }
  }
  e->voice_id = voice;
  va->data[voice].last = e;
  if (e->voice_params & SGS_GRAPH)
    va->data[voice].duration_ms = voice_duration(e);
  return voice;
}

typedef struct OperatorAllocData {
  SGSOperatorNode *last;
  struct SGSProgramEvent *out;
  uint32_t duration_ms;
} OperatorAllocData;

typedef struct OperatorAlloc {
  OperatorAllocData *data;
  uint32_t operatorc;
  uint32_t alloc;
} OperatorAlloc;

static void operator_alloc_init(OperatorAlloc *oa) {
  oa->data = calloc(1, sizeof(OperatorAllocData));
  oa->operatorc = 0;
  oa->alloc = 1;
}

static void operator_alloc_fini(OperatorAlloc *oa, struct SGSProgram *prg) {
  prg->operatorc = oa->operatorc;
  free(oa->data);
}

/*
 * Incremental operator allocation - allocate operator for event,
 * returning operator id.
 *
 * Only valid to call for single-operator nodes.
 */
static size_t operator_alloc_inc(OperatorAlloc *oa, SGSOperatorNode *op) {
  SGSEventNode *e = op->event;
  uint32_t operator;
  for (operator = 0; operator < oa->operatorc; ++operator) {
    if ((int32_t)oa->data[operator].duration_ms < e->wait_ms)
      oa->data[operator].duration_ms = 0;
    else
      oa->data[operator].duration_ms -= e->wait_ms;
  }
  if (op->on_prev) {
    SGSOperatorNode *pop = op->on_prev;
    operator = pop->operator_id;
  } else {
//    for (operator = 0; operator < oa->operatorc; ++operator)
//      if (!(oa->data[operator].last->on_flags & ON_OPERATOR_LATER_USED) &&
//          oa->data[operator].duration_ms == 0) break;
    /*
     * If no unused operator found, allocate new one.
     */
    if (operator == oa->operatorc) {
      ++oa->operatorc;
      if (oa->operatorc > oa->alloc) {
        uint32_t i = oa->alloc;
        oa->alloc <<= 1;
        oa->data = realloc(oa->data, oa->alloc * sizeof(OperatorAllocData));
        while (i < oa->alloc) {
          oa->data[i].last = 0;
          oa->data[i].duration_ms = 0;
          ++i;
        }
      }
    }
  }
  op->operator_id = operator;
  oa->data[operator].last = op;
//  oa->data[operator].duration_ms = op->time_ms;
  return operator;
}

typedef struct ProgramAlloc {
  struct SGSProgramEvent *oe, **oevents;
  size_t eventc;
  size_t alloc;
  OperatorAlloc oa;
  VoiceAlloc va;
} ProgramAlloc;

static void program_alloc_init(ProgramAlloc *pa) {
  voice_alloc_init(&(pa)->va);
  operator_alloc_init(&(pa)->oa);
  pa->oe = 0;
  pa->oevents = 0;
  pa->eventc = 0;
  pa->alloc = 0;
}

static void program_alloc_fini(ProgramAlloc *pa, struct SGSProgram *prg) {
  size_t i;
  /* copy output events to program & cleanup */
  *(struct SGSProgramEvent**)&prg->events
	= malloc(sizeof(struct SGSProgramEvent) * pa->eventc);
  for (i = 0; i < pa->eventc; ++i) {
    *(struct SGSProgramEvent*)&prg->events[i] = *pa->oevents[i];
    free(pa->oevents[i]);
  }
  free(pa->oevents);
  prg->eventc = pa->eventc;
  operator_alloc_fini(&pa->oa, prg);
  voice_alloc_fini(&pa->va, prg);
}

static struct SGSProgramEvent *program_alloc_oevent(ProgramAlloc *pa,
		uint32_t voice_id) {
  ++pa->eventc;
  if (pa->eventc > pa->alloc) {
    pa->alloc = (pa->alloc > 0) ? pa->alloc << 1 : 1;
    pa->oevents = realloc(pa->oevents, sizeof(struct SGSProgramEvent*) * pa->alloc);
  }
  pa->oevents[pa->eventc - 1] = calloc(1, sizeof(struct SGSProgramEvent));
  pa->oe = pa->oevents[pa->eventc - 1];
  pa->oe->voice_id = voice_id;
  return pa->oe;
}

/*
 * Overwrite parameters in dst that have values in src.
 */
static void copy_params(SGSOperatorNode *dst, const SGSOperatorNode *src) {
  if (src->operator_params & SGS_AMP) dst->amp = src->amp;
}

static void expand_operator(SGSOperatorNode *op) {
  SGSOperatorNode *pop;
  if (!(op->on_flags & ON_MULTIPLE_OPERATORS)) return;
  pop = op->on_prev;
  do {
    copy_params(pop, op);
    expand_operator(pop);
  } while ((pop = pop->next_bound));
  SGS_nodelist_clear(&op->fmods);
  SGS_nodelist_clear(&op->pmods);
  SGS_nodelist_clear(&op->amods);
  op->operator_params = 0;
}

/*
 * Convert data for an operator node to program operator data, setting it for
 * the program event given.
 */
static void program_convert_onode(ProgramAlloc *pa, SGSOperatorNode *op,
                                  uint32_t operator_id) {
  struct SGSProgramEvent *oe = pa->oa.data[operator_id].out;
  struct SGSProgramOperatorData *ood = calloc(1, sizeof(struct SGSProgramOperatorData));
  oe->operator = ood;
  oe->params |= op->operator_params;
  //printf("operator_id == %d | address == %x\n", op->operator_id, op);
  ood->operator_id = operator_id;
  ood->adjcs = 0;
  ood->attr = op->attr;
  ood->wave = op->wave;
  ood->time_ms = op->time_ms;
  ood->silence_ms = op->silence_ms;
  ood->freq = op->freq;
  ood->dynfreq = op->dynfreq;
  ood->phase = op->phase;
  ood->amp = op->amp;
  ood->dynamp = op->dynamp;
  ood->valitfreq = op->valitfreq;
  ood->valitamp = op->valitamp;
  if (op->operator_params & SGS_ADJCS) {
    build_adjcs(oe, op);
  }
}

/*
 * Visit each operator node in the node list and recurse through each node's
 * sublists in turn, following and converting operator data and allocating
 * new output events as needed.
 */
static void program_follow_onodes(ProgramAlloc *pa, SGSNodeList *nl) {
  uint32_t i;
  SGSOperatorNode **list = SGS_NODELIST_GET(nl);
  for (i = nl->inactive_count; i < nl->count; ++i) {
    SGSOperatorNode *op = list[i];
    OperatorAllocData *ad;
    uint32_t operator_id;
    if (op->on_flags & ON_MULTIPLE_OPERATORS) continue;
    operator_id = operator_alloc_inc(&pa->oa, op);
    program_follow_onodes(pa, &op->fmods);
    program_follow_onodes(pa, &op->pmods);
    program_follow_onodes(pa, &op->amods);
    ad = &pa->oa.data[operator_id];
    if (pa->oe->operator) {
      uint32_t voice_id = pa->oe->voice_id;
      program_alloc_oevent(pa, voice_id);
    }
    ad->out = pa->oe;
    program_convert_onode(pa, op, operator_id);
  }
}

/*
 * Convert voice and operator data for an event node into a (series of) output
 * events.
 *
 * This is the "main" parser data conversion function, to be called for every
 * event.
 */
static void program_convert_enode(ProgramAlloc *pa, SGSEventNode *e) {
  struct SGSProgramEvent *oe;
  struct SGSProgramVoiceData *ovd;
  /* Add to final output list */
  oe = program_alloc_oevent(pa, voice_alloc_inc(&pa->va, e));
  oe->wait_ms = e->wait_ms;
  program_follow_onodes(pa, &e->operators);
  oe = pa->oe; /* oe may have re(al)located */
  if (e->voice_params) {
    ovd = calloc(1, sizeof(struct SGSProgramVoiceData));
    oe->voice = ovd;
    oe->params |= e->voice_params;
    ovd->attr = e->voice_attr;
    ovd->panning = e->panning;
    ovd->valitpanning = e->valitpanning;
    if (e->voice_params & SGS_GRAPH) {
      build_graph(oe, e);
    }
  }
}

/**
 * Creates instance for the given parser output.
 *
 * Returns instance if successful, NULL on error.
 */
SGSProgram_t SGS_build_program(SGSParseResult_t parse_res) {
  //puts("build():");
  ProgramAlloc pa;
  struct SGSProgram *o = calloc(1, sizeof(struct SGSProgram));
  SGSEventNode *e;
  size_t event_id;
  /*
   * Pass #1 - Output event allocation, voice allocation,
   *           parameter data copying.
   */
  program_alloc_init(&pa);
  for (e = parse_res->events; e; e = e->next) {
    program_convert_enode(&pa, e);
  }
  program_alloc_fini(&pa, o);
  /*
   * Pass #2 - Cleanup of parsing data.
   */
  for (e = parse_res->events; e; ) {
    SGSEventNode *e_next = e->next;
    SGS_event_node_destroy(e);
    e = e_next;
  }
  //puts("/build()");
#if 1
  /*
   * Debug printing.
   */
  putchar('\n');
  printf("events: %ld\tvoices: %d\toperators: %d\n", o->eventc, o->voicec, o->operatorc);
  for (event_id = 0; event_id < o->eventc; ++event_id) {
    SGSProgramEvent_t oe;
    SGSProgramVoiceData_t ovo;
    SGSProgramOperatorData_t oop;
    oe = &o->events[event_id];
    ovo = oe->voice;
    oop = oe->operator;
    printf("\\%d \tEV %ld \t(VI %d)", oe->wait_ms, event_id, oe->voice_id);
    if (ovo) {
      SGSProgramGraph_t g = ovo->graph;
      printf("\n\tvo %d", oe->voice_id);
      if (g)
        print_linked("\n\t    {", "}", g->opc, g->ops);
    }
    if (oop) {
      SGSProgramGraphAdjcs_t ga = oop->adjcs;
      if (oop->time_ms == SGS_TIME_INF)
        printf("\n\top %d \tt=INF \tf=%.f", oop->operator_id, oop->freq);
      else
        printf("\n\top %d \tt=%d \tf=%.f", oop->operator_id, oop->time_ms, oop->freq);
      if (ga) {
        print_linked("\n\t    f!<", ">", ga->fmodc, ga->adjcs);
        print_linked("\n\t    p!<", ">", ga->pmodc, &ga->adjcs[ga->fmodc]);
        print_linked("\n\t    a!<", ">", ga->amodc, &ga->adjcs[ga->fmodc +
                                                               ga->pmodc]);
      }
    }
    putchar('\n');
  }
#endif
  return o;
}

/**
 * Destroys the instance.
 */
void SGS_destroy_program(SGSProgram_t o) {
  size_t i;
  for (i = 0; i < o->eventc; ++i) {
    struct SGSProgramEvent *e = (void*)&o->events[i];
    if (e->voice) {
      free((void*)e->voice->graph);
      free((void*)e->voice);
    }
    if (e->operator) {
      free((void*)e->operator->adjcs);
      free((void*)e->operator);
    }
  }
  free((void*)o->events);
}