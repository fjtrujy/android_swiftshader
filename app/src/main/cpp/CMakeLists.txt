cmake_minimum_required(VERSION 3.22.1)
project(swsrepro C)

add_library(swsrepro SHARED
    jni_glue.c
    repro.c
    padding.c
)

target_link_libraries(swsrepro
    android
    log
    EGL
    GLESv3
    jnigraphics
)

target_compile_options(swsrepro PRIVATE -Wall -Wextra -Werror)
