Note: This file is a mirror of /readme.md file.

# Hospital System Simulation 🏥

A robust, multi-process **Hospital System Simulation** written in **C (Linux)** that showcases core Operating Systems concepts: **process orchestration**, **POSIX threads**, **System V IPC** (Message Queues + Shared Memory + Semaphores), and **signal-driven shutdown**.

## Nota: 18,4

---

## 📋 Overview 

This project models a hospital as a set of cooperating processes and worker threads:

- **Manager (main process)** 🧠
  - Boots the system, loads configuration, creates IPC resources, and forks the subsystems.
  - Accepts user commands via a **named pipe** (`input_pipe`) and routes them to subsystems.
  - Monitors subsystem feedback asynchronously.

- **Triage** 🩺
  - Receives emergencies and appointments.
  - Manages internal queues and prioritization.

- **Surgery / Operating Block** 🏨
  - Manages surgeries, dependencies (tests/meds), and operating room scheduling.

- **Laboratory** 🧪
  - Executes lab tests using worker threads, respecting configured capacity.

- **Pharmacy** 💊
  - Prepares medication deliveries using worker threads and shared-stock synchronization.

### Communication Flow (high level) 🔄

- **Manager → Subsystems:** System V **Message Queues** (requests/events).
- **Subsystems → Manager:** System V **Message Queues** (notifications/results).
- **Shared state:** System V **Shared Memory** (stats, logs, inventories, shared state).
- **Resource limits:** **Semaphores** (e.g., lab equipment slots, pharmacy access, medical teams).
- **User input:** **Named pipe** (`input_pipe`) for commands.

---

## 🚀 Features & Technical Highlights 

### Architecture: Multi-process design (`fork()`) 🧩
- The Manager process forks child processes for **TRIAGE**, **SURGERY**, **PHARMACY**, and **LAB**.
- Each subsystem runs its own event loop and handles messages independently.

### IPC: System V Message Queues + Shared Memory + Semaphores 📮
- **Message Queues** carry typed messages (requests, results, shutdown notifications - poison pills).
- **Shared Memory** stores simulation state and statistics accessible across processes.
- **Semaphores** enforce physical constraints (e.g., limited rooms/teams/equipment).

### Concurrency: POSIX threads (`pthreads`) 🧵
- **Laboratory** uses a **fixed thread pool** (5 workers by default) with a **producer-consumer job queue**. Workers are joinable and are gracefully shut down when the system terminates.
- **Pharmacy** uses **detached worker threads** to process requests concurrently.
- Thread lifecycle is designed for safe shutdown and cleanup.

### Stability: Thread Pool & Backpressure patterns (high load protection) 🛡️
- **Laboratory**: Implements a **fixed thread pool** pattern. A dispatcher receives lab requests and enqueues them into a job queue. A fixed number of worker threads consume jobs from the queue, providing natural backpressure when all workers are busy.
- **Pharmacy**: Implements a **maximum concurrency cap** (default **200**) for worker threads. If the subsystem is at capacity, request spawning **waits** until a worker finishes (condition-variable based backpressure).
- Both patterns prevent unbounded thread creation and improve stability under stress.

### Synchronization: mutexes + condition variables ✅
- Shared counters, queues, and state transitions are guarded via **mutexes**.
- **Condition variables** coordinate:
  - backpressure (waiting for capacity), and
  - shutdown-aware cleanup (waiting for workers to drain).

---

## 🛠️ Installation & Compilation 🧰

### Prerequisites
- Linux 🐧
- `gcc` (with pthread support)
- `make`
- (optional) `valgrind` for memory/race analysis

### Compile
```bash
make
```

### Debug build
```bash
make debug
```

### Clean
```bash
make clean
```

### IPC cleanup (useful after crashes)
```bash
make ipc_clean
```

---

## ▶️ How to Run

From the project directory:
```bash
./hospital_system
```

### Configuration file ⚙️
- The system loads configuration from:
  - `config/config.cfg`
- It controls:
  - simulation time unit,
  - queue capacities,
  - operating room durations,
  - lab/pharmacy parameters,
  - initial medication stock.

### Sending commands (via `input_pipe`) 🧾
The Manager listens for commands through the FIFO `input_pipe`.

Example (in another terminal):
```bash
echo "STATUS ALL" > input_pipe
```
or with a file:
```bash
cat "file.txt" > input_pipe
```

Common commands include:
- `STATUS ALL`
- `EMERGENCY ...`
- `APPOINTMENT ...`
- `SURGERY ...`
- `LAB_REQUEST ...`
- `PHARMACY_REQUEST ...`

Note that you can also send commands directly through the terminal running the program.

### Graceful shutdown 🛑
- Recommended: send a shutdown command through the pipe:
```bash
echo "SHUTDOWN" > input_pipe
```
- Or press `CTRL+C` in the running terminal.

---

