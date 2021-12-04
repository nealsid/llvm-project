//===-- EditlineTest.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/Config.h"

#if LLDB_ENABLE_LIBEDIT

#define EDITLINE_TEST_DUMP_OUTPUT 0

#include <stdio.h>
#include <unistd.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <memory>
#include <thread>

#include "TestingSupport/SubsystemRAII.h"
#include "lldb/Host/Editline.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/Pipe.h"
#include "lldb/Host/PseudoTerminal.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/StringList.h"

#define ESCAPE "\x1b"

using namespace lldb_private;
using namespace testing;

namespace {
const size_t TIMEOUT_MILLIS = 5000;
}

class FilePointer {
public:
  FilePointer() = delete;

  FilePointer(const FilePointer &) = delete;

  FilePointer(FILE *file_p) : _file_p(file_p) {}

  ~FilePointer() {
    if (_file_p != nullptr) {
      const int close_result = fclose(_file_p);
      EXPECT_EQ(0, close_result);
    }
  }

  operator FILE *() { return _file_p; }

private:
  FILE *_file_p;
};

/**
 Wraps an Editline class, providing a simple way to feed
 input (as if from the keyboard) and receive output from Editline.
 */
class EditlineAdapter {
public:
  EditlineAdapter();

  void CloseInput();

  bool IsValid() const { return _editline_sp != nullptr; }

  lldb_private::Editline &GetEditline() { return *_editline_sp; }

  bool SendLine(const std::string &line);

  bool SendLines(const std::vector<std::string> &lines);

  bool GetLine(std::string &line, bool &interrupted, size_t timeout_millis);

  bool GetLines(lldb_private::StringList &lines, bool &interrupted,
                size_t timeout_millis);

  void ConsumeAllOutput();

  const std::string GetEditlineOutput();

private:
  bool IsInputComplete(lldb_private::Editline *editline,
                       lldb_private::StringList &lines);

  std::unique_ptr<lldb_private::Editline> _editline_sp;

  PseudoTerminal _pty;
  int _pty_primary_fd;
  int _pty_secondary_fd;

  std::unique_ptr<FilePointer> _el_secondary_file;
  std::stringstream testOutputBuffer;
};

EditlineAdapter::EditlineAdapter()
    : _editline_sp(), _pty(), _pty_primary_fd(-1), _pty_secondary_fd(-1),
      _el_secondary_file() {
  lldb_private::Status error;

  // Open the first primary pty available.
  EXPECT_THAT_ERROR(_pty.OpenFirstAvailablePrimary(O_RDWR), llvm::Succeeded());

  // Grab the primary fd.  This is a file descriptor we will:
  // (1) write to when we want to send input to editline.
  // (2) read from when we want to see what editline sends back.
  _pty_primary_fd = _pty.GetPrimaryFileDescriptor();

  // Open the corresponding secondary pty.
  EXPECT_THAT_ERROR(_pty.OpenSecondary(O_RDWR), llvm::Succeeded());
  _pty_secondary_fd = _pty.GetSecondaryFileDescriptor();

  _el_secondary_file.reset(new FilePointer(fdopen(_pty_secondary_fd, "w+")));
  EXPECT_FALSE(nullptr == *_el_secondary_file);
  if (*_el_secondary_file == nullptr)
    return;

  // We have to set the output stream we pass to Editline as not using
  // buffered I/O.  Otherwise we are missing editline's output when we
  // close the stream in the keybinding test (i.e. the EOF comes
  // before data previously written to the stream by editline).  This
  // behavior isn't as I understand the spec becuse fclose should
  // flush the stream, but my best guess is that it's some unexpected
  // interaction with stream I/O and ptys.
  EXPECT_EQ(setvbuf(*_el_secondary_file, nullptr, _IONBF, 0), 0)
      << "Could not set editline output stream to use unbuffered I/O.";

  // Create an Editline instance.
  _editline_sp.reset(new lldb_private::Editline(
      "gtest editor", *_el_secondary_file, *_el_secondary_file,
      *_el_secondary_file, false));
  _editline_sp->SetPrompt("> ");

  // Hookup our input complete callback.
  auto input_complete_cb = [this](Editline *editline, StringList &lines) {
    return this->IsInputComplete(editline, lines);
  };
  _editline_sp->SetIsInputCompleteCallback(input_complete_cb);
}

