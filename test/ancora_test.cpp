// ancora_test.cpp
// Tests for the driver side code that the logic dll runs on every stick update:
// edge stretch, edge measuring, edge learning during play, whole x/y pairs, the
// learned edge bookkeeping, the moving average, the output order, the thumb gate
// and ancora.ini (also that the shipped file is the default one). Plain console program, no
// SteamVR, no Windows API. build_driver.bat builds and runs it from the repo root
// before it deploys the logic dll, so a failing test stops the build.
//   ancora_test.exe        run every test, exit code 0 = all pass
//
// Numbers the README promises (full output at 96 % of the reach, the gate fallback
// after a quarter second, the 0.10 wear floor, 25 short pushes, ...) are written
// here as literals, not taken from the headers' constants, so a changed constant
// fails a test instead of moving the test along with it. Every scenario starts
// from fresh state, so one failure does not cascade into the next check.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include "../driver/src/ancora_average.h"
#include "../driver/src/ancora_center.h"
#include "../driver/src/ancora_edge.h"
#include "../driver/src/ancora_learn.h"
#include "../driver/src/ancora_output.h"
#include "../driver/src/ancora_config.h"
#include <fstream>
#include <sstream>

namespace {

int g_fails = 0, g_count = 0;

// what: the claim; fmt (optional): printf format of the values, printed on a failure
void check(bool ok, const char* what, const char* fmt = nullptr, ...) {
    ++g_count;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) return;
    ++g_fails;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        std::printf("     ");
        std::vprintf(fmt, ap);
        std::printf("\n");
        va_end(ap);
    }
}

bool near(double got, double want, double tol) { return std::fabs(got - want) <= tol; }

// one value against the expected one, both printed on a failure
void check_near(double got, double want, double tol, const char* what) {
    check(near(got, want, tol), what, "got %.6f, want %.6f (tolerance %g)", got, want, tol);
}

std::string g_root;                     // repo root, from the exe's own path (main)

const float kPi = 3.14159265f;
float rad(float deg) { return deg * kPi / 180; }

// simulated worn stick: reaches 0.999, dipping to 0.65 at 135 deg (forward left)
float reach_at(float deg) {
    float d = std::fabs(deg - 135.0f);
    if (d > 180) d = 360 - d;
    return d >= 90 ? 0.999f : 0.65f + (0.999f - 0.65f) * d / 90.0f;
}

const int k135 = kEdgeSectors * 3 / 8;   // sector of 135 deg

EdgeReach flat(float r) {
    EdgeReach e;
    e.on = true;
    for (int k = 0; k < kEdgeSectors; ++k) e.r[k] = r;
    return e;
}

// a learner with its own furthest-reach record, as the logic dll keeps one per hand
struct Learn {
    EdgeLearner L;
    EdgeReach best;
    bool feed(EdgeReach& e, float x, float y, double t) {
        if (!best.on) best = e;
        return L.feed(e, best, x, y, t);
    }
};

void test_average() {
    static StickAverage av;
    for (int k = 0; k <= 1000; ++k) av.add(k <= 500 ? 0.0f : 1.0f, 0.5f, (double)k, 100.0);
    check(std::fabs(av.avg_x() - 1.0f) < 1e-4 && std::fabs(av.avg_y() - 0.5f) < 1e-4, "average settles on a steady value");
    av.reset();
    for (int k = 0; k <= 500; ++k) av.add(0.0f, 0.0f, (double)k, 100.0);
    for (int k = 501; k <= 550; ++k) av.add(1.0f, 0.0f, (double)k, 100.0);
    check_near(av.avg_x(), 0.5, 0.02, "average is half way, half a window after a step");
    av.reset();
    for (int k = 0; k <= 500; ++k) av.add(k > 450 ? 1.0f : 0.0f, 0, k * 1.0, 100.0);
    float fast = av.avg_x();
    av.reset();
    for (int k = 0; k <= 50; ++k) av.add(k > 45 ? 1.0f : 0.0f, 0, k * 10.0, 100.0);
    // 0.07: at 100 a second the 100 ms window holds 11 samples, so 5 of 11 (0.4545)
    // against about 0.515 from the 3 ms steps at 1000 a second
    check(near(fast, av.avg_x(), 0.07), "same result at 1000 and 100 updates per second",
          "1000 a second %.4f, 100 a second %.4f", fast, av.avg_x());
    av.reset();
    for (int k = 0; k <= 200; ++k) av.add(((k / 3) & 1) ? 0.6f : 0.4f, 0, k * 1.0, 100.0);
    check(std::fabs(av.avg_x() - 0.5f) < 0.03f, "average removes fast jitter");
    av.reset();
    check(av.count == 0 && av.avg_x() == 0.0f, "reset empties the average");

    // readings inside one 3 ms step share a sample: the newest of them counts
    av.reset();
    av.add(0.2f, 0, 0.0, 100.0);
    av.add(0.4f, 0, 1.0, 100.0);
    av.add(0.6f, 0, 2.0, 100.0);
    check(av.count == 1 && near(av.avg_x(), 0.6f, 1e-6), "average: three readings within 3 ms are one sample, the newest",
          "count %d, average %.4f", av.count, av.avg_x());
    av.add(0.0f, 0, 3.0, 100.0);
    check(av.count == 2 && near(av.avg_x(), 0.3f, 1e-6), "average: a reading 3 ms later is a new sample",
          "count %d, average %.4f", av.count, av.avg_x());

    // the longest window, 300 ms, at 1000 updates a second: still a true 300 ms average
    av.reset();
    for (int k = 0; k <= 1000; ++k) av.add(0.0f, 0, (double)k, 300.0);
    for (int k = 1001; k <= 1150; ++k) av.add(1.0f, 0, (double)k, 300.0);
    float half = av.avg_x();
    for (int k = 1151; k <= 1310; ++k) av.add(1.0f, 0, (double)k, 300.0);
    check(near(half, 0.5f, 0.02f) && near(av.avg_x(), 1.0f, 1e-4f),
          "average: a 300 ms window at 1000 updates a second is half way after 150 ms, settled after 300 ms",
          "after 150 ms %.4f (want 0.5), after 310 ms %.4f (want 1)", half, av.avg_x());
}

// one turn round the simulated rim at a share of full push, 4 deg a step
void turn(EdgeMeasure& em, float push, float skip_from = 999, float skip_to = -1) {
    for (int k = 0; k < 90; ++k) {
        float deg = k * 4.0f, m = push * reach_at(deg);
        if (deg > skip_from && deg < skip_to) continue;
        em.feed(m * std::cos(rad(deg)), m * std::sin(rad(deg)));
    }
}

