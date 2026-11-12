// Host test for the cargo store (src/core/cargo_store.*; docs/adr/0045-cdn-texture-delivery.md): objects appended to a few store
// files, a blob index from SHA-256 to (file, offset, sizes), a boot that reads the index and hashes nothing, a kill at any point that
// leaves an honest store, and the mount into a shadow where one open file serves many objects.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/cargo_store.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;
using cargo_test::FakeEnv;
using cargo_test::MakeFrame;
using cargo_test::Sha256Of;

constexpr const char* kDir = "/cache";
constexpr std::uint64_t kArchive = 0x2a41cf1c1d9e5d32ULL;
constexpr std::uint64_t kOther = 0x1234567890abcdefULL;

nc::Limits TestLimits(std::uint64_t store_file_bytes = nc::kFormatMaxStoreBytes, std::uint64_t max_files = 8) {
  nc::Limits l;
  l.ok = true;
  l.manifest_text_bytes = std::uint64_t{64} << 20;
  l.max_entries = 1000000;
  l.max_object_bytes = std::uint64_t{1} << 30;
  l.store_file_bytes = store_file_bytes;
  l.max_store_files = max_files;
  l.storage_budget = std::uint64_t{50} << 30;
  l.index_bytes = nc::kIndexBytesFor(1000000);
  return l;
}

std::unique_ptr<nc::Store> Open(FakeEnv& env, const nc::Limits& l, nc::StoreOpenInfo* info = nullptr) {
  nc::StoreOpenInfo local;
  return nc::Store::Open(env, kDir, l, info != nullptr ? info : &local);
}

// Appends one object whole through the Writer; returns its sha.
std::string Put(nc::Store& store, const std::string& frame, std::uint64_t bytes, const char** reason = nullptr) {
  const char* why = "";
  std::unique_ptr<nc::Store::Writer> w = store.Begin(frame.size(), &why);
  if (reason != nullptr) *reason = why;
  if (!w) return std::string();
  const std::string sha = Sha256Of(frame);
  QCHECK(w->Write(frame.data(), frame.size() / 2));
  QCHECK(w->Write(frame.data() + frame.size() / 2, frame.size() - frame.size() / 2));
  QCHECK(w->Commit(sha, bytes));
  return sha;
}

const std::string& Store0(FakeEnv& env) { return env.fs_->files[nc::StoreFilePath(kDir, 0)]; }
const std::string& IndexBytes(FakeEnv& env) { return env.fs_->files[nc::IndexPath(kDir)]; }

void AnEmptyDirectoryOpensEmpty() {
  FakeEnv env;
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(), &info);
  QCHECK(store != nullptr);
  QCHECK(std::string(info.index) == "absent" && info.blobs == 0 && info.files == 0);
  QCHECK(!store->Has(std::string(64, 'a')) && store->Find(std::string(64, 'a')) == nullptr);
}

void ALimitsThatAreNotOkOpenNoStore() {
  FakeEnv env;
  nc::Limits l = TestLimits();
  l.ok = false;
  QCHECK(Open(env, l) == nullptr);
}

void ObjectsAreAppendedBackToBackToOneFileAndNamedByTheIndex() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const std::string a = MakeFrame(300, 120, 'a'), b = MakeFrame(500, 200, 'b');
  const std::string sa = Put(*store, a, 300), sb = Put(*store, b, 500);
  QCHECK(Store0(env) == a + b);
  const nc::BlobLoc* la = store->Find(sa);
  const nc::BlobLoc* lb = store->Find(sb);
  QCHECK(la != nullptr && lb != nullptr);
  if (la != nullptr && lb != nullptr) {
    QCHECK(la->file == 0 && la->offset == 0 && la->size == 120 && la->bytes == 300);
    QCHECK(lb->file == 0 && lb->offset == 120 && lb->size == 200 && lb->bytes == 500);
  }
  QCHECK(store->blob_count() == 2 && store->file_count() == 1 && store->held_bytes() == 320);
  QCHECK(env.fs_->files.count(nc::StoreFilePath(kDir, 1)) == 0);  // one file, not one per object
}

