/*
 * urlclean.h -- tracking parameters out of a stream URL.
 *
 * 5204. A stream URL copied from a station's web player often carries
 * query parameters that identify the listener or the ad campaign rather
 * than the stream: AdsWizz's aw_0_*, Triton's lsid/tdtok, the utm_*
 * family, click ids. Saved into stations.m3u they are sent on every
 * connect for as long as the station is kept. MPD's `add` and
 * `playlistadd` of a URL take them out first.
 *
 * Only names on the list below are dropped; every other parameter is
 * the station's own and stays, in its order. A '#' fragment -- Cantata's
 * "#StreamName=" -- is kept. PURE, host-tested in texttest/urlcleantest.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

/* Clean `url` in place. The number of parameters removed. */
int urlclean_strip(char *url);

/* Whether one parameter name (up to its '=') is a tracker. */
bool urlclean_is_tracker(const char *name, int len);