void test_measure() {
    EdgeMeasure em;
    turn(em, 1.0f);
    check(!em.done(), "measure: one turn is never enough");
    turn(em, 1.0f);
    EdgeReach e = em.result();
    check(em.done() && e.on, "measure: done once every direction has settled");
    check(edge_shortest(e) == k135 && std::fabs(e.r[k135] - 0.65f) < 0.02f, "measure: shortest reach 0.65 found at 135 deg");
    em = EdgeMeasure{};
    turn(em, 0.6f);
    turn(em, 1.0f);
    check(!em.done(), "measure: a full turn after a sloppy one still needs another");
    turn(em, 1.0f);
    e = em.result();
    check(em.done() && std::fabs(e.r[0] - 0.999f) < 0.01f && std::fabs(e.r[k135] - 0.65f) < 0.02f,
          "measure: settles on the full reach, not the sloppy turn");
    em = EdgeMeasure{};
    for (int k = 0; k < 2000; ++k) {   // jitter across one bin border
        float deg = (k & 1) ? 2.7f : 3.0f;
        em.feed(std::cos(rad(deg)), std::sin(rad(deg)));
    }
    check(em.passes[0] <= 1 && em.passes[1] <= 1, "measure: jitter on a bin border is one pass");
    em = EdgeMeasure{};
    for (int t = 0; t < 3; ++t) turn(em, 1.0f, 115, 155);   // snaps past 115..155 deg
    EdgeReach g = em.result();
    check(!em.done() && em.can_finish() && g.on, "measure: a skipped stretch can be finished with these");
    check(g.r[k135] > 0.65f && g.r[k135] < 0.85f, "measure: the skipped stretch is filled from both sides");
    em = EdgeMeasure{};
    for (int t = 0; t < 3; ++t)
        for (int k = 0; k < 45; ++k) em.feed(std::cos(rad(k * 4.0f)), std::sin(rad(k * 4.0f)));
    check(!em.can_finish() && !em.result().on, "measure: half a circle can not be finished");
    em = EdgeMeasure{};
    for (int k = 0; k < 300; ++k) em.feed(0.9f * std::cos(k * 0.05f), 0.0f);   // left and right only
    check(!em.done() && !em.can_finish(), "measure: never finishes without reaching every direction");
}

void test_map_and_text() {
    EdgeMeasure em;
    turn(em, 1.0f);
    turn(em, 1.0f);
    EdgeReach e = em.result();
    float m = reach_at(135), x = m * std::cos(rad(135)), y = m * std::sin(rad(135));
    check(std::fabs(edge_map(e, x, y, m, 0) - 1.0f) < 0.02f, "map: full push toward the short side gives 1");
    check(std::fabs(edge_map(e, 0.5f, 0, 0.5f, 0) - 0.5f / (e.r[0] * (1 - kEdgeOuter))) < 0.01f &&
              edge_map(e, 0.97f, 0, 0.97f, 0) == 1.0f,
          "map: a full side only gets the outer margin, full a little before the edge");
    check(edge_map(e, x * 0.3f, y * 0.3f, m * 0.3f, 0.25f) == 0.0f, "map: inside the deadzone still 0");
    check(std::fabs(edge_map(e, x, y, m, 0.25f) - 1.0f) < 0.02f, "map: with a deadzone the rim still gives 1");
    EdgeReach none;
    check(std::fabs(edge_map(none, 0.6f, 0, 0.6f, 0.2f) - 0.5f) < 1e-4, "map: edge off, deadzone scaling as before");
    check(edge_map(none, 0.0f, 0, 0.0f, 0.0f) == 0.0f, "map: centre gives 0");

    EdgeReach back = edge_parse(edge_format(e).c_str());
    bool same = back.on;
    for (int k = 0; k < kEdgeSectors; ++k) same = same && std::fabs(back.r[k] - e.r[k]) < 1e-3f;
    check(same, "text: ini round trip");
    check(!edge_parse("").on && !edge_parse("0.9,0.9").on && !edge_parse("abc").on && !edge_parse("0.1,0.1").on,
          "text: short, garbage or too small values are off");
    EdgeReach old = edge_parse("1,1,1,1,1,1,0.6,1,1,1,1,1,1,1,1,1");
    check(old.on && std::fabs(old.r[k135] - 0.6f) < 1e-4f && std::fabs(old.r[k135 + 2] - 0.8f) < 1e-4f && old.r[0] == 1.0f,
          "text: an older 16 value line is read and spread over 64 directions");

    // between two directions the reach is a straight line: 0.6 at 0 deg, 1.0 at 5.625 deg
    EdgeReach two = flat(0.6f);
    two.r[1] = 1.0f;
    const float half_step = 360.0f / 64 / 2;
    check_near(edge_reach(two, std::cos(rad(half_step)), std::sin(rad(half_step))), 0.8, 1e-3,
               "map: half way between two directions the reach is half way between theirs");
    check_near(edge_reach(two, std::cos(rad(half_step / 2)), std::sin(rad(half_step / 2))), 0.7, 1e-3,
               "map: a quarter of the way, a quarter of the difference");

    // README: full output at 96 % of the reach (4 % outer margin), straight below it
    EdgeReach r8 = flat(0.8f);
    check(edge_map(r8, 0.768f, 0, 0.768f, 0) == 1.0f, "map: full output at 96 % of the reach",
          "got %.4f at 0.768 of reach 0.8", edge_map(r8, 0.768f, 0, 0.768f, 0));
    check_near(edge_map(r8, 0.76f, 0, 0.76f, 0), 0.76 / 0.768, 1e-4, "map: just below 96 % of the reach is just below full");
    check_near(edge_map(r8, 0.384f, 0, 0.384f, 0), 0.5, 1e-4, "map: half of 96 % of the reach gives half");

    // a deadzone at or past the reach: the span never shrinks below 0.05 (no division by
    // zero or a negative span, which would turn a push into no output)
    EdgeReach r5 = flat(0.5f);
    check_near(edge_map(r5, 0.62f, 0, 0.62f, 0.6f), 0.4, 1e-4, "map: deadzone past the reach still ramps up over 0.05");
    check(edge_map(r5, 0.66f, 0, 0.66f, 0.6f) == 1.0f && edge_map(r5, 0.6f, 0, 0.6f, 0.6f) == 0.0f,
          "map: deadzone past the reach, full 0.05 past the deadzone, 0 at it");
    check_near(edge_map(r5, 0.53f, 0, 0.53f, 0.5f), 0.6, 1e-4, "map: deadzone equal to the reach ramps up over 0.05");

    // a line longer than 64 values is off; it is read into a 64 value buffer on
    // vrserver's stack, so the parser must stop at 64 (a crash here is that bug)
    std::string line65, line200;
    for (int k = 0; k < 65; ++k) line65 += k ? ",0.9" : "0.9";
    for (int k = 0; k < 200; ++k) line200 += k ? ",0.9" : "0.9";
    check(!edge_parse(line65.c_str()).on && !edge_parse(line200.c_str()).on, "text: 65 or 200 values are off");
    std::string line64;
    for (int k = 0; k < 64; ++k) line64 += k ? ",0.9" : "0.9";
    check(edge_parse(line64.c_str()).on, "text: exactly 64 values are read");

    // values: 0.3 .. 1.5, above 1 read as 1; anything outside is off
    std::string hi = line64.substr(0, line64.size() - 3) + "1.5", over = line64.substr(0, line64.size() - 3) + "1.6",
                low = line64.substr(0, line64.size() - 3) + "0.29", nan = line64.substr(0, line64.size() - 3) + "nan";
    EdgeReach h = edge_parse(hi.c_str());
    check(h.on && h.r[63] == 1.0f, "text: a value of 1.5 is read as 1", "on %d, last %.3f", h.on, h.r[63]);
    check(!edge_parse(over.c_str()).on && !edge_parse(low.c_str()).on && !edge_parse(nan.c_str()).on,
          "text: a value above 1.5, below 0.3 or nan makes the line off");
}

