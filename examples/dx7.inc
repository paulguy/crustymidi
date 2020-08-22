;based on https://homepages.abdn.ac.uk/d.j.benson/pages/dx7/sysex-format.txt
include midi.inc

expr dx7_pc_sysex_length 7
expr dx7_pc_sysex_type   midi_cmd_sysex
expr dx7_pc_sysex_last   midi_cmd_sysexend

; constants
expr dx7_pc_id_val      0x67 ;Yamaha
expr dx7_pc_ss_val      1    ;parameter change?
expr dx7_pc_ch_val      0    ;channel number 1

expr dx7_pc_id_idx          "midi_sysex_data + 0"
expr dx7_pc_ssch_idx        "midi_sysex_data + 1"
expr dx7_pc_ss_shift        4
expr dx7_pc_pgparam1_idx    "midi_sysex_data + 2"
expr dx7_pc_pg_shift        2
expr dx7_pc_param2_idx      "midi_sysex_data + 3"
expr dx7_pc_data_idx        "midi_sysex_data + 4"
expr dx7_pc_last_idx        "dx7_pc_sysex_length - 1"

macro DX7_MAKE_PACKET BUF CHAN PG PARAM DATA
    move BUF:midi_cmd_idx           dx7_pc_sysex_type
    move BUF:dx7_pc_id_idx          dx7_pc_id_val
    move BUF:dx7_pc_ssch_idx        dx7_pc_ss_val
    shl  BUF:dx7_pc_ssch_idx        dx7_pc_ss_shift
    or   BUF:dx7_pc_ssch_idx        CHAN
    move BUF:dx7_pc_pgparam1_idx    PARAM
    move BUF:dx7_pc_last_idx        PARAM
    shr  BUF:dx7_pc_last_idx        8
    move BUF:dx7_pc_pgparam1_idx    PG
    shl  BUF:dx7_pc_pgparam1_idx    dx7_pc_pg_shift
    or   BUF:dx7_pc_param2_idx      BUF:dx7_pc_last_idx
    move BUF:dx7_pc_data_idx        DATA
    move BUF:dx7_pc_last_idx        dx7_pc_sysex_last
endmacro DX7_MAKE_PACKET

; parameter group
expr dx7_pc_pg_voice    0    ;voice
expr dx7_pc_pg_func     2    ;function

