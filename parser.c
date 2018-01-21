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

enum {
  GRAPH = 1<<0,
  PMODS = 1<<1,
  FMODS = 1<<2,
  AMODS = 1<<3,
  OLD_VOICE_EVENT = 1<<4,
  ADD_WAIT_DURATION = 1<<5,
  SILENCE_ADDED = 1<<6
};

typedef struct VoiceData {
  struct EventNode *voprev; /* previous event with data for same voice */
  uint voiceid;
  /* parameters */
  float panning;
  SGSProgramValit valitpanning;
  /* operator linkage graph */
  uchar operatorc;
  struct EventNode **graph;
} VoiceData;

typedef struct OperatorData {
  struct EventNode *opprev; /* previous event with data for same operator */
  uint operatorid;
  struct EventNode *voice;
  /* parameters */
  uchar attr;
  uchar wave;
  int time_ms, silence_ms;
  float freq, dynfreq, phase, amp, dynamp;
  SGSProgramValit valitfreq, valitamp;
  /* node adjacents in operator linkage graph */
  uchar pmodc;
  uchar fmodc;
  uchar amodc;
  struct EventNode **pmods;
  struct EventNode **fmods;
  struct EventNode **amods;
} OperatorData;

typedef struct EventNode {
  struct EventNode *next, *lvnext;
  struct EventNode *groupfrom;
  struct EventNode *composite;
  int wait_ms;
  uint id;
  uint params;
  uint nestlevel;
  uchar parseflags;
  VoiceData *voice;
  OperatorData *operator;
} EventNode;

typedef struct SGSParser {
  FILE *f;
  const char *fn;
  SGSProgram *prg;
  SGSSymtab *st;
  uint line;
  uint calllevel;
  uint nestlevel;
  char nextc;
  /* node state */
  EventNode *events;
  EventNode *last_event;
  uint eventc;
  uint operatorc;
  uint voicec;
  /* settings/ops */
  float ampmult;
  uint def_time_ms;
  float def_freq, def_A4tuning, def_ratio;
} SGSParser;

typedef struct NodeTarget {
  EventNode *parent;
  uchar linktype;
} NodeTarget;

/* things that need to be separate for each nested parse_level() go here */
typedef struct NodeData {
  uchar setdef; /* adjusting default values */
  char scope;
  VoiceData voice;       /* state of voice changes for current event */
  OperatorData operator; /* state of operator changes for current event */
  EventNode *first, *current, *last;
  NodeTarget target;
  char *setsym;
  /* timing/delay */
  EventNode *group;
  EventNode *composite; /* grouping of events for a voice and/or operator */
  uchar end_composite;
  uint next_wait_ms; /* added for next event */
} NodeData;

static void add_adjc(EventNode *e, EventNode *adjc, uchar type) {
  EventNode ***adjcs = 0;
  uchar *count = 0;
  switch (type) {
  case GRAPH:
    count = &e->voice->operatorc;
    adjcs = &e->voice->graph;
    break;
  case PMODS:
    count = &e->operator->pmodc;
    adjcs = &e->operator->pmods;
    break;
  case FMODS:
    count = &e->operator->fmodc;
    adjcs = &e->operator->fmods;
    break;
  case AMODS:
    count = &e->operator->amodc;
    adjcs = &e->operator->amods;
    break;
  }
  if (*count && !(e->parseflags & type)) { /* adjacents were inherited */
    *count = 0;
    *adjcs = 0;
  }
  ++(*count);
  *adjcs = realloc(*adjcs, sizeof(OperatorData*) * (*count));
  (*adjcs)[*count - 1] = adjc;
  e->parseflags |= type;
}

static void end_operator(SGSParser *o, NodeData *nd);
static void end_voice(SGSParser *o, NodeData *nd);

