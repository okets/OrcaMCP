#ifdef _WIN32
	#include <windows.h>
	#include <boost/nowide/convert.hpp>
#else
	// any posix system
	#include <pthread.h>
#endif

#include <atomic>
#include <clocale>
#include <thread>
#include <tbb/task_scheduler_observer.h>

#include "Thread.hpp"
#include "Utils.hpp"

namespace Slic3r {

#ifdef _WIN32
// The new API is better than the old SEH style thread naming since the names also show up in crash dumpsand ETW traces.
// Because the new API is only available on newer Windows 10, look it up dynamically.

typedef HRESULT(__stdcall* SetThreadDescriptionType)(HANDLE, PCWSTR);
typedef HRESULT(__stdcall* GetThreadDescriptionType)(HANDLE, PWSTR*);

static HMODULE					s_hKernel32 = nullptr;
static SetThreadDescriptionType s_fnSetThreadDescription = nullptr;
static GetThreadDescriptionType	s_fnGetThreadDescription = nullptr;

// Convert the FARPROC from GetProcAddress to Fn through a generic function pointer.
template<typename Fn> static Fn load_proc(HMODULE module, const char* name) {
	return reinterpret_cast<Fn>(reinterpret_cast<void(*)()>(::GetProcAddress(module, name)));
}

static bool WindowsGetSetThreadNameAPIInitialize()
{
	// Orca: a function-local static looks the API up once, thread-safely: TBB workers name themselves
	// concurrently as they first enter an arena, and a test binary names no main thread first.
	static const bool looked_up = [] {
		s_hKernel32 = LoadLibraryW(L"Kernel32.dll");
		if (s_hKernel32) {
			s_fnSetThreadDescription = load_proc<SetThreadDescriptionType>(s_hKernel32, "SetThreadDescription");
			s_fnGetThreadDescription = load_proc<GetThreadDescriptionType>(s_hKernel32, "GetThreadDescription");
		}
		return true;
	}();
	return looked_up && s_fnSetThreadDescription && s_fnGetThreadDescription;
}

#ifndef NDEBUG
	// Use the old way by throwing an exception, so at least in Debug mode the thread names are shown by the debugger.
	static constexpr DWORD MSVC_SEH_EXCEPTION_NAME_THREAD = 0x406D1388;

#pragma pack(push,8)
	typedef struct tagTHREADNAME_INFO
	{
		DWORD  dwType; 		// Must be 0x1000.
		LPCSTR szName; 		// Pointer to name (in user addr space).
		DWORD  dwThreadID; 	// Thread ID (-1=caller thread).
		DWORD  dwFlags; 	// Reserved for future use, must be zero.
	} THREADNAME_INFO;
#pragma pack(pop)

