#pragma once

#include <array>
#include <sstream>
#include "common/types.h"

using namespace Common;

namespace Trading {
  // 交易引擎用于表示限价订单簿中单个订单的结构
  struct MarketOrder {
    OrderId order_id_ = OrderId_INVALID;  // 订单ID
    Side side_ = Side::INVALID;          // 买卖方向（买/卖）
    Price price_ = Price_INVALID;        // 订单价格
    Qty qty_ = Qty_INVALID;              // 订单数量
    Priority priority_ = Priority_INVALID;  // 订单优先级

    // MarketOrder同时作为同一价格层级下所有订单组成的双向链表的节点（按FIFO顺序排列）
    MarketOrder *prev_order_ = nullptr;  // 前一个订单指针
    MarketOrder *next_order_ = nullptr;  // 后一个订单指针

    // 仅用于内存池（MemPool）
    MarketOrder() = default;

    MarketOrder(OrderId order_id, Side side, Price price, Qty qty, Priority priority, MarketOrder *prev_order, MarketOrder *next_order) noexcept
        : order_id_(order_id), side_(side), price_(price), qty_(qty), priority_(priority), prev_order_(prev_order), next_order_(next_order) {}

    auto toString() const -> std::string;
  };

  // 从订单ID到MarketOrder的哈希映射
  typedef std::array<MarketOrder *, ME_MAX_ORDER_IDS> OrderHashMap;

  // 交易引擎用于表示限价订单簿中一个价格层级的结构
  // 内部维护按FIFO顺序排列的MarketOrder对象列表
  struct MarketOrdersAtPrice {
    Side side_ = Side::INVALID;          // 买卖方向（买/卖）
    Price price_ = Price_INVALID;        // 该层级的价格

    MarketOrder *first_mkt_order_ = nullptr;  // 该价格层级的首个订单（FIFO队列的头部）

    // MarketOrdersAtPrice同时作为价格层级双向链表的节点（按价格从最优到最差排序）
    MarketOrdersAtPrice *prev_entry_ = nullptr;  // 前一个价格层级指针
    MarketOrdersAtPrice *next_entry_ = nullptr;  // 后一个价格层级指针

    // 仅用于内存池（MemPool）
    MarketOrdersAtPrice() = default;

    MarketOrdersAtPrice(Side side, Price price, MarketOrder *first_mkt_order, MarketOrdersAtPrice *prev_entry, MarketOrdersAtPrice *next_entry)
        : side_(side), price_(price), first_mkt_order_(first_mkt_order), prev_entry_(prev_entry), next_entry_(next_entry) {}

    auto toString() const {
      std::stringstream ss;
      ss << "MarketOrdersAtPrice["
         << "side:" << sideToString(side_) << " "
         << "price:" << priceToString(price_) << " "
         << "first_mkt_order:" << (first_mkt_order_ ? first_mkt_order_->toString() : "null") << " "
         << "prev:" << priceToString(prev_entry_ ? prev_entry_->price_ : Price_INVALID) << " "
         << "next:" << priceToString(next_entry_ ? next_entry_->price_ : Price_INVALID) << "]";

      return ss.str();
    }
  };

  // 从价格到MarketOrdersAtPrice的哈希映射
  typedef std::array<MarketOrdersAtPrice *, ME_MAX_PRICE_LEVELS> OrdersAtPriceHashMap;

  // 最优买卖报价（BBO）抽象结构，供仅需盘口价格和流动性摘要而非完整订单簿的组件使用
  struct BBO {
    Price bid_price_ = Price_INVALID, ask_price_ = Price_INVALID;  // 买一价、卖一价
    Qty bid_qty_ = Qty_INVALID, ask_qty_ = Qty_INVALID;            // 买一量、卖一量

    auto toString() const {
      std::stringstream ss;
      ss << "BBO{"
         << qtyToString(bid_qty_) << "@" << priceToString(bid_price_)
         << "X"
         << priceToString(ask_price_) << "@" << qtyToString(ask_qty_)
         << "}";

      return ss.str();
    };
  };
}