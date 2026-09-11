/*
 * DICE FIVE — roll 5 dice at once, for Pebble Time 2 (emery)
 * by ClawBro for Daniel, 2026
 *
 * Neon dice table. SELECT / UP / DOWN = roll.
 * Dice tumble with a staggered settle, pips glow cyan on dark red
 * dice, total shows below. Best total + roll count are persisted.
 */

#include <pebble.h>
#include <stdlib.h>

// ---------------------------------------------------------------- config

#define SCREEN_W 200
#define SCREEN_H 228

#define DICE_N 5
#define DICE_SIZE 34
#define GAP 8

#define TUMBLE_MS 40          // animation tick
#define SETTLE_BASE 300       // first die settles after this
#define SETTLE_STEP 130       // each next die + this

#define COL_BG      GColorBlack
#define COL_DIE     GColorDarkCandyAppleRed
#define COL_DIE_OFF GColorOxfordBlue
#define COL_PIP     GColorElectricBlue
#define COL_TXT     GColorWhite
#define COL_ACCENT  GColorVividCerulean

// ---------------------------------------------------------------- state

static Window *s_win;
static Layer *s_field;
static AppTimer *s_timer;

static uint8_t s_face[DICE_N];      // final 1..6
static uint8_t s_shown[DICE_N];     // displayed (tumbling) face
static bool s_settled[DICE_N];
static uint16_t s_tick_n;            // ticks since roll start
static bool s_rolling;
static int32_t s_best;
static int32_t s_roll_count;

// pip grid offsets on a 3x3 lattice, center of die is (0,0)
static const int8_t PX[9] = {-9, 0, 9, -9, 0, 9, -9, 0, 9};
static const int8_t PY[9] = {-9, -9, -9, 0, 0, 0, 9, 9, 9};

// explicit pip slot lists per face, -1 terminated
static const int8_t *SLOTS[7] = {
  NULL,                                  // 0
  (const int8_t[]){4, -1},               // 1
  (const int8_t[]){0, 8, -1},            // 2
  (const int8_t[]){0, 4, 8, -1},         // 3
  (const int8_t[]){0, 2, 6, 8, -1},      // 4
  (const int8_t[]){0, 2, 4, 6, 8, -1},   // 5
  (const int8_t[]){0, 2, 3, 5, 6, 8, -1} // 6
};

// ---------------------------------------------------------------- helpers

static void draw_pips(GContext *ctx, GRect r, uint8_t face, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = r.origin.x + r.size.w / 2;
  int cy = r.origin.y + r.size.h / 2;
  if (face < 1 || face > 6) face = 1;
  for (int i = 0; i < 9 && SLOTS[face][i] != -1; i++) {
    int s = SLOTS[face][i];
    graphics_fill_circle(ctx, GPoint(cx + PX[s], cy + PY[s]), 3);
  }
}

static GRect die_rect(int idx) {
  // layout: row0 = dice 0,1,2 ; row1 = dice 3,4
  int row, col, row_n;
  if (idx < 3) { row = 0; col = idx; row_n = 3; }
  else { row = 1; col = idx - 3; row_n = 2; }
  int row_w = row_n * DICE_SIZE + (row_n - 1) * GAP;
  int x = (SCREEN_W - row_w) / 2 + col * (DICE_SIZE + GAP);
  int y = 50 + row * (DICE_SIZE + GAP + 10);
  return GRect(x, y, DICE_SIZE, DICE_SIZE);
}

static int total(void) {
  int t = 0;
  for (int i = 0; i < DICE_N; i++) t += s_face[i];
  return t;
}

// ---------------------------------------------------------------- drawing