void EditlineAdapter::CloseInput() {
  if (_el_secondary_file != nullptr)
    _el_secondary_file.reset(nullptr);
}

bool EditlineAdapter::SendLine(const std::string &line) {
  // Ensure we're valid before proceeding.
  if (!IsValid())
    return false;

  // Write the line out to the pipe connected to editline's input.
  ssize_t input_bytes_written =
      ::write(_pty_primary_fd, line.c_str(),
              line.length() * sizeof(std::string::value_type));

  const char *eoln = "\n";
  const size_t eoln_length = strlen(eoln);
  input_bytes_written =
      ::write(_pty_primary_fd, eoln, eoln_length * sizeof(char));

  EXPECT_NE(-1, input_bytes_written) << strerror(errno);
  EXPECT_EQ(eoln_length * sizeof(char), size_t(input_bytes_written));
  return eoln_length * sizeof(char) == size_t(input_bytes_written);
}

bool EditlineAdapter::SendLines(const std::vector<std::string> &lines) {
  for (auto &line : lines) {
#if EDITLINE_TEST_DUMP_OUTPUT
    printf("<stdin> sending line \"%s\"\n", line.c_str());
#endif
    if (!SendLine(line))
      return false;
  }
  return true;
}

// We ignore the timeout for now.
bool EditlineAdapter::GetLine(std::string &line, bool &interrupted,
                              size_t /* timeout_millis */) {
  // Ensure we're valid before proceeding.
  if (!IsValid())
    return false;

  _editline_sp->GetLine(line, interrupted);
  return true;
}

bool EditlineAdapter::GetLines(lldb_private::StringList &lines,
                               bool &interrupted, size_t /* timeout_millis */) {
  // Ensure we're valid before proceeding.
  if (!IsValid())
    return false;

  _editline_sp->GetLines(1, lines, interrupted);
  return true;
}

bool EditlineAdapter::IsInputComplete(lldb_private::Editline *editline,
                                      lldb_private::StringList &lines) {
  // We'll call ourselves complete if we've received a balanced set of braces.
  int start_block_count = 0;
  int brace_balance = 0;

  for (const std::string &line : lines) {
    for (auto ch : line) {
      if (ch == '{') {
        ++start_block_count;
        ++brace_balance;
      } else if (ch == '}')
        --brace_balance;
    }
  }

  return (start_block_count > 0) && (brace_balance == 0);
}

const std::string EditlineAdapter::GetEditlineOutput() {
  return testOutputBuffer.str();
}

void EditlineAdapter::ConsumeAllOutput() {
  FilePointer output_file(fdopen(_pty_primary_fd, "r"));

  int ch;
  while ((ch = fgetc(output_file)) != EOF) {
#if EDITLINE_TEST_DUMP_OUTPUT
    char display_str[] = {0, 0, 0};
    switch (ch) {
    case '\t':
      display_str[0] = '\\';
      display_str[1] = 't';
      break;
    case '\n':
      display_str[0] = '\\';
      display_str[1] = 'n';
      break;
    case '\r':
      display_str[0] = '\\';
      display_str[1] = 'r';
      break;
    default:
      display_str[0] = ch;
      break;
    }
    printf("<stdout> 0x%02x (%03d) (%s)\n", ch, ch, display_str);
#endif
    testOutputBuffer << (char)ch;
  }
}

class EditlineTestFixture : public ::testing::Test {
  SubsystemRAII<FileSystem> subsystems;
  EditlineAdapter _el_adapter;
  std::thread _sp_output_thread;

public:
  static void SetUpTestCase() {
    // We need a TERM set properly for editline to work as expected.
    setenv("TERM", "vt100", 1);
  }

