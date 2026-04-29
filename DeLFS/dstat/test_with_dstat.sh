#!/bin/bash

SESSION_NAME="fio_test_session"

compare_version="balance"
kernel_version=$(uname -r)

result_path="./"
file_name="fio_$2_$1"

# ▼ 추가: $1( NVMe 인덱스 ) 검증: 없거나 숫자가 아니면 종료
if ! [[ "$1" =~ ^[0-9]+$ ]]; then
  echo "사용법: $0 <nvme_idx> [arg2] [arg3]"
  echo "예시:  $0 3 heavy read   # dstat -D nvme3n1p1 ..."
  exit 1
fi

generate_unique_filename() {
        local base_name="$1"
        local extension="csv"
        local count=1

        local new_filename="${result_path}/${base_name}_${count}.${extension}"
        while [ -e "$new_filename" ]; do
                count=$((count + 1))
                new_filename="${result_path}/${base_name}_${count}.${extension}"
        done

        echo "$new_filename"
}

# $1을 NVMe 인덱스로 사용
nvme_idx="$1"
nvme_dev="nvme${nvme_idx}n1p1"

if [[ "$kernel_version" == *"$compare_version"* ]]; then
        result_path+="/proposed"
else
        result_path+="/original"
fi
mkdir -p "${result_path}"

unique_filename=$(generate_unique_filename "$file_name")
echo "$unique_filename"


# prepare code ############################################################################

fio ../fio_script/48thread_gc_30.fio
# prepare code ############################################################################

cur_path=$(pwd)
echo "${cur_path}/${unique_filename}"
#tmux new-session -d -s $SESSION_NAME "dstat -D md126,total --output ${cur_path}/${unique_filename}"
#tmux new-session -d -s $SESSION_NAME "dstat -D md127 --output ${cur_path}/${unique_filename}"
tmux new-session -d -s "$SESSION_NAME" "dstat -D ${nvme_dev} --output ${cur_path}/${unique_filename}"

# test code
# do something start
fio ../fio_script/48thread_gc_30.fio
#fio ../fio_script/start.fio
# do something end

tmux send-keys -t "$SESSION_NAME" C-c
sleep 1
tmux kill-session -t "$SESSION_NAME" # 혹시 모르니 한번 더 종료

