/*
 * NEON RUN: LANES — Subway-Surfers-style runner for Pebble Time 2 (emery)
 * by ClawBro for Daniel, 2026
 *
 * 3 lanes, pseudo-3D perspective road, neon trains, jump barriers,
 * hover drones, crystal trails. UP/DOWN = switch lane, SELECT = jump.
 */

#include <pebble.h>
#include <stdlib.h>

// ---------------------------------------------------------------- config

#define SCREEN_W 200
#define SCREEN_H 228

#define VANISH_X 100
#define VANISH_Y 96
#define BASE_Y   204        // player feet line (t=1)
#define CAM_D    42         // perspective constant: t = CAM_D/(z+CAM_D)

#define LANE_OFF 58         // lane x offset at t=1
#define EDGE_OFF 92         // road half-width at t=1

#define PLAYER_H 28
#define JUMP_V   (-235)
#define JUMP2_V  (-205)
#define LAUNCH_V (-215)     // ramp auto-launch toward roof
#define GRAVITY  750
#define TERM_VEL 520
#define FASTFALL_V 420      // DOWN in the air: slam toward the ground
#define SLIDE_MS 650        // slide duration

#define RUN_MS 50
#define BASE_SPEED 80      // calm start (95 felt fast on real watch)
#define SPEED_RAMP 3.8      // per 100 score (steeper ramp, old base reached ~score 900)
#define SPEED_MAX 265

#define SPAWN_Z 300
#define TRAIN_LEN 30
#define TRAIN_H 56          // train roof height (px at t=1)
#define RAMP_LEN 22         // ramp wedge length in world z
#define RAMP_RIDE_H 34      // ramp top height (px at t=1)

#define SWITCH_CD 140       // ms between lane switches

#define GODMODE 0   // test-only: disable collisions
#define RAMPTEST 0  // test-only: deterministic ramp as first pattern
#define SLIDETEST 0 // test-only: auto-start + scripted drone & slide

static GRect s_screen;

// ---------------------------------------------------------------- state

typedef enum { GS_READY = 0, GS_RUN, GS_DEAD } GameState;

typedef struct {
  bool active;
  int8_t lane;
  int16_t z;
  int8_t kind;        // 0 train, 1 barrier, 2 drone, 3 ramp, 4 landable train
  int16_t phase;      // drone anim; kind 4: extra length beyond TRAIN_LEN
} Ent;

typedef struct {
  bool active;
  int8_t lane;
  int16_t z;
  int8_t h;           // height above road (px at t=1)
} Gem;

#define MAX_ENT 10
#define MAX_GEMS 20
#define STARS_N 16

static Window *s_win;
static Window *s_shadow;      // stays under game window: enables BACK-as-left
static bool s_exit_app = false;
static Layer *s_field;
static GameState s_gs;

static int16_t s_star_x[STARS_N], s_star_y[STARS_N];
static bool s_star_tw[STARS_N];

static int32_t s_dist;
static int32_t s_score;
static int32_t s_best;

static Ent s_ents[MAX_ENT];
static Gem s_gems[MAX_GEMS];
static int32_t s_next_spawn_z;     // absolute world-z where next pattern spawns

static int8_t s_lane;              // logical lane -1..1
static int16_t s_px;               // visual player x
static int16_t s_py;               // 0 = ground, negative = up
static int16_t s_vy;
static bool s_jumping;
static int s_jumps;
static bool s_sliding;             // sliding under drones
static int16_t s_slide_left;       // ms remaining in slide
static bool s_slide_queued;        // fast-fall: start slide on touchdown
static int8_t s_riding;           // ramp entity index while riding, -1 none
static int s_roof_grace;          // ms of train-collision immunity after launch
static int s_switch_cd;
static int s_run_ms;
static int s_dead_ms;
static int s_ready_pulse;
static int s_new_best_flash;
static int32_t s_gem_streak;
static int32_t s_streak_timer;

// ---------------------------------------------------------------- colors

#define C_TEXT    GColorWhite
#define C_DIM      GColorLightGray
#define C_CYAN     GColorCyan
#define C_MAGENTA  GColorFashionMagenta
#define C_PINK     GColorShockingPink
#define C_GEM      GColorGreen
#define C_HOOD     GColorWhite
#define C_TOP      GColorOrange
#define C_PACK     GColorIcterine
#define C_LIMB     GColorLightGray

// ---------------------------------------------------------------- gpaths

static const GPoint GEM_PTS[4] = {{0,-5},{4,0},{0,5},{-4,0}};

static GPath *s_path;
static GPathInfo s_pi;
static GPath *s_gem_path;   // pre-allocated gem shape: hottest draw path

static void path_set(const GPoint *pts, int n, int dx, int dy) {
  s_pi.num_points = n;
  s_pi.points = (GPoint *)pts;
  if (s_path) gpath_destroy(s_path);
  s_path = gpath_create(&s_pi);
  gpath_move_to(s_path, GPoint(dx, dy));
}

// ---------------------------------------------------------------- helpers

static int16_t ent_len(Ent *e) {
  return TRAIN_LEN + (e->kind == 4 ? e->phase : 0);
}

static int32_t speed_now(void) {
  // difficulty tracks distance only: gem bonuses must not speed up the game
  int32_t s = BASE_SPEED + ((s_dist / 5) * SPEED_RAMP) / 100;
  return s > SPEED_MAX ? SPEED_MAX : s;
}

static int32_t proj_t(int16_t z) {
  if (z < -CAM_D + 4) z = -CAM_D + 4;
  return CAM_D * 1000 / (z + CAM_D);   // t in permille
}

static int16_t proj_y(int16_t z) {
  int32_t t = proj_t(z);
  return (int16_t)(VANISH_Y + ((BASE_Y - VANISH_Y) * t) / 1000);
}

static int16_t proj_x(int16_t z, int lane) {
  int32_t t = proj_t(z);
  return (int16_t)(VANISH_X + (lane * LANE_OFF * t) / 1000);
}

static void stars_reset(void) {
  for (int i = 0; i < STARS_N; i++) {
    s_star_x[i] = rand() % SCREEN_W;
    s_star_y[i] = 6 + rand() % 66;
    s_star_tw[i] = rand() % 2 == 0;
  }
}

