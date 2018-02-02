#include "sgensys.h"
#include "program.h"
#include "symtab.h"
#include "math.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * General-purpose functions
 */

static void *memdup(const void *src, size_t size) {
  void *ret = malloc(size);
  if (!ret) return 0;
  memcpy(ret, src, size);
  return ret;
}

#ifdef strdup /* deal with libc issues when already provided */
# undef strdup
# define strdup _strdup
#endif
static char *_strdup(const char *src) {
  size_t len = strlen(src);
  char *ret;
  if (!len) return 0;
  ret = memdup(src, len + 1);
  if (!ret) return 0;
  ret[len] = '\0';
  return ret;
}

#define IS_WHITESPACE(c) \
  ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r')

static uchar testc(char c, FILE *f) {
  char gc = getc(f);
  ungetc(gc, f);
  return (gc == c);
}

static uchar testgetc(char c, FILE *f) {
  char gc;
  if ((gc = getc(f)) == c) return 1;
  ungetc(gc, f);
  return 0;
}

static int getinum(FILE *f) {
  char c;
  int num = -1;
  c = getc(f);
  if (c >= '0' && c <= '9') {
    num = c - '0';
    for (;;) {
      c = getc(f);
      if (c >= '0' && c <= '9')
        num = num * 10 + (c - '0');
      else
        break;
    }
  }
  ungetc(c, f);
  return num;
}

static int strfind(FILE *f, const char *const*str) {
  int search, ret;
  uint i, len, pos, matchpos;
  char c, undo[256];
  uint strc;
  const char **s;

  for (len = 0, strc = 0; str[strc]; ++strc)
    if ((i = strlen(str[strc])) > len) len = i;
  s = malloc(sizeof(const char*) * strc);
  for (i = 0; i < strc; ++i)
    s[i] = str[i];
  search = ret = -1;
  pos = matchpos = 0;
  while ((c = getc(f)) != EOF) {
    undo[pos] = c;
    for (i = 0; i < strc; ++i) {
      if (!s[i]) continue;
      else if (!s[i][pos]) {
        s[i] = 0;
        if (search == (int)i) {
          ret = i;
          matchpos = pos-1;
        }
      } else if (c != s[i][pos]) {
        s[i] = 0;
        search = -1;
      } else
        search = i;
    }
    if (pos == len) break;
    ++pos;
  }
  free(s);
  for (i = pos; i > matchpos; --i) ungetc(undo[i], f);
  return ret;
}

static void eatws(FILE *f) {
  char c;
  while ((c = getc(f)) == ' ' || c == '\t') ;
  ungetc(c, f);
}

/*
 * Parsing code
 */

typedef struct NodeList {
  struct EventData **na;
  uint count;
} NodeList;

static void node_list_add(NodeList *nl, struct EventData *n) {
  ++nl->count;
  nl->na = realloc(nl->na, sizeof(struct EventData*) * nl->count);
  nl->na[nl->count - 1] = n;
}

static void node_list_clear(NodeList *nl) {
  free(nl->na);
  nl->na = 0;
  nl->count = 0;
}

enum {
  /* parsing scopes */
  SCOPE_SAME = 0,
  SCOPE_TOP = 1,
  SCOPE_BIND = '{',
  SCOPE_NEST = '<'
};

enum {
  /* link types */
  GRAPH = 1<<0,
  PMODS = 1<<1,
  FMODS = 1<<2,
  AMODS = 1<<3,
  /* other flags */
  MAKE_EVENT = 1<<4,
  EVENT_COMPOSITE = 1<<5,
  EVENT_LABELED = 1<<6,
  EVENT_LINKED = 1<<7,
  PARENT_OLD = 1<<8,
  ADD_WAIT_DURATION = 1<<9,
  SILENCE_ADDED = 1<<10
};

#define DEFAULT_TIME (-1)

typedef struct VoiceData {
  struct EventData *voice_prev; /* preceding event for same voice */
  uint voiceid;
  /* parameters */
  uchar attr;
  float panning;
  SGSProgramValit valitpanning;
  /* operator linkage graph */
  NodeList operators;
} VoiceData;

typedef struct OperatorData {
  struct EventData *operator_prev; /* preceding event for same operator */
  uint operatorid;
  uint voiceid;
  /* parameters */
  uchar attr;
  uchar wave;
  int time_ms, silence_ms;
  float freq, dynfreq, phase, amp, dynamp;
  SGSProgramValit valitfreq, valitamp;
  /* node adjacents in operator linkage graph */
  NodeList pmods, fmods, amods;
} OperatorData;

typedef struct EventData {
  struct EventData *next, *scope_next;
  struct EventData *group_from;
  struct EventData *sub_composite;
  int wait_ms;
  uint id;
  const char *sym;
  uint params;
  uint list_id; /* for NodeData operators list */
  uint nest_level;
  uint scope_id;
  uint parse_flags;
  VoiceData *voice;
  OperatorData *operator;
} EventData;

typedef struct SGSParser {
  FILE *f;
  const char *fn;
  SGSProgram *prg;
  SGSSymtab *st;
  uint line;
  uint calllevel;
  uint nest_level;
  uint scope_id;
  char nextc;
  /* node state */
  EventData *events;
  EventData *last_event;
  uint eventc;
  uint operatorc;
  uint voicec;
  /* settings/ops */
  float ampmult;
  int def_time_ms;
  float def_freq, def_A4tuning, def_ratio;
} SGSParser;

/* things that need to be separate for each nested parse_level() go here */
typedef struct NodeData {
  SGSParser *o;
  struct NodeData *parent;
  uchar set_settings, /* adjusting default values */
        set_step;     /* adjusting operator and/or voice */
  char scope;
  uint scope_id;
  /* data for next event */
  EventData event;
  VoiceData voice;
  OperatorData operator;
  uint add_wait_ms; /* added for event after next */
  uchar linktype;
  /* event tracking for current scope */
  EventData *bind_from, *preceding;
  EventData *voice_event;
  EventData *first, *last, *last_main;
  NodeList operators;
  uint parse_flags;
  /* timing/delay */
  EventData *group;
  EventData *composite; /* grouping of events for a voice and/or operator */
} NodeData;

