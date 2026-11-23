cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kvm_hub)
target_sources(app PRIVATE src/main.c src/hid_dev.c src/cfg_svc.c src/nfc.c src/ringlog.c)
