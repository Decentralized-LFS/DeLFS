#!/usr/bin/env bash
set -euo pipefail

FS_TYPE=delfs

print_usage() {
  echo "Usage: $(basename "$0") <THREADS> <TOTAL_PULLS> <PULLS_PER_TEST> <AGED_NVME_NUM> <UNAGED_NVME_NUM>" >&2
  echo "Example: $(basename "$0") 128 5 5 2 0" >&2
  echo "         -> AGED_DEV=/dev/nvme2n1p1, UNAGED_DEV=/dev/nvme0n1p1" >&2
}

is_positive_int() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_nonnegative_int() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

if [[ $# -ne 5 ]]; then
  echo "WARNING: required arguments are missing or the argument count is invalid." >&2
  print_usage
  exit 1
fi

THREADS="$1"
TOTAL_PULLS="$2"
PULLS_PER_TEST="$3"
AGED_NVME_NUM="$4"
UNAGED_NVME_NUM="$5"

if ! is_positive_int "${THREADS}" || \
   ! is_positive_int "${TOTAL_PULLS}" || \
   ! is_positive_int "${PULLS_PER_TEST}" || \
   ! is_nonnegative_int "${AGED_NVME_NUM}" || \
   ! is_nonnegative_int "${UNAGED_NVME_NUM}"; then
  echo "WARNING: THREADS, TOTAL_PULLS, and PULLS_PER_TEST must be positive integers; NVMe numbers must be 0 or positive integers." >&2
  print_usage
  exit 1
fi

AGED_DEV="/dev/nvme${AGED_NVME_NUM}n1p1"
UNAGED_DEV="/dev/nvme${UNAGED_NVME_NUM}n1p1"
AGED_MNT=/mnt/aged
UNAGED_MNT=/mnt/unaged

# Per-thread source repositories must exist as:
#   /mnt/git_bench_linux/linux0 ... /mnt/git_bench_linux/linux127
SRC_REPO_ROOT=/mnt/git_bench_linux
SRC_REPO_PREFIX=linux

WORKSPACE="benchmark_parallel_${THREADS}"

DELFS_MKFS=/home/syslab/workspace_hwan/hwan_working/DeLFS/delfs-mkfs/mkfs/mkfs.delfs
DELFS_KO=/home/syslab/workspace_hwan/hwan_working/DeLFS/DeLFS/module_DeLFS/delfs.ko

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY_SCRIPT="${SCRIPT_DIR}/percore-benchmark2.py"

# Every run gets its own result directory:
#   results/YYYYMMDD_HHMMSS_<filesystem-type>
RUN_TS="$(TZ=Asia/Seoul date '+%Y%m%d_%H%M%S')"
RESULT_ROOT="${SCRIPT_DIR}/results"
RUN_DIR="${RESULT_ROOT}/${RUN_TS}_${FS_TYPE}"
if [[ -e "${RUN_DIR}" ]]; then
  suffix=1
  while [[ -e "${RESULT_ROOT}/${RUN_TS}_${FS_TYPE}_${suffix}" ]]; do
    suffix=$((suffix + 1))
  done
  RUN_DIR="${RESULT_ROOT}/${RUN_TS}_${FS_TYPE}_${suffix}"
fi
mkdir -p "${RUN_DIR}"

OUT="${RUN_DIR}/output_${FS_TYPE}_${THREADS}t.txt"
DSTAT_DIR="${RUN_DIR}/dstat/${FS_TYPE}_${THREADS}t"
RUN_TAG="${FS_TYPE}_${THREADS}t_${RUN_TS}"
RUN_LOG="${RUN_DIR}/run.log"

# Capture all stdout/stderr from this shell script, mkfs/mount, and Python.
exec > >(tee -a "${RUN_LOG}") 2>&1

on_exit() {
  status=$?
  echo "--------------------------------------------------------------------------------"
  echo "Exit status     : ${status}"
  echo "Result directory: ${RUN_DIR}"
  echo "Run log         : ${RUN_LOG}"
  echo "Combined output : ${OUT}"
  echo "Phase summary   : ${OUT}.phase_summary.tsv"
  echo "Per-thread dir  : ${OUT}.threads"
  echo "Dstat dir       : ${DSTAT_DIR}"
}
trap on_exit EXIT

echo "--------------------------------------------------------------------------------"
echo "Run timestamp   : ${RUN_TS} KST"
echo "Filesystem type : ${FS_TYPE}"
echo "Result directory: ${RUN_DIR}"
echo "Python script   : ${PY_SCRIPT}"
echo "Flow            : aged git_pull -> aged grep -> unaged mkfs/mount -> cp aged to unaged -> unaged grep"
echo "--------------------------------------------------------------------------------"

# Keep copies of the scripts/config used for reproducibility.
cp "${BASH_SOURCE[0]}" "${RUN_DIR}/$(basename "${BASH_SOURCE[0]}").used" 2>/dev/null || true
if [[ -f "${PY_SCRIPT}" ]]; then
  cp "${PY_SCRIPT}" "${RUN_DIR}/percore-benchmark2.py.used" 2>/dev/null || true
fi
cat > "${RUN_DIR}/run_config.txt" <<EOF_CONFIG
RUN_TS=${RUN_TS}
FS_TYPE=${FS_TYPE}
AGED_NVME_NUM=${AGED_NVME_NUM}
UNAGED_NVME_NUM=${UNAGED_NVME_NUM}
AGED_DEV=${AGED_DEV}
UNAGED_DEV=${UNAGED_DEV}
AGED_MNT=${AGED_MNT}
UNAGED_MNT=${UNAGED_MNT}
SRC_REPO_ROOT=${SRC_REPO_ROOT}
SRC_REPO_PREFIX=${SRC_REPO_PREFIX}
THREADS=${THREADS}
TOTAL_PULLS=${TOTAL_PULLS}
PULLS_PER_TEST=${PULLS_PER_TEST}
WORKSPACE=${WORKSPACE}
OUT=${OUT}
DSTAT_DIR=${DSTAT_DIR}
RUN_TAG=${RUN_TAG}
DELFS_MKFS=${DELFS_MKFS}
DELFS_KO=${DELFS_KO}
EOF_CONFIG

if [[ ! -f "${PY_SCRIPT}" ]]; then
  echo "ERROR: Python benchmark not found: ${PY_SCRIPT}" >&2
  exit 1
fi

if [[ ! -x "${DELFS_MKFS}" ]]; then
  echo "ERROR: DELFS_MKFS not found or not executable: ${DELFS_MKFS}" >&2
  exit 1
fi

if [[ ! -f "${DELFS_KO}" ]]; then
  echo "ERROR: DELFS_KO not found: ${DELFS_KO}" >&2
  exit 1
fi

if [[ "${AGED_DEV}" == "${UNAGED_DEV}" ]]; then
  echo "ERROR: AGED_DEV and UNAGED_DEV must be different." >&2
  exit 1
fi

if ! command -v dstat >/dev/null 2>&1; then
  echo "ERROR: dstat not found. Install dstat or run Python with --disable_dstat." >&2
  exit 1
fi

for tid in $(seq 0 $((THREADS - 1))); do
  repo="${SRC_REPO_ROOT}/${SRC_REPO_PREFIX}${tid}"
  if [[ ! -d "${repo}/.git" ]]; then
    echo "ERROR: missing per-thread source repo: ${repo}" >&2
    exit 1
  fi
done

sudo umount "${AGED_MNT}" 2>/dev/null || true
sudo umount "${UNAGED_MNT}" 2>/dev/null || true

sudo "${DELFS_MKFS}" -f "${AGED_DEV}"
sudo "${DELFS_MKFS}" -f "${UNAGED_DEV}"

sudo modprobe lz4hc_compress
sudo modprobe lz4_compress

if ! lsmod | grep -q '^delfs'; then
  sudo insmod "${DELFS_KO}"
fi
sudo mkdir -p "${AGED_MNT}" "${UNAGED_MNT}"
sudo mount -t delfs "${AGED_DEV}" "${AGED_MNT}"

sudo python3 "${PY_SCRIPT}" \
  "${SRC_REPO_ROOT}" \
  "${AGED_MNT}" \
  "${OUT}" \
  "${TOTAL_PULLS}" \
  "${PULLS_PER_TEST}" \
  "${UNAGED_MNT}" \
  "${AGED_DEV}" \
  "${UNAGED_DEV}" \
  --threads "${THREADS}" \
  --fs_type "${FS_TYPE}" \
  --workspace "${WORKSPACE}" \
  --src_repo_prefix "${SRC_REPO_PREFIX}" \
  --mkfs_cmd "${DELFS_MKFS}" \
  --run_tag "${RUN_TAG}" \
  --dstat_dir "${DSTAT_DIR}" \
  --dstat_devices "nvme${AGED_NVME_NUM}n1,nvme${UNAGED_NVME_NUM}n1,total"

sleep 30
fio aged-rand-write.fio