// ---------------------------------------------------------------- game

static void game_reset(void) {
  for (int i = 0; i < MAX_ENT; i++) s_ents[i].active = false;
  for (int i = 0; i < MAX_GEMS; i++) s_gems[i].active = false;
  s_dist = 0;
  s_score = 0;
  s_next_spawn_z = 170;
  s_gem_streak = 0;
  s_streak_timer = 0;
  s_lane = 0;
  s_px = VANISH_X;
  s_py = 0;
  s_vy = 0;
  s_jumping = false;
  s_jumps = 0;
  s_sliding = false;
  s_slide_left = 0;
  s_slide_queued = false;
  s_riding = -1;
  s_switch_cd = 0;
  s_run_ms = 0;
  s_dead_ms = 0;
  s_roof_grace = 0;
#if RAMPTEST
  s_next_spawn_z = 60;   // first pattern arrives fast
#endif
#if SLIDETEST
  s_next_spawn_z = 60;   // scripted drone arrives fast
#endif
}

static Ent *ent_free(void) {
  for (int i = 0; i < MAX_ENT; i++)
    if (!s_ents[i].active) return &s_ents[i];
  return NULL;
}

static void gem_row(int lane, int z0, int n, int8_t h, int arc) {
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < MAX_GEMS; k++) {
      if (!s_gems[k].active) {
        s_gems[k].active = true;
        s_gems[k].lane = lane;
        s_gems[k].z = z0 + i * 14;
        s_gems[k].h = h + (arc ? (int8_t)(i % 2 == 1 ? 16 : 0) : 0);
        break;
      }
    }
  }
}

static void spawn_pattern(int32_t z_abs) {
  int r = rand() % 100;
  int lane = rand() % 3 - 1;
  int32_t gap_min = 62 + (speed_now() - BASE_SPEED) / 3;
  int16_t zr = (int16_t)(z_abs - s_dist);   // relative z for entity placement

#if SLIDETEST
  if (z_abs < 120) {
    Ent *e = ent_free();
    if (e) { e->active = true; e->lane = 0; e->z = zr; e->kind = 2; e->phase = rand() % 100; }
    s_next_spawn_z = z_abs + 220;
    return;
  }
#endif

#if RAMPTEST
  if (z_abs < 120) {
    // deterministic: ramp + long landable train in lane 0
    Ent *ramp = ent_free();
    if (ramp) ramp->active = true;
    Ent *tr = ramp ? ent_free() : NULL;
    if (tr) tr->active = true;
    if (ramp && tr) {
      ramp->lane = 0; ramp->z = zr; ramp->kind = 3; ramp->phase = 0;
      tr->lane = 0; tr->z = zr + RAMP_LEN + 4;
      tr->kind = 4; tr->phase = 400;                 // TEST long roof
      gem_row(0, zr + RAMP_LEN + 14, 6, TRAIN_H + 6, 0);
      APP_LOG(APP_LOG_LEVEL_INFO, "RAMPTEST: spawned ramp+train z=%d", zr);
    }
    s_next_spawn_z = z_abs + RAMP_LEN + 4 + TRAIN_LEN + 150 + 62;
    return;
  }
#endif

  if (r < 32) {
    // single train
    Ent *e = ent_free();
    if (e) {
      e->active = true; e->lane = lane; e->z = zr; e->kind = 0; e->phase = 0;
    }
    s_next_spawn_z = z_abs + TRAIN_LEN + gap_min;
  } else if (r < 50 && s_dist > 1200) {
    // double train, one lane free
    int free_lane = rand() % 3 - 1;
    for (int l = -1; l <= 1; l++) {
      if (l == free_lane) continue;
      Ent *e = ent_free();
      if (e) { e->active = true; e->lane = l; e->z = zr; e->kind = 0; e->phase = 0; }
    }
    s_next_spawn_z = z_abs + TRAIN_LEN + gap_min + 34;
  } else if (r < 62 && s_dist > 600) {
    // RAMP + LANDABLE TRAIN: ride up, launch, run on the roof
    Ent *ramp = ent_free();
    if (ramp) ramp->active = true;
    Ent *tr = ramp ? ent_free() : NULL;
    if (tr) tr->active = true;
    if (ramp && tr) {
      int16_t len = TRAIN_LEN + 30 + speed_now() / 3;
      ramp->lane = lane; ramp->z = zr; ramp->kind = 3; ramp->phase = 0;
      tr->lane = lane; tr->z = zr + RAMP_LEN + 4;
      tr->kind = 4; tr->phase = len - TRAIN_LEN;
      gem_row(lane, zr + RAMP_LEN + 10, 3, TRAIN_H + 6, 0);
      s_next_spawn_z = z_abs + RAMP_LEN + 4 + len + gap_min;
    } else {
      s_next_spawn_z = z_abs + gap_min;
    }
  } else if (r < 72) {
    // jump barrier (+ optional coin arc)
    Ent *e = ent_free();
    if (e) {
      e->active = true; e->lane = lane; e->z = zr; e->kind = 1; e->phase = 0;
    }
    if (rand() % 100 < 55) gem_row(lane, zr - 20, 3, 22, 1);
    s_next_spawn_z = z_abs + gap_min;
  } else if (r < 86 && s_dist > 900) {
    // hover drone at head height: slide under it or switch lanes
    Ent *e = ent_free();
    if (e) {
      e->active = true; e->lane = lane; e->z = zr; e->kind = 2; e->phase = rand() % 100;
    }
    s_next_spawn_z = z_abs + gap_min;
  } else {
    // crystal trail
    gem_row(lane, zr, 4, 0, 0);
    s_next_spawn_z = z_abs + 46;
  }
}

// ---------------------------------------------------------------- input

static void start_run(void) {
  game_reset();
  s_gs = GS_RUN;
#if RAMPTEST
  APP_LOG(APP_LOG_LEVEL_INFO, "RT RUN started");
#endif
}

static void do_jump(void) {
  s_riding = -1;               // jumping off the wedge cancels the ride
  s_sliding = false;           // jump cancels slide
  s_slide_queued = false;
  if (!s_jumping && s_jumps == 0) {
    s_vy = JUMP_V;
    s_jumps = 1;
    s_jumping = true;
  } else if (s_jumps < 2) {
    s_vy = JUMP2_V;
    s_jumps = 2;
    s_jumping = true;
    vibes_double_pulse();
  }
}