static void add_adjc(EventData *e, EventData *adjc, uchar type) {
  NodeList *nl = 0;
  switch (type) {
  case GRAPH:
    nl = &e->voice->operators;
    break;
  case PMODS:
    nl = &e->operator->pmods;
    break;
  case FMODS:
    nl = &e->operator->fmods;
    break;
  case AMODS:
    nl = &e->operator->amods;
    break;
  }
  if (nl->count && !(e->parse_flags & type)) { /* adjacents were inherited */
    nl->na = 0;
    nl->count = 0;
  }
  node_list_add(nl, adjc);
  e->parse_flags |= type;
  adjc->parse_flags |= EVENT_LINKED;
}

#define nd_set_linktype(nd, type) ((void)(\
  (nd)->linktype = (type) \
))

#define ND_LINK_FOR(nd) ((uint)(\
  ((nd)->parent) ? (nd)->parent->linktype : GRAPH \
))

/* The event, at any rate, starts out a standard one */
static void nd_standard_event(NodeData *nd) {
  nd->parse_flags &= ~EVENT_COMPOSITE;
}

/* Make the event a composite event when done */
static void nd_composite_event(NodeData *nd) {
  nd->parse_flags |= EVENT_COMPOSITE;
}

/* This can be done before nd_begin_event() for the event later begun */
static void nd_label_event(NodeData *nd, const char *label) {
  EventData *e = &nd->event;
  if (e->sym || !label) {
    free((char*)e->sym);
    e->sym = 0;
    return;
  }
  e->sym = strdup(label);
}

/* Current label for event or event to be made */
#define nd_current_label(nd) ((const char*)(nd)->event.sym)

/* Next operator defined will not belong to present voice. */
static void nd_new_voice(NodeData *nd) {
  nd->voice_event = 0;
}

static void nd_end_event(NodeData *nd) {
  SGSParser *o = nd->o;
  EventData *e = &nd->event;
  VoiceData *vd = &nd->voice;
  OperatorData *od = &nd->operator;
  if (!(nd->parse_flags & MAKE_EVENT)) return;
  /*
   * Flush voice sub-event
   */
  if (!vd->voice_prev) { /* initial event - all parameters initialized */
    e->params |= SGS_VOATTR |
                 SGS_GRAPH |
                 SGS_PANNING;
  }
  if (vd->valitpanning.type)
    e->params |= SGS_VOATTR |
                 SGS_VALITPANNING;
  if (SGS_VOICE_PARAMS(e->params)) {
    if (!vd->voice_prev) {
      vd->voiceid = o->voicec++;
      od->voiceid = vd->voiceid;
    }
    e->voice = memdup(vd, sizeof(VoiceData));
  }
  memset(&nd->voice, 0, sizeof(VoiceData));
  /*
   * Flush operator sub-event
   */
  if (!od->operator_prev) { /* initial event should reset its parameters */
    e->params |= SGS_ADJCS |
                 SGS_WAVE |
                 SGS_TIME |
                 SGS_SILENCE |
                 SGS_FREQ |
                 SGS_DYNFREQ |
                 SGS_PHASE |
                 SGS_AMP |
                 SGS_DYNAMP |
                 SGS_OPATTR;
  } else {
    OperatorData *pod = od->operator_prev->operator;
    if (od->attr != pod->attr)
      e->params |= SGS_OPATTR;
    if (od->wave != pod->wave)
      e->params |= SGS_WAVE;
    /* SGS_TIME set when time set */
    if (od->silence_ms)
      e->params |= SGS_SILENCE;
    /* SGS_FREQ set when freq set */
    if (od->dynfreq != pod->dynfreq)
      e->params |= SGS_DYNFREQ;
    /* SGS_PHASE set when phase set */
    /* SGS_AMP set when amp set */
    if (od->dynamp != pod->dynamp)
      e->params |= SGS_DYNAMP;
  }
  if (od->valitfreq.type)
    e->params |= SGS_OPATTR |
                 SGS_VALITFREQ;
  if (od->valitamp.type)
    e->params |= SGS_OPATTR |
                 SGS_VALITAMP;
  if (SGS_OPERATOR_PARAMS(e->params)) {
    if (e->nest_level == 0)
      od->amp *= o->ampmult;
    if (!od->operator_prev)
      od->operatorid = o->operatorc++;
    e->operator = memdup(od, sizeof(OperatorData));
  }
  memset(&nd->operator, 0, sizeof(OperatorData));
  /*
   * Flush event as whole
   */
  if (e->voice || e->operator) {
    uint link_for = ND_LINK_FOR(nd);
    e->id = o->eventc++;
    e = memdup(e, sizeof(EventData));
    memset(&nd->event, 0, sizeof(EventData));
    /*
     * Event linking
     */
    if (!o->events) o->events = e;
    if (!nd->first) nd->first = e;
    if (!nd->group) nd->group = e;
    if (e->voice) nd->voice_event = e;
    if (nd->parse_flags & EVENT_COMPOSITE) {
      if (!nd->composite) nd->composite = e;
      else {
        if (!nd->composite->sub_composite) nd->composite->sub_composite = e;
        else nd->last->next = e;
        link_for = 0; /* already linked first event */
      }
    } else {
      /* link all events that are not composite sub-parts */
      if (o->last_event) o->last_event->next = e;
      o->last_event = e;
      nd->last_main = e; /* then remains the same during composite events */
      nd->composite = 0;
    }
    nd->last = e;
    if (nd->preceding && (nd->preceding->parse_flags & EVENT_LINKED)) {
      /* FIXME:sync function? */
      uint p_voiceid = (nd->preceding->voice) ?
                       nd->preceding->voice->voiceid :
                       nd->preceding->operator->voiceid;
      uint e_voiceid = (e->voice) ?
                       e->voice->voiceid :
                       e->operator->voiceid;
      if (e_voiceid == p_voiceid) link_for = 0; /* already linked */
    }
    if (link_for) {
      uint i;
      NodeList *parents = (nd->parent) ? &nd->parent->operators : 0;
      if (parents) {
        for (i = 0; i < parents->count; ++i) {
          parents->na[i]->params |= (link_for == GRAPH) ? SGS_GRAPH :
                                                          SGS_ADJCS;
          add_adjc(parents->na[i], e, link_for);
        }
      } else {
        e->params |= SGS_GRAPH; /* operator has new voice for parent */
        add_adjc(nd->voice_event, e, GRAPH);
      }
      /* FIXME:sync function?
       *else if ((nd->parent->parse_flags & PARENT_OLD) ||
                 (nd->parent->scope_id != nd->scope_id)) {
        if (link_for == GRAPH)
          nd->parent = e;
      }*/
    }
    /*FIXME*/if (e->operator) node_list_add(&nd->operators, e);
    /* Assign label */
    if (e->sym && !(e->parse_flags & EVENT_LABELED)) {
      SGSSymtab_set(o->st, e->sym, e);
      e->parse_flags |= EVENT_LABELED;
    }
  }
  nd->parse_flags &= ~MAKE_EVENT;
  nd->preceding = 0;
}

