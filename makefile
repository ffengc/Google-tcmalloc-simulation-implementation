test:*.cc ./thread_cache/*.cc ./central_cache/*.cc
	g++ -o $@ $^ -std=c++11 -lpthread
.PHONY:clean
clean:
	rm -f test