cmake_minimum_required(VERSION 3.21)
project(blairqt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent Sql LinguistTools)

qt_standard_project_setup()

add_executable(blairqt
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/SearchTab.h
    src/SearchTab.cpp
    src/LibraryTab.h
    src/LibraryTab.cpp
    src/GalleryView.h
    src/GalleryView.cpp
    src/Post.h
    src/Paging.h
    src/AppInfo.h
    src/HtmlEntities.h
    src/Blacklist.h
    src/Blacklist.cpp
    src/TagLineEdit.h
    src/TagLineEdit.cpp
    src/GalleryModel.h
    src/GalleryModel.cpp
    src/ThumbnailLoader.h
    src/ThumbnailLoader.cpp
    src/SourceManager.h
    src/SourceManager.cpp
    src/AddSourceDialog.h
    src/AddSourceDialog.cpp
    src/SourceDetector.h
    src/SourceDetector.cpp
    src/sources/ISource.h
    src/sources/DanbooruSource.h
    src/sources/DanbooruSource.cpp
    src/sources/GelbooruSource.h
    src/sources/GelbooruSource.cpp
    src/sources/MoebooruSource.h
    src/sources/MoebooruSource.cpp
    src/sources/E621Source.h
    src/sources/E621Source.cpp
    src/sources/PhilomenaSource.h
    src/sources/PhilomenaSource.cpp
    src/sources/SzurubooruSource.h
    src/sources/SzurubooruSource.cpp
    src/sources/ShimmieSource.h
    src/sources/ShimmieSource.cpp
    src/sources/GelbooruHtmlSource.h
    src/sources/GelbooruHtmlSource.cpp
    src/SiteAccess.h
    src/SiteAccess.cpp
    src/sources/PythonPluginSource.h
    src/sources/PythonPluginSource.cpp
    src/NetworkLog.h
    src/NetworkLog.cpp
    src/DownloadManager.h
    src/DownloadManager.cpp
    src/LibraryDatabase.h
    src/LibraryDatabase.cpp
    src/DetailsPanel.h
    src/DetailsPanel.cpp
    src/ImageViewerDialog.h
    src/ImageViewerDialog.cpp
    src/FolderAccessGuard.h
    src/FolderAccessGuard.cpp
    src/ThemeManager.h
    src/ThemeManager.cpp
    resources/resources.qrc
)

target_include_directories(blairqt PRIVATE src)

target_link_libraries(blairqt PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::Sql
)

# Install layout per platform. The files the program reads at run time
# (translations/, plugin_runner.py, tools/) go into one data folder, and
# findAppFile() in src/AppInfo.h looks in exactly these places:
#   Linux, FreeBSD  FHS: bin/blairqt, data in share/blairqt
#   Windows         portable: blairqt.exe with the data next to it
#   macOS           blairqt.app, data in blairqt.app/Contents/Resources
include(GNUInstallDirs)
if(WIN32)
    # A GUI program: no console window opens next to it.
    set_target_properties(blairqt PROPERTIES WIN32_EXECUTABLE ON)
    install(TARGETS blairqt RUNTIME DESTINATION .)
    set(BLAIRQT_INSTALL_DATA_DIR .)
elseif(APPLE)
    # The icon file (blairqt.icns) is made from blairqt.svg by the release
    # workflow and copied into Contents/Resources there.
    set_target_properties(blairqt PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "B.LAIR_QT"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.gitlab.re-v0.blairqt"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "blairqt.icns"
    )
    install(TARGETS blairqt BUNDLE DESTINATION .)
    set(BLAIRQT_INSTALL_DATA_DIR blairqt.app/Contents/Resources)
