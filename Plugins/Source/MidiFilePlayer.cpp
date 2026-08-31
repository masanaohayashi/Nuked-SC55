#include "MidiFilePlayer.h"
#include "RcpFilePlayer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
struct Reader
{
    const uint8_t* p;
    const uint8_t* end;

    bool ok() const { return p < end; }
    uint8_t u8() { return p < end ? *p++ : 0; }
    uint32_t varlen()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const uint8_t c = u8();
            v = (v << 7) | (c & 0x7f);
            if (! (c & 0x80)) break;
        }
        return v;
    }
};

struct RawEvent
{
    uint32_t tick;
    uint32_t order;
    std::vector<uint8_t> bytes;
    uint32_t tempo;
    uint8_t numerator, denominator;
};
}

bool MidiFileData::load (const std::string& path, std::string& error, bool loadWrd)
{
    clear();
    error.clear();

    std::FILE* file = std::fopen (path.c_str(), "rb");
    if (file == nullptr) { error = "Cannot open: " + path; return false; }

    std::fseek (file, 0, SEEK_END);
    const long size = std::ftell (file);
    std::rewind (file);
    std::vector<uint8_t> data (static_cast<size_t> (std::max (0L, size)));
    const bool read = ! data.empty() && std::fread (data.data(), 1, data.size(), file) == data.size();
    std::fclose (file);
    if (! read) { error = "Cannot load: " + path; return false; }

    if (rcpfile::isRcpV2 (data))
        return rcpfile::loadRcpV2 (data, *this, error, loadWrd ? path : std::string {});

    if (data.size() < 14 || std::memcmp (data.data(), "MThd", 4) != 0)
    { error = "Not as standard MIDI file"; return false; }

    const uint16_t trackCount = static_cast<uint16_t> ((data[10] << 8) | data[11]);
    const uint16_t division   = static_cast<uint16_t> ((data[12] << 8) | data[13]);
    if (division == 0 || (division & 0x8000) != 0)
    { error = "SMPTE is not supported"; return false; }

    std::vector<RawEvent> raw;
    size_t pos = 8 + ((data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]);
    uint32_t order = 0;

    for (uint16_t track = 0; track < trackCount && pos + 8 <= data.size(); ++track)
    {
        const uint32_t length = (data[pos+4] << 24) | (data[pos+5] << 16) | (data[pos+6] << 8) | data[pos+7];
        Reader r { data.data() + pos + 8, data.data() + std::min (data.size(), pos + 8 + length) };
        pos += 8 + length;

        uint32_t tick = 0;
        uint8_t running = 0;

        while (r.ok())
        {
            tick += r.varlen();
            uint8_t status = *r.p;

            if (status & 0x80) { r.u8(); if (status < 0xf0) running = status; }
            else               { status = running; if (status == 0) break; }

            RawEvent e {};
            e.tick = tick;
            e.order = order++;

            if (status == 0xff)
            {
                const uint8_t type = r.u8();
                const uint32_t len = r.varlen();
                const uint8_t* body = r.p;
                r.p = std::min (r.end, r.p + len);

                if (type == 0x51 && len == 3) e.tempo = (body[0] << 16) | (body[1] << 8) | body[2];
                else if (type == 0x58 && len >= 2) { e.numerator = body[0]; e.denominator = static_cast<uint8_t> (1 << body[1]); }
                else continue;
            }
            else if (status == 0xf0 || status == 0xf7)
            {
                const uint32_t len = r.varlen();
                const uint8_t* body = r.p;
                r.p = std::min (r.end, r.p + len);

                if (status == 0xf0) e.bytes.push_back (0xf0);
                e.bytes.insert (e.bytes.end(), body, body + len);
            }
            else
            {
                const int operands = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 1 : 2;
                e.bytes.push_back (status);
                for (int i = 0; i < operands; ++i) e.bytes.push_back (r.u8());
            }

            raw.push_back (std::move (e));
        }
    }

    std::stable_sort (raw.begin(), raw.end(), [] (const RawEvent& a, const RawEvent& b)
    {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });

    double seconds = 0.0, quarter = 0.5;
    uint32_t lastTick = 0;
    int numerator = 4, denominator = 4;

    for (const auto& e : raw)
    {
        seconds += (e.tick - lastTick) * quarter / division;
        lastTick = e.tick;

        if (e.tempo != 0) quarter = e.tempo / 1e6;
        if (e.numerator != 0) { numerator = e.numerator; denominator = e.denominator; }

        if (! e.bytes.empty())
        {
            events.push_back ({ seconds, e.bytes });
            songEndSeconds = seconds;
        }

        lastBarSeconds = quarter * numerator * 4.0 / denominator;
    }

    if (events.empty()) { error = "演奏イベントがありません"; return false; }
    return true;
}