static void nd_begin_event(NodeData *nd, EventData *preceding) {
  SGSParser *o = nd->o;
  EventData *e = &nd->event, *pve, *poe;
  VoiceData *vd = &nd->voice;
  OperatorData *od = &nd->operator;
  uint link_for = ND_LINK_FOR(nd);
  uint i;
  nd_end_event(nd);
  /*
   * Prepare next event
   */
  if (preceding == &nd->event) preceding = nd->last;
  nd->preceding = preceding;
  if (!nd->preceding && !nd->parent) nd_new_voice(nd);
  e->wait_ms += nd->add_wait_ms;
  nd->add_wait_ms = 0;
  e->nest_level = o->nest_level;
  e->scope_id = nd->scope_id;
  od->time_ms = DEFAULT_TIME; /* context-specific default timing */
  pve = (nd->voice_event) ? nd->voice_event : nd->preceding;
  poe = nd->preceding;
  if (pve && pve->voice) {
    *vd = *pve->voice;
    vd->voice_prev = pve;
  }
  if (poe && poe->operator) {
    *od = *poe->operator;
    od->silence_ms = 0;
    od->operator_prev = poe;
  }
  if (!vd->voice_prev) { /* set defaults */
    vd->panning = 0.5f; /* center */
  }
  if (!od->operator_prev) { /* set defaults */
    od->amp = 1.0f;
    if (e->nest_level == 0) {
      od->freq = o->def_freq;
    } else {
      od->time_ms = o->def_time_ms;
      od->freq = o->def_ratio;
      od->attr |= SGS_ATTR_FREQRATIO;
    }
  }
  nd->parse_flags |= MAKE_EVENT;
}

static void nd_add_waittime(NodeData *nd, float wait) {
  uint wait_ms;
  SET_I2F(wait_ms, wait*1000.f);
  nd->add_wait_ms += wait_ms;
  if (nd->parse_flags & MAKE_EVENT) nd_begin_event(nd, &nd->event);
}

static void nd_set_silence(NodeData *nd, float f) {
  OperatorData *od = &nd->operator;
  SET_I2F(od->silence_ms, f * 1000.f);
}

static void nd_set_time(NodeData *nd, float *f) {
  EventData *e = &nd->event;
  OperatorData *od = &nd->operator;
  if (f) SET_I2F(od->time_ms, (*f) * 1000.f);
  else od->time_ms = DEFAULT_TIME;
  e->params |= SGS_TIME;
}

static void nd_set_amp(NodeData *nd, float *f, SGSProgramValit *vi) {
  EventData *e = &nd->event;
  OperatorData *od = &nd->operator;
  if (f) {
    if (!od->valitamp.type)
      od->attr &= ~SGS_ATTR_VALITAMP;
    od->amp = *f;
    e->params |= SGS_AMP;
  }
  if (vi) {
    od->attr |= SGS_ATTR_VALITAMP;
    od->valitamp = *vi;
  }
}

static void nd_set_panning(NodeData *nd, float *f, SGSProgramValit *vi) {
  EventData *e = &nd->event;
  VoiceData *vd = &nd->voice;
  if (f) {
    if (!vd->valitpanning.type)
      vd->attr &= ~SGS_ATTR_VALITPANNING;
    vd->panning = *f;
    e->params |= SGS_PANNING;
  }
  if (vi) {
    vd->attr |= SGS_ATTR_VALITPANNING;
    vd->valitpanning = *vi;
  }
  /* compare to preceding voice event if any */
  if (vd->voice_prev) {
    VoiceData *pvd = vd->voice_prev->voice;
    if (vd->panning == pvd->panning)
      e->params &= ~SGS_PANNING;
  }
}

static void nd_set_phase(NodeData *nd, float f) {
  EventData *e = &nd->event;
  OperatorData *od = &nd->operator;
  od->phase = fmod(f, 1.f);
  if (od->phase < 0.f)
    od->phase += 1.f;
  e->params |= SGS_PHASE;
}

static void nd_set_freq(NodeData *nd, float *f, SGSProgramValit *vi,
                        uchar ratio) {
  EventData *e = &nd->event;
  OperatorData *od = &nd->operator;
  if (f) {
    if (!od->valitamp.type)
      od->attr &= ~(SGS_ATTR_VALITFREQ |
                    SGS_ATTR_VALITFREQRATIO);
    if (ratio) {
      od->freq = 1.f / *f;
      od->attr |= SGS_ATTR_FREQRATIO;
    } else {
      od->freq = *f;
      od->attr &= ~SGS_ATTR_FREQRATIO;
    }
    e->params |= SGS_FREQ;
  }
  if (vi) {
    od->attr |= SGS_ATTR_VALITFREQ;
    od->valitamp = *vi;
    if (ratio) {
      od->valitamp.goal = 1.f / od->valitamp.goal;
      od->attr |= SGS_ATTR_VALITFREQRATIO;
    } else
      od->attr &= ~SGS_ATTR_VALITFREQRATIO;
  }
}

static void nd_init(NodeData *nd, SGSParser *o, NodeData *parentnd,
                    char scope) {
  memset(nd, 0, sizeof(NodeData));
  nd->o = o;
  nd->scope = scope;
  if (parentnd) {
    nd->parent = parentnd;
    nd->set_settings = parentnd->set_settings;
    nd->set_step = parentnd->set_step;
    if (scope == SCOPE_SAME)
      nd->scope = parentnd->scope;
    nd->scope_id = parentnd->scope_id;
    nd->voice_event = parentnd->voice_event;
  }
  nd_set_linktype(nd, GRAPH);
}