else()
    install(TARGETS blairqt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES resources/blairqt.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES resources/blairqt.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    set(BLAIRQT_INSTALL_DATA_DIR ${CMAKE_INSTALL_DATADIR}/blairqt)
    target_compile_definitions(blairqt PRIVATE BLAIRQT_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/blairqt")
endif()

# The Windows zip and the macOS app carry their own Qt, so they also carry
# the license texts: this program's (MPL-2.0), Qt's (LGPL-3.0, which builds
# on GPL-3.0) and a notice naming the Qt version and where its source is.
# The Linux and FreeBSD packages use the system's Qt and install none.
if(WIN32 OR APPLE)
    set(BLAIRQT_QT_VERSION "${Qt6_VERSION}")
    set(BLAIRQT_QT_MAJOR "${Qt6_VERSION_MAJOR}")
    set(BLAIRQT_QT_MINOR "${Qt6_VERSION_MINOR}")
    configure_file(packaging/licenses/NOTICE.txt.in "${CMAKE_CURRENT_BINARY_DIR}/licenses/NOTICE.txt" @ONLY)
    install(FILES LICENSE DESTINATION ${BLAIRQT_INSTALL_DATA_DIR}/licenses RENAME MPL-2.0.txt)
    install(FILES
        packaging/licenses/LGPL-3.0.txt
        packaging/licenses/GPL-3.0.txt
        "${CMAKE_CURRENT_BINARY_DIR}/licenses/NOTICE.txt"
        DESTINATION ${BLAIRQT_INSTALL_DATA_DIR}/licenses)
endif()

if(MSVC)
    # The sources are UTF-8 (the Polish tr() strings). Without /utf-8 MSVC
    # reads them in the local code page, the strings arrive garbled, and no
    # translation lookup ever matches.
    target_compile_options(blairqt PRIVATE /utf-8)
endif()

# Translations: .ts files are hand-maintained (translations/*.ts) and always
# compiled to .qm at build time via lrelease - lupdate is NOT run
# automatically (it would overwrite hand-written <translation> text with
# empty placeholders for any string it re-extracts), only via the manual
# "blairqt_lupdate" target below when someone wants to refresh the source
# strings after adding/changing tr() calls.
# Debian and Ubuntu keep the Qt 6 tools only in Qt's own bin dir
# (/usr/lib/qt6/bin), not on PATH; without the hint the .deb would ship
# with no translations.
set(BLAIRQT_QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
find_program(LRELEASE_EXECUTABLE NAMES lrelease6 lrelease-qt6 lrelease HINTS "${BLAIRQT_QT_BIN_DIR}")
find_program(LUPDATE_EXECUTABLE NAMES lupdate6 lupdate-qt6 lupdate HINTS "${BLAIRQT_QT_BIN_DIR}")

set(BLAIRQT_TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/blairqt_en.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/blairqt_de.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/blairqt_da.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/blairqt_ru.ts
)

target_compile_definitions(blairqt PRIVATE BLAIRQT_VERSION="${PROJECT_VERSION}")

# plugin_runner.py (the "python" source flavor's plugin bridge, see
# PythonPluginSource) is a plain script, not compiled - just copied next to
# the dev binary (mirroring how translations/*.qm land in build/translations/)
# and installed into the data folder.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/plugin_runner.py"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/plugin_runner.py"
        "${CMAKE_CURRENT_BINARY_DIR}/plugin_runner.py"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/plugin_runner.py"
    VERBATIM
)
add_custom_target(blairqt_plugin_runner ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/plugin_runner.py")
add_dependencies(blairqt blairqt_plugin_runner)
install(FILES resources/plugin_runner.py DESTINATION ${BLAIRQT_INSTALL_DATA_DIR})
install(PROGRAMS tools/html_scrape_bridge.py DESTINATION ${BLAIRQT_INSTALL_DATA_DIR}/tools)
# Example plugins: the "Add custom booru" file dialog opens here, and a
# source whose saved script path is gone falls back to the one here.
install(DIRECTORY examples/ DESTINATION ${BLAIRQT_INSTALL_DATA_DIR}/examples
    FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)

if(LRELEASE_EXECUTABLE)
    set(BLAIRQT_QM_FILES)
    set(BLAIRQT_QM_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/translations")
    foreach(ts_file ${BLAIRQT_TS_FILES})
        get_filename_component(ts_name ${ts_file} NAME_WE)
        set(qm_file "${BLAIRQT_QM_OUTPUT_DIR}/${ts_name}.qm")
        add_custom_command(
            OUTPUT ${qm_file}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${BLAIRQT_QM_OUTPUT_DIR}"
            COMMAND ${LRELEASE_EXECUTABLE} ${ts_file} -qm ${qm_file}
            DEPENDS ${ts_file}
            VERBATIM
        )
        list(APPEND BLAIRQT_QM_FILES ${qm_file})
    endforeach()
    add_custom_target(blairqt_translations ALL DEPENDS ${BLAIRQT_QM_FILES})
    add_dependencies(blairqt blairqt_translations)

    install(FILES ${BLAIRQT_QM_FILES} DESTINATION ${BLAIRQT_INSTALL_DATA_DIR}/translations)
else()
    message(WARNING "lrelease not found - blairqt will build without translations")
endif()

# Regression tests for the parts that can be checked without a server or a
# visible window: blacklist rules, tag input, response parsing, the database.
# include(CTest) gives BUILD_TESTING (ON by default), so a packager building
# with -DBUILD_TESTING=OFF does not pay for a test binary it will never run.
include(CTest)
if(BUILD_TESTING)
    add_executable(blairqt_tests
        tests/test_logic.cpp
        src/Blacklist.cpp
        src/TagLineEdit.cpp
        src/LibraryDatabase.cpp
        src/sources/GelbooruSource.cpp
        src/sources/DanbooruSource.cpp
        src/sources/MoebooruSource.cpp
        src/sources/E621Source.cpp
        src/sources/PhilomenaSource.cpp
        src/sources/SzurubooruSource.cpp
        src/sources/ShimmieSource.cpp
        src/sources/GelbooruHtmlSource.cpp
        src/SiteAccess.cpp
        src/sources/PythonPluginSource.cpp
        src/NetworkLog.cpp
        src/ThumbnailLoader.cpp
        src/ThemeManager.cpp
        src/DownloadManager.cpp
        src/FolderAccessGuard.cpp
        src/SourceManager.cpp
        src/SourceDetector.cpp
        resources/resources.qrc
    )
    target_include_directories(blairqt_tests PRIVATE src)
    if(MSVC)
        target_compile_options(blairqt_tests PRIVATE /utf-8)
    endif()
    target_link_libraries(blairqt_tests PRIVATE Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Concurrent)
    add_test(NAME logic COMMAND blairqt_tests)
    set_tests_properties(logic PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(LUPDATE_EXECUTABLE)
    add_custom_target(blairqt_lupdate
        COMMAND ${LUPDATE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src -ts ${BLAIRQT_TS_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Refreshing translations/*.ts from tr() calls in src/ (does not overwrite existing <translation> text)"
        VERBATIM
    )
endif()

# Debian/Ubuntu package: `cpack -G DEB` in the build dir, after building with
# -DCMAKE_INSTALL_PREFIX=/usr. Library dependencies come from dpkg-shlibdeps
# (package dpkg-dev); the SQLite driver is a Qt plugin, which shlibdeps
# cannot see, so it is listed by hand.
set(CPACK_PACKAGE_NAME "blairqt")
set(CPACK_PACKAGE_VENDOR "re-v0")
set(CPACK_PACKAGE_CONTACT "re-v0 <bird_of_hermes77@disroot.org>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Qt6 booru gallery client with a local, tag-indexed library")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://gitlab.com/re-v0/blairqt")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "python3")
include(CPack)
