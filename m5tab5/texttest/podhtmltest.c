/*
 * podhtmltest.c -- an episode's description, from HTML to text (8026).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "podhtml.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                           \
    checks++;                                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL (%s:%d): ", __FILE__, __LINE__);                 \
        printf(__VA_ARGS__);                                            \
        printf("\n");                                                   \
        failures++;                                                     \
    }                                                                   \
} while (0)

#define T(in, want) do { char b[512]; snprintf(b, sizeof(b), "%s", in); podhtml_text(b); \
    CHECK(!strcmp(b, want), "[%s] -> [%s], wanted [%s]", in, b, want); } while (0)

int main(void)
{
    printf("podhtmltest\n");
    printf("  tags\n");
    T("<p>On this episode, <strong>Jane</strong> talks.</p><p>Second.</p>",
      "On this episode, Jane talks.\nSecond.");
    T("one<br>two<br/>three<BR />four", "one\ntwo\nthree\nfour");
    T("<p>a</p>\n\n<p></p><p></p><p>b</p>", "a\nb");
    T("<a href=\"https://x.com/?a=1&b=2\">the site</a> here", "the site here");
    T("<ul><li>one</li><li>two</li></ul>after", "one\ntwo\nafter");
    T("x<script>var a = '<p>';</script>y<style>p{}</style>z", "xyz");
    T("word<span>next</span>", "word next");
    T("<p>unclosed <b", "unclosed");
    printf("  entities\n");
    T("Tom &amp; Jerry &lt;3 &quot;q&quot; it&#39;s &apos;x&apos;", "Tom & Jerry <3 \"q\" it's 'x'");
    T("caf&#233; &#x65E5; &#128512;", "caf\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x98\x80");
    T("a&nbsp;b&#160;c", "a b c");
    T("&unknown; &#; &#xZZ; & alone", "&unknown; &#; &#xZZ; & alone");
    T("&#55296;", "\xef\xbf\xbd");
    printf("  spaces and ends\n");
    T("   lots    of\t\tspace \r\n here  ", "lots of space here");
    T("\n\n<br><br>top", "top");
    T("end<br><br><br>", "end");
    T("", "");
    T("\xd8\xb4\xd8\xb9\xd8\xb1 <b>\xda\xa9\xd8\xb3\xd8\xaa</b>", "\xd8\xb4\xd8\xb9\xd8\xb1 \xda\xa9\xd8\xb3\xd8\xaa");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
