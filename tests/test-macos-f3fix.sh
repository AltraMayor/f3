#!/bin/bash
set -euo pipefail

# Test configurations
readonly TEST_CASES=(
    "--disk-type=msdos --fs-type=fat32"
    "--disk-type=gpt --fs-type=fat32"
    "--disk-type=mbr --fs-type=hfs+"
    "--disk-type=mbr --fs-type=exfat"
)

# Nice colors for output
YELLOW='\033[0;33m'
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

log() {
    echo -e "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

log_success() {
    log "${GREEN}SUCCESS: $1${NC}"
}

log_error() {
    log "${RED}ERROR: $1${NC}" >&2
    exit 1
}

log_info() {
    log "${YELLOW}INFO: $1${NC}"
}

check_requirements() {
    local commands=("diskutil" "hdiutil" "make" "fdisk" "dd" "awk")
    for cmd in "${commands[@]}"; do
        if ! command -v "$cmd" &> /dev/null; then
            log_error "Required command '$cmd' not found"
        fi
    done
}

check_disk_space() {
    local required_mb=200
    local available_mb=$(df -m . | awk 'NR==2 {print $4}')
    
    if [ "$available_mb" -lt "$required_mb" ]; then
        log_error "Insufficient disk space. Need at least ${required_mb}MB, but only ${available_mb}MB available."
    fi
}

log_system_info() {
    log_info "=== System Information ==="
    system_profiler SPSoftwareDataType SPHardwareDataType | grep -E 'System Version|Model Identifier|Cores|Memory'
    log_info "Disk space: $(df -h . | awk 'NR==2 {print $4}') available"
}

cleanup() {
    local exit_code=$?
    log_info "Cleaning up..."
    
    if [ -n "${device:-}" ]; then
        if ! diskutil unmountDisk "$device" 2>/dev/null; then
            log_info "Could not unmount $device, trying to force..."
            diskutil unmountDisk force "$device" 2>/dev/null || true
        fi
        diskutil eject "$device" 2>/dev/null || true
    fi
    
    if [ -f "test.img" ]; then
        rm -f test.img
    fi
    
    if [ $exit_code -eq 0 ]; then
        log_success "All tests completed successfully!"
    else
        log_error "Tests failed with exit code $exit_code"
    fi
}

main() {
    trap cleanup EXIT
    check_requirements
    check_disk_space
    log_system_info
    
    local test_start_time=$(date +%s)
    
    # Clean up from previous runs
    rm -f test.img
    
    log_info "Creating test image..."
    if ! dd if=/dev/zero of=test.img bs=1M count=100 status=progress; then
        log_error "Failed to create test image"
    fi
    
    log_info "Attaching disk image..."
    disk_info=$(hdiutil attach -nomount -readwrite test.img 2>&1) || {
        log_error "Failed to attach disk image: $disk_info"
    }
    
    device=$(echo "$disk_info" | awk '{print $1}')
    [ -z "$device" ] && log_error "Failed to get device name"
    
    log_info "Using device: $device"
    
    log_info "Building F3 with debug flags..."
    if ! CFLAGS="-DDEBUG" make clean all extra; then
        log_error "Build failed"
    fi
    
    local test_count=0
    local passed_count=0
    
    for test_case in "${TEST_CASES[@]}"; do
        ((test_count++))
        local start_time=$(date +%s)
        
        log_info "=== Test $test_count/${#TEST_CASES[@]}: $test_case ==="
        
        if ./build/bin/f3fix $test_case --first-sec=2048 --last-sec=102400 --boot "$device"; then
            log_success "Test passed: $test_case"
            ((passed_count++))
        else
            log_error "Test failed: $test_case"
        fi
        
        log_info "Current disk layout:"
        diskutil list "$device"
        fdisk "$device"
        
        local end_time=$(date +%s)
        local duration=$((end_time - start_time))
        log_info "Test completed in ${duration}s"
        echo "----------------------------------------"
    done
    
    local test_end_time=$(date +%s)
    local total_duration=$((test_end_time - test_start_time))
    local minutes=$((total_duration / 60))
    local seconds=$((total_duration % 60))
    
    log_info "Test summary: $passed_count/$test_count tests passed"
    log_info "Total time: ${minutes}m ${seconds}s"
    [ "$passed_count" -eq "$test_count" ] || log_error "Some tests failed"
}

main