  void SetUp() override {
    // Validate the editline adapter.
    EXPECT_TRUE(_el_adapter.IsValid());
    if (!_el_adapter.IsValid())
      return;

    // Dump output.
    _sp_output_thread = std::thread([&] { _el_adapter.ConsumeAllOutput(); });
  }

  void EndOutputThread() {
    _el_adapter.CloseInput();
    if (_sp_output_thread.joinable())
      _sp_output_thread.join();
  }

  void TearDown() override { EndOutputThread(); }

  EditlineAdapter &GetEditlineAdapter() { return _el_adapter; }
};

TEST_F(EditlineTestFixture, EditlineReceivesSingleLineText) {
  // Send it some text via our virtual keyboard.
  const std::string input_text("Hello, world");
  EXPECT_TRUE(GetEditlineAdapter().SendLine(input_text));

  // Verify editline sees what we put in.
  std::string el_reported_line;
  bool input_interrupted = false;
  const bool received_line = GetEditlineAdapter().GetLine(
      el_reported_line, input_interrupted, TIMEOUT_MILLIS);

  EXPECT_TRUE(received_line);
  EXPECT_FALSE(input_interrupted);
  EXPECT_EQ(input_text, el_reported_line);
}

TEST_F(EditlineTestFixture, EditlineReceivesMultiLineText) {
  // Send it some text via our virtual keyboard.
  std::vector<std::string> input_lines;
  input_lines.push_back("int foo()");
  input_lines.push_back("{");
  input_lines.push_back("printf(\"Hello, world\");");
  input_lines.push_back("}");
  input_lines.push_back("");

  EXPECT_TRUE(GetEditlineAdapter().SendLines(input_lines));

  // Verify editline sees what we put in.
  lldb_private::StringList el_reported_lines;
  bool input_interrupted = false;

  EXPECT_TRUE(GetEditlineAdapter().GetLines(el_reported_lines,
                                            input_interrupted, TIMEOUT_MILLIS));
  EXPECT_FALSE(input_interrupted);

  // Without any auto indentation support, our output should directly match our
  // input.
  std::vector<std::string> reported_lines;
  for (const std::string &line : el_reported_lines)
    reported_lines.push_back(line);

  EXPECT_THAT(reported_lines, testing::ContainerEq(input_lines));
}

