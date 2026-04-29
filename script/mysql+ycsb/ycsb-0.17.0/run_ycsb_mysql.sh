#!/bin/bash
set -e

YCSB_DIR="/mnt/delfs/benchmark/mysql+ycsb/ycsb-0.17.0"
WORKLOAD="workloada"
THREADS=128
DB_URL="jdbc:mysql://127.0.0.1:3306/ycsb"
DB_USER="ycsb"
DB_PASS=""

echo "[1] LOAD phase starting..."
$YCSB_DIR/bin/ycsb load jdbc \
	-P $YCSB_DIR/workloads/$WORKLOAD \
	-p db.driver=com.mysql.cj.jdbc.Driver \
	-p db.url=$DB_URL \
	-p db.user=$DB_USER \
	-p db.passwd=$DB_PASS \
	-threads $THREADS \
	-s | tee ycsb_load_result.txt

echo "[2] RUN phase starting..."
$YCSB_DIR/bin/ycsb run jdbc \
	-P $YCSB_DIR/workloads/$WORKLOAD \
	-p db.driver=com.mysql.cj.jdbc.Driver \
	-p db.url=$DB_URL \
	-p db.user=$DB_USER \
	-p db.passwd=$DB_PASS \
	-threads $THREADS \
	-s | tee ycsb_run_result.txt

echo "[+] Completed. Results:"
echo "- ycsb_load_result.txt"
echo "- ycsb_run_result.txt"

