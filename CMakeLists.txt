# 项目名称
project (threadpool)
# CMake最低版本号要求
cmake_minimum_required (VERSION 3.0)
# 头文件目录
include_directories(include)
# 查找./src目录下的所有源文件，保存到DIR_SRCS变量
aux_source_directory(./src DIR_SRCS)
# 用C++11
add_definitions(-std=c++11)
# 指定生成目标文件
add_executable(threadpool ${DIR_SRCS})
set(CMAKE_CXX_FLAGS "-I/usr/include/mysql")
# 库文件
# find_package (mysql)
target_link_libraries (threadpool -L/usr/lib/x86_64-linux-gnu -lmysqlclient -lzstd -lssl -lcrypto -lresolv -lm)
# muduo网络库的引入，muduo网络库
# find_package(muduo)





