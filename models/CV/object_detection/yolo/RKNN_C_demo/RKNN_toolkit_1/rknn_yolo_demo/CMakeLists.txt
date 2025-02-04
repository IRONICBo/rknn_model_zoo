cmake_minimum_required(VERSION 3.4.1)

project(rknn_yolo_demo_linux)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread")

# for linux
if (CMAKE_C_COMPILER MATCHES "aarch64")
  set(LIB_ARCH lib64)
else()
  set(LIB_ARCH lib)
endif()

# for android
if (${TARGET_SOC} STREQUAL "RK3399PRO")
  set(LIB_ARCH lib64)
endif()

# rga
set(RGA_DIR ${MZ_ROOT}/libs/rklibs/rknpu/rknn/rknn_api/examples/3rdparty/rga)
include_directories(${RGA_DIR}/include)

# drm
set(DRM_DIR ${MZ_ROOT}/libs/common/drm)
include_directories(${DRM_DIR}/include)
include_directories(${DRM_DIR}/include/libdrm)

#stb
set(STB_DIR ${MZ_ROOT}/libs/common/)
include_directories(${STB_DIR})

# rknn api
if (${TARGET_SOC} STREQUAL "RK3399PRO")
  set(RKNN_API_PATH ${MZ_ROOT}/libs/rklibs/RK3399Pro_npu/rknn-api/librknn_api)
  include_directories(${RKNN_API_PATH}/include)
  set(RKNN_API_LIB ${RKNN_API_PATH}/${CMAKE_SYSTEM_NAME}/${LIB_ARCH}/librknn_api.so)
else()
  set(RKNN_API_PATH ${MZ_ROOT}/libs/rklibs/rknpu/rknn/rknn_api/librknn_api)
  include_directories(${RKNN_API_PATH}/include)
  set(RKNN_API_LIB ${RKNN_API_PATH}/${LIB_ARCH}/librknn_api.so)
endif()

# libjpeg
set(LIBJPEG_DIR ${MZ_ROOT}/libs/platform/${TARGET_SOC}/libjpeg)
include_directories(${LIBJPEG_DIR}/include)
set(LIBJPEG_LIB ${LIBJPEG_DIR}/lib/${CMAKE_SYSTEM_NAME}/libjpeg.a)

# libpng
set(LIBPNG_DIR ${MZ_ROOT}/libs/platform/${TARGET_SOC}/libpng)
include_directories(${LIBPNG_DIR}/include)
set(LIBPNG_LIB ${LIBPNG_DIR}/lib/${CMAKE_SYSTEM_NAME}/libpng.a)

# zlib(used by libpng)
set(ZLIB_DIR ${MZ_ROOT}/libs/platform/${TARGET_SOC}/zlib)
include_directories(${ZLIB_DIR}/include)
set(ZLIB_LIB ${ZLIB_DIR}/lib/${CMAKE_SYSTEM_NAME}/libz.a)

include_directories(${CMAKE_SOURCE_DIR}/include)

set(CMAKE_INSTALL_RPATH "lib")

add_executable(rknn_yolo_demo
        src/drm_func.c
        src/rga_func.c
        src/yolo.cc
        src/main.cc
        )

target_link_libraries(rknn_yolo_demo
	${RKNN_API_LIB}
  ${LIBJPEG_LIB}
  ${LIBPNG_LIB}
  ${ZLIB_LIB}
  dl
)

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${TARGET_SOC}/${CMAKE_SYSTEM_NAME}/rknn_yolo_demo)
install(TARGETS rknn_yolo_demo DESTINATION ./)
install(DIRECTORY model DESTINATION ./)
install(PROGRAMS ${RKNN_API_LIB} DESTINATION lib)
