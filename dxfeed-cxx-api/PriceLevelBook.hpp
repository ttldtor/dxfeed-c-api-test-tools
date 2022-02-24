#pragma once

#include <DXFeed.h>
#include <fmt/format.h>

#include <algorithm>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index_container.hpp>
#include <cassert>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "StringConverter.hpp"

namespace dxf {
static inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

struct OrderData {
  dxf_long_t index = 0;
  double price = NaN;
  double size = NaN;
  dxf_long_t time = 0;
  dxf_order_side_t side = dxf_osd_undefined;
};

struct PriceLevel {
  double price = NaN;
  double size = NaN;
  std::int64_t time = 0;

  [[nodiscard]] bool isValid() const { return std::isnan(price); }

  friend bool operator<(const PriceLevel& a, const PriceLevel& b) {
    if (std::isnan(b.price)) return true;
    if (std::isnan(a.price)) return false;

    return a.price < b.price;
  }
};

struct AskPriceLevel : PriceLevel {
  friend bool operator<(const AskPriceLevel& a, const AskPriceLevel& b) {
    if (std::isnan(b.price)) return true;
    if (std::isnan(a.price)) return false;

    return a.price < b.price;
  }
};

struct BidPriceLevel : PriceLevel {
  friend bool operator<(const BidPriceLevel& a, const BidPriceLevel& b) {
    if (std::isnan(b.price)) return false;
    if (std::isnan(a.price)) return true;

    return a.price > b.price;
  }
};

struct PriceLevelChanges {
  std::vector<AskPriceLevel> asks{};
  std::vector<BidPriceLevel> bids{};
};

struct PriceLevelChangesSet {
  PriceLevelChanges additions{};
  PriceLevelChanges updates{};
  PriceLevelChanges removals{};
};

namespace bmi = boost::multi_index;

using PriceLevelContainer = std::set<PriceLevel>;

class PriceLevelBook final {
  dxf_snapshot_t snapshot_;
  std::string symbol_;
  std::string source_;
  std::size_t levelsNumber_;

  /*
   * Since we are working with std::set, which does not imply random access, we have to keep the iterators lastAsk,
   * lastBid for the case with a limit on the number of PLs. These iterators, as well as the look-ahead functions, are
   * used to find out if we are performing operations on PL within the range given to the user.
   */
  std::set<AskPriceLevel> asks_;
  std::set<AskPriceLevel>::iterator lastAsk_;
  std::set<BidPriceLevel> bids_;
  std::set<BidPriceLevel>::iterator lastBid_;
  std::unordered_map<dxf_long_t, OrderData> orderDataSnapshot_;
  bool isValid_;
  std::mutex mutex_;

  std::function<void(const PriceLevelChanges&)> onNewBook_;
  std::function<void(const PriceLevelChanges&)> onBookUpdate_;
  std::function<void(const PriceLevelChangesSet&)> onIncrementalChange_;

  static bool isZeroPriceLevel(const PriceLevel& pl) {
    return std::abs(pl.size) < std::numeric_limits<double>::epsilon();
  };

  static bool areEqualPrices(double price1, double price2) {
    return std::abs(price1 - price2) < std::numeric_limits<double>::epsilon();
  }

  PriceLevelBook(std::string symbol, std::string source, std::size_t levelsNumber = 0)
      : snapshot_{nullptr},
        symbol_{std::move(symbol)},
        source_{std::move(source)},
        levelsNumber_{levelsNumber},
        asks_{},
        lastAsk_{asks_.end()},
        bids_{},
        lastBid_{bids_.end()},
        orderDataSnapshot_{},
        isValid_{false},
        mutex_{} {}

