#include "clipium-fuzzy.h"
#include <string.h>
#include <ctype.h>

int
clipium_fuzzy_match(const char *query, const char *target)
{
    if (!query || !*query)
        return 0;
    if (!target || !*target)
        return -1;

    const char *q = query;
    const char *t = target;
    int score = 0;
    int consecutive = 0;
    gboolean first_char_bonus = TRUE;

    while (*q && *t) {
        char qc = (char)tolower((unsigned char)*q);
        char tc = (char)tolower((unsigned char)*t);

        if (qc == tc) {
            score += 1;
            /* Bonus for consecutive matches */
            if (consecutive > 0)
                score += consecutive * 2;
            consecutive++;
            /* Bonus for matching at start */
            if (first_char_bonus && t == target)
                score += 10;
            /* Bonus for matching after separator */
            if (t > target) {
                char prev = *(t - 1);
                if (prev == ' ' || prev == '/' || prev == '_' || prev == '-' || prev == '.')
                    score += 5;
            }
            q++;
        } else {
            consecutive = 0;
        }
        first_char_bonus = FALSE;
        t++;
    }

    /* All query chars must be consumed */
    if (*q)
        return -1;

    return score;
}
