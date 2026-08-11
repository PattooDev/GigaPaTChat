cmake_minimum_required(VERSION 3.16)

project(GigaPaTChat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(RTMP REQUIRED librtmp)

add_executable(gigapatchat
    src/main.cpp
)

target_include_directories(gigapatchat PRIVATE
    ${CURL_INCLUDE_DIRS}
    ${RTMP_INCLUDE_DIRS}
)

target_link_libraries(gigapatchat
    ${CURL_LIBRARIES}
    ${RTMP_LIBRARIES}
    OpenSSL::Crypto
)
