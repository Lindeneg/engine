.PHONY: generate build run test format clean

generate:
	cmake --preset default

build:
	cmake --build build --target ExploreApp -j 18

run:
	./build/ExploreApp.exe

test:
	cmake --preset default -DCL_BUILD_TESTS=ON
	cmake --build build --target ExploreTests -j 18
	./build/ExploreTests.exe

format:
	clang-format -i ./src/*.cpp ./src/*.h ./tests/*.cpp ./tests/*.h

clean:
	rm -rf build compile_commands.json .cache
