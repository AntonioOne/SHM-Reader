#include "single_instance.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

std::string lockPath(const std::string& name)
{
    return "/tmp/" + name + ".lock";
}

// Opens the lock file shared by all users. Existing files are opened without O_CREAT, because
// Linux' protected_regular forbids O_CREAT on another user's file in sticky /tmp.
int openLockFile(const std::string& path, bool create)
{
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0 || errno != ENOENT || !create)
        return fd;
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd >= 0) {
        fchmod(fd, 0666); // let other users take over / stop it as well
        return fd;
    }
    return errno == EEXIST ? open(path.c_str(), O_RDWR | O_CLOEXEC) : -1;
}

bool tryLock(int fd)
{
    return flock(fd, LOCK_EX | LOCK_NB) == 0;
}

pid_t readPid(int fd)
{
    char buf[32] = {};
    const ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    return n > 0 ? static_cast<pid_t>(std::atol(buf)) : 0;
}

void writePid(int fd)
{
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%d\n", static_cast<int>(getpid()));
    if (ftruncate(fd, 0) == 0 && pwrite(fd, buf, size_t(n), 0) < 0)
        std::perror("lock file");
}

bool waitForLock(int fd, int ms)
{
    for (int waited = 0; waited < ms; waited += 50) {
        if (tryLock(fd))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return tryLock(fd);
}

// Asks the instance holding the lock to stop and waits until the lock is free (it is then
// held by us). Returns false with a message if that does not work.
bool stopHolder(int fd, const char* prog)
{
    const pid_t pid = readPid(fd);
    if (pid > 0 && kill(pid, SIGTERM) != 0 && errno == EPERM) {
        std::fprintf(stderr, "%s: instance PID %d belongs to another user; stop it with: sudo kill %d\n", prog,
                     static_cast<int>(pid), static_cast<int>(pid));
        return false;
    }
    if (waitForLock(fd, 5000))
        return true;
    if (pid > 0) {
        std::fprintf(stderr, "%s: PID %d did not stop within 5 s, killing it\n", prog, static_cast<int>(pid));
        kill(pid, SIGKILL);
    }
    if (waitForLock(fd, 2000))
        return true;
    std::fprintf(stderr, "%s: could not stop the running instance (PID %d)\n", prog, static_cast<int>(pid));
    return false;
}

} // namespace

SingleInstance::SingleInstance(const std::string& name) : name_(name), path_(lockPath(name)) {}

SingleInstance::~SingleInstance()
{
    if (fd_ >= 0)
        close(fd_); // releases the lock
}

bool SingleInstance::acquire()
{
    const char* prog = name_.c_str();
    fd_ = openLockFile(path_, true);
    if (fd_ < 0) {
        std::fprintf(stderr, "%s: cannot open %s: %s\n", prog, path_.c_str(), std::strerror(errno));
        return false;
    }
    if (!tryLock(fd_)) {
        std::fprintf(stderr, "%s: another instance is running (PID %d), stopping it first...\n", prog,
                     static_cast<int>(readPid(fd_)));
        if (!stopHolder(fd_, prog))
            return false;
        std::fprintf(stderr, "%s: previous instance stopped\n", prog);
    }
    writePid(fd_);
    return true;
}

int SingleInstance::stopRunning(const std::string& name)
{
    const std::string path = lockPath(name);
    const int fd = openLockFile(path, false);
    if (fd < 0 || tryLock(fd)) {
        std::fprintf(stderr, "%s is not running\n", name.c_str());
        if (fd >= 0)
            close(fd);
        return 0;
    }
    const pid_t pid = readPid(fd);
    std::fprintf(stderr, "stopping %s (PID %d)...\n", name.c_str(), static_cast<int>(pid));
    const bool ok = stopHolder(fd, name.c_str());
    close(fd);
    if (ok)
        std::fprintf(stderr, "%s stopped\n", name.c_str());
    return ok ? 0 : 1;
}
