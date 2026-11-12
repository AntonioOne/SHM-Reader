// ShmRgbViewer: real-time, strictly read-only viewer for VTD RDB image shared memory.
//
// Scans all System V shared memory segments whose key lies in a range (default
// 0x8100-0x81FF, i.e. "0x81xx"), opens one window per segment that carries RDB
// images and closes the window automatically when the segment disappears.
//
// Segments are attached with SHM_RDONLY, so this process can never modify the
// buffer flags (LOCK / TC / IG) or any other content written by the producer.
//
// Usage: ShmRgbViewer [--range <lo> <hi>] [--no-flip] [--scale <factor>] [key ...]
//        Without explicit keys, all keys in the range are monitored.
// Click "Save" (bottom-right of a window) to save that frame; keys: q/ESC quit, f toggle vertical
// flip, s save the current frame of every window.
// Only one instance runs at a time (starting again stops the running one); --stop stops it.

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "rdb_shm.h"
#include "single_instance.h"

namespace {

std::atomic<bool> gStop{false};

struct ShmSegment {
    key_t key = 0;
    int id = -1;
    const uint8_t* base = nullptr;
    size_t size = 0;

    bool attach()
    {
        id = shmget(key, 0, 0);
        if (id < 0)
            return false;
        void* p = shmat(id, nullptr, SHM_RDONLY);
        if (p == reinterpret_cast<void*>(-1)) {
            id = -1;
            return false;
        }
        shmid_ds info{};
        if (shmctl(id, IPC_STAT, &info) < 0) {
            shmdt(p);
            id = -1;
            return false;
        }
        base = static_cast<const uint8_t*>(p);
        size = info.shm_segsz;
        return true;
    }

