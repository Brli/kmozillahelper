cmake_minimum_required(VERSION 3.16.0)

project( kmozillahelper )

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
set(QT_MIN_VERSION "6.5.0")
set(KF_MIN_VERSION "5.240.0")
set(QT_MAJOR_VERSION "6")

find_package(ECM ${KF_MIN_VERSION} CONFIG REQUIRED)

set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings)
include(FeatureSummary)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Qml)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS Notifications KIO WindowSystem I18n)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEBUG_KDE "ON")
endif()

add_executable(kmozillahelper main.cpp main.h)

target_link_libraries(kmozillahelper KF6::I18n KF6::KIOWidgets KF6::Notifications KF6::WindowSystem)

install(TARGETS kmozillahelper DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/mozilla/)
install(FILES kmozillahelper.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})