void test_learn() {
    EdgeReach lr = flat(0.6f);
    float lx = 0.9f * std::cos(rad(135)), ly = 0.9f * std::sin(rad(135));
    check(edge_learn(lr, lx, ly) && std::fabs(edge_reach(lr, lx, ly) - 0.9f) < 1e-3f,
          "learn: a further reading raises the reach there to it");
    check(!edge_learn(lr, lx * 0.5f, ly * 0.5f) && std::fabs(edge_reach(lr, lx, ly) - 0.9f) < 1e-3f,
          "learn: a shorter reading never lowers it");
    check(std::fabs(lr.r[0] - 0.6f) < 1e-6f && std::fabs(lr.r[kEdgeSectors / 2] - 0.6f) < 1e-6f,
          "learn: other directions are left alone");
    check(!edge_learn(lr, lx * 1.003f, ly * 1.003f), "learn: tiny shortfalls are ignored");
    EdgeReach off;
    check(!edge_learn(off, 1.0f, 0.0f) && !off.on, "learn: no stretch, nothing learned");

    const float step = 360.0f / kEdgeSectors;
    EdgeReach w = flat(0.64f);
    for (float deg = 110; deg <= 160; deg += 0.25f) {   // strong pushes all round a worn spot at 135
        if (std::fabs(deg - 135) < 2 * step) continue;
        edge_learn(w, 0.99f * std::cos(rad(deg)), 0.99f * std::sin(rad(deg)));
    }
    check(std::fabs(w.r[k135] - 0.64f) < 1e-6f && std::fabs(edge_reach(w, std::cos(rad(135)), std::sin(rad(135))) - 0.64f) < 1e-3f,
          "learn: strong pushes beside a worn spot leave the worn spot alone");
    bool ok = true;
    for (int k = 0; k < kEdgeSectors; ++k) {   // every direction: a reading on its angle raises it to the reading
        EdgeReach t = flat(0.5f);
        float deg = k * step + 0.2f * step;
        ok = ok && edge_learn(t, 0.97f * std::cos(rad(deg)), 0.97f * std::sin(rad(deg))) && std::fabs(t.r[k] - 0.97f) < 1e-6f;
        for (int j = 0; j < kEdgeSectors; ++j) ok = ok && (j == k || t.r[j] == 0.5f);
    }
    check(ok, "learn: a reading near a direction's angle raises that direction only");
    EdgeReach mid = flat(0.5f);
    check(!edge_learn(mid, 0.97f * std::cos(rad(step / 2)), 0.97f * std::sin(rad(step / 2))) && mid.r[0] == 0.5f && mid.r[1] == 0.5f,
          "learn: a reading half way between two directions teaches nothing");

    // a push held at (x, y) for ms milliseconds, a reading every step ms, from t
    auto hold = [](Learn& L, EdgeReach& e, float x, float y, double& t, double ms, double step = 1.0) {
        bool changed = false;
        for (double end = t + ms; t < end; t += step) changed |= L.feed(e, x, y, t);
        return changed;
    };
    auto centre = [](Learn& L, EdgeReach& e, double& t) { L.feed(e, 0, 0, t); t += 50; };

    // glitches, each from a fresh learner and reach
    {
        EdgeReach s = flat(0.6f);
        Learn L;
        double t = 0;
        L.feed(s, 0.5f, 0.0f, t); t += 1;
        L.feed(s, 1.0f, 0.0f, t); t += 1;
        centre(L, s, t);
        check_near(s.r[0], 0.6, 1e-6, "learn: a single glitch at the rim teaches nothing");
    }
    {
        EdgeReach s = flat(0.6f);
        Learn L;
        double t = 0;
        hold(L, s, 0.95f, 0.0f, t, 10);
        centre(L, s, t);
        check_near(s.r[0], 0.6, 1e-6, "learn: a glitch of 10 readings (10 ms) teaches nothing");
    }
    {
        EdgeReach s = flat(0.6f);
        Learn L;
        double t = 0;
        hold(L, s, 0.5f, 0.0f, t, 30);
        hold(L, s, 0.95f, 0.0f, t, 8);
        hold(L, s, 0.5f, 0.0f, t, 30);
        centre(L, s, t);
        check_near(s.r[0], 0.6, 1e-6, "learn: a short spike inside a long push teaches nothing");
    }
    {
        EdgeReach s = flat(0.6f);
        Learn L;
        double t = 0;
        hold(L, s, 0.9f, 0.0f, t, 19);
        check_near(s.r[0], 0.6, 1e-6, "learn: a push held 19 ms raises nothing yet");
        hold(L, s, 0.9f, 0.0f, t, 2);
        check_near(s.r[0], 0.9, 1e-4, "learn: at 20 ms the push raises the reach to it");
    }
    {
        EdgeReach s = flat(0.6f);
        Learn L;
        double t = 0;
        L.feed(s, 0.0f, 0.95f, t); t += 1;
        L.feed(s, 0.0f, -0.95f, t); t += 1;
        centre(L, s, t);
        check(near(edge_reach(s, 0, 1), 0.6, 1e-4) && near(edge_reach(s, 0, -1), 0.6, 1e-4),
              "learn: two readings far apart teach nothing", "up %.4f, down %.4f, want 0.6",
              edge_reach(s, 0, 1), edge_reach(s, 0, -1));
    }
    double t = 0;

    // playing on the simulated worn stick, starting from a measurement that came out short:
    // rolling round the rim, a little slower each turn (about 330 to 140 deg a second)
    EdgeReach play = flat(0.55f);
    Learn P;
    t = 0;
    for (int r = 0; r < 5; ++r)
        for (int k = 0; k < 360; ++k) {
            float deg = k * 1.0f, m = reach_at(deg);
            for (int j = 0; j < 3 + r; ++j, t += 1) P.feed(play, m * std::cos(rad(deg)), m * std::sin(rad(deg)), t);
        }
    float worst = 0;
    for (int k = 0; k < 360; k += 3) {
        float deg = (float)k;
        worst = std::fmax(worst, std::fabs(edge_reach(play, std::cos(rad(deg)), std::sin(rad(deg))) - reach_at(deg)));
    }
    check(worst < 0.03f, "learn: play on a worn stick brings a short measurement to the real edge");
    check(edge_reach(play, std::cos(rad(135)), std::sin(rad(135))) < 0.68f, "learn: the worn spot stays short after play");

    // the stick wears: full pushes right now stop at 0.85, the stored reach says 0.98
    EdgeReach worn = flat(0.98f);
    Learn W;
    t = 0;
    bool lowered = false;
    for (int p = 0; p < 24; ++p) { lowered |= hold(W, worn, 0.85f, 0.0f, t, 200); centre(W, worn, t); }
    check(!lowered && worn.r[0] == 0.98f, "learn: 24 short full pushes lower nothing", "reach %.4f", worn.r[0]);
    lowered = hold(W, worn, 0.85f, 0.0f, t, 200);
    lowered |= W.feed(worn, 0, 0, t); t += 50;
    check(lowered && near(worn.r[0], 0.915, 1e-4) && worn.r[32] == 0.98f,
          "learn: the 25th short full push lowers that direction half way (0.98 -> 0.915), and only it",
          "lowered %d, reach %.4f, opposite %.4f", lowered, worn.r[0], worn.r[32]);
    for (int p = 0; p < 100; ++p) { hold(W, worn, 0.85f, 0.0f, t, 200); centre(W, worn, t); }
    check_near(worn.r[0], 0.88, 1e-4, "learn: repeated wear stops at the floor, 0.10 below the furthest reach seen");

    // lowering goes half way to the furthest short push, not to the last one
    {
        EdgeReach e = flat(0.98f);
        Learn F;
        double tf = 0;
        hold(F, e, 0.90f, 0.0f, tf, 200); centre(F, e, tf);
        for (int p = 1; p < 25; ++p) { hold(F, e, 0.86f, 0.0f, tf, 200); centre(F, e, tf); }
        check_near(e.r[0], 0.94, 1e-4, "learn: one push to 0.90 among pushes to 0.86 lowers to (0.98 + 0.90) / 2");
    }

    // a push that sags at its end counts where it ends, not where it peaked
    {
        EdgeReach e = flat(0.98f);
        Learn D;
        double td = 0;
        for (int p = 0; p < 25; ++p) {
            hold(D, e, 0.95f, 0.0f, td, 30);
            hold(D, e, 0.86f, 0.0f, td, 200);
            centre(D, e, td);
        }
        check_near(e.r[0], 0.92, 1e-4, "learn: a push that peaks at 0.95 and is held at 0.86 counts as 0.86");
    }

    // raising the reach raises the floor with it: after a push to 0.95 from a
    // measured 0.6, wear can lower it to 0.85 at most
    {
        EdgeReach e = flat(0.6f);
        Learn B;
        double tb = 0;
        hold(B, e, 0.95f, 0.0f, tb, 200); centre(B, e, tb);
        check(near(e.r[0], 0.95, 1e-4) && near(B.best.r[0], 0.95, 1e-4), "learn: raising the reach raises the furthest reach",
              "reach %.4f, furthest %.4f, want 0.95", e.r[0], B.best.r[0]);
        for (int p = 0; p < 200; ++p) { hold(B, e, 0.82f, 0.0f, tb, 200); centre(B, e, tb); }
        check_near(e.r[0], 0.85, 1e-4, "learn: after a raise, wear stops at 0.10 below the raised reach");
    }

    // lowering does not depend on the update rate either
    bool lower_rates = true;
    for (double hz : {1000.0, 250.0, 144.0, 120.0, 90.0, 72.0, 60.0}) {
        EdgeReach e = flat(0.98f);
        Learn R;
        double tr = 0;
        for (int p = 0; p < 25; ++p) { hold(R, e, 0.85f, 0.0f, tr, 200, 1000.0 / hz); centre(R, e, tr); }
        if (!near(e.r[0], 0.915, 1e-4)) {
            lower_rates = false;
            std::printf("     at %.0f updates a second: reach %.4f, want 0.915\n", hz, e.r[0]);
        }
    }
    check(lower_rates, "learn: 25 short pushes lower the reach at 1000 down to 60 updates a second");

    EdgeReach keep = flat(0.98f);
    Learn K;
    t = 0;
    for (int p = 0; p < 75; ++p) {
        hold(K, keep, p % 10 == 9 ? 0.975f : 0.85f, 0.0f, t, 200);   // every tenth push reaches the edge
        centre(K, keep, t);
    }
    check(std::fabs(keep.r[0] - 0.98f) < 1e-6f, "learn: a push that still reaches the edge keeps the reach");
    EdgeReach soft = flat(0.98f);
    Learn S;
    t = 0;
    for (int p = 0; p < 75; ++p) { hold(S, soft, 0.6f, 0.0f, t, 200); centre(S, soft, t); }
    check(std::fabs(soft.r[0] - 0.98f) < 1e-6f, "learn: gentle half pushes never lower the reach");

    // a stick reporting slowly: learning must not depend on the update rate
    bool all_rates = true;
    for (double hz : {1000.0, 250.0, 144.0, 120.0, 90.0, 72.0, 60.0}) {
        EdgeReach slow = flat(0.6f);
        Learn R;
        double ts = 0;
        hold(R, slow, 0.95f, 0.0f, ts, 2000, 1000.0 / hz);
        all_rates = all_rates && std::fabs(slow.r[0] - 0.95f) < 1e-3f;
    }
    check(all_rates, "learn: a held push raises the reach at 1000 down to 60 updates a second");
    EdgeReach slow_glitch = flat(0.6f);
    Learn G;
    t = 0;
    hold(G, slow_glitch, 0.5f, 0.0f, t, 200, 1000.0 / 72);
    hold(G, slow_glitch, 0.95f, 0.0f, t, 1000.0 / 72 * 2, 1000.0 / 72);
    hold(G, slow_glitch, 0.5f, 0.0f, t, 200, 1000.0 / 72);
    check(slow_glitch.r[0] <= 0.6f + 1e-4f, "learn: at 72 updates a second, a two reading spike still teaches nothing");

    // a player who always holds 90 % of the current reach: the stretch makes that
    // stronger, the player pushes less, and so on; the floor stops the ratchet
    EdgeReach part = flat(0.98f);
    Learn Q;
    t = 0;
    for (int p = 0; p < 400; ++p) { hold(Q, part, 0.9f * part.r[0], 0.0f, t, 200); centre(Q, part, t); }
    check_near(part.r[0], 0.88, 1e-4, "learn: 400 part pushes that follow the reach down lower it to the floor and stop there");

    // the push starts at 0 deg and is held at 11.25 deg (direction 2): the held
    // direction is lowered, not the one the push started in
    EdgeReach moved = flat(0.98f);
    Learn M;
    t = 0;
    const float held_deg = 2 * 360.0f / kEdgeSectors;
    for (int p = 0; p < 25; ++p) {
        hold(M, moved, 0.85f, 0.0f, t, 5);
        hold(M, moved, 0.85f * std::cos(rad(held_deg)), 0.85f * std::sin(rad(held_deg)), t, 200);
        centre(M, moved, t);
    }
    check(moved.r[0] == 0.98f && moved.r[2] < 0.98f, "learn: wear counts for the direction the push was held in");
}

