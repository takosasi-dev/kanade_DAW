cmake_minimum_required(VERSION 3.22)
project(ScoreSmith VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "")
endif()

# --- JUCE -------------------------------------------------------------------
# Set SCORESMITH_JUCE_PATH to a local JUCE checkout to build offline.
option(SCORESMITH_ASIO_SDK_PATH "Path to Steinberg ASIO SDK (Windows, optional)" "")

if(SCORESMITH_JUCE_PATH)
    add_subdirectory(${SCORESMITH_JUCE_PATH} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# --- Target -----------------------------------------------------------------
juce_add_gui_app(ScoreSmith
    PRODUCT_NAME       "KANADE DAW"
    COMPANY_NAME       "ScoreSmith"
    BUNDLE_ID          "dev.scoresmith.app"
    VERSION            ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "ScoreSmith needs audio input for recording and transcription."
    DOCUMENT_EXTENSIONS "ssproj"
    NEEDS_STORE_KIT    FALSE
    ICON_BIG           "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon/icon.png"
    ICON_SMALL         "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon/icon.png")

file(GLOB_RECURSE SS_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")
target_sources(ScoreSmith PRIVATE ${SS_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${SS_SOURCES})

target_include_directories(ScoreSmith PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

# MSVC's default execution charset is the system ANSI codepage (CP932 on
# Japanese-locale build machines), which silently re-encodes non-ASCII
# narrow string literals in UTF-8 source files - corrupting anything wrapped
# in juce::CharPointer_UTF8(...). /utf-8 forces both source and execution
# charset to UTF-8; no-op on non-MSVC compilers.
target_compile_options(ScoreSmith PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

juce_add_binary_data(ScoreSmithData SOURCES
    Resources/lang/ja.txt
    Resources/lang/en.txt)

target_compile_definitions(ScoreSmith PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_LV2=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ScoreSmith,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ScoreSmith,JUCE_VERSION>"
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

# ASIO is only compiled in when the (non-redistributable) Steinberg SDK is supplied.
if(WIN32 AND SCORESMITH_ASIO_SDK_PATH)
    target_compile_definitions(ScoreSmith PRIVATE JUCE_ASIO=1)
    target_include_directories(ScoreSmith PRIVATE "${SCORESMITH_ASIO_SDK_PATH}/common")
    message(STATUS "ScoreSmith: ASIO enabled (${SCORESMITH_ASIO_SDK_PATH})")
else()
    message(STATUS "ScoreSmith: ASIO disabled - WASAPI exclusive mode will be used on Windows")
endif()

target_link_libraries(ScoreSmith PRIVATE
    ScoreSmithData
    juce::juce_audio_utils
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)



