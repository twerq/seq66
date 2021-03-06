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
 * \file          sequence.cpp
 *
 *  This module declares/defines the base class for handling the data and
 *  management of patterns/sequences.
 *
 * \library       seq66 application
 * \author        Chris Ahlstrom
 * \date          2015-07-24
 * \updates       2019-10-16
 * \license       GNU GPLv2 or above
 *
 *  The functionality of this class also includes handling some of the
 *  operations of pattern triggers.
 *
 *  We had added null-pointer checks for the master MIDI buss pointer, but
 *  these just take up needless time, in most cases.
 *
 *  Provides an option to save the Time Signature and Tempo data that may be
 *  present in a MIDI file (in the first track) in the sequence object, and
 *  write them back to the MIDI file when saved again, in Seq66 format.
 *  The SeqSpec events that Seq24 and Seq66 save for these "events" are
 *  not readable by other MIDI applications, such as QTractor.  So other
 *  sequencers can read the correct time-signature and tempo values.
 *
 * \note
 *      We leave a small gap in various functions where mark_selected() locks
 *      and unlocks, then we lock again.  This should only be an issue if
 *      altering selected notes while recording.  We will test this at some
 *      point, and add better locking coverage if necessary.
 */

#include <cstring>                      /* std::memset()                    */

#include "cfg/scales.hpp"
#include "cfg/settings.hpp"             /* seq66::rc()                      */
#include "seq66_features.hpp"           /* various feature #defines         */
#include "midi/event_list.hpp"          /* seq66::event_list                */
#include "midi/mastermidibus.hpp"
#include "midi/midibus.hpp"             /* seq66::midibus                   */
#include "midi/midi_vector_base.hpp"    /* seq66::c_midi_notes              */
#include "play/performer.hpp"
#include "play/sequence.hpp"
#include "play/triggers.hpp"            /* seq66::triggers, etc.            */
#include "util/automutex.hpp"           /* seq66::mutex, automutex          */
#include "util/calculations.hpp"        /* measures_to_ticks()              */
#include "util/palette.hpp"             /* enum class ThumbColor            */

/**
 *  Enables and marks a user's patch for issue #95.
 */

#define LAYK_PULL_REQUEST_95

/**
 *  This value is used as the minimal increment for growing a trigger during
 *  song-recording.  This value was originally 10, but let's us a power of 2.
 *  This increment allows the rest of the threads to notice the change.
 */

