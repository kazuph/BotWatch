idf_component_register(SRCS "saanotts.c" "saanotts_stream.c" "saanotts_int8.c" "fft.c" "g2p.c" "saan_pcm.c" INCLUDE_DIRS ".")
target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1 SAAN_PIE=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -std=c99)
