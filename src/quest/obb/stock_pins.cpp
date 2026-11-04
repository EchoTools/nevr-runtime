#include "quest/obb/stock_pins.h"
namespace nevr_quest::obb {
Status ParseStockPins(std::string_view, StockPins*) { return Status::Fail("not_implemented", "stub"); }
Status CheckObbAgainstPins(const StockPins&, const std::string&, std::uint64_t) { return Status::Fail("not_implemented", "stub"); }
Status CheckTreeAgainstPins(const StockPins&, const std::vector<MeasuredFile>&) { return Status::Fail("not_implemented", "stub"); }
}  // namespace nevr_quest::obb
