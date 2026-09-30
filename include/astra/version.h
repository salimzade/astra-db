#ifndef ASTRA_VERSION_H
#define ASTRA_VERSION_H

/** Product name, used for banners and diagnostics. */
#define ASTRA_DB_NAME "AstraDB"

/*
 * Single source of truth for the AstraDB version.
 *
 * The build system parses these three defines directly (see the top level
 * CMakeLists.txt) to derive the CMake project version, so each one must stay
 * on a single line in the exact form:
 *
 *     #define ASTRA_DB_VERSION_<PART> <unsigned decimal integer>
 *
 * Do not duplicate the version anywhere else in the tree.
 */
#define ASTRA_DB_VERSION_MAJOR 0
#define ASTRA_DB_VERSION_MINOR 1
#define ASTRA_DB_VERSION_PATCH 0

#define ASTRA_DB_STRINGIFY_IMPL(x) #x
#define ASTRA_DB_STRINGIFY(x) ASTRA_DB_STRINGIFY_IMPL(x)

/** Version as "MAJOR.MINOR.PATCH", derived from the defines above. */
#define ASTRA_DB_VERSION_STRING                                 \
    ASTRA_DB_STRINGIFY(ASTRA_DB_VERSION_MAJOR)                 \
    "." ASTRA_DB_STRINGIFY(ASTRA_DB_VERSION_MINOR)             \
    "." ASTRA_DB_STRINGIFY(ASTRA_DB_VERSION_PATCH)

#endif /* ASTRA_VERSION_H */