## 🧪 Testing & Validation (Crucial)

### Stress testing
The project includes Python-based stress generators and shell runners:

- Generate + inject commands:
```bash
./tests/runners/run_test.sh global
```

Available stress modes:
- `triage`
- `surgery`
- `lab_pharm`
- `pharmacy_restock`
- `global`
- `all`

Note: the runner expects the system to already be running because it writes into `input_pipe`.

### Memory safety with Valgrind (Memcheck)
A Memcheck target is available in the Makefile:
```bash
make mem
```

A clean Memcheck run should end with:
- `ERROR SUMMARY: 0 errors`
- `definitely lost: 0 bytes in 0 blocks`

### Thread safety with Valgrind (Helgrind / DRD)
Targets are also provided:
```bash
make helgrind
make drd
```

### Detached threads and shutdown cleanup
- Pharmacy worker threads are **detached**; Lab uses **joinable** threads in a fixed pool.
- The system tracks active workers and uses condition-variable signaling to:
  - limit concurrency (Pharmacy) or queue depth (Lab), and
  - allow shutdown to wait for workers to finish (with timeouts to avoid deadlocks).

---

## 📂 Directory Structure

```text
hospital_system/
├── Makefile
├── hospital_system              # compiled binary (after `make`)
├── input_pipe                   # FIFO created at runtime
├── config/
│   ├── config.cfg
│   └── ipc.txt
├── include/                     # public headers
├── src/                         # C implementation
├── logs/
│   └── hospital_log.log         # runtime logs (after run)
├── results/
│   ├── lab_results/
│   ├── pharmacy_deliveries/
│   └── stats_snapshots/
├── tests/
│   ├── generators/              # python stress generators
│   ├── generated/               # generated command files
│   └── runners/                 # shell runners
└── valgrind/                    # memcheck / helgrind / drd logs
```

---

## 👥 Authors

- **Hugo Afonso**
- **Mateus Silva**
- **Rodrigo Martins**
- **Tiago Bento**

# ---

# Hospital System Simulation 🏥

Uma **Simulação de Sistema Hospitalar** robusta, multi-processo, escrita em **C (Linux)**, que demonstra conceitos nucleares de Sistemas Operativos: **orquestração de processos**, **POSIX threads**, **IPC System V** (Message Queues + Shared Memory + Semaphores) e **shutdown orientado por sinais**.

---

## 📋 Visão Geral

Este projeto modela um hospital como um conjunto de processos cooperantes e threads trabalhadoras:

- **Manager (processo principal)** 🧠  
  - Arranca o sistema, carrega a configuração, cria recursos de IPC e faz `fork()` dos subsistemas.  
  - Aceita comandos do utilizador através de um **pipe nomeado** (`input_pipe`) e encaminha-os para os subsistemas.  
  - Monitoriza feedback dos subsistemas de forma assíncrona.

- **Triagem** 🩺  
  - Recebe emergências e consultas.  
  - Gere filas internas e priorização.

- **Cirurgia / Bloco Operatório** 🏨  
  - Gere cirurgias, dependências (testes/medicação) e o escalonamento das salas operatórias.

- **Laboratório** 🧪  
  - Executa testes laboratoriais recorrendo a threads trabalhadoras, respeitando a capacidade configurada.

- **Farmácia** 💊  
  - Prepara entregas de medicação usando threads trabalhadoras e sincronização de stock partilhado.

### Fluxo de Comunicação (alto nível) 🔄

- **Manager → Subsistemas:** **Message Queues System V** (pedidos/eventos).  
- **Subsistemas → Manager:** **Message Queues System V** (notificações/resultados).  
- **Estado partilhado:** **Shared Memory System V** (estatísticas, logs, inventário, estado global).  
- **Limites de recursos:** **Semáforos** (ex.: equipamentos de laboratório, acesso à farmácia, equipas médicas).  
- **Input do utilizador:** **Pipe nomeado** (`input_pipe`) para comandos.

---

## 🚀 Funcionalidades & Destaques Técnicos

### Arquitetura: Design multi-processo (`fork()`) 🧩
- O processo Manager cria processos filhos para **TRIAGE**, **SURGERY**, **PHARMACY** e **LAB**.  
- Cada subsistema corre o seu próprio ciclo de eventos e trata mensagens de forma independente.

### IPC: Message Queues + Shared Memory + Semaphores (System V) 📮
- **Message Queues** transportam mensagens tipadas (pedidos, resultados, notificações de shutdown – *poison pills*).  
- **Shared Memory** armazena o estado da simulação e estatísticas acessíveis entre processos.  
- **Semáforos** impõem restrições físicas reais (ex.: salas, equipas, equipamentos).

