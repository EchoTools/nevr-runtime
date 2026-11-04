#include "quest/obb/byte_source.h"
namespace nevr_quest::obb {
Status MemorySource::ReadAt(std::uint64_t, void*, std::size_t) const { return Status::Fail("not_implemented", "stub"); }
FileSource::~FileSource() = default;
Status FileSource::Open(const std::string&) { return Status::Fail("not_implemented", "stub"); }
Status FileSource::ReadAt(std::uint64_t, void*, std::size_t) const { return Status::Fail("not_implemented", "stub"); }
}  // namespace nevr_quest::obb
