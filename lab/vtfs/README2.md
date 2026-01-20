## Инструкция по применению

### Terminal 1
```bash
make clean && make
sudo rmmod vtfs 2>/dev/null || true
sudo insmod source/vtfs.ko

sudo dmesg | tail -n 30

sudo umount /mnt/vt 2>/dev/null || true
sudo mount -t vtfs -o ip=127.0.0.1,port=8080 TODO /mnt/vt

cd /mnt/vt
```

### Terminal 2
```bash
cd vtfs-server
DEBUG=1 node server.js
```

### Full script
```bash
# 0) server api smoke
cd ~/work/os-course__linux/lab/vtfs/vtfs-server
HOST=127.0.0.1 PORT=8080 TOKEN=TODO node test_api.js ping
HOST=127.0.0.1 PORT=8080 TOKEN=TODO node test_api.js list parent=1000

# 1) mount
cd ~/work/os-course__linux/lab/vtfs
sudo umount /mnt/vt 2>/dev/null || true
sudo rmmod vtfs 2>/dev/null || true
make
sudo insmod source/vtfs.ko
sudo mount -t vtfs none /mnt/vt

# 2) list/lookup through VFS
ls -la /mnt/vt
stat /mnt/vt/dir

# 3) create/remove dir/file through VFS
touch /mnt/vt/a.txt
echo "hello world from file1" > /mnt/vt/file1
cat /mnt/vt/file1
rm /mnt/vt/a.txt
mkdir /mnt/vt/x
rmdir /mnt/vt/x

# 4) overwrite/truncate behavior
echo "test" > /mnt/vt/file1
cat /mnt/vt/file1

# 5) hardlink scenario (Part 9*)
rm -f /mnt/vt/file3
ln /mnt/vt/file1 /mnt/vt/file3
cat /mnt/vt/file3
echo "test2" > /mnt/vt/file1
rm /mnt/vt/file1
cat /mnt/vt/file3
```