void TheIndexIsAHeaderAndOneFixedRecordPerObjectWithItsCrc() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const std::string a = MakeFrame(300, 120, 'a');
  const std::string sa = Put(*store, a, 300);
  const std::string& idx = IndexBytes(env);
  QCHECK(idx.size() == nc::kIndexHeaderBytes + nc::kIndexRecordBytes);
  QCHECK(idx.compare(0, 8, "NEVRCARG") == 0);
  std::uint32_t version = 0, recordBytes = 0;
  std::memcpy(&version, idx.data() + 8, 4);
  std::memcpy(&recordBytes, idx.data() + 12, 4);
  QCHECK(version == nc::kIndexVersion && recordBytes == nc::kIndexRecordBytes);
  const unsigned char* rec = reinterpret_cast<const unsigned char*>(idx.data()) + nc::kIndexHeaderBytes;
  std::string hex;
  for (int i = 0; i < 32; ++i) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", rec[i]);
    hex += buf;
  }
  QCHECK(hex == sa);
  std::uint32_t file = 9, offset = 9, size = 0, bytes = 0, crc = 0;
  std::memcpy(&file, rec + 32, 4);
  std::memcpy(&offset, rec + 36, 4);
  std::memcpy(&size, rec + 40, 4);
  std::memcpy(&bytes, rec + 44, 4);
  std::memcpy(&crc, rec + 48, 4);
  QCHECK(file == 0 && offset == 0 && size == 120 && bytes == 300);
  QCHECK(crc == nc::Crc32(rec, 48));
}

void TheCrcIsTheStandardOne() {
  QCHECK(nc::Crc32("123456789", 9) == 0xCBF43926u);
  QCHECK(nc::Crc32("", 0) == 0u);
}

void ABootReadsTheIndexAndHashesNothing() {
  FakeEnv env;
  std::string sa, sb;
  {
    std::unique_ptr<nc::Store> store = Open(env, TestLimits());
    sa = Put(*store, MakeFrame(300, 120, 'a'), 300);
    sb = Put(*store, MakeFrame(500, 200, 'b'), 500);
  }
  env.blob_bytes_read = 0;
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> again = Open(env, TestLimits(), &info);
  QCHECK(again != nullptr && std::string(info.index) == "ok");
  QCHECK(info.blobs == 2 && info.files == 1 && info.dropped == 0 && info.bytes == 320);
  QCHECK(again->Has(sa) && again->Has(sb));
  QCHECK(env.blob_bytes_read == 0);  // no object was read: it was hashed once, when it arrived
  QCHECK(env.open_blobs() == 0);     // and no file is held open by Open itself
}

void AStoreFileRollsWhenTheNextObjectWouldPassItsBound() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(/*store_file_bytes=*/250, /*max_files=*/2));
  const std::string s1 = Put(*store, MakeFrame(10, 100, '1'), 10);
  const std::string s2 = Put(*store, MakeFrame(10, 100, '2'), 10);
  const std::string s3 = Put(*store, MakeFrame(10, 100, '3'), 10);  // 200 + 100 > 250: the second file
  QCHECK(store->Find(s1)->file == 0 && store->Find(s2)->file == 0);
  QCHECK(store->Find(s3) != nullptr && store->Find(s3)->file == 1 && store->Find(s3)->offset == 0);
  QCHECK(env.fs_->files[nc::StoreFilePath(kDir, 0)].size() == 200);
  QCHECK(env.fs_->files[nc::StoreFilePath(kDir, 1)].size() == 100);
  QCHECK(store->file_count() == 2);
}

void TheFileCountIsHeldToTheDescriptorBudget() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(250, 2));
  const char* reason = "";
  Put(*store, MakeFrame(10, 200, '1'), 10);
  Put(*store, MakeFrame(10, 200, '2'), 10);
  const std::string third = Put(*store, MakeFrame(10, 200, '3'), 10, &reason);  // would be a third file
  QCHECK(third.empty() && std::string(reason) == "no_store_file");
  QCHECK(store->blob_count() == 2);
}

void AnObjectLargerThanAStoreFileIsRefusedByName() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(250, 2));
  const char* reason = "";
  const std::string s = Put(*store, MakeFrame(10, 251, 'x'), 10, &reason);
  QCHECK(s.empty() && std::string(reason) == "too_large");
}

