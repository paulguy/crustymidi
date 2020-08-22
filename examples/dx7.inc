;based on https://homepages.abdn.ac.uk/d.j.benson/pages/dx7/sysex-format.txt
include midi.inc

expr dx7_pc_sysex_length 7
expr dx7_pc_sysex_type   midi_cmd_sysex
expr dx7_pc_sysex_last   midi_cmd_sysexend

; constants
expr dx7_pc_id_val      0x67 ;Yamaha
expr dx7_pc_ss_val      1    ;parameter change?
expr dx7_pc_ch_val      0    ;channel number 1

expr dx7_pc_id_idx      "midi_sysex_data + 0"
expr dx7_pc_ss_idx      "midi_sysex_data + 1"
expr dx7_pc_ss_shift    4
expr dx7_pc_ch_idx      "midi_sysex_data + 1"
expr dx7_pc_pg_idx      "midi_sysex_data + 2"
expr dx7_pc_pg_shift    2
expr dx7_pc_param1_idx  "midi_sysex_data + 2"
expr dx7_pc_param2_idx  "midi_sysex_data + 3"
expr dx7_pc_data_idx    "midi_sysex_data + 4"
expr dx7_pc_last_idx    "dx7_pc_sysex_length - 1"

expr dx7_pc_ssch_idx    dx7_pc_ss_idx
expr dx7_pc_ssch_val    "(dx7_pc_ss_val << dx7_pc_ss_shift) | dx7_pc_ch_val"

macro _DX7_MAKE_PACKET
;some convoluted bit packing stuff.  PARAM becomes mutated.
macro DX7_MAKE_PACKET BUF PG PARAM DATA
    move BUF:midi_cmd_idx       dx7_pc_sysex_type
    move BUF:dx7_pc_id_idx      dx7_pc_id_val
    move BUF:dx7_pc_ssch_idx    dx7_pc_ssch_val
    move BUF:dx7_pc_param2_idx  PARAM
    move BUF:dx7_pc_last_idx    PARAM
    shr  BUF:dx7_pc_last_idx    8
    move BUF:dx7_pc_pg_idx      PG
    shl  BUF:dx7_pc_pg_idx      dx7_pc_pg_shift
    or   BUF:dx7_pc_param1_idx  BUF:dx7_pc_last_idx
    move BUF:dx7_pc_data_idx    DATA
    move BUF:dx7_pc_last_idx    dx7_pc_sysex_last
endmacro DX7_MAKE_PACKET
endmacro _DX7_MAKE_PACKET
_DX7_MAKE_PACKET

; parameter group
expr dx7_pc_pg_voice    0    ;voice
expr dx7_pc_pg_func     2    ;function

static _DX7_FLOATS floats "0.0 0.0 0.0"

; ugly crap so _DX7_ROUND is available by the time the DX7_SCALE_*
; macros are being used, ugh.
macro ___DX7_ROUND
macro __DX7_ROUND
; RESULT should be int otherwise it won't work
macro _DX7_ROUND    ROUND_RESULT ROUND_VAL
; isolate the floating point component of VAL by copying it to a
; scratch float value then also to a scratch integer value to trim
; off the floating value, then take the difference which would be
; stored as the floating point component.  Scale it to make it 0 to
; approaching 2, then add, coercing it back to an integer.
    move _DX7_FLOATS:0 ROUND_VAL
    move ROUND_RESULT  ROUND_VAL
    sub  _DX7_FLOATS:0 ROUND_RESULT
    mul  _DX7_FLOATS:0 2
    add  ROUND_RESULT  _DX7_FLOATS:0
endmacro _DX7_ROUND
endmacro __DX7_ROUND
__DX7_ROUND
endmacro ___DX7_ROUND
___DX7_ROUND

macro _DX7_MAKE_SCALE   NAME INRANGE OUTRANGE
    macro DX7_SCALE_NAME   SCALE_VAL
        move       _DX7_FLOATS:0 SCALE_VAL
        move       _DX7_FLOATS:1 INRANGE
        move       _DX7_FLOATS:2 OUTRANGE
        mul        _DX7_FLOATS:0 _DX7_FLOATS:2
        div        _DX7_FLOATS:0 _DX7_FLOATS:1
        _DX7_ROUND SCALE_VAL     _DX7_FLOATS:0
    endmacro DX7_SCALE_NAME
