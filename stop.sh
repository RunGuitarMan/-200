#!/bin/bash

echo "Остановка всех процессов сервера..."
pkill server

if [ $? -eq 0 ]; then
    echo "Серверы остановлены"
else
    echo "Процессы сервера не найдены"
fi