#include "trade_engine.h"

namespace Trading {
  TradeEngine::TradeEngine(Common::ClientId client_id,
                           AlgoType algo_type,
                           const TradeEngineCfgHashMap &ticker_cfg,
                           Exchange::ClientRequestLFQueue *client_requests,
                           Exchange::ClientResponseLFQueue *client_responses,
                           Exchange::MEMarketUpdateLFQueue *market_updates)
      : client_id_(client_id), outgoing_ogw_requests_(client_requests), incoming_ogw_responses_(client_responses),
        incoming_md_updates_(market_updates), logger_("trading_engine_" + std::to_string(client_id) + ".log"),
        feature_engine_(&logger_),
        position_keeper_(&logger_),
        order_manager_(&logger_, this, risk_manager_),
        risk_manager_(&logger_, &position_keeper_, ticker_cfg) {
    // 初始化每个股票的订单簿并关联交易引擎
    for (size_t i = 0; i < ticker_order_book_.size(); ++i) {
      ticker_order_book_[i] = new MarketOrderBook(i, &logger_);
      ticker_order_book_[i]->setTradeEngine(this);
    }

    // 初始化订单簿变化、交易事件和客户端响应的回调函数包装器（默认实现）
    algoOnOrderBookUpdate_ = [this](auto ticker_id, auto price, auto side, auto book) {
      defaultAlgoOnOrderBookUpdate(ticker_id, price, side, book);
    };
    algoOnTradeUpdate_ = [this](auto market_update, auto book) { defaultAlgoOnTradeUpdate(market_update, book); };
    algoOnOrderUpdate_ = [this](auto client_response) { defaultAlgoOnOrderUpdate(client_response); };

    // 根据指定的算法类型创建交易算法实例，构造函数会覆盖上述回调函数
    if (algo_type == AlgoType::MAKER) {
      mm_algo_ = new MarketMaker(&logger_, this, &feature_engine_, &order_manager_, ticker_cfg);
    } else if (algo_type == AlgoType::TAKER) {
      taker_algo_ = new LiquidityTaker(&logger_, this, &feature_engine_, &order_manager_, ticker_cfg);
    }

    for (TickerId i = 0; i < ticker_cfg.size(); ++i) {
      logger_.log("%:% %() % Initialized % Ticker:% %.\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_),
                  algoTypeToString(algo_type), i,
                  ticker_cfg.at(i).toString());
    }
  }

  TradeEngine::~TradeEngine() {
    run_ = false;

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);

    delete mm_algo_; mm_algo_ = nullptr;
    delete taker_algo_; taker_algo_ = nullptr;

    for (auto &order_book: ticker_order_book_) {
      delete order_book;
      order_book = nullptr;
    }