void AnAbortedObjectLeavesTheFileAsItWas() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  Put(*store, MakeFrame(300, 120, 'a'), 300);
  const std::string before = Store0(env);
  const char* why = "";
  {
    std::unique_ptr<nc::Store::Writer> w = store->Begin(500, &why);
    QCHECK(w != nullptr);
    const std::string junk(300, 'j');
    QCHECK(w->Write(junk.data(), junk.size()));
    QCHECK(Store0(env).size() == before.size() + 300);
    w->Abort();
  }
  QCHECK(Store0(env) == before);
  {
    std::unique_ptr<nc::Store::Writer> w = store->Begin(500, &why);
    const std::string junk(300, 'j');
    QCHECK(w->Write(junk.data(), junk.size()));
  }  // destroyed with neither Commit nor Abort
  QCHECK(Store0(env) == before);
  QCHECK(store->blob_count() == 1 && IndexBytes(env).size() == nc::kIndexHeaderBytes + nc::kIndexRecordBytes);
  // And the next object goes where the failed one began.
  const std::string sb = Put(*store, MakeFrame(500, 200, 'b'), 500);
  QCHECK(store->Find(sb)->offset == 120);
}

void OnlyOneWriterIsLiveAtATime() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const char* why = "";
  std::unique_ptr<nc::Store::Writer> w = store->Begin(100, &why);
  QCHECK(w != nullptr);
  std::unique_ptr<nc::Store::Writer> w2 = store->Begin(100, &why);
  QCHECK(w2 == nullptr && std::string(why) == "writer_live");
  w.reset();
  QCHECK(store->Begin(100, &why) != nullptr);
}

void AFullDiskFailsTheWriteAndTheBytesAreGivenBack() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  Put(*store, MakeFrame(300, 120, 'a'), 300);
  env.disk_bytes_left = 50;
  const char* why = "";
  std::unique_ptr<nc::Store::Writer> w = store->Begin(200, &why);
  const std::string junk(100, 'j');
  QCHECK(!w->Write(junk.data(), junk.size()));
  w->Abort();
  QCHECK(Store0(env).size() == 120);
}

void ATornIndexRecordIsCutOffAtTheNextBoot() {
  FakeEnv env;
  std::string sa;
  {
    std::unique_ptr<nc::Store> store = Open(env, TestLimits());
    sa = Put(*store, MakeFrame(300, 120, 'a'), 300);
  }
  env.fs_->files[nc::IndexPath(kDir)] += std::string(20, '\x7f');  // a record cut by a kill
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(), &info);
  QCHECK(std::string(info.index) == "tail_cut" && info.blobs == 1 && store->Has(sa));
  QCHECK(IndexBytes(env).size() == nc::kIndexHeaderBytes + nc::kIndexRecordBytes);
}

void ARecordWithABadCrcEndsTheIndexThere() {
  FakeEnv env;
  std::string sa, sb, sc;
  {
    std::unique_ptr<nc::Store> store = Open(env, TestLimits());
    sa = Put(*store, MakeFrame(300, 120, 'a'), 300);
    sb = Put(*store, MakeFrame(500, 200, 'b'), 500);
    sc = Put(*store, MakeFrame(500, 200, 'c'), 500);
  }
  env.fs_->files[nc::IndexPath(kDir)][nc::kIndexHeaderBytes + nc::kIndexRecordBytes + 40] ^= 0x01;  // the second record's size
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(), &info);
  QCHECK(std::string(info.index) == "tail_cut");
  QCHECK(store->Has(sa) && !store->Has(sb) && !store->Has(sc));  // what follows a record that cannot be trusted is not trusted either
}

void AnIndexWithAWrongHeaderIsResetAndWorksAgain() {
  FakeEnv env;
  env.fs_->files[nc::IndexPath(kDir)] = std::string(200, 'q');
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(), &info);
  QCHECK(store != nullptr && std::string(info.index) == "invalid" && info.blobs == 0);
  const std::string sa = Put(*store, MakeFrame(300, 120, 'a'), 300);
  QCHECK(IndexBytes(env).compare(0, 8, "NEVRCARG") == 0);
  QCHECK(IndexBytes(env).size() == nc::kIndexHeaderBytes + nc::kIndexRecordBytes);
  QCHECK(Open(env, TestLimits())->Has(sa));
}

void BytesNoRecordNamesAreCutOffBeforeTheNextObject() {
  FakeEnv env;
  std::string sa;
  {
    std::unique_ptr<nc::Store> store = Open(env, TestLimits());
    sa = Put(*store, MakeFrame(300, 120, 'a'), 300);
  }
  env.fs_->files[nc::StoreFilePath(kDir, 0)] += std::string(77, 'o');  // an object whose index record was never written
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  QCHECK(Store0(env).size() == 197);  // Open leaves the file alone: it is a read
  const std::string sb = Put(*store, MakeFrame(500, 200, 'b'), 500);
  QCHECK(store->Find(sb)->offset == 120);  // the orphan was given back
  QCHECK(Store0(env).size() == 320);
  QCHECK(store->Has(sa));
}

