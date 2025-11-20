#!/bin/bash

# Компиляция
echo "Компиляция сервера..."
clang -O3 -march=native -o server server.c

# Увеличение лимитов
echo "Настройка системных лимитов..."
ulimit -n 12288

# Определение количества ядер
CORES=$(sysctl -n hw.ncpu)
echo "Обнаружено ядер: $CORES"

# Запуск процессов
echo "Запуск $CORES экземпляров сервера..."
for i in $(seq 1 $CORES); do
    ./server &
done

echo "Серверы запущены. PID процессов:"
pgrep server

echo ""
echo "Для остановки используйте: pkill server"