  // Process the tx\snapshot data, converts it to PL changes. Also, changes the orderDataSnapshot_
  PriceLevelChanges convertToUpdates(const dxf_snapshot_data_ptr_t snapshotData) {
    assert(snapshotData->records_count != 0);
    assert(snapshotData->event_type != dx_eid_order);

    std::set<PriceLevel> askUpdates{};
    std::set<PriceLevel> bidUpdates{};

    auto orders = reinterpret_cast<const dxf_order_t*>(snapshotData->records);

    auto isOrderRemoval = [](const dxf_order_t& o) {
      return (o.event_flags & dxf_ef_remove_event) != 0 || o.size == 0 || std::isnan(o.size);
    };

    auto processOrderAddition = [&bidUpdates, &askUpdates](const dxf_order_t& order) {
      auto& updatesSide = order.side == dxf_osd_buy ? bidUpdates : askUpdates;
      auto priceLevelChange = PriceLevel{order.price, order.size, order.time};
      auto foundPriceLevel = updatesSide.find(priceLevelChange);

      if (foundPriceLevel != updatesSide.end()) {
        priceLevelChange.size = foundPriceLevel->size + priceLevelChange.size;
        updatesSide.erase(foundPriceLevel);
      }

      updatesSide.insert(priceLevelChange);
    };

    auto processOrderRemoval = [&bidUpdates, &askUpdates](const dxf_order_t& order, const OrderData& foundOrderData) {
      auto& updatesSide = foundOrderData.side == dxf_osd_buy ? bidUpdates : askUpdates;
      auto priceLevelChange = PriceLevel{foundOrderData.price, -foundOrderData.size, order.time};
      auto foundPriceLevel = updatesSide.find(priceLevelChange);

      if (foundPriceLevel != updatesSide.end()) {
        priceLevelChange.size = foundPriceLevel->size + priceLevelChange.size;
        updatesSide.erase(foundPriceLevel);
      }

      if (!isZeroPriceLevel(priceLevelChange)) {
        updatesSide.insert(priceLevelChange);
      }
    };

    for (std::size_t i = 0; i < snapshotData->records_count; i++) {
      auto order = orders[i];

      fmt::print("O:ind={},pr={},sz={},sd={}\n", order.index, order.price, order.size,
                 order.side == dxf_osd_buy ? "buy" : "sell");

      auto removal = isOrderRemoval(order);
      auto foundOrderDataIt = orderDataSnapshot_.find(order.index);

      if (foundOrderDataIt == orderDataSnapshot_.end()) {
        if (removal) {
          continue;
        }

        processOrderAddition(order);
        orderDataSnapshot_[order.index] = OrderData{order.index, order.price, order.size, order.time, order.side};
      } else {
        const auto& foundOrderData = foundOrderDataIt->second;

        if (removal) {
          processOrderRemoval(order, foundOrderData);
          orderDataSnapshot_.erase(foundOrderData.index);
        } else {
          if (order.side != foundOrderData.side) {
            processOrderRemoval(order, foundOrderData);
          }

          processOrderAddition(order);
          orderDataSnapshot_[foundOrderData.index] =
            OrderData{order.index, order.price, order.size, order.time, order.side};
        }
      }
    }

    return {std::vector<AskPriceLevel>{askUpdates.begin(), askUpdates.end()},
            std::vector<BidPriceLevel>{bidUpdates.rbegin(), bidUpdates.rend()}};
  }

  // TODO: C++14 lambda
  template <typename PriceLevelUpdateSide, typename PriceLevelStorageSide, typename PriceLevelChangesSide>
  static void generatePriceLevelChanges(const PriceLevelUpdateSide& priceLevelUpdateSide,
                                const PriceLevelStorageSide& priceLevelStorageSide, PriceLevelChangesSide& additions,
                                PriceLevelChangesSide& removals, PriceLevelChangesSide& updates) {
    auto found = priceLevelStorageSide.find(priceLevelUpdateSide);

    if (found == priceLevelStorageSide.end()) {
      additions.push_back(priceLevelUpdateSide);
    } else {
      auto newPriceLevelChange = *found;

      newPriceLevelChange.size += priceLevelUpdateSide.size;
      newPriceLevelChange.time = priceLevelUpdateSide.time;

      if (isZeroPriceLevel(newPriceLevelChange)) {
        removals.push_back(*found);
      } else {
        updates.push_back(newPriceLevelChange);
      }
    }
  }

