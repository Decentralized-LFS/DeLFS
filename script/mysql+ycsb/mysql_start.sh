#!/bin/bash
set -e

DATADIR="/mnt/test/mysql-data"

echo "[+] Stop MySQL if running"
sudo systemctl stop mysql || true
sudo pkill mysqld || true

echo "[+] Prepare MySQL datadir"
sudo rm -rf "$DATADIR"
sudo mkdir -p "$DATADIR"
sudo chown mysql:mysql "$DATADIR"
sudo chmod 750 "$DATADIR"

echo "[+] Initialize datadir"
sudo mysqld --initialize --user=mysql --datadir="$DATADIR"

echo "[+] Ensure socket dir"
sudo mkdir -p /var/run/mysqld
sudo chown mysql:mysql /var/run/mysqld
sudo chmod 777 /var/run/mysqld

echo "[+] Start MySQL in skip-grant-tables mode"
sudo mysqld_safe --skip-grant-tables --datadir="$DATADIR" &
sleep 5

echo "[+] Remove root password"
mysql -u root <<EOF
FLUSH PRIVILEGES;
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '';
FLUSH PRIVILEGES;
EOF

echo "[+] Kill safe-mode MySQL"
sudo pkill mysqld || true
sudo pkill -9 mysqld_safe || true
sudo rm -f /var/run/mysqld/mysqld.pid
sudo rm -f /mnt/test/mysql-data/*.pid
sleep 2

echo "[+] Starting MySQL on /mnt/test/mysql-data..."
sudo systemctl start mysql
sleep 1
sudo systemctl status mysql --no-pager

echo "[+] Test root login & datadir"
mysql -u root -e "SELECT @@datadir;"
