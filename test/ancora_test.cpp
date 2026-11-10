// ancora_test.cpp
// Tests for the driver side math that the logic dll runs on every stick update:
// edge stretch, edge measuring, edge learning during play, whole x/y pairs, the
// learned edge bookkeeping and the moving average. Plain console program, no
// SteamVR, no Windows API. build_driver.bat builds and runs it before it deploys
// the logic dll, so a failing test stops the build.
//   ancora_test.exe        run every test, exit code 0 = all pass

#include <cmath>
#include <cstdio>
#include "../driver/src/ancora_average.h"
#include "../driver/src/ancora_edge.h"
#include "../driver/src/ancora_learn.h"

namespace {

int g_fails = 0, g_count = 0;

void check(bool ok, const char* what) {
    ++g_count;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

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

void test_average() {
    static StickAverage av;
    for (int k = 0; k <= 1000; ++k) av.add(k <= 500 ? 0.0f : 1.0f, 0.5f, (double)k, 100.0);
    check(std::fabs(av.avg_x() - 1.0f) < 1e-4 && std::fabs(av.avg_y() - 0.5f) < 1e-4, "average settles on a steady value");
    av.reset();
    for (int k = 0; k <= 500; ++k) av.add(0.0f, 0.0f, (double)k, 100.0);
    for (int k = 501; k <= 550; ++k) av.add(1.0f, 0.0f, (double)k, 100.0);
    check(av.avg_x() > 0.4f && av.avg_x() < 0.6f, "average is about half way, half a window after a step");
    av.reset();
    for (int k = 0; k <= 500; ++k) av.add(k > 450 ? 1.0f : 0.0f, 0, k * 1.0, 100.0);
    float fast = av.avg_x();
    av.reset();
    for (int k = 0; k <= 50; ++k) av.add(k > 45 ? 1.0f : 0.0f, 0, k * 10.0, 100.0);
    check(std::fabs(fast - av.avg_x()) < 0.1f, "same result at 1000 and 100 updates per second");
    av.reset();
    for (int k = 0; k <= 200; ++k) av.add(((k / 3) & 1) ? 0.6f : 0.4f, 0, k * 1.0, 100.0);
    check(std::fabs(av.avg_x() - 0.5f) < 0.03f, "average removes fast jitter");
    av.reset();
    check(av.count == 0 && av.avg_x() == 0.0f, "reset empties the average");
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

    EdgeReach s = flat(0.6f);
    EdgeLearner L;
    L.feed(s, 0.5f, 0.0f);
    L.feed(s, 1.0f, 0.0f);
    check(edge_reach(s, 1, 0) <= 0.6f + 1e-4f, "learn: a single glitch at the rim teaches nothing");
    L = EdgeLearner{};
    L.feed(s, 0.9f, 0.0f);
    L.feed(s, 0.95f, 0.0f);
    check(std::fabs(edge_reach(s, 1, 0) - 0.9f) < 1e-3f, "learn: two readings in a row raise it to the shorter one");
    L = EdgeLearner{};
    s = flat(0.6f);
    L.feed(s, 0.0f, 0.95f);
    L.feed(s, 0.0f, -0.95f);
    check(std::fabs(edge_reach(s, 0, 1) - 0.6f) < 1e-4f && std::fabs(edge_reach(s, 0, -1) - 0.6f) < 1e-4f,
          "learn: two readings far apart teach nothing");

    // playing on the simulated worn stick, starting from a measurement that came out short
    EdgeReach play = flat(0.55f);
    EdgeLearner P;
    for (int t = 0; t < 5; ++t)
        for (int k = 0; k < 360; ++k) {
            float deg = k * 1.0f, m = reach_at(deg);
            P.feed(play, m * std::cos(rad(deg)), m * std::sin(rad(deg)));
        }
    float worst = 0;
    for (int k = 0; k < 360; k += 3) worst = std::fmax(worst, std::fabs(edge_reach(play, std::cos(rad(k)), std::sin(rad(k))) - reach_at(k)));
    check(worst < 0.03f, "learn: play on a worn stick brings a short measurement to the real edge");
    check(edge_reach(play, std::cos(rad(135)), std::sin(rad(135))) < 0.68f, "learn: the worn spot stays short after play");
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
    st.on_pair(e, 0.9f, 0.0f, 1000);
    check(!st.on_pair(e, 0.95f, 0.0f, 1010) || st.dirty, "state: learning marks the edge to save");
    check(!st.save_due(1010 + kEdgeSaveMs - 1) && st.save_due(1010 + kEdgeSaveMs), "state: saved only after a quiet time");

    // another key in the file changed before the save: learned reach is kept
    EdgeReach loaded = edge_parse(st.text.c_str());
    st.reload(loaded, e, st.text);
    check(st.dirty && std::fabs(edge_reach(loaded, 1, 0) - 0.9f) < 1e-3f, "state: a reload for another key keeps unsaved learning");

    // a new measurement in the file: it wins
    EdgeReach fresh = flat(0.8f);
    std::string ft = edge_format(fresh);
    EdgeReach got = edge_parse(ft.c_str());
    st.reload(got, e, ft);
    check(!st.dirty && std::fabs(edge_reach(got, 1, 0) - 0.8f) < 1e-3f, "state: a new measurement replaces unsaved learning");

    // removed in the panel: stays off
    st.on_pair(got, 0.95f, 0.0f, 2000);
    st.on_pair(got, 0.97f, 0.0f, 2010);
    EdgeReach removed;
    st.reload(removed, got, "");
    check(!st.dirty && !removed.on, "state: removing the stretch is not undone by unsaved learning");

    // saved by the dll itself: the file now holds the learned values
    EdgeReach mine = flat(0.7f);
    st = EdgeLearnState{};
    st.text = edge_format(flat(0.6f));
    std::string saved = edge_format(mine);
    EdgeReach reread = edge_parse(saved.c_str());
    st.reload(reread, mine, saved);
    check(!st.dirty && std::fabs(edge_reach(reread, 1, 0) - 0.7f) < 1e-3f, "state: reading back its own save keeps the values");
}

} // namespace

int main() {
    test_average();
    test_measure();
    test_map_and_text();
    test_learn();
    test_pairs();
    test_learn_state();
    std::printf("%d test(s), %d failure(s)\n", g_count, g_fails);
    return g_fails ? 1 : 0;
}
