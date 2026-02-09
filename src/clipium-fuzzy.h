#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Returns a score >= 0 if query fuzzy-matches target, -1 if no match.
 * Higher score = better match. */
int clipium_fuzzy_match(const char *query, const char *target);

G_END_DECLS