void test_pairs() {
    StickPair p;
    check(p.update(true, 0.5f) == 0 && p.seq == 0, "pairs: x alone is not a pair yet");
    check(p.update(false, 0.25f) == 1 && p.x == 0.5f && p.y == 0.25f, "pairs: x then y makes one whole pair");
    p.update(true, 0.7f);
    check(p.x == 0.5f && p.y == 0.25f, "pairs: a new x alone does not change the published pair");
    check(p.update(true, 0.8f) == 1 && p.x == 0.7f && p.y == 0.25f, "pairs: x twice in a row, y unchanged, the first x pairs");
    p = StickPair{};
    p.update(false, -0.3f);
    check(p.update(true, 0.1f) == 1 && p.x == 0.1f && p.y == -0.3f, "pairs: y then x works the same");
    uint32_t before = p.seq;
    p.update(true, 0.2f);
    p.update(false, 0.4f);
    check(p.seq == before + 1 && p.x == 0.2f && p.y == 0.4f, "pairs: every whole pair bumps the count once");
    // a stick going round: no published pair ever mixes a new x with an old y
    p = StickPair{};
    bool whole = true;
    for (int k = 0; k < 720; ++k) {
        float x = std::cos(rad(k * 3.0f)), y = std::sin(rad(k * 3.0f));
        p.update(true, x);
        if (p.update(false, y)) whole = whole && p.x == x && p.y == y;
    }
    check(whole, "pairs: a stick going round only ever publishes whole pairs");
}