  template <typename PriceLevelRemovalSide, typename PriceLevelStorageSide, typename PriceLevelChangesSideSet,
            typename LastElementIter>
  static void processSideRemoval(const PriceLevelRemovalSide& priceLevelRemovalSide,
                                 PriceLevelStorageSide& priceLevelStorageSide, PriceLevelChangesSideSet& removals,
                                 PriceLevelChangesSideSet& additions, LastElementIter& lastElementIter,
                                 std::size_t levelsNumber) {
    if (priceLevelStorageSide.empty()) return;

    auto found = priceLevelStorageSide.find(priceLevelRemovalSide);

    if (levelsNumber == 0) {
      removals.insert(priceLevelRemovalSide);
      priceLevelStorageSide.erase(found);
      lastElementIter = priceLevelStorageSide.end();

      return;
    }

    auto removed = false;

    // Determine what will be the removal given the number of price levels.
    if (priceLevelStorageSide.size() <= levelsNumber || priceLevelRemovalSide < *std::next(lastElementIter)) {
      removals.insert(priceLevelRemovalSide);
      removed = true;
    }

    // Determine what will be the shift in price levels after removal.
    if (removed && priceLevelStorageSide.size() > levelsNumber) {
      additions.insert(*std::next(lastElementIter));
    }

    // Determine the adjusted last price (NaN -- end)
    typename std::decay<decltype(*lastElementIter)>::type newLastPL{};

    if (removed) {                                                      // removal <= last
      if (std::next(lastElementIter) != priceLevelStorageSide.end()) {  // there is another PL after last
        newLastPL = *std::next(lastElementIter);
      } else {
        if (priceLevelRemovalSide < *lastElementIter) {
          newLastPL = *lastElementIter;
        } else if (lastElementIter != priceLevelStorageSide.begin()) {
          newLastPL = *std::prev(lastElementIter);
        }
      }
    } else {
      newLastPL = *lastElementIter;
    }

    priceLevelStorageSide.erase(found);

    if (!newLastPL.isValid()) {
      lastElementIter = priceLevelStorageSide.end();
    } else {
      lastElementIter = priceLevelStorageSide.find(newLastPL);
    }
  }

  template <typename PriceLevelAdditionSide, typename PriceLevelStorageSide, typename PriceLevelChangesSideSet,
            typename LastElementIter>
  static void processSideAddition(const PriceLevelAdditionSide& priceLevelAdditionSide,
                                  PriceLevelStorageSide& priceLevelStorageSide, PriceLevelChangesSideSet& additions,
                                  PriceLevelChangesSideSet& removals, LastElementIter& lastElementIter,
                                  std::size_t levelsNumber) {
    if (levelsNumber == 0) {
      additions.insert(priceLevelAdditionSide);
      priceLevelStorageSide.insert(priceLevelAdditionSide);
      lastElementIter = priceLevelStorageSide.end();

      return;
    }

    auto added = false;

    // We determine what will be the addition of the price level, taking into account the possible quantity.
    if (priceLevelStorageSide.size() < levelsNumber || priceLevelAdditionSide < *lastElementIter) {
      additions.insert(priceLevelAdditionSide);
      added = true;
    }

    // We determine what will be the shift after adding
    if (added && priceLevelStorageSide.size() >= levelsNumber) {
      const auto& toRemove = *lastElementIter;

      // We take into account the possibility that the previously added price level will be deleted.
      if (additions.count(toRemove) > 0) {
        additions.erase(toRemove);
      } else {
        removals.insert(toRemove);
      }
    }

    // Determine the adjusted last price (NaN -- end)
    typename std::decay<decltype(*lastElementIter)>::type newLastPL = *lastElementIter;

    if (added) {
      newLastPL = priceLevelAdditionSide;

      if (lastElementIter != priceLevelStorageSide.end() && priceLevelAdditionSide < *lastElementIter) {
        if (priceLevelStorageSide.size() < levelsNumber) {
          newLastPL = *lastElementIter;
        } else if (lastElementIter != priceLevelStorageSide.begin() &&
                   priceLevelAdditionSide < *std::prev(lastElementIter)) {
          newLastPL = *std::prev(lastElementIter);
        }
      }
    }

    priceLevelStorageSide.insert(priceLevelAdditionSide);

    if (!newLastPL.isValid()) {
      lastElementIter = priceLevelStorageSide.end();
    } else {
      lastElementIter = priceLevelStorageSide.find(newLastPL);
    }
  }

