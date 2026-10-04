/* Collect startup settings in one pass instead of repeatedly walking environ.
 * Values borrow the original environment strings; hacman's startup does not
 * mutate them. Shell setup exports happen later in separate child processes. */

#include "hacman.h"

#include <string.h>

extern char **environ;

const hm_env *hm_env_get(void)
{
    static hm_env values;
    static int    initialized;
    char        **entry;

    if (initialized) return &values;

    for (entry = environ; entry != NULL && *entry != NULL; ++entry) {
        const char *name = *entry;

        /* Most environment variables can be rejected with one byte. Share
         * the HACMAN prefix check before distinguishing the three settings. */
        if (name[0] != 'H') continue;
        if (strncmp(name, "HACMAN", 6) == 0) {
            const char *suffix = name + 6;

            /* Like getenv, the first exact match wins, even when its value is
             * empty. Non-NULL pointers distinguish an empty value from absence. */
            if (suffix[0] == '=' && values.options == NULL) {
                values.options = suffix + 1;
            } else if (strncmp(suffix, "_DEBUG=", 7) == 0 && values.debug == NULL) {
                values.debug = suffix + 7;
            } else if (strncmp(suffix, "_CACHE=", 7) == 0 && values.cache == NULL) {
                values.cache = suffix + 7;
            }
        } else if (strncmp(name, "HOME=", 5) == 0 && values.home == NULL) {
            values.home = name + 5;
        }
    }
    initialized = 1;
    return &values;
}
