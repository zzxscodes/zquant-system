#pragma once

#include <sstream>

#include "common/types.h"
#include "common/lf_queue.h"

using namespace Common;

namespace Exchange {
  // 表示市场更新消息中的类型/操作
  enum class MarketUpdateType : uint8_t {
    INVALID = 0,       // 无效
    CLEAR = 1,         // 清除订单簿
    ADD = 2,           // 添加订单
    MODIFY = 3,        // 修改订单
    CANCEL = 4,        // 取消订单
    TRADE = 5,         // 成交
    SNAPSHOT_START = 6,// 快照开始
    SNAPSHOT_END = 7   // 快照结束
  };

  // 将MarketUpdateType转换为字符串
  inline std::string marketUpdateTypeToString(MarketUpdateType type) {
    switch (type) {
      case MarketUpdateType::CLEAR:
        return "CLEAR";
      case MarketUpdateType::ADD:
        return "ADD";
      case MarketUpdateType::MODIFY:
        return "MODIFY";
      case MarketUpdateType::CANCEL:
        return "CANCEL";
      case MarketUpdateType::TRADE:
        return "TRADE";
      case MarketUpdateType::SNAPSHOT_START:
        return "SNAPSHOT_START";
      case MarketUpdateType::SNAPSHOT_END:
        return "SNAPSHOT_END";
      case MarketUpdateType::INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
  }

  // 这些结构用于网络传输，因此对二进制结构进行紧凑打包以消除系统相关的额外填充
#pragma pack(push, 1)

  // 匹配引擎内部使用的市场更新结构
  struct MEMarketUpdate {
    MarketUpdateType type_ = MarketUpdateType::INVALID;  // 更新类型

    OrderId order_id_ = OrderId_INVALID;    // 订单ID
    TickerId ticker_id_ = TickerId_INVALID;  // 股票代码
    Side side_ = Side::INVALID;              // 买卖方向
    Price price_ = Price_INVALID;            // 价格
    Qty qty_ = Qty_INVALID;                  // 数量
    Priority priority_ = Priority_INVALID;   // 优先级

    // 将市场更新信息转换为字符串
    auto toString() const {
      std::stringstream ss;
      ss << "MEMarketUpdate"
         << " ["
         << " type:" << marketUpdateTypeToString(type_)
         << " ticker:" << tickerIdToString(ticker_id_)
         << " oid:" << orderIdToString(order_id_)
         << " side:" << sideToString(side_)
         << " qty:" << qtyToString(qty_)
         << " price:" << priceToString(price_)
         << " priority:" << priorityToString(priority_)
         << "]";
      return ss.str();
    }
  };

  // 市场数据发布器通过网络发布的市场更新结构
  struct MDPMarketUpdate {
    size_t seq_num_ = 0;                   // 序列号
    MEMarketUpdate me_market_update_;      // 匹配引擎市场更新

    // 将发布的市场更新信息转换为字符串
    auto toString() const {
      std::stringstream ss;
      ss << "MDPMarketUpdate"
         << " ["
         << " seq:" << seq_num_
         << " " << me_market_update_.toString()
         << "]";
      return ss.str();
    }
  };

#pragma pack(pop) // 取消后续结构的紧凑打包指令

  // 分别为匹配引擎市场更新消息和市场数据发布器市场更新消息的无锁队列
  typedef Common::LFQueue<Exchange::MEMarketUpdate> MEMarketUpdateLFQueue;
  typedef Common::LFQueue<Exchange::MDPMarketUpdate> MDPMarketUpdateLFQueue;
}