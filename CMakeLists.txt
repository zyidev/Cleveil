# ============================================================================
#  Cleveil —— Windows 程序卸载与残留清理工具
#  技术栈：C++20 + Qt 6 Widgets（MSVC 2022 工具链）
#
#  构建方式（见 build.ps1）：
#     cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
#           -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
#     cmake --build build
# ============================================================================
cmake_minimum_required(VERSION 3.21)

project(Cleveil VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# AUTORCC：让 .qrc 里的图标自动编译进 exe（Qt 标准工程默认只开 MOC/UIC）
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
qt_standard_project_setup()

qt_add_executable(Cleveil WIN32
    # ---- 程序入口 ----
    src/main.cpp

    # ---- 核心逻辑层（无 UI 依赖，可单独复用）----
    src/core/appinfo.h
    src/core/winutil.h          src/core/winutil.cpp
    src/core/iconprovider.h     src/core/iconprovider.cpp
    src/core/registryscanner.h  src/core/registryscanner.cpp
    src/core/uninstaller.h      src/core/uninstaller.cpp
    src/core/leftovers.h        src/core/leftovers.cpp
    src/core/remover.h          src/core/remover.cpp
    src/core/exporter.h         src/core/exporter.cpp
    src/core/installmonitor.h   src/core/installmonitor.cpp   # 实时安装监控
    src/core/signature.h        src/core/signature.cpp        # 数字签名校验

    # ---- 界面层 ----
    src/ui/appmodel.h           src/ui/appmodel.cpp
    src/ui/mainwindow.h         src/ui/mainwindow.cpp
    src/ui/uninstalldialog.h    src/ui/uninstalldialog.cpp
    src/ui/leftoverdialog.h     src/ui/leftoverdialog.cpp
    src/ui/settingsdialog.h     src/ui/settingsdialog.cpp
    src/ui/monitordialog.h      src/ui/monitordialog.cpp      # 安装监控窗口
    src/ui/batchwizard.h        src/ui/batchwizard.cpp        # 批量卸载向导

    # ---- 资源：图标 + 版本信息 ----
    resources/app.rc
    resources/resources.qrc
)

# resources 目录同时作为 rc.exe 的 /I 搜索路径，否则 app.rc 里的 "app.ico" 找不到
target_include_directories(Cleveil PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/resources)

# Windows 系统库：注册表、Shell、进程、版本信息、签名校验
target_link_libraries(Cleveil PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent
    advapi32    # 注册表 API / 服务控制 / RegNotifyChangeKeyValue
    shell32     # 回收站、图标
    ole32       # 初始化 COM（SHFileOperation 需要）
    user32      # 窗口与消息
    psapi       # 进程枚举
    version     # 读取 exe 版本信息（备用）
    shlwapi     # 路径处理
    comctl32    # 通知提示条任务栏集成
    crypt32     # 证书/签名数据解析（CryptQueryObject）
    wintrust    # WinVerifyTrust 验签
)

if (MSVC)
    # /utf-8：源码里有大量中文字面量，必须让 MSVC 按 UTF-8 解析，否则会乱码
    target_compile_options(Cleveil PRIVATE /utf-8 /W3 /permissive-)
    target_link_options(Cleveil PRIVATE /SUBSYSTEM:WINDOWS)
    # 内嵌资源里的中文同样按 UTF-8 交给 rc.exe
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /nologo")
endif()

set_target_properties(Cleveil PROPERTIES
    VERSION ${PROJECT_VERSION}
    WIN32_EXECUTABLE TRUE
)

# 运行时的 Qt 依赖（windeployqt 生成可分发的目录，见 build.ps1）
