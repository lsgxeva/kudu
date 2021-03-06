// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>

#include "kudu/fs/block_manager.h"
#include "kudu/fs/data_dirs.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/flags.h"
#include "kudu/util/oid_generator.h"
#include "kudu/util/path_util.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::unordered_set;
using std::vector;
using strings::Substitute;

DECLARE_bool(crash_on_eio);
DECLARE_double(env_inject_eio);
DECLARE_string(block_manager);
DECLARE_string(env_inject_eio_globs);
DECLARE_string(umask);

namespace kudu {

class FsManagerTestBase : public KuduTest {
 public:
  FsManagerTestBase()
     : fs_root_(GetTestPath("fs_root")) {
  }

  void SetUp() OVERRIDE {
    KuduTest::SetUp();

    // Initialize File-System Layout
    ReinitFsManager();
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
  }

  void ReinitFsManager() {
    ReinitFsManagerWithPaths(fs_root_, { fs_root_ });
  }

  void ReinitFsManagerWithPaths(string wal_path, vector<string> data_paths) {
    FsManagerOpts opts;
    opts.wal_root = std::move(wal_path);
    opts.data_roots = std::move(data_paths);
    ReinitFsManagerWithOpts(std::move(opts));
  }

  void ReinitFsManagerWithOpts(FsManagerOpts opts) {
    fs_manager_.reset(new FsManager(env_, std::move(opts)));
  }

  void TestReadWriteDataFile(const Slice& data) {
    uint8_t buffer[64];
    DCHECK_LT(data.size(), sizeof(buffer));

    // Test Write
    unique_ptr<fs::WritableBlock> writer;
    ASSERT_OK(fs_manager()->CreateNewBlock({}, &writer));
    ASSERT_OK(writer->Append(data));
    ASSERT_OK(writer->Close());

    // Test Read
    Slice result(buffer, data.size());
    unique_ptr<fs::ReadableBlock> reader;
    ASSERT_OK(fs_manager()->OpenBlock(writer->id(), &reader));
    ASSERT_OK(reader->Read(0, result));
    ASSERT_EQ(0, result.compare(data));
  }

  FsManager *fs_manager() const { return fs_manager_.get(); }

 protected:
  const string fs_root_;

 private:
  unique_ptr<FsManager> fs_manager_;
};

TEST_F(FsManagerTestBase, TestBaseOperations) {
  fs_manager()->DumpFileSystemTree(std::cout);

  TestReadWriteDataFile(Slice("test0"));
  TestReadWriteDataFile(Slice("test1"));

  fs_manager()->DumpFileSystemTree(std::cout);
}

TEST_F(FsManagerTestBase, TestIllegalPaths) {
  vector<string> illegal = { "", "asdf", "/foo\n\t" };
  for (const string& path : illegal) {
    ReinitFsManagerWithPaths(path, { path });
    ASSERT_TRUE(fs_manager()->CreateInitialFileSystemLayout().IsIOError());
  }
}

TEST_F(FsManagerTestBase, TestMultiplePaths) {
  string wal_path = GetTestPath("a");
  vector<string> data_paths = { GetTestPath("a"), GetTestPath("b"), GetTestPath("c") };
  ReinitFsManagerWithPaths(wal_path, data_paths);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());
}

TEST_F(FsManagerTestBase, TestMatchingPathsWithMismatchedSlashes) {
  string wal_path = GetTestPath("foo");
  vector<string> data_paths = { wal_path + "/" };
  ReinitFsManagerWithPaths(wal_path, data_paths);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
}

TEST_F(FsManagerTestBase, TestDuplicatePaths) {
  string path = GetTestPath("foo");
  ReinitFsManagerWithPaths(path, { path, path, path });
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_EQ(vector<string>({ JoinPathSegments(path, fs_manager()->kDataDirName) }),
            fs_manager()->GetDataRootDirs());
}

TEST_F(FsManagerTestBase, TestListTablets) {
  vector<string> tablet_ids;
  ASSERT_OK(fs_manager()->ListTabletIds(&tablet_ids));
  ASSERT_EQ(0, tablet_ids.size());

  string path = fs_manager()->GetTabletMetadataDir();
  unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(
      JoinPathSegments(path, "foo.kudutmp"), &writer));
  ASSERT_OK(env_->NewWritableFile(
      JoinPathSegments(path, "foo.kudutmp.abc123"), &writer));
  ASSERT_OK(env_->NewWritableFile(
      JoinPathSegments(path, ".hidden"), &writer));
  ASSERT_OK(env_->NewWritableFile(
      JoinPathSegments(path, "a_tablet_sort_of"), &writer));

  ASSERT_OK(fs_manager()->ListTabletIds(&tablet_ids));
  ASSERT_EQ(1, tablet_ids.size()) << tablet_ids;
}

