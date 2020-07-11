; based from https://www.midi.org/specifications-old/item/table-1-summary-of-midi-message

expr midi_cmd_idx        0
expr midi_cmd_mask       0xF0
expr midi_chan_mask      0x0F

; channel messages, cmd must be masked
expr midi_cmd_note_off   0x80 ; note off
expr midi_cmd_note_on    0x90 ; note on
expr midi_cmd_polytouch  0xA0 ; polyphonic aftertouch
expr midi_cmd_cc         0xB0 ; control change
expr midi_cmd_progch     0xC0 ; program change
expr midi_cmd_chantouch  0xD0 ; channel aftertouch
expr midi_cmd_pitchbend  0xE0 ; pitch bend

; system messages, not masked, no channel
expr midi_cmd_sysex      0xF0 ; system exclusive message
expr midi_cmd_timeqframe 0xF1 ; time code quarter frame
expr midi_cmd_songpos    0xF2 ; song position counter
expr midi_cmd_songsel    0xF3 ; song selection
expr midi_cmd_tunereq    0xF6 ; tune request
expr midi_cmd_sysexend   0xF7 ; end of sysex message
expr midi_cmd_timeclock  0xF8 ; timing clock, shouldn't come from JACK, so used
                              ; for timer events
expr midi_cmd_start      0xFA ; start
expr midi_cmd_continue   0xFB ; continue
expr midi_cmd_stop       0xFC ; stop
expr midi_cmd_sense      0xFE ; active sense pulse
expr midi_cmd_reset      0xFF ; reset

; note on, note off, polyphonic aftertouch
expr midi_note_length  3
expr midi_note_idx     1

; note on, note off
expr midi_note_velocity_idx 2

; polyphonic aftertouch
expr midi_note_pressure_idx 2

; control change
expr midi_cc_length         3
expr midi_cc_controller_idx 1
expr midi_cc_value_idx      2

; program change
expr midi_progch_length     2
expr midi_progch_prog_idx   1

; channel pressure
expr midi_chantouch_length       2
expr midi_chantouch_pressure_idx 1

; pitch bend, use midi_dword_(merge|split)
expr midi_pitchbend_length   3
expr midi_pitchbend_low_idx  1
expr midi_pitchbend_high_idx 2

; sysex messages are just the whole message until the final 0xF7
expr midi_sysex_data 1

; time code quarter frame, use midi_timeqframe_(merge|split)
expr midi_timeqframe_length 2
expr midi_timeqframe_data   1

; song position pointer, use midi_dword_(merge|split)
expr midi_songpos_length    3
expr midi_songpos_low_idx   1
expr midi_songpos_high_idx  2

; song select
expr midi_songsel_legnth    2
expr midi_songsel_song_idx  1

; tune request
expr midi_tunereq_length 1

; realtime messages, no arguments
expr midi_timeclock_length 1
expr midi_start_length     1
expr midi_continue_length  1
expr midi_stop_length      1
expr midi_sense_length     1
expr midi_reset_length     1

; reserved control change controllers
expr midi_cc_res_alloff    120
expr midi_cc_res_allreset  121
expr midi_cc_res_localcont 122
expr midi_cc_res_notesoff  123
expr midi_cc_res_omnioff   124
expr midi_cc_res_omnion    125
expr midi_cc_res_monomode  126
expr midi_cc_res_polymode  127

; macros
macro midi_dword_merge dest low high
    move dest high
    shl  dest 7
    or   dest low
endmacro midi_dword_merge

macro midi_dword_split destlow desthigh value
    move destlow  value
    and  destlow  0x7F
    move desthigh value
    and  desthigh 0x3F80
    shr  desthigh 7
endmacro midi_dword_split

macro midi_timeqframe_merge dest type value
    move dest mask
    shl  dest 4
    or   dest value
endmacro midi_timeqframe_merge

macro midi_timeqframe_split desttype destvalue value
    move destvalue value
    and  destvalue 0x0F
    move desttype  value
    and  desttype  0x70
    shr  desttype  4
endmacro midi_timeqframe_split

; crustymidi stuff
expr commit_pass 0
expr commit_new  1

; for older scripts, shouldn't be used
expr midi_velocity_idx   2
expr midi_cmd_time_clock 0xF8
