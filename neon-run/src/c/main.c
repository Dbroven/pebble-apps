/*
 * NEON RUN — endless runner for Pebble Time 2 (emery)
 * by ClawBro for Daniel, 2026
 *
 * Synthwave night: parallax mountains, moon, stars, falling stars.
 * Astro-runner bot. UP = jump / double jump, DOWN = slide.
 * Spikes (jump), patrol drones (slide), crystals, combo scoring,
 * best score persisted.
 */

#include <pebble.h>
#include <stdlib.h>

// ---------------------------------------------------------------- config

#define SCREEN_W 200
#define SCREEN_H 228
#define GROUND_Y 190
#define FLOOR_Y (GROUND_Y + 6)

#define PLAYER_GX 48
#define PLAYER_GW 16
#define PLAYER_STAND_H 24
#define PLAYER_SLIDE_H 13
#define PLAYER_JUMP_V  (-220)
#define PLAYER_JUMP2_V (-195)
#define GRAVITY 640
#define TERM_VEL 600

#define RUN_MS 50
#define BASE_SPEED 88
#define SPEED_RAMP 1.7
#define SPEED_MAX 176
#define GAP_MIN 64

#define FAR_SPEED   12
#define MID_SPEED   26
#define NEAR_SPEED  46

#define PATTERN_W 200   // mountain pattern tiles seamlessly at this width

static GRect s_screen;

// ---------------------------------------------------------------- state

typedef enum { GS_READY = 0, GS_RUN, GS_DEAD } GameState;

typedef struct {
  int16_t x;
  bool active;
  int16_t count;   // spikes: 1..3
  int16_t phase;   // drone bob
  int kind;        // 0 spike cluster, 1 drone
  int16_t hover_y; // drone hover center
} Obst;

typedef struct {
  int16_t x, y;
  bool active;
} Gem;

typedef struct {
  int16_t x, y, len, vy;
  bool active;
} ShootingStar;

#define MAX_OBST 6
#define MAX_GEMS 5
#define MAX_TRIP 3
#define STARS_N 18

static Window *s_win;
static Layer *s_field;
static GameState s_gs;

static GPoint s_far[4];
static GPoint s_mid[4];
static GPoint s_near[4];

static int16_t s_star_x[STARS_N], s_star_y[STARS_N];
static bool s_star_tw[STARS_N];
static ShootingStar s_trips[MAX_TRIP];

static int32_t s_scroll_far, s_scroll_mid, s_scroll_near;
static int32_t s_dist;
static int32_t s_score;
static int32_t s_best;

static Obst s_obst[MAX_OBST];
static Gem s_gems[MAX_GEMS];
static int32_t s_gem_streak;
static int32_t s_streak_timer;

static bool s_paused;
static int16_t s_py;      // feet y
static int16_t s_vy;
static bool s_sliding;
static bool s_sliding_held;
static bool s_jumping;
static int s_jumps;
static int s_run_ms;
static int s_dead_ms;
static int s_ready_pulse;
static int s_new_best_flash;

// ---------------------------------------------------------------- colors

#define C_BG      GColorOxfordBlue
#define C_BG2     GColorIndigo
#define C_STAR    GColorWhite
#define C_MT_MID  GColorIndigo
#define C_MT_NEAR GColorBlueMoon
#define C_NEON    GColorElectricUltramarine
#define C_MAGENTA GColorFashionMagenta
#define C_CYAN    GColorCyan
#define C_GRID    GColorVividCerulean
#define C_PLAYER  GColorWhite
#define C_P_ACC   GColorCyan
#define C_SPIKE   GColorFashionMagenta
#define C_DRONE   GColorYellow
#define C_GEM     GColorGreen
#define C_TEXT    GColorWhite
#define C_DIM     GColorLightGray

// ---------------------------------------------------------------- gpaths

static const GPoint MOON_PTS[8] = {
  {13,5},{11,11},{5,13},{-2,11},{-11,5},{-13,-2},{-11,-9},{-2,-13}
};

