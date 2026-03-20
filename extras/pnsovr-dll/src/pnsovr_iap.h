/**
 * @file pnsovr_iap.h
 * @brief NEVR PNSOvr Compatibility - In-App Purchases
 *
 * Implements product catalog and purchase transaction management.
 *
 * Binary Reference: pnsovr.dll v34.4 (Echo VR)
 * - ovr_IAP_GetProductsBySKU: 0x1801d0000
 * - ovr_IAP_LaunchCheckoutFlow: 0x1801d0100
 * - ovr_IAP_GetViewerPurchases: 0x1801d0200
 * - ovr_IAP_GetViewerPurchasesDurableCache: 0x1801d0300
 * - ovr_IAP_GetNextProductArrayPage: 0x1801d0400
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Product information for in-app purchase.
 *
 * Binary reference: Structure at 0x1801d0000
 * Size: 256 bytes
 *
 * Field layout:
 * +0x00: sku (string, max 64 chars)
 * +0x40: name (string, max 128 chars)
 * +0xc0: description (string, max 256 chars)
 * +0x1c0: price (string, formatted e.g. "$9.99")
 * +0x200: price_in_cents (uint64_t)
 * +0x208: image_path (string, max 256 chars)
 * +0x308: released (uint8_t)
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress struct padding warnings
struct Product {
  std::string sku;          // Stock keeping unit (e.g., "monthly_sub")
  std::string name;         // Display name (e.g., "Monthly Pass")
  std::string description;  // Product description
  std::string price;        // Formatted price (e.g., "$9.99")
  uint64_t price_in_cents;  // Price in cents (999 = $9.99)
  std::string image_path;   // URL/path to product image
  bool released;            // Whether product is currently available
};
#pragma warning(pop)

/**
 * @brief Product catalog page (supports pagination).
 *
 * Binary reference: Page structure at 0x1801d0400
 * Size: 4096+ bytes (variable due to product array)
 *
 * Field layout:
 * +0x00: items (vector<Product>)
 * +0x80: offset (uint32_t)
 * +0x84: limit (uint32_t)
 * +0x88: has_next_page (uint8_t)
 * +0x89: total_count (uint32_t)
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress struct padding warnings
struct ProductPage {
  std::vector<Product> items;
  uint32_t offset;
  uint32_t limit;
  bool has_next_page;
  uint32_t total_count;
};
#pragma warning(pop)

/**
 * @brief Purchase transaction record.
 *
 * Binary reference: Structure at 0x1801d0200
 * Size: 256 bytes
 *
 * Field layout:
 * +0x00: transaction_id (uint64_t)
 * +0x08: sku (string, max 64 chars)
 * +0x48: purchase_time (int64_t)
 * +0x50: durable (uint8_t) - persistent across sessions
 * +0x51: receipt_token (string, max 256 chars)
 * +0x151: consumed (uint8_t) - marked as used
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress struct padding warnings
struct Purchase {
  uint64_t transaction_id;    // Unique transaction identifier
  std::string sku;            // Purchased product SKU
  int64_t purchase_time;      // Unix timestamp of purchase
  bool durable;               // Persistent across sessions
  std::string receipt_token;  // For server-side verification
  bool consumed;              // Whether item has been used/consumed
};
#pragma warning(pop)

/**
 * @brief In-app purchase subsystem.
 *
 * Manages product catalog and purchase transactions.
 * Integrates with platform billing systems for actual payment processing.
 *
 * Binary reference: Implementation at 0x1801d0000-0x1801d1000
 */
class IAPSubsystem {
 public:
  IAPSubsystem();
  ~IAPSubsystem();

  /**
   * @brief Initialize IAP subsystem with product catalog.
   * @return true if initialization succeeded.
   *
   * Binary reference: Initialization at 0x1801d0000
   */
  bool Initialize();

  /**
   * @brief Shutdown IAP subsystem.
   *
   * Binary reference: Cleanup at 0x1801d0f00
   */
  void Shutdown();

