#include <assert.h>
#include "raop_volume.h"
int main(void) {
    assert(raop_volume_db_to_pct(0.0f)    == 100);
    assert(raop_volume_db_to_pct(-30.0f)  == 0);
    assert(raop_volume_db_to_pct(-144.0f) == 0);   /* mute */
    assert(raop_volume_db_to_pct(-15.0f)  == 50);  /* midpoint */
    assert(raop_volume_db_to_pct(5.0f)    == 100); /* clamp high */
    assert(raop_volume_db_to_pct(-100.f)  == 0);   /* clamp low */
    return 0;
}