void ARecordPastTheEndOfItsStoreFileIsDroppedAndCounted() {
  FakeEnv env;
  std::string sa, sb;
  {
    std::unique_ptr<nc::Store> store = Open(env, TestLimits());
    sa = Put(*store, MakeFrame(300, 120, 'a'), 300);
    sb = Put(*store, MakeFrame(500, 200, 'b'), 500);
  }
  env.fs_->files[nc::StoreFilePath(kDir, 0)].resize(250);  // cut short: the second object is not whole
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(), &info);
  QCHECK(info.dropped == 1 && info.blobs == 1 && store->Has(sa) && !store->Has(sb));
  QCHECK(info.bytes == 120);
}

void ARecordNamingAFileThatIsNotThereIsDropped() {
  FakeEnv env;
  std::string sa;
  {
    std::unique_ptr<nc::Store> store = Open(env, TestLimits(250, 4));
    sa = Put(*store, MakeFrame(10, 200, '1'), 10);
    Put(*store, MakeFrame(10, 200, '2'), 10);  // file 1
  }
  env.fs_->files.erase(nc::StoreFilePath(kDir, 1));
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(250, 4), &info);
  QCHECK(info.dropped == 1 && info.blobs == 1 && store->Has(sa));
}

void ARecordNamingAFileBeyondTheLimitIsDropped() {
  FakeEnv env;
  {
    std::unique_ptr<nc::Store> store = Open(env, TestLimits(250, 4));
    Put(*store, MakeFrame(10, 200, '1'), 10);
    Put(*store, MakeFrame(10, 200, '2'), 10);  // file 1
  }
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(250, 1), &info);  // the device now allows one file
  QCHECK(info.blobs == 1 && info.dropped == 1);
}

void VerifyReadsEveryObjectBackAndFindsAFlippedByte() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  Put(*store, MakeFrame(300, 120, 'a'), 300);
  const std::string sb = Put(*store, MakeFrame(500, 200, 'b'), 500);
  std::size_t checked = 0;
  QCHECK(store->Verify(&checked) == 0 && checked == 2);
  env.fs_->files[nc::StoreFilePath(kDir, 0)][150] ^= 0x40;  // inside the second object
  QCHECK(store->Verify(&checked) == 1 && checked == 2);
  (void)sb;
}

// ── the mount ────────────────────────────────────────────────────────────────────────────────────────────────────────────
nc::Entry MakeEntry(const char* label, std::uint64_t type, std::uint64_t name, const std::string& frame, std::uint64_t bytes,
                    std::uint32_t align = 16) {
  nc::Entry e;
  e.label = label;
  e.platform = "android-arm64";
  e.type = type;
  e.name = name;
  e.sha256 = Sha256Of(frame);
  e.size = frame.size();
  e.bytes = bytes;
  e.align = align;
  return e;
}

void AMountGivesOneHandleForManyObjectsAndOneChunkForAnObject() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const std::string a = MakeFrame(300, 120, 'a'), b = MakeFrame(500, 200, 'b');
  Put(*store, a, 300);
  Put(*store, b, 500);
  nc::Manifest m;
  m.labels = {"game"};
  m.entries = {MakeEntry("game", kArchive, 1, a, 300), MakeEntry("game", kOther, 2, b, 500, 4096), MakeEntry("game", kOther, 3, a, 300)};
  const nc::Mounted mounted = nc::Mount(env, *store, m, nc::kAndroidArm64, nc::Policy());
  QCHECK(mounted.shadow != nullptr);
  QCHECK(mounted.records == 3 && mounted.names == 3 && mounted.missing == 0 && mounted.refused == 0);
  QCHECK(mounted.files == 1 && env.open_blobs() == 1);  // three records, one open file
  QCHECK(mounted.shadow->chunk_count() == 2);           // entries 1 and 3 name the same object: one chunk
  nevr_shadow::Found f1, f2, f3;
  QCHECK(mounted.shadow->Find(kArchive, 1, &f1) && mounted.shadow->Find(kOther, 2, &f2) && mounted.shadow->Find(kOther, 3, &f3));
  QCHECK(f1.chunk == f3.chunk && f1.chunk != f2.chunk);
  QCHECK(f1.handle == f2.handle && f1.package == 0);
  QCHECK(f1.file_offset == 0 && f1.compressed == 120 && f1.decompressed == 300 && f1.size == 300 && f1.alignment == 16);
  QCHECK(f2.file_offset == 120 && f2.compressed == 200 && f2.decompressed == 500 && f2.size == 500 && f2.alignment == 4096);
  QCHECK(f1.offset_in_chunk == 0 && f2.offset_in_chunk == 0);
  // The handle the engine is given reads the stored frame at the chunk's offset.
  std::string got(200, '\0');
  std::size_t n = 0;
  QCHECK(env.ReadBlob(f2.handle, f2.file_offset, &got[0], got.size(), &n) && n == 200 && got == b);
}