namespace lldb_private {

// Parameter structure for parameterized tests.
struct KeybindingTestValue {
  // A number that is used to name the test, so test output can be
  // mapped back to a specific input.
  const std::string testNumber;
  // Whether this keyboard shortcut is only bound in multi-line mode.
  const bool multilineOnly;
  // The actual key sequence.
  const std::string keySequence;
  // The command the key sequence is supposed to execute.
  const std::string commandName;
  // This is initialized to the keySequence by default, but gtest has
  // some errors when the test name as created by the overloaded
  // operator<< has embedded newlines.  This is even true when we
  // specify a custom test name function, as we do below when we
  // instantiate the test case.  In cases where the keyboard shortcut
  // has a newline or carriage return, this field in the struct can be
  // set to something that is printable.
  const std::string &printableKeySequence = keySequence;
};

std::ostream &operator<<(std::ostream &os, const KeybindingTestValue &kbtv) {
  return os << "{" << kbtv.printableKeySequence << "  =>  " << kbtv.commandName
            << " (multiline only: " << kbtv.multilineOnly << ")}";
}

// The keyboard shortcuts that we're testing.
const KeybindingTestValue keySequences[] = {
    {"1", false, "^w", "ed-delete-prev-word"},
    {"2", false, "\t", "lldb-complete"},
    {"3", false, ESCAPE "[1;5C", "em-next-word"},
    {"4", false, ESCAPE "[1;5D", "ed-prev-word"},
    {"5", false, ESCAPE "[5C", "em-next-word"},
    {"6", false, ESCAPE "[5D", "ed-prev-word"},
    {"7", false, ESCAPE ESCAPE "[C", "em-next-word"},
    {"8", false, ESCAPE ESCAPE "[D", "ed-prev-word"},
    {"9", true, "\n", "lldb-end-or-add-line", "<CR>"},
    {"10", true, "\r", "lldb-end-or-add-line", "<LF>"},
    {"11", true, ESCAPE "\n", "lldb-break-line", ESCAPE "<CR>"},
    {"12", true, ESCAPE "\r", "lldb-break-line", ESCAPE "<LF>"},
    {"13", true, "^p", "lldb-previous-line"},
    {"14", true, "^n", "lldb-next-line"},
    {"15", true, "^?", "lldb-delete-previous-char"},
    {"16", true, "^d", "lldb-delete-next-char"},
    {"17", true, ESCAPE "[3~", "lldb-delete-next-char"},
    {"18", true, ESCAPE "[\\^", "lldb-revert-line"},
    {"19", true, ESCAPE "<", "lldb-buffer-start"},
    {"20", true, ESCAPE ">", "lldb-buffer-end"},
    {"21", true, ESCAPE "[A", "lldb-previous-line"},
    {"22", true, ESCAPE "[B", "lldb-next-line"},
    {"23", true, ESCAPE ESCAPE "[A", "lldb-previous-history"},
    {"24", true, ESCAPE ESCAPE "[B", "lldb-next-history"},
    {"25", true, ESCAPE "[1;3A", "lldb-previous-history"},
    {"26", true, ESCAPE "[1;3B", "lldb-next-history"},
};

class EditlineKeyboardBindingTest
    : public EditlineTestFixture,
      public testing::WithParamInterface<KeybindingTestValue> {};

// Helper method to call into libedit to have it output a keyboard
// shortcut mapping.
void retrieveEditlineShortcutKey(::EditLine *el,
                                 const std::string &keySequence) {
  ASSERT_EQ(el_set(el, EL_BIND, keySequence.c_str(), nullptr), 0)
      << "Retrieving editline keybinding failed for " << keySequence;
}

// Test cases for editline in single-line mode.
TEST_P(EditlineKeyboardBindingTest, SingleLineEditlineKeybindings) {
  KeybindingTestValue kbtv = GetParam();

  auto &editLine = GetEditlineAdapter().GetEditline();

  editLine.ConfigureEditor(false);

  retrieveEditlineShortcutKey(editLine.m_editline, kbtv.keySequence);
  EndOutputThread();
  const std::string output = GetEditlineAdapter().GetEditlineOutput();
  // If the shortcut key is only in multiline mode, verify that it is
  // not mapped to the command.  It could still be mapped by default,
  // so we just check if our command doesn't appear in the output.
  if (kbtv.multilineOnly) {
    ASSERT_THAT(output, Not(HasSubstr(kbtv.commandName)))
        << "Multiline only key was bound in single-line mode.";
    return;
  }

  // Otherwise, compare the output to make sure our command is mapped
  // to the shortcut key.
  ASSERT_THAT(output, HasSubstr(kbtv.commandName))
      << "Key sequence was not bound to expected command name.";
}

// Test cases for editline in multi-line mode.
TEST_P(EditlineKeyboardBindingTest, MultiLineEditlineKeybindings) {
  KeybindingTestValue kbtv = GetParam();

  auto &editLine = GetEditlineAdapter().GetEditline();

  editLine.ConfigureEditor(true);

  retrieveEditlineShortcutKey(editLine.m_editline, kbtv.keySequence);
  EndOutputThread();
  const std::string output = GetEditlineAdapter().GetEditlineOutput();

  ASSERT_THAT(output, HasSubstr(kbtv.commandName))
      << "Key sequence was not bound to expected command name.";
}

INSTANTIATE_TEST_SUITE_P(KeyboardShortcuts, EditlineKeyboardBindingTest,
                         testing::ValuesIn(keySequences),
                         [](const TestParamInfo<KeybindingTestValue> &kvb) {
                           return kvb.param.testNumber;
                         });

} // namespace lldb_private

#endif
