/**
 * @file pnsovr_iap.cpp
 * @brief NEVR PNSOvr Compatibility - In-App Purchase Implementation
 *
 * Binary reference: pnsovr.dll v34.4 (Echo VR)
 */

#include "pnsovr_iap.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

/**
 * @brief Internal implementation for IAPSubsystem.
 *
 * Reference: IAP storage at 0x1801d0000+
 */
#pragma warning(push)
#pragma warning(disable : 4625 5026 4626 5027)  // Suppress copy constructor warnings
struct IAPSubsystem::Impl {
  // Product catalog
  // Reference: Product storage at 0x1801d0000
  std::map<std::string, Product> products;  // Indexed by SKU

  // Purchase history
  // Reference: Purchase storage at 0x1801d0200
  std::map<uint64_t, Purchase> purchases;  // Indexed by transaction ID
  uint64_t next_transaction_id;

  // Thread safety
  mutable std::mutex products_mutex;
  mutable std::mutex purchases_mutex;

  Impl() : next_transaction_id(1) {}
};
#pragma warning(pop)

IAPSubsystem::IAPSubsystem() : impl_(new Impl()) {}

IAPSubsystem::~IAPSubsystem() {
  if (impl_) {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
  }
}

bool IAPSubsystem::Initialize() {
  // Reference: Initialization at 0x1801d0000
  return true;
}

void IAPSubsystem::Shutdown() {
  // Reference: Cleanup at 0x1801d0f00
  {
    std::lock_guard<std::mutex> lock(impl_->products_mutex);
    impl_->products.clear();
  }

  {
    std::lock_guard<std::mutex> lock(impl_->purchases_mutex);
    impl_->purchases.clear();
  }
}

std::vector<Product> IAPSubsystem::GetProductsBySKU(const std::vector<std::string>& skus) {
  // Reference: Lookup at 0x1801d0000 (ovr_IAP_GetProductsBySKU)
  std::lock_guard<std::mutex> lock(impl_->products_mutex);

  std::vector<Product> result;
  for (const auto& sku : skus) {
    auto it = impl_->products.find(sku);
    if (it != impl_->products.end()) {
      result.push_back(it->second);
    }
  }

  return result;
}

ProductPage IAPSubsystem::GetProductCatalog(uint32_t offset, uint32_t limit) {
  // Reference: Enumeration at 0x1801d0400 (ovr_IAP_GetNextProductArrayPage)
  std::lock_guard<std::mutex> lock(impl_->products_mutex);

  ProductPage page;
  page.offset = offset;
  page.limit = limit;
  page.total_count = (uint32_t)impl_->products.size();

  uint32_t current_index = 0;
  for (const auto& pair : impl_->products) {
    if (current_index >= offset && page.items.size() < limit) {
      page.items.push_back(pair.second);
    }
    current_index++;
  }

  // Set has_next_page flag
  page.has_next_page = (offset + limit) < page.total_count;

  return page;
}

uint64_t IAPSubsystem::LaunchCheckoutFlow(const std::vector<std::string>& skus) {
  // Reference: Checkout at 0x1801d0100 (ovr_IAP_LaunchCheckoutFlow)
  std::lock_guard<std::mutex> lock(impl_->purchases_mutex);

  // Validate all SKUs exist
  {
    std::lock_guard<std::mutex> products_lock(impl_->products_mutex);
    for (const auto& sku : skus) {
      if (impl_->products.find(sku) == impl_->products.end()) {
        return 0;  // Invalid SKU
      }
    }
  }

  // Create transaction (in real implementation, this would launch payment UI)
  uint64_t transaction_id = impl_->next_transaction_id++;

  // Create purchase record for first SKU (simplified - real impl would handle multiple)
  if (!skus.empty()) {
    Purchase purchase;
    purchase.transaction_id = transaction_id;
    purchase.sku = skus[0];
    purchase.purchase_time =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    purchase.durable = true;
    purchase.receipt_token = "receipt_" + std::to_string(transaction_id);
    purchase.consumed = false;

    impl_->purchases[transaction_id] = purchase;
  }

  return transaction_id;
}

std::vector<Purchase> IAPSubsystem::GetPurchaseHistory() {
  // Reference: History at 0x1801d0200 (ovr_IAP_GetViewerPurchases)
  std::lock_guard<std::mutex> lock(impl_->purchases_mutex);

  std::vector<Purchase> result;
  for (const auto& pair : impl_->purchases) {
    result.push_back(pair.second);
  }

  return result;
}

std::vector<Purchase> IAPSubsystem::GetDurablePurchases() {
  // Reference: Cache at 0x1801d0300 (ovr_IAP_GetViewerPurchasesDurableCache)
  std::lock_guard<std::mutex> lock(impl_->purchases_mutex);

  std::vector<Purchase> result;
  for (const auto& pair : impl_->purchases) {
    if (pair.second.durable && !pair.second.consumed) {
      result.push_back(pair.second);
    }
  }

  return result;
}

bool IAPSubsystem::VerifyPurchaseReceipt(const std::string& receipt_token) {
  // Reference: Verification at 0x1801d0500
  std::lock_guard<std::mutex> lock(impl_->purchases_mutex);

  // Simple verification: check if receipt token exists in database
  // Real implementation would do cryptographic signature validation
  for (const auto& pair : impl_->purchases) {
    if (pair.second.receipt_token == receipt_token) {
      return true;
    }
  }

  return false;
}

bool IAPSubsystem::ConsumePurchase(uint64_t transaction_id) {
  // Reference: Consumption at 0x1801d0600
  std::lock_guard<std::mutex> lock(impl_->purchases_mutex);

  auto it = impl_->purchases.find(transaction_id);
  if (it == impl_->purchases.end()) {
    return false;
  }

  it->second.consumed = true;
  return true;
}

bool IAPSubsystem::AddProduct(const Product& product) {
  // Reference: Catalog update at 0x1801d0000
  std::lock_guard<std::mutex> lock(impl_->products_mutex);

  impl_->products[product.sku] = product;
  return true;
}

bool IAPSubsystem::RecordPurchase(const Purchase& purchase) {
  // Reference: Purchase recording at 0x1801d0700
  std::lock_guard<std::mutex> lock(impl_->purchases_mutex);

  impl_->purchases[purchase.transaction_id] = purchase;
  return true;
}
