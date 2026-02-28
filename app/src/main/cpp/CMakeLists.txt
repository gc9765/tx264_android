cmake_minimum_required(VERSION 3.22.1)
project("myvideoapp")

set(CMAKE_CXX_STANDARD 11)

set(LIB_DIR ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})

add_library(avcodec STATIC IMPORTED)
set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libavcodec.a)

add_library(avformat STATIC IMPORTED)
set_target_properties(avformat PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libavformat.a)

add_library(avutil STATIC IMPORTED)
set_target_properties(avutil PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libavutil.a)

add_library(swscale STATIC IMPORTED)
set_target_properties(swscale PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libswscale.a)

add_library(swresample STATIC IMPORTED)
set_target_properties(swresample PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libswresample.a)

add_library(avfilter STATIC IMPORTED)
set_target_properties(avfilter PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libavfilter.a)

add_library(postproc STATIC IMPORTED)
set_target_properties(postproc PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libpostproc.a)

add_library(x264 STATIC IMPORTED)
set_target_properties(x264 PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libx264.a)

add_library(video-transcoder SHARED native-lib.cpp)

target_include_directories(video-transcoder PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(
    video-transcoder
    avfilter avformat avcodec swresample swscale postproc avutil x264
    z m dl log android
)
