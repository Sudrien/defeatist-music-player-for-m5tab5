/* SPDX-License-Identifier: MIT
 *
 * The PCM hash of the first BW_MAX_AUDIO_MS of each file of
 * test_audio_files/ that boxwin decodes, as a host gets it
 * (u-defeatist-0010). Written by `make -C boxwin/test expect`; test/host.c
 * fails if a file decodes to anything else. `exact`: an integer decoder,
 * which must give this on any machine.
 */
static const struct { const char *name; uint32_t pcm; bool exact; } bw_expect[] = {
    { "03 mp3-cbr-noxing.mp3", 0x9fdd6ecau, false },
    { "04 mp3-vbr-xing.mp3", 0x510d1673u, false },
    { "12 mp3-bigart-noxing.mp3", 0x9fdd6ecau, false },
    { "40 mp2.mp2", 0x5c34823au, false },
    { "48 ape-normal.ape", 0x96460869u, true },
    { "49 ape-fast.ape", 0x96460869u, true },
    { "50 ape-high.ape", 0x96460869u, true },
    { "51 ape-extrahigh.ape", 0x96460869u, true },
    { "52 ape-insane.ape", 0x96460869u, true },
    { "53 ape-insane-noisy.ape", 0x5d2fee52u, true },
    { "54 ape-24bit.ape", 0x01bca688u, true },
    { "55 ape-mono-22k.ape", 0xf8504d37u, true },
    { "56 ape-apev2-tag.ape", 0x96460869u, true },
    { "41 dsf-dsd64.dsf", 0x7508c734u, false },
    { "42 dsf-dsd128.dsf", 0x79723799u, false },
    { "43 dsf-dsd256.dsf", 0xafb74f16u, false },
    { "44 dsf-dsd64-mono-odd.dsf", 0x8bc393a7u, false },
    { "45 dff-dsd64.dff", 0x09f80b7bu, false },
    { "46 dff-dsd64-mono.dff", 0x720bbd8au, false },
};
