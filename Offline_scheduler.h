#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <queue>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace std;
struct Process {
  string command;
  bool finished = false;
  bool error = false;
  uint64_t start_time = 0;
  uint64_t completion_time = 0;
  uint64_t turnaround_time = 0;
  uint64_t waiting_time = 0;
  uint64_t response_time = 0;
  bool started = false;
  int process_id = -1;
  int priority = 0;
};

inline uint64_t get_current_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

inline void parse_command(const string &command, vector<char *> &argv,
                          vector<string> &tokens) {
  istringstream iss(command);
  string tok;
  while (iss >> tok)
    tokens.push_back(tok);
  argv.clear();
  for (auto &s : tokens)
    argv.push_back(const_cast<char *>(s.c_str()));
  argv.push_back(nullptr);
}

inline void write_results_to_csv(const vector<Process> &processes,
                                 const string &filename) {
  ofstream fp(filename);
  if (!fp) {
    cerr << "Could not open file " << filename << "\n";
    return;
  }
  fp << "Command,Finished,Error,CompletionTime,Turnaround,Waiting,Response\n";
  for (const auto &proc : processes) {
    fp << "\"" << proc.command << "\"," << (proc.finished ? "Yes" : "No") << ","
       << (proc.error ? "Yes" : "No") << "," << proc.completion_time << ","
       << proc.turnaround_time << "," << proc.waiting_time << ","
       << proc.response_time << "\n";
  }
}
struct SchedulerProcess {
  Process *p;
  int current_queue = 0;
  uint64_t total_cpu_time = 0;
  pid_t pid = -1;
};

inline void FCFS(vector<Process> &processes) {
  uint64_t scheduler_start = get_current_time_ms();
  for (auto &proc : processes) {
    vector<string> tokens;
    vector<char *> argv;
    parse_command(proc.command, argv, tokens);

    proc.start_time = get_current_time_ms() - scheduler_start;
    proc.start_time = get_current_time_ms() - scheduler_start;
    pid_t pid = fork();
    if (pid < 0) {
      cerr << "Fork failed for process " << proc.command << "\n";
      proc.error = true;
      continue;
    }
    if (pid == 0) {
      execvp(argv[0], argv.data());
      perror("execvp failed");
      exit(1);
    } else if (pid > 0) {
      proc.process_id = pid;
      proc.started = true;
      int status;
      waitpid(pid, &status, 0);
      proc.completion_time = get_current_time_ms() - scheduler_start;
      proc.finished = WIFEXITED(status) && (WEXITSTATUS(status) == 0);
      proc.error = !proc.finished;
      proc.turnaround_time = proc.completion_time - proc.start_time;
      proc.waiting_time = proc.turnaround_time;
      proc.response_time = proc.start_time;
    }
  }
  write_results_to_csv(processes, "result_offline_FCFS.csv");
}

void RoundRobin(vector<Process> &processes, int quantum_ms) {
  auto scheduler_start = get_current_time_ms();
  vector<uint64_t> total_cpu_times(processes.size(), 0);
  queue<int> ready_queue;
  int completed = 0;
  for (int i = 0; i < processes.size(); ++i)
    ready_queue.push(i);

  while (completed < (int)processes.size()) {
    if (ready_queue.empty()) {
      this_thread::sleep_for(chrono::milliseconds(1));
      continue;
    }
    int idx = ready_queue.front();
    ready_queue.pop();
    pid_t pid = processes[idx].process_id;
    auto start_t = get_current_time_ms();
    if (pid == -1) {
      processes[idx].started = true;
      processes[idx].response_time = start_t - scheduler_start;
      vector<string> tokens;
      vector<char *> argv;
      parse_command(processes[idx].command, argv, tokens);
      pid = fork();
      if (pid < 0) {
        cerr << "Fork failed (RR) for process " << processes[idx].command
             << "\n";
        processes[idx].error = true;
        // If fork fails, maybe we mark it as finished/error and don't requeue?
        // For simplicity, let's just mark it done with error.
        processes[idx].finished = true;
        completed++;
        continue;
      }
      if (pid == 0) {
        execvp(argv[0], argv.data());
        perror("execvp failed");
        exit(1);
      }
      processes[idx].process_id = pid;
    } else {
      kill(pid, SIGCONT);
    }
    this_thread::sleep_for(chrono::milliseconds(quantum_ms));
    int status;
    int wait_ret = waitpid(pid, &status, WNOHANG);
    auto end_t = get_current_time_ms();
    total_cpu_times[idx] += (end_t - start_t);
    if (wait_ret == pid) {
      completed++;
      processes[idx].completion_time = end_t - scheduler_start;
      processes[idx].finished = WIFEXITED(status) && (WEXITSTATUS(status) == 0);
      processes[idx].error = !processes[idx].finished;
    } else if (wait_ret == 0) {
      kill(pid, SIGSTOP);
      ready_queue.push(idx);
    }
    // If wait_ret == -1: process is done/gone, do not requeue.
  }
  for (int i = 0; i < (int)processes.size(); ++i) {
    processes[i].turnaround_time = processes[i].completion_time;
    processes[i].waiting_time =
        processes[i].turnaround_time - total_cpu_times[i];
  }
  write_results_to_csv(processes, "result_offline_RR.csv");
}

