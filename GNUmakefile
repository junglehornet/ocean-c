# Convenience entry point; CMake owns the generated Makefile.
BUILD_DIR ?= build
VERSION ?= dev
export VERSION COMMIT COMMIT_DATE COMMIT_YEAR BUILD_DATE
CMAKE_ARGS ?=

.PHONY: all configure ocean ocean-c test clean install
all: configure
	cmake --build "$(BUILD_DIR)"
configure:
	cmake -S . -B "$(BUILD_DIR)" $(CMAKE_ARGS)
ocean ocean-c: configure
	cmake --build "$(BUILD_DIR)" --target $@
test: all
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure
clean: configure
	cmake --build "$(BUILD_DIR)" --target clean
install: all
	cmake --install "$(BUILD_DIR)"