static void new_event(SGSParser *o, NodeData *nd, EventNode *previous,
                      uchar composite) {
  EventNode *e;
  VoiceData *vd, *pvd;
  OperatorData *od;
  EventNode *parent;
  uchar linktype;
  if (previous) {
    if (!nd->next_wait_ms)
      return; /* nothing to do; event continues */
    if (!previous->params) {
      previous->wait_ms += nd->next_wait_ms;
      nd->next_wait_ms = 0;
      return; /* reuse repositioned event */
    }
  }
  end_operator(o, nd);
  end_voice(o, nd);
  e = calloc(1, sizeof(EventNode));
  e->wait_ms = nd->next_wait_ms;
  nd->next_wait_ms = 0;
  e->id = o->eventc++;
  e->nestlevel = o->nestlevel;
  e->voice = &nd->voice;
  vd = &nd->voice;
  e->operator = &nd->operator;
  od = &nd->operator;
  if (previous) {
    if (previous->voice) {
      *vd = *previous->voice;
      vd->voprev = previous;
    } else { /* set defaults */
      vd->voiceid = o->voicec++;
      vd->panning = 0.5f; /* center */
    }

    if (previous->operator) {
      *od = *previous->operator;
      od->opprev = previous;
    } else { /* set defaults */
      od->operatorid = o->operatorc++;
      od->amp = 1.0f;
    }
  }
  if (!nd->target.parent) {
    nd->target.parent = e; /* operator has new voice for parent */
    nd->target.linktype = GRAPH;
  }
  parent = nd->target.parent;
  linktype = nd->target.linktype;
  parent->params |= (linktype == GRAPH) ? SGS_GRAPH : SGS_ADJC;
  add_adjc(parent, e, linktype);

  /* Linkage */
  if (!nd->first)
    nd->first = e;
  if (!nd->group)
    nd->group = e;
  if (composite) {
    if (!nd->composite) {
      nd->composite = nd->current;
      nd->composite->composite = e;
      nd->last = nd->composite;
    } else {
      nd->last = (nd->last->composite) ? nd->last->composite :
                 nd->last->next;
      nd->last->next = e;
    }
    od->time_ms = -1; /* defaults to time of previous step of composite */
  } else {
    nd->last = nd->current;
    nd->current = e; /* then remains the same during composite events */
    nd->composite = 0;
  }
  if (!o->events)
    o->events = e;
  else
    o->last_event->next = e;
  o->last_event = e;

  /* Assign label? */
  if (nd->setsym) {
    SGSSymtab_set(o->st, nd->setsym, e);
    free(nd->setsym);
    nd->setsym = 0;
  }
}

static void end_voice(SGSParser *o, NodeData *nd) {
  EventNode *e = nd->current;
  VoiceData *vd = &nd->voice;
  if (!e)
    return; /* nothing to do */
  if (!vd->voprev) { /* initial event should reset its parameters */
    e->params |= SGS_GRAPH |
                 SGS_PANNING;
  } else {
    VoiceData *pvd = vd->voprev->voice;
    if (vd->panning != pvd->panning)
      e->params |= SGS_PANNING;
  }
  if (vd->valitpanning.type)
    e->params |= SGS_ATTR |
                 SGS_VALITPANNING;
  if (e->params) {
    e->voice = malloc(sizeof(VoiceData));
    *e->voice = *vd;
  } else {
    e->voice = 0;
    if (!vd->voprev)
      --o->voicec;
  }
  memset(vd, 0, sizeof(VoiceData));
}

