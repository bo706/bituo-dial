# =============================================================================
# CMakeLists.txt - Bituo Dial (BLE 多表采集显示网关) 项目级构建脚本
# 目标硬件: M5Stack Dial (ESP32-S3FN8)
# 框架版本: ESP-IDF v5.2.3
# 创建日期: 2026-09-01
# =============================================================================
cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 构建系统（必须在 project() 之前）
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(bituo-dial)