static void do_slide(void) {
  if (s_gs != GS_RUN) return;
  if (s_jumping || (-s_py) > 4) {
    // airborne: slam down fast, auto-slide on landing
    if (s_vy < FASTFALL_V) s_vy = FASTFALL_V;
    s_slide_queued = true;
  } else {
    s_sliding = true;
    s_slide_left = SLIDE_MS;
  }
}

static void up_handler(ClickRecognizerRef rec, void *ctx) {
  (void)rec; (void)ctx;
  if (s_gs == GS_READY) { start_run(); return; }
  if (s_gs == GS_DEAD) { if (s_dead_ms >= 500) start_run(); return; }
  if (s_gs == GS_RUN && s_switch_cd <= 0 && s_lane < 1) { s_lane++; s_switch_cd = SWITCH_CD; }
}

static void down_handler(ClickRecognizerRef rec, void *ctx) {
  (void)rec; (void)ctx;
  if (s_gs == GS_RUN) do_slide();
}

static void select_handler(ClickRecognizerRef rec, void *ctx) {
  (void)rec; (void)ctx;
  if (s_gs == GS_RUN) {
    do_jump();
  } else if (s_gs == GS_READY) {
    start_run();
  } else if (s_gs == GS_DEAD && s_dead_ms >= 500) {
    start_run();
  }
}

static void back_handler(ClickRecognizerRef rec, void *ctx) {
  (void)rec; (void)ctx;
  // preferred path: if firmware delivers BACK clicks, no window pop = no flicker
  if (s_gs == GS_RUN) {
    if (s_switch_cd <= 0 && s_lane > -1) { s_lane--; s_switch_cd = SWITCH_CD; }
  } else {
    s_exit_app = true;
    window_stack_pop_all(true);
  }
}

static void click_config(void *ctx) {
  window_raw_click_subscribe(BUTTON_ID_UP, up_handler, NULL, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, down_handler, NULL, NULL);
  window_raw_click_subscribe(BUTTON_ID_SELECT, select_handler, NULL, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_handler);
  // FALLBACK: if BACK still cannot be intercepted, the OS pops the window and
  // the shadow-window pattern below handles it (move left + instant re-push).
}

// ---------------------------------------------------------------- update

static void check_collisions(void) {
#if GODMODE
  return;
#endif

  for (int i = 0; i < MAX_ENT; i++) {
    Ent *e = &s_ents[i];
    if (!e->active) continue;
    if (e->lane != s_lane) continue;
    if (e->kind == 0 || e->kind == 4) {
      // train occupies z..z+len; safe when on/above the roof,
      // and forgiven while rising in a jump (landing-on-roof attempts)
      if (e->z - 4 < 4 && e->z + ent_len(e) > -6) {
        bool rising = (s_jumping && s_vy < 0);
        // forgiven while rising in a jump, during post-launch roof grace,
        // and while climbing the ramp (launch will carry us above the roof)
        if ((-s_py) < TRAIN_H - 4 && !rising && s_roof_grace <= 0 && s_riding < 0) goto died;
      }
    } else if (e->kind == 1) {
      // barrier: jump clears at py < -16; slide does NOT clear a barrier
      if (e->z > -6 && e->z < 8 && s_py > -16) goto died;
    } else if (e->kind == 2) {
      // drone at head height: slide under it, switch lanes,
      // or be well above it (double jump / roof running)
      if (e->z > -6 && e->z < 8 && !s_sliding && (-s_py) < 44) goto died;
    }
  }
  return;

died:
  if (s_score > s_best) {
    s_best = s_score;
    persist_write_int(0, s_best);
    s_new_best_flash = 2600;
  }
  s_gs = GS_DEAD;
  s_dead_ms = 0;
  vibes_long_pulse();
}

