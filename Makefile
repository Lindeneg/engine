.PHONY: generate build run format clean

generate:
	cmake --preset default

build:
	cmake --build build --target ExploreApp -j 18

run:
	./build/ExploreApp.exe

format:
	clang-format -i ./src/*.cpp

clean:
	rm -rf build compile_commands.json .cache