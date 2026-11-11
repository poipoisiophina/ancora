// ancora_learn.h
// The parts of the logic dll's stick handling that need no SteamVR or Windows,
// kept here so test\ancora_test.cpp can run them: forming whole x/y pairs from
// separate axis updates, and the bookkeeping of edge learning during play (when
// learned reach is saved, and what a config reload keeps).

#pragma once
#include <cstdint>
#include <string>
#include "ancora_edge.h"

// x and y arrive as two separate updates. A pair is whole once both axes are in;
// the same axis twice in a row means the other one did not change, so the pair
// so far is whole too. Only whole pairs are used for edge learning and published
// to the panel's edge measurement, never a new x with an old y.
struct StickPair {
    float raw_x = 0, raw_y = 0;         // latest value of each axis
    unsigned fresh = 0;                 // bit0 x, bit1 y updated since the last pair
    uint32_t seq = 0;                   // bumps on every pair
    float x = 0, y = 0;                 // the latest whole pair

    // one axis update; returns how many pairs it completed (0, 1 or 2)
    int update(bool is_x, float v) {
        int made = 0;
        unsigned bit = is_x ? 1u : 2u;
        if (fresh & bit) { make(); ++made; }
        (is_x ? raw_x : raw_y) = v;
        fresh |= bit;
        if (fresh == 3) { make(); ++made; }
        return made;
    }

private:
    void make() { x = raw_x; y = raw_y; ++seq; fresh = 0; }
};

const double kEdgeSaveMs = 5000;        // learned reach is saved this long after the last change

// per hand bookkeeping of edge learning
struct EdgeLearnState {
    EdgeLearner learner;
    EdgeReach best;                     // furthest reach ever confirmed (<hand>_edge_base)
    bool dirty = false;                 // learned reach not saved yet
    double last_ms = 0;                 // last change, ms
    std::string text;                   // <hand>_edge as last read from the ini

    // a whole pair at time now (ms); returns true when it changed the reach
    bool on_pair(EdgeReach& e, float x, float y, double now) {
        if (!learner.feed(e, best, x, y, now)) return false;
        dirty = true;
        last_ms = now;
        return true;
    }

    // time to save: learned something, and nothing more for kEdgeSaveMs
    bool save_due(double now) const { return dirty && now - last_ms >= kEdgeSaveMs; }

    // the config file was read again. loaded: the edge as read; current: the one in
    // use, with any learning not saved yet; loaded_best: <hand>_edge_base as read
    // (off when the key is missing, then the loaded edge is the best seen so far).
    // When only other keys changed, unsaved learning is kept; a changed edge in the
    // file (a new measurement, removed, or learned values just saved) replaces it.
    void reload(EdgeReach& loaded, const EdgeReach& current, const std::string& loaded_text,
                const EdgeReach& loaded_best) {
        bool same = loaded_text == text;
        if (dirty && same && loaded.on) {
            loaded = current;
        } else {
            dirty = false;
            best = loaded_best.on ? loaded_best : loaded;
            // a best below the reach in use is out of date: the reach is the better floor
            if (best.on && loaded.on)
                for (int k = 0; k < kEdgeSectors; ++k) if (loaded.r[k] > best.r[k]) best.r[k] = loaded.r[k];
        }
        if (!loaded.on) best = EdgeReach{};
        text = loaded_text;
    }
};