void ARecordResolvesLikeTheEngineDoes() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits(250, 4));
  const std::string a = MakeFrame(300, 200, 'a'), b = MakeFrame(500, 200, 'b');
  Put(*store, a, 300);
  Put(*store, b, 500);  // the second store file
  nc::Manifest m;
  m.labels = {"game"};
  m.entries = {MakeEntry("game", kArchive, 1, a, 300), MakeEntry("game", kOther, 2, b, 500)};
  const nc::Mounted mounted = nc::Mount(env, *store, m, nc::kAndroidArm64, nc::Policy());
  QCHECK(mounted.files == 2 && env.open_blobs() == 2);
  nevr_shadow::Found f1, f2;
  QCHECK(nevr_shadow::ReadLikeTheEngine(mounted.shadow->manager(), kArchive, 1, &f1));
  QCHECK(nevr_shadow::ReadLikeTheEngine(mounted.shadow->manager(), kOther, 2, &f2));
  QCHECK(f1.handle != f2.handle && f1.package != f2.package);
  QCHECK(f1.chunk >= nevr_shadow::kFirstChunkIndex);
}

void ANameWhoseBlobIsNotHeldIsLeftOutWholeAndCounted() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const std::string a = MakeFrame(300, 120, 'a'), b = MakeFrame(500, 200, 'b');
  Put(*store, a, 300);  // b is never stored
  nc::Manifest m;
  m.labels = {"game"};
  // Name 7 has two on-disk records, one held and one not: neither mounts. Name 8 is whole.
  m.entries = {MakeEntry("game", kArchive, 7, a, 300), MakeEntry("game", kOther, 7, b, 500), MakeEntry("game", kOther, 8, a, 300)};
  const nc::Mounted mounted = nc::Mount(env, *store, m, nc::kAndroidArm64, nc::Policy());
  QCHECK(mounted.names == 1 && mounted.records == 1 && mounted.missing == 1);
  nevr_shadow::Found f;
  QCHECK(!mounted.shadow->Find(kArchive, 7, &f) && mounted.shadow->Find(kOther, 8, &f));
}

void ALabelThatIsOffAndAnotherPlatformsEntryAreNotMounted() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const std::string a = MakeFrame(300, 120, 'a');
  Put(*store, a, 300);
  nc::Manifest m;
  m.labels = {"game", "extra", "other"};
  nc::Entry win = MakeEntry("game", kArchive, 4, a, 300);
  win.platform = "windows-x86_64";
  m.entries = {MakeEntry("game", kArchive, 1, a, 300), MakeEntry("extra", kArchive, 2, a, 300), win};
  const nc::Mounted mounted = nc::Mount(env, *store, m, nc::kAndroidArm64, nc::Policy());  // `extra` is off by code default
  QCHECK(mounted.records == 1 && mounted.labels.size() == 1 && mounted.labels[0] == "game");
  nevr_shadow::Found f;
  QCHECK(mounted.shadow->Find(kArchive, 1, &f) && !mounted.shadow->Find(kArchive, 2, &f) && !mounted.shadow->Find(kArchive, 4, &f));
  std::uint32_t user = 99;
  QCHECK(mounted.shadow->Find(kArchive, 1, &f, &user) && user == 0);
}

void ARecordOfZeroBytesPointsAtARealChunkWithSizeZero() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const std::string a = MakeFrame(300, 120, 'a');
  Put(*store, a, 300);
  nc::Manifest m;
  m.labels = {"game"};
  m.entries = {MakeEntry("game", kArchive, 1, a, 300)};
  nc::EmptyRecord e;
  e.type = kOther;
  e.name = 50;
  e.align = 4;
  m.empties = {e};
  const nc::Mounted mounted = nc::Mount(env, *store, m, nc::kAndroidArm64, nc::Policy());
  QCHECK(mounted.records == 2 && mounted.empty_records == 1);
  nevr_shadow::Found real, empty;
  QCHECK(mounted.shadow->Find(kArchive, 1, &real) && mounted.shadow->Find(kOther, 50, &empty));
  QCHECK(empty.size == 0 && empty.offset_in_chunk == 0 && empty.alignment == 4);
  QCHECK(empty.chunk == real.chunk);  // a chunk that exists: the engine reads the chunk a record names whatever its size
  QCHECK(empty.compressed == 120 && empty.handle == real.handle);
}

