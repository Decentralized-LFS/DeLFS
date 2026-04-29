#!/bin/bash

for i in {1..7}; do
	fio rand-write.fio
done
