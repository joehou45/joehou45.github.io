#!/usr/bin/env python3
"""
控制 Raspberry Pi Zero (W) 板載 LED (綠色 ACT 燈, /sys/class/leds/led0)

用法:
    sudo python3 led_control.py on            # 恆亮
    sudo python3 led_control.py off           # 熄滅
    sudo python3 led_control.py blink          # 持續閃爍 (Ctrl+C 停止)
    sudo python3 led_control.py blink -n 10    # 閃 10 下後結束
    sudo python3 led_control.py blink -i 0.2   # 間隔 0.2 秒
    sudo python3 led_control.py restore        # 還原系統預設 (活動指示)

說明:
    板載 LED 平常由系統控制 (trigger)，要手動控制必須先把 trigger 設為 none。
    Pi Zero 這顆 LED 是反相的 (active-low)：寫 0 會亮、寫 255 會暗，
    本程式已內建處理，on 就是亮、off 就是暗。
    結束 blink / off 時可用 restore 把燈交還給系統。
"""

import argparse
import sys
import time
import signal

LED_PATH = "/sys/class/leds/led0"
TRIGGER = LED_PATH + "/trigger"
BRIGHTNESS = LED_PATH + "/brightness"

# Pi Zero 板載 ACT LED 為反相：brightness 0 = 亮, 255 = 暗
ON_VALUE = 0
OFF_VALUE = 255

# blink 結束後要還原的系統預設 trigger（Pi Zero W 為 mmc0 磁碟活動）
DEFAULT_TRIGGER = "mmc0"


def write(path, value):
    try:
        with open(path, "w") as f:
            f.write(str(value))
    except PermissionError:
        sys.exit("需要 root 權限，請用 sudo 執行：sudo python3 " + sys.argv[0] + " ...")
    except FileNotFoundError:
        sys.exit("找不到 " + path + "，此板可能沒有可控制的板載 LED。")


def set_manual():
    """切換成手動控制模式（停用系統 trigger）。"""
    write(TRIGGER, "none")


def led_on():
    set_manual()
    write(BRIGHTNESS, ON_VALUE)


def led_off():
    set_manual()
    write(BRIGHTNESS, OFF_VALUE)


def led_restore():
    """把 LED 交還系統（恢復活動指示燈）。"""
    write(TRIGGER, DEFAULT_TRIGGER)


def led_blink(count, interval):
    set_manual()
    # count <= 0 代表無限閃爍
    n = 0
    try:
        while count <= 0 or n < count:
            write(BRIGHTNESS, ON_VALUE)
            time.sleep(interval)
            write(BRIGHTNESS, OFF_VALUE)
            time.sleep(interval)
            n += 1
    except KeyboardInterrupt:
        pass
    finally:
        write(BRIGHTNESS, OFF_VALUE)


def main():
    p = argparse.ArgumentParser(description="控制 Pi Zero 板載 LED (led0)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("on", help="恆亮")
    sub.add_parser("off", help="熄滅")
    sub.add_parser("restore", help="還原系統預設活動指示")
    b = sub.add_parser("blink", help="閃爍")
    b.add_argument("-n", "--count", type=int, default=0,
                   help="閃幾下 (預設 0 = 無限，Ctrl+C 停止)")
    b.add_argument("-i", "--interval", type=float, default=0.5,
                   help="亮/暗間隔秒數 (預設 0.5)")

    args = p.parse_args()

    if args.cmd == "on":
        led_on()
        print("LED 恆亮")
    elif args.cmd == "off":
        led_off()
        print("LED 熄滅")
    elif args.cmd == "restore":
        led_restore()
        print("已還原系統預設 (活動指示)")
    elif args.cmd == "blink":
        where = "無限" if args.count <= 0 else str(args.count) + " 下"
        print("LED 閃爍 " + where + "，間隔 " + str(args.interval) + " 秒")
        led_blink(args.count, args.interval)
        print("閃爍結束 (LED 已熄滅，可用 restore 交還系統)")


if __name__ == "__main__":
    main()
