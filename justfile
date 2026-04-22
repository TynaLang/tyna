default: run

configure:
  cmake -B build -S .

build: configure
  cmake --build build

# run tyna (optionally pass args)
run *args: build
  ./build/tyna {{args}}

# run with a test file
test: build
  ./build/tyna examples/test.tn

# emit LLVM IR
ir: build
  ./build/tyna -emit-ir examples/test.tn

# emit object file
obj: build
  ./build/tyna -emit-obj examples/test.tn

debug: 
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
  cmake --build build

release: 
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
  cmake --build build

clean: 
  rm -rf build

rebuild: clean build