// v1.2.0 "Neon Cat": ears integrated into one clean outline (no self-intersection)
static const GPoint BODY_RUN_A[12] = {
  {-11,0},{-12,-6},{-10,-9},{-8,-14},{-5,-11},{-2,-12},
  {2,-12},{5,-11},{8,-14},{10,-9},{12,-6},{11,0}
};
static const GPoint BODY_RUN_B[12] = {
  {-11,0},{-12,-6},{-10,-9},{-8,-14},{-5,-11},{-2,-12},
  {2,-12},{5,-11},{8,-14},{10,-9},{12,-6},{11,0}
};
static const GPoint BODY_SLIDE[9] = {
  {-13,-2},{-14,-6},{-8,-9},{0,-10},{8,-9},{15,-5},{14,-1},{6,0},{-6,0}
};
static const GPoint SPIKE_PTS[3] = {{-5,0},{5,0},{0,-13}};
static const GPoint DRONE_PTS[7] = {
  {-10,0},{-7,-5},{0,-7},{7,-5},{10,0},{5,5},{-5,5}
};
static const GPoint GEM_PTS[4] = {{0,-5},{4,0},{0,5},{-4,0}};

// v1.1.0: shapes are created once, moved per draw (was destroy/create per frame)
static GPath *p_moon, *p_run_a, *p_run_b, *p_slide, *p_spike, *p_drone, *p_gem;

static void paths_init(void) {
  GPathInfo mi = {8, (GPoint *)MOON_PTS};
  GPathInfo ra = {12, (GPoint *)BODY_RUN_A};
  GPathInfo rb = {12, (GPoint *)BODY_RUN_B};
  GPathInfo sl = {9, (GPoint *)BODY_SLIDE};
  GPathInfo sp = {3, (GPoint *)SPIKE_PTS};
  GPathInfo dr = {7, (GPoint *)DRONE_PTS};
  GPathInfo ge = {4, (GPoint *)GEM_PTS};
  p_moon = gpath_create(&mi);  p_run_a = gpath_create(&ra);
  p_run_b = gpath_create(&rb); p_slide = gpath_create(&sl);
  p_spike = gpath_create(&sp); p_drone = gpath_create(&dr);
  p_gem = gpath_create(&ge);
}

static void paths_free(void) {
  GPath **all[] = {&p_moon, &p_run_a, &p_run_b, &p_slide,
                   &p_spike, &p_drone, &p_gem};
  for (int i = 0; i < 7; i++)
    if (*all[i]) { gpath_destroy(*all[i]); *all[i] = NULL; }
}

// ---------------------------------------------------------------- helpers

static int32_t speed_now(void) {
  int32_t s = BASE_SPEED + (s_score * SPEED_RAMP) / 100;
  return s > SPEED_MAX ? SPEED_MAX : s;
}

static void layers_reset(void) {
  int16_t h = 118;
  // first and last y match for seamless tiling at PATTERN_W
  s_far[0]  = GPoint(0, h);       s_far[1]  = GPoint(60, h - 38);
  s_far[2]  = GPoint(128, h + 6); s_far[3]  = GPoint(PATTERN_W, h);
  s_mid[0]  = GPoint(0, h + 16);  s_mid[1]  = GPoint(74, h - 2);
  s_mid[2]  = GPoint(140, h + 22); s_mid[3] = GPoint(PATTERN_W, h + 16);
  s_near[0] = GPoint(0, h + 34);  s_near[1] = GPoint(86, h + 10);
  s_near[2] = GPoint(152, h + 36); s_near[3] = GPoint(PATTERN_W, h + 34);
}

static void stars_reset(void) {
  for (int i = 0; i < STARS_N; i++) {
    s_star_x[i] = rand() % SCREEN_W;
    s_star_y[i] = 8 + rand() % 96;
    s_star_tw[i] = rand() % 2 == 0;
  }
}

static int16_t ridge_y_at(const GPoint *lay, int16_t x) {
  int i = 0;
  while (i < 2 && x > lay[i + 1].x) i++;
  int16_t x0 = lay[i].x, x1 = lay[i + 1].x;
  int16_t y0 = lay[i].y, y1 = lay[i + 1].y;
  if (x1 == x0) return y0;
  int32_t t = ((int32_t)(x - x0) * 1000) / (x1 - x0);
  return (int16_t)(y0 + (t * (y1 - y0)) / 1000);
}

// ---------------------------------------------------------------- game

