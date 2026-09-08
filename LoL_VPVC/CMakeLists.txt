cmake_minimum_required(VERSION 3.20)
project(lol_proximity LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# So os modulos que realmente usamos: sem highgui/imgcodecs/videoio o
# executavel deixa de arrastar uma dezena de DLLs para a pasta Release.
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

# Opus: usa o do vcpkg quando existir; senao compila da fonte.
find_package(Opus CONFIG QUIET)
if(TARGET Opus::opus)
    set(OPUS_TARGET Opus::opus)
else()
    include(FetchContent)
    FetchContent_Declare(opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG v1.4)
    FetchContent_MakeAvailable(opus)
    set(OPUS_TARGET opus)
endif()

# RNNoise (supressao de ruido por rede neural). O port do vcpkg nao suporta
# Windows, entao a biblioteca vive junto do projeto, como o miniaudio.
add_library(rnnoise STATIC
    rnnoise/celt_lpc.c
    rnnoise/denoise.c
    rnnoise/kiss_fft.c
    rnnoise/pitch.c
    rnnoise/rnn.c
    rnnoise/rnn_data.c
    rnnoise/rnn_reader.c
)
target_include_directories(rnnoise PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rnnoise")
target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(rnnoise PRIVATE /wd4244 /wd4305 /wd4996 $<$<CONFIG:Release>:/O2>)
endif()

# SpeexDSP (AGC, VAD e cancelamento de eco)
find_path(SPEEXDSP_INCLUDE_DIR speex/speex_preprocess.h REQUIRED)
find_library(SPEEXDSP_LIBRARY NAMES speexdsp REQUIRED)

add_executable(lol_proximity
    main.cpp
    minimap_reader.cpp
    AudioManager.cpp
    NetworkManager.cpp
    app.rc
)

target_link_libraries(lol_proximity PRIVATE
    Ws2_32
    Iphlpapi
    Ole32
    opengl32
    opencv_core
    opencv_imgproc
    LibDataChannel::LibDataChannel
    ${OPUS_TARGET}
    ${SPEEXDSP_LIBRARY}
    rnnoise
    imgui::imgui
    glfw
)

target_include_directories(lol_proximity PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${SPEEXDSP_INCLUDE_DIR}
)

target_compile_definitions(lol_proximity PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    # miniaudio: so precisamos de captura/reproducao de PCM.
    MA_NO_DECODING
    MA_NO_ENCODING
    MA_NO_GENERATION
    MA_NO_RESOURCE_MANAGER
    MA_NO_NODE_GRAPH
    MA_NO_ENGINE
)

if(MSVC)
    target_compile_options(lol_proximity PRIVATE
        /MP                                   # compila em paralelo
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/GL>              # otimizacao entre modulos
        $<$<CONFIG:Release>:/fp:fast>
    )
    target_link_options(lol_proximity PRIVATE
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
else()
    target_compile_options(lol_proximity PRIVATE -O2)
endif()

# Copia a arte de fundo para junto do executavel.
add_custom_command(TARGET lol_proximity POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Fundo.png"
            "$<TARGET_FILE_DIR:lol_proximity>/fundo.png"
)
