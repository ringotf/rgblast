# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
#set(PICO_SDK_PATH "C:/Users/jotap/Documents/Circuits/pico-sdk")

set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
  message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

project(RGBLAST_SMS C CXX ASM)

set(project_name_ntsc RGBLAST_SMS_NTSC)
set(project_name_pal RGBLAST_SMS_PAL )
set(project_name_pal60 RGBLAST_SMS_PAL60 )

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

set(COMMON_SOURCES 
    main.c 
)
    
set(COMMON_LIBRARIES
        hardware_adc
        hardware_dma
        hardware_pio
        pico_time 
        pico_multicore 
        libdvi
        pico_stdlib
)

add_executable(${project_name_ntsc} ${COMMON_SOURCES})
add_executable(${project_name_pal} ${COMMON_SOURCES})
add_executable(${project_name_pal60} ${COMMON_SOURCES})

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/libdvi)

pico_set_program_name(${project_name_ntsc} "RGBlast SMS NTSC")
pico_set_program_name(${project_name_pal} "RGBlast SMS PAL")
pico_set_program_name(${project_name_pal} "RGBlast SMS PAL60")
pico_set_program_version(${project_name_ntsc} "0.1")
pico_set_program_version(${project_name_pal} "0.1")
pico_set_program_version(${project_name_pal60} "0.1")

#pico_enable_stdio_uart(${project_name_ntsc} 0)
#pico_enable_stdio_uart(${project_name_pal} 0)
# pico_enable_stdio_usb(${PROJECT_NAME} 1)

pico_generate_pio_header(${project_name_ntsc} ${CMAKE_CURRENT_LIST_DIR}/vsync_detector.pio)
pico_generate_pio_header(${project_name_pal} ${CMAKE_CURRENT_LIST_DIR}/vsync_detector.pio)
pico_generate_pio_header(${project_name_pal60} ${CMAKE_CURRENT_LIST_DIR}/vsync_detector.pio)

pico_generate_pio_header(${project_name_ntsc} ${CMAKE_CURRENT_LIST_DIR}/pio_6bpp_color_read.pio)
pico_generate_pio_header(${project_name_pal} ${CMAKE_CURRENT_LIST_DIR}/pio_6bpp_color_read.pio)
pico_generate_pio_header(${project_name_pal60} ${CMAKE_CURRENT_LIST_DIR}/pio_6bpp_color_read.pio)


#pico_generate_pio_header(${project_name_pal} ${CMAKE_CURRENT_LIST_DIR}/flash_adc.pio)

pico_set_binary_type(${project_name_ntsc} copy_to_ram)
pico_set_binary_type(${project_name_pal} copy_to_ram)
pico_set_binary_type(${project_name_pal60} copy_to_ram)

# Add the standard include files to the build
target_include_directories(${project_name_ntsc} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
)

target_include_directories(${project_name_pal} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
)

target_include_directories(${project_name_pal60} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
)


# Add any user requested libraries
target_link_libraries(${project_name_ntsc} ${COMMON_LIBRARIES} )
target_link_libraries(${project_name_pal} ${COMMON_LIBRARIES} )
target_link_libraries(${project_name_pal60} ${COMMON_LIBRARIES} )

pico_add_extra_outputs(${project_name_ntsc})
pico_add_extra_outputs(${project_name_pal})
pico_add_extra_outputs(${project_name_pal60})

pico_enable_stdio_usb(${project_name_ntsc} 1)
pico_enable_stdio_usb(${project_name_pal} 1)
pico_enable_stdio_usb(${project_name_pal60} 1)

pico_enable_stdio_uart(${project_name_ntsc} 0)
pico_enable_stdio_uart(${project_name_pal} 0)
pico_enable_stdio_uart(${project_name_pal60} 0)


target_compile_definitions(${project_name_ntsc} PRIVATE VIDEO_MODE_NTSC=1)
target_compile_definitions(${project_name_pal} PRIVATE VIDEO_MODE_PAL=1)
target_compile_definitions(${project_name_pal60} PRIVATE VIDEO_MODE_PAL60=1)
