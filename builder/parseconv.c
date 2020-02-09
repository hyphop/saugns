/* sgensys: Parser output to script data converter.
 * Copyright (c) 2011-2012, 2017-2019 Joel K. Pettersson
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

#include "parser.h"
#include <stdlib.h>

/*
 * Script data construction from parse data.
 *
 * Adjust and replace data structures. The per-event
 * operator list becomes flat, with separate lists kept for
 * recursive traversal in scriptconv.
 */

/*
 * Adjust timing for event groupings; the script syntax for time grouping is
 * only allowed on the "top" operator level, so the algorithm only deals with
 * this for the events involved.
 */
static void group_events(SGS_ParseEvData *restrict to) {
	SGS_ParseEvData *e, *e_after = to->next;
	size_t i;
	uint32_t wait = 0, waitcount = 0;
	for (e = to->groupfrom; e != e_after; ) {
		SGS_ParseOpData **ops;
		ops = (SGS_ParseOpData**) SGS_PtrList_ITEMS(&e->operators);
		for (i = 0; i < e->operators.count; ++i) {
			SGS_ParseOpData *op = ops[i];
			if (e->next == e_after &&
i == (e->operators.count - 1) && (op->op_flags & SGS_SDOP_TIME_DEFAULT) != 0) {
				/* default for last node in group */
				op->op_flags &= ~SGS_SDOP_TIME_DEFAULT;
			}
			if (wait < op->time_ms)
				wait = op->time_ms;
		}
		e = e->next;
		if (e != NULL) {
			/*wait -= e->wait_ms;*/
			waitcount += e->wait_ms;
		}
	}
	for (e = to->groupfrom; e != e_after; ) {
		SGS_ParseOpData **ops;
		ops = (SGS_ParseOpData**) SGS_PtrList_ITEMS(&e->operators);
		for (i = 0; i < e->operators.count; ++i) {
			SGS_ParseOpData *op = ops[i];
			if ((op->op_flags & SGS_SDOP_TIME_DEFAULT) != 0) {
				/* fill in sensible default time */
				op->op_flags &= ~SGS_SDOP_TIME_DEFAULT;
				op->time_ms = wait + waitcount;
			}
		}
		e = e->next;
		if (e != NULL) {
			waitcount -= e->wait_ms;
		}
	}
	to->groupfrom = NULL;
	if (e_after != NULL)
		e_after->wait_ms += wait;
}

static inline void time_ramp(SGS_Ramp *restrict ramp,
		uint32_t default_time_ms) {
	if (!(ramp->flags & SGS_RAMP_TIME_SET))
		ramp->time_ms = default_time_ms;
}

static void time_operator(SGS_ParseOpData *restrict op) {
	SGS_ParseEvData *e = op->event;
	if ((op->op_flags & (SGS_SDOP_TIME_DEFAULT | SGS_SDOP_NESTED)) ==
			(SGS_SDOP_TIME_DEFAULT | SGS_SDOP_NESTED)) {
		op->op_flags &= ~SGS_SDOP_TIME_DEFAULT;
		if (!(op->op_flags & SGS_SDOP_HAS_COMPOSITE))
			op->time_ms = SGS_TIME_INF;
	}
	if (op->time_ms != SGS_TIME_INF) {
		time_ramp(&op->freq, op->time_ms);
		time_ramp(&op->freq2, op->time_ms);
		time_ramp(&op->amp, op->time_ms);
		time_ramp(&op->amp2, op->time_ms);
		if (!(op->op_flags & SGS_SDOP_SILENCE_ADDED)) {
			op->time_ms += op->silence_ms;
			op->op_flags |= SGS_SDOP_SILENCE_ADDED;
		}
	}
	if ((e->ev_flags & SGS_SDEV_ADD_WAIT_DURATION) != 0) {
		if (e->next != NULL) e->next->wait_ms += op->time_ms;
		e->ev_flags &= ~SGS_SDEV_ADD_WAIT_DURATION;
	}
	size_t i;
	SGS_ParseOpData **ops;
	ops = (SGS_ParseOpData**) SGS_PtrList_ITEMS(&op->fmods);
	for (i = op->fmods.old_count; i < op->fmods.count; ++i) {
		time_operator(ops[i]);
	}
	ops = (SGS_ParseOpData**) SGS_PtrList_ITEMS(&op->pmods);
	for (i = op->pmods.old_count; i < op->pmods.count; ++i) {
		time_operator(ops[i]);
	}
	ops = (SGS_ParseOpData**) SGS_PtrList_ITEMS(&op->amods);
	for (i = op->amods.old_count; i < op->amods.count; ++i) {
		time_operator(ops[i]);
	}
}

