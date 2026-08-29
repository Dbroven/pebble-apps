// PULSE — flip runner for Pebble Time 2 (emery)
// v1.1.0: procedural drifting walls, causal audio, beat-pulsed style
#include <pebble.h>

// ---------------------------------------------------------------- config
#define SCREEN_W      200
#define SCREEN_H      228
#define HUD_H         22
#define PY            180

#define BASE_L        26
#define BASE_R        174

#define ANG_PER_PX    340

#define FLIP_MS       280
#define INVULN_MS     320
#define PLAYER_HALF   5

#define SPEED_BASE    90
#define SPEED_MAX     200
#define DIST_RAMP     9000

#define MAX_OBST      12
#define MAX_COINS     8

#define TIER_STEPS    5
static const int TIER_DIST[TIER_STEPS] = {0, 2200, 5000, 8500, 12500};
static const int SPAWN_GAP[TIER_STEPS] = {150, 130, 112, 96, 84};

#define RIFT_NEED     100
#define RIFT_MS       4000

#define COIN_POINTS   25
#define CHARGE_COIN   8
#define CHARGE_PX     150

#define KEY_BEST      0
#define KEY_MUTE      1
#define SPK_VOL       85

// ---------------------------------------------------------------- colors
#define C_BG      GColorOxfordBlue
#define C_LBODY   GColorDukeBlue
#define C_RBODY   GColorIndigo
#define C_OBST    GColorFashionMagenta
#define C_TIP     GColorShockingPink
#define C_COIN    GColorYellow
#define C_TEXT    GColorWhite
#define C_DIM     GColorLightGray

// tier palettes: [left edge, right edge]
static const GColor PAL_L[TIER_STEPS] = {
  GColorCyan, GColorGreen, GColorIcterine, GColorOrange, GColorFolly };
static const GColor PAL_R[TIER_STEPS] = {
  GColorLavenderIndigo, GColorElectricBlue, GColorBrilliantRose,
  GColorFashionMagenta, GColorChromeYellow };

// ---------------------------------------------------------------- state
typedef enum { GS_READY = 0, GS_RUN, GS_DEAD } GameState;

typedef struct {
  bool active;
  bool counted;
  int8_t side;
  int16_t y;
  uint8_t type;       // 0 spike, 1 long, 2 beam, 3 pair->block
} Obst;

typedef struct {
  bool active;
  int8_t side;
  int16_t y;
  uint8_t pulse;
} Coin;

typedef struct { bool active; int16_t x, y; } Streak;
#define MAX_STREAKS 9

typedef struct { bool active; int16_t x, y; } Dot;
#define MAX_DOTS 10

static Window *s_win;
static Layer *s_field;

static GameState s_gs;
static int s_side;
static int16_t s_px;
static uint16_t s_invuln;

static int32_t s_dist;
static int s_score;
static int s_best;
static int s_coins_got;
static int8_t s_charge;
static uint16_t s_rift_ms;
static int8_t s_tier;

static Obst s_obst[MAX_OBST];
static Streak s_stk[MAX_STREAKS];
static int16_t s_stk_cnt;
static Dot s_dot[MAX_DOTS];
static int16_t s_dot_cnt;

// character animation
static GPath *p_plyr;
static GPoint s_pp[4];
static int16_t s_flip_t;        // ms left in current flip transit
static uint16_t s_anim_t;       // global animation clock
typedef struct { bool active; int16_t x, y; int8_t vx, vy; } Shard;
#define MAX_SHARDS 8
static Shard s_shard[MAX_SHARDS];
static int16_t s_last_moved;
static Coin s_coins[MAX_COINS];
static int16_t s_spawn_cnt;
static int16_t s_coin_cnt;
static int8_t s_last_block_side;

// --- procedural walls: angular cave sections + smooth-wave bonus blend ---
// Faithful port of original race game: piecewise-linear jittered sections
// (90-320px, offset +/-9..35) + slow macro sine; rift blends toward smooth
// sine "mushroom tunnel" via s_wave_amp 0..12 (original: waveAmp bonus).
typedef struct { int32_t start; int16_t h; int16_t off; } Sec;
#define SECS_MAX 28
static Sec s_secs[2][SECS_MAX];       // [0]=left [1]=right
static int s_sec_n[2];
static int s_sec_cnt[2];              // absolute generated count per side
static int8_t s_wave_amp;             // 0..12 smooth-blend charge

// --- beat / style ---
static uint16_t s_beat_ms;
static int16_t s_trail_x[8];
static uint8_t s_trail_n;
static int8_t s_combo;
static int32_t s_wave_off;         // wall pattern scroll
static uint16_t s_dead_ms;
static uint16_t s_ready_pulse;
static uint16_t s_flash_ms;
static char s_flash[24];
static AppTimer *s_gt;

static bool s_mute;

