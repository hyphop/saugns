#include "sgensys.h"
#include "osc.h"
#include "program.h"
#include <stdio.h>
#include <stdlib.h>

enum {
  SGS_FLAG_INIT = 1<<0,
  SGS_FLAG_EXEC = 1<<1
};

typedef union BufData {
  int i;
  float f;
} BufData;

#define BUF_LEN 256
typedef BufData Buf[BUF_LEN];

typedef struct ParameterValit {
  uint time, pos;
  float goal;
  uchar type;
} ParameterValit;

typedef struct OperatorNode {
  int time;
  uint silence;
  const SGSProgramGraphAdjcs *adjcs;
  uchar type, attr;
  float freq, dynfreq;
  SGSOscLuv *osctype;
  SGSOsc osc;
  float amp, dynamp;
  ParameterValit valitamp, valitfreq;
} OperatorNode;

typedef struct VoiceNode {
  int pos; /* negative for wait time */
  uchar flag, attr;
  const SGSProgramGraph *graph;
  float panning;
  ParameterValit valitpanning;
} VoiceNode;

typedef union Data {
  int i;
  float f;
  void *v;
} Data;

typedef struct EventNode {
  void *node;
  uint waittime;
} EventNode;

typedef struct SetNode {
  int voiceid, operatorid;
  uint params;
  Data data[1]; /* sized for number of parameters set */
} SetNode;

static uint count_flags(uint flags) {
  uint i, count = 0;
  for (i = 0; i < (8 * sizeof(uint)); ++i) {
    count += flags & 1;
    flags >>= 1;
  }
  return count;
}

struct SGSGenerator {
  uint srate;
  Buf *bufs;
  uint bufc;
  double osc_coeff;
  uint event, eventc;
  uint eventpos;
  EventNode *events;
  uint voice, voicec;
  VoiceNode *voices;
  OperatorNode operators[1]; /* sized to number of nodes */
};

/*
 * Count buffers needed for operator, including linked operators.
 * TODO: Verify, remove debug printing when parser module done.
 */
static uint calc_bufs(SGSGenerator *o, OperatorNode *n) {
  uint count = 0, i, res;
  if (n->adjcs) {
    const int *mods = n->adjcs->adjcs;
    const uint modc = n->adjcs->fmodc + n->adjcs->pmodc + n->adjcs->amodc;
    for (i = 0; i < modc; ++i) {
  printf("visit node %d\n", mods[i]);
      res = calc_bufs(o, &o->operators[mods[i]]);
      if (res > count) count = res;
    }
  }
  return count + 5;
}

/*
 * Check operators for voice and increase the buffer allocation if needed.
 * TODO: Verify, remove debug printing when parser module done.
 */
static void upsize_bufs(SGSGenerator *o, VoiceNode *vn) {
  uint count = 0, i, res;
  if (!vn->graph) return;
  for (i = 0; i < vn->graph->opc; ++i) {
  printf("visit node %d\n", vn->graph->ops[i]);
    res = calc_bufs(o, &o->operators[vn->graph->ops[i]]);
    if (res > count) count = res;
  }
  printf("%d -?!\n", count);
  if (count > o->bufc) {
    o->bufs = realloc(o->bufs, sizeof(Buf) * count);
    o->bufc = count;
  }
}

/*
 * Allocate SGSGenerator with the passed sample rate and using the
 * given SGSProgram.
 */
