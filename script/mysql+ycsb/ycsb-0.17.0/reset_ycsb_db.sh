#!/bin/bash
set -e

echo "[+] Resetting YCSB MySQL Database..."

mysql -u root <<EOF
-- 1. 기존 DB 삭제
DROP DATABASE IF EXISTS ycsb;

-- 2. ycsb 유저 생성
DROP USER IF EXISTS 'ycsb'@'localhost';
CREATE USER 'ycsb'@'localhost' IDENTIFIED WITH mysql_native_password BY '';

-- 3. 새 DB 생성
CREATE DATABASE ycsb;

-- 4. 테스트용 유저 권한 보장
GRANT ALL PRIVILEGES ON ycsb.* TO 'ycsb'@'localhost';
FLUSH PRIVILEGES;

-- 5. usertable 생성
USE ycsb;

CREATE TABLE usertable (
  YCSB_KEY VARCHAR(255) PRIMARY KEY,
  FIELD0 TEXT,
  FIELD1 TEXT,
  FIELD2 TEXT,
  FIELD3 TEXT,
  FIELD4 TEXT,
  FIELD5 TEXT,
  FIELD6 TEXT,
  FIELD7 TEXT,
  FIELD8 TEXT,
  FIELD9 TEXT
);
EOF

echo "[+] Done."
echo "[+] Database 'ycsb' and table 'usertable' recreated."

