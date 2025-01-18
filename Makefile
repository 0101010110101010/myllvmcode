run:
	g++ lexer.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o ast && ./ast < testfile | grep -v source_filename | grep -v datalayout > t.ll
	#llvm-as < t.ll | opt -passes=view-cfg 
	#opt -dot-cfg t.ll && cat .*.dot > alldot.dot
	#dot -Tpng alldot.dot -o abc.png
