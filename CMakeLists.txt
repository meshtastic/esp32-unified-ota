cmake_minimum_required(VERSION 3.16)

# Read the version file (Standard CMake)
file(STRINGS "version.txt" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MeshtasticOTA)

# Pass the version to the project
project(MeshtasticOTA VERSION "${PROJECT_VER}")