    void detach()
    {
        if (base)
            shmdt(base);
        base = nullptr;
        size = 0;
        id = -1;
    }
};

// Snapshot of a POD located in shared memory (the producer writes concurrently,
// so never keep pointers into it).
template <typename T>
bool readAt(const ShmSegment& shm, size_t offset, T& out)
{
    if (offset + sizeof(T) > shm.size)
        return false;
    std::memcpy(&out, shm.base + offset, sizeof(T));
    return true;
}

struct Candidate {
    size_t infoOffset = 0;
    size_t msgOffset = 0;
    uint32_t frameNo = 0;
    double simTime = 0.0;
    size_t msgSize = 0;
};

// Picks the newest complete, currently unlocked buffer. Flags are only inspected, never written.
bool findNewestBuffer(const ShmSegment& shm, Candidate& best)
{
    RDB_SHM_HDR_t hdr{};
    if (!readAt(shm, 0, hdr) || hdr.headerSize == 0 || hdr.noBuffers == 0)
        return false;

    bool found = false;
    for (unsigned i = 0; i < hdr.noBuffers; ++i) {
        const size_t infoOffset = hdr.headerSize + i * sizeof(RDB_SHM_BUFFER_INFO_t);
        RDB_SHM_BUFFER_INFO_t info{};
        if (!readAt(shm, infoOffset, info))
            break;
        if (info.flags & RDB_SHM_BUFFER_FLAG_LOCK)
            continue;

        RDB_MSG_HDR_t msgHdr{};
        if (!readAt(shm, info.offset, msgHdr) || msgHdr.magicNo != RDB_MAGIC_NO)
            continue;

        const size_t msgSize = size_t(msgHdr.headerSize) + msgHdr.dataSize;
        if (msgHdr.dataSize == 0 || msgSize > info.bufferSize || info.offset + msgSize > shm.size)
            continue;

        if (!found || msgHdr.frameNo > best.frameNo) {
            best.infoOffset = infoOffset;
            best.msgOffset = info.offset;
            best.frameNo = msgHdr.frameNo;
            best.simTime = msgHdr.simTime;
            best.msgSize = msgSize;
            found = true;
        }
    }
    return found;
}

// Copies the message out of SHM, then verifies the producer did not start rewriting
// the buffer during the copy (seqlock-style check, without touching any flag).
bool copyMessage(const ShmSegment& shm, const Candidate& c, std::vector<uint8_t>& out)
{
    out.resize(c.msgSize);
    std::memcpy(out.data(), shm.base + c.msgOffset, c.msgSize);
    std::atomic_thread_fence(std::memory_order_acquire);

    RDB_SHM_BUFFER_INFO_t info{};
    RDB_MSG_HDR_t msgHdr{};
    if (!readAt(shm, c.infoOffset, info) || !readAt(shm, c.msgOffset, msgHdr))
        return false;
    if (info.flags & RDB_SHM_BUFFER_FLAG_LOCK)
        return false;
    return msgHdr.frameNo == c.frameNo && msgHdr.magicNo == RDB_MAGIC_NO;
}

// Returns pointer to RDB_IMAGE_t (followed by its pixel data) inside a copied message.
const RDB_IMAGE_t* findImage(const std::vector<uint8_t>& msg)
{
    RDB_MSG_HDR_t hdr{};
    std::memcpy(&hdr, msg.data(), sizeof(hdr));

    size_t pos = hdr.headerSize;
    const size_t end = msg.size();
    while (pos + sizeof(RDB_MSG_ENTRY_HDR_t) <= end) {
        RDB_MSG_ENTRY_HDR_t entry{};
        std::memcpy(&entry, msg.data() + pos, sizeof(entry));
        if (entry.headerSize == 0)
            break;
        const size_t dataPos = pos + entry.headerSize;
        if (dataPos + entry.dataSize > end)
            break;

        if (entry.pkgId == RDB_PKG_ID_IMAGE && entry.dataSize >= sizeof(RDB_IMAGE_t)) {
            const auto* img = reinterpret_cast<const RDB_IMAGE_t*>(msg.data() + dataPos);
            if (sizeof(RDB_IMAGE_t) + size_t(img->imgSize) <= entry.dataSize)
                return img;
        }
        pos = dataPos + entry.dataSize;
    }
    return nullptr;
}

const char* pixelFormatName(int fmt)
{
    switch (fmt) {
    case RDB_PIX_FORMAT_RGB_24:   return "RGB_24";
    case RDB_PIX_FORMAT_RGBA_24:  return "RGBA_24";
    case RDB_PIX_FORMAT_RGB8:     return "RGB8";
    case RDB_PIX_FORMAT_RGBA8:    return "RGBA8";
    case RDB_PIX_FORMAT_RGB16:    return "RGB16";
    case RDB_PIX_FORMAT_RGBA16:   return "RGBA16";
    case RDB_PIX_FORMAT_RGB16F:   return "RGB16F";
    case RDB_PIX_FORMAT_RGBA16F:  return "RGBA16F";
    case RDB_PIX_FORMAT_RGB32F:   return "RGB32F";
    case RDB_PIX_FORMAT_RGBA32F:  return "RGBA32F";
    case RDB_PIX_FORMAT_RED8:     return "RED8";
    case RDB_PIX_FORMAT_BW_8:     return "BW_8";
    case RDB_PIX_FORMAT_RED16:    return "RED16";
    default:                      return "?";
    }
}

// Converts an RDB image to an 8-bit BGR cv::Mat (OpenCV display order).
bool toBgr8(const RDB_IMAGE_t& img, cv::Mat& out, std::string& err)
{
    const int w = img.width;
    const int h = img.height;
    if (w == 0 || h == 0) {
        err = "empty image";
        return false;
    }

    int depth = CV_8U;
    int channels = 0;
    double scale = 1.0;
    switch (img.pixelFormat) {
    case RDB_PIX_FORMAT_RGB_24:
    case RDB_PIX_FORMAT_RGB8:      channels = 3; break;
    case RDB_PIX_FORMAT_RGBA_24:
    case RDB_PIX_FORMAT_RGBA8:     channels = 4; break;
    case RDB_PIX_FORMAT_RGB16:     depth = CV_16U; channels = 3; scale = 1.0 / 257.0; break;
    case RDB_PIX_FORMAT_RGBA16:    depth = CV_16U; channels = 4; scale = 1.0 / 257.0; break;
    case RDB_PIX_FORMAT_RGB16F:
    case RDB_PIX_FORMAT_RGB_16_F:  depth = CV_16F; channels = 3; scale = 255.0; break;
    case RDB_PIX_FORMAT_RGBA16F:
    case RDB_PIX_FORMAT_RGBA_16_F: depth = CV_16F; channels = 4; scale = 255.0; break;
    case RDB_PIX_FORMAT_RGB32F:
    case RDB_PIX_FORMAT_RGB_32_F:  depth = CV_32F; channels = 3; scale = 255.0; break;
    case RDB_PIX_FORMAT_RGBA32F:
    case RDB_PIX_FORMAT_RGBA_32_F: depth = CV_32F; channels = 4; scale = 255.0; break;
    case RDB_PIX_FORMAT_RED8:
    case RDB_PIX_FORMAT_BW_8:      channels = 1; break;
    case RDB_PIX_FORMAT_RED16:
    case RDB_PIX_FORMAT_BW_16:     depth = CV_16U; channels = 1; scale = 1.0 / 257.0; break;
    default:
        // Unknown format: assume 8-bit channels derived from pixelSize.
        if (img.pixelSize == 1 || img.pixelSize == 3 || img.pixelSize == 4) {
            channels = img.pixelSize;
        } else {
            err = "unsupported pixelFormat " + std::to_string(img.pixelFormat) +
                  " (pixelSize " + std::to_string(img.pixelSize) + ")";
            return false;
        }
    }

    const size_t minStride = size_t(CV_ELEM_SIZE1(depth)) * channels * w;
    // Rows may be padded (e.g. GL_PACK_ALIGNMENT); derive stride from imgSize.
    const size_t stride = img.imgSize / h;
    if (stride < minStride) {
        err = "imgSize " + std::to_string(img.imgSize) + " too small for " + std::to_string(w) + "x" +
              std::to_string(h) + " format " + std::to_string(img.pixelFormat);
        return false;
    }

    auto* data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&img) + sizeof(RDB_IMAGE_t));
    const cv::Mat raw(h, w, CV_MAKETYPE(depth, channels), data, stride);

    cv::Mat tmp;
    if (depth == CV_8U)
        tmp = raw;
    else
        raw.convertTo(tmp, CV_MAKETYPE(CV_8U, channels), scale);

    switch (channels) {
    case 1: cv::cvtColor(tmp, out, cv::COLOR_GRAY2BGR); break;
    case 3: cv::cvtColor(tmp, out, cv::COLOR_RGB2BGR); break;
    case 4: cv::cvtColor(tmp, out, cv::COLOR_RGBA2BGR); break;
    }
    return true;
}