  template <typename PriceLevelUpdateSide, typename PriceLevelStorageSide, typename PriceLevelChangesSideSet,
            typename LastElementIter>
  static void processSideUpdate(const PriceLevelUpdateSide& priceLevelUpdateSide,
                                  PriceLevelStorageSide& priceLevelStorageSide, PriceLevelChangesSideSet& updates,
                                  LastElementIter& lastElementIter, std::size_t levelsNumber) {
    if (levelsNumber == 0) {
      priceLevelStorageSide.erase(priceLevelUpdateSide);
      priceLevelStorageSide.insert(priceLevelUpdateSide);
      updates.insert(priceLevelUpdateSide);
      lastElementIter = priceLevelStorageSide.end();

      return;
    }

    if (priceLevelStorageSide.count(priceLevelUpdateSide) > 0) {
      updates.insert(priceLevelUpdateSide);
    }

    typename std::decay<decltype(*lastElementIter)>::type newLastPL{};

    if (lastElementIter != priceLevelStorageSide.end()) {
      newLastPL = *lastElementIter;
    }

    priceLevelStorageSide.erase(priceLevelUpdateSide);
    priceLevelStorageSide.insert(priceLevelUpdateSide);

    if (!newLastPL.isValid()) {
      lastElementIter = priceLevelStorageSide.end();
    } else {
      lastElementIter = priceLevelStorageSide.find(newLastPL);
    }
  }

  PriceLevelChangesSet applyUpdates(const PriceLevelChanges& priceLevelUpdates) {
    PriceLevelChanges additions{};
    PriceLevelChanges removals{};
    PriceLevelChanges updates{};

    // We generate lists of additions, updates, removals
    for (const auto& askUpdate : priceLevelUpdates.asks) {
      generatePriceLevelChanges(askUpdate, asks_, additions.asks, removals.asks, updates.asks);
    }

    for (const auto& bidUpdate : priceLevelUpdates.bids) {
      generatePriceLevelChanges(bidUpdate, bids_, additions.bids, removals.bids, updates.bids);
    }

    std::set<AskPriceLevel> askRemovals{};
    std::set<BidPriceLevel> bidRemovals{};
    std::set<AskPriceLevel> askAdditions{};
    std::set<BidPriceLevel> bidAdditions{};
    std::set<AskPriceLevel> askUpdates{};
    std::set<BidPriceLevel> bidUpdates{};

    // O(R)
    for (const auto& askRemoval : removals.asks) {
      processSideRemoval(askRemoval, asks_, askRemovals, askAdditions, lastAsk_, levelsNumber_);
    }

    for (const auto& askAddition : additions.asks) {
      processSideAddition(askAddition, asks_, askAdditions, askRemovals, lastAsk_, levelsNumber_);
    }

    for (const auto& askUpdate : updates.asks) {
      processSideUpdate(askUpdate, asks_, askUpdates, lastAsk_, levelsNumber_);
    }

    for (const auto& bidRemoval : removals.bids) {
      processSideRemoval(bidRemoval, bids_, bidRemovals, bidAdditions, lastBid_, levelsNumber_);
    }

    for (const auto& bidAddition : additions.bids) {
      processSideAddition(bidAddition, bids_, bidAdditions, bidRemovals, lastBid_, levelsNumber_);
    }

    for (const auto& bidUpdate : updates.bids) {
      processSideUpdate(bidUpdate, bids_, bidUpdates, lastBid_, levelsNumber_);
    }

    return {PriceLevelChanges{std::vector<AskPriceLevel>{askAdditions.begin(), askAdditions.end()},
                              std::vector<BidPriceLevel>{bidAdditions.begin(), bidAdditions.end()}},
            PriceLevelChanges{std::vector<AskPriceLevel>{askUpdates.begin(), askUpdates.end()},
                              std::vector<BidPriceLevel>{bidUpdates.begin(), bidUpdates.end()}},
            PriceLevelChanges{std::vector<AskPriceLevel>{askRemovals.begin(), askRemovals.end()},
                              std::vector<BidPriceLevel>{bidRemovals.begin(), bidRemovals.end()}}};
  }

