#!/bin/sh
set -eu
cd "$(dirname "$0")"
binary=$(mktemp /tmp/mlx90640-test.XXXXXX)
trap 'rm -f "$binary"' EXIT
cc -std=c11 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all \
    -I stubs -I ../include -I ../private test_driver.c ../mosaico_thermal.c ../MLX90640_API.c ../MLX90640_I2C_Driver.c -lm -o "$binary"
"$binary"