; voice parameters
; OP6 - 0
expr dx7_voice_opOP6_eg_rate1                0
expr dx7_voice_opOP6_eg_rate2                1
expr dx7_voice_opOP6_eg_rate3                2
expr dx7_voice_opOP6_eg_rate4                3
expr dx7_voice_opOP6_eg_level1               4
expr dx7_voice_opOP6_eg_level2               5
expr dx7_voice_opOP6_eg_level3               6
expr dx7_voice_opOP6_eg_level4               7
expr dx7_voice_opOP6_kbd_lev_scl_brk_pt      8
expr dx7_voice_opOP6_kbd_lev_scl_lft_depth   9
expr dx7_voice_opOP6_kbd_lev_scl_rht_depth   10
expr dx7_voice_opOP6_kbd_lev_scl_lft_curve   11
expr dx7_voice_opOP6_kbd_lev_scl_rht_curve   12
expr dx7_voice_opOP6_kbd_rate_scaling        13
expr dx7_voice_opOP6_amp_mod_sensitivity     14
expr dx7_voice_opOP6_key_vel_sensitivity     15
expr dx7_voice_opOP6_operator_output_level   16
expr dx7_voice_opOP6_osc_mode                17
expr dx7_voice_opOP6_osc_freq_coarse         18
expr dx7_voice_opOP6_osc_freq_fine           19
expr dx7_voice_opOP6_osc_detune              20
; OP5 - 21
expr dx7_voice_opOP5_eg_rate1                21
expr dx7_voice_opOP5_eg_rate2                22
expr dx7_voice_opOP5_eg_rate3                23
expr dx7_voice_opOP5_eg_rate4                24
expr dx7_voice_opOP5_eg_level1               25
expr dx7_voice_opOP5_eg_level2               26
expr dx7_voice_opOP5_eg_level3               27
expr dx7_voice_opOP5_eg_level4               28
expr dx7_voice_opOP5_kbd_lev_scl_brk_pt      29
expr dx7_voice_opOP5_kbd_lev_scl_lft_depth   30
expr dx7_voice_opOP5_kbd_lev_scl_rht_depth   31
expr dx7_voice_opOP5_kbd_lev_scl_lft_curve   32
expr dx7_voice_opOP5_kbd_lev_scl_rht_curve   33
expr dx7_voice_opOP5_kbd_rate_scaling        34
expr dx7_voice_opOP5_amp_mod_sensitivity     35
expr dx7_voice_opOP5_key_vel_sensitivity     36
expr dx7_voice_opOP5_operator_output_level   37
expr dx7_voice_opOP5_osc_mode                38
expr dx7_voice_opOP5_osc_freq_coarse         39
expr dx7_voice_opOP5_osc_freq_fine           40
expr dx7_voice_opOP5_osc_detune              41
; OP4 - 42
expr dx7_voice_opOP4_eg_rate1                42
expr dx7_voice_opOP4_eg_rate2                43
expr dx7_voice_opOP4_eg_rate3                44
expr dx7_voice_opOP4_eg_rate4                45
expr dx7_voice_opOP4_eg_level1               46
expr dx7_voice_opOP4_eg_level2               47
expr dx7_voice_opOP4_eg_level3               48
expr dx7_voice_opOP4_eg_level4               49
expr dx7_voice_opOP4_kbd_lev_scl_brk_pt      50
expr dx7_voice_opOP4_kbd_lev_scl_lft_depth   51
expr dx7_voice_opOP4_kbd_lev_scl_rht_depth   52
expr dx7_voice_opOP4_kbd_lev_scl_lft_curve   53
expr dx7_voice_opOP4_kbd_lev_scl_rht_curve   54
expr dx7_voice_opOP4_kbd_rate_scaling        55
expr dx7_voice_opOP4_amp_mod_sensitivity     56
expr dx7_voice_opOP4_key_vel_sensitivity     57
expr dx7_voice_opOP4_operator_output_level   58
expr dx7_voice_opOP4_osc_mode                59
expr dx7_voice_opOP4_osc_freq_coarse         60
expr dx7_voice_opOP4_osc_freq_fine           61
expr dx7_voice_opOP4_osc_detune              62
; OP3 - 63
expr dx7_voice_opOP3_eg_rate1                63
expr dx7_voice_opOP3_eg_rate2                64
expr dx7_voice_opOP3_eg_rate3                65
expr dx7_voice_opOP3_eg_rate4                66
expr dx7_voice_opOP3_eg_level1               67
expr dx7_voice_opOP3_eg_level2               68
expr dx7_voice_opOP3_eg_level3               69
expr dx7_voice_opOP3_eg_level4               70
expr dx7_voice_opOP3_kbd_lev_scl_brk_pt      71
expr dx7_voice_opOP3_kbd_lev_scl_lft_depth   72
expr dx7_voice_opOP3_kbd_lev_scl_rht_depth   73
expr dx7_voice_opOP3_kbd_lev_scl_lft_curve   74
expr dx7_voice_opOP3_kbd_lev_scl_rht_curve   75
expr dx7_voice_opOP3_kbd_rate_scaling        76
expr dx7_voice_opOP3_amp_mod_sensitivity     77
expr dx7_voice_opOP3_key_vel_sensitivity     78
expr dx7_voice_opOP3_operator_output_level   79
expr dx7_voice_opOP3_osc_mode                80
expr dx7_voice_opOP3_osc_freq_coarse         81
expr dx7_voice_opOP3_osc_freq_fine           82
expr dx7_voice_opOP3_osc_detune              83
; OP2 - 84
expr dx7_voice_opOP2_eg_rate1                84
expr dx7_voice_opOP2_eg_rate2                85
expr dx7_voice_opOP2_eg_rate3                86
expr dx7_voice_opOP2_eg_rate4                87
expr dx7_voice_opOP2_eg_level1               88
expr dx7_voice_opOP2_eg_level2               89
expr dx7_voice_opOP2_eg_level3               90
expr dx7_voice_opOP2_eg_level4               91
expr dx7_voice_opOP2_kbd_lev_scl_brk_pt      92
expr dx7_voice_opOP2_kbd_lev_scl_lft_depth   93
expr dx7_voice_opOP2_kbd_lev_scl_rht_depth   94
expr dx7_voice_opOP2_kbd_lev_scl_lft_curve   95
expr dx7_voice_opOP2_kbd_lev_scl_rht_curve   96
expr dx7_voice_opOP2_kbd_rate_scaling        97
expr dx7_voice_opOP2_amp_mod_sensitivity     98
expr dx7_voice_opOP2_key_vel_sensitivity     99
expr dx7_voice_opOP2_operator_output_level   100
expr dx7_voice_opOP2_osc_mode                101
expr dx7_voice_opOP2_osc_freq_coarse         102
expr dx7_voice_opOP2_osc_freq_fine           103
expr dx7_voice_opOP2_osc_detune              104
; OP1 - 105
expr dx7_voice_opOP1_eg_rate1                105
expr dx7_voice_opOP1_eg_rate2                106
expr dx7_voice_opOP1_eg_rate3                107
expr dx7_voice_opOP1_eg_rate4                108
expr dx7_voice_opOP1_eg_level1               109
expr dx7_voice_opOP1_eg_level2               110
expr dx7_voice_opOP1_eg_level3               111
expr dx7_voice_opOP1_eg_level4               112
expr dx7_voice_opOP1_kbd_lev_scl_brk_pt      113
expr dx7_voice_opOP1_kbd_lev_scl_lft_depth   114
expr dx7_voice_opOP1_kbd_lev_scl_rht_depth   115
expr dx7_voice_opOP1_kbd_lev_scl_lft_curve   116
expr dx7_voice_opOP1_kbd_lev_scl_rht_curve   117
expr dx7_voice_opOP1_kbd_rate_scaling        118
expr dx7_voice_opOP1_amp_mod_sensitivity     119
expr dx7_voice_opOP1_key_vel_sensitivity     120
expr dx7_voice_opOP1_operator_output_level   121
expr dx7_voice_opOP1_osc_mode                122
expr dx7_voice_opOP1_osc_freq_coarse         123
expr dx7_voice_opOP1_osc_freq_fine           124
expr dx7_voice_opOP1_osc_detune              125

