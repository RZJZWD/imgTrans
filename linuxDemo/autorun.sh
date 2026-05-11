# user command
# cd /usr/bin/lvgl/
# /usr/bin/lvgl/demo_lvgl &
#fltest_wifi.sh -i wlan0 -s "CU_5qN2" -p b3pje6jm &

# 自动连接网络，udhcpc开启较早看不到分配的ip
killall udhcpc 2>/dev/null
udhcpc -i wlan0

cd /userdata
# /userdata/run_launcher.sh ./img_trans "1"

exit 0

