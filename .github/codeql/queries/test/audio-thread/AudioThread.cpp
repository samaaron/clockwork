// A canary for audio-thread-forbidden-calls.ql: an audio entry point that
// reaches, two calls deep, an allocation, a lock and a file write, and one
// placement new that it must not report.
extern "C" void* malloc(unsigned long);
extern "C" int fprintf(void*, const char*, ...);
extern void* stderr_stream;

struct mutex { void lock(); void unlock(); };
struct lock_guard { explicit lock_guard(mutex& m) : m(m) { m.lock(); } ~lock_guard() { m.unlock(); } mutex& m; };
inline void* operator new(unsigned long, void* where) { return where; }

struct Voice { int gain; };
static mutex g_mutex;
static unsigned char g_slot[sizeof(Voice)];

static void grow() { malloc(64); }                       // allocates
static void guard() { lock_guard lk(g_mutex); }         // may block on a lock
static void say() { fprintf(stderr_stream, "x"); }      // does file IO
static void place() { ::new (g_slot) Voice{1}; }         // placement new: fine

static void mix() { grow(); guard(); }

void renderAudioBlock(float* out, int frames) {
    mix();
    say();
    place();
    for (int i = 0; i < frames; ++i) out[i] = 0.0f;
}

void notOnTheAudioThread() { malloc(8); }               // never reached from an entry: fine
