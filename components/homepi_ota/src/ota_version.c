// components/homepi_ota/src/ota_version.c
// Firmware version comparison, kept free of ESP-IDF headers so that it is tested on the host
// (test/test_ota_version.c).
#include "homepi_ota.h"

#include <stdio.h>
#include <string.h>

// vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-rcN; rc is 0 for a release (sorted after every rc)
static bool parse(const char *v, long n[4])
{
    if (!v || v[0] != 'v') return false;
    int used = 0;
    long rc = 0;
    if (sscanf(v, "v%ld.%ld.%ld%n", &n[0], &n[1], &n[2], &used) != 3) return false;
    const char *rest = v + used;
    if (*rest == '\0') {
        n[3] = 1L << 30;            // a release: newer than any release candidate of the same version
    } else if (sscanf(rest, "-rc%ld%n", &rc, &used) == 1 && rest[used] == '\0' && rc > 0) {
        n[3] = rc;
    } else {
        return false;               // -dirty, -g<hash> and anything else are not comparable
    }
    return n[0] >= 0 && n[1] >= 0 && n[2] >= 0;
}

int hota_version_cmp(const char *a, const char *b, bool *ok)
{
    long x[4], y[4];
    *ok = parse(a, x) && parse(b, y);
    if (!*ok) return 0;
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