### Concorrência: POSIX threads (`pthreads`) 🧵
- **Laboratório** utiliza um **pool fixo de threads** (5 workers por defeito) com uma **fila de trabalhos produtor-consumidor**. Os workers são *joinable* e são encerrados de forma graciosa quando o sistema termina.
- **Farmácia** utiliza threads trabalhadoras destacadas (*detached*) para processamento concorrente.
- O ciclo de vida das threads é desenhado para permitir shutdown e limpeza seguros.

### Estabilidade: Padrões Thread Pool & Backpressure (proteção sob carga elevada) 🛡️
- **Laboratório**: Implementa um padrão de **pool fixo de threads**. Um dispatcher recebe os pedidos e coloca-os numa fila de trabalhos. Um número fixo de threads trabalhadoras consome trabalhos da fila, proporcionando backpressure natural quando todos os workers estão ocupados.
- **Farmácia**: Implementa um **limite máximo de concorrência** (por defeito **200**) para threads trabalhadoras. Quando o subsistema atinge a capacidade máxima, a criação de novas threads **fica em espera** até que uma termine (backpressure com *condition variables*).
- Ambos os padrões evitam a criação ilimitada de threads e melhoram a estabilidade sob stress.

### Sincronização: mutexes + condition variables ✅
- Contadores partilhados, filas e transições de estado são protegidos por **mutexes**.  
- **Condition variables** coordenam:  
  - backpressure (espera por capacidade), e  
  - limpeza consciente do shutdown (espera pelo término dos workers).

---

## 🛠️ Instalação & Compilação

### Pré-requisitos
- Linux 🐧
- `gcc` (com pthread)
- `make`
- (opcional) `valgrind` para análise de memória e concorrência

### Compilar
```bash
make
```

### Debug
```bash
make debug
```

### Limpar
```bash
make clean
```

### Limpeza de IPC (útil após crash) 🧼
```bash
make ipc_clean
```

---

## ▶️ Como Executar

No diretório do projeto:
```bash
./hospital_system
```

### Ficheiro de configuração ⚙️
- O sistema carrega:
  - `config/config.cfg`

Controla, por exemplo:
- unidade de tempo,
- limites de filas,
- tempos de cirurgia,
- parâmetros de laboratório/farmácia,
- stock inicial de medicamentos.

### Enviar comandos (via `input_pipe`) 🧾
O gestor central recebe os comandos através do FIFO `input_pipe`.

Exemplo (noutro terminal):
```bash
echo "STATUS ALL" > input_pipe
```
ou com um ficheiro:
```bash
cat "ficheiro.txt" > input_pipe
```

Comandos mais utlizados:
- `STATUS ALL`
- `EMERGENCY ...`
- `APPOINTMENT ...`
- `SURGERY ...`
- `LAB_REQUEST ...`
- `PHARMACY_REQUEST ...`

Nota: também é possível enviar comandos diretos pelo terminal onde o programa está a correr.

### Encerramento gracioso 🛑
- Recomendado:
```bash
echo "SHUTDOWN" > input_pipe
```
- Alternativamente: `CTRL+C`.

---

## 🧪 Testes & Validação (Crucial)

### Stress testing
Inclui geradores Python e runners:
```bash
./tests/runners/run_test.sh global
```

Modos disponíveis:
- `triage`, `surgery`, `lab_pharm`, `pharmacy_restock`, `global`, `all`

Nota: o runner assume que o sistema já está a correr, porque escreve no `input_pipe`.

### Segurança de memória com Valgrind (Memcheck)
```bash
make mem
```

Uma execução limpa deve terminar com:
- `ERROR SUMMARY: 0 errors`
- `definitely lost: 0 bytes in 0 blocks`

### Segurança de threads com Valgrind (Helgrind / DRD)
```bash
make helgrind
make drd
```

### Threads detached e limpeza no shutdown
- Os workers da Farmácia são **detached**; o Laboratório usa threads **joinable** num pool fixo.
- O sistema mantém contagem de workers e usa condition variables para:
  - limitar concorrência (Farmácia) ou profundidade da fila (Laboratório), e
  - coordenar o shutdown de forma segura.

---

## 📂 Estrutura do Projeto

```text
hospital_system/
├── Makefile
├── hospital_system              # binário compilado (após make)
├── input_pipe                   # FIFO criado em runtime
├── config/
│   ├── config.cfg
│   └── ipc.txt
├── include/                     # ficheios .h
├── src/                         # ficheiros .c
├── logs/
│   └── hospital_log.log         # runtime logs (depois e durante a execução)
├── results/
│   ├── lab_results/
│   ├── pharmacy_deliveries/
│   └── stats_snapshots/
├── tests/
│   ├── generators/              # geradores stress python
│   ├── generated/               # ficheiros de comandos gerados
│   └── runners/                 # scripts shell com os testes
└── valgrind/                    # memcheck / helgrind / drd logs
```

---

## 👥 Autores

- **Hugo Afonso**
- **Mateus Silva**
- **Rodrigo Martins**
- **Tiago Bento**
