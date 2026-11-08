cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Installed Arduino images validate this field before accepting an OTA image.
project(arduino-lib-builder)

# G3 reproduces the light-sleep tick assertion fixed by ESP-IDF 5a5ef9fb08.
# Backport its two-tick tolerance to the pinned 4.4.7 SDK, in the build tree.
# Larger overruns still reach the original FreeRTOS assertion.
idf_component_get_property(pm_library esp_pm COMPONENT_LIB)
set(pm_original "$ENV{IDF_PATH}/components/esp_pm/pm_impl.c")
file(SHA256 "${pm_original}" pm_sha256)
if(NOT pm_sha256 STREQUAL "93c91ac03b4d309c7edf1e401e64cc96d1d5ba877af36c9d422b51ccc8eced7f")
    message(FATAL_ERROR "Review the G3 PM backport for this ESP-IDF source")
endif()
file(READ "${pm_original}" pm_source)
string(REPLACE "            if (slept_ticks > 0) {" [=[
            if (slept_ticks > xExpectedIdleTime &&
                slept_ticks - xExpectedIdleTime <= 2) {
                slept_ticks = xExpectedIdleTime;
            }
            if (slept_ticks > 0) {]=] pm_source "${pm_source}")
set(pm_patched "${CMAKE_CURRENT_BINARY_DIR}/pm_impl_tick_overflow.c")
file(WRITE "${pm_patched}" "${pm_source}")
get_target_property(pm_sources ${pm_library} SOURCES)
list(REMOVE_ITEM pm_sources "${pm_original}")
set_property(TARGET ${pm_library} PROPERTY SOURCES "${pm_sources};${pm_patched}")