void test_learn_state() {
    EdgeLearnState st;
    EdgeReach e = flat(0.6f);
    st.text = edge_format(e);
    bool learned = false;
    double t = 1000;
    for (; t < 1040; t += 1) learned |= st.on_pair(e, 0.9f, 0.0f, t);
    check(learned && st.dirty && std::fabs(edge_reach(e, 1, 0) - 0.9f) < 1e-3f, "state: learning marks the edge to save");
    double last = st.last_ms;
    check(!st.save_due(last + kEdgeSaveMs - 1) && st.save_due(last + kEdgeSaveMs), "state: saved only after a quiet time");

    // another key in the file changed before the save: learned reach is kept
    EdgeReach loaded = edge_parse(st.text.c_str());
    st.reload(loaded, e, st.text, EdgeReach{});
    check(st.dirty && std::fabs(edge_reach(loaded, 1, 0) - 0.9f) < 1e-3f, "state: a reload for another key keeps unsaved learning");

    // a new measurement in the file: it wins
    EdgeReach fresh = flat(0.8f);
    std::string ft = edge_format(fresh);
    EdgeReach got = edge_parse(ft.c_str());
    st.reload(got, e, ft, EdgeReach{});
    check(!st.dirty && std::fabs(edge_reach(got, 1, 0) - 0.8f) < 1e-3f, "state: a new measurement replaces unsaved learning");

    // removed in the panel: stays off
    for (t = 2000; t < 2040; t += 1) st.on_pair(got, 0.95f, 0.0f, t);
    EdgeReach removed;
    st.reload(removed, got, "", EdgeReach{});
    check(!st.dirty && !removed.on, "state: removing the stretch is not undone by unsaved learning");

    // saved by the dll itself: the file now holds the learned values
    EdgeReach mine = flat(0.7f);
    st = EdgeLearnState{};
    st.text = edge_format(flat(0.6f));
    std::string saved = edge_format(mine);
    EdgeReach reread = edge_parse(saved.c_str());
    st.reload(reread, mine, saved, EdgeReach{});
    check(!st.dirty && std::fabs(edge_reach(reread, 1, 0) - 0.7f) < 1e-3f, "state: reading back its own save keeps the values");

    EdgeLearnState sb;
    EdgeReach le = flat(0.7f);
    sb.reload(le, le, edge_format(le), flat(0.9f));
    check(sb.best.on && std::fabs(sb.best.r[0] - 0.9f) < 1e-6f, "state: the furthest reach comes from the file");
    EdgeLearnState so;
    EdgeReach lo = flat(0.7f);
    so.reload(lo, lo, edge_format(lo), EdgeReach{});
    check(so.best.on && std::fabs(so.best.r[0] - 0.7f) < 1e-6f, "state: without one in the file, the edge is the furthest reach");

    // a furthest reach in the file below the edge in use is out of date: raised to the edge
    EdgeLearnState sr;
    EdgeReach hi = flat(0.9f);
    EdgeReach base = flat(0.7f);
    base.r[5] = 0.95f;
    sr.reload(hi, hi, edge_format(hi), base);
    check(near(sr.best.r[0], 0.9, 1e-6) && near(sr.best.r[5], 0.95, 1e-6),
          "state: a stale furthest reach below the edge is raised to it, a further one kept",
          "direction 0 %.4f (want 0.9), direction 5 %.4f (want 0.95)", sr.best.r[0], sr.best.r[5]);

    // the stretch removed in the panel: the furthest reach goes with it
    EdgeLearnState sx;
    EdgeReach on = flat(0.8f);
    sx.reload(on, on, edge_format(on), flat(0.9f));
    EdgeReach gone;
    sx.reload(gone, on, "", flat(0.9f));
    check(sx.best.on == false && !gone.on, "state: removing the stretch drops the furthest reach too");
}

