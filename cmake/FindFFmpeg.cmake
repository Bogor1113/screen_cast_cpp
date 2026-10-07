# cmake/FindFFmpeg.cmake —— 查找 FFmpeg 开发库(avcodec/avutil/swscale)
# 优先级: -DFFMPEG_ROOT=<dir> > 环境变量 FFMPEG_ROOT > third_party/ffmpeg

if(NOT DEFINED FFMPEG_ROOT)
    if(DEFINED ENV{FFMPEG_ROOT})
        set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
    else()
        set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg")
    endif()
endif()

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH)

set(FFMPEG_LIBRARIES "")
foreach(comp avcodec avutil swscale)
    find_library(FFMPEG_${comp}_LIB ${comp} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${comp}_LIB})
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg DEFAULT_MSG
    FFMPEG_INCLUDE_DIR FFMPEG_avcodec_LIB FFMPEG_avutil_LIB FFMPEG_swscale_LIB)

set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIR})
