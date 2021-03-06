cmake_minimum_required( VERSION 3.8 )
project( monzza )

set( CMAKE_C_COMPILER /opt/rh/devtoolset-6/root/usr/bin/gcc )
set( CMAKE_CXX_COMPILER /opt/rh/devtoolset-6/root/usr/bin/g++ )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lstdc++fs -pthread" )

link_libraries( stdc++fs )

set( SOURCE_FILES source/main.cpp

                  source/settings/settings.cpp
                  source/settings/settings.hpp

                  source/http/http_helpers.hpp
                  source/http/http_request.cpp
                  source/http/http_request.hpp
                  source/http/http_response.cpp
                  source/http/http_response.hpp
                  source/http/http_request_parser.cpp
                  source/http/http_request_parser.hpp
                  source/http/http_response_serializer.cpp
                  source/http/http_response_serializer.hpp
                  source/http/http_file_sender.cpp
                  source/http/http_file_sender.hpp

                  source/buffer/buffer.cpp
                  source/buffer/buffer.hpp

                  external/cpl/cpl.hpp
                  external/cpl/cpl.cpp
                  external/cpl/cpl_event_queue.hpp
                  external/cpl/cpl_time.hpp

                  source/logger/local_logger.hpp
                  source/logger/local_logger.cpp
                  source/logger/log_functionality.hpp
                  source/logger/log_functionality.cpp
                  source/logger/log_level.hpp
                  source/logger/log_message.hpp
                  source/logger/log_message.cpp
                  source/logger/logger.hpp
                  source/logger/logger.cpp
                  source/logger/logger_thread.hpp
                  source/logger/logger_thread.cpp

                  source/master/master_settings.hpp
                  source/master/master_settings.cpp
                  source/master/master_thread.hpp
                  source/master/master_thread.cpp
                  source/master/master.hpp
                  source/master/master.cpp

                  source/worker/worker_settings.hpp
                  source/worker/worker_settings.cpp
                  source/worker/worker_thread.hpp
                  source/worker/worker_thread.cpp
                  source/worker/worker.hpp
                  source/worker/worker.cpp

                  source/table/table.hpp
                  source/table/table.cpp )

add_executable( monzza ${SOURCE_FILES} )