void test_config() {
    using namespace ancfg;
    Config c = parse("");
    check(!c.enable && c.hand[0].enable && c.hand[1].enable && c.hand[0].touch_gate && c.hand[1].touch_gate &&
              c.hand[0].deadzone == 0 && !c.hand[0].edge.on && c.hand[0].edge_on && c.language == "auto",
          "config: missing file is off, with both sticks set to be corrected");

    // the shipped file, found from the exe (driver\obj\ancora_test.exe) or the working directory
    std::string ini_path = g_root + "driver/ancora/ancora.ini";
    std::ifstream f(ini_path, std::ios::binary);
    if (!f) { ini_path = "driver/ancora/ancora.ini"; f.open(ini_path, std::ios::binary); }
    std::stringstream ss;
    ss << f.rdbuf();
    bool read_ok = f.good();
    check(read_ok, "config: the shipped ancora.ini can be read", "could not open %s", ini_path.c_str());
    std::string shipped = ss.str(), made = format(Config{});
    auto strip = [](std::string s) { std::string o; for (char ch : s) if (ch != '\r') o += ch; return o; };
    if (read_ok) {
        std::string a = strip(shipped), b = strip(made);
        size_t i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
        check(a == b, "config: the shipped ancora.ini is exactly the default file",
              "%s differs from format(Config{}) at byte %zu", ini_path.c_str(), i);
    }
    Config d = parse(shipped);
    check(d.enable && d.hand[0].enable && d.hand[1].enable, "config: the shipped file corrects both sticks");

    std::string text = "; my note\r\n[other]\r\nleft=0\r\n[Ancora]\r\n  LEFT_deadzone = 0.3 \r\nkeep=me\r\n[tail]\r\nx=1\r\n";
    c = parse(text);
    check(c.hand[0].enable && std::fabs(c.hand[0].deadzone - 0.3f) < 1e-6f,
          "config: keys only from [ancora], any case, trimmed");
    std::string t2 = set(text, "left_deadzone", "0.45");
    check(t2 == "; my note\r\n[other]\r\nleft=0\r\n[Ancora]\r\nleft_deadzone=0.45\r\nkeep=me\r\n[tail]\r\nx=1\r\n",
          "config: changing a key keeps every other line");
    std::string t3 = set(t2, "right", "0");
    check(t3 == "; my note\r\n[other]\r\nleft=0\r\n[Ancora]\r\nleft_deadzone=0.45\r\nkeep=me\r\nright=0\r\n[tail]\r\nx=1\r\n" &&
              !parse(t3).hand[1].enable,
          "config: a new key goes at the end of the section");
    check(set("", "enable", "1") == "[ancora]\r\nenable=1\r\n" && set("a=1\n", "enable", "1") == "a=1\n[ancora]\nenable=1\n",
          "config: a missing section is added, line endings kept");
    c = parse("[ancora]\nright_deadzone=7\nleft_deadzone=abc\nleft_average_ms=5000\nright_average_ms=-5\nleft_edge=0.9,0.9\n");
    check(c.hand[1].deadzone == 0.95f && c.hand[0].deadzone == 0 && c.hand[0].average_ms == 300 &&
              c.hand[1].average_ms == 0 && c.hand[0].edge_bad && !c.hand[0].edge.on,
          "config: out of range and garbage values are clamped (deadzone 0.95, average 300 ms) or off",
          "deadzones %.3f %.3f, averages %d %d, edge_bad %d", c.hand[0].deadzone, c.hand[1].deadzone,
          c.hand[0].average_ms, c.hand[1].average_ms, c.hand[0].edge_bad);

    // the same key in another section is never read or changed
    std::string two = "[other]\nleft_deadzone=0.9\n[ancora]\nleft_deadzone=0.3\n";
    check(near(parse(two).hand[0].deadzone, 0.3, 1e-6), "config: a key in another section is not read");
    check(set(two, "left_deadzone", "0.4") == "[other]\nleft_deadzone=0.9\n[ancora]\nleft_deadzone=0.4\n",
          "config: set() changes the key in [ancora], not the one in another section",
          "got: %s", set(two, "left_deadzone", "0.4").c_str());
    std::string only_other = "[other]\nright=0\n[ancora]\nenable=1\n";
    check(set(only_other, "right", "1") == "[other]\nright=0\n[ancora]\nenable=1\nright=1\n",
          "config: set() of a key found only in another section adds it to [ancora]");

    // a UTF-8 BOM (Notepad) is skipped
    std::string bom = "\xEF\xBB\xBF[ancora]\nenable=1\nleft_deadzone=0.25\n";
    Config cb = parse(bom);
    check(cb.enable && near(cb.hand[0].deadzone, 0.25, 1e-6), "config: a file starting with a UTF-8 BOM is read",
          "enable %d, left deadzone %.3f", cb.enable, cb.hand[0].deadzone);
    check(set(bom, "enable", "0") == "\xEF\xBB\xBF[ancora]\nenable=0\nleft_deadzone=0.25\n", "config: set() keeps the BOM");

    // '#' and ';' lines are comments: a new key goes after the last key, not after a comment
    std::string commented = "[ancora]\nenable=1\n# was right=1\n; was left=1\n[tail]\nx=1\n";
    check(set(commented, "right", "0") == "[ancora]\nenable=1\nright=0\n# was right=1\n; was left=1\n[tail]\nx=1\n",
          "config: '#' and ';' lines are comments, a new key goes after the section's last key",
          "got: %s", set(commented, "right", "0").c_str());
    check(!parse("[ancora]\n#enable=1\n").enable && !parse("[ancora]\n;enable=1\n").enable,
          "config: a commented out key is not read");

    // a key twice: the first one counts and is the one set() changes
    std::string dup = "[ancora]\nleft_deadzone=0.2\nleft_deadzone=0.6\n";
    check(near(parse(dup).hand[0].deadzone, 0.2, 1e-6), "config: of a key given twice the first counts",
          "got %.3f", parse(dup).hand[0].deadzone);
    check(set(dup, "left_deadzone", "0.3") == "[ancora]\nleft_deadzone=0.3\nleft_deadzone=0.6\n",
          "config: set() of a key given twice changes the first");
    Config e;
    e.hand[0].edge = flat(0.8f);
    e.hand[0].edge_on = false;
    Config back = parse(format(e));
    check(back.hand[0].edge.on && !effective_edge(back.hand[0]).on && std::fabs(back.hand[0].edge.r[5] - 0.8f) < 1e-3f,
          "config: an edge switched off keeps its values but is not used");
    e.hand[1].edge = flat(0.7f);
    e.hand[1].edge_base = flat(0.9f);
    back = parse(format(e));
    check(back.hand[1].edge_base.on && std::fabs(back.hand[1].edge_base.r[3] - 0.9f) < 1e-3f && !back.hand[0].edge_base.on,
          "config: the furthest reach per direction is kept per hand");
}

void test_output() {
    StickSettings st;
    st.correct = true;
    float x, y;
    stick_output(st, true, false, 0, 0, 0.3f, 0.2f, x, y);
    check(x == 0 && y == 0, "output: a closed gate gives 0,0");
    st.deadzone = 0.5f;
    stick_output(st, false, false, 0, 0, 0.3f, 0.0f, x, y);
    check(x == 0 && y == 0, "output: inside the deadzone gives 0,0");
    stick_output(st, false, false, 0, 0, 0.0f, 0.75f, x, y);
    check(x == 0 && std::fabs(y - 0.5f) < 1e-5f, "output: the deadzone rescales the rest to full");
    stick_output(st, false, true, 0.2f, 0.0f, 0.9f, 0.0f, x, y);
    check(x == 0 && y == 0, "output: the average comes before the deadzone");
    // radial: (0.4, 0.4) is 0.566 out, past a 0.5 deadzone, though each axis is inside it
    stick_output(st, false, false, 0, 0, 0.4f, 0.4f, x, y);
    float out_m = std::sqrt(x * x + y * y), want_m = (std::sqrt(0.32f) - 0.5f) / 0.5f;
    check(near(out_m, want_m, 1e-4) && near(x, y, 1e-6), "output: the deadzone is radial (0.4,0.4 is past 0.5, same direction)",
          "out (%.4f, %.4f), magnitude %.4f, want %.4f", x, y, out_m, want_m);
    stick_output(st, false, true, 0.3f, 0.4f, 0.0f, 0.0f, x, y);
    check(x == 0 && y == 0, "output: (0.3,0.4) is exactly at a 0.5 deadzone, 0");
    st.correct = false;
    stick_output(st, true, true, 0, 0, 0.3f, 0.2f, x, y);
    check(x == 0.3f && y == 0.2f, "output: correction off passes the raw stick, gate and all");

    // a deadzone outside 0 < dz < 1 (or nan) is off, the stick passes unchanged
    st.correct = true;
    bool dz_ok = true;
    for (float bad : {1.0f, 1.5f, -0.2f, std::numeric_limits<float>::quiet_NaN()}) {
        st.deadzone = bad;
        stick_output(st, false, false, 0, 0, 0.6f, 0.0f, x, y);
        if (x != 0.6f || y != 0.0f) {
            dz_ok = false;
            std::printf("     deadzone %g: out (%.4f, %.4f), want (0.6, 0)\n", bad, x, y);
        }
    }
    check(dz_ok, "output: a deadzone of 1, 1.5, -0.2 or nan is off");

    // the radial deadzone makes one axis's output depend on the other: only y
    // changes, yet x's output changes, so the logic dll has to send x again. This
    // checks only that dependence; the re-send itself is in ancora_logic.cpp.
    st.deadzone = 0.2f;
    float x1, y1, x2, y2;
    stick_output(st, false, false, 0, 0, 0.5f, 0.0f, x1, y1);
    stick_output(st, false, false, 0, 0, 0.5f, 0.5f, x2, y2);
    check(x1 != x2, "output: with a deadzone, x's output depends on y", "x output %.4f then %.4f", x1, x2);
}

