#if ! defined SEQ66_QSLOTBUTTON_HPP
#define SEQ66_QSLOTBUTTON_HPP

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
 * \file          qslotbutton.hpp
 *
 *  This module declares/defines the base class for drawing on a pattern-slot
 *  button.
 *
 * \library       seq66 application
 * \author        Chris Ahlstrom
 * \date          2019-06-26
 * \updates       2019-09-09
 * \license       GNU GPLv2 or above
 *
 *  All this button can do is enable a new pattern to be created.
 */

#include <string>

#include <QPushButton>

#include "gui_palette_qt5.hpp"          /* seq66::gui_palette_qt5::Color    */
#include "play/seq.hpp"                 /* seq66::seq sequence-plus class   */

/**
 *  It is impossible to recreate live-frame features like drag-and-drop
 *  patterns, using Qt slots for toggle or press actions.  This macro disables
 *  the use of slots.  Leave it be unless experimenting.
 */

#define SEQ66_SLOTBUTTON_TRANSPARENT_TO_MOUSE

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq66
{
    class performer;
    class qslivegrid;

/**
 * The timebar for the sequence editor
 */

class qslotbutton : public QPushButton
{

    friend class qslivegrid;

protected:

    /**
     *  Holds a pointer to the parent, needed to evaluated changes in
     *  user-interface size.
     */

    const qslivegrid * const m_slot_parent;

    /**
     *  Indicates the sequence number of the slot.  Needed when the slot is
     *  empty (has a null seq::pointer), which will always be true for a
     *  slot-button.
     */

    seq::number m_slot_number;

    /**
     *  Provides the initial labelling for this button.
     */

    std::string m_label;

    /**
     *  Provides the hot-key (slot-key) for this button, provided by the
     *  performer.
     */

    std::string m_hotkey;

    /**
     *  The background color of the whole button.  This value comes from the
     *  sequence, which either has no color (white for muted, black for
     *  unmuted), or a color specified and saved with the sequence.
     */

    QColor m_back_color;

    /**
     *  Provide the color of foreground drawing.  Generally, this is black for
     *  the buttons, but for the qslotbutton, which is empty, and has a black
     *  background, this is white or yellow.
     */

    QColor m_fore_color;

    /**
     *  This color is black, except when the background color is dark.
     */

    QColor m_text_color;

    /**
     *  Holds the current width of the button, useful in scaling font size.
     */

    int m_slot_width;

    /**
     *  Indicates if the button is checkable, or just clickable.  Empty slots
     *  need to be enabled, but not checkable, so that we can do different
     *  things with them.  We could probably just hook up a different callback
     *  to qslotbuttons versus qloopbuttons.
     */

    bool m_is_checkable;

    /**
     *  Used in repainting the button.  Usually more important for the derived
     *  class (qloopbutton).
     */

    mutable bool m_is_dirty;

public:

    qslotbutton
    (
        const qslivegrid * const slotparent,
        seq::number slotnumber,
        const std::string & label,
        const std::string & hotkey,
        QWidget * parent    = nullptr
    );

    virtual ~qslotbutton ()
    {
        // no code needed
    }

    virtual void setup ();

    virtual seq::pointer loop ()
    {
        static seq::pointer s_dummy;
        return s_dummy;
    }

    virtual void set_checked (bool /*flag*/)
    {
        // no code for this base class
    }

    virtual bool toggle_checked ()
    {
        return false;                   /* no functionality in base class   */
    }

    seq::number slot_number () const
    {
        return m_slot_number;
    }

    const std::string & label () const
    {
        return m_label;
    }

    bool is_checkable () const
    {
        return m_is_checkable;
    }

protected:

    const qslivegrid * slot_parent () const
    {
        return m_slot_parent;
    }

    void make_checkable ()
    {
        m_is_checkable = true;;
    }

    bool is_dirty () const
    {
        return m_is_dirty;
    }

    QColor back_color () const
    {
        return m_back_color;
    }

    QColor fore_color () const
    {
        return m_fore_color;
    }

    QColor text_color () const
    {
        return m_text_color;
    }

    virtual void reupdate (bool all = true);

    virtual void draw_progress (QPainter & /*p*/, midipulse /*tick*/)
    {
        // no code yet
    }

protected:

    static gui_palette_qt5 & slotpal ();

    int slot_width () const
    {
        return m_slot_width;
    }

    void slot_width (int w)
    {
        m_slot_width = w;
    }

};          // class qslotbutton

}           // namespace seq66

#endif      // SEQ66_QSLOTBUTTON_HPP

/*
 * qslotbutton.hpp
 *
 * vim: sw=4 ts=4 wm=4 et ft=cpp
 */

