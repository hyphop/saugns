enum {
  SGS_TYPE_TOP = 0,               /* value */
  SGS_TYPE_NESTED,                /* value, flag */
  SGS_TYPE_SET,                   /* flag */
  SGS_TYPE_SETTOP = SGS_TYPE_SET, /* value */
  SGS_TYPE_SETNESTED,             /* value (sets NESTED, not itself NESTED!) */
  SGS_TYPE_ENV                    /* value */
};

enum {
  SGS_FLAG_EXEC = 1<<0,
  SGS_FLAG_ENTERED = 1<<1
};

enum {
  SGS_ATTR_FREQRATIO = 1<<0,
  SGS_ATTR_DYNFREQRATIO = 1<<1
};

enum {
  SGS_WAVE_SIN = 0,
  SGS_WAVE_SQR,
  SGS_WAVE_TRI,
  SGS_WAVE_SAW
};

enum {
  SGS_TIME = 1<<0,
  SGS_SILENCE = 1<<1,
  SGS_FREQ = 1<<2,
  SGS_DYNFREQ = 1<<3,
  SGS_PHASE = 1<<4,
  SGS_AMP = 1<<5,
  SGS_DYNAMP = 1<<6,
  SGS_PANNING = 1<<7,
  SGS_ATTR = 1<<8,
  SGS_VALUES = 9
};

enum {
  SGS_PMODS = 1<<0,
  SGS_FMODS = 1<<1,
  SGS_AMODS = 1<<2
};

typedef struct SGSProgramNodeChain {
  uint count;
  struct SGSProgramNode *chain;
} SGSProgramNodeChain;

typedef struct SGSProgramOpNode {
  struct SGSProgramOpNode *next;
  uchar type, wave;
  uint id;
  struct SGSProgramOpNode *link;
} SGSProgramOpNode;

typedef struct SGSProgramEventNode {
  SGSProgramOpNode *node;
  SGSProgramNodeChain pmod, fmod, amod;
  float delay, time, silence, freq, dynfreq, phase, amp, dynamp, panning;
  ushort values;
  uchar attr;
  uchar mods;
  uint id;
} SGSProgramEventNode;

struct SGSProgram {
  SGSProgramNode *oplist;
  SGSProgramEvent *eventlist;
  uint nodec;
  uint topc; /* nodes >= topc are nested ones, ids starting over from 0 */
  uint eventc;
};