static void update(int ms) {
  for (int i = 0; i < STARS_N; i++)
    if (rand() % 100 < 2) s_star_tw[i] = !s_star_tw[i];

  if (s_gs == GS_READY) {
    s_ready_pulse += ms;
#if SLIDETEST
    if (s_ready_pulse > 8000) start_run();   // long enough to screenshot the title
#endif
#if RAMPTEST
    if (s_ready_pulse > 3000) start_run();
#endif
    return;
  }
  if (s_gs == GS_DEAD) {
    s_dead_ms += ms;
    if (s_new_best_flash > 0) s_new_best_flash -= ms;
    return;
  }

  int32_t sp = speed_now();
  s_dist += (sp * ms) / 1000;
  s_score = s_dist / 5;
  s_run_ms += ms;
  if (s_switch_cd > 0) s_switch_cd -= ms;

  if (s_sliding) {
    s_slide_left -= ms;
    if (s_slide_left <= 0 || s_jumping) { s_sliding = false; s_slide_queued = false; }
  }

#if SLIDETEST
  // scripted: long slide window while the first drone closes in
  if (s_gs == GS_RUN && !s_jumping) {
    for (int i = 0; i < MAX_ENT; i++) {
      Ent *e = &s_ents[i];
      if (e->active && e->kind == 2 && e->lane == s_lane && e->z < 130 && e->z > -20) {
        if (!s_sliding) { s_sliding = true; s_slide_left = 3000; }
      }
    }
  }
#endif

  if (s_streak_timer > 0) {
    s_streak_timer -= ms;
    if (s_streak_timer <= 0) s_gem_streak = 0;
  }

  // player visual x follows logical lane
  int16_t target = VANISH_X + s_lane * LANE_OFF;
  s_px += (target - s_px) * ms / 80;
  if ((target - s_px) * (target - s_px) < 4) s_px = target;

  // ramp ride tracking: pick up wedge, auto-launch at crest
  if (s_riding >= 0) {
    Ent *re = &s_ents[s_riding];
    bool still = re->active && re->kind == 3 && re->lane == s_lane &&
                 re->z <= 6 && re->z + RAMP_LEN >= -6;
    if (!still) {
      s_riding = -1;
      // crest passed: launch up onto the roof (boost even mid-jump)
      if (s_gs == GS_RUN && s_py < -6) {
        s_vy = LAUNCH_V;
        s_jumping = true;
        if (s_jumps < 1) s_jumps = 1;
        s_roof_grace = 300;
#if RAMPTEST
        APP_LOG(APP_LOG_LEVEL_INFO, "RAMPTEST: LAUNCH py=%d", s_py);
#endif
      }
    }
  }
  if (s_roof_grace > 0) s_roof_grace -= ms;

  // support height under the player (ramp slope or landable roof)
  int16_t support = 0;
  for (int i = 0; i < MAX_ENT; i++) {
    Ent *e = &s_ents[i];
    if (!e->active || e->lane != s_lane) continue;
    if (e->kind == 3) {
      if (e->z <= 6 && e->z + RAMP_LEN >= -6) {
        int16_t prog = (int16_t)((0 - e->z) * 1000 / RAMP_LEN);
        if (prog < 0) prog = 0;
        if (prog > 1000) prog = 1000;
        int16_t h = (int16_t)(RAMP_RIDE_H * prog / 1000);
        if (h > support) support = h;
        if (s_riding < 0) s_riding = (int8_t)i;
      }
    } else if (e->kind == 4) {
      if (e->z - 8 <= 0 && e->z + ent_len(e) >= 0 && TRAIN_H > support)
        support = TRAIN_H;
    }
  }
#if RAMPTEST
  for (int i = 0; i < MAX_ENT; i++) {
    Ent *e = &s_ents[i];
    if (e->active && e->kind == 3 && e->z < 90 && e->z > -70) {
      APP_LOG(APP_LOG_LEVEL_INFO,
              "RT ramp%d z=%d sup=%d py=%d vy=%d ride=%d j=%d",
              i, e->z, support, s_py, s_vy, s_riding, s_jumping);
    }
  }
#endif

  // vertical physics with support surfaces
  if (s_jumping || (-s_py) > support + 6) {
    // airborne
    s_vy += (int16_t)(GRAVITY * ms / 1000);
    if (s_vy > TERM_VEL) s_vy = TERM_VEL;
    s_py += (int16_t)(s_vy * ms / 1000);
    if (s_vy >= 0 && (-s_py) <= support) {
      // touchdown: road, ramp slope or train roof
#if RAMPTEST
      if (support > 20) APP_LOG(APP_LOG_LEVEL_INFO, "RAMPTEST: touchdown support=%d py=%d", support, s_py);
#endif
      s_py = -support;
      s_vy = 0;
      s_jumping = false;
      s_jumps = 0;
      if (s_slide_queued) { s_sliding = true; s_slide_left = SLIDE_MS; s_slide_queued = false; }
    }
    if (s_py > 0 && support == 0) {
      s_py = 0;
      s_vy = 0;
      s_jumping = false;
      s_jumps = 0;
      if (s_slide_queued) { s_sliding = true; s_slide_left = SLIDE_MS; s_slide_queued = false; }
    }
  } else if ((-s_py) >= support - 10) {
    // grounded: follow the support surface (ramp slope / roof)
    s_py = -support;
    s_vy = 0;
    s_jumping = false;
    s_jumps = 0;
  }
  // else: ran into a train front below roof level -> collision handles it

  // entities
  for (int i = 0; i < MAX_ENT; i++) {
    Ent *e = &s_ents[i];
    if (!e->active) continue;
    e->z -= (int16_t)(sp * ms / 1000);
    if (e->kind == 2) e->phase += ms / 12;
    if (e->z + ent_len(e) < -30) e->active = false;
  }
  for (int i = 0; i < MAX_GEMS; i++) {
    Gem *g = &s_gems[i];
    if (!g->active) continue;
    g->z -= (int16_t)(sp * ms / 1000);
    if (g->z < -20) g->active = false;
  }

  // spawn ahead: cursor is absolute world distance
  while (s_next_spawn_z < s_dist + SPAWN_Z) {
    int32_t before = s_next_spawn_z;
    spawn_pattern(s_next_spawn_z);
    if (s_next_spawn_z <= before) { s_next_spawn_z = before + 40; }  // safety
  }

  // gems
  for (int i = 0; i < MAX_GEMS; i++) {
    Gem *g = &s_gems[i];
    if (!g->active) continue;
    if (g->lane == s_lane && g->z > -8 && g->z < 10) {
      int gem_up = g->h;                 // positive up
      int pyr = -s_py;                   // positive up
      if (pyr > gem_up - 13 && pyr < gem_up + 13) {
        g->active = false;
        s_gem_streak++;
        s_streak_timer = 2600;
        s_score += 10 * s_gem_streak;
        if (s_gem_streak >= 2) vibes_short_pulse();
#if RAMPTEST
        APP_LOG(APP_LOG_LEVEL_INFO, "RT gem picked py=%d h=%d", s_py, gem_up);
#endif
      }
    }
  }

  check_collisions();
}

// ---------------------------------------------------------------- render

static void draw_sky(GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, 44), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorLiberty);
  graphics_fill_rect(ctx, GRect(0, 44, SCREEN_W, 20), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorIndigo);
  graphics_fill_rect(ctx, GRect(0, 64, SCREEN_W, 20), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorPurple);
  graphics_fill_rect(ctx, GRect(0, 84, SCREEN_W, 12), 0, GCornerNone);

  for (int i = 0; i < STARS_N; i++) {
    graphics_context_set_fill_color(ctx, s_star_tw[i] ? C_TEXT : C_DIM);
    int16_t s = s_star_tw[i] ? 2 : 1;
    graphics_fill_rect(ctx, GRect(s_star_x[i], s_star_y[i], s, s), 0, GCornerNone);
  }

  // retro striped sun (precomputed half-widths, r=30, rows every 4px)
  static const int8_t SUN_HALF[8] = {10, 18, 22, 25, 27, 28, 29, 30};
  int cx = VANISH_X, cy = 74;
  for (int i = 0; i < 8; i++) {
    int dy = -28 + i * 4;                  // -28 .. 0
    int8_t half = SUN_HALF[i];
    // retro gaps: skip stripes in lower half, alternating
    if (i >= 5 && (i % 2) == 0) continue;
    graphics_context_set_fill_color(ctx, (i < 2) ? GColorYellow :
                                    (i < 4) ? GColorOrange : C_PINK);
    graphics_fill_rect(ctx, GRect(cx - half, cy + dy, half * 2, 3), 0, GCornerNone);
  }

  // mountain silhouette along horizon
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  static const GPoint MSKY[12] = {
    {0,96},{18,88},{38,96},{52,84},{70,96},{86,90},
    {114,90},{130,96},{148,84},{164,96},{180,88},{200,96}
  };
  for (int i = 0; i < 11; i++) {
    GPoint pts[4] = { MSKY[i], MSKY[i + 1],
                      GPoint(MSKY[i + 1].x, 96), GPoint(MSKY[i].x, 96) };
    s_pi.num_points = 4; s_pi.points = pts;
    if (s_path) gpath_destroy(s_path);
    s_path = gpath_create(&s_pi);
    gpath_draw_filled(ctx, s_path);
  }

  // horizon glow
  graphics_context_set_stroke_color(ctx, C_CYAN);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(0, VANISH_Y), GPoint(SCREEN_W, VANISH_Y));
}

