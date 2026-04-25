# Minimal C Web Server

Сверхпростой высокопроизводительный веб-сервер на языке C, который отвечает `200 OK` на любой HTTP запрос. Создан для измерения максимальной пропускной способности (RPS) системы.

## Особенности

- Минимальная обработка запросов
- Предподготовленный HTTP ответ
- Keep-Alive и HTTP pipelining
- Edge-triggered epoll (Linux) / kqueue (macOS/BSD)
- TCP_NODELAY, TCP_FASTOPEN
- SO_REUSEPORT для многопроцессного режима
- Встроенный счётчик RPS
- Graceful shutdown по SIGINT/SIGTERM
- Нет зависимостей (только стандартная библиотека C)

## Компиляция

```bash
make
```

Или вручную:

```bash
# Linux
gcc -O3 -march=native -o server server.c

# macOS
clang -O3 -march=native -o server server.c
```

## Запуск

```bash
# Порт по умолчанию (8080), один воркер
./server

# Кастомный порт
./server -p 3000
./server 3000          # legacy синтаксис

# Многопроцессный режим (4 воркера)
./server -w 4

# Кастомный размер ответа (для тестирования throughput)
./server -s 1024

# Комбинирование опций
./server -p 3000 -w 8 -s 512
```

### Быстрый старт — максимальный RPS

```bash
make && ./server -w 2    # пиковая конфигурация: ~450K RPS
./stop.sh                # остановка через PID-файл
```

### Автоматический запуск (macOS)

```bash
./start.sh    # собирает, определяет кол-во ядер, запускает с -w N
./stop.sh     # останавливает через PID-файл
```

## Опции

| Опция | Описание | По умолчанию |
|-------|----------|-------------|
| `-p PORT` | Порт для прослушивания | 8080 |
| `-w N` | Количество worker-процессов | 1 |
| `-s SIZE` | Размер тела ответа в байтах | 2 |
| `-h` | Показать справку | - |

## Встроенный RPS-счётчик

Сервер автоматически выводит в stderr количество обработанных запросов в секунду:

```
[pid 12345] RPS: 85000  total: 340000
[pid 12346] RPS: 82000  total: 328000
```

## Тестирование производительности

Максимальная производительность (Apple M3 Pro): **451K RPS**.
Подробные результаты и анализ: **[BENCHMARK.md](BENCHMARK.md)**

Рекомендуется использовать **wrk** — он написан на C с нативным event loop и поддерживает HTTP pipelining (отправляет пачки запросов не дожидаясь ответов). `hey` использует Go `net/http`, который не поддерживает pipelining и добавляет overhead на goroutine scheduling — в наших тестах это даёт потолок ~220K RPS против ~450K у wrk при том же сервере.

```bash
# Оптимальный запуск (пиковый RPS)
./server -w 2

# Нагрузка (wrk, рекомендуется)
wrk -t3 -c600 -d15s http://localhost:8080/

# Нагрузка (hey, ~2x медленнее из-за отсутствия pipelining)
hey -c 400 -z 10s http://localhost:8080/

# Apache Bench
ab -n 100000 -c 100 -k http://localhost:8080/
```

## Остановка сервера

```bash
# Через скрипт (использует PID-файл)
./stop.sh

# Или Ctrl+C в терминале
# Или kill по PID из server.pid
kill $(cat server.pid)
```

## Оптимизация системы

### Linux

```bash
ulimit -n 65535
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
```

### macOS

```bash
ulimit -n 12288
sudo sysctl -w kern.ipc.somaxconn=4096
```

## Ожидаемая производительность

На Apple M3 Pro (12 cores, 18 GB):
- **Один воркер** (`-w 1`): ~310,000 RPS (стабильно, минимальная latency)
- **Два воркера** (`-w 2`): **~450,000 RPS** (пиковая конфигурация)
- **4+ воркеров**: снижение из-за конкуренции за CPU с клиентом бенчмарка

## Структура HTTP ответа

```
HTTP/1.1 200 OK
Content-Length: 2
Connection: keep-alive
Keep-Alive: timeout=60, max=1000
Date: Fri, 25 Apr 2026 12:00:00 GMT

OK
```

## Лицензия

Публичный домен. Используйте как хотите.

## Примечания

- Этот сервер создан исключительно для бенчмарков
- Не используйте в продакшене
- Нет обработки ошибок HTTP
- Нет логирования запросов