static void game_reset(void) {
  s_scroll_far = s_scroll_mid = s_scroll_near = 0;
  s_dist = 0;
  s_score = 0;
  for (int i = 0; i < MAX_OBST; i++) s_obst[i].active = false;
  for (int i = 0; i < MAX_GEMS; i++) s_gems[i].active = false;
  for (int i = 0; i < MAX_TRIP; i++) s_trips[i].active = false;
  s_gem_streak = 0;
  s_streak_timer = 0;
  s_py = GROUND_Y;
  s_vy = 0;
  s_sliding = false;
  s_sliding_held = false;
  s_jumping = false;
  s_jumps = 0;
  s_run_ms = 0;
  s_dead_ms = 0;
  s_paused = false;
}

static void obst_spawn(void) {
  int32_t speed = speed_now();
  int16_t rx = 0;
  for (int i = 0; i < MAX_OBST; i++)
    if (s_obst[i].active && s_obst[i].x > rx) rx = s_obst[i].x;
  int16_t allow = SCREEN_W + 12 - GAP_MIN -
                  (int16_t)(((speed - BASE_SPEED) * 3) / 4);
  // spawn only when the rightmost obstacle has moved far enough left
  if (rx > allow) return;

  Obst *o = NULL;
  for (int i = 0; i < MAX_OBST; i++)
    if (!s_obst[i].active) { o = &s_obst[i]; break; }
  if (!o) return;

  o->kind = (s_dist > 900 && rand() % 100 < 28) ? 1 : 0;
  o->x = SCREEN_W + 12;
  o->phase = rand() % 100;
  if (o->kind == 1) {
    o->hover_y = GROUND_Y - 27; // head height: standing ALWAYS hits, slide safe
    o->count = 0;
  } else {
    o->count = 1;
    if (rand() % 100 < 30) o->count++;
    if (s_dist > 600 && rand() % 100 < 25) o->count++;
  }
  o->active = true;

  if (rand() % 100 < 42) {
    for (int i = 0; i < MAX_GEMS; i++) {
      if (!s_gems[i].active) {
        s_gems[i].x = o->x + 24 + rand() % 26;
        s_gems[i].y = GROUND_Y - (36 + rand() % 30);
        s_gems[i].active = true;
        break;
      }
    }
  }
}

static void spawn_trip(void) {
  for (int i = 0; i < MAX_TRIP; i++) {
    if (!s_trips[i].active) {
      s_trips[i].x = 30 + rand() % (SCREEN_W - 60);
      s_trips[i].y = 8 + rand() % 36;
      s_trips[i].len = 6 + rand() % 5;
      s_trips[i].vy = 90 + rand() % 70;
      s_trips[i].active = true;
      return;
    }
  }
}

// ---------------------------------------------------------------- input

static void start_run(void) {
  game_reset();
  s_gs = GS_RUN;
}

static void do_jump(void) {
  if (!s_jumping && s_jumps == 0 && !s_sliding) {
    s_vy = PLAYER_JUMP_V;
    s_jumps = 1;
    s_jumping = true;
  } else if (s_jumps < 2) {
    s_vy = PLAYER_JUMP2_V;
    s_jumps = 2;
    s_jumping = true;
    vibes_double_pulse();
  }
}

static void do_jump_now(void) {
  if (s_sliding) {
    // cancel slide into a jump
    s_sliding = false;
    s_sliding_held = false;
    s_jumps = 0;
    s_jumping = false;
  }
  do_jump();
}

// v1.1.1: UP = pause toggle, SELECT = jump
static void up_click_handler(ClickRecognizerRef rec, void *ctx) {
  (void)rec; (void)ctx;
  if (s_gs == GS_READY) { start_run(); return; }
  if (s_gs == GS_DEAD) {
    if (s_dead_ms >= 500) start_run();
    return;
  }
  if (s_gs != GS_RUN) return;
  s_paused = !s_paused;
  vibes_short_pulse();
}

static void select_handler(ClickRecognizerRef rec, void *ctx) {
  (void)rec; (void)ctx;
  if (s_gs == GS_READY) { start_run(); return; }
  if (s_gs == GS_DEAD) {
    if (s_dead_ms >= 500) start_run();
    return;
  }
  if (!s_paused) do_jump_now();
}

static void down_handler(ClickRecognizerRef rec, void *ctx) {
  (void)rec; (void)ctx;
  if (s_gs != GS_RUN || s_paused) return;
  s_sliding_held = true;
  if (s_jumping) {
    if (s_vy < 260) s_vy = 260;  // fast fall into slide
  } else {
    s_sliding = true;
  }
}

