# Инструкция по установке и запуску на macOS

Подробное руководство по компиляции, настройке и запуску минимального веб-сервера на macOS.

## Требования

- macOS 10.12 (Sierra) или новее
- Xcode Command Line Tools или полная версия Xcode

## Установка инструментов разработки

### Установка Xcode Command Line Tools (рекомендуется)

```bash
xcode-select --install
```

После выполнения команды появится диалоговое окно. Нажмите "Install" и дождитесь завершения установки.

### Проверка установки

```bash
# Проверить наличие компилятора
gcc --version

# Или
clang --version
```

Вы должны увидеть информацию о версии компилятора.

## Компиляция

### Базовая компиляция с GCC

```bash
gcc -O3 -o server server.c
```

### Компиляция с Clang (рекомендуется для macOS)

```bash
clang -O3 -o server server.c
```

### Компиляция с максимальной оптимизацией

```bash
clang -O3 -march=native -o server server.c
```

### Компиляция для конкретной архитектуры

Для Apple Silicon (M1/M2/M3):
```bash
clang -O3 -arch arm64 -o server server.c
```

Для Intel Mac:
```bash
clang -O3 -arch x86_64 -o server server.c
```

Универсальный бинарник (работает на обеих архитектурах):
```bash
clang -O3 -arch arm64 -arch x86_64 -o server server.c
```

## Запуск сервера

### Базовый запуск

```bash
./server
```

Сервер запустится на порту 8080. Вы увидите сообщение:
```
Minimal web server listening on port 8080
Press Ctrl+C to stop
```

### Запуск на кастомном порту

```bash
./server 3000
```

### Запуск в фоновом режиме

```bash
./server &
```

Для просмотра запущенных процессов:
```bash
ps aux | grep server
```

### Запуск нескольких экземпляров

Благодаря SO_REUSEPORT можно запустить несколько процессов на одном порту для максимальной производительности:

```bash
# Автоматически определить количество ядер и запустить по процессу на ядро
for i in $(seq 1 $(sysctl -n hw.ncpu)); do ./server & done
```

Для остановки всех процессов:
```bash
pkill server
```

## Оптимизация системы для максимальной производительности

### Увеличение лимита открытых файлов

Проверить текущий лимит:
```bash
ulimit -n
```

Установить новый лимит для текущей сессии:
```bash
ulimit -n 12288
```

### Постоянное увеличение лимита файлов

Создайте файл `/Library/LaunchDaemons/limit.maxfiles.plist`:

```bash
sudo nano /Library/LaunchDaemons/limit.maxfiles.plist
```

Вставьте следующее содержимое:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
  <dict>
    <key>Label</key>
    <string>limit.maxfiles</string>
    <key>ProgramArguments</key>
    <array>
      <string>launchctl</string>
      <string>limit</string>
      <string>maxfiles</string>
      <string>65536</string>
      <string>200000</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>ServiceIPC</key>
    <false/>
  </dict>
</plist>
```

Установите правильные права:
```bash
sudo chown root:wheel /Library/LaunchDaemons/limit.maxfiles.plist
sudo chmod 644 /Library/LaunchDaemons/limit.maxfiles.plist
```

Загрузите конфигурацию:
```bash
sudo launchctl load -w /Library/LaunchDaemons/limit.maxfiles.plist
```

### Настройка сетевых параметров

Увеличить размер backlog очереди:
```bash
sudo sysctl -w kern.ipc.somaxconn=4096
```

Проверить текущие настройки:
```bash
sysctl kern.ipc.somaxconn
```

Для постоянного применения создайте `/etc/sysctl.conf`:
```bash
sudo nano /etc/sysctl.conf
```

Добавьте:
```
kern.ipc.somaxconn=4096
```

## Тестирование производительности

### Установка инструментов для тестирования

#### Apache Bench (обычно уже установлен)

```bash
# Проверить наличие
ab -V
```

#### Установка wrk через Homebrew

```bash
# Установить Homebrew (если не установлен)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Установить wrk
brew install wrk
```

#### Установка hey через Homebrew

```bash
brew install hey
```

### Тестирование с Apache Bench

Базовый тест:
```bash
ab -n 100000 -c 100 http://localhost:8080/
```

Расширенный тест:
```bash
ab -n 500000 -c 1000 -k http://localhost:8080/
```

### Тестирование с wrk

Базовый тест (4 потока, 100 соединений, 30 секунд):
```bash
wrk -t4 -c100 -d30s http://localhost:8080/
```

Интенсивный тест (8 потоков, 1000 соединений, 60 секунд):
```bash
wrk -t8 -c1000 -d60s http://localhost:8080/
```

### Тестирование с hey

```bash
hey -n 1000000 -c 200 -q 0 http://localhost:8080/
```

## Мониторинг производительности

### Мониторинг использования CPU

```bash
# Использование CPU процессом
top -pid $(pgrep server)

# Или с помощью Activity Monitor (графический интерфейс)
open -a "Activity Monitor"
```

### Мониторинг сетевой активности

```bash
# Просмотр сетевых соединений
netstat -an | grep 8080

# Или более детально
lsof -i :8080
```

### Использование Instruments

Для детального анализа производительности:

```bash
# Запустить сервер с профилированием
instruments -t "Time Profiler" -D ./profile.trace ./server
```

## Ожидаемая производительность на macOS

### MacBook Pro / iMac (Intel)

- **Один процесс**: 40,000 - 80,000 RPS
- **Несколько процессов** (по одному на ядро): 150,000 - 300,000 RPS

### MacBook Pro / Mac Studio (Apple Silicon M1/M2/M3)

- **Один процесс**: 60,000 - 120,000 RPS
- **Несколько процессов** (по одному на ядро): 300,000 - 600,000+ RPS

*Производительность Apple Silicon значительно выше благодаря эффективной архитектуре и большому количеству ядер.*

## Автоматизация

### Создание скрипта запуска

Создайте файл `start.sh`:

```bash
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
```

Сделайте скрипт исполняемым:
```bash
chmod +x start.sh
```

Запустите:
```bash
./start.sh
```

### Создание скрипта остановки

Создайте файл `stop.sh`:

```bash
#!/bin/bash

echo "Остановка всех процессов сервера..."
pkill server

if [ $? -eq 0 ]; then
    echo "Серверы остановлены"
else
    echo "Процессы сервера не найдены"
fi
```

Сделайте скрипт исполняемым:
```bash
chmod +x stop.sh
```

## Решение проблем

### Ошибка "Address already in use"

```bash
# Найти процесс, использующий порт
lsof -i :8080

# Остановить процесс
kill -9 <PID>
```

### Ошибка "Permission denied" при запуске на порту < 1024

Порты ниже 1024 требуют root-привилегий:

```bash
sudo ./server 80
```

Рекомендуется использовать порты >= 1024 для разработки.

### Компилятор не найден

```bash
# Установить Command Line Tools
xcode-select --install

# Проверить путь к инструментам
xcode-select -p
```

## Дополнительные ресурсы

- [Документация по сетевому программированию на macOS](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/NetworkingOverview/)
- [Оптимизация производительности на macOS](https://developer.apple.com/documentation/performance)
- [Apple Developer Documentation](https://developer.apple.com/documentation/)

## Полезные команды

```bash
# Проверить архитектуру системы
uname -m

# Информация о процессоре
sysctl -n machdep.cpu.brand_string

# Количество ядер
sysctl -n hw.ncpu

# Количество физических ядер
sysctl -n hw.physicalcpu

# Общая память
sysctl -n hw.memsize

# Версия macOS
sw_vers
```
