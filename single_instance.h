// Single running instance per program, based on an flock()ed lock file that also stores the
// PID. The kernel releases the lock when the process exits (even when killed), so a stale lock
// file never blocks a new start.
#pragma once

#include <string>

class SingleInstance {
public:
    // Lock file: /tmp/<name>.lock
    explicit SingleInstance(const std::string& name);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // Becomes the running instance. If another instance is running it is asked to stop
    // (SIGTERM, SIGKILL after 5 s) first. Returns false with a message on stderr if that is
    // impossible (e.g. it belongs to another user).
    bool acquire();

    // For --stop: stops the running instance, if any. Returns the process exit code.
    static int stopRunning(const std::string& name);

private:
    std::string name_;
    std::string path_;
    int fd_ = -1;
};
