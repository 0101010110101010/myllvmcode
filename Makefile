run:
	g++ lexer.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o ast && ./ast < testfile