static void draw_road(GContext *ctx) {
  // side ground
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, VANISH_Y, SCREEN_W, SCREEN_H - VANISH_Y), 0, GCornerNone);

  // road surface trapezoid
  GPoint road[4] = {
    GPoint(VANISH_X - 7, VANISH_Y + 2),
    GPoint(VANISH_X + 7, VANISH_Y + 2),
    GPoint(VANISH_X + EDGE_OFF, BASE_Y + 4),
    GPoint(VANISH_X - EDGE_OFF, BASE_Y + 4)
  };
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  s_pi.num_points = 4; s_pi.points = road;
  if (s_path) gpath_destroy(s_path);
  s_path = gpath_create(&s_pi);
  gpath_draw_filled(ctx, s_path);

  // neon edges
  graphics_context_set_stroke_color(ctx, C_CYAN);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(VANISH_X - 7, VANISH_Y + 2),
                     GPoint(VANISH_X - EDGE_OFF, BASE_Y + 4));
  graphics_draw_line(ctx, GPoint(VANISH_X + 7, VANISH_Y + 2),
                     GPoint(VANISH_X + EDGE_OFF, BASE_Y + 4));

  // moving dashed lane dividers (z-space dashes)
  graphics_context_set_stroke_color(ctx, C_MAGENTA);
  graphics_context_set_stroke_width(ctx, 1);
  int32_t off = s_dist % 26;
  for (int l = -1; l <= 1; l += 2) {
    for (int k = 0; k < 9; k++) {
      int16_t z0 = k * 26 - off;
      int16_t z1 = z0 + 13;
      if (z1 < -30) continue;
      if (z0 < -30) z0 = -30;
      if (z0 > 320) continue;
      int32_t t0 = proj_t(z0), t1 = proj_t(z1);
      graphics_draw_line(ctx,
        GPoint(VANISH_X + (l * (LANE_OFF / 2 + 0) * t0) / 1000, proj_y(z0)),
        GPoint(VANISH_X + (l * (LANE_OFF / 2 + 0) * t1) / 1000, proj_y(z1)));
    }
  }
}

static void draw_train(GContext *ctx, Ent *e) {
  int16_t zf = e->z, zb = e->z + ent_len(e);
  int32_t tf = proj_t(zf), tb = proj_t(zb);
  int16_t xf = proj_x(zf, e->lane), xb = proj_x(zb, e->lane);
  int16_t yf = proj_y(zf), yb = proj_y(zb);
  int16_t hw_f = (int16_t)((26 * tf) / 1000), hw_b = (int16_t)((26 * tb) / 1000);
  int16_t h_f = (int16_t)((56 * tf) / 1000), h_b = (int16_t)((56 * tb) / 1000);
  if (hw_f < 2) return;

  // roof
  GPoint roof[4] = {
    GPoint(xf - hw_f, yf - h_f), GPoint(xf + hw_f, yf - h_f),
    GPoint(xb + hw_b, yb - h_b), GPoint(xb - hw_b, yb - h_b)
  };
  graphics_context_set_fill_color(ctx, GColorBlueMoon);
  s_pi.num_points = 4; s_pi.points = roof;
  if (s_path) gpath_destroy(s_path);
  s_path = gpath_create(&s_pi);
  gpath_draw_filled(ctx, s_path);

  // side face (visible side depends on lane)
  int side = (xf > xb) ? 1 : -1;   // back is toward vanish: draw inner side
  GPoint sface[4] = {
    GPoint(xf + side * hw_f, yf - h_f),
    GPoint(xf + side * hw_f, yf),
    GPoint(xb + side * hw_b, yb),
    GPoint(xb + side * hw_b, yb - h_b)
  };
  graphics_context_set_fill_color(ctx, GColorImperialPurple);
  s_pi.points = sface;
  if (s_path) gpath_destroy(s_path);
  s_path = gpath_create(&s_pi);
  gpath_draw_filled(ctx, s_path);

  // front face
  graphics_context_set_fill_color(ctx, GColorImperialPurple);
  graphics_fill_rect(ctx, GRect(xf - hw_f, yf - h_f, hw_f * 2, h_f), 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, C_MAGENTA);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_rect(ctx, GRect(xf - hw_f, yf - h_f, hw_f * 2, h_f));

  // windshield
  if (hw_f > 5) {
    graphics_context_set_fill_color(ctx, C_CYAN);
    graphics_fill_rect(ctx, GRect(xf - hw_f + 2, yf - h_f + 2, hw_f * 2 - 4, h_f / 3), 0, GCornerNone);
    // headlights
    graphics_context_set_fill_color(ctx, GColorYellow);
    graphics_fill_rect(ctx, GRect(xf - hw_f + 2, yf - 4, 3, 2), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(xf + hw_f - 5, yf - 4, 3, 2), 0, GCornerNone);
  }

  // landable roof: neon runway stripes + hazard tail
  if (e->kind == 4 && hw_f > 3) {
    graphics_context_set_fill_color(ctx, C_CYAN);
    for (int k = 0; k < 4; k++) {
      int16_t zd = zf + ent_len(e) - 6 - k * 7;
      if (zd < -CAM_D + 8) continue;
      int32_t td = proj_t(zd);
      int16_t xw = (int16_t)((8 * td) / 1000);
      if (xw < 1) continue;
      int16_t yd = proj_y(zd) - (int16_t)((TRAIN_H * td) / 1000) - 1;
      graphics_fill_rect(ctx, GRect(proj_x(zd, e->lane) - xw, yd, xw * 2, 2), 0, GCornerNone);
    }
  }
}

