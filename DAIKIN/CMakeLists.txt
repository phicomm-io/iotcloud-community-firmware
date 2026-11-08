cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Installed Arduino images validate this field before accepting an OTA image.
project(arduino-lib-builder)
