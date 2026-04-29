et -e

DB=sbtest
TABLES=20
TABLE_SIZE=10000000
THREADS=128
TIME=60   # 실행 시간(초) — 필요하면 조절

echo "[+] Start MySQL..."
sudo systemctl start mysql || true

echo "[+] Running sysbench OLTP read/write benchmark..."
sysbench oltp_read_write \
	--mysql-user=root \
	--mysql-db=$DB \
	--threads=$THREADS \
	--tables=$TABLES \
	--table-size=$TABLE_SIZE \
	--time=$TIME \
	run

echo "[+] sysbench RUN phase complete."

