
file(GLOB_RECURSE GUI_APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"  # 查找当前目录及子目录中的所有 .c 文件
)

# 定义 gui_app 模块为一个静态库
add_library(gui_app STATIC ${GUI_APP_SOURCES})

# 设置 gui_app 的头文件路径
target_include_directories(gui_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/common/stack
    ${CMAKE_CURRENT_SOURCE_DIR}/common/page_manager
)

# # 手动指定库路径（交叉编译环境）
# if(TARGET_ARM)
#     # 使用 sysroot 中的 ARM 版本库
    
#     # json-c 库
#     set(JSON_C_LIBRARY ${CMAKE_SYSROOT}/usr/lib/libjson-c.so)
#     set(JSON_C_INCLUDE_DIR ${CMAKE_SYSROOT}/usr/include)
    
#     # CURL 库 - 使用链接标志
#     set(CURL_LINK_FLAGS "-lcurl")
#     set(CURL_INCLUDE_DIR ${CMAKE_SYSROOT}/usr/include)
    
#     # 检查库文件是否存在
#     if(NOT EXISTS ${JSON_C_LIBRARY})
#         message(FATAL_ERROR "json-c library not found at: ${JSON_C_LIBRARY}")
#     endif()
    
#     message(STATUS "Using ARM json-c library: ${JSON_C_LIBRARY}")
#     message(STATUS "Using ARM CURL library with flags: ${CURL_LINK_FLAGS}")
    
#     # 链接库文件
#     target_link_libraries(gui_app PRIVATE ${JSON_C_LIBRARY})
#     target_link_options(gui_app PRIVATE ${CURL_LINK_FLAGS})
#     target_include_directories(gui_app PRIVATE ${JSON_C_INCLUDE_DIR} ${CURL_INCLUDE_DIR})
# else()
#     # x86 本地编译：正常查找库
#     find_package(json-c REQUIRED)
#     find_package(CURL REQUIRED)
#     target_link_libraries(gui_app PRIVATE json-c)
#     target_include_directories(gui_app PRIVATE ${CURL_INCLUDE_DIRS})
#     target_link_libraries(gui_app PRIVATE CURL::libcurl)
# endif()

# 查找包
find_package(json-c REQUIRED)
find_package(CURL REQUIRED)
# 将库链接到 gui_app
target_link_libraries(gui_app PRIVATE json-c)
target_include_directories(gui_app PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(gui_app PRIVATE ${CURL_LIBRARIES})