/*************************************************************************
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

// Host-only unit tests for the inspector profiler plugin
// (plugins/profiler/inspector).
//
// inspector.cc is compiled by test/host/inspector_wrapper.cc, which also holds
// the stubs for the plugin's sibling TUs and the C forwarders these tests call.
// The plugin vendors its own nccl/ headers, which collide with RCCL's, so this
// TU deliberately includes none of them and reaches the code under test only
// through those forwarders. Add a forwarder there when a new test needs to
// reach further into the plugin.
//
// ---------------------------------------------------------------------------
// InspectorOutputFileTest -- output-file creation contract
// ---------------------------------------------------------------------------
// The plugin writes its Prometheus and JSON output into a directory that, in the
// deployments its README describes, is shared with other local users. These
// tests pin the two properties that makes safe:
//
//   1. Output files are not writable or executable by group/other, whatever the
//      umask. A world-writable telemetry file lets any local user forge the
//      metrics an exporter scrapes.
//   2. Output file creation does not follow a symlink at the output path, so a
//      link planted by another user cannot redirect the write onto a file of
//      their choosing.
//
// Both production sites are driven against a scratch directory:
// getOrCreateFileHandle() (Prometheus) and inspectorStateDumpJSON() (JSON). The
// real json.cc is linked too, so the file-creation path under test is
// production code end to end.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// All exposed by test/host/inspector_wrapper.cc.
extern "C" {
void test_inspectorInstallNoOpLogger(void);
void test_inspectorSetPromDumpEnabled(int enabled);
void* test_inspectorDumpThreadCreate(const char* outputRoot,
                                     int64_t sampleIntervalUsecs);
void test_inspectorDumpThreadDestroy(void* dumper);
int test_inspectorGetOrCreateFileHandle(void* dumper,
                                        const char* deviceUuidStr,
                                        const char* filename,
                                        uint64_t currentTime);
int test_inspectorStateDumpJSON(void* dumper, const char* outputRoot);
int test_inspectorResultSuccess(void);
int test_inspectorResultFileOpenError(void);
}

namespace {

// Owns an inspectorDumpThread for the duration of a test.
class ScopedDumper {
 public:
  ScopedDumper(const std::string& root, int64_t intervalUsecs)
      : handle_(test_inspectorDumpThreadCreate(root.c_str(), intervalUsecs)) {}
  ~ScopedDumper() { test_inspectorDumpThreadDestroy(handle_); }
  ScopedDumper(const ScopedDumper&) = delete;
  ScopedDumper& operator=(const ScopedDumper&) = delete;
  void* get() const { return handle_; }

 private:
  void* handle_;
};

// Permission bits of `path`, following symlinks. Returns -1 if absent.
int PermsOf(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return -1;
  return static_cast<int>(st.st_mode & 07777);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void WriteFile(const std::string& path, const std::string& contents) {
  std::ofstream out(path);
  out << contents;
}

// The JSON writer derives its filename from the host name and pid; mirror that
// so a test can pre-place a symlink at the exact path production will use.
std::string JsonOutputPathFor(const std::string& root) {
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    hostname[0] = '\0';
  } else {
    hostname[sizeof(hostname) - 1] = '\0';
  }
  char buf[2048];
  snprintf(buf, sizeof(buf), "%s/%s-pid%d.log", root.c_str(), hostname,
           getpid());
  return std::string(buf);
}

class InspectorOutputFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_inspectorInstallNoOpLogger();
    test_inspectorSetPromDumpEnabled(1);

    // A permissive umask would mask the defect under test: assert the
    // production code narrows the mode itself rather than relying on the
    // caller's umask to do it.
    savedUmask_ = umask(0);

    char tmpl[] = "/tmp/rccl-inspector-perms-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    ASSERT_NE(dir, nullptr) << "mkdtemp: " << strerror(errno);
    root_ = dir;
  }

  void TearDown() override {
    umask(savedUmask_);
    if (!root_.empty()) {
      std::string cmd = "rm -rf '" + root_ + "'";
      if (system(cmd.c_str()) != 0) {
        // Best effort; a leaked scratch dir must not fail the test.
      }
    }
  }

  std::string root_;
  mode_t savedUmask_ = 0;
};

// --- Prometheus writer: inspectorDumpThread::getOrCreateFileHandle ---------

TEST_F(InspectorOutputFileTest,
       GetOrCreateFileHandle_NewFile_CreatesFileNotGroupOrWorldAccessible) {
  ScopedDumper dumper(root_, /*sampleIntervalUsecs=*/0);
  const std::string path = root_ + "/nccl_inspector_metrics_uuid0.prom";

  const int opened = test_inspectorGetOrCreateFileHandle(
      dumper.get(), "uuid0", path.c_str(), /*currentTime=*/1);

  ASSERT_EQ(opened, 1);
  const int perms = PermsOf(path);
  ASSERT_NE(perms, -1) << "output file was not created";
  EXPECT_EQ(perms & S_IWGRP, 0) << "group-writable, mode " << std::oct << perms;
  EXPECT_EQ(perms & S_IWOTH, 0) << "world-writable, mode " << std::oct << perms;
  EXPECT_EQ(perms & 0111, 0) << "executable, mode " << std::oct << perms;
}

