# 仅导入安装脚本生成的连接器；不搜索未应用 reactor TLS 补丁的系统库。
get_filename_component(_mariadb_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET MariaDB::client)
    add_library(MariaDB::client SHARED IMPORTED)
    set_target_properties(MariaDB::client PROPERTIES
        IMPORTED_LOCATION "${_mariadb_prefix}/lib/libmariadb.so"
        INTERFACE_INCLUDE_DIRECTORIES "${_mariadb_prefix}/include/mariadb")
endif()
unset(_mariadb_prefix)