; may look like a typo but the original document listed it this way
expr dx7_voice_op_kbd_lev_scl_curve_neg_lin     0
expr dx7_voice_op_kbd_lev_scl_curve_neg_exp     1
expr dx7_voice_op_kbd_lev_scl_curve_pos_exp     2
expr dx7_voice_op_kbd_lev_scl_curve_pos_lin     3

expr dx7_voice_op_osc_mode_ratio    0
expr dx7_voice_op_osc_mode_fixed    1

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

; function parameters
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

static _DX7_SCALETABLE ints "\
    99 99 99 99 99 99 99 99\
    99 99 99  3  3  7  3  7\
    99  1 31 99 14\
    99 99 99 99 99 99 99 99\
    99 99 99  3  3  7  3  7\
    99  1 31 99 14\
    99 99 99 99 99 99 99 99\
    99 99 99  3  3  7  3  7\
    99  1 31 99 14\
    99 99 99 99 99 99 99 99\
    99 99 99  3  3  7  3  7\
    99  1 31 99 14\
    99 99 99 99 99 99 99 99\
    99 99 99  3  3  7  3  7\
    99  1 31 99 14\
    99 99 99 99 99 99 99 99\
    99 99 99  3  3  7  3  7\
    99  1 31 99 14\
    99 99 99 99 99 99 99 99\
    31  7  1 99 99 99 99  1\
     5  7 48\
    127 127 127 127 127 127 127 127 127 127\
    63"

static _DX7_FLOATS floats "0.0 0.0 0.0"
macro DX7_SCALE PARAM VALUE UNIQUE
    cmp   _DX7_SCALETABLE:PARAM 127
    jumpz noscale_UNIQUE
    move _DX7_FLOATS:0 VALUE
    move _DX7_FLOATS:1 127
    move _DX7_FLOATS:2 _DX7_SCALETABLE:PARAM
    mul  _DX7_FLOATS:0 _DX7_FLOATS:2
    div  _DX7_FLOATS:0 _DX7_FLOATS:1

; crappy rounding algorithm!
; isolate the floating point component of VAL by copying it to a
; scratch float value then also to a scratch integer value to trim
; off the floating value, then take the difference which would be
; stored as the floating point component.  Scale it to make it 0 to
; approaching 2, then add, coercing it back to an integer.
    move VALUE         _DX7_FLOATS:0
    sub  _DX7_FLOATS:0 VALUE
    mul  _DX7_FLOATS:0 2
    add  VALUE         _DX7_FLOATS:0
label noscale_UNIQUE
endmacro DX7_SCALE
