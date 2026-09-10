#!/bin/bash

mkdir -p target

build() {
	gcc src/main.c -o target/main -g -Wall -Wpedantic
}

run() {
	time target/main
}

watch() {
	while true; do
		clear
		build && run
		inotifywait -e modify src/*.c
	done
}

case "$1" in
	"build") build;;
	"run") build && run;;
	"watch") watch;;
	*) build;;
esac