  /**
   * @brief Get products by SKU list.
   * @param skus Vector of product SKUs to look up.
   * @return Vector of matching products.
   *
   * Binary reference: Lookup at 0x1801d0000 (ovr_IAP_GetProductsBySKU)
   * Returns product information for:
   * - Display in shop UI
   * - Price calculation
   * - Availability checking
   */
  std::vector<Product> GetProductsBySKU(const std::vector<std::string>& skus);

  /**
   * @brief Get products from catalog with pagination.
   * @param offset Starting index.
   * @param limit Number of products to return.
   * @return Product page with pagination info.
   *
   * Binary reference: Enumeration at 0x1801d0400 (ovr_IAP_GetNextProductArrayPage)
   * Supports browsing full catalog with:
   * - Configurable page size
   * - Continuation token (has_next_page)
   * - Total count
   */
  ProductPage GetProductCatalog(uint32_t offset, uint32_t limit);

  /**
   * @brief Launch checkout flow for purchase.
   * @param skus Products to purchase (may allow multiple).
   * @return Transaction ID, or 0 if checkout failed.
   *
   * Binary reference: Checkout at 0x1801d0100 (ovr_IAP_LaunchCheckoutFlow)
   * Process:
   * 1. Validate SKUs exist and are available
   * 2. Launch platform-specific payment UI (in real implementation)
   * 3. Wait for user to complete payment
   * 4. Validate receipt
   * 5. Record purchase in database
   * 6. Return transaction ID
   *
   * This is typically async - caller should set up callbacks
   * or poll for completion.
   */
  uint64_t LaunchCheckoutFlow(const std::vector<std::string>& skus);

  /**
   * @brief Get purchase history for current user.
   * @return Vector of purchase records.
   *
   * Binary reference: History at 0x1801d0200 (ovr_IAP_GetViewerPurchases)
   * Returns:
   * - All purchases by current user
   * - Including unconsumed items
   * - With receipt tokens for server verification
   */
  std::vector<Purchase> GetPurchaseHistory();

  /**
   * @brief Get durable purchases (cached for offline use).
   * @return Vector of durable purchase records.
   *
   * Binary reference: Cache at 0x1801d0300 (ovr_IAP_GetViewerPurchasesDurableCache)
   * Includes:
   * - Persistent items (season passes, permanent unlocks)
   * - Cached locally for instant access
   * - Verified server-side on next connection
   */
  std::vector<Purchase> GetDurablePurchases();

  /**
   * @brief Verify purchase receipt with server.
   * @param receipt_token Receipt to verify.
   * @return true if receipt is valid and purchase confirmed.
   *
   * Binary reference: Verification at 0x1801d0500
   * Performs:
   * 1. Signature validation (if applicable)
   * 2. Expiry checking (if subscription)
   * 3. Fraud detection
   * 4. Consumption tracking
   *
   * Should be called server-side for security.
   */
  bool VerifyPurchaseReceipt(const std::string& receipt_token);

  /**
   * @brief Mark purchase as consumed (used/claimed).
   * @param transaction_id Transaction to mark consumed.
   * @return true if consumption succeeded.
   *
   * Binary reference: Consumption at 0x1801d0600
   * Used for consumable items (currency, boosters, etc.)
   * Prevents reuse of item by other accounts.
   */
  bool ConsumePurchase(uint64_t transaction_id);

  /**
   * @brief Add product to catalog (typically called during initialization).
   * @param product Product to add.
   * @return true if addition succeeded.
   *
   * Binary reference: Catalog update at 0x1801d0000
   */
  bool AddProduct(const Product& product);

  /**
   * @brief Record a purchase (typically called after payment verified).
   * @param purchase Purchase to record.
   * @return true if recording succeeded.
   *
   * Binary reference: Purchase recording at 0x1801d0700
   */
  bool RecordPurchase(const Purchase& purchase);

 private:
  struct Impl;
  Impl* impl_;
};
