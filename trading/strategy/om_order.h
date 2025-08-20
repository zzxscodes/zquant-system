#pragma once

#include <array>
#include <sstream>
#include "common/types.h"

using namespace Common;

namespace Trading {
  // 订单管理器中订单结构的类型/操作状态表示
  enum class OMOrderState : int8_t {
    INVALID = 0,       // 无效状态
    PENDING_NEW = 1,   // 待新建状态（订单已发出但未确认）
    LIVE = 2,          // 活跃状态（订单已确认并在市场中）
    PENDING_CANCEL = 3,// 待取消状态（取消请求已发出但未确认）
    DEAD = 4           // 终止状态（订单已完成或被取消）
  };

  inline auto OMOrderStateToString(OMOrderState state) -> std::string {
    switch (state) {
      case OMOrderState::PENDING_NEW:
        return "PENDING_NEW";
      case OMOrderState::LIVE:
        return "LIVE";
      case OMOrderState::PENDING_CANCEL:
        return "PENDING_CANCEL";
      case OMOrderState::DEAD:
        return "DEAD";
      case OMOrderState::INVALID:
        return "INVALID";
    }

    return "UNKNOWN";
  }

  // 订单管理器用于表示单个策略订单的内部结构
  struct OMOrder {
    TickerId ticker_id_ = TickerId_INVALID;  // 股票代码
    OrderId order_id_ = OrderId_INVALID;     // 订单ID
    Side side_ = Side::INVALID;              // 买卖方向（买/卖）
    Price price_ = Price_INVALID;            // 订单价格
    Qty qty_ = Qty_INVALID;                  // 订单数量
    OMOrderState order_state_ = OMOrderState::INVALID;  // 订单状态

    auto toString() const {
      std::stringstream ss;
      ss << "OMOrder" << "["
         << "tid:" << tickerIdToString(ticker_id_) << " "
         << "oid:" << orderIdToString(order_id_) << " "
         << "side:" << sideToString(side_) << " "
         << "price:" << priceToString(price_) << " "
         << "qty:" << qtyToString(qty_) << " "
         << "state:" << OMOrderStateToString(order_state_) << "]";

      return ss.str();
    }
  };

  // 从买卖方向（Side）到OMOrder的哈希映射
  typedef std::array<OMOrder, sideToIndex(Side::MAX) + 1> OMOrderSideHashMap;

  // 从股票代码（TickerId）到买卖方向（Side）再到OMOrder的哈希映射
  typedef std::array<OMOrderSideHashMap, ME_MAX_TICKERS> OMOrderTickerSideHashMap;
}