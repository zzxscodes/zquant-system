#pragma once

#include "common/macros.h"
#include "common/logging.h"

#include "exchange/order_server/client_response.h"

#include "om_order.h"
#include "risk_manager.h"

using namespace Common;

namespace Trading {
  class TradeEngine;

  // 为交易算法管理订单，隐藏订单管理的复杂性以简化交易策略
  class OrderManager {
  public:
    OrderManager(Common::Logger *logger, TradeEngine *trade_engine, RiskManager& risk_manager)
        : trade_engine_(trade_engine), risk_manager_(risk_manager), logger_(logger) {
    }

    // 处理来自客户端响应的订单更新，并更新所管理订单的状态
    auto onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
      logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   client_response->toString().c_str());
      // 获取对应的订单对象
      auto order = &(ticker_side_order_.at(client_response->ticker_id_).at(sideToIndex(client_response->side_)));
      logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   order->toString().c_str());

      // 根据响应类型更新订单状态
      switch (client_response->type_) {
        case Exchange::ClientResponseType::ACCEPTED: {
          order->order_state_ = OMOrderState::LIVE;  // 订单已被接受，状态变为活跃
        }
          break;
        case Exchange::ClientResponseType::CANCELED: {
          order->order_state_ = OMOrderState::DEAD;  // 订单已被取消，状态变为终止
        }
          break;
        case Exchange::ClientResponseType::FILLED: {
          order->qty_ = client_response->leaves_qty_;  // 更新剩余数量
          if(!order->qty_)  // 若剩余数量为0，订单状态变为终止
            order->order_state_ = OMOrderState::DEAD;
        }
          break;
        case Exchange::ClientResponseType::CANCEL_REJECTED:
        case Exchange::ClientResponseType::INVALID: {
          // 取消被拒绝或无效响应，不更新状态
        }
          break;
      }
    }

    // 发送具有指定属性的新订单，并更新传入的OMOrder对象
    auto newOrder(OMOrder *order, TickerId ticker_id, Price price, Side side, Qty qty) noexcept -> void;

    // 发送指定订单的取消请求，并更新传入的OMOrder对象
    auto cancelOrder(OMOrder *order) noexcept -> void;

    // 调整指定方向的单个订单，使其具有指定的价格和数量
    // 发送订单前会执行风险检查，并更新传入的OMOrder对象
    auto moveOrder(OMOrder *order, TickerId ticker_id, Price price, Side side, Qty qty) noexcept {
      switch (order->order_state_) {
        case OMOrderState::LIVE: {
          // 若订单处于活跃状态且价格发生变化，则取消当前订单
          if(order->price_ != price) {
            START_MEASURE(Trading_OrderManager_cancelOrder);
            cancelOrder(order);
            END_MEASURE(Trading_OrderManager_cancelOrder, (*logger_));
          }
        }
          break;
        case OMOrderState::INVALID:
        case OMOrderState::DEAD: {
          // 若订单无效或已终止，且价格有效，则尝试新建订单
          if(LIKELY(price != Price_INVALID)) {
            START_MEASURE(Trading_RiskManager_checkPreTradeRisk);
            const auto risk_result = risk_manager_.checkPreTradeRisk(ticker_id, side, qty);
            END_MEASURE(Trading_RiskManager_checkPreTradeRisk, (*logger_));
            if(LIKELY(risk_result == RiskCheckResult::ALLOWED)) {
              // 风险检查通过，发送新订单
              START_MEASURE(Trading_OrderManager_newOrder);
              newOrder(order, ticker_id, price, side, qty);
              END_MEASURE(Trading_OrderManager_newOrder, (*logger_));
            } else
              // 风险检查未通过，记录日志
              logger_->log("%:% %() % Ticker:% Side:% Qty:% RiskCheckResult:%\n", __FILE__, __LINE__, __FUNCTION__,
                           Common::getCurrentTimeStr(&time_str_),
                           tickerIdToString(ticker_id), sideToString(side), qtyToString(qty),
                           riskCheckResultToString(risk_result));
          }
        }
          break;
        case OMOrderState::PENDING_NEW:
        case OMOrderState::PENDING_CANCEL:
          // 订单处于待新建或待取消状态，不执行操作
          break;
      }
    }

    // 按指定的买卖价格放置指定数量(clip)的订单
    // 若当前无订单，可能会发送新订单
    // 若现有订单价格或数量不符，可能会取消现有订单
    // 若买卖价格指定为Price_INVALID，表示该方向不需要订单
    auto moveOrders(TickerId ticker_id, Price bid_price, Price ask_price, Qty clip) noexcept {
      {
        // 处理买单
        auto bid_order = &(ticker_side_order_.at(ticker_id).at(sideToIndex(Side::BUY)));
        START_MEASURE(Trading_OrderManager_moveOrder);
        moveOrder(bid_order, ticker_id, bid_price, Side::BUY, clip);
        END_MEASURE(Trading_OrderManager_moveOrder, (*logger_));
      }

      {
        // 处理卖单
        auto ask_order = &(ticker_side_order_.at(ticker_id).at(sideToIndex(Side::SELL)));
        START_MEASURE(Trading_OrderManager_moveOrder);
        moveOrder(ask_order, ticker_id, ask_price, Side::SELL, clip);
        END_MEASURE(Trading_OrderManager_moveOrder, (*logger_));
      }
    }

    // 辅助方法：获取指定股票代码的买卖方向OMOrder哈希映射
    auto getOMOrderSideHashMap(TickerId ticker_id) const {
      return &(ticker_side_order_.at(ticker_id));
    }

    OrderManager() = delete;
    OrderManager(const OrderManager &) = delete;
    OrderManager(const OrderManager &&) = delete;
    OrderManager &operator=(const OrderManager &) = delete;
    OrderManager &operator=(const OrderManager &&) = delete;

  private:
    // 父交易引擎对象，用于发送客户端请求
    TradeEngine *trade_engine_ = nullptr;

    // 风险管理器，用于执行交易前的风险检查
    const RiskManager& risk_manager_;

    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    // 从股票代码(TickerId)到买卖方向(Side)再到OMOrder的哈希映射容器
    OMOrderTickerSideHashMap ticker_side_order_;

    // 用于为发出的新订单请求设置订单ID
    OrderId next_order_id_ = 1;
  };
}