void MultiLevelFeedbackQueue(vector<Process> &processes, int quantum0,
                             int quantum1, int quantum2, int boostTime) {

  uint64_t scheduler_start = get_current_time_ms();
  uint64_t last_boost_time = scheduler_start;
  int completed_count = 0;
  vector<SchedulerProcess> sp;
  for (auto &proc : processes)
    sp.push_back(SchedulerProcess{&proc, 0, 0, -1});

  queue<int> q0, q1, q2;
  int quantums[] = {quantum0, quantum1, quantum2};
  for (int i = 0; i < processes.size(); ++i)
    q0.push(i);

  while (completed_count < processes.size()) {
    uint64_t now = get_current_time_ms();
    if (now - last_boost_time > (uint64_t)boostTime) {
      while (!q1.empty()) {
        int idx = q1.front();
        q1.pop();
        sp[idx].current_queue = 0;
        q0.push(idx);
      }
      while (!q2.empty()) {
        int idx = q2.front();
        q2.pop();
        sp[idx].current_queue = 0;
        q0.push(idx);
      }
      last_boost_time = now;
    }

    int idx = -1, active_queue = -1;
    if (!q0.empty()) {
      idx = q0.front();
      q0.pop();
      active_queue = 0;
    } else if (!q1.empty()) {
      idx = q1.front();
      q1.pop();
      active_queue = 1;
    } else if (!q2.empty()) {
      idx = q2.front();
      q2.pop();
      active_queue = 2;
    } else {
      this_thread::sleep_for(chrono::milliseconds(1));
      continue;
    }

    uint64_t start_t = get_current_time_ms();
    pid_t pid = sp[idx].pid;
    if (pid == -1) {
      sp[idx].p->started = true;
      sp[idx].p->response_time = start_t - scheduler_start;
      vector<string> tokens;
      vector<char *> argv;
      parse_command(sp[idx].p->command, argv, tokens);
      pid = fork();
      if (pid < 0) {
        cerr << "Fork failed (MLFQ) for process " << sp[idx].p->command << "\n";
        sp[idx].p->error = true;
        sp[idx].p->finished = true;
        completed_count++;
        continue;
      }
      if (pid == 0) {
        execvp(argv[0], argv.data());
        perror("execvp failed");
        exit(1);
      }
      sp[idx].pid = pid;
    } else {
      kill(pid, SIGCONT);
    }

    uint64_t slice_deadline = start_t + quantums[active_queue];
    uint64_t actual_end_t = start_t;
    bool finished_within_quantum = false;
    int status;
    while ((actual_end_t = get_current_time_ms()) < slice_deadline) {
      int wait_ret = waitpid(pid, &status, WNOHANG);
      if (wait_ret == pid) {
        finished_within_quantum = true;
        break;
      }
      this_thread::sleep_for(chrono::milliseconds(1));
    }
    if (!finished_within_quantum)
      actual_end_t = get_current_time_ms();
    uint64_t burst = actual_end_t - start_t;
    sp[idx].total_cpu_time += burst;

    if (finished_within_quantum) {
      completed_count++;
      sp[idx].p->completion_time = actual_end_t - scheduler_start;
      sp[idx].p->finished = WIFEXITED(status) && (WEXITSTATUS(status) == 0);
      sp[idx].p->error = !sp[idx].p->finished;
    } else {
      kill(pid, SIGSTOP);
      if (active_queue < 2)
        sp[idx].current_queue++;
      if (sp[idx].current_queue == 1)
        q1.push(idx);
      else
        q2.push(idx);
    }
  }
  for (auto &s : sp) {
    s.p->turnaround_time = s.p->completion_time;
    s.p->waiting_time = s.p->turnaround_time - s.total_cpu_time;
  }
  write_results_to_csv(processes, "result_offline_MLFQ.csv");
}

inline void PriorityScheduling(vector<Process> &processes) {
  // Sort descending by priority
  sort(processes.begin(), processes.end(),
       [](const Process &a, const Process &b) {
         return a.priority > b.priority;
       });

  uint64_t scheduler_start = get_current_time_ms();
  for (auto &proc : processes) {
    vector<string> tokens;
    vector<char *> argv;
    parse_command(proc.command, argv, tokens);

    proc.start_time = get_current_time_ms() - scheduler_start;
    pid_t pid = fork();
    if (pid < 0) {
      cerr << "Fork failed (Priority) for process " << proc.command << "\n";
      proc.error = true;
      continue;
    }
    if (pid == 0) {
      execvp(argv[0], argv.data());
      perror("execvp failed");
      exit(1);
    } else if (pid > 0) {
      proc.process_id = pid;
      proc.started = true;
      int status;
      waitpid(pid, &status, 0);
      proc.completion_time = get_current_time_ms() - scheduler_start;
      proc.finished = WIFEXITED(status) && (WEXITSTATUS(status) == 0);
      proc.error = !proc.finished;
      proc.turnaround_time = proc.completion_time - proc.start_time;
      proc.waiting_time = proc.turnaround_time;
      // In non-preemptive priority, waiting time is simply start time (assuming
      // all arrived at 0) But we track start_time as when it actually ran. If
      // arrival is 0, then waiting = start_time.
      proc.response_time = proc.start_time;
    }
  }
  write_results_to_csv(processes, "result_offline_Priority.csv");
}
