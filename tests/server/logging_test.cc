#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "server/trace.h"
#include "server/storage/file.h"

using namespace std;

static void Check(bool condition, const char *message) {
    if (!condition) {
        cerr << message << endl;
        exit(EXIT_FAILURE);
    }
}

static string ReadFile(const string &name) {
    ifstream input(name.c_str());
    return string((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
}

static string FindLog(const string &directory, const string &prefix) {
    DIR *files = opendir(directory.c_str());
    Check(files != NULL, "Could not list logging test files");
    string result;
    for (dirent *entry = readdir(files); entry != NULL; entry = readdir(files)) {
        string name = entry->d_name;
        if (name.compare(0, prefix.size(), prefix) == 0 &&
            name.size() >= 4 && name.compare(name.size() - 4, 4, ".log") == 0) {
            result = directory + "/" + name;
            break;
        }
    }
    closedir(files);
    return result;
}

int main() {
    char directory[] = "/tmp/esajpip-log-XXXXXX";
    Check(mkdtemp(directory) != NULL, "Could not create logging test directory");
    string base = string(directory) + "/server";
    Check(server::trace::Initialize(base), "Could not initialize logging");

    LOG("first message");
    LOG("second message");
    server::trace::Flush();

    thread worker([] {
        LOG("worker message");
    });
    worker.join();
    server::trace::Flush();

    LOG(string(2000, 'x'));
    server::trace::Flush();
    LOG("message after rollover");
    server::trace::Flush();

    string active = FindLog(directory, "server.");
    Check(!active.empty(), "Active log was not created");
    string backup = active + ".1";

    string old_messages = ReadFile(backup);
    string new_messages = ReadFile(active);
    Check(old_messages.find("first message") != string::npos,
          "First message missing from rolled log");
    Check(old_messages.find("first message\n") != string::npos,
          "Log line contains trailing characters");
    Check(old_messages.find("second message") != string::npos,
          "Second message missing from rolled log");
    Check(old_messages.find("worker message") != string::npos,
          "Worker message missing from rolled log");
    Check(old_messages.find(string(1900, 'x') + "...\n") != string::npos,
          "Oversized log message was not marked as truncated");
    Check(new_messages.find("message after rollover") != string::npos,
          "Message missing after rollover");

    unlink(backup.c_str());
    Check(mkdir(backup.c_str(), 0700) == 0, "Could not obstruct log rollover");
    LOG(string(1100, 'x'));
    server::trace::Flush();
    int active_fd = open(active.c_str(), O_RDONLY);
    Check(active_fd >= 0, "Could not open the disabled log");
    off_t disabled_size = lseek(active_fd, 0, SEEK_END);
    close(active_fd);
    Check(disabled_size >= 0, "Could not measure the disabled log");

    LOG("message after failed rollover");
    server::trace::Flush();
    active_fd = open(active.c_str(), O_RDONLY);
    Check(active_fd >= 0, "Could not reopen the disabled log");
    Check(lseek(active_fd, 0, SEEK_END) == disabled_size,
          "Logging continued after rollover failure");
    close(active_fd);

    server::trace::Drain();
    Check(rmdir(backup.c_str()) == 0, "Could not remove rollover obstruction");

    string shutdown_base = string(directory) + "/shutdown";
    Check(server::trace::Initialize(shutdown_base),
          "Could not reinitialize logging for shutdown");
    LOG("message queued before shutdown");
    server::trace::Drain();
    string shutdown_log = FindLog(directory, "shutdown.");
    Check(!shutdown_log.empty() &&
                  ReadFile(shutdown_log).find("message queued before shutdown") !=
                      string::npos,
          "Logger shutdown did not drain queued records");

    Check(server::trace::Initialize(string(directory) + "/escaped"),
          "Could not initialize file-path logging");
    server::File missing;
    string path = string(directory) + "/missing\nFORGED\r\t.jp2";
    Check(missing.Open(path.c_str(), 1024) == server::File::OpenResult::NOT_FOUND,
          "Missing control-character path did not report not found");
    server::trace::Drain();
    string escaped_log = FindLog(directory, "escaped.");
    Check(!escaped_log.empty(), "File-path log was not created");
    string escaped_message = ReadFile(escaped_log);
    Check(escaped_message.find("missing\\nFORGED\\r\\t.jp2'") != string::npos &&
                  count(escaped_message.begin(), escaped_message.end(), '\n') == 1 &&
                  escaped_message.find('\r') == string::npos &&
                  escaped_message.find('\t') == string::npos,
          "File-path control characters split or altered the log record");

    // Queue saturation: the child logs to a pipe that is not read until its
    // producers are done, so the logger blocks on the full pipe, the queue
    // fills, and messages are dropped; then the pipe is read to the end.
    int saturated_output[2], burst_done[2];
    Check(pipe(saturated_output) == 0 && pipe(burst_done) == 0,
          "Could not create the queue saturation test pipes");
    pid_t producer = fork();
    Check(producer >= 0, "Could not fork the queue saturation test");
    if (producer == 0) {
        close(saturated_output[0]);
        close(burst_done[0]);
        if (dup2(saturated_output[1], STDOUT_FILENO) < 0)
            _exit(2);
        close(saturated_output[1]);
        if (!server::trace::Initialize(""))
            _exit(3);
        vector<thread> producers;
        for (int thread_index = 0; thread_index < 8; ++thread_index) {
            producers.emplace_back([thread_index] {
                for (int i = 0; i < 2000; ++i)
                    LOG("concurrent message " << thread_index << ':' << i);
            });
        }
        for (thread &producer_thread : producers)
            producer_thread.join();
        char done = 0;
        if (write(burst_done[1], &done, 1) != 1)
            _exit(4);
        close(burst_done[1]);
        server::trace::Flush();
        LOG("message after queue saturation");
        server::trace::Drain();
        _exit(0);
    }
    close(saturated_output[1]);
    close(burst_done[1]);
    char done;
    Check(read(burst_done[0], &done, 1) == 1,
          "The queue saturation test did not finish its producers");
    close(burst_done[0]);
    string saturated;
    char buffer[4096];
    for (;;) {
        ssize_t n = read(saturated_output[0], buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        saturated.append(buffer, n);
    }
    close(saturated_output[0]);
    int producer_status;
    Check(waitpid(producer, &producer_status, 0) == producer &&
                  WIFEXITED(producer_status) && WEXITSTATUS(producer_status) == 0,
          "The queue saturation test failed");
    size_t summary = saturated.find(" dropped\n");
    Check(summary != string::npos, "Dropped log messages were not reported");
    Check(saturated.find("message after queue saturation", summary) != string::npos,
          "Logging did not recover after queue saturation");

    int broken_output[2];
    Check(pipe(broken_output) == 0,
          "Could not create the failed-output test pipe");
    close(broken_output[0]);
    pid_t child = fork();
    Check(child >= 0, "Could not fork the failed-output test");
    if (child == 0) {
        signal(SIGPIPE, SIG_IGN);
        if (dup2(broken_output[1], STDOUT_FILENO) < 0)
            _exit(2);
        if (broken_output[1] != STDOUT_FILENO)
            close(broken_output[1]);
        if (!server::trace::Initialize(""))
            _exit(3);
        LOG("message to closed standard output");
        server::trace::Flush();
        bool stdout_open = fcntl(STDOUT_FILENO, F_GETFD) >= 0;
        server::trace::Drain();
        _exit(stdout_open ? 0 : 4);
    }
    close(broken_output[1]);
    int child_status;
    Check(waitpid(child, &child_status, 0) == child &&
                  WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "A logging failure closed standard output");

    unlink(backup.c_str());
    unlink(active.c_str());
    unlink(shutdown_log.c_str());
    unlink(escaped_log.c_str());
    Check(rmdir(directory) == 0, "Logging test left temporary files");
    return EXIT_SUCCESS;
}
