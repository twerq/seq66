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
 * \file          configfile.cpp
 *
 *  This module defines the base class for configuration and options
 *  files.
 *
 * \library       seq66 application
 * \author        Chris Ahlstrom
 * \date          2018-11-23
 * \updates       2019-03-26
 * \license       GNU GPLv2 or above
 *
 */

#include <cctype>                       /* std::isspace(), std::isdigit()   */

#include "cfg/configfile.hpp"           /* seq66:: configfile class         */
#include "cfg/settings.hpp"             /* seq66::rc() accessor             */
#include "util/calculations.hpp"        /* seq66::string_to_integer() etc.  */
#include "util/strfunctions.hpp"        /* strncompare() for std::string    */

#if defined SEQ66_PLATFORM_DEBUG_TMI
#include <iostream>                     /* std::cout, std::cerr             */
#endif

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq66
{

/**
 *  Provides the string plus rcsettings constructor for a configuration file.
 *
 * \param name
 *      The name of the configuration file.
 *
 * \param rcs
 *      A reference to the rcsettings object to hold the settings of the
 *      configuration file.  It applies to all configuration files, including
 *      usrfile.
 */

configfile::configfile (const std::string & name, rcsettings & rcs) :
    m_rc            (rcs),
    m_error_message (),
    m_name          (name),
    m_line          ()                  /* input std::string                */
{
    // no code needed
}

/**
 *  Returns a pre-trimmed line from the configuration file.
 *
 * \return
 *      Returns a copy of line(), but trimmed of white space.
 */

std::string
configfile::trimline () const
{
    std::string result = line();
    result = trim(result);
    return result;
}

/**
 * [comments]
 *
 * Header commentary is skipped during parsing.  However, we now try to
 * read an optional comment block.
 */

std::string
configfile::parse_comments (std::ifstream & file)
{
    std::string result;
    if (line_after(file, "[comments]", false))  /* get 1st line w/out strip */
    {
        do
        {
            result += line();
            result += "\n";

        } while (next_data_line(file, false));  /* ditto                    */
    }
    return result;
}

/**
 *  Helper function for error-handling.  It assembles a message and then
 *  passes it to error_message().
 *
 * \param sectionname
 *      Provides the name of the section for reporting the error.
 *
 * \param additional
 *      Additional context information to help in finding the error.
 *
 * \return
 *      Always returns false.
 */

bool
configfile::make_error_message
(
    const std::string & sectionname,
    const std::string & additional
)
{
    std::string msg = "BAD OR MISSING DATA in [";
    msg += sectionname;
    msg += "]: ";
    if (! additional.empty())
        msg += additional;

    errprint(msg);
    error_message(msg);
    return false;
}

/**
 *  A useful intermediate function to save a call and allow for debugging.
 *  In addition, it also trims basic white-space from the beginning and end of
 *  the line, to make parsing a little more robust.
 *
 *  Also replaces file.get_line(m_line, sizeof m_line) with an std::string
 *  get_line().
 *
 * \param file
 *      The refernce to the opened input file-stream.
 *
 * \param strip
 *      If true, then strip out any following comment in the line, as denoted
 *      by a hash-tag character not enclosed in single- or double-quotes.
 *      The default value is false.
 *
 * \return
 *      Returns the value of file.good().  If the trimmed line is empty,
 *      returns true, too; the caller can ignore the line.
 */

bool
configfile::get_line (std::ifstream & file, bool strip)
{
    (void) std::getline(file, m_line);
    if (strip)
    {
        m_line = trim(m_line);
        m_line = strip_comments(m_line);
    }

#if defined SEQ66_PLATFORM_DEBUG_TMI
    std::cout << "line: '" << m_line << "'" << std::endl;
#endif

    return file.good();             // TOO MUCH: && ! m_line.empty();
}

/**
 *  Gets the next line of data from an input stream.  If the line starts with
 *  a number-sign, or a null, it is skipped, to try the next line.  This
 *  occurs until a section marker ("[") or an EOF is encountered.
 *
 *  Member m_line is a "return" value (side-effect).
 *
 * \param file
 *      Points to an input stream.  We converted this item to a reference;
 *      pointers can be subject to problems.  For example, what if someone
 *      passes a null pointer?  Nonetheless, this is a small library.  Since
 *      this function has this parameter, the caller can deal with multiple
 *      files at the same time.
 *
 * \param strip
 *      If true (the default), trims white space and strips out hash-tag
 *      comments.
 *
 * \return
 *      Returns true if a presumed data line was found.  False is returned if
 *      not found before an EOF or a section marker ("[") is found.  This is a
 *      a new (ca 2016-02-14) feature of this function, to assist in adding
 *      new data to the file without crapping out on old-style configuration
 *      files.
 */

bool
configfile::next_data_line (std::ifstream & file, bool strip)
{
    bool result = get_line(file, strip);        /* zaps whitespace/comments */
    if (result)
    {
        char ch = m_line[0];
        while ((ch == '#' || ch == '[' || ch == 0) && ! file.eof())
        {
            if (m_line[0] == '[')               /* we hit the next section  */
            {
                result = false;
                break;
            }
            if (get_line(file))                 /* trims the white space    */
            {
                ch = m_line[0];
            }
            else
            {
                result = false;
                break;
            }
        }
        if (file.eof())
            result = false;
    }
    return result;
}

/**
 *  Acts like a combination of line_after() and next_data_line(), but
 *  requires a specific variable-name to be found.  For example, given the
 *  following lines (blank lines are simply ignored):
 *
\verbatim
        [loop-control]
        liveplay = true
        bpm = 120
        name = "Funky"
\endverbatim
 *
 *  <code>std::string s = next_variable(f, "[loop-control]", "name");</code>
 *  will return <code>s = "Funky"</code>.
 *
 *  The spaces around the '=' are optional.  This function is meant to
 *  better support an "INI" style of value specification.  The variable name
 *  should be obvious, whereas in the Seq24 implementation we had to append a
 *  comment to make it easier for the user to comprehend the configuration file.
 *
 * \warning
 *      This function assumes that the next_data_line() function
 *
 * \param file
 *      Provides the file-stream that is being read.
 *
 * \param tag
 *      Provides the section tag (e.g. "[midi-control]") to be found before the
 *      variable name parameter can be processed.
 *
 * \param variablename
 *      Provides the variablename to be found in the specified tag section.
 *
 * \return
 *      If the "variablename = value" clause is found, then the
 *      the value that is read is returned.  Otherwise, an empty string is
 *      returned, which might be an error.
 */

std::string
configfile::get_variable
(
    std::ifstream & file,
    const std::string & tag,
    const std::string & variablename
)
{
    std::string result;
    bool good = line_after(file, tag);
    while (good)                                /* not at end of section?   */
    {
        if (! line().empty())                   /* any value in section?    */
        {
            std::string::size_type epos = line().find_first_of("=");
            good = epos != std::string::npos;
            if (good)
            {
                /*
                 * See if there is any space after the variable name, but
                 * before the "=" sign.  If so, the first space, not the "="
                 * sign, terminates the variable name.
                 */

                std::string::size_type spos = line().find_first_of(" ");
                if (spos > epos)
                    spos = epos;

                std::string vname = line().substr(0, spos);
                if (vname == variablename)
                {
                    /*
                     *  Now get the first non-space after the "=" sign.  If
                     *  there is a quote character ("=" or "'"), then see if
                     *  there a match quote, and get everything inside the
                     *  quotes.  Otherwise, grab the first token on the value
                     *  (right) side.
                     */

                    bool havequotes = false;
                    char quotechar[2] = { 'x', 0 };
                    std::string::size_type qpos = line().find_first_of
                    (
                         "'\"", epos + 1
                    );
                    std::string::size_type qpos2 = std::string::npos;
                    if (qpos != std::string::npos)
                    {
                        quotechar[0] = line()[qpos];
                        qpos2 = line().find_first_of(quotechar, qpos + 1);
                        if (qpos2 != std::string::npos)
                            havequotes = true;
                    }
                    if (havequotes)
                    {
                        result = line().substr(qpos, qpos2 - qpos);
                    }
                    else
                    {
                        spos = line().find_first_not_of(" ", epos + 1);
                        if (spos != std::string::npos)
                        {
                            epos = line().find_first_of(" ", spos);
                            result = line().substr(spos, epos-spos);
                        }
                    }
                }
            }
            else
                continue;
        }
        else
            break;

        good = next_data_line(file);            /* not at end of section?   */
    }
    return result;
}

/**
 *  Looks for the next named section.  Unlike line_after(), it does not
 *  restart from the beginning of the file.  Like next_data_line(), it starts
 *  at the current line in the file.  This makes it useful in parsing files,
 *  such as a playlist, that has multiple sections with the same name.
 *
 *  Note one other quirk.  If we are on a line matching the tag, then we do
 *  not search, but instead use that line.  The reason is that the
 *  next_data_line() function for the previous section will often end up
 *  at the beginning of the next section.  Especially important with
 *  play-lists.
 *
 * \param file
 *      Points to the input file stream.  Since this function has this
 *      parameter, the caller can deal with multiple files at the same time.
 *      However, the caller must either close the file explicitly or open it
 *      on the stack.
 *
 * \param tag
 *      Provides a tag to be found.  Lines are read until a match occurs
 *      with this tag.  Normally, the tag is a section marker, such as
 *      "[user-interface]".  Best to assume an exact match is needed.
 *
 * \return
 *      Returns true if the tag was found.  Otherwise, false is returned.
 */

bool
configfile::next_section (std::ifstream & file, const std::string & tag)
{
    bool result = false;
    file.clear();
    if (tag == m_line)
    {
        result = true;
    }
    else
    {
        bool ok = get_line(file);       /* fills in m_line as a side-effect */
        while (ok)                      /* includes the EOF check           */
        {
            result = strncompare(m_line, tag, tag.length());
            if (result)
            {
                break;
            }
            else
            {
                if (file.bad())
                {
                    errprint("bad file stream reading config file");
                }
                else
                    ok = get_line(file);        /* trims the white space   */
            }
        }
    }
    if (result)
        result = next_data_line(file);

    return result;
}

/**
 *  This function gets a specific line of text, specified as a tag.
 *  Then it gets the next non-blank line (i.e. data line) after that.
 *  This function is normally used to find major sections enclosed in brackets,
 *  such as "[midi-control]".
 *
 *  This function always starts from the beginning of the file.  Therefore,
 *  it can handle reading Seq66 configuration files that have had
 *  their tagged sections arranged in a different order.  This feature makes
 *  the configuration file a little more robust against errors.
 *
 * \param file
 *      Points to the input file stream.  Since this function has this
 *      parameter, the caller can deal with multiple files at the same time.
 *      However, the caller must either close the file explicitly or open it
 *      on the stack.
 *
 * \param tag
 *      Provides a tag to be found.  Lines are read until a match occurs
 *      with this tag.  Normally, the tag is a section marker, such as
 *      "[user-interface]".  Best to assume an exact match is needed.
 *
 * \param strip
 *      If true (the default), trims white space and strips out hash-tag
 *      comments, but only in lines after the tag is found.
 *
 * \return
 *      Returns true if the tag was found.  Otherwise, false is returned.
 */

bool
configfile::line_after
(
    std::ifstream & file,
    const std::string & tag,
    bool strip
)
{
    bool result = false;
    file.clear();                               /* clear the file flags     */
    file.seekg(0, std::ios::beg);               /* reseek to the beginning  */
    bool ok = get_line(file, true);             /* trims spaces/comments    */
    while (ok)                                  /* includes the EOF check   */
    {
        result = strncompare(m_line, tag, tag.length());
        if (result)
        {
            break;
        }
        else
        {
            if (file.bad())
            {
                errprint("bad file stream reading config file");
            }
            else
                ok = get_line(file);            /* trims the white space    */
        }
    }
    if (result)
        result = next_data_line(file, strip);   /* might preserve space etc */

    return result;
}

}           // namespace seq66

/*
 * configfile.cpp
 *
 * vim: sw=4 ts=4 wm=4 et ft=cpp
 */

