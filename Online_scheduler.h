#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <queue>
#include <string>
#include <sys/select.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;

#define MAX_PROCS 200
#define MAX_CMD_LEN 1000
#define MAX_HISTORY 50
#define POLL_SLEEP_MS 20 // ms poll granularity
#define MAX_UNIQUE_CMDS 200

static queue<int> q0arr, q1arr, q2arr;

struct CmdHistory {
  string cmd;
  vector<double> bursts = vector<double>(MAX_HISTORY, 0.0);
  int count = 0;
  int next_idx = 0;
};

struct OnlineProcess {
  string command;
  bool finished = false, error = false, started = false;
  pid_t pid = -1;
  uint64_t arrival_time = 0, completion_time = 0, turnaround_time = 0,
           waiting_time = 0, response_time = 0, total_cpu_time = 0;
  int history_index = -1;
  bool csv_written = false;
  uint64_t slice_start_ms = 0;
};

static struct timespec program_start_ts;

static void set_program_start_time() {
  clock_gettime(CLOCK_MONOTONIC, &program_start_ts);
}

static uint64_t now_ms() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  int64_t sec_diff = (int64_t)t.tv_sec - (int64_t)program_start_ts.tv_sec;
  int64_t ns_diff = (int64_t)t.tv_nsec - (int64_t)program_start_ts.tv_nsec;
  return (uint64_t)(sec_diff * 1000LL + ns_diff / 1000000LL);
}

static void record_burst_to_history(vector<CmdHistory> &cmd_history,
                                    int hist_idx, double burst_ms) {
  if (hist_idx < 0 || hist_idx >= static_cast<int>(cmd_history.size()))
    return;
  CmdHistory &h = cmd_history[hist_idx];
  h.bursts[h.next_idx] = burst_ms;
  h.next_idx = (h.next_idx + 1) % MAX_HISTORY;
  if (h.count < MAX_HISTORY)
    h.count++;
}

static double get_avg_burst_ms(const vector<CmdHistory> &cmd_history,
                               int hist_idx, int k) {
  if (hist_idx < 0 || hist_idx >= static_cast<int>(cmd_history.size()))
    return -1.0;
  const CmdHistory &h = cmd_history[hist_idx];
  if (h.count == 0)
    return -1.0;
  int to_take = (k <= 0) ? h.count : min(h.count, k);
  double sum = 0.0;
  int idx = (h.next_idx - 1 + MAX_HISTORY) % MAX_HISTORY;
  for (int i = 0; i < to_take; ++i) {
    sum += h.bursts[idx];
    idx = (idx - 1 + MAX_HISTORY) % MAX_HISTORY;
  }
  return sum / static_cast<double>(to_take);
}

inline void set_stdin_nonblocking(bool enable) {
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (flags == -1)
    return;
  if (enable)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
  fcntl(STDIN_FILENO, F_SETFL, flags);
}

inline int find_history_index(const vector<CmdHistory> &ch, const string &cmd) {
  for (int i = 0; i < static_cast<int>(ch.size()); ++i)
    if (ch[i].cmd == cmd)
      return i;
  return -1;
}

inline int ensure_history_index(vector<CmdHistory> &ch, const string &cmd) {
  int idx = find_history_index(ch, cmd);
  if (idx >= 0)
    return idx;
  CmdHistory newEntry;
  newEntry.cmd = cmd;
  ch.push_back(newEntry);
  return static_cast<int>(ch.size()) - 1;
}

inline void spawn_and_stop_child(OnlineProcess &p) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork failed");
    p.pid = -1; // Indicate failure
    return;
  }
  if (pid == 0) {
    setpgid(0, 0);
    raise(SIGSTOP);
    execl("/bin/sh", "sh", "-c", p.command.c_str(), (char *)NULL);
    perror("execl failed");
    _exit(127);
  } else {
    p.pid = pid;
  }
}

inline bool check_child_exited(pid_t pid, int *status_out) {
  int status;
  pid_t r = waitpid(pid, &status, WNOHANG);
  if (r == 0 || r == -1)
    return false;
  *status_out = status;
  return true;
}

inline void write_results_to_csv(const vector<OnlineProcess> &processes,
                                 const string &filename) {
  ofstream fp(filename);
  if (!fp) {
    cerr << "Could not open file " << filename << "\n";
    return;
  }
  fp << "Command,Finished,Error,CompletionTime,Turnaround,Waiting,Response,"
        "TotalCPU\n";
  for (const auto &p : processes) {
    fp << "\"" << p.command << "\"," << (p.finished ? "Yes" : "No") << ","
       << (p.error ? "Yes" : "No") << "," << p.completion_time << ","
       << p.turnaround_time << "," << p.waiting_time << "," << p.response_time
       << "," << p.total_cpu_time << "\n";
  }
}

