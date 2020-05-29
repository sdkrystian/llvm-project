# Install script for directory: C:/Users/Krystian/src/llvm-project/llvm

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Users/Krystian/src/llvm-project/llvm/out/install/x64-Debug")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xllvm-headersx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE DIRECTORY FILES
    "C:/Users/Krystian/src/llvm-project/llvm/include/llvm"
    "C:/Users/Krystian/src/llvm-project/llvm/include/llvm-c"
    FILES_MATCHING REGEX "/[^/]*\\.def$" REGEX "/[^/]*\\.h$" REGEX "/[^/]*\\.td$" REGEX "/[^/]*\\.inc$" REGEX "/license\\.txt$" REGEX "/\\.svn$" EXCLUDE)
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xllvm-headersx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE DIRECTORY FILES
    "C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/include/llvm"
    "C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/include/llvm-c"
    FILES_MATCHING REGEX "/[^/]*\\.def$" REGEX "/[^/]*\\.h$" REGEX "/[^/]*\\.gen$" REGEX "/[^/]*\\.inc$" REGEX "/cmakefiles$" EXCLUDE REGEX "/config\\.h$" EXCLUDE REGEX "/\\.svn$" EXCLUDE)
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xcmake-exportsx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/llvm" TYPE FILE FILES "C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/lib/cmake/llvm/LLVMConfigExtensions.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/lib/Demangle/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/lib/Support/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/lib/TableGen/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/TableGen/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/include/llvm/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/lib/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/FileCheck/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/PerfectShuffle/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/count/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/not/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/yaml-bench/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/projects/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/tools/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/runtimes/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/examples/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/lit/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/test/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/unittests/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/unittest/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/KillTheDoctor/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/docs/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/cmake/modules/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/llvm-lit/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/benchmark/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/benchmarks/cmake_install.cmake")
  include("C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/llvm-locstats/cmake_install.cmake")

endif()

if(CMAKE_INSTALL_COMPONENT)
  set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
file(WRITE "C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
