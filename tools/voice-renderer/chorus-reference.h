#pragma once
#include <cstdint>
// Frozen original PCM chorus-address generator; test-only register view.
struct ChorusReferenceState {
    uint32_t ram1[32][8]{};
    uint16_t ram2[32][16]{};
    bool nfs=true;
};
inline void referenceChorusAdvance(ChorusReferenceState& pcm)
        {
            // address generator

            const bool key = 1;
            const bool okey = (pcm.ram2[31][7] & 0x20) != 0;
            const bool active = key && okey;
            const bool kon = key && !okey;

            bool b15 = (pcm.ram2[31][8] & 0x8000) != 0; // 0
            const bool b6 = (pcm.ram2[31][7] & 0x40) != 0; // 1
            const bool b7 = (pcm.ram2[31][7] & 0x80) != 0; // 1
            int old_nibble = (pcm.ram2[31][7] >> 12) & 15; // 1
            (void)old_nibble; // unused

            int address = (int)pcm.ram1[31][4]; // 0
            int address_end = (int)pcm.ram1[31][0]; // 1 or 2
            int address_loop = (int)pcm.ram1[31][2]; // 2 or 1

            int sub_phase = (pcm.ram2[31][8] & 0x3fff); // 1
            int interp_ratio = (sub_phase >> 7) & 127;
            (void)interp_ratio; // unused
            sub_phase += pcm.ram2[pcm.ram2[31][7] & 31][0]; // 5
            int sub_phase_of = (sub_phase >> 14) & 7;
            if (pcm.nfs)
            {
                pcm.ram2[31][8] &= ~0x3fff;
                pcm.ram2[31][8] |= sub_phase & 0x3fff;
            }


            // address 0
            int address_cnt = address;

            int cmp1 = b15 ? address_loop : address_end;
            int cmp2 = address_cnt;
            bool address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); // 9
            bool next_b15 = b15;

            int next_address = address_cnt; // 11

            cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
            cmp2 = address_cnt;
            int address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

            const bool address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
            const bool address_sub = !address_cmp && b6 && b15;
            if (b7)
                address_cnt2 -= address_add - address_sub;
            else
                address_cnt2 += address_add - address_sub;
            address_cnt = address_cnt2 & 0xfffff; // 11
            b15 = b6 && (b15 ^ address_cmp); // 11

            cmp1 = b15 ? address_loop : address_end;
            cmp2 = address_cnt;
            address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); // 13

            if (sub_phase_of >= 1)
            {
                next_address = address_cnt; // 13
                next_b15 = b15;
            }

            if (active && pcm.nfs)
                pcm.ram1[31][4] = (uint32_t)next_address;

            if (pcm.nfs)
            {
                pcm.ram2[31][8] &= ~0x8000;
                pcm.ram2[31][8] |= (uint16_t)(next_b15 << 15);
            }

            int t1 = address_loop; // 18
            int t2 = (int)pcm.ram1[31][4] - t1; // 19
            int t3 = address_end - t2; // 20
            int t4 = (int)pcm.ram1[31][4]; // 23

            pcm.ram2[29][10] = (uint16_t)t3;
            pcm.ram2[29][11] = (uint16_t)t4;
        }