static void down_release_handler(ClickRecognizerRef rec, void *ctx) {
  (void)rec; (void)ctx;
  s_sliding_held = false;
  if (s_gs == GS_RUN && !s_jumping) s_sliding = false;
}

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_raw_click_subscribe(BUTTON_ID_SELECT, select_handler, NULL, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, down_handler, down_release_handler, NULL);
  // BACK keeps default behavior (exit app)
}

// ---------------------------------------------------------------- update

static void check_collisions(void) {
  int16_t px0 = PLAYER_GX - PLAYER_GW / 2;
  int16_t px1 = PLAYER_GX + PLAYER_GW / 2;
  int16_t ph = s_sliding ? PLAYER_SLIDE_H : PLAYER_STAND_H;
  int16_t py0 = s_py - ph;

  // gems
  for (int i = 0; i < MAX_GEMS; i++) {
    Gem *g = &s_gems[i];
    if (!g->active) continue;
    if (g->x + 5 > px0 - 6 && g->x - 5 < px1 + 6 && g->y + 5 > py0 && g->y - 5 < s_py) {
      g->active = false;
      s_gem_streak++;
      s_streak_timer = 2600;
      s_score += 10 * s_gem_streak;
      if (s_gem_streak >= 2) vibes_short_pulse();
    }
  }

  for (int i = 0; i < MAX_OBST; i++) {
    Obst *o = &s_obst[i];
    if (!o->active) continue;
    if (o->kind == 0) {
      int16_t x0 = o->x, x1 = o->x + o->count * 11;
      if (x1 > px0 && x0 < px1 && s_py > GROUND_Y - 12) {
        goto died;
      }
    } else {
      int16_t by = o->hover_y +
        (int16_t)(3 * sin_lookup(o->phase * TRIG_MAX_ANGLE / 100) / TRIG_MAX_ANGLE);
      if (o->x + 10 > px0 && o->x - 10 < px1 &&
          by + 7 > py0 && by - 7 < s_py) {
        goto died;
      }
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
  // ambient: twinkle + shooting stars on ready & run
  for (int i = 0; i < STARS_N; i++) {
    if (rand() % 100 < 2) s_star_tw[i] = !s_star_tw[i];
  }
  if ((s_gs == GS_READY || s_gs == GS_RUN) && rand() % 1000 < 8) {
    spawn_trip();
  }
  for (int i = 0; i < MAX_TRIP; i++) {
    ShootingStar *t = &s_trips[i];
    if (!t->active) continue;
    t->y += t->vy * ms / 1000;
    t->x -= 1;
    if (t->y > 108) t->active = false;
  }

  if (s_gs == GS_READY) {
    s_ready_pulse += ms;
    s_scroll_near += (NEAR_SPEED * ms) / 1000;
    if (s_scroll_near >= PATTERN_W) s_scroll_near -= PATTERN_W;
    return;
  }

  if (s_gs == GS_DEAD) {
    s_dead_ms += ms;
    if (s_new_best_flash > 0) s_new_best_flash -= ms;
    return;
  }

  if (s_paused) return; // v1.1.0: world frozen, ambient keeps breathing

  int32_t sp = speed_now();

  s_scroll_far += (FAR_SPEED * ms) / 1000;
  s_scroll_mid += (MID_SPEED * ms) / 1000;
  s_scroll_near += (NEAR_SPEED * ms) / 1000;
  if (s_scroll_far >= PATTERN_W) s_scroll_far -= PATTERN_W;
  if (s_scroll_mid >= PATTERN_W) s_scroll_mid -= PATTERN_W;
  if (s_scroll_near >= PATTERN_W) s_scroll_near -= PATTERN_W;

  s_dist += (sp * ms) / 1000;
  s_score = s_dist / 4;
  s_run_ms += ms;

  // physics
  s_vy += (int16_t)(GRAVITY * ms / 1000);
  if (s_vy > TERM_VEL) s_vy = TERM_VEL;
  s_py += (int16_t)(s_vy * ms / 1000);
  if (s_py >= GROUND_Y) {
    s_py = GROUND_Y;
    s_vy = 0;
    if (s_jumping) {
      s_jumping = false;
      s_jumps = 0;
      s_sliding = s_sliding_held; // fast-fall lands into slide if held
    }
  }

  if (s_streak_timer > 0) {
    s_streak_timer -= ms;
    if (s_streak_timer <= 0) s_gem_streak = 0;
  }

  for (int i = 0; i < MAX_OBST; i++) {
    Obst *o = &s_obst[i];
    if (!o->active) continue;
    o->x -= (int16_t)(sp * ms / 1000);
    if (o->kind == 1) o->phase = (o->phase + ms / 16) % 100;
    if (o->x < -40) o->active = false;
  }
  obst_spawn();

  for (int i = 0; i < MAX_GEMS; i++) {
    Gem *g = &s_gems[i];
    if (!g->active) continue;
    g->x -= (int16_t)(sp * ms / 1000);
    if (g->x < -12) g->active = false;
  }

  check_collisions();
}

// ---------------------------------------------------------------- render

static void draw_bg(GContext *ctx) {
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, s_screen, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorLiberty);
  graphics_fill_rect(ctx, GRect(0, 66, SCREEN_W, 20), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, C_BG2);
  graphics_fill_rect(ctx, GRect(0, 86, SCREEN_W, 26), 0, GCornerNone);

  for (int i = 0; i < STARS_N; i++) {
    graphics_context_set_fill_color(ctx, s_star_tw[i] ? C_STAR : C_DIM);
    int16_t s = s_star_tw[i] ? 2 : 1;
    graphics_fill_rect(ctx, GRect(s_star_x[i], s_star_y[i], s, s), 0, GCornerNone);
  }

  graphics_context_set_stroke_width(ctx, 1);
  for (int i = 0; i < MAX_TRIP; i++) {
    ShootingStar *t = &s_trips[i];
    if (!t->active) continue;
    graphics_context_set_stroke_color(ctx, C_STAR);
    graphics_draw_line(ctx, GPoint(t->x, t->y), GPoint(t->x + 3, t->y - t->len));
    graphics_context_set_fill_color(ctx, C_STAR);
    graphics_fill_rect(ctx, GRect(t->x, t->y, 2, 2), 0, GCornerNone);
  }

  // moon
  graphics_context_set_fill_color(ctx, GColorLightGray);
  gpath_move_to(p_moon, GPoint(172, 30));
  gpath_draw_filled(ctx, p_moon);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(167, 24, 4, 3), 1, GCornerNone);
  graphics_fill_rect(ctx, GRect(174, 33, 3, 3), 1, GCornerNone);
  graphics_fill_rect(ctx, GRect(169, 35, 2, 2), 1, GCornerNone);

  // mountain layers (2px column raster for scroll)
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  for (int x = 0; x < SCREEN_W; x += 2) {
    int16_t y = ridge_y_at(s_far, (int16_t)((x + s_scroll_far) % PATTERN_W));
    graphics_fill_rect(ctx, GRect(x, y, 2, 120 - y), 0, GCornerNone);
  }
  graphics_context_set_fill_color(ctx, C_MT_MID);
  for (int x = 0; x < SCREEN_W; x += 2) {
    int16_t y = ridge_y_at(s_mid, (int16_t)((x + s_scroll_mid) % PATTERN_W));
    graphics_fill_rect(ctx, GRect(x, y, 2, 136 - y), 0, GCornerNone);
  }
  graphics_context_set_fill_color(ctx, C_MT_NEAR);
  for (int x = 0; x < SCREEN_W; x += 2) {
    int16_t y = ridge_y_at(s_near, (int16_t)((x + s_scroll_near) % PATTERN_W));
    graphics_fill_rect(ctx, GRect(x, y, 2, 156 - y), 0, GCornerNone);
  }
}

