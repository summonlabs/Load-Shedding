# Installs Load Shedding into a clean prefix, then configures, builds, and runs an
# independent out-of-tree consumer against the installed package with
# find_package. Invoked by CTest and by release verification.
#
# A nested CMake project needs a usable C++ toolchain in this process's
# environment. On Windows with MSVC that environment comes from vcvars64.bat, so
# this script locates it through vswhere (never a hard-coded path) and runs the
# nested commands through it. When no toolchain can be reached the check fails
# with the exact reason rather than reporting a pass it did not earn.

foreach(required LS_SOURCE_DIR LS_BINARY_DIR LS_PREFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "RunDownstreamCheck.cmake requires -D${required}=...")
  endif()
endforeach()

set(ls_vcvars "")
if(WIN32 AND DEFINED LS_VCVARS AND NOT LS_VCVARS STREQUAL "")
  set(ls_vcvars "${LS_VCVARS}")
elseif(WIN32)
  set(ls_program_files_x86 "$ENV{ProgramFiles\(x86\)}")
  set(ls_vswhere "${ls_program_files_x86}/Microsoft Visual Studio/Installer/vswhere.exe")
  if(EXISTS "${ls_vswhere}")
    execute_process(COMMAND "${ls_vswhere}" -latest -products * -requires
                            Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                            -property installationPath
                    OUTPUT_VARIABLE ls_vs_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT ls_vs_path STREQUAL "")
      set(ls_vcvars "${ls_vs_path}/VC/Auxiliary/Build/vcvars64.bat")
    endif()
  endif()
endif()

set(ls_run_serial 0)
function(ls_run description)
  math(EXPR ls_run_serial "${ls_run_serial} + 1")
  if(WIN32 AND NOT ls_vcvars STREQUAL "" AND EXISTS "${ls_vcvars}")
    set(ls_batch "${LS_BINARY_DIR}/ls-run-${ls_run_serial}.bat")
    set(ls_script "@echo off\r\ncall \"${ls_vcvars}\" >nul 2>&1\r\n")
    foreach(argument IN LISTS ARGN)
      set(ls_script "${ls_script}\"${argument}\" ")
    endforeach()
    set(ls_script "${ls_script}\r\nexit /b %ERRORLEVEL%\r\n")
    file(WRITE "${ls_batch}" "${ls_script}")
    execute_process(COMMAND cmd /c "${ls_batch}"
      RESULT_VARIABLE ls_result
      OUTPUT_VARIABLE ls_output
      ERROR_VARIABLE ls_output)
    file(REMOVE "${ls_batch}")
  else()
    execute_process(COMMAND ${ARGN}
      RESULT_VARIABLE ls_result
      OUTPUT_VARIABLE ls_output
      ERROR_VARIABLE ls_output)
  endif()

  if(NOT ls_result EQUAL 0)
    if(ls_output MATCHES "No CMAKE_CXX_COMPILER could be found" OR
       ls_output MATCHES "CMAKE_CXX_COMPILER not set")
      message(FATAL_ERROR
              "${description} could not run because no C++ compiler is reachable from this "
              "environment. Run the suite from a developer environment (for MSVC, after "
              "vcvars64.bat) to execute the installed-package check for real.\n${ls_output}")
    endif()
    if(ls_output MATCHES "Could not find a package configuration file provided by \"LoadShedding\"")
      message(FATAL_ERROR
              "${description} failed because no Load Shedding package was found under "
              "'${LS_PREFIX}'.\n${ls_output}")
    endif()
    message(FATAL_ERROR "${description} failed with exit ${ls_result}:\n${ls_output}")
  endif()
  set(ls_last_output "${ls_output}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${LS_PREFIX}")
file(MAKE_DIRECTORY "${LS_PREFIX}")

set(ls_install_command "${CMAKE_COMMAND}" --install "${LS_BINARY_DIR}" --prefix "${LS_PREFIX}")
if(DEFINED LS_CONFIG AND NOT LS_CONFIG STREQUAL "")
  list(APPEND ls_install_command --config "${LS_CONFIG}")
endif()
ls_run("install into ${LS_PREFIX}" ${ls_install_command})

set(ls_consumer_build "${LS_BINARY_DIR}/downstream-build")
file(REMOVE_RECURSE "${ls_consumer_build}")
set(ls_configure "${CMAKE_COMMAND}"
  -S "${LS_SOURCE_DIR}/downstream/consumer"
  -B "${ls_consumer_build}"
  "-DCMAKE_PREFIX_PATH=${LS_PREFIX}")
if(DEFINED LS_GENERATOR AND NOT LS_GENERATOR STREQUAL "")
  list(APPEND ls_configure -G "${LS_GENERATOR}")
endif()
if(DEFINED LS_CONFIG AND NOT LS_CONFIG STREQUAL "")
  list(APPEND ls_configure "-DCMAKE_BUILD_TYPE=${LS_CONFIG}")
endif()

ls_run("downstream configure" ${ls_configure})
ls_run("downstream build" "${CMAKE_COMMAND}" --build "${ls_consumer_build}")
ls_run("downstream run" "${CMAKE_COMMAND}" --build "${ls_consumer_build}" --target run_consumer)

message(STATUS "downstream consumer output:\n${ls_last_output}")
