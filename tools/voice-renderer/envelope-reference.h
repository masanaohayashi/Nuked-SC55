/*
 * Copyright (C) 2021, 2024 nukeykt
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */
#pragma once
#include <cstdint>
// Frozen pre-extraction PCM arithmetic. Test-only independent oracle;
// do not replace this with the production ramp implementation.
struct ReferenceEnvelopeClock { uint16_t tv_counter; bool nfs; };
inline void referenceRamp(ReferenceEnvelopeClock& pcm, int e, int adjust, uint16_t *levelcur, int active, int *volmul)
{
    // int adjust = ram2[3+e];
    // int levelcur = ram2[9+e] & 0x7fff;
    *levelcur &= 0x7fff;
    int speed = adjust & 0xff;
    int target = (adjust >> 8) & 0xff;

                
    const bool w1 = (speed & 0xf0) == 0;
    const bool w2 = w1 || (speed & 0x10) != 0;
    const bool w3 = pcm.nfs &&
        ((speed & 0x80) == 0 || ((speed & 0x40) == 0 && (!w2 || (speed & 0x20) == 0)));

    int type = (int)w2 | ((int)w3 << 3);
    if (speed & 0x20)
        type |= 2;
    if ((speed & 0x80) == 0 || (speed & 0x40) == 0)
        type |= 4;


    bool write = !active;
    int addlow = 0;
    if (type & 4)
    {
        if (pcm.tv_counter & 8)
            addlow |= 1;
        if (pcm.tv_counter & 4)
            addlow |= 2;
        if (pcm.tv_counter & 2)
            addlow |= 4;
        if (pcm.tv_counter & 1)
            addlow |= 8;
        write |= 1;
    }
    else
    {
        switch (type & 3)
        {
        case 0:
            if (pcm.tv_counter & 0x20)
                addlow |= 1;
            if (pcm.tv_counter & 0x10)
                addlow |= 2;
            if (pcm.tv_counter & 8)
                addlow |= 4;
            if (pcm.tv_counter & 4)
                addlow |= 8;
            write |= (pcm.tv_counter & 3) == 0;
            break;
        case 1:
            if (pcm.tv_counter & 0x80)
                addlow |= 1;
            if (pcm.tv_counter & 0x40)
                addlow |= 2;
            if (pcm.tv_counter & 0x20)
                addlow |= 4;
            if (pcm.tv_counter & 0x10)
                addlow |= 8;
            write |= (pcm.tv_counter & 15) == 0;
            break;
        case 2:
            if (pcm.tv_counter & 0x200)
                addlow |= 1;
            if (pcm.tv_counter & 0x100)
                addlow |= 2;
            if (pcm.tv_counter & 0x80)
                addlow |= 4;
            if (pcm.tv_counter & 0x40)
                addlow |= 8;
            write |= (pcm.tv_counter & 63) == 0;
            break;
        case 3:
            if (pcm.tv_counter & 0x800)
                addlow |= 1;
            if (pcm.tv_counter & 0x400)
                addlow |= 2;
            if (pcm.tv_counter & 0x200)
                addlow |= 4;
            if (pcm.tv_counter & 0x100)
                addlow |= 8;
            write |= (pcm.tv_counter & 127) == 0;
            break;
        }
    }

    if ((type & 8) == 0)
    {
        int shift = speed & 15;
        shift = (10 - shift) & 15;

        int sum1 = (target << 11); // 5
        if (e != 2 || active)
            sum1 -= (*levelcur << 4); // 6
        const bool neg = (sum1 & 0x80000) != 0;
        (void)neg; // unused

        int preshift = sum1;

        int shifted = preshift >> shift;
        shifted -= sum1;

        int sum2 = (target << 11) + addlow + shifted;
        if (write && pcm.nfs)
            *levelcur = (sum2 >> 4) & 0x7fff;

        if (e == 0)
        {
            *volmul = (sum2 >> 4) & 0x7ffe;
        }
        else if (e == 1)
        {
            *volmul = (sum2 >> 4) & 0x7ffe;
        }
    }
    else
    {
        int shift = (speed >> 4) & 14;
        shift |= (int)w2;
        shift = (10 - shift) & 15;

        int sum1 = target << 11; // 5
        if (e != 2 || active)
            sum1 -= (*levelcur << 4); // 6
        const bool neg = (sum1 & 0x80000) != 0;
        int preshift = (speed & 15) << 9;
        if (!w1)
            preshift |= 0x2000;
        if (neg)
            preshift ^= ~0x3f;

        int shifted = preshift >> shift;
        int sum2 = shifted;
        if (e != 2 || active)
            sum2 += (*levelcur << 4) | addlow;

        int sum2_l = (sum2 >> 4);

        int sum3 = (target << 11) - (sum2_l << 4);

        const bool neg2 = (sum3 & 0x80000) != 0;
        const bool xnor = !(neg2 ^ neg);

        if (write && pcm.nfs)
        {
            if (xnor)
                *levelcur = sum2_l & 0x7fff;
            else
                *levelcur = (uint16_t)(target << 7);
        }

        if (e == 0)
        {
            *volmul = sum2_l & 0x7ffe;
        }
        else if (e == 1)
        {
            if (xnor)
                *volmul = sum2_l & 0x7ffe;
            else
                *volmul = target << 7;
        }
    }
}

