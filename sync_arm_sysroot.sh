#!/bin/bash
# 最简单的同步脚本
rsync -av /home/forlinx/work/OK3506_Linux_Source/buildroot/output/rockchip_ok3506_emmc/host/ /home/forlinx/work/toolchain/

echo "同步完成"