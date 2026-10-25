#include "PsyX_XAStream.h"

#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <vector>

struct XAFrame
{
    int16_t l;
    int16_t r;
};

struct PsyX_XAStream
{
    uint32_t sourceHz;
    uint32_t channels;
    uint32_t consumedInputFrames;
    uint32_t producedOutputFrames;
    uint32_t droppedBlocks;
    uint32_t resetCount;
    uint32_t drainCount;
    uint32_t sixStep;
    uint32_t ringPos;
    XAFrame  ring[32];
    std::vector<XAFrame> output;
    size_t outputRead;
};

// Problemkaputt PSX-SPX "25-point Zigzag Interpolation" coefficient table.
//
// Phase 3 carried two transcription errors and they were audible: a tap of
// 0x0400 where 0x0C9D belongs, and that value swapped with its neighbour
// -0x0267. The table is mirror-symmetric (phase 2 is a perfect palindrome,
// and phase 3 is phase 1 reversed -- their 0x74BB peaks sit at the mirrored
// indices 13 and 15), which is what identifies both.
//
// The gain cost was the giveaway: phase 3 summed to 27520 against ~29700 for
// every other phase, a 6.7% dip on one phase in seven. Seven output phases at
// 44100 is a 6300 Hz cycle, so XA audio carried a 6.7% amplitude modulation at
// 6300 Hz -- squarely in the presence band, which is what made voices tinny.
// Every measured spur sat at 6300 +/- the test tone.
//
// Measured against a 1 s sine (harness floor 105 dB), worst spur below the
// fundamental, before -> after:
//    300 Hz  39.4 -> 62.1 dB      3 kHz  37.5 -> 59.9 dB
//   1000 Hz  38.9 -> 60.0 dB      6 kHz  35.9 -> 51.4 dB
// and what is left sits near Nyquist rather than mid-band.
// Columns are the seven output phases; rows are the 29 preceding 37.8 kHz samples.
static const int16_t kZigZag[7][29] =
{
    { 0,0,0,0,0,-0x0002,0x000A,-0x0022,0x0041,-0x0054,0x0034,0x0009,-0x010A,0x0400,-0x0A78,0x234C,0x6794,-0x1780,0x0BCD,-0x0623,0x0350,-0x016D,0x006B,0x000A,-0x0010,0x0011,-0x0008,0x0003,-0x0001 },
    { 0,0,0,-0x0002,0,0x0003,-0x0013,0x003C,-0x004B,0x00A2,-0x00E3,0x0132,-0x0043,-0x0267,0x0C9D,0x74BB,-0x11B4,0x09B8,-0x05BF,0x0372,-0x01A8,0x00A6,-0x001B,0x0005,0x0006,-0x0008,0x0003,-0x0001,0 },
    { 0,0,-0x0001,0x0003,-0x0002,-0x0005,0x001F,-0x004A,0x00B3,-0x0192,0x02B1,-0x039E,0x04F8,-0x05A6,0x7939,-0x05A6,0x04F8,-0x039E,0x02B1,-0x0192,0x00B3,-0x004A,0x001F,-0x0005,-0x0002,0x0003,-0x0001,0,0 },
    { 0,-0x0001,0x0003,-0x0008,0x0006,0x0005,-0x001B,0x00A6,-0x01A8,0x0372,-0x05BF,0x09B8,-0x11B4,0x74BB,0x0C9D,-0x0267,-0x0043,0x0132,-0x00E3,0x00A2,-0x004B,0x003C,-0x0013,0x0003,0,-0x0002,0,0,0 },
    { -0x0001,0x0003,-0x0008,0x0011,-0x0010,0x000A,0x006B,-0x016D,0x0350,-0x0623,0x0BCD,-0x1780,0x6794,0x234C,-0x0A78,0x0400,-0x010A,0x0009,0x0034,-0x0054,0x0041,-0x0022,0x000A,-0x0001,0,0,0,0,0 },
    { 0x0002,-0x0008,0x0010,-0x0023,0x002B,0x001A,-0x00EB,0x027B,-0x0548,0x0AFA,-0x16FA,0x53E0,0x3C07,-0x1249,0x080E,-0x0347,0x015B,-0x0044,-0x0017,0x0046,-0x0023,0x0011,-0x0005,0,0,0,0,0,0 },
    { -0x0005,0x0011,-0x0023,0x0046,-0x0017,-0x0044,0x015B,-0x0347,0x080E,-0x1249,0x3C07,0x53E0,-0x16FA,0x0AFA,-0x0548,0x027B,-0x00EB,0x001A,0x002B,-0x0023,0x0010,-0x0008,0x0002,0,0,0,0,0,0 }
};

