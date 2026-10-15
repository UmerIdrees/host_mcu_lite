#ifndef HML_EXPORT_H
#define HML_EXPORT_H

/*
 * Attribute-noise suppression, declared before anything else in this header.
 *
 * HML_API puts a visibility attribute on a declaration. Where it lands on a
 * `static` function -- hml_port.h's byte-order helpers, and the private
 * hml_bridge.h helpers in src/bridge/ -- the compiler is right to ignore the
 * attribute, but it still warns ("'visibility' attribute ignored"), and
 * -Werror turns that into a build failure for code this header did not touch.
 * The attribute and the diagnostic are both introduced here, so this header
 * also neutralises the diagnostic. -Wno-attributes / -Wno-ignored-attributes
 * respectively; the rest of the project's warning set is untouched.
 *
 * Define HML_NO_VISIBILITY to get the pre-0.2.1 behaviour: HML_API expands to
 * nothing and the warning stays enabled, at the cost of a .so that exports
 * whatever the object files happen to contain.
 */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wignored-attributes"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wattributes"
#endif

/*
 * Symbol visibility for the hml partitions (see docs/final_partitioning.md).
 *
 * hml ships as a set of independent components, each of which is produced twice:
 * once as a static archive (libhml_core.a) and once as a shared object
 * (libhml_core.so). The two builds consume the *same* sources and the *same*
 * public headers, so the export decoration cannot depend on a per-target
 * compile definition on ELF platforms -- otherwise the static arm and the
 * shared arm could not be produced from one set of objects.
 *
 * Therefore:
 *   - GCC/Clang (ELF, Mach-O): HML_API is visibility("default") unconditionally.
 *     On a static archive the attribute is inert; on a shared object built with
 *     -fvisibility=hidden it is what actually exports the symbol.
 *   - MSVC/PE: there the decoration genuinely differs per arm, so the side is
 *     selected by macro. Build the DLL with -DHML_BUILDING_DLL, and build every
 *     static consumer with -DHML_STATIC (hml's exported CMake targets already
 *     carry that definition for you).
 *
 * HML_API is purely additive: it is applied to declarations that already
 * existed, and it changes no signature. hml_types.h includes this header, so
 * every public header sees it.
 *
 * Escape hatches, for toolchains whose GCC-compatible frontend rejects the
 * attribute:
 *   -DHML_NO_VISIBILITY   HML_API and HML_HIDDEN both expand to nothing.
 *   -DHML_STATIC          same as HML_NO_VISIBILITY on MSVC; also accepted on
 *                         ELF as an explicit "I am linking an archive".
 */

#if defined(HML_NO_VISIBILITY)
#  define HML_API
#  define HML_HIDDEN
#elif defined(_WIN32) || defined(__CYGWIN__)
#  if defined(HML_STATIC) || defined(HML_STATIC_DEFINE)
#    define HML_API
#    define HML_HIDDEN
#  elif defined(HML_BUILDING_DLL)
#    define HML_API __declspec(dllexport)
#    define HML_HIDDEN
#  else
#    define HML_API __declspec(dllimport)
#    define HML_HIDDEN
#  endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#  define HML_API __attribute__((visibility("default")))
#  define HML_HIDDEN __attribute__((visibility("hidden")))
#else
#  define HML_API
#  define HML_HIDDEN
#endif

/*
 * Header-defined helpers.
 *
 * A visibility attribute on a `static` function is ignored by GCC and Clang,
 * and -Werror turns that diagnostic into a build failure, so the inline
 * helpers in hml_port.h use HML_INLINE and carry no attribute. The declared
 * signatures of those helpers are unchanged from 0.2.0.
 */
#define HML_INLINE static inline

#endif /* HML_EXPORT_H */