// Returns key -> shmid of all live segments (removed segments report key 0 and are skipped).
std::map<key_t, int> listShmSegments()
{
    std::map<key_t, int> result;
    std::ifstream f("/proc/sysvipc/shm");
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        long long key = 0;
        int shmid = -1;
        if (ss >> key >> shmid && key != 0)
            result[static_cast<key_t>(key)] = shmid;
    }
    return result;
}

std::string hexKey(key_t key)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%05x", static_cast<unsigned>(key));
    return buf;
}

struct Options {
    bool flip = true;
    double displayScale = 1.0;
    std::string saveDir = ".";
};

// Clickable "Save" button drawn in the bottom-right corner of every window.
constexpr int kButtonW = 90;
constexpr int kButtonH = 30;
constexpr int kButtonMargin = 10;

class Viewer {
public:
    Viewer(key_t key, int shmid, const Options& opt) : title_("SHM " + hexKey(key)), shmid_(shmid), opt_(opt)
    {
        shm_.key = key;
    }
    ~Viewer() { close(); }

    bool attach()
    {
        if (!shm_.attach())
            return false;
        // The key may have been recreated between scan and attach.
        shmid_ = shm_.id;
        std::fprintf(stderr, "[%s] attached read-only (%zu bytes)\n", hexKey(shm_.key).c_str(), shm_.size);
        return true;
    }

