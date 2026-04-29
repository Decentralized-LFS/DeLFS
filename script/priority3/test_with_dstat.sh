#!/bin/bash

if [ -z "$1" ]; then
    echo "사용법: $0 <DEVNUM> [tag]"
    echo "예시: $0 5 test1"
    exit 1
fi

SESSION_NAME="stress_no_time_limit"

DEVNUM="$1"
TAG="$2"

DEVICE="nvme${DEVNUM}n1"

compare_version="DeLFS"
kernel_version=$(uname -r)

result_path="dstat"
file_name="stress_no_time_limit_${TAG}_${DEVNUM}"

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

if [[ "$kernel_version" == *"$compare_version"* ]]; then
        result_path+="/proposed"
else
        result_path+="/original"
fi
mkdir -p "${result_path}"

unique_filename=$(generate_unique_filename "$file_name")
echo "$unique_filename"

cur_path=$(pwd)
echo "${cur_path}/${unique_filename}"

tmux new-session -d -s "$SESSION_NAME" "dstat -D ${DEVICE},${DEVICE}p1,total --output ${cur_path}/${unique_filename}"

# test code
# do something start

#filebench -f 90-varmail.f
#filebench -f 90-varmail1.f
#filebench -f 90-varmail2.f
#filebench -f 90-varmail3.f
#filebench -f 90-varmail4.f
#filebench -f 90-varmail5.f
#filebench -f 90-varmail6.f
#filebench -f 90-varmail7.f
#filebench -f myfileserver.f
#filebench -f myfileserver-1.f
#filebench -f myfileserver-2.f
filebench -f myfileserver-4.f
#filebench -f myfileserver-test.f
#filebench -f 99-varmail.f
#filebench -f 99-varmail2.f
#filebench -f 75-varmail.f
#filebench -f 90-fileserver.f

# do something end

tmux send-keys -t "$SESSION_NAME" C-c
sleep 1

tmux kill-session -t "$SESSION_NAME" 2>/dev/null
