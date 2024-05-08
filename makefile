out: bench_mark.cc ./src/*.cc
	g++ -o $@ $^ -std=c++11 -lpthread -m32
debug: bench_mark.cc ./src/*.cc
	g++ -o $@ $^ -std=c++11 -lpthread -DPROJECT_DEBUG -g -m32
unit: unit_test.cc ./src/*.cc
	g++ -o $@ $^ -std=c++11 -lpthread -DPROJECT_DEBUG -g -m32
.PHONY:clean
clean:
	rm -f out debug

# out: bench_mark.cc ./src/*.cc
# 	arm-linux-gnueabihf-g++ -o $@ $^ -std=c++11 -lpthread
# debug: bench_mark.cc ./src/*.cc
# 	arm-linux-gnueabihf-g++ -o $@ $^ -std=c++11 -lpthread -DPROJECT_DEBUG -g
# unit: unit_test.cc ./src/*.cc
# 	arm-linux-gnueabihf-g++ -o $@ $^ -std=c++11 -lpthread -DPROJECT_DEBUG -g
# .PHONY:clean
# clean:
# 	rm -f out debug unit
