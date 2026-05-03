/**
 * @file doip_version.h
 * @brief Library version constants and API visibility macro.
 *
 * Include this header (or the umbrella <doip/doip.h>) to access
 * version information and the DOIP_API export attribute.
 */
#ifndef DOIP_VERSION_H
#define DOIP_VERSION_H

/** @defgroup doip_version Library Version
 *  @brief Compile-time library version constants.
 *  @{
 */
#define DOIP_LIB_VERSION_MAJOR  1   /**< Major version (ABI-breaking changes) */
#define DOIP_LIB_VERSION_MINOR  0   /**< Minor version (backwards-compatible additions) */
#define DOIP_LIB_VERSION_PATCH  0   /**< Patch version (bug-fixes only) */
#define DOIP_LIB_VERSION_STR    "1.0.0"  /**< Human-readable version string */
/** @} */

/**
 * @def DOIP_API
 * @brief Marks a symbol as part of the public library API.
 *
 * When the shared library is built with @c -fvisibility=hidden,
 * only functions annotated with DOIP_API are exported.
 * On Windows this maps to @c __declspec(dllexport).
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef DOIP_BUILDING_LIB
#    define DOIP_API __declspec(dllexport)
#  else
#    define DOIP_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define DOIP_API __attribute__((visibility("default")))
#else
#  define DOIP_API
#endif

#endif /* DOIP_VERSION_H */
