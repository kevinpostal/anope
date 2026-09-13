# Unit tests over the dependency-free headers. They link nothing from
# Anope so that a test failure is always a logic failure.
function(anope_unit_test NAME)
  add_executable(${NAME} ${ARGN})
  target_include_directories(${NAME} PRIVATE ${Anope_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(${NAME} PROPERTIES FOLDER "Tests" UNITY_BUILD OFF)
  add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

anope_unit_test(test_bridgeserv_render bridgeserv/test_render.cpp)
