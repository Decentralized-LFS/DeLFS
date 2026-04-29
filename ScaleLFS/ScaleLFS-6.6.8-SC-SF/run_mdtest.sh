if mountpoint -q "/mnt/test"; then
	#mpirun -np 72 mdtest -n 125000 -F -C -P -u -d /mnt/test
	mpirun -np 72 mdtest -n 500000 -F -C -P -u -d /mnt/test
	#mpirun -np 72 mdtest -n 50000 -F -C -P -u -d /mnt/test
else
    # 마운트되지 않은 경우 아무 작업도 하지 않음
    echo "디렉토리가 마운트되어 있지 않습니다. 작업을 종료합니다."
fi




