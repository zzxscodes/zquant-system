#pragma once

#include "common/macros.h"
#include "common/types.h"
#include "common/logging.h"

#include "exchange/order_server/client_response.h"

#include "market_order_book.h"

using namespace Common;

namespace Trading {
  // PositionInfo用于跟踪单个交易工具的持仓、盈亏（已实现和未实现）以及成交量
  struct PositionInfo {
    int32_t position_ = 0;  // 持仓量（正数表示多头，负数表示空头）
    double real_pnl_ = 0, unreal_pnl_ = 0, total_pnl_ = 0;  // 已实现盈亏、未实现盈亏、总盈亏
    std::array<double, sideToIndex(Side::MAX) + 1> open_vwap_;  // 开仓均价（按买卖方向区分）
    Qty volume_ = 0;  // 总成交量
    const BBO *bbo_ = nullptr;  // 当前最优买卖报价

    auto toString() const {
      std::stringstream ss;
      ss << "Position{"
         << "pos:" << position_
         << " u-pnl:" << unreal_pnl_
         << " r-pnl:" << real_pnl_
         << " t-pnl:" << total_pnl_
         << " vol:" << qtyToString(volume_)
         << " vwaps:[" << (position_ ? open_vwap_.at(sideToIndex(Side::BUY)) / std::abs(position_) : 0)
         << "X" << (position_ ? open_vwap_.at(sideToIndex(Side::SELL)) / std::abs(position_) : 0)
         << "] "
         << (bbo_ ? bbo_->toString() : "") << "}";

      return ss.str();
    }

    // 处理成交并更新持仓、盈亏和成交量
    auto addFill(const Exchange::MEClientResponse *client_response, Logger *logger) noexcept {
      const auto old_position = position_;  // 记录成交前的持仓
      const auto side_index = sideToIndex(client_response->side_);  // 成交方向索引
      const auto opp_side_index = sideToIndex(client_response->side_ == Side::BUY ? Side::SELL : Side::BUY);  // 相反方向索引
      const auto side_value = sideToValue(client_response->side_);  // 方向值（买为1，卖为-1）
      position_ += client_response->exec_qty_ * side_value;  // 更新持仓
      volume_ += client_response->exec_qty_;  // 更新成交量

      if (old_position * sideToValue(client_response->side_) >= 0) {  // 开仓或加仓
        open_vwap_[side_index] += (client_response->price_ * client_response->exec_qty_);  // 累加开仓金额
      } else {  // 减仓
        const auto opp_side_vwap = open_vwap_[opp_side_index] / std::abs(old_position);  // 反向持仓均价
        open_vwap_[opp_side_index] = opp_side_vwap * std::abs(position_);  // 更新剩余反向持仓金额
        // 计算已实现盈亏（平仓部分）
        real_pnl_ += std::min(static_cast<int32_t>(client_response->exec_qty_), std::abs(old_position)) *
                     (opp_side_vwap - client_response->price_) * sideToValue(client_response->side_);
        if (position_ * old_position < 0) {  // 持仓方向反转
          open_vwap_[side_index] = (client_response->price_ * std::abs(position_));  // 新方向持仓金额
          open_vwap_[opp_side_index] = 0;  // 清空原方向持仓金额
        }
      }

      if (!position_) {  // 平仓（持仓为0）
        open_vwap_[sideToIndex(Side::BUY)] = open_vwap_[sideToIndex(Side::SELL)] = 0;  // 清空均价
        unreal_pnl_ = 0;  // 未实现盈亏为0
      } else {
        // 根据当前持仓方向计算未实现盈亏
        if (position_ > 0)
          unreal_pnl_ =
              (client_response->price_ - open_vwap_[sideToIndex(Side::BUY)] / std::abs(position_)) *
              std::abs(position_);
        else
          unreal_pnl_ =
              (open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_) - client_response->price_) *
              std::abs(position_);
      }

      total_pnl_ = unreal_pnl_ + real_pnl_;  // 更新总盈亏

      std::string time_str;
      logger->log("%:% %() % % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
                  toString(), client_response->toString().c_str());
    }

    // 处理盘口价格（BBO）变化，若有持仓则更新未实现盈亏
    auto updateBBO(const BBO *bbo, Logger *logger) noexcept {
      std::string time_str;
      bbo_ = bbo;  // 更新当前最优买卖报价

      if (position_ && bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID) {
        const auto mid_price = (bbo->bid_price_ + bbo->ask_price_) * 0.5;  // 计算中间价
        // 根据持仓方向计算未实现盈亏（基于中间价）
        if (position_ > 0)
          unreal_pnl_ =
              (mid_price - open_vwap_[sideToIndex(Side::BUY)] / std::abs(position_)) *
              std::abs(position_);
        else
          unreal_pnl_ =
              (open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_) - mid_price) *
              std::abs(position_);

        const auto old_total_pnl = total_pnl_;
        total_pnl_ = unreal_pnl_ + real_pnl_;  // 更新总盈亏

        // 若总盈亏变化，记录日志
        if (total_pnl_ != old_total_pnl)
          logger->log("%:% %() % % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
                      toString(), bbo_->toString());
      }
    }
  };

  // 顶级持仓管理类，用于计算所有交易工具的持仓、盈亏和成交量
  class PositionKeeper {
  public:
    PositionKeeper(Common::Logger *logger)
        : logger_(logger) {
    }

    PositionKeeper() = delete;
    PositionKeeper(const PositionKeeper &) = delete;
    PositionKeeper(const PositionKeeper &&) = delete;
    PositionKeeper &operator=(const PositionKeeper &) = delete;
    PositionKeeper &operator=(const PositionKeeper &&) = delete;

  private:
    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    // 从股票代码（TickerId）到PositionInfo的哈希映射容器
    std::array<PositionInfo, ME_MAX_TICKERS> ticker_position_;

  public:
    // 处理成交并更新对应股票的持仓信息
    auto addFill(const Exchange::MEClientResponse *client_response) noexcept {
      ticker_position_.at(client_response->ticker_id_).addFill(client_response, logger_);
    }

    // 更新指定股票的最优买卖报价（BBO）并调整未实现盈亏
    auto updateBBO(TickerId ticker_id, const BBO *bbo) noexcept {
      ticker_position_.at(ticker_id).updateBBO(bbo, logger_);
    }

    // 获取指定股票的持仓信息
    auto getPositionInfo(TickerId ticker_id) const noexcept {
      return &(ticker_position_.at(ticker_id));
    }

    auto toString() const {
      double total_pnl = 0;  // 总盈亏
      Qty total_vol = 0;     // 总成交量

      std::stringstream ss;
      for(TickerId i = 0; i < ticker_position_.size(); ++i) {
        ss << "TickerId:" << tickerIdToString(i) << " " << ticker_position_.at(i).toString() << "\n";

        total_pnl += ticker_position_.at(i).total_pnl_;
        total_vol += ticker_position_.at(i).volume_;
      }
      ss << "Total PnL:" << total_pnl << " Vol:" << total_vol << "\n";

      return ss.str();
    }
  };
}