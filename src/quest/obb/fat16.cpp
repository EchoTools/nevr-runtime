#include "quest/obb/fat16.h"
namespace nevr_quest::obb {
Status Fat16Volume::Open(const ByteSource&, std::unique_ptr<Fat16Volume>*) { return Status::Fail("not_implemented", "stub"); }
Status Fat16Volume::ReadDirectory(const FatEntry*, std::vector<FatEntry>*) const { return Status::Fail("not_implemented", "stub"); }
Status Fat16Volume::ReadFile(const FatEntry&, const Sink&) const { return Status::Fail("not_implemented", "stub"); }
Status Fat16Volume::Walk(const Visitor&) const { return Status::Fail("not_implemented", "stub"); }
}  // namespace nevr_quest::obb
