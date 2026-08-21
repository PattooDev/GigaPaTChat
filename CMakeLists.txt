cmake_minimum_required(VERSION 3.16)

project(GigaPaTChat VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(GNUTLS REQUIRED gnutls)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)

set(RTMP_LOCAL_ROOT
    "${CMAKE_SOURCE_DIR}/rtmpdump-source"
)

set(RTMP_LOCAL_LIBRARY
    "${RTMP_LOCAL_ROOT}/librtmp/librtmp.a"
)

if(NOT EXISTS "${RTMP_LOCAL_LIBRARY}")
    message(FATAL_ERROR
        "La bibliothèque RTMP locale est introuvable : ${RTMP_LOCAL_LIBRARY}"
    )
endif()

add_executable(gigapatchat
    src/main.cpp
)

target_include_directories(gigapatchat PRIVATE
    ${CURL_INCLUDE_DIRS}
    ${GNUTLS_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${RTMP_LOCAL_ROOT}
)

target_link_libraries(gigapatchat
    ${CURL_LIBRARIES}
    ${RTMP_LOCAL_LIBRARY}
    ${GNUTLS_LIBRARIES}
    hogweed
    nettle
    gmp
    ZLIB::ZLIB
    ${SDL2_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    OpenSSL::Crypto
)