static void draw_ground(GContext *ctx) {
  int16_t bot = s_screen.size.h; // v1.1.0: real screen height (268 on emery)
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, FLOOR_Y, SCREEN_W, bot - FLOOR_Y), 0, GCornerNone);

  graphics_context_set_stroke_color(ctx, C_NEON);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(0, GROUND_Y), GPoint(SCREEN_W, GROUND_Y));
  graphics_context_set_stroke_color(ctx, C_MAGENTA);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(0, GROUND_Y + 4), GPoint(SCREEN_W, GROUND_Y + 4));

  // perspective grid
  graphics_context_set_stroke_color(ctx, C_GRID);
  int32_t off = s_dist % 24;
  for (int i = -1; i < 10; i++) {
    int16_t x0 = (int16_t)(i * 24 - off);
    int16_t x1 = 100 + (x0 - 100) * 2;
    graphics_draw_line(ctx, GPoint(x0, FLOOR_Y), GPoint(x1, bot));
  }
  for (int16_t y = FLOOR_Y + 6; y < bot; y += 9) {
    graphics_draw_line(ctx, GPoint(0, y), GPoint(SCREEN_W, y));
  }
}

static void draw_player(GContext *ctx) {
  int16_t cx = PLAYER_GX;
  int16_t feet = s_py;

  graphics_context_set_fill_color(ctx, C_PLAYER);
  if (s_gs == GS_DEAD) {
    gpath_move_to(p_run_b, GPoint(cx, feet));
    gpath_draw_filled(ctx, p_run_b);
    graphics_context_set_fill_color(ctx, C_P_ACC);
    graphics_fill_rect(ctx, GRect(cx - 3, feet - 12, 6, 3), 0, GCornerNone);
    return;
  }

  if (s_sliding && !s_jumping) {
    gpath_move_to(p_slide, GPoint(cx, feet));
    gpath_draw_filled(ctx, p_slide);
    graphics_context_set_fill_color(ctx, C_P_ACC);
    graphics_fill_rect(ctx, GRect(cx + 4, feet - 8, 6, 2), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, C_BG);
    graphics_fill_rect(ctx, GRect(cx + 8, feet - 8, 2, 2), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, GColorShockingPink);
    graphics_fill_rect(ctx, GRect(cx + 12, feet - 3, 2, 2), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, C_MAGENTA);
    graphics_fill_rect(ctx, GRect(cx - 22, feet - 4, 10, 2), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(cx - 14, feet - 5, 8, 3), 0, GCornerNone);
  } else {
    bool frameB = (s_run_ms / RUN_MS) % 2 == 1;
    GPath *body = frameB ? p_run_b : p_run_a;
    gpath_move_to(body, GPoint(cx, feet));
    gpath_draw_filled(ctx, body);
    graphics_context_set_fill_color(ctx, C_P_ACC);
    graphics_fill_rect(ctx, GRect(cx + 3, feet - 9, 5, 4), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, C_BG);
    graphics_fill_rect(ctx, GRect(cx + (frameB ? 6 : 5), feet - 9, 2, 4), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, GColorShockingPink);
    graphics_fill_rect(ctx, GRect(cx - 8, feet - 13, 1, 2), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(cx + 7, feet - 13, 1, 2), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(cx + 11, feet - 5, 1, 2), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, C_MAGENTA);
    if (frameB) {
      graphics_fill_rect(ctx, GRect(cx - 15, feet - 7, 5, 3), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx - 19, feet - 6, 5, 2), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx - 23, feet - 8, 4, 2), 0, GCornerNone);
    } else {
      graphics_fill_rect(ctx, GRect(cx - 15, feet - 6, 5, 3), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx - 19, feet - 7, 5, 2), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx - 23, feet - 5, 4, 2), 0, GCornerNone);
    }
    graphics_context_set_fill_color(ctx, C_PLAYER);
    if (!s_jumping) {
      if (frameB) {
        graphics_fill_rect(ctx, GRect(cx - 9, feet - 4, 6, 4), 1, GCornerNone);
        graphics_fill_rect(ctx, GRect(cx + 4, feet - 3, 5, 3), 1, GCornerNone);
      } else {
        graphics_fill_rect(ctx, GRect(cx - 8, feet - 3, 5, 3), 1, GCornerNone);
        graphics_fill_rect(ctx, GRect(cx + 5, feet - 4, 6, 4), 1, GCornerNone);
      }
    } else {
      graphics_fill_rect(ctx, GRect(cx - 7, feet - 4, 5, 4), 1, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx + 3, feet - 4, 5, 4), 1, GCornerNone);
    }
  }
}

