/* event types */
enum {
  SGS_EVENT_VOICE = 0,
  SGS_EVENT_OPERATOR
};

/* operator parameters */
enum {
  /* voice-operators or operator-operators linkage */
  SGS_ADJC = 1<<0,
  /* voice values */
  SGS_PANNING = 1<<1,
  SGS_VALITPANNING = 1<<2,
  /* operator values */
  SGS_WAVE = 1<<3,
  SGS_TIME = 1<<4,
  SGS_SILENCE = 1<<5,
  SGS_FREQ = 1<<6,
  SGS_VALITFREQ = 1<<7,
  SGS_DYNFREQ = 1<<8,
  SGS_PHASE = 1<<9,
  SGS_AMP = 1<<10,
  SGS_VALITAMP = 1<<11,
  SGS_DYNAMP = 1<<12,
  SGS_ATTR = 1<<13
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

typedef struct SGSProgramOpAdjc {
  uchar mainc;  /* either pmod count or operator-per-voice count */
  uchar fmodc;
  uchar amodc;
  uchar level;  /* index for buffer used to store result to use if node
                   revisited when traversing the graph. */
  int adjcs[1]; /* sized to total number */
} SGSProgramOpAdjc;

typedef struct SGSProgramValit {
  int time_ms, pos_ms;
  float goal;
  uchar type;
} SGSProgramValit;

typedef struct SGSProgramEvent {
  /* event info: (-1 for blank id) */
  int opprevid, opnextid; /* previous & next event for same operator */
  uchar type;
  int wait_ms;
  uint params;
  union {
    struct {
      uint id;
      const SGSProgramOpAdjc *graph;
      float panning;
      SGSProgramValit valitpanning;
    } voice;
    struct {
      uint id;
      const SGSProgramOpAdjc *adjc;
      uchar attr, wave;
      int time_ms, silence_ms;
      float freq, dynfreq, phase, amp, dynamp;
      SGSProgramValit valitfreq, valitamp;
    } operator;
  } data;
} SGSProgramEvent;

struct SGSProgram {
  SGSProgramEvent *events;
  uint eventc;
  uint operatorc,
       voicec;
};
