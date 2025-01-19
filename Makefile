run:
	g++ lexer.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o ast && ./ast < testfile | grep -v source_filename | grep -v datalayout > t.ll
	#llvm-as < t.ll | opt -passes=view-cfg 
	#opt -dot-cfg t.ll && cat .*.dot > alldot.dot
	#dot -Tpng alldot.dot -o abc.png
all:
	gcc -fPIC -c myfun.c -o mylib.o
	gcc -shared -o libmylib.so mylib.o
	g++ -ldl -ggdb3 lexer.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core orcjit native` -L./ -lmylib  -Wl,-rpath,./  -o ast&& ./ast < testfile 