// ---------------------------------------------------------------- utils
static int16_t clampi(int16_t v, int16_t lo, int16_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static uint16_t quarter_ms(void) {
  int bpm = 128 + (s_tier < 0 ? 0 : (s_tier > 4 ? 4 : s_tier)) * 5;
  return 60000 / bpm;
}

#define OFFS (1 << 26)   // keeps hashed angles positive

static void sec_push(int sd, int32_t start, int16_t h, int16_t off) {
  if (s_sec_n[sd] >= SECS_MAX) {
    for (int i = 1; i < SECS_MAX; i++) s_secs[sd][i-1] = s_secs[sd][i];
    s_sec_n[sd]--;
  }
  s_secs[sd][s_sec_n[sd]++] = (Sec){start, h, off};
}

static void sec_gen(int sd) {
  int i = s_sec_cnt[sd]++;
  uint32_t phase = sd ? 7919u : 0u;
  uint32_t r1 = (i * 2654435761u + phase) % 1000u;
  int16_t h = 90 + (int16_t)(r1 * 230 / 1000);            // 90..320 px
  uint32_t r2 = (i * 1103515245u + phase) % 1000u;
  int jit = 9 + (int)(r2 * 26 / 1000);                     // 9..35
  uint32_t r3 = (i * 1664525u + phase) % 1000u;
  int16_t off = (int16_t)(((int)r3 - 500) * 2 * jit / 1000);
  int32_t start = s_sec_n[sd] ? s_secs[sd][s_sec_n[sd]-1].start
                              + s_secs[sd][s_sec_n[sd]-1].h : -400;
  sec_push(sd, start, h, off);
}

static void secs_ensure(int sd, int32_t worldY) {
  while (s_sec_n[sd] > 4 &&
         s_secs[sd][0].start + s_secs[sd][0].h < worldY - 300) {
    for (int i = 1; i < s_sec_n[sd]; i++) s_secs[sd][i-1] = s_secs[sd][i];
    s_sec_n[sd]--;
  }
  while (!s_sec_n[sd] ||
         s_secs[sd][s_sec_n[sd]-1].start + s_secs[sd][s_sec_n[sd]-1].h
             < worldY + SCREEN_H + 80) {
    sec_gen(sd);
  }
}

// angular cave: linear interp inside section + slow macro sine (per side)
static int32_t cave_off(int sd, int16_t y) {
  int32_t wy = y + s_wave_off;
  secs_ensure(sd, wy);
  int k = 0;
  while (k < s_sec_n[sd] - 1 &&
         s_secs[sd][k].start + s_secs[sd][k].h <= wy) k++;
  Sec *a = &s_secs[sd][k];
  Sec *b = &s_secs[sd][(k + 1 < s_sec_n[sd]) ? k + 1 : k];
  int32_t ah = a->h > 0 ? a->h : 1;
  int32_t local = a->off + (b->off - a->off) * (wy - a->start) / ah;
  int32_t mph = sd ? -19752 : 7276;                        // -1.9 / 0.7 rad
  uint32_t ma = (uint32_t)(wy * 19 + mph + OFFS) % TRIG_MAX_ANGLE;
  int32_t macro = (int32_t)sin_lookup(ma) * 13 / TRIG_MAX_RATIO;
  return -(local + macro);
}

// combined: cave -> smooth sine as waveAmp charges (original wallOffset)
static int32_t wall_off(int sd, int16_t y) {
  int32_t cave = cave_off(sd, y);
  if (s_wave_amp == 0) return cave;
  int32_t wy = y + s_wave_off;
  uint32_t wa = (uint32_t)(wy * 83 + (sd ? 16384 : 0) + OFFS) % TRIG_MAX_ANGLE;
  int32_t samp = 13 + s_wave_amp * 9 / 20;
  int32_t smooth = (int32_t)sin_lookup(wa) * samp / TRIG_MAX_RATIO;
  int32_t bl = s_wave_amp * 256 / 12;
  return (cave * (256 - bl) + smooth * bl) / 256;
}

#define L_MIN 4
#define L_MAX 70
#define R_MIN 130
#define R_MAX 196
static int16_t wall_l(int16_t y) {
  int16_t e = BASE_L + (int16_t)wall_off(0, y);
  return e < L_MIN ? L_MIN : (e > L_MAX ? L_MAX : e);
}
static int16_t wall_r(int16_t y) {
  int16_t e = BASE_R + (int16_t)wall_off(1, y);
  return e < R_MIN ? R_MIN : (e > R_MAX ? R_MAX : e);
}

static void walls_init(void) {
  s_sec_n[0] = s_sec_n[1] = 0;
  s_sec_cnt[0] = s_sec_cnt[1] = 0;
  s_wave_amp = 0;
  secs_ensure(0, 0);
  secs_ensure(1, 0);
}

// ---------------------------------------------------------------- audio
// music removed in v1.8: SFX-only soundscape over the reactive grid

static void note_set(SpeakerNote *n, uint8_t midi, uint8_t wave,
                     uint16_t dur, uint8_t vel) {
  n->midi_note = midi;
  n->waveform = wave;
  n->duration_ms = dur;
  n->velocity = vel;
  n->reserved = 0;
}

static void sfx_flip(bool to_right) {
  if (!s_mute)
    speaker_play_tone(to_right ? 660 : 392, 45, 70, SpeakerWaveformSquare);
}

// causal: coin blip pitch climbs with combo (octave over 12 coins)
static void sfx_coin(void) {
  if (s_mute) return;
  int semi = s_combo > 12 ? 12 : s_combo;
  uint16_t f = 700 + semi * 55;
  speaker_play_tone((uint16_t)f, 35, 80, SpeakerWaveformSquare);
}

// causal: heavy obstacle whooshes past
static void sfx_pass(void) {
  if (!s_mute) speaker_play_tone(150, 25, 40, SpeakerWaveformTriangle);
}

static void sfx_death(void) {
  if (s_mute) return;
  speaker_stop();
  SpeakerNote nj[3];
  note_set(&nj[0], 69, SpeakerWaveformSquare, 140, 100);
  note_set(&nj[1], 65, SpeakerWaveformSquare, 140, 100);
  note_set(&nj[2], 57, SpeakerWaveformSquare, 320, 110);
  speaker_play_notes(nj, 3, SPK_VOL);
}

static void sfx_rift(void) {
  if (s_mute) return;
  SpeakerNote nr[5];
  note_set(&nr[0], 69, SpeakerWaveformSquare, 60, 95);
  note_set(&nr[1], 72, SpeakerWaveformSquare, 60, 95);
  note_set(&nr[2], 76, SpeakerWaveformSquare, 60, 95);
  note_set(&nr[3], 81, SpeakerWaveformSquare, 60, 100);
  note_set(&nr[4], 88, SpeakerWaveformSquare, 180, 105);
  speaker_play_notes(nr, 5, SPK_VOL);
}

// ---------------------------------------------------------------- game
static int tier_for_dist(int32_t d) {
  int t = 0;
  for (int i = TIER_STEPS - 1; i >= 0; i--)
    if (d >= TIER_DIST[i]) { t = i; break; }
  return t;
}

static int16_t coin_x(int8_t side, int16_t y) {
  return side < 0 ? wall_l(y) + 20 : wall_r(y) - 20;
}
static int16_t obst_x0(const Obst *o, int len, int depth) {
  int16_t e = o->side < 0 ? wall_l(o->y + len / 2) : wall_r(o->y + len / 2);
  return o->side < 0 ? e : e - depth;
}

static void obst_add(int8_t side, int16_t y, uint8_t type) {
  for (int i = 0; i < MAX_OBST; i++) {
    if (!s_obst[i].active) {
      s_obst[i] = (Obst){true, false, side, y, type};
      return;
    }
  }
}

static void spawn_obstacle(void) {
  int8_t tier = s_tier;
  uint8_t types[4], nt = 0;
  types[nt++] = 0;
  if (tier >= 1) types[nt++] = 1;
  if (tier >= 2) types[nt++] = 2;
  if (tier >= 3) types[nt++] = 3;
  uint8_t type = types[rand() % nt];

  int8_t side = (rand() % 2) ? 1 : -1;
  if (type == 2 || type == 3) {
    side = -s_last_block_side;
    s_last_block_side = side;
  }
  if (type == 3) {
    obst_add(side, -40, 0);
    obst_add(-side, -96, 1);
    return;
  }
  obst_add(side, -40, type);
}

static void spawn_coins(void) {
  int8_t side = (rand() % 2) ? 1 : -1;
  int cnt = 3 + rand() % 3;
  for (int i = 0; i < cnt; i++) {
    for (int j = 0; j < MAX_COINS; j++) {
      if (!s_coins[j].active) {
        s_coins[j] = (Coin){true, side, (int16_t)(-20 - i * 24), (uint8_t)(i * 30)};
        break;
      }
    }
  }
}

static void clear_obstacles(void) {
  for (int i = 0; i < MAX_OBST; i++) s_obst[i].active = false;
}

static void start_run(void) {
  s_gs = GS_RUN;
  s_side = -1;
  s_px = BASE_L + 13;
  s_invuln = 0;
  s_dist = 0;
  s_wave_off = 0;
  s_score = 0;
  s_coins_got = 0;
  s_charge = 0;
  s_rift_ms = 0;
  s_tier = 0;
  s_spawn_cnt = 260;
  s_coin_cnt = 170;
  s_last_block_side = -1;
  s_dead_ms = 0;
  s_combo = 0;
  s_beat_ms = 0;
  s_trail_n = 0;
  s_flip_t = 0;
  for (int i = 0; i < MAX_SHARDS; i++) s_shard[i].active = false;
  clear_obstacles();
  for (int i = 0; i < MAX_COINS; i++) s_coins[i].active = false;
  for (int i = 0; i < MAX_STREAKS; i++) s_stk[i].active = false;
  s_stk_cnt = 60;
  for (int i = 0; i < MAX_DOTS; i++) s_dot[i].active = false;
  s_dot_cnt = 30;
  walls_init();
}

static void die(void) {
  s_gs = GS_DEAD;
  s_dead_ms = 0;
  for (int i = 0; i < MAX_SHARDS; i++) {
    uint32_t ang = (uint32_t)(i * TRIG_MAX_ANGLE / 8 + 4000) % TRIG_MAX_ANGLE;
    int spd = 40 + rand() % 70;
    s_shard[i] = (Shard){true, s_px, PY,
        (int8_t)(sin_lookup(ang) * spd / TRIG_MAX_RATIO),
        (int8_t)(cos_lookup(ang) * spd / TRIG_MAX_RATIO)};
  }
  sfx_death();
  vibes_long_pulse();
  if (s_score > s_best) {
    s_best = s_score;
    persist_write_int(KEY_BEST, s_best);
  }
}

static void do_flip(void) {
  bool to_right = s_side < 0;
  s_side = -s_side;
  s_flip_t = FLIP_MS;
  s_invuln = INVULN_MS;
  static const uint32_t seq[] = {25};               // 25ms micro buzz
  const VibePattern p = {.durations = seq, .num_segments = 1};
  vibes_enqueue_custom_pattern(p);
  sfx_flip(to_right);
}

static void update(int ms) {
  s_ready_pulse += ms;
  s_anim_t += ms;
  if (s_flip_t > 0) s_flip_t = (s_flip_t > ms) ? s_flip_t - ms : 0;
  if (s_flash_ms > 0) s_flash_ms -= ms;

  if (s_gs != GS_RUN) {
    if (s_gs == GS_DEAD) {
      s_dead_ms += ms;
      for (int i = 0; i < MAX_SHARDS; i++) {
        if (!s_shard[i].active) continue;
        s_shard[i].x += (int16_t)(s_shard[i].vx * ms / 1000);
        s_shard[i].y += (int16_t)(s_shard[i].vy * ms / 1000);
        if (s_dead_ms > 600 ||
            s_shard[i].x < -8 || s_shard[i].x > SCREEN_W + 8 ||
            s_shard[i].y < -8 || s_shard[i].y > SCREEN_H + 8)
          s_shard[i].active = false;
      }
    }
    return;
  }

  s_tier = tier_for_dist(s_dist);

  bool rift = s_rift_ms > 0;
  if (rift) {
    s_rift_ms -= ms;
    s_score += 2;
  }

  int32_t speed = SPEED_BASE + (int32_t)(DIST_RAMP < s_dist ? DIST_RAMP : s_dist)
                  * SPEED_BASE / DIST_RAMP;
  if (rift) speed = speed * 5 / 4;
  int16_t moved = (int16_t)(speed * ms / 1000);
  if (moved < 1) moved = 1;

  s_dist += moved;
  s_wave_off += moved;
  s_score += moved / 9;

  // beat clock for visual pulse
  uint16_t qm = quarter_ms();
  s_beat_ms += ms;
  while (s_beat_ms >= qm) s_beat_ms -= qm;

  if (s_wave_amp < (rift ? 12 : 0)) s_wave_amp++;
  else if (s_wave_amp > (rift ? 12 : 0)) s_wave_amp--;
  s_last_moved = moved;

  // speed streaks: fly with the world, stretch with speed at draw time
  s_stk_cnt -= moved;
  if (s_stk_cnt <= 0) {
    s_stk_cnt = 30 + rand() % 36;
    for (int j = 0; j < MAX_STREAKS; j++) {
      if (!s_stk[j].active) {
        int16_t el = wall_l(-6), er = wall_r(-6);
        int16_t span = er - el - 12;
        s_stk[j] = (Streak){true,
                            (int16_t)(el + 6 + (span > 0 ? rand() % span : 0)),
                            -10};
        break;
      }
    }
  }
  for (int j = 0; j < MAX_STREAKS; j++) {
    if (!s_stk[j].active) continue;
    s_stk[j].y += moved;
    if (s_stk[j].y > SCREEN_H + 12) s_stk[j].active = false;
  }

  // parallax depth dots: half speed -> background feels far away
  s_dot_cnt -= moved;
  if (s_dot_cnt <= 0) {
    s_dot_cnt = 40 + rand() % 44;
    for (int j = 0; j < MAX_DOTS; j++) {
      if (!s_dot[j].active) {
        int16_t el = wall_l(-4), er = wall_r(-4);
        int16_t span = er - el - 10;
        s_dot[j] = (Dot){true,
                         (int16_t)(el + 5 + (span > 0 ? rand() % span : 0)),
                         -6};
        break;
      }
    }
  }
  for (int j = 0; j < MAX_DOTS; j++) {
    if (!s_dot[j].active) continue;
    s_dot[j].y += (moved + 1) / 2;
    if (s_dot[j].y > SCREEN_H + 6) s_dot[j].active = false;
  }

  if (!rift) {
    static int16_t charge_px = 0;
    charge_px += moved;
    if (charge_px >= CHARGE_PX) {
      charge_px -= CHARGE_PX;
      if (s_charge < RIFT_NEED) s_charge++;
    }
  }

  for (int i = 0; i < MAX_OBST; i++) {
    Obst *o = &s_obst[i];
    if (!o->active) continue;
    int len = (o->type == 1) ? 30 : (o->type == 3) ? 40 : 14;
    if (!o->counted && o->y + len >= PY - PLAYER_HALF) {
      o->counted = true;
      if (o->type >= 2) sfx_pass();          // causal: heavy flyby
    }
    o->y += moved;
    if (o->y > SCREEN_H + 44) o->active = false;
  }
  for (int i = 0; i < MAX_COINS; i++) {
    if (!s_coins[i].active) continue;
    s_coins[i].y += moved;
    s_coins[i].pulse += ms;
    if (s_coins[i].y > SCREEN_H + 20) s_coins[i].active = false;
  }

  s_spawn_cnt -= moved;
  if (s_spawn_cnt <= 0) {
    spawn_obstacle();
    s_spawn_cnt = SPAWN_GAP[s_tier] + rand() % 30;
  }
  s_coin_cnt -= moved;
  if (s_coin_cnt <= 0) {
    spawn_coins();
    s_coin_cnt = 130 + rand() % 90;
  }

  if (s_invuln > 0) s_invuln -= ms;
  int16_t target = s_side < 0 ? wall_l(PY) + 13 : wall_r(PY) - 13;
  int16_t step = (int16_t)((SCREEN_W / 2) * ms / FLIP_MS);
  if (step < 1) step = 1;
  if (s_px < target) s_px = clampi(s_px + step, s_px, target);
  else if (s_px > target) s_px = clampi(s_px - step, target, s_px);

  // trail ring
  s_trail_x[s_trail_n % 8] = s_px;
  s_trail_n++;

  if (s_invuln == 0) {
    int16_t px0 = s_px - PLAYER_HALF, px1 = s_px + PLAYER_HALF;
    int16_t py0 = PY - PLAYER_HALF, py1 = PY + PLAYER_HALF;
    for (int i = 0; i < MAX_OBST; i++) {
      if (!s_obst[i].active) continue;
      Obst *o = &s_obst[i];
      int len = (o->type == 1) ? 30 : (o->type == 3) ? 40 : 14;
      int depth = (o->type >= 2) ? (o->type == 3 ? 52 : 36) : 14;
      int16_t ox0 = obst_x0(o, len, depth);
      int16_t ox1 = ox0 + depth;
      if (px1 > ox0 + 1 && px0 < ox1 - 1 && py1 > o->y && py0 < o->y + len) {
        die();
        return;
      }
    }
  }

  for (int i = 0; i < MAX_COINS; i++) {
    if (!s_coins[i].active) continue;
    Coin *c = &s_coins[i];
    int16_t cx = coin_x(c->side, c->y);
    if (abs(s_px - cx) < 12 && abs(PY - c->y) < 11) {
      c->active = false;
      s_score += COIN_POINTS;
      s_coins_got++;
      if (s_combo < 20) s_combo++;
      if (s_charge < RIFT_NEED) s_charge += CHARGE_COIN;
      sfx_coin();
    }
  }

  if (!rift && s_charge >= RIFT_NEED) {
    s_charge = 0;
    s_combo = 0;
    s_rift_ms = RIFT_MS;
    vibes_double_pulse();
    sfx_rift();
  }
}

// ---------------------------------------------------------------- drawing
static GPath *p_diam, *p_spike_l, *p_spike_r;
static GPath *p_lwall, *p_rwall;
static GPoint s_lpts[22], s_rpts[22];   // mutable wall polygons
#define WSTEP 12
#define GRID_G 28
static const GPoint DIAMOND_PTS[4] = {{0,-8},{6,0},{0,8},{-6,0}};
static const GPoint SPIKE_L_PTS[3] = {{0,-7},{14,0},{0,7}};
static const GPoint SPIKE_R_PTS[3] = {{0,-7},{-14,0},{0,7}};

static void draw_walls(GContext *ctx) {
  bool rift = s_rift_ms > 0;
  int t = s_tier < 0 ? 0 : (s_tier > 4 ? 4 : s_tier);
  GColor lc = rift ? PAL_R[t] : PAL_L[t];
  GColor rc = rift ? PAL_L[t] : PAL_R[t];
  uint16_t qm = quarter_ms();
  int16_t pulse = 3 - (int16_t)((uint32_t)s_beat_ms * 2 / qm);   // 3..1

  // sample curves into polygons
  s_lpts[0] = GPoint(0, 0);
  s_rpts[0] = GPoint(SCREEN_W - 1, 0);
  for (int i = 0; i <= 19; i++) {
    int16_t y = (i == 19) ? SCREEN_H - 1 : i * WSTEP;
    int16_t el = wall_l(y);
    int16_t er = wall_r(y);
    s_lpts[i + 1] = GPoint(el, y);
    s_rpts[i + 1] = GPoint(er, y);
    if (i == 18) { s_lpts[20] = GPoint(el, SCREEN_H - 1); s_rpts[20] = GPoint(er, SCREEN_H - 1); }
  }
  s_lpts[21] = GPoint(0, SCREEN_H - 1);
  s_rpts[21] = GPoint(SCREEN_W - 1, SCREEN_H - 1);

  graphics_context_set_fill_color(ctx, C_LBODY);
  gpath_draw_filled(ctx, p_lwall);
  graphics_context_set_fill_color(ctx, C_RBODY);
  gpath_draw_filled(ctx, p_rwall);

  // green mesh ON the wall bodies, locked to world scroll
  graphics_context_set_stroke_color(ctx,
      rift ? GColorBrightGreen : GColorGreen);
  for (int16_t my = (int16_t)(s_wave_off % GRID_G) - GRID_G;
       my < SCREEN_H; my += GRID_G) {
    if (my < 0) continue;
    graphics_draw_line(ctx, GPoint(0, my), GPoint(wall_l(my), my));
    graphics_draw_line(ctx, GPoint(wall_r(my), my), GPoint(SCREEN_W - 1, my));
  }
  // inner contour parallel to each edge -> panel bands
  for (int i = 0; i < 19; i++) {
    GPoint a = s_lpts[i + 1], b = s_lpts[i + 2];
    graphics_draw_line(ctx, GPoint(a.x + 7, a.y), GPoint(b.x + 7, b.y));
    a = s_rpts[i + 1];
    b = s_rpts[i + 2];
    graphics_draw_line(ctx, GPoint(a.x - 7, a.y), GPoint(b.x - 7, b.y));
  }

  // vector edge lines (pulse width via parallel pass)
  graphics_context_set_stroke_color(ctx, lc);
  for (int i = 0; i < 19; i++) {
    GPoint a = s_lpts[i + 1], b = s_lpts[i + 2];
    graphics_draw_line(ctx, a, b);
    if (pulse >= 2 && a.x > 1)
      graphics_draw_line(ctx, GPoint(a.x - 1, a.y), GPoint(b.x - 1, b.y));
    if (pulse >= 3 && a.x > 2)
      graphics_draw_line(ctx, GPoint(a.x + 1, a.y), GPoint(b.x + 1, b.y));
  }
  graphics_context_set_stroke_color(ctx, rc);
  for (int i = 0; i < 19; i++) {
    GPoint a = s_rpts[i + 1], b = s_rpts[i + 2];
    graphics_draw_line(ctx, a, b);
    if (pulse >= 2 && a.x < SCREEN_W - 2)
      graphics_draw_line(ctx, GPoint(a.x + 1, a.y), GPoint(b.x + 1, b.y));
    if (pulse >= 3 && a.x < SCREEN_W - 3)
      graphics_draw_line(ctx, GPoint(a.x - 1, a.y), GPoint(b.x - 1, b.y));
  }
}

static void draw_dots(GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
  for (int j = 0; j < MAX_DOTS; j++) {
    if (!s_dot[j].active) continue;
    graphics_fill_rect(ctx, GRect(s_dot[j].x, s_dot[j].y, 2, 2), 0, GCornerNone);
  }
}

static void draw_streaks(GContext *ctx) {
  int16_t len = 4 + s_last_moved;      // longer streak = faster feel
  graphics_context_set_stroke_color(ctx, GColorDukeBlue);
  for (int j = 0; j < MAX_STREAKS; j++) {
    if (!s_stk[j].active) continue;
    graphics_draw_line(ctx, GPoint(s_stk[j].x, s_stk[j].y),
                       GPoint(s_stk[j].x, s_stk[j].y + len));
  }
}

static void draw_obstacles(GContext *ctx) {
  for (int i = 0; i < MAX_OBST; i++) {
    Obst *o = &s_obst[i];
    if (!o->active || o->y > SCREEN_H) continue;
    int len = (o->type == 1) ? 30 : (o->type == 3) ? 40 : 14;
    int depth = (o->type >= 2) ? (o->type == 3 ? 52 : 36) : 14;
    if (o->type <= 1) {
      gpath_move_to(o->side < 0 ? p_spike_l : p_spike_r,
                    GPoint(o->side < 0 ? wall_l(o->y + 7) : wall_r(o->y + 7),
                           o->y + 7));
      graphics_context_set_fill_color(ctx, C_OBST);
      gpath_draw_filled(ctx, o->side < 0 ? p_spike_l : p_spike_r);
    } else {
      int16_t x0 = o->side < 0 ? BASE_L : BASE_R - depth;
      graphics_context_set_fill_color(ctx, C_OBST);
      x0 = obst_x0(o, len, depth);
      graphics_fill_rect(ctx, GRect(x0, o->y, depth, len), 0, GCornerNone);
      graphics_context_set_fill_color(ctx, C_TIP);
      if (o->side < 0) graphics_fill_rect(ctx, GRect(x0 + depth - 2, o->y, 2, len), 0, GCornerNone);
      else graphics_fill_rect(ctx, GRect(x0, o->y, 2, len), 0, GCornerNone);
    }
  }
}

static void draw_coins(GContext *ctx) {
  for (int i = 0; i < MAX_COINS; i++) {
    Coin *c = &s_coins[i];
    if (!c->active || c->y > SCREEN_H) continue;
    int32_t a = ((uint32_t)c->pulse * 400) % TRIG_MAX_ANGLE;
    int16_t r = 4 + (int16_t)(sin_lookup(a) * 2 / TRIG_MAX_RATIO);
    int16_t cx = coin_x(c->side, c->y);
    graphics_context_set_fill_color(ctx, C_COIN);
    graphics_fill_circle(ctx, GPoint(cx, c->y), r);
    graphics_context_set_fill_color(ctx, C_TEXT);
    graphics_fill_circle(ctx, GPoint(cx, c->y), 1);
  }
}

static void draw_player(GContext *ctx) {
  bool rift = s_rift_ms > 0;
  int t = s_tier < 0 ? 0 : (s_tier > 4 ? 4 : s_tier);
  GColor glow = rift ? PAL_L[t] : GColorCyan;
  bool transit = s_flip_t > 0;

  // trail ghosts
  for (int i = 1; i <= 4 && s_trail_n > 8; i++) {
    uint8_t idx = (uint8_t)((s_trail_n - i * 2) % 8);
    graphics_context_set_stroke_color(ctx,
        transit ? GColorIndigo : GColorDukeBlue);
    graphics_draw_circle(ctx, GPoint(s_trail_x[idx], PY), 7 - i);
  }

  if (s_invuln > 0 && (s_invuln / 80) % 2 == 1) {
    graphics_context_set_fill_color(ctx, glow);
    graphics_fill_circle(ctx, GPoint(s_px, PY), 2);
    return;
  }

  // body: diamond with squash&stretch, spins 90 deg across the flip
  int16_t rx = transit ? 9 : 5;
  int16_t ry = transit ? 5 : 9;
  int32_t th = transit
      ? (int32_t)(FLIP_MS - s_flip_t) * (TRIG_MAX_ANGLE / 4) / FLIP_MS : 0;
  int32_t cs = sin_lookup(TRIG_MAX_ANGLE / 4 - th);
  int32_t sn = sin_lookup(th);
  const int16_t bx[4] = {0, rx, 0, -rx};
  const int16_t by[4] = {-ry, 0, ry, 0};
  for (int i = 0; i < 4; i++) {
    s_pp[i] = GPoint(
        (int16_t)((bx[i] * cs - by[i] * sn) / TRIG_MAX_RATIO),
        (int16_t)((bx[i] * sn + by[i] * cs) / TRIG_MAX_RATIO));
  }
  gpath_move_to(p_plyr, GPoint(s_px, PY));
  graphics_context_set_fill_color(ctx, C_TEXT);
  gpath_draw_filled(ctx, p_plyr);

  // living core: pulsing heart with hover bob
  int32_t pa = ((uint32_t)s_anim_t * 700) % TRIG_MAX_ANGLE;
  int16_t bob = (int16_t)(sin_lookup(pa) / (TRIG_MAX_RATIO / 2));
  int16_t cr = 2 + (int16_t)(sin_lookup(pa * 2) / (TRIG_MAX_RATIO / 1));
  graphics_context_set_fill_color(ctx, glow);
  graphics_fill_circle(ctx, GPoint(s_px, PY + bob), cr);

  if (!transit) {
    // thruster sparks: flicker away from the wall we ride
    graphics_context_set_stroke_color(ctx, GColorElectricBlue);
    int16_t d0 = (s_side < 0) ? 4 : -4;
    for (int k = 0; k < 3; k++) {
      int16_t len = 2 + rand() % 5;
      int16_t x0 = s_px + d0;
      int16_t x1 = s_px + d0 + ((s_side < 0) ? len : -len);
      int16_t yy = PY - 3 + k * 3 + bob;
      graphics_draw_line(ctx, GPoint(x0, yy), GPoint(x1, yy));
    }
    // rift aura: breathing outer ring
    if (rift) {
      int16_t ar = 8 + (int16_t)((s_anim_t / 60) % 8);
      graphics_context_set_stroke_color(ctx, glow);
      graphics_draw_circle(ctx, GPoint(s_px, PY), ar);
    }
  } else {
    // transit speed lines trailing behind the dart
    int16_t tx = (s_side < 0) ? wall_l(PY) + 13 : wall_r(PY) - 13;
    int16_t dm = (tx > s_px) ? 1 : -1;
    graphics_context_set_stroke_color(ctx, glow);
    graphics_draw_line(ctx, GPoint(s_px - dm * 12, PY - 2),
                       GPoint(s_px - dm * 20, PY - 2));
    graphics_draw_line(ctx, GPoint(s_px - dm * 12, PY + 2),
                       GPoint(s_px - dm * 20, PY + 2));
  }
}

static void draw_hud(GContext *ctx) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", s_score);
  graphics_context_set_text_color(ctx, C_TEXT);
  graphics_draw_text(ctx, buf,
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(4, -4, 70, 26), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  snprintf(buf, sizeof(buf), "BEST %d", s_best);
  graphics_draw_text(ctx, buf,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(SCREEN_W - 86, -2, 82, 22), GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);

  bool rift = s_rift_ms > 0;
  int fw = rift ? 80 : (80 * s_charge) / RIFT_NEED;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect((SCREEN_W - 80) / 2, HUD_H - 7, 80, 5), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, rift ? C_TIP : GColorCyan);
  if (fw > 0) graphics_fill_rect(ctx, GRect((SCREEN_W - 80) / 2, HUD_H - 7, fw, 5), 0, GCornerNone);

  if (rift) {
    if ((s_rift_ms / 200) % 2 == 0) {
      graphics_draw_text(ctx, "RIFT",
          fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
          GRect(0, 40, SCREEN_W, 40), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }
  }
  if (s_flash_ms > 0 && s_flash[0]) {
    graphics_draw_text(ctx, s_flash,
        fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(0, 26, SCREEN_W, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void draw_ready(GContext *ctx) {
  graphics_context_set_text_color(ctx, C_TEXT);
  graphics_draw_text(ctx, "PULSE",
      fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
      GRect(0, 52, SCREEN_W, 40), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, (s_ready_pulse / 300) % 2 ? GColorCyan : C_DIM);
  graphics_draw_text(ctx, "~ flip runner ~",
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(0, 92, SCREEN_W, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  char buf[24];
  snprintf(buf, sizeof(buf), "BEST %d", s_best);
  graphics_context_set_text_color(ctx, C_COIN);
  graphics_draw_text(ctx, buf,
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(0, 118, SCREEN_W, 26), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, C_DIM);
  graphics_draw_text(ctx, "TAP = FLIP\nHOLD SEL = SOUND",
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(0, 156, SCREEN_W, 40), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void draw_dead(GContext *ctx) {
  for (int i = 0; i < MAX_SHARDS; i++) {
    if (!s_shard[i].active) continue;
    graphics_context_set_fill_color(ctx,
        (i % 2) ? GColorFashionMagenta : C_TEXT);
    graphics_fill_rect(ctx, GRect(s_shard[i].x, s_shard[i].y, 2, 2),
                       0, GCornerNone);
  }
  if (s_dead_ms < 250) return;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(20, 58, SCREEN_W - 40, 108), 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, GColorCyan);
  graphics_draw_rect(ctx, GRect(20, 58, SCREEN_W - 40, 108));
  graphics_context_set_text_color(ctx, C_OBST);
  graphics_draw_text(ctx, "CRASHED",
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(20, 62, SCREEN_W - 40, 26), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  char buf[32];
  graphics_context_set_text_color(ctx, C_TEXT);
  snprintf(buf, sizeof(buf), "SCORE %d", s_score);
  graphics_draw_text(ctx, buf,
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(20, 92, SCREEN_W - 40, 24), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  snprintf(buf, sizeof(buf), "COINS %d", s_coins_got);
  graphics_draw_text(ctx, buf,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(20, 116, SCREEN_W - 40, 18), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  if (s_score >= s_best && s_score > 0 && (s_dead_ms / 250) % 2 == 0) {
    graphics_context_set_text_color(ctx, C_COIN);
    graphics_draw_text(ctx, "NEW BEST!",
        fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(20, 132, SCREEN_W - 40, 18), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  } else {
    snprintf(buf, sizeof(buf), "BEST %d", s_best);
    graphics_context_set_text_color(ctx, C_DIM);
    graphics_draw_text(ctx, buf,
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(20, 132, SCREEN_W - 40, 18), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
  if (s_dead_ms > 600) {
    graphics_context_set_text_color(ctx, C_DIM);
    graphics_draw_text(ctx, "TAP TO RETRY",
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(20, 146, SCREEN_W - 40, 18), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void draw_handler(Layer *layer, GContext *ctx) {
  (void)layer;
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, SCREEN_H), 0, GCornerNone);
  draw_walls(ctx);
  if (s_gs == GS_RUN) {
    draw_dots(ctx);
    draw_streaks(ctx);
    draw_obstacles(ctx);
    draw_coins(ctx);
    draw_player(ctx);
  } else if (s_gs == GS_READY) {
    int32_t a = ((uint32_t)s_ready_pulse * 200) % TRIG_MAX_ANGLE;
    int16_t dy = (int16_t)(sin_lookup(a) * 6 / TRIG_MAX_RATIO);
    gpath_move_to(p_diam, GPoint(SCREEN_W / 2, 140 + dy));
    graphics_context_set_fill_color(ctx, C_TEXT);
    gpath_draw_filled(ctx, p_diam);
  }
  draw_hud(ctx);
  if (s_gs == GS_READY) draw_ready(ctx);
  if (s_gs == GS_DEAD) draw_dead(ctx);
}

// ---------------------------------------------------------------- input
static void press_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  if (s_gs == GS_READY) start_run();
  else if (s_gs == GS_DEAD) {
    if (s_dead_ms > 500) start_run();
  } else do_flip();
}

static void sel_long_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  s_mute = !s_mute;
  persist_write_int(KEY_MUTE, s_mute ? 1 : 0);
  strcpy(s_flash, s_mute ? "SOUND OFF" : "SOUND ON");
  s_flash_ms = 1200;
  vibes_short_pulse();
  if (s_mute) speaker_stop();
}

static void click_config(void *ctx) {
  (void)ctx;
  window_single_click_subscribe(BUTTON_ID_UP, press_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, press_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, press_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 600, sel_long_handler, NULL);
}

// ---------------------------------------------------------------- touch
static void touch_handler(const TouchEvent *event, void *ctx) {
  (void)ctx;
  if (event->type != TouchEvent_Touchdown) return;
  if (s_gs == GS_READY) start_run();
  else if (s_gs == GS_DEAD) {
    if (s_dead_ms > 500) start_run();
  } else do_flip();
}

// ---------------------------------------------------------------- lifecycle
static void gtick(void *data) {
  (void)data;
  update(40);
  layer_mark_dirty(s_field);
  s_gt = app_timer_register(40, gtick, NULL);
}

static void win_load(Window *win) {
  (void)win;
  s_field = layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  layer_set_update_proc(s_field, draw_handler);
  layer_add_child(window_get_root_layer(s_win), s_field);
  window_set_click_config_provider(s_win, click_config);
}

static void win_unload(Window *win) {
  (void)win;
  layer_destroy(s_field);
}

static void init(void) {
  s_best = persist_exists(KEY_BEST) ? persist_read_int(KEY_BEST) : 0;
  s_mute = persist_exists(KEY_MUTE) ? persist_read_int(KEY_MUTE) != 0 : false;
  s_gs = GS_READY;
  s_px = BASE_L + 13;
  s_side = -1;
  s_flash_ms = 0;
  s_flash[0] = 0;
  clear_obstacles();
  walls_init();

  p_diam = gpath_create(&(GPathInfo){4, (GPoint *)DIAMOND_PTS});
  p_spike_l = gpath_create(&(GPathInfo){3, (GPoint *)SPIKE_L_PTS});
  p_spike_r = gpath_create(&(GPathInfo){3, (GPoint *)SPIKE_R_PTS});
  // wall polygons: corner, 20 edge samples (y=0..228), corner
  for (int i = 0; i < 22; i++) { s_lpts[i] = GPoint(0,0); s_rpts[i] = GPoint(0,0); }
  p_lwall = gpath_create(&(GPathInfo){22, s_lpts});
  p_rwall = gpath_create(&(GPathInfo){22, s_rpts});
  for (int i = 0; i < 4; i++) s_pp[i] = GPoint(0, 0);
  p_plyr = gpath_create(&(GPathInfo){4, s_pp});

  touch_service_subscribe(touch_handler, NULL);
  s_win = window_create();
  window_set_background_color(s_win, C_BG);
  window_set_window_handlers(s_win, (WindowHandlers){
    .load = win_load, .unload = win_unload
  });
  window_stack_push(s_win, true);
  s_gt = app_timer_register(40, gtick, NULL);
}

static void deinit(void) {
  app_timer_cancel(s_gt);
  speaker_stop();
  touch_service_unsubscribe();
  gpath_destroy(p_diam);
  gpath_destroy(p_spike_l);
  gpath_destroy(p_spike_r);
  gpath_destroy(p_lwall);
  gpath_destroy(p_rwall);
  gpath_destroy(p_plyr);
  window_destroy(s_win);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
