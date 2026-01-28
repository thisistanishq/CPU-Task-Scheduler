# CPU Scheduling Simulator in C++

This repository features a comprehensive simulator for CPU scheduling algorithms implemented in modern C++17.  
It is designed for educational purposes, interview preparation, and in-depth understanding of operating system scheduling techniques.

---

## Features

### Offline Scheduling Algorithms
- First-Come-First-Serve (FCFS)  
- Round Robin (RR)  
- Multi-Level Feedback Queue (MLFQ)
- Priority Scheduling (Non-preemptive)  

Offline schedulers simulate fixed process lists for deterministic analysis and testing.

### Online Scheduling Algorithms
- Shortest Job First (SJF) with burst history prediction  
- Multi-Level Feedback Queue (MLFQ) with dynamic job arrivals and priority boosting  

Online schedulers mimic real-time systems with processes arriving during execution.

---

## Usage

### Running the Simulator

Clone the repository and simply run: ./main (main.cpp)
 
`g++ main.cpp -std=c++17 -o main`
`./main`

### Running Unit Tests

To verify the correctness of the scheduling algorithms:

`g++ tests.cpp -std=c++17 -o tests`
`./tests`

### Interactive Testing

- For online schedulers, enter shell commands line by line (e.g., `sleep 1`, `ls`, `echo Hello`).
- Terminate input by pressing `Ctrl+D` to signal end of commands.
- Results are saved in CSV files (e.g., `result_online_SJF.csv`, `result_online_MLFQ.csv`) for analysis.

---

## Design Highlights

- Modular, header-only C++17 implementations for ease of integration.
- Uses POSIX system calls (`fork`, `waitpid`, `kill`) for realistic process simulation.
- Adaptive burst time prediction enhances Shortest Job First scheduling.
- Multi-Level Feedback Queue scheduler with priority boost and aging.
- Priority Scheduling with higher numerical priority values executing first.
- Real-time command polling via non-blocking stdin.
- Detailed metrics and CSV output for performance benchmarking.

---

## Skills Demonstrated

- Operating Systems fundamentals (process scheduling, signals)  
- Modern C++ programming with STL 
- Real-time and adaptive scheduling strategies  
- Performance analysis and benchmarking with logging and CSV exports  

---

## Contribution

Contributions welcome! Opening pull requests for new schedulers, bug fixes, or feature improvements is encouraged.

---





