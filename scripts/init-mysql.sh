#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   初始化本地 MySQL 测试数据库和应用账号。
#   会创建/授权 IMM_DB_USER，并导入 scripts/sql/init_message_mysql_test.sql，
#   因此会重置测试库中的表结构和种子数据。
# 常用方式：
#   ./scripts/init-mysql.sh
# 可选环境变量：
#   MYSQL_ROOT_CMD 指定管理命令，MYSQL_HOST/MYSQL_PORT/IMM_DB_* 指定连接信息。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

: "${MYSQL_ROOT_CMD:=sudo mysql}"
: "${MYSQL_HOST:=127.0.0.1}"
: "${MYSQL_PORT:=3306}"
: "${IMM_DB:=chatServ}"
: "${IMM_DB_USER:=zorjen}"
: "${IMM_DB_PASSWORD:=root}"

if [[ ! "$IMM_DB" =~ ^[A-Za-z0-9_]+$ ]]; then
  echo "Invalid IMM_DB: only letters, digits and underscores are allowed." >&2
  exit 2
fi

$MYSQL_ROOT_CMD <<SQL
CREATE DATABASE IF NOT EXISTS $IMM_DB
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS '$IMM_DB_USER'@'localhost' IDENTIFIED BY '$IMM_DB_PASSWORD';
CREATE USER IF NOT EXISTS '$IMM_DB_USER'@'%' IDENTIFIED BY '$IMM_DB_PASSWORD';
GRANT ALL PRIVILEGES ON $IMM_DB.* TO '$IMM_DB_USER'@'localhost';
GRANT ALL PRIVILEGES ON $IMM_DB.* TO '$IMM_DB_USER'@'%';
FLUSH PRIVILEGES;
SQL

mysql --protocol=tcp -h"$MYSQL_HOST" -P"$MYSQL_PORT" \
  -u"$IMM_DB_USER" -p"$IMM_DB_PASSWORD" "$IMM_DB" \
  < "$ROOT_DIR/scripts/sql/init_message_mysql_test.sql"

default_user_count="$(
  mysql --protocol=tcp -h"$MYSQL_HOST" -P"$MYSQL_PORT" \
    -u"$IMM_DB_USER" -p"$IMM_DB_PASSWORD" "$IMM_DB" \
    -N -B -e "
SELECT COUNT(*)
FROM users
WHERE (uid, username) IN (
  (1001, 'zorjen'),
  (1002, 'alice'),
  (1003, 'bob')
);" \
    2>/dev/null
)"
if [[ "$default_user_count" != "3" ]]; then
  echo "MySQL initialization failed: expected default users zorjen(1001), alice(1002), bob(1003)." >&2
  exit 1
fi

mysql --protocol=tcp -h"$MYSQL_HOST" -P"$MYSQL_PORT" \
  -u"$IMM_DB_USER" -p"$IMM_DB_PASSWORD" "$IMM_DB" \
  -e "
SELECT uid, username
FROM users
WHERE uid IN (1001, 1002, 1003)
ORDER BY uid;
SELECT COUNT(*) AS messages FROM messages;"
