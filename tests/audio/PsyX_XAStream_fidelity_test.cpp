/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Spectral fidelity of the XA 37800 -> 44100 resampler.
 *
 * This exists because two coefficients in the zigzag table were wrong for a
 * long time and nothing caught them. Phase 3 summed 6.7% low against the other
 * six, which put a 6.7% amplitude modulation at 44100/7 = 6300 Hz on every line
 * of dialogue -- audible as "tinny", invisible to every functional test,
 * because the resampler still produced the right NUMBER of samples at the right
 * pitch. Only the spectrum showed it.
 *
 * So this test measures the thing that broke: push a pure tone in, and assert
 * nothing else comes out anywhere near it. A per-phase gain error shows up as a
 * spur at 6300 +/- tone, which is exactly what these thresholds catch. The old
 * table scored 37-39 dB here; the corrected one scores 51-62.
 *
 * Deliberately behavioural rather than a check on the table's numbers: it also
 * covers the resampling STRUCTURE (phase order, window advance, rounding), all
 * of which were suspected and cleared while hunting the real fault. */

#include "PsyX_XAStream.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
/* Single-bin DFT magnitude. Cheaper than a full FFT and this test only ever
 * asks about specific frequencies. */
double BinMagnitude(const std::vector<double>& x, double hz, double sampleRate)
{
    const double w = 2.0 * M_PI * hz / sampleRate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;

    for (double v : x)
    {
        const double s0 = v + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w);
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im);
}

/* Worst spur, in dB below the fundamental, for a tone pushed through the real
 * resampler. Higher is better; a perfect path measures ~105 dB on this harness
 * (the 16-bit quantisation of the source tone sets that floor). */
double SpurRejectionDb(double toneHz)
{
    const double kInRate  = 37800.0;
    const double kOutRate = 44100.0;
    const int    kInFrames = 37800;          /* one second */

    PsyX_XAStream* stream = PsyX_XAStream_Create();
    assert(stream != nullptr);
    PsyX_XAStream_Reset(stream);

    std::vector<int16_t> in;
    in.reserve(static_cast<size_t>(kInFrames) * 2u);
    for (int i = 0; i < kInFrames; ++i)
    {
        const int16_t v = static_cast<int16_t>(
            std::lrint(24000.0 * std::sin(2.0 * M_PI * toneHz * i / kInRate)));
        in.push_back(v);
        in.push_back(v);
    }
    PsyX_XAStream_Push(stream, in.data(), kInFrames, 37800, 2);

    std::vector<int16_t> out(60000u * 2u);
    const uint32_t got = PsyX_XAStream_Pop44100Stereo(stream, out.data(), 60000);
    PsyX_XAStream_Destroy(stream);
    assert(got > 40000u);

    /* Skip the filter's start-up and tail-off; window the rest, or the
     * fundamental's own leakage buries everything at about -40 dB and the test
     * ends up measuring itself rather than the resampler. */
    std::vector<double> mono;
    for (uint32_t i = 4000; i + 4000 < got; ++i)
        mono.push_back(static_cast<double>(out[i * 2]));
    const size_t n = mono.size();
    for (size_t i = 0; i < n; ++i)
        mono[i] *= 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) /
                                        static_cast<double>(n - 1));

    const double fundamental = BinMagnitude(mono, toneHz, kOutRate);
    double worst = 0.0;
    for (double hz = 100.0; hz < 22000.0; hz += 25.0)
    {
        if (std::fabs(hz - toneHz) < 120.0)
            continue;                       /* the fundamental's own skirt */
        worst = std::max(worst, BinMagnitude(mono, hz, kOutRate));
    }
    assert(worst > 0.0);
    return 20.0 * std::log10(fundamental / worst);
}
}

int main()
{
    /* Thresholds sit well below what the corrected table achieves and well
     * above what the broken one did, so this fails on a coefficient regression
     * without tripping on ordinary fixed-point noise. */
    struct { double hz; double minDb; } cases[] = {
        {  300.0, 55.0 },   /* corrected: 62.1, broken: 39.4 */
        { 1000.0, 55.0 },   /* corrected: 60.0, broken: 38.9 */
        { 3000.0, 55.0 },   /* corrected: 59.9, broken: 37.5 */
        { 6000.0, 45.0 },   /* corrected: 51.4, broken: 35.9 */
    };

    for (const auto& c : cases)
    {
        const double db = SpurRejectionDb(c.hz);
        std::printf("XA resampler %6.0f Hz: worst spur %.1f dB below (need %.0f)\n",
                    c.hz, db, c.minDb);
        assert(db >= c.minDb);
    }

    std::printf("PsyX_XAStream fidelity: OK\n");
    return 0;
}
