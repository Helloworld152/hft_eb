pkill hft_engine
sleep 15
cd bin
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:.
./hft_engine ../conf/config_real_test.yaml
