default: run

configure:
    cmake -B build -S .

build: configure
    cmake --build build

# run tyl (optionally pass args)
run *args:
    ./build/tyl {{args}}

# run with a test file
test: build
    ./build/tyl examples/test.tyl

# emit LLVM IR
ir: build
    ./build/tyl -emit-ir examples/test.tyl

# emit object file
obj: build
    ./build/tyl -emit-obj examples/test.tyl

debug:
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
    cmake --build build

release:
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
    cmake --build build

clean:
    rm -rf build

rebuild: clean build
