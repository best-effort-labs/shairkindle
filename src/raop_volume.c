#include "raop_volume.h"
int raop_volume_db_to_pct(float db) {
    if (db <= -30.0f) return 0;      /* includes the -144 mute sentinel */
    if (db >= 0.0f)  return 100;
    float pct = (db + 30.0f) / 30.0f * 100.0f;   /* -30..0 -> 0..100 */
    return (int)(pct + 0.5f);
}
