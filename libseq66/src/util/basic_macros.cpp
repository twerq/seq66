/*
 *  This file is part of seq66.
 *
 *  seq66 is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  seq66 is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with seq66; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * \file          basic_macros.cpp
 *
 *  This module defines some informative functions that are actually
 *  better off as functions.
 *
 * \library       seq66 application
 * \author        Chris Ahlstrom
 * \date          2018-11-10
 * \updates       2019-04-21
 * \license       GNU GPLv2 or above
 *
 *  These functions are used in macros such as func_message().
 */

#include <assert.h>
#include <cstdarg>                      /* see "man stdarg(3)"              */
#include <iostream>

#include "cfg/settings.hpp"             /* seq66::rc()                      */
#include "util/basic_macros.hpp"        /* basic macros-cum-functions       */

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq66
{

bool
is_debug ()
{
#if defined SEQ66_PLATFORM_DEBUG
    return true;
#else
    return rc().verbose();
#endif
}

/**
 *  Provides a way to still get the benefits of assert() output in release
 *  mode, without aborting the application.
 *
 * \todo
 *      This can slow down client code slightly, and it would be good to
 *      reduce the impact.
 *
 * \param ptr
 *      Provides the pointer to be tested.
 *
 * \param context
 *      Provides context for the message.  Usually the __func__ macro is
 *      the best option for this parameter.
 *
 * \return
 *      Returns true in release mode, if the pointer was not null.  In
 *      debug mode, will always return true, but the assert() will abort
 *      the application anyway.
 */

#if defined SEQ66_PLATFORM_DEBUG

bool
not_nullptr_assert (void * ptr, const std::string & context)
{
    bool result = true;
    int flag = int(not_nullptr(ptr));
    if (! flag)
    {
        std::cerr << "? null pointer in context " << context << std::endl;
        result = false;
    }

#if defined SEQ66_PLATFORM_GNU_TMI              /* does not work in Mingw   */
    int errornumber = flag ? 0 : 1 ;
    assert_perror(errornumber);
#else
    assert(flag);
#endif

    return result;
}

#endif  // SEQ66_PLATFORM_DEBUG

/**
 *  This function concatenates two C string pointers and returns them as
 *  a string message.  Note that we don't bother with error-checking the
 *  pointers.  You're on your own, Hoss.
 *
 * \param m1
 *      The first message, often a __func__ macro.
 *
 * \param m2
 *      The second message.
 *
 * \return
 *      Returns "m1: m2" as a standard C++ string.
 */

std::string
message_concatenate (const std::string & m1, const std::string & m2)
{
    std::string result(m1);
    result += ": ";
    result += m2;
    return result;
}

/**
 *  Common-code for console informationational messages.  Adds markers and a
 *  newline.
 *
 * \param msg
 *      The message to print, sans the newline.
 *
 * \return
 *      Returns true, so that the caller can show the message and return the
 *      status at the same time.
 */

bool
info_message (const std::string & msg)
{
    if (is_debug())
        std::cout << "[" << msg << "]" << std::endl;    /* end and flush    */

    return true;
}

/**
 *  Common-code for console warning messages.  Adds markers and a newline.
 *
 * \param msg
 *      The message to print, sans the newline.
 *
 * \return
 *      Returns true, so that the caller can show the message and return the
 *      status at the same time.
 */

bool
warn_message (const std::string & msg)
{
    if (is_debug())
        std::cout << "[Warning: " << msg << "]" << std::endl;

    return true;
}

/**
 *  Common-code for error messages.  Adds markers, and returns false.
 *
 * \param msg
 *      The message to print, sans the newline.
 *
 * \return
 *      Returns false for convenience/brevity in setting function return
 *      values.
 */

bool
error_message (const std::string & msg)
{
    if (is_debug())
        std::cerr << "[ERROR: " << msg << "]" << std::endl;

    return false;
}

/**
 *  Common-code for error messages involving file issues, a very common use case
 *  in error-reporting.  Adds markers, and returns false.
 *
 * \param tag
 *      The message to print, sans the newline.
 *
 * \param filename
 *      The name of the file to be shown.
 *
 * \return
 *      Returns false for convenience/brevity in setting function return
 *      values.
 */

bool
file_error (const std::string & tag, const std::string & filename)
{
    if (is_debug())
    {
        std::cerr
            << "[ERROR: " << tag << " '" << filename << "'" << "]"
            << std::endl
            ;
    }
    return false;
}

/**
 *  Shows a path-name (or other C++ string) as an info message.
 *
 * \todo
 *      Using this function in place of file_message() or other means of
 *      printing file specifications.
 *
 * \param tag
 *      Provides the text to precede the name of the path.
 *
 * \param path
 *      Provides the path-name to print.
 */

void
pathprint (const std::string & tag, const std::string & path)
{
    std::cout << "[" << tag << " " << path << "]" << std::endl;
}

/**
 *  Shows a boolean value as a "true/false" info message.
 *
 * \param tag
 *      Provides the text to precede the boolean value.
 *
 * \param flag
 *      Provides the boolean value to print.
 */

void
boolprint (const std::string & tag, bool flag)
{
    std::string fmt = tag + " %s";
    msgprintf(msg_level::info, fmt, flag ? "true" : "false");
}

/**
 *  Shows a boolean value as an "on/off" info message.
 *
 * \param tag
 *      Provides the text to precede the boolean value.
 *
 * \param flag
 *      Provides the boolean value to print.
 */

void
toggleprint (const std::string & tag, bool flag)
{
    std::string fmt = tag + " %s";
    msgprintf(msg_level::info, fmt, flag ? "on" : "off");
}

/**
 *  Formats a variable list of input and returns the formatted string.
 *  Not the fastest function ever, but useful.  Adapted from the last sample
 *  in:
 *
 *      https://stackoverflow.com/questions/19009094/
 *              c-variable-arguments-with-stdstring-only
 *
 *  -#  Initialize the usage of a variable-argument array.
 *  -#  Acquire the destination size from a copy of the variable-argument
 *      array; mock the formatting with vsnprintf() and a null destination to
 *      find out how many characters will be constructed.  The copy is
 *      necessary to avoid messing up the original variable-argument array.
 *  -#  Return a formatted string without needing memory management, and
 *      without assuming any compiler/platform-specific behavior.
 *
 * \param lev
 *      Indicates the desired message level: info, warn, or error.
 *
 * \param fmt
 *      Indicates the desired format for the message.  Use "%s" for strings.
 *
 * \param ...
 *      Provides the printf() parameters for the format string.  Please note
 *      that C++ strings cannot be used directly... std::string::c_str() must
 *      be used.
 */

void
msgprintf (msg_level lev, std::string fmt, ...)
{
    if (is_debug() && ! fmt.empty())
    {
        va_list args;                                       /* Step 1       */

        /*
         * cppcheck: Using reference 'fmt' as parameter for va_start() results
         * in undefined behaviour.  Also claims we need to add a va_end(), so we
         * did, below, on 2019-04-21.
         */

        va_start(args, fmt);

        std::string output = formatted(fmt, args);          /* Steps 2 & 3  */
        switch (lev)
        {
        case msg_level::info:

            std::cout << "[" << output << "]" << std::endl;
            break;

        case msg_level::warn:

            std::cout << "[Warning: " << output << "]" << std::endl;
            break;

        case msg_level::error:

            std::cerr << "[ERROR: " << output << "]" << std::endl;
            break;
        }

        va_end(args);                                       /* 2019-04-21   */
    }
}

/**
 *  Takes a format string and a variable-argument list and returns the
 *  formatted string.
 *
 *  Although currently a public function, its usage is meant to be internal
 *  for the msgprintf() function.  See that function's description.
 *
 *  C++11 is required, due to the use of va_copy().
 *
 * \param fmt
 *      Provides the printf() format string.
 *
 * \param args
 *      Provides the variable-argument list.
 *
 * \return
 *      Returns the formatted string.  If an error occurs, the string is
 *      empty.
 */

std::string
formatted (const std::string & fmt, va_list args)
{
    std::string result;
    va_list args_copy;                                      /* Step 2       */
    va_copy(args_copy, args);

    const char * const szfmt = fmt.c_str();
    int ilen = std::vsnprintf(NULL, 0, szfmt, args_copy);
    va_end(args_copy);

    if (ilen > 0)
    {
        std::vector<char> dest(ilen + 1);                   /* Step 3       */
        std::vsnprintf(dest.data(), dest.size(), szfmt, args);
        result = std::string(dest.data(), dest.size());
    }
    va_end(args);
    return result;
}

/**
 *  Easy conversion from boolean to string, "true" or "false".
 */

const std::string &
bool_string (bool x)
{
    static const std::string s_true { "true" };
    static const std::string s_false { "false" };
    return x ? s_true : s_false ;
}

/**
 *  Easy conversion from boolean to character, "T" or "F".
 */

char
bool_char (bool x)
{
    static char s_true { 'T' };
    static char s_false { 'F' };
    return x ? s_true : s_false ;
}

}           // namespace seq66

/*
 * basic_macros.cpp
 *
 * vim: sw=4 ts=4 wm=4 et ft=cpp
 */