endmacro _DX7_MAKE_SCALE

expr dx7_param_default_range    99
_DX7_MAKE_SCALE DEFAULT 127 dx7_param_default_range

; voice parameters
macro _DX7_MAKE_OP OPER START
    expr dx7_voice_opOPER_eg_rate1                "START + 0"
    expr dx7_voice_opOPER_eg_rate2                "START + 1"
    expr dx7_voice_opOPER_eg_rate3                "START + 2"
    expr dx7_voice_opOPER_eg_rate4                "START + 3"
    expr dx7_voice_opOPER_eg_level1               "START + 4"
    expr dx7_voice_opOPER_eg_level2               "START + 5"
    expr dx7_voice_opOPER_eg_level3               "START + 6"
    expr dx7_voice_opOPER_eg_level4               "START + 7"
    expr dx7_voice_opOPER_kbd_lev_scl_brk_pt      "START + 8"
    expr dx7_voice_opOPER_kbd_lev_scl_lft_depth   "START + 9"
    expr dx7_voice_opOPER_kbd_lev_scl_rht_depth   "START + 10"
    expr dx7_voice_opOPER_kbd_lev_scl_lft_curve   "START + 11"
    expr dx7_voice_opOPER_kbd_lev_scl_rht_curve   "START + 12"
    expr dx7_voice_opOPER_kbd_rate_scaling        "START + 13"
    expr dx7_voice_opOPER_amp_mod_sensitivity     "START + 14"
    expr dx7_voice_opOPER_key_vel_sensitivity     "START + 15"
    expr dx7_voice_opOPER_operator_output_level   "START + 16"
    expr dx7_voice_opOPER_osc_mode                "START + 17"
    expr dx7_voice_opOPER_osc_freq_coarse         "START + 18"
    expr dx7_voice_opOPER_osc_freq_fine           "START + 19"
    expr dx7_voice_opOPER_osc_detune              "START + 20"
endmacro _DX7_MAKE_OP
; may look like a typo but the original document listed it this way
expr dx7_voice_op_kbd_lev_scl_curve_neg_lin     0
expr dx7_voice_op_kbd_lev_scl_curve_neg_exp     1
expr dx7_voice_op_kbd_lev_scl_curve_pos_exp     2
expr dx7_voice_op_kbd_lev_scl_curve_pos_lin     3

expr dx7_voice_op_osc_mode_ratio    0
expr dx7_voice_op_osc_mode_fixed    1

expr dx7_voice_op_kbd_lev_scl_curve_range   3
expr dx7_voice_op_kbd_rate_scaling_range    7
expr dx7_voice_op_amp_mod_sensitivity_range 3
expr dx7_voice_op_key_vel_sensitivity_range 7
expr dx7_voice_op_osc_mode_range            1
expr dx7_voice_op_osc_freq_coarse_range     31
expr dx7_voice_op_osc_detune_range          14

_DX7_MAKE_SCALE OP_KBD_LEV_SCL_CURVE   127 dx7_voice_op_kbd_lev_scl_curve_range
_DX7_MAKE_SCALE OP_KBD_RATE_SCALING    127 dx7_voice_op_kbd_rate_scaling_range
_DX7_MAKE_SCALE OP_AMP_MOD_SENSITIVITY 127 dx7_voice_op_amp_mod_sensitivity_range
_DX7_MAKE_SCALE OP_KEY_VEL_SENSITIVITY 127 dx7_voice_op_key_vel_sentivitity_range
_DX7_MAKE_SCALE OP_OSC_MODE            127 dx7_voice_op_osc_mode_range
_DX7_MAKE_SCALE OP_OSC_FREQ_COARSE     127 dx7_voice_op_osc_freq_coarse_range
_DX7_MAKE_SCALE OP_OSC_DETUNE          127 dx7_voice_op_osc_detune_range

_DX7_MAKE_OP 6   0
_DX7_MAKE_OP 5  21
_DX7_MAKE_OP 4  42
_DX7_MAKE_OP 3  63
_DX7_MAKE_OP 2  84
_DX7_MAKE_OP 1 105

