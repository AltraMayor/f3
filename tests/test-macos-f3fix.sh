#!/bin/bash
set -e  # Exit on error

echo "=== Starting F3 Test Suite on macOS ==="

# Clean up from previous runs
rm -f test.img

# Create test image
echo "Creating test image..."
dd if=/dev/zero of=test.img bs=1M count=100

# Attach disk image
echo "Attaching disk image..."
disk_info=$(hdiutil attach -nomount -readwrite test.img)
device=$(echo $disk_info | awk '{print $1}')

trap 'echo "Cleaning up..."; diskutil unmountDisk $device 2>/dev/null || true; diskutil eject $device 2>/dev/null || true; rm -f test.img' EXIT

echo "Using device: $device"

# Build with debug flags
echo "Building F3 with debug flags..."
CFLAGS="-DDEBUG" make clean all extra

# Test different configurations
test_cases=(
    "--disk-type=msdos --fs-type=fat32"
    "--disk-type=gpt --fs-type=fat32"
    "--disk-type=mbr --fs-type=hfs+"
    "--disk-type=mbr --fs-type=exfat"
)

for test_case in "${test_cases[@]}"; do
    echo -e "\n=== Testing configuration: $test_case ==="
    ./build/bin/f3fix $test_case --first-sec=2048 --last-sec=102400 --boot $device
    echo "Current disk layout:"
    diskutil list $device
    fdisk $device
    echo "----------------------------------------"
done

echo -e "\n=== All tests completed successfully! ==="