SGSGenerator* SGS_generator_create(uint srate, struct SGSProgram *prg) {
  SGSGenerator *o;
  const SGSProgramEvent *step;
  void *data;
  uint i, indexwaittime;
  uint size, eventssize, voicessize, operatorssize, setssize;
  /*
   * Establish allocation sizes.
   */
  size = sizeof(SGSGenerator) - sizeof(OperatorNode);
  eventssize = sizeof(EventNode) * prg->eventc;
  voicessize = sizeof(VoiceNode) * prg->voicec;
  operatorssize = sizeof(OperatorNode) * prg->operatorc;
  setssize = 0;
  for (i = 0; i < prg->eventc; ++i) {
    step = &prg->events[i];
    setssize += sizeof(SetNode) +
                (sizeof(Data) *
                 (count_flags(step->params) +
                  count_flags(step->params & (SGS_VALITFREQ |
                                              SGS_VALITAMP |
                                              SGS_VALITPANNING))*2 - 1));
  }
  /*
   * Allocate & initialize.
   */
  o = calloc(1, size + operatorssize + eventssize + voicessize + setssize);
  o->srate = srate;
  o->osc_coeff = SGSOsc_COEFF(srate);
  o->event = 0;
  o->eventc = prg->eventc;
  o->eventpos = 0;
  o->events = (void*)(((uchar*)o) + size + operatorssize);
  o->voice = 0;
  o->voicec = prg->voicec;
  o->voices = (void*)(((uchar*)o) + size + operatorssize + eventssize);
  data      = (void*)(((uchar*)o) + size + operatorssize + eventssize + voicessize);
  SGSOsc_init();
  /*
   * Fill in events according to the SGSProgram, ie. copy timed state
   * changes for voices and operators.
   */
  indexwaittime = 0;
  for (i = 0; i < prg->eventc; ++i) {
    EventNode *e;
    SetNode *s;
    Data *set;
    step = &prg->events[i];
    e = &o->events[i];
    s = data;
    set = s->data;
    e->node = s;
    e->waittime = ((float)step->wait_ms) * srate * .001f;
    indexwaittime += e->waittime;
    s->params = step->params;
    if (step->voice) {
      const SGSProgramVoiceData *vd = step->voice;
      s->voiceid = step->voiceid;
      if (s->params & SGS_GRAPH)
        (*set++).v = (void*)vd->graph;
      if (s->params & SGS_VOATTR)
        (*set++).i = vd->attr;
      if (s->params & SGS_PANNING)
        (*set++).f = vd->panning;
      if (s->params & SGS_VALITPANNING) {
        (*set++).i = ((float)vd->valitpanning.time_ms) * srate * .001f;
        (*set++).f = vd->valitpanning.goal;
        (*set++).i = vd->valitpanning.type;
      }
      o->voices[s->voiceid].pos = -indexwaittime;
      indexwaittime = 0;
    } else {
      s->voiceid = -1;
    }
    if (step->operator) {
      const SGSProgramOperatorData *od = step->operator;
      s->voiceid = step->voiceid;
      s->operatorid = od->operatorid;
      if (s->params & SGS_ADJCS)
        (*set++).v = (void*)od->adjcs;
      if (s->params & SGS_OPATTR)
        (*set++).i = od->attr;
      if (s->params & SGS_WAVE)
        (*set++).i = od->wave;
      if (s->params & SGS_TIME) {
        (*set++).i = (od->time_ms == SGS_TIME_INF) ?
                     SGS_TIME_INF :
                     (int) ((float)od->time_ms) * srate * .001f;
      }
      if (s->params & SGS_SILENCE)
        (*set++).i = ((float)od->silence_ms) * srate * .001f;
      if (s->params & SGS_FREQ)
        (*set++).f = od->freq;
      if (s->params & SGS_VALITFREQ) {
        (*set++).i = ((float)od->valitfreq.time_ms) * srate * .001f;
        (*set++).f = od->valitfreq.goal;
        (*set++).i = od->valitfreq.type;
      }
      if (s->params & SGS_DYNFREQ)
        (*set++).f = od->dynfreq;
      if (s->params & SGS_PHASE)
        (*set++).i = SGSOsc_PHASE(od->phase);
      if (s->params & SGS_AMP)
        (*set++).f = od->amp;
      if (s->params & SGS_VALITAMP) {
        (*set++).i = ((float)od->valitamp.time_ms) * srate * .001f;
        (*set++).f = od->valitamp.goal;
        (*set++).i = od->valitamp.type;
      }
      if (s->params & SGS_DYNAMP)
        (*set++).f = od->dynamp;
    } else {
      s->operatorid = -1;
    }
    data = (void*)(((uchar*)data) +
                   (sizeof(SetNode) - sizeof(Data)) +
                   (((uchar*)set) - ((uchar*)s->data)));
  }
  return o;
}

/*
 * Processes one event; to be called for the event when its time comes.
 */