static void draw_ramp(GContext *ctx, Ent *e) {
  // wedge: low at front (zf, meets the runner first), high at rear (zn);
  // player rides up as it passes under, then LAUNCH off the crest
  int16_t zf = e->z, zn = e->z + RAMP_LEN;
  int32_t tf = proj_t(zf), tn = proj_t(zn);
  int16_t xf = proj_x(zf, e->lane), xn = proj_x(zn, e->lane);
  int16_t yf = proj_y(zf), yn = proj_y(zn);
  int16_t hw_f = (int16_t)((24 * tf) / 1000), hw_n = (int16_t)((24 * tn) / 1000);
  int16_t h_n = (int16_t)((RAMP_RIDE_H * tn) / 1000);
  if (hw_f < 2 && hw_n < 2) return;

  int side = (xf > xn) ? 1 : -1;   // which flank is visible

  // vertical cut under the slope on the visible flank (gives volume)
  GPoint sface[3] = {
    GPoint(xf + side * hw_f, yf),
    GPoint(xn + side * hw_n, yn - h_n),
    GPoint(xn + side * hw_n, yn)
  };
  graphics_context_set_fill_color(ctx, GColorImperialPurple);
  s_pi.num_points = 3; s_pi.points = sface;
  if (s_path) gpath_destroy(s_path);
  s_path = gpath_create(&s_pi);
  gpath_draw_filled(ctx, s_path);

  // bright slope surface you actually ride
  GPoint top[4] = {
    GPoint(xf - hw_f, yf), GPoint(xf + hw_f, yf),
    GPoint(xn + hw_n, yn - h_n), GPoint(xn - hw_n, yn - h_n)
  };
  graphics_context_set_fill_color(ctx, C_MAGENTA);
  s_pi.num_points = 4; s_pi.points = top;
  if (s_path) gpath_destroy(s_path);
  s_path = gpath_create(&s_pi);
  gpath_draw_filled(ctx, s_path);

  // neon edges along both sides of the slope
  graphics_context_set_stroke_color(ctx, C_PINK);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(xf - hw_f, yf), GPoint(xn - hw_n, yn - h_n));
  graphics_draw_line(ctx, GPoint(xf + hw_f, yf), GPoint(xn + hw_n, yn - h_n));

  // yellow chevrons pointing up the slope
  if (hw_n > 4 && h_n > 8) {
    graphics_context_set_stroke_color(ctx, GColorYellow);
    graphics_context_set_stroke_width(ctx, 1);
    for (int k = 0; k < 2; k++) {
      int ft = 420 + k * 280;                 // 42% / 70% along the slope
      int cx = xf + ((xn - xf) * ft) / 1000;
      int cy = yf + (((yn - h_n) - yf) * ft) / 1000;
      graphics_draw_line(ctx, GPoint(cx - 3, cy + 2), GPoint(cx, cy - 2));
      graphics_draw_line(ctx, GPoint(cx + 3, cy + 2), GPoint(cx, cy - 2));
    }
  }
}

static void draw_barrier(GContext *ctx, Ent *e) {
  int32_t t = proj_t(e->z);
  int16_t x = proj_x(e->z, e->lane);
  int16_t y = proj_y(e->z);
  int16_t hw = (int16_t)((22 * t) / 1000);
  int16_t h = (int16_t)((20 * t) / 1000);
  if (hw < 2) return;
  graphics_context_set_stroke_color(ctx, GColorYellow);
  graphics_context_set_stroke_width(ctx, 2);
  // posts
  graphics_draw_line(ctx, GPoint(x - hw, y), GPoint(x - hw, y - h));
  graphics_draw_line(ctx, GPoint(x + hw, y), GPoint(x + hw, y - h));
  // crossbar
  graphics_context_set_fill_color(ctx, GColorYellow);
  graphics_fill_rect(ctx, GRect(x - hw, y - h, hw * 2, 3), 0, GCornerNone);
  // hazard stripes
  graphics_context_set_fill_color(ctx, C_PINK);
  graphics_fill_rect(ctx, GRect(x - hw / 2, y - h, 3, 3), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(x + hw / 4, y - h, 3, 3), 0, GCornerNone);
}

static void draw_drone(GContext *ctx, Ent *e) {
  int32_t t = proj_t(e->z);
  int16_t x = proj_x(e->z, e->lane);
  int32_t ang = (TRIG_MAX_ANGLE / 100) * (e->phase % 100);   // normalized: no overflow
  int16_t y = proj_y(e->z) - (int16_t)((30 * t) / 1000) -
              (int16_t)(3 * sin_lookup(ang) / TRIG_MAX_ANGLE);
  int16_t r = (int16_t)((8 * t) / 1000);
  if (r < 2) return;
  GPoint hex[6] = {
    {x - r, y}, {x - r / 2, y - r}, {x + r / 2, y - r},
    {x + r, y}, {x + r / 2, y + r}, {x - r / 2, y + r}
  };
  graphics_context_set_fill_color(ctx, GColorYellow);
  s_pi.num_points = 6; s_pi.points = hex;
  if (s_path) gpath_destroy(s_path);
  s_path = gpath_create(&s_pi);
  gpath_draw_filled(ctx, s_path);
  if (r > 3) {
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, GRect(x - 2, y - 1, 4, 2), 0, GCornerNone);
    graphics_context_set_stroke_color(ctx, GColorChromeYellow);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(x - r - 2, y - r - 1), GPoint(x + r + 2, y - r - 1));
  }
}

