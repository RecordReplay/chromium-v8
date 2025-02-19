// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/logging.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "src/base/debug/stack_trace.h"
#include "src/base/platform/platform.h"

namespace v8 {
namespace base {

namespace {

void DefaultDcheckHandler(const char* file, int line, const char* message);

void (*g_print_stack_trace)() = nullptr;

void (*g_dcheck_function)(const char*, int, const char*) = DefaultDcheckHandler;

std::string PrettyPrintChar(int ch) {
  std::ostringstream oss;
  switch (ch) {
#define CHAR_PRINT_CASE(ch) \
  case ch:                  \
    oss << #ch;             \
    break;

    CHAR_PRINT_CASE('\0')
    CHAR_PRINT_CASE('\'')
    CHAR_PRINT_CASE('\\')
    CHAR_PRINT_CASE('\a')
    CHAR_PRINT_CASE('\b')
    CHAR_PRINT_CASE('\f')
    CHAR_PRINT_CASE('\n')
    CHAR_PRINT_CASE('\r')
    CHAR_PRINT_CASE('\t')
    CHAR_PRINT_CASE('\v')
#undef CHAR_PRINT_CASE
    default:
      if (std::isprint(ch)) {
        oss << '\'' << ch << '\'';
      } else {
        oss << std::hex << "\\x" << static_cast<unsigned int>(ch);
      }
  }
  return oss.str();
}

void DefaultDcheckHandler(const char* file, int line, const char* message) {
#ifdef DEBUG
  V8_Fatal(file, line, "Debug check failed: %s.", message);
#else
  // This case happens only for unit tests.
  V8_Fatal("Debug check failed: %s.", message);
#endif
}

}  // namespace

void SetPrintStackTrace(void (*print_stack_trace)()) {
  g_print_stack_trace = print_stack_trace;
}

void SetDcheckFunction(void (*dcheck_function)(const char*, int, const char*)) {
  g_dcheck_function = dcheck_function ? dcheck_function : &DefaultDcheckHandler;
}

// Define specialization to pretty print characters (escaping non-printable
// characters) and to print c strings as pointers instead of strings.
#define DEFINE_PRINT_CHECK_OPERAND_CHAR(type)                    \
  template <>                                                    \
  std::string PrintCheckOperand<type>(type ch) {                 \
    return PrettyPrintChar(ch);                                  \
  }                                                              \
  template <>                                                    \
  std::string PrintCheckOperand<type*>(type * cstr) {            \
    return PrintCheckOperand<void*>(cstr);                       \
  }                                                              \
  template <>                                                    \
  std::string PrintCheckOperand<const type*>(const type* cstr) { \
    return PrintCheckOperand<const void*>(cstr);                 \
  }

DEFINE_PRINT_CHECK_OPERAND_CHAR(char)
DEFINE_PRINT_CHECK_OPERAND_CHAR(signed char)
DEFINE_PRINT_CHECK_OPERAND_CHAR(unsigned char)
#undef DEFINE_PRINT_CHECK_OPERAND_CHAR

// Explicit instantiations for commonly used comparisons.
#define DEFINE_MAKE_CHECK_OP_STRING(type)                           \
  template std::string* MakeCheckOpString<type, type>(type, type,   \
                                                      char const*); \
  template std::string PrintCheckOperand<type>(type);
DEFINE_MAKE_CHECK_OP_STRING(int)
DEFINE_MAKE_CHECK_OP_STRING(long)       // NOLINT(runtime/int)
DEFINE_MAKE_CHECK_OP_STRING(long long)  // NOLINT(runtime/int)
DEFINE_MAKE_CHECK_OP_STRING(unsigned int)
DEFINE_MAKE_CHECK_OP_STRING(unsigned long)       // NOLINT(runtime/int)
DEFINE_MAKE_CHECK_OP_STRING(unsigned long long)  // NOLINT(runtime/int)
DEFINE_MAKE_CHECK_OP_STRING(void const*)
#undef DEFINE_MAKE_CHECK_OP_STRING

}  // namespace base
}  // namespace v8

namespace {

// FailureMessage is a stack allocated object which has a special marker field
// at the start and at the end. This makes it possible to retrieve the embedded
// message from the stack.
//
class FailureMessage {
 public:
  explicit FailureMessage(const char* format, va_list arguments) {
    memset(&message_, 0, arraysize(message_));
    v8::base::OS::VSNPrintF(&message_[0], arraysize(message_), format,
                            arguments);
  }

  static const uintptr_t kStartMarker = 0xdecade10;
  static const uintptr_t kEndMarker = 0xdecade11;
  static const int kMessageBufferSize = 512;

  uintptr_t start_marker_ = kStartMarker;
  char message_[kMessageBufferSize];
  uintptr_t end_marker_ = kEndMarker;
};

}  // namespace

const char* gCrashReason;

extern "C" const char* V8RecordReplayCrashReasonCallback() {
  return gCrashReason;
}

static __attribute__((noinline)) void BusyWait() {
  fprintf(stderr, "Busy-waiting... (pid %d)", getpid());
  volatile int x = 1;
  while (x) {}
}

#ifdef DEBUG
void V8_Fatal(const char* file, int line, const char* format, ...) {
#else
void V8_Fatal(const char* format, ...) {
  const char* file = "";
  int line = 0;
#endif

  {
    char str[4096];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(str, sizeof(str), format, arguments);
    str[sizeof(str) - 1] = 0;
    va_end(arguments);

    gCrashReason = strdup(str);
  }

  va_list arguments;
  va_start(arguments, format);
  // Format the error message into a stack object for later retrieveal by the
  // crash processor.
  FailureMessage message(format, arguments);
  va_end(arguments);

  fflush(stdout);
  fflush(stderr);
  // Print the formatted message to stdout without cropping the output.
  v8::base::OS::PrintError("\n\n#\n# Fatal error in %s, line %d\n# ", file,
                           line);

  // Print the error message.
  va_start(arguments, format);
  v8::base::OS::VPrintError(format, arguments);
  va_end(arguments);
  // Print the message object's address to force stack allocation.
  v8::base::OS::PrintError("\n#\n#\n#\n#FailureMessage Object: %p", &message);

  if (v8::base::g_print_stack_trace) v8::base::g_print_stack_trace();

  fflush(stderr);

  if (getenv("RECORD_REPLAY_WAIT_AT_CRASH") ||
      getenv("RECORD_REPLAY_WAIT_AT_FATAL_ERROR")) {
    BusyWait();
  }

  v8::base::OS::Abort();
}

void V8_Dcheck(const char* file, int line, const char* message) {
  v8::base::g_dcheck_function(file, line, message);
}