// a deadzone shape: share 1 forward (90 deg, direction 4), `rest` everywhere else
DeadzoneShape forward_shape(float rest) {
    DeadzoneShape s;
    s.on = true;
    for (int k = 0; k < kDzSectors; ++k) s.r[k] = rest;
    s.r[4] = 1.0f;
    return s;
}

void test_shape() {
    DeadzoneShape off;
    check(dz_shape_at(off, 0.3f, 0.1f) == 1.0f, "shape: off is round, share 1 everywhere");
    DeadzoneShape s = forward_shape(0.2f);
    check(near(dz_shape_at(s, 0.0f, 0.5f), 1.0, 1e-5) && near(dz_shape_at(s, 0.0f, -0.5f), 0.2, 1e-5),
          "shape: share read per direction (forward 1, back 0.2)");
    float half = dz_shape_at(s, std::cos(rad(78.75f)), std::sin(rad(78.75f)));   // halfway between 67.5 and 90
    check(near(half, 0.6, 1e-4), "shape: linear between two directions", "got %.4f", half);

    // forward drift of 0.3 with a 0.35 deadzone in that shape: forward 0.3 is held,
    // a small push back of 0.1 moves (round, it would be held)
    StickSettings st;
    st.correct = true;
    st.deadzone = 0.35f;
    st.deadzone_shape = s;
    float x, y;
    stick_output(st, false, false, 0, 0, 0.0f, 0.3f, x, y);
    check(x == 0 && y == 0, "shape: drift toward the wide side is held");
    stick_output(st, false, false, 0, 0, 0.0f, -0.1f, x, y);
    check(y < 0, "shape: a small push toward the narrow side moves", "got (%.4f, %.4f)", x, y);
    float want = (0.1f - 0.07f) / (1.0f - 0.07f);
    check(near(y, -want, 1e-4), "shape: the narrow side scales from its own deadzone to full", "got %.4f, want %.4f", y, -want);
    stick_output(st, false, false, 0, 0, 0.0f, -1.0f, x, y);
    check(near(y, -1.0, 1e-5), "shape: a full push still reads full");

    // measuring: forward rest points widen the forward directions only
    float xs[3] = {0.0f, 0.02f, -0.02f}, ys[3] = {0.30f, 0.25f, 0.28f}, radius[kDzSectors];
    dz_from_points(xs, ys, 3, radius);
    check(near(radius[4], 0.33, 1e-4) && near(radius[12], kDzFloor, 1e-6),
          "shape: measured forward is the furthest rest plus 0.03, back is the floor",
          "forward %.4f, back %.4f", radius[4], radius[12]);
    DeadzoneShape m = dz_shape_from(radius, 0.35f);
    check(m.on && near(m.r[4], 0.33 / 0.35, 1e-4) && m.r[12] < 0.2f, "shape: shares of the rounded deadzone");
    float ring[kDzSectors];
    for (int k = 0; k < kDzSectors; ++k) ring[k] = 0.3f;
    check(!dz_shape_from(ring, 0.35f).on, "shape: about the same in every direction stays round");

    DeadzoneShape p = dz_shape_parse(dz_shape_format(m).c_str());
    bool same = p.on;
    for (int k = 0; k < kDzSectors && same; ++k) same = near(p.r[k], m.r[k], 0.006);
    check(same, "shape: survives a write and read back");
    check(!dz_shape_parse("").on && !dz_shape_parse("0.5,0.5").on && !dz_shape_parse("a").on,
          "shape: empty, short or malformed is round");
    ancfg::Config e;
    e.hand[1].deadzone_shape = m;
    ancfg::Config back = ancfg::parse(ancfg::format(e));
    check(back.hand[1].deadzone_shape.on && !back.hand[0].deadzone_shape.on, "shape: read per hand from ancora.ini");
}

void test_curve() {
    check(curve_apply(0.0f, 0.4f) == 0.4f, "curve: 0 is off");
    check(near(curve_apply(0.5f, 0.0f), 0.0, 1e-7) && near(curve_apply(0.5f, 1.0f), 1.0, 1e-6),
          "curve: 0 and full stay where they are");
    check(near(curve_apply(0.5f, 0.5f), 0.3125, 1e-6), "curve: 0.5 at half push gives 0.3125",
          "got %.6f", curve_apply(0.5f, 0.5f));
    bool rising = true;
    for (float c : {0.25f, 1.0f}) {
        float last = -1;
        for (int k = 0; k <= 100; ++k) {
            float o = curve_apply(c, k / 100.0f);
            if (o < last) rising = false;
            last = o;
        }
    }
    check(rising, "curve: never goes back as the push grows");

    StickSettings st;
    st.correct = true;
    st.curve = 0.5f;
    float x, y;
    stick_output(st, false, false, 0, 0, 0.3f, 0.4f, x, y);   // magnitude 0.5, same direction
    check(near(std::sqrt(x * x + y * y), 0.3125, 1e-5) && near(x / y, 0.75, 1e-5),
          "curve: works without a deadzone and keeps the direction", "got (%.4f, %.4f)", x, y);
    st.deadzone = 0.2f;
    stick_output(st, false, false, 0, 0, 0.0f, 0.6f, x, y);   // (0.6 - 0.2) / 0.8 = 0.5 after the deadzone
    check(near(y, 0.3125, 1e-5), "curve: comes after the deadzone", "got %.4f", y);
    ancfg::Config c = ancfg::parse("[ancora]\nleft_curve=0.4\nright_curve=7\n");
    check(near(c.hand[0].curve, 0.4, 1e-6) && c.hand[1].curve == 1.0f, "curve: read per hand, clamped to 1");
}

