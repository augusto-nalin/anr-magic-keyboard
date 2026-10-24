# CCache improves build speed, make sure it's installed with apt install ccache
find_program(CCACHE ccache)
if (CCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE} CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE} CACHE STRING "" FORCE)
endif ()
