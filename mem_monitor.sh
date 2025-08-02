# LOG_FILE="io_uring_fdp_always_fsync_log_2.csv"
# LOG_FILE="io_uring_no_fdp_always_fsync_log_3.csv"

# LOG_FILE="fs_fdp_always_fsync_log_1.csv"
# LOG_FILE="fs_no_fdp_always_fsync_log_1.csv"

# LOG_FILE="io_uring_fdp_everysec_log_0.csv"
# LOG_FILE="io_uring_no_fdp_everysec_log_0.csv"

# LOG_FILE="fs_fdp_everysec_log_1.csv"
# LOG_FILE="mem_fs_no_fdp_everysec_log_0.csv"

LOG_FILE="/home/solesie/redis/test-results/fs_no_fdp_everysec/0/mem.csv"

# LOG_FILE="/home/solesie/redis/test-results/io_uring_no_fdp_everysec/1/mem.csv"

# CSV 헤더 작성
echo "time,used_memory_rss(bytes)" > $LOG_FILE

get_redis_rss() {
    local kb_sum
    kb_sum=$(ps -C redis-server -o rss= | awk '{sum+=$1} END {print sum}')
    echo $((kb_sum * 1024))
}

start_ms=$(date +%s%3N)

while true; do
    now_ms=$(date +%s%3N)
    rel_ms=$((now_ms - start_ms))
    rel_sec=$(echo "scale=2; $rel_ms/1000" | bc)

    mem=$(get_redis_rss)

    echo "$rel_sec,$mem" >> $LOG_FILE

    sleep 0.25
done