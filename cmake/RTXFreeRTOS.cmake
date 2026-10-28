# Create a source-tree FreeRTOS library against an application-configured kernel target.
# Propagate the RTOS mutex ABI to every consumer instead of mixing it with the hosted core.
# Add the FreeRTOS backend to an application that already configures its kernel.
# Keep it separate from the hosted RTX ABI: RTX_FREERTOS changes RuntimeMutex.
function(rtx_add_freertos_library target kernel_target)
 if(NOT TARGET "${kernel_target}")
  message(FATAL_ERROR "rtx_add_freertos_library requires an existing configured FreeRTOS kernel target")
 endif()
 if(TARGET "${target}")
  message(FATAL_ERROR "RTX FreeRTOS target already exists: ${target}")
 endif()
 get_filename_component(rtx_source_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
 add_library(${target} STATIC
  "${rtx_source_root}/src/scheduler.cpp"
  "${rtx_source_root}/src/launch.cpp"
  "${rtx_source_root}/src/freertos.cpp")
 target_include_directories(${target} PUBLIC "${rtx_source_root}/include")
 target_compile_definitions(${target} PUBLIC RTX_FREERTOS)
 target_compile_features(${target} PUBLIC cxx_std_20)
 target_compile_options(${target} PRIVATE
  $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Werror>)
 target_link_libraries(${target} PUBLIC "${kernel_target}")
endfunction()
