function(oos_add_sonivox target source_dir)
  add_library(${target} STATIC
    "${source_dir}/lib_src/eas_data.c"
    "${source_dir}/lib_src/eas_dlssynth.c"
    "${source_dir}/lib_src/eas_flog.c"
    "${source_dir}/lib_src/eas_imelody.c"
    "${source_dir}/lib_src/eas_imelodydata.c"
    "${source_dir}/lib_src/eas_math.c"
    "${source_dir}/lib_src/eas_mdls.c"
    "${source_dir}/lib_src/eas_midi.c"
    "${source_dir}/lib_src/eas_mididata.c"
    "${source_dir}/lib_src/eas_mixbuf.c"
    "${source_dir}/lib_src/eas_mixer.c"
    "${source_dir}/lib_src/eas_ota.c"
    "${source_dir}/lib_src/eas_otadata.c"
    "${source_dir}/lib_src/eas_pan.c"
    "${source_dir}/lib_src/eas_pcm.c"
    "${source_dir}/lib_src/eas_pcmdata.c"
    "${source_dir}/lib_src/eas_public.c"
    "${source_dir}/lib_src/eas_reverb.c"
    "${source_dir}/lib_src/eas_reverbdata.c"
    "${source_dir}/lib_src/eas_rtttl.c"
    "${source_dir}/lib_src/eas_rtttldata.c"
    "${source_dir}/lib_src/eas_smf.c"
    "${source_dir}/lib_src/eas_smfdata.c"
    "${source_dir}/lib_src/eas_voicemgt.c"
    "${source_dir}/lib_src/eas_wtengine.c"
    "${source_dir}/lib_src/eas_wtsynth.c"
    "${source_dir}/lib_src/eas_xmf.c"
    "${source_dir}/lib_src/eas_xmfdata.c"
    "${source_dir}/lib_src/wt_22khz.c"
    "${source_dir}/host_src/eas_config.c"
    "${source_dir}/host_src/eas_hostmm.c"
    "${source_dir}/host_src/eas_report.c")
  target_include_directories(${target} SYSTEM PUBLIC
    "${source_dir}/host_src"
    "${source_dir}/lib_src")
  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../compat/sonivox")
  target_compile_definitions(${target} PRIVATE
    UNIFIED_DEBUG_MESSAGES
    EAS_WT_SYNTH
    _IMELODY_PARSER
    _RTTTL_PARSER
    _OTA_PARSER
    _XMF_PARSER
    NUM_OUTPUT_CHANNELS=2
    _SAMPLE_RATE_22050
    MAX_SYNTH_VOICES=32
    _16_BIT_SAMPLES
    _FILTER_ENABLED
    DLS_SYNTHESIZER
    _REVERB_ENABLED)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-include>
    $<$<COMPILE_LANGUAGE:C>:stdbool.h>
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-implicit-fallthrough)
endfunction()
