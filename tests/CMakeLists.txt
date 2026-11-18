# Testes do ProcForge (ctest). Todos usam apps falsos (fakeapp), nunca
# processos reais do usuário. GUI roda com QT_QPA_PLATFORM=offscreen.
find_package(Qt6 REQUIRED COMPONENTS Test Network)

add_executable(procforge_fakeapp fake/fakeapp.cpp)
target_link_libraries(procforge_fakeapp PRIVATE Threads::Threads)

set(_pf_env "QT_QPA_PLATFORM=offscreen;LANGUAGE=pt_BR;QT_LOGGING_RULES=*.debug=false")

function(procforge_test name)
    add_executable(${name} ${ARGN} FakeApp.h)
    target_link_libraries(${name} PRIVATE procforge_gui Qt6::Test Qt6::Network)
    target_compile_definitions(${name} PRIVATE
        FAKEAPP_PATH="$<TARGET_FILE:procforge_fakeapp>"
        TEST_DISK_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(${name} procforge_fakeapp)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_pf_env}" TIMEOUT 120)
endfunction()

procforge_test(test_core    test_core.cpp)
procforge_test(test_actions test_actions.cpp)
procforge_test(test_gui     test_gui.cpp)

add_test(NAME test_memory_selftest COMMAND procforge_memtest)

find_program(PYTHON3 python3)
find_program(CURL curl)
if(PYTHON3 AND CURL)
    add_test(NAME test_updater
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_updater.sh ${CMAKE_SOURCE_DIR}/scripts/procforge-update)
    set_tests_properties(test_updater PROPERTIES TIMEOUT 120)
endif()

add_test(NAME test_cli_version COMMAND procforge --version)
set_tests_properties(test_cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^procforge ${PROJECT_VERSION}")
