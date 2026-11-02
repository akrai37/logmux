function(logmux_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE logmux_core logmux_warnings)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

logmux_test(poller_test)
logmux_test(collector_test)
set_tests_properties(collector_test PROPERTIES TIMEOUT 120)