    outgoing_ogw_requests_ = nullptr;
    incoming_ogw_responses_ = nullptr;
    incoming_md_updates_ = nullptr;
  }

  // 发送客户端请求（订单）到外部网关队列
  auto TradeEngine::sendClientRequest(const Exchange::MEClientRequest *client_request) noexcept -> void {
    logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
              client_request->toString().c_str());
    // 将请求写入无锁队列
    auto next_write = outgoing_ogw_requests_->getNextToWriteTo();
    *next_write = std::move(*client_request);
    outgoing_ogw_requests_->updateWriteIndex();
    TTT_MEASURE(T10_TradeEngine_LFQueue_write, logger_);  // 测量队列写入时间
    // 记录订单（trade）发送完成的时间戳
    TTT_MEASURE(Order_Sent, logger_);  // 标记trade终点
  }

  // 处理传入的客户端响应和市场数据更新，可能生成新的客户端请求
  auto TradeEngine::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_) {
      // 处理客户端响应（如订单确认、成交回报等）
      for (auto client_response = incoming_ogw_responses_->getNextToRead(); client_response; client_response = incoming_ogw_responses_->getNextToRead()) {
        TTT_MEASURE(T9t_TradeEngine_LFQueue_read, logger_);  // 测量队列读取时间

        logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                    client_response->toString().c_str());
        onOrderUpdate(client_response);  // 处理订单更新
        incoming_ogw_responses_->updateReadIndex();  // 更新队列读取索引
        last_event_time_ = Common::getCurrentNanos();  // 更新最后事件时间
      }

      // 处理市场数据更新（如订单簿变化、成交等）
      for (auto market_update = incoming_md_updates_->getNextToRead(); market_update; market_update = incoming_md_updates_->getNextToRead()) {
        TTT_MEASURE(T9_TradeEngine_LFQueue_read, logger_);  // 测量队列读取时间

        logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
              market_update->toString().c_str());
        // 记录市场数据更新（tick）开始处理的时间戳
        TTT_MEASURE(Tick_Received, logger_);  // 标记tick起点
        ASSERT(market_update->ticker_id_ < ticker_order_book_.size(),
            "Unknown ticker-id on update:" + market_update->toString());  // 断言股票代码有效
        ticker_order_book_[market_update->ticker_id_]->onMarketUpdate(market_update);  // 处理市场更新（更新订单簿）
        incoming_md_updates_->updateReadIndex();  // 更新队列读取索引
        last_event_time_ = Common::getCurrentNanos();  // 更新最后事件时间
      }
    }
  }

  // 处理订单簿变化：更新持仓管理器、特征引擎，并通知交易算法
  auto TradeEngine::onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook *book) noexcept -> void {
    logger_.log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), ticker_id, Common::priceToString(price).c_str(),
                Common::sideToString(side).c_str());

    auto bbo = book->getBBO();  // 获取最优买卖报价

    // 更新持仓管理器的BBO信息
    START_MEASURE(Trading_PositionKeeper_updateBBO);
    position_keeper_.updateBBO(ticker_id, bbo);
    END_MEASURE(Trading_PositionKeeper_updateBBO, logger_);

    // 通知特征引擎处理订单簿更新
    START_MEASURE(Trading_FeatureEngine_onOrderBookUpdate);
    feature_engine_.onOrderBookUpdate(ticker_id, price, side, book);
    END_MEASURE(Trading_FeatureEngine_onOrderBookUpdate, logger_);

    // 通知交易算法处理订单簿更新
    START_MEASURE(Trading_TradeEngine_algoOnOrderBookUpdate_);
    algoOnOrderBookUpdate_(ticker_id, price, side, book);
    END_MEASURE(Trading_TradeEngine_algoOnOrderBookUpdate_, logger_);
  }

  // 处理交易事件：更新特征引擎，并通知交易算法
  auto TradeEngine::onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book) noexcept -> void {
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                market_update->toString().c_str());

    // 通知特征引擎处理交易事件
    START_MEASURE(Trading_FeatureEngine_onTradeUpdate);
    feature_engine_.onTradeUpdate(market_update, book);
    END_MEASURE(Trading_FeatureEngine_onTradeUpdate, logger_);

    // 通知交易算法处理交易事件
    START_MEASURE(Trading_TradeEngine_algoOnTradeUpdate_);
    algoOnTradeUpdate_(market_update, book);
    END_MEASURE(Trading_TradeEngine_algoOnTradeUpdate_, logger_);
  }

  // 处理客户端响应：更新持仓管理器，并通知交易算法
  auto TradeEngine::onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                client_response->toString().c_str());

    // 若为成交响应，更新持仓管理器
    if (UNLIKELY(client_response->type_ == Exchange::ClientResponseType::FILLED)) {
      START_MEASURE(Trading_PositionKeeper_addFill);
      position_keeper_.addFill(client_response);
      END_MEASURE(Trading_PositionKeeper_addFill, logger_);
    }

    // 通知交易算法处理客户端响应
    START_MEASURE(Trading_TradeEngine_algoOnOrderUpdate_);
    algoOnOrderUpdate_(client_response);
    END_MEASURE(Trading_TradeEngine_algoOnOrderUpdate_, logger_);
  }
}