  [[nodiscard]] std::vector<AskPriceLevel> getAsks() const { return {asks_.begin(), lastAsk_}; }

  [[nodiscard]] std::vector<BidPriceLevel> getBids() const { return {bids_.begin(), lastBid_}; }

 public:
  // TODO: move to another thread
  void processSnapshotData(const dxf_snapshot_data_ptr_t snapshotData, int newSnapshot) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto newSnap = newSnapshot != 0;

    if (newSnap) {
      asks_.clear();
      bids_.clear();
      orderDataSnapshot_.clear();
    }

    if (snapshotData->records_count == 0) {
      if (newSnap && onNewBook_) {
        onNewBook_({});
      }

      return;
    }

    auto updates = convertToUpdates(snapshotData);
    auto resultingChangesSet = applyUpdates(updates);

    if (newSnap) {
      if (onNewBook_) {
        onNewBook_(PriceLevelChanges{getAsks(), getBids()});
      }
    } else {
      if (onIncrementalChange_) {
        onIncrementalChange_(resultingChangesSet);
      }

      if (onBookUpdate_) {
        onBookUpdate_(PriceLevelChanges{getAsks(), getBids()});
      }
    }
  }

  ~PriceLevelBook() {
    if (isValid_) {
      dxf_close_price_level_book(snapshot_);
    }
  }

  static std::unique_ptr<PriceLevelBook> create(dxf_connection_t connection, const std::string& symbol,
                                                const std::string& source, std::size_t levelsNumber) {
    auto plb = std::unique_ptr<PriceLevelBook>(new PriceLevelBook(symbol, source, levelsNumber));
    auto wSymbol = StringConverter::utf8ToWString(symbol);
    dxf_snapshot_t snapshot = nullptr;

    dxf_create_order_snapshot(connection, wSymbol.c_str(), source.c_str(), 0, &snapshot);

    dxf_attach_snapshot_inc_listener(
      snapshot,
      [](const dxf_snapshot_data_ptr_t snapshot_data, int new_snapshot, void* user_data) {
        static_cast<PriceLevelBook*>(user_data)->processSnapshotData(snapshot_data, new_snapshot);
      },
      plb.get());
    plb->isValid_ = true;

    return plb;
  }

  void setOnNewBook(std::function<void(const PriceLevelChanges&)> onNewBookHandler) {
    onNewBook_ = std::move(onNewBookHandler);
  }

  void setOnBookUpdate(std::function<void(const PriceLevelChanges&)> onBookUpdateHandler) {
    onBookUpdate_ = std::move(onBookUpdateHandler);
  }

  void setOnIncrementalChange(std::function<void(const PriceLevelChangesSet&)> onIncrementalChangeHandler) {
    onIncrementalChange_ = std::move(onIncrementalChangeHandler);
  }
};

}  // namespace dxf