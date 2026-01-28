#include "Offline_scheduler.h"
#include "Online_scheduler.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace std;

void reset_process_state(std::vector<Process> &processes) {
  for (auto &p : processes) {
    p.finished = false;
    p.error = false;
    p.start_time = 0;
    p.completion_time = 0;
    p.turnaround_time = 0;
    p.waiting_time = 0;
    p.response_time = 0;
    p.started = false;
    p.process_id = -1;
    // Keep priority same
  }
}

int main() {
  std::vector<Process> processes = {
      {"ls", false, false, 0, 0, 0, 0, 0, false, -1, 1},         // Priority 1
      {"echo Hello", false, false, 0, 0, 0, 0, 0, false, -1, 2}, // Priority 2
      {"sleep 1", false, false, 0, 0, 0, 0, 0, false, -1, 3}     // Priority 3
  };

  reset_process_state(processes);
  FCFS(processes);

  reset_process_state(processes);
  RoundRobin(processes, 500); // Time quantum of 500 ms

  reset_process_state(processes);
  MultiLevelFeedbackQueue(processes, 500, 1000, 2000, 4000);

  reset_process_state(processes);
  cout << "=== Priority Scheduling (Higher number = Higher Priority) ===\n";
  PriorityScheduling(processes);

  cout << "All Offline scheduling algorithms executed.\n";

  cout << "=== Online Shortest Job First (SJF) Test ===\n";
  cout << "Enter shell commands (one per line). Example:\n";
  cout << "  sleep 1\n  echo Hello\n  ls -l\n";
  cout << "(Press Ctrl+D or close stdin to stop)\n\n";

  OnlineScheduler scheduler;
  //  Run the SJF algorithm with k = 3 (average of last 3 bursts)
  scheduler.ShortestJobFirst(3);
  scheduler.MultiLevelFeedbackQueue(500, 1000, 2000, 4000);

  return 0;
}