inline int poll_and_enqueue_new_commands(vector<OnlineProcess> &proc_table,
                                         vector<CmdHistory> &cmd_history,
                                         uint64_t now) {
  static char buf[8192];
  static size_t leftover = 0;
  int added = 0;

  while (true) {
    ssize_t r = read(STDIN_FILENO, buf + leftover, sizeof(buf) - leftover - 1);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      else
        break;
    } else if (r == 0) {
      break;
    } else {
      leftover += static_cast<size_t>(r);
      buf[leftover] = '\0';
      char *line_start = buf;
      char *nl;

      while ((nl = strchr(line_start, '\n')) != nullptr) {
        size_t linelen = static_cast<size_t>(nl - line_start);
        while (linelen > 0 && (line_start[linelen - 1] == '\r' ||
                               line_start[linelen - 1] == '\n'))
          linelen--;

        string cmd(line_start, linelen);
        if (!cmd.empty()) {
          OnlineProcess p;
          p.command = cmd;
          p.arrival_time = now;
          p.history_index = ensure_history_index(cmd_history, cmd);

          spawn_and_stop_child(p);
          if (p.pid <= 0) {
            p.error = true;
            p.finished = true;
            p.completion_time = now_ms();
          }

          proc_table.push_back(p);
          added++;
        }
        line_start = nl + 1;
      }

      size_t remaining = strlen(line_start);
      memmove(buf, line_start, remaining);
      leftover = remaining;
      buf[leftover] = '\0';
    }
  }
  return added;
}

class OnlineScheduler {
public:
  OnlineScheduler() {
    set_program_start_time();
    program_start_ms = now_ms();
  }

  void ShortestJobFirst(int k);
  void MultiLevelFeedbackQueue(int q0, int q1, int q2, int boostTime);

private:
  vector<OnlineProcess> proc_table;
  vector<CmdHistory> cmd_histories;
  uint64_t program_start_ms;
};

void OnlineScheduler::ShortestJobFirst(int k) {
  set_stdin_nonblocking(true);
  poll_and_enqueue_new_commands(proc_table, cmd_histories, now_ms());

  while (true) {
    poll_and_enqueue_new_commands(proc_table, cmd_histories, now_ms());

    int active = 0;
    for (auto &p : proc_table)
      if (!p.finished)
        active++;

    if (active == 0) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(STDIN_FILENO, &rfds);
      int sel = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, nullptr);
      if (sel > 0)
        continue;
      if (sel == -1 && errno == EINTR)
        continue;
      break;
    }

    int best_idx = -1;
    double best_est = 1e308;

    for (int i = 0; i < (int)proc_table.size(); ++i) {
      auto &p = proc_table[i];
      if (p.finished)
        continue;
      double avg = get_avg_burst_ms(cmd_histories, p.history_index, k);
      double est = (avg < 0.0) ? 1000.0 : avg;
      if (est < best_est) {
        best_est = est;
        best_idx = i;
      }
    }

    if (best_idx == -1)
      break;
    OnlineProcess &job = proc_table[best_idx];
    if (job.pid == -1)
      spawn_and_stop_child(job);

    kill(-job.pid, SIGCONT);
    if (!job.started) {
      job.started = true;
      job.response_time = now_ms() - job.arrival_time;
    }

    uint64_t start = now_ms();
    while (true) {
      int status = 0;
      if (check_child_exited(job.pid, &status)) {
        uint64_t end = now_ms();
        uint64_t ran = end - start;
        job.finished = true;
        job.total_cpu_time += ran;
        job.completion_time = end;
        job.turnaround_time = job.completion_time - job.arrival_time;
        job.waiting_time = job.turnaround_time - job.total_cpu_time;
        record_burst_to_history(cmd_histories, job.history_index, (double)ran);
        break;
      }
      this_thread::sleep_for(chrono::milliseconds(50));
    }

    write_results_to_csv(proc_table, "result_online_SJF.csv");
  }

  set_stdin_nonblocking(false);
}

static void finalize_proc_metrics(OnlineProcess &p) {
  if (!p.finished)
    return;

  p.turnaround_time = p.completion_time - p.arrival_time;

  if (p.total_cpu_time > p.turnaround_time)
    p.waiting_time = 0;
  else
    p.waiting_time = p.turnaround_time - p.total_cpu_time;

  if (p.slice_start_ms > 0 && p.response_time == 0 && p.started) {
    p.response_time = p.slice_start_ms - p.arrival_time;
  }
}

