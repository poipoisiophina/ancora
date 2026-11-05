// ancora_average.h
// Moving average of a stick over a time window, independent of the update rate.
// Holds at most one sample per kAverageStepMs; newer readings inside a step
// replace that step's sample. Times are passed in, so the panel self test can
// replay a stick. Used by the logic dll.

#pragma once

const int kMaxAverageMs = 300;
const double kAverageStepMs = 3.0;
const int kAverageSlots = 128;      // kMaxAverageMs / kAverageStepMs, with room to spare

struct StickAverage {
    float  x[kAverageSlots], y[kAverageSlots];
    double t[kAverageSlots];
    int    head = 0, count = 0;     // head: next slot to write
    double sum_x = 0, sum_y = 0;

    void reset() { head = count = 0; sum_x = sum_y = 0; }

    // add a reading; drops samples older than window_ms, always keeps the newest
    void add(float vx, float vy, double now_ms, double window_ms) {
        int newest = (head - 1 + kAverageSlots) % kAverageSlots;
        if (count > 0 && now_ms - t[newest] < kAverageStepMs) {
            sum_x += vx - x[newest];
            sum_y += vy - y[newest];
            x[newest] = vx;
            y[newest] = vy;
        } else {
            if (count == kAverageSlots) drop_oldest();
            x[head] = vx;
            y[head] = vy;
            t[head] = now_ms;
            sum_x += vx;
            sum_y += vy;
            head = (head + 1) % kAverageSlots;
            ++count;
        }
        while (count > 1 && now_ms - t[(head - count + kAverageSlots) % kAverageSlots] > window_ms) drop_oldest();
    }

    float avg_x() const { return count ? (float)(sum_x / count) : 0.0f; }
    float avg_y() const { return count ? (float)(sum_y / count) : 0.0f; }

private:
    void drop_oldest() {
        int oldest = (head - count + kAverageSlots) % kAverageSlots;
        sum_x -= x[oldest];
        sum_y -= y[oldest];
        --count;
    }
};
