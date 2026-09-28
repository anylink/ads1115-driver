cmake_minimum_required(VERSION 3.16)

# 工程就在仓库根，components/ 与本文件同级。
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ads1115_quad_channel)
