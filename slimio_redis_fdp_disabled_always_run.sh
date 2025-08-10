#!/usr/bin/env bash
set -euo pipefail

#--------------------------------------------------
# 설정
#--------------------------------------------------
TYPE="io_uring_redis_fdp_disabled_always"

RESULTS_DIR="/home/solesie/redis/bench-results/${TYPE}/1"
SERVER_LOG="${RESULTS_DIR}/server.txt"
CONF_FILE="./${TYPE}.conf"
REDIS_SERVER="./src/redis-server"
REDIS_CLI="./src/redis-cli"
REDIS_BENCH="./src/redis-benchmark"

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
# Redis 서버 기동
#--------------------------------------------------
echo "[1] Redis 서버 시작 (로그: ${SERVER_LOG})"
sudo ${REDIS_SERVER} "${CONF_FILE}" > "${SERVER_LOG}" 2>&1 & 
SERVER_PID=$!
echo "    → Redis PID=${SERVER_PID}"
sleep 60

#--------------------------------------------------
# 1) 초기 데이터 적재 & AOF 리라이트
#--------------------------------------------------
echo "[2] 초기 데이터 적재 (20GiB 기준)"
sudo ${REDIS_BENCH} \
    -h 127.0.0.1 -p 6379 \
    -c 50 -n 5321523 -d 4096 -t set -r 5321523 -k 1 

echo "[3] BGREWRITEAOF 실행"
sudo ${REDIS_CLI} BGREWRITEAOF
sleep 60

#--------------------------------------------------
# 2) PHASE loop
#--------------------------------------------------
START_PHASE="${1:-1}"
END_PHASE="${2:-5}"
for PHASE in $(seq "${START_PHASE}" "${END_PHASE}"); do
    echo "[PHASE ${PHASE}] 시작"
    RPS_FILE="${RESULTS_DIR}/rps_${PHASE}.csv"
    MEM_FILE="${RESULTS_DIR}/mem_${PHASE}.csv"
    SUMMARY_FILE="${RESULTS_DIR}/summary_${PHASE}.txt"

    # 2-1) 메모리 모니터링 시작
    mem_monitor "${MEM_FILE}" &
    MEM_PID=$!
    sleep 0.5

    # 2-2) BGSAVE 스케줄링
    echo "    → BGSAVE schedule"
    sudo ${REDIS_CLI} BGSAVE schedule

    # 2-3) 약 110GiB 데이터 적재 및 RPS 측정
    echo "    → redis-benchmark (110GiB, RPS 기록 → ${RPS_FILE})"
    sudo ${REDIS_BENCH} \
        -h 127.0.0.1 -p 6379 \
        -c 50 \
        -n 28835840 -d 4096 -t set -r 5321523 -k 1 \
        --rps-filename "${RPS_FILE}" \
        > "${SUMMARY_FILE}" 2>&1

    # 2-4) 메모리 모니터링 종료
    echo "    → 메모리 모니터링 종료 (PID=${MEM_PID})"
    kill "${MEM_PID}" || true

    echo "[PHASE ${PHASE}] 완료"
    echo
done

sleep 60

#--------------------------------------------------
# 마무리: Redis 서버 종료
#--------------------------------------------------
echo "[END] Redis 서버 종료 (PID=${SERVER_PID})"
sudo kill "${SERVER_PID}"

sudo chmod 766 ${RESULTS_DIR}/*