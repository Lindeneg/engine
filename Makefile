.PHONY: generate build run test format clean

FORMAT_FILES := $(wildcard src/*.cpp src/*.h src/*/*.cpp src/*/*.h \
                           tests/*.cpp tests/*.h tests/*/*.cpp tests/*/*.h)

generate:
	cmake --preset default -DCL_MEM_STATS=ON

build:
	cmake --build build --target ExploreApp -j 18

run:
	./build/ExploreApp.exe

lines:
	cloc ./src/

test:
	cmake --preset default -DCL_BUILD_TESTS=ON -DCL_MEM_STATS=ON
	cmake --build build --target ExploreTests -j 18
	./build/ExploreTests.exe

format:
	clang-format -i $(FORMAT_FILES)

clean:
	rm -rf build compile_commands.json .cache