    void close()
    {
        if (windowOpen_) {
            cv::destroyWindow(title_);
            windowOpen_ = false;
        }
        if (shm_.base) {
            shm_.detach();
            std::fprintf(stderr, "[%s] closed\n", hexKey(shm_.key).c_str());
        }
    }

    int shmid() const { return shmid_; }
    bool windowOpen() const { return windowOpen_; }

    void update(const Options& opt)
    {
        if (windowOpen_ && cv::getWindowProperty(title_, cv::WND_PROP_AUTOSIZE) < 0) {
            // Closed by the user: it reopens with the next new frame.
            windowOpen_ = false;
            std::fprintf(stderr, "[%s] window closed by user\n", hexKey(shm_.key).c_str());
        }

        Candidate c;
        if (!findNewestBuffer(shm_, c) || (haveFrame_ && c.frameNo == lastFrameNo_) || !copyMessage(shm_, c, msg_))
            return;
        lastFrameNo_ = c.frameNo;
        haveFrame_ = true;

        const RDB_IMAGE_t* img = findImage(msg_);
        if (!img)
            return;

        cv::Mat bgr;
        std::string err;
        if (!toBgr8(*img, bgr, err)) {
            if (err != lastErr_)
                std::fprintf(stderr, "[%s] frame %u: %s\n", hexKey(shm_.key).c_str(), c.frameNo, err.c_str());
            lastErr_ = err;
            return;
        }
        lastErr_.clear();

        if (opt.flip)
            cv::flip(bgr, bgr, 0);
        clean_ = bgr.clone(); // full resolution, no overlays: what the Save button writes
        if (opt.displayScale > 0.0 && opt.displayScale != 1.0)
            cv::resize(bgr, bgr, cv::Size(), opt.displayScale, opt.displayScale, cv::INTER_AREA);

        ++fpsCount_;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - fpsStart_).count();
        if (dt >= 1.0) {
            fps_ = fpsCount_ / dt;
            fpsCount_ = 0;
            fpsStart_ = now;
        }

        char text[160];
        std::snprintf(text, sizeof(text), "%s  frame %u  t=%.3fs  %ux%u %s  %.1f fps",
                      hexKey(shm_.key).c_str(), c.frameNo, c.simTime, unsigned(img->width),
                      unsigned(img->height), pixelFormatName(img->pixelFormat), fps_);
        cv::putText(bgr, text, cv::Point(8, 20), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 3,
                    cv::LINE_AA);
        cv::putText(bgr, text, cv::Point(8, 20), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1,
                    cv::LINE_AA);

