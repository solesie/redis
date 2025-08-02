# sudo ./ycsb_io_uring_redis_fdp_disabled_always_run.sh
# sleep 10
# ps -aux | grep redis | grep -v grep | awk '{print $2}' | xargs sudo kill -9
# sleep 10
# sudo sh ../setup_cofdp.sh
# sudo sh ../setup_cofdp.sh
# sleep 10


sudo ./ycsb_fs_redis_fdp_disabled_always_run.sh
sleep 10
ps -aux | grep redis | grep -v grep | awk '{print $2}' | xargs sudo kill -9
sleep 10
sudo umount /dev/nvme0n1
sudo ../setup_cofdp.sh
sudo umount /dev/nvme0n1
sudo mkfs.ext4 /dev/nvme0n1
sudo mount /dev/nvme0n1 ./mnt
sudo mkdir ./mnt/p0
sleep 10


sudo ./ycsb_fs_redis_fdp_disabled_everysec_run.sh
sleep 10
ps -aux | grep redis | grep -v grep | awk '{print $2}' | xargs sudo kill -9
sleep 10


# sudo ./ycsb_io_uring_redis_fdp_disabled_always_run.sh
# sleep 10
# ps -aux | grep redis | grep -v grep | awk '{print $2}' | xargs sudo kill -9
# sleep 10
# sudo sh ../setup_cofdp.sh
# sudo sh ../setup_cofdp.sh
# sleep 10