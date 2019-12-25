all : clean web_proxy

web_proxy: main.o
	g++ -g -o web_proxy web_proxy.o -pthread

main.o:
	g++ -g -c -o web_proxy.o web_proxy.cpp

clean:
	rm -f web_proxy
	rm -f *.o