static void nd_fini(NodeData *nd) {
  nd_end_event(nd);
  if (nd->last && nd->last->operator) {
    if (nd->last->operator->time_ms < 0)
      nd->last->operator->time_ms = nd->o->def_time_ms; /* use default */
  }
  if (nd->last_main)
    nd->last_main->group_from = nd->group;
  /* Event linkage across scopes */
  if (nd->scope == SCOPE_BIND) {
    nd->parent->bind_from = nd->first;
    if (nd->parent->last) nd->parent->last->scope_next = nd->first;
    nd->parent->last = nd->last;
  }
}

/*
 * Parsing routines
 */

#define NEWLINE '\n'
static char read_char(SGSParser *o) {
  char c;
  eatws(o->f);
  if (o->nextc) {
    c = o->nextc;
    o->nextc = 0;
  } else {
    c = getc(o->f);
  }
  if (c == '#')
    while ((c = getc(o->f)) != '\n' && c != '\r' && c != EOF) ;
  if (c == '\n') {
    testgetc('\r', o->f);
    c = NEWLINE;
  } else if (c == '\r') {
    testgetc('\n', o->f);
    c = NEWLINE;
  } else {
    eatws(o->f);
  }
  return c;
}

static void read_ws(SGSParser *o) {
  char c;
  do {
    c = getc(o->f);
    if (c == ' ' || c == '\t')
      continue;
    if (c == '\n') {
      ++o->line;
      testgetc('\r', o->f);
    } else if (c == '\r') {
      ++o->line;
      testgetc('\n', o->f);
    } else if (c == '#') {
      while ((c = getc(o->f)) != '\n' && c != '\r' && c != EOF) ;
    } else {
      ungetc(c, o->f);
      break;
    }
  } while (c != EOF);
}

static float read_num_r(SGSParser *o, float (*read_symbol)(SGSParser *o),
                        char *buf, uint len, uchar pri, uint level) {
  char *p = buf;
  uchar dot = 0;
  float num;
  char c;
  c = getc(o->f);
  if (level) read_ws(o);
  if (c == '(') {
    return read_num_r(o, read_symbol, buf, len, 255, level+1);
  }
  if (read_symbol &&
      ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
    ungetc(c, o->f);
    num = read_symbol(o);
    if (num == num) /* not NAN; was recognized */
      goto LOOP;
  }
  if (c == '-') {
    *p++ = c;
    c = getc(o->f);
    if (level) read_ws(o);
  }
  while ((c >= '0' && c <= '9') || (!dot && (dot = (c == '.')))) {
    if ((p+1) == (buf+len)) {
      break;
    }
    *p++ = c;
    c = getc(o->f);
  }
  ungetc(c, o->f);
  if (p == buf) return NAN;
  *p = '\0';
  num = strtod(buf, 0);
LOOP:
  if (level) read_ws(o);
  for (;;) {
    c = getc(o->f);
    if (level) read_ws(o);
    switch (c) {
    case '(':
      num *= read_num_r(o, read_symbol, buf, len, 255, level+1);
      break;
    case ')':
      if (pri < 255)
        ungetc(c, o->f);
      return num;
      break;
    case '^':
      num = exp(log(num) * read_num_r(o, read_symbol, buf, len, 0, level));
      break;
    case '*':
      num *= read_num_r(o, read_symbol, buf, len, 1, level);
      break;
    case '/':
      num /= read_num_r(o, read_symbol, buf, len, 1, level);
      break;
    case '+':
      if (pri < 2)
        return num;
      num += read_num_r(o, read_symbol, buf, len, 2, level);
      break;
    case '-':
      if (pri < 2)
        return num;
      num -= read_num_r(o, read_symbol, buf, len, 2, level);
      break;
    default:
      ungetc(c, o->f);
      return num;
    }
    if (num != num) {
      ungetc(c, o->f);
      return num;
    }
  }
}
static uchar read_num(SGSParser *o, float (*read_symbol)(SGSParser *o),
                      float *var) {
  char buf[64];
  float num = read_num_r(o, read_symbol, buf, 64, 254, 0);
  if (num != num)
    return 0;
  *var = num;
  return 1;
}

static void warning(SGSParser *o, const char *s, char c) {
  char buf[4] = {'\'', c, '\'', 0};
  printf("warning: %s [line %d, at %s] - %s\n", o->fn, o->line,
         (c == EOF ? "EOF" : buf), s);
}
#define WARN_INVALID "invalid character"

#define OCTAVES 11
static float read_note(SGSParser *o) {
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
  float freq;
  char c = getc(o->f);
  int octave;
  int semitone = 1, note;
  int subnote = -1;
  if (c >= 'a' && c <= 'g') {
    subnote = c - 'c';
    if (subnote < 0) /* a, b */
      subnote += 7;
    c = getc(o->f);
  }
  if (c < 'A' || c > 'G') {
    warning(o, "invalid note specified - should be C, D, E, F, G, A or B", c);
    return NAN;
  }
  note = c - 'C';
  if (note < 0) /* A, B */
    note += 7;
  c = getc(o->f);
  if (c == 's')
    semitone = 2;
  else if (c == 'f')
    semitone = 0;
  else
    ungetc(c, o->f);
  octave = getinum(o->f);
  if (octave < 0) /* none given, default to 4 */
    octave = 4;
  else if (octave >= OCTAVES) {
    warning(o, "invalid octave specified for note - valid range 0-10", c);
    octave = 4;
  }
  freq = o->def_A4tuning * (3.f/5.f); /* get C4 */
  freq *= octaves[octave] * notes[semitone][note];
  if (subnote >= 0)
    freq *= 1.f + (notes[semitone][note+1] / notes[semitone][note] - 1.f) *
                  (notes[1][subnote] - 1.f);
  return freq;
}

#define SYMKEY_LEN 80
#define SYMKEY_LEN_A "80"
typedef char SymBuf[SYMKEY_LEN];
static uchar read_sym(SGSParser *o, SymBuf sym, char op) {
  uint i = 0;
  char nosym_msg[] = "ignoring ? without symbol name";
  nosym_msg[9] = op; /* replace ? */
  for (;;) {
    char c = getc(o->f);
    if (IS_WHITESPACE(c) || c == EOF) {
      ungetc(c, o->f);
      if (i == 0)
        warning(o, nosym_msg, c);
      else END_OF_SYM: {
        sym[i] = '\0';
        return 1;
      }
      break;
    } else if (i == SYMKEY_LEN) {
      warning(o, "ignoring symbol name from "SYMKEY_LEN_A"th digit", c);
      goto END_OF_SYM;
    }
    sym[i++] = c;
  }
  return 0;
}

