#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file    serial_capture.py
@brief   M5Dial 串口日志抓取工具（Phase 0~5 验收通用）
         打开指定串口 -> DTR/RTS 硬件复位 -> 抓取 N 秒日志 -> 同时落盘。
         不依赖 idf.py monitor（monitor 为交互式，不适合自动化验收）。
@usage   python tools/serial_capture.py COM7 15
         第一个参数：串口号（默认 COM7）
         第二个参数：抓取秒数（默认 15）
@version ESP-IDF v5.2.3 配套工具 / Python 3 + pyserial
@date    2026-09-01
"""
import sys
import time
from pathlib import Path

import serial  # pyserial，随 ESP-IDF Python 环境或 pip install pyserial 获得


def capture(port_name: str, seconds: float, baud: int = 115200) -> str:
    """复位开发板并抓取指定时长的串口文本，返回完整字符串。"""
    # timeout=0.2s：让读取循环能周期性检查是否到时
    ser = serial.Serial(port=port_name, baudrate=baud, timeout=0.2)
    time.sleep(0.1)

    # ESP32-S3 USB-Serial/JTAG 复位时序：RTS 脉冲拉低 EN
    ser.dtr = False
    ser.rts = True          # EN=0，芯片保持复位
    time.sleep(0.1)
    ser.rts = False         # EN=1，芯片启动
    # 释放后立即清空复位期间的杂散字节
    ser.reset_input_buffer()

    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
    ser.close()

    # 串口日志为 UTF-8，个别杂散字节用 replace 兜底，避免崩溃丢日志
    return buf.decode("utf-8", errors="replace")


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

    print(f"[capture] port={port} duration={seconds}s, resetting board ...")
    text = capture(port, seconds)

    # 控制台输出
    print(text)

    # 落盘到 tools/captures/ 下，文件名带端口与时间戳，便于验收留档
    out_dir = Path(__file__).resolve().parent / "captures"
    out_dir.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out_file = out_dir / f"boot_{port}_{stamp}.log"
    out_file.write_text(text, encoding="utf-8")
    print(f"[capture] saved -> {out_file}  ({len(text)} chars)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