        if (!windowOpen_) {
            cv::namedWindow(title_, cv::WINDOW_AUTOSIZE);
            cv::setMouseCallback(title_, &Viewer::onMouse, this);
            windowOpen_ = true;
            std::fprintf(stderr, "[%s] window opened (%ux%u %s)\n", hexKey(shm_.key).c_str(),
                         unsigned(img->width), unsigned(img->height), pixelFormatName(img->pixelFormat));
        }
        display_ = bgr;
        show();
    }

    // Saves the current frame at full resolution without overlays. Returns false if nothing to save.
    bool save()
    {
        if (!windowOpen_ || clean_.empty())
            return false;
        if (mkdir(opt_.saveDir.c_str(), 0775) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "Cannot create %s: %s\n", opt_.saveDir.c_str(), std::strerror(errno));
            return false;
        }

        const auto wall = std::chrono::system_clock::now();
        const std::time_t secs = std::chrono::system_clock::to_time_t(wall);
        const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000);
        std::tm tm{};
        localtime_r(&secs, &tm);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        char name[128];
        std::snprintf(name, sizeof(name), "shm_%s_frame%u_%s_%03d.png", hexKey(shm_.key).c_str(), lastFrameNo_, stamp, ms);
        const std::string path = opt_.saveDir + "/" + name;

        bool ok = false;
        try {
            ok = cv::imwrite(path, clean_);
        } catch (const cv::Exception& e) {
            std::fprintf(stderr, "Saving %s failed: %s\n", path.c_str(), e.what());
        }
        if (ok)
            std::fprintf(stderr, "Saved %s\n", path.c_str());
        else
            std::fprintf(stderr, "Saving %s failed\n", path.c_str());
        feedback_ = ok ? std::string("Saved ") + name : std::string("Save failed");
        feedbackUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        show();
        return ok;
    }

    // Removes the save feedback once it has expired (also when no new frames arrive).
    void refreshOverlay()
    {
        if (!feedback_.empty() && std::chrono::steady_clock::now() >= feedbackUntil_) {
            feedback_.clear();
            show();
        }
    }

