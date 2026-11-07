/* SPDX-License-Identifier: MIT
 * sdm.c -- 2nd-order sigma-delta modulator for the DSD test files.
 * stdin: float32 LE stereo interleaved at the DSD rate. argv[1]: channels
 * to keep (1|2). stdout: bytes, MSB first, one byte per channel per 8 samples,
 * channels interleaved (DFF's layout). */
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    int nch = argc > 1 ? atoi(argv[1]) : 2;
    const float gain = 0.6f;            /* DSD is not meant to exceed ~50%; 2nd order is stable here */
    double y1[2] = {0}, y2[2] = {0};
    unsigned char acc[2] = {0};
    static float buf[1 << 16];
    int bit = 0;
    size_t n;
    while ((n = fread(buf, sizeof(float), 1 << 16, stdin)) >= 2) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            for (int c = 0; c < nch; c++) {
                double x = buf[i + c] * gain;
                double v = y2[c] >= 0 ? 1.0 : -1.0;
                y1[c] += x - v;
                y2[c] += y1[c] - v;
                acc[c] = (unsigned char)((acc[c] << 1) | (v > 0));
            }
            if (++bit == 8) {
                fwrite(acc, 1, nch, stdout);
                bit = 0;
            }
        }
    }
    return 0;
}
