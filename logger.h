#if !defined(LOG_H_INCLUDED)
#define LOG_H_INCLUDED

/*****************************************************************************

MIT License

Copyright (c) 2016 Andrea Griffini

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <vector>
#include <string>
#include <deque>
#include <map>
#include <functional>
#include <mutex>
#include <thread>
#include <algorithm>
#include <chrono>
#include <atomic>

namespace logger {

    // Intrusive refcounting utility
    struct RefCounted {
        std::atomic_int refcount;
        RefCounted() : refcount(0) {}
    };

    template<typename T>
    struct Ref {
        T *p;

        template<typename U>
        Ref(U *p) : p(p) {
            if (p) p->refcount++;
        }

        template<typename U>
        Ref(const Ref<U>& other) : p(other.p) {
            if (p) p->refcount++;
        }

        void swap(Ref& other) {
            std::swap(p, other.p);
        }

        template<typename U>
        Ref& operator=(const Ref<U>& other) {
            swap(Ref(other.p));
            return *this;
        }

        template<typename U>
        Ref& operator=(U *u) {
            Ref(u).swap(*this);
            return *this;
        }

        Ref(const Ref& other) : p(other.p) {
            if (p) p->refcount++;
        }

        Ref& operator=(const Ref& other) {
            swap(Ref(other.p));
            return *this;
        }

        Ref& operator=(std::nullptr_t) {
            if (p && --p->refcount==0) delete p;
            p = nullptr;
            return *this;
        }

        ~Ref() {
            if (p && --p->refcount==0) delete p;
        }

        T* operator->() {
            return p;
        }

        const T* operator->() const {
            return p;
        }

        T& operator*() {
            return *p;
        }

        const T& operator*() const {
            return *p;
        }

        operator bool() const {
            return p;
        }
    };

    // String formatting (why oh why isn't this part of std??)
    std::string stringf(const char *fmt, ...) {
        std::vector<char> buffer(256);
        va_list args, cp;
        va_start(args, fmt);
        va_copy(cp, args);
        int sz = vsnprintf(&buffer[0], buffer.size(), fmt, args);
        if (sz >= int(buffer.size())) {
            buffer.resize(sz + 1);
            vsnprintf(&buffer[0], buffer.size(), fmt, cp);
        }
        va_end(cp);
        va_end(args);
        return &buffer[0];
    }

    inline std::map<int, std::string>& severities(){
        static std::map<int, std::string> s{
            {0, "info"},
            {100, "warning"},
            {200, "error"},
            {1000, "fatal error"}};
        return s;
    }

    inline std::string sevname(int severity) {
        auto& s = severities();
        auto it = s.find(severity);
        if (it == s.end()) it = s.find(severity/100*100);
        if (it != s.end()) return it->second;
        return stringf("severity=%i", severity);
    }

    struct Entry {
        double time;
        int severity;
        std::string context;
        std::string message;
    };

    typedef std::function<std::string(const Entry&)> Formatter;
    typedef std::function<bool(const Entry&)> Filter;

    struct Logger : RefCounted {
        Logger() {}
        virtual ~Logger() {}
        virtual void log(const Entry& e) = 0;
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;
    };

    inline Formatter default_formatter() {
        return [](const Entry& e) -> std::string {
            char ctimebuf[30];
            time_t t = e.time;
            ctime_r(&t, ctimebuf); ctimebuf[strlen(ctimebuf)-1] = '\0';
            return stringf("%s - %s: (%s) -- %s",
                           ctimebuf,
                           sevname(e.severity).c_str(),
                           e.context.c_str(),
                           e.message.c_str());
        };
    }

    // Spread logging to multiple destinations
    struct MultiLogger : Logger {
        std::vector<Ref<Logger>> Ls;
        void log(const Entry& e) {
            for (auto& x : Ls) x->log(e);
        }
    };

    // Abstract filtering
    struct FilteringLogger : Logger {
        Ref<Logger> L;
        Filter f;
        FilteringLogger(Ref<Logger> L, Filter f)
            : L(L), f(f)
        {}
        void log(const Entry& e) {
            if (f(e)) L->log(e);
        }
    };

    // Severity filtering
    inline Ref<Logger> severityFilter(Ref<Logger> L, int low, int high=-1) {
        return new FilteringLogger{L, [low, high](const Entry& e) {
                return e.severity >= low && (high == -1 || e.severity <= high);
            }};
    }

    // Logging to memory (in Entry form)
    struct MemLogger : Logger {
        std::deque<Entry> q;
        int max_size;
        std::mutex m;

        MemLogger(int max_size=-1) : max_size(max_size) {}
        void log(const Entry& e) {
            std::lock_guard<std::mutex> lock(m);
            q.push_back(e);
            while (max_size != -1 && int(q.size()) > max_size) {
                q.pop_front();
            }
        }
    };

    // Log to stdio files
    struct FileLogger : Logger {
        FILE *file;
        Formatter formatter;
        bool autoclose;
        std::mutex m;

        FileLogger(FILE *file, const Formatter& formatter = default_formatter(), bool autoclose = false)
            : file(file), formatter(formatter), autoclose(autoclose)
        { }

        ~FileLogger() {
            if (autoclose) fclose(file);
        }

        void log(const Entry& e) {
            std::lock_guard<std::mutex> lock(m);
            fprintf(file, "%s\n", formatter(e).c_str());
        }
    };

    // Log asynchronously
    struct AsyncLogger : Logger {
        std::deque<Entry> q;
        Ref<Logger> L;
        std::mutex m;
        std::thread worker;

        AsyncLogger(Ref<Logger> L) : L(L) {}

        void log(const Entry& e) {
            std::lock_guard<std::mutex> lock(m);
            q.push_back(e);
            if (q.size() == 1) {
                if (worker.joinable()) worker.join();
                Ref<Logger> me(this);
                worker = std::thread([me, this](){
                        m.lock();
                        while (q.size() > 0) {
                            Entry& e = q.front();
                            m.unlock();
                            L->log(e);
                            m.lock();
                            q.pop_front();
                        }
                        worker.detach();
                        m.unlock();
                    });
            }
        }
    };

    // By default logging is synchronous and goes to stderr
    inline Ref<Logger>& root() {
        static Ref<Logger> L = new FileLogger(stderr);
        return L;
    }

    inline double now() {
        // This is how you get seconds since epoch (!)
        return std::chrono::duration_cast<std::chrono::milliseconds>
            (std::chrono::system_clock::now().time_since_epoch()).count() / 1000.;
        // (my rationalization for this level of type obsession crap
        // is that the proposal was done on April 1st but the committee
        // didn't realize it was just a joke).
    }
}

#define LOGSTRINGIFY(x) #x
#define LOGTOSTRING(x) LOGSTRINGIFY(x)
#define LOG(severity, ...)                                  \
    logger::root()->log(logger::Entry{logger::now(),        \
                        severity,                           \
                        __FILE__ ":" LOGTOSTRING(__LINE__), \
                        logger::stringf(__VA_ARGS__)})

#define INFO(...)    LOG(   0, __VA_ARGS__)
#define WARNING(...) LOG( 100, __VA_ARGS__)
#define ERROR(...)   LOG( 200, __VA_ARGS__)
#define FATAL(...)   LOG(1000, __VA_ARGS__)

#endif