static void time_event(SGS_ParseEvData *restrict e) {
	/*
	 * Adjust default ramp durations, handle silence as well as the case of
	 * adding present event duration to wait time of next event.
	 */
	// e->pan.flags |= SGS_RAMP_TIME_SET; // TODO: revisit semantics
	size_t i;
	SGS_ParseOpData **ops;
	ops = (SGS_ParseOpData**) SGS_PtrList_ITEMS(&e->operators);
	for (i = e->operators.old_count; i < e->operators.count; ++i) {
		time_operator(ops[i]);
	}
	/*
	 * Timing for composites - done before event list flattened.
	 */
	if (e->composite != NULL) {
		SGS_ParseEvData *ce = e->composite;
		SGS_ParseOpData *ce_op, *ce_op_prev, *e_op;
		ce_op = (SGS_ParseOpData*) SGS_PtrList_GET(&ce->operators, 0);
		ce_op_prev = ce_op->op_prev;
		e_op = ce_op_prev;
		if ((e_op->op_flags & SGS_SDOP_TIME_DEFAULT) != 0)
			e_op->op_flags &= ~SGS_SDOP_TIME_DEFAULT;
		for (;;) {
			ce->wait_ms += ce_op_prev->time_ms;
			if ((ce_op->op_flags & SGS_SDOP_TIME_DEFAULT) != 0) {
				ce_op->op_flags &= ~SGS_SDOP_TIME_DEFAULT;
				ce_op->time_ms = ((ce_op->op_flags &
(SGS_SDOP_NESTED | SGS_SDOP_HAS_COMPOSITE)) == SGS_SDOP_NESTED) ?
					SGS_TIME_INF :
					(ce_op_prev->time_ms - ce_op_prev->silence_ms);
			}
			time_event(ce);
			if (ce_op->time_ms == SGS_TIME_INF)
				e_op->time_ms = SGS_TIME_INF;
			else if (e_op->time_ms != SGS_TIME_INF)
				e_op->time_ms += ce_op->time_ms +
					(ce->wait_ms - ce_op_prev->time_ms);
			ce_op->op_params &= ~SGS_POPP_TIME;
			ce_op_prev = ce_op;
			ce = ce->next;
			if (!ce) break;
			ce_op = (SGS_ParseOpData*)
				SGS_PtrList_GET(&ce->operators, 0);
		}
	}
}

/*
 * Deals with events that are "composite" (attached to a main event as
 * successive "sub-events" rather than part of the big, linear event sequence).
 *
 * Such events, if attached to the passed event, will be given their place in
 * the ordinary event list.
 */
static void flatten_events(SGS_ParseEvData *restrict e) {
	SGS_ParseEvData *ce = e->composite;
	SGS_ParseEvData *se = e->next, *se_prev = e;
	uint32_t wait_ms = 0;
	uint32_t added_wait_ms = 0;
	while (ce != NULL) {
		if (!se) {
			/*
			 * No more events in the ordinary sequence,
			 * so append all composites.
			 */
			se_prev->next = ce;
			break;
		}
		/*
		 * If several events should pass in the ordinary sequence
		 * before the next composite is inserted, skip ahead.
		 */
		wait_ms += se->wait_ms;
		if (se->next != NULL &&
(wait_ms + se->next->wait_ms) <= (ce->wait_ms + added_wait_ms)) {
			se_prev = se;
			se = se->next;
			continue;
		}
		/*
		 * Insert next composite before or after the next event
		 * of the ordinary sequence.
		 */
		if (se->wait_ms >= (ce->wait_ms + added_wait_ms)) {
			SGS_ParseEvData *ce_next = ce->next;
			se->wait_ms -= ce->wait_ms + added_wait_ms;
			added_wait_ms = 0;
			wait_ms = 0;
			se_prev->next = ce;
			se_prev = ce;
			se_prev->next = se;
			ce = ce_next;
		} else {
			SGS_ParseEvData *se_next, *ce_next;
			se_next = se->next;
			ce_next = ce->next;
			ce->wait_ms -= wait_ms;
			added_wait_ms += ce->wait_ms;
			wait_ms = 0;
			se->next = ce;
			ce->next = se_next;
			se_prev = ce;
			se = se_next;
			ce = ce_next;
		}
	}
	e->composite = NULL;
}

