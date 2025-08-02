#!/usr/bin/env bash
set -euo pipefail

#--------------------------------------------------
# 설정
#--------------------------------------------------
TYPE="io_uring_redis_fdp_disabled_always"

RESULTS_DIR="/home/solesie/redis/bench-results/ycsb/${TYPE}/2"
SERVER_LOG="${RESULTS_DIR}/server.txt"
CONF_FILE="./${TYPE}.conf"
REDIS_SERVER="./src/redis-server"
YCSB_DIR="/home/solesie/YCSB"
YCSB_BENCH="${YCSB_DIR}/bin/ycsb"
YCSB_WORKLOAD="${YCSB_DIR}/workloads/workloada"
YCSB_LOAD_FILE="${RESULTS_DIR}/outputLoad.txt"
YCSB_RUN_LOG="${RESULTS_DIR}/outputRunLog.txt"
MEM_FILE="${RESULTS_DIR}/mem.csv"

mkdir -p "${RESULTS_DIR}"

#--------------------------------------------------
# 함수 정의
#--------------------------------------------------
get_redis_mem() {
    local kb_sum
    kb_sum=$(ps -C redis-server -o rss= | awk '{sum+=$1} END {print sum}')
    echo $((kb_sum * 1024))
}
mem_monitor() {
    local out_file="$1"
    echo "time,used_memory_rss(bytes)" > "${out_file}"
    local start_ms=$(date +%s%3N)
    while true; do
        local now_ms=$(date +%s%3N)
        local rel_ms=$((now_ms - start_ms))
        local rel_sec=$(awk "BEGIN { printf \"%.2f\", ${rel_ms}/1000 }")
        local mem
        mem=$(get_redis_mem)
        echo "${rel_sec},${mem}" >> "${out_file}"
        sleep 0.25
    done
}

#--------------------------------------------------
# 1) Redis 서버 기동
#--------------------------------------------------
echo "[1] Redis 서버 시작 (로그: ${SERVER_LOG})"
sudo ${REDIS_SERVER} "${CONF_FILE}" > "${SERVER_LOG}" 2>&1 & 
SERVER_PID=$!
echo "    → Redis PID=${SERVER_PID}"
sleep 2

#--------------------------------------------------
# 2) 초기 데이터 적재
#--------------------------------------------------
cd ../YCSB
echo "[2] 초기 데이터 적재 (20GiB 기준)"
sudo ${YCSB_BENCH} \
    load redis -s -P ${YCSB_WORKLOAD} -threads 16 > ${YCSB_LOAD_FILE}
cd ../redis

#--------------------------------------------------
# 3) PHASE
#--------------------------------------------------
echo "[3] YCSB run (220GiB read/write)"

# 3-1) 메모리 모니터링 시작
mem_monitor "${MEM_FILE}" &
MEM_PID=$!
sleep 0.5

# 3-2) YCSB run
cd ../YCSB
sudo ${YCSB_BENCH} \
    run redis -s -P ${YCSB_WORKLOAD} -threads 8 >> ${YCSB_RUN_LOG} 2>&1
cd ../redis

# 3-3) 메모리 모니터링 종료
echo "    → 메모리 모니터링 종료 (PID=${MEM_PID})"
kill "${MEM_PID}" || true

sleep 20

#--------------------------------------------------
# 마무리: Redis 서버 종료
#--------------------------------------------------
echo "[END] Redis 서버 종료 (PID=${SERVER_PID})"
sudo kill "${SERVER_PID}"

sudo chmod 766 ${RESULTS_DIR}/*