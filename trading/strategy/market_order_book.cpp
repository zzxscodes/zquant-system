#include "market_order_book.h"

#include "trade_engine.h"

namespace Trading {
  MarketOrderBook::MarketOrderBook(TickerId ticker_id, Logger *logger)
      : ticker_id_(ticker_id), orders_at_price_pool_(ME_MAX_PRICE_LEVELS), order_pool_(ME_MAX_ORDER_IDS), logger_(logger) {
  }

  MarketOrderBook::~MarketOrderBook() {
    logger_->log("%:% %() % OrderBook\n%\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_), toString(false, true));

    trade_engine_ = nullptr;
    bids_by_price_ = asks_by_price_ = nullptr;
    oid_to_order_.fill(nullptr);
  }

  // 处理市场数据更新并更新限价订单簿
  auto MarketOrderBook::onMarketUpdate(const Exchange::MEMarketUpdate *market_update) noexcept -> void {
    // 判断买卖盘最优报价是否更新
    const auto bid_updated = (bids_by_price_ && market_update->side_ == Side::BUY && market_update->price_ >= bids_by_price_->price_);
    const auto ask_updated = (asks_by_price_ && market_update->side_ == Side::SELL && market_update->price_ <= asks_by_price_->price_);

    // 根据市场更新类型进行不同处理
    switch (market_update->type_) {
      case Exchange::MarketUpdateType::ADD: {
        // 分配新订单并添加到订单簿
        auto order = order_pool_.allocate(market_update->order_id_, market_update->side_, market_update->price_,
                                          market_update->qty_, market_update->priority_, nullptr, nullptr);
        START_MEASURE(Trading_MarketOrderBook_addOrder);
        addOrder(order);
        END_MEASURE(Trading_MarketOrderBook_addOrder, (*logger_));
      }
        break;
      case Exchange::MarketUpdateType::MODIFY: {
        // 修改现有订单的数量
        auto order = oid_to_order_.at(market_update->order_id_);
        order->qty_ = market_update->qty_;
      }
        break;
      case Exchange::MarketUpdateType::CANCEL: {
        // 从订单簿中移除订单
        auto order = oid_to_order_.at(market_update->order_id_);
        START_MEASURE(Trading_MarketOrderBook_removeOrder);
        removeOrder(order);
        END_MEASURE(Trading_MarketOrderBook_removeOrder, (*logger_));
      }
        break;
      case Exchange::MarketUpdateType::TRADE: {
        // 处理交易事件并通知交易引擎
        trade_engine_->onTradeUpdate(market_update, this);
        return;
      }
        break;
      case Exchange::MarketUpdateType::CLEAR: { // 清空整个限价订单簿并释放相关对象
        // 释放所有订单
        for (auto &order: oid_to_order_) {
          if (order)
            order_pool_.deallocate(order);
        }
        oid_to_order_.fill(nullptr);

        // 释放所有买单价格层级
        if(bids_by_price_) {
          for(auto bid = bids_by_price_->next_entry_; bid != bids_by_price_; bid = bid->next_entry_)
            orders_at_price_pool_.deallocate(bid);
          orders_at_price_pool_.deallocate(bids_by_price_);
        }

        // 释放所有卖单价格层级
        if(asks_by_price_) {
          for(auto ask = asks_by_price_->next_entry_; ask != asks_by_price_; ask = ask->next_entry_)
            orders_at_price_pool_.deallocate(ask);
          orders_at_price_pool_.deallocate(asks_by_price_);
        }

        bids_by_price_ = asks_by_price_ = nullptr;
      }
        break;
      case Exchange::MarketUpdateType::INVALID:
      case Exchange::MarketUpdateType::SNAPSHOT_START:
      case Exchange::MarketUpdateType::SNAPSHOT_END:
        // 不处理无效更新和快照开始/结束事件
        break;
    }

    // 更新最优买卖报价
    START_MEASURE(Trading_MarketOrderBook_updateBBO);
    updateBBO(bid_updated, ask_updated);
    END_MEASURE(Trading_MarketOrderBook_updateBBO, (*logger_));

    // 记录订单簿更新日志
    logger_->log("%:% %() % % %", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_), market_update->toString(), bbo_.toString());

    // 通知交易引擎订单簿已更新
    trade_engine_->onOrderBookUpdate(market_update->ticker_id_, market_update->price_, market_update->side_, this);
  }

