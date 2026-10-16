// 設定模式：Stick 自己開一個 Wi-Fi 熱點，手機連上後打開 http://192.168.4.1
// 就能新增 / 刪除 / 排序 Wi-Fi，以及改 Pi 位址、token、裝置名稱。
// 進入後不會返回；按 A 重新開機套用設定。
#pragma once

[[noreturn]] void runSetupPortal();