typedef struct ParseConv {
	SGS_ScriptEvData *ev, *first_ev;
} ParseConv;

/*
 * Convert data for an operator node to script operator data,
 * adding it to the list to be used for the current script event.
 */
static bool ParseConv_add_opdata(ParseConv *restrict o,
		SGS_ParseOpData *restrict pod) {
	SGS_ScriptOpData *od = calloc(1, sizeof(SGS_ScriptOpData));
	if (!od)
		return false;
	pod->op_conv = od;
	od->event = o->ev;
	/* next_bound */
	/* label */
	od->op_flags = pod->op_flags;
	od->op_params = pod->op_params;
	od->time_ms = pod->time_ms;
	od->silence_ms = pod->silence_ms;
	od->wave = pod->wave;
	od->freq = pod->freq;
	od->freq2 = pod->freq2;
	od->amp = pod->amp;
	od->amp2 = pod->amp2;
	od->phase = pod->phase;
	if (pod->op_prev != NULL) od->op_prev = pod->op_prev->op_conv;
	/* op_next */
	/* fmods */
	/* pmods */
	/* amods */
	if (!SGS_PtrList_add(&o->ev->op_all, od)) goto ERROR;
	return true;

ERROR:
	free(od);
	return false;
}

/*
 * Recursively create needed operator data nodes,
 * visiting new operator nodes as they branch out.
 */
static bool ParseConv_add_ops(ParseConv *restrict o,
		const SGS_PtrList *restrict pod_list) {
	SGS_ParseOpData **pods;
	pods = (SGS_ParseOpData**) SGS_PtrList_ITEMS(pod_list);
	for (size_t i = pod_list->old_count; i < pod_list->count; ++i) {
		SGS_ParseOpData *pod = pods[i];
		// TODO: handle multiple operator nodes
		//if (pod->op_flags & SGS_SDOP_MULTIPLE) continue;
		if (!ParseConv_add_opdata(o, pod)) goto ERROR;
		if (!ParseConv_add_ops(o, &pod->fmods)) goto ERROR;
		if (!ParseConv_add_ops(o, &pod->pmods)) goto ERROR;
		if (!ParseConv_add_ops(o, &pod->amods)) goto ERROR;
	}
	return true;

ERROR:
	return false;
}

/*
 * Recursively fill in lists for operator node graph,
 * visiting all operator nodes as they branch out.
 */
static bool ParseConv_link_ops(ParseConv *restrict o,
		SGS_PtrList *restrict od_list,
		const SGS_PtrList *restrict pod_list) {
	SGS_ParseOpData **pods;
	pods = (SGS_ParseOpData**) SGS_PtrList_ITEMS(pod_list);
	for (size_t i = 0; i < pod_list->count; ++i) {
		SGS_ParseOpData *pod = pods[i];
		// TODO: handle multiple operator nodes
		//if (pod->op_flags & SGS_SDOP_MULTIPLE) continue;
		SGS_ScriptOpData *od = pod->op_conv;
		if (!od) goto ERROR;
		SGS_ScriptEvData *e = od->event;
		if (e->ev_flags & SGS_SDEV_NEW_OPGRAPH) {
			// Handle linking for carriers separately
			if (od->op_flags & SGS_SDOP_NEW_CARRIER)
				SGS_PtrList_add(&e->op_graph, od);
		}
		if (od_list != NULL) SGS_PtrList_add(od_list, od);
		if (od->op_params & SGS_POPP_ADJCS) {
			if (!ParseConv_link_ops(o,
					&od->fmods, &pod->fmods)) goto ERROR;
			if (!ParseConv_link_ops(o,
					&od->pmods, &pod->pmods)) goto ERROR;
			if (!ParseConv_link_ops(o,
					&od->amods, &pod->amods)) goto ERROR;
		}
	}
	return true;

ERROR:
	SGS_error("parseconv", "converted node missing at some level");
	return false;
}