static void complete_process(OnlineProcess &p, uint64_t end_ms,
                             bool wstatus_valid, int wstatus, ofstream &csv,
                             vector<CmdHistory> &cmd_history) {
  if (p.finished)
    return;

  p.finished = true;
  p.completion_time = end_ms;

  if (wstatus_valid) {
    if (WIFEXITED(wstatus))
      p.error = (WEXITSTATUS(wstatus) != 0);
    else
      p.error = true;
  }

  if (!p.error && p.history_index >= 0) {
    record_burst_to_history(cmd_history, p.history_index,
                            static_cast<double>(p.total_cpu_time));
  }

  finalize_proc_metrics(p);

  cout << "Context switch: " << p.command << " | Start: " << p.slice_start_ms
       << " | End: " << end_ms << "\n";

  csv << "\"" << p.command << "\"," << (p.finished ? "Yes" : "No") << ","
      << (p.error ? "Yes" : "No") << "," << p.completion_time << ","
      << p.turnaround_time << "," << p.waiting_time << "," << p.response_time
      << "," << p.total_cpu_time << "\n";
}

static void print_context_switch(const string &cmd, uint64_t start_ms,
                                 uint64_t end_ms) {
  cout << cmd << ", " << start_ms << ", " << end_ms << endl;
  cout.flush();
}

static void promote_all_to_q0(queue<int> &q0arr, queue<int> &q1arr,
                              queue<int> &q2arr, int max_procs,
                              const function<bool(int)> &is_queued) {
  vector<int> leftover_q1;
  while (!q1arr.empty()) {
    int idx = q1arr.front();
    q1arr.pop();
    if (!is_queued(idx))
      continue;
    if ((int)q0arr.size() < max_procs)
      q0arr.push(idx);
    else {
      leftover_q1.push_back(idx); // Could not promote, keep in q1
    }
  }
  queue<int> temp;
  for (int idx : leftover_q1)
    temp.push(idx);
  q1arr.swap(temp);

  // Promote all from q2 to q0
  vector<int> leftover_q2;
  while (!q2arr.empty()) {
    int idx = q2arr.front();
    q2arr.pop();
    if (!is_queued(idx))
      continue;
    if ((int)q0arr.size() < max_procs)
      q0arr.push(idx);
    else {
      leftover_q2.push_back(idx); // Could not promote, keep in q1
    }
  }
  queue<int> temp2;
  for (int idx : leftover_q2)
    temp2.push(idx);
  q1arr.swap(temp2);
}

inline bool is_queued(int idx) {
  auto in_queue = [&](queue<int> &q) {
    queue<int> copy = q;
    while (!copy.empty()) {
      if (copy.front() == idx)
        return true;
      copy.pop();
    }
    return false;
  };

  return in_queue(q0arr) || in_queue(q1arr) || in_queue(q2arr);
}

static void place_new_arrivals_mlfq(vector<OnlineProcess> &proc_table,
                                    vector<CmdHistory> &cmd_histories,
                                    queue<int> &q0arr, queue<int> &q1arr,
                                    queue<int> &q2arr, int q0_time, int q1_time,
                                    int q2_time,
                                    const function<bool(int)> &is_queued) {

  (void)q2_time;

  for (int i = 0; i < (int)proc_table.size(); ++i) {
    auto &p = proc_table[i];
    if (p.finished)
      continue;
    if (is_queued(i))
      continue;

    double avg = get_avg_burst_ms(cmd_histories, p.history_index, 3);
    int target = 1;

    if (avg > 0.0) {
      if ((double)q0_time >= avg)
        q0arr.push(i);
      else if ((double)q1_time >= avg)
        q1arr.push(i);
      else
        q2arr.push(i);
    } else {
      q1arr.push(i);
    }
  }
}

