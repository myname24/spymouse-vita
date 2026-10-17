cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(spymouse_vita C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Spy Mouse HD")
set(VITA_TITLEID  "SPYM00001")
set(VITA_VERSION  "01.00")

# kubridge user stubs (third_party/kubridge, from TheOfficialFloW/kubridge v0.3.1); the plugin kubridge.skprx must be installed on the Vita
set(KUB_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kubridge CACHE PATH "kubridge stub library folder")
set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/src/gen)

# ATTRIBUTE2=12 -> 365 MB of main memory (loader + game heap + vitaGL pool)
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

add_executable(spymouse src/main.c src/elfload.c src/imports.c src/glwrap.c src/jni.c src/java.c src/audio.c ${GEN}/vfp_patch.c ${GEN}/gen_syms.c ${GEN}/textpad.c)
target_include_directories(spymouse PRIVATE src)
target_compile_options(spymouse PRIVATE -O2 -Wall -Wno-unused-function -Wno-unused-parameter -fno-strict-aliasing -fsigned-char)
target_link_directories(spymouse PRIVATE ${KUB_LIB})
target_link_libraries(spymouse
  vitaGL vitashark SceShaccCgExt mathneon taihen_stub kubridge_stub
  SceShaccCg_stub SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub SceAppMgr_stub SceAppUtil_stub
  SceCommonDialog_stub SceSysmodule_stub SceKernelDmacMgr_stub SceLibKernel_stub ScePower_stub SceAudio_stub
  pthread m c stdc++)

vita_create_self(spymouse.self spymouse UNSAFE)
vita_create_vpk(spymouse.vpk ${VITA_TITLEID} spymouse.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE livearea/sce_sys/icon0.png sce_sys/icon0.png
  FILE livearea/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE livearea/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE livearea/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