void EmptyRecordsWithNoObjectToBorrowAChunkFromAreRefused() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  nc::Manifest m;
  m.labels = {"game"};
  nc::EmptyRecord e;
  e.type = kOther;
  e.name = 50;
  e.align = 4;
  m.empties = {e};
  const nc::Mounted mounted = nc::Mount(env, *store, m, nc::kAndroidArm64, nc::Policy());
  QCHECK(mounted.shadow == nullptr && mounted.refused == 1 && mounted.records == 0);
}

void NothingToMountGivesANullShadowAndHoldsNoFile() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const nc::Mounted mounted = nc::Mount(env, *store, nc::Manifest(), nc::kAndroidArm64, nc::Policy());
  QCHECK(mounted.shadow == nullptr && env.open_blobs() == 0);
}

void AStoreFileThatCannotBeOpenedFailsItsNamesAndLeaksNothing() {
  FakeEnv env;
  std::unique_ptr<nc::Store> store = Open(env, TestLimits());
  const std::string a = MakeFrame(300, 120, 'a');
  Put(*store, a, 300);
  env.fs_->files.erase(nc::StoreFilePath(kDir, 0));  // gone after the store was opened
  nc::Manifest m;
  m.labels = {"game"};
  m.entries = {MakeEntry("game", kArchive, 1, a, 300)};
  const nc::Mounted mounted = nc::Mount(env, *store, m, nc::kAndroidArm64, nc::Policy());
  QCHECK(mounted.shadow == nullptr && mounted.refused == 1 && env.open_blobs() == 0);
}

void TheHexFormIsTheManifestsForm() {
  QCHECK(nc::FormatHex64(0) == "0x0");
  QCHECK(nc::FormatHex64(0x2a41cf1c1d9e5d32ULL) == "0x2a41cf1c1d9e5d32");
  QCHECK(nc::FormatHex64(0xffffffffffffffffULL) == "0xffffffffffffffff");
}

}  // namespace

int main() {
  AnEmptyDirectoryOpensEmpty();
  ALimitsThatAreNotOkOpenNoStore();
  ObjectsAreAppendedBackToBackToOneFileAndNamedByTheIndex();
  TheIndexIsAHeaderAndOneFixedRecordPerObjectWithItsCrc();
  TheCrcIsTheStandardOne();
  ABootReadsTheIndexAndHashesNothing();
  AStoreFileRollsWhenTheNextObjectWouldPassItsBound();
  TheFileCountIsHeldToTheDescriptorBudget();
  AnObjectLargerThanAStoreFileIsRefusedByName();
  AnAbortedObjectLeavesTheFileAsItWas();
  OnlyOneWriterIsLiveAtATime();
  AFullDiskFailsTheWriteAndTheBytesAreGivenBack();
  ATornIndexRecordIsCutOffAtTheNextBoot();
  ARecordWithABadCrcEndsTheIndexThere();
  AnIndexWithAWrongHeaderIsResetAndWorksAgain();
  BytesNoRecordNamesAreCutOffBeforeTheNextObject();
  ARecordPastTheEndOfItsStoreFileIsDroppedAndCounted();
  ARecordNamingAFileThatIsNotThereIsDropped();
  ARecordNamingAFileBeyondTheLimitIsDropped();
  VerifyReadsEveryObjectBackAndFindsAFlippedByte();
  AMountGivesOneHandleForManyObjectsAndOneChunkForAnObject();
  ARecordResolvesLikeTheEngineDoes();
  ANameWhoseBlobIsNotHeldIsLeftOutWholeAndCounted();
  ALabelThatIsOffAndAnotherPlatformsEntryAreNotMounted();
  ARecordOfZeroBytesPointsAtARealChunkWithSizeZero();
  EmptyRecordsWithNoObjectToBorrowAChunkFromAreRefused();
  NothingToMountGivesANullShadowAndHoldsNoFile();
  AStoreFileThatCannotBeOpenedFailsItsNamesAndLeaksNothing();
  TheHexFormIsTheManifestsForm();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_store_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_store_test: all checks passed\n");
  return 0;
}
