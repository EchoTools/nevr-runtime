#include "quest/obb/stock_data.h"
namespace nevr_quest::obb {
Status BuildStockData(const StockDataOptions&, BuildLog&, StockDataResult*) { return Status::Fail("not_implemented", "stub"); }
Status HashFile(const std::string&, std::string*, std::uint64_t*) { return Status::Fail("not_implemented", "stub"); }
}  // namespace nevr_quest::obb
