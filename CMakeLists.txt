cmake_minimum_required(VERSION 3.4.1)
 
project(live555)
 
set(LIVE555_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
 
link_directories(${LIBRARY_OUTPUT_PATH} ${OPENSSL_LIBRARIES})
 
macro(source_group_by_dir source_files)
    set(sgbd_cur_dir ${LIVE555_SRC_ROOT}/)
    foreach(sgbd_file ${${source_files}})
        string(REGEX REPLACE ${sgbd_cur_dir} "" sgbd_fpath ${sgbd_file})
        string(REGEX REPLACE "[^/\\\\]+$" "" sgbd_group_name ${sgbd_fpath})
        if(sgbd_group_name)
            string(REPLACE "/"   "\\\\"  sgbd_group_name_normal ${sgbd_group_name})
            source_group(${sgbd_group_name_normal} FILES ${sgbd_file})
        endif(sgbd_group_name)
    endforeach(sgbd_file)
endmacro(source_group_by_dir)
 
file(GLOB_RECURSE All_SOURCES ${LIVE555_SRC_ROOT}/BasicUsageEnvironment/*.cpp
                              ${LIVE555_SRC_ROOT}/BasicUsageEnvironment/*.c
                              ${LIVE555_SRC_ROOT}/liveMedia/*.cpp
                              ${LIVE555_SRC_ROOT}/liveMedia/*.c
                              ${LIVE555_SRC_ROOT}/groupsock/*.cpp
                              ${LIVE555_SRC_ROOT}/groupsock/*.c
                              ${LIVE555_SRC_ROOT}/UsageEnvironment/*.cpp
                              ${LIVE555_SRC_ROOT}/UsageEnvironment/*.c)
source_group_by_dir(All_SOURCES)
 
set(CMAKE_CXX_STANDARD 11)
add_library(live555 SHARED ${All_SOURCES})
 
target_include_directories(live555
        PUBLIC ${LIVE555_SRC_ROOT}/BasicUsageEnvironment/include
        PUBLIC ${LIVE555_SRC_ROOT}/liveMedia/include
        PUBLIC ${LIVE555_SRC_ROOT}/groupsock/include
        PUBLIC ${LIVE555_SRC_ROOT}/UsageEnvironment/include
        PRIVATE ${LIVE555_SRC_ROOT}/..)
 
target_compile_options(live555
        PRIVATE -O3
        PRIVATE -Wall
        PRIVATE -fPIC
        PRIVATE -fexceptions)
 
target_compile_definitions(live555
                PRIVATE SOCKLEN_T=socklen_t
                PRIVATE _LARGEFILE_SOURCE=1
                PRIVATE BSD=1
                PRIVATE ALLOW_RTSP_SERVER_PORT_REUSE
                PRIVATE ALLOW_SERVER_PORT_REUSE=1
                PRIVATE REUSE_FOR_TCP=1
                PRIVATE SO_REUSEPORT=1)
 
if (WIN32)
    target_compile_definitions(live555
        PRIVATE WINNT
        PRIVATE _CRT_SECURE_NO_WARNINGS
        PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS
        PRIVATE NO_GETIFADDRS
        PRIVATE LIVE555_LIB)
endif()
 
if (APPLE)
    target_compile_definitions(live555
    PUBLIC NEWLOCALE_NOT_USED
    PUBLIC NEED_XLOCALE_H)
endif()
 
# Open live555 debug log
#target_compile_definitions(live555 PRIVATE DEBUG)
 
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(live555
                    PRIVATE _FILE_OFFSET_BITS=64)
endif()
 
if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    target_compile_definitions(live555
                    PRIVATE ANDROID
                    PRIVATE __ANDROID_NDK__)
endif()

if (WIN32)
set(PROJECT_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../windows/Lib/x64/Release_Static)
target_link_libraries(live555 PRIVATE ${PROJECT_LIB_PATH}/libcrypto.lib ${PROJECT_LIB_PATH}/libssl.lib Crypt32 Wldap32 Ws2_32)
else()
if (APPLE)
set(PROJECT_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../mac/Lib)
target_link_libraries(live555 PRIVATE ${PROJECT_LIB_PATH}/libcrypto.a ${PROJECT_LIB_PATH}/libssl.a)
elseif (UNIX)
set(PROJECT_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../linux/Lib)
target_link_libraries(live555 PRIVATE ${PROJECT_LIB_PATH}/libssl_tutk.a ${PROJECT_LIB_PATH}/libcrypto_tutk.a)
endif()
endif()