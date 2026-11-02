function(logmux_test name)
  add_executable(${name} ${name}.c)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/tools)
  target_link_libraries(${name} PRIVATE logmux_core logmux_warnings)
  target_compile_definitions(${name} PRIVATE LOGMUX_CHILD="$<TARGET_FILE:logmux-child>")
  add_dependencies(${name} logmux-child)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

logmux_test(poller_test)
logmux_test(collector_test)
logmux_test(verify_test)
logmux_test(deadlock_test)

# The command-line tools end to end.
add_test(NAME cli_logmux COMMAND logmux --stats -c "a=echo one; echo two >&2" -c "b=printf tail")
add_test(NAME cli_bench COMMAND logmux-bench --idle 16 --mb 8)