static void draw_entities(GContext *ctx) {
  // far to near: insertion by z descending
  int idx[MAX_ENT];
  int n = 0;
  for (int i = 0; i < MAX_ENT; i++)
    if (s_ents[i].active) idx[n++] = i;
  for (int i = 0; i < n - 1; i++)
    for (int k = 0; k < n - 1 - i; k++)
      if (s_ents[idx[k]].z < s_ents[idx[k + 1]].z) {
        int tmp = idx[k]; idx[k] = idx[k + 1]; idx[k + 1] = tmp;
      }
  for (int i = 0; i < n; i++) {
    Ent *e = &s_ents[idx[i]];
    if (e->kind == 0 || e->kind == 4) draw_train(ctx, e);
    else if (e->kind == 1) draw_barrier(ctx, e);
    else if (e->kind == 3) draw_ramp(ctx, e);
    else draw_drone(ctx, e);
  }
}

static void draw_gems(GContext *ctx) {
  for (int i = 0; i < MAX_GEMS; i++) {
    Gem *g = &s_gems[i];
    if (!g->active) continue;
    if (g->z < -20 || g->z > 320) continue;
    int32_t t = proj_t(g->z);
    int16_t x = proj_x(g->z, g->lane);
    int16_t y = proj_y(g->z) - (int16_t)((g->h + 8) * t) / 1000;
    if (t < 120) continue;   // too far
    gpath_move_to(s_gem_path, GPoint(x, y));
    graphics_context_set_fill_color(ctx, C_GEM);
    gpath_draw_filled(ctx, s_gem_path);
    if (t > 500) {
      graphics_context_set_fill_color(ctx, GColorMintGreen);
      graphics_fill_rect(ctx, GRect(x - 1, y - 2, 2, 2), 0, GCornerNone);
    }
  }
}

static void draw_player(GContext *ctx) {
  int16_t feet = BASE_Y + s_py;
  int16_t cx = s_px;
  bool frameB = (s_run_ms / RUN_MS) % 2 == 1;

  if (s_gs == GS_DEAD) {
    // splat on the track: flat body, head rolled to the side
    graphics_context_set_fill_color(ctx, C_TOP);
    graphics_fill_rect(ctx, GRect(cx - 9, feet - 5, 18, 5), 2, GCornersAll);
    graphics_context_set_fill_color(ctx, C_HOOD);
    graphics_fill_rect(ctx, GRect(cx + 7, feet - 11, 6, 6), 2, GCornersAll);
    graphics_context_set_fill_color(ctx, C_PACK);
    graphics_fill_rect(ctx, GRect(cx - 3, feet - 4, 6, 2), 1, GCornersAll);
    return;
  }

  if (s_sliding && !s_jumping) {
    // low crouch: torso flat, hood leading, speed lines behind
    graphics_context_set_fill_color(ctx, C_LIMB);
    graphics_fill_rect(ctx, GRect(cx - 8, feet - 4, 6, 4), 1, GCornersAll);
    graphics_fill_rect(ctx, GRect(cx + 3, feet - 5, 6, 5), 1, GCornersAll);
    graphics_context_set_fill_color(ctx, C_TOP);
    graphics_fill_rect(ctx, GRect(cx - 7, feet - 13, 15, 9), 2, GCornersAll);
    graphics_context_set_fill_color(ctx, C_PACK);
    graphics_fill_rect(ctx, GRect(cx - 10, feet - 11, 4, 6), 1, GCornersAll);
    graphics_context_set_fill_color(ctx, C_CYAN);
    graphics_fill_rect(ctx, GRect(cx + 5, feet - 15, 5, 2), 1, GCornersAll);
    graphics_context_set_fill_color(ctx, C_HOOD);
    graphics_fill_rect(ctx, GRect(cx + 5, feet - 21, 7, 6), 2, GCornersAll);
    graphics_context_set_stroke_color(ctx, C_CYAN);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(cx - 14, feet - 9), GPoint(cx - 9, feet - 9));
    graphics_draw_line(ctx, GPoint(cx - 16, feet - 5), GPoint(cx - 10, feet - 5));
    return;
  }

  // legs / feet (light gray, gap separates them from orange torso)
  graphics_context_set_fill_color(ctx, C_LIMB);
  if (s_jumping) {
    // tucked up
    graphics_fill_rect(ctx, GRect(cx - 6, feet - 6, 5, 6), 1, GCornersAll);
    graphics_fill_rect(ctx, GRect(cx + 1, feet - 6, 5, 6), 1, GCornersAll);
  } else if (frameB) {
    graphics_fill_rect(ctx, GRect(cx - 7, feet - 6, 5, 6), 1, GCornersAll);
    graphics_fill_rect(ctx, GRect(cx + 3, feet - 5, 5, 5), 1, GCornersAll);
  } else {
    graphics_fill_rect(ctx, GRect(cx - 5, feet - 5, 5, 5), 1, GCornersAll);
    graphics_fill_rect(ctx, GRect(cx + 5, feet - 6, 5, 6), 1, GCornersAll);
  }

  // torso: bright orange hoodie (seen from behind)
  graphics_context_set_fill_color(ctx, C_TOP);
  graphics_fill_rect(ctx, GRect(cx - 6, feet - 19, 13, 11), 2, GCornersAll);
  // backpack: yellow, on the back
  graphics_context_set_fill_color(ctx, C_PACK);
  graphics_fill_rect(ctx, GRect(cx - 3, feet - 17, 7, 6), 1, GCornersAll);

  // pumping arms (light gray)
  graphics_context_set_fill_color(ctx, C_LIMB);
  if (s_jumping) {
    // arms up
    graphics_fill_rect(ctx, GRect(cx - 11, feet - 24, 4, 8), 1, GCornersAll);
    graphics_fill_rect(ctx, GRect(cx + 8, feet - 24, 4, 8), 1, GCornersAll);
  } else if (frameB) {
    graphics_fill_rect(ctx, GRect(cx - 11, feet - 18, 4, 7), 1, GCornersAll);
    graphics_fill_rect(ctx, GRect(cx + 8, feet - 15, 4, 6), 1, GCornersAll);
  } else {
    graphics_fill_rect(ctx, GRect(cx - 11, feet - 15, 4, 6), 1, GCornersAll);
    graphics_fill_rect(ctx, GRect(cx + 8, feet - 18, 4, 7), 1, GCornersAll);
  }

  // cyan collar between torso and head
  graphics_context_set_fill_color(ctx, C_CYAN);
  graphics_fill_rect(ctx, GRect(cx - 3, feet - 21, 7, 2), 1, GCornersAll);
  // head: white hood
  graphics_context_set_fill_color(ctx, C_HOOD);
  graphics_fill_rect(ctx, GRect(cx - 3, feet - 27, 7, 6), 2, GCornersAll);
}

