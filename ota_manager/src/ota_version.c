#include <stdbool.h>
#include <stdio.h>

#ifndef UNIT_TEST
#include "esp_log.h"
static const char *TAG = "OTA_MGR";
#endif

static bool parse_semver(const char *s, unsigned *maj, unsigned *min, unsigned *pat) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    return sscanf(s, "%u.%u.%u", maj, min, pat) == 3;
}

bool ota_version_is_newer(const char *remote, const char *local) {
    unsigned rj, rn, rp, lj, ln, lp;
    if (!parse_semver(remote, &rj, &rn, &rp) || !parse_semver(local, &lj, &ln, &lp)) {
#ifndef UNIT_TEST
        ESP_LOGW(TAG, "ota_version_is_newer: unparseable version string");
#else
        printf("ota_version_is_newer: unparseable version string\n");
#endif
        return false;
    }
    if (rj != lj) return rj > lj;
    if (rn != ln) return rn > ln;
    return rp > lp;
}
