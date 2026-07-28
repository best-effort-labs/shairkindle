#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
extern "C" {
#include "sink.h"
}
extern int16_t sink_rec_buf[16384];
extern size_t  sink_rec_n;
extern size_t  sink_rec_max_accept;
extern int     sink_rec_flushed;

int main() {
    assert(sink_open(44100, 70) == 0);
    assert(sink_rec_n == 0);                                       // open resets the record buffer

    int16_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = (int16_t)i;
    assert(sink_write(buf, 512) == 512);                            // default: accepts all
    assert(sink_rec_n == 512);
    assert(memcmp(sink_rec_buf, buf, 512 * sizeof(int16_t)) == 0);  // PCM actually recorded

    sink_rec_max_accept = 200;                                      // scripted partial-accept
    assert(sink_write(buf, 512) == 200);
    assert(sink_rec_n == 512 + 200);
    sink_rec_max_accept = (size_t)-1;                               // restore default

    assert(sink_driver_delay_frames() == 3);                        // record backend fixed values
    assert(sink_queued_frames() == 8);

    assert(sink_pending_bytes() == 0);
    assert(sink_respawns() == 0);
    assert(sink_child_alive() == 1);

    // ---- phone-volume software gain (attenuation-only; see src/sink.cpp) ----
    int16_t vin[8]  = { 1000, -1000, 20000, -20000, 5, -5, 32767, -32768 };
    int16_t vout[8];

    assert(sink_open(44100, 70) == 0);                              // reset rec buffer; gain untouched
    assert(sink_write(vin, 8) == 8);
    assert(memcmp(sink_rec_buf, vin, sizeof vin) == 0);             // default (no SET_PARAMETER yet): unity

    assert(sink_open(44100, 70) == 0);
    sink_set_volume(100);
    assert(sink_write(vin, 8) == 8);
    assert(memcmp(sink_rec_buf, vin, sizeof vin) == 0);             // 100 == 0dB == unity, exact

    assert(sink_open(44100, 70) == 0);
    sink_set_volume(50);
    assert(sink_write(vin, 8) == 8);
    memcpy(vout, sink_rec_buf, sizeof vout);
    /* pct 50 -> -15 dB -> gain 10^(-0.75)=0.1778 -> Q8 gain_q8=46; out = (in*46)>>8
     * (arithmetic shift, floors toward -inf). Exact values track the CURRENT perceptual
     * curve -- if you retune the curve, this fails on purpose: update it consciously.
     * This pins the Q8 conversion + shift (a wrong curve/shift would slip past a mere
     * attenuated-with-sign check). */
    int16_t vexp50[8] = { 179, -180, 3593, -3594, 0, -1, 5887, -5888 };
    for (int i = 0; i < 8; i++) assert(vout[i] == vexp50[i]);

    assert(sink_open(44100, 70) == 0);
    sink_set_volume(0);
    assert(sink_write(vin, 8) == 8);
    for (int i = 0; i < 8; i++) assert(sink_rec_buf[i] == 0);       // mute: all samples 0

    assert(sink_open(44100, 70) == 0);
    sink_set_volume(100);                                           // gain is not latched -- restores
    assert(sink_write(vin, 8) == 8);
    assert(memcmp(sink_rec_buf, vin, sizeof vin) == 0);

    sink_flush();
    assert(sink_rec_flushed);
    assert(sink_rec_n == 0);                                        // flush resets the buffer

    sink_close();
    return 0;
}