TEST_F(FsManagerTestBase, TestCannotUseNonEmptyFsRoot) {
  string path = GetTestPath("new_fs_root");
  ASSERT_OK(env_->CreateDir(path));
  {
    unique_ptr<WritableFile> writer;
    ASSERT_OK(env_->NewWritableFile(
        JoinPathSegments(path, "some_file"), &writer));
  }

  // Try to create the FS layout. It should fail.
  ReinitFsManagerWithPaths(path, { path });
  ASSERT_TRUE(fs_manager()->CreateInitialFileSystemLayout().IsAlreadyPresent());
}

TEST_F(FsManagerTestBase, TestEmptyWALPath) {
  ReinitFsManagerWithPaths("", {});
  Status s = fs_manager()->CreateInitialFileSystemLayout();
  ASSERT_TRUE(s.IsIOError());
  ASSERT_STR_CONTAINS(s.ToString(), "directory (fs_wal_dir) not provided");
}

TEST_F(FsManagerTestBase, TestOnlyWALPath) {
  string path = GetTestPath("new_fs_root");
  ASSERT_OK(env_->CreateDir(path));

  ReinitFsManagerWithPaths(path, {});
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_TRUE(HasPrefixString(fs_manager()->GetWalsRootDir(), path));
  ASSERT_TRUE(HasPrefixString(fs_manager()->GetConsensusMetadataDir(), path));
  ASSERT_TRUE(HasPrefixString(fs_manager()->GetTabletMetadataDir(), path));
  vector<string> data_dirs = fs_manager()->GetDataRootDirs();
  ASSERT_EQ(1, data_dirs.size());
  ASSERT_TRUE(HasPrefixString(data_dirs[0], path));
}

TEST_F(FsManagerTestBase, TestFormatWithSpecificUUID) {
  string path = GetTestPath("new_fs_root");
  ReinitFsManagerWithPaths(path, {});

  // Use an invalid uuid at first.
  string uuid = "not_a_valid_uuid";
  Status s = fs_manager()->CreateInitialFileSystemLayout(uuid);
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(), Substitute("invalid uuid $0", uuid));

  // Now use a valid one.
  ObjectIdGenerator oid_generator;
  uuid = oid_generator.Next();
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout(uuid));
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(uuid, fs_manager()->uuid());
}

Status CountTmpFiles(Env* env, const string& path, const vector<string>& children,
                     unordered_set<string>* checked_dirs, size_t* count) {
  vector<string> sub_objects;
  for (const string& name : children) {
    if (name == "." || name == "..") continue;

    string sub_path;
    RETURN_NOT_OK(env->Canonicalize(JoinPathSegments(path, name), &sub_path));
    bool is_directory;
    RETURN_NOT_OK(env->IsDirectory(sub_path, &is_directory));
    if (is_directory) {
      if (!ContainsKey(*checked_dirs, sub_path)) {
        checked_dirs->insert(sub_path);
        RETURN_NOT_OK(env->GetChildren(sub_path, &sub_objects));
        RETURN_NOT_OK(CountTmpFiles(env, sub_path, sub_objects, checked_dirs, count));
      }
    } else if (name.find(kTmpInfix) != string::npos) {
      (*count)++;
    }
  }
  return Status::OK();
}

Status CountTmpFiles(Env* env, const vector<string>& roots, size_t* count) {
  unordered_set<string> checked_dirs;
  for (const string& root : roots) {
    vector<string> children;
    RETURN_NOT_OK(env->GetChildren(root, &children));
    RETURN_NOT_OK(CountTmpFiles(env, root, children, &checked_dirs, count));
  }
  return Status::OK();
}

