cmake_minimum_required(VERSION 3.10)

# Option to choose target architecture
option(TARGET_ARM "Build for ARM architecture" ON)

# If building for ARM, set the toolchain file before anything else
if(TARGET_ARM)
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/toolchain.cmake" CACHE FILEPATH "Default toolchain file for ARM builds")
        message(STATUS "No toolchain file specified; using default: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
    message(STATUS "Building for ARM using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
else()
    message(STATUS "Building for x86 native architecture.")
endif()

if(TARGET_ARM)
    # 设置交叉编译环境下 pkg-config 使用的路径
    set(ENV{PKG_CONFIG_PATH} "/home/hsia/project/Echo-Mate/SDK/imx6ull-sdk/usr/lib/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "/home/hsia/project/Echo-Mate/SDK/imx6ull-sdk")
endif()

project(DeskBot)

set(CMAKE_C_STANDARD 99)      # C99
set(CMAKE_CXX_STANDARD 17)    # C++17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

add_subdirectory(lvgl)          # LVGL 核心模块
add_subdirectory(gui_app)       # GUI_APP 应用模块
add_subdirectory(utils)         # 公共工具模块
add_subdirectory(common)        # common

# ------------------------------------------------------------
# YOLO 模块配置（ARM 交叉编译）
# ------------------------------------------------------------
# if(TARGET_ARM)
#     # YOLO 库路径
#     set(YOLO_NCNN_PATH "/home/hsia/project/Echo-Mate-with-yolo/Demo/yolo/yolo_ncnn")
#     set(YOLO_NCNN_LIB "${YOLO_NCNN_PATH}/build/libyolo_ncnn.a")
#     set(NCNN_INSTALL_PATH "/home/hsia/project/Echo-Mate-with-yolo/Demo/yolo/ncnn/build/install")
    
#     # 头文件路径
#     include_directories(${NCNN_INSTALL_PATH}/include)
#     include_directories(${YOLO_NCNN_PATH})   # 包含 yolo_ncnn.h 所在目录
# endif()
# if(TARGET_ARM)
#     target_include_directories(gui_app PRIVATE 
#         ${NCNN_INSTALL_PATH}/include
#         ${YOLO_NCNN_PATH}
#     )
# endif()
# ------------------------------------------------------------

# Include 配置头文件路径
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR})
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR}/conf)

# 定义主程序源文件
set(MAIN_SOURCES main.c)

# 链接库
add_executable(main ${MAIN_SOURCES})

# 链接自定义的文件夹库
target_link_libraries(
    main 
    gui_app
    utils
    common
)

include(${CMAKE_CURRENT_LIST_DIR}/lvgl/tests/FindLibDRM.cmake)
include_directories(${Libdrm_INCLUDE_DIRS})

find_package(SDL2)
find_package(SDL2_image)
include_directories(${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS})

# # 链接 YOLO 相关库（仅在 ARM 下）
# if(TARGET_ARM)
#     target_link_libraries(main 
#         ${YOLO_NCNN_LIB}
#         ${NCNN_INSTALL_PATH}/lib/libncnn.a
#         pthread
#     )
# endif()

# 其他链接（LVGL, SDL, 等）
target_link_libraries(main 
    lvgl lvgl::examples lvgl::demos lvgl::thorvg 
    ${SDL2_LIBRARIES} ${SDL2_IMAGE_LIBRARIES} ${Libdrm_LIBRARIES} 
    m pthread
    # -fopenmp
)

add_custom_target (run COMMAND ${OUTPUT_PATH}/main DEPENDS main)

# 复制配置文件
add_custom_command(
    TARGET main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/utils/system_para.conf
            ${OUTPUT_PATH}/system_para.conf
    COMMENT "Copying system_para.conf to bin directory"
)

add_custom_command(
    TARGET main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/utils/gaode_adcode.json
            ${OUTPUT_PATH}/gaode_adcode.json
    COMMENT "Copying gaode_adcode.json to bin directory"
)

add_custom_command(
    TARGET main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/utils/cacert.pem
            ${OUTPUT_PATH}/cacert.pem
    COMMENT "Copying cacert.pem to bin directory"
)

# 引用 AIChat_demo 中的库
set(AI_CHAT_CLIENT_PATH "${PROJECT_SOURCE_DIR}/../AIChat_demo/Client" CACHE PATH "Path to AIChat_demo Client")
add_subdirectory(${AI_CHAT_CLIENT_PATH} ${CMAKE_BINARY_DIR}/AIChat_build)
target_link_libraries(main AIchat-c-interface AIChatCore)

# 复制 AI Chat 资源文件
add_custom_command(
    TARGET main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_PATH}/third_party/snowboy/resources
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_PATH}/third_party/snowboy/resources/models
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_PATH}/third_party/audio
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${AI_CHAT_CLIENT_PATH}/third_party/snowboy/resources/common.res
            ${OUTPUT_PATH}/third_party/snowboy/resources/common.res
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${AI_CHAT_CLIENT_PATH}/third_party/snowboy/resources/models/echo.pmdl
            ${OUTPUT_PATH}/third_party/snowboy/resources/models/echo.pmdl
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${AI_CHAT_CLIENT_PATH}/third_party/audio/waked.pcm
            ${OUTPUT_PATH}/third_party/audio/waked.pcm
    COMMENT "Copying AI Chat necessary resource files to executable output directory"
)

# # 复制 YOLO 模型文件（仅在 ARM 下）
# if(TARGET_ARM)
#     add_custom_command(
#         TARGET main POST_BUILD
#         COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_PATH}/model
#         COMMENT "Ensuring model directory exists"
#     )

#     set(YOLO_MODEL_FILES
#         "/home/hsia/project/Echo-Mate-with-yolo/Demo/yolo/model/yolov6n.param"
#         "/home/hsia/project/Echo-Mate-with-yolo/Demo/yolo/model/yolov6n.bin"
#         "/home/hsia/project/Echo-Mate-with-yolo/Demo/yolo/model/coco.names"
#     )

#     foreach(MODEL_FILE ${YOLO_MODEL_FILES})
#         get_filename_component(FILE_NAME ${MODEL_FILE} NAME)
#         add_custom_command(
#             TARGET main POST_BUILD
#             COMMAND ${CMAKE_COMMAND} -E copy_if_different
#                     ${MODEL_FILE}
#                     ${OUTPUT_PATH}/model/${FILE_NAME}
#             COMMENT "Copying ${FILE_NAME} to executable output directory"
#         )
#     endforeach()
# endif()

# 使用 set_target_properties 设置 main 的输出目录
set_target_properties(main PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_PATH}"
)

# Custom command to clean up generated files
if(NOT TARGET clean-all)
    add_custom_target(clean-all
        COMMAND find "${CMAKE_BINARY_DIR}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
        COMMENT "Cleaning all generated files."
    )
endif()