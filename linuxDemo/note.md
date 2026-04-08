# 只编译主程序
cmake -DBUILD_MAIN=ON -DBUILD_SINGLE_THREAD=OFF -DBUILD_TEST2=OFF ..

# 只编译单线程程序
cmake -DBUILD_MAIN=OFF -DBUILD_SINGLE_THREAD=ON -DBUILD_TEST2=OFF ..

# 只编译测试程序 memmantest
cmake -DBUILD_MAIN=OFF -DBUILD_SINGLE_THREAD=OFF -DBUILD_TEST2=ON ..

# 编译主程序和单线程程序
cmake -DBUILD_MAIN=ON -DBUILD_SINGLE_THREAD=ON -DBUILD_TEST2=OFF ..

# 编译所有（主程序、单线程、memmantest）
cmake -DBUILD_MAIN=ON -DBUILD_SINGLE_THREAD=ON -DBUILD_TEST2=ON -DBUILD_TEST_REMOTECMD=ON ..

# 编译主程序和 memmantest
cmake -DBUILD_MAIN=ON -DBUILD_SINGLE_THREAD=OFF -DBUILD_TEST2=ON ..

# img_trans函数参数
./img_trans -c 640 480 25 yuyv -s 0 -o 1 rtsp://192.168.1.3:8554/live?codec=h264 -r http://192.168.1.3:5000?device_id=1