static int read_wavetype(SGSParser *o, char lastc) {
  static const char *const wavetypes[] = {
    "sin",
    "srs",
    "tri",
    "sqr",
    "saw",
    0
  };
  int wave = strfind(o->f, wavetypes);
  if (wave < 0)
    warning(o, "invalid wave type follows; sin, sqr, tri, saw available", lastc);
  return wave;
}

static uchar read_valit(SGSParser *o, float (*read_symbol)(SGSParser *o),
                        SGSProgramValit *vi) {
  static const char *const valittypes[] = {
    "lin",
    "exp",
    "log",
    0
  };
  char c;
  uchar goal = 0;
  int type;
  vi->time_ms = DEFAULT_TIME;
  vi->type = SGS_VALIT_LIN; /* default */
  while ((c = read_char(o)) != EOF) {
    switch (c) {
    case NEWLINE:
      ++o->line;
      break;
    case 'c':
      type = strfind(o->f, valittypes);
      if (type >= 0) {
        vi->type = type + SGS_VALIT_LIN;
        break;
      }
      goto INVALID;
    case 't': {
      float time;
      if (read_num(o, 0, &time)) {
        if (time < 0.f) {
          warning(o, "ignoring 't' with sub-zero time", c);
          break;
        }
        SET_I2F(vi->time_ms, time*1000.f);
      }
      break; }
    case 'v':
      if (read_num(o, read_symbol, &vi->goal))
        goal = 1;
      break;
    case ']':
      goto RETURN;
    default:
    INVALID:
      warning(o, WARN_INVALID, c);
      break;
    }
  }
  warning(o, "end of file without closing ']'", c);
RETURN:
  if (!goal) {
    warning(o, "ignoring gradual parameter change with no target value", c);
    vi->type = SGS_VALIT_NONE;
    return 0;
  }
  return 1;
}

static uchar read_waittime(SGSParser *o, NodeData *nd, char c) {
  if (testgetc('t', o->f)) {
    if (!nd->last) {
      warning(o, "add wait for last duration before any parts given", c);
      return 0;
    }
    nd->last->parse_flags |= ADD_WAIT_DURATION;
  } else {
    float wait;
    read_num(o, 0, &wait);
    if (wait < 0.f) {
      warning(o, "ignoring '\\' with sub-zero time", c);
      return 0;
    }
    nd_add_waittime(nd, wait);
  }
  return 1;
}

/*
 * Main parser functions
 */

static uchar parse_settings(SGSParser *o, NodeData *nd) {
  char c;
  nd->set_settings = 1;
  nd->set_step = 0;
  while ((c = read_char(o)) != EOF) {
    switch (c) {
    case 'a':
      read_num(o, 0, &o->ampmult);
      break;
    case 'f':
      read_num(o, read_note, &o->def_freq);
      break;
    case 'n': {
      float freq;
      read_num(o, 0, &freq);
      if (freq < 1.f) {
        warning(o, "ignoring tuning frequency smaller than 1.0", c);
        break;
      }
      o->def_A4tuning = freq;
      break; }
    case 'r':
      if (read_num(o, 0, &o->def_ratio))
        o->def_ratio = 1.f / o->def_ratio;
      break;
    case 't': {
      float time;
      read_num(o, 0, &time);
      if (time < 0.f) {
        warning(o, "ignoring 't' with sub-zero time", c);
        break;
      }
      SET_I2F(o->def_time_ms, time*1000.f);
      break; }
    default:
    /*UNKNOWN:*/
      o->nextc = c;
      return 1; /* let parse_level() take care of it */
    }
  }
  return 0;
}

static uchar parse_level(SGSParser *o, NodeData *parentnd, char newscope);

static uchar parse_step(SGSParser *o, NodeData *nd) {
  char c;
  EventData *e = &nd->event;
  OperatorData *od = &nd->operator;
  VoiceData *vd = &nd->voice;
  uint link_for = ND_LINK_FOR(nd);
  SGSProgramValit vi;
  float f;
  nd->set_settings = 0;
  nd->set_step = 1;
  while ((c = read_char(o)) != EOF) {
    switch (c) {
    case '\\':
      read_waittime(o, nd, c);
      break;
    case 'a':
      if (link_for == AMODS ||
          link_for == FMODS)
        goto UNKNOWN;
      if (testgetc('!', o->f)) {
        if (!testc('{', o->f)) {
          read_num(o, 0, &od->dynamp);
        }
        if (testgetc('{', o->f)) {
          if (e->params & SGS_ADJCS)
            node_list_clear(&od->amods);
          ++o->nest_level;
          nd_set_linktype(nd, AMODS);
          parse_level(o, nd, '{');
          nd_set_linktype(nd, GRAPH);
          --o->nest_level;
        }
      } else if (testgetc('[', o->f)) {
        if (read_valit(o, 0, &vi))
          nd_set_amp(nd, 0, &vi);
      } else {
        read_num(o, 0, &f);
        nd_set_amp(nd, &f, 0);
      }
      break;
    case 'b':
      if (e->nest_level)
        goto UNKNOWN;
      if (testgetc('[', o->f)) {
        if (read_valit(o, 0, &vi))
          nd_set_panning(nd, 0, &vi);
      } else if (read_num(o, 0, &f)) {
        nd_set_panning(nd, &f, 0);
      }
      break;
    case 'f':
      if (testgetc('!', o->f)) {
        if (!testc('{', o->f)) {
          if (read_num(o, 0, &od->dynfreq)) {
            od->attr &= ~SGS_ATTR_DYNFREQRATIO;
          }
        }
        if (testgetc('{', o->f)) {
          if (e->params & SGS_ADJCS)
            node_list_clear(&od->fmods);
          ++o->nest_level;
          nd_set_linktype(nd, FMODS);
          parse_level(o, nd, '{');
          nd_set_linktype(nd, GRAPH);
          --o->nest_level;
        }
      } else if (testgetc('[', o->f)) {
        if (read_valit(o, read_note, &vi))
          nd_set_freq(nd, 0, &vi, 0);
      } else if (read_num(o, read_note, &f)) {
        nd_set_freq(nd, &f, 0, 0);
      }
      break;
    case 'p':
      if (read_num(o, 0, &f))
        nd_set_phase(nd, f);
      break;
    case 'r':
      if (e->nest_level == 0)
        goto UNKNOWN;
      if (testgetc('!', o->f)) {
        if (!testc('{', o->f)) {
          if (read_num(o, 0, &od->dynfreq)) {
            od->dynfreq = 1.f / od->dynfreq;
            od->attr |= SGS_ATTR_DYNFREQRATIO;
          }
        }
        if (testgetc('{', o->f)) {
          if (e->params & SGS_ADJCS)
            node_list_clear(&od->fmods);
          ++o->nest_level;
          nd_set_linktype(nd, FMODS);
          parse_level(o, nd, '{');
          nd_set_linktype(nd, GRAPH);
          --o->nest_level;
        }
      } else if (testgetc('[', o->f)) {
        if (read_valit(o, read_note, &vi))
          nd_set_freq(nd, 0, &vi, 1);
      } else if (read_num(o, read_note, &f)) {
        nd_set_freq(nd, &f, 0, 1);
      }
      break;
    case 's':
      read_num(o, 0, &f);
      if (f < 0.f) {
        warning(o, "ignoring 's' with sub-zero time", c);
        break;
      }
      nd_set_silence(nd, f);
      break;
    case 't':
      if (testgetc('*', o->f))
        nd_set_time(nd, 0); /* default time */
      else {
        read_num(o, 0, &f);
        if (f < 0.f) {
          warning(o, "ignoring 't' with sub-zero time", c);
          break;
        }
        nd_set_time(nd, &f);
      }
      break;
    case 'w': {
      int wave = read_wavetype(o, c);
      if (wave < 0)
        break;
      od->wave = wave;
      break; }
    default:
    UNKNOWN:
      o->nextc = c;
      return 1; /* let parse_level() take care of it */
    }
  }
  return 0;
}