static void SGS_generator_handle_event(SGSGenerator *o, EventNode *e) {
  if (1) /* more types to be added in the future */ {
    const SetNode *s = e->node;
    VoiceNode *vn;
    OperatorNode *on;
    const Data *data = s->data;
    /*
     * Set state of voice and/or operator.
     */
    if (s->voiceid >= 0) {
      vn = &o->voices[s->voiceid];
      if (s->params & SGS_GRAPH)
        vn->graph = (*data++).v;
      if (s->params & SGS_VOATTR) {
        uchar attr = (uchar)(*data++).i;
        vn->attr = attr;
      }
      if (s->params & SGS_PANNING)
        vn->panning = (*data++).f;
      if (s->params & SGS_VALITPANNING) {
        vn->valitpanning.time = (*data++).i;
        vn->valitpanning.pos = 0;
        vn->valitpanning.goal = (*data++).f;
        vn->valitpanning.type = (*data++).i;
      }
      upsize_bufs(o, vn);
      vn->flag |= SGS_FLAG_INIT | SGS_FLAG_EXEC;
      vn->pos = 0;
      if ((int)o->voice > s->voiceid) /* go back to re-activated node */
        o->voice = s->voiceid;
    }
    if (s->operatorid >= 0) {
      on = &o->operators[s->operatorid];
      if (s->params & SGS_ADJCS)
        on->adjcs = (*data++).v;
      if (s->params & SGS_OPATTR) {
        uchar attr = (uchar)(*data++).i;
        if (!(s->params & SGS_FREQ)) {
          /* May change during processing; preserve state of FREQRATIO flag */
          attr &= ~SGS_ATTR_FREQRATIO;
          attr |= on->attr & SGS_ATTR_FREQRATIO;
        }
        on->attr = attr;
      }
      if (s->params & SGS_WAVE) switch ((*data++).i) {
      case SGS_WAVE_SIN:
        on->osctype = SGSOsc_sin;
        break;
      case SGS_WAVE_SRS:
        on->osctype = SGSOsc_srs;
        break;
      case SGS_WAVE_TRI:
        on->osctype = SGSOsc_tri;
        break;
      case SGS_WAVE_SQR:
        on->osctype = SGSOsc_sqr;
        break;
      case SGS_WAVE_SAW:
        on->osctype = SGSOsc_saw;
        break;
      }
      if (s->params & SGS_TIME)
        on->time = (*data++).i;
      if (s->params & SGS_SILENCE)
        on->silence = (*data++).i;
      if (s->params & SGS_FREQ)
        on->freq = (*data++).f;
      if (s->params & SGS_VALITFREQ) {
        on->valitfreq.time = (*data++).i;
        on->valitfreq.pos = 0;
        on->valitfreq.goal = (*data++).f;
        on->valitfreq.type = (*data++).i;
      }
      if (s->params & SGS_DYNFREQ)
        on->dynfreq = (*data++).f;
      if (s->params & SGS_PHASE)
        SGSOsc_SET_PHASE(&on->osc, (uint)(*data++).i);
      if (s->params & SGS_AMP)
        on->amp = (*data++).f;
      if (s->params & SGS_VALITAMP) {
        on->valitamp.time = (*data++).i;
        on->valitamp.pos = 0;
        on->valitamp.goal = (*data++).f;
        on->valitamp.type = (*data++).i;
      }
      if (s->params & SGS_DYNAMP)
        on->dynamp = (*data++).f;
    }
  }
}

void SGS_generator_destroy(SGSGenerator *o) {
  free(o->bufs);
  free(o);
}

/*
 * Fill buffer with buflen float values for a parameter; these may
 * either simply be a copy of the supplied state, or modified.
 *
 * If a parameter valit (VALue ITeration) is supplied, the values
 * are shaped according to its timing, target value and curve
 * selection. Once elapsed, the state will also be set to its final
 * value.
 *
 * Passing a modifier buffer will accordingly multiply each output
 * value, done to get absolute values from ratios.
 */