private:
    cv::Rect buttonRect() const
    {
        return cv::Rect(display_.cols - kButtonW - kButtonMargin, display_.rows - kButtonH - kButtonMargin, kButtonW,
                        kButtonH);
    }

    // Shows the last frame with the Save button (and the save feedback, if any).
    void show()
    {
        if (!windowOpen_ || display_.empty())
            return;
        cv::Mat canvas = display_.clone();
        const cv::Rect r = buttonRect();
        if (r.x >= 0 && r.y >= 0) {
            cv::rectangle(canvas, r, cv::Scalar(60, 60, 60), cv::FILLED);
            cv::rectangle(canvas, r, cv::Scalar(230, 230, 230), 1);
            int base = 0;
            const cv::Size ts = cv::getTextSize("Save", cv::FONT_HERSHEY_SIMPLEX, 0.6, 1, &base);
            cv::putText(canvas, "Save", cv::Point(r.x + (r.width - ts.width) / 2, r.y + (r.height + ts.height) / 2),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
            if (!feedback_.empty()) {
                const cv::Size fs = cv::getTextSize(feedback_, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &base);
                const cv::Point p(std::max(4, r.x - fs.width - 10), r.y + (r.height + fs.height) / 2);
                cv::putText(canvas, feedback_, p, cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 3, cv::LINE_AA);
                cv::putText(canvas, feedback_, p, cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
            }
        }
        cv::imshow(title_, canvas);
    }

    static void onMouse(int event, int x, int y, int, void* self)
    {
        auto* v = static_cast<Viewer*>(self);
        if (event == cv::EVENT_LBUTTONDOWN && v->buttonRect().contains(cv::Point(x, y)))
            v->save();
    }

    ShmSegment shm_;
    std::string title_;
    int shmid_ = -1;
    const Options& opt_;
    bool windowOpen_ = false;
    bool haveFrame_ = false;
    uint32_t lastFrameNo_ = 0;
    std::vector<uint8_t> msg_;
    cv::Mat clean_;   // last frame, full resolution, as displayed (flipped), without overlays
    cv::Mat display_; // last frame scaled for display, with the info text
    std::string feedback_;
    std::chrono::steady_clock::time_point feedbackUntil_;
    std::string lastErr_;
    std::chrono::steady_clock::time_point fpsStart_ = std::chrono::steady_clock::now();
    int fpsCount_ = 0;
    double fps_ = 0.0;
};

void usage(const char* prog)
{
    std::fprintf(stderr,
                 "Usage: %s [--range <lo> <hi>] [--no-flip] [--scale <factor>] [--save-dir <dir>] [key ...]\n"
                 "       %s --stop\n"
                 "  Default: monitor all shared memory keys 0x8100-0x81ff.\n"
                 "  A window opens for each segment carrying RDB images and closes when it disappears.\n"
                 "  --stop  stop the running instance (also when it runs in the background) and exit\n"
                 "  Only one instance runs at a time: starting it again stops the running one first.\n"
                 "  --save-dir  where the Save button / 's' key write PNG files (default: current directory)\n"
                 "Click 'Save' (bottom-right of a window) to save that window's current frame at full\n"
                 "resolution without overlays.\n"
                 "Keys:  q/ESC quit, f toggle vertical flip, s save the current frame of every window\n",
                 prog, prog);
}

bool parseKey(const char* s, long& out)
{
    char* end = nullptr;
    out = std::strtol(s, &end, 0);
    return *s != '\0' && end && *end == '\0';
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    long rangeLo = 0x8100;
    long rangeHi = 0x81ff;
    std::set<key_t> explicitKeys;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        long v = 0;
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--stop") {
            return SingleInstance::stopRunning("ShmRgbViewer");
        } else if (a == "--no-flip") {
            opt.flip = false;
        } else if (a == "--save-dir" && i + 1 < argc) {
            opt.saveDir = argv[++i];
        } else if (a == "--scale" && i + 1 < argc) {
            opt.displayScale = std::atof(argv[++i]);
        } else if (a == "--range" && i + 2 < argc) {
            if (!parseKey(argv[i + 1], rangeLo) || !parseKey(argv[i + 2], rangeHi)) {
                usage(argv[0]);
                return 1;
            }
            i += 2;
        } else if (parseKey(argv[i], v)) {
            explicitKeys.insert(static_cast<key_t>(v));
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    auto wanted = [&](key_t key) {
        if (!explicitKeys.empty())
            return explicitKeys.count(key) > 0;
        return key >= rangeLo && key <= rangeHi;
    };

    std::signal(SIGINT, [](int) { gStop = true; });
    std::signal(SIGTERM, [](int) { gStop = true; });
    std::signal(SIGHUP, [](int) { gStop = true; });

    SingleInstance instance("ShmRgbViewer");
    if (!instance.acquire())
        return 1;

    if (explicitKeys.empty())
        std::fprintf(stderr, "Monitoring shared memory keys %s-%s ...\n", hexKey(rangeLo).c_str(),
                     hexKey(rangeHi).c_str());
    else
        std::fprintf(stderr, "Monitoring %zu shared memory key(s) ...\n", explicitKeys.size());

    std::map<key_t, std::unique_ptr<Viewer>> viewers;
    auto lastScan = std::chrono::steady_clock::time_point{};
    const auto scanInterval = std::chrono::milliseconds(500);

    while (!gStop) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastScan >= scanInterval) {
            lastScan = now;
            const auto segments = listShmSegments();

            // Close viewers whose segment was removed or recreated with a new id.
            for (auto it = viewers.begin(); it != viewers.end();) {
                const auto seg = segments.find(it->first);
                if (seg == segments.end() || seg->second != it->second->shmid())
                    it = viewers.erase(it);
                else
                    ++it;
            }

            for (const auto& seg : segments) {
                if (!wanted(seg.first) || viewers.count(seg.first))
                    continue;
                std::unique_ptr<Viewer> v(new Viewer(seg.first, seg.second, opt));
                if (v->attach())
                    viewers[seg.first] = std::move(v);
            }
        }

        bool anyWindow = false;
        for (auto& kv : viewers) {
            kv.second->update(opt);
            kv.second->refreshOverlay();
            anyWindow = anyWindow || kv.second->windowOpen();
        }

        // waitKey also pumps GUI events (needed for destroyWindow to take effect).
        const int k = cv::waitKey(anyWindow ? 1 : 50);
        if (k == 'q' || k == 27)
            break;
        if (k == 'f')
            opt.flip = !opt.flip;
        if (k == 's')
            for (auto& kv : viewers)
                kv.second->save();
    }

    viewers.clear();
    cv::destroyAllWindows();
    cv::waitKey(1);
    return 0;
}
