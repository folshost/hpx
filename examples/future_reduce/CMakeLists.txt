# Copyright (c) 2014 John Biddiscombe
#
# SPDX-License-Identifier: BSL-1.0
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

set(example_programs
   )

if(HPX_HAVE_CXX11_STD_RANDOM)
  set(example_programs
      ${example_programs}
      rnd_future_reduce)
  set(rnd_future_reduce_FLAGS DEPENDENCIES iostreams_component)
  set(rnd_future_reduce_PARAMETERS THREADS_PER_LOCALITY 4)
endif()

foreach(example_program ${example_programs})
  set(sources
      ${example_program}.cpp)

  source_group("Source Files" FILES ${sources})

  # add example executable
  add_hpx_executable(${example_program}
                     SOURCES ${sources}
                     ${${example_program}_FLAGS}
                     FOLDER "Examples/FutureReduce")

  add_hpx_example_target_dependencies("future_reduce" ${example_program})

  if(HPX_WITH_TESTS AND HPX_WITH_TESTS_EXAMPLES)
    add_hpx_example_test("future_reduce" ${example_program} ${${example_program}_PARAMETERS})
  endif()
endforeach()
