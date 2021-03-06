#if ! defined SEQ66_CONFIGFILE_HPP
#define SEQ66_CONFIGFILE_HPP

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
 * \file          configfile.hpp
 *
 *  This module declares the abstract base class for configuration and
 *  options files.
 *
 * \library       seq66 application
 * \author        Chris Ahlstrom
 * \date          2018-11-23
 * \updates       2019-06-07
 * \license       GNU GPLv2 or above
 *
 *  This is actually an elegant little parser, and works well as long as one
 *  respects its limitations.
 */

#include <fstream>
#include <string>

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq66
{

/**
 *  Currently these strings are just for reference, and not yet used in the
 *  code.
 *
 *  std::string filetag = "config-type";
 *  std::string ctrltag = "ctrl";
 *  std::string mutetag = "mutes";
 *  std::string rctag   = "rc";
 *  std::string usrtag  = "usr";
 *  std::string playtag = "playlist";
 *
 */

/*
 * Forward references.
 */

class rcsettings;

/**
 *    This class is the abstract base class for rcfile and usrfile.
 */

class configfile
{

private:

    /**
     *  Hold a reference to the "rc" settings object.
     */

    rcsettings & m_rc;

    /**
     *  Holds the last error message, if any.  Not a 100% foolproof yet.
     */

    std::string m_error_message;

    /**
     * Indicates if we are in an error status.
     */

    bool m_is_error;

    /**
     *  Provides the name of the configuration or other file being parsed.
     *  This will normally be a full-path specification.
     */

    std::string m_name;

protected:

    /**
     *  The current line of text being processed.  This member receives
     *  an input line, and so needs to be a character buffer.
     */

    std::string m_line;

public:

    configfile (const std::string & name, rcsettings & rcs);

    configfile () = delete;
    configfile (const configfile &) = delete;
    configfile & operator = (const configfile &) = delete;
    configfile (configfile &&) = default;
    configfile & operator = (configfile &&) = default;

    /**
     *  A rote destructor needed for a base class.
     */

    virtual ~configfile()
    {
        // empty body
    }

    virtual bool parse () = 0;
    virtual bool write () = 0;

    std::string parse_comments (std::ifstream & file);

    /**
     *  Provides the input file-name, to keep m_name private.
     *
     * \return
     *      Returns a constant reference to m_name.
     */

    const std::string & name () const
    {
        return m_name;
    }

    const std::string & error_message () const
    {
        return m_error_message;
    }

    bool is_error () const
    {
        return m_is_error;
    }

protected:

    bool make_error_message
    (
        const std::string & sectionname,
        const std::string & additional = ""
    );

    /**
     *
     */

    rcsettings & rc_ref ()
    {
        return m_rc;
    }

    /**
     *  Sometimes we need to know if there are new data lines at the end of an
     *  existing section.  One clue that there is not is that we're at the
     *  next section marker.  This function tests for that condition.
     *
     * \return
     *      Returns true if m_line[0] is the left-bracket character.
     */

    bool at_section_start () const
    {
        return m_line[0] == '[';
    }

    /**
     *  Sets the error message, which can later be displayed to the user.
     *
     * \param msg
     *      Provides the error message to be set.
     */

    void error_message (const std::string & msg)
    {
        m_error_message = msg;
        m_is_error = ! msg.empty();
    }

    /**
     *  Provides the input string, to keep m_line private.
     *
     * \return
     *      Returns a constant reference to m_line.
     */

    const std::string & line () const
    {
        return m_line;
    }

    std::string trimline () const;

    /**
     *  Provides a pointer to the input string in a form that sscanf() can use.
     *
     * \return
     *      Returns a constant character pointer to the data in m_line.
     */

    const char * scanline () const
    {
        return m_line.c_str();
    }

    bool get_line (std::ifstream & file, bool strip = true);
    bool line_after
    (
        std::ifstream & file,
        const std::string & tag,
        bool strip = true
    );
    bool next_data_line (std::ifstream & file, bool strip = true);
    bool next_section (std::ifstream & file, const std::string & tag);
    std::string get_variable
    (
        std::ifstream & file,
        const std::string & tag,
        const std::string & variablename
    );

};          // class configfile

}           // namespace seq66

#endif      // SEQ66_CONFIGFILE_HPP

/*
 * configfile.hpp
 *
 * vim: sw=4 ts=4 wm=4 et ft=cpp
 */

