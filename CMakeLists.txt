cmake_minimum_required(VERSION 3.20)
project(procforge VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets DBus)
# KF6 no Fedora vem dividido (sem umbrella KF6Config.cmake): procura por componente.
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)
find_package(KF6WidgetsAddons REQUIRED)
find_package(Threads REQUIRED)   # std::thread no ThreadController

### GUI sem privilégio -------------------------------------------------------
add_executable(procforge
    resources.qrc
    src/main.cpp
    src/MainWindow.cpp
    src/HelperClient.cpp
    src/core/ProcInfo.cpp
    src/core/Procfs.cpp
    src/core/MemoryIO.cpp
    src/core/MemoryScanner.cpp
    src/core/ThreadController.cpp
    src/core/KWinControl.cpp
    src/core/ThemeManager.cpp
    src/model/ProcessCollector.cpp
    src/model/ProcessModel.cpp
    src/model/ProcessFilterProxy.cpp
    src/actions/ProcessActions.cpp
    src/inject/FdInjector.cpp
    src/inject/LibraryInjector.cpp
    src/inject/RemoteMem.cpp
    src/core/ProcessIcons.cpp
    src/panels/DetailPanel.cpp
    src/panels/ScannerWindow.cpp
    src/panels/SystemTabs.cpp
    src/panels/ProcessProperties.cpp
)

target_include_directories(procforge PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(procforge PRIVATE
    Qt6::Widgets
    Qt6::DBus
    KF6::CoreAddons
    KF6::I18n
    KF6::WidgetsAddons
    ${CMAKE_DL_LIBS}
    Threads::Threads
)

install(TARGETS procforge RUNTIME DESTINATION bin)

### Helper privilegiado (D-Bus, sem Qt GUI, sem KF6) -------------------------
add_executable(procforged
    helper/main.cpp
    helper/Helper.cpp
    helper/Polkit.cpp
    helper/ProcConnector.cpp
    src/core/Procfs.cpp
    src/inject/LibraryInjector.cpp
    src/inject/RemoteMem.cpp
)

target_include_directories(procforged PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/helper)

target_link_libraries(procforged PRIVATE
    Qt6::Core
    Qt6::DBus
    ${CMAKE_DL_LIBS}
)

install(TARGETS procforged RUNTIME DESTINATION libexec)

### Arquivos de dados (para pacote/instalação em /usr) -----------------------
install(FILES data/org.procforge.helper.policy     DESTINATION share/polkit-1/actions)
install(FILES data/org.procforge.Helper1.conf      DESTINATION share/dbus-1/system.d)
install(FILES data/org.procforge.Helper1.service   DESTINATION share/dbus-1/system-services)
install(FILES data/procforged.service              DESTINATION lib/systemd/system)
install(FILES data/org.procforge.ProcForge.desktop DESTINATION share/applications)
# Ícone do app (raster PNG, tamanhos do tema hicolor)
foreach(_sz 16 22 24 32 48 64 128 256)
    install(FILES data/icons/procforge-${_sz}.png
            DESTINATION share/icons/hicolor/${_sz}x${_sz}/apps
            RENAME procforge.png)
endforeach()

# Tradução en_US (pt-BR é o idioma base = msgids)
find_program(MSGFMT msgfmt)
if(MSGFMT AND EXISTS ${CMAKE_SOURCE_DIR}/po/en.po)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/en.mo
        COMMAND ${MSGFMT} ${CMAKE_SOURCE_DIR}/po/en.po -o ${CMAKE_BINARY_DIR}/en.mo
        DEPENDS ${CMAKE_SOURCE_DIR}/po/en.po
        COMMENT "Compilando tradução en_US")
    add_custom_target(procforge_i18n ALL DEPENDS ${CMAKE_BINARY_DIR}/en.mo)
    install(FILES ${CMAKE_BINARY_DIR}/en.mo
            DESTINATION share/locale/en/LC_MESSAGES RENAME procforge.mo)
endif()

### Núcleo de memória em Rust (staticlib linkada no helper) ------------------
# Se o cargo existir, compila procforge-rmem e roteia Read/WriteMem por ele.
# Sem cargo, o helper usa o caminho C++ (process_vm_* direto). Fallback limpo.
find_program(CARGO cargo)
if(CARGO)
    set(RMEM_DIR ${CMAKE_SOURCE_DIR}/rust/procforge-rmem)
    set(RMEM_LIB ${RMEM_DIR}/target/release/libprocforge_rmem.a)
    add_custom_command(
        OUTPUT ${RMEM_LIB}
        COMMAND ${CARGO} build --release --quiet
        WORKING_DIRECTORY ${RMEM_DIR}
        DEPENDS ${RMEM_DIR}/src/lib.rs ${RMEM_DIR}/Cargo.toml
        COMMENT "Compilando núcleo de memória em Rust (procforge-rmem)")
    add_custom_target(procforge_rmem_build DEPENDS ${RMEM_LIB})
    add_dependencies(procforged procforge_rmem_build)
    target_link_libraries(procforged PRIVATE ${RMEM_LIB})
    target_compile_definitions(procforged PRIVATE PROCFORGE_HAVE_RUST=1)
    message(STATUS "procforge: núcleo de memória em Rust ATIVADO (${CARGO})")
else()
    message(STATUS "procforge: sem cargo — helper usa o núcleo de memória em C++")
endif()

### Self-test do núcleo de memória (scan/refine/escrita) -------------------
add_executable(procforge_memtest
    tests/scanner_selftest.cpp
    src/core/MemoryScanner.cpp
    src/core/MemoryIO.cpp
    src/core/Procfs.cpp
)
target_include_directories(procforge_memtest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(procforge_memtest PRIVATE Qt6::Core)

### CLI de teste da injeção de biblioteca ----------------------------------
add_executable(procforge_inject
    tests/inject_cli.cpp
    src/inject/LibraryInjector.cpp
    src/core/Procfs.cpp
)
target_include_directories(procforge_inject PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(procforge_inject PRIVATE Qt6::Core ${CMAKE_DL_LIBS})

### CLI de teste do suspend por-thread -------------------------------------
add_executable(procforge_tsuspend
    tests/tsuspend_cli.cpp
    src/core/ThreadController.cpp
)
target_include_directories(procforge_tsuspend PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(procforge_tsuspend PRIVATE Qt6::Core Threads::Threads)
