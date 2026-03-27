
file(GLOB_RECURSE COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"  # 查找当前目录及子目录中的所有 .c 文件
)

# 定义 common 模块为一个静态库
add_library(common STATIC ${COMMON_SOURCES})

# 设置 common 的头文件路径
target_include_directories(common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_manager
    ${CMAKE_CURRENT_SOURCE_DIR}/gpio_manager
)

if(TARGET_ARM)
    # 手动指定 ALSA 库路径（交叉编译环境）
    set(ALSA_LIBRARY ${CMAKE_SYSROOT}/usr/lib/libasound.so)
    set(ALSA_INCLUDE_DIR ${CMAKE_SYSROOT}/usr/include)
    
    # 检查库文件是否存在
    if(NOT EXISTS ${ALSA_LIBRARY})
        # 如果 libasound.so 不存在，尝试使用链接标志
        set(ALSA_LINK_FLAGS "-lasound")
        message(STATUS "Using ARM ALSA library with flags: ${ALSA_LINK_FLAGS}")
        target_link_options(common PRIVATE ${ALSA_LINK_FLAGS})
    else()
        message(STATUS "Using ARM ALSA library: ${ALSA_LIBRARY}")
        target_link_libraries(common PRIVATE ${ALSA_LIBRARY})
    endif()
    
    target_include_directories(common PRIVATE ${ALSA_INCLUDE_DIR})
else()
    # x86 本地编译：正常查找 ALSA 库
    find_package(ALSA REQUIRED)
    target_link_libraries(common PRIVATE ALSA::ALSA)
endif()

# if(TARGET_ARM)
#     # 查找 ALSA 库
#     find_package(ALSA REQUIRED)
#     # 添加 ALSA 库到 common 静态库
#     target_link_libraries(common PRIVATE ALSA::ALSA)
# endif()