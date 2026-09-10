#include "sc55_voice_set.h"
#include <cstdlib>
#include <iostream>

static void check(bool condition)
{
    if (!condition) std::abort();
}

int main()
{
    sc55::VoiceSet voices, odd;
    check(voices.empty() && voices.count() == 0);
    for (unsigned i = 0; i < 128; ++i) {
        check(voices.set(i));
        check(voices.contains(i) && voices.count() == i + 1);
        if (i & 1) check(odd.set(i));
    }
    check(!voices.set(128) && !voices.set(~0u));
    check(!voices.contains(128) && !voices.contains(~0u));
    auto intersection = voices;
    intersection &= odd;
    check(intersection == odd);
    voices.remove(odd);
    check(voices.count() == 64);
    for (unsigned i = 0; i < 128; ++i) check(voices.contains(i) == !(i & 1));
    voices |= odd;
    check(voices.count() == 128);
    for (const auto i : {23u, 24u, 27u, 28u, 31u, 32u, 63u, 64u, 95u, 96u, 127u}) {
        check(voices.set(i, false) && !voices.contains(i));
        check(voices.set(i) && voices.contains(i));
    }
    voices.clear();
    check(voices.empty());
    std::cout << "128-voice set: boundaries, count, union, intersection, removal PASS\n";
}
