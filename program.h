/* operator parameters */
enum {
  /* voice values */
  SGS_GRAPH = 1<<0,
  SGS_PANNING = 1<<1,
  SGS_VALITPANNING = 1<<2,
  /* operator values */
  SGS_ADJC = 1<<3,
  SGS_WAVE = 1<<4,
  SGS_TIME = 1<<5,
  SGS_SILENCE = 1<<6,
  SGS_FREQ = 1<<7,
  SGS_VALITFREQ = 1<<8,
  SGS_DYNFREQ = 1<<9,
  SGS_PHASE = 1<<10,
  SGS_AMP = 1<<11,
  SGS_VALITAMP = 1<<12,
  SGS_DYNAMP = 1<<13,
  SGS_ATTR = 1<<14
};

/* operator wave types */
enum {
  SGS_WAVE_SIN = 0,
  SGS_WAVE_SRS,
  SGS_WAVE_TRI,
  SGS_WAVE_SQR,
  SGS_WAVE_SAW
};

/* operator atttributes */
enum {
  SGS_ATTR_FREQRATIO = 1<<1,
  SGS_ATTR_DYNFREQRATIO = 1<<2,
  SGS_ATTR_VALITFREQ = 1<<3,
  SGS_ATTR_VALITFREQRATIO = 1<<4,
  SGS_ATTR_VALITAMP = 1<<5,
  SGS_ATTR_VALITPANNING = 1<<6
};

/* value iteration types */
enum {
  SGS_VALIT_NONE = 0, /* when none given */
  SGS_VALIT_LIN,
  SGS_VALIT_EXP,
  SGS_VALIT_LOG
};

typedef struct SGSProgramGraphAdjc {
  uchar mainc;  /* either pmod count or operator-per-voice count */
  uchar fmodc;
  uchar amodc;
  uchar level;  /* index for buffer used to store result to use if node
                   revisited when traversing the graph. */
  int adjcs[1]; /* sized to total number */
} SGSProgramGraphAdjc;

typedef struct SGSProgramValit {
  int time_ms, pos_ms;
  float goal;
  uchar type;
} SGSProgramValit;

typedef struct SGSProgramVoiceData {
  const SGSProgramGraphAdjc *graph;
  uint id;
  float panning;
  SGSProgramValit valitpanning;
} SGSProgramVoiceData;

typedef struct SGSProgramOperatorData {
  const SGSProgramGraphAdjc *adjc;
  uint id;
  uchar attr, wave;
  int time_ms, silence_ms;
  float freq, dynfreq, phase, amp, dynamp;
  SGSProgramValit valitfreq, valitamp;
} SGSProgramOperatorData;

typedef struct SGSProgramEvent {
  int wait_ms;
  uint params;
  const SGSProgramVoiceData *voice;
  const SGSProgramOperatorData *operator;
} SGSProgramEvent;

struct SGSProgram {
  const SGSProgramEvent *events;
  uint eventc;
  uint operatorc,
       voicec;
};