enum {
  HANDLE_DEFER = 1<<1,
  DEFERRED_STEP = 1<<2,
  DEFERRED_SETTINGS = 1<<4
};
static uchar parse_level(SGSParser *o, NodeData *parentnd, char newscope) {
  char c;
  uchar endscope = 0;
  uchar flags = 0;
  SymBuf sym;
  NodeData nd;
  nd_init(&nd, o, parentnd, newscope);
  ++o->calllevel;
  while ((c = read_char(o)) != EOF) {
    flags &= ~HANDLE_DEFER;
    switch (c) {
    case NEWLINE:
      ++o->line;
      if (nd.scope == SCOPE_TOP) {
        if (o->calllevel > 1)
          goto RETURN;
        flags = 0;
        nd.first = 0;
        nd.set_settings = 0;
        if (nd.set_step) {
          nd.set_step = 0;
          nd.scope_id = ++o->scope_id;
        }
      }
      break;
    case '-': {
      EventData *first, *last;
      uchar ret;
      first = nd.first;
      last = &nd.event;
      if (!first) {
        if (o->calllevel == 1) NO_CARRIER: {
          warning(o, "no preceding carrier operators", c);
          break;
        }
        first = parentnd->first;
        if (!last)
          goto NO_CARRIER;
      }
      if (first && first != last) {
        warning(o, "multiple carriers not yet supported", c);
        break;
      }
      if (last->params & SGS_ADJCS)
        node_list_clear(&last->operator->pmods);
      ++o->nest_level;
      nd_set_linktype(&nd, PMODS);
      ret = parse_level(o, &nd, SCOPE_SAME);
      nd_set_linktype(&nd, GRAPH);
      --o->nest_level;
      if (ret)
        goto RETURN;
      break; }
    case ':':
      if (nd_current_label(&nd)) {
        warning(o, "ignoring label assignment to label reference", c);
        nd_label_event(&nd, 0);
      }
      nd.set_settings = 0;
      nd.set_step = 0;
      if (read_sym(o, sym, ':')) {
        EventData *ref = SGSSymtab_get(o->st, sym);
        if (!ref)
          warning(o, "ignoring reference to undefined label", c);
        else {
          nd_begin_event(&nd, ref);
          nd_standard_event(&nd);
          nd_label_event(&nd, sym);
          flags = parse_step(o, &nd) ? (HANDLE_DEFER | DEFERRED_STEP) : 0;
        }
      }
      break;
    case ';':
      if (newscope == SCOPE_SAME) {
        o->nextc = c;
        goto RETURN;
      }
      if (nd.set_settings || !(nd.parse_flags & MAKE_EVENT))
        goto INVALID;
      nd_composite_event(&nd);
      nd_begin_event(&nd, &nd.event);
      nd_composite_event(&nd);
      flags = parse_step(o, &nd) ? (HANDLE_DEFER | DEFERRED_STEP) : 0;
      break;
    case '<':
      if (parse_level(o, &nd, SCOPE_NEST))
        goto RETURN;
      break;
    case '>':
      if (nd.scope != SCOPE_NEST) {
        warning(o, "closing '>' without opening '<'", c);
        break;
      }
      endscope = 1;
      goto RETURN;
    case 'O': {
      int wave = read_wavetype(o, c);
      if (wave < 0)
        break;
      nd_begin_event(&nd, 0);
      nd_standard_event(&nd);
      nd.operator.wave = wave;
      flags = parse_step(o, &nd) ? (HANDLE_DEFER | DEFERRED_STEP) : 0;
      break; }
    case 'Q':
      goto FINISH;
    case 'S':
      flags = parse_settings(o, &nd) ? (HANDLE_DEFER | DEFERRED_SETTINGS) : 0;
      break;
    case '\\':
      if (nd.set_settings || nd.event.nest_level != 0) goto INVALID;
      read_waittime(o, &nd, c);
      break;
    case '\'':
      if (nd.parse_flags & MAKE_EVENT) nd_end_event(&nd);
      else if (nd_current_label(&nd)) {
        warning(o, "ignoring label assignment to label assignment", c);
        break;
      }
      read_sym(o, sym, '\'');
      nd_label_event(&nd, sym);
      break;
    case '{':
      nd_end_event(&nd);
      if (parse_level(o, &nd, SCOPE_BIND))
        goto RETURN;
      break;
    case '|':
      if (nd.set_settings || nd.event.nest_level != 0)
        goto INVALID;
      if (newscope == SCOPE_SAME) {
        o->nextc = c;
        goto RETURN;
      }
      nd_end_event(&nd);
      if (!nd.last) {
        warning(o, "end of sequence before any parts given", c);
        break;
      }
      if (nd.group) {
        nd.last_main->group_from = nd.group;
        nd.group = 0;
      }
      nd.set_step = 0;
      break;
    case '}':
      if (nd.scope != SCOPE_BIND) {
        warning(o, "closing '}' without opening '{'", c);
        break;
      }
      endscope = 1;
      goto RETURN;
    default:
    INVALID:
      warning(o, WARN_INVALID, c);
      break;
    }
    /* Return to sub-parsing routines. */
    if (flags && !(flags & HANDLE_DEFER)) {
      uchar test = flags;
      flags = 0;
      if (test & DEFERRED_STEP) {
        if (parse_step(o, &nd))
          flags = HANDLE_DEFER | DEFERRED_STEP;
      } else if (test & DEFERRED_SETTINGS)
        if (parse_settings(o, &nd))
          flags = HANDLE_DEFER | DEFERRED_SETTINGS;
    }
  }
FINISH:
  if (newscope == SCOPE_NEST)
    warning(o, "end of file without closing '>'s", c);
  if (newscope == SCOPE_BIND)
    warning(o, "end of file without closing '}'s", c);
RETURN:
  --o->calllevel;
  nd_fini(&nd);
  /* Should return from the calling scope if/when the parent scope is ended. */
  return (endscope && nd.scope != newscope);
}

