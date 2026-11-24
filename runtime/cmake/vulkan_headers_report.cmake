# Names the Vulkan headers a configure builds against, in one STATUS line a GPU test record can quote:
#   -- Vulkan headers: <include dir> (VK_HEADER_VERSION <n>), <where they come from>
# The headers decide what the GPU test tools can ask the driver for, so two records of the same GPU and driver differ
# when their headers do: rebuilt against conda-forge's Vulkan 1.3 headers instead of the pin, run_gpu_tests.py on a
# Radeon 780M skipped ps_texoff, ps_texoff_forms and ps_texoff_implicit (225/248 passed instead of 228/248).
# Kept in its own file so tests/test_cmake_vulkan_headers_pin.py can run it with `cmake -P`.

# include_dirs: the INTERFACE_INCLUDE_DIRECTORIES of Vulkan::Headers. Generator expressions such as
# $<BUILD_INTERFACE:...> (FetchContent's target) are unwrapped. origin: PIN (the hash-checked download of
# LEONIDA_VULKAN_HEADERS_PIN), SOURCE_DIR (FETCHCONTENT_SOURCE_DIR_VULKANHEADERS) or FOUND (anything else).
function(leonida_vulkan_headers_report include_dirs origin)
  set(dir "")
  set(header "")
  foreach(d IN LISTS include_dirs)
    string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" d "${d}")
    if(NOT d OR d MATCHES "^\\$<")
      continue()
    endif()
    foreach(name vulkan_core.h vulkan.h)
      if(EXISTS "${d}/vulkan/${name}")
        set(dir "${d}")
        set(header "${d}/vulkan/${name}")
        break()
      endif()
    endforeach()
    if(dir)
      break()
    endif()
  endforeach()
  set(version "unknown")
  if(header)
    file(STRINGS "${header}" line REGEX "^#define VK_HEADER_VERSION [0-9]+" LIMIT_COUNT 1)
    if(line MATCHES "VK_HEADER_VERSION ([0-9]+)")
      set(version "${CMAKE_MATCH_1}")
    endif()
  else()
    set(dir "${include_dirs}")
  endif()
  if(origin STREQUAL "PIN")
    set(what "the pin: Khronos Vulkan-Headers ${LEONIDA_VULKAN_HEADERS_PIN}, hash-checked")
  elseif(origin STREQUAL "SOURCE_DIR")
    set(what "from FETCHCONTENT_SOURCE_DIR_VULKANHEADERS: a local copy used in place of the pin, not checked "
             "against it")
  else()
    set(what "found on this machine, not the pin (-DLEONIDA_VULKAN_HEADERS_FETCH=ON builds against the pinned "
             "Khronos Vulkan-Headers)")
  endif()
  string(CONCAT what ${what})
  message(STATUS "Vulkan headers: ${dir} (VK_HEADER_VERSION ${version}), ${what}")
endfunction()