static uchar run_param(BufData *buf, uint buflen, ParameterValit *vi,
                       float *state, const BufData *modbuf) {
  uint i, end, len, filllen;
  double coeff;
  float s0 = *state;
  if (!vi) {
    filllen = buflen;
    goto FILL;
  }
  coeff = 1.f / vi->time;
  len = vi->time - vi->pos;
  if (len > buflen) {
    len = buflen;
    filllen = 0;
  } else {
    filllen = buflen - len;
  }
  switch (vi->type) {
  case SGS_VALIT_LIN:
    for (i = vi->pos, end = i + len; i < end; ++i) {
      (*buf++).f = s0 + (vi->goal - s0) * (i * coeff);
    }
    break;
  case SGS_VALIT_EXP:
    for (i = vi->pos, end = i + len; i < end; ++i) {
      double mod = 1.f - i * coeff,
             modp2 = mod * mod,
             modp3 = modp2 * mod;
      mod = modp3 + (modp2 * modp3 - modp2) *
                    (mod * (629.f/1792.f) + modp2 * (1163.f/1792.f));
      (*buf++).f = vi->goal + (s0 - vi->goal) * mod;
    }
    break;
  case SGS_VALIT_LOG:
    for (i = vi->pos, end = i + len; i < end; ++i) {
      double mod = i * coeff,
             modp2 = mod * mod,
             modp3 = modp2 * mod;
      mod = modp3 + (modp2 * modp3 - modp2) *
                    (mod * (629.f/1792.f) + modp2 * (1163.f/1792.f));
      (*buf++).f = s0 + (vi->goal - s0) * mod;
    }
    break;
  }
  if (modbuf) {
    buf -= len;
    for (i = 0; i < len; ++i) {
      (*buf++).f *= (*modbuf++).f;
    }
  }
  vi->pos += len;
  if (vi->time == vi->pos) {
    s0 = *state = vi->goal; /* when reached, valit target becomes new state */
  FILL:
    /*
     * Set the remaining values, if any, using the state.
     */
    if (modbuf) {
      for (i = 0; i < filllen; ++i)
        buf[i].f = s0 * modbuf[i].f;
    } else {
      for (i = 0; i < filllen; ++i)
        buf[i].f = s0;
    }
    return (vi != 0);
  }
  return 0;
}

/*
 * Generate buflen samples for an operator node, recursively visiting
 * its subnodes, if any.
 */