void OnlineScheduler::MultiLevelFeedbackQueue(int quantum0, int quantum1,
                                              int quantum2, int boostTime) {
  set_program_start_time();
  set_stdin_nonblocking(true);

  // queue<int> q0, q1, q2;

  int q[3] = {quantum0, quantum1, quantum2};
  poll_and_enqueue_new_commands(proc_table, cmd_histories, now_ms());
  uint64_t last_boost = now_ms();

  while (true) {
    poll_and_enqueue_new_commands(proc_table, cmd_histories, now_ms());
    place_new_arrivals_mlfq(proc_table, cmd_histories, q0arr, q1arr, q2arr,
                            q[0], q[1], q[2], is_queued);

    uint64_t cur = now_ms();

    // Priority Boost
    if (boostTime > 0 &&
        (cur - last_boost) >= static_cast<uint64_t>(boostTime)) {
      promote_all_to_q0(q0arr, q1arr, q2arr, MAX_PROCS, is_queued);
      last_boost = cur;
      cout << "Priority boost at " << last_boost << "\n";
    }

    int pick_q = -1;
    if (!q0arr.empty())
      pick_q = 0;
    else if (!q1arr.empty())
      pick_q = 1;
    else if (!q2arr.empty())
      pick_q = 2;
    else {
      if (count_if(proc_table.begin(), proc_table.end(),
                   [](const OnlineProcess &p) { return !p.finished; }) == 0) {
        fd_set rfds;
        while (count_if(proc_table.begin(), proc_table.end(),
                        [](const OnlineProcess &p) { return !p.finished; }) ==
               0) {
          FD_ZERO(&rfds);
          FD_SET(STDIN_FILENO, &rfds);
          int sel = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, nullptr);
          if (sel > 0) {
            poll_and_enqueue_new_commands(proc_table, cmd_histories, now_ms());
            place_new_arrivals_mlfq(proc_table, cmd_histories, q0arr, q1arr,
                                    q2arr, q[0], q[1], q[2], is_queued);
            break;
          } else if (sel == -1 && errno == EINTR)
            continue;
        }
        continue;
      } else {
        struct timespec ts {
          0, POLL_SLEEP_MS * 1000000L
        };
        nanosleep(&ts, nullptr);
        continue;
      }
    }

    int proc_idx = -1;
    if (pick_q == 0 && !q0arr.empty()) {
      proc_idx = q0arr.front();
      q0arr.pop();
    } else if (pick_q == 1 && !q1arr.empty()) {
      proc_idx = q1arr.front();
      q1arr.pop();
    } else if (pick_q == 2 && !q2arr.empty()) {
      proc_idx = q2arr.front();
      q2arr.pop();
    }
    while (proc_idx >= 0 && proc_table[proc_idx].finished) {
      if (pick_q == 0 && !q0arr.empty()) {
        proc_idx = q0arr.front();
        q0arr.pop();
      } else if (pick_q == 1 && !q1arr.empty()) {
        proc_idx = q1arr.front();
        q1arr.pop();
      } else if (pick_q == 2 && !q2arr.empty()) {
        proc_idx = q2arr.front();
        q2arr.pop();
      }
    }

    if (proc_idx < 0)
      continue;
    auto &p = proc_table[proc_idx];

    if (p.pid == -1) {
      spawn_and_stop_child(p);
      if (p.pid <= 0) {
        p.finished = true;
        p.error = true;
        p.completion_time = now_ms();

        continue;
      }
    }

    uint64_t start = now_ms();
    if (!p.started) {
      kill(-p.pid, SIGCONT);
      p.started = true;
      p.response_time = start - p.arrival_time;
    } else {
      kill(-p.pid, SIGCONT);
    }

    p.slice_start_ms = start;
    int slice_len_ms = q[pick_q];

    double est = get_avg_burst_ms(cmd_histories, p.history_index, 3);
    if (est > 0.0) {
      double rem_est = est - static_cast<double>(p.total_cpu_time);
      if (rem_est <= 0.0)
        slice_len_ms = POLL_SLEEP_MS;
      else
        slice_len_ms =
            std::max(POLL_SLEEP_MS,
                     static_cast<int>(std::min(rem_est, (double)slice_len_ms)));
    }

    bool finished_in_slice = false;
    int elapsed = 0;

    while (elapsed < slice_len_ms) {
      int to_sleep = std::min(slice_len_ms - elapsed, POLL_SLEEP_MS);
      struct timespec ts {
        0, to_sleep * 1000000L
      };
      nanosleep(&ts, nullptr);
      elapsed += to_sleep;
      poll_and_enqueue_new_commands(proc_table, cmd_histories, now_ms());

      int wstatus = 0;
      int r = check_child_exited(p.pid, &wstatus);
      if (r > 0 || r == -1) {
        uint64_t end = now_ms();
        uint64_t ran = end - start;
        p.total_cpu_time += ran;
        p.error = (r == -1);
        finished_in_slice = true;
        break;
      }

      if (pick_q > 0 && ((pick_q == 1 && !q0arr.empty()) ||
                         (pick_q == 2 && (!q0arr.empty() || !q1arr.empty()))))
        break;
    }

    if (!finished_in_slice) {
      uint64_t end = now_ms();
      uint64_t ran = end - start;
      int wstatus = 0;
      p.total_cpu_time += ran;
      kill(-p.pid, SIGSTOP);
      print_context_switch(p.command, start, end);
      if (elapsed >= slice_len_ms)
        (pick_q < 2 ? q1arr : q2arr).push(proc_idx);
      else
        (pick_q == 0 ? q0arr : (pick_q == 1 ? q1arr : q2arr)).push(proc_idx);
    }
  }

  write_results_to_csv(proc_table, "result_online_MLFQ.csv");
  set_stdin_nonblocking(false);
}