#!/bin/bash

# YCSB + RocksDB 루트 디렉토리
YCSB_HOME="/mnt/delfs/benchmark/RocksDB"

# Workload 파일
WORKLOAD="${YCSB_HOME}/workloads/myworkloadb"

# RocksDB DB 디렉토리
DB_DIR="/mnt/test/rocksdb"

mkdir -p "${DB_DIR}"

cd "${YCSB_HOME}"

echo "=== LOAD (myworkloadb, RocksDB) ==="
./bin/ycsb load rocksdb \
  -P "${WORKLOAD}" \
  -p rocksdb.dir="${DB_DIR}"

echo
echo "=== RUN (myworkloadb, RocksDB) ==="
./bin/ycsb run rocksdb \
  -P "${WORKLOAD}" \
  -p rocksdb.dir="${DB_DIR}"