static void parse(FILE *f, const char *fn, SGSParser *o) {
  memset(o, 0, sizeof(SGSParser));
  o->f = f;
  o->fn = fn;
  o->prg = calloc(1, sizeof(SGSProgram));
  o->st = SGSSymtab_create();
  o->line = 1;
  o->ampmult = 1.f; /* default until changed */
  o->def_time_ms = 1000; /* default until changed */
  o->def_freq = 444.f; /* default until changed */
  o->def_A4tuning = 444.f; /* default until changed */
  o->def_ratio = 1.f; /* default until changed */
  parse_level(o, 0, SCOPE_TOP);
  SGSSymtab_destroy(o->st);
}

static void group_events(EventData *to, int def_time_ms) {
  EventData *ge, *from = to->group_from, *until;
  int wait = 0, waitcount = 0;
  for (until = to->next;
       until && until->nest_level;
       until = until->next) ;
  for (ge = from; ge != until; ) {
    OperatorData *od;
    if (ge->nest_level) {
      ge = ge->next;
      continue;
    }
    if ((od = ge->operator)) {
      if (ge->next == until && od->time_ms < 0) /* Set and use default for last node in group */
        od->time_ms = def_time_ms;
      if (wait < od->time_ms)
        wait = od->time_ms;
    }
    ge = ge->next;
    if (ge) {
      //wait -= ge->wait_ms;
      waitcount += ge->wait_ms;
    }
  }
  for (ge = from; ge != until; ) {
    OperatorData *od;
    if (ge->nest_level) {
      ge = ge->next;
      continue;
    }
    if ((od = ge->operator)) {
      if (od->time_ms < 0)
        od->time_ms = wait + waitcount; /* fill in sensible default time */
    }
    ge = ge->next;
    if (ge) {
      waitcount -= ge->wait_ms;
    }
  }
  to->group_from = 0;
  if (until)
    until->wait_ms += wait;
}

static void time_event(EventData *e, int def_time_ms) {
  OperatorData *od = e->operator;
  VoiceData *vd = e->voice;
  /* Fill in blank valit durations */
  if (vd) {
    if (vd->valitpanning.time_ms < 0)
      vd->valitpanning.time_ms = def_time_ms;
  }
  if (od) {
    if (od->valitfreq.time_ms < 0)
      od->valitfreq.time_ms = od->time_ms;
    if (od->valitamp.time_ms < 0)
      od->valitamp.time_ms = od->time_ms;
    if (od->time_ms >= 0 && !(e->parse_flags & SILENCE_ADDED)) {
      od->time_ms += od->silence_ms;
      e->parse_flags |= SILENCE_ADDED;
    }
  }
  if (e->parse_flags & ADD_WAIT_DURATION) {
    if (e->next)
      ((EventData*)e->next)->wait_ms += od->time_ms;
    e->parse_flags &= ~ADD_WAIT_DURATION;
  }
}

