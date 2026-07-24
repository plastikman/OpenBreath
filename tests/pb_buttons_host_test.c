// SPDX-License-Identifier: MIT
// Host unit test for the pure button state machine (no ESP/FreeRTOS deps).
#include "pb_buttons_sm.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

// LOW == pressed for every Panda button.
#define PRESSED 0
#define IDLE    1

static pb_btn_sm_t make(int seed_level)
{
    pb_btn_sm_t b = { .active_low = true };
    pb_buttons_sm_seed(&b, seed_level);
    return b;
}

// A transition is accepted after exactly the configured number of stable
// sample intervals, not one poll later.
static void test_exact_debounce_timing(void)
{
    pb_btn_sm_t b = make(IDLE);
    CHECK(pb_buttons_sm_step(&b, PRESSED) == PB_BTN_EV_NONE);
    CHECK(!b.pressed);
    CHECK(pb_buttons_sm_step(&b, PRESSED) == PB_BTN_EV_NONE);
    CHECK(!b.pressed);
    CHECK(pb_buttons_sm_step(&b, PRESSED) == PB_BTN_EV_NONE);
    CHECK(b.pressed);
}

// Feed a level for n ticks, returning the single non-NONE event if exactly one
// occurred, asserting there was at most one.
static pb_btn_event_t feed(pb_btn_sm_t *b, int level, int ticks)
{
    pb_btn_event_t seen = PB_BTN_EV_NONE;
    for (int i = 0; i < ticks; ++i) {
        pb_btn_event_t ev = pb_buttons_sm_step(b, level);
        if (ev != PB_BTN_EV_NONE) {
            CHECK(seen == PB_BTN_EV_NONE);   // no more than one event per feed
            seen = ev;
        }
    }
    return seen;
}

// A clean short press: settle low, hold briefly (< long threshold), release.
static void test_short_press_on_release(void)
{
    pb_btn_sm_t b = make(IDLE);
    CHECK(feed(&b, PRESSED, 10) == PB_BTN_EV_NONE);   // held 100 ms, no event yet
    CHECK(feed(&b, IDLE, 5) == PB_BTN_EV_SHORT);       // fires exactly once on release
    // Fully released now: no further events.
    CHECK(feed(&b, IDLE, 10) == PB_BTN_EV_NONE);
}

// A long press fires once at the threshold and suppresses the trailing short.
static void test_long_press_fires_once_no_trailing_short(void)
{
    pb_btn_sm_t b = make(IDLE);
    // 1 change tick + 2 debounce ticks establish the press, then the hold
    // counter needs 200 ticks to reach 2000 ms -> LONG at tick 203.
    pb_btn_event_t ev = feed(&b, PRESSED, 203);
    CHECK(ev == PB_BTN_EV_LONG);
    // Keep holding: no repeat.
    CHECK(feed(&b, PRESSED, 50) == PB_BTN_EV_NONE);
    // Release: the short is suppressed.
    CHECK(feed(&b, IDLE, 10) == PB_BTN_EV_NONE);
}

// Contact bounce shorter than the debounce window produces nothing.
static void test_bounce_produces_no_event(void)
{
    pb_btn_sm_t b = make(IDLE);
    // Flip level every tick for a while: settle never elapses, no state change.
    for (int i = 0; i < 20; ++i)
        CHECK(pb_buttons_sm_step(&b, i & 1) == PB_BTN_EV_NONE);
    // A single-tick glitch to low then back is also absorbed.
    CHECK(pb_buttons_sm_step(&b, PRESSED) == PB_BTN_EV_NONE);
    CHECK(pb_buttons_sm_step(&b, IDLE) == PB_BTN_EV_NONE);
    CHECK(feed(&b, IDLE, 5) == PB_BTN_EV_NONE);
}

// A button held low at startup is ignored until it physically releases: no
// SHORT on release, no LONG while held.
static void test_held_at_startup_is_ignored(void)
{
    pb_btn_sm_t b = make(PRESSED);
    CHECK(feed(&b, PRESSED, 300) == PB_BTN_EV_NONE);   // no LONG despite a long hold
    CHECK(feed(&b, IDLE, 5) == PB_BTN_EV_NONE);         // no SHORT on release
    // After releasing, a fresh press behaves normally.
    CHECK(feed(&b, PRESSED, 10) == PB_BTN_EV_NONE);
    CHECK(feed(&b, IDLE, 5) == PB_BTN_EV_SHORT);
}

// Two buttons stepped on the same ticks are fully independent: A gets a short
// press-and-release while C is held long, and each produces its own one event.
static void test_two_buttons_independent(void)
{
    pb_btn_sm_t a = make(IDLE);
    pb_btn_sm_t c = make(IDLE);
    pb_btn_event_t a_ev = PB_BTN_EV_NONE, c_ev = PB_BTN_EV_NONE;

    for (int i = 0; i < 220; ++i) {
        int a_level = (i >= 5 && i < 15) ? PRESSED : IDLE;  // brief tap, releases early
        pb_btn_event_t ea = pb_buttons_sm_step(&a, a_level);
        pb_btn_event_t ec = pb_buttons_sm_step(&c, PRESSED); // held throughout
        if (ea != PB_BTN_EV_NONE) { CHECK(a_ev == PB_BTN_EV_NONE); a_ev = ea; }
        if (ec != PB_BTN_EV_NONE) { CHECK(c_ev == PB_BTN_EV_NONE); c_ev = ec; }
    }
    CHECK(a_ev == PB_BTN_EV_SHORT);   // A's tap
    CHECK(c_ev == PB_BTN_EV_LONG);    // C's hold, unaffected by A
}

// Active-high polarity: pressed == electrical high.
static void test_active_high_polarity(void)
{
    pb_btn_sm_t b = { .active_low = false };
    pb_buttons_sm_seed(&b, 0);                 // idle low for active-high
    CHECK(feed(&b, 1, 10) == PB_BTN_EV_NONE);  // press == high
    CHECK(feed(&b, 0, 5) == PB_BTN_EV_SHORT);
}

int main(void)
{
    test_exact_debounce_timing();
    test_short_press_on_release();
    test_long_press_fires_once_no_trailing_short();
    test_bounce_produces_no_event();
    test_held_at_startup_is_ignored();
    test_two_buttons_independent();
    test_active_high_polarity();
    puts("pb_buttons host tests: PASS");
    return 0;
}