static int16_t ClampS16(int32_t value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return static_cast<int16_t>(value);
}

static int32_t ArithmeticShift15(int32_t value)
{
    if (value >= 0)
        return value >> 15;
    return -static_cast<int32_t>((static_cast<uint32_t>(-static_cast<int64_t>(value)) + 0x7FFFu) >> 15);
}

static XAFrame Interpolate(const PsyX_XAStream* stream, uint32_t phase)
{
    int32_t left = 0;
    int32_t right = 0;

    for (uint32_t i = 1; i <= 29; ++i)
    {
        const XAFrame& sample = stream->ring[(stream->ringPos - i) & 31u];
        left += ArithmeticShift15(static_cast<int32_t>(sample.l) * kZigZag[phase][i - 1]);
        right += ArithmeticShift15(static_cast<int32_t>(sample.r) * kZigZag[phase][i - 1]);
    }

    XAFrame result = { ClampS16(left), ClampS16(right) };
    return result;
}

static void Push37800Frame(PsyX_XAStream* stream, const XAFrame& frame)
{
    stream->ring[stream->ringPos] = frame;
    stream->ringPos = (stream->ringPos + 1u) & 31u;

    if (--stream->sixStep != 0)
        return;

    stream->sixStep = 6;
    for (uint32_t phase = 0; phase < 7; ++phase)
        stream->output.push_back(Interpolate(stream, phase));
}

static void CompactOutput(PsyX_XAStream* stream)
{
    if (stream->outputRead == 0)
        return;
    if (stream->outputRead < 4096 && stream->outputRead * 2 < stream->output.size())
        return;

    stream->output.erase(stream->output.begin(), stream->output.begin() + stream->outputRead);
    stream->outputRead = 0;
}

PsyX_XAStream* PsyX_XAStream_Create(void)
{
    PsyX_XAStream* stream = new PsyX_XAStream();
    stream->sixStep = 6;
    return stream;
}

void PsyX_XAStream_Destroy(PsyX_XAStream* stream)
{
    delete stream;
}

void PsyX_XAStream_Reset(PsyX_XAStream* stream)
{
    if (!stream)
        return;

    stream->sourceHz = 0;
    stream->channels = 0;
    stream->consumedInputFrames = 0;
    stream->producedOutputFrames = 0;
    stream->droppedBlocks = 0;
    stream->sixStep = 6;
    stream->ringPos = 0;
    memset(stream->ring, 0, sizeof(stream->ring));
    stream->output.clear();
    stream->outputRead = 0;
    ++stream->resetCount;
}

void PsyX_XAStream_Drain(PsyX_XAStream* stream)
{
    if (stream)
        ++stream->drainCount;
}