  // 将订单簿信息转换为字符串（支持详细模式和有效性检查）
  auto MarketOrderBook::toString(bool detailed, bool validity_check) const -> std::string {
    std::stringstream ss;
    std::string time_str;

    // 用于打印价格层级及其包含订单的 lambda 函数
    auto printer = [&](std::stringstream &ss, MarketOrdersAtPrice *itr, Side side, Price &last_price,
                       bool sanity_check) {
      char buf[4096];
      Qty qty = 0;
      size_t num_orders = 0;

      // 计算该价格层级的总数量和订单数
      for (auto o_itr = itr->first_mkt_order_;; o_itr = o_itr->next_order_) {
        qty += o_itr->qty_;
        ++num_orders;
        if (o_itr->next_order_ == itr->first_mkt_order_)
          break;
      }
      // 打印价格层级基本信息
      sprintf(buf, " <px:%3s p:%3s n:%3s> %-3s @ %-5s(%-4s)",
              priceToString(itr->price_).c_str(), priceToString(itr->prev_entry_->price_).c_str(),
              priceToString(itr->next_entry_->price_).c_str(),
              priceToString(itr->price_).c_str(), qtyToString(qty).c_str(), std::to_string(num_orders).c_str());
      ss << buf;
      // 详细模式下打印每个订单的信息
      for (auto o_itr = itr->first_mkt_order_;; o_itr = o_itr->next_order_) {
        if (detailed) {
          sprintf(buf, "[oid:%s q:%s p:%s n:%s] ",
                  orderIdToString(o_itr->order_id_).c_str(), qtyToString(o_itr->qty_).c_str(),
                  orderIdToString(o_itr->prev_order_ ? o_itr->prev_order_->order_id_ : OrderId_INVALID).c_str(),
                  orderIdToString(o_itr->next_order_ ? o_itr->next_order_->order_id_ : OrderId_INVALID).c_str());
          ss << buf;
        }
        if (o_itr->next_order_ == itr->first_mkt_order_)
          break;
      }

      ss << std::endl;

      // 有效性检查：确保买卖盘价格排序正确
      if (sanity_check) {
        if ((side == Side::SELL && last_price >= itr->price_) || (side == Side::BUY && last_price <= itr->price_)) {
          FATAL("Bids/Asks not sorted by ascending/descending prices last:" + priceToString(last_price) + " itr:" +
                itr->toString());
        }
        last_price = itr->price_;
      }
    };

    // 构建订单簿字符串
    ss << "Ticker:" << tickerIdToString(ticker_id_) << std::endl;
    {
      // 打印卖单价格层级
      auto ask_itr = asks_by_price_;
      auto last_ask_price = std::numeric_limits<Price>::min();
      for (size_t count = 0; ask_itr; ++count) {
        ss << "ASKS L:" << count << " => ";
        auto next_ask_itr = (ask_itr->next_entry_ == asks_by_price_ ? nullptr : ask_itr->next_entry_);
        printer(ss, ask_itr, Side::SELL, last_ask_price, validity_check);
        ask_itr = next_ask_itr;
      }
    }

    ss << std::endl << "                          X" << std::endl << std::endl;

    {
      // 打印买单价格层级
      auto bid_itr = bids_by_price_;
      auto last_bid_price = std::numeric_limits<Price>::max();
      for (size_t count = 0; bid_itr; ++count) {
        ss << "BIDS L:" << count << " => ";
        auto next_bid_itr = (bid_itr->next_entry_ == bids_by_price_ ? nullptr : bid_itr->next_entry_);
        printer(ss, bid_itr, Side::BUY, last_bid_price, validity_check);
        bid_itr = next_bid_itr;
      }
    }

    return ss.str();
  }
}