#define SEQ66_SONG_RECORD_INC      16

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq66
{

/**
 *  Shows the note_info values. Purely for dev trouble-shooting.
 */

void
sequence::note_info::show () const
{
    printf
    (
        "note_info %d: ticks %ld to %ld, velocity %d\n",
        ni_note, ni_tick_start, ni_tick_finish, ni_velocity
    );
}

/**
 *  A static clipboard for holding pattern/sequence events.  Being static
 *  allows for copy/paste between patterns.
 */

event_list sequence::m_clipboard;

/**
 *  Provides the default name/title for the sequence.
 */

const std::string sequence::sm_default_name = "Untitled";

/**
 *  Principal constructor.
 *
 * \param ppqn
 *      Provides the PPQN parameter to perhaps alter the default PPQN value of
 *      this sequence.
 */

sequence::sequence (int ppqn)
 :
    m_parent                    (nullptr),      // set when sequence installed
    m_events                    (),
    m_triggers                  (*this),
    m_events_undo_hold          (),             // stazed
    m_have_undo                 (false),        // stazed
    m_have_redo                 (false),        // stazed
    m_events_undo               (),
    m_events_redo               (),
    m_iterator_draw             (m_events.begin()),
    m_channel_match             (false),        // stazed
    m_midi_channel              (0),
    m_bus                       (0),
    m_song_mute                 (false),
    m_transposable              (true),
    m_notes_on                  (0),
    m_master_bus                (nullptr),
    m_playing_notes             (),             // an array
    m_was_playing               (false),
    m_playing                   (false),
    m_recording                 (false),
    m_expanded_recording        (false),
    m_quantized_rec             (false),
    m_thru                      (false),
    m_queued                    (false),
    m_one_shot                  (false),
    m_one_shot_tick             (0),
    m_off_from_snap             (false),
    m_song_playback_block       (false),
    m_song_recording            (false),
    m_song_recording_snap       (false),
    m_song_record_tick          (0),
    m_overwrite_recording       (false),
    m_loop_reset                (false),
    m_unit_measure              (0),
    m_dirty_main                (true),
    m_dirty_edit                (true),
    m_dirty_perf                (true),
    m_dirty_names               (true),
    m_editing                   (false),
    m_raise                     (false),
    m_status                    (0),
    m_cc                        (0),
    m_snap                      (0),
    m_scale                     (0),
    m_name                      (),
    m_last_tick                 (0),
    m_queued_tick               (0),
    m_trigger_offset            (0),
    m_maxbeats                  (c_maxbeats),
    m_ppqn                      (choose_ppqn(ppqn)),
    m_seq_number                (unassigned()),
    m_seq_color                 (c_seq_color_none), /* PaletteColor::NONE   */
    m_seq_edit_mode             (sequence::editmode::note),
    m_length                    (4 * midipulse(m_ppqn)),  /* 1 bar of ticks */
    m_snap_tick                 (int(m_ppqn) / 4),
    m_time_beats_per_measure    (4),
    m_time_beat_width           (4),
    m_clocks_per_metronome      (24),
    m_32nds_per_quarter         (8),
    m_us_per_quarter_note       (tempo_us_from_bpm(SEQ66_DEFAULT_BPM)),
    m_rec_vol                   (SEQ66_PRESERVE_VELOCITY),
    m_note_on_velocity          (SEQ66_DEFAULT_NOTE_ON_VELOCITY),
    m_note_off_velocity         (SEQ66_DEFAULT_NOTE_OFF_VELOCITY),
    m_musical_key               (c_key_of_C),
    m_musical_scale             (c_scales_off),
    m_background_sequence       (sequence::limit()),
    m_mutex                     (),
    m_note_off_margin           (2)
{
    m_triggers.set_ppqn(int(m_ppqn));
    m_triggers.set_length(m_length);

    /*
     * This is the C++11 way to initialize m_playing_notes[p] for all p.
     */

    for (auto & p : m_playing_notes)            /* no notes are playing now */
        p = 0;
}

/**
 *  A rote destructor.
 */

sequence::~sequence ()
{
    // Empty body
}

/**
 *  A convenience function that we have to put here so that the m_parent
 *  pointer can be used without an additional include-file in the sequence.hpp
 *  module.  One minor issue is how can we unmodify the performance?  We'd need
 *  to keep a count/stack of modifications over all sequences in the
 *  performance.  Probably not practical, in general.  We will probably keep
 *  track of the modification of the buss (port) and channel numbers, as per
 *  GitHub Issue #47.
 */

void
sequence::modify ()
{
    if (not_nullptr(m_parent))
        m_parent->modify();
}

/**
 *  A cut-down version of principal assignment operator.  We're replacing that
 *  incomplete seq66 function (many members are not assigned) with the more
 *  accurately-named partial_assign() function.  It did not assign them all,
 *  so we created this partial_assign() function to do this work, and replaced
 *  operator =() with this function in client code.
 *
 * \threadsafe
 *
 * \param rhs
 *      Provides the source of the new member values.
 */

void
sequence::partial_assign (const sequence & rhs)
{
    if (this != &rhs)
    {
        automutex locker(m_mutex);
        m_parent        = rhs.m_parent;             /* a pointer, careful!  */
        m_events        = rhs.m_events;
        m_triggers      = rhs.m_triggers;
        m_channel_match = rhs.m_channel_match;
        m_midi_channel  = rhs.m_midi_channel;
        m_bus           = rhs.m_bus;
        m_transposable  = rhs.m_transposable;
        m_master_bus    = rhs.m_master_bus;         /* a pointer, be aware! */
        m_was_playing   = false;
        m_playing       = false;
        m_recording     = false;
        m_expanded_recording  = rhs.m_expanded_recording;
        m_overwrite_recording = rhs.m_overwrite_recording;
        m_scale         = rhs.m_scale;
        m_name          = rhs.m_name;
        m_ppqn          = rhs.m_ppqn;
        m_seq_color     = rhs.m_seq_color;
        m_seq_edit_mode = rhs.m_seq_edit_mode;
        m_length        = rhs.m_length;
        m_snap_tick     = rhs.m_snap_tick;
        m_time_beats_per_measure = rhs.m_time_beats_per_measure;
        m_time_beat_width        = rhs.m_time_beat_width;
        m_clocks_per_metronome   = rhs.m_clocks_per_metronome;
        m_32nds_per_quarter      = rhs.m_32nds_per_quarter;
        m_us_per_quarter_note    = rhs.m_us_per_quarter_note;
        m_rec_vol                = rhs.m_rec_vol;
        m_note_on_velocity       = rhs.m_note_on_velocity;
        m_note_off_velocity      = rhs.m_note_off_velocity;
        m_musical_key            = rhs.m_musical_key;
        m_musical_scale          = rhs.m_musical_scale;
        m_background_sequence    = rhs.m_background_sequence;
        for (auto & p : m_playing_notes)            /* no notes playing now */
            p = 0;

        m_last_tick = 0;                            /* reset to tick 0      */
        verify_and_link();
    }
}

/**
 * \setter m_seq_color
 *
 * \param c
 *      Provides the index into the color-palette.  The only rules here are
 *      that -1 represents no color or a default color, and values of zero
 *      and above (to an unknown limit) represent a legal palette color.
 *
 * \return
 *      Returns true if the color actually changed.
 */

bool
sequence::color (int c)
{
    bool result = false;
    if (c >= 0 || c == c_seq_color_none)
    {
        if (colorbyte(c) != m_seq_color)
        {
            m_seq_color = colorbyte(c);
            result = true;
        }
    }
    return result;
}

/**
 * If empty, sets the color to classic Sequencer64 yellow.  Called by performer
 * when installing a sequence.
 */

void
sequence::empty_coloring ()
{
    if (event_count() == 0)
        (void) color(color_to_int(yellow));
}

/**
 *  Modifies the undo-hold container.
 *
 * \param hold
 *      If true, then the events in the m_events container are added to the
 *      m_events_undo_hold container.  Otherwise, that container is cleared.
 */

void
sequence::set_hold_undo (bool hold)
{
    automutex locker(m_mutex);
    if (hold)
        m_events_undo_hold = m_events;
    else
       m_events_undo_hold.clear();
}

/**
 *  Returns the number of events stored in m_events.  Note that only playable
 *  events are counted in a sequence.  If a sequence class function provides a
 *  mutex, call m_events.count() instead.
 *
 * \threadsafe
 *
 * \return
 *      Returns m_events.count().
 */

int
sequence::event_count () const
{
    automutex locker(m_mutex);
    return int(m_events.count());
}

/**
 *  Pushes the event-list into the undo-list or the upcoming undo-hold-list.
 *
 * \threadsafe
 *
 * \param hold
 *      A new parameter for the stazed undo/redo support, not yet used.
 *      If true (the default is false), then the events go into the
 *      undo-hold-list.
 */

void
sequence::push_undo (bool hold)
{
    automutex locker(m_mutex);
    if (hold)
        m_events_undo.push(m_events_undo_hold);     /* stazed   */
    else
        m_events_undo.push(m_events);

    set_have_undo();                                /* stazed   */
}

/**
 *  If there are items on the undo list, this function pushes the event-list
 *  into the redo-list, puts the top of the undo-list into the event-list, pops
 *  from the undo-list, calls verify_and_link(), and then calls unselect().
 *
 *  We would like to be able to set performer's modify flag to false here, but
 *  other sequences might still be in a modified state.  We could add a modify
 *  flag to sequence, and falsify that flag here.  Something to think about.
 *
 * \threadsafe
 */

void
sequence::pop_undo ()
{
    automutex locker(m_mutex);
    if (! m_events_undo.empty())                // stazed: m_list_undo
    {
        m_events_redo.push(m_events);           // move to triggers module?
        m_events = m_events_undo.top();
        m_events_undo.pop();
        verify_and_link();
        unselect();
    }
    set_have_undo();                            // stazed
    set_have_redo();                            // stazed
}

/**
 *  If there are items on the redo list, this function pushes the
 *  event-list into the undo-list, puts the top of the redo-list into the
 *  event-list, pops from the redo-list, calls verify_and_link(), and then
 *  calls unselect.
 *
 * \threadsafe
 */

void
sequence::pop_redo ()
{
    automutex locker(m_mutex);
    if (! m_events_redo.empty())                // move to triggers module?
    {
        m_events_undo.push(m_events);
        m_events = m_events_redo.top();
        m_events_redo.pop();
        verify_and_link();
        unselect();
    }
    set_have_undo();                            // stazed
    set_have_redo();                            // stazed
}

/**
 *  Calls triggers::push_undo() with locking.
 *
 * \threadsafe
 */

void
sequence::push_trigger_undo ()
{
    automutex locker(m_mutex);
    m_triggers.push_undo(); // todo:  see how stazed's sequence function works
}

/**
 *  Calls triggers::pop_undo() with locking.
 *
 * \threadsafe
 */

void
sequence::pop_trigger_undo ()
{
    automutex locker(m_mutex);
    m_triggers.pop_undo();
}

/**
 *  Calls triggers::pop_redo() with locking.
 *
 * \threadsafe
 */

void
sequence::pop_trigger_redo ()
{
    automutex locker(m_mutex);
    m_triggers.pop_redo();
}

/**
 * \setter m_master_bus
 *      Do we need to call set_dirty_mp() here?  It doesn't affect any
 *      user-interface elements.
 *
 * \threadsafe
 *
 * \param mmb
 *      Provides a pointer to the master MIDI buss for this sequence.  This
 *      should be a reference, but isn't, nor is it checked.
 */

void
sequence::set_master_midi_bus (const mastermidibus * mmb)
{
    automutex locker(m_mutex);
    m_master_bus = const_cast<mastermidibus *>(mmb);
}

/**
 * \setter m_time_beats_per_measure
 *
 * \threadsafe
 *
 * \param beatspermeasure
 *      The new setting of the beats-per-bar value.
 */

void
sequence::set_beats_per_bar (int beatspermeasure)
{
    automutex locker(m_mutex);
    if (beatspermeasure <= int(USHRT_MAX))
    {
        m_time_beats_per_measure = (unsigned short)(beatspermeasure);
        set_dirty_mp();
    }
}

/**
 * \setter m_time_beat_width
 *
 * \threadsafe
 *
 * \param beatwidth
 *      The new setting of the beat width value.
 */

void
sequence::set_beat_width (int beatwidth)
{
    automutex locker(m_mutex);
    if (beatwidth <= int(USHRT_MAX))
    {
        m_time_beat_width = (unsigned short)(beatwidth);
        set_dirty_mp();
    }
}

/**
 *  Calculates and sets u = 4BP/W, where u is m_unit_measure, B is the
 *  beats/bar, P is the PPQN, and W is the beat-width.
 */

void
sequence::calculate_unit_measure () const
{
    automutex locker(m_mutex);
    m_unit_measure = get_beats_per_bar() * (m_ppqn * 4) / get_beat_width();
}

/**
 * \getter m_unit_measure
 */

midipulse
sequence::unit_measure () const
{
    if (m_unit_measure == 0)
        calculate_unit_measure();

    return m_unit_measure;
}

/**
 *  Provides a consistent threshold value for the triggering of the
 *  addition of a measure when recording a pattern in Expand mode.
 *
 *  We want to have the threshold be a quarter of a beat, that is,
 *  unit_measure() / 4 / 4, but that setting makes the drawing occur twice, in
 *  slightly different places, and we have not figured that out yet.  So we
 *  stick with unit_measure() / 4, which is one beat.
 *
 *  \return
 *      Returns the current length of the sequence minus a beat.
 */

midipulse
sequence::expand_threshold () const
{
    // return get_length() - unit_measure() / 16;
    return get_length() - unit_measure() / 4;
}

/**
 *  Provides the new value of the horizontal scroll bar to set when doing
 *  expanded recording.  See expand_threshold() for an issue we currently
 *  have not solved.
 *
 * \return
 *      Returns the expand_threshold() minus a unit_measure() and a quarter.
 */

midipulse
sequence::progress_value () const
{
    // return get_length() - (unit_measure() + unit_measure() / 16);
    return expand_threshold() - (unit_measure() + unit_measure() / 4);
}

/**
 *  Calculates the number of measures in the sequence based on the
 *  unit-measure and the current length, in pulses, of the sequence.  Used by
 *  seqedit.  If m_unit_measure hasn't been calculated yet, it is calculated
 *  here.
 *
 * \change ca 2017-08-15
 *      Fixed issue #106, where the measure count of a pattern kept
 *      incrementing when edited.
 *
 * \return
 *      Returns the sequence length divided by the measure length, roughly.
 *      m_unit_measure is 0.  The lowest valid measure is 1.
 */

int
sequence::calculate_measures () const
{
    if (m_unit_measure == 0)
        calculate_unit_measure();

    return 1 + (m_length - 1) / m_unit_measure;
}

/**
 *  Encapsulates a calculation needed in the qseqbase class (and elsewhere).
 *  We could just assume m_unit_measures is always up-to-date and use that
 *  value.
 *
 * \return
 *      Returns the whole number of measure in the current length of the
 *      sequence.  Essentially rounds up if there is some leftover ticks.
 */

int
sequence::get_measures () const
{
    int units = get_beats_per_bar() * get_ppqn() * 4 / get_beat_width();
    int measures = get_length() / units;
    if (get_length() % units != 0)
        ++measures;

    return measures;
}

#if defined USE_STAZED_ODD_EVEN_SELECTION

/**
 *  Selects every other note.  Enabled only if USE_STAZED_ODD_EVEN_SELECTION
 *  is defined.
 *
 * \param note_len
 *      The desired note lengths for the selection.
 *
 * \param even
 *      True if we want the even notes.
 *
 * \return
 *      Returns the number of notes selected.
 */

int
sequence::select_even_or_odd_notes (int note_len, bool even)
{
    int result = 0;
    automutex locker(m_mutex);
    unselect();
    for (auto & e : m_events)
    {
        if (e.is_note_on())
        {
           midipulse tick = e.timestamp();
            if ((tick % note_len) == 0)
            {
                /*
                 * Note that from the user's point of view of even and odd,
                 * we start counting from 1, not 0.
                 */

                int is_even = (tick / note_len) % 2;
                if ((even && is_even) || (! even && ! is_even))
                {
                    e.select();
                    ++result;
                    if (e.is_linked())
                    {
                        event * note_off = e.link();
                        note_off->select();
                        ++result;
                    }
                }
            }
        }
    }
    return result;
}

/**
 *  Selects events in range provided: tick start, note high, tick end, and
 *  note low.
 *
 *  Be aware the the event::is_note() function is used, and that it includes
 *  Aftertouch events, which generally need to stick with their Note On
 *  counterparts.
 *
 *  If a "note" event is detected, then we skip it.  This is necessary since
 *  channel pressure and control change use d0 for seqdata, and d0 is returned
 *  by get_note().  This causes note selection to occasionally select them
 *  when their seqdata values are within range of the tick selection.  So
 *  therefore we want only Note Ons and Note Offs.
 *
 * \note
 *      The continuation below ("continue") is necessary since channel
 *      pressure and control change use d0 for seqdata [which is returned by
 *      get_note()]. This causes seqroll note selection to occasionally
 *      select them when their seqdata values are within the range of tick
 *      selection. So only, note ons and offs.  What about Aftertouch?
 *      We have the event::is_note() function for that.
 *
 * \param tick_s
 *      The starting tick.
 *
 * \param note_h
 *      The highest note selected.
 *
 * \param tick_f
 *      The ending, or finishing, tick.
 *
 * \param note_l
 *      The lowest note selected.
 *
 * \param action
 *      The action to performer on the selection.
 *
 * \return
 *      Returns the number of notes selected.
 */

int
sequence::select_note_events
(
    midipulse tick_s, int note_h,
    midipulse tick_f, int note_l,
    select action
)
{
    int result = 0;
    automutex locker(m_mutex);
    for (auto & e : m_events)
    {
        if (! e.is_note())              /* HMMMM, includes Aftertouch   */
            continue;                   /* see the note in the banner   */

        if (e.get_note() <= note_h && e.get_note() >= note_l)
        {
            midipulse ts = 0;
            midipulse tf = 0;
            if (e.is_linked())
            {
                event * ev = e.link();
                if (e.is_note_off())
                {
                    ts = ev->timestamp();
                    tf = e.timestamp();
                }
                else if (e.is_note_on())
                {
                    ts = e.timestamp();
                    tf = ev->timestamp();
                }

                /*
                 * A pretty convoluted check!  Someone tell me what this is!
                 *    :-D
                 */

                bool tand = (ts <= tick_f && tf >= tick_s);
                bool tor =  (ts <= tick_f || tf >= tick_s);
                bool ok = ((ts <= tf) && tand) || ((ts > tf) && tor);
			    if (ok)
                {
                    /*
                     * Could use a switch statement here.
                     */

                    if (action == select::selecting || action == select::select_one)
                    {
                        e.select();
                        ev->select();
                        ++result;
                        if (action == select::select_one)
                            break;
                    }
                    if (action == select::selected)
                    {
                        if (e.is_selected())
                        {
                            result = 1;
                            break;
                        }
                    }
                    if (action == select::would_select)
                    {
                        result = 1;
                        break;
                    }
                    if (action == select::deselect)
                    {
                        result = 0;
                        e.unselect();
                        ev->unselect();
                    }

                    /*
                     * Don't toggle twice.
                     */

                    if (action == select::toggle && e.is_note_on())
                    {
                        if (e.is_selected())
                        {
                            e.unselect();
                            ev->unselect();
                        }
                        else
                        {
                            e.select();
                            ev->select();
                        }
                        ++result;
                    }
                    if (action == select::remove)
                    {
                        remove(i);
                        remove(e);
                        reset_draw_marker();
                        ++result;
                        break;
                    }
                }
            }
            else
            {
                /*
                 * If NOT linked note on/off, then it is junk....  What is the
                 * 17?  It used to be 16.
                 */

                ts = tf = e.timestamp();
                if (ts >= tick_s - 17 && tf <= tick_f)
                {
                    /*
                     * Could use a switch statement here.
                     */

                    if (action == select::selecting || action == select::select_one)
                    {
                        e.select();
                        ++result;
                        if (action == select::select_one)
                            break;
                    }
                    if (action == select::selected)
                    {
                        if (e.is_selected())
                        {
                            result = 1;
                            break;
                        }
                    }
                    if (action == select::would_select)
                    {
                        result = 1;
                        break;
                    }
                    if (action == select::deselect)
                    {
                        result = 0;
                        e.unselect();
                    }
                    if (action == select::toggle)
                    {
                        if (e.is_selected())
                        {
                            e.unselect();
                            ++result;
                        }
                        else
                        {
                            e.select();
                            ++result;
                        }
                    }
                    if (action == select::remove)
                    {
                        remove(i);
                        reset_draw_marker();
                        ++result;
                        break;
                    }
                }
            }
        }
    }
    return result;
}

/**
 *  Use selected note ons if any.
 *
 * \param tick_s
 *      Provides the starting tick.
 *
 * \param tick_f
 *      Provides the ending (finishing) tick.
 *
 * \param status
 *      Provides the desired MIDI event to be selected.
 *
 * \param cc
 *      Provides the desired MIDI control value to be selected.
 *
 * \param dats
 *      Provides the center of a small data value range of plus or minus 2.
 *
 * \return
 *      Returns the number of events selected.
 */

int
sequence::select_event_handle
(
    midipulse tick_s, midipulse tick_f,
    midibyte status, midibyte cc, int dats
)
{
    int result = 0;
    bool have_selection = false;
    if (status == EVENT_NOTE_ON)                    // use a function!
        have_selection = m_events.any_selected_events(status, cc);

    for (auto & e : m_events)
    {
        if (event_in_range(e, status, tick_s, tick_f))
        {
            midibyte d0, d1;
            e.get_data(d0, d1);
            if (status == EVENT_CONTROL_CHANGE && d0 == cc)
            {
                if (d1 <= (dats + 2) && d1 >= (dats - 2))   // is it in range
                {
                    unselect();
                    e.select();
                    ++result;
                    break;
                }
            }
            if (status != EVENT_CONTROL_CHANGE)             // use a function!
            {
                if (e.is_note_msg(status) || status == EVENT_PITCH_WHEEL)
                {
                    if(d1 <= (dats+2) && d1 >= (dats-2))    // is it in range
                    {
                        if (have_selection)                 // note on only
                        {
                            if (e.is_selected())
                            {
                                unselect();                 // all events
                                e.select();                 // only this one
                                if (result)
                                {
                                    /*
                                     * If we have a marked one, clear it.
                                     */

                                    for (auto & et : m_events)
                                    {
                                        if (e.is_marked())
                                        {
                                            et.unmark();
                                            break;
                                        }
                                    }
                                    --result;

                                    // reset for marked flag at end

                                    have_selection = false;
                                }
                                ++result;            // for the selected one
                                break;
                            }
                            else        // NOT selected note on, but in range
                            {
                                if (! result)       // only mark the first one
                                {
                                    e.mark();       // marked for hold until done
                                    ++result;       // indicate we got one
                                }

                                // keep going until we find a selected one if
                                // any, or are done

                                continue;
                            }
                        }
                        else                      // NOT note on
                        {
                            unselect();
                            e.select();
                            ++result;
                            break;
                        }
                    }
                }
                else
                {
                    if (d0 <= (dats + 2) && d0 >= (dats - 2))   // is it in range
                    {
                        unselect();
                        e.select();
                        ++result;
                        break;
                    }
                }
            }
        }
    }

    /*
     * Is it a note on that is unselected but in range? Then use it...
     * have_selection will be set to false if we found a selected one in
     * range.
     */

    if (result && have_selection)
    {
        for (auto & e : m_events)
        {
            if (e.is_marked())
            {
                unselect();
                e.unmark();
                e.select();
                break;
            }
        }
    }
    set_dirty();
    return result;
}

#endif  // USE_STAZED_SELECTION_EXTENSIONS

/**
 *  Used with seqevent when selecting Note On or Note Off, this function will
 *  select the opposite linked event.  This is a Stazed selection fix we have
 *  activated unilaterally.
 *
 * \param tick_s
 *      Provides the starting tick.
 *
 * \param tick_f
 *      Provides the ending (finishing) tick.
 *
 * \param status
 *      Provides the desired MIDI event to be selected.
 *
 * \return
 *      Returns the number of notes selected.
 */

int
sequence::select_linked (midipulse tick_s, midipulse tick_f, midibyte status)
{
    int result = 0;
    automutex locker(m_mutex);
    for (auto & e : m_events)
    {
        if
        (
            e.get_status() == status &&
            e.timestamp() >= tick_s && e.timestamp() <= tick_f
        )
        {
            if (e.is_linked())
            {
                if (e.is_selected())
                    e.link()->select();
                else
                    e.link()->unselect();

                ++result;
            }
        }
    }
    return result;
}

/**
 * \setter m_rec_vol
 *      If this velocity is greater than zero, then m_note_on_velocity will
 *      be set as well.
 *
 * \threadsafe
 *
 * \param recvol
 *      The new setting of the recording volume setting.  It is used only if
 *      it ranges from 0 to SEQ66_MAX_NOTE_ON_VELOCITY, or is set to
 *      SEQ66_PRESERVE_VELOCITY.
 */

void
sequence::set_rec_vol (int recvol)
{
    automutex locker(m_mutex);
    bool valid = recvol >= 0;
    if (valid)
        valid = recvol <= SEQ66_MAX_NOTE_ON_VELOCITY;

    if (! valid)
        valid = recvol == SEQ66_PRESERVE_VELOCITY;

    if (valid)
    {
        m_rec_vol = short(recvol);
        if (m_rec_vol > 0)
            m_note_on_velocity = m_rec_vol;
    }
}

/**
 *  Toggles the playing status of this sequence.  How exactly does this
 *  differ from toggling the mute status?
 *
 * \param tick
 *      The position from which to resume Note Ons, if appplicable. Resuming
 *      is a song-recording feature.
 *
 * \param resumenoteons
 *      A song-recording option.
 */

void
sequence::toggle_playing (midipulse tick, bool resumenoteons)
{
    toggle_playing();
    if (get_playing() && resumenoteons)
        resume_note_ons(tick);

    m_off_from_snap = false;
}

/**
 * \setter m_queued and m_queued_tick
 *      Toggles the queued flag and sets the dirty-mp flag.  Also calculates
 *      the queued tick based on m_last_tick.
 *
 * \threadsafe
 */

void
sequence::toggle_queued ()
{
    automutex locker(m_mutex);
    m_queued = ! m_queued;
    m_queued_tick = m_last_tick - mod_last_tick() + m_length;
    m_off_from_snap = true;
    set_dirty_mp();

    // DO WE NEED to call performer::announce_sequence() here?
    /*
    midi_control_out * mco = m_parent->get_midi_control_out();
    if (not_nullptr(mco))
    {
        if (m_queued)
            mco->send_seq_event(number(), midi_control_out::seq_action_queue);
        else if (get_playing())
            mco->send_seq_event(number(), midi_control_out::seq_action_arm);
        else
            mco->send_seq_event(number(), midi_control_out::seq_action_mute);
    }
    */
}

/**
 *  The play() function dumps notes starting from the given tick, and it
 *  pre-buffers ahead.  This function is called by the sequencer thread in
 *  performer.  The tick comes in as global tick.  It turns the sequence off
 *  after we play in this frame.
 *
 * \note
 *      With pause support, the progress bar for the pattern/sequence editor
 *      does what we want:  pause with the pause button, and rewind with the
 *      stop button.  Works with JACK, with issues, but we'd like to have
 *      the stop button do a rewind in JACK, too.
 *
 *  If we are playing the song data (sequence on/off triggers, we are in
 *  playback mode.  And if we are song-recording, we then keep growing the
 *  sequence's song-data triggers.
 *
 *  Note that the song_playback_block() is handled in the trigger::play()
 *  function.  If we have reached a new chunk of drawn patterns in the song
 *  data, and we are not recording, then trigger unsets the playback block on
 *  this pattern's events.
 *
 *  The trigger calculations have been offloaded to the triggers::play()
 *  function.  Its return value and side-effects tell if there's a change in
 *  playing based on triggers, and provides the ticks that bracket it.
 *
 * \param tick
 *      Provides the current end-tick value.  The tick comes in as a global
 *      tick.
 *
 * \param playback_mode
 *      Provides how playback is managed.  True indicates that it is
 *      performance/song-editor playback, controlled by the set of patterns
 *      and triggers set up in that editor, and saved with the song in seq66
 *      format.  False indicates that the playback is controlled by the main
 *      window, in live mode.
 *
 * \param resume_note_ons
 *      A song-recording parameter.
 *
 * \threadsafe
 */

void
sequence::play
(
    midipulse tick,
    bool playback_mode,
    bool /*resume_note_ons*/
)
{
    automutex locker(m_mutex);
    bool trigger_turning_off = false;       /* turn off after in-frame play */
    midipulse start_tick = m_last_tick;     /* modified in triggers::play() */
    midipulse end_tick = tick;              /* ditto                        */
    if (m_song_mute)
    {
        set_playing(false);
    }
    else
    {
        /*
         *  We make the song_recording() clause active for both Live and Song
         *  mode now.
         */

        if (song_recording())
        {
            grow_trigger(song_record_tick(), end_tick, SEQ66_SONG_RECORD_INC);
            set_dirty_mp();                 /* force redraw                 */
        }
        if (playback_mode)                  /* song mode: on/off triggers   */
            trigger_turning_off = m_triggers.play(start_tick, end_tick);
    }
    if (m_playing)                          /* play notes in frame          */
    {
        midipulse offset = m_length - m_trigger_offset;
        midipulse start_tick_offset = start_tick + offset;
        midipulse end_tick_offset = end_tick + offset;
        midipulse times_played = m_last_tick / m_length;
        midipulse offset_base = times_played * m_length;
        int transpose = transposable() ? m_parent->get_transpose() : 0 ;
        auto e = m_events.begin();
        while (e != m_events.end())
        {
            event & er = event_list::dref(e);
            midipulse stamp = er.timestamp() + offset_base;
            if (stamp >= start_tick_offset && stamp <= end_tick_offset)
            {

#if defined SEQ66_PLATFORM_DEBUG_TMI
                /*
                 * When we've reach the play state, we always find that start =
                 * stamp = end, and offset is a constant equal to the first
                 * stamp found (at least for awhile).
                 */

                printf
                (
                    "start = %ld <= %ld <= end = %ld; offset = %ld, base = %ld\n",
                    start_tick_offset, stamp, end_tick_offset, offset,
                    offset_base
                );
#endif
                if (transpose != 0 && er.is_note()) /* includes Aftertouch  */
                {
                    // COMMENTED TO AVOID NEEDLESS COPYING
                    // event transposed_event = er;    /* assign ALL members   */
                    // transposed_event.transpose_note(transpose);
                    // put_event_on_bus(transposed_event);
                    er.transpose_note(transpose);
                    put_event_on_bus(er);
                }
                else
                {
                    if (er.is_tempo())
                    {
                        if (not_nullptr(m_parent))
                            m_parent->set_beats_per_minute(er.tempo());
                    }
                    else if (! er.is_ex_data())
                    {
                        put_event_on_bus(er);       /* frame still going    */
                    }
                }
            }
            else if (stamp > end_tick_offset)
                break;                              /* frame is done        */

            ++e;                                    /* go to next event     */
            if (e == m_events.end())                /* did we hit the end ? */
            {
                e = m_events.begin();               /* yes, start over      */
                offset_base += m_length;            /* for another go at it */

                /*
                 * Putting this sleep here doesn't reduce the total CPU load,
                 * but it does prevent one CPU from being hammered at 100%.
                 * However, it also makes the live-grid progress bar jittery
                 * when unmuted, for shorter patterns, which play()
                 * relentlessly here.
                 *
                 *      millisleep(1);
                 *
                 * Another possible criteria is note_count(), but currently
                 * that is a looping (time-eating) function.
                 */

                if (measure_threshold())
                    microsleep(1);                  /* EXPERIMENTAL         */
            }
        }
    }
    if (trigger_turning_off)                        /* triggers: "turn off" */
        set_playing(false);

    m_last_tick = end_tick + 1;                     /* for next frame       */
    m_was_playing = m_playing;
}

/**
 *  This function verifies state: all note-ons have a note-off, and it links
 *  note-offs with their note-ons.
 *
 * \threadsafe
 */

void
sequence::verify_and_link ()
{
    automutex locker(m_mutex);
    m_events.verify_and_link(m_length);
}

/**
 *  Links a new event.  Locked elsewhere, no need for automutex locker(m_mutex);
 */

void
sequence::link_new ()
{
    m_events.link_new();
}

/**
 *  A helper function, which does not lock/unlock, so it is unsafe to call
 *  without supplying an iterator from the event-list.  We no longer
 *  bother checking the pointer.  If it is bad, all hope is lost.
 *  If the event is a note off, and that note is currently playing, then send
 *  a note off before removing the note.
 *
 * \threadunsafe
 *
 * \param i
 *      Provides the iterator to the event to remove from the event list.
 */

void
sequence::remove (event_list::iterator evi)
{
    event & er = event_list::dref(evi);
    if (er.is_note_off() && m_playing_notes[er.get_note()] > 0)
    {
        m_master_bus->play(m_bus, &er, m_midi_channel);
        --m_playing_notes[er.get_note()];                   // ugh
    }
    m_events.remove(evi);
}

/**
 *  A helper function, which does not lock/unlock, so it is unsafe to call
 *  without supplying an iterator from the event-list.
 *
 *  Finds the given event in m_events, and removes the first iterator
 *  matching that.  If there are events that would match after that, they
 *  remain in the container.  This matches seq66 behavior.
 *
 * \todo
 *      Use the find() function to find the matching event more
 *      conventionally.
 *
 * \threadunsafe
 *
 * \param e
 *      Provides a reference to the event to be removed.
 */

void
sequence::remove (event & e)
{
    for (auto i = m_events.begin(); i != m_events.end(); ++i)
    {
        event & er = event_list::dref(i);
        if (&e == &er)                  /* comparing pointers, not values   */
        {
            (void) m_events.remove(i);  /* an iterator is required here     */
            break;
        }
    }
}

/**
 *  Removes marked events.  Note how this function forwards the call to
 *  m_event.remove_marked().
 *
 *  We have to make Note Offs before moving and cutting. Here or in event_list?
 *
 * \threadsafe
 *
 * \return
 *      Returns true if at least one event was removed.
 */

bool
sequence::remove_marked ()
{
    automutex locker(m_mutex);
    for (auto & e : m_events)           /* see issue #95                */
    {
        if (e.is_marked())
        {
            if (e.is_note_on())         /* or maybe just e.is_note()?   */
            {
                midibyte note_to_play = e.get_note();
                play_note_off(int(note_to_play));
            }
        }
    }

    bool result = m_events.remove_marked();
    reset_draw_marker();
    return result;
}

/**
 *  Marks the selected events.
 *
 * \threadsafe
 *
 * \return
 *      Returns true if there were any events that got marked.
 */

bool
sequence::mark_selected ()
{
    automutex locker(m_mutex);
    bool result = m_events.mark_selected();
    reset_draw_marker();
    return result;
}

/**
 *  Removes selected events.  This is a new convenience function to fold in
 *  the push_undo() and mark_selected() calls.  It makes the process slightly
 *  faster, as well.
 *
 * \threadsafe
 *      Also makes the whole process threadsafe.
 */

bool
sequence::remove_selected ()
{
    bool result = false;
    automutex locker(m_mutex);
    if (m_events.mark_selected())
    {
        m_events_undo.push(m_events);           /* push_undo() without lock */
        result = m_events.remove_marked();
        reset_draw_marker();
    }
    return result;
}

/**
 *  Unpaints all events in the event-list.
 *
 * \threadsafe
 */

void
sequence::unpaint_all ()
{
    automutex locker(m_mutex);
    m_events.unpaint_all();
}

/**
 *  Returns the 'box' of the selected items.  Note the common-code betweem
 *  this function and clipboard_box().  Also note we could return a
 *  boolean indicating if the return values were filled in.
 *
 * \threadsafe
 *
 * \param [out] tick_s
 *      Side-effect return reference for the start time.
 *
 * \param [out] note_h
 *      Side-effect return reference for the high note.
 *
 * \param [out] tick_f
 *      Side-effect return reference for the finish time.
 *
 * \param [out] note_l
 *      Side-effect return reference for the low note.
 */

bool
sequence::selected_box
(
    midipulse & tick_s, int & note_h, midipulse & tick_f, int & note_l
)
{
    automutex locker(m_mutex);
    bool result = false;
    tick_s = m_maxbeats * m_ppqn;
    tick_f = 0;
    note_h = 0;
    note_l = SEQ66_MIDI_COUNT_MAX;
    for (auto & e : m_events)
    {
        result = true;
        if (e.is_selected())
        {
            midipulse time = e.timestamp();
            if (time < tick_s)
                tick_s = time;

            if (time > tick_f)
                tick_f = time;

            int note = e.get_note();
            if (note < note_l)
                note_l = note;

            if (note > note_h)
                note_h = note;
        }
    }
    return result;
}

/**
 *  Returns the 'box' of the selected items for only Note On values.
 *  Compare to selected_box().
 *
 * \threadsafe
 *
 * \param [out] tick_s
 *      Side-effect return reference for the start time.
 *
 * \param [out] note_h
 *      Side-effect return reference for the high note.
 *
 * \param [out] tick_f
 *      Side-effect return reference for the finish time.
 *
 * \param [out] note_l
 *      Side-effect return reference for the low note.
 */

bool
sequence::onsets_selected_box
(
    midipulse & tick_s, int & note_h, midipulse & tick_f, int & note_l
)
{
    automutex locker(m_mutex);
    bool result = false;
    tick_s = m_maxbeats * m_ppqn;
    tick_f = 0;
    note_h = 0;
    note_l = SEQ66_MIDI_COUNT_MAX;
    for (auto & e : m_events)
    {
        if (e.is_selected() && e.is_note_on())
        {
            /*
             * We cannot check On/Off here.  It screws up seqevent selection,
             * which has no "off".
             */

            midipulse time = e.timestamp();
            result = true;
            if (time < tick_s)
                tick_s = time;

            if (time > tick_f)
                tick_f = time;

            int note = e.get_note();
            if (note < note_l)
                note_l = note;

            if (note > note_h)
                note_h = note;
        }
    }
    return result;
}

/**
 *  Returns the 'box' of the clipboard items.  Note the common-code betweem
 *  this function and selected_box().  Also note we could return a boolean
 *  indicating if the return values were filled in.
 *
 *  This function is called in qstriggereditor and in qseqroll.
 *
 * \threadsafe
 *
 * \param [out] tick_s
 *      Side-effect return reference for the start time.
 *
 * \param [out] note_h
 *      Side-effect return reference for the high note.
 *
 * \param [out] tick_f
 *      Side-effect return reference for the finish time.
 *
 * \param [out] note_l
 *      Side-effect return reference for the low note.
 */

bool
sequence::clipboard_box
(
    midipulse & tick_s, int & note_h,
    midipulse & tick_f, int & note_l
)
{
    automutex locker(m_mutex);
    bool result = false;
    tick_s = m_maxbeats * m_ppqn;
    tick_f = 0;
    note_h = 0;
    note_l = SEQ66_MIDI_COUNT_MAX;
    if (m_clipboard.empty())
    {
        tick_s = tick_f = note_h = note_l = 0;
    }
    else
    {
        result = true;                  /* FIXME */
        for (auto & e : m_clipboard)
        {
            midipulse time = e.timestamp();
            int note = e.get_note();
            if (time < tick_s)
                tick_s = time;

            if (time > tick_f)
                tick_f = time;

            if (note < note_l)
                note_l = note;

            if (note > note_h)
                note_h = note;
        }
    }
    return result;
}

/**
 *  Counts the selected notes in the event list.
 *
 * \threadsafe
 *
 * \return
 *      Returns m_events.count_selected_notes().
 */

int
sequence::get_num_selected_notes () const
{
    automutex locker(m_mutex);
    return m_events.count_selected_notes();
}

/**
 *  Counts the selected events, with the given status, in the event list.
 *  If the event is a control change (CC), then it must also match the
 *  given CC value.
 *
 * \threadsafe
 *
 * \param status
 *      The desired kind of event to count.
 *
 * \param cc
 *      The desired control-change to count, if the event is a control-change.
 *
 * \return
 *      Returns m_events.count_selected_events().
 */

int
sequence::get_num_selected_events (midibyte status, midibyte cc) const
{
    automutex locker(m_mutex);
    return m_events.count_selected_events(status, cc);
}

#if ! defined USE_STAZED_SELECTION_EXTENSIONS

/**
 *  This function selects events in range of tick start, note high, tick end,
 *  and note low.  Returns the number selected.
 *
 *  Compare this function to the convenience function select_all_notes(),
 *  which doesn't use range information.
 *
 * \threadsafe
 *
 * \param tick_s
 *      The start time of the selection.
 *
 * \param note_h
 *      The high note of the selection.
 *
 * \param tick_f
 *      The finish time of the selection.
 *
 * \param note_l
 *      The low note of the selection.
 *
 * \param action
 *      The action to performer, one of e_select, e_select_one, e_is_selected,
 *      e_would_select, e_deselect, e_toggle_selection, and e_remove_one.
 *
 * \return
 *      Returns the number of events acted on, or 0 if no desired event was
 *      found.
 */

int
sequence::select_note_events
(
    midipulse tick_s, int note_h,
    midipulse tick_f, int note_l, select action
)
{
    int result = 0;
    automutex locker(m_mutex);
    for (auto & er : m_events)
    {
        if (er.get_note() <= note_h && er.get_note() >= note_l)
        {
            midipulse stick = 0;                    // must be initialized
            midipulse ftick = 0;                    // must be initialized
            if (er.is_linked())
            {
                event * ev = er.link();       // pointer
                if (er.is_note_off())
                {
                    stick = ev->timestamp();
                    ftick = er.timestamp();
                }
                else if (er.is_note_on())
                {
                    ftick = ev->timestamp();
                    stick = er.timestamp();
                }

                bool tand = (stick <= tick_f) && (ftick >= tick_s);
                bool tor = (stick <= tick_f) || (ftick >= tick_s);
                if (((stick <= ftick) && tand) || ((stick > ftick) && tor))
                {
                    if (action == select::selecting || action == select::select_one)
                    {
                        er.select();
                        ev->select();
                        ++result;
                        if (action == select::select_one)
                            break;
                    }
                    if (action == select::selected)
                    {
                        if (er.is_selected())
                        {
                            result = 1;
                            break;
                        }
                    }
                    if (action == select::would_select)
                    {
                        result = 1;
                        break;
                    }
                    if (action == select::deselect)
                    {
                        result = 0;
                        er.unselect();
                        ev->unselect();
                    }
                    if (action == select::toggle && er.is_note_on())
                    {
                        ++result;
                        if (er.is_selected())       // don't toggle twice
                        {
                            er.unselect();
                            ev->unselect();
                        }
                        else
                        {
                            er.select();
                            ev->select();
                        }
                    }
                    if (action == select::remove)
                    {
                        remove(er);
                        remove(*ev);
                        reset_draw_marker();
                        ++result;
                        break;
                    }
                }
            }
            else
            {
                stick = ftick = er.timestamp();
                if (stick >= tick_s - 16 && ftick <= tick_f)
                {
                    if
                    (
                        action == select::selecting ||
                        action == select::select_one
                    )
                    {
                        er.select();
                        ++result;
                        if (action == select::select_one)
                            break;
                    }
                    if (action == select::selected)
                    {
                        if (er.is_selected())
                        {
                            result = 1;
                            break;
                        }
                    }
                    if (action == select::would_select)
                    {
                        result = 1;
                        break;
                    }
                    if (action == select::deselect)
                    {
                        result = 0;
                        er.unselect();
                    }
                    if (action == select::toggle)
                    {
                        ++result;
                        if (er.is_selected())
                            er.unselect();
                        else
                            er.select();
                    }
                    if (action == select::remove)
                    {
                        remove(er);
                        reset_draw_marker();
                        ++result;
                        break;
                    }
                }
            }
        }
    }
    return result;
}

#endif  // ! definded USE_STAZED_SELECTION_EXTENSIONS

/**
 *  A convenience function used a couple of times.  Makes if-clauses
 *  easier to read.
 *
 * \param e
 *      Provides the event to be checked.
 *
 * \param status
 *      Provides the event type that must be matched.  However, Set Tempo
 *      events will always be matched.
 *
 * \param tick_s
 *      The lower end of the range of timestamps that the event must fall
 *      within.
 *
 * \param tick_f
 *      The upper end of the range of timestamps that the event must fall
 *      within.
 *
 * \return
 *      Returns true if the event matchs all of the restrictions noted.
 */

bool
sequence::event_in_range
(
    const event & e, midibyte status,
    midipulse tick_s, midipulse tick_f
) const
{
    bool result = e.is_tempo() || e.get_status() == status;
    if (result)
        result = e.timestamp() >= tick_s && e.timestamp() <= tick_f;

    return result;
}

/**
 *  Select all events in the given range, and returns the number
 *  selected.  Note that there is also an overloaded version of this
 *  function.
 *
 * \threadsafe
 *
 * \param tick_s
 *      The start time of the selection.
 *
 * \param tick_f
 *      The finish time of the selection.
 *
 * \param status
 *      The desired event in the selection.  Now, as a new feature, tempo
 *      events are also selectable, in addition to events selected by this
 *      parameter.
 *
 * \param cc
 *      The desired control-change in the selection, if the event is a
 *      control-change.
 *
 * \param action
 *      The desired selection action.
 *
 * \return
 *      Returns the number of events selected.
 */

int
sequence::select_events
(
    midipulse tick_s, midipulse tick_f,
    midibyte status, midibyte cc,
    select action
)
{
    int result = 0;
    automutex locker(m_mutex);
    for (auto & er : m_events)
    {
        if (event_in_range(er, status, tick_s, tick_f))
        {
            midibyte d0, d1;
            er.get_data(d0, d1);
            if (er.is_tempo() || event::is_desired_cc_or_not_cc(status, cc, d0))
            {
                if (action == select::selecting || action == select::select_one)
                {
                    er.select();
                    ++result;
                    if (action == select::select_one)
                        break;
                }
                if (action == select::selected)
                {
                    if (er.is_selected())
                    {
                        result = 1;
                        break;
                    }
                }
                if (action == select::would_select)
                {
                    result = 1;
                    break;
                }
                if (action == select::toggle)
                {
                    if (er.is_selected())
                        er.unselect();
                    else
                        er.select();
                }
                if (action == select::deselect)
                    er.unselect();

                if (action == select::remove)
                {
                    remove(er);
                    reset_draw_marker();
                    ++result;
                    break;
                }
            }
        }
    }
    return result;
}

/**
 *  Selects all events, unconditionally.
 *
 * \threadsafe
 */

void
sequence::select_all ()
{
    automutex locker(m_mutex);
    m_events.select_all();
}

/**
 *  Deselects all events, unconditionally.
 *
 * \threadsafe
 */

void
sequence::unselect ()
{
    automutex locker(m_mutex);
    m_events.unselect_all();
}

/**
 *  Removes and adds selected notes in position.  Also currently moves any
 *  other events in the range of the selection.
 *
 *  Also, we've moved external calls to push_undo() into this function.
 *  The caller shouldn't have to do that.
 *
 *  Another thing this function does is wrap-around when movement occurs.
 *  Any events (except Note Off) that will start just after the END of the
 *  pattern will be wrapped around to the beginning of the pattern.
 *
 * Fixed:
 *
 *  Select all notes in a short pattern that starts at time 0 and has non-note
 *  events starting at time 0 (see contrib/midi/allofarow.mid); move them with
 *  the right arrow, and move them back with the left arrow; then view in the
 *  event editor, and see that the non-Note events have not moved back, and in
 *  fact move way too far to the right, actually to near the END marker.
 *  We've fixed that in the new adjust_timestamp() function.
 *
 *  This function checks for any marked events in seq66, but now we make sure
 *  the event is a Note On or Note Off event before dealing with it.  We now
 *  handle properly events like Program Change, Control Change, and Pitch
 *  Wheel. Remember that Aftertouch is treated like a note, as it has
 *  velocity. For non-Notes, event::get_note() returns m_data[0], and we don't
 *  want to adjust that.
 *
 * \note
 *      We leave a small gap where mark_selected() locks and unlocks, then
 *      we lock again.  This should only be an issue if moving notes while
 *      the sequence is playing.
 *
 * \param delta_tick
 *      Provides the amount of time to move the selected notes.  Note that it
 *      also applies to events.  Note-Off events are expanded to m_length if
 *      their timestamp would be 0.  All other events will wrap around to 0.
 *
 * \param delta_note
 *      Provides the amount of pitch to move the selected notes.  This value
 *      is applied only to Note (On and Off) events.  Also, if this value
 *      would bring a note outside the range of 0 to 127, that note is not
 *      changed and the event is not moved.
 */

void
sequence::move_selected_notes (midipulse delta_tick, int delta_note)
{
    if (mark_selected())                            /* locked recursively   */
    {
        automutex locker(m_mutex);
        m_events_undo.push(m_events);               /* push_undo(), no lock */
        for (auto & er : m_events)
        {
            if (er.is_marked())                     /* is it being moved ?  */
            {
                event e = er;                       /* copy event           */
                e.unmark();                         /* unmark the new event */
                int newnote = e.get_note() + delta_note;
                if (newnote >= 0 && newnote < c_num_keys)
                {
                    midipulse newts = e.timestamp() + delta_tick;
                    newts = adjust_timestamp(newts, e.is_note_off());
                    if (e.is_note())                /* Note On or Note Off  */
                        e.set_note(midibyte(newnote));

                    e.set_timestamp(newts);
                    e.select();                     /* keep it selected     */
                    append_event(e);                /* no sorting is done   */
                    modify();
                }
            }
        }
        if (remove_marked())
            verify_and_link();                      /* may re-sort the list */
    }
}

/**
 *  A new function to consolidate the adjustment of timestamps in a pattern.
 *  Similar to adjust_timestamp, but it doesn't have an \a isnoteoff
 *  parameter.
 *
 * \param t
 *      Provides the timestamp to be adjusted based on m_length.
 *
 * \return
 *      Returns the adjusted timestamp.
 */

midipulse
sequence::trim_timestamp (midipulse t)
{
    if (t >= m_length)
        t -= m_length;

    if (t < 0)                          /* only if midipulse is signed  */
        t += m_length;

    if (t == 0)
        t = m_length - m_note_off_margin;

    return t;
}

/**
 *  A new function to consolidate the adjustment of timestamps in a pattern.
 *
 *      -   If the timestamp plus the delta is greater that m_length, we do
 *          round robin magic.
 *      -   If the timestamp is greater than m_length, then it is wrapped
 *          around to the beginning.
 *      -   If the timestamp equals m_length, then it is set to 0, and later,
 *          trimmed.
 *      -   If the timestamp is less than 0, then it is set to the end.
 *
 *  Taken from similar code in move_selected_notes() and grow_selected().  Be
 *  careful using this function.
 *
 * \param t
 *      Provides the timestamp to be adjusted based on m_length.
 *
 * \param isnoteoff
 *      Used for "expanding" the timestamp from 0 to just less than m_length,
 *      if necessary.  Should be set to true only for Note Off events; it
 *      defaults to false, which means to wrap the events around the end of
 *      the sequence if necessary, and is used only in movement, not in growth.
 *
 * \return
 *      Returns the adjusted timestamp.
 */

midipulse
sequence::adjust_timestamp (midipulse t, bool isnoteoff)
{
    if (t > m_length)
        t -= m_length;

    if (t < 0)                          /* only if midipulse is signed  */
        t += m_length;

    if (isnoteoff)
    {
        if (t == 0)
            t = m_length - m_note_off_margin;
    }
    else                                /* if (wrap)                    */
    {
        if (t == m_length)
            t = 0;
    }
    return t;
}

/**
 *  A new function to consolidate the growth/shrinkage of timestamps in a
 *  pattern.  If the new (off) timestamp is less than the on-time, it is
 *  clipped to the snap value.  If it is greater than the length of the
 *  sequence, then it is clipped to the sequence length.  No wrap-around.
 *
 * \param ontime
 *      Provides the original time, which limits the amount of negative
 *      adjustment that can be done.
 *
 * \param offtime
 *      Provides the timestamp to be adjusted and clipped.
 *
 * \return
 *      Returns the adjusted timestamp.
 */

midipulse
sequence::clip_timestamp (midipulse ontime, midipulse offtime)
{
    if (offtime <= ontime)
        offtime = ontime + get_snap_tick() - note_off_margin();
    else if (offtime >= m_length)
        offtime = m_length - note_off_margin();

    return offtime;
}

/**
 *  Performs a stretch operation on the selected events.  This should move
 *  a note off event, according to old comments, but it doesn't seem to do
 *  that.  See the grow_selected() function.  Rather, it moves any event in
 *  the selection.
 *
 *  Also, we've moved external calls to push_undo() into this function.
 *  The caller shouldn't have to do that.
 *
 * \threadsafe
 *
 * \param delta_tick
 *      Provides the amount of time to stretch the selected notes.
 */

void
sequence::stretch_selected (midipulse delta_tick)
{
    if (mark_selected())
    {
        automutex locker(m_mutex);
        unsigned first_ev = 0x7fffffff;             /* timestamp lower limit */
        unsigned last_ev = 0x00000000;              /* timestamp upper limit */
        m_events_undo.push(m_events);               /* push_undo(), no lock  */
        for (auto & er : m_events)
        {
            if (er.is_selected())
            {
                event * e = &er;
                if (e->timestamp() < midipulse(first_ev))
                    first_ev = e->timestamp();

                if (e->timestamp() > midipulse(last_ev))
                    last_ev = e->timestamp();
            }
        }
        unsigned old_len = last_ev - first_ev;
        unsigned new_len = old_len + delta_tick;
        if (new_len > 1)
        {
            float ratio = float(new_len) / float(old_len);
            mark_selected();                        /* locked recursively   */
            for (auto & er : m_events)
            {
                if (er.is_marked())
                {
                    event n = er;                   /* copy the event       */
                    midipulse t = er.timestamp();
                    n.set_timestamp(midipulse(ratio * (t - first_ev)) + first_ev);
                    n.unmark();
                    add_event(n);
                }
            }
            if (remove_marked())
                verify_and_link();
        }
    }
}

/**
 *  The original description was "Moves note off event."  But this also gets
 *  called when simply selecting a second note via a ctrl-left-click, even in
 *  seq66.  And, though it doesn't move Note Off events, it does reconstruct
 *  them.
 *
 *  This function is called when doing a ctrl-left mouse move on the selected
 *  notes or when using ctrl-left-arrow or ctrl-right-arrow to shrink or
 *  stretch the selected notes.  Using the mouse allows pretty much any amount
 *  of growth or shrinkage, but use the arrow keys limits the changes to the
 *  current snap value.
 *
 *  This function grows/shrinks only Note On events that are marked and linked.
 *  If an event is not linked, this function now ignores the event's timestamp,
 *  rather than risk a segfault on a null pointer.  Compare this function to
 *  the stretch_selected() and move_selected_notes() functions.
 *
 *  This function would strip out non-Notes, but now it at least preserves
 *  them and moves them, to try to preserve their relative position re the
 *  notes.
 *
 *  In any case, we want to mark the original off-event for deletion, otherwise
 *  we get duplicate off events, for example in the "Begin/End" pattern in the
 *  test.midi file.
 *
 *  This function now tries to prevent pathological growth, such as trying to
 *  shrink the notes to zero length or less, or stretch them beyond the length
 *  of the sequence.  Otherwise we get weird and unexpected results.  Also,
 *  we've moved external calls to push_undo() into this function.  The caller
 *  shouldn't have to do that.
 *
 *  A comment on terminology:  The user "selects" notes, while the sequencer
 *  "marks" notes. The first thing this function does is mark all the selected
 *  notes.
 *
 * \threadsafe
 *
 * \param delta
 *      An offset for each linked event's timestamp.
 */

void
sequence::grow_selected (midipulse delta)
{
    if (mark_selected())                            /* locked recursively   */
    {
        automutex locker(m_mutex);                  /* lock it again, dude  */
        m_events_undo.push(m_events);               /* push_undo(), no lock */
        for (auto & er : m_events)
        {
            if (er.is_note())
            {
                if (er.is_marked() && er.is_note_on() && er.is_linked())
                {
                    event * off = er.link();
                    event e = *off;                 /* original off-event   */
                    midipulse offtime = off->timestamp();

                    /*
                     * midipulse ontime = er.timestamp();
                     * midipulse newtime = clip_timestamp(ontime, offtime+delta);
                     */

                    midipulse newtime = trim_timestamp(offtime + delta);

                    off->mark();                    /* kill old off event   */
                    er.unmark();                    /* keep old on event    */
                    e.unmark();                     /* keep new off event   */
                    e.set_timestamp(newtime);       /* new off-time         */
                    add_event(e);                   /* add fixed off event  */
                    modify();
                }
            }
            else if (er.is_marked())                /* non-Note event?      */
            {
                event e = er;                       /* copy original event  */
                midipulse ontime = er.timestamp();
                midipulse newtime = clip_timestamp(ontime, ontime + delta);
                e.set_timestamp(newtime);           /* adjust time-stamp    */
                add_event(e);                       /* add adjusted event   */
                modify();
            }
        }
        if (remove_marked())
            verify_and_link();
    }
}

#if defined USE_STAZED_RANDOMIZE_SUPPORT

void
sequence::randomize_selected
(
    midibyte status, midibyte control, int plus_minus
)
{
    int random;
    midibyte data[2];
    midibyte datitem;
    int datidx = 0;
    automutex locker(m_mutex);
    m_events_undo.push(m_events);               /* push_undo(), no lock  */
    for (auto & e : m_events)
    {
        if (e.is_selected() && e.get_status() == status)
        {
            e.get_data(data[0], data[1]);           /* \tricky code */
            if (event::is_two_byte_msg(status))
                datidx = 1;

            if (event::is_one_byte_msg(status))
                datidx = 0;

            datitem = data[datidx];

            // See http://c-faq.com/lib/randrange.html

            random = (rand() / (RAND_MAX / ((2 * plus_minus) + 1) + 1)) -
                plus_minus;

            datitem += random;
            if (datitem > SEQ66_MAX_DATA_VALUE)         /* 127 */
                datitem = SEQ66_MAX_DATA_VALUE;

            /*
             * Not possible with an unsigned data type.
             *
             * else if (datitem < 0)
             *     datitem = 0;
             */

            data[datidx] = datitem;
            e.set_data(data[0], data[1]);
        }
    }
}

void
sequence::adjust_data_handle (midibyte status, int adata)
{
    midibyte data[2];
    midibyte datitem;
    int datidx = 0;
    automutex locker(m_mutex);
    for (auto & e : m_events)
    {
        if (e.is_selected() && e.get_status() == status)
        {
            e.get_data(data[0], data[1]);           /* \tricky code */
            if (event::is_two_byte_msg(status))
                datidx = 1;

            if (event::is_one_byte_msg(status))
                datidx = 0;

            datitem = adata;
            if (datitem > (SEQ66_MIDI_COUNT_MAX - 1))
                datitem = (SEQ66_MIDI_COUNT_MAX - 1);

            /*
             * Not possible with an unsigned value
             *
             * else if (datitem < 0)
             *     datitem = 0;
             */

            data[datidx] = datitem;
            e.set_data(data[0], data[1]);
        }
    }
}

#endif   // USE_STAZED_RANDOMIZE_SUPPORT

/**
 *  Increments events the match the given status and control values.
 *  The supported statuses are:
 *
 *      -   EVENT_NOTE_ON
 *      -   EVENT_NOTE_OFF
 *      -   EVENT_AFTERTOUCH
 *      -   EVENT_CONTROL_CHANGE
 *      -   EVENT_PITCH_WHEEL
 *      -   EVENT_PROGRAM_CHANGE
 *      -   EVENT_CHANNEL_PRESSURE
 *
 * \threadsafe
 *
 * \param astat
 *      The desired event.
 *
 *  Parameter "acontrol", the desired control-change, is unused.
 *  This might be a bug, or at least a missing feature.
 */

void
sequence::increment_selected (midibyte astat, midibyte /*acontrol*/)
{
    automutex locker(m_mutex);
    for (auto & er : m_events)
    {
        if (er.is_selected())
        {
            if (er.get_status() == astat)   // && er.get_control == acontrol
            {
                if (event::is_two_byte_msg(astat))
                    er.increment_data2();
                else if (event::is_one_byte_msg(astat))
                    er.increment_data1();
            }
        }
    }
}

/**
 *  Decrements events the match the given status and control values.
 *  The supported statuses are:
 *
 *  -   One-byte messages
 *      -   EVENT_PROGRAM_CHANGE
 *      -   EVENT_CHANNEL_PRESSURE
 *  -   Two-byte messages
 *      -   EVENT_NOTE_ON
 *      -   EVENT_NOTE_OFF
 *      -   EVENT_AFTERTOUCH
 *      -   EVENT_CONTROL_CHANGE
 *      -   EVENT_PITCH_WHEEL
 *
 * \threadsafe
 *
 * \param astat
 *      The desired event.
 *
 *  Parameter "acontrol", the desired control-change, is unused.
 *  This might be a bug, or at least a missing feature.
 */

void
sequence::decrement_selected (midibyte astat, midibyte /*acontrol*/)
{
    automutex locker(m_mutex);
    for (auto & er : m_events)
    {
        if (er.is_selected())
        {
            if (er.get_status() == astat)   // && er.get_control == acontrol
            {
                if (event::is_two_byte_msg(astat))
                    er.decrement_data2();
                else if (event::is_one_byte_msg(astat))
                    er.decrement_data1();
            }
        }
    }
}

/**
 *  Copies the selected events.  This function also has the danger, discovered
 *  by user 0rel, of events being modified after being added to the clipboard.
 *  So we add his reconstruction fix here as well.  To summarize the steps:
 *
 *      -#  Clear the m_clipboard.  NO!  If we have no events to
 *          copy to the clipboard, we do not want to clear it.  This kills
 *          cut-and-paste functionality.
 *      -#  Add all selected events in this clipboard to the sequence.
 *      -#  Normalize the timestamps of the events in the clip relative to the
 *          timestamp of the first selected event.  (Is this really needed?)
 *      -#  Reconstruct/reconstitute the m_clipboard.
 *
 *  This process is a bit easier to manage than erase/insert on events because
 *  std::multimap has no erase() function that returns the next valid
 *  iterator.  Also, we use a local clipboard first, to save on copying.
 *  We've enhanced the error-checking, too.
 *
 *  Finally, note that m_clipboard is a static member of sequence, so:
 *
 *      -#  Copying can be done between sequences.
 *      -#  Access to it needs to be protected by a mutex.
 *
 * \threadsafe
 */

void
sequence::copy_selected ()
{
    automutex locker(m_mutex);
    event_list clipbd;
    for (auto & e : m_events)
    {
        if (e.is_selected())
            clipbd.add(e);                              /* sorts every time */
    }
    if (! clipbd.empty())
    {
        midipulse first_tick = event_list::dref(clipbd.begin()).timestamp();
        if (first_tick >= 0)
        {
            for (auto & e : clipbd)                     /* 2019-09-12       */
            {
                midipulse t = e.timestamp();
                if (t >= first_tick)
                    e.set_timestamp(t - first_tick);    /* slide left!      */
            }
        }
        m_clipboard = clipbd;
    }
}

/**
 *  Cuts the selected events.  Pushes onto the undo stack, may copy the
 *  events, marks the selected events, and removes them.  Now also sets the
 *  dirty flag so that the caller doesn't have to.  Also raises the modify
 *  flag on the parent performer object.
 *
 * \threadsafe
 *
 * \param copyevents
 *      If true, copy the selected events before marking and removing them.
 */

void
sequence::cut_selected (bool copyevents)
{
    push_undo();
    if (copyevents)
        copy_selected();

    if (mark_selected())                            /* locked recursively   */
    {
        if (remove_marked())
        {
            set_dirty();                            /* do it for the caller */
            modify();
        }
    }
}

/**
 *  Pastes the selected notes (and only note events) at the given tick and
 *  the given note value.
 *
 *  Also, we've moved external calls to push_undo() into this function.
 *  The caller shouldn't have to do that.
 *
 *  The event_keys used to access/sort the multimap event_list is not updated
 *  after changing timestamp/rank of the stored events.  Regenerating all
 *  key/value pairs before merging them solves this issue, so that
 *  the order of events in the sequence will be preserved.  This action is not
 *  needed for moving or growing events.  Nor is it needed if the old
 *  std::list implementation of the event container is compiled in.  However,
 *  it is needed in any operation that modifies the timestamp of an event
 *  inside the container:
 *
 *      -   copy_selected()
 *      -   paste_selected()
 *      -   quantize_events()
 *
 *  The alternative to reconstructing the map is to erase-and-insert the
 *  events modified in the code above, rather than just tweaking their values,
 *  which have an effect on sorting for the event-map implementation.
 *  However, multimap does not provide an erase() function that returns the
 *  next valid iterator, which would complicate this method of operation.  So
 *  we're inclined to stick with this solution.
 *
 *  There was an issue with copy/pasting a whole sequence.  The pasted events
 *  did not go to their destination, but overlayed the original events.  This
 *  bugs also occurred in Seq24 0.9.2.  It occurs with the allofarow.mid file
 *  when doing Ctrl-A Ctrl-C Ctrl-V Move-Mouse Left-Click.  It turns out the
 *  original code was checking only the first event to see if it was a Note
 *  event.  For sequences that started with a Control Change or Program Change
 *  (or other non-Note events), the highest note was never modified, and none
 *  of the note events were adjusted.
 *
 *  Finally, we only want to transpose note events (i.e. alter m_data[0]),
 *  and not other kinds of events.  We still need to figure out what to do
 *  with aftertouch, though.  Currently likely to be covered by the processing
 *  of the note that it accompanies.
 *
 * \threadsafe
 *
 * \param tick
 *      The time destination for the paste. This represents the "x" coordinate
 *      of the upper left corner of the paste-box.  It will be converted to an
 *      offset, for example pasting every event 48 ticks forward from the
 *      original copy.
 *
 * \param note
 *      The note/pitch destination for the paste. This represents the "y"
 *      coordinate of the upper left corner of the paste-box.  It will be
 *      converted to an offset, for example pasting every event 7 notes
 *      higher than the original copy.
 */

void
sequence::paste_selected (midipulse tick, int note)
{
    if (! m_clipboard.empty())
    {
        automutex locker(m_mutex);
        event_list clipbd = m_clipboard;            /* copy the clipboard   */
        int highest_note = 0;
        push_undo();                                /* push undo, no lock   */
        for (auto & e : clipbd)
        {
            midipulse t = e.timestamp();
            e.set_timestamp(t + tick);
            if (e.is_note())                        /* includes Aftertouch  */
            {
                midibyte n = e.get_note();
                if (n > highest_note)
                    highest_note = n;
            }
        }

        int note_delta = note - highest_note;
        for (auto & e : clipbd)
        {
            if (e.is_note())                        /* includes Aftertouch  */
            {
                midibyte n = e.get_note();
                e.set_note(n + note_delta);
            }
        }

        /*
         * Suitable for std::list and std::vector.  The std::multimap no longer
         * supported.  Note that merge() requires m_events and Clipbd to be
         * sorted first.
         */

        m_events.merge(clipbd);                 /* will presort clipboard   */
        verify_and_link();
        reset_draw_marker();
        modify();
    }
}

/**
 *  Changes the event data range.  Changes only selected events, if any.
 *
 * \threadsafe
 *
 *  Let t == the current tick value; ts == tick start value; tf == tick
 *  finish value; ds = data start value; df == data finish value; d = the
 *  new data value.  Then
 *
\verbatim
             df (t - ts) + ds (tf - t)
        d = --------------------------
                    tf  -   ts
\endverbatim
 *
 *  If this were an interpolation formula it would be:
 *
\verbatim
                            t -  ts
        d = ds + (df - ds) ---------
                            tf - ts
\endverbatim
 *
 *  Something is not quite right; to be investigated.
 *
 * \param tick_s
 *      Provides the starting tick value.
 *
 * \param tick_f
 *      Provides the ending tick value.
 *
 * \param status
 *      Provides the event status that is to be changed.
 *
 * \param cc
 *      Provides the event control value.
 *
 * \param data_s
 *      Provides the starting data value.
 *
 * \param data_f
 *      Provides the finishing data value.
 *
 * \param useundo
 *      If true, then log the events for a potential undo operation.
 *      This value defaults to false, because, for long patterns, there is a
 *      significant delay while moving the mouse.  Set this value to true only
 *      for the button-release event.
 *
 * \return
 *      Returns true if the data was changed.
 */

bool
sequence::change_event_data_range
(
    midipulse tick_s, midipulse tick_f,
    midibyte status, midibyte cc,
    int data_s, int data_f, bool useundo
)
{
    automutex locker(m_mutex);
    bool result = false;
    bool have_selection = m_events.any_selected_events(status, cc);
    if (useundo)
    {
        if (! get_hold_undo())                          /* stazed           */
            set_hold_undo(true);
    }
    for (auto & er : m_events)
    {
        midibyte d0, d1;
        er.get_data(d0, d1);

        /*
         * We should also match tempo events here.  But we have to treat them
         * differently from the matched status events.
         */

        bool match = er.get_status() == status;
        bool good;                          /* event::is_desired_cc_or_not_cc */
        if (status == EVENT_CONTROL_CHANGE)
            good = match && d0 == cc;       /* correct status & correct cc  */
        else
        {
            if (er.is_tempo())
                good = true;                /* Set tempo always editable    */
            else
                good = match;               /* correct status and not a cc  */
        }

        /*
         * Optimize:  stop at the first event past the high end of the
         * range.
         */

        midipulse tick = er.timestamp();
        if (tick > tick_f)                              /* in range?        */
            break;

        if (tick < tick_s)                              /* in range?         */
            good = false;

        if (have_selection && ! er.is_selected())       /* in selection?     */
            good = false;

        if (good)
        {
#if defined USE_SET_HOLD_UNDO                           /* issue #130       */
            if (! get_hold_undo())                      /* stazed           */
                set_hold_undo(true);
#endif

            if (tick_f == tick_s)
                tick_f = tick_s + 1;                    /* no divide-by-0   */

            int newdata =
            (
                (tick - tick_s) * data_f + (tick_f - tick) * data_s
            ) / (tick_f - tick_s);

            if (newdata < 0)
                newdata = 0;
            else if (newdata > SEQ66_MAX_DATA_VALUE)    /* 127              */
                newdata = SEQ66_MAX_DATA_VALUE;

            /*
             * I think we can assume, at this point, that this is a good
             * channel-message status byte.  However, we must treat tempo
             * events differently.
             */

            if (er.is_tempo())
            {
                midibpm tempo = note_value_to_tempo(midibyte(newdata));
                er.set_tempo(tempo);
            }
            else
            {
                if (event::is_one_byte_msg(status))     /* patch or pressure */
                    d0 = newdata;
                else
                    d1 = newdata;

                er.set_data(d0, d1);
            }
            result = true;
        }
    }
    return result;
}

/**
 *  Changes the event data range.  Changes only selected events, if any.
 *
 * \threadsafe
 *
 * \param tick_s
 *      Provides the starting tick value.
 *
 * \param tick_f
 *      Provides the ending tick value.
 *
 * \param status
 *      Provides the event status that is to be changed.
 *
 * \param cc
 *      Provides the event control value.
 *
 * \param newval
 *      Provides the new data value for (additive) "scaling".
 *
 * \param useundo
 *      If true, then log the events for a potential undo operation.
 *      This value defaults to false, because, for long patterns, there is a
 *      significant delay while moving the mouse.  Set this value to true only
 *      for the button-release event.
 *
 * \return
 *      Returns true if the data was changed.
 */

bool
sequence::change_event_data_relative
(
    midipulse tick_s, midipulse tick_f,
    midibyte status, midibyte cc,
    int newval, bool useundo
)
{
    automutex locker(m_mutex);
    bool result = false;
    bool have_selection = m_events.any_selected_events(status, cc);
    if (useundo)
    {
        if (! get_hold_undo())                          /* stazed           */
            set_hold_undo(true);
    }
    for (auto & er : m_events)
    {
        midibyte d0, d1;
        er.get_data(d0, d1);

        /*
         * We should also match tempo events here.  But we have to treat them
         * differently from the matched status events.
         */

        bool match = er.get_status() == status;
        bool good;                          /* event::is_desired_cc_or_not_cc */
        if (status == EVENT_CONTROL_CHANGE)
            good = match && d0 == cc;       /* correct status & correct cc  */
        else
        {
            if (er.is_tempo())
                good = true;                /* Set tempo always editable    */
            else
                good = match;               /* correct status and not a cc  */
        }

        /*
         * Optimize:  stop at the first event past the high end of the
         * range.
         */

        midipulse tick = er.timestamp();
        if (tick > tick_f)                              /* in range?        */
            break;

        if (tick < tick_s)                              /* in range?         */
            good = false;

        if (have_selection && ! er.is_selected())       /* in selection?    */
            good = false;

        if (good)
        {
            int newdata = d1 + newval;                  /* "scale" data     */
#if defined USE_SET_HOLD_UNDO                           /* issue #130       */
            if (! get_hold_undo())                      /* stazed           */
                set_hold_undo(true);
#endif

            if (newdata < 0)
                newdata = 0;
            else if (newdata > SEQ66_MAX_DATA_VALUE)    /* 127              */
                newdata = SEQ66_MAX_DATA_VALUE;

            if (status == EVENT_NOTE_ON)
                d1 = newdata;

            if (status == EVENT_NOTE_OFF)
                d1 = newdata;

            if (status == EVENT_AFTERTOUCH)
                d1 = newdata;

            if (status == EVENT_CONTROL_CHANGE)
                d1 = newdata;

            if (status == EVENT_PROGRAM_CHANGE)
                d0 = newdata;                           /* d0 == new patch  */

            if (status == EVENT_CHANNEL_PRESSURE)
                d0 = newdata;                           /* d0 == pressure   */

            if (status == EVENT_PITCH_WHEEL)
                d1 = newdata;

            er.set_data(d0, d1);
        }
    }
    return result;
}

/**
 *  Modifies data events according to the parameters active in the LFO window
 *  (lfownd).
 *
 * \param value
 *      Provides the base value for the event data value.  Ranges from 0 to
 *      127 in increments of 0.1.  This amount is added to the result of the
 *      wave_func() calculation.
 *
 * \param range
 *      Provides the range for the event data value.  Ranges from 0 to
 *      127 in increments of 0.1.
 *
 * \param speed
 *      Provides the inverse periodicity (?) for the modifications.  Ranges
 *      from 0 to 16 in increments of 0.01.  Not sure what units this value is
 *      in.
 *
 * \param phase
 *      The phase of the event modification.  Ranges from 0 to 1 (what units?)
 *      in increments of 0.01.
 *
 * \param w
 *      The wave type to apply.  Ranges from 1 to 5.
 *
 * \param status
 *      The status value for the events to modify.
 *
 * \param cc
 *      Provides the control-change value for Control Change events that are
 *      to be modified.
 *
 * \param useundo
 *      If true, then log the events for a potential undo operation.
 *      This value defaults to false, because, for long patterns, there is a
 *      significant delay while moving the mouse.  Set this value to true only
 *      for the button-release event.
 */

void
sequence::change_event_data_lfo
(
    double value, double range, double speed, double phase,
    wave w, midibyte status, midibyte cc, bool useundo
)
{
    automutex locker(m_mutex);
    double dlength = double(m_length);
    double dbw = double(m_time_beat_width);
    bool have_selection = m_events.any_selected_events(status, cc);
    if (m_length == 0)                      /* should never happen, though  */
        dlength = double(m_ppqn);

#if defined SEQ66_PLATFORM_DEBUG_TMI
    printf("DC %g Mod %g Period %g Phase %g\n", value, range, speed, phase);
#endif

    if (useundo)
    {
        if (! get_hold_undo())                          /* stazed           */
            set_hold_undo(true);
    }
    for (auto & e : m_events)
    {
        bool is_set = false;
        midibyte d0, d1;
        e.get_data(d0, d1);

        /*
         * If the event is in the selection, or there is no selection, and if
         * it has the desired status and not CC, or the desired status and
         * the correct control-change value, the we will modify (set) the
         * event.
         */

        if (e.is_selected() || ! have_selection)
            is_set = e.non_cc_match(status) || e.cc_match(status, cc);

        if (is_set)
        {
#if defined USE_SET_HOLD_UNDO                           /* issue #130       */
            if (! get_hold_undo())
                set_hold_undo(true);
#endif

            double dtick = double(e.timestamp());
            double angle = speed * dtick * dbw / dlength + phase;
            int newdata = value + wave_func(angle, w) * range;
            if (newdata < 0)
                newdata = 0;
            else if (newdata > (SEQ66_MIDI_COUNT_MAX - 1))
                newdata = SEQ66_MIDI_COUNT_MAX - 1;

            if (event::is_two_byte_msg(status))
                d1 = newdata;
            else if (event::is_one_byte_msg(status))
                d0 = newdata;

            e.set_data(d0, d1);
        }
    }
}

/**
 *  Adds a note of a given length and  note value, at a given tick
 *  location.  It adds a single Note-On/Note-Off pair.
 *
 *  The paint parameter indicates if we care about the painted event,
 *  so then the function runs though the events and deletes the painted
 *  ones that overlap the ones we want to add.
 *
 *  Also note that push_undo() is not incorporated into this function, for
 *  the sake of speed.
 *
 *  Here, we could ignore events not on the sequence's channel, as an option.
 *  We have to be careful because this function can be used in painting notes.
 *
 * Stazed:
 *
 *      http://www.blitter.com/~russtopia/MIDI/~jglatt/tech/midispec.htm
 *
 *      Note Off: The first data is the note number. There are 128 possible
 *      notes on a MIDI device, numbered 0 to 127 (where Middle C is note
 *      number 60). This indicates which note should be released.  The second
 *      data byte is the velocity, a value from 0 to 127. This indicates how
 *      quickly the note should be released (where 127 is the fastest). It's
 *      up to a MIDI device how it uses velocity information. Often velocity
 *      will be used to tailor the VCA release time.  MIDI devices that can
 *      generate Note Off messages, but don't implement velocity features,
 *      will transmit Note Off messages with a preset velocity of 64.
 *
 *  Also, we now see that seq66 never used the recording-velocity member
 *  (m_rec_vol).  We use it to modify the new m_note_on_velocity member if
 *  the user changes it in the seqedit window.
 *
 * \threadsafe
 *
 * \param tick
 *      The time destination of the new note, in pulses.
 *
 * \param len
 *      The duration of the new note, in pulses.
 *
 * \param note
 *      The pitch destination of the new note.
 *
 * \param paint
 *      If true, repaint the whole set of events, in order to be left with
 *      a clean view of the inserted event.  The default is false.
 *
 * \param velocity
 *      If not set to SEQ66_PRESERVE_VELOCITY, the velocity of the note is
 *      set to this value.  Otherwise, it is hard-wired to the stored note-on
 *      velocity.  The name of this macro is counter-intuitive here.
 *      Currently, the note-off velocity is HARD-WIRED!
 *
 * \return
 *      Returns true if the event was added.
 */

bool
sequence::add_note
(
    midipulse tick, midipulse len, int note,
    bool paint, int velocity
)
{
    bool result = false;
    if (tick >= 0 && note >= 0 && note < c_num_keys)
    {
        automutex locker(m_mutex);
        bool hardwire = velocity == SEQ66_PRESERVE_VELOCITY;
        bool ignore = false;
        if (paint)                        /* see the banner above */
        {
            result = true;
            for (auto & er : m_events)
            {
                if (er.is_painted() && er.is_note_on() && er.timestamp() == tick)
                {
                    if (er.get_note() == note)
                    {
                        ignore = true;
                        break;
                    }
                    er.mark();
                    if (er.is_linked())
                        er.link()->mark();

                    set_dirty();
                }
            }
            (void) remove_marked();
        }
        if (! ignore)
        {
            event e
            (
                tick, EVENT_NOTE_ON, note,
                hardwire ? midibyte(m_note_on_velocity) : velocity
            );
            if (paint)
                e.paint();

            result = add_event(e);
            if (result)
            {
                /*
                 * Will be consistent with how m_note_on_velocity is handled
                 * above; enable 0 velocity (a standard?) for note off when not
                 * playing.
                 */

                e.set_status(EVENT_NOTE_OFF);
                e.set_data(note, hardwire ? midibyte(m_note_off_velocity) : 0);
                e.set_timestamp(tick + len);
                result = add_event(e);
            }
        }
        if (result)
            verify_and_link();
    }
    return result;
}

/**
 *  Adds a chord of a given length and  note value, at a given tick
 *  location.
 *
 * \todo
 *      Add the ability to preserve the incoming velocity.
 *
 * \threadsafe
 *
 * \param chord
 *      If greater than 0 (and less than c_chord_number), a chord (multiple
 *      notes) will be generated using this chord in the c_chord_table[]
 *      array.  Otherwise, only a single note will be added.
 *
 * \param tick
 *      The time destination of the new note, in pulses.
 *
 * \param len
 *      The duration of the new note, in pulses.
 *
 * \param note
 *      The pitch destination of the new note.
 *
 * \return
 *      Returns true if the events were added.
 */

bool
sequence::add_chord (int chord, midipulse tick, midipulse len, int note)
{
    bool result = false;
    push_undo();
    if (chord > 0 && chord < c_chord_number)
    {
        for (auto cnote : c_chord_table[chord])
        {
            if (cnote == -1)
                break;

            result = add_note(tick, len, note + cnote, false);
            if (! result)
                break;
        }
    }
    else
        result = add_note(tick, len, note, true);

    return result;
}

/**
 *  Adds an event to the internal event list in a sorted manner.  Then it
 *  reset the draw-marker and sets the dirty flag.
 *
 *  Currently, when reading a MIDI file [see the midifile::parse() function],
 *  only the main events (notes, after-touch, pitch, program changes, etc.)
 *  are added with this function.  So, we can rely on reading only playable
 *  events into a sequence.  Well, actually, certain meta-events are also
 *  read, to obtain channel, buss, and more settings.  Also read for a
 *  sequence, if the global-sequence flag is not set, are the new key, scale,
 *  and background sequence parameters.
 *
 *  This module (sequencer) adds all of those events as well, but it
 *  can surely add other events.  We should assume that any events
 *  added by sequencer are playable/usable.
 *
 *  Here, we could ignore events not on the sequence's channel, as an option.
 *  We have to be careful because this function can be used in painting events.
 *
 * \threadsafe
 *
 * \warning
 *      This pushing (and, in writing the MIDI file, the popping),
 *      causes events with identical timestamps to be written in
 *      reverse order.  Doesn't affect functionality, but it's puzzling
 *      until one understands what is happening.  Actually, this is true only
 *      in Seq24, we've fixed that behavior for Seq66.
 *
 * \param er
 *      Provide a reference to the event to be added; the event is copied into
 *      the events container.
 *
 * \return
 *      Returns true if the event was added.
 */

bool
sequence::add_event (const event & er)
{
    automutex locker(m_mutex);
    bool result = m_events.add(er);     /* post/auto-sorts by time & rank   */
    if (result)
    {
        reset_draw_marker();
        set_dirty();
    }
    else
    {
        errprint("sequence::add_event(): failed");
    }
    return result;
}

/**
 *  An alternative to add_event() that does not sort the events, even if the
 *  event list is implemented by an std::list.  This function is meant mainly
 *  for reading the MIDI file, to save a lot of time.  We could also add a
 *  channel parameter, if the event has a channel.  This reveals that in
 *  midifile and wrkfile, we update the channel setting too many times.
 *  SOMETHING TO INVESTIGATE.
 *
 * \param er
 *      Provide a reference to the event to be added; the event is copied into
 *      the events container.
 *
 * \return
 *      Returns true if the event was appended.
 */

bool
sequence::append_event (const event & er)
{
    automutex locker(m_mutex);
    return m_events.append(er);     /* does *not* sort, too time-consuming */
}

/**
 *  Adds a event of a given status value and data values, at a given tick
 *  location.
 *
 *  The paint parameter indicates if we care about the painted event,
 *  so then the function runs though the events and deletes the painted
 *  ones that overlap the ones we want to add.
 *
 * \threadsafe
 *
 * \param tick
 *      The time destination of the event.
 *
 * \param status
 *      The type of event to add.
 *
 * \param d0
 *      The first data byte for the event.
 *
 * \param d1
 *      The second data byte for the event (if needed).
 *
 * \param paint
 *      If true, the inserted event is marked for painting.
 */

bool
sequence::add_event
(
    midipulse tick, midibyte status,
    midibyte d0, midibyte d1, bool paint
)
{
    automutex locker(m_mutex);
    bool result = false;
    if (tick >= 0)
    {
        if (paint)
        {
            for (auto & er : m_events)
            {
                if (er.is_painted() && er.timestamp() == tick)
                {
                    er.mark();
                    if (er.is_linked())
                        er.link()->mark();

                    set_dirty();
                }
            }
            (void) remove_marked();
        }
        event e(tick, status, d0, d1);
        if (paint)
            e.paint();

        result = add_event(e);
    }
    if (result)
        verify_and_link();

    return result;
}

/**
 *  Handles loop/replace status on behalf of seqrolls.  This sets the
 *  loop-reset status, which is checked in the stream_event() function in this
 *  module.  This status is set when the time-stamp remainder is less than a
 *  quarter note, meaning we have just gotten back to the beginning of the
 *  loop.
 */

bool
sequence::check_loop_reset ()
{
    bool result = false;
    if (m_overwrite_recording && m_parent->is_running() && m_length > 0)
    {
        midipulse tstamp = m_parent->get_tick() % m_length;
printf("check loop reset\n");
        if (tstamp < (m_ppqn / 4))
        {
            loop_reset(true);
            result = true;
        }
    }
    return result;
}

/**
 *  Streams the given event.  The event's timestamp is adjusted, if needed.
 *  If recording:
 *
 *      -   If the pattern is playing, the event is added.
 *      -   If the pattern is playing and quantized record is in force, the
 *          note's timestamp is altered.
 *      -   If not playing, but the event is a Note On or Note Off, we add it
 *          and keep track of it.
 *
 *  If MIDI Thru is enabled, the event is also put on the buss.
 *
 *  We are adding a feature where events are rejected if their channel
 *  doesn't match that of the sequence.  This has been a complaint of some
 *  people.  Could modify the add_event() and add_note() functions, but
 *  better to do it here for comprehensive event support.  Also have to make
 *  sure the event-channel is preserved before this function is called, and
 *  also need to make sure that the channel is appended on both playback and
 *  in saving of the MIDI file.
 *
 *  We are also adding the usage, at last, of the m_rec_vol member, including
 *  the "Free" menu entry in seqedit, which sets the velocity to
 *  SEQ66_PRESERVE_VELOCITY (-1).
 *
 * \todo
 *      When we feel like debugging, we will replace the global is-playing
 *      call with the parent performer's is-running call.
 *
 * \threadsafe
 *
 * \param ev
 *      Provides the event to stream.
 *
 * \return
 *      Returns true if the event's channel matched that of this sequence,
 *      and the channel-matching feature was set to true.  Also returns true
 *      if we're not using channel-matching.  A return value of true means
 *      the event should be saved.
 */

bool
sequence::stream_event (event & ev)
{
    automutex locker(m_mutex);
    bool result = channels_match(ev);           /* set if channel matches   */
    if (result)
    {
        /**
         * If in overwrite loop-record mode, any events after reset should
         * clear the old items from the previous pass through the loop.
         *
         * \todo
         *      If the last event was a Note Off, we should clear it here, and
         *      how?
         */

        if (overwrite_recording() && loop_reset())
        {
            loop_reset(false);
            remove_all();                       /* clear old items          */
            set_dirty();
        }
        ev.set_status(ev.get_status());         /* clear the channel nybble */
        ev.mod_timestamp(m_length);             /* adjust tick re length    */
        if (m_recording)
        {
            if (m_parent->is_pattern_playing()) /* m_parent->is_running()   */
            {
                if (ev.is_note_on() && m_rec_vol > SEQ66_PRESERVE_VELOCITY)
                    ev.set_note_velocity(m_rec_vol);    /* modify incoming  */

                add_event(ev);                          /* locks and sorts  */
                set_dirty();
            }
            else
            {
                /*
                 * Supports the step-edit feature, where we are entering notes
                 * without playback occurring, so we set the generic default
                 * note length and volume to the snap.  If the
                 * recording-volume is SEQ66_DEFAULT_NOTE_ON_VELOCITY, then we
                 * have to set a default value, 100.
                 */

                if (ev.is_note_on())
                {
                    bool keepvelocity =
                        m_rec_vol == SEQ66_PRESERVE_VELOCITY || m_rec_vol == 0;

                    int velocity = int(ev.get_note_velocity());
                    if (velocity == 0)
                        velocity = SEQ66_DEFAULT_NOTE_ON_VELOCITY;

                    if (! keepvelocity)
                        velocity = m_rec_vol;

                    m_events_undo.push(m_events);       /* push_undo()      */
                    add_note                            /* more locking     */
                    (
                        mod_last_tick(), get_snap_tick() - m_note_off_margin,
                        ev.get_note(), false, velocity
                    );
                    set_dirty();
                    ++m_notes_on;
                }
                else if (ev.is_note_off())
                {
                    if (m_notes_on > 0)
                    --m_notes_on;

                    if (m_notes_on == 0)
                        m_last_tick += get_snap_tick();
                }
            }
        }
        if (m_thru)
            put_event_on_bus(ev);                       /* removed locking  */

        link_new();                                     /* removed locking  */
        if (m_quantized_rec && m_parent->is_pattern_playing())
        {
            if (ev.is_note_off())
            {
                midipulse timestamp = ev.timestamp();
                midibyte note = ev.get_note();
                select_note_events
                (
                    timestamp, note, timestamp, note, select::selecting
                );
                quantize_events(EVENT_NOTE_ON, 0, get_snap_tick(), 1, true);
            }
        }
    }
    return result;
}

/**
 *  Sets the dirty flags for names, main, and performance.  These flags are
 *  meant for causing user-interface refreshes, not for performance
 *  modification.
 *
 *  m_dirty_names is set to false in is_dirty_names(); m_dirty_main is set to
 *  false in is_dirty_main(); m_dirty_perf is set to false in
 *  is_dirty_perf().
 *
 * \threadunsafe
 */

void
sequence::set_dirty_mp ()
{
    m_dirty_names = m_dirty_main = m_dirty_perf = true;
}

/**
 *  Call set_dirty_mp() and then sets the dirty flag for editing. Note that it
 *  does not call performer::modify().
 *
 * \threadsafe
 */

void
sequence::set_dirty ()
{
    set_dirty_mp();
    m_dirty_edit = true;
}

/**
 *  Returns the value of the dirty names (heh heh) flag, and sets that
 *  flag to false.  Not sure that we need to lock a boolean on modern
 *  processors.
 *
 *  At this point, another thread might read the initial value of the flag,
 *  before it is falsified.  Since it is used to initiate updates in callers,
 *  the penalty is that the caller updates when it doesn't have to.
 *
 * \threadsafe
 *
 * \return
 *      Returns the dirty status.
 */

bool
sequence::is_dirty_names () const
{
    bool result = m_dirty_names;        /* atomic   */
    m_dirty_names = false;              /* mutable  */
    return result;
}

/**
 *  Returns the value of the dirty main flag, and sets that flag to false
 *  (i.e. resets it).  This flag signals that a redraw is needed from
 *  recording.
 *
 * \threadsafe
 *
 * \return
 *      Returns the dirty status.
 */

bool
sequence::is_dirty_main () const
{
    bool result = m_dirty_main;         /* atomic   */
    m_dirty_main = false;               /* mutable  */
    return result;
}

/**
 *  Returns the value of the dirty performance flag, and sets that
 *  flag to false.
 *
 * \threadsafe
 *
 * \return
 *      Returns the dirty status.
 */

bool
sequence::is_dirty_perf () const
{
    bool result = m_dirty_perf;         /* atomic   */
    m_dirty_perf = false;               /* mutable  */
    return result;
}

/**
 *  Returns the value of the dirty edit flag, and sets that flag to false.
 *  The m_dirty_edit flag is set by the function set_dirty().
 *
 * \threadsafe
 *
 * \return
 *      Returns the dirty status.
 */

bool
sequence::is_dirty_edit () const
{
    bool result = m_dirty_edit;         /* atomic   */
    m_dirty_edit = false;               /* mutable  */
    return result;
}

/**
 *  Plays a note from the piano roll on the main bus on the master MIDI
 *  buss.  It flushes a note to the midibus to preview its sound, used by
 *  the virtual piano.
 *
 * \threadsafe
 *
 * \param note
 *      The note to play.  It is not checked for range validity, for the sake
 *      of speed.
 */

void
sequence::play_note_on (int note)
{
    automutex locker(m_mutex);
    event e(0, EVENT_NOTE_ON, midibyte(note), midibyte(m_note_on_velocity));
    m_master_bus->play(m_bus, &e, m_midi_channel);
    m_master_bus->flush();
}

/**
 *  Turns off a note from the piano roll on the main bus on the master MIDI
 *  buss.
 *
 * \threadsafe
 *
 * \param note
 *      The note to turn off.  It is not checked for range validity, for the
 *      sake of speed.
 */

void
sequence::play_note_off (int note)
{
    automutex locker(m_mutex);
    event e(0, EVENT_NOTE_OFF, midibyte(note), midibyte(m_note_on_velocity));
    m_master_bus->play(m_bus, &e, m_midi_channel);
    m_master_bus->flush();
}

/**
 *  Clears the whole list of triggers.
 *
 * \threadsafe
 */

void
sequence::clear_triggers ()
{
    automutex locker(m_mutex);
    m_triggers.clear();
}

/**
 *  Adds a trigger.  A pass-through function that calls triggers::add().
 *  See that function for more details.
 *
 * \threadsafe
 *
 * \param tick
 *      The time destination of the trigger.
 *
 * \param len
 *      The duration of the trigger.
 *
 * \param offset
 *      The performance offset of the trigger.
 *
 * \param fixoffset
 *      If true, adjust the offset.
 */

void
sequence::add_trigger
(
    midipulse tick, midipulse len, midipulse offset, bool fixoffset
)
{
    automutex locker(m_mutex);
    m_triggers.add(tick, len, offset, fixoffset);
}

/**
 *  This function examines each trigger in the trigger list.  If the given
 *  position is between the current trigger's tick-start and tick-end
 *  values, the these values are copied to the start and end parameters,
 *  respectively, and then we exit.  See triggers::intersect().
 *
 * \threadsafe
 *
 * \param position
 *      The position to examine.
 *
 * \param start
 *      The destination for the starting tick of the matching trigger.
 *
 * \param ender
 *      The destination for the ending tick of the matching trigger.
 *
 * \return
 *      Returns true if a trigger was found whose start/end ticks
 *      contained the position.  Otherwise, false is returned, and the
 *      start and end return parameters should not be used.
 */

bool
sequence::intersect_triggers
(
    midipulse position, midipulse & start, midipulse & ender
)
{
    automutex locker(m_mutex);
    return m_triggers.intersect(position, start, ender);
}

/**
 *
 */

bool
sequence::intersect_triggers (midipulse position)
{
    automutex locker(m_mutex);
    return m_triggers.intersect(position);
}

/**
 *  This function examines each note in the event list.  If the given position
 *  is between the current note's on and off time values, the these
 *  values are copied to the start and end parameters, respectively, and the
 *  note value is copied to the note parameter, and then we exit.
 *
 * \threadsafe
 *
 * \param position
 *      The tick position to examine; where the mouse pointer is horizontally,
 *      already converted to a tick number.
 *
 * \param position_note
 *      This is the position of the mouse pointer vertically, already
 *      converted to a note number.
 *
 * \param [out] start
 *      The destination for the starting timestamp of the matching note.
 *
 * \param [out] ender
 *      The destination for the ending timestamp of the matching note.
 *
 * \param [out] note
 *      The destination for the note of the matching event.
 *
 * \return
 *      Returns true if a note-on event was found whose start/end ticks
 *      contained the position.  Otherwise, false is returned, and the
 *      start and end return parameters should not be used.
 */

bool
sequence::intersect_notes
(
    midipulse position, int position_note,
    midipulse & start, midipulse & ender, int & note
)
{
    automutex locker(m_mutex);
    auto on = m_events.begin();
    auto off = m_events.begin();
    while (on != m_events.end())
    {
        event & eon = event_list::dref(on);
        if (position_note == eon.get_note() && eon.is_note_on())
        {
            off = on;                   /* find next "off" event for note   */
            ++off;                                  /* hope this is it!     */
            event & eoff = event_list::dref(off);   /* access event itself  */

            /*
             *  (eon.get_note() != eoff.get_note() || eoff.is_note_on())
             *
             *  Not quite De Morgan's Law below.  It sounds like we want to
             *  find the next Note Off event for the current Note On that
             *  matches the mouse position.
             */

            bool notematch = false;
            for ( ; off != m_events.end(); ++off)
            {
                if (eon.get_note() == eoff.get_note() && eoff.is_note_off())
                {
                    notematch = true;
                    break;
                }
            }
            if (notematch)
            {
                midipulse ontime = eon.timestamp();
                midipulse offtime = eoff.timestamp();
                if (ontime <= position && position <= offtime)
                {
                    start = eon.timestamp();
                    ender = eoff.timestamp();
                    note = eon.get_note();
                    return true;
                }
            }
        }
        ++on;
    }
    return false;
}

/**
 *  This function examines each non-note event in the event list.
 *
 *  If the given position is between the current notes's timestamp-start and
 *  timestamp-end values, the these values are copied to the posstart and posend
 *  parameters, respectively, and then we exit.
 *
 * \threadsafe
 *
 * \param posstart
 *      The starting position to examine.
 *
 * \param posend
 *      The ending position to examine.
 *
 * \param status
 *      The desired status value.
 *
 * \param start
 *      The destination for the starting timestamp  of the matching trigger.
 *
 * \return
 *      Returns true if a event was found whose start/end timestamps
 *      contained the position.  Otherwise, false is returned, and the
 *      start and end return parameters should not be used.
 */

bool
sequence::intersect_events
(
    midipulse posstart, midipulse posend,
    midibyte status, midipulse & start
)
{
    automutex locker(m_mutex);
    midipulse poslength = posend - posstart;
    for (auto & eon : m_events)
    {
        if (status == eon.get_status())
        {
            midipulse ts = eon.timestamp();
            if (ts <= posstart && posstart <= (ts + poslength))
            {
                start = eon.timestamp();    /* side-effect return value */
                return true;
            }
        }
    }
    return false;
}

/**
 *  Grows a trigger.  See triggers::grow_trigger() for more information.
 *  We need to keep the automutex here because qperfroll calls this function
 *  directly.
 *
 * \param tickfrom
 *      The desired from-value back which to expand the trigger, if necessary.
 *
 * \param tickto
 *      The desired to-value towards which to expand the trigger, if necessary.
 *
 * \param len
 *      The additional length to append to tickto for the check.
 *
 * \threadsafe
 */

void
sequence::grow_trigger (midipulse tickfrom, midipulse tickto, midipulse len)
{
    automutex locker(m_mutex);

    /*
     * This check doesn't hurt, but doesn't prevent creating the new trigger.
     *
     * if (! get_queued())
     */

    m_triggers.grow_trigger(tickfrom, tickto, len);
}

/**
 *  Deletes a trigger, that brackets the given tick, from the trigger-list.
 *  See triggers::remove().
 *
 * \threadsafe
 *
 * \param tick
 *      Provides the tick to be used for finding the trigger to be erased.
 */

void
sequence::delete_trigger (midipulse tick)
{
    automutex locker(m_mutex);
    m_triggers.remove(tick);
}

/**
 *  Sets m_trigger_offset and wraps it to m_length.  If m_length is 0, then
 *  m_trigger_offset is simply set to the parameter.
 *
 * \threadsafe
 *
 * \param trigger_offset
 *      The full trigger offset to set.
 */

void
sequence::set_trigger_offset (midipulse trigger_offset)
{
    automutex locker(m_mutex);
    if (m_length > 0)
    {
        m_trigger_offset = trigger_offset % m_length;
        m_trigger_offset += m_length;
        m_trigger_offset %= m_length;
    }
    else
    {
        errprint("set_trigger_offset(): seq length = 0");
        m_trigger_offset = trigger_offset;
    }
}

/**
 *  Splits a trigger.  This is the public overload of split_trigger.
 *
 * \threadsafe
 *
 * \param splittick
 *      The time location of the split.
 */

void
sequence::split_trigger (midipulse splittick)
{
    automutex locker(m_mutex);
    m_triggers.split(splittick);
}

/**
 *  Half-splits a trigger.
 *
 * \param splittick
 *      The time location of the split.
 */

void
sequence::half_split_trigger (midipulse splittick)
{
    automutex locker(m_mutex);
    m_triggers.half_split(splittick);
}

/**
 *  Exact-splits a trigger.
 *
 * \param splittick
 *      The time location of the split.
 */

void
sequence::exact_split_trigger (midipulse splittick)
{
    automutex locker(m_mutex);
    m_triggers.exact_split(splittick);
}

/**
 *  Adjusts trigger offsets to the length specified for all triggers, and undo
 *  triggers.
 *
 * \threadsafe
 *
 *  Might can get rid of this function?
 *
 * \param newlength
 *      The new length of the adjusted trigger.
 */

void
sequence::adjust_trigger_offsets_to_length (midipulse newlength)
{
    automutex locker(m_mutex);
    m_triggers.adjust_offsets_to_length(newlength);
}

/**
 *  Copies triggers to another location.
 *
 * \threadsafe
 *
 * \param starttick
 *      The current location of the triggers.
 *
 * \param distance
 *      The distance away from the current location to which to copy the
 *      triggers.
 */

void
sequence::copy_triggers (midipulse starttick, midipulse distance)
{
    automutex locker(m_mutex);
    m_triggers.copy(starttick, distance);
}

/**
 *  Moves triggers in the trigger-list.
 *
 *  Note the dependence on the m_length member being kept in sync with the
 *  parent's value of m_length.
 *
 * \threadsafe
 *
 * \param starttick
 *      The current location of the triggers.
 *
 * \param distance
 *      The distance away from the current location to which to move the
 *      triggers.
 *
 * \param direction
 *      If true, the triggers are moved forward. If false, the triggers are
 *      moved backward.
 */

void
sequence::move_triggers (midipulse starttick, midipulse distance, bool direction)
{
    automutex locker(m_mutex);
    m_triggers.move(starttick, distance, direction);
}

/**
 *
 */

bool
sequence::selected_trigger
(
    midipulse droptick, midipulse & tick0, midipulse & tick1
)
{
    automutex locker(m_mutex);
    bool result = m_triggers.select(droptick);
    tick0 = m_triggers.get_selected_start();
    tick1 = m_triggers.get_selected_end();
    return result;
}

/**
 *  Gets the last-selected trigger's start tick.
 *
 * \threadsafe
 *
 * \return
 *      Returns the tick_start() value of the last-selected trigger.  If no
 *      triggers are selected, then -1 is returned.
 */

midipulse
sequence::selected_trigger_start ()
{
    automutex locker(m_mutex);
    return m_triggers.get_selected_start();
}

/**
 *  Gets the selected trigger's end tick.
 *
 * \threadsafe
 *
 * \return
 *      Returns the tick_end() value of the last-selected trigger.  If no
 *      triggers are selected, then -1 is returned.
 */

midipulse
sequence::selected_trigger_end ()
{
    automutex locker(m_mutex);
    return m_triggers.get_selected_end();
}

/**
 *  Moves selected triggers as per the given parameters.
 *
\verbatim
          min_tick][0                1][max_tick
                            2
\endverbatim
 *
 *  The \a which parameter has three possible values:
 *
 *  -#  If we are moving the 0, use first as offset.
 *  -#  If we are moving the 1, use the last as the offset.
 *  -#  If we are moving both (2), use first as offset.
 *
 * \threadsafe
 *
 * \param tick
 *      The tick at which the trigger starts.
 *
 * \param adjustoffset
 *      Set to true if the offset is to be adjusted.
 *
 * \param which
 *      Selects which movement will be done, as discussed above.
 *
 * \return
 *      Returns the value of triggers::move_selected(), which indicates
 *      that the movement could be made.  Used in
 *      Seq24PerfInput::handle_motion_key().
 */

bool
sequence::move_triggers
(
    midipulse tick, bool adjustoffset, triggers::grow which
)
{
    automutex locker(m_mutex);
    return m_triggers.move_selected(tick, adjustoffset, which);
}

/**
 *  Used in the song-sequence grid TODO TODO TODO
 */

void
sequence::offset_triggers (midipulse tick, triggers::grow editmode)
{
    automutex locker(m_mutex);
    m_triggers.offset_selected(tick, editmode);
}

/**
 *  Get the ending value of the last trigger in the trigger-list.
 *
 * \threadsafe
 *
 * \return
 *      Returns the maximum trigger value.
 */

midipulse
sequence::get_max_trigger () const
{
    automutex locker(m_mutex);
    return m_triggers.get_maximum();
}

/**
 *  Checks the list of triggers against the given tick.  If any
 *  trigger is found to bracket that tick, then true is returned.
 *
 * \param tick
 *      Provides the tick of interest.
 *
 * \return
 *      Returns true if a trigger is found that brackets the given tick.
 */

bool
sequence::get_trigger_state (midipulse tick) const
{
    automutex locker(m_mutex);
    return m_triggers.get_state(tick);
}

/**
 *  Returns a copy of the triggers for this sequence.  This function is
 *  basically a threadsafe version of sequence::triggerlist().
 *
 * \return
 *      Returns of copy of m_triggers.triggerlist().
 */

triggers::List
sequence::get_triggers () const
{
    automutex locker(m_mutex);
    return triggerlist();
}

/**
 *  Checks the list of triggers against the given tick.  If any
 *  trigger is found to bracket that tick, then true is returned, and
 *  the trigger is marked as selected.
 *
 * \param tick
 *      Provides the tick of interest.
 *
 * \return
 *      Returns true if a trigger is found that brackets the given tick;
 *      this is the return value of m_triggers.select().
 */

bool
sequence::select_trigger (midipulse tick)
{
    automutex locker(m_mutex);
    return m_triggers.select(tick);
}

/**
 *  Unselects the desired trigger.
 *
 * \param tick
 *      Indicates the trigger to be unselected.
 */

bool
sequence::unselect_trigger (midipulse tick)
{
    automutex locker(m_mutex);
    return m_triggers.unselect(tick);
}

/**
 *  Unselects all triggers.
 *
 * \return
 *      Returns the m_triggers.unselect() return value.
 */

bool
sequence::unselect_triggers ()
{
    automutex locker(m_mutex);
    return m_triggers.unselect();
}

/**
 *  Deletes the first selected trigger that is found.
 *
 * \return
 *      Returns true if a trigger was found and deleted.
 */

bool
sequence::delete_selected_triggers ()
{
    automutex locker(m_mutex);
    return m_triggers.remove_selected();
}

/**
 *  Copies and deletes the first selected trigger that is found.
 *
 * \return
 *      Returns true if a trigger was found, copied, and deleted.
 */

bool
sequence::cut_selected_trigger ()
{
    automutex locker(m_mutex);
    copy_selected_trigger();                    /* locks itself (recursive) */
    return m_triggers.remove_selected();
}

/**
 *  First, this function clears any unpasted middle-click tick setting.
 *  Then it copies the first selected trigger that is found.
 */

void
sequence::copy_selected_trigger ()
{
    automutex locker(m_mutex);
    set_trigger_paste_tick(SEQ66_NO_PASTE_TRIGGER);
    m_triggers.copy_selected();
}

/**
 *  If there is a copied trigger, then this function grabs it from the trigger
 *  clipboard and adds it.
 *
 *  Why isn't this protected by a mutex?  We will enable this if anything bad
 *  happens, such as a deadlock, or corruption, that we can prove happens
 *  here.
 *
 * \param paste_tick
 *      A new parameter that provides the tick for pasting, or
 *      SEQ66_NO_PASTE_TRIGGER (-1) if there is none.
 */

void
sequence::paste_trigger (midipulse paste_tick)
{
    automutex locker(m_mutex);          /* @new ca 2016-08-03   */
    m_triggers.paste(paste_tick);
}

/**
 *  Provides a helper function simplify and speed up performer ::
 *  reset_sequences().  In Live mode, the user controls playback, while in
 *  Song mode, JACK or the performance/song editor controls playback.  This
 *  function used to be called "reset()".
 *
 * \param song_mode
 *      Set to true if song mode is in force.  This setting corresponds to
 *      sequence::playback::song.  False (the default) corresponds to
 *      sequence::playback::live.
 */

void
sequence::stop (bool song_mode)
{
    bool state = get_playing();
    off_playing_notes();
    set_playing(false);
    zero_markers();                         /* sets the "last-tick" value   */
    if (! song_mode)
        set_playing(state);
}

/**
 *  A pause version of stop().  It still includes the note-shutoff capability
 *  to prevent notes from lingering.  Note that we do not call
 *  set_playing(false)... it disarms the sequence, which we do not want upon
 *  pausing.
 *
 * \param song_mode
 *      Set to true if song mode is in force.  This setting corresponds to
 *      performer::playback::song.  False (the default) corresponds to
 *      performer::playback::live.
 */

void
sequence::pause (bool song_mode)
{
    bool state = get_playing();
    off_playing_notes();
    if (! song_mode)
        set_playing(state);
}

/**
 *  This refreshes the draw marker to the first event. It resets the draw marker
 *  so that calls to get_next_note() will start from the first event.
 *
 * \warning
 *      This iterator is shared by about four GUI object, and they might
 *      interfere with each other!
 *
 * \threadsafe
 */

void
sequence::reset_draw_marker ()
{
    automutex locker(m_mutex);
    m_iterator_draw = m_events.begin();
}

/**
 *  This increments the draw marker.
 *
 * \warning
 *      This iterator is shared by about four GUI objects, and they might
 *      interfere with each other!
 *
 * \threadsafe
 */

void
sequence::inc_draw_marker ()
{
    automutex locker(m_mutex);
    ++m_iterator_draw;
}

/**
 *  Sets the draw-trigger iterator to the beginning of the trigger list.
 *
 * \threadsafe
 */

void
sequence::reset_draw_trigger_marker ()
{
    automutex locker(m_mutex);
    m_triggers.reset_draw_trigger_marker();
}

/**
 *  A new function provided so that we can find the minimum and maximum notes
 *  with only one (not two) traversal of the event list.
 *
 * \todo
 *      For efficency, we should calculate this only when the event set
 *      changes, and save the results and return them if good.
 *
 * \threadsafe
 *
 * \param lowest
 *      A reference parameter to return the note with the lowest value.
 *      if there are no notes, then it is set to SEQ66_MAX_DATA_VALUE, and
 *      false is returned.
 *
 * \param highest
 *      A reference parameter to return the note with the highest value.
 *      if there are no notes, then it is set to 0, and false is returned.
 *
 * \return
 *      If there are no notes or tempo events in the list, then false is
 *      returned, and the results should be disregarded.  If true is returned,
 *      but there are only tempo events, then the low/high range is 0 to 127.
 */

bool
sequence::minmax_notes (int & lowest, int & highest) // const
{
    automutex locker(m_mutex);
    bool result = false;
    int low = SEQ66_MAX_DATA_VALUE;
    int high = -1;
    for (auto & er : m_events)
    {
        if (er.is_note_on() || er.is_note_off())
        {
            if (er.get_note() < low)
            {
                low = er.get_note();
                result = true;
            }
            else if (er.get_note() > high)
            {
                high = er.get_note();
                result = true;
            }
        }
        else if (er.is_tempo())
        {
            midibyte notebyte = tempo_to_note_value(er.tempo());
            if (notebyte < low)
                low = notebyte;
            else if (notebyte > high)
                high = notebyte;

            result = true;
        }
    }
    lowest = low;
    highest = high;
    return result;
}

/**
 *
 */

int
sequence::note_count () // const
{
    automutex locker(m_mutex);
    int result = 0;
    for (auto & er : m_events)
    {
        if (er.is_note_on())
            ++result;
    }
    return result;
}

/**
 *  Each call to seqdata() fills the passed references with a events
 *  elements, and returns true.  When it has no more events, returns a
 *  false.
 *
 *  Note that, before the first call to draw a sequence, the
 *  reset_draw_marker() function must be called, to reset m_iterator_draw.
 *
 * \param [out] niout
 *      Provides a pointer destination for a structure hold all of the values
 *      for a note.  Saves a lot of stack pushes.  The note_info class is
 *      nested in the sequence class, which is a friend.
 *
 * \return
 *      Returns a sequence::draw value:  linked, note_on, note_off, or finish.
 *      Note that the new value sequence::draw::tempo could be returned, as
 *      well.
 */

sequence::draw
sequence::get_next_note (note_info & niout) const
{
    niout.ni_tick_finish = 0;
    while (m_iterator_draw != m_events.cend())          /* not threadsafe   */
    {
        draw status = get_note_info(niout, m_iterator_draw);
        ++m_iterator_draw;
        if (status != draw::none)
            return status;
    }
    return draw::finish;
}

/**
 *
 */

sequence::draw
sequence::get_next_note_ex
(
    note_info & niout,
    event_list::const_iterator & evi
) const
{
    while (evi != m_events.cend())
    {
        draw status = get_note_info(niout, evi);
        ++evi;
        if (status != draw::none)
            return status;
    }
    return draw::finish;
}

/**
 *  Copies important information for drawing a note event.
 *
 * \param evi
 *      The current event iterator value.  It is not checked, and is not
 *      iterated after getting the data.
 */

sequence::draw
sequence::get_note_info
(
    note_info & niout,
    event_list::const_iterator & evi
) const
{
    const event & drawevent = event_list::cdref(evi);
    bool isnoteon = drawevent.is_note_on();
    bool islinked = drawevent.is_linked();
    niout.ni_tick_finish    = 0;
    niout.ni_tick_start     = drawevent.timestamp();
    niout.ni_note           = drawevent.get_note();
    niout.ni_selected       = drawevent.is_selected();
    niout.ni_velocity       = drawevent.get_note_velocity();
    if (isnoteon)
    {
        if (islinked)
        {
            niout.ni_tick_finish = drawevent.link()->timestamp();
            return draw::linked;
        }
        else
            return draw::note_on;
    }
    else if (drawevent.is_note_off() && ! islinked)
    {
        return draw::note_off;
    }
    else if (drawevent.is_tempo())
    {
        midibpm bpm = drawevent.tempo();
        midibyte notebyte = tempo_to_note_value(bpm);
        niout.ni_note = int(notebyte);

        /*
         * Hmmmm, must check if tempo events ever have a link. No, they
         * don't but they might need one to properly draw tempo changes!!!
         */

        if (islinked)
            niout.ni_tick_finish = drawevent.link()->timestamp();
        else
            niout.ni_tick_finish = get_length();

        /*
         * Tempo needs to be attained.  This is good only for drawing a
         * horizontal tempo line; we need a way to return both a starting
         * tempo and ending tempo.  Return the latter in velocity?
         */

        return draw::tempo;
    }
    return draw::none;
}

/**
 *  Get the next event in the event list.  Then set the status and control
 *  character parameters using that event.  This overload is used only in
 *  seqedit::popup_event_menu().
 *
 * \param status
 *      Provides a pointer to the MIDI status byte to be set, as a way to
 *      retrieve the event.
 *
 * \param cc
 *      The return pointer for the control value.
 *
 * \return
 *      Returns true if the data is useable, and false if there are no more
 *      events.
 */

bool
sequence::get_next_event (midibyte & status, midibyte & cc)
{
    while (m_iterator_draw != m_events.end())       /* NOT THREADSAFE!!!    */
    {
        midibyte d1;
        const event & drawevent = event_list::cdref(m_iterator_draw);
        status = drawevent.get_status();
        drawevent.get_data(cc, d1);
        inc_draw_marker();
        return true;                /* we have a good one; update and return */
    }
    return false;
}

/**
 *  Reset the caller's iterator.  This is used with get_next_event_match()
 *  and get_next_event_ex().
 *
 * \param evi
 *      The caller's "copy" of the m_events iterator to be reset to
 *      m_events.begin().
 */

void
sequence::reset_ex_iterator (event_list::const_iterator & evi) const
{
    evi = m_events.cbegin();
}

/**
 *
 * \return
 *      Returns true if there is at least one non-terminated linked note in the
 *      interval.
 */

bool
sequence::reset_interval
(
    midipulse t0, midipulse t1,
    event_list::const_iterator & it0,
    event_list::const_iterator & it1
) const
{
    bool result = false;
    bool got_beginning = false;
    event_list::const_iterator iter = m_events.cbegin();
    it0 = iter;
    it1 = m_events.cend();
    for
    (
        event_list::const_iterator iter = m_events.cbegin();
        iter != m_events.cend(); ++iter
    )
    {
        midipulse t = iter->timestamp();
        if (t >= t0)
        {
            if (! got_beginning)
            {
                it0 = iter;                                 /* side-effect  */
                got_beginning = true;
            }
            if (iter->is_linked())
            {
                event * ev = iter->link();
                if (ev->timestamp() >= t1)
                {
                    result = true;

                    // What about terminating iterator ????????????????

                    break;
                }
            }
        }
        else if (t >= t1)
        {
            it1 = iter;                                     /* side-effect  */
            break;
        }
    }
    return result;
}

/**
 *  Get the next event in the event list.  Then set the status and control
 *  character parameters using that event.  This function requires that
 *  reset_ex_iterator() be called to reset to the beginnign of the events list.
 *
 * \param status
 *      Provides a pointer to the MIDI status byte to be set, as a way to
 *      retrieve the event.
 *
 * \param cc
 *      The return pointer for the control value.
 *
 * \param [out] evi
 *      An iterator return value for the next event found.  The caller might
 *      want to check if it is a Tempo event.  Do not use this iterator if
 *      false is returned!  For consistency with get_next_event_match(),
 *      we rely on the caller to increment this pointer for the next call.
 *
 * \return
 *      Returns true if the data is useable, and false if there are no more
 *      events.
 */

bool
sequence::get_next_event_ex
(
    midibyte & status,
    midibyte & cc,
    event_list::const_iterator & evi
)
{
    if (evi != m_events.end())
    {
        midibyte d1;                            /* will be ignored          */
        const event & ev = event_list::cdref(evi);
        status = ev.get_status();
        ev.get_data(cc, d1);
        return true;                /* we have a good one; update and return */
    }
    else
        return false;
}

/**
 *  New version in progress.  This version makes the caller responsible for
 *  providing and maintaining the iterator, so that there are no conflicting
 *  operations on m_draw_iterator from seqdata, seqevent, seqroll, and
 *  perfroll.
 *
 *  This rational version of get_next_event() returns the whole event, rather
 *  than filling in a bunch of parameters.  In addition, it always allows
 *  Tempo events to be found.  Gets the next event in the event list that
 *  matches the given status and control character.  Then set the rest of the
 *  parameters parameters using that event.  If the status is the new value
 *  EVENT_ANY, then any event will be obtained.
 *
 *  Note the usage of event::is_desired_cc_or_not_cc(status, cc, *d0); Either
 *  we have a control change with the right CC or it's a different type of
 *  event.
 *
 * \param status
 *      The type of event to be obtained.  The special value EVENT_ANY can be
 *      provided so that no event statuses are filtered.
 *
 * \param cc
 *      The continuous controller value that might be desired.
 *
 * \param [out] evi
 *      An iterator return value for the next event found.  The caller might
 *      want to check if it is a Tempo event.  Do not use this iterator if
 *      false is returned!
 *
 * \param evtype
 *      A stazed parameter for picking either all event or unselected events.
 *      Defaults to EVENTS_ALL.  Not used unless the macro
 *      USE_STAZED_SELECTION_EXTENSIONS is defined.
 *
 * \return
 *      Returns true if the current event was one of the desired ones, or was
 *      a Tempo event.  In this case, the caller <i> must </i> increment the
 *      iterator.
 */

bool
sequence::get_next_event_match
(
    midibyte status, midibyte cc,
    event_list::const_iterator & evi,
    int /* evtype [see macro below] */
)
{
    while (evi != m_events.end())
    {
        const event & drawevent = event_list::cdref(evi);
        bool istempo = drawevent.is_tempo();
        bool ok = drawevent.get_status() == status || istempo;
        if (! ok)
            ok = status == EVENT_ANY;

#if defined USE_STAZED_SELECTION_EXTENSIONS          // LATER
        if (ok)
        {
            if (evtype == EVENTS_UNSELECTED && drawevent.is_selected())
            {
                ++evi;
                continue;           /* keep trying to find one              */
            }
            if (evtype > EVENTS_UNSELECTED && ! drawevent.is_selected())
            {
                ++evi;
                continue;           /* keep trying to find one              */
            }
        }
#endif  // USE_STAZED_SELECTION_EXTENSIONS

        if (ok)
        {
            midibyte d0;
            drawevent.get_data(d0);
            ok = istempo || event::is_desired_cc_or_not_cc(status, cc, d0);
            if (ok)
            {
                /*
                 * The caller needs to use this iterator, and so must performer
                 * the increment afterwards itself.
                 *
                 * ++evi;
                 */

                return true;
            }
        }
        ++evi;                              /* keep going                   */
    }
    return false;
}

/**
 *  Get the next trigger in the trigger list, and set the parameters based
 *  on that trigger.
 */

bool
sequence::next_trigger (trigger & trig)
{
    trig = m_triggers.next();
    return trig.is_valid();
}

/**
 *  Clears all events from the event container.  Unsets the modified flag.
 *  (Why?) Also see the new copy_events() function.
 */

void
sequence::remove_all ()
{
    automutex locker(m_mutex);
    m_events.clear();
    m_events.unmodify();
}

/**
 * \setter m_last_tick
 *      This function used to be called "set_orig_tick()", now renamed to
 *      match up with get_last_tick().
 *
 * \threadsafe
 */

void
sequence::set_last_tick (midipulse tick)
{
    automutex locker(m_mutex);
    m_last_tick = tick;
}

/**
 *  Returns the last tick played, and is used by the editor's idle function.
 *  If m_length is 0, this function returns m_last_tick - m_trigger_offset, to
 *  avoid an arithmetic exception.  Should we return 0 instead?
 *
 *  Note that seqroll calls this function to help get the location of the
 *  progress bar.  What does perfedit do?
 */

midipulse
sequence::get_last_tick () const
{
    if (m_length > 0)
        return (m_last_tick + m_length - m_trigger_offset) % m_length;
    else
        return m_last_tick - m_trigger_offset;
}

/**
 *  Sets the MIDI buss/port number to dump MIDI data to.
 *
 * \threadsafe
 *
 * \param mb
 *      The MIDI buss to set as the buss number for this sequence.  Also
 *      called the "MIDI port" number.
 *
 * \param user_change
 *      If true (the default value is false), the user has decided to change
 *      this value, and we might need to modify the performer's dirty flag, so
 *      that the user gets prompted for a change,  This is a response to
 *      GitHub issue #47, where buss changes do not cause a prompt to save the
 *      sequence.
 */

void
sequence::set_midi_bus (char mb, bool user_change)
{
    automutex locker(m_mutex);
    if (mb != m_bus)
    {
        off_playing_notes();            /* off notes except initial         */
        m_bus = mb;
        if (user_change)
            modify();                   /* no easy way to undo this, though */

        set_dirty();                    /* this is for display updating     */
    }
}

/**
 *  Sets the length (m_length) and adjusts triggers for it, if desired.
 *  This function is called in seqedit::apply_length(), when the user
 *  selects a sequence length in measures.  This function is also called
 *  when reading a MIDI file.
 *
 *  There's an issue, though.  If the application is compiled to use the
 *  original std::list container for MIDI events, that implementation sorts
 *  the container after every event insertion.  If the application is compiled
 *  to used the std::map container (to speed up the reading of large MIDI
 *  files *greatly*), sorting happens automatically.  But, if we use the
 *  original std::list implementation, but leave the sorting until later (to
 *  speed up the reading of large MIDI files *greatly*), then the
 *  verify_and_link() call that happens with every new event happens before
 *  the events are sorted, and the result is elongated notes showing up in the
 *  pattern slot in the main window.  Therefore, we need a way to skip the
 *  verification when reading a MIDI file, and do the verification only after
 *  all events are read.
 *
 *  That function calculates the length in ticks:
 *
\verbatim
    L = M x B x 4 x P / W
        L == length (ticks or pulses)
        M == number of measures
        B == beats per measure
        P == pulses per quarter-note
        W == beat width in beats per measure
\endverbatim
 *
 *  For our "b4uacuse" MIDI file, M can be about 100 measures, B is 4,
 *  P can be 192 (but we want to support higher values), and W is 4.
 *  So L = 100 * 4 * 4 * 192 / 4 = 76800 ticks.  Seems small.
 *
 * \threadsafe
 *
 * \param len
 *      The length value to be set.  If it is smaller than ppqn/4, then
 *      it is set to that value, unless it is zero, in which case m_length is
 *      used and does not change.  It also sets the length value for the
 *      sequence's triggers.
 *
 * \param adjust_triggers
 *      If true, m_triggers.adjust_offsets_to_length() is called.  The value
 *      defaults to true.
 *
 * \param verify
 *      This new parameter defaults to true.  If true, verify_and_link() is
 *      called.  Otherwise, it is not, and the caller should call this
 *      function with the default value after reading all the events.
 *
 * \return
 *      Returns true if the length value actually changed.
 */

bool
sequence::set_length (midipulse len, bool adjust_triggers, bool verify)
{
    automutex locker(m_mutex);
    bool result = false;
    bool was_playing = get_playing();
    set_playing(false);                 /* turn everything off              */
    if (len > 0)
    {
        if (len < midipulse(m_ppqn / 4))
            len = midipulse(m_ppqn / 4);

        m_length = len;
        result = true;
    }
    else
        len = m_length;

    /*
     * We should set the measures count here.
     */

    m_triggers.set_length(len);         /* must precede adjust call         */
    if (adjust_triggers)
        m_triggers.adjust_offsets_to_length(len);

    if (verify)
    {
        verify_and_link();
        reset_draw_marker();
    }
    if (was_playing)                    /* start up and refresh             */
        set_playing(true);

    return result;
}

/**
 *  Sets the sequence length based on the three given parameters.  There's an
 *  implicit "adjust-triggers = true" parameter used in this function.  Please
 *  note that there is an overload that takes only a measure number and uses
 *  the current beats/bar, PPQN, and beat-width values of this sequence.  The
 *  calculate_unit_measure() function is called, but won't change any values
 *  just because the length (number of measures) changed.
 *
 * \warning
 *      The measures calculation is useless if the BPM (beats/minute) varies
 *      throughout the song.
 *
 * \param bpb
 *      Provides the beats per bar (measure).
 *
 * \param ppqn
 *      Provides the pulses-per-quarter-note to apply to the length
 *      application.
 *
 * \param bw
 *      Provides the beatwidth (typically 4) from the time signature.
 *
 * \param measures
 *      Provides the number of measures the sequence should cover, obtained
 *      from the user-interface.
 */

bool
sequence::apply_length (int bpb, int ppqn, int bw, int measures)
{
    if (ppqn != m_ppqn)
    {
        // what to do?
    }
    bool result = set_length(seq66::measures_to_ticks(bpb, ppqn, bw, measures));
    calculate_unit_measure();                 /* for progress and redrawing   */
    return result;
}

/**
 *  Extends the length of the sequence.  Calls set_length() with the new
 *  length and its default parameters.  Not sure how useful this function is.
 *
 * \param len
 *      The new length of the sequence.
 *
 * \return
 *      Returns the new number of measures.
 */

int
sequence::extend (midipulse len)
{
    (void) set_length(len);
    return calculate_measures();
}

/**
 *  Sets the playing state of this sequence.  When playing, and the sequencer
 *  is running, notes get dumped to the ALSA buffers.
 *
 * \param p
 *      Provides the playing status to set.  True means to turn on the
 *      playing, false means to turn it off, and turn off any notes still
 *      playing.
 */

void
sequence::set_playing (bool p)
{
    automutex locker(m_mutex);
    if (p != get_playing())
    {
        m_playing = p;
        if (! p)
            off_playing_notes();

        set_dirty();
    }
    m_queued = false;
    m_one_shot = false;
}

/**
 * \setter m_recording and m_notes_on
 *
 *  This function sets m_notes_on to 0, only if the recording status has
 *  changed.  It is called by set_input_recording().  We probably need to
 *  explicitly turn off all playing notes; not sure yet.
 *
 * \threadsafe
 */

void
sequence::set_recording (bool r)
{
    automutex locker(m_mutex);
    if (r != m_recording)
    {
        m_notes_on = 0;         // is there a more robust way to do this?
        m_recording = r;
    }
}

/**
 * \setter m_quantized_rec
 *
 *  What about doing this?
 *
 *      m_master_bus->set_sequence_input(record_active, this);
 *
 * \threadsafe
 */

void
sequence::set_quantized_recording (bool qr)
{
    automutex locker(m_mutex);
    if (qr != m_quantized_rec)
    {
        m_notes_on = 0;         // is there a more robust way to do this?
        m_quantized_rec = qr;
    }
}

/**
 *  Like performer::set_sequence_input(), but it uses the internal recording
 *  status directly, rather than getting it from seqedit.
 *
 *  Do we need a quantized recording version, or is setting the
 *  quantized-recording flag sufficient?
 *
 *  Except if already Thru and trying to turn recording (input) off, set input
 *  on here no matter what, because even if m_thru, input could have been
 *  replaced in another sequence.
 *
 * \param record_active
 *      Provides the desired status to set recording.
 *
 * \param toggle
 *      If true, ignore the first parameter and toggle the flag.  The default
 *      value is false.
 */

void
sequence::set_input_recording (bool record_active, bool toggle)
{
    if (toggle)
        record_active = ! m_recording;

    m_master_bus->set_sequence_input(record_active, this);
    set_recording(record_active);
}

/**
 * \setter m_snap_tick
 *
 * \threadsafe
 */

void
sequence::set_snap_tick (int st)
{
    automutex locker(m_mutex);
    m_snap_tick = st;
}

/**
 * \setter m_overwrite_recording
 *
 * \threadsafe
 */

void
sequence::overwrite_recording (bool ovwr)
{
    automutex locker(m_mutex);
    m_overwrite_recording = ovwr;
}

/**
 * \setter m_loop_reset
 *
 * \threadsafe
 */

void
sequence::loop_reset (bool reset)
{
    automutex locker(m_mutex);
    m_loop_reset = reset;
}

/**
 * \setter m_thru
 *
 * \threadsafe
 */

void
sequence::set_thru (bool r)
{
    automutex locker(m_mutex);
    m_thru = r;
}

/**
 *  Like performer::set_sequence_input(), but it uses the internal thru
 *  status directly, rather than getting it from seqedit.
 *
 * \param thru_active
 *      Provides the desired status to set the through state.
 *
 * \param toggle
 *      If true, ignore the first parameter and toggle the flag.  The default
 *      value is false.
 */

void
sequence::set_input_thru (bool thru_active, bool toggle)
{
    if (toggle)
        thru_active = ! m_thru;

    /*
     * Except if already recording and trying to turn Thru (hence input) off,
     * set input to here no matter what, because even in m_recording,
     * input could have been replaced in another sequence.
     *
     * LET's try putting in the original conditional.
     */

    //// if (thru_active or ! m_recording)
     if (! m_recording)
        m_master_bus->set_sequence_input(thru_active, this);

    set_thru(thru_active);
}

/**
 *  Sets the sequence name member, m_name.  This is the name shown in the top
 *  of a mainwid pattern slot.
 *
 *  We now try to include the length of the sequences in measures at the end
 *  of the name, and limit the length of the entire string.  As noted in the
 *  printing of sequence::get_name() in mainwid, this length is 13 characters.
 */

void
sequence::set_name (const std::string & name)
{
    if (name.empty())
        m_name = sm_default_name;
    else
        m_name = name;                                /* legacy behavior  */

    set_dirty();
}

/**
 *  Gets the title of the pattern, to show in the pattern slot.  This function
 *  differs from name, which just returns the value of m_name.  Here, we also
 *  include the length of the sequences in measures at the end of the name,
 *  and limit the length of the entire string.  As noted in the printing of
 *  sequence::get_name() in mainwid, this length is 13 characters.
 *
 * \return
 *      Returns the name of the sequence, with the length in measures of the
 *      pattern wedged in at the end, if non-zero.
 */

std::string
sequence::title () const
{
    int measures = calculate_measures();
    bool showmeasures = true;
    if (measures > 0 && showmeasures)           /* do we have bars to show? */
    {
        char mtemp[16];                         /* holds measures as string */
        char fulltemp[32];                      /* seq name + measures      */
        std::memset(fulltemp, ' ', sizeof fulltemp);
        snprintf(mtemp, sizeof mtemp, " %d", measures);
        for (int i = 0; i < int(m_name.size()); ++i)
        {
            if (i <= (14 - 1))                  /* max size fitting in slot */
                fulltemp[i] = m_name[i];        /* add sequence name/title  */
            else
                break;
        }
        int mlen = int(strlen(mtemp));          /* no. of chars in measures */
        int offset = 14 - mlen;                 /* we're allowed 14 chars   */
        for (int i = 0; i < mlen; ++i)
            fulltemp[i + offset] = mtemp[i];

        fulltemp[14] = 0;                       /* guarantee C string term. */
        return std::string(fulltemp);
    }
    else
        return m_name;
}

/**
 *  Sets the m_midi_channel number, which is the output channel for this
 *  sequence.
 *
 * \threadsafe
 *
 * \param ch
 *      The MIDI channel to set as the output channel number for this
 *      sequence.
 *
 * \param user_change
 *      If true (the default value is false), the user has decided to change
 *      this value, and we might need to modify the performer's dirty flag, so
 *      that the user gets prompted for a change,  This is a response to
 *      GitHub issue #47, where channel changes do not cause a prompt to save
 *      the sequence.
 */

void
sequence::set_midi_channel (midibyte ch, bool user_change)
{
    automutex locker(m_mutex);
    if (ch != m_midi_channel)
    {
        off_playing_notes();
        m_midi_channel = ch;
        if (user_change)
            modify();                   /* no easy way to undo this, though */

        set_dirty();                    /* this is for display updating     */
    }
}

/**
 *  Prints a list of the currently-held events.
 *
 * \threadunsafe
 */

void
sequence::print () const
{
    printf("Seq %d '%s':\n", seq_number(), name().c_str());
    m_events.print();
}

/**
 *  Prints a list of the currently-held triggers.
 *
 * \threadunsafe
 */

void
sequence::print_triggers () const
{
    m_triggers.print(m_name);
}

/**
 *  Takes an event that this sequence is holding, and places it on the MIDI
 *  buss.  This function does not bother checking if m_master_bus is a null
 *  pointer.
 *
 * \param ev
 *      The event to put on the buss.
 *
 * \threadsafe
 */

void
sequence::put_event_on_bus (event & ev)
{
    midibyte note = ev.get_note();
    bool skip = false;
    if (ev.is_note_on())
        ++m_playing_notes[note];        // m_playing_notes[note]++;

    if (ev.is_note_off())
    {
        if (m_playing_notes[note] == 0)
            skip = true;
        else
            --m_playing_notes[note];
    }
    if (! skip)
    {
        m_master_bus->play(m_bus, &ev, m_midi_channel);
        m_master_bus->flush();
    }
}

/**
 *  Sends a note-off event for all active notes.  This function does not
 *  bother checking if m_master_bus is a null pointer.
 *
 * \threadsafe
 */

void
sequence::off_playing_notes ()
{
    automutex locker(m_mutex);
    event e;
    e.set_status(EVENT_NOTE_OFF);
    for (int x = 0; x < c_midi_notes; ++x)
    {
        while (m_playing_notes[x] > 0)
        {
            e.set_data(x, midibyte(0));               /* or is 127 better?  */
            m_master_bus->play(m_bus, &e, m_midi_channel);
            if (m_playing_notes[x] > 0)
                --m_playing_notes[x];
        }
    }
    m_master_bus->flush();
}

/**
 *  Select all events with the given status, and returns the number
 *  selected.  Note that there is also an overloaded version of this
 *  function.
 *
 * \threadsafe
 *
 * \warning
 *      This used to be a void function, so it just returns 0 for now.
 *
 * \param status
 *      Provides the status value to be selected.
 *
 * \param cc
 *      If the status is EVENT_CONTROL_CHANGE, then data byte 0 must
 *      match this value.
 *
 * \param inverse
 *      If true, invert the selection.
 *
 * \return
 *      Always returns 0.
 */

int
sequence::select_events (midibyte status, midibyte cc, bool inverse)
{
    automutex locker(m_mutex);
    midibyte d0, d1;
    for (auto & er : m_events)
    {
        er.get_data(d0, d1);
        bool match = er.get_status() == status;
        bool canselect;
        if (status == EVENT_CONTROL_CHANGE)
            canselect = match && d0 == cc;  /* correct status and correct cc */
        else
            canselect = match;              /* correct status, cc irrelevant */

        if (canselect)
        {
            if (inverse)
            {
                if (! er.is_selected())
                    er.select();
                else
                    er.unselect();
            }
            else
                er.select();
        }
    }
    return 0;
}

/**
 *  Transposes notes by the given steps, in accordance with the given
 *  scale.  If the scale value is 0, this is "no scale", which is the
 *  chromatic scale, where all 12 notes, including sharps and flats, are
 *  part of the scale.
 *
 *  Also, we've moved external calls to push_undo() into this function.
 *  The caller shouldn't have to do that.
 *
 * \note
 *      We noticed (ca 2016-06-10) that MIDI aftertouch events need to be
 *      transposed, but are not being transposed here.  Assuming they are
 *      selectable (another question!), the test for note-on and note-off is
 *      not sufficient, and so has been replaced by a call to
 *      event::is_note_msg().
 *
 * \param steps
 *      The number of steps to transpose the notes.
 *
 * \param scale
 *      The scale to make the notes adhere to while transposing.
 */

void
sequence::transpose_notes (int steps, int scale)
{
    if (mark_selected())                            /* mark original notes  */
    {
        automutex locker(m_mutex);
        event_list transposed_events;
        const int * transpose_table;
        m_events_undo.push(m_events);               /* push_undo(), no lock  */
        if (steps < 0)
        {
            transpose_table = &c_scales_transpose_dn[scale][0];     /* down */
            steps *= -1;
        }
        else
            transpose_table = &c_scales_transpose_up[scale][0];     /* up   */

        for (auto & er : m_events)
        {
            if (er.is_marked() && er.is_note())     /* transposable event?  */
            {
                event e = er;
                e.unmark();
                int note = e.get_note();
                bool off_scale = false;
                if (transpose_table[note % c_octave_size] == 0)
                {
                    off_scale = true;
                    note -= 1;
                }
                for (int x = 0; x < steps; ++x)
                    note += transpose_table[note % c_octave_size];

                if (off_scale)
                    note += 1;

                e.set_note(note);
                transposed_events.add(e);
            }
            else
                er.unmark();                        /* ignore, no transpose  */
        }
        (void) remove_marked();                     /* remove original notes */
        m_events.merge(transposed_events);          /* events get presorted  */
        verify_and_link();
    }
}

#if defined USE_STAZED_SHIFT_SUPPORT

/**
 *  We need to look into this function.
 */

void
sequence::shift_notes (midipulse ticks)
{
    if (mark_selected())
    {
        automutex locker(m_mutex);
        event_list shifted_events;
        m_events_undo.push(m_events);               /* push_undo(), no lock */
        for (auto & er : m_events)
        {
            if (er.is_marked() && er.is_note())     /* shiftable event?     */
            {
                event e = er;
                e.unmark();

                midipulse timestamp = e.timestamp() + ticks;
                if (timestamp < 0L)                 /* wraparound           */
                    timestamp = m_length - ((-timestamp) % m_length);
                else
                    timestamp %= m_length;

                e.set_timestamp(timestamp);
                shifted_events.add(e);
            }
        }
        (void) remove_marked();

        shifted_events.sort();
        m_events.merge(shifted_events);
        verify_and_link();
        set_dirty();                        /* tells perfedit to update     */
    }
}

#endif  // USE_STAZED_SHIFT_SUPPORT

/**
 *  Applies the transpose value held by the master MIDI buss object, if
 *  non-zero, and if the sequence is set to be transposable.
 */

void
sequence::apply_song_transpose ()
{
    int transpose = transposable() ? m_parent->get_transpose() : 0 ;
    if (transpose != 0)
    {
        automutex locker(m_mutex);
        m_events_undo.push(m_events);               /* push_undo(), no lock */
        for (auto & er : m_events)
        {
            if (er.is_note())                       /* also aftertouch      */
                er.transpose_note(transpose);
        }
        set_dirty();
    }
}

/**
 * \setter m_transposable
 *      Changing this flag modifies the sequence and performance.  Note that
 *      when a sequence is being read from a MIDI file, it will not yet have a
 *      parent, so we have to check for that before setting the performer modify
 *      flag.
 */

void
sequence::set_transposable (bool flag)
{
    if (flag != m_transposable)
        modify();

    m_transposable = flag;
}

/**
 *  Grabs the specified events, puts them into a list, quantizes them against
 *  the snap ticks, and merges them in to the event container.  One confusing
 *  things is why the original versions of the events don't seem to be
 *  deleted.
 *
 * \param status
 *      Indicates the type of event to be quantized.
 *
 * \param cc
 *      The desired control-change to count, if the event is a control-change.
 *
 * \param snap_tick
 *      Provides the maximum amount to move the events.  Actually, events are
 *      moved to the previous or next snap_tick value depend on whether they
 *      are halfway to the next one or not.
 *
 * \param divide
 *      A rough indicator of the amount of quantization.  The only values used
 *      in the application are either 1 ("quantize") or 2 ("tighten").
 *      The latter value reduces the amount of change slightly.
 *
 * \param linked
 *      False by default, this parameter indicates if marked events are to be
 *      relinked, as far as we can tell.
 */

void
sequence::quantize_events
(
    midibyte status, midibyte cc,
    midipulse snap_tick, int divide, bool linked
)
{
    automutex locker(m_mutex);
    if (mark_selected())
    {
        /*
         * \note
         *      Do NOT call push_undo() here; quantize_events() is used in
         *      recording.  If you need the push_undo(), please use the
         *      push_quantize() function!
         */

        event_list quantized_events;
        for (auto & er : m_events)
        {
            midibyte d0, d1;
            er.get_data(d0, d1);
            bool match = er.get_status() == status;
            bool canselect;
            if (status == EVENT_CONTROL_CHANGE)
                canselect = match && d0 == cc;  /* correct status, correct cc */
            else
                canselect = match;              /* correct status, any cc     */

            if (! er.is_marked())
                canselect = false;

            if (canselect)
            {
                event e = er;                   /* copy the event             */
                er.select();                    /* selected original event    */
                e.unmark();                     /* unmark copy of the event   */

                midipulse t = e.timestamp();
                midipulse t_remainder = t % snap_tick;
                midipulse t_delta = 0;
                if (t_remainder < snap_tick / 2)
                    t_delta = -(t_remainder / divide);
                else
                    t_delta = (snap_tick - t_remainder) / divide;

                if ((t_delta + t) >= m_length)      /* wrap-around Note On    */
                    t_delta = -e.timestamp();

                e.set_timestamp(e.timestamp() + t_delta);
                quantized_events.add(e);

                /*
                 * The only events linked are notes; the status of all notes
                 * in this function are On, so the link must be only Note Off.
                 */

                if (er.is_linked() && linked)
                {
                    event f = *er.link();
                    midipulse ft = f.timestamp() + t_delta; /* seq32 */
                    f.unmark();
                    er.link()->select();

                    /*
                     * Seq32: If ft is negative, then we have a Note Off
                     * previously wrapped before adjustment. Since the delta
                     * is based on the Note On (not wrapped), we must add back
                     * the m_length for the wrapping.  If the ft is then >=
                     * m_length, it will be deleted by verify_and_link(),
                     * which discards any notes (ON or OFF) that are >=
                     * m_length. So we must wrap if > m_length and trim if ==
                     * m_length.  Compare to trim_timestamp().
                     */

                    if (ft < 0)                     /* unwrap Note Off      */
                        ft += m_length;

                    if (ft == m_length)             /* trim it a little     */
                        ft -= m_note_off_margin;

                    if (ft > m_length)              /* wrap it around       */
                        ft -= m_length;

                    f.set_timestamp(ft);
                    quantized_events.add(f);
                }
            }
        }
        (void) remove_marked();
        m_events.merge(quantized_events);
        verify_and_link();
        set_dirty();                        /* tells perfedit to update     */
    }
}

/**
 *  A new convenience function.  See the sequence::quantize_events() function
 *  for more information.  This function just does locking and a push-undo
 *  before calling that function.
 *
 * \param status
 *      The kind of event to quantize, such as Note On, or the event type
 *      selected in the pattern editor's data pane.
 *
 * \param cc
 *      The control-change value to quantize, again as selected in the pattern
 *      editor's data pane.  For Note Ons, this value should be set to 0.
 *
 * \param snap_tick
 *      The number of ticks to use for quantizing the events.  Usually, this
 *      is the snap value selected in the pattern editor.
 *
 * \param divide
 *      Provides a division value, usually either 1 ("quantize") or 2
 *      ("tighten").
 *
 * \param linked
 *      Set this value to true for tightening notes.  The default value of
 *      this parameter is false.
 */

void
sequence::push_quantize
(
    midibyte status, midibyte cc,
    midipulse snap_tick, int divide, bool linked
)
{
    automutex locker(m_mutex);
    m_events_undo.push(m_events);
    quantize_events(status, cc, snap_tick, divide, linked);
}

#if defined USE_STAZED_COMPANDING

void
sequence::multiply_pattern (double multiplier)
{
    automutex locker(m_mutex);
    m_events_undo.push(m_events);               /* push_undo(), no lock */
    midipulse orig_length = get_length();
    midipulse new_length = midipulse(orig_length * multiplier);
    if (new_length > orig_length)
        (void) set_length(new_length);

    for (auto & er : m_events)
    {
        midipulse timestamp = er.timestamp();
        if (er.is_note_off())
            timestamp += note_off_margin();

        timestamp *= multiplier;
        if (er.is_note_off())
            timestamp -= note_off_margin();

        timestamp %= m_length;
        er.set_timestamp(timestamp);
    }
    verify_and_link();
    if (new_length < orig_length)
        (void) set_length(new_length);

    set_dirty();                        /* tells perfedit to update     */
}

#endif

/**
 *  A member function to dump a summary of events stored in the event-list of
 *  a sequence.
 */

void
sequence::show_events () const
{
    printf
    (
        "sequence #%d '%s': channel %d, events %d\n",
        seq_number(), name().c_str(), get_midi_channel(), event_count()
    );
    const event_list & evl = events();
    for (auto i = evl.cbegin(); i != evl.cend(); ++i)
    {
        const event & er = event_list::cdref(i);
        std::string evdump = to_string(er);
        printf("%s", evdump.c_str());
    }
}

/**
 *  Copies an external container of events into the current container,
 *  effectively replacing all of its events.  Compare this function to the
 *  remove_all() function.  Copying the container is a lot of work, but
 *  fairly fast, even with an std::multimap as the container.  Also note that
 *  we have to recalculate the length of the sequence.
 *
 * \threadsafe
 *      Note that we had to consolidate the replacement of all the events in
 *      the container in order to prevent the "Save to Sequence" button in the
 *      eventedit object from causing the application to segfault.  It would
 *      segfault when the mainwnd timer callback would fire, causing updates
 *      to the sequence's slot pixmap, which would then try to access deleted
 *      events.  Part of the issue was that note links were dropped when
 *      copying the events, so now we call verify_and_link() to hopefully
 *      reconstitute the links.
 *
 * \param newevents
 *      Provides the container of MIDI events that will completely replace the
 *      current container.  Normally this container is supplied by the event
 *      editor, via the eventslots class.
 */

void
sequence::copy_events (const event_list & newevents)
{
    automutex locker(m_mutex);
    m_events.clear();
    m_events = newevents;
    if (m_events.empty())
    {
        m_events.unmodify();
        m_length = 0;
    }
    else
    {
        // WTF?
    }

    m_iterator_draw = m_events.begin();     /* same as in reset_draw_marker */
    if (! m_events.empty())                 /* need at least 1 (2?) events  */
    {
        /*
         * Another option, if we have a new sequence length value (in pulses)
         * would be to call sequence::set_length(len, adjust_triggers).  We do
         * need to re-evaluate the length (last timestamp) of the sequence.
         */

        m_length = m_events.get_length();   /* get potentially new length   */
        verify_and_link();                  /* function uses m_length       */
    }
    set_dirty();
    modify();
}

/**
 * \setter m_parent
 *      Sets the "parent" of this sequence, so that it can get some extra
 *      information about the performance.  Remember that m_parent is not at
 *      all owned by the sequence.  We just don't want to do all the work
 *      necessary to make it a reference, at this time.
 *
 * \param p
 *      A pointer to the parent, assigned only if not already assigned.
 */

void
sequence::set_parent (performer * p)
{
    if (is_nullptr(m_parent) && not_nullptr(p))
        m_parent = p;
}

/**
 *  Why don't we see this in kepler34?  We do, in the MidiPerformance::play()
 *  function.  We refactored this, Chris.  Remember?  :-D
 *
 * \param tick
 *      Provides the current active pulse position, the tick/pulse from which
 *      to start playing.
 *
 * \param playbackmode
 *      If true, we are in Song mode.  Otherwise, Live mode.
 *
 * \param resumenoteons
 *      Indicates if we are to resume Note Ons.  Used by performer::play().
 */

void
sequence::play_queue (midipulse tick, bool playbackmode, bool resumenoteons)
{
    if (check_queued_tick(tick))
    {
        play(get_queued_tick() - 1, playbackmode, resumenoteons);
        toggle_playing(tick, resumenoteons);
    }
    if (one_shot() && one_shot_tick() <= tick)
    {
        play(one_shot_tick() - 1, playbackmode, resumenoteons);
        toggle_playing(tick, resumenoteons);
        toggle_queued();        /* queue it to mute it again after one play */
    }
    play(tick, playbackmode, resumenoteons);
}

/**
 *  Actually, useful mainly for the user-interface, this function calculates
 *  the size of the left and right handles of a note.  The s_handlesize value
 *  is n internal variable for handle size.  Note that, with the default PPQN
 *  of 192, a sixteenth note (a typical snap value) is 48 pulses (ticks), so
 *  that a sixteenth note is broken into equal left, center, and right sides.
 *  However, for a PPQN of, say, 960, 16 pulses is 5 times smaller in width.
 *  We really need to scale the handle size.
 *
 * \param start
 *      The starting tick of the note event.
 *
 * \param finish
 *      The ending tick of the note event.
 *
 * \return
 *      Returns 16 or one-third of the note length.  This value is scaled
 *      according to PPQN if different from 192.
 */

midipulse
sequence::handle_size (midipulse start, midipulse finish)
{
    static const long s_handlesize = 16;
    midipulse result = s_handlesize * m_ppqn / SEQ66_DEFAULT_PPQN;
    midipulse notelength = finish - start;
    if (notelength < result / 3)
        result = notelength / 3;

    return result;
}

/**
 *  Toggles the m_one_shot flag, sets m_off_from_snap to true, and adjusts
 *  m_one_shot_tick according to m_last_tick and m_length.
 */

void
sequence::toggle_one_shot ()
{
    automutex locker(m_mutex);
    set_dirty_mp();
    m_one_shot = ! m_one_shot;
    m_one_shot_tick = m_last_tick - mod_last_tick() + m_length;
    m_off_from_snap = true;
}

/**
 *  Sets the dirty flag, sets m_one_shot to false, and m_off_from_snap to
 *  true. This function remains unused here and in Kepler34.
 */

void
sequence::off_one_shot ()
{
    automutex locker(m_mutex);
    set_dirty_mp();
    m_one_shot = false;
    m_off_from_snap = true;
}

/**
 *  Starts the growing of the sequence for Song recording.  This process
 *  starts by adding a chunk of SEQ66_SONG_RECORD_INC ticks to the
 *  trigger, which allows the rest of the threads to notice the change.
 *
 * \question
 *      Do we need to call set_dirty_mp() here?
 *
 * \param tick
 *      Provides the current tick, which helps set the recorded block's
 *      boundaries, and is copied into m_song_record_tick.
 */

void
sequence::song_recording_start (midipulse tick, bool snap)
{
    add_trigger(tick, SEQ66_SONG_RECORD_INC);
    m_song_recording_snap = snap;
    m_song_record_tick = tick;
    m_song_recording = true;

    /*
     * Do we need to add this setting?
     *
     * m_song_recording_block = false;
     *
     * ALSO, see the ORIGINAL sequence module!
     */
}

/**
 *  Stops the growing of the sequence for Song recording.  If we have been
 *  recording, we snap the end of the trigger segment to the next whole
 *  sequence interval.
 *
 * \question
 *      Do we need to call set_dirty_mp() here?
 *
 * \param tick
 *      Provides the current tick, which helps set the recorded block's
 *      boundaries.
 */

void
sequence::song_recording_stop (midipulse tick)
{
    m_song_playback_block = m_song_recording = false;
    if (m_song_recording_snap)
    {
        midipulse len = m_length - (tick % m_length);
        m_triggers.grow_trigger(m_song_record_tick, tick, len);
        m_off_from_snap = true;
    }
}

/**
 *  If the Note-On event is after the Note-Off event, the pattern wraps around,
 *  so that we play it now to resume.
 *
 * \param tick
 *      The current tick-time, in MIDI pulses.
 */

void
sequence::resume_note_ons (midipulse tick)
{
    for (auto & ei : m_events)
    {
        if (ei.is_note_on())
        {
            event * link = ei.link();
            if (not_nullptr(link))
            {
                midipulse on = ei.timestamp();      /* see banner notes */
                midipulse off = link->timestamp();
                if (on < (tick % m_length) && off > (tick % m_length))
                {
                    automutex locker(m_mutex);
                    put_event_on_bus(ei);
                }
            }
        }
    }
}

/**
 *  Makes a calculation for expanded recording, used in seqedit and qseqroll.
 *  This is the way Seq32 does it now, and it seems to work for Seq66.
 *
 * \return
 *      Returns true if we are recording, expanded-record is enabled,
 *      and we're far enough along in the current length to move to the next
 *      "time window".
 */

bool
sequence::expand_recording () const
{
    bool result = false;
    if (m_recording && m_expanded_recording)
    {
        midipulse tstamp = m_last_tick;     // m_parent->get_tick() % m_length;
        if (tstamp >= expand_threshold())
        {
#if defined SEQ66_PLATFORM_DEBUG_TMI
            printf
            (
                "tick %ld >= length %ld - measure %ld / 4\n",
                tstamp, m_length, unit_measure()
            );
#endif
            result = true;
        }
    }
    return result;
}

/**
 *  Code to help user-interface callers.
 */

bool
sequence::update_recording (int index)
{
    record rectype = static_cast<record>(index);
    bool result = rectype >= record::legacy && rectype <= record::expand;
    if (result)
    {
        switch (rectype)
        {
        case record::legacy:

            overwrite_recording(false);
            expanded_recording(false);
            break;

        case record::overwrite:

            overwrite_recording(true);
            expanded_recording(false);
            break;

        case record::expand:

            overwrite_recording(false);
            expanded_recording(true);
            break;

        default:

            break;
        }
    }
    return result;
}

/**
 *  Implements the actions brought forth from the Tools (hammer) button.
 *
 *  Note that the push_undo() calls push all of the current events (in
 *  sequence::m_events) onto the stack (as a single entry).
 *
 * \todo
 *      Move this code into sequence (without the redraw call).
 */

void
sequence::handle_edit_action (edit action, int var)
{
    switch (action)
    {
    case edit::select_all_notes:

        select_all_notes();
        break;

    case edit::select_inverse_notes:

        select_all_notes(true);
        break;

    case edit::select_all_events:

        select_events(m_status, m_cc);
        break;

    case edit::select_inverse_events:

        select_events(m_status, m_cc, true);
        break;

#if defined USE_STAZED_ODD_EVEN_SELECTION

    case edit::select_even_notes:

        select_even_or_odd_notes(var, true);
        break;

    case edit::select_odd_notes:

        select_even_or_odd_notes(var, false);
        break;

#endif

#if defined USE_STAZED_RANDOMIZE_SUPPORT

    case edit::randomize_events:

        randomize_selected(m_status, m_cc, var);
        break;

#endif

    case edit::quantize_notes:

        /*
         * sequence::quantize_events() is used in recording as well, so we do
         * not want to incorporate sequence::push_undo() into it.  So we make
         * a new function to do that.
         */

        push_quantize(EVENT_NOTE_ON, 0, m_snap, 1, true);
        break;

    case edit::quantize_events:

        push_quantize(m_status, m_cc, m_snap, 1);
        break;

    case edit::tighten_notes:

        push_quantize(EVENT_NOTE_ON, 0, m_snap, 2, true);
        break;

    case edit::tighten_events:

        push_quantize(m_status, m_cc, m_snap, 2);
        break;

    case edit::transpose_notes:                 /* regular transpose    */

        transpose_notes(var, 0);
        set_dirty();                            /* updates perfedit     */
        break;

    case edit::transpose_harmonic:              /* harmonic transpose   */

        transpose_notes(var, m_scale);
        set_dirty();                            /* updates perfedit     */
        break;

#if defined USE_STAZED_COMPANDING

    case edit::expand_pattern:

        multiply_pattern(2.0);
        break;

    case edit::compress_pattern:
        multiply_pattern(0.5);
        break;
#endif

    default:
        break;
    }
}

}           // namespace seq66

/*
 * sequence.cpp
 *
 * vim: sw=4 ts=4 wm=4 et ft=cpp
 */

