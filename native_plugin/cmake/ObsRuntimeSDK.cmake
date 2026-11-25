include_guard(GLOBAL)

set(STERNENFLUG_SDK_ROOT "" CACHE PATH "Prepared SDK root from scripts/Build.ps1")
if(NOT STERNENFLUG_SDK_ROOT)
  message(FATAL_ERROR "Pass -DSTERNENFLUG_SDK_ROOT=<cache>/sdk, or use scripts/Build.ps1.")
endif()
file(TO_CMAKE_PATH "${STERNENFLUG_SDK_ROOT}" STERNENFLUG_SDK_ROOT)
set(_obs_source "${STERNENFLUG_SDK_ROOT}/obs-source")
foreach(_required "${_obs_source}/libobs/obs.h" "${_obs_source}/frontend/api/obs-frontend-api.h"
                  "${STERNENFLUG_SDK_ROOT}/include/obsconfig.h"
                  "${STERNENFLUG_SDK_ROOT}/lib/obs.lib"
                  "${STERNENFLUG_SDK_ROOT}/lib/obs-frontend-api.lib")
  if(NOT EXISTS "${_required}")
    message(FATAL_ERROR "Incomplete OBS runtime SDK: ${_required}. Run scripts/Build.ps1.")
  endif()
endforeach()
add_library(OBS::libobs SHARED IMPORTED GLOBAL)
set_target_properties(OBS::libobs PROPERTIES
  IMPORTED_IMPLIB "${STERNENFLUG_SDK_ROOT}/lib/obs.lib"
  IMPORTED_LOCATION "${STERNENFLUG_SDK_ROOT}/runtime/obs.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${_obs_source}/libobs;${STERNENFLUG_SDK_ROOT}/include")
add_library(OBS::frontend SHARED IMPORTED GLOBAL)
set_target_properties(OBS::frontend PROPERTIES
  IMPORTED_IMPLIB "${STERNENFLUG_SDK_ROOT}/lib/obs-frontend-api.lib"
  IMPORTED_LOCATION "${STERNENFLUG_SDK_ROOT}/runtime/obs-frontend-api.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${_obs_source}/frontend/api")
