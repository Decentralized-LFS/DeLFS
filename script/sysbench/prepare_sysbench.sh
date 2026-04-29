#!/bin/bash
set -e

DB=sbtest
TABLES=20
TABLE_SIZE=10000000
THREADS=128

echo "[+] Start MySQL..."
sudo systemctl start mysql || true

echo "[+] Create database: $DB"
mysql -u root -e "DROP DATABASE IF EXISTS $DB; CREATE DATABASE $DB;"

echo "[+] Sysbench prepare..."
sysbench oltp_read_write \
	--mysql-user=root \
	--mysql-db=$DB \
	--tables=$TABLES \
	--table-size=$TABLE_SIZE \
	prepare

echo "[+] Prepare done."

