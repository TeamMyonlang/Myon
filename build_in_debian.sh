#!/bin/bash
# Build/run/test Myon inside the proot-distro Debian (glibc) environment.
# Usage: build_in_debian.sh [make args]
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
cd /data/data/com.termux/files/home/Myon
exec make "$@"
