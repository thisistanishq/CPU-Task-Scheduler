#include "Offline_scheduler.h"
#include <cassert>
#include <iostream>
#include <vector>

void test_priority_scheduling() {
  std::cout << "Running Priority Scheduling Test...\n";
  std::vector<Process> processes = {
      {"echo Low", false, false, 0, 0, 0, 0, 0, false, -1, 1},
      {"echo High", false, false, 0, 0, 0, 0, 0, false, -1, 10},
      {"echo Medium", false, false, 0, 0, 0, 0, 0, false, -1, 5}};

  // Run Priority Scheduling
  PriorityScheduling(processes);

  // Sort logic in function sorts by desc priority (10, 5, 1)
  // Check if they finished (simple check)
  for (const auto &p : processes) {
    assert(p.finished == true);
    assert(p.error == false);
  }

  // Verify completion times verify order roughly (process 1 starts before 2)
  // processes vector is resorted inside the function?
  // Wait, the function sorts the vector passed by reference!
  // So the first element should be High (10), then Medium (5), then Low (1).
  assert(processes[0].priority == 10);
  assert(processes[1].priority == 5);
  assert(processes[2].priority == 1);

  std::cout << "Priority Scheduling Test Passed!\n";
}

void test_fcfs() {
  std::cout << "Running FCFS Test...\n";
  std::vector<Process> processes = {{"echo First"}, {"echo Second"}};
  FCFS(processes);
  assert(processes[0].finished);
  assert(processes[1].finished);
  std::cout << "FCFS Test Passed!\n";
}

int main() {
  test_fcfs();
  test_priority_scheduling();
  std::cout << "All Tests Passed Successsfully!\n";
  return 0;
}
