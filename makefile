test: *.cc ./src/*.cc
	g++ -o $@ $^ -std=c++11 -lpthread -DPROJECT_DEBUG
.PHONY:clean
clean:
	rm -f test