static void draw_obst(GContext *ctx) {
  for (int i = 0; i < MAX_OBST; i++) {
    Obst *o = &s_obst[i];
    if (!o->active) continue;
    if (o->kind == 0) {
      for (int k = 0; k < o->count; k++) {
        int16_t x = o->x + k * 11;
        gpath_move_to(p_spike, GPoint(x + 5, GROUND_Y));
        graphics_context_set_fill_color(ctx, C_SPIKE);
        gpath_draw_filled(ctx, p_spike);
        graphics_context_set_fill_color(ctx, GColorShockingPink);
        graphics_fill_rect(ctx, GRect(x + 4, GROUND_Y - 13, 2, 2), 0, GCornerNone);
      }
    } else {
      int16_t by = o->hover_y +
        (int16_t)(3 * sin_lookup(o->phase * TRIG_MAX_ANGLE / 100) / TRIG_MAX_ANGLE);
      gpath_move_to(p_drone, GPoint(o->x, by));
      graphics_context_set_fill_color(ctx, C_DRONE);
      gpath_draw_filled(ctx, p_drone);
      graphics_context_set_fill_color(ctx, GColorBlack);
      graphics_fill_rect(ctx, GRect(o->x - 3, by - 3, 6, 2), 0, GCornerNone);
      graphics_context_set_stroke_color(ctx, GColorChromeYellow);
      graphics_context_set_stroke_width(ctx, 1);
      graphics_draw_line(ctx, GPoint(o->x - 12, by - 7), GPoint(o->x + 12, by - 7));
    }
  }
}

