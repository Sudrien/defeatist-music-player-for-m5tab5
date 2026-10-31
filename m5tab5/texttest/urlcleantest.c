/*
 * urlcleantest.c -- urlclean.c: trackers out, everything else kept.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/urlclean.h"

static int checks, failures;

static void t(const char *in, const char *want, int n_want)
{
    char b[512];
    snprintf(b, sizeof(b), "%s", in);
    const int n = urlclean_strip(b);
    checks++;
    if (strcmp(b, want) != 0 || n != n_want) {
        failures++;
        printf("FAIL: [%s] gave [%s] (%d), want [%s] (%d)\n", in, b, n, want, n_want);
    }
}

int main(void)
{
    t("https://a.b/s.aac", "https://a.b/s.aac", 0);
    t("https://a.b/s.aac?utm_source=x", "https://a.b/s.aac", 1);
    t("https://a.b/s.aac?utm_source=x&utm_medium=y&sid=3", "https://a.b/s.aac?sid=3", 2);
    t("https://a.b/s.aac?sid=3&utm_source=x", "https://a.b/s.aac?sid=3", 1);
    t("https://a.b/s.aac?aw_0_1st.playerid=web&aw_0_req.gdpr=1&bitrate=128",
      "https://a.b/s.aac?bitrate=128", 2);
    t("https://a.b/s?lsid=abc&tdtok=q", "https://a.b/s", 2);
    t("https://a.b/s?fbclid=1#StreamName=WDET", "https://a.b/s#StreamName=WDET", 1);
    t("https://a.b/s?x=1#StreamName=WDET", "https://a.b/s?x=1#StreamName=WDET", 0);
    t("https://a.b/s#StreamName=a?utm_source=x", "https://a.b/s#StreamName=a?utm_source=x", 0);
    t("https://a.b/s?", "https://a.b/s?", 0);
    t("https://a.b/s?&&utm_x=1&&y=2", "https://a.b/s?y=2", 1);
    t("https://a.b/s?utm_=1", "https://a.b/s?utm_=1", 0);     /* the bare prefix is not a name */
    t("https://a.b/s?UTM_SOURCE=1", "https://a.b/s", 1);
    t("https://a.b/s?awesome=1", "https://a.b/s?awesome=1", 0);
    t("https://a.b/s?utm_source", "https://a.b/s", 1);         /* no '=' */
    /* 5205 */
    t("https://a.b/s.aac?listeningSessionID=7d2f-11&sid=3", "https://a.b/s.aac?sid=3", 1);
    t("https://a.b/s.aac?listeningSessionId=x", "https://a.b/s.aac", 1);
    checks++;
    if (urlclean_strip(NULL) != 0) { failures++; printf("FAIL: NULL\n"); }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