	static void WindowsSetThreadNameSEH(HANDLE hThread, const char* thread_name)
	{
		THREADNAME_INFO info;
		info.dwType 	= 0x1000;
		info.szName 	= thread_name;
		info.dwThreadID = ::GetThreadId(hThread);
		info.dwFlags 	= 0;
		__try {
			RaiseException(MSVC_SEH_EXCEPTION_NAME_THREAD, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
#endif // NDEBUG

static bool WindowsSetThreadName(HANDLE hThread, const char *thread_name)
{
	if (! WindowsGetSetThreadNameAPIInitialize()) {
#ifdef NDEBUG
		return false;
#else // NDEBUG
		// Running on Windows 7 or old Windows 7 in debug mode,
		// inform the debugger about the thread name by throwing an SEH.
		WindowsSetThreadNameSEH(hThread, thread_name);
		return true;
#endif // NDEBUG
	}

	size_t len = strlen(thread_name);
	if (len < 1024) {
		// Allocate the temp string on stack.
		wchar_t buf[1024];
		s_fnSetThreadDescription(hThread, boost::nowide::widen(buf, 1024, thread_name));
	} else {
		// Allocate dynamically.
		s_fnSetThreadDescription(hThread, boost::nowide::widen(thread_name).c_str());
	}
	return true;
}

bool set_thread_name(std::thread &thread, const char *thread_name)
{
   	return WindowsSetThreadName(static_cast<HANDLE>(thread.native_handle()), thread_name);
}

bool set_thread_name(boost::thread &thread, const char *thread_name)
{
   	return WindowsSetThreadName(static_cast<HANDLE>(thread.native_handle()), thread_name);
}

bool set_current_thread_name(const char *thread_name)
{
    return WindowsSetThreadName(::GetCurrentThread(), thread_name);
}

std::optional<std::string> get_current_thread_name()
{
	if (! WindowsGetSetThreadNameAPIInitialize())
		return std::nullopt;

	wchar_t *ptr = nullptr;
	s_fnGetThreadDescription(::GetCurrentThread(), &ptr);
	return (ptr == nullptr) ? std::string() : boost::nowide::narrow(ptr);
}

#else // _WIN32

#ifdef __APPLE__

// Appe screwed the Posix norm.
bool set_thread_name(std::thread &thread, const char *thread_name)
{
// not supported
//   	pthread_setname_np(thread.native_handle(), thread_name);
	return false;
}

bool set_thread_name(boost::thread &thread, const char *thread_name)
{
// not supported	
//   	pthread_setname_np(thread.native_handle(), thread_name);
	return false;
}

bool set_current_thread_name(const char *thread_name)
{
	pthread_setname_np(thread_name);
	return true;
}

#else

// posix
bool set_thread_name(std::thread &thread, const char *thread_name)
{
   	pthread_setname_np(thread.native_handle(), thread_name);
	return true;
}

bool set_thread_name(boost::thread &thread, const char *thread_name)
{
   	pthread_setname_np(thread.native_handle(), thread_name);
	return true;
}

bool set_current_thread_name(const char *thread_name)
{
	pthread_setname_np(pthread_self(), thread_name);
	return true;
}

#endif

// Orca: macOS reads a thread's name back with the same call as the other posix systems.
std::optional<std::string> get_current_thread_name()
{
	char buf[16];
	return std::string(pthread_getname_np(pthread_self(), buf, 16) == 0 ? buf : "");
}

#endif // _WIN32

// To be called at the start of the application to save the current thread ID as the main (UI) thread ID.
static boost::thread::id g_main_thread_id;

void save_main_thread_id()
{
	g_main_thread_id = boost::this_thread::get_id();
}

// Retrieve the cached main (UI) thread ID.
boost::thread::id get_main_thread_id()
{
	return g_main_thread_id;
}

// Checks whether the main (UI) thread is active.
bool is_main_thread_active()
{
	return get_main_thread_id() == boost::this_thread::get_id();
}

namespace {

// Sets the current thread's locale to "C", for the G-code generator to produce "." as a decimal separator.
void set_current_thread_locale_c()
{
#ifdef _WIN32
	_configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
	std::setlocale(LC_ALL, "C");
#else
	// We are leaking some memory here, because the newlocale() produced memory will never be released.
	// This is not a problem though, as there will be a maximum one worker thread created per physical thread.
	uselocale(newlocale(
#ifdef __APPLE__
		LC_ALL_MASK
#else // some Unix / Linux / BSD
		LC_ALL
#endif
		, "C", nullptr));
#endif
}

// Names the current TBB worker slic3r_tbb_<n> and sets its locale to "C", once per thread.
void prepare_current_tbb_worker()
{
	thread_local bool prepared = false;
	if (prepared)
		return;
	prepared = true;
	static std::atomic<unsigned> last_number{ 0 };
	set_current_thread_name("slic3r_tbb_" + std::to_string(++last_number));
	set_current_thread_locale_c();
}

// Prepares every TBB worker that enters the arena it observes, before the worker runs a task there.
// TBB calls it on the entering worker, and it waits for nothing.
class TbbWorkerPreparer : public tbb::task_scheduler_observer
{
public:
	TbbWorkerPreparer() { observe(true); }
	void on_scheduler_entry(bool is_worker) override
	{
		if (is_worker)
			prepare_current_tbb_worker();
	}
};

} // namespace

// Orca: upstream ran a parallel_for whose tasks each waited until max_concurrency() of them ran at once.
// TBB never promises that many threads at once: when one worker did not come, the first slice hung for
// good. Instead, observe the arena of the calling thread (TBB gives every thread an arena of its own),
// once per thread, from before its first task.
void name_tbb_thread_pool_threads_set_locale()
{
	thread_local bool observing = false;
	if (observing)
		return;
	observing = true;

#ifdef SLIC3R_PROFILE
	// Shiny profiler is not thread safe, thus disable parallelization.
	disable_multi_threading();
#endif

	// Never deleted: TBB still writes to the observer when it frees the arena, which may outlive this thread.
	new TbbWorkerPreparer();
}

}