TEST_F(FsManagerTestBase, TestCreateWithFailedDirs) {
  string wal_path = GetTestPath("wals");
  // Create some top-level paths to place roots in.
  vector<string> data_paths = { GetTestPath("data1"), GetTestPath("data2"), GetTestPath("data3") };
  for (const string& path : data_paths) {
    env_->CreateDir(path);
  }
  // Initialize the FS layout with roots in subdirectories of data_paths. When
  // we canonicalize paths, we canonicalize the dirname of each path (e.g.
  // data1) to ensure it exists. With this, we can inject failures in
  // canonicalization by failing the dirname.
  vector<string> data_roots = JoinPathSegmentsV(data_paths, "root");

  FLAGS_crash_on_eio = false;
  FLAGS_env_inject_eio = 1.0;

  // Fail a directory, avoiding the metadata directory.
  FLAGS_env_inject_eio_globs = data_paths[1];
  ReinitFsManagerWithPaths(wal_path, data_roots);
  Status s = fs_manager()->CreateInitialFileSystemLayout();
  ASSERT_STR_MATCHES(s.ToString(), "cannot create FS layout; at least one directory "
                                   "failed to canonicalize");
  FLAGS_env_inject_eio = 0;
}

TEST_F(FsManagerTestBase, TestOpenWithFailedDirs) {
  // Successfully create an FS layout.
  string wal_path = GetTestPath("wals");
  vector<string> data_paths = { GetTestPath("data1"), GetTestPath("data2"), GetTestPath("data3") };
  for (const string& path : data_paths) {
    env_->CreateDir(path);
  }
  vector<string> data_roots = { GetTestPath("data1/roots"), GetTestPath("data2/roots"),
                                GetTestPath("data3/roots") };
  ReinitFsManagerWithPaths(wal_path, data_roots);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());

  // Now fail one of the directories.
  FLAGS_crash_on_eio = false;
  FLAGS_env_inject_eio = 1.0;
  FLAGS_env_inject_eio_globs = JoinPathSegments(Substitute("$0,$0", data_paths[1]), "**");
  ReinitFsManagerWithPaths(wal_path, data_roots);
  ASSERT_OK(fs_manager()->Open(nullptr));
  ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDataDirs().size());

  FLAGS_env_inject_eio = 0;
}

TEST_F(FsManagerTestBase, TestTmpFilesCleanup) {
  string wal_path = GetTestPath("wals");
  vector<string> data_paths = { GetTestPath("data1"), GetTestPath("data2"), GetTestPath("data3") };
  ReinitFsManagerWithPaths(wal_path, data_paths);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());

  // Create a few tmp files here
  shared_ptr<WritableFile> tmp_writer;

  string tmp_path = JoinPathSegments(fs_manager()->GetWalsRootDir(), "wal.kudutmp.file");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->env(), tmp_path, &tmp_writer));

  tmp_path = JoinPathSegments(fs_manager()->GetDataRootDirs()[0], "data1.kudutmp.file");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->env(), tmp_path, &tmp_writer));

  tmp_path = JoinPathSegments(fs_manager()->GetConsensusMetadataDir(), "12345.kudutmp.asdfg");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->env(), tmp_path, &tmp_writer));

  tmp_path = JoinPathSegments(fs_manager()->GetTabletMetadataDir(), "12345.kudutmp.asdfg");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->env(), tmp_path, &tmp_writer));

  // Not a misprint here: checking for just ".kudutmp" as well
  tmp_path = JoinPathSegments(fs_manager()->GetDataRootDirs()[1], "data2.kudutmp");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->env(), tmp_path, &tmp_writer));

  // Try with nested directory
  string nested_dir_path = JoinPathSegments(fs_manager()->GetDataRootDirs()[2], "data4");
  ASSERT_OK(env_util::CreateDirIfMissing(fs_manager()->env(), nested_dir_path));
  tmp_path = JoinPathSegments(nested_dir_path, "data4.kudutmp.file");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->env(), tmp_path, &tmp_writer));

  // Add a loop using symlink
  string data3_link = JoinPathSegments(nested_dir_path, "data3-link");
  int symlink_error = symlink(fs_manager()->GetDataRootDirs()[2].c_str(), data3_link.c_str());
  ASSERT_EQ(0, symlink_error);

  vector<string> lookup_dirs = fs_manager()->GetDataRootDirs();
  lookup_dirs.emplace_back(fs_manager()->GetWalsRootDir());
  lookup_dirs.emplace_back(fs_manager()->GetConsensusMetadataDir());
  lookup_dirs.emplace_back(fs_manager()->GetTabletMetadataDir());

  size_t n_tmp_files = 0;
  ASSERT_OK(CountTmpFiles(fs_manager()->env(), lookup_dirs, &n_tmp_files));
  ASSERT_EQ(6, n_tmp_files);

  // Opening fs_manager should remove tmp files
  ReinitFsManagerWithPaths(wal_path, data_paths);
  ASSERT_OK(fs_manager()->Open());

  n_tmp_files = 0;
  ASSERT_OK(CountTmpFiles(fs_manager()->env(), lookup_dirs, &n_tmp_files));
  ASSERT_EQ(0, n_tmp_files);
}