static void run_block(SGSGenerator *o, Buf *bufs, uint buflen,
                      OperatorNode *n, BufData *parentfreq,
                      uchar waveenv, uint acc_ind) {
  uint i, len, zerolen;
  BufData *sbuf, *freq, *freqmod, *pm, *amp;
  Buf *nextbuf = bufs + 1;
  ParameterValit *vi;
  uchar fmodc, pmodc, amodc;
  fmodc = pmodc = amodc = 0;
  if (n->adjcs) {
    fmodc = n->adjcs->fmodc;
    pmodc = n->adjcs->pmodc;
    amodc = n->adjcs->amodc;
  }
  sbuf = *bufs;
  len = buflen;
  /*
   * If silence, zero-fill and delay processing for duration.
   */
  if (n->silence) {
    zerolen = n->silence;
    if (zerolen > len)
      zerolen = len;
    if (!acc_ind) for (i = 0; i < zerolen; ++i)
      sbuf[i].i = 0;
    len -= zerolen;
    if (n->time != SGS_TIME_INF) n->time -= zerolen;
    n->silence -= zerolen;
    if (!len)
      return;
    sbuf += zerolen;
  }
  /*
   * Limit length to time duration of operator.
   */
  zerolen = 0;
  if (n->time < (int)len && n->time != SGS_TIME_INF) {
    zerolen = len - n->time;
    len = n->time;
  }
  /*
   * Handle frequency (alternatively ratio) parameter, including frequency
   * modulation if modulators linked.
   */
  freq = *(nextbuf++);
  if (n->attr & SGS_ATTR_VALITFREQ) {
    vi = &n->valitfreq;
    if (n->attr & SGS_ATTR_VALITFREQRATIO) {
      freqmod = parentfreq;
      if (!(n->attr & SGS_ATTR_FREQRATIO)) {
        n->attr |= SGS_ATTR_FREQRATIO;
        n->freq /= parentfreq[0].f;
      }
    } else {
      freqmod = 0;
      if (n->attr & SGS_ATTR_FREQRATIO) {
        n->attr &= ~SGS_ATTR_FREQRATIO;
        n->freq *= parentfreq[0].f;
      }
    }
  } else {
    vi = 0;
    freqmod = (n->attr & SGS_ATTR_FREQRATIO) ? parentfreq : 0;
  }
  if (run_param(freq, len, vi, &n->freq, freqmod))
    n->attr &= ~(SGS_ATTR_VALITFREQ|SGS_ATTR_VALITFREQRATIO);
  if (fmodc) {
    const int *fmods = n->adjcs->adjcs;
    BufData *fmbuf;
    for (i = 0; i < fmodc; ++i)
      run_block(o, nextbuf, len, &o->operators[fmods[i]], freq, 1, i);
    fmbuf = *nextbuf;
    if (n->attr & SGS_ATTR_FREQRATIO) {
      for (i = 0; i < len; ++i)
        freq[i].f += (n->dynfreq * parentfreq[i].f - freq[i].f) * fmbuf[i].f;
    } else {
      for (i = 0; i < len; ++i)
        freq[i].f += (n->dynfreq - freq[i].f) * fmbuf[i].f;
    }
  }
  /*
   * If phase modulators linked, get phase offsets for modulation.
   */
  pm = 0;
  if (pmodc) {
    const int *pmods = &n->adjcs->adjcs[fmodc];
    for (i = 0; i < pmodc; ++i)
      run_block(o, nextbuf, len, &o->operators[pmods[i]], freq, 0, i);
    pm = *(nextbuf++);
  }
  if (!waveenv) {
    /*
     * Handle amplitude parameter, including amplitude modulation if
     * modulators linked.
     */
    if (amodc) {
      const int *amods = &n->adjcs->adjcs[fmodc+pmodc];
      float dynampdiff = n->dynamp - n->amp;
      for (i = 0; i < amodc; ++i)
        run_block(o, nextbuf, len, &o->operators[amods[i]], freq, 1, i);
      amp = *(nextbuf++);
      for (i = 0; i < len; ++i)
        amp[i].f = n->amp + amp[i].f * dynampdiff;
    } else {
      amp = *(nextbuf++);
      vi = (n->attr & SGS_ATTR_VALITAMP) ? &n->valitamp : 0;
      if (run_param(amp, len, vi, &n->amp, 0))
        n->attr &= ~SGS_ATTR_VALITAMP;
    }
    /*
     * Generate integer output - either for voice output or phase modulation
     * input.
     */
    for (i = 0; i < len; ++i) {
      int s, spm = 0;
      float sfreq = freq[i].f, samp = amp[i].f;
      if (pm)
        spm = pm[i].i;
      SGSOsc_RUN_PM(&n->osc, n->osctype, o->osc_coeff, sfreq, spm, samp, s);
      if (acc_ind)
        s += sbuf[i].i;
      sbuf[i].i = s;
    }
  } else {
    /*
     * Generate float output - used as waveform envelopes for modulating
     * frequency or amplitude.
     */
    for (i = 0; i < len; ++i) {
      float s, sfreq = freq[i].f;
      int spm = 0;
      if (pm)
        spm = pm[i].i;
      SGSOsc_RUN_PM_ENVO(&n->osc, n->osctype, o->osc_coeff, sfreq, spm, s);
      if (acc_ind)
        s *= sbuf[i].f;
      sbuf[i].f = s;
    }
  }
  /*
   * Update time duration left, zero rest of buffer if unfilled.
   */
  if (n->time != SGS_TIME_INF) {
    if (!acc_ind && zerolen > 0) {
      sbuf += len;
      for (i = 0; i < zerolen; ++i)
        sbuf[i].i = 0;
    }
    n->time -= len;
  }
}

/*
 * Generate up to len samples for a voice, these mixed into the
 * interleaved output stereo buffer by simple addition.
 */
