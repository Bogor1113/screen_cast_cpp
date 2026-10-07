cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 OLD)
endif()
project(screen_cast_cpp VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt6 (GUI layer only: teacher_gui / client_gui; core/client libs stay Qt-free)
set(QT6_ROOT_DIR "" CACHE PATH "Path to a Qt 6 install root (contains bin/, lib/, mkspecs/)")
if(QT6_ROOT_DIR)
    list(APPEND CMAKE_PREFIX_PATH "${QT6_ROOT_DIR}")
endif()
find_package(Qt6 QUIET COMPONENTS Widgets Network)

if(MSVC)
    add_compile_options(/utf-8 /W3)
else()
    add_compile_options(-Wall -Wextra)
endif()

if(WIN32)
    # NOMINMAX: Qt and Windows headers define min/max macros that clash with std::min/std::max
    # WIN32_LEAN_AND_MEAN: windows.h must not auto-include winsock.h -- Qt headers (e.g. QNetworkInterface)
    # pull in windows.h first, and our net/udp_socket.h explicitly includes winsock2.h; without this the
    # two Winsock headers collide with C2375 redefinition errors when compiling Qt GUI sources
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(FFmpeg REQUIRED)

# ===== 核心库 =====
add_library(core_lib STATIC
    core/streamer.cpp
    ui/preview.cpp
    capture/dxgi_capture.cpp
    encode/ffmpeg_encoder.cpp
    net/udp_socket.cpp
)
target_include_directories(core_lib PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(core_lib PUBLIC ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(core_lib PUBLIC ws2_32 d3d11 dxgi ${FFMPEG_LIBRARIES})

# ===== 教师端 =====
# Qt6 is REQUIRED for the GUI targets (migration from Win32 to Qt). Original Win32
# implementations are archived in ui/legacy/ (not compiled). CLI/diag tools stay Qt-free.
if(NOT Qt6_FOUND)
    message(FATAL_ERROR "Qt6 (Widgets) not found. Install Qt 6.x and pass -DQT6_ROOT_DIR=<qt root> or set CMAKE_PREFIX_PATH.")
endif()

qt_add_executable(teacher_gui WIN32 teacher_gui.cpp msi/teacher_gui.rc ui/teacher_window.cpp ui/preview_widget.cpp ui/qss_dark.h)   # msi/teacher.ico 嵌入 exe
set_target_properties(teacher_gui PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
target_link_libraries(teacher_gui PRIVATE core_lib Qt6::Widgets Qt6::Network)
target_include_directories(teacher_gui PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(teacher_cli teacher_cli.cpp)
target_link_libraries(teacher_cli PRIVATE core_lib)

# ===== 客户端 =====
add_library(client_lib STATIC
    net/client_receiver.cpp
    net/udp_socket.cpp
    decode/ffmpeg_decoder.cpp
    render/d3d11_renderer.cpp
)
target_include_directories(client_lib PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(client_lib PUBLIC ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(client_lib PUBLIC ws2_32 d3d11 dxgi d3dcompiler ${FFMPEG_LIBRARIES})

add_executable(client_console client_console.cpp)
target_link_libraries(client_console PRIVATE client_lib)

qt_add_executable(client_gui WIN32 client_gui.cpp msi/client_gui.rc ui/client_window.cpp ui/video_widget.cpp ui/connect_dialog.cpp ui/qss_dark.h)   # WIN32: GUI 子系统 + msi/student.ico 嵌入 exe
set_target_properties(client_gui PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
target_link_libraries(client_gui PRIVATE client_lib Qt6::Widgets)
target_include_directories(client_gui PRIVATE ${CMAKE_SOURCE_DIR})

# ===== 安装程序(纯 Win32 GUI: 单选 学生端/教师端, 免 NSIS/Inno 外部依赖) =====
add_executable(installer WIN32 installer.cpp msi/installer.rc)
target_link_libraries(installer PRIVATE shell32 ole32 user32 gdi32)

# ===== 单文件安装包外壳(自解压启动器: 解压 payload + 自动运行 installer.exe) =====
add_executable(setup_pack WIN32 setup_pack.cpp)

# ===== 诊断工具 =====
add_executable(diag_dirty diag_dirty.cpp capture/dxgi_capture.cpp net/udp_socket.cpp)
target_include_directories(diag_dirty PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(diag_dirty PRIVATE ws2_32 d3d11 dxgi)

# 解码诊断工具(排查组播/单播解码"失败"模式)
add_executable(diag_decode diag_decode.cpp net/client_receiver.cpp net/udp_socket.cpp decode/ffmpeg_decoder.cpp)
target_include_directories(diag_decode PRIVATE ${CMAKE_SOURCE_DIR} ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(diag_decode PRIVATE ws2_32 ${FFMPEG_LIBRARIES})

# FEC 验证工具(随机丢包验证奇偶恢复)
add_executable(diag_fec diag_fec.cpp net/udp_socket.cpp decode/ffmpeg_decoder.cpp)
target_include_directories(diag_fec PRIVATE ${CMAKE_SOURCE_DIR} ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(diag_fec PRIVATE ws2_32 ${FFMPEG_LIBRARIES})

# FFmpeg DLL 复制到每个 exe 旁
set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")
if(WIN32 AND EXISTS "${FFMPEG_BIN_DIR}")
    file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_BIN_DIR}/avcodec-*.dll" "${FFMPEG_BIN_DIR}/avutil-*.dll" "${FFMPEG_BIN_DIR}/swscale-*.dll" "${FFMPEG_BIN_DIR}/swresample-*.dll")
    foreach(tgt teacher_gui teacher_cli client_console client_gui diag_dirty diag_decode diag_fec)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS}
            $<TARGET_FILE_DIR:${tgt}>)
    endforeach()
endif()