namespace {

string FilePermsAsString(const string& path) {
  struct stat s;
  CHECK_ERR(stat(path.c_str(), &s));
  return StringPrintf("%03o", s.st_mode & ACCESSPERMS);
}

} // anonymous namespace

TEST_F(FsManagerTestBase, TestUmask) {
  // With the default umask, we should create files with permissions 600
  // and directories with permissions 700.
  ASSERT_EQ(077, g_parsed_umask) << "unexpected default value";
  string root = GetTestPath("fs_root");
  EXPECT_EQ("700", FilePermsAsString(root));
  EXPECT_EQ("700", FilePermsAsString(fs_manager()->GetConsensusMetadataDir()));
  EXPECT_EQ("600", FilePermsAsString(fs_manager()->GetInstanceMetadataPath(root)));

  // With umask 007, we should create files with permissions 660
  // and directories with 770.
  FLAGS_umask = "007";
  HandleCommonFlags();
  ASSERT_EQ(007, g_parsed_umask);
  root = GetTestPath("new_root");
  ReinitFsManagerWithPaths(root, { root });
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  EXPECT_EQ("770", FilePermsAsString(root));
  EXPECT_EQ("770", FilePermsAsString(fs_manager()->GetConsensusMetadataDir()));
  EXPECT_EQ("660", FilePermsAsString(fs_manager()->GetInstanceMetadataPath(root)));

  // If we change the umask back to being restrictive and re-open the filesystem,
  // the permissions on the root dir should be fixed accordingly.
  FLAGS_umask = "077";
  HandleCommonFlags();
  ASSERT_EQ(077, g_parsed_umask);
  ReinitFsManagerWithPaths(root, { root });
  ASSERT_OK(fs_manager()->Open());
  EXPECT_EQ("700", FilePermsAsString(root));
}

TEST_F(FsManagerTestBase, TestOpenFailsWhenMissingImportantDir) {
  const string kWalRoot = fs_manager()->GetWalsRootDir();

  ASSERT_OK(env_->DeleteDir(kWalRoot));
  ReinitFsManager();
  Status s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), "could not verify required directory");

  unique_ptr<WritableFile> f;
  ASSERT_OK(env_->NewWritableFile(kWalRoot, &f));
  s = fs_manager()->Open();
  ASSERT_TRUE(s.IsCorruption());
  ASSERT_STR_CONTAINS(s.ToString(), "exists but is not a directory");
}


TEST_F(FsManagerTestBase, TestAddDataDirs) {
  if (FLAGS_block_manager == "file") {
    LOG(INFO) << "Skipping test, file block manager not supported";
    return;
  }

  // Try to open with a new data dir in the list to be opened; this should fail.
  const string new_path1 = GetTestPath("new_path1");
  FsManagerOpts opts;
  opts.wal_root = fs_root_;
  opts.data_roots = { fs_root_, new_path1 };
  ReinitFsManagerWithOpts(opts);
  Status s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), fs_manager()->GetInstanceMetadataPath(new_path1));

  // This time allow new data directories to be created.
  opts.update_on_disk = true;
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(2, fs_manager()->dd_manager()->GetDataDirs().size());

  // Try to open with a data dir removed; this should fail.
  opts.data_roots = { fs_root_ };
  ReinitFsManagerWithOpts(opts);
  s = fs_manager()->Open();
  ASSERT_TRUE(s.IsIOError());
  ASSERT_STR_CONTAINS(s.ToString(), "could not verify integrity of files");

  // Try to open with a new data dir at the front of the list; this should fail.
  const string new_path2 = GetTestPath("new_path2");
  opts.data_roots = { new_path2, fs_root_, new_path1 };
  ReinitFsManagerWithOpts(opts);
  s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), "could not verify required directory");

  // But adding a new data dir elsewhere in the list is OK.
  opts.data_roots = { fs_root_, new_path2, new_path1 };
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(3, fs_manager()->dd_manager()->GetDataDirs().size());

  // Try to open with a new data dir although an existing data dir has failed;
  // this should fail.
  {
    google::FlagSaver saver;
    FLAGS_crash_on_eio = false;
    FLAGS_env_inject_eio = 1.0;
    FLAGS_env_inject_eio_globs = JoinPathSegments(new_path2, "**");

    const string new_path3 = GetTestPath("new_path3");
    opts.data_roots = { fs_root_, new_path2, new_path1, new_path3 };
    ReinitFsManagerWithOpts(opts);
    Status s = fs_manager()->Open();
    ASSERT_TRUE(s.IsIOError());
    ASSERT_STR_CONTAINS(s.ToString(), "found failed data directory");
  }
}

