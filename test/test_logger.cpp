#include "logger.h"
#include <thread>
#include <chrono>

using namespace logger;

struct String : RefCounted {
    std::string s;

    String(const std::string& s) : s(s) {
        INFO("String(\"%s\")", s.c_str());
    }

    ~String() {
        INFO("~String(\"%s\")", s.c_str());
    }
};

struct SlowLogger : Logger {
    Ref<Logger> L;
    SlowLogger(Ref<Logger> L) : L(L) {}
    void log(const Entry& e) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        L->log(e);
    }
};

int main(int argc, const char *argv[]) {
    root() = new AsyncLogger(new SlowLogger(root()));
    Ref<String> s = new String("This is a test");
    INFO("Refcount/1 = %i", int(s->refcount));
    Ref<String> s2 = s;
    INFO("Refcount/2 = %i", int(s->refcount));
    s2 = nullptr;
    INFO("Refcount/3 = %i", int(s->refcount));
    s = nullptr;
    INFO("Done!");
    while (root()->refcount > 1) {
        printf("waiting....\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return 0;
}