static void run_voice(SGSGenerator *o, VoiceNode *vn, short *out, uint len) {
  const int *ops;
  uint i, opc, finished = 1;
  int time = 0;
  short *sp;
  if (!vn->graph)
    goto RETURN;
  opc = vn->graph->opc;
  ops = vn->graph->ops;
  time = len;
  for (i = 0; i < opc; ++i) {
    OperatorNode *n = &o->operators[ops[i]];
    if (n->time == 0)
      continue;
    if (time > n->time && n->time != SGS_TIME_INF)
      time = n->time;
  }
  vn->pos += time;
  /*
   * Repeatedly generate up to BUF_LEN samples until done.
   */
  sp = out;
  while (time) {
    uint acc_ind = 0;
    len = (time < BUF_LEN) ? time : BUF_LEN;
    time -= len;
    for (i = 0; i < opc; ++i) {
      OperatorNode *n = &o->operators[ops[i]];
      if (n->time == 0) continue;
      run_block(o, o->bufs, len, n, 0, 0, acc_ind++);
    }
    if (vn->attr & SGS_ATTR_VALITPANNING) {
      BufData *buf = o->bufs[1];
      if (run_param(buf, len, &vn->valitpanning, &vn->panning, 0))
        vn->attr &= ~SGS_ATTR_VALITPANNING;
      for (i = 0; i < len; ++i) {
        int s = (*o->bufs)[i].i, p;
        SET_I2FV(p, ((float)s) * buf[i].f);
        *sp++ += s - p;
        *sp++ += p;
      }
    } else {
      for (i = 0; i < len; ++i) {
        int s = (*o->bufs)[i].i, p;
        SET_I2FV(p, ((float)s) * vn->panning);
        *sp++ += s - p;
        *sp++ += p;
      }
    }
  }
  for (i = 0; i < opc; ++i) {
    OperatorNode *n = &o->operators[ops[i]];
    if (n->time != 0) {
      finished = 0;
      break;
    }
  }
RETURN:
  if (finished)
    vn->flag &= ~SGS_FLAG_EXEC;
}

/*
 * Main sound generation/processing function. Call repeatedly to fill
 * interleaved stereo buffer buf with len new samples.
 *
 * Returns non-zero until the end of the generated signal has been
 * reached.
 */
uchar SGS_generator_run(SGSGenerator *o, short *buf, uint len) {
  short *sp;
  uint i, skiplen;
  sp = buf;
  for (i = len; i--; sp += 2) {
    sp[0] = 0;
    sp[1] = 0;
  }
PROCESS:
  skiplen = 0;
  while (o->event < o->eventc) {
    EventNode *e = &o->events[o->event];
    if (o->eventpos < e->waittime) {
      uint waittime = e->waittime - o->eventpos;
      if (waittime < len) {
        /*
         * Limit len to waittime, further splitting processing into two
         * blocks; otherwise, voice processing could get ahead of event
         * handling in some cases - which would give undefined results!
         */
        skiplen = len - waittime;
        len = waittime;
      }
      o->eventpos += len;
      break;
    }
    SGS_generator_handle_event(o, e);
    ++o->event;
    o->eventpos = 0;
  }
  for (i = o->voice; i < o->voicec; ++i) {
    VoiceNode *vn = &o->voices[i];
    if (vn->pos < 0) {
      uint waittime = -vn->pos;
      if (waittime >= len) {
        vn->pos += len;
        break; /* end for now; waittimes accumulate across nodes */
      }
      buf += waittime+waittime; /* doubled given stereo interleaving */
      len -= waittime;
      vn->pos = 0;
    }
    if (vn->flag & SGS_FLAG_EXEC)
      run_voice(o, vn, buf, len);
  }
  if (skiplen) {
    buf += len+len; /* doubled given stereo interleaving */
    len = skiplen;
    goto PROCESS;
  }
  /*
   * Advance starting voice and check for end of signal.
   */
  for(;;) {
    VoiceNode *vn;
    if (o->voice == o->voicec)
      return (o->event != o->eventc);
    vn = &o->voices[o->voice];
    if (!(vn->flag & SGS_FLAG_INIT) || vn->flag & SGS_FLAG_EXEC)
      break;
    ++o->voice;
  }
  return 1;
}