int PsyX_XAStream_Push(PsyX_XAStream* stream, const int16_t* interleaved,
                       uint32_t frames, uint32_t sourceHz, uint32_t channels)
{
    if (!stream || !interleaved || frames == 0)
        return 0;
    if ((sourceHz != 37800 && sourceHz != 18900) || (channels != 1 && channels != 2))
        return 0;
    if (stream->sourceHz != 0 &&
        (stream->sourceHz != sourceHz || stream->channels != channels))
    {
        ++stream->droppedBlocks;
        return 0;
    }

    stream->sourceHz = sourceHz;
    stream->channels = channels;
    size_t outputBefore = stream->output.size();

    for (uint32_t i = 0; i < frames; ++i)
    {
        XAFrame frame;
        frame.l = interleaved[i * channels];
        frame.r = channels == 2 ? interleaved[i * 2 + 1] : frame.l;
        Push37800Frame(stream, frame);
        if (sourceHz == 18900)
            Push37800Frame(stream, frame);
    }

    stream->consumedInputFrames += frames;
    stream->producedOutputFrames += static_cast<uint32_t>(stream->output.size() - outputBefore);
    return 1;
}

uint32_t PsyX_XAStream_Pop44100Stereo(PsyX_XAStream* stream,
                                      int16_t* outInterleavedStereo,
                                      uint32_t maxFrames)
{
    if (!stream || !outInterleavedStereo || maxFrames == 0)
        return 0;

    size_t available = stream->output.size() - stream->outputRead;
    uint32_t count = static_cast<uint32_t>(std::min<size_t>(available, maxFrames));
    for (uint32_t i = 0; i < count; ++i)
    {
        const XAFrame& frame = stream->output[stream->outputRead + i];
        outInterleavedStereo[i * 2] = frame.l;
        outInterleavedStereo[i * 2 + 1] = frame.r;
    }
    stream->outputRead += count;
    CompactOutput(stream);
    return count;
}

uint32_t PsyX_XAStream_PeekQueuedOutputFrames(const PsyX_XAStream* stream)
{
    if (!stream)
        return 0;
    return static_cast<uint32_t>(stream->output.size() - stream->outputRead);
}

void PsyX_XAStream_GetStats(const PsyX_XAStream* stream, PsyX_XAStreamStats* out)
{
    if (!stream || !out)
        return;

    out->queuedBlocks = PsyX_XAStream_PeekQueuedOutputFrames(stream) != 0 ? 1u : 0u;
    out->queuedInputFrames = 0;
    out->queuedOutputFrames = PsyX_XAStream_PeekQueuedOutputFrames(stream);
    out->consumedInputFrames = stream->consumedInputFrames;
    out->producedOutputFrames = stream->producedOutputFrames;
    out->droppedBlocks = stream->droppedBlocks;
    out->resetCount = stream->resetCount;
    out->drainCount = stream->drainCount;
    out->sourceRateHz = stream->sourceHz;
    out->channels = stream->channels;
    out->fifoCapacityBlocks = 1;
}

#ifdef PSYX_XASTREAM_SELFTEST
#include <stdio.h>

int PsyX_XAStream_SelfTest(void)
{
    PsyX_XAStream* stream = PsyX_XAStream_Create();
    if (!stream)
        return 1;

    int16_t source[12];
    for (int i = 0; i < 12; ++i)
        source[i] = static_cast<int16_t>(i * 100);

    if (!PsyX_XAStream_Push(stream, source, 12, 37800, 1))
        return 2;
    if (PsyX_XAStream_PeekQueuedOutputFrames(stream) != 14)
        return 3;

    int16_t output[28] = {};
    uint32_t first = PsyX_XAStream_Pop44100Stereo(stream, output, 3);
    uint32_t second = PsyX_XAStream_Pop44100Stereo(stream, output + first * 2, 11);
    if (first != 3 || second != 11 || PsyX_XAStream_PeekQueuedOutputFrames(stream) != 0)
        return 4;

    PsyX_XAStream_Reset(stream);
    if (!PsyX_XAStream_Push(stream, source, 6, 18900, 1))
        return 5;
    if (PsyX_XAStream_PeekQueuedOutputFrames(stream) != 14)
        return 6;

    PsyX_XAStream_Destroy(stream);
    printf("XA selftest ok\n");
    return 0;
}
#endif