expr dx7_voice_pitch_eg_rate1                   126
expr dx7_voice_pitch_eg_rate2                   127
expr dx7_voice_pitch_eg_rate3                   128
expr dx7_voice_pitch_eg_rate4                   129
expr dx7_voice_pitch_eg_level1                  130
expr dx7_voice_pitch_eg_level2                  131
expr dx7_voice_pitch_eg_level3                  132
expr dx7_voice_pitch_eg_level4                  133
expr dx7_voice_algorithm_no                     134
expr dx7_voice_feedback                         135
expr dx7_voice_oscillator_sync                  136
expr dx7_voice_lfo_speed                        137
expr dx7_voice_lfo_delay                        138
expr dx7_voice_lfo_pitch_mod_depth              139
expr dx7_voice_lfo_amp_mod_depth                140
expr dx7_voice_lfo_sync                         141
expr dx7_voice_lfo_waveform                     142
expr dx7_voice_pitch_mod_sensitivity            143
expr dx7_voice_transpose                        144
expr dx7_voice_name_char1                       145
expr dx7_voice_name_char2                       146
expr dx7_voice_name_char3                       147
expr dx7_voice_name_char4                       148
expr dx7_voice_name_char5                       149
expr dx7_voice_name_char6                       150
expr dx7_voice_name_char7                       151
expr dx7_voice_name_char8                       152
expr dx7_voice_name_char9                       153
expr dx7_voice_name_char10                      154
expr dx7_voice_operator_onoff                   155

expr dx7_voice_lfo_waveform_tr  0
expr dx7_voice_lfo_waveform_sd  1
expr dx7_voice_lfo_waveform_su  2
expr dx7_voice_lfo_waveform_sq  3
expr dx7_voice_lfo_waveform_si  4
expr dx7_voice_lfo_waveform_sh  5
 
expr dx7_voice_operator_onoff_op1   0x20
expr dx7_voice_operator_onoff_op2   0x10
expr dx7_voice_operator_onoff_op3   0x08
expr dx7_voice_operator_onoff_op4   0x04
expr dx7_voice_operator_onoff_op5   0x02
expr dx7_voice_operator_onoff_op6   0x01

expr dx7_voice_algorithm_no_range           31
expr dx7_voice_feedback_range               7
expr dx7_voice_oscillator_sync_range        1
expr dx7_voice_lfo_sync_range               1
expr dx7_voice_lfo_waveform_range           5
expr dx7_voice_pitch_mod_sensitivity_range  7
expr dx7_voice_transpose_range              48
expr dx7_voice_operator_onoff_range         63

_DX7_MAKE_SCALE ALGORITHM_NO            127 dx7_voice_algorithm_no_range
_DX7_MAKE_SCALE FEEDBACK                127 dx7_voice_feedback_range
_DX7_MAKE_SCALE OSCILLATOR_SYNC         127 dx7_voice_oscillator_sync_range
_DX7_MAKE_SCALE LFO_SYNC                127 dx7_voice_lfo_sync_range
_DX7_MAKE_SCALE LFO_WAVEFORM            127 dx7_voice_lfo_waveform_range
_DX7_MAKE_SCALE PITCH_MOD_SENSITIVITY   127 dx7_voice_pitch_mod_sensitivity_range
_DX7_MAKE_SCALE TRANSPOSE               127 dx7_voice_transpose_range
_DX7_MAKE_SCALE OPERATOR_ONOFF          127 dx7_voice_operator_onoff_range

expr dx7_func_mono_mode             64
expr dx7_func_pitch_bend_range      65
expr dx7_func_pitch_bend_step       66
expr dx7_func_portamento_mode       67
expr dx7_func_portamento_gliss      68
expr dx7_func_portamento_time       69
expr dx7_func_mod_wheel_range       70
expr dx7_func_mod_wheel_assign      71
expr dx7_func_foot_control_range    72
expr dx7_func_foot_control_assign   73
expr dx7_func_breath_cont_range     74
expr dx7_func_breath_cont_assign    75
expr dx7_func_aftertouch_range      76
expr dx7_func_aftertouch_assign     77