TEST_F(FsManagerTestBase, TestAddDataDirsFuzz) {
  const int kNumAttempts = AllowSlowTests() ? 1000 : 100;

  if (FLAGS_block_manager == "file") {
    LOG(INFO) << "Skipping test, file block manager not supported";
    return;
  }

  FsManagerOpts fs_opts;
  fs_opts.wal_root = fs_root_;
  fs_opts.data_roots = { fs_root_ };
  for (int i = 0; i < kNumAttempts; i++) {
    // Create a new fs root.
    string new_data_root = GetTestPath(Substitute("new_data_$0", i));
    fs_opts.data_roots.emplace_back(new_data_root);

    // Try to add it with failure injection enabled.
    bool add_succeeded;
    {
      google::FlagSaver saver;
      FLAGS_crash_on_eio = false;

      // This value isn't arbitrary: most attempts fail and only some succeed.
      FLAGS_env_inject_eio = 0.01;

      fs_opts.update_on_disk = true;
      ReinitFsManagerWithOpts(fs_opts);
      add_succeeded = fs_manager()->Open().ok();
    }

    // Reopen regardless, to ensure that failures didn't corrupt anything.
    fs_opts.update_on_disk = false;
    ReinitFsManagerWithOpts(fs_opts);
    Status open_status = fs_manager()->Open();
    if (add_succeeded) {
      ASSERT_OK(open_status);
    }

    // The rollback logic built into the add fs root operation isn't robust
    // enough to handle every possible sequence of injected failures. Let's see
    // if we need to apply a "workaround" in order to fix the filesystem.
    if (!open_status.ok()) {
      // Failed? Perhaps the new fs root and data directory were created, but
      // there was an injected failure later on, which led to a rollback and
      // the removal of the new fs instance file.
      //
      // Fix this as a user might (by copying the original fs instance file
      // into the new fs root) then retry.
      string source_instance = fs_manager()->GetInstanceMetadataPath(fs_root_);
      bool is_dir;
      Status s = env_->IsDirectory(new_data_root, &is_dir);
      if (s.ok()) {
        ASSERT_TRUE(is_dir);
        string new_instance = fs_manager()->GetInstanceMetadataPath(new_data_root);
        if (!env_->FileExists(new_instance)) {
          WritableFileOptions wr_opts;
          wr_opts.mode = Env::CREATE_NON_EXISTING;
          ASSERT_OK(env_util::CopyFile(env_, source_instance, new_instance, wr_opts));
          ReinitFsManagerWithOpts(fs_opts);
          open_status = fs_manager()->Open();
        }
      }
    }
    if (!open_status.ok()) {
      // Still failing? Unfortunately, there's not enough information to know
      // whether the injected failure occurred during creation of the new data
      // directory or just afterwards, when the DataDirManager reopened itself.
      // If the former, the add itself failed and the failure should resolve if
      // we drop the new fs root and retry.
      fs_opts.data_roots.pop_back();
      ReinitFsManagerWithOpts(fs_opts);
      open_status = fs_manager()->Open();
    }
    if (!open_status.ok()) {
      // We're still failing? Okay, there's only one legitimate case left, and
      // that's if an error was injected during the update of existing data
      // directory instance files AND during the restoration phase of cleanup.
      //
      // Fix this as a user might (by completing the restoration phase
      // manually), then retry.
      ASSERT_TRUE(open_status.IsIOError());
      bool repaired = false;
      for (const auto& root : fs_opts.data_roots) {
        string data_dir = JoinPathSegments(root, fs::kDataDirName);
        string instance = JoinPathSegments(data_dir,
                                           fs::kInstanceMetadataFileName);
        ASSERT_TRUE(env_->FileExists(instance));
        string copy = instance + kTmpInfix;
        if (env_->FileExists(copy)) {
          ASSERT_OK(env_->RenameFile(copy, instance));
          repaired = true;
        }
      }
      if (repaired) {
        ReinitFsManagerWithOpts(fs_opts);
        open_status = fs_manager()->Open();
      }
    }

    // We've exhausted all of our manual repair options; if this still fails,
    // something else is wrong.
    ASSERT_OK(open_status);
  }
}

} // namespace kudu