static void update_field(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, COL_BG);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // title
  graphics_context_set_text_color(ctx, COL_ACCENT);
  graphics_draw_text(ctx, "DICE FIVE",
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(0, 8, SCREEN_W, 22), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);

  // dice
  for (int i = 0; i < DICE_N; i++) {
    GRect r = die_rect(i);
    graphics_context_set_fill_color(ctx, s_settled[i] ? COL_DIE : COL_DIE_OFF);
    graphics_context_set_stroke_color(ctx, s_settled[i] ? COL_PIP : GColorCadetBlue);
    graphics_fill_rect(ctx, r, 6, GCornersAll);
    graphics_draw_round_rect(ctx, r, 6);

    GColor pip = s_settled[i] ? COL_PIP : GColorDarkGray;
    draw_pips(ctx, r, s_shown[i], pip);
  }

  // total
  static char total_buf[24];
  if (s_rolling) {
    snprintf(total_buf, sizeof(total_buf), "...");
  } else {
    snprintf(total_buf, sizeof(total_buf), "SUM %d", total());
  }
  graphics_context_set_text_color(ctx, COL_TXT);
  graphics_draw_text(ctx, total_buf,
                     fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, 150, SCREEN_W, 34), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);

  // footer
  static char foot_buf[48];
  snprintf(foot_buf, sizeof(foot_buf), "best %d  rolls %d",
           (int)s_best, (int)s_roll_count);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, foot_buf,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, 194, SCREEN_W, 20), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
}

// ---------------------------------------------------------------- animation

static void tick(void *data) {
  (void)data;
  uint32_t now = (uint32_t)s_tick_n * TUMBLE_MS;
  s_tick_n++;
  bool all_done = true;

  for (int i = 0; i < DICE_N; i++) {
    if (!s_settled[i]) {
      uint32_t settle_at = SETTLE_BASE + i * SETTLE_STEP;
      if (now >= settle_at) {
        s_settled[i] = true;
        s_shown[i] = s_face[i];
      } else {
        s_shown[i] = 1 + (rand() % 6);
        all_done = false;
      }
    }
  }

  layer_mark_dirty(s_field);

  if (!all_done) {
    s_timer = app_timer_register(TUMBLE_MS, tick, NULL);
  } else {
    s_rolling = false;
    int t = total();
    if (t > s_best) {
      s_best = t;
      persist_write_int(1, s_best);
    }
    s_roll_count++;
    persist_write_int(2, s_roll_count);
    vibes_double_pulse();
    layer_mark_dirty(s_field);
  }
}

static void roll(void) {
  if (s_rolling) return;
  s_rolling = true;
  s_tick_n = 0;
  for (int i = 0; i < DICE_N; i++) {
    s_face[i] = 1 + (rand() % 6);
    s_settled[i] = false;
  }
  vibes_short_pulse();
  s_timer = app_timer_register(TUMBLE_MS, tick, NULL);
}

// ---------------------------------------------------------------- input

static void any_click(ClickRecognizerRef ref, void *ctx) { roll(); (void)ref; (void)ctx; }

static void click_cfg(void *ctx) {
  (void)ctx;
  window_single_click_subscribe(BUTTON_ID_SELECT, any_click);
  window_single_click_subscribe(BUTTON_ID_UP, any_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, any_click);
}

// ---------------------------------------------------------------- lifecycle

static void load(Window *win) {
  s_win = win;
  Layer *root = window_get_root_layer(win);
  s_field = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_field, update_field);
  layer_add_child(root, s_field);

  if (persist_exists(1)) s_best = persist_read_int(1);
  if (persist_exists(2)) s_roll_count = persist_read_int(2);

  // seed the dice RNG from wall clock (second-level) + a fast counter,
  // so two launches within the same second still diverge
  uint16_t t_hi, t_ms;
  time_ms(&t_hi, &t_ms);
  srand((unsigned int)time(NULL) ^ (unsigned int)t_ms ^ (unsigned int)(uintptr_t)&s_timer);
  for (int i = 0; i < DICE_N; i++) {
    s_face[i] = 1 + (rand() % 6);
    s_shown[i] = s_face[i];
    s_settled[i] = true;
  }
  s_rolling = false;
}

static void unload(Window *win) {
  if (s_timer) app_timer_cancel(s_timer);
  layer_destroy(s_field);
}

static void init(void) {
  Window *win = window_create();
  window_set_background_color(win, COL_BG);
  window_set_click_config_provider(win, click_cfg);
  window_set_window_handlers(win, (WindowHandlers){
    .load = load, .unload = unload});
  window_stack_push(win, true);
}

static void deinit(void) {
  window_destroy(s_win);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
