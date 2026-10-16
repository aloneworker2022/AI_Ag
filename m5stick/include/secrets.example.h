// 出廠預設值範本。
// 複製成 secrets.h（已被 .gitignore，不會進版控）再改成你自己的。
// 沒有 secrets.h 也能編譯：會用這份範本，開機後再用「設定模式」網頁填。
//
// 這裡的值只在「裝置裡還沒有存過設定」時使用；
// 之後在設定網頁改過的內容會存在裝置的 NVS，不會被重新燒錄覆蓋
// （想重置就在設定網頁按「恢復出廠預設」）。
#pragma once

// 裝置名稱：Pi 用這個名字辨識裝置（/devices、/devices/<名稱>/notify）
#define DEFAULT_DEVICE_NAME "stick"

// Pi 的位址。可以是 IP（推薦，在路由器固定 Pi 的 IP）或 mDNS 名稱（xxx.local）
#define DEFAULT_SERVER_HOST "raspberrypi.local"
#define DEFAULT_SERVER_PORT 8000

// 要跟 Pi 上 config.yaml 的 device.token 一樣
#define DEFAULT_TOKEN "change-me"

// 設定模式時，Stick 自己開的熱點密碼（至少 8 碼）
#define SETUP_AP_PASSWORD "aistick123"

// 預設 Wi-Fi 清單，排越前面越優先。
// 第三欄 server_host 可留空 = 用上面的 DEFAULT_SERVER_HOST。
// 用手機熱點時 Pi 的 IP 通常不一樣（Pi 也要連到同一個熱點），就填在這裡。
#define DEFAULT_NETWORKS                                  \
  {                                                       \
    {"MyHomeWiFi", "home-password", ""},                  \
    {"MyPhoneHotspot", "hotspot-password", "172.20.10.2"}, \
  }