static void flatten_events(EventData *e) {
  EventData *ce = e->sub_composite;
  EventData *se = e->next, *se_prev = e;
  int wait_ms = 0;
  int added_wait_ms = 0;
  if (!ce)
    return;
  /* Flatten composites */
  do {
    if (!se) {
      se_prev->next = ce;
      break;
    }
    wait_ms += se->wait_ms;
    if (se->next) {
      if ((wait_ms + se->next->wait_ms) <= (ce->wait_ms + added_wait_ms)) {
        se_prev = se;
        se = se->next;
        continue;
      }
    }
    if (se->wait_ms >= (ce->wait_ms + added_wait_ms)) {
      EventData *ce_next = ce->next;
      se->wait_ms -= ce->wait_ms + added_wait_ms;
      added_wait_ms = 0;
      wait_ms = 0;
      se_prev->next = ce;
      se_prev = ce;
      se_prev->next = se;
      ce = ce_next;
    } else {
      EventData *se_next, *ce_next;
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
  } while (ce);
  e->sub_composite = 0;
}

static void build_graph(SGSProgramEvent *root, EventData *root_in) {
  SGSProgramGraph *graph;
  uint i;
  uint size;
  VoiceData *voice_in = root_in->voice;
  if (!voice_in || !(root_in->params & SGS_GRAPH))
    return;
  size = voice_in->operators.count;
  if (!size)
    return;
  graph = malloc(sizeof(SGSProgramGraph) + sizeof(int) * (size - 1));
  graph->opc = size;
  for (i = 0; i < size; ++i)
    graph->ops[i] = voice_in->operators.na[i]->operator->operatorid;
  *(SGSProgramGraph**)&root->voice->graph = graph;
}

static void build_adjcs(SGSProgramEvent *root, EventData *root_in) {
  SGSProgramGraphAdjcs *adjcs;
  int *data;
  uint i;
  uint size;
  OperatorData *operator_in = root_in->operator;
  if (!operator_in || !(root_in->params & SGS_ADJCS))
    return;
  size = operator_in->pmods.count +
         operator_in->fmods.count +
         operator_in->amods.count;
  if (!size)
    return;
  adjcs = malloc(sizeof(SGSProgramGraphAdjcs) + sizeof(int) * (size - 1));
  adjcs->pmodc = operator_in->pmods.count;
  adjcs->fmodc = operator_in->fmods.count;
  adjcs->amodc = operator_in->amods.count;
  data = adjcs->adjcs;
  for (i = 0; i < adjcs->pmodc; ++i)
    *data++ = operator_in->pmods.na[i]->operator->operatorid;
  for (i = 0; i < adjcs->fmodc; ++i)
    *data++ = operator_in->fmods.na[i]->operator->operatorid;
  for (i = 0; i < adjcs->amodc; ++i)
    *data++ = operator_in->amods.na[i]->operator->operatorid;
  *(SGSProgramGraphAdjcs**)&root->operator->adjcs = adjcs;
}

static SGSProgram* build(SGSParser *o) {
  SGSProgram *prg = o->prg;
  EventData *e;
  SGSProgramEvent *oevents, *oe;
  uint id;
  uint alloc = 0;
  /* Pass #1 - perform timing adjustments */
  for (e = o->events; e; e = e->next) {
    time_event(e, o->def_time_ms);
    /* Handle composites (flatten in next loop) */
    if (e->sub_composite) {
      EventData *ce = e->sub_composite, *ce_prev = e;
      EventData *se = e->next;
      OperatorData *ceod = ce->operator;
      if (ceod->time_ms < 0)
        ceod->time_ms = o->def_time_ms;
      /* Timing for composites */
      for (;;) {
        OperatorData *ceod_prev = ce_prev->operator;
        if (ce->wait_ms) { /* Simulate delay with silence */
          ceod->silence_ms += ce->wait_ms;
          ce->params |= SGS_SILENCE;
          if (se)
            se->wait_ms += ce->wait_ms;
          ce->wait_ms = 0;
        }
        ce->wait_ms += ceod_prev->time_ms;
        if (ceod->time_ms < 0)
          ceod->time_ms = ceod_prev->time_ms - ceod_prev->silence_ms;
        time_event(ce, o->def_time_ms);
        e->operator->time_ms += ceod->time_ms;
        ce_prev = ce;
        ce = ce->next;
        if (!ce) break;
        ceod = ce->operator;
      }
    }
    /* Time |-terminated sequences */
    if (e->group_from)
      group_events(e, o->def_time_ms);
  }
  /* Pass #2 - flatten list */
  for (id = 0, e = o->events; e; e = e->next) {
    if (e->sub_composite)
      flatten_events(e);
    e->id = id++;
  }
  /* Pass #3 - produce output */
  for (id = 0, e = o->events; e; ) {
    EventData *e_next = e->next;
    OperatorData *od = e->operator;
    SGSProgramOperatorData *ood;
    VoiceData *vd = e->voice;
    SGSProgramVoiceData *ovd;
    /* Add to final output list */
    if (id >= alloc) {
      if (!alloc) alloc = 1;
      alloc <<= 1;
      oevents = realloc(oevents, sizeof(SGSProgramEvent) * alloc);
    }
    oe = &oevents[id];
    oe->wait_ms = e->wait_ms;
    oe->params = e->params;
    if (vd) {
      ovd = calloc(1, sizeof(SGSProgramVoiceData));
      oe->voiceid = vd->voiceid;
      oe->voice = ovd;
      ovd->attr = vd->attr;
      ovd->panning = vd->panning;
      ovd->valitpanning = vd->valitpanning;
      if (oe->params & SGS_GRAPH) {
        build_graph(oe, e);
        node_list_clear(&vd->operators);
      }
      free(vd);
    }
    if (od) {
      ood = calloc(1, sizeof(SGSProgramOperatorData));
      oe->voiceid = od->voiceid;
      oe->operator = ood;
      ood->operatorid = od->operatorid;
      ood->adjcs = 0;
      ood->attr = od->attr;
      ood->wave = od->wave;
      ood->time_ms = od->time_ms;
      ood->silence_ms = od->silence_ms;
      ood->freq = od->freq;
      ood->dynfreq = od->dynfreq;
      ood->phase = od->phase;
      ood->amp = od->amp;
      ood->dynamp = od->dynamp;
      ood->valitfreq = od->valitfreq;
      ood->valitamp = od->valitamp;
      if (oe->params & SGS_ADJCS) {
        build_adjcs(oe, e);
        node_list_clear(&od->pmods);
        node_list_clear(&od->fmods);
        node_list_clear(&od->amods);
      }
      free(od);
    }
    ++id;
    free((void*)e->sym);
    free(e);
    e = e_next;
  }
  *(SGSProgramEvent**)&prg->events = oevents;
  prg->eventc = id;
  prg->operatorc = o->operatorc;
  prg->voicec = o->voicec;
#if 1
  /* Debug printing */
  oe = oevents;
  putchar('\n');
  printf("events: %d\tvoices: %d\toperators: %d\n", prg->eventc, o->voicec, o->operatorc);
  for (id = 0; id < prg->eventc; ++id) {
    oe = &oevents[id];
    printf("\\%d \tEV %d", oe->wait_ms, id);
    if (oe->voice)
      printf("\n\tvo %d", oe->voiceid);
    if (oe->operator)
      printf("\n\top %d \tt=%d \tf=%.f", oe->operator->operatorid, oe->operator->time_ms, oe->operator->freq);
    putchar('\n');
  }
#endif

  return o->prg;
}

SGSProgram* SGSProgram_create(const char *filename) {
  SGSParser p;
  FILE *f = fopen(filename, "r");
  if (!f) return 0;

  parse(f, filename, &p);
  fclose(f);
  return build(&p);
}

void SGSProgram_destroy(SGSProgram *o) {
  uint i;
  for (i = 0; i < o->eventc; ++i) {
    SGSProgramEvent *e = (void*)&o->events[i];
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