void test_center() {
    float x, y;
    CenterOffset off;
    center_map(off, 0.3f, -0.2f, x, y);
    check(x == 0.3f && y == -0.2f, "center: off passes the stick unchanged");

    CenterOffset c = center_clamped(0.1f, 0.05f);
    center_map(c, 0.1f, 0.05f, x, y);
    check(x == 0 && y == 0, "center: the rest point reads 0,0", "got (%.4f, %.4f)", x, y);
    // every point on the rim stays on the rim, in its own direction, also toward the offset
    bool rim_ok = true;
    for (int deg = 0; deg < 360; deg += 15) {
        float rx = std::cos(rad((float)deg)), ry = std::sin(rad((float)deg));
        center_map(c, rx, ry, x, y);
        float m = std::sqrt(x * x + y * y);
        if (!near(m, 1.0, 1e-4)) {
            rim_ok = false;
            std::printf("     rim at %d deg: out (%.4f, %.4f), magnitude %.4f\n", deg, x, y, m);
        }
    }
    check(rim_ok, "center: a full push reads full in every direction");
    // the direction is the one from the rest point, not from 0,0
    center_map(c, 0.55f, 0.05f, x, y);
    check(x > 0 && near(y, 0.0, 1e-5), "center: a push straight right from the rest point reads straight right",
          "got (%.4f, %.4f)", x, y);
    CenterOffset h = center_clamped(0.0f, 0.2f);
    center_map(h, 0.0f, 0.6f, x, y);   // 0.4 of the 0.8 from rest to the top rim
    check(near(x, 0.0, 1e-6) && near(y, 0.5, 1e-5), "center: halfway from rest to the rim reads 0.5",
          "got (%.4f, %.4f)", x, y);
    center_map(h, 0.0f, -0.4f, x, y);  // 0.6 of the 1.2 from rest to the bottom rim
    check(near(x, 0.0, 1e-6) && near(y, -0.5, 1e-5), "center: halfway toward the far rim reads 0.5 too",
          "got (%.4f, %.4f)", x, y);
    center_map(h, 0.0f, 1.2f, x, y);
    check(near(y, 1.0, 1e-6), "center: a reading past the rim is capped at 1", "got %.4f", y);

    CenterOffset far = center_clamped(0.9f, 0.0f);
    check(far.on && near(far.x, 0.5, 1e-6), "center: a rest point past 0.5 is clamped to 0.5", "x %.4f", far.x);
    check(!center_clamped(std::numeric_limits<float>::quiet_NaN(), 0).on, "center: nan is off");

    CenterOffset p = center_parse(" 0.031 , -0.012 ");
    check(p.on && near(p.x, 0.031, 1e-6) && near(p.y, -0.012, 1e-6), "center: x,y is read, spaces allowed");
    check(!center_parse("").on && !center_parse("0.1").on && !center_parse("0.1,").on && !center_parse("a,b").on &&
              !center_parse("0.1,0.2,0.3").on,
          "center: empty or malformed text is off");
    check(center_format(p) == "0.031,-0.012" && center_format(CenterOffset{}).empty(), "center: written as x,y, off as empty");

    ancfg::Config cfg = ancfg::parse("[ancora]\nright_center=0.05,0.1\n");
    check(cfg.hand[1].center.on && !cfg.hand[0].center.on && near(cfg.hand[1].center.y, 0.1, 1e-6),
          "center: read per hand from ancora.ini");
    ancfg::Config e;
    e.hand[0].center = center_clamped(-0.04f, 0.02f);
    ancfg::Config back = ancfg::parse(ancfg::format(e));
    check(back.hand[0].center.on && near(back.hand[0].center.x, -0.04, 1e-6) && !back.hand[1].center.on,
          "center: survives a write and read back");

    // output order: average, then center, then deadzone
    StickSettings st;
    st.correct = true;
    st.center = center_clamped(0.0f, 0.2f);
    stick_output(st, false, false, 0, 0, 0.0f, 0.2f, x, y);
    check(x == 0 && y == 0, "center: in the output a stick at its rest point gives 0,0");
    st.deadzone = 0.1f;
    stick_output(st, false, false, 0, 0, 0.0f, 0.25f, x, y);   // 0.05 / 0.8 = 0.0625 off center, inside 0.1
    check(x == 0 && y == 0, "center: the deadzone is measured from the rest point", "got (%.4f, %.4f)", x, y);
    stick_output(st, false, false, 0, 0, 0.0f, 0.06f, x, y);   // without centering 0.06 is inside 0.1; centered it is 0.14 / 1.2
    check(x == 0 && y < 0, "center: a small pull the other way past the rest point moves", "got (%.4f, %.4f)", x, y);
    stick_output(st, true, false, 0, 0, 0.5f, 0.5f, x, y);
    check(x == 0 && y == 0, "center: a closed gate still gives 0,0");
}

void test_gate() {
    TouchGate g;
    check(g.closed(0), "gate: closed before the thumb ever touches");
    g.on_touch(true, 100);
    check(!g.closed(100), "gate: open while touched");
    g.on_touch(false, 200);
    check(!g.closed(210) && g.closed(200 + kGateReleaseMs), "gate: closes only after the release debounce");
    g.on_touch(true, 300);
    g.on_touch(false, 310);
    g.on_touch(true, 315);
    check(!g.closed(400), "gate: a flicker of the sensor does not close it");
    g.on_touch(false, 500);
    bool flagged = false;
    for (double t = 500; t < 500 + kGateSuspectMs - 10; t += 1) flagged |= g.on_stick(0.95f, t);
    check(!flagged && g.closed(500 + kGateSuspectMs - 10), "gate: a short push at the rim with touch off stays gated");
    g.on_stick(0.1f, 800);
    for (double t = 900; t <= 900 + kGateSuspectMs; t += 1) flagged |= g.on_stick(0.95f, t);
    check(flagged && !g.closed(900 + kGateSuspectMs), "gate: held at the rim with touch off, the sensor is not trusted");
    check(!g.closed(5000) && !g.on_stick(0.0f, 5000), "gate: stays open while the sensor is not trusted");
    g.on_touch(true, 6000);
    g.on_touch(false, 6100);
    check(g.closed(6100 + kGateReleaseMs), "gate: a working sensor again brings the gate back");
    TouchGate d;
    for (double t = 0; t < 1000; t += 1) d.on_stick(0.6f, t);
    check(d.closed(1000), "gate: drift short of the rim never opens the gate");

    // README numbers: closes 25 ms after touch goes off, falls back after a quarter
    // second at 0.9 or more of full travel
    TouchGate r;
    r.on_touch(true, 0);
    r.on_touch(false, 100);
    check(!r.closed(124) && r.closed(125), "gate: closes 25 ms after touch goes off");
    TouchGate q;
    bool early = false;
    for (double t = 0; t < 250; t += 1) early |= q.on_stick(0.9f, t);
    check(!early && q.on_stick(0.9f, 250), "gate: held at 0.9 with touch off, the fallback comes at 250 ms, not before");
    TouchGate n;
    bool never = false;
    for (double t = 0; t < 2000; t += 1) never |= n.on_stick(0.89f, t);
    check(!never && n.closed(2000), "gate: held at 0.89 the fallback never comes");

    // a dip below the rim starts the quarter second over: 200 ms + dip + 200 ms is no fallback
    TouchGate p;
    bool dipped = false;
    for (double t = 0; t < 200; t += 1) dipped |= p.on_stick(0.95f, t);
    dipped |= p.on_stick(0.5f, 200);
    for (double t = 201; t < 401; t += 1) dipped |= p.on_stick(0.95f, t);
    check(!dipped && p.closed(401), "gate: a dip below the rim restarts the fallback's quarter second");
    bool later = false;
    for (double t = 401; t <= 451; t += 1) later |= p.on_stick(0.95f, t);
    check(later, "gate: 250 ms after the dip the fallback comes");

    // a touch while waiting also starts it over
    TouchGate u;
    bool touched = false;
    for (double t = 0; t < 200; t += 1) touched |= u.on_stick(0.95f, t);
    u.on_touch(true, 200);
    u.on_touch(false, 201);
    for (double t = 202; t < 400; t += 1) touched |= u.on_stick(0.95f, t);
    check(!touched, "gate: a touch while held at the rim restarts the fallback's quarter second");
}

} // namespace

int main(int argc, char** argv) {
    // the exe is built to driver\obj, two folders below the repo root
    if (argc > 0 && argv[0]) {
        std::string exe = argv[0];
        size_t slash = exe.find_last_of("\\/");
        g_root = slash == std::string::npos ? "../../" : exe.substr(0, slash + 1) + "../../";
    }
    test_average();
    test_measure();
    test_map_and_text();
    test_learn();
    test_pairs();
    test_learn_state();
    test_config();
    test_output();
    test_center();
    test_shape();
    test_curve();
    test_gate();
    std::printf("%d test(s), %d failure(s)\n", g_count, g_fails);
    return g_fails ? 1 : 0;
}
