#if ! defined SEQ66_BASIC_MACROS_HPP
#define SEQ66_BASIC_MACROS_HPP

/**
 * \file          basic_macros.hpp
 *
 *    This module provides macros for generating simple messages, MIDI
 *    parameters, and more.
 *
 * \library       seq66
 * \author        Chris Ahlstrom and other authors; see documentation
 * \date          2018-11-10
 * \updates       2019-09-23
 * \version       $Revision$
 * \license       GNU GPL v2 or above
 *
 *  This file defines macros for both C and C++11 (or greater) code.  Seq66
 *  requires C++11 support or above.  The macros in this file cover:
 *
 *       -  Compiler-support macros.
 *       -  Error and information output macros.
 *       -  One or more global debugging functions that are better suited
 *          than using a macro.
 *
 *  This module replaces Seq64's easy_macros.
 */

#include <cstdio>
#include <string>

#include "seq66_features.hpp"           /* platform and config macros       */
#include "util/basic_macros.h"          /* C-compatible definitions         */

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq66
{

/**
 *  An indicator of the message level.
 */

enum class msg_level
{
    info,
    warn,
    error
};

/*
 * Global functions.  The not_nullptr_assert() function is a macro in
 * release mode, to speed up release mode.  It cannot do anything at
 * all, since it is used in the conditional part of if-statements.
 */

#if defined SEQ66_PLATFORM_DEBUG
#if defined __cplusplus
extern bool not_nullptr_assert (void * ptr, const std::string & context);
#else
#define not_nullptr_assert(ptr, context) (not_nullptr(ptr))
#endif  // C++
#else
#define not_nullptr_assert(ptr, context) (not_nullptr(ptr))
#endif

extern std::string message_concatenate
(
    const std::string & m1, const std::string & m2
);
extern bool info_message (const std::string & msg);
extern bool warn_message (const std::string & msg);
extern bool error_message (const std::string & msg);
extern bool file_error (const std::string & tag, const std::string & filename);
extern void pathprint (const std::string & tag, const std::string & path);
extern void boolprint (const std::string & tag, bool flag);
extern void toggleprint (const std::string & tag, bool flag);
extern void msgprintf (seq66::msg_level lev, std::string fmt, ...);
extern std::string formatted (const std::string & fmt, va_list args);
extern const std::string & bool_string (bool x);
extern char bool_char (bool x);
extern bool is_debug ();

/**
 *  Prepends "Debug" to the string and then printf()'s it.  No need
 *  for an iostream for this simple task.
 *
 * \param msg
 *      The debug message to be shown.
 */

inline void
debug_message (const std::string & msg)
{
    if (! msg.empty())
        printf("Debug: '%s'\n", msg.c_str());
}

}               // namespace seq66

#endif          // SEQ66_BASIC_MACROS_HPP

/*
 * basic_macros.hpp
 *
 * vim: sw=4 ts=4 wm=4 et ft=cpp
 */

