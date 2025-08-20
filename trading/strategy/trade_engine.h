#pragma once

#include <functional>

#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"
#include "common/logging.h"

#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "exchange/market_data/market_update.h"

#include "market_order_book.h"

#include "feature_engine.h"
#include "position_keeper.h"
#include "order_manager.h"
#include "risk_manager.h"

#include "market_maker.h"
#include "liquidity_taker.h"

namespace Trading {
  // 交易引擎类，负责协调交易算法、订单管理、风险控制和市场数据处理
  class TradeEngine {
  public:
    TradeEngine(Common::ClientId client_id,
                AlgoType algo_type,
                const TradeEngineCfgHashMap &ticker_cfg,
                Exchange::ClientRequestLFQueue *client_requests,
                Exchange::ClientResponseLFQueue *client_responses,
                Exchange::MEMarketUpdateLFQueue *market_updates);

    ~TradeEngine();

    // 启动和停止交易引擎主线程
    auto start() -> void {
      run_ = true;
      ASSERT(Common::createAndStartThread(6, "Trading/TradeEngine", [this] { run(); }) != nullptr, "Failed to start TradeEngine thread.");
    }

    auto stop() -> void {
      // 等待所有更新处理完成
      while(incoming_ogw_responses_->size() || incoming_md_updates_->size()) {
        logger_.log("%:% %() % Sleeping till all updates are consumed ogw-size:% md-size:%\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), incoming_ogw_responses_->size(), incoming_md_updates_->size());

        using namespace std::literals::chrono_literals;
        std::this_thread::sleep_for(10ms);
      }

      // 记录最终持仓信息
      logger_.log("%:% %() % POSITIONS\n%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  position_keeper_.toString());

      run_ = false;
    }

    auto run() noexcept -> void;

    // 将客户端请求写入无锁队列，供订单网关消费并发送到交易所
    auto sendClientRequest(const Exchange::MEClientRequest *client_request) noexcept -> void;

    // 处理订单簿变化：更新持仓管理器、特征引擎，并通知交易算法
    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook *book) noexcept -> void;

    // 处理交易事件：更新特征引擎，并通知交易算法
    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book) noexcept -> void;

    // 处理客户端响应：更新持仓管理器，并通知交易算法
    auto onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void;

    // 函数包装器，用于将订单簿更新、交易事件和客户端响应分发到交易算法
    std::function<void(TickerId ticker_id, Price price, Side side, MarketOrderBook *book)> algoOnOrderBookUpdate_;
    std::function<void(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book)> algoOnTradeUpdate_;
    std::function<void(const Exchange::MEClientResponse *client_response)> algoOnOrderUpdate_;

    // 初始化最后事件时间
    auto initLastEventTime() {
      last_event_time_ = Common::getCurrentNanos();
    }

    // 计算静默时间（自最后一个事件以来的秒数）
    auto silentSeconds() {
      return (Common::getCurrentNanos() - last_event_time_) / NANOS_TO_SECS;
    }

    // 获取客户端ID
    auto clientId() const {
      return client_id_;
    }

    TradeEngine() = delete;
    TradeEngine(const TradeEngine &) = delete;
    TradeEngine(const TradeEngine &&) = delete;
    TradeEngine &operator=(const TradeEngine &) = delete;
    TradeEngine &operator=(const TradeEngine &&) = delete;

  private:
    const ClientId client_id_;  // 本交易引擎的客户端ID

    // 从股票代码（TickerId）到MarketOrderBook的哈希映射容器
    MarketOrderBookHashMap ticker_order_book_;

    // 无锁队列：
    // 一个用于发布 outgoing 客户端请求，供订单网关消费并发送到交易所
    // 第二个用于消费 incoming 客户端响应，由订单网关根据从交易所收到的数据写入
    // 第三个用于消费 incoming 市场数据更新，由市场数据消费者根据从交易所收到的数据写入
    Exchange::ClientRequestLFQueue *outgoing_ogw_requests_ = nullptr;
    Exchange::ClientResponseLFQueue *incoming_ogw_responses_ = nullptr;
    Exchange::MEMarketUpdateLFQueue *incoming_md_updates_ = nullptr;

    Nanos last_event_time_ = 0;  // 最后一个事件的时间戳
    volatile bool run_ = false;   // 运行状态标志

    std::string time_str_;
    Logger logger_;

    // 交易算法的特征引擎
    FeatureEngine feature_engine_;

    // 用于跟踪持仓、盈亏和成交量的持仓管理器
    PositionKeeper position_keeper_;

    // 为交易算法简化订单管理任务的订单管理器
    OrderManager order_manager_;

    // 用于跟踪和执行交易前风险检查的风险管理器
    RiskManager risk_manager_;

    // 做市或流动性获取算法实例（单个交易引擎实例中只创建其中一个）
    MarketMaker *mm_algo_ = nullptr;
    LiquidityTaker *taker_algo_ = nullptr;

    // 用于初始化函数包装器的默认方法
    auto defaultAlgoOnOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook *) noexcept -> void {
      logger_.log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), ticker_id, Common::priceToString(price).c_str(),
                  Common::sideToString(side).c_str());
    }

    auto defaultAlgoOnTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *) noexcept -> void {
      logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  market_update->toString().c_str());
    }

    auto defaultAlgoOnOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
      logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  client_response->toString().c_str());
    }
  };
}