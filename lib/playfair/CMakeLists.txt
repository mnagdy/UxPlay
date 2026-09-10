aux_source_directory(. playfair_src)
set(DIR_SRCS ${playfair_src})
include_directories(.)
add_library( playfair
        STATIC
        ${DIR_SRCS})

# modified_md5.c uses sin(); consumers of this static library need libm.
if(UNIX)
  target_link_libraries(playfair PUBLIC m)
endif()