/*
 * Convert the given event data node and all associated operator data nodes.
 */
static bool ParseConv_add_event(ParseConv *restrict o,
		SGS_ParseEvData *restrict pe) {
	SGS_ScriptEvData *e = calloc(1, sizeof(SGS_ScriptEvData));
	if (!e)
		return false;
	pe->ev_conv = e;
	if (o->ev != NULL) o->ev->next = e;
	o->ev = e;
	if (!o->first_ev) o->first_ev = e;
	/* groupfrom */
	/* composite */
	e->wait_ms = pe->wait_ms;
	e->ev_flags = pe->ev_flags;
	e->vo_params = pe->vo_params;
	if (pe->vo_prev != NULL) e->vo_prev = pe->vo_prev->ev_conv;
	e->pan = pe->pan;
	if (!ParseConv_add_ops(o, &pe->operators)) goto ERROR;
	if (!ParseConv_link_ops(o, NULL, &pe->operators)) goto ERROR;
	return true;

ERROR:
	free(e);
	return false;
}

/*
 * Convert parser output to script data, performing
 * post-parsing passes - perform timing adjustments, flatten event list.
 *
 * Ideally, adjustments of parse data would be
 * more cleanly separated into the later stages.
 */
static SGS_Script *ParseConv_convert(ParseConv *restrict o,
		SGS_Parse *restrict p) {
	SGS_ParseEvData *pe;
	for (pe = p->events; pe != NULL; pe = pe->next) {
		time_event(pe);
		if (pe->groupfrom != NULL) group_events(pe);
	}
	/*
	 * Flatten in separate pass following timing adjustments for events;
	 * otherwise, cannot always arrange events in the correct order.
	 */
	for (pe = p->events; pe != NULL; pe = pe->next) {
		if (pe->composite != NULL) flatten_events(pe);
	}
	/*
	 * Convert adjusted parser output to script data.
	 */
	SGS_Script *s = calloc(1, sizeof(SGS_Script));
	if (!s)
		return NULL;
	s->name = p->name;
	s->sopt = p->sopt;
	for (pe = p->events; pe != NULL; pe = pe->next) {
		if (!ParseConv_add_event(o, pe)) goto ERROR;
	}
	s->events = o->first_ev;
	return s;

ERROR:
	SGS_discard_Script(s);
	return NULL;
}

/**
 * Create script data for the given script. Invokes the parser.
 *
 * \return instance or NULL on error
 */
SGS_Script *SGS_load_Script(struct SGS_File *restrict f) {
	ParseConv pc = (ParseConv){0};
	SGS_Parse *p = SGS_create_Parse(f);
	if (!p)
		return NULL;
	SGS_Script *o = ParseConv_convert(&pc, p);
	SGS_destroy_Parse(p);
	return o;
}

/*
 * Destroy the given operator data node.
 */
static void destroy_operator(SGS_ScriptOpData *restrict op) {
	SGS_PtrList_clear(&op->op_next);
	SGS_PtrList_clear(&op->fmods);
	SGS_PtrList_clear(&op->pmods);
	SGS_PtrList_clear(&op->amods);
	free(op);
}

/*
 * Destroy the given event data node and all associated operator data nodes.
 */
static void destroy_event_node(SGS_ScriptEvData *restrict e) {
	size_t i;
	SGS_ScriptOpData **ops;
	ops = (SGS_ScriptOpData**) SGS_PtrList_ITEMS(&e->op_all);
	for (i = e->op_all.old_count; i < e->op_all.count; ++i) {
		destroy_operator(ops[i]);
	}
	SGS_PtrList_clear(&e->op_all);
	SGS_PtrList_clear(&e->op_graph);
	free(e);
}

/**
 * Destroy script data.
 */
void SGS_discard_Script(SGS_Script *restrict o) {
	if (!o)
		return;
	SGS_ScriptEvData *e;
	for (e = o->events; e != NULL; ) {
		SGS_ScriptEvData *e_next = e->next;
		destroy_event_node(e);
		e = e_next;
	}
	free(o);
}
