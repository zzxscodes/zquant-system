#pragma once

#include "common/macros.h"
#include "common/logging.h"

#include "position_keeper.h"
#include "om_order.h"

using namespace Common;

namespace Trading {
  class OrderManager;

  // 用于表示风险检查结果的枚举 - ALLOWED表示通过所有风险检查，其他值表示失败原因
  enum class RiskCheckResult : int8_t {
    INVALID = 0,             // 无效状态
    ORDER_TOO_LARGE = 1,     // 订单规模过大
    POSITION_TOO_LARGE = 2,  // 持仓规模过大
    LOSS_TOO_LARGE = 3,      // 亏损过大
    ALLOWED = 4              // 允许交易（通过所有检查）
  };

  inline auto riskCheckResultToString(RiskCheckResult result) {
    switch (result) {
      case RiskCheckResult::INVALID:
        return "INVALID";
      case RiskCheckResult::ORDER_TOO_LARGE:
        return "ORDER_TOO_LARGE";
      case RiskCheckResult::POSITION_TOO_LARGE:
        return "POSITION_TOO_LARGE";
      case RiskCheckResult::LOSS_TOO_LARGE:
        return "LOSS_TOO_LARGE";
      case RiskCheckResult::ALLOWED:
        return "ALLOWED";
    }

    return "";
  }

  // 表示单个交易工具风险检查所需信息的结构
  struct RiskInfo {
    const PositionInfo *position_info_ = nullptr;  // 关联的持仓信息

    RiskCfg risk_cfg_;  // 风险配置参数

    // 检查风险以确定是否允许发送指定方向和数量的订单
    // 返回RiskCheckResult值以传达风险检查的结果
    auto checkPreTradeRisk(Side side, Qty qty) const noexcept {
      // 检查订单规模
      if (UNLIKELY(qty > risk_cfg_.max_order_size_))
        return RiskCheckResult::ORDER_TOO_LARGE;
      // 检查持仓规模（下单后）
      if (UNLIKELY(std::abs(position_info_->position_ + sideToValue(side) * static_cast<int32_t>(qty)) > static_cast<int32_t>(risk_cfg_.max_position_)))
        return RiskCheckResult::POSITION_TOO_LARGE;
      // 检查总亏损
      if (UNLIKELY(position_info_->total_pnl_ < risk_cfg_.max_loss_))
        return RiskCheckResult::LOSS_TOO_LARGE;

      return RiskCheckResult::ALLOWED;  // 所有风险检查通过
    }

    auto toString() const {
      std::stringstream ss;
      ss << "RiskInfo" << "["
         << "pos:" << position_info_->toString() << " "
         << risk_cfg_.toString()
         << "]";

      return ss.str();
    }
  };

  // 从股票代码（TickerId）到RiskInfo的哈希映射
  typedef std::array<RiskInfo, ME_MAX_TICKERS> TickerRiskInfoHashMap;

  // 风险管理器类，用于计算和检查所有交易工具的风险
  class RiskManager {
  public:
    RiskManager(Common::Logger *logger, const PositionKeeper *position_keeper, const TradeEngineCfgHashMap &ticker_cfg);

    // 检查交易前风险（指定股票、方向和数量）
    auto checkPreTradeRisk(TickerId ticker_id, Side side, Qty qty) const noexcept {
      return ticker_risk_.at(ticker_id).checkPreTradeRisk(side, qty);
    }

    RiskManager() = delete;
    RiskManager(const RiskManager &) = delete;
    RiskManager(const RiskManager &&) = delete;
    RiskManager &operator=(const RiskManager &) = delete;
    RiskManager &operator=(const RiskManager &&) = delete;

  private:
    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    // 从股票代码（TickerId）到RiskInfo的哈希映射容器
    TickerRiskInfoHashMap ticker_risk_;
  };
}