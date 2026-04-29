#!/bin/bash

cd /mnt/delfs/benchmark/filebench || exit 1

for i in 1 2; do
    echo "=== Run $i ==="
    filebench -f myfileserver.f
done