static void end_operator(SGSParser *o, NodeData *nd) {
  EventNode *e = nd->current;
  OperatorData *od = &nd->operator;
  if (!e)
    return; /* nothing to do */
  if (!od->opprev) { /* initial event should reset its parameters */
    e->params |= SGS_ADJC |
                 SGS_WAVE |
                 SGS_TIME |
                 SGS_SILENCE |
                 SGS_FREQ |
                 SGS_DYNFREQ |
                 SGS_PHASE |
                 SGS_AMP |
                 SGS_DYNAMP |
                 SGS_ATTR;
  } else {
    VoiceData *pod = od->opprev->operator;
    if (od->attr != pod->attr)
      e->params |= SGS_ATTR;
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
    e->params |= SGS_ATTR |
                 SGS_VALITFREQ;
  if (od->valitamp.type)
    e->params |= SGS_ATTR |
                 SGS_VALITAMP;
  if (e->params) {
    e->operator = malloc(sizeof(OperatorData));
    *e->operator = *od;
    /* further changes */
    od = e->operator;
    if (e->nestlevel == 0)
      od->amp *= o->ampmult;
  } else {
    e->operator = 0;
    if (!od->opprev)
      --o->operatorc;
  }
  memset(od, 0, sizeof(OperatorData));
}

static void new_operator(SGSParser *o, NodeData *nd, NodeTarget *target,
                         uchar wave) {
  OperatorData *oe;
  end_operator(o, nd);
  new_event(o, nd, 0, SGS_EVENT_OPERATOR, 0);
  oe = nd->operator;
  od->wave = wave;
  nd->target = target;

  /* Set defaults */
  od->amp = 1.f;
  if (!e->nestlevel) {
    od->time_ms = -1; /* later fitted or set to default */
    od->freq = o->def_freq;
  } else {
    od->time_ms = o->def_time_ms;
    od->freq = o->def_ratio;
    od->attr |= SGS_ATTR_FREQRATIO;
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

static float read_num_r(SGSParser *o, float (*read_symbol)(SGSParser *o), char *buf, uint len, uchar pri, uint level) {
  char *p = buf;
  uchar dot = 0;
  float num;
  char c;
  c = getc(o->f);
  if (level)
    read_ws(o);
  if (c == '(') {
    return read_num_r(o, read_symbol, buf, len, 255, level+1);
  }
  if (read_symbol &&
      ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
    ungetc(c, o->f);
    num = read_symbol(o);
    if (num == num) { /* not NAN; was recognized */
      c = getc(o->f);
      goto LOOP;
    }
  }
  if (c == '-') {
    *p++ = c;
    c = getc(o->f);
    if (level)
      read_ws(o);
  }
  while ((c >= '0' && c <= '9') || (!dot && (dot = (c == '.')))) {
    if ((p+1) == (buf+len)) {
      break;
    }
    *p++ = c;
    c = getc(o->f);
  }
  if (p == buf) {
    ungetc(c, o->f);
    return NAN;
  }
  *p = '\0';
  num = strtod(buf, 0);
LOOP:
  for (;;) {
    if (level)
      read_ws(o);
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
    c = getc(o->f);
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
static uchar read_sym(SGSParser *o, char **sym, char op) {
  uint i = 0;
  char nosym_msg[] = "ignoring ? without symbol name";
  nosym_msg[9] = op; /* replace ? */
  if (!*sym)
    *sym = malloc(SYMKEY_LEN);
  for (;;) {
    char c = getc(o->f);
    if (IS_WHITESPACE(c) || c == EOF) {
      ungetc(c, o->f);
      if (i == 0)
        warning(o, nosym_msg, c);
      else END_OF_SYM: {
        (*sym)[i] = '\0';
        return 1;
      }
      break;
    } else if (i == SYMKEY_LEN) {
      warning(o, "ignoring symbol name from "SYMKEY_LEN_A"th digit", c);
      goto END_OF_SYM;
    }
    (*sym)[i++] = c;
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
  vi->time_ms = -1;
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
    nd->last->e.parseflags |= ADD_WAIT_DURATION;
  } else {
    float wait;
    int wait_ms;
    read_num(o, 0, &wait);
    if (wait < 0.f) {
      warning(o, "ignoring '\\' with sub-zero time", c);
      return 0;
    }
    SET_I2F(wait_ms, wait*1000.f);
    nd->next_wait_ms += wait_ms;
  }
  return 1;
}

/*
 * Main parser functions
 */

enum {
  SCOPE_SAME = 0,
  SCOPE_TOP = 1
};

static uchar parse_settings(SGSParser *o) {
  char c;
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

static uchar parse_level(SGSParser *o, NodeData *parentnd,
                         NodeTarget *target, char newscope);

static uchar parse_step(SGSParser *o, NodeData *nd) {
  char c;
  OperatorData *oe = nd->operator;
  VoiceData *ve = od->voice;
  NodeTarget *target = nd->target;
  while ((c = read_char(o)) != EOF) {
    switch (c) {
    case '\\':
      if (read_waittime(o, nd, c)) {
        end_operator(o, nd);
        new_event(o, nd, &nd->last->e, 0, 0);
        oe = nd->operator;
      }
      break;
    case 'a':
      if (target &&
          (target->linktype == AMOD_ADJCS ||
           target->linktype == FMOD_ADJCS))
        goto UNKNOWN;
      if (testgetc('!', o->f)) {
        if (!testc('{', o->f)) {
          read_num(o, 0, &od->dynamp);
        }
        if (testgetc('{', o->f)) {
          NodeTarget nt = {&od->e, AMOD_ADJCS};
          if (e->params & SGS_ADJC)
            e->amodc = 0;
          ++o->nestlevel;
          parse_level(o, nd, &nt, '{');
          --o->nestlevel;
        }
      } else if (testgetc('[', o->f)) {
        if (read_valit(o, 0, &od->valitamp))
          od->attr |= SGS_ATTR_VALITAMP;
      } else {
        read_num(o, 0, &od->amp);
        e->params |= SGS_AMP;
        if (!od->valitamp.type)
          od->attr &= ~SGS_ATTR_VALITAMP;
      }
      break;
    case 'b':
      if (e->nestlevel)
        goto UNKNOWN;
      if (testgetc('[', o->f)) {
        if (read_valit(o, 0, &ve->valitpanning))
          od->attr |= SGS_ATTR_VALITPANNING;
      } else if (read_num(o, 0, &ve->panning)) {
        if (!ve->valitpanning.type)
          od->attr &= ~SGS_ATTR_VALITPANNING;
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
          NodeTarget nt = {&od->e, FMOD_ADJCS};
          if (e->params & SGS_ADJC)
            e->fmodc = 0;
          ++o->nestlevel;
          parse_level(o, nd, &nt, '{');
          --o->nestlevel;
        }
      } else if (testgetc('[', o->f)) {
        if (read_valit(o, read_note, &od->valitfreq)) {
          od->attr |= SGS_ATTR_VALITFREQ;
          od->attr &= ~SGS_ATTR_VALITFREQRATIO;
        }
      } else if (read_num(o, read_note, &od->freq)) {
        od->attr &= ~SGS_ATTR_FREQRATIO;
        e->params |= SGS_FREQ;
        if (!od->valitfreq.type)
          od->attr &= ~(SGS_ATTR_VALITFREQ |
                              SGS_ATTR_VALITFREQRATIO);
      }
      break;
    case 'p':
      if (read_num(o, 0, &od->phase)) {
        od->phase = fmod(od->phase, 1.f);
        if (od->phase < 0.f)
          od->phase += 1.f;
        e->params |= SGS_PHASE;
      }
      break;
    case 'r':
      if (!target)
        goto UNKNOWN;
      if (testgetc('!', o->f)) {
        if (!testc('{', o->f)) {
          if (read_num(o, 0, &od->dynfreq)) {
            od->dynfreq = 1.f / od->dynfreq;
            od->attr |= SGS_ATTR_DYNFREQRATIO;
          }
        }
        if (testgetc('{', o->f)) {
          NodeTarget nt = {&od->e, FMOD_ADJCS};
          if (e->params & SGS_ADJC)
            e->fmodc = 0;
          ++o->nestlevel;
          parse_level(o, nd, &nt, '{');
          --o->nestlevel;
        }
      } else if (testgetc('[', o->f)) {
        if (read_valit(o, read_note, &od->valitfreq)) {
          od->valitfreq.goal = 1.f / od->valitfreq.goal;
          od->attr |= SGS_ATTR_VALITFREQ |
                            SGS_ATTR_VALITFREQRATIO;
        }
      } else if (read_num(o, 0, &od->freq)) {
        od->freq = 1.f / od->freq;
        od->attr |= SGS_ATTR_FREQRATIO;
        e->params |= SGS_FREQ;
        if (!od->valitfreq.type)
          od->attr &= ~(SGS_ATTR_VALITFREQ |
                              SGS_ATTR_VALITFREQRATIO);
      }
      break;
    case 's': {
      float silence;
      read_num(o, 0, &silence);
      if (silence < 0.f) {
        warning(o, "ignoring 's' with sub-zero time", c);
        break;
      }
      SET_I2F(od->silence_ms, silence*1000.f);
      break; }
    case 't':
      if (testgetc('*', o->f))
        od->time_ms = -1; /* later fitted or set to default */
      else {
        float time;
        read_num(o, 0, &time);
        if (time < 0.f) {
          warning(o, "ignoring 't' with sub-zero time", c);
          break;
        }
        SET_I2F(od->time_ms, time*1000.f);
      }
      e->params |= SGS_TIME;
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
static uchar parse_level(SGSParser *o, NodeData *parentnd,
                         NodeTarget *target, char newscope) {
  char c;
  uchar endscope = 0;
  uchar flags = 0;
  NodeData nd;
  ++o->calllevel;
  memset(&nd, 0, sizeof(NodeData));
  nd.scope = newscope;
  if (parentnd) {
    nd.setdef = parentnd->setdef;
    if (newscope == SCOPE_SAME)
      nd.scope = parentnd->scope;
    nd.voice = parentnd->voice;
  }
  while ((c = read_char(o)) != EOF) {
    flags &= ~HANDLE_DEFER;
    switch (c) {
    case NEWLINE:
      ++o->line;
      if (nd.scope == SCOPE_TOP) {
        end_operator(o, &nd);
        if (o->calllevel > 1)
          goto RETURN;
        flags = 0;
        nd.first = 0;
        nd.setdef = 0;
      }
      break;
    case '-': {
      OperatorData *first, *last;
      NodeTarget nt;
      uchar ret;
      end_operator(o, &nd);
      first = nd.first;
      last = nd.last;
      if (!last) {
        if (o->calllevel == 1) NO_CARRIER: {
          warning(o, "no preceding carrier operators", c);
          break;
        }
        first = parentnd->first;
        last = (parentnd->operator ? parentnd->operator : parentnd->last);
        if (!last)
          goto NO_CARRIER;
      }
      nt = (NodeTarget){&last->e, MAIN_ADJCS};
      if (first && first != last) {
        warning(o, "multiple carriers not yet supported", c);
        break;
      }
      if (last->e.params & SGS_ADJC)
        last->e.pmodc = 0;
      ++o->nestlevel;
      ret = parse_level(o, &nd, &nt, SCOPE_SAME);
      --o->nestlevel;
      if (ret)
        goto RETURN;
      break; }
    case ':':
      end_operator(o, &nd);
      if (nd.setsym)
        warning(o, "ignoring label assignment to label reference", c);
      nd.setdef = 0;
      if (read_sym(o, &nd.setsym, ':')) {
        OperatorData *ref = SGSSymtab_get(o->st, nd.setsym);
        if (!ref)
          warning(o, "ignoring reference to undefined label", c);
        else {
          new_event(o, &nd, &ref->e, 0, 0);
          flags = parse_step(o, &nd) ? (HANDLE_DEFER | DEFERRED_STEP) : 0;
        }
      }
      break;
    case ';':
      if (newscope == SCOPE_SAME) {
        o->nextc = c;
        goto RETURN;
      }
      if (nd.setdef || (!nd.operator && !nd.last))
        goto INVALID;
      end_operator(o, &nd);
      new_event(o, &nd, &nd.last->e, 0, 1);
      flags = parse_step(o, &nd) ? (HANDLE_DEFER | DEFERRED_STEP) : 0;
      break;
    case '<':
      if (parse_level(o, &nd, target, '<'))
        goto RETURN;
      break;
    case '>':
      if (nd.scope != '<') {
        warning(o, "closing '>' without opening '<'", c);
        break;
      }
      end_operator(o, &nd);
      endscope = 1;
      goto RETURN;
    case 'O': {
      int wave = read_wavetype(o, c);
      if (wave < 0)
        break;
      nd.setdef = 0;
      new_operator(o, &nd, target, wave);
      flags = parse_step(o, &nd) ? (HANDLE_DEFER | DEFERRED_STEP) : 0;
      break; }
    case 'Q':
      goto FINISH;
    case 'S':
      nd.setdef = 1;
      flags = parse_settings(o) ? (HANDLE_DEFER | DEFERRED_SETTINGS) : 0;
      break;
    case '\\':
      if (nd.setdef ||
          (nd.operator && nd.operator->e.nestlevel))
        goto INVALID;
      read_waittime(o, &nd, c);
      break;
    case '\'':
      end_operator(o, &nd);
      if (nd.setsym) {
        warning(o, "ignoring label assignment to label assignment", c);
        break;
      }
      read_sym(o, &nd.setsym, '\'');
      break;
    case '{':
      /* is always got elsewhere before a nesting call to this function */
      warning(o, "opening curly brace out of place", c);
      break;
    case '|':
      if (nd.setdef ||
          (nd.operator && nd.operator->e.nestlevel))
        goto INVALID;
      if (newscope == SCOPE_SAME) {
        o->nextc = c;
        goto RETURN;
      }
      end_operator(o, &nd);
      if (!nd.last) {
        warning(o, "end of sequence before any parts given", c);
        break;
      }
      if (nd.group) {
        nd.current->e.groupfrom = nd.group;
        nd.group = 0;
      }
      break;
    case '}':
      if (nd.scope != '{') {
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
        if (parse_settings(o))
          flags = HANDLE_DEFER | DEFERRED_SETTINGS;
    }
  }
FINISH:
  if (newscope == '<')
    warning(o, "end of file without closing '>'s", c);
  if (newscope == '{')
    warning(o, "end of file without closing '}'s", c);
RETURN:
  if (nd.operator) {
    if (nd.operator->time_ms < 0)
      nd.operator->time_ms = o->def_time_ms; /* use default */
    end_operator(o, &nd);
  }
  if (nd.current)
    nd.current->e.groupfrom = nd.group;
  if (nd.setsym)
    free(nd.setsym);
  --o->calllevel;
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
  parse_level(o, 0, 0, SCOPE_TOP);
  SGSSymtab_destroy(o->st);
}

static void group_events(OperatorData *to, int def_time_ms) {
  OperatorData *ge, *from = to->e.groupfrom, *until;
  int wait = 0, waitcount = 0;
  for (until = to->e.next;
       until && until->e.nestlevel;
       until = until->e.next) ;
  for (ge = from; ge != until; ) {
    if (ge->e.nestlevel)
      continue;
    if (ge->e.type == SGS_EVENT_OPERATOR) {
      if (ge->e.next == until && ge->time_ms < 0) /* Set and use default for last node in group */
        ge->time_ms = def_time_ms;
      if (wait < ge->time_ms)
        wait = ge->time_ms;
    }
    ge = ge->e.next;
    if (ge) {
      //wait -= ge->e.wait_ms;
      waitcount += ge->e.wait_ms;
    }
  }
  for (ge = from; ge != until; ) {
    if (ge->e.type == SGS_EVENT_OPERATOR) {
      if (ge->time_ms < 0)
        ge->time_ms = wait + waitcount; /* fill in sensible default time */
    }
    ge = ge->e.next;
    if (ge) {
      waitcount -= ge->e.wait_ms;
    }
  }
  to->e.groupfrom = 0;
  if (until)
    until->e.wait_ms += wait;
}

static void time_event(OperatorData *oe) {
  VoiceData *ve = od->voice;
  /* Fill in blank valit durations */
  if (od->valitfreq.time_ms < 0)
    od->valitfreq.time_ms = od->time_ms;
  if (od->valitamp.time_ms < 0)
    od->valitamp.time_ms = od->time_ms;
  if (ve->valitpanning.time_ms < 0)
    ve->valitpanning.time_ms = od->time_ms;
  if (od->time_ms >= 0 && !(e->parseflags & SILENCE_ADDED)) {
    od->time_ms += od->silence_ms;
    e->parseflags |= SILENCE_ADDED;
  }
  if (e->parseflags & ADD_WAIT_DURATION) {
    if (e->next)
      ((EventNode*)e->next)->wait_ms += od->time_ms;
    e->parseflags &= ~ADD_WAIT_DURATION;
  }
}

static void flatten_events(OperatorData *e) {
  EventNode *ce = e->composite;
  EventNode *se = e->e.next, *se_prev = e;
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
      EventNode *ce_next = ce->next;
      se->wait_ms -= ce->wait_ms + added_wait_ms;
      added_wait_ms = 0;
      wait_ms = 0;
      se_prev->next = ce;
      se_prev = ce;
      se_prev->next = se;
      ce = ce_next;
    } else {
      EventNode *se_next, *ce_next;
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
  e->composite = 0;
}

static SGSProgram* build(SGSParser *o) {
  SGSProgram *prg = o->prg;
  OperatorData *e;
  SGSProgramEvent *oe;
  uint id;
  uint alloc = 0;
  /* Pass #1 - perform timing adjustments */
  for (e = o->events; e; e = e->next) {
    time_event(e);
    /* Handle composites (flatten in next loop) */
    if (e->composite) {
      OperatorData *ce = e->composite, *ce_prev = e;
      OperatorData *se = e->next;
      if (ce->time_ms < 0)
        ce->time_ms = o->def_time_ms;
      /* Timing for composites */
      do {
        if (ce->wait_ms) { /* Simulate delay with silence */
          ce->silence_ms += ce->wait_ms;
          ce->params |= SGS_SILENCE;
          if (se)
            se->wait_ms += ce->wait_ms;
          ce->wait_ms = 0;
        }
        ce->wait_ms += ce_prev->time_ms;
        if (ce->time_ms < 0)
          ce->time_ms = ce_prev->time_ms - ce_prev->silence_ms;
        time_event(ce);
        e->time_ms += ce->time_ms;
        ce_prev = ce;
        ce = ce->next;
      } while (ce);
    }
    /* Time |-terminated sequences */
    if (e->groupfrom)
      group_events(e, o->def_time_ms);
  }
  /* Pass #2 - flatten list */
  for (id = 0, e = o->events; e; e = e->next) {
    if (e->composite)
      flatten_events(e);
    e->id = id++;
  }
  /* Pass #3 - produce output */
  for (id = 0, e = o->events; e; ) {
    OperatorData *e_next = e->next;
    /* Add to final output list */
    if (id >= alloc) {
      if (!alloc) alloc = 1;
      alloc <<= 1;
      prg->events = realloc(prg->events, sizeof(SGSProgramEvent) * alloc);
    }
    oe = &prg->events[id];
    od->opprevid = (e->opprev ? (int)e->opprev->id : -1);
    od->type = e->type;
    od->wait_ms = e->wait_ms;
    e->params = e->params;
    if (od->type == SGS_EVENT_VOICE) {
      od->data.voice.id = e->voiceid;
      od->data.voice.graph = 0;
      od->data.voice.panning = e->topop.panning;
      od->data.voice.valitpanning = e->topop.valitpanning;
    } else {
      od->data.operator.id = e->opid;
      od->data.operator.adjc = 0;
      od->data.operator.attr = e->attr;
      od->data.operator.wave = e->wave;
      od->data.operator.time_ms = e->time_ms;
      od->data.operator.silence_ms = e->silence_ms;
      od->data.operator.freq = e->freq;
      od->data.operator.dynfreq = e->dynfreq;
      od->data.operator.phase = e->phase;
      od->data.operator.amp = e->amp;
      od->data.operator.dynamp = e->dynamp;
      od->data.operator.valitfreq = e->valitfreq;
      od->data.operator.valitamp = e->valitamp;
    }
    ++id;
    free(e);
    e = e_next;
  }
  prg->eventc = id;
#if 1
  /* Debug printing */
  oe = prg->events;
  putchar('\n');
  for (id = 0; id < prg->eventc; ++id) {
    oe = &prg->events[id];
    if (od->type == SGS_EVENT_VOICE)
      printf("ev %d, vo %d: \t/=%d", id, od->data.voice.id, od->wait_ms);
    else
      printf("ev %d, op %d: \t/=%d \tt=%d\n", id, od->data.operator.id, od->wait_ms, od->data.operator.time_ms);
  }
#else
  /* Debug printing */
  e = o->events;
  putchar('\n');
  do{
    printf("ev %d, op %d (%s): \t/=%d \tt=%d\n", e->id, e->opid, e->optype ? "nested" : "top", e->wait_ms, e->time_ms);
  } while ((e = e->next));
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
  }
  free(o->events);
}
