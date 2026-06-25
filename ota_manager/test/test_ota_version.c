#include <assert.h>
#include <stdio.h>
#include <stdbool.h>

bool ota_version_is_newer(const char *remote, const char *local);

int main(void) {
    assert(ota_version_is_newer("1.2.3", "1.2.2") == true);
    assert(ota_version_is_newer("1.2.3", "1.2.3") == false);
    assert(ota_version_is_newer("1.2.2", "1.2.3") == false);
    assert(ota_version_is_newer("v1.2.3", "1.2.3") == false);   /* leading v tolerated, equal */
    assert(ota_version_is_newer("1.2.3-4-gabc", "1.2.2") == true); /* suffix ignored */
    assert(ota_version_is_newer("2.0.0", "1.9.9") == true);
    assert(ota_version_is_newer("1.10.0", "1.9.0") == true);    /* numeric, not lexical */
    assert(ota_version_is_newer("garbage", "1.0.0") == false);  /* unparseable -> false */
    assert(ota_version_is_newer("1.0.0", "garbage") == false);

    printf("ALL PASS\n");
    return 0;
}
