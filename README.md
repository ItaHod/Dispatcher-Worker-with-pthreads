# 🧵 Dispatcher-Worker Threading System

## 📄 Overview

This program implements a robust **dispatcher/worker model** using **Linux pthreads**. The dispatcher reads commands from a file and assigns them to worker threads via a **shared job queue**. Worker threads process these jobs in parallel, updating **counter files** and **logs** as required. The program ensures proper synchronization using **mutexes** and **condition variables** to prevent race conditions.

## ✨ Features

- **Job Queue Management**: Commands for worker threads are stored in a thread-safe queue.
- **Configurable Worker Pool**: Supports a configurable number of threads to process jobs concurrently.
- **Counter Files**: Worker commands can update counter files with proper synchronization.
- **Comprehensive Statistics**: Computes and records job turnaround times and various performance metrics.
- **Detailed Logging**:
  - `dispatcher.txt`: Logs commands read by the dispatcher.
  - `threadXX.txt`: Logs start and end times of jobs processed by each thread.
- **Flexible Command System**: Supports both dispatcher commands and worker commands.

## 🔢 Command Line Arguments

```bash
./dispatcher <command_file> <num_threads> <num_counters> <log_enabled>
```

- **command_file**: Path to the file containing commands.
- **num_threads**: Number of worker threads to create.
- **num_counters**: Number of counter files to initialize.
- **log_enabled**: Set to `1` to enable logging, `0` to disable.

## 📚 Command Syntax

### Dispatcher Commands

- `dispatcher_msleep x`: Pauses the dispatcher for `x` milliseconds.
- `dispatcher_wait`: Blocks until all worker jobs in the queue have been processed.

### Worker Commands

- `worker msleep x`: Worker sleeps for `x` milliseconds.
- `worker increment x`: Increments the value in counter file `countXX.txt`.
- `worker decrement x`: Decrements the value in counter file `countXX.txt`.
- `worker repeat x; cmd1; cmd2; ...`: Repeats the commands after the semicolon `x` times.

📌 **Commands can be chained with semicolons:**

```bash
worker msleep 100; increment 5; decrement 2
```

## 🔄 Program Flow

### 1. Initialization

- Parse command line arguments.
- Create and initialize counter files to `0` (`count00.txt` through `countXX.txt`).
- Set up synchronization primitives (**mutexes** and **condition variables**).
- Create worker threads.

### 2. Dispatcher

- Read commands from the input file line by line.
- Process **dispatcher commands** immediately.
- Enqueue **worker commands** into the shared job queue.
- Record command read time for statistics.

### 3. Worker Threads

- Continuously check the job queue for new jobs.
- Execute jobs by parsing and processing command sequences.
- Update statistics and logs as jobs start and complete.
- Support **repeat commands** for job sequences.