static void draw_hud(GContext *ctx) {
  static char buf[28];
  snprintf(buf, sizeof(buf), "%ld", (long)s_score);
  graphics_context_set_text_color(ctx, C_TEXT);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS),
                     GRect(96, 4, 100, 24), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentRight, NULL);
  snprintf(buf, sizeof(buf), "HI %ld", (long)s_best);
  graphics_context_set_text_color(ctx, C_DIM);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, 6, 80, 16), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  if (s_gem_streak >= 2 && s_streak_timer > 0) {
    snprintf(buf, sizeof(buf), "x%ld", (long)s_gem_streak);
    graphics_context_set_text_color(ctx, C_GEM);
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(4, 24, 40, 16), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
  }
}

static void draw_ready(GContext *ctx) {
  graphics_context_set_text_color(ctx, C_TEXT);
  graphics_draw_text(ctx, "NEON RUN", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, 26, SCREEN_W, 34), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, C_MAGENTA);
  graphics_draw_text(ctx, "LANES", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, 56, SCREEN_W, 34), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, C_CYAN);
  graphics_draw_text(ctx, "BACK/UP lane\nSELECT jump\nDOWN slide",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, 116, SCREEN_W, 52), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  if ((s_ready_pulse / 500) % 2 == 0) {
    graphics_context_set_text_color(ctx, C_PINK);
    graphics_draw_text(ctx, "PRESS UP TO RUN",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(0, 174, SCREEN_W, 20), GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
  }
}

static void draw_dead(GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 58, SCREEN_W, 96), 0, GCornerNone);
  graphics_context_set_text_color(ctx, C_MAGENTA);
  graphics_draw_text(ctx, "WIPEOUT", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, 62, SCREEN_W, 34), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  static char buf[36];
  snprintf(buf, sizeof(buf), "SCORE %ld  BEST %ld", (long)s_score, (long)s_best);
  graphics_context_set_text_color(ctx, C_TEXT);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(0, 98, SCREEN_W, 20), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  if (s_new_best_flash > 0) {
    bool on = (s_new_best_flash / 240) % 2 == 0;
    graphics_context_set_text_color(ctx, on ? C_PINK : C_MAGENTA);
    graphics_draw_text(ctx, "NEW RECORD!", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(0, 116, SCREEN_W, 18), GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
  }
  if (s_dead_ms > 500 && (s_dead_ms / 500) % 2 == 0) {
    graphics_context_set_text_color(ctx, C_CYAN);
    graphics_draw_text(ctx, "UP to retry", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(0, 136, SCREEN_W, 16), GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
  }
}

static void field_update(Layer *layer, GContext *ctx) {
  (void)layer;
  draw_sky(ctx);
  draw_road(ctx);
  draw_entities(ctx);
  draw_gems(ctx);
  draw_player(ctx);
  draw_hud(ctx);
  if (s_gs == GS_READY) draw_ready(ctx);
  if (s_gs == GS_DEAD) draw_dead(ctx);
}

// ---------------------------------------------------------------- app

static AppTimer *s_gt;

static void gtick(void *data) {
  (void)data;
  update(40);
  if (s_field) layer_mark_dirty(s_field);
#if RAMPTEST
  static int16_t log_div = 0;
  if (++log_div >= 6) {
    log_div = 0;
    APP_LOG(APP_LOG_LEVEL_INFO, "RT state gs=%d lane=%d py=%d jump=%d ride=%d",
            s_gs, s_lane, s_py, s_jumping, s_riding);
  }
#endif
  s_gt = app_timer_register(40, gtick, NULL);
}

static void win_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_screen = layer_get_frame(root);
  if (!s_field) {
    // kept alive across quick shadow re-push: no teardown = less flicker
    s_field = layer_create(s_screen);
    layer_set_update_proc(s_field, field_update);
  }
  layer_add_child(root, s_field);
  // restart game loop if it was stopped by unload (shadow re-push path)
  if (!s_gt) s_gt = app_timer_register(40, gtick, NULL);
}

static void push_game_cb(void *data) {
  (void)data;
  window_stack_push(s_win, false);
}

static void exit_cb(void *data) {
  (void)data;
  window_stack_pop_all(true);
}

static void win_unload(Window *w) {
  (void)w;
  // NOTE: s_gt / s_field / s_path stay alive across the quick re-push —
  // tearing them down here adds extra blank frames to the BACK flicker.

  if ((s_gs == GS_RUN || (s_gs == GS_DEAD && s_dead_ms < 600)) && !s_exit_app) {
    // BACK during run = move one lane left, then instantly re-push.
    // BACK right after death (<600ms) = ignored, no accidental app exit.
    if (s_gs == GS_RUN && s_switch_cd <= 0 && s_lane > -1) { s_lane--; s_switch_cd = SWITCH_CD; }
    app_timer_register(1, push_game_cb, NULL);
  } else {
    // BACK on title/death screen = exit app
    s_exit_app = true;
    app_timer_register(1, exit_cb, NULL);
  }
}

static void init(void) {
  srand((unsigned)time(NULL));
  s_best = persist_exists(0) ? persist_read_int(0) : 0;
  s_gs = GS_READY;
  stars_reset();
  game_reset();
  s_ready_pulse = 0;

  static GPathInfo gem_pi = { 4, (GPoint *)GEM_PTS };
  s_gem_path = gpath_create(&gem_pi);

  s_win = window_create();
  window_set_background_color(s_win, GColorOxfordBlue);
  window_set_window_handlers(s_win, (WindowHandlers){
    .load = win_load, .unload = win_unload
  });
  window_set_click_config_provider(s_win, click_config);

  // shadow window underneath: keeps app alive when BACK pops the game window
  s_shadow = window_create();
  window_set_background_color(s_shadow, GColorOxfordBlue);
  window_stack_push(s_shadow, true);
  window_stack_push(s_win, true);
}

int main(void) {
  init();
  s_gt = app_timer_register(40, gtick, NULL);
  app_event_loop();
  return 0;
}
