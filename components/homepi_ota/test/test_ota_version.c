// Host test of the version comparison: cc -Istub -I../include test_ota_version.c ../src/ota_version.c && ./a.out
#include <stdio.h>
#include "homepi_ota.h"

static int fails = 0;
static void expect(const char *a, const char *b, int want, int want_ok)
{
    bool ok;
    int r = hota_version_cmp(a, b, &ok);
    int got = r < 0 ? -1 : r > 0 ? 1 : 0;
    if (ok != want_ok || (ok && got != want)) {
        printf("FAIL %s vs %s: got %d ok=%d, want %d ok=%d\n", a, b, got, ok, want, want_ok);
        fails++;
    }
}

int main(void)
{
    expect("v2.4.1", "v2.4.0", 1, 1);
    expect("v2.4.0", "v2.4.1", -1, 1);
    expect("v2.4.0", "v2.4.0", 0, 1);
    expect("v2.4.0", "v2.4.0-rc2", 1, 1);       // release newer than its candidates
    expect("v2.4.0-rc2", "v2.4.0-rc1", 1, 1);
    expect("v2.4.1-rc1", "v2.4.0", 1, 1);
    expect("v2.10.0", "v2.9.9", 1, 1);          // numeric, not lexical
    expect("v3.0.0", "v2.99.99", 1, 1);
    expect("v2.3.2-dirty", "v2.4.0", 0, 0);     // not comparable: refused
    expect("2.4.1", "v2.4.0", 0, 0);
    expect("v2.4", "v2.4.0", 0, 0);
    expect("v2.4.1-rc0", "v2.4.0", 0, 0);
    expect("v2.4.1x", "v2.4.0", 0, 0);
    expect(NULL, "v2.4.0", 0, 0);
    printf(fails ? "%d FAILED\n" : "all version tests passed\n", fails);
    return fails != 0;
}
