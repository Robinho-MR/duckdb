//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/stack_overflow_handler.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

// Header-only, intended for executables (CLI, unittest) - libraries should not install signal handlers.
#if defined(__linux__) || defined(__APPLE__)
#define DUCKDB_HAS_STACK_OVERFLOW_HANDLER 1
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <ucontext.h>
#endif
#endif

namespace duckdb {

#ifdef DUCKDB_HAS_STACK_OVERFLOW_HANDLER

class StackOverflowHandler {
public:
	//! Reports faults near the stack pointer as stack overflows, then re-raises with the default action.
	//! Only the calling thread gets an alternate stack. Returns false if installation failed.
	static bool Install() {
		static constexpr size_t ALT_STACK_SIZE = 64 * 1024;
		// intentionally leaked: must stay valid for the lifetime of the thread
		void *memory = malloc(ALT_STACK_SIZE);
		if (!memory) {
			return false;
		}
		stack_t alt_stack;
		memset(&alt_stack, 0, sizeof(alt_stack));
		alt_stack.ss_sp = memory;
		alt_stack.ss_size = ALT_STACK_SIZE;
		if (sigaltstack(&alt_stack, nullptr) != 0) {
			return false;
		}
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_sigaction = Handler;
		action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
		sigemptyset(&action.sa_mask);
		return sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0;
	}

private:
	static bool GetStackPointer(void *context, uintptr_t &sp) {
		auto ctx = static_cast<ucontext_t *>(context);
#if defined(__linux__) && defined(__x86_64__)
		sp = static_cast<uintptr_t>(ctx->uc_mcontext.gregs[REG_RSP]);
#elif defined(__linux__) && defined(__aarch64__)
		sp = static_cast<uintptr_t>(ctx->uc_mcontext.sp);
#elif defined(__APPLE__) && defined(__aarch64__)
		sp = static_cast<uintptr_t>(ctx->uc_mcontext->__ss.__sp);
#elif defined(__APPLE__) && defined(__x86_64__)
		sp = static_cast<uintptr_t>(ctx->uc_mcontext->__ss.__rsp);
#else
		(void)ctx;
		return false;
#endif
		return true;
	}

	static void Handler(int signum, siginfo_t *info, void *context) {
		// A stack overflow faults at (or a few pages from) the stack pointer. Unknown platforms report
		// any fault, since the handler is reset and the default action runs right after.
		uintptr_t sp = 0;
		bool is_overflow = true;
		if (GetStackPointer(context, sp)) {
			auto addr = reinterpret_cast<uintptr_t>(info->si_addr);
			static constexpr uintptr_t WINDOW = 1024 * 1024;
			is_overflow = addr + WINDOW >= sp && addr <= sp + WINDOW;
		}
		if (is_overflow) {
			static const char message[] = "\nFatal error: stack overflow (the query or input is likely nested too "
			                              "deeply or recursion is unbounded)\n";
			ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
			(void)ignored;
		}
		// SA_RESETHAND restored the default action; returning re-executes the fault, or raise explicitly
		raise(signum);
	}
};

#else

class StackOverflowHandler {
public:
	static bool Install() {
		return false;
	}
};

#endif

} // namespace duckdb