TEST_F(InspectorOutputFileTest,
       GetOrCreateFileHandle_SymlinkAtOutputPath_LeavesTargetIntact) {
  const std::string target = root_ + "/victim-owned-file";
  WriteFile(target, "original contents\n");
  ASSERT_EQ(chmod(target.c_str(), 0600), 0);

  const std::string link = root_ + "/nccl_inspector_metrics_uuid0.prom";
  ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0);

  ScopedDumper dumper(root_, /*sampleIntervalUsecs=*/0);
  const int opened = test_inspectorGetOrCreateFileHandle(
      dumper.get(), "uuid0", link.c_str(), /*currentTime=*/1);

  EXPECT_EQ(opened, 0) << "opened through a symlink";
  EXPECT_EQ(ReadFile(target), "original contents\n") << "target was truncated";
  EXPECT_EQ(PermsOf(target), 0600) << "target permissions were widened";
}

// --- JSON writer: inspectorDumpThread::inspectorStateDumpJSON -------------

TEST_F(InspectorOutputFileTest,
       StateDumpJSON_NewFile_CreatesFileNotGroupOrWorldAccessible) {
  ScopedDumper dumper(root_, /*sampleIntervalUsecs=*/0);

  ASSERT_EQ(test_inspectorStateDumpJSON(dumper.get(), root_.c_str()),
            test_inspectorResultSuccess());

  const std::string path = JsonOutputPathFor(root_);
  const int perms = PermsOf(path);
  ASSERT_NE(perms, -1) << "output file was not created";
  EXPECT_EQ(perms & S_IWGRP, 0) << "group-writable, mode " << std::oct << perms;
  EXPECT_EQ(perms & S_IWOTH, 0) << "world-writable, mode " << std::oct << perms;
  EXPECT_EQ(perms & 0111, 0) << "executable, mode " << std::oct << perms;
}

TEST_F(InspectorOutputFileTest,
       StateDumpJSON_SymlinkAtOutputPath_LeavesTargetIntact) {
  const std::string target = root_ + "/victim-owned-file";
  WriteFile(target, "original contents\n");
  ASSERT_EQ(chmod(target.c_str(), 0600), 0);

  const std::string link = JsonOutputPathFor(root_);
  ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0);

  ScopedDumper dumper(root_, /*sampleIntervalUsecs=*/0);
  const int res = test_inspectorStateDumpJSON(dumper.get(), root_.c_str());

  EXPECT_EQ(res, test_inspectorResultFileOpenError())
      << "opened through a symlink";
  EXPECT_EQ(ReadFile(target), "original contents\n") << "target was truncated";
  EXPECT_EQ(PermsOf(target), 0600) << "target permissions were widened";
}

}  // namespace