static void draw_gems(GContext *ctx) {
  for (int i = 0; i < MAX_GEMS; i++) {
    Gem *g = &s_gems[i];
    if (!g->active) continue;
    gpath_move_to(p_gem, GPoint(g->x, g->y));
    graphics_context_set_fill_color(ctx, C_GEM);
    gpath_draw_filled(ctx, p_gem);
  }
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
                     GRect(0, 62, SCREEN_W, 34), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, C_CYAN);
  graphics_draw_text(ctx, "UP jump 2x\nDOWN slide", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, 118, SCREEN_W, 36), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  if ((s_ready_pulse / 500) % 2 == 0) {
    graphics_context_set_text_color(ctx, C_MAGENTA);
    graphics_draw_text(ctx, "PRESS UP TO RUN", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(0, 150, SCREEN_W, 20), GTextOverflowModeWordWrap,
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
    graphics_context_set_text_color(ctx, on ? GColorShockingPink : C_MAGENTA);
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

static void draw_pause(GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 88, SCREEN_W, 84), 0, GCornerNone);
  graphics_context_set_text_color(ctx, C_CYAN);
  graphics_draw_text(ctx, "PAUSE", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, 94, SCREEN_W, 34), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  if ((s_ready_pulse / 500) % 2 == 0) {
    graphics_context_set_text_color(ctx, C_DIM);
    graphics_draw_text(ctx, "hold UP", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(0, 132, SCREEN_W, 18), GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
  }
}

static void field_update(Layer *layer, GContext *ctx) {
  (void)layer;
  draw_bg(ctx);
  draw_ground(ctx);
  draw_gems(ctx);
  draw_obst(ctx);
  draw_player(ctx);
  draw_hud(ctx);
  if (s_gs == GS_READY) draw_ready(ctx);
  if (s_gs == GS_DEAD) draw_dead(ctx);
  if (s_paused && s_gs == GS_RUN) draw_pause(ctx);
}

// ---------------------------------------------------------------- app

static AppTimer *s_gt;

static void gtick(void *data) {
  (void)data;
  update(40);
  if (s_field) layer_mark_dirty(s_field);
  s_gt = app_timer_register(40, gtick, NULL);
}

static void win_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_screen = layer_get_frame(root);
  s_field = layer_create(s_screen);
  layer_set_update_proc(s_field, field_update);
  layer_add_child(root, s_field);
}

static void win_unload(Window *w) {
  (void)w;
  if (s_gt) app_timer_cancel(s_gt);
  layer_destroy(s_field);
  paths_free();
}

static void init(void) {
  srand((unsigned)time(NULL));
  paths_init();
  s_best = persist_exists(0) ? persist_read_int(0) : 0;
  s_gs = GS_READY;
  layers_reset();
  stars_reset();
  game_reset();

  s_win = window_create();
  window_set_background_color(s_win, C_BG);
  window_set_window_handlers(s_win, (WindowHandlers){
    .load = win_load, .unload = win_unload
  });
  window_set_click_config_provider(s_win, click_config);
  window_stack_push(s_win, true);
}

int main(void) {
  init();
  s_gt = app_timer_register(40, gtick, NULL);
  app_event_loop();
  return 0;
}
